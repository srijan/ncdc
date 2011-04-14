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
      memmove(hub->out_buf+written, hub->out_buf, hub->out_buf_pos);
    }
    g_assert(hub->out_buf_pos >= 0);
    if(hub->out_buf_pos > 0)
      g_output_stream_write_async(hub->out, hub->out_buf, hub->out_buf_pos, G_PRIORITY_DEFAULT, hub->cancel, handle_write, hub);
  }
}


static void send_cmd(struct nmdc_hub *hub, char *cmd) {
  // ignore writes when we're not connected
  if(hub->state != HUBS_CONNECTED)
    return;

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


static void handle_cmd(struct nmdc_hub *hub, char *cmd) {
  // TODO
  // DEBUG: just send back what we received.
  //g_warning("< %d %s", hub->state, cmd);
  send_cmd(hub, cmd);
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

  } else {
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
    // does this guarantee that a write() does not block?
    g_buffered_output_stream_set_auto_grow(G_BUFFERED_OUTPUT_STREAM(hub->out), TRUE);

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

