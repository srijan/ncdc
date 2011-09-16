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
#include <stdio.h>
#include <unistd.h>
#include <libxml/xmlreader.h>
#include <libxml/xmlwriter.h>
#include <bzlib.h>


#if INTERFACE

// file list

struct fl_list {
  struct fl_list *parent; // root = NULL
  GSequence *sub;
  guint64 size;   // including sub-items
  char tth[24];
  time_t lastmod; // only used for files in own list
  int hastth;     // for files: 1/0, directories: (number of directories) + (number of files with hastth==1)
  gboolean isfile : 1;
  gboolean incomplete : 1; // when a directory is missing files
  char name[];
};

#endif





// Utility functions

// only frees the given item and its childs. leaves the parent(s) untouched
void fl_list_free(gpointer dat) {
  struct fl_list *fl = dat;
  if(!fl)
    return;
  if(fl->sub)
    g_sequence_free(fl->sub);
  g_free(fl);
}


// This should be somewhat equivalent to
//   strcmp(g_utf8_casefold(a), g_utf8_casefold(b)),
// but hopefully faster by avoiding the memory allocations.
// Note that g_utf8_collate() is not suitable for case-insensitive filename
// comparison. For example, g_utf8_collate('a', 'A') != 0.
int fl_list_cmp_name(const char *a, const char *b) {
  int d;
  while(*a && *b) {
    d = g_unichar_tolower(g_utf8_get_char(a)) - g_unichar_tolower(g_utf8_get_char(b));
    if(d)
      return d;
    a = g_utf8_next_char(a);
    b = g_utf8_next_char(b);
  }
  return *a ? 1 : *b ? -1 : 0;
}


// Used for sorting items within a directory and determining whether two files
// in the same directory are equivalent (that is, have the same name). File
// names are case-insensitive, as required by the ADC protocol.
gint fl_list_cmp(gconstpointer a, gconstpointer b, gpointer dat) {
  return fl_list_cmp_name(((struct fl_list *)a)->name, ((struct fl_list *)b)->name);
}


// Adds `cur' to the directory `parent'. Returns NULL on success.
// This may fail when an item with the same name (according to fl_list_cmp())
// is already present. In such an event, two things can happen:
// 1. strcmp(dup, cur) <= 0:
//    `cur' is not inserted and this function returns `cur'.
// 2. strcmp(dup, cur) > 0:
//    `cur' is inserted and `dup' is returned.
// In either case, if the return value is non-NULL, fl_list_remove() should be
// called on it.
struct fl_list *fl_list_add(struct fl_list *parent, struct fl_list *cur) {
  // Check for duplicate.
  GSequenceIter *iter = g_sequence_iter_prev(g_sequence_search(parent->sub, cur, fl_list_cmp, NULL));
  struct fl_list *dup = g_sequence_iter_is_end(iter) ? NULL : g_sequence_get(iter);
  if(dup && fl_list_cmp_name(cur->name, dup->name) != 0)
    dup = NULL;
  if(dup)
    g_debug("fl_list_add(): Duplicate file found: \"%s\" == \"%s\"", cur->name, dup->name);
  if(dup && strcmp(dup->name, cur->name) <= 0)
    return cur;

  cur->parent = parent;
  g_sequence_insert_sorted(parent->sub, cur, fl_list_cmp, NULL);
  if(!cur->isfile || (cur->isfile && cur->hastth))
    parent->hastth++;
  // update parents size
  while(parent) {
    parent->size += cur->size;
    parent = parent->parent;
  }
  return dup;
}


// Removes an item from the file list, making sure to update the parents.
void fl_list_remove(struct fl_list *fl) {
  struct fl_list *par = fl->parent;
  GSequenceIter *iter = NULL;
  if(par) {
    // Can't use _lookup(), too new (2.28).
    // Note that in normal situations, there are no two items in a directory
    // for which fl_list_cmp() returns 0. However, since this does happen just
    // after fl_list_add() has returned `dup', we need to handle it in order to
    // create a correct list again.
    iter = g_sequence_iter_prev(g_sequence_search(par->sub, fl, fl_list_cmp, NULL));
    g_return_if_fail(!g_sequence_iter_is_end(iter));
    while(g_sequence_get(iter) != fl && fl_list_cmp_name(((struct fl_list *)g_sequence_get(iter))->name, fl->name) == 0)
      iter = g_sequence_iter_prev(iter);
    g_return_if_fail(g_sequence_get(iter) == fl);

    // update parent->hastth
    if(!fl->isfile || (fl->isfile && fl->hastth))
      par->hastth--;
  }
  // update parents size
  while(par) {
    par->size -= fl->size;
    par = par->parent;
  }
  // and free
  if(iter)
    g_sequence_remove(iter); // also frees the item
  else
    fl_list_free(fl);
}


