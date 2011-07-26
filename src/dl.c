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
#include <errno.h>

#include <sys/stat.h>
#include <fcntl.h>
#include <gdbm.h>


#if INTERFACE

struct dl_user {
  guint64 uid;
  struct cc_expect *expect;
  struct cc *cc;
  GSList *queue; // ordered list of struct dl's. First item = active. (TODO: GSequence for performance?)
  gboolean active; // whether we're currently downloading or not. (i.e. whether this user is occupying a download slot)
};


struct dl {
  char hash[24]; // TTH for files, tiger(uid) for filelists
  gboolean islist;
  struct dl_user *u; // user who has this file (should be a list for multi-source downloading)
  int incfd;    // file descriptor for this file in <conf_dir>/inc/
  guint64 size; // total size of the file
  guint64 have; // what we have so far
  char *inc;    // path to the incomplete file (/inc/<base32-hash>)
  char *dest;   // destination path (must be on same filesystem as the incomplete file)
  GSequenceIter *iter; // used by UIT_DL
};

#endif


// GDBM data file.
static GDBM_FILE dl_dat;

// The 'have' field is currently not saved in the data file, stat() is used on
// startup on the incomplete file to get this information. We'd still need some
// progress indication in the data file in the future, to indicate which parts
// have been TTH-checked, etc.
#define DLDAT_INFO  0 // <8 bytes: size><8 bytes: reserved><zero-terminated-string: destination>
#define DLDAT_USERS 1 // <8 bytes: amount(=1)><8 bytes: uid>

static int dl_dat_needsync = FALSE;

// Performs a gdbm_sync() in an upcoming iteration of the main loop. This
// delayed write allows doing bulk operations on the data file while avoiding a
// gdbm_sync() on every change.
#define dl_dat_sync()\
  if(!dl_dat_needsync) {\
    dl_dat_needsync = TRUE;\
    g_idle_add(dl_dat_sync_do, NULL);\
  }


// Download queue.
// Key = dl->hash, Value = struct dl
GHashTable *dl_queue = NULL;


// uid -> dl_user lookup table.
static GHashTable *queue_users = NULL;



static gboolean dl_dat_sync_do(gpointer dat) {
  gdbm_sync(dl_dat);
  dl_dat_needsync = FALSE;
  return FALSE;
}


// Number of active downloads. (Can be cached to increase performance)
static int dl_num_active() {
  int n = 0;
  struct dl_user *du;
  GHashTableIter iter;
  g_hash_table_iter_init(&iter, queue_users);
  while(g_hash_table_iter_next(&iter, NULL, (gpointer *)&du))
    if(du->active)
      n++;
  return n;
}


// Returns the first item in the download queue for this user, prepares the
// item for downloading and sets this user as 'active'.
struct dl *dl_queue_next(guint64 uid) {
  struct dl_user *du = g_hash_table_lookup(queue_users, &uid);
  if(!du || !du->queue) {
    du->active = FALSE;
    // Nothing more for this user, check for other users.
    dl_queue_startany();
    return NULL;
  }

  // If we weren't downloading from this user yet, check that we can start a
  // new download. If we can't, it means we accidentally connected to this
  // user... let the connection idle.
  if(!du->active && dl_num_active() >= conf_download_slots())
    return NULL;
  du->active = TRUE;

  // For filelists: Don't allow resuming of the download. It could happen that
  // the client modifies its filelist in between our retries. In that case the
  // downloaded filelist would end up being corrupted. To avoid that: make sure
  // lists are downloaded in one go, and throw away any incomplete data.
  struct dl *dl = du->queue->data;
  if(dl->islist && dl->have > 0) {
    dl->have = dl->size = 0;
    g_return_val_if_fail(close(dl->incfd) == 0, NULL);
    dl->incfd = open(dl->inc, O_WRONLY|O_CREAT|O_TRUNC, 0666);
    g_return_val_if_fail(dl->incfd >= 0, NULL);
  }

  return dl;
}


