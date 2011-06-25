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
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>


// List of expected incoming connections.
// This is list managed by the functions/macros below, in addition to
// cc_init_global(), cc_remove_hub() and cc_get_hub().

#if INTERFACE

struct cc_expect {
  struct hub *hub;
  char *nick; // hub encoding
  time_t added;
};

#define cc_expect_add(h, n) do {\
    struct cc_expect *e = g_slice_new0(struct cc_expect);\
    e->hub = h;\
    e->nick = g_strdup(n);\
    time(&(e->added));\
    g_queue_push_tail(cc_expected, e);\
  } while(0)

#endif

GQueue *cc_expected;


gboolean cc_expect_check(gpointer data) {
  time_t t = time(NULL)-300; // keep them in the list for 5 min.
  GList *p, *n;
  for(n=cc_expected->head; n;) {
    p = n->next;
    struct cc_expect *e = n->data;
    if(e->added < t) {
      g_message("Expected connection from %s on %s, but received none.", e->nick, e->hub->tab->name);
      g_free(e->nick);
      g_slice_free(struct cc_expect, e);
      g_queue_delete_link(cc_expected, n);
    } else
      break;
    n = p;
  }
  return TRUE;
}




// Main C-C objects

#if INTERFACE

struct cc {
  struct net *net;
  struct hub *hub;
  char *nick_raw; // (NMDC)
  char *nick;
  gboolean adc : 1;
  gboolean active : 1;
  gboolean isop : 1;
  gboolean slot_mini : 1;
  gboolean slot_granted : 1;
  int state;      // (ADC)
  char cid[24];   // (ADC);
  int timeout_src;
  time_t last_action;
  char remoteaddr[24]; // xxx.xxx.xxx.xxx:ppppp
  char *token;    // (ADC)
  char *last_file;
  guint64 last_size;
  guint64 last_length;
  guint64 last_offset;
  GError *err;
  GSequenceIter *iter;
};

#define cc_init_global() do {\
    cc_expected = g_queue_new();\
    g_timeout_add_seconds_full(G_PRIORITY_LOW, 120, cc_expect_check, NULL, NULL);\
    cc_list = g_sequence_new(NULL);\
  } while(0)

#endif

// opened connections - ui_conn is responsible for the ordering
GSequence *cc_list;


// When a hub tab is closed (not just disconnected), make sure all hub fields
// are reset to NULL - since we won't be able to dereference it anymore.  Note
// that we do keep the connections opened, and things can resume as normal
// without the hub field, since it is only used in the initial phase (with the
// $MyNick's being exchanged.)
// Note that the connection will remain hubless even when the same hub is later
// opened again. I don't think this is a huge problem, however.
void cc_remove_hub(struct hub *hub) {
  // Remove from cc objects
  GSequenceIter *i = g_sequence_get_begin_iter(cc_list);
  for(; !g_sequence_iter_is_end(i); i=g_sequence_iter_next(i)) {
    struct cc *c = g_sequence_get(i);
    if(c->hub == hub)
      c->hub = NULL;
  }

  // Remove from expects list
  GList *p, *n;
  for(n=cc_expected->head; n;) {
    p = n->next;
    struct cc_expect *e = n->data;
    if(e->hub == hub) {
      g_free(e->nick);
      g_slice_free(struct cc_expect, e);
      g_queue_delete_link(cc_expected, n);
    }
    n = p;
  }
}


// Can be cached if performance is an issue. Note that even file transfers that
// do not require a slot are still counted as taking a slot. For this reason,
// the return value can be larger than the configured number of slots. This
// also means that an upload that requires a slot will not be granted if there
// are many transfers active that don't require a slot.
int cc_slots_in_use(int *mini) {
  int num = 0;
  int m = 0;
  GSequenceIter *i = g_sequence_get_begin_iter(cc_list);
  for(; !g_sequence_iter_is_end(i); i=g_sequence_iter_next(i)) {
    struct cc *c = g_sequence_get(i);
    if(c->net->file_left)
      num++;
    if(c->net->file_left && c->slot_mini)
      m++;
  }
  if(mini)
    *mini = m;
  return num;
}