struct fl_list *fl_list_copy(const struct fl_list *fl) {
  struct fl_list *cur = g_memdup(fl, sizeof(struct fl_list)+strlen(fl->name)+1);
  cur->parent = NULL;
  if(fl->sub) {
    cur->sub = g_sequence_new(fl_list_free);
    GSequenceIter *iter;
    // No need to use _insert_sorted() here, since we walk through the list in
    // the correct order already.
    for(iter=g_sequence_get_begin_iter(fl->sub); !g_sequence_iter_is_end(iter); iter=g_sequence_iter_next(iter)) {
      struct fl_list *tmp = fl_list_copy(g_sequence_get(iter));
      tmp->parent = cur;
      g_sequence_append(cur->sub, tmp);
    }
  }
  return cur;
}


// get a file by name in a directory
struct fl_list *fl_list_file(const struct fl_list *dir, const char *name) {
  struct fl_list *cmp = g_alloca(sizeof(struct fl_list)+strlen(name)+1);
  strcpy(cmp->name, name);
  GSequenceIter *iter = g_sequence_iter_prev(g_sequence_search(dir->sub, cmp, fl_list_cmp, NULL));
  return g_sequence_iter_is_end(iter)
    || fl_list_cmp_name(name, ((struct fl_list *)g_sequence_get(iter))->name) != 0 ? NULL : g_sequence_get(iter);
}


gboolean fl_list_is_child(const struct fl_list *parent, const struct fl_list *child) {
  for(child=child->parent; child; child=child->parent)
    if(child == parent)
      return TRUE;
  return FALSE;
}


// Get the virtual path to a file
char *fl_list_path(struct fl_list *fl) {
  if(!fl->parent)
    return g_strdup("/");
  char *tmp, *path = g_strdup(fl->name);
  struct fl_list *cur = fl->parent;
  while(cur->parent) {
    tmp = path;
    path = g_build_filename(cur->name, path, NULL);
    g_free(tmp);
    cur = cur->parent;
  }
  tmp = path;
  path = g_build_filename("/", path, NULL);
  g_free(tmp);
  return path;
}


// Resolves a path string (Either absolute or relative to root). Does not
// support stuff like ./ and ../, and '/' is assumed to refer to the given
// root. (So '/dir' and 'dir' are simply equivalent)
// Case-insensitive, and '/' is the only recognised path separator
struct fl_list *fl_list_from_path(struct fl_list *root, const char *path) {
  while(path[0] == '/')
    path++;
  if(!path[0])
    return root;
  g_return_val_if_fail(root->sub, NULL);
  int slash = strcspn(path, "/");
  char *name = g_strndup(path, slash);
  struct fl_list *n = fl_list_file(root, name);
  g_free(name);
  if(!n)
    return NULL;
  if(slash == strlen(path))
    return n;
  if(n->isfile)
    return NULL;
  return fl_list_from_path(n, path+slash+1);
}


// Auto-complete for fl_list_from_path()
void fl_list_suggest(struct fl_list *root, char *opath, char **sug) {
  struct fl_list *parent = root;
  char *path = g_strdup(opath);
  char *name = path;
  char *sep = strrchr(path, '/');
  if(sep) {
    *sep = 0;
    name = sep+1;
    parent = fl_list_from_path(parent, path);
  } else {
    name = path;
    path = "";
  }
  if(parent) {
    int i = 0, len = strlen(name);
    // Note: performance can be improved by using _search() instead
    GSequenceIter *iter;
    for(iter=g_sequence_get_begin_iter(parent->sub); i<20 && !g_sequence_iter_is_end(iter); iter=g_sequence_iter_next(iter)) {
      struct fl_list *n = g_sequence_get(iter);
      if(strncmp(n->name, name, len) == 0)
        sug[i++] = n->isfile ? g_strconcat(path, "/", n->name, NULL) : g_strconcat(path, "/", n->name, "/", NULL);
    }
  }
  if(sep)
    g_free(path);
  else
    g_free(name);
}




