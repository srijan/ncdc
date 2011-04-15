/* ncdc - NCurses Direct Connect client

  Copyright (c) 2011 Yoran Heling

  Permission is hereby granted, free of charge, to any person obtaining
  a copy of this software and associated documentation files (the
  "Software"), to deal in the Software without restriction, including
  without limitation the rights to use, copy, modify, merge, publish,
  distribute, sublicense, and/or sell copies of the Software, and to
  permit persons to whom the Software is furnished to do so, subject to
  the following conditions:

  The above copyright notice and this permission notice shall be included
  in all copies or substantial portions of the Software.

  THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND,
  EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF
  MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.
  IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY
  CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT,
  TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN CONNECTION WITH THE
  SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.

*/


#include "ncdc.h"
#include <errno.h>
#include <string.h>


#if INTERFACE

#define HUBS_IDLE       0
#define HUBS_CONNECTING 1
#define HUBS_CONNECTED  2

struct nmdc_hub {
  struct ui_tab *tab; // to get name (for config) and for logging & setting of title
  int state;
  GSocketConnection *conn;
  GDataInputStream *in;
  GOutputStream *out;
  char *out_buf;
  char *out_buf_old;
  int out_buf_len;
  int out_buf_pos;
  // Used for all async operations. Cancelling is only used on user-disconnect,
  // and when we disconnect we want to cancel *all* outstanding operations at
  // the same time. The documentation advices against this, but...
  //   http://www.mail-archive.com/gtk-devel-list@gnome.org/msg10628.html
  GCancellable *cancel;
  // nick as used in this connection, NULL when no $ValidateNick has been sent yet
  char *nick_hub; // in hub encoding
  char *nick;     // UTF-8
  // TRUE is the above nick has also been validated (and we're properly logged in)
  gboolean nick_valid;
};

#endif


#define hubconf_get(type, hub, key) (\
  g_key_file_has_key(conf_file, (hub)->tab->name, (key), NULL)\
    ? g_key_file_get_##type(conf_file, (hub)->tab->name, (key), NULL)\
    : g_key_file_get_##type(conf_file, "global", (key), NULL))


struct nmdc_hub *nmdc_create(struct ui_tab *tab) {
  struct nmdc_hub *hub = g_new0(struct nmdc_hub, 1);
  hub->tab = tab;
  hub->cancel = g_cancellable_new();
  hub->out_buf_len = 1024;
  hub->out_buf = g_new(char, hub->out_buf_len);
  return hub;
}


static void handle_write(GObject *src, GAsyncResult *res, gpointer dat) {
  struct nmdc_hub *hub = dat;

  GError *err = NULL;
  gsize written = g_output_stream_write_finish(G_OUTPUT_STREAM(src), res, &err);

  if(!err || err->code != G_IO_ERROR_CANCELLED) {
    g_free(hub->out_buf_old);
    hub->out_buf_old = NULL;
  }

  if(err) {
    if(err->code != G_IO_ERROR_CANCELLED) {
      ui_logwindow_printf(hub->tab->log, "Write error: %s", err->message);
      nmdc_disconnect(hub);
    }
    g_error_free(err);

  } else {
    if(written > 0) {
      hub->out_buf_pos -= written;
      memmove(hub->out_buf, hub->out_buf+written, hub->out_buf_pos);
    }
    g_assert(hub->out_buf_pos >= 0);
    if(hub->out_buf_pos > 0)
      g_output_stream_write_async(hub->out, hub->out_buf, hub->out_buf_pos, G_PRIORITY_DEFAULT, hub->cancel, handle_write, hub);
  }
}


