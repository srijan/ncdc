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
#include <sys/stat.h>
#include <gdbm.h>


char           *fl_local_list_file;
struct fl_list *fl_local_list  = NULL;
GQueue         *fl_refresh_queue = NULL;
time_t          fl_refresh_last = 0;
static gboolean fl_needflush = FALSE;
// index of the files in fl_local_list. Key = TTH root, value = GSList of files
static GHashTable *fl_hash_index;
guint64         fl_local_list_size; // total share size, minus duplicate files

static GThreadPool *fl_scan_pool;
static GThreadPool *fl_hash_pool;

GHashTable            *fl_hash_queue = NULL; // set of files-to-hash
guint64                fl_hash_queue_size = 0;
static struct fl_list *fl_hash_cur = NULL;   // most recent file initiated for hashing
static int             fl_hash_reset = 0;    // increased when fl_hash_cur is removed from the queue
struct ratecalc        fl_hash_rate;

static char       *fl_hashdat_file;
static GDBM_FILE   fl_hashdat;



// Utility functions

// Get full path to an item in our list. Result should be free'd. This function
// isn't particularly fast.
char *fl_local_path(struct fl_list *fl) {
  if(!fl->parent->parent)
    return g_key_file_get_string(conf_file, "share", fl->name, NULL);
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


// Similar to fl_list_from_path, except this function starts from the root of
// the local filelist, and also accepts filesystem paths. In the latter case,
// the path must be absolute and "real" (i.e., what realpath() would return),
// since that is the path that is stored in the config file. This function
// makes no attempt to convert the given path into a real path.
struct fl_list *fl_local_from_path(const char *path) {
  struct fl_list *n = fl_list_from_path(fl_local_list, path);
  if(!n && path[0] == '/') {
    char **names = g_key_file_get_keys(conf_file, "share", NULL, NULL);
    char **name;
    char *cpath = NULL;
    for(name=names; name && *name; name++) {
      cpath = g_key_file_get_string(conf_file, "share", *name, NULL);
      if(strncmp(path, cpath, strlen(cpath)) == 0)
        break;
      g_free(cpath);
    }
    if(name && *name) {
      char *npath = g_strconcat(*name, path+strlen(cpath), NULL);
      n = fl_list_from_path(fl_local_list, npath);
      g_free(cpath);
      g_free(npath);
    }
    g_strfreev(names);
  }
  return n;
}


// Auto-complete for fl_local_from_path()
void fl_local_suggest(char *path, char **sug) {
  fl_list_suggest(fl_local_list, path, sug);
  if(!sug[0])
    path_suggest(path, sug);
}


// get files with the (raw) TTH. Result does not have to be freed.
GSList *fl_local_from_tth(const char *root) {
  return g_hash_table_lookup(fl_hash_index, root);
}


// should be run from a timer. periodically flushes all unsaved data to disk.
gboolean fl_flush(gpointer dat) {
  if(fl_needflush) {
    // save our file list
    if(fl_local_list) {
      GError *err = NULL;
      if(!fl_save(fl_local_list, fl_local_list_file, &err)) {
        // this is a pretty fatal error... oh well, better luck next time
        ui_msgf(UIMSG_MAIN, "Error saving file list: %s", err->message);
        g_error_free(err);
      }
    } else
      unlink(fl_local_list_file);

    // sync the hash data
    gdbm_sync(fl_hashdat);
  }
  fl_needflush = FALSE;
  return TRUE;
}





// Hash data file interface (these operate on hashdata.dat)

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
  memcpy(key+1, tth, 24);

  key[0] = HASHDAT_TTHL;
  datum val = { (char *)blocks, 24*tth_num_blocks(nfo->filesize, nfo->blocksize) };
  gdbm_store(fl_hashdat, keydat, val, GDBM_REPLACE);

  key[0] = HASHDAT_INFO;
  guint64 info[] = { GINT64_TO_LE(nfo->lastmod), GINT64_TO_LE(nfo->filesize), GINT64_TO_LE(nfo->blocksize) };
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


// return value must be freed using free()
char *fl_hashdat_get(const char *tth, int *len) {
  char key[25];
  datum keydat = { key, 25 };
  key[0] = HASHDAT_TTHL;
  memcpy(key+1, tth, 24);
  datum res = gdbm_fetch(fl_hashdat, keydat);
  if(res.dsize <= 0)
    return NULL;
  if(len)
    *len = res.dsize;
  return res.dptr;
}





// Hash index interface (these operate on both fl_hash_index and the above hashdata.dat)
// These functions are also responsible for updating fl_local_list_size.

// low-level insert-into-index function
static void fl_hashindex_insert(struct fl_list *fl) {
  GSList *cur = g_hash_table_lookup(fl_hash_index, fl->tth);
  if(cur) {
    g_assert(cur == g_slist_insert(cur, fl, 1)); // insert item without modifying the pointer
  } else {
    cur = g_slist_prepend(cur, fl);
    g_hash_table_insert(fl_hash_index, fl->tth, cur);
    fl_local_list_size += fl->size;
  }
}


// Load a file entry that was loaded from our local filelist (files.xml.bz2)
// Cross-checks with hashdata.dat, sets lastmod field and adds it to the index.
// Returns true if hash data was not found.
static gboolean fl_hashindex_load(struct fl_list *fl) {
  if(!fl->hastth)
    return TRUE;

  struct fl_hashdat_info nfo = {};
  if(!fl_hashdat_getinfo(fl->tth, &nfo)) {
    fl->hastth = FALSE;
    return TRUE;
  }
  fl->lastmod = nfo.lastmod;

  fl_hashindex_insert(fl);
  return FALSE;
}


// Should be called when a file is removed from our list. Removes the item from
// the index and removes, when necessary, the associated hash data.
static void fl_hashindex_del(struct fl_list *fl) {
  if(!fl->hastth)
    return;
  GSList *cur = g_hash_table_lookup(fl_hash_index, fl->tth);
  if(!cur) {
    fl_hashdat_del(fl->tth);
    return;
  }

  cur = g_slist_remove(cur, fl);
  if(!cur) {
    fl_hashdat_del(fl->tth);
    g_hash_table_remove(fl_hash_index, fl->tth);
    fl_local_list_size -= fl->size;
  // there's another file with the same TTH.
  } else
    g_hash_table_replace(fl_hash_index, ((struct fl_list *)cur->data)->tth, cur);
}


// update hash info for a file.
static void fl_hashindex_sethash(struct fl_list *fl, char *tth, time_t lastmod, guint64 blocksize, char *blocks) {
  fl_hashindex_del(fl);

  // update file
  memcpy(fl->tth, tth, 24);
  if(!fl->hastth)
    fl->parent->hastth++;
  fl->hastth = 1;
  fl->lastmod = lastmod;
  fl_hashindex_insert(fl);

  // save to hashdata file
  struct fl_hashdat_info nfo;
  nfo.lastmod = fl->lastmod;
  nfo.filesize = fl->size;
  nfo.blocksize = blocksize;
  fl_hashdat_set(fl->tth, &nfo, blocks);
}





// Scanning directories

struct fl_scan_args {
  struct fl_list **file, **res;
  char **path;
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
    if(r < 0 || !(S_ISREG(dat.st_mode) || S_ISDIR(dat.st_mode))) {
      if(r < 0)
        ui_msgf(UIMSG_MAIN, "Error stat'ing \"%s\": %s", cpath, g_strerror(errno));
      else
        ui_msgf(UIMSG_MAIN, "Not sharing \"%s\": Neither file nor directory.", cpath);
      g_free(confname);
      g_free(cpath);
      continue;
    }
    g_free(cpath);
    // and create the node
    struct fl_list *cur = g_slice_new0(struct fl_list);
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


// Must be called in a separate thread.
static void fl_scan_thread(gpointer data, gpointer udata) {
  struct fl_scan_args *args = data;

  int i, len = g_strv_length(args->path);
  for(i=0; i<len; i++) {
    struct fl_list *cur = g_slice_new0(struct fl_list);
    char *tmp = g_filename_from_utf8(args->path[i], -1, NULL, NULL, NULL);
    cur->sub = g_sequence_new(fl_list_free);
    fl_scan_dir(cur, tmp);
    g_free(tmp);
    args->res[i] = cur;
  }

  g_idle_add_full(G_PRIORITY_HIGH_IDLE, args->donefun, args, NULL);
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
  int resetnum;      // used by hash thread to validate that *file is still in the queue
};


// adding/removing items from the files-to-be-hashed queue
#define fl_hash_queue_append(fl) do {\
    gboolean start = !g_hash_table_size(fl_hash_queue);\
    g_hash_table_insert(fl_hash_queue, fl, (void *)1);\
    fl_hash_queue_size += fl->size;\
    if(start)\
      fl_hash_process();\
  } while(0)


#define fl_hash_queue_del(fl) do {\
    if((fl)->isfile && g_hash_table_lookup(fl_hash_queue, fl)) {\
      fl_hash_queue_size -= fl->size;\
      g_hash_table_remove(fl_hash_queue, fl);\
      if((fl) == fl_hash_cur)\
        g_atomic_int_inc(&fl_hash_reset);\
    }\
  } while(0)


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
    ratecalc_add(&fl_hash_rate, r);
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


