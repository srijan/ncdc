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


// List (well, table) of users who are granted a slot. Key = CID (g_strdup'ed),
// value = (void *)1. cc_init_global() is responsible for initializing it.
GHashTable *cc_granted = NULL;


void cc_grant(struct hub_user *u) {
  if(!g_hash_table_lookup(cc_granted, &(u->uid)))
    g_hash_table_insert(cc_granted, g_memdup(&(u->uid), 8), (void *)1);
}






// List of expected incoming or outgoing connections.  This is list managed by
// the functions below, in addition to cc_init_global() and cc_remove_hub(),

struct cc_expect {
  struct hub *hub;
  char *nick;   // NMDC, hub encoding. Also set on ADC, but only for debugging purposes
  guint64 uid;
  char cid[8];  // ADC
  char *token;  // ADC
  time_t added;
  int timeout_src;
  gboolean adc : 1;
  gboolean dl : 1;  // if we were the one starting the connection (i.e. we want to download)
};


static GQueue *cc_expected;


static void cc_expect_rm(GList *n, struct cc *success) {
  struct cc_expect *e = n->data;
  if(success && e->dl) {
    success->dl = TRUE;
    dl_queue_cc(e->uid, success);
  }
  if(e->dl)
    dl_queue_expect(e->uid, NULL);
  g_source_remove(e->timeout_src);
  g_free(e->token);
  g_free(e->nick);
  g_slice_free(struct cc_expect, e);
  g_queue_delete_link(cc_expected, n);
}


static gboolean cc_expect_timeout(gpointer data) {
  GList *n = data;
  struct cc_expect *e = n->data;
  g_message("Expected connection from %s on %s, but received none.", e->nick, e->hub->tab->name);
  cc_expect_rm(n, NULL);
  return FALSE;
}


void cc_expect_add(struct hub *hub, struct hub_user *u, char *t, gboolean dl) {
  struct cc_expect *e = g_slice_new0(struct cc_expect);
  e->adc = hub->adc;
  e->hub = hub;
  e->dl = dl;
  e->uid = u->uid;
  if(e->adc) {
    e->nick = g_strdup(u->name);
    memcpy(e->cid, u->cid, 8);
  } else
    e->nick = g_strdup(u->name_hub);
  if(t)
    e->token = g_strdup(t);
  if(e->dl)
    dl_queue_expect(e->uid, e);
  time(&(e->added));
  g_queue_push_tail(cc_expected, e);
  e->timeout_src = g_timeout_add_seconds_full(G_PRIORITY_LOW, 60, cc_expect_timeout, cc_expected->tail, NULL);
}


// Checks the expects list for the current connection, cc->dl, cc->uid and
// cc->hub and removes it from the expects list. cc->cid and cc->token must be
// known.
static gboolean cc_expect_adc_rm(struct cc *cc) {
  GList *n;
  for(n=cc_expected->head; n; n=n->next) {
    struct cc_expect *e = n->data;
    if(e->adc && memcmp(cc->cid, e->cid, 8) == 0 && strcmp(cc->token, e->token) == 0) {
      cc->uid = e->uid;
      cc->hub = e->hub;
      cc_expect_rm(n, cc);
      return TRUE;
    }
  }
  return FALSE;
}


