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
#include <stdlib.h>
#include <stdarg.h>
#include <errno.h>
#include <limits.h>
#include <glib/gstdio.h>
#include <glib/gprintf.h>


// Log window "widget"


#if INTERFACE

#define LOGWIN_BUF 1023 // must be 2^x-1

struct ui_logwindow {
  int lastlog;
  int lastvis;
  FILE *file;
  char *file_path;
  struct stat file_st;
  char *buf[LOGWIN_BUF+1];
  gboolean updated;
};

#endif


// (Re-)opens the log file and checks for inode and file size changes.
static void ui_logwindow_checkfile(struct ui_logwindow *lw) {
  if(!lw->file_path)
    return;

  // stat
  gboolean restat = !lw->file;
  struct stat st;
  if(lw->file && stat(lw->file_path, &st) < 0) {
    g_warning("Unable to stat log file '%s': %s. Attempting to re-create it.", lw->file_path, g_strerror(errno));
    fclose(lw->file);
    lw->file = NULL;
    restat = TRUE;
  }

  // if we have the log open, compare inode & size
  if(lw->file && (lw->file_st.st_ino != st.st_ino || lw->file_st.st_size > st.st_size)) {
    fclose(lw->file);
    lw->file = NULL;
  }

  // if the log hadn't been opened or has been closed earlier, try to open it again
  if(!lw->file)
    lw->file = fopen(lw->file_path, "a");
  if(!lw->file)
    g_warning("Unable to open log file '%s' for writing: %s", lw->file_path, g_strerror(errno));

  // stat again if we need to
  if(lw->file && restat && stat(lw->file_path, &st) < 0) {
    g_warning("Unable to stat log file '%s': %s. Closing.", lw->file_path, g_strerror(errno));
    fclose(lw->file);
    lw->file = NULL;
  }

  memcpy(&lw->file_st, &st, sizeof(struct stat));
}


static void ui_logwindow_addline(struct ui_logwindow *lw, const char *msg, gboolean raw, gboolean nolog) {
  if(lw->lastlog == lw->lastvis)
    lw->lastvis = lw->lastlog + 1;
  lw->lastlog++;
  lw->updated = TRUE;

  time_t tm = time(NULL);
  char ts[50];
  strftime(ts, 10, "%H:%M:%S ", localtime(&tm));
  lw->buf[lw->lastlog & LOGWIN_BUF] = raw ? g_strdup(msg) : g_strconcat(ts, msg, NULL);

  ui_logwindow_checkfile(lw);
  if(!nolog && lw->file) {
    strftime(ts, 49, "[%F %H:%M:%S %Z] ", localtime(&tm));
    if(fprintf(lw->file, "%s%s\n", ts, msg) < 0 && !strstr(msg, "(LOGERR)"))
      g_warning("Error writing to log file: %s (LOGERR)", strerror(errno));
  }

  int next = (lw->lastlog + 1) & LOGWIN_BUF;
  if(lw->buf[next]) {
    g_free(lw->buf[next]);
    lw->buf[next] = NULL;
  }
}


static void ui_logwindow_load(struct ui_logwindow *lw, const char *fn, int num) {
  char **l = file_tail(fn, num);
  if(!l) {
    g_warning("Unable to tail log file '%s': %s", fn, g_strerror(errno));
    return;
  }
  int i, len = g_strv_length(l);
  char *m;
  for(i=0; i<len; i++) {
    if(!g_utf8_validate(l[i], -1, NULL))
      continue;
    // parse line: [yyyy-mm-dd hh:mm:ss TIMEZONE] <string>
    char *msg = strchr(l[i], ']');
    char *time = strchr(l[i], ' ');
    char *tmp = time ? strchr(time+1, ' ') : NULL;
    if(l[i][0] != '[' || !msg || !time || !tmp || tmp < time || msg[1] != ' ')
      continue;
    time++;
    *msg = 0;
    msg += 2;
    // if this is the first line, display a notice
    if(!i) {
      m = g_strdup_printf("-- Backlog starts on %s.", l[i]+1);
      ui_logwindow_addline(lw, m, FALSE, TRUE);
      g_free(m);
    }
    // display the line
    *tmp = 0;
    m = g_strdup_printf("%s %s", time, msg);
    ui_logwindow_addline(lw, m, TRUE, TRUE);
    g_free(m);
    *tmp = ' ';
    // if this is the last line, display another notice
    if(i == len-1) {
      m = g_strdup_printf("-- Backlog ends on %s", l[i]+1);
      ui_logwindow_addline(lw, m, FALSE, TRUE);
      g_free(m);
      ui_logwindow_addline(lw, "", FALSE, TRUE);
    }
  }
  g_strfreev(l);
}