// Check whether we've got a duplicate.
static gboolean cc_check_dupe(struct cc *cc) {
  GSequenceIter *i = g_sequence_get_begin_iter(cc_list);
  for(; !g_sequence_iter_is_end(i); i=g_sequence_iter_next(i)) {
    struct cc *c = g_sequence_get(i);
    if(cc == c || c->timeout_src || !!c->adc != !!cc->adc)
      continue;
    // NMDC (cc->hub and cc->nick_raw must be known)
    if(!c->adc && c->hub == cc->hub && c->nick_raw && cc->nick_raw && strcmp(c->nick_raw, cc->nick_raw) == 0)
      return TRUE;
    // ADC (cc->cid must be known)
    if(c->adc && memcmp(c->cid, cc->cid, 24) == 0)
      return TRUE;
  }
  return FALSE;
}


// Figure out from which hub a connection came
static struct hub *cc_get_hub(struct cc *cc) {
  GList *n;
  for(n=cc_expected->head; n; n=n->next) {
    struct cc_expect *e = n->data;
    if(strcmp(e->nick, cc->nick_raw) == 0) {
      struct hub *hub = e->hub;
      g_free(e->nick);
      g_slice_free(struct cc_expect, e);
      g_queue_delete_link(cc_expected, n);
      return hub;
    }
  }

  // This is a fallback, and quite an ugly one at that.
  for(n=ui_tabs; n; n=n->next) {
    struct ui_tab *t = n->data;
    if(t->type == UIT_HUB && g_hash_table_lookup(t->hub->users, cc->nick_raw)) {
      g_warning("Unexpected incoming connection from %s", cc->nick_raw);
      return t->hub;
    }
  }
  return NULL;
}


static gboolean request_slot(struct cc *cc, gboolean need_full) {
  int minislots;
  int slots = cc_slots_in_use(&minislots);

  cc->slot_mini = FALSE;

  // if this connection is granted a slot, then just allow it
  if(cc->slot_granted)
    return TRUE;

  // if we have a free slot, use that
  if(slots < conf_slots())
    return TRUE;

  // if we can use a minislot, do so
  if(!need_full && minislots < conf_minislots()) {
    cc->slot_mini = TRUE;
    return TRUE;
  }

  // if we can use a minislot yet we don't have one, still allow an OP
  if(!need_full && cc->isop)
    return TRUE;

  // none of the above? then we're out of slots
  return FALSE;
}


static void handle_error(struct net *n, int action, GError *err) {
  struct cc *cc = n->handle;
  if(!cc->err) // ignore network errors if there already was a protocol error
    g_propagate_error(&(cc->err), g_error_copy(err));
  cc_disconnect(n->handle);
}