// searching

#if INTERFACE

struct fl_search {
  char sizem;   // -2 any, -1 <=, 0 ==, 1 >=
  char filedir; // 1 = file, 2 = dir, 3 = any
  guint64 size;
  char **ext;   // extension list
  char **and;   // keywords that must all be present
  char **not;   // keywords that may not be present
};


#define fl_search_match(fl, s) (\
     ((((s)->filedir & 2) && !(fl)->isfile) || (((s)->filedir & 1) && (fl)->isfile && (fl)->hastth))\
  && ((s)->sizem == -2 || (!(s)->sizem && (fl)->size == (s)->size)\
      || ((s)->sizem < 0 && (fl)->size <= (s)->size) || ((s)->sizem > 0 && (fl)->size > (s)->size))\
  && fl_search_match_name(fl, s))


#endif


// Only matches against fl->name itself, not the path to it (AND keywords
// matched in the path are assumed to be removed already)
gboolean fl_search_match_name(struct fl_list *fl, struct fl_search *s) {
  char **tmp;

  for(tmp=s->and; tmp&&*tmp; tmp++)
    if(G_LIKELY(!str_casestr(fl->name, *tmp)))
      return FALSE;

  for(tmp=s->not; tmp&&*tmp; tmp++)
    if(str_casestr(fl->name, *tmp))
      return FALSE;

  tmp = s->ext;
  if(!tmp || !*tmp)
    return TRUE;

  char *l = strrchr(fl->name, '.');
  if(G_UNLIKELY(!l || !l[1]))
    return FALSE;
  l++;
  for(; *tmp; tmp++)
    if(G_UNLIKELY(g_ascii_strcasecmp(l, *tmp) == 0))
      return TRUE;
  return FALSE;
}


// Recursive depth-first search through the list, used for replying to non-TTH
// $Search and SCH requests. Not exactly fast, but what did you expect? :-(
int fl_search_rec(struct fl_list *parent, struct fl_search *s, struct fl_list **res, int max) {
  if(!parent || !parent->sub)
    return 0;
  GSequenceIter *iter;
  // weed out stuff from 'and' if it's already matched in parent (I'm assuming
  // that stuff matching the parent of parent has already been removed)
  char **o = s->and;
  char *nand[o ? g_strv_length(o) : 0];
  int i = 0;
  for(; o&&*o; o++)
    if(G_LIKELY(!parent->parent || !str_casestr(parent->name, *o)))
      nand[i++] = *o;
  nand[i] = NULL;
  o = s->and;
  s->and = nand;
  // loop through the directory
  i = 0;
  for(iter=g_sequence_get_begin_iter(parent->sub); i<max && !g_sequence_iter_is_end(iter); iter=g_sequence_iter_next(iter)) {
    struct fl_list *n = g_sequence_get(iter);
    if(fl_search_match(n, s))
      res[i++] = n;
    if(!n->isfile && i < max)
      i += fl_search_rec(n, s, res+i, max-i);
  }
  s->and = o;
  return i;
}


// Similar to fl_search_match(), but also matches the name of the parents.
gboolean fl_search_match_full(struct fl_list *fl, struct fl_search *s) {
  // weed out stuff from 'and' if it's already matched in any of its parents.
  char **oand = s->and;
  int len = s->and ? g_strv_length(s->and) : 0;
  char *nand[len];
  struct fl_list *p = fl->parent;
  int i;
  memcpy(nand, s->and, len*sizeof(char *));
  for(; p && p->parent; p=p->parent)
    for(i=0; i<len; i++)
      if(G_UNLIKELY(nand[i] && str_casestr(p->name, nand[i])))
        nand[i] = NULL;
  char *and[len];
  int j=0;
  for(i=0; i<len; i++)
    if(nand[i])
      and[j++] = nand[i];
  and[j] = NULL;
  s->and = and;
  // and now match
  gboolean r = fl_search_match(fl, s);
  s->and = oand;
  return r;
}







// Internal structure used by fl_load() and fl_save()

struct fl_loadsave_context {
  char *file;     // some name, for debugging purposes
  BZFILE *fh_bz;  // if BZ2 compression is enabled (implies fh_h!=NULL)
  FILE *fh_f;     // if we're working with a file
  GString *buf;   // if we're working with a buffer (only fl_save() supports this)
  GError **err;
  gboolean stream_end;
};




