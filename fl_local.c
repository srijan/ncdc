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
#include <stdlib.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <gdbm.h>


char           *fl_own_list_file;
struct fl_list *fl_own_list  = NULL;
gboolean        fl_is_refreshing = FALSE;
static gboolean fl_own_needflush = FALSE;

static GThreadPool *fl_scan_pool;
static GThreadPool *fl_hash_pool;
static GSList *fl_hash_queue = NULL; // more like a stack, but hashing order doesn't matter anyway
static int     fl_hash_reset = 0;    // increased every time fl_hash_queue is re-generated
int            fl_hash_queue_length = 0;
guint64        fl_hash_queue_size = 0;
static char      *fl_hashdat_file;
static GDBM_FILE  fl_hashdat;



// Utility functions

// Get full path to an item in our list. Result should be free'd. This function
// isn't particularly fast.
static char *fl_own_path(struct fl_list *fl) {
  char *tmp, *root, *path = g_strdup(fl->name);
  struct fl_list *cur = fl->parent;
  while(cur->parent && cur->parent->parent) {
    tmp = path;
    path = g_build_filename(cur->name, path, NULL);
    g_free(tmp);
    cur = cur->parent;
  }
  root = g_key_file_get_string(conf_file, "share", cur->name, NULL);
  tmp = path;
  path = g_build_filename(root, path, NULL);
  g_free(root);
  g_free(tmp);
  return path;
}


// should be run from a timer. periodically flushes all unsaved data to disk.
static gboolean fl_flush(gpointer dat) {
  if(fl_own_needflush && fl_own_list) {
    // save our file list
    GError *err = NULL;
    if(!fl_save(fl_own_list, fl_own_list_file, &err)) {
      // this is a pretty fatal error... oh well, better luck next time
      ui_msgf(UIMSG_MAIN, "Error saving file list: %s", err->message);
      g_error_free(err);
    }

    // sync the hash data
    gdbm_sync(fl_hashdat);
  }
  fl_own_needflush = FALSE;
  return TRUE;
}


// flush and close
void fl_close() {
  fl_flush(NULL);
  gdbm_close(fl_hashdat);
}





// Hash data file interface

#define HASHDAT_INFO 0
#define HASHDAT_TTHL 1

struct fl_hashdat_info {
  time_t lastmod;
  guint64 filesize;
  guint64 blocksize;
};


static void fl_hashdat_open(gboolean trash) {
  fl_hashdat = gdbm_open(fl_hashdat_file, 0, trash ? GDBM_NEWDB : GDBM_WRCREAT, 0600, NULL);
  // this is a bit extreme, but I have no idea what else to do
  if(!fl_hashdat)
    g_error("Unable to open hashdata.dat.");
}


static gboolean fl_hashdat_getinfo(const char *tth, struct fl_hashdat_info *nfo) {
  char key[25];
  datum keydat = { key, 25 };
  key[0] = HASHDAT_INFO;
  memcpy(key+1, tth, 24);
  datum res = gdbm_fetch(fl_hashdat, keydat);
  if(res.dsize <= 0)
    return FALSE;
  g_assert(res.dsize >= 3*8);
  guint64 *r = (guint64 *)res.dptr;
  nfo->lastmod = GINT64_FROM_LE(r[0]);
  nfo->filesize = GINT64_FROM_LE(r[1]);
  nfo->blocksize = GINT64_FROM_LE(r[2]);
  free(res.dptr);
  return TRUE;
}


static void fl_hashdat_set(const char *tth, const struct fl_hashdat_info *nfo, const char *blocks) {
  char key[25];
  datum keydat = { key, 25 };

  datum val = { (char *)blocks, 24*tth_num_blocks(nfo->filesize, nfo->blocksize) };
  memcpy(key+1, tth, 24);
  key[0] = HASHDAT_TTHL;
  gdbm_store(fl_hashdat, keydat, val, GDBM_REPLACE);

  key[0] = HASHDAT_INFO;
  guint64 info[3] = { GINT64_TO_LE(nfo->lastmod), GINT64_TO_LE(nfo->filesize), GINT64_TO_LE(nfo->blocksize) };
  val.dptr = (char *)info;
  val.dsize = 3*8;
  gdbm_store(fl_hashdat, keydat, val, GDBM_REPLACE);
}