// err->code:
//  40: Generic protocol error
//  50: Generic internal error
//  51: File not available
//  53: No slots
// Handles both ADC GET and the NMDC $ADCGET
static void handle_adcget(struct cc *cc, char *type, char *id, guint64 start, gint64 bytes, GError **err) {
  // tthl
  if(strcmp(type, "tthl") == 0) {
    if(strncmp(id, "TTH/", 4) != 0 || strlen(id) != 4+39 || start != 0 || bytes != -1) {
      g_set_error_literal(err, 1, 40, "Invalid arguments");
      return;
    }
    char root[24];
    base32_decode(id+4, root);
    int len;
    char *dat = fl_hashdat_get(root, &len);
    if(!dat)
      g_set_error_literal(err, 1, 51, "File Not Available");
    else {
      // no need to adc_escape(id) here, since it cannot contain any special characters
      net_sendf(cc->net, cc->adc ? "CSND tthl %s 0 %d" : "$ADCSND tthl %s 0 %d", id, len);
      net_send_raw(cc->net, dat, len);
      free(dat);
    }
    return;
  }

  // list
  if(strcmp(type, "list") == 0) {
    if(id[0] != '/' || id[strlen(id)-1] != '/' || start != 0 || bytes != -1) {
      g_set_error_literal(err, 1, 40, "Invalid arguments");
      return;
    }
    struct fl_list *f = fl_local_list ? fl_list_from_path(fl_local_list, id) : NULL;
    if(!f || f->isfile) {
      g_set_error_literal(err, 1, 51, "File Not Available");
      return;
    }
    // We don't support recursive lists (yet), as these may be somewhat expensive.
    GString *buf = g_string_new("");
    GError *e = NULL;
    if(!fl_save(f, NULL, buf, 1, &e)) {
      g_set_error(err, 1, 50, "Creating partial XML list: %s", e->message);
      g_error_free(e);
      g_string_free(buf, TRUE);
      return;
    }
    char *eid = adc_escape(id, !cc->adc);
    net_sendf(cc->net, cc->adc ? "CSND list %s 0 %d" : "$ADCSND list %s 0 %d", eid, buf->len);
    net_send_raw(cc->net, buf->str, buf->len);
    g_free(eid);
    g_string_free(buf, TRUE);
    return;
  }

  // file
  if(strcmp(type, "file") != 0) {
    g_set_error_literal(err, 40, 0, "Unsupported ADCGET type");
    return;
  }

  // get path (for file uploads)
  // TODO: files.xml? (Required by ADC, but I doubt it's used)
  char *path = NULL;
  char *vpath;
  struct fl_list *f = NULL;
  gboolean needslot = TRUE;

  // files.xml.bz2
  if(strcmp(id, "files.xml.bz2") == 0) {
    path = g_strdup(fl_local_list_file);
    vpath = g_strdup("files.xml.bz2");
    needslot = FALSE;
  // / (path in the nameless root)
  } else if(id[0] == '/' && fl_local_list) {
    f = fl_list_from_path(fl_local_list, id);
  // TTH/
  } else if(strncmp(id, "TTH/", 4) == 0 && strlen(id) == 4+39) {
    char root[24];
    base32_decode(id+4, root);
    GSList *l = fl_local_from_tth(root);
    f = l ? l->data : NULL;
  }

  if(f) {
    char *enc_path = fl_local_path(f);
    path = g_filename_from_utf8(enc_path, -1, NULL, NULL, NULL);
    g_free(enc_path);
    vpath = fl_list_path(f);
  }

  // validate
  struct stat st;
  if(!path || stat(path, &st) < 0 || !S_ISREG(st.st_mode) || start > st.st_size) {
    if(start > st.st_size)
      g_set_error_literal(err, 1, 52, "File Part Not Available");
    else
      g_set_error_literal(err, 1, 51, "File Not Available");
    g_free(path);
    g_free(vpath);
    return;
  }
  if(bytes < 0 || bytes > st.st_size-start)
    bytes = st.st_size-start;
  if(needslot && st.st_size < conf_minislot_size())
    needslot = FALSE;

  // send
  if(request_slot(cc, needslot)) {
    g_free(cc->last_file);
    cc->last_file = vpath;
    cc->last_length = bytes;
    cc->last_offset = start;
    cc->last_size = st.st_size;
    char *tmp = adc_escape(id, !cc->adc);
    net_sendf(cc->net, cc->adc ? "CSND file %s %"G_GUINT64_FORMAT" %"G_GINT64_FORMAT : "$ADCSND file %s %"G_GUINT64_FORMAT" %"G_GINT64_FORMAT, tmp, start, bytes);
    net_sendfile(cc->net, path, start, bytes);
    g_free(tmp);
    g_free(path);
  } else {
    g_set_error_literal(err, 1, 53, "No Slots Available");
    g_free(vpath);
  }
}


