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
#include <stdlib.h>
#include <fcntl.h>
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
    GError *err = NULL;
    if(!fl_save(fl_local_list, fl_local_list_file, NULL, 9999, &err)) {
      // this is a pretty fatal error... oh well, better luck next time
      ui_mf(ui_main, UIP_MED, "Error saving file list: %s", err->message);
      g_error_free(err);
    }
  }
  fl_needflush = FALSE;
  return TRUE;
}





// Hash index interface. These operate on fl_hash_index and make sure
// fl_local_list_size stays correct.

// Add to the hash index
static void fl_hashindex_insert(struct fl_list *fl) {
  GSList *cur = g_hash_table_lookup(fl_hash_index, fl->tth);
  if(cur) {
    g_return_if_fail(cur == g_slist_insert(cur, fl, 1)); // insert item without modifying the pointer
  } else {
    cur = g_slist_prepend(cur, fl);
    g_hash_table_insert(fl_hash_index, fl->tth, cur);
    fl_local_list_size += fl->size;
  }
}


// ...and remove a file. This is done when a file is actually removed from the
// share, or when its TTH information has been invalidated.
static void fl_hashindex_del(struct fl_list *fl) {
  if(!fl->hastth)
    return;
  GSList *cur = g_hash_table_lookup(fl_hash_index, fl->tth);
  fl->hastth = FALSE;

  cur = g_slist_remove(cur, fl);
  if(!cur) {
    g_hash_table_remove(fl_hash_index, fl->tth);
    fl_local_list_size -= fl->size;
  // there's another file with the same TTH.
  } else
    g_hash_table_replace(fl_hash_index, ((struct fl_list *)cur->data)->tth, cur);
}





// Scanning directories
// TODO: rewrite this with the new SQLite backend idea

// Note: The `file' structure points to a (sub-)item in fl_local_list, and will
// be accessed from both the scan thread and the main thread. It is therefore
// important that no changes are made to the local file list while the scan
// thread is active.
struct fl_scan_args {
  struct fl_list **file, **res;
  char **path;
  GRegex *excl_regex;
  gboolean inc_hidden;
  gboolean (*donefun)(gpointer);
};


static void fl_scan_rmdupes(struct fl_list *fl, const char *vpath) {
  int i = 1;
  while(i<fl->sub->len) {
    struct fl_list *a = g_ptr_array_index(fl->sub, i-1);
    struct fl_list *b = g_ptr_array_index(fl->sub, i);
    if(fl_list_cmp_strict(a, b) == 0) {
      char *tmp = g_build_filename(vpath, b->name, NULL);
      ui_mf(ui_main, UIP_MED, "Not sharing \"%s\": Other file with same name (but different case) already shared.", tmp);
      fl_list_remove(b);
      g_free(tmp);
    } else
      i++;
  }
}


// Fetches TTH information either from the database or from *oldpar, and
// invalidates this data if the file has changed.
static void fl_scan_check(struct fl_list *oldpar, struct fl_list *new, const char *real) {
  time_t oldlastmod;
  guint64 oldsize;
  char oldhash[24];
  gint64 oldid = 0;

  struct fl_list *old = oldpar && oldpar->sub ? fl_list_file_strict(oldpar, new) : NULL;
  // Get from the previous in-memory structure
  if(old && fl_list_getlocal(old).id) {
    oldid = fl_list_getlocal(old).id;
    oldlastmod = fl_list_getlocal(old).lastmod;
    oldsize = old->size;
    memcpy(oldhash, old->tth, 24);
  // Otherwise, do a database lookup on the file path
  } else
    oldid = db_fl_getfile(real, &oldlastmod, &oldsize, oldhash);

  if(oldid && (oldlastmod < fl_list_getlocal(new).lastmod || oldsize != new->size)) {
    // TODO: file has changed, invalidate hash associated with oldid
  } else if(oldid) {
    new->hastth = TRUE;
    memcpy(new->tth, oldhash, 24);
    fl_list_getlocal(new).lastmod = oldlastmod;
    fl_list_getlocal(new).id = oldid;
  }
}