static void fl_hashdat_del(const char *tth) {
  char key[25];
  datum keydat = { key, 25 };
  key[0] = HASHDAT_INFO;
  memcpy(key+1, tth, 24);
  gdbm_delete(fl_hashdat, keydat);
  key[0] = HASHDAT_TTHL;
  gdbm_delete(fl_hashdat, keydat);
}





// Scanning directories

struct fl_scan_args {
  char **name, **path;
  gboolean (*donefun)(gpointer);
};

// recursive
// Doesn't handle paths longer than PATH_MAX, but I don't think it matters all that much.
static void fl_scan_dir(struct fl_list *parent, const char *path) {
  GError *err = NULL;
  GDir *dir = g_dir_open(path, 0, &err);
  if(!dir) {
    ui_msgf(UIMSG_MAIN, "Error reading directory \"%s\": %s", path, g_strerror(errno));
    g_error_free(err);
    return;
  }
  const char *name;
  while((name = g_dir_read_name(dir))) {
    if(strcmp(name, ".") == 0 || strcmp(name, "..") == 0)
      continue;
    // Try to get a UTF-8 filename which can be converted back.  If it can't be
    // converted back, we won't share the file at all. Keeping track of a
    // raw-to-UTF-8 filename lookup table isn't worth the effort.
    char *confname = g_filename_to_utf8(name, -1, NULL, NULL, NULL);
    if(!confname)
      confname = g_filename_display_name(name);
    char *encname = g_filename_from_utf8(confname, -1, NULL, NULL, NULL);
    if(!encname) {
      ui_msgf(UIMSG_MAIN, "Error reading directory entry in \"%s\": Invalid encoding.");
      g_free(confname);
      continue;
    }
    char *cpath = g_build_filename(path, encname, NULL);
    struct stat dat;
    // we're currently following symlinks, but I'm not sure whether that's a good idea yet
    int r = stat(cpath, &dat);
    g_free(encname);
    g_free(cpath);
    if(r < 0 || !(S_ISREG(dat.st_mode) || S_ISDIR(dat.st_mode))) {
      if(r < 0)
        ui_msgf(UIMSG_MAIN, "Error stat'ing \"%s\": %s", cpath, g_strerror(errno));
      else
        ui_msgf(UIMSG_MAIN, "Not sharing \"%s\": Neither file nor directory.", cpath);
      g_free(confname);
      continue;
    }
    // and create the node
    struct fl_list *cur = g_new0(struct fl_list, 1);
    cur->name = confname;
    if(S_ISREG(dat.st_mode)) {
      cur->isfile = TRUE;
      cur->size = dat.st_size;
      cur->lastmod = dat.st_mtime;
    }
    fl_list_add(parent, cur);
  }
  g_dir_close(dir);

  // check for directories (outside of the above loop, to avoid having too many
  // directories opened at the same time. Costs some extra CPU cycles, though...)
  GSequenceIter *iter;
  for(iter=g_sequence_get_begin_iter(parent->sub); !g_sequence_iter_is_end(iter); iter=g_sequence_iter_next(iter)) {
    struct fl_list *cur = g_sequence_get(iter);
    g_assert(cur);
    if(!cur->isfile) {
      char *cpath = g_build_filename(path, cur->name, NULL);
      cur->sub = g_sequence_new(fl_list_free);
      fl_scan_dir(cur, cpath);
      g_free(cpath);
    }
  }
}


// Must be called in a separate thread. The ownership over the first argument
// is passed to this function, and should not be relied upon after calling
// g_thread_pool_push().
static void fl_scan_thread(gpointer data, gpointer udata) {
  struct fl_scan_args *args = data;

  struct fl_list *root = g_new0(struct fl_list, 1);
  root->sub = g_sequence_new(fl_list_free);

  int i, len = g_strv_length(args->name);
  for(i=0; i<len; i++) {
    struct fl_list *cur = g_new0(struct fl_list, 1);
    char *tmp = g_filename_from_utf8(args->path[i], -1, NULL, NULL, NULL);
    cur->sub = g_sequence_new(fl_list_free);
    cur->name = g_strdup(args->name[i]);
    fl_scan_dir(cur, tmp);
    g_free(tmp);
    fl_list_add(root, cur);
  }

  g_idle_add_full(G_PRIORITY_HIGH_IDLE, args->donefun, root, NULL);
  g_strfreev(args->name);
  g_strfreev(args->path);
  g_free(args);
}






// Hashing files