static void adc_handle(struct cc *cc, char *msg) {
  struct adc_cmd cmd;
  GError *err = NULL;

  if(!msg[0])
    return;

  adc_parse(msg, &cmd, &err);
  if(err) {
    g_warning("ADC parse error from %s: %s. --> %s", net_remoteaddr(cc->net), err->message, msg);
    g_error_free(err);
    return;
  }

  if(cmd.type != 'C') {
    g_warning("Not a client command from %s: %s. --> %s", net_remoteaddr(cc->net), err->message, msg);
    g_strfreev(cmd.argv);
    return;
  }

  switch(cmd.cmd) {

  case ADCC_SUP:
    // TODO: actually do something with the arguments.
    if(cc->state == ADC_S_PROTOCOL) {
      cc->state = ADC_S_IDENTIFY;
      if(cc->active)
        net_send(cc->net, "CSUP ADBASE ADTIGR ADBZIP");

      GString *r = adc_generate('C', ADCC_INF, 0, 0);
      char *cid = g_key_file_get_string(conf_file, "global", "cid", NULL);
      adc_append(r, "ID", cid);
      if(!cc->active)
        adc_append(r, "TO", cc->token);
      net_send(cc->net, r->str);
      g_free(cid);
      g_string_free(r, TRUE);
    }
    break;

  case ADCC_INF:
    if(cc->state == ADC_S_IDENTIFY) {
      cc->state = ADC_S_NORMAL;
      char *id = adc_getparam(cmd.argv, "ID", NULL);
      char cid[24];
      if(strlen(id) == 39)
        base32_decode(id, cid);
      if(!id) {
        g_warning("User did not sent a CID. (%s): %s", net_remoteaddr(cc->net), msg);
        cc_disconnect(cc);
      } else if(strlen(id) != 39 || (!cc->active && memcmp(cid, cc->cid, 24) != 0)) {
        g_warning("Incorrect CID. (%s): %s", net_remoteaddr(cc->net), msg);
        cc_disconnect(cc);
      } else if(cc->active) {
        // TODO: if cc->active, figure out hub, validate token and get nick
      }
    }
    break;

  case ADCC_GET:
    if(cc->state == ADC_S_NORMAL && cmd.argc >= 4) {
      guint64 start = g_ascii_strtoull(cmd.argv[2], NULL, 0);
      gint64 len = g_ascii_strtoll(cmd.argv[3], NULL, 0);
      GError *err = NULL;
      handle_adcget(cc, cmd.argv[0], cmd.argv[1], start, len, &err);
      if(err) {
        GString *r = adc_generate('C', ADCC_STA, 0, 0);
        g_string_append_printf(r, " 1%02d", err->code);
        adc_append(r, NULL, err->message);
        net_send(cc->net, r->str);
        g_error_free(err);
        g_string_free(r, TRUE);
      }
    }
    break;

  default:
    g_message("Unknown command from %s: %s", net_remoteaddr(cc->net), msg);
  }

  g_strfreev(cmd.argv);
}


// To be called when we know with which user and on which hub this connection is.
static void handle_id(struct cc *cc, struct hub_user *u) {
  cc->nick = g_strdup(u->name);
  cc->isop = u->isop;
  if(cc->adc)
    memcpy(cc->cid, u->cid, 24);

  // Don't allow multiple connections from the same user.
  // Note: This is usually determined after receiving $Direction (in NMDC),
  // since it is possible to have two connections with a single user: One for
  // Upload and one for Download. But since we only support uploading, checking
  // it here is enough.
  if(cc_check_dupe(cc)) {
    g_set_error_literal(&(cc->err), 1, 0, "too many open connections with this user");
    cc_disconnect(cc);
    return;
  }

  // TODO: ADC should use CIDs here...
  if(!cc->adc)
    cc->slot_granted = g_hash_table_lookup(cc->hub->grants, cc->nick_raw) ? TRUE : FALSE;
}


static void nmdc_mynick(struct cc *cc, const char *nick) {
  if(cc->nick) {
    g_warning("Received a $MyNick from %s when we have already received one.", cc->nick);
    return;
  }

  cc->nick_raw = g_strdup(nick);

  if(!cc->hub)
    cc->hub = cc_get_hub(cc);
  if(!cc->hub) {
    g_warning("Received incoming connection from %s (%s), who is on none of the connected hubs.", nick, net_remoteaddr(cc->net));
    cc_disconnect(cc);
    return;
  }

  struct hub_user *u = g_hash_table_lookup(cc->hub->users, nick);
  if(!u) {
    g_set_error_literal(&(cc->err), 1, 0, "User is not on the hub");
    cc_disconnect(cc);
    return;
  }

  handle_id(cc, u);

  if(cc->active) {
    net_sendf(cc->net, "$MyNick %s", cc->hub->nick_hub);
    net_sendf(cc->net, "$Lock EXTENDEDPROTOCOL/wut? Pk=%s-%s", PACKAGE_NAME, PACKAGE_VERSION);
  }
}


