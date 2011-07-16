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
#include <errno.h>

#include <sys/stat.h>
#include <fcntl.h>


#if INTERFACE

// no need to make this one public
struct dl_user;

struct dl {
  char hash[24]; // TTH for files, tiger(uid) for filelists
  gboolean islist;
  struct dl_user *u; // user who has this file (should be a list for multi-source downloading)
  int incfd;    // file descriptor for this file in <conf_dir>/inc/
  guint64 size; // total size of the file
  guint64 have; // what we have so far
  char *inc;    // path to the incomplete file (/inc/<base32-hash>)
  char *dest;   // destination path (must be on same filesystem as the incomplete file)
};

#endif


struct dl_user {
  guint64 uid;
  struct cc_expect *expect;
  struct cc *cc;
  GQueue queue; // queue of struct dl's. First item = active
};


// Download queue. This should be serialized into a file when we can download
// more than only file lists - since we don't want to lose those from the queue
// after a restart.
//
// Key = dl->hash
// Value = struct dl
static GHashTable *queue = NULL;


// uid -> dl_user lookup table.
static GHashTable *queue_users = NULL;


// Returns the first item in the download queue for this user.
struct dl *dl_queue_user(guint64 uid) {
  struct dl_user *du = g_hash_table_lookup(queue_users, &uid);
  return du ? du->queue.head->data : NULL;
}


static void dl_queue_start(struct dl *dl) {
  g_return_if_fail(dl && dl->u && !(dl->u->cc || dl->u->expect));
  // get user/hub
  struct hub_user *u = g_hash_table_lookup(hub_uids, &dl->u->uid);
  if(!u)
    return;
  g_debug("dl:%016"G_GINT64_MODIFIER"x: trying to open a connection", u->uid);
  // try to open a C-C connection
  // TODO: re-use an existing download connection if we have one open
  hub_opencc(u->hub, u);
}


// Adds a dl item to the queue. dl->inc will be determined and opened here.
static void dl_queue_insert(struct dl *dl, guint64 uid) {
  // figure out dl->inc
  char hash[40] = {};
  base32_encode(dl->hash, hash);
  dl->inc = g_build_filename(conf_dir, "inc", hash, NULL);
  // open the incomplete file (actually only needed when writing to it... but
  // keeping it opened for the lifetime of the queue item is easier for now)
  dl->incfd = open(dl->inc, O_WRONLY|O_CREAT, 0666);
  if(dl->incfd < 0) {
    g_warning("Error opening %s: %s", dl->inc, g_strerror(errno));
    return;
  }
  // create or re-use dl_user struct
  dl->u = g_hash_table_lookup(queue_users, &uid);
  if(!dl->u) {
    dl->u = g_slice_new0(struct dl_user);
    dl->u->uid = uid;
    g_queue_init(&dl->u->queue);
    g_hash_table_insert(queue_users, &dl->u->uid, dl->u);
  }
  g_queue_push_tail(&dl->u->queue, dl);
  // insert and start
  g_hash_table_insert(queue, dl->hash, dl);
  dl_queue_start(dl);
}


// removes an item from the queue
static void dl_queue_rm(struct dl *dl) {
  // update and optionally remove dl_user struct
  g_queue_remove(&dl->u->queue, dl);
  if(!dl->u->queue.head) {
    g_hash_table_remove(queue_users, &dl->u->uid);
    g_slice_free(struct dl_user, dl->u);
  }
  // free and remove dl struct
  g_free(dl->inc);
  g_free(dl->dest);
  g_hash_table_remove(queue, dl->hash);
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
  g_return_if_fail(du);
  du->expect = e;
  if(!e && !du->cc)
    dl_queue_start(du->queue.head->data); // TODO: this only works with single-source downloading
}