// This struct is passed from the main thread to the hasher and back with modifications.
struct fl_hash_args {
  struct fl_list *file; // only accessed from main thread
  char *path;        // owned by main thread, read from hash thread
  guint64 filesize;  // set by main thread
  guint64 blocksize; // set by hash thread
  char root[24];     // set by hash thread
  char *blocks;      // allocated by hash thread, ownership passed to main
  GError *err;       // set by hash thread
  time_t lastmod;    // set by hash thread
  gdouble time;      // set by hash thread
  int resetnum;      // used by main thread to validate that *file is still alive (results are discarded otherwise)
};

static gboolean fl_hash_done(gpointer dat);

static void fl_hash_thread(gpointer data, gpointer udata) {
  struct fl_hash_args *args = data;
  // static, since only one hash thread is allowed and this saves stack space
  static struct tth_ctx tth;
  static char buf[10240];

  time(&(args->lastmod));
  GTimer *tm = g_timer_new();

  FILE *f = NULL;
  if(!(f = fopen(args->path, "r"))) {
    g_set_error(&(args->err), 1, 0, "Error reading file: %s", g_strerror(errno));
    goto fl_hash_finish;
  }

  tth_init(&tth, args->filesize);
  int r;
  while((r = fread(buf, 1, 10240, f)) > 0) {
    // no need to hash any further? quit!
    if(g_atomic_int_get(&fl_hash_reset) != args->resetnum)
      goto fl_hash_finish;
    // file has been modified. time to back out
    if(tth.totalsize+(guint64)r > args->filesize) {
      g_set_error_literal(&(args->err), 1, 0, "File has been modified.");
      goto fl_hash_finish;
    }
    tth_update(&tth, buf, r);
  }
  if(ferror(f)) {
    g_set_error(&(args->err), 1, 0, "Error reading file: %s", g_strerror(errno));
    goto fl_hash_finish;
  }
  if(tth.totalsize != args->filesize) {
    g_set_error_literal(&(args->err), 1, 0, "File has been modified.");
    goto fl_hash_finish;
  }

  tth_final(&tth, args->root);
  args->blocksize = tth.blocksize;
  g_assert(tth.lastblock == tth_num_blocks(args->filesize, tth.blocksize));
  args->blocks = g_memdup(tth.blocks, tth.lastblock*24);

fl_hash_finish:
  if(f)
    fclose(f);
  args->time = g_timer_elapsed(tm, NULL);
  g_timer_destroy(tm);
  g_idle_add_full(G_PRIORITY_HIGH_IDLE, fl_hash_done, args, NULL);
}


// actually does a prepend, but whatever
static void fl_hash_queue_append(struct fl_list *fl) {
  fl_hash_queue = g_slist_prepend(fl_hash_queue, fl);
  fl_hash_queue_length++;
  fl_hash_queue_size += fl->size;
}


static void fl_hash_process() {
  if(!fl_hash_queue)
    return;
  struct fl_list *file = fl_hash_queue->data;
  fl_hash_queue = g_slist_remove_link(fl_hash_queue, fl_hash_queue);

  struct fl_hash_args *args = g_new0(struct fl_hash_args, 1);
  args->file = file;
  char *tmp = fl_own_path(file);
  args->path = g_filename_from_utf8(tmp, -1, NULL, NULL, NULL);
  g_free(tmp);
  args->filesize = file->size;
  args->resetnum = g_atomic_int_get(&fl_hash_reset);
  g_thread_pool_push(fl_hash_pool, args, NULL);
}


static gboolean fl_hash_done(gpointer dat) {
  struct fl_hash_args *args = dat;
  struct fl_list *fl;

  // discard this hash if the queue/filelist has been modified
  if(g_atomic_int_get(&fl_hash_reset) != args->resetnum)
    goto fl_hash_done_f;

  fl = args->file;
  fl_hash_queue_length--;
  fl_hash_queue_size -= fl->size;

  if(args->err) {
    ui_msgf(UIMSG_MAIN, "Error hashing \"%s\": %s", args->path, args->err->message);
    g_error_free(args->err);
    goto fl_hash_done_f;
  }

  // update file info
  memcpy(fl->tth, args->root, 24);
  if(!fl->hastth)
    fl->parent->hastth++;
  fl->hastth = 1;
  fl->lastmod = args->lastmod;

  // save to hashdata file
  struct fl_hashdat_info nfo;
  nfo.lastmod = fl->lastmod;
  nfo.filesize = fl->size;
  nfo.blocksize = args->blocksize;
  fl_hashdat_set(fl->tth, &nfo, args->blocks);
  g_free(args->blocks);

  ui_msgf(UIMSG_MAIN, "Finished hashing %s. [%.2f MiB/s]", fl->name, ((double)fl->size)/(1024.0*1024.0)/args->time);
  fl_own_needflush = TRUE;

  // Hash next file in the queue
  fl_hash_process();

fl_hash_done_f:
  g_free(args->path);
  g_free(args);
  return FALSE;
}