// Read filelist from an xml file


static int fl_load_input(void *context, char *buf, int len) {
  struct fl_loadsave_context *xc = context;
  if(xc->stream_end)
    return 0;
  int bzerr;
  if(xc->fh_bz) {
    int r = BZ2_bzRead(&bzerr, xc->fh_bz, buf, len);
    if(bzerr != BZ_OK && bzerr != BZ_STREAM_END) {
      g_set_error(xc->err, 1, 0, "bzip2 decompression error. (%d)", bzerr);
      return -1;
    } else {
      xc->stream_end = bzerr == BZ_STREAM_END;
      return r;
    }
  } else if(xc->fh_f) {
    int r = fread(buf, 1, len, xc->fh_f);
    if(r < 0)
      g_set_error(xc->err, 1, 0, "Read error: %s", g_strerror(errno));
    xc->stream_end = r <= 0;
    return r;
  } else
    g_return_val_if_reached(-1);
}


static int fl_load_close(void *context) {
  struct fl_loadsave_context *xc = context;
  int bzerr;
  if(xc->fh_bz)
    BZ2_bzReadClose(&bzerr, xc->fh_bz);
  fclose(xc->fh_f);
  g_free(xc->file);
  g_slice_free(struct fl_loadsave_context, xc);
  return 0;
}


static void fl_load_error(void *arg, const char *msg, xmlParserSeverities severity, xmlTextReaderLocatorPtr locator) {
  struct fl_loadsave_context *xc = arg;
  if(severity == XML_PARSER_SEVERITY_VALIDITY_WARNING || severity == XML_PARSER_SEVERITY_WARNING)
    g_warning("XML parse warning in %s line %d: %s", xc->file, xmlTextReaderLocatorLineNumber(locator), msg);
  else if(xc->err && !*(xc->err))
    g_set_error(xc->err, 1, 0, "XML parse error on input line %d: %s", xmlTextReaderLocatorLineNumber(locator), msg);
}


static int fl_load_handle(xmlTextReaderPtr reader, gboolean *havefl, gboolean *newdir, struct fl_list **cur) {
  struct fl_list *tmp;
  char *attr[3];
  char name[50], *tmpname;

  tmpname = (char *)xmlTextReaderName(reader);
  strncpy(name, tmpname, 50);
  name[49] = 0;
  free(tmpname);

  switch(xmlTextReaderNodeType(reader)) {

  case XML_READER_TYPE_ELEMENT:
    // <FileListing ..>
    // We ignore its attributes (for now)
    if(strcmp(name, "FileListing") == 0) {
      if(*havefl)
        return -1;
      *havefl = TRUE;
    // <Directory ..>
    } else if(strcmp(name, "Directory") == 0) {
      if(!*havefl)
        return -1;
      if(!(attr[0] = (char *)xmlTextReaderGetAttribute(reader, (xmlChar *)"Name")))
        return -1;
      attr[1] = (char *)xmlTextReaderGetAttribute(reader, (xmlChar *)"Incomplete");
      if(attr[1] && strcmp(attr[1], "0") != 0 && strcmp(attr[1], "1") != 0) {
        free(attr[0]);
        return -1;
      }
      tmp = g_malloc0(sizeof(struct fl_list)+strlen(attr[0])+1);
      strcpy(tmp->name, attr[0]);
      tmp->isfile = FALSE;
      tmp->incomplete = attr[1] && attr[1][0] == '1';
      tmp->sub = g_sequence_new(fl_list_free);
      if(fl_list_add(*newdir ? *cur : (*cur)->parent, tmp)) {
        fl_list_remove(tmp);
        free(attr[0]);
        free(attr[1]);
        return -1;
      }
      *cur = tmp;
      *newdir = !xmlTextReaderIsEmptyElement(reader);
      free(attr[0]);
      free(attr[1]);
    // <File .. />
    } else if(strcmp(name, "File") == 0) {
      if(!*havefl || !xmlTextReaderIsEmptyElement(reader))
        return -1;
      if(!(attr[0] = (char *)xmlTextReaderGetAttribute(reader, (xmlChar *)"Name")))
        return -1;
      attr[1] = (char *)xmlTextReaderGetAttribute(reader, (xmlChar *)"Size");
      if(!attr[1] || strspn(attr[1], "0123456789") != strlen(attr[1])) {
        free(attr[0]);
        return -1;
      }
      attr[2] = (char *)xmlTextReaderGetAttribute(reader, (xmlChar *)"TTH");
      if(!attr[2] || !istth(attr[2])) {
        free(attr[0]);
        free(attr[1]);
        return -1;
      }
      tmp = g_malloc0(sizeof(struct fl_list)+strlen(attr[0])+1);
      strcpy(tmp->name, attr[0]);
      tmp->isfile = TRUE;
      tmp->size = g_ascii_strtoull(attr[1], NULL, 10);
      tmp->hastth = 1;
      base32_decode(attr[2], tmp->tth);
      if(fl_list_add(*newdir ? *cur : (*cur)->parent, tmp)) {
        fl_list_remove(tmp);
        free(attr[0]);
        free(attr[1]);
        free(attr[2]);
        return -1;
      }
      *cur = tmp;
      *newdir = FALSE;
      free(attr[0]);
      free(attr[1]);
      free(attr[2]);
    }
    break;

  case XML_READER_TYPE_END_ELEMENT:
    // </Directory>
    if(strcmp(name, "Directory") == 0) {
      if(!*newdir)
        *cur = (*cur)->parent;
      else
        *newdir = FALSE;
    // </FileListing>
    } else if(strcmp(name, "FileListing") == 0) {
      return 0; // stop reading
    }
    break;
  }
  return 1;
}