static void send_cmd(struct nmdc_hub *hub, const char *cmd) {
  // ignore writes when we're not connected
  if(hub->state != HUBS_CONNECTED)
    return;

  fprintf(stderr, "%s> %s\n", hub->tab->name, cmd); // debug

  // append cmd to the buffer
  int len = strlen(cmd)+1; // the 1 is the termination char
  if(hub->out_buf_pos + len > hub->out_buf_len) {
    hub->out_buf_len = MAX(hub->out_buf_len*2, hub->out_buf_pos+len);
    // we can only use g_renew() if no async operation is in progress.
    // Otherwise the async operation may be trying to read from the buffer
    // which we just freed. Instead, allocate a new buffer for the new data and
    // keep the old buffer in memory, to be freed after the write has finished.
    // (I don't suppose this will happen very often)
    if(!g_output_stream_has_pending(hub->out) || hub->out_buf_old)
      hub->out_buf = g_renew(char, hub->out_buf, hub->out_buf_len);
    else {
      hub->out_buf_old = hub->out_buf;
      hub->out_buf = g_new(char, hub->out_buf_len);
      memcpy(hub->out_buf, hub->out_buf_old, hub->out_buf_pos);
    }
  }
  memcpy(hub->out_buf + hub->out_buf_pos, cmd, len-1);
  hub->out_buf[hub->out_buf_pos+len-1] = '|';
  hub->out_buf_pos += len;

  // write it, if we aren't doing so already
  if(!g_output_stream_has_pending(hub->out))
    g_output_stream_write_async(hub->out, hub->out_buf, hub->out_buf_pos, G_PRIORITY_DEFAULT, hub->cancel, handle_write, hub);
}


static void send_cmdf(struct nmdc_hub *hub, const char *fmt, ...) {
  va_list va;
  va_start(va, fmt);
  char *str = g_strdup_vprintf(fmt, va);
  va_end(va);
  send_cmd(hub, str);
  g_free(str);
}


/* A best-effort character conversion function.
 *
 * If, for whatever reason, a character could not be converted, a question mark
 * will be inserted instead. Unlike g_convert_with_fallback(), this function
 * does not fail on invalid byte sequences in the input string, either. Those
 * will simply be replaced with question marks as well.
 *
 * The character sets in 'to' and 'from' are assumed to form a valid conversion
 * according to your iconv implementation.
 *
 * Modifying this function to not require glib, but instead use the iconv and
 * memory allocation functions provided by your system, should be trivial.
 *
 * This function does not correctly handle character sets that may use zeroes
 * in the middle of a string (e.g. UTF-16).
 *
 * This function may not represent best practice with respect to character set
 * conversion, nor has it been thouroughly tested.
 */
static char *convert_best_effort(const char *to, const char *from, const char *str) {
  GIConv cd = g_iconv_open(to, from);
  if(cd == (GIConv)-1) {
    g_critical("No conversion from '%s' to '%s': %s", from, to, g_strerror(errno));
    return g_strdup("<encoding-error>");
  }
  gsize inlen = strlen(str);
  gsize outlen = inlen+96;
  gsize outsize = inlen+100;
  char *inbuf = (char *)str;
  char *dest = g_malloc(outsize);
  char *outbuf = dest;
  while(inlen > 0) {
    gsize r = g_iconv(cd, &inbuf, &inlen, &outbuf, &outlen);
    if(r != (gsize)-1)
      continue;
    if(errno == E2BIG) {
      gsize used = outsize - outlen - 4;
      outlen += outsize;
      outsize += outsize;
      dest = g_realloc(dest, outsize);
      outbuf = dest + used;
    } else if(errno == EILSEQ || errno == EINVAL) {
      // skip this byte from the input
      inbuf++;
      inlen--;
      // Only output question mark if we happen to have enough space, otherwise
      // it's too much of a hassle...  (In most (all?) cases we do have enough
      // space, otherwise we'd have gotten E2BIG anyway)
      if(outlen >= 1) {
        *outbuf = '?';
        outbuf++;
        outlen--;
      }
    } else
      g_assert_not_reached();
  }
  memset(outbuf, 0, 4);
  g_iconv_close(cd);
  return dest;
}


static char *charset_convert(struct nmdc_hub *hub, gboolean to_utf8, const char *str) {
  char *fmt = hubconf_get(string, hub, "encoding");
  char *res = convert_best_effort(to_utf8||!fmt?"UTF-8":fmt, !to_utf8||!fmt?"UTF-8":fmt, str);
  g_free(fmt);
  return res;
}


void nmdc_send_myinfo(struct nmdc_hub *hub) {
  if(!hub->nick_valid)
    return;
  // TODO: escape description & email
  char *tmp;
  tmp = hubconf_get(string, hub, "description"); char *desc = charset_convert(hub, FALSE, tmp?tmp:""); g_free(tmp);
  tmp = hubconf_get(string, hub, "connection");  char *conn = charset_convert(hub, FALSE, tmp?tmp:""); g_free(tmp);
  tmp = hubconf_get(string, hub, "email");       char *mail = charset_convert(hub, FALSE, tmp?tmp:""); g_free(tmp);
  // TODO: more dynamic...
  send_cmdf(hub, "$MyINFO $ALL %s %s<ncdc V:0.1,M:P,H:1/0/0,S:1>$ $%s\01$%s$0$",
    hub->nick_hub, desc, conn, mail);
  g_free(desc);
  g_free(conn);
  g_free(mail);
}