// Try to start the download for a specific dl item. Called from
// dl_queue_uchange() (when we're not connected to a user, yet have something
// queued) or dl_queue_insert() (when a new item has been inserted).
static void dl_queue_start(struct dl *dl) {
  g_return_if_fail(dl && dl->u);
  // If we don't have download slots, then just forget it.
  if(dl_num_active() >= conf_download_slots())
    return;
  // If we expect an incoming connection, then things will resolve itself automatically.
  if(dl->u->expect)
    return;
  // try to re-use an existing connection
  if(dl->u->cc) {
    // download connection in the idle state
    if(dl->u->cc->candl && dl->u->cc->net->conn && !dl->u->cc->net->recv_left) {
      g_debug("dl:%016"G_GINT64_MODIFIER"x: re-using connection", dl->u->uid);
      cc_download(dl->u->cc);
    }
    return;
  }
  // get user/hub
  struct hub_user *u = g_hash_table_lookup(hub_uids, &dl->u->uid);
  if(!u)
    return;
  // try to open a C-C connection
  g_debug("dl:%016"G_GINT64_MODIFIER"x: trying to open a connection", u->uid);
  hub_opencc(u->hub, u);
}


// Called when one or more download slots have become free. Will attempt to
// start a new download.
// TODO: Prioritize downloads according do dl_user_queue_sort_func()?
void dl_queue_startany() {
  int n = conf_download_slots() - dl_num_active();
  if(n < 1)
    return;
  struct dl_user *du;
  GHashTableIter iter;
  g_hash_table_iter_init(&iter, queue_users);
  while(n > 0 && g_hash_table_iter_next(&iter, NULL, (gpointer *)&du)) {
    // Don't consider any user from which are actively downloading, or from
    // which we have nothing to donwload.
    if(du->active || !du->queue)
      continue;
    if(// Case 1: We are still connected to a user, in idle state.
      (du->cc && du->cc->net->conn && du->cc->candl) ||
      // Case 2: We are not connected and do not expect to get connected.
      (!du->cc && !du->expect)
    ) {
      dl_queue_start(du->queue->data);
      n--;
    }
  }
}


// Sort function when inserting items in the queue of a single user. This
// ensures that the files are downloaded in a predictable order.
static gint dl_user_queue_sort_func(gconstpointer a, gconstpointer b) {
  const struct dl *x = a;
  const struct dl *y = b;
  return x->islist && !y->islist ? -1 : !x->islist && y->islist ? 1 : strcmp(x->dest, y->dest);
}


// Adds a dl item to the queue. dl->inc will be determined and opened here.
static void dl_queue_insert(struct dl *dl, guint64 uid, gboolean init) {
  // figure out dl->inc
  char hash[40] = {};
  base32_encode(dl->hash, hash);
  dl->inc = g_build_filename(conf_dir, "inc", hash, NULL);
  // create or re-use dl_user struct
  dl->u = g_hash_table_lookup(queue_users, &uid);
  if(!dl->u) {
    dl->u = g_slice_new0(struct dl_user);
    dl->u->uid = uid;
    g_hash_table_insert(queue_users, &dl->u->uid, dl->u);
  }
  dl->u->queue = g_slist_insert_sorted(dl->u->queue, dl, dl_user_queue_sort_func);
  // insert in the global queue
  g_hash_table_insert(dl_queue, dl->hash, dl);
  if(ui_dl)
    ui_dl_listchange(dl, UIDL_ADD);

  // insert in the data file
  if(!dl->islist && !init) {
    char key[25];
    datum keydat = { key, 25 };
    datum val;
    memcpy(key+1, dl->hash, 24);

    key[0] = DLDAT_INFO;
    guint64 size = GINT64_TO_LE(dl->size);
    int nfovallen = 16 + strlen(dl->dest) + 1;
    char nfo[nfovallen];
    memset(nfo, 0, nfovallen);
    memcpy(nfo, &size, 8);
    // bytes 8-15 are reserved, and initialized to zero
    memcpy(nfo+16, dl->dest, strlen(dl->dest));
    val.dptr = nfo;
    val.dsize = nfovallen;
    gdbm_store(dl_dat, keydat, val, GDBM_REPLACE);

    key[0] = DLDAT_USERS;
    guint64 users[2] = { GINT64_TO_LE(1), GINT64_TO_LE(uid) };
    val.dptr = (char *)users;
    val.dsize = 2*8;
    gdbm_store(dl_dat, keydat, val, GDBM_REPLACE);
    dl_dat_sync();
  }

  // start download, if possible
  if(!init)
    dl_queue_start(dl);
}