static xmlTextReaderPtr fl_load_open(const char *file, GError **err) {
  gboolean isbz2 = strlen(file) > 4 && strcmp(file+(strlen(file)-4), ".bz2") == 0;

  // open file
  FILE *f = fopen(file, "r");
  if(!f) {
    g_set_error_literal(err, 1, 0, g_strerror(errno));
    return NULL;
  }

  // open BZ2 decompression
  BZFILE *bzf = NULL;
  if(isbz2) {
    int bzerr;
    bzf = BZ2_bzReadOpen(&bzerr, f, 0, 0, NULL, 0);
    if(bzerr != BZ_OK) {
      g_set_error(err, 1, 0, "Unable to open BZ2 file (%d)", bzerr);
      fclose(f);
      return NULL;
    }
  }

  // create reader
  struct fl_loadsave_context *xc = g_slice_new0(struct fl_loadsave_context);
  xc->err = err;
  xc->file = g_strdup(file);
  xc->fh_f = f;
  xc->fh_bz = bzf;
  xmlTextReaderPtr reader = xmlReaderForIO(fl_load_input, fl_load_close, xc, NULL, NULL, XML_PARSE_NOENT);

  if(!reader) {
    fl_load_close(xc);
    if(err && !*err)
      g_set_error_literal(err, 1, 0, "Failed to open file.");
    return NULL;
  }

  xmlTextReaderSetErrorHandler(reader, fl_load_error, xc);
  return reader;
}


struct fl_list *fl_load(const char *file, GError **err) {
  g_return_val_if_fail(err == NULL || *err == NULL, NULL);

  // open the file
  xmlTextReaderPtr reader = fl_load_open(file, err);
  if(!reader)
    return NULL;

  // parse & read
  struct fl_list *cur, *root;
  gboolean havefl = FALSE, newdir = TRUE;
  int ret;

  root = g_malloc0(sizeof(struct fl_list)+1);
  root->name[0] = 0;
  root->sub = g_sequence_new(fl_list_free);
  cur = root;

  while((ret = xmlTextReaderRead(reader)) == 1)
    if((ret = fl_load_handle(reader, &havefl, &newdir, &cur)) <= 0)
      break;

  if(ret < 0 || !havefl) {
    if(err && !*err) // rather uninformative error message as fallback
      g_set_error_literal(err, 1, 0, !havefl ? "No <FileListing> tag found." : "Error parsing or validating XML.");
    fl_list_free(root);
    root = NULL;
  }

  // close (ignoring errors)
  xmlTextReaderClose(reader);
  xmlFreeTextReader(reader);

  return root;
}





// Async version of fl_load(). Performs the load in a background thread.

static GThreadPool *fl_load_pool = NULL;