void nmdc_say(struct nmdc_hub *hub, const char *str) {
  if(!hub->nick_valid)
    return;
  char *msg = charset_convert(hub, FALSE, str);
  // TODO: escape message
  send_cmdf(hub, "<%s> %s", hub->nick_hub, msg);
  g_free(msg);
}


// Info & algorithm @ http://www.teamfair.info/wiki/index.php?title=Lock_to_key
// This function modifies "lock" in-place for temporary data
static char *lock2key(char *lock) {
  char n;
  int i;
  int len = strlen(lock);
  if(len < 3)
    return g_strdup("STUPIDKEY!"); // let's not crash on invalid data
  for(i=1; i<len; i++)
    lock[i] = lock[i] ^ lock[i-1];
  lock[0] = lock[0] ^ lock[len-1] ^ lock[len-2] ^ 5;
  for(i=0; i<len; i++)
    lock[i] = ((lock[i]<<4) & 0xF0) | ((lock[i]>>4) & 0x0F);
  GString *key = g_string_sized_new(len+100);
  for(i=0; i<len; i++) {
    n = lock[i];
    if(n == 0 || n == 5 || n == 36 || n == 96 || n == 124 || n == 126)
      g_string_append_printf(key, "/%%DCN%03d%%/", n);
    else
      g_string_append_c(key, n);
  }
  return g_string_free(key, FALSE);
}


static void handle_cmd(struct nmdc_hub *hub, const char *cmd) {
  fprintf(stderr, "%s< %s\n", hub->tab->name, cmd); // debug

  GMatchInfo *nfo;

  // create regexes (declared statically, allocated/compiled on first call)
#define CMDREGEX(name, regex) \
  static GRegex * name = NULL;\
  if(!name) name = g_regex_new("\\$" regex, G_REGEX_OPTIMIZE|G_REGEX_ANCHORED|G_REGEX_DOLLAR_ENDONLY|G_REGEX_DOTALL, 0, NULL)

  CMDREGEX(lock, "Lock ([^ $]+) Pk=[^ $]+");
  CMDREGEX(hello, "Hello ([^ $]+)");

  // $Lock
  if(g_regex_match(lock, cmd, 0, &nfo)) { // 1 = lock
    char *lock = g_match_info_fetch(nfo, 1);
    // TODO: check for EXTENDEDPROTOCOL
    char *key = lock2key(lock);
    send_cmdf(hub, "$Key %s", key);
    hub->nick = hubconf_get(string, hub, "nick");
    hub->nick_hub = charset_convert(hub, FALSE, hub->nick);
    send_cmdf(hub, "$ValidateNick %s", hub->nick_hub);
    g_free(key);
    g_free(lock);
  }
  g_match_info_free(nfo);

  // $Hello
  if(g_regex_match(hello, cmd, 0, &nfo)) { // 1 = nick
    char *nick = g_match_info_fetch(nfo, 1);
    if(strcmp(nick, hub->nick_hub) == 0) {
      ui_logwindow_add(hub->tab->log, "Nick validated.");
      hub->nick_valid = TRUE;
      nmdc_send_myinfo(hub);
    } else {
      // TODO: keep track of users
    }
    g_free(nick);
  }
  g_match_info_free(nfo);

  // $GetPass
  if(strncmp(cmd, "$GetPass", 8) == 0) {
    ui_logwindow_add(hub->tab->log, "Hub requires a password. This version of ncdc does not support passworded login yet.");
    nmdc_disconnect(hub);
  }

  // $ValidateDenide
  if(strncmp(cmd, "$ValidateDenide", 15) == 0) {
    ui_logwindow_add(hub->tab->log, "Username invalid or already taken.");
    nmdc_disconnect(hub);
  }

  // $HubIsFull
  if(strncmp(cmd, "$HubIsFull", 10) == 0) {
    ui_logwindow_add(hub->tab->log, "Hub is full.");
    nmdc_disconnect(hub);
  }

  // global hub message
  if(cmd[0] != '$') {
    char *msg = charset_convert(hub, TRUE, cmd);
    // TODO: unescape message
    ui_logwindow_add(hub->tab->log, msg);
    g_free(msg);
  }
}