static void fl_hash_process() {
  if(!g_hash_table_size(fl_hash_queue)) {
    ratecalc_unregister(&fl_hash_rate);
    ratecalc_reset(&fl_hash_rate);
    return;
  }
  ratecalc_register(&fl_hash_rate);

  // get one item from fl_hash_queue
  GHashTableIter iter;
  struct fl_list *file;
  g_hash_table_iter_init(&iter, fl_hash_queue);
  g_hash_table_iter_next(&iter, (gpointer *)&file, NULL);
  fl_hash_cur = file;

  // pass stuff to the hash thread
  struct fl_hash_args *args = g_new0(struct fl_hash_args, 1);
  args->file = file;
  char *tmp = fl_local_path(file);
  args->path = g_filename_from_utf8(tmp, -1, NULL, NULL, NULL);
  g_free(tmp);
  args->filesize = file->size;
  args->resetnum = g_atomic_int_get(&fl_hash_reset);
  g_thread_pool_push(fl_hash_pool, args, NULL);
}


static gboolean fl_hash_done(gpointer dat) {
  struct fl_hash_args *args = dat;
  struct fl_list *fl = args->file;

  // remove file from queue, ignore this hash if the file was already removed
  // by some other process.
  if(!g_hash_table_remove(fl_hash_queue, fl))
    goto fl_hash_done_f;

  fl_hash_queue_size -= fl->size;

  if(args->err) {
    ui_msgf(UIMSG_MAIN, "Error hashing \"%s\": %s", args->path, args->err->message);
    g_error_free(args->err);
    goto fl_hash_done_f;
  }

  // update file and hash info
  fl_hashindex_sethash(fl, args->root, args->lastmod, args->blocksize, args->blocks);

  ui_msgf(UIMSG_MAIN, "Finished hashing %s. [%.2f MiB/s]", fl->name, ((double)fl->size)/(1024.0*1024.0)/args->time);
  fl_needflush = TRUE;

fl_hash_done_f:
  if(args->blocks)
    g_free(args->blocks);
  g_free(args->path);
  g_free(args);
  // Hash next file in the queue
  fl_hash_process();
  return FALSE;
}





