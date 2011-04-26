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
#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <libxml/xmlreader.h>
#include <libxml/xmlwriter.h>
#include <bzlib.h>
#include <glib/gstdio.h>


#if INTERFACE

// file list

struct fl_list {
  char *name; // root = NULL
  struct fl_list *parent;
  GSequence *sub;
  guint64 size;   // including sub-items
  time_t lastmod; // only used for own list. for dirs: lastmod = max(sub->lastmod)
  gboolean isfile : 1;
  gboolean hastth : 1; // "iscomplete" for directories
  char tth[24];
};

#endif

// own list
char *fl_own_list_file;
struct fl_list *fl_own_list;

static GThreadPool *fl_scan_pool;





// Utility functions

// only frees the given item and its childs. leaves the parent(s) untouched
static void fl_list_free(gpointer dat) {
  struct fl_list *fl = dat;
  if(!fl)
    return;
  g_free(fl->name);
  if(fl->sub)
    g_sequence_free(fl->sub);
  g_free(fl);
}


static void fl_list_add(struct fl_list *parent, struct fl_list *cur) {
  cur->parent = parent;
  g_sequence_append(parent->sub, cur); // TODO: sorted
  if(cur->isfile && parent->hastth && !cur->hastth)
    parent->hastth = FALSE;
  // update parents size & lastmod
  while(parent) {
    parent->size += cur->size;
    parent->lastmod = MAX(parent->lastmod, cur->lastmod);
    parent = parent->parent;
  }
}


static gboolean fl_has_incomplete(struct fl_list *fl) {
  GSequenceIter *iter;
  for(iter=g_sequence_get_begin_iter(fl->sub); !g_sequence_iter_is_end(iter); iter=g_sequence_iter_next(iter)) {
    struct fl_list *c = g_sequence_get(iter);
    if(!c->isfile && !c->hastth)
      return TRUE;
    if(!c->isfile)
      fl_has_incomplete(c);
  }
  return FALSE;
}





// Read filelist from an xml file

struct fl_loadsave_context {
  char *file;
  BZFILE *fh_bz;
  FILE *fh_f;
  GError **err;
  gboolean stream_end;
};


static int fl_load_input(void *context, char *buf, int len) {
  struct fl_loadsave_context *xc = context;
  if(xc->stream_end)
    return 0;
  int bzerr;
  int r = BZ2_bzRead(&bzerr, xc->fh_bz, buf, len);
  if(bzerr != BZ_OK && bzerr != BZ_STREAM_END) {
    g_set_error(xc->err, 1, 0, "bzip2 decompression error. (%d)", bzerr);
    return -1;
  } else {
    xc->stream_end = bzerr == BZ_STREAM_END;
    return r;
  }
}