// Same as above, but for NMDC. Sets cc->dl, cc->uid and cc->hub. cc->nick_raw
// must be known, and for passive connections cc->hub must also be known.
static gboolean cc_expect_nmdc_rm(struct cc *cc) {
  GList *n;
  for(n=cc_expected->head; n; n=n->next) {
    struct cc_expect *e = n->data;
    if(cc->hub && cc->hub != e->hub)
      continue;
    if(!e->adc && strcmp(e->nick, cc->nick_raw) == 0) {
      cc->hub = e->hub;
      cc->uid = e->uid;
      cc_expect_rm(n, cc);
      return TRUE;
    }
  }
  return FALSE;
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
  gboolean dl : 1;
  gboolean candl : 1;  // if cc_download() has been called at least once
  gboolean isdl : 1;   // if we're currently busy downloading something
  int dir;        // (NMDC) our direction. -1 = Upload, otherwise: Download $dir
  int state;      // (ADC)
  char cid[8];    // (ADC)
  int timeout_src;
  time_t last_action;
  char remoteaddr[24]; // xxx.xxx.xxx.xxx:ppppp
  char dl_hash[24];
  char *token;    // (ADC)
  char *last_file;
  char *tthl_dat;
  guint64 uid;
  guint64 last_size;
  guint64 last_length;
  guint64 last_offset;
  GError *err;
  GSequenceIter *iter;
};

#endif

// opened connections - ui_conn is responsible for the ordering
GSequence *cc_list;


void cc_init_global() {
  cc_expected = g_queue_new();
  cc_list = g_sequence_new(NULL);
  cc_granted = g_hash_table_new_full(g_int64_hash, g_int64_equal, g_free, NULL);
}


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
    if(e->hub == hub)
      cc_expect_rm(n, NULL);
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