static void nmdc_handle(struct cc *cc, char *cmd) {
  GMatchInfo *nfo;

  time(&(cc->last_action));
  g_clear_error(&(cc->err));

  // create regexes (declared statically, allocated/compiled on first call)
#define CMDREGEX(name, regex) \
  static GRegex * name = NULL;\
  if(!name) name = g_regex_new("\\$" regex, G_REGEX_OPTIMIZE|G_REGEX_ANCHORED|G_REGEX_DOTALL|G_REGEX_RAW, 0, NULL)

  CMDREGEX(mynick, "MyNick ([^ $]+)");
  CMDREGEX(lock, "Lock ([^ $]+) Pk=[^ $]+");
  CMDREGEX(supports, "Supports (.+)");
  CMDREGEX(adcget, "ADCGET ([^ ]+) (.+) ([0-9]+) (-?[0-9]+)");

  // $MyNick
  if(g_regex_match(mynick, cmd, 0, &nfo)) { // 1 = nick
    char *nick = g_match_info_fetch(nfo, 1);
    nmdc_mynick(cc, nick);
    if(ui_conn)
      ui_conn_listchange(cc->iter, UICONN_MOD);
    g_free(nick);
  }
  g_match_info_free(nfo);

  // $Lock
  if(g_regex_match(lock, cmd, 0, &nfo)) { // 1 = lock
    char *lock = g_match_info_fetch(nfo, 1);
    // we don't implement the classic NMDC get, so we can't talk with non-EXTENDEDPROTOCOL clients
    if(strncmp(lock, "EXTENDEDPROTOCOL", 16) != 0) {
      g_set_error_literal(&(cc->err), 1, 0, "Client does not support ADCGet");
      g_warning("C-C connection with %s (%s), but it does not support EXTENDEDPROTOCOL.", net_remoteaddr(cc->net), cc->nick);
      cc_disconnect(cc);
    } else {
      net_send(cc->net, "$Supports MiniSlots XmlBZList ADCGet TTHL TTHF");
      char *key = nmdc_lock2key(lock);
      net_send(cc->net, "$Direction Upload 0"); // we don't support downloading yet.
      net_sendf(cc->net, "$Key %s", key);
      g_free(key);
      g_free(lock);
    }
  }
  g_match_info_free(nfo);

  // $Supports
  if(g_regex_match(supports, cmd, 0, &nfo)) { // 1 = list
    char *list = g_match_info_fetch(nfo, 1);
    // Client must support ADCGet to download from us, since we haven't implemented the old NMDC $Get.
    if(!strstr(list, "ADCGet")) {
      g_set_error_literal(&(cc->err), 1, 0, "Client does not support ADCGet");
      g_warning("C-C connection with %s (%s), but it does not support ADCGet.", net_remoteaddr(cc->net), cc->nick);
      cc_disconnect(cc);
    }
    g_free(list);
  }
  g_match_info_free(nfo);

  // $ADCGET
  if(g_regex_match(adcget, cmd, 0, &nfo)) { // 1 = type, 2 = identifier, 3 = start_pos, 4 = bytes
    char *type = g_match_info_fetch(nfo, 1);
    char *id = g_match_info_fetch(nfo, 2);
    char *start = g_match_info_fetch(nfo, 3);
    char *bytes = g_match_info_fetch(nfo, 4);
    guint64 st = g_ascii_strtoull(start, NULL, 10);
    gint64 by = g_ascii_strtoll(bytes, NULL, 10);
    char *un_id = adc_unescape(id, TRUE);
    if(!cc->nick) {
      g_set_error_literal(&(cc->err), 1, 0, "Received $ADCGET before $MyNick");
      g_warning("Received $ADCGET before $MyNick, disconnecting client.");
      cc_disconnect(cc);
    } else if(un_id && g_utf8_validate(un_id, -1, NULL)) {
      GError *err = NULL;
      handle_adcget(cc, type, un_id, st, by, &err);
      if(err) {
        if(err->code != 53)
          net_sendf(cc->net, "$Error %s", err->message);
        else
          net_send(cc->net, "$MaxedOut");
        g_propagate_error(&(cc->err), err);
      }
    }
    g_free(un_id);
    g_free(type);
    g_free(id);
    g_free(start);
    g_free(bytes);
  }
  g_match_info_free(nfo);
}