// set/unset the cc field. When cc goes from NULL to some value, it is expected
// that we're either connected to the user or we're in the remove timeout. When
// it is set, it means we're connected and the download is in progress or is
// being negotiated. Otherwise, it means we've failed to initiate the download
// and should try again.
void dl_queue_cc(guint64 uid, struct cc *cc) {
  g_debug("dl:%016"G_GINT64_MODIFIER"x: cc = %s", uid, cc?"true":"false");
  struct dl_user *du = g_hash_table_lookup(queue_users, &uid);
  g_return_if_fail(du);
  du->cc = cc;
  if(!cc && !du->expect)
    dl_queue_start(du->queue.head->data); // TODO: this only works with single-source downloading
}


// To be called when a user joins a hub. Checks whether we have something to
// get from that user.
void dl_queue_useronline(guint64 uid) {
  struct dl_user *du = g_hash_table_lookup(queue_users, &uid);
  if(du && !du->cc && !du->expect)
    dl_queue_start(du->queue.head->data); // TODO: this only works with single-source downloading
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
  if(g_hash_table_lookup(queue, dl->hash)) {
    g_warning("dl:%016"G_GINT64_MODIFIER"x: files.xml.bz2 already in the queue.", u->uid);
    g_slice_free(struct dl, dl);
    return;
  }
  // figure out dl->dest
  char *fn = g_strdup_printf("%016"G_GINT64_MODIFIER"x.xml.bz2", u->uid);
  dl->dest = g_build_filename(conf_dir, "fl", fn, NULL);
  g_free(fn);
  // insert & start
  dl_queue_insert(dl, u->uid);
  g_debug("dl:%016"G_GINT64_MODIFIER"x: queueing files.xml.bz2", u->uid);
}



// Indicates how many bytes we received from a user before we disconnected.
void dl_received(struct dl *dl, guint64 bytes) {
  dl->have += bytes;
  g_debug("dl:%016"G_GINT64_MODIFIER"x: Received %"G_GUINT64_FORMAT" bytes for %s (size = %"G_GUINT64_FORMAT", have = %"G_GUINT64_FORMAT")", dl->u->uid, bytes, dl->dest, dl->size, dl->have);

  // For filelists: Don't allow resuming of the download. It could happen that
  // the client modifies its filelist in between our retries. In that case the
  // downloaded filelist would end up being corrupted. To avoid that: make sure
  // lists are downloaded in one go, and throw away any incomplete data.
  if(dl->islist && dl->have < dl->size) {
    dl->have = dl->size = 0;
    g_return_if_fail(close(dl->incfd) == 0);
    dl->incfd = open(dl->inc, O_WRONLY|O_CREAT|O_TRUNC, 0666);
    g_return_if_fail(dl->incfd >= 0);
    return;
  }

  // For non-filelists:
  // If we're not done yet, let it download more in a next try. (dl_queue_cc()
  // is called automatically after the connection with the user has been
  // removed, and that will continue the download)
  //if(dl->have < dl->size)
  //  return;

  // Now we have a completed download. Rename it to the final destination.
  g_return_if_fail(close(dl->incfd) == 0);
  g_return_if_fail(rename(dl->inc, dl->dest) == 0);
  g_debug("dl:%016"G_GINT64_MODIFIER"x: download finished, removing from queue", dl->u->uid);
  // open the file list
  if(dl->islist) {
    struct ui_tab *t = ui_fl_create(dl->u->uid, TRUE);
    if(t)
      ui_tab_open(t, FALSE);
  }
  // and remove
  dl_queue_rm(dl);
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


void dl_init_global() {
  queue_users = g_hash_table_new(g_int64_hash, g_int64_equal);
  queue = g_hash_table_new(g_int_hash, tiger_hash_equal);
  // Delete old filelists
  dl_fl_clean(NULL);
}


void dl_close_global() {
  // Delete incomplete files. They won't be completed anyway, since we don't
  // have a persistent download queue at the moment.
  GHashTableIter iter;
  struct dl *dl;
  g_hash_table_iter_init(&iter, queue_users);
  while(g_hash_table_iter_next(&iter, NULL, (gpointer *)&dl))
    unlink(dl->inc);
  // Delete old filelists
  dl_fl_clean(NULL);
}