// Refresh filelist & (un)share directories


// get or create a root directory
static struct fl_list *fl_refresh_getroot(const char *name) {
  // no root? create!
  if(!fl_local_list) {
    fl_local_list = g_slice_new0(struct fl_list);
    fl_local_list->sub = g_sequence_new(fl_list_free);
  }
  struct fl_list *cur = fl_list_file(fl_local_list, name);
  // dir not present yet? create a stub
  if(!cur) {
    cur = g_slice_new0(struct fl_list);
    cur->name = g_strdup(name);
    cur->incomplete = TRUE;
    cur->sub = g_sequence_new(fl_list_free);
    fl_list_add(fl_local_list, cur);
  }
  return cur;
}


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
  fl_hash_queue_del(cur);
  if(cur->isfile && cur->hastth)
    fl_hashindex_del(cur);
  else if(!cur->isfile) {
    GSequenceIter *i;
    for(i=g_sequence_get_begin_iter(cur->sub); !g_sequence_iter_is_end(i); i=g_sequence_iter_next(i))
      fl_refresh_delhash(g_sequence_get(i));
  }
}


static void fl_refresh_compare(struct fl_list *old, struct fl_list *new) {
  GSequenceIter *oldi = g_sequence_get_begin_iter(old->sub);
  GSequenceIter *newi = g_sequence_get_begin_iter(new->sub);
  while(!g_sequence_iter_is_end(oldi) || !g_sequence_iter_is_end(newi)) {
    struct fl_list *oldl = g_sequence_iter_is_end(oldi) ? NULL : g_sequence_get(oldi);
    struct fl_list *newl = g_sequence_iter_is_end(newi) ? NULL : g_sequence_get(newi);
    int cmp = !oldl ? 1 : !newl ? -1 : fl_list_cmp(oldl, newl, NULL);

    // old == new: same
    if(cmp == 0) {
      g_assert(oldl->isfile == newl->isfile); // TODO: handle this case
      if(oldl->isfile && (!oldl->hastth || newl->lastmod > oldl->lastmod || newl->size != oldl->size))
        fl_hash_queue_append(oldl);
      if(!oldl->isfile)
        fl_refresh_compare(oldl, newl);
      oldi = g_sequence_iter_next(oldi);
      newi = g_sequence_iter_next(newi);

    // old > new: insert new
    } else if(cmp > 0) {
      struct fl_list *tmp = fl_list_copy(newl);
      fl_list_add(old, tmp);
      if(tmp->isfile)
        fl_hash_queue_append(tmp);
      else
        fl_refresh_addhash(tmp);
      newi = g_sequence_iter_next(newi);

    // new > old: delete old
    } else {
      oldi = g_sequence_iter_next(oldi);
      fl_refresh_delhash(oldl);
      fl_list_remove(oldl);
    }
  }
  old->incomplete = FALSE;
}

