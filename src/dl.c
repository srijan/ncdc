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

struct dl {
  char hash[24]; // unused - for non-filelists
  gboolean islist;
  guint64 uid;  // user who has this file (should be a list for multi-source downloading)
  int incfd;    // file descriptor for this file in <conf_dir>/inc/
  guint64 size; // total size of the file
  guint64 have; // what we have so far
  char *inc;    // path to the incomplete file
  char *dest;   // destination path (must be on same filesystem as the incomplete file)
  struct cc_expect *expect;
  struct cc *cc;
};

#endif


// Download queue. This should be serialized into a file when we can download
// more than only file lists - since we don't want to lose those from the queue
// after a restart.
//
// Key = TTH (for normal files) or ??? (for file lists)
// Value = struct dl
//
// This table is not necessary at the moment
//static GHashTable *queue = NULL;


// uid -> dl lookup table. Value should be a GSList or something when we can
// download more than only file lists.
static GHashTable *queue_users = NULL;


// This should actually return a list of dls...
struct dl *dl_queue_user(guint64 uid) {
  return g_hash_table_lookup(queue_users, &uid);
}


static void dl_queue_start(struct dl *dl) {
  g_return_if_fail(dl && !(dl->cc || dl->expect));
  // get user/hub
  struct hub_user *u = g_hash_table_lookup(hub_uids, &(dl->uid));
  if(!u)
    return;
  // try to open a C-C connection
  // TODO: re-use an existing download connection if we have one open
  hub_opencc(u->hub, u);
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
  struct dl *dl = dl_queue_user(uid);
  g_return_if_fail(dl);
  dl->expect = e;
  if(!e && !dl->cc)
    dl_queue_start(dl);
}


// set/unset the cc field. When cc goes from NULL to some value, it is expected
// that we're either connected to the user or we're in the remove timeout. When
// it is set, it means we're connected and the download is in progress or is
// being negotiated. Otherwise, it means we've failed to initiate the download
// and should try again.
void dl_queue_cc(guint64 uid, struct cc *cc) {
  struct dl *dl = dl_queue_user(uid);
  g_return_if_fail(dl);
  dl->cc = cc;
  if(!cc && !dl->expect)
    dl_queue_start(dl);
}


// To be called when a user joins a hub. Checks whether we have something to
// get from that user.
void dl_queue_useronline(guint64 uid) {
  struct dl *dl = dl_queue_user(uid);
  if(dl && !dl->expect && !dl->cc)
    dl_queue_start(dl);
}




// Add the file list of some user to the queue
void dl_queue_addlist(struct hub_user *u) {
  g_return_if_fail(u && u->hasinfo && !g_hash_table_lookup(queue_users, &(u->uid)));
  struct dl *dl = g_slice_new0(struct dl);
  dl->islist = TRUE;
  dl->uid = u->uid;
  // figure out dl->dest
  char *fn = g_strdup_printf("%016"G_GINT64_MODIFIER"x.xml.bz2", dl->uid);
  dl->dest = g_build_filename(conf_dir, "fl", fn, NULL);
  // open the incomplete file (actually only needed when writing to it... but
  // keeping it opened for the lifetime of the queue item is easier for now)
  dl->inc = g_build_filename(conf_dir, "inc", fn, NULL);
  g_free(fn);
  dl->incfd = open(dl->inc, O_WRONLY|O_CREAT, 0666);
  if(dl->incfd < 0) {
    g_warning("Error opening %s: %s", dl->inc, g_strerror(errno));
    return;
  }
  // insert & start
  g_debug("dl: Added to queue: %s", dl->dest);
  g_hash_table_insert(queue_users, &(dl->uid), dl);
  dl_queue_start(dl);
}



// Indicates how many bytes we received from a user before we disconnected.
void dl_received(struct dl *dl, guint64 bytes) {
  dl->have += bytes;
  g_debug("dl: Received %"G_GUINT64_FORMAT" bytes for %s (size = %"G_GUINT64_FORMAT", have = %"G_GUINT64_FORMAT")", bytes, dl->dest, dl->size, dl->have);

  // For filelists: Don't allow resuming of the download. It could happen that
  // the client modifies its filelist in between our retries. In that case the
  // downloaded filelist would end up being corrupted. To avoid that: make sure
  // lists are downloaded in one go, and throw away any incomplete data.
  if(dl->have < dl->size) {
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
  // open the file list
  struct ui_tab *t = ui_fl_create(dl->uid, TRUE);
  if(t)
    ui_tab_open(t, FALSE);
  // and free
  g_free(dl->inc);
  g_free(dl->dest);
  g_hash_table_remove(queue_users, &(dl->uid));
  g_slice_free(struct dl, dl);
}





void dl_init_global() {
  queue_users = g_hash_table_new(g_int64_hash, g_int64_equal);
}


void dl_close_global() {
  // Delete incomplete files. They won't be completed anyway, since we don't
  // have a persistent download queue at the moment.
  GHashTableIter iter;
  struct dl *dl;
  g_hash_table_iter_init(&iter, queue_users);
  while(g_hash_table_iter_next(&iter, NULL, (gpointer *)&dl))
    unlink(dl->inc);
}