static int fl_load_close(void *context) {
  struct fl_loadsave_context *xc = context;
  int bzerr;
  BZ2_bzReadClose(&bzerr, xc->fh_bz);
  fclose(xc->fh_f);
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
  char *name = (char *)xmlTextReaderName(reader);

  switch(xmlTextReaderNodeType(reader)) {

  case XML_READER_TYPE_ELEMENT:
    // <FileListing ..>
    // We ignore its attributes (for now)
    if(strcmp(name, "FileListing") == 0) {
      if(*havefl || xmlTextReaderIsEmptyElement(reader))
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
      tmp = g_new0(struct fl_list, 1);
      tmp->name = g_strdup(attr[0]);
      tmp->isfile = FALSE;
      tmp->hastth = !attr[1] || attr[1][0] == '0';
      tmp->sub = g_sequence_new(fl_list_free);
      fl_list_add(*newdir ? *cur : (*cur)->parent, tmp);
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
      if(!attr[2] || strlen(attr[2]) != 39 || strspn(attr[2], "ABCDEFGHIJKLMNOPQRSTUVWXYZ234567") != 39) {
        free(attr[0]);
        free(attr[1]);
        return -1;
      }
      tmp = g_new0(struct fl_list, 1);
      tmp->name = g_strdup(attr[0]); // TODO: check for UTF-8? or does libxml2 already do this?
      tmp->isfile = TRUE;
      tmp->size = g_ascii_strtoull(attr[1], NULL, 10);
      tmp->hastth = TRUE;
      base32_decode(attr[2], tmp->tth);
      fl_list_add(*newdir ? *cur : (*cur)->parent, tmp);
      *newdir = FALSE;
      *cur = tmp;
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


struct fl_list *fl_load(const char *file, GError **err) {
  g_return_val_if_fail(err == NULL || *err == NULL, NULL);

  // open the file
  gboolean isbz2 = strlen(file) > 4 && strcmp(file+(strlen(file)-4), ".bz2") == 0;
  xmlTextReaderPtr reader;

  struct fl_loadsave_context xc;
  xc.stream_end = FALSE;
  xc.err = err;

  if(isbz2) {
    xc.fh_f = fopen(file, "r");
    if(!xc.fh_f) {
      g_set_error_literal(err, 1, 0, g_strerror(errno));
      return NULL;
    }
    int bzerr;
    xc.fh_bz = BZ2_bzReadOpen(&bzerr, xc.fh_f, 0, 0, NULL, 0);
    g_return_val_if_fail(bzerr == BZ_OK, NULL);
    reader = xmlReaderForIO(fl_load_input, fl_load_close, &xc, NULL, NULL, XML_PARSE_NOENT);
  } else
    reader = xmlReaderForFile(file, NULL, XML_PARSE_NOENT);

  if(!reader) {
    if(err && !*err)
      g_set_error_literal(err, 1, 0, "Failed to open file.");
    return NULL;
  }
  xmlTextReaderSetErrorHandler(reader, fl_load_error, &xc);

  // parse & read
  struct fl_list *cur, *root;
  gboolean havefl = FALSE, newdir = TRUE;
  int ret;

  root = g_new0(struct fl_list, 1);
  root->sub = g_sequence_new(fl_list_free);
  cur = root;

  while((ret = xmlTextReaderRead(reader)) == 1)
    if((ret = fl_load_handle(reader, &havefl, &newdir, &cur)) <= 0)
      break;

  if(ret < 0) {
    if(err && !*err) // rather uninformative error message as fallback
      g_set_error_literal(err, 1, 0, "Error parsing or validating XML.");
    fl_list_free(root);
    return NULL;
  }

  // close (ignoring errors)
  xmlTextReaderClose(reader);
  // TODO: check whether we need to free() something as well. Documentation is unclear on this.

  return root;
}





// Save a filelist to a .xml(.bz?) file

static int fl_save_write(void *context, const char *buf, int len) {
  struct fl_loadsave_context *xc = context;
  int bzerr;
  BZ2_bzWrite(&bzerr, xc->fh_bz, (char *)buf, len);
  if(bzerr == BZ_OK)
    return len;
  if(bzerr == BZ_IO_ERROR) {
    g_set_error_literal(xc->err, 1, 0, "bzip2 write error.");
    return -1;
  }
  g_return_val_if_reached(-1);
}


static int fl_save_close(void *context) {
  struct fl_loadsave_context *xc = context;
  int bzerr;
  BZ2_bzWriteClose(&bzerr, xc->fh_bz, 0, NULL, NULL);
  fclose(xc->fh_f);
  return 0;
}


// recursive
static gboolean fl_save_childs(xmlTextWriterPtr writer, struct fl_list *fl) {
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
      if(!cur->hastth)
        CHECKFAIL(xmlTextWriterWriteAttribute(writer, (xmlChar *)"Incomplete", (xmlChar *)"1"));
      fl_save_childs(writer, cur);
      CHECKFAIL(xmlTextWriterEndElement(writer));
    }
#undef CHECKFAIL
  }
  return TRUE;
}


gboolean fl_save(struct fl_list *fl, const char *file, GError **err) {
  g_return_val_if_fail(err == NULL || *err == NULL, FALSE);

  // open the file
  gboolean isbz2 = strlen(file) > 4 && strcmp(file+(strlen(file)-4), ".bz2") == 0;
  xmlTextWriterPtr writer;

  struct fl_loadsave_context xc;
  xc.err = err;

  if(isbz2) {
    xc.fh_f = fopen(file, "w");
    if(!xc.fh_f) {
      g_set_error_literal(err, 0, 0, g_strerror(errno));
      return FALSE;
    }
    int bzerr;
    xc.fh_bz = BZ2_bzWriteOpen(&bzerr, xc.fh_f, 7, 0, 0);
    g_return_val_if_fail(bzerr == BZ_OK, FALSE);
    writer = xmlNewTextWriter(xmlOutputBufferCreateIO(fl_save_write, fl_save_close, &xc, NULL));
  } else
    writer = xmlNewTextWriterFilename(file, 0);

  if(!writer) {
    if(err && !*err)
      g_set_error_literal(err, 1, 0, "Failed to open file.");
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
  CHECKFAIL(xmlTextWriterWriteAttribute(writer, (xmlChar *)"Base", (xmlChar *)"/"));
  // TODO: generate a proper CID
  CHECKFAIL(xmlTextWriterWriteAttribute(writer, (xmlChar *)"CID", (xmlChar *)"NCDCDOESNOTHAVECIDSUPPORTYET23456723456"));

  // all <Directory ..> elements
  if(!fl_save_childs(writer, fl)) {
    success = FALSE;
    goto fl_save_error;
  }

  CHECKFAIL(xmlTextWriterEndElement(writer));

  // close
fl_save_error:
  xmlTextWriterEndDocument(writer);
  xmlFreeTextWriter(writer);

  return success;
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
    // TODO: report error
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
    char *confname = g_filename_from_utf8(name, -1, NULL, NULL, NULL);
    if(!confname)
      confname = g_filename_display_name(name);
    char *encname = g_filename_from_utf8(confname, -1, NULL, NULL, NULL);
    if(!encname) { // TODO: report error
      g_free(confname);
      continue;
    }
    char *cpath = g_build_filename(path, encname, NULL);
    GStatBuf dat;
    // we're currently following symlinks, but I'm not sure whether that's a good idea yet
    int r = g_stat(cpath, &dat);
    g_free(encname);
    g_free(cpath);
    if(r < 0 || !(S_ISREG(dat.st_mode) || S_ISDIR(dat.st_mode))) {// TODO: report error
      g_free(confname);
      continue;
    }
    // and create the node
    struct fl_list *cur = g_new0(struct fl_list, 1);
    cur->name = g_filename_display_name(confname);
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
    char *cpath = g_build_filename(path, cur->name, NULL);
    cur->sub = g_sequence_new(fl_list_free);
    fl_scan_dir(cur, cpath);
    g_free(cpath);
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
    cur->sub = g_sequence_new(fl_list_free);
    cur->name = g_strdup(args->name[i]);
    fl_scan_dir(cur, args->path[i]);
    fl_list_add(root, cur);
  }

  g_idle_add_full(G_PRIORITY_HIGH_IDLE, args->donefun, root, NULL);
  g_strfreev(args->name);
  g_strfreev(args->path);
  g_free(args);
}





// Own file list management

void fl_init() {
  GError *err = NULL;
  gboolean dorefresh = FALSE;

  // init stuff
  fl_own_list = NULL;
  fl_own_list_file = g_build_filename(conf_dir, "files.xml.bz2", NULL);
  fl_scan_pool = g_thread_pool_new(fl_scan_thread, NULL, 1, FALSE, NULL);

  // read config
  char **shares = g_key_file_get_keys(conf_file, "share", NULL, NULL);
  if(!shares)
    return;

  // load our files.xml.bz
  fl_own_list = fl_load(fl_own_list_file, &err);
  if(!fl_own_list) {
    g_assert(err);
    ui_msgf(FALSE, "Error loading own filelist: %s. Re-building list.", err->message);
    g_error_free(err);
    dorefresh = TRUE;
  }

  // TODO: load the "lastmod" info from a hash data file

  // Check for any incomplete directories and initiate a refresh if there is
  // one.  (If there is an incomplete directory, it means that ncdc was closed
  // while it was hashing files, a refresh will continue where it left off)
  if(!dorefresh) {
    dorefresh = fl_has_incomplete(fl_own_list);
    if(dorefresh)
      ui_msg(TRUE, "File list incomplete, refreshing...");
  }

  if(dorefresh)
    fl_refresh(NULL);

  g_strfreev(shares);
}


static gboolean fl_refresh_scanned(gpointer dat) {
  struct fl_list *list = dat;

  if(!fl_own_list) {
    fl_own_list = list;
    // TODO: all files have to be hashed
  } else {
    // TODO: compare list with fl_own_list and determine which files need to be added/removed/rehashed
    fl_list_free(list);
  }

  // TODO: only save when something changed
  GError *err = NULL;
  if(!fl_save(fl_own_list, fl_own_list_file, &err)) {
    // this is a pretty fatal error...
    ui_msgf(TRUE, "Error saving file list: %s", err->message);
    g_error_free(err);
  } else
    ui_msgf(TRUE, "File list refresh finished.");
  return FALSE;
}


void fl_refresh(const char *dir) {
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