// *name is in filesystem encoding. For *path and *vpath see fl_scan_dir().
static struct fl_list *fl_scan_item(struct fl_list *old, const char *path, const char *vpath, const char *name, GRegex *excl) {
  char *uname = NULL;  // name-to-UTF8
  char *vcpath = NULL; // vpath + uname
  char *ename = NULL;  // uname-to-filesystem
  char *cpath = NULL;  // path + ename
  char *real = NULL;   // realpath(cpath)-to-UTF8
  struct fl_list *node = NULL;

  // Try to get a UTF-8 filename
  uname = g_filename_to_utf8(name, -1, NULL, NULL, NULL);
  if(!uname)
    uname = g_filename_display_name(name);

  // Check for share_exclude as soon as we have the confname
  if(excl && g_regex_match(excl, uname, 0, NULL))
    goto done;

  // Get the virtual path (for reporting purposes)
  vcpath = g_build_filename(vpath, uname, NULL);

  // Check that the UTF-8 filename can be converted back to something we can
  // access on the filesystem. If it can't be converted back, we won't share
  // the file at all. Keeping track of a raw-to-UTF-8 filename lookup table
  // isn't worth the effort.
  ename = g_filename_from_utf8(uname, -1, NULL, NULL, NULL);
  if(!ename) {
    ui_mf(ui_main, UIP_MED, "Error reading directory entry in \"%s\": Invalid encoding.", vcpath);
    goto done;
  }

  // Get cpath and try to stat() the file
  // we're currently following symlinks, but I'm not sure whether that's a good idea yet
  cpath = g_build_filename(path, ename, NULL);
  struct stat dat;
  int r = stat(cpath, &dat);
  if(r < 0 || !(S_ISREG(dat.st_mode) || S_ISDIR(dat.st_mode))) {
    if(r < 0)
      ui_mf(ui_main, UIP_MED, "Error stat'ing \"%s\": %s", vcpath, g_strerror(errno));
    else
      ui_mf(ui_main, UIP_MED, "Not sharing \"%s\": Neither file nor directory.", vcpath);
    goto done;
  }

  // Get the realpath() (for lookup in the database)
  // TODO: this path is only used if fl_scan_check() can't find the item in the
  // old fl_list structure. It may be more efficient to only try to execute
  // this code when this is the case.
  char *tmp = realpath(cpath, NULL);
  if(!tmp) {
    ui_mf(ui_main, UIP_MED, "Error getting file path for \"%s\": %s", vcpath, g_strerror(errno));
    goto done;
  }
  real = g_filename_to_utf8(tmp, -1, NULL, NULL, NULL);
  free(tmp);
  if(!real) {
    ui_mf(ui_main, UIP_MED, "Error getting file path for \"%s\": %s", vcpath, "Encoding error.");
    goto done;
  }

  // create the node
  node = fl_list_create(uname, S_ISREG(dat.st_mode) ? TRUE : FALSE);
  if(S_ISREG(dat.st_mode)) {
    node->isfile = TRUE;
    node->size = dat.st_size;
    fl_list_getlocal(node).lastmod = dat.st_mtime;
  }

  // Fetch id, tth, and hashtth fields.
  if(node->isfile)
    fl_scan_check(old, node, real);

done:
  g_free(uname);
  g_free(vcpath);
  g_free(cpath);
  g_free(ename);
  g_free(real);
  return node;
}


