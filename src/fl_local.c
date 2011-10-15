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
    GError *err = NULL;
    if(!fl_save(fl_local_list, fl_local_list_file, NULL, 9999, &err)) {
      // this is a pretty fatal error... oh well, better luck next time
      ui_mf(ui_main, UIP_MED, "Error saving file list: %s", err->message);
      g_error_free(err);
    }
    // sync the hash data
    gdbm_sync(fl_hashdat);
  }
  fl_needflush = FALSE;
  return TRUE;
}





// Hash data file interface (these operate on hashdata.dat)

#define HASHDAT_INFO 0
#define HASHDAT_TTHL 1
#define HASHDAT_DONE 2

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


static gboolean fl_hashdat_getdone() {
  char dat[] = { HASHDAT_DONE };
  datum key = { dat, 1 };
  return gdbm_exists(fl_hashdat, key) ? TRUE : FALSE;
}


// Also performs a flush to make sure the value is stored.
static void fl_hashdat_setdone(gboolean val) {
  if(!!fl_hashdat_getdone() == !!val)
    return;
  char dat[] = { HASHDAT_DONE };
  datum key = { dat, 1 }; // also used as value
  if(val)
    gdbm_store(fl_hashdat, key, key, GDBM_REPLACE);
  else
    gdbm_delete(fl_hashdat, key);
  gdbm_sync(fl_hashdat);
}