struct fl_load_async_dat {
  char *file;
  void (*cb)(struct fl_list *, GError *, void *);
  void *dat;
  GError *err;
  struct fl_list *fl;
};


static gboolean fl_load_async_d(gpointer dat) {
  struct fl_load_async_dat *arg = dat;
  arg->cb(arg->fl, arg->err, arg->dat);
  g_free(arg->file);
  g_slice_free(struct fl_load_async_dat, arg);
  return FALSE;
}


static void fl_load_async_f(gpointer dat, gpointer udat) {
  struct fl_load_async_dat *arg = dat;
  arg->fl = fl_load(arg->file, &arg->err);
  g_idle_add(fl_load_async_d, arg);
}


// Ownership of both the file list and the error is passed to the callback
// function.
void fl_load_async(const char *file, void (*cb)(struct fl_list *, GError *, void *), void *dat) {
  if(!fl_load_pool)
    fl_load_pool = g_thread_pool_new(fl_load_async_f, NULL, 2, FALSE, NULL);
  struct fl_load_async_dat *arg = g_slice_new0(struct fl_load_async_dat);
  arg->file = g_strdup(file);
  arg->dat = dat;
  arg->cb = cb;
  g_thread_pool_push(fl_load_pool, arg, NULL);
}





// Save a filelist to a .xml file

static int fl_save_write(void *context, const char *buf, int len) {
  struct fl_loadsave_context *xc = context;
  if(xc->fh_bz) {
    int bzerr;
    BZ2_bzWrite(&bzerr, xc->fh_bz, (char *)buf, len);
    if(bzerr == BZ_OK)
      return len;
    if(bzerr == BZ_IO_ERROR) {
      g_set_error_literal(xc->err, 1, 0, "bzip2 write error.");
      return -1;
    }
    g_return_val_if_reached(-1);
  } else if(xc->fh_f) {
    int r = fwrite(buf, 1, len, xc->fh_f);
    if(r < 0)
      g_set_error(xc->err, 1, 0, "Write error: %s", g_strerror(errno));
    return r;
  } else if(xc->buf) {
    g_string_append_len(xc->buf, buf, len);
    return len;
  } else
    g_return_val_if_reached(-1);
}


static int fl_save_close(void *context) {
  struct fl_loadsave_context *xc = context;
  int bzerr;
  if(xc->fh_bz)
    BZ2_bzWriteClose(&bzerr, xc->fh_bz, 0, NULL, NULL);
  if(xc->fh_f)
    fclose(xc->fh_f);
  g_free(xc->file);
  g_slice_free(struct fl_loadsave_context, xc);
  return 0;
}


// recursive
static gboolean fl_save_childs(xmlTextWriterPtr writer, struct fl_list *fl, int level) {
  GSequenceIter *iter;
  for(iter=g_sequence_get_begin_iter(fl->sub); !g_sequence_iter_is_end(iter); iter=g_sequence_iter_next(iter)) {
    struct fl_list *cur = g_sequence_get(iter);
#define CHECKFAIL(f) if(f < 0) return FALSE
    if(cur->isfile && cur->hastth) {
      char tth[40];
      base32_encode(cur->tth, tth);
      tth[39] = 0;
      CHECKFAIL(xmlTextWriterStartElement(writer, (xmlChar *)"File"));
      CHECKFAIL(xmlTextWriterWriteAttribute(writer, (xmlChar *)"Name", (xmlChar *)cur->name));
      CHECKFAIL(xmlTextWriterWriteFormatAttribute(writer, (xmlChar *)"Size", "%"G_GUINT64_FORMAT, cur->size));
      CHECKFAIL(xmlTextWriterWriteAttribute(writer, (xmlChar *)"TTH", (xmlChar *)tth));
      CHECKFAIL(xmlTextWriterEndElement(writer));
    }
    if(!cur->isfile) {
      CHECKFAIL(xmlTextWriterStartElement(writer, (xmlChar *)"Directory"));
      CHECKFAIL(xmlTextWriterWriteAttribute(writer, (xmlChar *)"Name", (xmlChar *)cur->name));
      if(cur->incomplete || cur->hastth != g_sequence_iter_get_position(g_sequence_get_end_iter(cur->sub))
          || (cur->hastth && level < 1))
        CHECKFAIL(xmlTextWriterWriteAttribute(writer, (xmlChar *)"Incomplete", (xmlChar *)"1"));
      if(level > 0)
        fl_save_childs(writer, cur, level-1);
      CHECKFAIL(xmlTextWriterEndElement(writer));
    }
#undef CHECKFAIL
  }
  return TRUE;
}


