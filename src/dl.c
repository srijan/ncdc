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
#include <errno.h>

#include <sys/stat.h>
#include <fcntl.h>
#include <gdbm.h>


#if INTERFACE

struct dl_user {
  guint64 uid;
  struct cc_expect *expect;
  struct cc *cc;
  GSList *queue; // ordered list of struct dl's. (TODO: GSequence for performance?)
  gboolean active; // whether we're currently downloading or not. (i.e. whether this user is occupying a download slot)
};


// Note: The following numbers are also stored in dl.dat. Keep this in mind
// when changing or extending. (Both DLP_ and DLE_)

#define DLP_ERR   -65 // disabled due to (permanent) error
#define DLP_OFF   -64 // disabled by user
#define DLP_VLOW   -2
#define DLP_LOW    -1
#define DLP_MED     0
#define DLP_HIGH    1
#define DLP_VHIGH   2


#define DLE_NONE    0 // No error
#define DLE_INVTTHL 1 // TTHL data does not match the file root
#define DLE_NOFILE  2 // User does not have the file at all
#define DLE_IO_INC  3 // I/O error with incoming file, error_sub = errno
#define DLE_IO_DEST 4 // I/O error when moving to destination file/dir
#define DLE_HASH    5 // Hash check failed, error_sub = block index of failed hash


struct dl {
  gboolean islist : 1;
  gboolean hastthl : 1;
  char prio;                // DLP_*
  char error;               // DLE_*
  unsigned short error_sub; // errno or block number (it is assumed that 0 <= errno <= USHRT_MAX)
  int incfd;                // file descriptor for this file in <conf_dir>/inc/
  char hash[24];            // TTH for files, tiger(uid) for filelists
  struct dl_user *u;        // user who has this file (should be a list for multi-source downloading)
  guint64 size;             // total size of the file
  guint64 have;             // what we have so far
  char *inc;                // path to the incomplete file (/inc/<base32-hash>)
  char *dest;               // destination path (must be on same filesystem as the incomplete file)
  guint64 hash_block;       // number of bytes that each block represents
  struct tth_ctx *hash_tth; // TTH state of the last block that we have
  GSequenceIter *iter;      // used by UIT_DL
};

#endif

// Minimum filesize for which we request TTHL data. If a file is smaller than
// this, the TTHL data would simply add more overhead than it is worth.
#define DL_MINTTHLSIZE (512*1024)

// GDBM data file.
static GDBM_FILE dl_dat;

// The 'have' field is currently not saved in the data file, stat() is used on
// startup on the incomplete file to get this information. We'd still need some
// progress indication in the data file in the future, to indicate which parts
// have been TTH-checked, etc.
#define DLDAT_INFO  0 // <8 bytes: size><1 byte: prio><1 byte: error><2 bytes: error_sub>
                      // <4 bytes: reserved><zero-terminated-string: destination>
#define DLDAT_USERS 1 // <8 bytes: amount(=1)><8 bytes: uid>
#define DLDAT_TTHL  2 // <24 bytes: hash1>..

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


// Cached value. Should be equivalent to the number of items in the queue_users
// table where active = TRUE.
static int queue_users_active = 0;


static gboolean dl_dat_sync_do(gpointer dat) {
  gdbm_sync(dl_dat);
  dl_dat_needsync = FALSE;
  return FALSE;
}


// Utility function that returns an error string for DLE_* errors.
char *dl_strerror(char err, unsigned short sub) {
  static char buf[200];
  switch(err) {
    case DLE_NONE:    strcpy(buf, "No error."); break;
    case DLE_INVTTHL: strcpy(buf, "TTHL data does not match TTH root."); break;
    case DLE_NOFILE:  strcpy(buf, "File not available from this user."); break;
    case DLE_IO_INC:  g_snprintf(buf, 200, "Error writing to temporary file: %s", g_strerror(sub)); break;
    case DLE_IO_DEST:
      if(!sub)
        strcpy(buf, "Error moving file to destination.");
      else
        g_snprintf(buf, 200, "Error moving file to destination: %s", g_strerror(sub));
      break;
    case DLE_HASH:    g_snprintf(buf, 200, "Hash chunk %d does not match downloaded data.", sub); break;
    default:          strcpy(buf, "Unknown error.");
  }
  return buf;
}


