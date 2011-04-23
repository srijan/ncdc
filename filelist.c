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
#include <bzlib.h>


/* functions:

void fl_scan_thread(dir);              // to-be-run in a separate thread
void fl_scan_finish(struct fl_list *); // called from fl_scan_thread(), in the main thread

void fl_hash_thread(file);       // to-be-run in a separate thread
void fl_hash_finish(tth_hash *); // called from fl_hash_thread(), in the main thread

struct fl_list *fl_load(file); // loads a filelist (main thread?)
void fl_init();   // loads own filelist (cross-checks with hashdata and fills out lastmod)
void fl_update(); // updates own list in the background (uses fl_scan and fl_hash)

void fl_list_free(list);

*/

#if INTERFACE

// file list

struct fl_list {
  char *name; // root = NULL
  struct fl_list *parent;
  GSequence *sub;
  guint64 size;   // including sub-items
  time_t lastmod; // only used for own list. for dirs: lastmod = min(sub->lastmod)
  gboolean isfile : 1;
  gboolean hastth : 1; // "iscomplete" for directories
  char tth[24];
};

#endif


// only frees the given item and its childs. leaves the parent(s) untouched
void fl_list_free(gpointer dat) {
  struct fl_list *fl = dat;
  if(!fl)
    return;
  g_free(fl->name);
  if(fl->sub)
    g_sequence_free(fl->sub);
  g_free(fl);
}





// Read filelist from an xml file

struct fl_load_context {
  char *file;
  BZFILE *fh_bz;
  FILE *fh_f;
  GError **err;
  gboolean stream_end;
};


static int fl_load_input(void *context, char *buf, int len) {
  struct fl_load_context *xc = context;
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
  struct fl_load_context *xc = context;
  int bzerr;
  BZ2_bzReadClose(&bzerr, xc->fh_bz);
  fclose(xc->fh_f);
  return 0;
}


static void fl_load_error(void *arg, const char *msg, xmlParserSeverities severity, xmlTextReaderLocatorPtr locator) {
  struct fl_load_context *xc = arg;
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
      if(attr[1] && (strcmp(attr[1], "0") != 0 || strcmp(attr[1], "1") != 0)) {
        free(attr[0]);
        return -1;
      }
      tmp = g_new0(struct fl_list, 1);
      tmp->name = g_strdup(attr[0]);
      tmp->isfile = FALSE;
      tmp->hastth = !attr[1] || attr[1][0] == '0';
      tmp->sub = g_sequence_new(fl_list_free);
      tmp->parent = *newdir ? *cur : (*cur)->parent;
      g_sequence_append(tmp->parent->sub, tmp); // TODO: sorted
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
      tmp->name = g_strdup(attr[0]);
      tmp->isfile = TRUE;
      tmp->size = g_ascii_strtoull(attr[1], NULL, 10);
      tmp->hastth = TRUE;
      base32_decode(attr[2], tmp->tth);
      tmp->parent = *newdir ? *cur : (*cur)->parent;
      g_sequence_append(tmp->parent->sub, tmp); // TODO: sorted
      *newdir = FALSE;
      *cur = tmp;
      for(tmp=tmp->parent; tmp; tmp=tmp->parent)
        tmp->size += (*cur)->size;
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

  struct fl_load_context xc;
  xc.stream_end = FALSE;
  xc.err = err;

  if(isbz2) {
    xc.fh_f = fopen(file, "r");
    if(!xc.fh_f) {
      g_set_error_literal(err, 0, 0, g_strerror(errno));
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

  return root;
}