static xmlTextWriterPtr fl_save_open(const char *file, gboolean isbz2, GString *buf, GError **err) {
  // open file (if any)
  FILE *f = NULL;
  if(file) {
    f = fopen(file, "w");
    if(!f) {
      g_set_error_literal(err, 1, 0, g_strerror(errno));
      return NULL;
    }
  }

  // open compressor (if needed)
  BZFILE *bzf = NULL;
  if(f && isbz2) {
    int bzerr;
    bzf = BZ2_bzWriteOpen(&bzerr, f, 7, 0, 0);
    if(bzerr != BZ_OK) {
      g_set_error(err, 1, 0, "Unable to create BZ2 file (%d)", bzerr);
      fclose(f);
      return NULL;
    }
  }

  // create writer
  struct fl_loadsave_context *xc = g_slice_new0(struct fl_loadsave_context);
  xc->err = err;
  xc->file = file ? g_strdup(file) : g_strdup("string buffer");
  xc->fh_f = f;
  xc->fh_bz = bzf;
  xc->buf = buf;
  xmlTextWriterPtr writer = xmlNewTextWriter(xmlOutputBufferCreateIO(fl_save_write, fl_save_close, xc, NULL));

  if(!writer) {
    fl_save_close(xc);
    if(err && !*err)
      g_set_error_literal(err, 1, 0, "Failed to open file.");
    return NULL;
  }
  return writer;
}


gboolean fl_save(struct fl_list *fl, const char *file, GString *buf, int level, GError **err) {
  g_return_val_if_fail(err == NULL || *err == NULL, FALSE);

  // open a temporary file for writing
  gboolean isbz2 = FALSE;
  char *tmpfile = NULL;
  if(file) {
    isbz2 = strlen(file) > 4 && strcmp(file+(strlen(file)-4), ".bz2") == 0;
    tmpfile = g_strdup_printf("%s.tmp-%d", file, rand());
  }

  xmlTextWriterPtr writer = fl_save_open(tmpfile, isbz2, buf, err);
  if(!writer) {
    g_free(tmpfile);
    return FALSE;
  }

  // write
  gboolean success = TRUE;
#define CHECKFAIL(f) if((f) < 0) { success = FALSE; goto fl_save_error; }
  CHECKFAIL(xmlTextWriterSetIndent(writer, 1));
  CHECKFAIL(xmlTextWriterSetIndentString(writer, (xmlChar *)"\t"));
  // <FileListing ..>
  CHECKFAIL(xmlTextWriterStartDocument(writer, NULL, "utf-8", "yes"));
  CHECKFAIL(xmlTextWriterStartElement(writer, (xmlChar *)"FileListing"));
  CHECKFAIL(xmlTextWriterWriteAttribute(writer, (xmlChar *)"Version", (xmlChar *)"1"));
  CHECKFAIL(xmlTextWriterWriteAttribute(writer, (xmlChar *)"Generator", (xmlChar *)PACKAGE_STRING));

  char cid[40] = {};
  base32_encode(conf_cid, cid);
  CHECKFAIL(xmlTextWriterWriteAttribute(writer, (xmlChar *)"CID", (xmlChar *)cid));

  char *path = fl ? fl_list_path(fl) : g_strdup("/");
  CHECKFAIL(xmlTextWriterWriteAttribute(writer, (xmlChar *)"Base", (xmlChar *)path));
  g_free(path);

  // all <Directory ..> elements
  if(fl && fl->sub) {
    if(!fl_save_childs(writer, fl, level-1)) {
      success = FALSE;
      goto fl_save_error;
    }
  }

  CHECKFAIL(xmlTextWriterEndElement(writer));

  // close
fl_save_error:
  xmlTextWriterEndDocument(writer);
  xmlFreeTextWriter(writer);

  // rename or unlink file
  if(file) {
    if(success && rename(tmpfile, file) < 0) {
      if(err && !*err)
        g_set_error_literal(err, 1, 0, g_strerror(errno));
      success = FALSE;
    }
    if(!success)
      unlink(tmpfile);
    g_free(tmpfile);
  }
  return success;
}