static void handle_cmd(struct net *n, char *cmd) {
  struct cc *cc = n->handle;

  // TODO: for incoming connections, detect whether this ADC or NMDC
  if(cc->adc)
    adc_handle(cc, cmd);
  else
    nmdc_handle(cc, cmd);
}


// Hub may be unknown when this is an incoming connection
struct cc *cc_create(struct hub *hub) {
  struct cc *cc = g_new0(struct cc, 1);
  cc->net = net_create('|', cc, FALSE, handle_cmd, handle_error);
  cc->hub = hub;
  time(&(cc->last_action));
  cc->iter = g_sequence_append(cc_list, cc);
  if(ui_conn)
    ui_conn_listchange(cc->iter, UICONN_ADD);
  return cc;
}


static void handle_connect(struct net *n) {
  struct cc *cc = n->handle;
  time(&(cc->last_action));
  strncpy(cc->remoteaddr, net_remoteaddr(cc->net), 23);
  if(!cc->hub)
    cc_disconnect(cc);
  else if(cc->adc) {
    cc->state = ADC_S_PROTOCOL;
    net_send(n, "CSUP ADBASE ADTIGR ADBZIP");
    // TODO: send a "CSTA 000  RF<hub_url>"?
  } else {
    net_sendf(n, "$MyNick %s", cc->hub->nick_hub);
    net_sendf(n, "$Lock EXTENDEDPROTOCOL/wut? Pk=%s-%s", PACKAGE_NAME, PACKAGE_VERSION);
  }
}


void cc_nmdc_connect(struct cc *cc, const char *addr) {
  g_return_if_fail(!cc->timeout_src);
  strncpy(cc->remoteaddr, addr, 23);
  net_connect(cc->net, addr, 0, handle_connect);
  g_clear_error(&(cc->err));
}


void cc_adc_connect(struct cc *cc, struct hub_user *u, unsigned short port, char *token) {
  g_return_if_fail(!cc->timeout_src);
  g_return_if_fail(cc->hub);
  g_return_if_fail(u && u->active && u->ip4);
  cc->adc = TRUE;
  cc->token = g_strdup(token);
  cc->net->eom[0] = '\n';
  // build address
  strncpy(cc->remoteaddr, ip4_unpack(u->ip4), 23);
  char tmp[10];
  g_snprintf(tmp, 10, "%d", port);
  strncat(cc->remoteaddr, ":", 23-strlen(cc->remoteaddr));
  strncat(cc->remoteaddr, tmp, 23-strlen(cc->remoteaddr));
  // check / update user info
  handle_id(cc, u);
  // connect
  net_connect(cc->net, cc->remoteaddr, 0, handle_connect);
  g_clear_error(&(cc->err));
}


static void cc_incoming(struct cc *cc, GSocket *sock) {
  net_setsock(cc->net, sock);
  cc->active = TRUE;
  strncpy(cc->remoteaddr, net_remoteaddr(cc->net), 23);
}


static gboolean handle_timeout(gpointer dat) {
  cc_free(dat);
  return FALSE;
}


void cc_disconnect(struct cc *cc) {
  g_return_if_fail(!cc->timeout_src);
  time(&(cc->last_action));
  net_disconnect(cc->net);
  cc->timeout_src = g_timeout_add_seconds(30, handle_timeout, cc);
  g_free(cc->token);
  cc->token = NULL;
}