// Refresh filelist

static void fl_refresh_addhash(struct fl_list *cur) {
  GSequenceIter *i;
  for(i=g_sequence_get_begin_iter(cur->sub); !g_sequence_iter_is_end(i); i=g_sequence_iter_next(i)) {
    struct fl_list *l = g_sequence_get(i);
    if(l->isfile)
      fl_hash_queue_append(l);
    else
      fl_refresh_addhash(l);
  }
}


static void fl_refresh_delhash(struct fl_list *cur) {
  if(cur->isfile && cur->hastth) {
    // TODO: don't delete the hash data when we have another file with the
    // same hash in our filelist. (Either that, or we should disallow
    // sharing the same file more than once).
    fl_hashdat_del(cur->tth);
  } else if(!cur->isfile) {
    GSequenceIter *i;
    for(i=g_sequence_get_begin_iter(cur->sub); !g_sequence_iter_is_end(i); i=g_sequence_iter_next(i))
      fl_refresh_delhash(g_sequence_get(i));
  }
}


static void fl_refresh_compare(struct fl_list *own, struct fl_list *new) {
  GSequenceIter *owni = g_sequence_get_begin_iter(own->sub);
  GSequenceIter *newi = g_sequence_get_begin_iter(new->sub);
  while(!g_sequence_iter_is_end(owni) || !g_sequence_iter_is_end(newi)) {
    struct fl_list *ownl = g_sequence_iter_is_end(owni) ? NULL : g_sequence_get(owni);
    struct fl_list *newl = g_sequence_iter_is_end(newi) ? NULL : g_sequence_get(newi);
    int cmp = !ownl ? 1 : !newl ? -1 : fl_list_cmp(ownl, newl, NULL);

    // own == new: same
    if(cmp == 0) {
      g_assert(ownl->isfile == newl->isfile); // TODO: handle this case
      if(ownl->isfile && (!ownl->hastth || newl->lastmod > ownl->lastmod || newl->size != ownl->size))
        fl_hash_queue_append(ownl);
      if(!ownl->isfile)
        fl_refresh_compare(ownl, newl);
      owni = g_sequence_iter_next(owni);
      newi = g_sequence_iter_next(newi);

    // own > new: insert new
    } else if(cmp > 0) {
      struct fl_list *tmp = fl_list_copy(newl);
      fl_list_add(own, tmp);
      if(tmp->isfile)
        fl_hash_queue_append(tmp);
      else
        fl_refresh_addhash(tmp);
      newi = g_sequence_iter_next(newi);

    // new > own: delete own
    } else {
      owni = g_sequence_iter_next(owni);
      fl_refresh_delhash(ownl);
      fl_list_remove(ownl);
    }
  }
  own->incomplete = FALSE;
}

/* A visual explanation of the above algorithm:
 * own new
 *  a  a  same (new == own; new++, own++)
 *  b  b  same
 *  d  c  insert c (!own || new < own; new++, own stays)
 *  d  d  same
 *  e  f  delete e (!new || new > own; new stays, own++)
 *  f  f  same
 *
 * More advanced example:
 *  a  c  delete a
 *  b  c  delete b
 *  e  c  insert c
 *  e  d  insert d
 *  e  e  same
 *  f  -  delete f
 */


static gboolean fl_refresh_scanned(gpointer dat) {
  struct fl_list *list = dat;

  fl_is_refreshing = FALSE;

  // Note: we put *all* to-be-hashed files in a queue. This queue can be pretty
  // huge. It may be a better idea to put a maximum on it and check the actual
  // file list for more to-be-hashed files each time the queue has been
  // depleted. This probably requires a "needsrehash" flag in struct fl_list.
  g_atomic_int_inc(&fl_hash_reset);
  g_slist_free(fl_hash_queue);
  fl_hash_queue = NULL;
  fl_hash_queue_length = 0;
  fl_hash_queue_size = 0;

  if(!fl_own_list) {
    fl_own_list = list;
    fl_refresh_addhash(list);
  } else {
    // TODO: handle partial refreshes
    fl_refresh_compare(fl_own_list, list);
    fl_list_free(list);
  }

  // initiate hashing (if necessary)
  fl_hash_process();

  // Force a flush
  fl_own_needflush = TRUE;
  fl_flush(NULL);
  ui_msgf(UIMSG_NOTIFY, "File list refresh finished.");
  return FALSE;
}