// Called on either dl_queue_expect(), dl_queue_cc(), dl_queue_useronline() or
// dl_queue_rm().  Removes the dl_user struct if we're not connected and
// nothing is queued, and tries to start a connection if we're not connected
// but something is queued.
static void dl_queue_uchange(struct dl_user *du) {
  g_warn_if_fail(du->cc || (!du->cc && !du->active));
  if(!du->expect && !du->cc && du->queue)
    dl_queue_start(du->queue->data); // TODO: this only works with single-source downloading
  else if(!du->expect && !du->cc && !du->queue) {
    g_hash_table_remove(queue_users, &du->uid);
    g_slice_free(struct dl_user, du);
  }
}


// removes an item from the queue
void dl_queue_rm(struct dl *dl) {
  // close the incomplete file, in case it's still open
  if(dl->incfd > 0)
    g_warn_if_fail(close(dl->incfd) == 0);
  // remove the incomplete file, in case we still have it
  if(dl->inc && g_file_test(dl->inc, G_FILE_TEST_EXISTS))
    unlink(dl->inc);
  // update and optionally remove dl_user struct
  dl->u->queue = g_slist_remove(dl->u->queue, dl);
  dl_queue_uchange(dl->u);
  // remove from the data file
  if(!dl->islist) {
    char key[25];
    datum keydat = { key, 25 };
    memcpy(key+1, dl->hash, 24);
    key[0] = DLDAT_INFO;
    gdbm_delete(dl_dat, keydat);
    key[0] = DLDAT_USERS;
    gdbm_delete(dl_dat, keydat);
    dl_dat_sync();
  }
  // free and remove dl struct
  if(ui_dl)
    ui_dl_listchange(dl, UIDL_DEL);
  g_hash_table_remove(dl_queue, dl->hash);
  g_free(dl->inc);
  g_free(dl->dest);
  g_slice_free(struct dl, dl);
}


// set/unset the expect field. When expect goes from NULL to some value, it is
// expected that the connection is being established. When it goes from some
// value to NULL, it is expected that either the connection has been
// established (and dl_queue_cc() is called), the connection timed out (in
// which case we should try again), or the hub connection has been removed (in
// which case we should look for other hubs and try again).
// Note that in the case of a timeout (which is currently set to 60 seconds),
// we immediately try to connect again. Some hubs might not like this
// "aggressive" behaviour...
void dl_queue_expect(guint64 uid, struct cc_expect *e) {
  g_debug("dl:%016"G_GINT64_MODIFIER"x: expect = %s", uid, e?"true":"false");
  struct dl_user *du = g_hash_table_lookup(queue_users, &uid);
  if(!du)
    return;
  du->expect = e;
  dl_queue_uchange(du);
}


// set/unset the cc field. When cc goes from NULL to some value, it is expected
// that we're either connected to the user or we're in the remove timeout. When
// it is set, it means we're connected and the download is in progress or is
// being negotiated. Otherwise, it means we've failed to initiate the download
// and should try again.
void dl_queue_cc(guint64 uid, struct cc *cc) {
  g_debug("dl:%016"G_GINT64_MODIFIER"x: cc = %s", uid, cc?"true":"false");
  struct dl_user *du = g_hash_table_lookup(queue_users, &uid);
  if(!du)
    return;
  du->cc = cc;
  dl_queue_uchange(du);
}


// To be called when a user joins a hub. Checks whether we have something to
// get from that user.
void dl_queue_useronline(guint64 uid) {
  struct dl_user *du = g_hash_table_lookup(queue_users, &uid);
  if(du)
    dl_queue_uchange(du);
}


// To be called when a cc connection is disconnected. (This happens before
// _cc() is called, since _cc() is called on a timeout).
void dl_queue_userdisconnect(guint64 uid) {
  struct dl_user *du = g_hash_table_lookup(queue_users, &uid);
  if(du) {
    du->active = FALSE;
    dl_queue_startany();
  }
}