struct ui_logwindow *ui_logwindow_create(const char *file, int load) {
  struct ui_logwindow *lw = g_new0(struct ui_logwindow, 1);
  if(file) {
    char *n = g_strconcat(file, ".log", NULL);
    lw->file_path = g_build_filename(conf_dir, "logs", n, NULL);
    g_free(n);

    if(load)
      ui_logwindow_load(lw, lw->file_path, load);
  }
  ui_logwindow_checkfile(lw);
  return lw;
}


void ui_logwindow_free(struct ui_logwindow *lw) {
  if(lw->file)
    fclose(lw->file);
  ui_logwindow_clear(lw);
  g_free(lw->file_path);
  g_free(lw);
}


void ui_logwindow_add(struct ui_logwindow *lw, const char *msg) {
  if(!msg[0]) {
    ui_logwindow_addline(lw, "", FALSE, FALSE);
    return;
  }

  char **lines = g_strsplit(msg, "\n", 0);

  // For chat messages and /me's, prefix every line with "<nick>" or "** nick"
  char *prefix = NULL;
  char *tmp;
  if( (**lines == '<' && (tmp = strchr(*lines, '>')) != NULL && *(++tmp) == ' ') || // <nick>
      (**lines == '*' && lines[0][1] == '*' && (tmp = strchr(*lines, ' ')) != NULL)) { // ** nick
    char old = tmp[1];
    tmp[1] = 0;
    prefix = g_strdup(*lines);
    tmp[1] = old;
  }

  // add the lines
  char **line;
  for(line=lines; *line; line++) {
    if(!prefix || lines == line)
      ui_logwindow_addline(lw, *line, FALSE, FALSE);
    else {
      tmp = g_strconcat(prefix, *line, NULL);
      ui_logwindow_addline(lw, tmp, FALSE, FALSE);
      g_free(tmp);
    }
  }
  g_free(prefix);
  g_strfreev(lines);
}


void ui_logwindow_clear(struct ui_logwindow *lw) {
  int i;
  for(i=0; i<=LOGWIN_BUF; i++) {
    g_free(lw->buf[i]);
    lw->buf[i] = NULL;
  }
  lw->lastlog = lw->lastvis = 0;
}


void ui_logwindow_scroll(struct ui_logwindow *lw, int i) {
  lw->lastvis += i;
  // lastvis may never be larger than the last entry present
  lw->lastvis = MIN(lw->lastvis, lw->lastlog);
  // lastvis may never be smaller than the last entry still in the log
  lw->lastvis = MAX(lw->lastlog - LOGWIN_BUF+1, lw->lastvis);
  // lastvis may never be smaller than one
  lw->lastvis = MAX(1, lw->lastvis);
}