static void handle_input(GObject *src, GAsyncResult *res, gpointer dat) {
  struct nmdc_hub *hub = dat;

  GError *err = NULL;
  char *str = g_data_input_stream_read_upto_finish(G_DATA_INPUT_STREAM(src), res, NULL, &err);

  if(err) {
    if(err->code != G_IO_ERROR_CANCELLED) {
      ui_logwindow_printf(hub->tab->log, "Read error: %s", err->message);
      nmdc_disconnect(hub);
    }
    g_error_free(err);

  } else if(hub->state == HUBS_CONNECTED) {
    // consume the termination character.  the char should be in the read
    // buffer, and this call should therefore not block. if the hub
    // disconnected for some reason, this function will fail and all data that
    // the hub sent before disconnecting (but after the last termination char)
    // is in str (which we just discard since it probably wasn't anything
    // useful anyway)
    g_data_input_stream_read_byte(hub->in, NULL, &err);
    if(err) {
      ui_logwindow_printf(hub->tab->log, "Read error: %s", err->message);
      g_free(str);
      g_error_free(err);
      nmdc_disconnect(hub);
    } else {
      if(str) {
        handle_cmd(hub, str);
        g_free(str);
      }
      if(hub->state == HUBS_CONNECTED) // handle_cmd() may change the state
        g_data_input_stream_read_upto_async(hub->in, "|", -1, G_PRIORITY_DEFAULT, hub->cancel, handle_input, hub);
    }
  }
}


static void handle_connect(GObject *src, GAsyncResult *res, gpointer dat) {
  struct nmdc_hub *hub = dat;

  GError *err = NULL;
  GSocketConnection *conn = g_socket_client_connect_to_uri_finish(G_SOCKET_CLIENT(src), res, &err);

  if(!conn) {
    if(err->code != G_IO_ERROR_CANCELLED) {
      ui_logwindow_printf(hub->tab->log, "Could not connect to hub: %s", err->message);
      hub->state = HUBS_IDLE;
    }
    g_error_free(err);
  } else {
    hub->state = HUBS_CONNECTED;
    hub->conn = conn;
    hub->in = g_data_input_stream_new(g_io_stream_get_input_stream(G_IO_STREAM(hub->conn)));
    hub->out = g_io_stream_get_output_stream(G_IO_STREAM(hub->conn));

    // continuously wait for incoming commands
    g_data_input_stream_read_upto_async(hub->in, "|", -1, G_PRIORITY_DEFAULT, hub->cancel, handle_input, hub);

    GInetSocketAddress *addr = G_INET_SOCKET_ADDRESS(g_socket_connection_get_remote_address(conn, NULL));
    g_assert(addr);
    char *ip = g_inet_address_to_string(g_inet_socket_address_get_address(addr));
    ui_logwindow_printf(hub->tab->log, "Connected to %s:%d.", ip, g_inet_socket_address_get_port(addr));
    g_free(ip);
    g_object_unref(addr);
  }
}


void nmdc_connect(struct nmdc_hub *hub) {
  char *addr = hubconf_get(string, hub, "hubaddr");
  g_assert(addr);

  ui_logwindow_printf(hub->tab->log, "Connecting to %s...", addr);
  hub->state = HUBS_CONNECTING;

  GSocketClient *sc = g_socket_client_new();
  g_socket_client_connect_to_host_async(sc, addr, 411, hub->cancel, handle_connect, hub);
  g_object_unref(sc);

  g_free(addr);
}


void nmdc_disconnect(struct nmdc_hub *hub) {
  // cancel current operations and create a new cancellable object for later operations
  // (_reset() is not a good idea when there are still async jobs using the cancellable object)
  g_cancellable_cancel(hub->cancel);
  g_object_unref(hub->cancel);
  hub->cancel = g_cancellable_new();
  if(hub->conn) {
    // do these functions block?
    g_object_unref(hub->in);
    g_object_unref(hub->conn); // also closes the connection
    hub->conn = NULL;
  }
  g_free(hub->nick);     hub->nick = NULL;
  g_free(hub->nick_hub); hub->nick_hub = NULL;
  hub->nick_valid = FALSE;
  hub->state = HUBS_IDLE;
  ui_logwindow_printf(hub->tab->log, "Disconnected.");
}


void nmdc_free(struct nmdc_hub *hub) {
  nmdc_disconnect(hub);
  g_object_unref(hub->cancel);
  g_free(hub->out_buf);
  g_free(hub->out_buf_old);
  g_free(hub);
}