// recursive
// Doesn't handle paths longer than PATH_MAX, but I don't think it matters all that much.
// *path is the filesystem path in filename encoding, vpath is the virtual path in UTF-8.
static void fl_scan_dir(struct fl_list *parent, struct fl_list *old, const char *path, const char *vpath, gboolean inc_hidden, GRegex *excl) {
  GError *err = NULL;
  GDir *dir = g_dir_open(path, 0, &err);
  if(!dir) {
    ui_mf(ui_main, UIP_MED, "Error reading directory \"%s\": %s", vpath, g_strerror(errno));
    g_error_free(err);
    return;
  }
  const char *name;
  while((name = g_dir_read_name(dir))) {
    if(strcmp(name, ".") == 0 || strcmp(name, "..") == 0)
      continue;
    if(!inc_hidden && name[0] == '.')
      continue;
    // check with *excl, stat and create
    struct fl_list *item = fl_scan_item(old, path, vpath, name, excl);
    // and add it
    if(item)
      fl_list_add(parent, item, -1);
  }
  g_dir_close(dir);

  // Sort
  fl_list_sort(parent);
  fl_scan_rmdupes(parent, vpath);

  // check for directories (outside of the above loop, to avoid having too many
  // directories opened at the same time. Costs some extra CPU cycles, though...)
  int i;
  for(i=0; i<parent->sub->len; i++) {
    struct fl_list *cur = g_ptr_array_index(parent->sub, i);
    if(!cur->isfile) {
      char *enc = g_filename_from_utf8(cur->name, -1, NULL, NULL, NULL);
      char *cpath = g_build_filename(path, enc, NULL);
      char *virtpath = g_build_filename(vpath, cur->name, NULL);
      cur->sub = g_ptr_array_new_with_free_func(fl_list_free);
      fl_scan_dir(cur, old && old->sub ? fl_list_file_strict(old, cur) : NULL, cpath, virtpath, inc_hidden, excl);
      g_free(virtpath);
      g_free(cpath);
      g_free(enc);
    }
  }
}