// Draws a line between x and x+cols on row y (continuing on y-1 .. y-(rows+1) for
// multiple rows). Returns the actual number of rows written to.
static int ui_logwindow_drawline(struct ui_logwindow *lw, int y, int x, int nrows, int cols, char *str) {
  // Determine the indentation for multi-line rows. This is:
  // - Always after the time part (hh:mm:ss )
  // - For chat messages: after the nick (<nick> )
  // - For /me's: after the (** )
  int indent = 0;
  char *tmp = strchr(str, ' ');
  if(tmp)
    indent = tmp-str+1;
  if(tmp && tmp[1] == '<' && (tmp = strchr(tmp, '>')) != NULL)
    indent = tmp-str+2;
  else if(tmp && tmp[1] == '*' && tmp[2] == '*')
    indent += 3;

  // defines a mask over the string: <#0,#1), <#1,#2), ..
  static unsigned short rows[201];
  rows[0] = rows[1] = 0;
  int cur = 1, curcols = 0, i = 0;

  // loop through the characters and determine on which row to put them
  while(str[i]) {
    int width = gunichar_width(g_utf8_get_char(str+i));
    if(curcols+width >= (cur > 1 ? cols-indent : cols)) {
      // too many rows? don't display the rest
      if(cur >= 200)
        break;
      cur++;
      curcols = 0;
    }
    i = g_utf8_next_char(str+i) - str;
    rows[cur] = i;
    curcols += width;
    // too many bytes in the string? don't display the rest
    if(i > USHRT_MAX-10)
      break;
  }

  // print the rows
  for(i=cur-1; nrows>0 && i>=0; i--, nrows--, y--)
    mvaddnstr(y, i == 0 ? x : x+indent, str+rows[i], rows[i+1]-rows[i]);

  return cur;
}


// TODO: this function is called often and can be optimized.
// TODO: wrap on word boundary
void ui_logwindow_draw(struct ui_logwindow *lw, int y, int x, int rows, int cols) {
  int top = rows + y - 1;
  int cur = lw->lastvis;
  lw->updated = FALSE;

  while(top >= y) {
    char *str = lw->buf[cur & LOGWIN_BUF];
    if(!str)
      break;
    top -= ui_logwindow_drawline(lw, top, x, top-y+1, cols, str);
    cur = (cur-1) & LOGWIN_BUF;
  }
}


gboolean ui_logwindow_key(struct ui_logwindow *lw, guint64 key, int rows) {
  switch(key) {
  case INPT_KEY(KEY_NPAGE):
    ui_logwindow_scroll(lw, rows/2);
    return TRUE;
  case INPT_KEY(KEY_PPAGE):
    ui_logwindow_scroll(lw, -rows/2);
    return TRUE;
  }
  return FALSE;
}






// Command history
// We only have one command history, so the struct and its instance is local to
// this file, and the functions work with this instead of accepting an instance
// as argument. The ui_textinput functions also access the struct and static
// functions, but these don't need to be public - since ui_textinput is defined
// below.

#define CMDHIST_BUF 511 // must be 2^x-1
#define CMDHIST_MAXCMD 2000


struct ui_cmdhist {
  char *buf[CMDHIST_BUF+1]; // circular buffer
  char *fn;
  int last;
  gboolean ismod;
};

// we only have one command history, so this can be a global
static struct ui_cmdhist *cmdhist;


static void ui_cmdhist_add(const char *str) {
  int cur = cmdhist->last & CMDHIST_BUF;
  // ignore empty lines, or lines that are the same as the previous one
  if(!str || !str[0] || (cmdhist->buf[cur] && 0 == strcmp(str, cmdhist->buf[cur])))
    return;

  cmdhist->last++;
  cur = cmdhist->last & CMDHIST_BUF;
  if(cmdhist->buf[cur]) {
    g_free(cmdhist->buf[cur]);
    cmdhist->buf[cur] = NULL;
  }

  // truncate the string if it is longer than CMDHIST_MAXCMD bytes, making sure
  // to not truncate within a UTF-8 sequence
  int len = 0;
  while(len < CMDHIST_MAXCMD-10 && str[len])
    len = g_utf8_next_char(str+len) - str;
  cmdhist->buf[cur] = g_strndup(str, len);
  cmdhist->ismod = TRUE;
}