// Add the file list of some user to the queue
void dl_queue_addlist(struct hub_user *u) {
  g_return_if_fail(u && u->hasinfo);
  struct dl *dl = g_slice_new0(struct dl);
  dl->islist = TRUE;
  // figure out dl->hash
  tiger_ctx tg;
  tiger_init(&tg);
  tiger_update(&tg, (char *)&u->uid, 8);
  tiger_final(&tg, dl->hash);
  if(g_hash_table_lookup(dl_queue, dl->hash)) {
    g_warning("dl:%016"G_GINT64_MODIFIER"x: files.xml.bz2 already in the queue.", u->uid);
    g_slice_free(struct dl, dl);
    return;
  }
  // figure out dl->dest
  char *fn = g_strdup_printf("%016"G_GINT64_MODIFIER"x.xml.bz2", u->uid);
  dl->dest = g_build_filename(conf_dir, "fl", fn, NULL);
  g_free(fn);
  // insert & start
  g_debug("dl:%016"G_GINT64_MODIFIER"x: queueing files.xml.bz2", u->uid);
  dl_queue_insert(dl, u->uid, FALSE);
}


static gboolean check_dupe_dest(char *dest) {
  GHashTableIter iter;
  struct dl *dl;
  g_hash_table_iter_init(&iter, dl_queue);
  // Note: it is assumed that dl->dest is a cannonical path. That is, it does
  // not have any funky symlink magic, duplicate slashes, or references to
  // current/parent directories. This check will fail otherwise.
  while(g_hash_table_iter_next(&iter, NULL, (gpointer *)&dl))
    if(strcmp(dl->dest, dest) == 0)
      return TRUE;

  if(g_file_test(dest, G_FILE_TEST_EXISTS))
    return TRUE;
  return FALSE;
}


// Add a regular file to the queue. If there is another file in the queue with
// the same filename, something else will be chosen instead.
// Returns true if it was added, false if it was already in the queue.
static gboolean dl_queue_addfile(guint64 uid, char *hash, guint64 size, char *fn) {
  if(g_hash_table_lookup(dl_queue, hash))
    return FALSE;
  g_return_val_if_fail(size >= 0, FALSE);
  struct dl *dl = g_slice_new0(struct dl);
  memcpy(dl->hash, hash, 24);
  dl->size = size;
  // Figure out dl->dest. It is assumed that fn + any dupe-prevention-extension
  // does not exceed NAME_MAX. (Not that checking against NAME_MAX is really
  // reliable - some filesystems have an even more strict limit)
  int num = 1;
  char *tmp = conf_download_dir();
  char *base = g_build_filename(tmp, fn, NULL);
  g_free(tmp);
  dl->dest = g_strdup(base);
  while(check_dupe_dest(dl->dest)) {
    g_free(dl->dest);
    dl->dest = g_strdup_printf("%s.%d", base, num++);
  }
  g_free(base);
  // and add to the queue
  g_debug("dl:%016"G_GINT64_MODIFIER"x: queueing %s", uid, fn);
  dl_queue_insert(dl, uid, FALSE);
  return TRUE;
}


// Recursively adds a file or directory to the queue.
void dl_queue_add_fl(guint64 uid, struct fl_list *fl, char *base) {
  char *name = base ? g_build_filename(base, fl->name, NULL) : g_strdup(fl->name);
  if(fl->isfile) {
    if(!dl_queue_addfile(uid, fl->tth, fl->size, name))
      ui_mf(NULL, 0, "Ignoring `%s': already queued.", name);
  } else {
    GSequenceIter *i = g_sequence_get_begin_iter(fl->sub);
    for(; !g_sequence_iter_is_end(i); i=g_sequence_iter_next(i))
      dl_queue_add_fl(uid, g_sequence_get(i), name);
  }
  if(!base)
    ui_mf(NULL, 0, "%s added to queue.", name);
  g_free(name);
}


// Called when we've got a complete file
static void dl_finished(struct dl *dl) {
  g_debug("dl:%016"G_GINT64_MODIFIER"x: download of `%s' finished, removing from queue", dl->u->uid, dl->dest);
  // close
  if(dl->incfd > 0)
    g_warn_if_fail(close(dl->incfd) == 0);
  dl->incfd = 0;
  // Create destination directory, if it does not exist yet.
  char *parent = g_path_get_dirname(dl->dest);
  GError *err = NULL;
  if(!g_file_test(parent, G_FILE_TEST_IS_DIR)) {
    GFile *p = g_file_new_for_path(parent);
    g_file_make_directory_with_parents(p, NULL, &err);
    g_object_unref(p);
  }
  g_free(parent);
  // Move the file to the destination.
  // TODO: this may block for a while if they are not on the same filesystem,
  // do this in a separate thread?
  if(!err) {
    GFile *src = g_file_new_for_path(dl->inc);
    GFile *dest = g_file_new_for_path(dl->dest);
    g_file_move(src, dest, dl->islist ? G_FILE_COPY_OVERWRITE : G_FILE_COPY_BACKUP, NULL, NULL, NULL, &err);
    g_object_unref(src);
    g_object_unref(dest);
  }
  if(err) {
    ui_mf(ui_main, UIP_MED, "Error moving `%s' to `%s': %s", dl->inc, dl->dest, err->message);
    g_error_free(err);
  }
  // open the file list
  if(!err && dl->islist) {
    struct ui_tab *t = ui_fl_create(dl->u->uid, TRUE);
    if(t)
      ui_tab_open(t, FALSE);
  }
  // and remove from the queue
  dl_queue_rm(dl);
}