static gboolean fl_hashdat_getinfo(const char *tth, struct fl_hashdat_info *nfo) {
  char key[25];
  datum keydat = { key, 25 };
  key[0] = HASHDAT_INFO;
  memcpy(key+1, tth, 24);
  datum res = gdbm_fetch(fl_hashdat, keydat);
  if(res.dsize <= 0)
    return FALSE;
  g_return_val_if_fail(res.dsize >= 3*8, FALSE);
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


// "garbage collect" - removes unused items from hashdata.dat and performs a
// gdbm_reorganize() (the only fl_hashdat_ function that uses, but does not
// modify, fl_hash_index).
void fl_hashdat_gc() {
  fl_flush(NULL);
  GSList *rm = NULL;
  char tth[40];
  tth[39] = 0;

  // check for unused keys
  datum key = gdbm_firstkey(fl_hashdat);
  char *freethis = NULL;
  for(; key.dptr; key=gdbm_nextkey(fl_hashdat, key)) {
    char *str = key.dptr;
    if(freethis)
      free(freethis);
    // We only touch keys that this or earlier versions of ncdc could have
    // created. Unknown keys are left untouched as a later version could have
    // made these and there is no way to tell whether these need to be cleaned
    // up or not.
    if(key.dsize == 25 && (str[0] == HASHDAT_INFO || str[0] == HASHDAT_TTHL)
        && !g_hash_table_lookup(fl_hash_index, str+1)) {
      base32_encode(str+1, tth);
      g_message("Removing unused key in hashdata.dat: type = %d, hash = %s", str[0], tth);
      rm = g_slist_prepend(rm, str);
      freethis = NULL;
    } else
      freethis = str;
  }
  if(freethis)
    free(freethis);

  // delete the unused keys
  GSList *n = rm;
  key.dsize = 25; // all keys in the list are 25 bytes
  while(n) {
    rm = n->next;
    key.dptr = n->data;
    gdbm_delete(fl_hashdat, key);
    free(n->data);
    g_slist_free_1(n);
    n = rm;
  }

  // perform the reorganize
  gdbm_reorganize(fl_hashdat);
}





// Hash index interface (these operate on both fl_hash_index and the above hashdata.dat)
// These functions are also responsible for updating fl_local_list_size.

// low-level insert-into-index function
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
  fl->hastth = FALSE;

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


// recursive
// Doesn't handle paths longer than PATH_MAX, but I don't think it matters all that much.
static void fl_scan_dir(struct fl_list *parent, const char *path, const char *vpath, gboolean inc_hidden, GRegex *excl) {
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
    // Try to get a UTF-8 filename
    char *confname = g_filename_to_utf8(name, -1, NULL, NULL, NULL);
    if(!confname)
      confname = g_filename_display_name(name);
    // Check for share_exclude as soon as we have the confname
    if(excl && g_regex_match(excl, confname, 0, NULL)) {
      g_free(confname);
      continue;
    }
    // Check that the UTF-8 filename can be converted back to something we can
    // access on the filesystem. If it can't be converted back, we won't share
    // the file at all. Keeping track of a raw-to-UTF-8 filename lookup table
    // isn't worth the effort.
    char *vcpath = g_build_filename(vpath, confname, NULL);
    char *encname = g_filename_from_utf8(confname, -1, NULL, NULL, NULL);
    if(!encname) {
      ui_mf(ui_main, UIP_MED, "Error reading directory entry in \"%s\": Invalid encoding.", vcpath);
      g_free(vcpath);
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
        ui_mf(ui_main, UIP_MED, "Error stat'ing \"%s\": %s", vcpath, g_strerror(errno));
      else
        ui_mf(ui_main, UIP_MED, "Not sharing \"%s\": Neither file nor directory.", vcpath);
      g_free(confname);
      g_free(cpath);
      g_free(vcpath);
      continue;
    }
    g_free(cpath);
    // create the node
    struct fl_list *cur = fl_list_create(confname);
    g_free(confname);
    if(S_ISREG(dat.st_mode)) {
      cur->isfile = TRUE;
      cur->size = dat.st_size;
      cur->lastmod = dat.st_mtime;
    }
    g_free(vcpath);
    // and add it
    fl_list_add(parent, cur, -1);
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
      fl_scan_dir(cur, cpath, virtpath, inc_hidden, excl);
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
    struct fl_list *cur = fl_list_create("");
    char *tmp = g_filename_from_utf8(args->path[i], -1, NULL, NULL, NULL);
    cur->sub = g_ptr_array_new_with_free_func(fl_list_free);
    fl_scan_dir(cur, tmp, args->path[i], args->inc_hidden, args->excl_regex);
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
#define fl_hash_queue_append(fl) do {\
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

  // Initialize some stuff
  args->blocksize = tth_blocksize(args->filesize, 1<<(fl_hash_keep_level-1));
  args->blocksize = MAX(args->blocksize, fl_hash_max_granularity);
  int blocks_num = tth_num_blocks(args->filesize, args->blocksize);
  args->blocks = g_malloc(24*blocks_num);
  tth_init(&tth);

  int r;
  guint64 read = 0;
  int block_cur = 0;
  guint64 block_len = 0;

  while((r = fread(buf, 1, 10240, f)) > 0) {
    read += r;
    // no need to hash any further? quit!
    if(g_atomic_int_get(&fl_hash_reset) != args->resetnum)
      goto fl_hash_finish;
    // file has been modified. time to back out
    if(read > args->filesize) {
      g_set_error_literal(&(args->err), 1, 0, "File has been modified.");
      goto fl_hash_finish;
    }
    ratecalc_add(&fl_hash_rate, r);
    // and hash
    char *b = buf;
    while(r > 0) {
      int w = MIN(r, args->blocksize-block_len);
      tth_update(&tth, b, w);
      block_len += w;
      b += w;
      r -= w;
      if(block_len >= args->blocksize) {
        tth_final(&tth, args->blocks+(block_cur*24));
        tth_init(&tth);
        block_cur++;
        block_len = 0;
      }
    }
  }
  if(ferror(f)) {
    g_set_error(&(args->err), 1, 0, "Error reading file: %s", g_strerror(errno));
    goto fl_hash_finish;
  }
  if(read != args->filesize) {
    g_set_error_literal(&(args->err), 1, 0, "File has been modified.");
    goto fl_hash_finish;
  }
  // Calculate last block
  if(!args->filesize || block_len) {
    tth_final(&tth, args->blocks+(block_cur*24));
    block_cur++;
  }
  g_return_if_fail(block_cur == blocks_num);
  // Calculate root hash
  tth_root(args->blocks, blocks_num, args->root);

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
    fl_hashdat_setdone(TRUE);
    return;
  }
  fl_hashdat_setdone(FALSE);
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
  fl_hashindex_sethash(fl, args->root, args->lastmod, args->blocksize, args->blocks);
  fl_needflush = TRUE;

fl_hash_done_f:
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
    fl_local_list = fl_list_create("");
    fl_local_list->sub = g_ptr_array_new_with_free_func(fl_list_free);
  }
  struct fl_list *cur = fl_list_file(fl_local_list, name);
  // dir not present yet? create a stub
  if(!cur) {
    cur = fl_list_create(name);
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
      if(oldl->isfile && (!oldl->hastth || newl->lastmod > oldl->lastmod || newl->size != oldl->size)) {
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
    fl_hashdat_setdone(TRUE);

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


// fetches the last modification times from the hashdata file and checks
// whether we have any incomplete directories.
static gboolean fl_init_list(struct fl_list *fl) {
  gboolean incomplete = FALSE;
  int i;
  for(i=0; i<fl->sub->len; i++) {
    struct fl_list *c = g_ptr_array_index(fl->sub, i);
    if(c->isfile && fl_hashindex_load(c))
      incomplete = TRUE;
    if(!c->isfile && fl_init_list(c))
      incomplete = TRUE;
  }
  return incomplete;
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
  fl_local_list = sharing ? fl_load(fl_local_list_file, &err) : NULL;
  if(sharing && !fl_local_list) {
    ui_mf(ui_main, UIP_MED, "Error loading local filelist: %s. Re-building list.", err->message);
    g_error_free(err);
    dorefresh = TRUE;
    fl_hashdat_open(TRUE);
  } else if(sharing)
    fl_hashdat_open(FALSE);
  else {
    fl_hashdat_open(TRUE);
    // Force a refresh when we're not sharing anything. This makes sure that we
    // at least have a files.xml.bz2
    dorefresh = TRUE;
  }

  // If ncdc was previously closed while hashing, make sure to force a refresh
  // this time to continue the hash progress.
  if(!fl_hashdat_getdone())
    dorefresh = TRUE;

  // Get last modification times and check that all hashdata is present.
  if(fl_local_list) {
    if(fl_init_list(fl_local_list))
      dorefresh = TRUE;
    if(dorefresh)
      ui_m(ui_main, UIM_NOTIFY, "File list incomplete, refreshing...");
  }

  // reset loading indicator
  if(!fl_local_list || !dorefresh)
    ui_m(NULL, UIM_NOLOG|UIM_DIRECT, NULL);

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