// Must be called in a separate thread.
static void fl_scan_thread(gpointer data, gpointer udata) {
  struct fl_scan_args *args = data;

  int i, len = g_strv_length(args->path);
  for(i=0; i<len; i++) {
    struct fl_list *cur = fl_list_create("", FALSE);
    char *tmp = g_filename_from_utf8(args->path[i], -1, NULL, NULL, NULL);
    cur->sub = g_ptr_array_new_with_free_func(fl_list_free);
    fl_scan_dir(cur, args->file[i], tmp, args->path[i], args->inc_hidden, args->excl_regex);
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
  char root[24];     // set by hash thread
  GError *err;       // set by hash thread
  time_t lastmod;    // set by hash thread
  gint64 id;         // set by hash thread
  gdouble time;      // set by hash thread
  int resetnum;      // used by hash thread to validate that *file is still in the queue
};

// Maximum number of levels, including root (level 0).  The ADC docs specify
// that this should be at least 7, and everyone else uses 10. Since keeping 10
// levels of hash data does eat up more space than I'd be willing to spend on
// it, let's use 8.
#if INTERFACE
#define fl_hash_keep_level 8
#endif
/* Space requirements per file for different levels (in bytes, min - max) and
 * the block size of a 1GiB file.
 *
 * 10  6144 - 12288    2 MiB
 *  9  3072 -  6144    4 MiB
 *  8  1536 -  3072    8 MiB
 *  7   768 -  1536   16 MiB
 *  6   384 -   768   32 MiB
 */

// there's no need for better granularity than this
#define fl_hash_max_granularity G_GUINT64_CONSTANT(64 * 1024)



// adding/removing items from the files-to-be-hashed queue
// _append() assumes that fl->hastth is false.
#define fl_hash_queue_append(fl) do {\
    g_warn_if_fail(!fl->hastth);\
    gboolean start = !g_hash_table_size(fl_hash_queue);\
    if(start || !g_hash_table_lookup(fl_hash_queue, fl))\
      fl_hash_queue_size += fl->size;\
    g_hash_table_insert(fl_hash_queue, fl, (void *)1);\
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



// Recursively deletes a fl_list structure from the hash queue
static void fl_hash_queue_delrec(struct fl_list *f) {
  if(f->isfile)
    fl_hash_queue_del(f);
  else {
    int i;
    for(i=0; i<f->sub->len; i++)
      fl_hash_queue_delrec(g_ptr_array_index(f->sub, i));
  }
}


static gboolean fl_hash_done(gpointer dat);

static void fl_hash_thread(gpointer data, gpointer udata) {
  struct fl_hash_args *args = data;
  // static, since only one hash thread is allowed and this saves stack space
  static struct tth_ctx tth;
  static char buf[10240];
  char *blocks = NULL;
  int f = -1;
  char *real = NULL;

  time(&args->lastmod);
  GTimer *tm = g_timer_new();

  char *tmp = realpath(args->path, NULL);
  if(!tmp) {
    g_set_error(&args->err, 1, 0, "Error getting file path: %s", g_strerror(errno));
    goto finish;
  }
  real = g_filename_to_utf8(tmp, -1, NULL, NULL, NULL);
  free(tmp);
  g_return_if_fail(real); // really shouldn't happen, we fetched this from a UTF-8 string after all.

  f = open(args->path, O_RDONLY);
  if(f < 0) {
    g_set_error(&args->err, 1, 0, "Error reading file: %s", g_strerror(errno));
    goto finish;
  }

  // Initialize some stuff
  guint64 blocksize = tth_blocksize(args->filesize, 1<<(fl_hash_keep_level-1));
  blocksize = MAX(blocksize, fl_hash_max_granularity);
  int blocks_num = tth_num_blocks(args->filesize, blocksize);
  blocks = g_malloc(24*blocks_num);
  tth_init(&tth);

  struct fadv adv;
  fadv_init(&adv, f, 0);

  int r;
  guint64 rd = 0;
  int block_cur = 0;
  guint64 block_len = 0;

  while((r = read(f, buf, 10240)) > 0) {
    rd += r;
    fadv_purge(&adv, r);
    // no need to hash any further? quit!
    if(g_atomic_int_get(&fl_hash_reset) != args->resetnum)
      goto finish;
    // file has been modified. time to back out
    if(rd > args->filesize) {
      g_set_error_literal(&args->err, 1, 0, "File has been modified.");
      goto finish;
    }
    ratecalc_add(&fl_hash_rate, r);
    // and hash
    char *b = buf;
    while(r > 0) {
      int w = MIN(r, blocksize-block_len);
      tth_update(&tth, b, w);
      block_len += w;
      b += w;
      r -= w;
      if(block_len >= blocksize) {
        tth_final(&tth, blocks+(block_cur*24));
        tth_init(&tth);
        block_cur++;
        block_len = 0;
      }
    }
  }
  if(r < 0) {
    g_set_error(&args->err, 1, 0, "Error reading file: %s", g_strerror(errno));
    goto finish;
  }
  if(rd != args->filesize) {
    g_set_error_literal(&args->err, 1, 0, "File has been modified.");
    goto finish;
  }
  // Calculate last block
  if(!args->filesize || block_len) {
    tth_final(&tth, blocks+(block_cur*24));
    block_cur++;
  }
  g_return_if_fail(block_cur == blocks_num);
  // Calculate root hash
  tth_root(blocks, blocks_num, args->root);

  // Add to database
  args->id = db_fl_addhash(real, args->filesize, args->lastmod, args->root, blocks, 24*blocks_num);
  if(!args->id)
    g_set_error_literal(&args->err, 1, 0, "Error saving hash data to the database.");

finish:
  if(f > 0) {
    fadv_close(&adv);
    close(f);
  }
  g_free(real);
  g_free(blocks);
  args->time = g_timer_elapsed(tm, NULL);
  g_timer_destroy(tm);
  g_idle_add_full(G_PRIORITY_HIGH_IDLE, fl_hash_done, args, NULL);
}


static void fl_hash_process() {
  if(!g_hash_table_size(fl_hash_queue)) {
    ratecalc_unregister(&fl_hash_rate);
    ratecalc_reset(&fl_hash_rate);
    db_fl_setdone(TRUE);
    return;
  }
  db_fl_setdone(FALSE);
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
    ui_mf(ui_main, UIP_MED, "Error hashing \"%s\": %s", args->path, args->err->message);
    g_error_free(args->err);
    goto fl_hash_done_f;
  }

  // update file and hash info
  memcpy(fl->tth, args->root, 24);
  fl->hastth = 1;
  fl_list_getlocal(fl).lastmod = args->lastmod;
  fl_list_getlocal(fl).id = args->id;
  fl_hashindex_insert(fl);
  fl_needflush = TRUE;

fl_hash_done_f:
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
    fl_local_list = fl_list_create("", FALSE);
    fl_local_list->sub = g_ptr_array_new_with_free_func(fl_list_free);
  }
  struct fl_list *cur = fl_list_file(fl_local_list, name);
  // dir not present yet? create a stub
  if(!cur) {
    cur = fl_list_create(name, FALSE);
    cur->sub = g_ptr_array_new_with_free_func(fl_list_free);
    fl_list_add(fl_local_list, cur, -1);
    fl_list_sort(fl_local_list);
  }
  return cur;
}