void ui_cmdhist_init(const char *file) {
  static char buf[CMDHIST_MAXCMD+2]; // + \n and \0
  cmdhist = g_new0(struct ui_cmdhist, 1);

  cmdhist->fn = g_build_filename(conf_dir, file, NULL);
  FILE *f = fopen(cmdhist->fn, "r");
  if(f) {
    while(fgets(buf, CMDHIST_MAXCMD+2, f)) {
      int len = strlen(buf);
      if(len > 0 && buf[len-1] == '\n')
        buf[len-1] = 0;

      if(g_utf8_validate(buf, -1, NULL))
        ui_cmdhist_add(buf);
    }
  }
}


// searches the history either backward or forward for the string q. The line 'start' is also counted.
// (only used by ui_textinput below, so can be static)
static int ui_cmdhist_search(gboolean backward, const char *q, int start) {
  int i;
  for(i=start; cmdhist->buf[i&CMDHIST_BUF] && (backward ? (i>=MAX(1, cmdhist->last-CMDHIST_BUF)) : (i<=cmdhist->last)); backward ? i-- : i++) {
    if(g_str_has_prefix(cmdhist->buf[i & CMDHIST_BUF], q))
      return i;
  }
  return -1;
}


static void ui_cmdhist_save() {
  if(!cmdhist->ismod)
    return;
  cmdhist->ismod = FALSE;

  FILE *f = fopen(cmdhist->fn, "w");
  if(!f) {
    g_warning("Unable to open history file '%s' for writing: %s", cmdhist->fn, g_strerror(errno));
    return;
  }

  int i;
  for(i=0; i<=CMDHIST_BUF; i++) {
    char *l = cmdhist->buf[(cmdhist->last+1+i)&CMDHIST_BUF];
    if(l) {
      if(fputs(l, f) < 0 || fputc('\n', f) < 0)
        g_warning("Error writing to history file '%s': %s", cmdhist->fn, strerror(errno));
    }
  }
  if(fclose(f) < 0)
    g_warning("Error writing to history file '%s': %s", cmdhist->fn, strerror(errno));
}


void ui_cmdhist_close() {
  int i;
  ui_cmdhist_save();
  for(i=0; i<=CMDHIST_BUF; i++)
    if(cmdhist->buf[i])
      g_free(cmdhist->buf[i]);
  g_free(cmdhist->fn);
  g_free(cmdhist);
}




// Text input "widget"

struct ui_textinput {
  int pos; // position of the cursor, in number of characters
  GString *str;
  gboolean usehist;
  int s_pos;
  char *s_q;
  gboolean s_top;
  void (*complete)(char *, char **);
  char *c_q, *c_last, **c_sug;
  int c_cur;
};



struct ui_textinput *ui_textinput_create(gboolean usehist, void (*complete)(char *, char **)) {
  struct ui_textinput *ti = g_new0(struct ui_textinput, 1);
  ti->str = g_string_new("");
  ti->usehist = usehist;
  ti->s_pos = -1;
  ti->complete = complete;
  return ti;
}


static void ui_textinput_complete_reset(struct ui_textinput *ti) {
  if(ti->complete) {
    g_free(ti->c_q);
    g_free(ti->c_last);
    g_strfreev(ti->c_sug);
    ti->c_q = ti->c_last = NULL;
    ti->c_sug = NULL;
  }
}