// Called when we've received some file data.
// TODO: hash checking
// TODO: do this in a separate thread to avoid blocking on HDD writes.
// TODO: improved error handling
void dl_received(struct dl *dl, int length, char *buf) {
  //g_debug("dl:%016"G_GINT64_MODIFIER"x: Received %d bytes for %s (size = %"G_GUINT64_FORMAT", have = %"G_GUINT64_FORMAT")", dl->u->uid, length, dl->dest, dl->size, dl->have+length);
  g_return_if_fail(dl->have + length <= dl->size);
  g_warn_if_fail(dl->u->active);

  // open dl->incfd, if it's not open yet
  if(dl->incfd <= 0) {
    dl->incfd = open(dl->inc, O_WRONLY|O_CREAT|O_APPEND, 0666);
    if(dl->incfd < 0) {
      g_warning("Error opening %s: %s", dl->inc, g_strerror(errno));
      return;
    }
  }

  // save to the file and update dl->have
  while(length > 0) {
    int r = write(dl->incfd, buf, length);
    if(r < 0) {
      g_warning("Error writing to %s: %s", dl->inc, g_strerror(errno));
      return;
    }
    length -= r;
    buf += r;
    dl->have += r;
  }

  if(dl->have >= dl->size) {
    g_warn_if_fail(dl->have == dl->size);
    dl_finished(dl);
  }
}




// loads a single item from the data file
static void dl_queue_loaditem(char *hash) {
  char key[25];
  memcpy(key+1, hash, 24);
  datum keydat = { key, 25 };

  key[0] = DLDAT_INFO;
  datum nfo = gdbm_fetch(dl_dat, keydat);
  g_return_if_fail(nfo.dsize > 17);
  key[0] = DLDAT_USERS;
  datum users = gdbm_fetch(dl_dat, keydat);
  g_return_if_fail(users.dsize >= 16);

  struct dl *dl = g_slice_new0(struct dl);
  memcpy(dl->hash, hash, 24);
  memcpy(&dl->size, nfo.dptr, 8);
  dl->size = GINT64_FROM_LE(dl->size);
  dl->dest = g_strdup(nfo.dptr+16);

  // get size of the incomplete file
  char tth[40] = {};
  base32_encode(dl->hash, tth);
  char *fn = g_build_filename(conf_dir, "inc", tth, NULL);
  struct stat st;
  if(stat(fn, &st) >= 0)
    dl->have = st.st_size;
  g_free(fn);

  // and insert in the hash tables
  guint64 uid;
  memcpy(&uid, users.dptr+8, 8);
  uid = GINT64_FROM_LE(uid);
  dl_queue_insert(dl, uid, TRUE);
  g_debug("dl:%016"G_GINT64_MODIFIER"x: load `%s' from data file (size = %"G_GUINT64_FORMAT", have = %"G_GUINT64_FORMAT")", uid, dl->dest, dl->size, dl->have);

  free(nfo.dptr);
  free(users.dptr);
}


// loads the queued items from the data file
static void dl_queue_loaddat() {
  datum key = gdbm_firstkey(dl_dat);
  while(key.dptr) {
    char *str = key.dptr;
    if(key.dsize == 25 && str[0] == DLDAT_INFO)
      dl_queue_loaditem(str+1);
    key = gdbm_nextkey(dl_dat, key);
    free(str);
  }
}