// Returns the cc object of a connection with the same user, if there is one.
static struct cc *cc_check_dupe(struct cc *cc) {
  GSequenceIter *i = g_sequence_get_begin_iter(cc_list);
  for(; !g_sequence_iter_is_end(i); i=g_sequence_iter_next(i)) {
    struct cc *c = g_sequence_get(i);
    if(cc != c && !c->timeout_src && !!c->adc == !!cc->adc && c->uid == cc->uid)
      return c;
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


void cc_download(struct cc *cc) {
  cc->candl = TRUE;
  struct dl *dl = dl_queue_next(cc->uid);
  if(!dl)
    return;
  memcpy(cc->dl_hash, dl->hash, 24);
  // get virtual path
  char fn[45] = {};
  if(dl->islist)
    strcpy(fn, "files.xml.bz2"); // TODO: fallback for clients that don't support BZIP? (as if they exist...)
  else {
    strcpy(fn, "TTH/");
    base32_encode(dl->hash, fn+4);
  }

  // if we have not received TTHL data yet, request it
  if(!dl->islist && !dl->hastthl) {
    if(cc->adc)
      net_sendf(cc->net, "CGET tthl %s 0 -1", fn);
    else
      net_sendf(cc->net, "$ADCGET tthl %s 0 -1", fn);
  // otherwise, send GET request
  } else {
    if(cc->adc)
      net_sendf(cc->net, "CGET file %s %"G_GUINT64_FORMAT" -1", fn, dl->have);
    else
      net_sendf(cc->net, "$ADCGET file %s %"G_GUINT64_FORMAT" -1", fn, dl->have);
  }
  g_free(cc->last_file);
  cc->last_file = g_strdup(dl->islist ? "files.xml.bz2" : dl->dest);
  cc->last_offset = dl->have;
  cc->last_size = dl->size;
  cc->isdl = TRUE;
}


static void handle_recvfile(struct net *n, int read, char *buf, guint64 left) {
  struct cc *cc = n->handle;
  struct dl *dl = g_hash_table_lookup(dl_queue, cc->dl_hash);
  if(dl && !dl_received(dl, read, buf)) {
    g_set_error_literal(&cc->err, 1, 0, "Download error.");
    cc_disconnect(cc);
    return;
  }
  // If the item has been removed from the queue while there is still data left
  // to download, interrupt the download by forcing a disconnect.
  if(!dl && left)
    cc_disconnect(cc);
  // check for more stuff to download
  if(!left) {
    cc->isdl = FALSE;
    cc_download(cc);
  }
}


static void handle_recvtth(struct net *n, int read, char *buf, guint64 left) {
  struct cc *cc = n->handle;
  struct dl *dl = g_hash_table_lookup(dl_queue, cc->dl_hash);
  if(dl) {
    g_return_if_fail(read + left <= cc->last_length);
    memcpy(cc->tthl_dat+(cc->last_length-(left+read)), buf, read);
    if(!left)
      dl_settthl(dl, cc->tthl_dat, cc->last_length);
  }
  if(!left) {
    g_free(cc->tthl_dat);
    cc->tthl_dat = NULL;
    cc->isdl = FALSE;
    cc_download(cc);
  }
}


static void handle_adcsnd(struct cc *cc, gboolean tthl, guint64 start, guint64 bytes) {
  struct dl *dl = g_hash_table_lookup(dl_queue, cc->dl_hash);
  if(!dl) {
    cc_disconnect(cc);
    return;
  }
  cc->last_length = bytes;
  if(!tthl) {
    g_return_if_fail(dl->have == start);
    if(!dl->size)
      cc->last_size = dl->size = bytes;
    net_recvfile(cc->net, bytes, handle_recvfile);
  } else {
    g_return_if_fail(start == 0 && bytes > 0 && (bytes%24) == 0 && bytes < 48*1024);
    cc->tthl_dat = g_malloc(bytes);
    net_recvfile(cc->net, bytes, handle_recvtth);
  }
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
  char *vpath = NULL;
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
  struct stat st = {};
  if(!path || stat(path, &st) < 0 || !S_ISREG(st.st_mode) || start > st.st_size) {
    if(st.st_size && start > st.st_size)
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
  } else {
    g_set_error_literal(err, 1, 53, "No Slots Available");
    g_free(vpath);
  }
  g_free(path);
}


// To be called when we know with which user and on which hub this connection is.
static void handle_id(struct cc *cc, struct hub_user *u) {
  cc->nick = g_strdup(u->name);
  cc->isop = u->isop;
  cc->uid = u->uid;

  if(ui_conn)
    ui_conn_listchange(cc->iter, UICONN_MOD);

  if(cc->adc)
    memcpy(cc->cid, u->cid, 8);

  // Don't allow multiple connections with the same user for the same purpose
  // (up/down).  For NMDC, the purpose of this connection is determined when we
  // receive a $Direction, so it's only checked here for ADC.
  if(cc->adc) {
    struct cc *dup = cc_check_dupe(cc);
    if(dup && !!cc->dl == !!dup->dl) {
      g_set_error_literal(&(cc->err), 1, 0, "too many open connections with this user");
      cc_disconnect(cc);
      return;
    }
  }

  cc->slot_granted = g_hash_table_lookup(cc_granted, &(u->uid)) ? TRUE : FALSE;
}


static void adc_handle(struct cc *cc, char *msg) {
  struct adc_cmd cmd;
  GError *err = NULL;

  if(!msg[0])
    return;

  adc_parse(msg, &cmd, NULL, &err);
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
      char cid[40] = {};
      base32_encode(conf_cid, cid);
      adc_append(r, "ID", cid);
      if(!cc->active)
        adc_append(r, "TO", cc->token);
      net_send(cc->net, r->str);
      g_string_free(r, TRUE);
    }
    break;

  case ADCC_INF:
    if(cc->state == ADC_S_IDENTIFY) {
      cc->state = ADC_S_NORMAL;
      char *id = adc_getparam(cmd.argv, "ID", NULL);
      char *token = adc_getparam(cmd.argv, "TO", NULL);
      char cid[24];
      if(strlen(id) == 39)
        base32_decode(id, cid);
      if(!id || (cc->active && !token)) {
        g_warning("User did not sent a CID or token. (%s): %s", net_remoteaddr(cc->net), msg);
        cc_disconnect(cc);
      } else if(strlen(id) != 39 || (!cc->active && memcmp(cid, cc->cid, 8) != 0)) {
        g_warning("Incorrect CID. (%s): %s", net_remoteaddr(cc->net), msg);
        cc_disconnect(cc);
      } else if(cc->active) {
        cc->token = g_strdup(token);
        memcpy(cc->cid, cid, 8);
        cc_expect_adc_rm(cc);
        struct hub_user *u = cc->uid ? g_hash_table_lookup(hub_uids, &cc->uid) : NULL;
        if(!u) {
          g_warning("Unexpected ADC connection. (%s): %s", net_remoteaddr(cc->net), msg);
          cc_disconnect(cc);
        } else
          handle_id(cc, u);
      }
      if(cc->dl && cc->net->conn)
        cc_download(cc);
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

  case ADCC_SND:
    if(cc->state == ADC_S_NORMAL && cmd.argc >= 4)
      handle_adcsnd(cc, strcmp(cmd.argv[0], "tthl") == 0, g_ascii_strtoull(cmd.argv[2], NULL, 0), g_ascii_strtoull(cmd.argv[3], NULL, 0));
    break;

  case ADCC_STA:
    if(cmd.argc < 2 || strlen(cmd.argv[0]) != 3)
      g_message("Unknown command from %s: %s", net_remoteaddr(cc->net), msg);
    else if(cmd.argv[0][1] == '5' && cmd.argv[0][2] == '3') {
      // Make a "slots full" message fatal; dl.c assumes this behaviour.
      g_set_error_literal(&cc->err, 1, 0, "No Slots Available");
      cc_disconnect(cc);
    } else if(cmd.argv[0][1] == '5' && (cmd.argv[0][2] == '1' || cmd.argv[0][2] == '2') && cc->candl) {
      // File (Part) Not Available: notify dl.c
      struct dl *dl = g_hash_table_lookup(dl_queue, cc->dl_hash);
      if(dl)
        dl_queue_seterr(dl, DLE_NOFILE, 0);
      if(cmd.argv[0][0] == '2')
        cc_disconnect(cc);
      else
        cc_download(cc);
    } else if(cmd.argv[0][0] == '1' || cmd.argv[0][0] == '2') {
      g_set_error(&cc->err, 1, 0, "(%s) %s", cmd.argv[0], cmd.argv[1]);
      if(cmd.argv[0][0] == '2')
        cc_disconnect(cc);
    } else if(!adc_getparam(cmd.argv, "RF", NULL))
      g_message("Status: %s: (%s) %s", net_remoteaddr(cc->net), cmd.argv[0], cmd.argv[1]);
    break;

  default:
    g_message("Unknown command from %s: %s", net_remoteaddr(cc->net), msg);
  }

  g_strfreev(cmd.argv);
}


static void nmdc_mynick(struct cc *cc, const char *nick) {
  if(cc->nick) {
    g_warning("Received a $MyNick from %s when we have already received one.", cc->nick);
    return;
  }
  cc->nick_raw = g_strdup(nick);

  // check the expects list
  cc_expect_nmdc_rm(cc);

  // Normally the above function should figure out from which hub this
  // connection came. This is a fallback in the case it didn't (i.e. it's an
  // unexpected connection)
  if(!cc->hub) {
    GList *n;
    for(n=ui_tabs; n; n=n->next) {
      struct ui_tab *t = n->data;
      if(t->type == UIT_HUB && g_hash_table_lookup(t->hub->users, cc->nick_raw)) {
        g_warning("Unexpected incoming connection from %s", cc->nick_raw);
        cc->hub = t->hub;
      }
    }
  }

  // still not found? disconnect
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


static void nmdc_direction(struct cc *cc, gboolean down, int num) {
  gboolean old_dl = cc->dl;

  // if they want to download and we don't, then it's simple.
  if(down && !cc->dl)
    ;
  // if we want to download and they don't, then it's just as simple.
  else if(cc->dl && !down)
    ;
  // if neither of us wants to download... then what the heck are we doing?
  else if(!down && !cc->dl) {
    g_warning("Connection with %s (%s) while none of us wants to download.", cc->nick, net_remoteaddr(cc->net));
    g_set_error_literal(&(cc->err), 1, 0, "None wants to download.");
    cc_disconnect(cc);
    return;
  // if we both want to download and the numbers are equal... then fuck it!
  } else if(cc->dir == num) {
    g_warning("$Direction numbers with %s (%s) are equal!?", cc->nick, net_remoteaddr(cc->net));
    g_set_error_literal(&(cc->err), 1, 0, "$Direction numbers are equal.");
    cc_disconnect(cc);
    return;
  // if we both want to download and the numbers aren't equal, then check the numbers
  } else
    cc->dl = cc->dir > num;

  // Now that this connection has a purpose, make sure it's the only connection with that purpose.
  struct cc *dup = cc_check_dupe(cc);
  if(dup && !!cc->dl == !!dup->dl) {
    g_set_error_literal(&(cc->err), 1, 0, "too many open connections with this user");
    cc_disconnect(cc);
    return;
  }

  // If we wanted to download, but didn't get the chance to do so, notify the dl manager.
  if(old_dl && !cc->dl) {
    dl_queue_userdisconnect(cc->uid);
    dl_queue_cc(cc->uid, NULL);
  }

  // if we can download, do so!
  if(cc->dl)
    cc_download(cc);
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
  CMDREGEX(direction, "Direction (Download|Upload) ([0-9]+)");
  CMDREGEX(adcget, "ADCGET ([^ ]+) (.+) ([0-9]+) (-?[0-9]+)");
  CMDREGEX(adcsnd, "ADCSND (file|tthl) .+ ([0-9]+) (-?[0-9]+)");
  CMDREGEX(error, "Error (.+)");
  CMDREGEX(maxedout, "MaxedOut");

  // $MyNick
  if(g_regex_match(mynick, cmd, 0, &nfo)) { // 1 = nick
    char *nick = g_match_info_fetch(nfo, 1);
    nmdc_mynick(cc, nick);
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
      cc->dir = cc->dl ? g_random_int_range(0, 65535) : -1;
      net_sendf(cc->net, "$Direction %s %d", cc->dl ? "Download" : "Upload", cc->dl ? cc->dir : 0);
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

  // $Direction
  if(g_regex_match(direction, cmd, 0, &nfo)) { // 1 = dir, 2 = num
    char *dir = g_match_info_fetch(nfo, 1);
    char *num = g_match_info_fetch(nfo, 2);
    nmdc_direction(cc, strcmp(dir, "Download") == 0, strtol(num, NULL, 10));
    g_free(dir);
    g_free(num);
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

  // $ADCSND
  if(g_regex_match(adcsnd, cmd, 0, &nfo)) { // 1 = file/tthl, 2 = start_pos, 3 = bytes
    char *type = g_match_info_fetch(nfo, 1);
    char *start = g_match_info_fetch(nfo, 2);
    char *bytes = g_match_info_fetch(nfo, 3);
    handle_adcsnd(cc, strcmp(type, "tthl") == 0, g_ascii_strtoull(start, NULL, 10), g_ascii_strtoull(bytes, NULL, 10));
    g_free(type);
    g_free(start);
    g_free(bytes);
  }
  g_match_info_free(nfo);

  // $Error
  if(g_regex_match(error, cmd, 0, &nfo)) { // 1 = message
    char *msg = g_match_info_fetch(nfo, 1);
    g_set_error(&cc->err, 1, 0, msg);
    // Handle "File Not Available" and ".. no more exists"
    if(cc->candl && (str_casestr(msg, "file not available") || str_casestr(msg, "no more exists"))) {
      struct dl *dl = g_hash_table_lookup(dl_queue, cc->dl_hash);
      if(dl)
        dl_queue_seterr(dl, DLE_NOFILE, 0);
      cc_download(cc);
    }
    g_free(msg);
  }
  g_match_info_free(nfo);

  // $MaxedOut
  if(g_regex_match(maxedout, cmd, 0, &nfo)) {
    g_set_error_literal(&cc->err, 1, 0, "No Slots Available");
    cc_disconnect(cc);
  }
  g_match_info_free(nfo);
}


static void handle_cmd(struct net *n, char *cmd) {
  struct cc *cc = n->handle;

  // No input is allowed while we're sending file data.
  if(cc->net->file_left) {
    g_message("Received message from %s while we're sending a file.", net_remoteaddr(cc->net));
    g_set_error_literal(&cc->err, 1, 0, "Received message in upload state.");
    cc_disconnect(cc);
    return;
  }

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
    // Note that while http://www.adcportal.com/wiki/REF says we should send
    // the hostname used to connect to the hub, the actual IP is easier to get
    // in our case. I personally don't see how having a hostname is better than
    // having an actual IP, but an attacked user who gets incoming connections
    // from both ncdc and other clients now knows both the DNS *and* the IP of
    // the hub. :-)
    net_sendf(n, "CSTA 000 referrer RFadc://%s", net_remoteaddr(cc->hub->net));
  } else {
    net_sendf(n, "$MyNick %s", cc->hub->nick_hub);
    net_sendf(n, "$Lock EXTENDEDPROTOCOL/wut? Pk=%s-%s,Ref=%s", PACKAGE_NAME, PACKAGE_VERSION, net_remoteaddr(cc->hub->net));
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
  memcpy(cc->cid, u->cid, 8);
  cc->net->eom[0] = '\n';
  // build address
  strncpy(cc->remoteaddr, ip4_unpack(u->ip4), 23);
  char tmp[10];
  g_snprintf(tmp, 10, "%d", port);
  strncat(cc->remoteaddr, ":", 23-strlen(cc->remoteaddr));
  strncat(cc->remoteaddr, tmp, 23-strlen(cc->remoteaddr));
  // check whether this was as a reply to a RCM from us
  cc_expect_adc_rm(cc);
  // check / update user info
  handle_id(cc, u);
  // handle_id() can do a cc_disconnect() when it discovers a duplicate
  // connection. This will reset cc->token, and we should stop this connection
  // attempt.
  if(!cc->token)
    return;
  // connect
  net_connect(cc->net, cc->remoteaddr, 0, handle_connect);
  g_clear_error(&(cc->err));
}


static void handle_detectprotocol(struct net *net, char *dat) {
  struct cc *cc = net->handle;
  net->cb_datain = NULL;
  if(dat[0] == 'C') {
    cc->adc = TRUE;
    net->eom[0] = '\n';
  }
  // otherwise, assume defaults (= NMDC)
}


static void cc_incoming(struct cc *cc, GSocket *sock) {
  net_setsock(cc->net, sock);
  cc->active = TRUE;
  cc->net->cb_datain = handle_detectprotocol;
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
  cc->timeout_src = g_timeout_add_seconds(60, handle_timeout, cc);
  g_free(cc->token);
  cc->token = NULL;
  cc->isdl = FALSE;
  if(cc->dl && cc->uid)
    dl_queue_userdisconnect(cc->uid);
}


void cc_free(struct cc *cc) {
  if(!cc->timeout_src)
    cc_disconnect(cc);
  if(cc->timeout_src)
    g_source_remove(cc->timeout_src);
  if(ui_conn)
    ui_conn_listchange(cc->iter, UICONN_DEL);
  if(cc->dl && cc->uid)
    dl_queue_cc(cc->uid, NULL);
  g_sequence_remove(cc->iter);
  net_unref(cc->net);
  if(cc->err)
    g_error_free(cc->err);
  g_free(cc->tthl_dat);
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


static void cc_listen_stop() {
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
      hub_global_nfochange();
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
  if(!g_key_file_get_boolean(conf_file, "global", "active", NULL)) {
    hub_global_nfochange();
    return FALSE;
  }

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
  hub_global_nfochange();
  return TRUE;
}