// Returns the first item in the download queue for this user, prepares the
// item for downloading and sets this user as 'active'.
struct dl *dl_queue_next(guint64 uid) {
  struct dl_user *du = g_hash_table_lookup(queue_users, &uid);
  if(!du || !du->queue || ((struct dl *)du->queue->data)->prio <= DLP_OFF) {
    if(du->active)
      queue_users_active--;
    du->active = FALSE;
    // Nothing more for this user, check for other users.
    dl_queue_startany();
    return NULL;
  }

  // If we weren't downloading from this user yet, check that we can start a
  // new download. If we can't, it means we accidentally connected to this
  // user... let the connection idle.
  if(!du->active && queue_users_active >= conf_download_slots())
    return NULL;
  if(!du->active)
    queue_users_active++;
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


// Try to start the download for a specific dl item. Called from:
// - dl_queue_uchange() when we're not connected to a user, yet have something queued
// - dl_queue_insert()  when a new item has been inserted
// - dl_queue_setprio() when an item has been enabled
static void dl_queue_start(struct dl *dl) {
  g_return_if_fail(dl && dl->u);
  // Don't even try if it is marked as disabled
  if(dl->prio <= DLP_OFF)
    return;
  // If we don't have download slots, then just forget it.
  if(queue_users_active >= conf_download_slots())
    return;
  // If we expect an incoming connection, then things will resolve itself automatically.
  if(dl->u->expect)
    return;
  // try to re-use an existing connection
  if(dl->u->cc) {
    // download connection in the idle state
    if(dl->u->cc->candl && dl->u->cc->net->conn && !dl->u->cc->isdl) {
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
  int n = conf_download_slots() - queue_users_active;
  if(n < 1)
    return;
  struct dl_user *du;
  GHashTableIter iter;
  g_hash_table_iter_init(&iter, queue_users);
  while(n > 0 && g_hash_table_iter_next(&iter, NULL, (gpointer *)&du)) {
    // Don't consider any user from which are actively downloading, or from
    // which we have nothing to donwload.
    if(du->active || !du->queue || ((struct dl *)du->queue->data)->prio <= DLP_OFF)
      continue;
    if(// Case 1: We are still connected to a user, in idle state.
      (du->cc && du->cc->net->conn && du->cc->candl && !du->cc->isdl) ||
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
  return
      x->islist && !y->islist ? -1 : !x->islist && y->islist ? 1
    : x->prio > y->prio ? -1 : x->prio < y->prio ? 1
    : strcmp(x->dest, y->dest);
}


static void dl_dat_saveinfo(struct dl *dl) {
  char key[25];
  key[0] = DLDAT_INFO;
  memcpy(key+1, dl->hash, 24);
  datum keydat = { key, 25 };

  guint64 size = GINT64_TO_LE(dl->size);
  int len = 16 + strlen(dl->dest) + 1;
  char nfo[len];
  memset(nfo, 0, len);
  memcpy(nfo, &size, 8);
  nfo[8] = dl->prio;
  nfo[9] = dl->error;
  guint16 err_sub = GINT16_TO_LE(dl->error_sub);
  memcpy(nfo+10, &err_sub, 2);
  // bytes 12-15 are reserved, and initialized to zero
  strcpy(nfo+16, dl->dest);
  datum val = { nfo, len };

  gdbm_store(dl_dat, keydat, val, GDBM_REPLACE);
  dl_dat_sync();
}


void dl_queue_setprio(struct dl *dl, char prio) {
  gboolean enabled = dl->prio <= DLP_OFF && prio > DLP_OFF;
  dl->prio = prio;
  dl_dat_saveinfo(dl);
  // Make sure dl->u->queue is still in the correct order
  dl->u->queue = g_slist_remove(dl->u->queue, dl);
  dl->u->queue = g_slist_insert_sorted(dl->u->queue, dl, dl_user_queue_sort_func);
  // Start the download if it is enabled
  if(enabled)
    dl_queue_start(dl);
}


#if INTERFACE

#define dl_queue_seterr(dl, e, sub) do {\
    (dl)->error = e;\
    (dl)->error_sub = sub;\
    dl_queue_setprio(dl, DLP_ERR);\
    ui_mf(ui_main, 0, "Download of `%s' failed: %s", (dl)->dest, dl_strerror(e, sub));\
  } while(0)

#endif


// Adds a dl item to the queue. dl->inc will be determined and opened here.
// dl->hastthl will be set if the file is small enough to not need TTHL data.
static void dl_queue_insert(struct dl *dl, guint64 uid, gboolean init) {
  // Set dl->hastthl for files smaller than MINTTHLSIZE.
  if(!dl->islist && !dl->hastthl && dl->size <= DL_MINTTHLSIZE) {
    dl->hastthl = TRUE;
    dl->hash_block = DL_MINTTHLSIZE;
  }
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
    dl_dat_saveinfo(dl);

    char key[25];
    key[0] = DLDAT_USERS;
    memcpy(key+1, dl->hash, 24);
    guint64 users[2] = { GINT64_TO_LE(1), GINT64_TO_LE(uid) };
    datum keydat = { key, 25 };
    datum val = { (char *)users, 2*8 };
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
    key[0] = DLDAT_TTHL;
    gdbm_delete(dl_dat, keydat);
    dl_dat_sync();
  }
  // free and remove dl struct
  if(ui_dl)
    ui_dl_listchange(dl, UIDL_DEL);
  g_hash_table_remove(dl_queue, dl->hash);
  if(dl->hash_tth)
    g_slice_free(struct tth_ctx, dl->hash_tth);
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
    if(du->active)
      queue_users_active--;
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
  if(g_mkdir_with_parents(parent, 0755) < 0)
    dl_queue_seterr(dl, DLE_IO_DEST, errno);
  g_free(parent);
  // Move the file to the destination.
  // TODO: this may block for a while if they are not on the same filesystem,
  // do this in a separate thread?
  GError *err = NULL;
  if(dl->prio != DLP_ERR) {
    GFile *src = g_file_new_for_path(dl->inc);
    GFile *dest = g_file_new_for_path(dl->dest);
    g_file_move(src, dest, dl->islist ? G_FILE_COPY_OVERWRITE : G_FILE_COPY_BACKUP, NULL, NULL, NULL, &err);
    g_object_unref(src);
    g_object_unref(dest);
    if(err) {
      g_warning("Error moving `%s' to `%s': %s", dl->inc, dl->dest, err->message);
      dl_queue_seterr(dl, DLE_IO_DEST, 0); // g_file_move does not give the value of errno :(
      g_error_free(err);
    }
  }
  // open the file list
  if(dl->prio != DLP_ERR && dl->islist)
    ui_tab_open(ui_fl_create(dl->u->uid), FALSE);
  // and remove from the queue
  dl_queue_rm(dl);
}


static gboolean dl_hash_check(struct dl *dl, int num, char *tth) {
  // We don't have TTHL data for small files, so check against the root hash instead.
  if(dl->size < dl->hash_block) {
    g_return_val_if_fail(num == 0, FALSE);
    return memcmp(tth, dl->hash, 24) == 0 ? TRUE : FALSE;
  }
  // Otherwise, fetch the TTHL data and check against the right block hash.
  // It is probably faster to keep the TTHL data in memory, but since this data
  // may be around 200KiB and we can have a large number of dl structs, let's
  // just hope GDBM has a sensible caching mechanism.
  char key[25] = { DLDAT_TTHL };
  memcpy(key+1, dl->hash, 24);
  datum keydat = { key, 25 };
  datum val = gdbm_fetch(dl_dat, keydat);
  g_return_val_if_fail(val.dsize >= (num+1)*24, FALSE);
  gboolean r = memcmp(tth, val.dptr+(num*24), 24) == 0 ? TRUE : FALSE;
  free(val.dptr);
  return r;
}


// (Incrementally) hashes the incoming data and checks that what we have is
// still correct. When this function is called, dl->length should refer to the
// point where the newly received data will be written to.
// Returns -1 if nothing went wrong, any other number to indicate which block
// failed the hash check.
static int dl_hash_update(struct dl *dl, int length, char *buf) {
  g_return_val_if_fail(dl->hastthl, 0);

  int block = dl->have / dl->hash_block;
  guint64 cur = dl->have % dl->hash_block;

  if(!dl->hash_tth) {
    g_return_val_if_fail(!cur, FALSE);
    dl->hash_tth = g_slice_new(struct tth_ctx);
    tth_init(dl->hash_tth);
  }

  char tth[24];
  while(length > 0) {
    int w = MIN(dl->hash_block - cur, length);
    tth_update(dl->hash_tth, buf, w);
    length -= w;
    cur += w;
    buf += w;
    // we have a complete block, validate it.
    if(cur == dl->hash_block || (!length && dl->size == (block*dl->hash_block)+cur)) {
      tth_final(dl->hash_tth, tth);
      tth_init(dl->hash_tth);
      if(!dl_hash_check(dl, block, tth))
        return block;
      cur = 0;
      block++;
    }
  }

  return -1;
}


// Called when we've received some file data. Returns TRUE to continue the
// download, FALSE when something went wrong and the transfer should be
// aborted.
// TODO: do this in a separate thread to avoid blocking on HDD writes.
gboolean dl_received(struct dl *dl, int length, char *buf) {
  //g_debug("dl:%016"G_GINT64_MODIFIER"x: Received %d bytes for %s (size = %"G_GUINT64_FORMAT", have = %"G_GUINT64_FORMAT")", dl->u->uid, length, dl->dest, dl->size, dl->have+length);
  g_return_val_if_fail(dl->have + length <= dl->size, FALSE);
  g_warn_if_fail(dl->u->active);

  // open dl->incfd, if it's not open yet
  if(dl->incfd <= 0) {
    dl->incfd = open(dl->inc, O_WRONLY|O_CREAT, 0666);
    if(dl->incfd < 0 || lseek(dl->incfd, dl->have, SEEK_SET) == (off_t)-1) {
      g_warning("Error opening %s: %s", dl->inc, g_strerror(errno));
      dl_queue_seterr(dl, DLE_IO_INC, errno);
      return FALSE;
    }
  }

  // save to the file and update dl->have
  while(length > 0) {
    // write
    int r = write(dl->incfd, buf, length);
    if(r < 0) {
      g_warning("Error writing to %s: %s", dl->inc, g_strerror(errno));
      dl_queue_seterr(dl, DLE_IO_INC, errno);
      return FALSE;
    }

    // check hash
    int fail = dl->islist ? -1 : dl_hash_update(dl, r, buf);
    if(fail >= 0) {
      g_warning("Hash failed for %s (block %d)", dl->inc, fail);
      dl_queue_seterr(dl, DLE_HASH, fail);
      // Delete the failed block and everything after it, so that a resume is possible.
      dl->have = fail*dl->hash_block;
      // I have no idea what to do when these functions fail. Resuming the
      // download without an ncdc restart won't work if lseek() fails, resuming
      // the download after an ncdc restart may result in a corrupted download
      // if the ftruncate() fails. Either way, resuming isn't possible in a
      // reliable fashion, so perhaps we should throw away the entire inc file?
      lseek(dl->incfd, dl->have, SEEK_SET);
      ftruncate(dl->incfd, dl->have);
      return FALSE;
    }

    // update vars
    length -= r;
    buf += r;
    dl->have += r;
  }

  if(dl->have >= dl->size) {
    g_warn_if_fail(dl->have == dl->size);
    dl_finished(dl);
  }
  return TRUE;
}


// Called when we've received TTHL data. For now we'll just store it dl.dat
// without modifications.
// TODO: combine hashes to remove uneeded granularity? (512kB is probably enough)
void dl_settthl(struct dl *dl, char *tthl, int len) {
  g_return_if_fail(!dl->islist);
  g_return_if_fail(!dl->have);
  // Ignore this if we already have the TTHL data. Currently, this situation
  // can't happen, but it may be possible with multisource downloading.
  g_return_if_fail(!dl->hastthl);

  g_debug("dl:%016"G_GINT64_MODIFIER"x: Received TTHL data for %s (len = %d, bs = %"G_GUINT64_FORMAT")", dl->u->uid, dl->dest, len, tth_blocksize(dl->size, len/24));

  // Validate correctness with the root hash
  char root[24];
  tth_root(tthl, len/24, root);
  if(memcmp(root, dl->hash, 24) != 0) {
    g_warning("dl:%016"G_GINT64_MODIFIER"x: Incorrect TTHL for %s.", dl->u->uid, dl->dest);
    dl_queue_seterr(dl, DLE_INVTTHL, 0);
    return;
  }

  dl->hastthl = TRUE;
  dl->hash_block = tth_blocksize(dl->size, len/24);

  // save to dl.dat
  char key[25];
  key[0] = DLDAT_TTHL;
  memcpy(key+1, dl->hash, 24);
  datum keydat = { key, 25 };
  datum val = { tthl, len };
  gdbm_store(dl_dat, keydat, val, GDBM_REPLACE);
  dl_dat_sync();
}




// Calculates dl->hash_block, dl->have and if necessary updates dl->hash_tth
// for the last block that we have.
static void dl_queue_loadpartial(struct dl *dl) {
  // get size of the incomplete file
  char tth[40] = {};
  base32_encode(dl->hash, tth);
  char *fn = g_build_filename(conf_dir, "inc", tth, NULL);
  struct stat st;
  if(stat(fn, &st) >= 0)
    dl->have = st.st_size;

  // get TTHL info
  if(dl->size < DL_MINTTHLSIZE) {
    dl->hastthl = TRUE;
    dl->hash_block = DL_MINTTHLSIZE;
  } else {
    char key[25] = { DLDAT_TTHL };
    memcpy(key+1, dl->hash, 24);
    datum keydat = { key, 25 };
    datum val = gdbm_fetch(dl_dat, keydat);
    if(!val.dptr) {
      // No TTHL data found? Make sure to redownload the entire file
      dl->have = 0;
      unlink(fn);
    } else {
      dl->hastthl = TRUE;
      dl->hash_block = tth_blocksize(dl->size, val.dsize/24);
      free(val.dptr);
    }
  }

  // If we have already downloaded some data, hash the last block and update
  // dl->hash_tth.
  guint64 left = dl->hash_block ? dl->have % dl->hash_block : 0;
  if(left > 0) {
    dl->have -= left;
    int fd = open(fn, O_RDONLY);
    if(fd < 0 || lseek(fd, dl->have, SEEK_SET) == (off_t)-1) {
      g_warning("Error opening %s: %s. Throwing away last block.", fn, g_strerror(errno));
      left = 0;
      if(fd >= 0)
        close(fd);
    }
    while(left > 0) {
      char buf[1024];
      int r = read(fd, buf, MIN(left, 1024));
      if(r < 0) {
        g_warning("Error reading from %s: %s. Throwing away unreadable data.", fn, g_strerror(errno));
        left = 0;
        break;
      }
      dl_hash_update(dl, r, buf);
      dl->have += r;
      left -= r;
    }
    if(fd >= 0)
      close(fd);
  }

  g_free(fn);
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
  dl->prio = nfo.dptr[8];
  dl->error = nfo.dptr[9];
  memcpy(&dl->error_sub, nfo.dptr+10, 2);
  dl->error_sub = GINT16_FROM_LE(dl->error_sub);
  dl->dest = g_strdup(nfo.dptr+16);

  // check what we already have
  dl_queue_loadpartial(dl);

  // and insert in the hash tables
  guint64 uid;
  memcpy(&uid, users.dptr+8, 8);
  uid = GINT64_FROM_LE(uid);
  dl_queue_insert(dl, uid, TRUE);
  g_debug("dl:%016"G_GINT64_MODIFIER"x: load `%s' from data file (size = %"G_GUINT64_FORMAT", have = %"G_GUINT64_FORMAT", bs = %"G_GUINT64_FORMAT")", uid, dl->dest, dl->size, dl->have, dl->hash_block);

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
    if(!istth(n))
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
    if(key.dsize == 25 && (str[0] == DLDAT_INFO || str[0] == DLDAT_USERS || str[0] == DLDAT_TTHL)
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