void cc_free(struct cc *cc) {
  if(!cc->timeout_src)
    cc_disconnect(cc);
  if(cc->timeout_src)
    g_source_remove(cc->timeout_src);
  if(ui_conn)
    ui_conn_listchange(cc->iter, UICONN_DEL);
  g_sequence_remove(cc->iter);
  net_unref(cc->net);
  g_error_free(cc->err);
  g_free(cc->nick_raw);
  g_free(cc->nick);
  g_free(cc->last_file);
  g_free(cc);
}





// Active mode

// listen socket. NULL if we aren't active.
GSocket *cc_listen = NULL;
char    *cc_listen_ip = NULL; // human-readable string. This is the remote IP, not the one we bind to.
guint16  cc_listen_port = 0;

static int cc_listen_src = 0;


// TODO: immediately send $MyINFO on A/P change?
void cc_listen_stop() {
  if(!cc_listen)
    return;
  g_free(cc_listen_ip);
  cc_listen_ip = NULL;
  g_source_remove(cc_listen_src);
  g_object_unref(cc_listen);
  cc_listen = FALSE;
}


static gboolean listen_handle(GSocket *sock, GIOCondition cond, gpointer dat) {
  GError *err = NULL;
  GSocket *s = g_socket_accept(sock, NULL, &err);
  if(!s) {
    if(err->code != G_IO_ERROR_WOULD_BLOCK) {
      ui_mf(ui_main, 0, "Listen error: %s. Switching to passive mode.", err->message);
      cc_listen_stop();
    }
    g_error_free(err);
    return FALSE;
  }
  cc_incoming(cc_create(NULL), s);
  return TRUE;
}


// more like a "restart()"
gboolean cc_listen_start() {
  GError *err = NULL;

  cc_listen_stop();
  if(!g_key_file_get_boolean(conf_file, "global", "active", NULL))
    return FALSE;

  // can be 0, in which case it'll be randomly assigned
  int port = g_key_file_get_integer(conf_file, "global", "active_port", NULL);

  // TODO: option to bind to a specific IP, for those who want that functionality
  GInetAddress *laddr = g_inet_address_new_any(G_SOCKET_FAMILY_IPV4);
  GSocketAddress *saddr = G_SOCKET_ADDRESS(g_inet_socket_address_new(laddr, port));
  g_object_unref(laddr);

  // create(), bind() and listen()
  GSocket *s = g_socket_new(G_SOCKET_FAMILY_IPV4, G_SOCKET_TYPE_STREAM, G_SOCKET_PROTOCOL_TCP, NULL);
  g_socket_set_blocking(s, FALSE);
  g_socket_bind(s, saddr, TRUE, &err);
  g_object_unref(saddr);
  if(err) {
    ui_mf(ui_main, 0, "Error creating listen socket: %s", err->message);
    g_error_free(err);
    g_object_unref(s);
    return FALSE;
  }
  if(!g_socket_listen(s, &err)) {
    ui_mf(ui_main, 0, "Error creating listen socket: %s", err->message);
    g_error_free(err);
    g_object_unref(s);
    return FALSE;
  }

  // attach incoming connections handler to the event loop
  GSource *src = g_socket_create_source(s, G_IO_IN, NULL);
  g_source_set_callback(src, (GSourceFunc)listen_handle, NULL, NULL);
  cc_listen_src = g_source_attach(src, NULL);
  g_source_unref(src);

  // set global variables
  cc_listen = s;
  cc_listen_ip = g_key_file_get_string(conf_file, "global", "active_ip", NULL);
  // actual listen port may be different from what we specified (notably when port = 0)
  GSocketAddress *addr = g_socket_get_local_address(s, NULL);
  cc_listen_port = g_inet_socket_address_get_port(G_INET_SOCKET_ADDRESS(addr));
  g_object_unref(addr);

  ui_mf(ui_main, 0, "Listening on port %d (%s).", cc_listen_port, cc_listen_ip);
  return TRUE;
}