static void ui_textinput_complete(struct ui_textinput *ti) {
  if(!ti->complete)
    return;
  if(!ti->c_q) {
    ti->c_q = ui_textinput_get(ti);
    char *sep = g_utf8_offset_to_pointer(ti->c_q, ti->pos);
    ti->c_last = g_strdup(sep);
    *(sep) = 0;
    ti->c_cur = -1;
    ti->c_sug = g_new0(char *, 25);
    ti->complete(ti->c_q, ti->c_sug);
  }
  if(!ti->c_sug[++ti->c_cur])
    ti->c_cur = -1;
  char *first = ti->c_cur < 0 ? ti->c_q : ti->c_sug[ti->c_cur];
  char *str = g_strconcat(first, ti->c_last, NULL);
  ui_textinput_set(ti, str);
  ti->pos = g_utf8_strlen(first, -1);
  g_free(str);
  if(!g_strv_length(ti->c_sug))
    ui_beep = TRUE;
  // If there is only one suggestion: finalize this auto-completion and reset
  // state. This may be slightly counter-intuitive, but makes auto-completing
  // paths a lot less annoying.
  if(g_strv_length(ti->c_sug) <= 1)
    ui_textinput_complete_reset(ti);
}


void ui_textinput_free(struct ui_textinput *ti) {
  ui_textinput_complete_reset(ti);
  g_string_free(ti->str, TRUE);
  if(ti->s_q)
    g_free(ti->s_q);
  g_free(ti);
}


void ui_textinput_set(struct ui_textinput *ti, const char *str) {
  g_string_assign(ti->str, str);
  ti->pos = g_utf8_strlen(ti->str->str, -1);
}


char *ui_textinput_get(struct ui_textinput *ti) {
  return g_strdup(ti->str->str);
}



char *ui_textinput_reset(struct ui_textinput *ti) {
  char *str = ui_textinput_get(ti);
  ui_textinput_set(ti, "");
  if(ti->usehist) {
    // as a special case, don't allow /password to be logged. /set password is
    // okay, since it will be stored anyway.
    if(!strstr(str, "/password "))
      ui_cmdhist_add(str);
    if(ti->s_q)
      g_free(ti->s_q);
    ti->s_q = NULL;
    ti->s_pos = -1;
  }
  return str;
}


// must be drawn last, to keep the cursor position correct
// also not the most efficient function ever, but probably fast enough.
void ui_textinput_draw(struct ui_textinput *ti, int y, int x, int col) {
  //       |              |
  // "Some random string etc etc"
  //       f         #    l
  // f = function(#, strwidth(upto_#), wincols)
  // if(strwidth(upto_#) < wincols*0.85)
  //   f = 0
  // else
  //   f = strwidth(upto_#) - wincols*0.85
  int i;

  // calculate f (in number of columns)
  int width = 0;
  char *str = ti->str->str;
  for(i=0; i<=ti->pos && *str; i++) {
    width += gunichar_width(g_utf8_get_char(str));
    str = g_utf8_next_char(str);
  }
  int f = width - (col*85)/100;
  if(f < 0)
    f = 0;

  // now print it on the screen, starting from column f in the string and
  // stopping when we're out of screen columns
  mvhline(y, x, ' ', col);
  move(y, x);
  int pos = 0;
  str = ti->str->str;
  i = 0;
  while(*str) {
    char *ostr = str;
    str = g_utf8_next_char(str);
    int l = gunichar_width(g_utf8_get_char(ostr));
    f -= l;
    if(f <= -col)
      break;
    if(f < 0) {
      addnstr(ostr, str-ostr);
      if(i < ti->pos)
        pos += l;
    }
    i++;
  }
  move(y, x+pos);
  curs_set(1);
}


static void ui_textinput_search(struct ui_textinput *ti, gboolean backwards) {
  int start;
  if(ti->s_pos < 0) {
    if(!backwards) {
      ui_beep = TRUE;
      return;
    }
    ti->s_q = ui_textinput_get(ti);
    start = cmdhist->last;
  } else
    start = ti->s_pos+(backwards ? -1 : 1);
  int pos = ui_cmdhist_search(backwards, ti->s_q, start);
  if(pos >= 0) {
    ti->s_pos = pos;
    ti->s_top = FALSE;
    ui_textinput_set(ti, cmdhist->buf[pos & CMDHIST_BUF]);
  } else if(backwards)
    ui_beep = TRUE;
  else {
    ti->s_pos = -1;
    ui_textinput_set(ti, ti->s_q);
    g_free(ti->s_q);
    ti->s_q = NULL;
  }
}


