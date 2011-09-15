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
#if TLS_SUPPORT
  char *kp;     // ADC - slice-alloc'ed with 32 bytes
#endif
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
#if TLS_SUPPORT
  if(e->kp)
    g_slice_free1(32, e->kp);
#endif
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
#if TLS_SUPPORT
  if(u->kp) {
    e->kp = g_slice_alloc(32);
    memcpy(e->kp, u->kp, 32);
  }
#endif
  if(t)
    e->token = g_strdup(t);
  if(e->dl)
    dl_queue_expect(e->uid, e);
  time(&(e->added));
  g_queue_push_tail(cc_expected, e);
  e->timeout_src = g_timeout_add_seconds_full(G_PRIORITY_LOW, 60, cc_expect_timeout, cc_expected->tail, NULL);
}


// Checks the expects list for the current connection, sets cc->dl, cc->uid,
// cc->hub and cc->kp_user and removes it from the expects list. cc->cid and
// cc->token must be known.
static gboolean cc_expect_adc_rm(struct cc *cc) {
  GList *n;
  for(n=cc_expected->head; n; n=n->next) {
    struct cc_expect *e = n->data;
    if(e->adc && memcmp(cc->cid, e->cid, 8) == 0 && strcmp(cc->token, e->token) == 0) {
      cc->uid = e->uid;
      cc->hub = e->hub;
#if TLS_SUPPORT
      cc->kp_user = e->kp;
      e->kp = NULL;
#endif
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




// Throttling of GET file offset, for buggy clients that keep requesting the
// same file+offset. Throttled to 1 request per hour, with an allowed burst of 10.

#define THROTTLE_INTV 3600
#define THROTTLE_BURST 10

struct throttle_get {
  char tth[24];
  guint64 uid;
  guint64 offset;
  time_t throttle;
};

static GHashTable *throttle_list; // initialized in cc_init_global()


static guint throttle_hash(gconstpointer key) {
  const struct throttle_get *t = key;
  guint *tth = (guint *)t->tth;
  return *tth + (gint)t->offset + (gint)t->uid;
}


static gboolean throttle_equal(gconstpointer a, gconstpointer b) {
  const struct throttle_get *x = a;
  const struct throttle_get *y = b;
  return x->uid == y->uid && memcmp(x->tth, y->tth, 24) == 0 && x->offset == y->offset;
}


static void throttle_free(gpointer dat) {
  g_slice_free(struct throttle_get, dat);
}


static gboolean throttle_check(struct cc *cc, char *tth, guint64 offset) {
  // construct a key
  struct throttle_get key;
  memcpy(key.tth, tth, 24);
  key.uid = cc->uid;
  key.offset = offset;
  time(&key.throttle);

  // lookup
  struct throttle_get *val = g_hash_table_lookup(throttle_list, &key);
  // value present and above threshold, throttle!
  if(val && val->throttle-key.throttle > THROTTLE_BURST*THROTTLE_INTV)
    return TRUE;
  // value present and below threshold, update throttle value
  if(val) {
    val->throttle = MAX(key.throttle, val->throttle+THROTTLE_INTV);
    return FALSE;
  }
  // value not present, add it
  val = g_slice_dup(struct throttle_get, &key);
  g_hash_table_insert(throttle_list, val, val);
  return FALSE;
}


static gboolean throttle_purge_func(gpointer key, gpointer val, gpointer dat) {
  struct throttle_get *v = val;
  time_t *t = dat;
  return v->throttle < *t ? TRUE : FALSE;
}


// Purge old throttle items from the throttle_list. Called from a timer that is
// initialized in cc_init_global().
static gboolean throttle_purge(gpointer dat) {
  time_t t = time(NULL);
  int r = g_hash_table_foreach_remove(throttle_list, throttle_purge_func, &t);
  g_debug("throttle_purge: Purged %d items, %d items left.", r, g_hash_table_size(throttle_list));
  return TRUE;
}





// Main C-C objects

#if INTERFACE

// States
#define CCS_CONN       0
#define CCS_HANDSHAKE  1
#define CCS_IDLE       2
#define CCS_TRANSFER   3 // check cc->dl whether it's up or down
#define CCS_DISCONN    4 // waiting to get removed on a timeout

struct cc {
  struct net *net;
  struct hub *hub;
  char *nick_raw; // (NMDC)
  char *nick;
  char *hub_name; // Copy of hub->tab->name when hub is reset to NULL
  gboolean adc : 1;
  gboolean active : 1;
  gboolean isop : 1;
  gboolean slot_mini : 1;
  gboolean slot_granted : 1;
  gboolean dl : 1;
  int dir;        // (NMDC) our direction. -1 = Upload, otherwise: Download $dir
  int state;
  char cid[24];   // (ADC) only the first 8 bytes are used for checking,
                  // but the full 24 bytes are stored after receiving CINF (for logging)
  int timeout_src;
  char remoteaddr[24]; // xxx.xxx.xxx.xxx:ppppp
  char *token;    // (ADC)
  char *last_file;
  char *tthl_dat;
  guint64 uid;
  guint64 last_size;
  guint64 last_length;
  guint64 last_offset;
  time_t last_start;
  char last_hash[24];
#if TLS_SUPPORT
  char *kp_real;  // (ADC) slice-alloc'ed with 32 bytes. This is the actually calculated keyprint.
  char *kp_user;  // (ADC) This is the keyprint from the users' INF
#endif
  GError *err;
  GSequenceIter *iter;
};

#endif

/* State machine:

  Event                     allowed states     next states

 Generic init:
  cc_create                 -                  conn
  incoming connection       conn               handshake
  hub-initiated connect     conn               conn
  connected after ^         conn               handshake

 NMDC:
  $MaxedOut                 transfer_d         disconn
  $Error                    transfer_d         idle_d [1]
  $ADCSND                   transfer_d         transfer_d
  $ADCGET                   idle_u             transfer_u
  $Direction                handshake          idle_[ud] [1]
  $Supports                 handshake          handshake
  $Lock                     handshake          handshake
  $MyNick                   handshake          handshake

 ADC:
  SUP                       handshake          handshake
  INF                       handshake          idle_[ud] [1]
  GET                       idle_u             transfer_u
  SND                       transfer_d         transfer_d
  STA x53 (slots full)      transfer_d         disconn
  STA x5[12] (file error)   transfer_d         idle_d or disconn
  STA other                 any                no change or disconn

 Generic other:
  transfer complete         transfer_[ud]      idle_[ud] [1]
  any protocol error        any                disconn
  network error[2]          any                disconn
  user disconnect           any                disconn
  dl.c wants download       idle_d             transfer_d


  [1] possibly immediately followed by transfer_d if cc->dl and we have
      something to download.
  [2] includes the idle timeout.

  Note that the ADC protocol distinguishes between "protocol" and "identify", I
  combined that into a single "handshake" state since NMDC lacks something
  similar.

  Also note that the "transfer" state does not mean that a file is actually
  being sent over the network: when downloading, it also refers to the period
  of initiating the download (i.e. a GET has been sent and we're waiting for a
  SND).

  The _d and _u suffixes relate to the value of cc->dl, and is relevant for the
  idle and transfer states.

  Exchanging TTHL data is handled differently with uploading and downloading:
  With uploading it is done in a single call to net_send_raw(), and as such the
  transfer_u state will not be used. Downloading, on the other hand, uses
  net_recvfile(), and the cc instance will stay in the transfer_d state until
  the TTHL data has been fully received.
*/



// opened connections - ui_conn is responsible for the ordering
GSequence *cc_list;


void cc_init_global() {
  cc_expected = g_queue_new();
  cc_list = g_sequence_new(NULL);
  cc_granted = g_hash_table_new_full(g_int64_hash, g_int64_equal, g_free, NULL);

  throttle_list = g_hash_table_new_full(throttle_hash, throttle_equal, NULL, throttle_free);
  g_timeout_add_seconds_full(G_PRIORITY_LOW, 600, throttle_purge, NULL, NULL);
}


// Calls cc_disconnect() on every open cc connection. This makes sure that any
// current transfers are aborted and logged to the transfer log.
void cc_close_global() {
  GSequenceIter *i = g_sequence_get_begin_iter(cc_list);
  for(; !g_sequence_iter_is_end(i); i=g_sequence_iter_next(i)) {
    struct cc *c = g_sequence_get(i);
    if(c->state != CCS_DISCONN)
      cc_disconnect(c);
  }
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
    if(c->hub == hub) {
      c->hub_name = g_strdup(hub->tab->name);
      c->hub = NULL;
    }
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
    if(!c->dl && c->state == CCS_TRANSFER)
      num++;
    if(!c->dl && c->state == CCS_TRANSFER && c->slot_mini)
      m++;
  }
  if(mini)
    *mini = m;
  return num;
}



// To be called when an upload or download has finished. Will get info from the
// cc struct and write it to the transfer log.
static void xfer_log_add(struct cc *cc) {
  g_return_if_fail(cc->state == CCS_TRANSFER && cc->last_file);
  // we don't log tthl transfers or transfers that hadn't been started yet
  if(cc->tthl_dat || !cc->last_length)
    return;

  char *key = cc->dl ? "log_downloads" : "log_uploads";
  if(g_key_file_has_key(conf_file, "log", key, NULL) && !g_key_file_get_boolean(conf_file, "log", key, NULL))
    return;

  static struct logfile *log = NULL;
  if(!log)
    log = logfile_create("transfers");

  char cid[40] = {};
  if(cc->adc)
    base32_encode(cc->cid, cid);
  else
    strcpy(cid, "-");

  char tth[40] = {};
  if(strcmp(cc->last_file, "files.xml.bz2") == 0)
    strcpy(tth, "-");
  else
    base32_encode(cc->last_hash, tth);

  guint64 transfer_size = cc->last_length - (cc->dl ? cc->net->recv_raw_left : net_file_left(cc->net));

  char *nick = adc_escape(cc->nick, FALSE);
  char *file = adc_escape(cc->last_file, FALSE);

  char *tmp = strchr(cc->remoteaddr, ':');
  if(tmp)
    *tmp = 0;

  char *msg = g_strdup_printf("%s %s %s %s %c %c %s %d %"G_GUINT64_FORMAT" %"G_GUINT64_FORMAT" %"G_GUINT64_FORMAT" %s",
    cc->hub ? cc->hub->tab->name : cc->hub_name, cid, nick, cc->remoteaddr, cc->dl ? 'd' : 'u',
    transfer_size == cc->last_length ? 'c' : 'i', tth, (int)(time(NULL)-cc->last_start),
    cc->last_size, cc->last_offset, transfer_size, file);
  logfile_add(log, msg);
  g_free(msg);

  if(tmp)
    *tmp = ':';
  g_free(nick);
  g_free(file);
}


// Returns the cc object of a connection with the same user, if there is one.
static struct cc *cc_check_dupe(struct cc *cc) {
  GSequenceIter *i = g_sequence_get_begin_iter(cc_list);
  for(; !g_sequence_iter_is_end(i); i=g_sequence_iter_next(i)) {
    struct cc *c = g_sequence_get(i);
    if(cc != c && c->state != CCS_DISCONN && !!c->adc == !!cc->adc && c->uid == cc->uid)
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
  g_return_if_fail(cc->state == CCS_IDLE && cc->dl);
  struct dl *dl = dl_queue_next(cc->uid);
  if(!dl)
    return;
  memcpy(cc->last_hash, dl->hash, 24);
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
  cc->last_length = 0; // to be filled in handle_adcsnd()
  cc->state = CCS_TRANSFER;
}


static void handle_recvfile(struct net *n, char *buf, int read, guint64 left) {
  struct cc *cc = n->handle;
  struct dl *dl = g_hash_table_lookup(dl_queue, cc->last_hash);
  if(dl && !dl_received(dl, buf, read)) {
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
    xfer_log_add(cc);
    cc->state = CCS_IDLE;
    cc_download(cc);
  }
}


static void handle_recvtth(struct net *n, char *buf, int read, guint64 left) {
  struct cc *cc = n->handle;
  struct dl *dl = g_hash_table_lookup(dl_queue, cc->last_hash);
  if(dl) {
    g_return_if_fail(read + left <= cc->last_length);
    memcpy(cc->tthl_dat+(cc->last_length-(left+read)), buf, read);
    if(!left)
      dl_settthl(dl, cc->tthl_dat, cc->last_length);
  }
  if(!left) {
    g_free(cc->tthl_dat);
    cc->tthl_dat = NULL;
    cc->state = CCS_IDLE;
    cc_download(cc);
  }
}


static void handle_adcsnd(struct cc *cc, gboolean tthl, guint64 start, gint64 bytes) {
  struct dl *dl = g_hash_table_lookup(dl_queue, cc->last_hash);
  if(!dl) {
    g_set_error_literal(&cc->err, 1, 0, "Download interrupted.");
    cc_disconnect(cc);
    return;
  }
  // Some buggy clients (DCGUI) return $ADCSND with bytes = -1. They probably
  // mean "all of the remaining bytes of the file" with that.
  if(bytes < 0) {
    if(tthl) {
      g_set_error_literal(&cc->err, 1, 0, "Protocol error.");
      cc_disconnect(cc);
      return;
    }
    bytes = cc->last_size - cc->last_offset;
  }

  cc->last_length = bytes;
  if(!tthl) {
    g_return_if_fail(dl->have == start);
    if(!dl->size)
      cc->last_size = dl->size = bytes;
    net_recvraw(cc->net, bytes, handle_recvfile);
  } else {
    g_return_if_fail(start == 0 && bytes > 0 && (bytes%24) == 0 && bytes < 48*1024);
    cc->tthl_dat = g_malloc(bytes);
    net_recvraw(cc->net, bytes, handle_recvtth);
  }
  time(&cc->last_start);
}


static void handle_sendcomplete(struct net *net) {
  struct cc *cc = net->handle;
  xfer_log_add(cc);
  cc->state = CCS_IDLE;
}


// err->code:
//  40: Generic protocol error
//  50: Generic internal error
//  51: File not available
//  53: No slots
// Handles both ADC GET and the NMDC $ADCGET.
static void handle_adcget(struct cc *cc, char *type, char *id, guint64 start, gint64 bytes, GError **err) {
  // tthl
  if(strcmp(type, "tthl") == 0) {
    if(strncmp(id, "TTH/", 4) != 0 || !istth(id+4) || start != 0 || bytes != -1) {
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
      net_sendraw(cc->net, dat, len);
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
    net_sendraw(cc->net, buf->str, buf->len);
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
  } else if(strncmp(id, "TTH/", 4) == 0 && istth(id+4)) {
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

  if(f && throttle_check(cc, f->tth, start)) {
    g_message("CC:%s: File upload throttled: %s offset %"G_GUINT64_FORMAT, net_remoteaddr(cc->net), vpath, start);
    g_set_error_literal(err, 1, 50, "Action throttled");
    g_free(path);
    g_free(vpath);
    return;
  }

  // send
  if(request_slot(cc, needslot)) {
    g_free(cc->last_file);
    cc->last_file = vpath;
    cc->last_length = MIN(bytes, G_MAXINT-1);
    cc->last_offset = start;
    cc->last_size = st.st_size;
    if(f)
      memcpy(cc->last_hash, f->tth, 24);
    char *tmp = adc_escape(id, !cc->adc);
    net_sendf(cc->net,
      cc->adc ? "CSND file %s %"G_GUINT64_FORMAT" %"G_GINT64_FORMAT : "$ADCSND file %s %"G_GUINT64_FORMAT" %"G_GINT64_FORMAT,
      tmp, start, bytes);
    net_sendfile(cc->net, path, start, bytes, handle_sendcomplete);
    g_free(tmp);
    cc->state = CCS_TRANSFER;
    time(&cc->last_start);
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
    g_message("CC:%s: ADC parse error: %s. --> %s", net_remoteaddr(cc->net), err->message, msg);
    g_error_free(err);
    return;
  }

  if(cmd.type != 'C') {
    g_message("CC:%s: Not a client command: %s. --> %s", net_remoteaddr(cc->net), err->message, msg);
    g_strfreev(cmd.argv);
    return;
  }

  switch(cmd.cmd) {

  case ADCC_SUP:
    if(cc->state != CCS_HANDSHAKE) {
      g_set_error_literal(&cc->err, 1, 0, "Protocol error.");
      g_message("CC:%s: Received message in wrong state: %s", net_remoteaddr(cc->net), msg);
      cc_disconnect(cc);
    } else {
      // TODO: actually do something with the arguments.
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
    if(cc->state != CCS_HANDSHAKE) {
      g_set_error_literal(&cc->err, 1, 0, "Protocol error.");
      g_message("CC:%s: Received message in wrong state: %s", net_remoteaddr(cc->net), msg);
      cc_disconnect(cc);
    } else {
      cc->state = CCS_IDLE;;
      char *id = adc_getparam(cmd.argv, "ID", NULL);
      char *token = adc_getparam(cmd.argv, "TO", NULL);
      char cid[24];
      if(istth(id))
        base32_decode(id, cid);
      if(!id || (cc->active && !token)) {
        g_set_error_literal(&cc->err, 1, 0, "Protocol error.");
        g_warning("CC:%s: No token or CID present: %s", net_remoteaddr(cc->net), msg);
        cc_disconnect(cc);
      } else if(!istth(id) || (!cc->active && memcmp(cid, cc->cid, 8) != 0)) {
        g_set_error_literal(&cc->err, 1, 0, "Protocol error.");
        g_warning("CC:%s: Incorrect CID: %s", net_remoteaddr(cc->net), msg);
        cc_disconnect(cc);
      } else if(cc->active) {
        cc->token = g_strdup(token);
        memcpy(cc->cid, cid, 24);
        cc_expect_adc_rm(cc);
        struct hub_user *u = cc->uid ? g_hash_table_lookup(hub_uids, &cc->uid) : NULL;
        if(!u) {
          g_set_error_literal(&cc->err, 1, 0, "Protocol error.");
          g_warning("CC:%s: Unexpected ADC connection: %s", net_remoteaddr(cc->net), msg);
          cc_disconnect(cc);
        } else
          handle_id(cc, u);
      } else
        memcpy(cc->cid, cid, 24);
      // Perform keyprint validation
#if TLS_SUPPORT
      if(cc->kp_real && cc->kp_user && memcmp(cc->kp_real, cc->kp_user, 32) != 0) {
        g_set_error_literal(&cc->err, 1, 0, "Protocol error.");
        char user[53] = {}, real[53] = {};
        base32_encode_dat(cc->kp_user, user, 32);
        base32_encode_dat(cc->kp_real, real, 32);
        g_warning("CC:%s: Client keyprint does not match TLS keyprint: %s != %s", net_remoteaddr(cc->net), user, real);
        cc_disconnect(cc);
      } else if(cc->kp_real && cc->kp_user)
        g_debug("CC:%s: Client authenticated using KEYP.", net_remoteaddr(cc->net));
#endif
      if(cc->dl && cc->state == CCS_IDLE)
        cc_download(cc);
    }
    break;

  case ADCC_GET:
    if(cmd.argc < 4) {
      g_message("CC:%s: Invalid command: %s", net_remoteaddr(cc->net), msg);
    } else if(cc->dl || cc->state != CCS_IDLE) {
      g_set_error_literal(&cc->err, 1, 0, "Protocol error.");
      g_message("CC:%s: Received message in wrong state: %s", net_remoteaddr(cc->net), msg);
      cc_disconnect(cc);
    } else {
      guint64 start = g_ascii_strtoull(cmd.argv[2], NULL, 0);
      gint64 len = g_ascii_strtoll(cmd.argv[3], NULL, 0);
      GError *err = NULL;
      handle_adcget(cc, cmd.argv[0], cmd.argv[1], start, len, &err);
      if(err) {
        GString *r = adc_generate('C', ADCC_STA, 0, 0);
        g_string_append_printf(r, " 1%02d", err->code);
        adc_append(r, NULL, err->message);
        net_send(cc->net, r->str);
        g_string_free(r, TRUE);
        g_propagate_error(&cc->err, err);
      }
    }
    break;

  case ADCC_SND:
    if(cmd.argc < 4) {
      g_message("CC:%s: Invalid command: %s", net_remoteaddr(cc->net), msg);
    } else if(!cc->dl || cc->state != CCS_TRANSFER) {
      g_set_error_literal(&cc->err, 1, 0, "Protocol error.");
      g_message("CC:%s: Received message in wrong state: %s", net_remoteaddr(cc->net), msg);
      cc_disconnect(cc);
    } else
      handle_adcsnd(cc, strcmp(cmd.argv[0], "tthl") == 0, g_ascii_strtoull(cmd.argv[2], NULL, 0), g_ascii_strtoll(cmd.argv[3], NULL, 0));
    break;

  case ADCC_GFI:
    if(cmd.argc < 2 || strcmp(cmd.argv[0], "file") != 0) {
      g_message("CC:%s: Invalid command: %s", net_remoteaddr(cc->net), msg);
    } else if(cc->dl || cc->state != CCS_IDLE) {
      g_set_error_literal(&cc->err, 1, 0, "Protocol error.");
      g_message("CC:%s: Received message in wrong state: %s", net_remoteaddr(cc->net), msg);
      cc_disconnect(cc);
    } else {
      // Get file
      struct fl_list *f = NULL;
      if(cmd.argv[1][0] == '/' && fl_local_list) {
        f = fl_list_from_path(fl_local_list, cmd.argv[1]);
      } else if(strncmp(cmd.argv[1], "TTH/", 4) == 0 && istth(cmd.argv[1]+4)) {
        char root[24];
        base32_decode(cmd.argv[1]+4, root);
        GSList *l = fl_local_from_tth(root);
        f = l ? l->data : NULL;
      }
      // Generate response
      GString *r;
      if(!f) {
        r = adc_generate('C', ADCC_STA, 0, 0);
        g_string_append_printf(r, " 151 File Not Available");
      } else {
        r = adc_generate('C', ADCC_RES, 0, 0);
        g_string_append_printf(r, " SL%d SI%"G_GUINT64_FORMAT, conf_slots() - cc_slots_in_use(NULL), f->size);
        char *path = fl_list_path(f);
        adc_append(r, "FN", path);
        g_free(path);
        if(f->isfile) {
          char tth[40] = {};
          base32_encode(f->tth, tth);
          g_string_append_printf(r, " TR%s", tth);
        } else
          g_string_append_c(r, '/');
      }
      net_send(cc->net, r->str);
      g_string_free(r, TRUE);
    }
    break;

  case ADCC_STA:
    if(cmd.argc < 2 || strlen(cmd.argv[0]) != 3) {
      g_message("CC:%s: Invalid command: %s", net_remoteaddr(cc->net), msg);
      // Don't disconnect here for compatibility with old DC++ cores that
      // incorrectly send "0" instead of "000" as first argument.

    // Slots full
    } else if(cmd.argv[0][1] == '5' && cmd.argv[0][2] == '3') {
      if(!cc->dl || cc->state != CCS_TRANSFER) {
        g_set_error_literal(&cc->err, 1, 0, "Protocol error.");
        g_message("CC:%s: Received message in wrong state: %s", net_remoteaddr(cc->net), msg);
        cc_disconnect(cc);
      } else {
        // Make a "slots full" message fatal; dl.c assumes this behaviour.
        g_set_error_literal(&cc->err, 1, 0, "No Slots Available");
        cc_disconnect(cc);
      }

    // File (Part) Not Available: notify dl.c
    } else if(cmd.argv[0][1] == '5' && (cmd.argv[0][2] == '1' || cmd.argv[0][2] == '2')) {
      if(!cc->dl || cc->state != CCS_TRANSFER) {
        g_set_error_literal(&cc->err, 1, 0, "Protocol error.");
        g_message("CC:%s: Received message in wrong state: %s", net_remoteaddr(cc->net), msg);
        cc_disconnect(cc);
      } else {
        struct dl *dl = g_hash_table_lookup(dl_queue, cc->last_hash);
        if(dl)
          dl_queue_seterr(dl, DLE_NOFILE, 0);
        if(cmd.argv[0][0] == '2')
          cc_disconnect(cc);
        else {
          cc->state = CCS_IDLE;
          cc_download(cc);
        }
      }

    // Other message
    } else if(cmd.argv[0][0] == '1' || cmd.argv[0][0] == '2') {
      g_set_error(&cc->err, 1, 0, "(%s) %s", cmd.argv[0], cmd.argv[1]);
      if(cmd.argv[0][0] == '2')
        cc_disconnect(cc);
    } else if(!adc_getparam(cmd.argv, "RF", NULL))
      g_message("CC:%s: Status: (%s) %s", net_remoteaddr(cc->net), cmd.argv[0], cmd.argv[1]);
    break;

  default:
    g_message("CC:%s: Unknown command: %s", net_remoteaddr(cc->net), msg);
  }

  g_strfreev(cmd.argv);
}


static void nmdc_mynick(struct cc *cc, const char *nick) {
  if(cc->nick_raw) {
    g_message("CC:%s: Received $MyNick twice.", net_remoteaddr(cc->net));
    cc_disconnect(cc);
    return;
  }
  cc->nick_raw = g_strdup(nick);

  // check the expects list
  cc_expect_nmdc_rm(cc);

  // Normally the above function should figure out from which hub this
  // connection came. This is a fallback in the case it didn't (i.e. it's an
  // unexpected connection)
  // TODO: remove this fallback and simply disallow unexpected connections
  if(!cc->hub) {
    GList *n;
    for(n=ui_tabs; n; n=n->next) {
      struct ui_tab *t = n->data;
      if(t->type == UIT_HUB && g_hash_table_lookup(t->hub->users, cc->nick_raw)) {
        g_warning("CC:%s: Unexpected incoming connection from %s", net_remoteaddr(cc->net), cc->nick_raw);
        cc->hub = t->hub;
      }
    }
  }

  // still not found? disconnect
  if(!cc->hub) {
    g_message("CC:%s: Received incoming connection from %s, who is on none of the connected hubs.", net_remoteaddr(cc->net), nick);
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
    g_warning("CC:%s: None of us wants to download.", net_remoteaddr(cc->net));
    g_set_error_literal(&cc->err, 1, 0, "Protocol error.");
    cc_disconnect(cc);
    return;
  // if we both want to download and the numbers are equal... then fuck it!
  } else if(cc->dir == num) {
    g_warning("CC:%s: $Direction numbers are equal.", net_remoteaddr(cc->net));
    g_set_error_literal(&cc->err, 1, 0, "Protocol error.");
    cc_disconnect(cc);
    return;
  // if we both want to download and the numbers aren't equal, then check the numbers
  } else
    cc->dl = cc->dir > num;

  // Now that this connection has a purpose, make sure it's the only connection with that purpose.
  struct cc *dup = cc_check_dupe(cc);
  if(dup && !!cc->dl == !!dup->dl) {
    g_set_error_literal(&cc->err, 1, 0, "Too many open connections with this user");
    cc_disconnect(cc);
    return;
  }
  cc->state = CCS_IDLE;

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
    if(cc->state != CCS_HANDSHAKE) {
      g_set_error_literal(&cc->err, 1, 0, "Protocol error.");
      g_message("CC:%s: Received message in wrong state: %s", net_remoteaddr(cc->net), cmd);
      cc_disconnect(cc);
    } else {
      char *nick = g_match_info_fetch(nfo, 1);
      nmdc_mynick(cc, nick);
      g_free(nick);
    }
  }
  g_match_info_free(nfo);

  // $Lock
  if(g_regex_match(lock, cmd, 0, &nfo)) { // 1 = lock
    char *lock = g_match_info_fetch(nfo, 1);
    if(cc->state != CCS_HANDSHAKE) {
      g_set_error_literal(&cc->err, 1, 0, "Protocol error.");
      g_message("CC:%s: Received message in wrong state: %s", net_remoteaddr(cc->net), cmd);
      cc_disconnect(cc);
    // we don't implement the classic NMDC get, so we can't talk with non-EXTENDEDPROTOCOL clients
    } else if(strncmp(lock, "EXTENDEDPROTOCOL", 16) != 0) {
      g_set_error_literal(&cc->err, 1, 0, "Protocol error.");
      g_warning("CC:%s: Does not advertise EXTENDEDPROTOCOL.", net_remoteaddr(cc->net));
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
    if(cc->state != CCS_HANDSHAKE) {
      g_set_error_literal(&cc->err, 1, 0, "Protocol error.");
      g_message("CC:%s: Received message in wrong state: %s", net_remoteaddr(cc->net), cmd);
      cc_disconnect(cc);
    // Client must support ADCGet to download from us, since we haven't implemented the old NMDC $Get.
    } else if(!strstr(list, "ADCGet")) {
      g_set_error_literal(&cc->err, 1, 0, "Protocol error.");
      g_warning("CC:%s: Does not support ADCGet.", net_remoteaddr(cc->net));
      cc_disconnect(cc);
    }
    g_free(list);
  }
  g_match_info_free(nfo);

  // $Direction
  if(g_regex_match(direction, cmd, 0, &nfo)) { // 1 = dir, 2 = num
    if(cc->state != CCS_HANDSHAKE) {
      g_set_error_literal(&cc->err, 1, 0, "Protocol error.");
      g_message("CC:%s: Received message in wrong state: %s", net_remoteaddr(cc->net), cmd);
      cc_disconnect(cc);
    } else {
      char *dir = g_match_info_fetch(nfo, 1);
      char *num = g_match_info_fetch(nfo, 2);
      nmdc_direction(cc, strcmp(dir, "Download") == 0, strtol(num, NULL, 10));
      g_free(dir);
      g_free(num);
    }
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
    if(cc->dl || cc->state != CCS_IDLE) {
      g_set_error_literal(&cc->err, 1, 0, "Protocol error.");
      g_message("CC:%s: Received message in wrong state: %s", net_remoteaddr(cc->net), cmd);
      cc_disconnect(cc);
    } else if(un_id && g_utf8_validate(un_id, -1, NULL)) {
      GError *err = NULL;
      handle_adcget(cc, type, un_id, st, by, &err);
      if(err) {
        if(err->code != 53)
          net_sendf(cc->net, "$Error %s", err->message);
        else
          net_send(cc->net, "$MaxedOut");
        g_propagate_error(&cc->err, err);
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
    if(!cc->dl || cc->state != CCS_TRANSFER) {
      g_set_error_literal(&cc->err, 1, 0, "Protocol error.");
      g_message("CC:%s: Received message in wrong state: %s", net_remoteaddr(cc->net), cmd);
      cc_disconnect(cc);
    } else {
      char *type = g_match_info_fetch(nfo, 1);
      char *start = g_match_info_fetch(nfo, 2);
      char *bytes = g_match_info_fetch(nfo, 3);
      handle_adcsnd(cc, strcmp(type, "tthl") == 0, g_ascii_strtoull(start, NULL, 10), g_ascii_strtoll(bytes, NULL, 10));
      g_free(type);
      g_free(start);
      g_free(bytes);
    }
  }
  g_match_info_free(nfo);

  // $Error
  if(g_regex_match(error, cmd, 0, &nfo)) { // 1 = message
    if(!cc->dl || cc->state != CCS_TRANSFER) {
      g_set_error_literal(&cc->err, 1, 0, "Protocol error.");
      g_message("CC:%s: Received message in wrong state: %s", net_remoteaddr(cc->net), cmd);
      cc_disconnect(cc);
    } else {
      char *msg = g_match_info_fetch(nfo, 1);
      g_set_error(&cc->err, 1, 0, msg);
      // Handle "File Not Available" and ".. no more exists"
      if(str_casestr(msg, "file not available") || str_casestr(msg, "no more exists")) {
        struct dl *dl = g_hash_table_lookup(dl_queue, cc->last_hash);
        if(dl)
          dl_queue_seterr(dl, DLE_NOFILE, 0);
      }
      g_free(msg);
      cc->state = CCS_IDLE;
      cc_download(cc);
    }
  }
  g_match_info_free(nfo);

  // $MaxedOut
  if(g_regex_match(maxedout, cmd, 0, &nfo)) {
    if(!cc->dl || cc->state != CCS_TRANSFER) {
      g_set_error_literal(&cc->err, 1, 0, "Protocol error.");
      g_message("CC:%s: Received message in wrong state: %s", net_remoteaddr(cc->net), cmd);
    } else
      g_set_error_literal(&cc->err, 1, 0, "No Slots Available");
    cc_disconnect(cc);
  }
  g_match_info_free(nfo);
}


static void handle_cmd(struct net *n, char *cmd) {
  struct cc *cc = n->handle;
  g_return_if_fail(cc->state != CCS_CONN && cc->state != CCS_DISCONN);

  // No input is allowed while we're sending file data.
  if(!cc->dl && cc->state == CCS_TRANSFER) {
    g_message("CC:%s: Received message from while we're sending a file.", net_remoteaddr(cc->net));
    g_set_error_literal(&cc->err, 1, 0, "Received message in upload state.");
    cc_disconnect(cc);
    return;
  }

  if(cc->adc)
    adc_handle(cc, cmd);
  else
    nmdc_handle(cc, cmd);
}


#if TLS_SUPPORT

// Simply stores the keyprint of the certificate in cc->kp_real, it will be
// checked when receiving CINF.
static gboolean handle_accept_cert(GTlsConnection *conn, GTlsCertificate *cert, GTlsCertificateFlags errors, gpointer dat) {
  struct net *n = dat;
  struct cc *c = n->handle;
  if(!c->kp_real)
    c->kp_real = g_slice_alloc(32);
  certificate_sha256(cert, c->kp_real);
  return TRUE;
}

#endif


// Hub may be unknown when this is an incoming connection
struct cc *cc_create(struct hub *hub) {
  struct cc *cc = g_new0(struct cc, 1);
  cc->net = net_create('|', cc, FALSE, handle_cmd, handle_error);
#if TLS_SUPPORT
  cc->net->conn_accept_cert = handle_accept_cert;
#endif
  cc->hub = hub;
  cc->iter = g_sequence_append(cc_list, cc);
  cc->state = CCS_CONN;
  if(ui_conn)
    ui_conn_listchange(cc->iter, UICONN_ADD);
  return cc;
}


static void handle_connect(struct net *n) {
  struct cc *cc = n->handle;
  strncpy(cc->remoteaddr, net_remoteaddr(cc->net), 23);
  if(!cc->hub)
    cc_disconnect(cc);
  else if(cc->adc) {
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
  cc->state = CCS_HANDSHAKE;
}


void cc_nmdc_connect(struct cc *cc, const char *addr, gboolean tls) {
  g_return_if_fail(cc->state == CCS_CONN);
  g_return_if_fail(!tls || have_tls_support);
  strncpy(cc->remoteaddr, addr, 23);
  net_connect(cc->net, addr, 0, tls, handle_connect);
  g_clear_error(&(cc->err));
}


void cc_adc_connect(struct cc *cc, struct hub_user *u, unsigned short port, gboolean tls, char *token) {
  g_return_if_fail(cc->state == CCS_CONN);
  g_return_if_fail(cc->hub);
  g_return_if_fail(u && u->active && u->ip4);
  g_return_if_fail(!tls || have_tls_support);
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
#if TLS_SUPPORT
  if(!cc->kp_user && u->kp) {
    cc->kp_user = g_slice_alloc(32);
    memcpy(cc->kp_user, u->kp, 32);
  }
#endif
  // check / update user info
  handle_id(cc, u);
  // handle_id() can do a cc_disconnect() when it discovers a duplicate
  // connection. This will reset cc->token, and we should stop this connection
  // attempt.
  if(!cc->token)
    return;
  // connect
  net_connect(cc->net, cc->remoteaddr, 0, tls, handle_connect);
  g_clear_error(&(cc->err));
}


static void handle_detectprotocol(struct net *net, char *dat, int len) {
  g_return_if_fail(len > 0);
  struct cc *cc = net->handle;
  net->recv_datain = NULL;
  if(dat[0] == 'C') {
    cc->adc = TRUE;
    net->eom[0] = '\n';
  }
  // otherwise, assume defaults (= NMDC)
}


static void cc_incoming(struct cc *cc, GSocketConnection *conn, gboolean tls) {
  net_setconn(cc->net, conn, tls, TRUE);
  cc->active = TRUE;
  cc->net->recv_datain = handle_detectprotocol;
  cc->state = CCS_HANDSHAKE;
  strncpy(cc->remoteaddr, net_remoteaddr(cc->net), 23);
}


static gboolean handle_timeout(gpointer dat) {
  cc_free(dat);
  return FALSE;
}


void cc_disconnect(struct cc *cc) {
  g_return_if_fail(cc->state != CCS_DISCONN);
  if(cc->state == CCS_TRANSFER)
    xfer_log_add(cc);
  net_disconnect(cc->net);
  cc->timeout_src = g_timeout_add_seconds(60, handle_timeout, cc);
  g_free(cc->token);
  cc->token = NULL;
  cc->state = CCS_DISCONN;
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
#if TLS_SUPPORT
  if(cc->kp_real)
    g_slice_free1(32, cc->kp_real);
  if(cc->kp_user)
    g_slice_free1(32, cc->kp_user);
#endif
  g_free(cc->tthl_dat);
  g_free(cc->nick_raw);
  g_free(cc->nick);
  g_free(cc->hub_name);
  g_free(cc->last_file);
  g_free(cc);
}





// Active mode


GSocketListener *cc_listen = NULL;     // TCP and TLS listen object. NULL if we aren't active.
GSocket         *cc_listen_udp = NULL; // UDP listen socket.
char            *cc_listen_ip = NULL;  // human-readable string. This is the remote IP, not the one we bind to.
guint16          cc_listen_port = 0;   // Port used for both UDP and TCP

static GCancellable *cc_listen_tcp_can = NULL;
static int cc_listen_udp_src = 0;


static void cc_listen_stop() {
  if(!cc_listen)
    return;
  g_free(cc_listen_ip);
  cc_listen_ip = NULL;

  g_cancellable_cancel(cc_listen_tcp_can);
  g_object_unref(cc_listen_tcp_can);
  cc_listen_tcp_can = NULL;
  g_socket_listener_close(cc_listen);
  g_object_unref(cc_listen);
  cc_listen = NULL;

  g_source_remove(cc_listen_udp_src);
  g_object_unref(cc_listen_udp);
  cc_listen_udp = NULL;
}


static void listen_tcp_handle(GObject *src, GAsyncResult *res, gpointer dat) {
  GError *err = NULL;
  GObject *istls = NULL;
  GSocketConnection *s = g_socket_listener_accept_finish(G_SOCKET_LISTENER(src), res, &istls, &err);

  if(!s) {
    if(cc_listen && err->code != G_IO_ERROR_CANCELLED && err->code != G_IO_ERROR_CLOSED) {
      ui_mf(ui_main, 0, "Listen error: %s. Switching to passive mode.", err->message);
      cc_listen_stop();
      hub_global_nfochange();
    }
    g_error_free(err);
  } else {
    cc_incoming(cc_create(NULL), s, istls ? TRUE : FALSE);
    g_socket_listener_accept_async(cc_listen, cc_listen_tcp_can, listen_tcp_handle, NULL);
    g_object_ref(cc_listen);
  }
  g_object_unref(cc_listen);
}


static void listen_udp_handle_msg(char *addr, char *msg, gboolean adc) {
  if(!msg[0])
    return;
  struct search_r *r = NULL;

  // ADC
  if(adc) {
    GError *err = NULL;
    struct adc_cmd cmd;
    adc_parse(msg, &cmd, NULL, &err);
    if(err) {
      g_warning("ADC parse error from UDP:%s: %s. --> %s", addr, err->message, msg);
      g_error_free(err);
      return;
    }
    r = search_parse_adc(NULL, &cmd);
    g_strfreev(cmd.argv);

  // NMDC
  } else
    r = search_parse_nmdc(NULL, msg);

  // Handle search result
  if(r) {
    ui_search_global_result(r);
    search_r_free(r);
  } else
    g_warning("Invalid search result from UDP:%s: %s", addr, msg);
}


static gboolean listen_udp_handle(GSocket *sock, GIOCondition cond, gpointer dat) {
  static char buf[5000]; // can be static, this function is only called in the main thread.
  GError *err = NULL;
  GSocketAddress *addr = NULL;
  int r = g_socket_receive_from(sock, &addr, buf, 5000, NULL, &err);

  // handle error
  if(r < 0) {
    if(err->code != G_IO_ERROR_WOULD_BLOCK) {
      ui_mf(ui_main, 0, "UDP read error: %s. Switching to passive mode.", err->message);
      cc_listen_stop();
      hub_global_nfochange();
    }
    g_error_free(err);
    return FALSE;
  }

  // get source ip:port in a readable fashion, for debugging
  char addr_str[100];
  GInetSocketAddress *socka = G_INET_SOCKET_ADDRESS(addr);
  if(!socka)
    strcat(addr_str, "(addr error)");
  else {
    char *ip = g_inet_address_to_string(g_inet_socket_address_get_address(socka));
    g_snprintf(addr_str, 100, "%s:%d", ip, g_inet_socket_address_get_port(socka));
    g_free(ip);
  }
  g_object_unref(addr);

  // check for ADC or NMDC
  gboolean adc = FALSE;
  if(buf[0] == 'U')
    adc = TRUE;
  else if(buf[0] != '$') {
    g_message("CC:UDP:%s: Received invalid message: %s", addr_str, buf);
    return TRUE;
  }

  // handle message. since all we receive is either URES or $SR, we can handle that here
  char *cur = buf, *next = buf;
  while((next = strchr(cur, adc ? '\n' : '|')) != NULL) {
    *(next++) = 0;
    g_debug("UDP:%s< %s", addr_str, cur);
    listen_udp_handle_msg(addr_str, cur, adc);
    cur = next;
  }

  return TRUE;
}


// TODO: option to bind to a specific IP, for those who want that functionality
static GSocket *listen_udp_create(int port, GError **err) {
  GError *tmperr = NULL;

  // get local address
  GInetAddress *laddr = g_inet_address_new_any(G_SOCKET_FAMILY_IPV4);
  GSocketAddress *saddr = G_SOCKET_ADDRESS(g_inet_socket_address_new(laddr, port));
  g_object_unref(laddr);

  // create the socket
  GSocket *s = g_socket_new(G_SOCKET_FAMILY_IPV4, G_SOCKET_TYPE_DATAGRAM, G_SOCKET_PROTOCOL_UDP, NULL);
  g_socket_set_blocking(s, FALSE);

  // bind to the address
  g_socket_bind(s, saddr, TRUE, &tmperr);
  g_object_unref(saddr);
  if(tmperr) {
    g_propagate_error(err, tmperr);
    g_object_unref(s);
    return NULL;
  }

  return s;
}


// TODO: same as for listen_udp_create
static GSocketListener *listen_tcp_create(int *port, GError **err) {
  GSocketListener *s = g_socket_listener_new();
  // TCP port
  if(*port == 0) {
    if(!(*port = g_socket_listener_add_any_inet_port(s, NULL, err))) {
      g_object_unref(s);
      return NULL;
    }
  } else if(!g_socket_listener_add_inet_port(s, *port, NULL, err)) {
    g_object_unref(s);
    return NULL;
  }

  // TLS port (use a bogus GCancellable object to differenciate betwen the two)
  if(s && conf_certificate) {
    GCancellable *t = g_cancellable_new();
    gboolean r = g_socket_listener_add_inet_port(s, *port+1, G_OBJECT(t), err);
    g_object_unref(t);
    if(!r) {
      g_object_unref(s);
      return NULL;
    }
  }
  return s;
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

  // Open TCP listen socket (and determine the port if it was 0)
  GSocketListener *tcp = listen_tcp_create(&port, &err);
  if(!tcp) {
    ui_mf(ui_main, 0, "Error creating TCP listen socket: %s", err->message);
    g_error_free(err);
    return FALSE;
  }

  // Open UDP listen socket
  GSocket *udp = listen_udp_create(port, &err);
  if(!udp) {
    ui_mf(ui_main, 0, "Error creating UDP listen socket: %s", err->message);
    g_object_unref(tcp);
    g_error_free(err);
    return FALSE;
  }

  // start accepting incoming TCP connections
  cc_listen_tcp_can = g_cancellable_new();
  g_socket_listener_accept_async(tcp, cc_listen_tcp_can, listen_tcp_handle, NULL);
  g_object_ref(tcp);

  // start receiving incoming UDP messages
  GSource *src = g_socket_create_source(udp, G_IO_IN, NULL);
  g_source_set_callback(src, (GSourceFunc)listen_udp_handle, NULL, NULL);
  cc_listen_udp_src = g_source_attach(src, NULL);
  g_source_unref(src);

  // set global variables
  cc_listen = tcp;
  cc_listen_udp = udp;
  cc_listen_port = port;
  cc_listen_ip = g_key_file_get_string(conf_file, "global", "active_ip", NULL);

  if(conf_certificate)
    ui_mf(ui_main, 0, "Listening on port TCP+UDP port %d and TCP port %d, remote IP is %s.", cc_listen_port, cc_listen_port+1, cc_listen_ip);
  else
    ui_mf(ui_main, 0, "Listening on port TCP+UDP port %d, remote IP is %s.", cc_listen_port, cc_listen_ip);
  hub_global_nfochange();
  return TRUE;
}