// Removes old filelists from /fl/. Can be run from a timer.
gboolean dl_fl_clean(gpointer dat) {
  char *dir = g_build_filename(conf_dir, "fl", NULL);
  GDir *d = g_dir_open(dir, 0, NULL);
  if(!d) {
    g_free(dir);
    return TRUE;
  }

  const char *n;
  time_t ref = time(NULL) - 7*24*3600; // keep lists for one week
  while((n = g_dir_read_name(d))) {
    if(strcmp(n, ".") == 0 || strcmp(n, "..") == 0)
      continue;
    char *fn = g_build_filename(dir, n, NULL);
    struct stat st;
    if(stat(fn, &st) >= 0 && st.st_mtime < ref)
      unlink(fn);
    g_free(fn);
  }
  g_dir_close(d);
  g_free(dir);
  return TRUE;
}


// Removes unused files in /inc/.
static void dl_inc_clean() {
  char *dir = g_build_filename(conf_dir, "inc", NULL);
  GDir *d = g_dir_open(dir, 0, NULL);
  if(!d) {
    g_free(dir);
    return;
  }

  const char *n;
  char hash[24];
  while((n = g_dir_read_name(d))) {
    // Only consider files that we have created, which always happen to have a
    // base32-encoded hash as filename.
    if(strlen(n) != 39 || strspn(n, "ABCDEFGHIJKLMNOPQRSTUVWXYZ234567") != 39)
      continue;
    base32_decode(n, hash);
    if(g_hash_table_lookup(dl_queue, hash))
      continue;
    // not in the queue? delete.
    char *fn = g_build_filename(dir, n, NULL);
    unlink(fn);
    g_free(fn);
  }
  g_dir_close(d);
  g_free(dir);
}


// Removes unused items from dl.dat, performs a gdbm_reorganize(), removes
// unused files in /inc/ and calls dl_fl_clean().
// The dl.dat cleaning code is based on based on fl_hashdat_gc().
void dl_gc() {
  GSList *rm = NULL;
  char hash[40];
  hash[39] = 0;

  // check for unused keys
  datum key = gdbm_firstkey(dl_dat);
  char *freethis = NULL;
  for(; key.dptr; key=gdbm_nextkey(dl_dat, key)) {
    char *str = key.dptr;
    if(freethis)
      free(freethis);
    // We only touch keys that this or earlier versions of ncdc could have
    // created. Unknown keys are left untouched as a later version could have
    // made these and there is no way to tell whether these need to be cleaned
    // up or not.
    if(key.dsize == 25 && (str[0] == DLDAT_INFO || str[0] == DLDAT_USERS)
        && !g_hash_table_lookup(dl_queue, str+1)) {
      base32_encode(str+1, hash);
      g_message("Removing unused key in dl.dat: type = %d, hash = %s", str[0], hash);
      rm = g_slist_prepend(rm, str);
      freethis = NULL;
    } else
      freethis = str;
  }
  if(freethis)
    free(freethis);

  // delete the unused keys
  GSList *n = rm;
  key.dsize = 25;
  while(n) {
    rm = n->next;
    key.dptr = n->data;
    gdbm_delete(dl_dat, key);
    free(n->data);
    g_slist_free_1(n);
    n = rm;
  }

  // perform the reorganize
  gdbm_reorganize(dl_dat);

  // remove unused files in /inc/
  dl_inc_clean();

  // remove old files in /fl/
  dl_fl_clean(NULL);
}


void dl_init_global() {
  queue_users = g_hash_table_new(g_int64_hash, g_int64_equal);
  dl_queue = g_hash_table_new(g_int_hash, tiger_hash_equal);
  // open data file
  char *fn = g_build_filename(conf_dir, "dl.dat", NULL);
  dl_dat = gdbm_open(fn, 0, GDBM_WRCREAT, 0600, NULL);
  g_free(fn);
  if(!dl_dat)
    g_error("Unable to open dl.dat.");
  dl_queue_loaddat();
  // Delete old filelists
  dl_fl_clean(NULL);
}


void dl_close_global() {
  gdbm_close(dl_dat);
  // Delete incomplete file lists. They won't be completed anyway.
  GHashTableIter iter;
  struct dl *dl;
  g_hash_table_iter_init(&iter, dl_queue);
  while(g_hash_table_iter_next(&iter, NULL, (gpointer *)&dl))
    if(dl->islist)
      unlink(dl->inc);
  // Delete old filelists
  dl_fl_clean(NULL);
}