/* A visual explanation of the above algorithm:
 * old new
 *  a  a  same (new == old; new++, old++)
 *  b  b  same
 *  d  c  insert c (!old || new < old; new++, old stays)
 *  d  d  same
 *  e  f  delete e (!new || new > old; new stays, old++)
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

static gboolean fl_refresh_scanned(gpointer dat);

static void fl_refresh_process() {
  if(!fl_refresh_queue->head)
    return;

  // construct the list of to-be-scanned directories
  struct fl_list *dir = fl_refresh_queue->head->data;
  struct fl_scan_args *args = g_new0(struct fl_scan_args, 1);
  args->donefun = fl_refresh_scanned;

  // one dir, the simple case
  if(dir != fl_local_list) {
    args->file = g_new0(struct fl_list *, 2);
    args->res = g_new0(struct fl_list *, 2);
    args->path = g_new0(char *, 2);
    args->file[0] = dir;
    args->path[0] = fl_local_path(dir);

  // refresh the entire share, which consists of multiple dirs.
  } else {
    time(&fl_refresh_last);
    char **names = g_key_file_get_keys(conf_file, "share", NULL, NULL);
    int i, len = g_strv_length(names);
    args->file = g_new0(struct fl_list *, len+1);
    args->res  = g_new0(struct fl_list *, len+1);
    args->path = g_new0(char *, len+1);
    for(i=0; i<len; i++) {
      args->file[i] = fl_refresh_getroot(names[i]);
      args->path[i] = g_key_file_get_string(conf_file, "share", names[i], NULL);
    }
    g_strfreev(names);
  }

  // scan the requested directories in the background
  g_thread_pool_push(fl_scan_pool, args, NULL);
}


static gboolean fl_refresh_scanned(gpointer dat) {
  struct fl_scan_args *args = dat;

  // TODO: I'm assuming here that the scanned directory has not been /unshare'd
  // in the meanwhile. In the case that did happen, we're going to crash. :-(
  // (Note that besides /unshare it is not possible for a dir to get removed
  // from the share while refreshing. Only one refresh is allowed to be
  // processed at a time, so no directories could have been removed in some
  // concurrent refresh.)

  int i, len = g_strv_length(args->path);
  for(i=0; i<len; i++) {
    fl_refresh_compare(args->file[i], args->res[i]);
    fl_list_free(args->res[i]);
  }

  fl_needflush = TRUE;
  g_strfreev(args->path);
  g_free(args->file);
  g_free(args->res);
  g_free(args);

  g_queue_pop_head(fl_refresh_queue);
  if(fl_refresh_queue->head)
    fl_refresh_process();
  else {
    // force a flush when all queued refreshes have been processed
    fl_flush(NULL);
    ui_msgf(UIMSG_NOTIFY, "File list refresh finished.");
  }
  return FALSE;
}


void fl_refresh(struct fl_list *dir) {
  if(!dir)
    dir = fl_local_list;
  GList *n;
  for(n=fl_refresh_queue->head; n; n=n->next) {
    struct fl_list *c = n->data;
    // if current dir is part of listed dir then it's already queued
    if(dir == c || fl_list_is_child(c, dir))
      return;
    // if listed dir is part of current dir, then we can remove that item (provided it's not being refreshed currently)
    if(n->prev && (dir == fl_local_list || fl_list_is_child(c, dir))) {
      n = n->prev;
      g_queue_delete_link(fl_refresh_queue, n->next);
    }
  }

  // add to queue and process
  g_queue_push_tail(fl_refresh_queue, dir);
  if(fl_refresh_queue->head == fl_refresh_queue->tail)
    fl_refresh_process();
}


// Adds a directory to the file list and initiates a refresh on it (Assumes the
// directory has already been added to the config file).
void fl_share(const char *dir) {
  fl_refresh(fl_refresh_getroot(dir));
}


// Only affects the filelist and hash data, does not modify the config file.
// This function is far more efficient than removing the dir from the config
// and doing a /refresh. (Which may also result in some errors being displayed
// when a currently-being-hashed file is removed due to the directory not being
// present in the config file anymore).
void fl_unshare(const char *dir) {
  if(dir) {
    struct fl_list *fl = fl_list_file(fl_local_list, dir);
    g_return_if_fail(fl);
    fl_refresh_delhash(fl);
    fl_list_remove(fl);
  } else if(fl_local_list) {
    fl_refresh_delhash(fl_local_list);
    fl_list_free(fl_local_list);
    fl_local_list = NULL;
  }
  // force a refresh, people may be in a hurry with removing stuff
  fl_needflush = TRUE;
  fl_flush(NULL);
}






// Initialize local filelist


// fetches the last modification times from the hashdata file and checks
// whether we have any incomplete directories.
static gboolean fl_init_list(struct fl_list *fl) {
  GSequenceIter *iter;
  gboolean incomplete = FALSE;
  for(iter=g_sequence_get_begin_iter(fl->sub); !g_sequence_iter_is_end(iter); iter=g_sequence_iter_next(iter)) {
    struct fl_list *c = g_sequence_get(iter);
    if(c->isfile && fl_hashindex_load(c))
      incomplete = TRUE;
    if(!c->isfile && (fl_init_list(c) || c->incomplete))
      incomplete = TRUE;
  }
  return incomplete;
}


static gboolean fl_init_hash_equal(gconstpointer a, gconstpointer b) {
  return memcmp(a, b, 24) == 0;
}


static gboolean fl_init_autorefresh(gpointer dat) {
  int r = conf_autorefresh();
  time_t t = time(NULL);
  if(r && fl_refresh_last+(r*60) < t)
    fl_refresh(NULL);
  return TRUE;
}


void fl_init() {
  GError *err = NULL;
  gboolean dorefresh = FALSE;

  // init stuff
  fl_local_list = NULL;
  fl_local_list_file = g_build_filename(conf_dir, "files.xml.bz2", NULL);
  fl_refresh_queue = g_queue_new();
  fl_hashdat_file = g_build_filename(conf_dir, "hashdata.dat", NULL);
  fl_scan_pool = g_thread_pool_new(fl_scan_thread, NULL, 1, FALSE, NULL);
  fl_hash_pool = g_thread_pool_new(fl_hash_thread, NULL, 1, FALSE, NULL);
  fl_hash_queue = g_hash_table_new(g_direct_hash, g_direct_equal);
  // Even though the keys are the tth roots, we can just use g_int_hash. The
  // first four bytes provide enough unique data anyway.
  fl_hash_index = g_hash_table_new(g_int_hash, fl_init_hash_equal);
  ratecalc_init(&fl_hash_rate, 10);

  // flush unsaved data to disk every 60 seconds
  g_timeout_add_seconds_full(G_PRIORITY_LOW, 60, fl_flush, NULL, NULL);
  // Check every 60 seconds whether we need to refresh. This automatically
  // adapts itself to changes to the autorefresh config variable. Unlike using
  // the configured interval as a timeout, in which case we need to manually
  // adjust the timer on every change.
  g_timeout_add_seconds_full(G_PRIORITY_LOW, 60, fl_init_autorefresh, NULL, NULL);

  // read config
  char **shares = g_key_file_get_keys(conf_file, "share", NULL, NULL);
  if(!shares || !g_strv_length(shares)) {
    fl_hashdat_open(TRUE);
    g_strfreev(shares);
    return;
  }
  g_strfreev(shares);

  // load our files.xml.bz
  fl_local_list = fl_load(fl_local_list_file, &err);
  if(!fl_local_list) {
    g_assert(err);
    ui_msgf(UIMSG_MAIN, "Error loading local filelist: %s. Re-building list.", err->message);
    g_error_free(err);
    dorefresh = TRUE;
    fl_hashdat_open(TRUE);
  } else
    fl_hashdat_open(FALSE);

  // Get last modification times, check for any incomplete directories and
  // initiate a refresh if there is one.  (If there is an incomplete directory,
  // it means that ncdc was closed while it was hashing files, a refresh will
  // continue where it left off)
  if(fl_local_list) {
    dorefresh = fl_init_list(fl_local_list);
    if(dorefresh)
      ui_msg(UIMSG_NOTIFY, "File list incomplete, refreshing...");
  }

  if(dorefresh || conf_autorefresh())
    fl_refresh(NULL);
}



// flush and close
void fl_close() {
  // tell the hasher to stop
  g_atomic_int_inc(&fl_hash_reset);
  fl_flush(NULL);
  gdbm_close(fl_hashdat);
}