gboolean ui_textinput_key(struct ui_textinput *ti, guint64 key, char **str) {
  int chars = g_utf8_strlen(ti->str->str, -1);
  gboolean completereset = TRUE;
  switch(key) {
  case INPT_KEY(KEY_LEFT):
    if(ti->pos > 0) ti->pos--;
    break;
  case INPT_KEY(KEY_RIGHT):
    if(ti->pos < chars) ti->pos++;
    break;
  case INPT_KEY(KEY_END):
    ti->pos = chars;
    break;
  case INPT_KEY(KEY_HOME):
    ti->pos = 0;
    break;
  case INPT_KEY(KEY_BACKSPACE):
    if(ti->pos > 0) {
      char *pos = g_utf8_offset_to_pointer(ti->str->str, ti->pos-1);
      g_string_erase(ti->str, pos-ti->str->str, g_utf8_next_char(pos)-pos);
      ti->pos--;
    }
    break;
  case INPT_KEY(KEY_DC):
    if(ti->pos < chars) {
      char *pos = g_utf8_offset_to_pointer(ti->str->str, ti->pos);
      g_string_erase(ti->str, pos-ti->str->str, g_utf8_next_char(pos)-pos);
    }
    break;
  case INPT_KEY(KEY_UP):
  case INPT_KEY(KEY_DOWN):
    if(ti->usehist)
      ui_textinput_search(ti, key == INPT_KEY(KEY_UP));
    else
      return FALSE;
    break;
  case INPT_CTRL('i'): // tab
    ui_textinput_complete(ti);
    completereset = FALSE;
    break;
  case INPT_CTRL('j'): // newline
    *str = ui_textinput_reset(ti);
    break;
  default:
    if(INPT_TYPE(key) == 1) { // char
      g_string_insert_unichar(ti->str, g_utf8_offset_to_pointer(ti->str->str, ti->pos)-ti->str->str, INPT_CODE(key));
      ti->pos++;
    } else
      return FALSE;
  }
  if(completereset)
    ui_textinput_complete_reset(ti);
  return TRUE;
}






// Generic listing "widget".
// This widget allows easy listing, selecting and paging of (dynamic) GSequence
// lists.  The list is managed by the user, but the widget does need to be
// notified of insertions and deletions.

#if INTERFACE

struct ui_listing {
  GSequence *list;
  GSequenceIter *sel;
  GSequenceIter *top;
  gboolean topisbegin;
  gboolean selisbegin;
}


// update top/sel in case they used to be the start of the list but aren't anymore
#define ui_listing_inserted(ul) do {\
    if((ul)->topisbegin != g_sequence_iter_is_begin((ul)->top))\
      (ul)->top = g_sequence_get_begin_iter((ul)->list);\
    if((ul)->selisbegin != g_sequence_iter_is_begin((ul)->sel))\
      (ul)->sel = g_sequence_get_begin_iter((ul)->list);\
  } while(0)


// called after the order of the list has changed
// update sel in case it used to be the start of the list but isn't anymore
#define ui_listing_sorted(ul) do {\
    if((ul)->selisbegin != g_sequence_iter_is_begin((ul)->sel))\
      (ul)->sel = g_sequence_get_begin_iter((ul)->list);\
  } while(0)


#define ui_listing_updateisbegin(ul) do {\
    (ul)->topisbegin = g_sequence_iter_is_begin((ul)->top);\
    (ul)->selisbegin = g_sequence_iter_is_begin((ul)->sel);\
  } while(0)


