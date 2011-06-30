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

struct dl {
  char hash[24];
  gboolean islist;
  struct cc_expect *expect;
  struct cc *cc;
};

#endif


// Download queue. This should be serialized into a file when we can download
// more than only file lists - since we don't want to lose those from the queue
// after a restart.
//
// Key = TTH (for normal files) or CID (for file lists)
// Value = struct dl
static GHashTable *queue = NULL;


// As we currently only support file list downloads, the CID = key to the queue.
// In the rest of the code I'm assuming this function to be fast, so we'd still
// need a CID->download lookup table when supporting file downloads.
// This should actually return a list of dls...
static struct dl *dl_queue_user(char *cid) {
  return g_hash_table_lookup(queue, cid);
}


// Check whether we have something new to do for a particular user.
gboolean dl_queue_needaction(char *cid) {
  struct dl *dl = dl_queue_user(cid);
  return dl && !dl->expect && !dl->cc;
}


// set/unset the expect field. When expect goes from NULL to some value, it is
// expected that the connection is being established. When it goes from some
// value to NULL, it is expected that either the connection has been
// established (and dl_queue_cc() is called), or the connection timed out (in
// which case we should try again).
void dl_queue_expect(char *cid, struct cc_expect *e) {
  struct dl *dl = dl_queue_user(cid);
  g_return_if_fail(dl);
  dl->expect = e;
  if(!e && !dl->cc)
    ; // TODO: re-initiate the connection
}


// set/unset the cc field. When cc goes from NULL to some value, it is expected
// that we're either connected to the user or we're in the remove timeout. When
// it is set, it means we're connected and the download is in progress or is
// being negotiated. Otherwise, it means we've failed to initiate the download
// and should try again.
void dl_queue_cc(char *cid, struct cc *cc) {
  struct dl *dl = dl_queue_user(cid);
  g_return_if_fail(dl);
  dl->cc = cc;
  if(!cc && !dl->expect)
    ; // TODO: re-initiate the connection
}




// Add the file list of some user to the queue
void dl_queue_addlist(struct hub_user *u) {
  g_return_if_fail(u && u->hasinfo && !g_hash_table_lookup(queue, u->cid));
  struct dl *dl = g_slice_new0(struct dl);
  dl->islist = TRUE;
  memcpy(dl->hash, u->cid, 24);
  g_hash_table_insert(queue, dl->hash, dl);
  // TODO: initiate connection
}





void dl_init_global() {
  queue = g_hash_table_new(g_int_hash, tiger_hash_equal);
}