static void fl_refresh_addhash(struct fl_list *cur) {
  int i;
  for(i=0; i<cur->sub->len; i++) {
    struct fl_list *l = g_ptr_array_index(cur->sub, i);
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
    int i;
    for(i=0; i<cur->sub->len; i++)
      fl_refresh_delhash(g_ptr_array_index(cur->sub, i));
  }
}


static void fl_refresh_compare(struct fl_list *old, struct fl_list *new) {
  int oldi = 0;
  int newi = 0;
  while(oldi < old->sub->len || newi < new->sub->len) {
    struct fl_list *oldl = oldi >= old->sub->len ? NULL : g_ptr_array_index(old->sub, oldi);
    struct fl_list *newl = newi >= new->sub->len ? NULL : g_ptr_array_index(new->sub, newi);
    // Don't use fl_list_cmp() here, since that one doesn't return 0
    int cmp = !oldl ? 1 : !newl ? -1 : fl_list_cmp_strict(oldl, newl);
    gboolean check = FALSE, remove = FALSE, insert = FALSE;

    // special case #1: old == new, but one is a directory and the other is a file
    // special case #2: old == new, but the file names have different case
    // In both situations we just delete our information and overwrite/rehash it with new.
    if(cmp == 0 && (!!oldl->isfile != !!newl->isfile || strcmp(oldl->name, newl->name) != 0))
      remove = insert = TRUE;
    else if(cmp == 0) // old == new, just check
      check = TRUE;
    else if(cmp > 0)  // old > new: insert new
      insert = TRUE;
    else              // old < new: remove
      remove = TRUE;

    // check
    if(check) {
      // If it's a file, it may have been modified or we're missing the TTH
      if(oldl->isfile && (!oldl->hastth
            || fl_list_getlocal(newl).lastmod > fl_list_getlocal(oldl).lastmod
            || newl->size != oldl->size)) {
        fl_hashindex_del(oldl);
        oldl->size = newl->size;
        fl_hash_queue_append(oldl);
      }
      // If it's a dir, recurse into it
      if(!oldl->isfile)
        fl_refresh_compare(oldl, newl);
      oldi++;
      newi++;
    }

    // remove
    if(remove) {
      fl_refresh_delhash(oldl);
      fl_list_remove(oldl);
      // don't modify oldi, after deletion it will automatically point to the next item in the list
    }

    // insert
    if(insert) {
      struct fl_list *tmp = fl_list_copy(newl);
      fl_list_add(old, tmp, oldi);
      if(tmp->isfile)
        fl_hash_queue_append(tmp);
      else
        fl_refresh_addhash(tmp);
      oldi++; // after fl_list_add(), oldi points to the new item. But we don't have to check that one again, so increase.
      newi++;
    }
  }
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
  struct fl_scan_args *args = g_slice_new0(struct fl_scan_args);
  args->donefun = fl_refresh_scanned;
  args->inc_hidden = g_key_file_get_boolean(conf_file, "global", "share_hidden", NULL);

  char *excl = g_key_file_get_string(conf_file, "global", "share_exclude", NULL);
  if(excl)
    args->excl_regex = g_regex_new(excl, G_REGEX_OPTIMIZE, 0, NULL);
  g_free(excl);

  // Don't allow files in the scanned directory to be hashed while refreshing.
  // Since the refresh thread will create a completely new fl_list structure,
  // any changes to the old one will be lost.
  fl_hash_queue_delrec(dir);

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
    int i, len = names ? g_strv_length(names) : 0;
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

  // If the hash queue is empty after calling fl_refresh_compare() then it
  // means the file list is completely hashed.
  if(!g_hash_table_size(fl_hash_queue))
    db_fl_setdone(TRUE);

  fl_needflush = TRUE;
  g_strfreev(args->path);
  if(args->excl_regex)
    g_regex_unref(args->excl_regex);
  g_free(args->file);
  g_free(args->res);
  g_slice_free(struct fl_scan_args, args);

  g_queue_pop_head(fl_refresh_queue);
  if(fl_refresh_queue->head)
    fl_refresh_process();
  else {
    // force a flush when all queued refreshes have been processed
    fl_flush(NULL);
    if(fl_local_list && fl_local_list->sub && fl_local_list->sub->len)
      ui_mf(ui_main, UIM_NOTIFY, "File list refresh finished.");
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


// Walks through the file list and inserts everything into the fl_hashindex.
static void fl_init_list(struct fl_list *fl) {
  int i;
  for(i=0; i<fl->sub->len; i++) {
    struct fl_list *c = g_ptr_array_index(fl->sub, i);
    if(c->isfile && c->hastth)
      fl_hashindex_insert(c);
    else if(!c->isfile)
      fl_init_list(c);
  }
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
  fl_scan_pool = g_thread_pool_new(fl_scan_thread, NULL, 1, FALSE, NULL);
  fl_hash_pool = g_thread_pool_new(fl_hash_thread, NULL, 1, FALSE, NULL);
  fl_hash_queue = g_hash_table_new(g_direct_hash, g_direct_equal);
  // Even though the keys are the tth roots, we can just use g_int_hash. The
  // first four bytes provide enough unique data anyway.
  fl_hash_index = g_hash_table_new(g_int_hash, tiger_hash_equal);
  ratecalc_init(&fl_hash_rate);

  // flush unsaved data to disk every 60 seconds
  g_timeout_add_seconds_full(G_PRIORITY_LOW, 60, fl_flush, NULL, NULL);
  // Check every 60 seconds whether we need to refresh. This automatically
  // adapts itself to changes to the autorefresh config variable. Unlike using
  // the configured interval as a timeout, in which case we need to manually
  // adjust the timer on every change.
  g_timeout_add_seconds_full(G_PRIORITY_LOW, 60, fl_init_autorefresh, NULL, NULL);

  // The following code may take a while on large shares, so indicate to the UI
  // that we're busy.
  ui_m(NULL, UIM_NOLOG|UIM_DIRECT, "Loading file list...");
  ui_draw();

  // read config
  gboolean sharing = TRUE;
  char **shares = g_key_file_get_keys(conf_file, "share", NULL, NULL);
  if(!shares || !g_strv_length(shares))
    sharing = FALSE;
  g_strfreev(shares);

  // load our files.xml.bz2
  fl_local_list = sharing ? fl_load(fl_local_list_file, &err, TRUE) : NULL;
  if(sharing && !fl_local_list) {
    ui_mf(ui_main, UIP_MED, "Error loading local filelist: %s. Re-building list.", err->message);
    g_error_free(err);
    dorefresh = TRUE;
  } else if(!sharing)
    // Force a refresh when we're not sharing anything. This makes sure that we
    // at least have a files.xml.bz2
    dorefresh = TRUE;

  // If ncdc was previously closed while hashing, make sure to force a refresh
  // this time to continue the hash progress.
  if(sharing && !db_fl_getdone()) {
    dorefresh = TRUE;
    ui_m(ui_main, UIM_NOTIFY, "File list incomplete, refreshing...");
  }

  // Initialize the fl_hash_index
  if(fl_local_list)
    fl_init_list(fl_local_list);

  // reset loading indicator
  if(!fl_local_list || !dorefresh)
    ui_m(NULL, UIM_NOLOG|UIM_DIRECT, NULL);

  if(dorefresh || conf_autorefresh())
    fl_refresh(NULL);
}