// update top/sel in case one of them is removed.
// call this before using g_sequence_remove()
#define ui_listing_remove(ul, iter) do {\
    if((ul)->top == (iter))\
      (ul)->top = g_sequence_iter_prev(iter);\
    if((ul)->top == (iter))\
      (ul)->top = g_sequence_iter_next(iter);\
    if((ul)->sel == (iter)) {\
      (ul)->sel = g_sequence_iter_next(iter);\
      if(g_sequence_iter_is_end((ul)->sel))\
        (ul)->sel = g_sequence_iter_prev(iter);\
      if((ul)->sel == (iter))\
        (ul)->sel = g_sequence_get_end_iter((ul)->list);\
    }\
    ui_listing_updateisbegin(ul);\
  } while(0)


// does not free the GSequence (we don't control the list, after all)
#define ui_listing_free(ul) g_slice_free(struct ui_listing, ul)


#endif


struct ui_listing *ui_listing_create(GSequence *list) {
  struct ui_listing *ul = g_slice_new0(struct ui_listing);
  ul->list = list;
  ul->sel = ul->top = g_sequence_get_begin_iter(list);
  ul->topisbegin = ul->selisbegin = TRUE;
  return ul;
}


gboolean ui_listing_key(struct ui_listing *ul, guint64 key, int page) {
  switch(key) {
  case INPT_KEY(KEY_NPAGE): // page down
    ul->sel = g_sequence_iter_move(ul->sel, page);
    if(g_sequence_iter_is_end(ul->sel))
      ul->sel = g_sequence_iter_prev(ul->sel);
    break;
  case INPT_KEY(KEY_PPAGE): // page up
    ul->sel = g_sequence_iter_move(ul->sel, -page);
    // workaround for https://bugzilla.gnome.org/show_bug.cgi?id=648313
    if(g_sequence_iter_is_end(ul->sel))
      ul->sel = g_sequence_get_begin_iter(ul->list);
    break;
  case INPT_KEY(KEY_DOWN): // item down
  case INPT_CHAR('j'):
    ul->sel = g_sequence_iter_next(ul->sel);
    if(g_sequence_iter_is_end(ul->sel))
      ul->sel = g_sequence_iter_prev(ul->sel);
    break;
  case INPT_KEY(KEY_UP): // item up
  case INPT_CHAR('k'):
    ul->sel = g_sequence_iter_prev(ul->sel);
    break;
  case INPT_KEY(KEY_HOME): // home
    ul->sel = g_sequence_get_begin_iter(ul->list);
    break;
  case INPT_KEY(KEY_END): // end
    ul->sel = g_sequence_iter_prev(g_sequence_get_end_iter(ul->list));
    break;
  default:
    return FALSE;
  }
  ui_listing_updateisbegin(ul);
  return TRUE;
}


// Every item is assumed to occupy exactly one line.
// Returns the relative position of the current page (in percent)
int ui_listing_draw(struct ui_listing *ul, int top, int bottom, void (*cb)(struct ui_listing *, GSequenceIter *, int, void *), void *dat) {
  // get or update the top row to make sure sel is visible
  int height = 1 + bottom - top;
  int row_last = g_sequence_iter_get_position(g_sequence_get_end_iter(ul->list));
  int row_top = g_sequence_iter_get_position(ul->top);
  int row_sel = g_sequence_iter_get_position(ul->sel);
  // sel is before top? top = sel!
  if(row_top > row_sel)
    row_top = row_sel;
  // otherwise, is it out of the screen? top = sel - height
  else if(row_top <= row_sel-height)
    row_top = row_sel-height+1;
  // make sure there are no empty lines when len > height
  if(row_top && row_top+height > row_last)
    row_top = MAX(0, row_last-height);
  ul->top = g_sequence_get_iter_at_pos(ul->list, row_top);

  // draw
  GSequenceIter *n = ul->top;
  while(top <= bottom && !g_sequence_iter_is_end(n)) {
    cb(ul, n, top++, dat);
    n = g_sequence_iter_next(n);
  }

  ui_listing_updateisbegin(ul);
  return MIN(100, row_last ? (row_top+height)*100/row_last : 0);
}