// TODO: specifying a dir does not really work yet.
void fl_refresh(const char *dir) {
  fl_is_refreshing = TRUE;
  // TODO: allow only one refresh at a time?

  // construct the list of to-be-scanned directories
  struct fl_scan_args *args = g_new0(struct fl_scan_args, 1);
  args->donefun = fl_refresh_scanned;
  if(dir) {
    args->name = g_new0(char *, 2);
    args->path = g_new0(char *, 2);
    args->name[0] = g_strdup(dir);
    args->path[0] = g_key_file_get_string(conf_file, "share", dir, NULL);
  } else {
    args->name = g_key_file_get_keys(conf_file, "share", NULL, NULL);
    int i, len = g_strv_length(args->name);
    args->path = g_new0(char *, len+1);
    for(i=0; i<len; i++)
      args->path[i] = g_key_file_get_string(conf_file, "share", args->name[i], NULL);
  }

  // scan the requested directories in the background
  g_thread_pool_push(fl_scan_pool, args, NULL);
}






// Initialize local filelist


// fetches the last modification times from the hashdata file and checks
// whether we have any incomplete directories.
static gboolean fl_init_getlastmod(struct fl_list *fl) {
  struct fl_hashdat_info nfo = {};
  GSequenceIter *iter;
  gboolean incomplete = FALSE;
  for(iter=g_sequence_get_begin_iter(fl->sub); !g_sequence_iter_is_end(iter); iter=g_sequence_iter_next(iter)) {
    struct fl_list *c = g_sequence_get(iter);
    if(c->isfile && c->hastth && fl_hashdat_getinfo(c->tth, &nfo))
      c->lastmod = nfo.lastmod;
    if(!c->isfile && (fl_init_getlastmod(c) || c->incomplete))
      incomplete = TRUE;
  }
  return incomplete;
}


void fl_init() {
  GError *err = NULL;
  gboolean dorefresh = FALSE;

  // init stuff
  fl_own_list = NULL;
  fl_own_list_file = g_build_filename(conf_dir, "files.xml.bz2", NULL);
  fl_hashdat_file = g_build_filename(conf_dir, "hashdata.dat", NULL);
  fl_scan_pool = g_thread_pool_new(fl_scan_thread, NULL, 1, FALSE, NULL);
  fl_hash_pool = g_thread_pool_new(fl_hash_thread, NULL, 1, FALSE, NULL);

  // read config
  char **shares = g_key_file_get_keys(conf_file, "share", NULL, NULL);
  if(!shares || !g_strv_length(shares)) {
    fl_hashdat_open(TRUE);
    g_strfreev(shares);
    return;
  }

  // load our files.xml.bz
  fl_own_list = fl_load(fl_own_list_file, &err);
  if(!fl_own_list) {
    g_assert(err);
    ui_msgf(UIMSG_MAIN, "Error loading own filelist: %s. Re-building list.", err->message);
    g_error_free(err);
    dorefresh = TRUE;
    fl_hashdat_open(TRUE);
  } else
    fl_hashdat_open(FALSE);

  // Get last modification times, check for any incomplete directories and
  // initiate a refresh if there is one.  (If there is an incomplete directory,
  // it means that ncdc was closed while it was hashing files, a refresh will
  // continue where it left off)
  if(fl_own_list) {
    dorefresh = fl_init_getlastmod(fl_own_list);
    if(dorefresh)
      ui_msg(UIMSG_NOTIFY, "File list incomplete, refreshing...");
  }

  // Note: LinuxDC++ (and without a doubt other clients as well) force a
  // refresh on startup. I can't say I'm a huge fan of that, but maybe it
  // should be an option?
  if(dorefresh)
    fl_refresh(NULL);

  // flush unsaved data to disk every 60 seconds
  g_timeout_add_seconds_full(G_PRIORITY_LOW, 60, fl_flush, NULL, NULL);

  g_strfreev(shares);
}

