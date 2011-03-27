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
#include <stdarg.h>
#include <errno.h>
#include <unistd.h>
#include <fcntl.h>
#include <assert.h>
#include <glib/gstdio.h>
#include <glib/gprintf.h>

#define g_unichar_width(x) (g_unichar_iswide(x) ? 2 : g_unichar_iszerowidth(x) ? 0 : 1)


// Log window "widget"


#if INTERFACE
struct ui_logwindow;
#endif

#define LOGWIN_PAD 9
#define LOGWIN_BUF 1023 // must be 2^x-1

struct ui_logwindow {
  int lastlog;
  int lastvis;
  int file;
  char *buf[LOGWIN_BUF+1];
};


struct ui_logwindow *ui_logwindow_create(const char *file) {
  struct ui_logwindow *lw = g_new0(struct ui_logwindow, 1);
  if(file) {
    char *fn = g_build_filename(conf_dir, "logs", file, NULL);
    lw->file = g_open(fn, O_WRONLY | O_CREAT | O_APPEND, 0700);
    if(lw->file < 0)
      g_warning("Unable to open log file '%s' for writing: %s", fn, strerror(errno));
    g_free(fn);
  } else {
    lw->file = -1;
  }
  return lw;
}


void ui_logwindow_free(struct ui_logwindow *lw) {
  int i;
  if(lw->file >= 0)
    close(lw->file);
  for(i=0; i<LOGWIN_BUF; i++)
    if(lw->buf[i])
      g_free(lw->buf[i]);
  g_free(lw);
}


static void ui_logwindow_addline(struct ui_logwindow *lw, const char *msg) {
  if(lw->lastlog == lw->lastvis)
    lw->lastvis = lw->lastlog + 1;
  lw->lastlog++;

  // TODO: convert invalid characters being written to the buffer
  //  ui_logwindow_draw() really requires valid UTF-8
  assert(g_utf8_validate(msg, -1, NULL));

  GDateTime *dt = g_date_time_new_now_local();
  char *tmp = g_date_time_format(dt, "%H:%M:%S ");
  lw->buf[lw->lastlog & LOGWIN_BUF] = g_strconcat(tmp, msg, NULL);
  g_free(tmp);

  if(lw->file >= 0) {
    tmp = g_date_time_format(dt, "[%F %H:%M:%S %Z] ");
    // TODO: don't ignore errors
    write(lw->file, tmp, strlen(tmp));
    write(lw->file, msg, strlen(msg));
    write(lw->file, "\n", 1);
    g_free(tmp);
  }
  g_date_time_unref(dt);

  int next = (lw->lastlog + 1) & LOGWIN_BUF;
  if(lw->buf[next]) {
    g_free(lw->buf[next]);
    lw->buf[next] = NULL;
  }
}


void ui_logwindow_add(struct ui_logwindow *lw, const char *msg) {
  if(!msg[0]) {
    ui_logwindow_addline(lw, "");
    return;
  }
  char **lines = g_strsplit(msg, "\n", 0);
  char **line;
  for(line=lines; *line; line++)
    ui_logwindow_addline(lw, *line);
  g_strfreev(lines);
}


void ui_logwindow_printf(struct ui_logwindow *lw, const char *fmt, ...) {
  va_list va;
  va_start(va, fmt);
  char *str = g_strdup_vprintf(fmt, va);
  va_end(va);
  ui_logwindow_add(lw, str);
  g_free(str);
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


// TODO: this function is called often and can be optimized.
void ui_logwindow_draw(struct ui_logwindow *lw, int y, int x, int rows, int cols) {
  int top = rows + y - 1;
  int cur = lw->lastvis;

  // assumes we can't have more than 100 lines per log item, and that each log
  // item does not exceed 64k characters
  unsigned short lines[101];
  lines[0] = 0;

  while(top >= y) {
    char *tmp = lw->buf[cur & LOGWIN_BUF];
    if(!tmp)
      break;
    gunichar *str = g_utf8_to_ucs4_fast(tmp, -1, NULL);
    int curline = 1, curlinecols = 0, i = 0;
    lines[1] = 0;

    // loop through the characters and determine on which line to put them
    while(str[i]) {
      int width = g_unichar_width(str[i]);
      if(curlinecols+width >= (curline > 1 ? cols-LOGWIN_PAD : cols)) {
        assert(++curline <= 100);
        curlinecols = 0;
      }
      lines[curline] = i+1;
      curlinecols += width;
      i++;
    }

    // print the lines
    for(i=curline-1; top>=y && i>=0; i--, top--) {
      tmp = g_ucs4_to_utf8(str+lines[i], lines[i+1]-lines[i], NULL, NULL, NULL);
      mvaddstr(top, i == 0 ? x : x+9, tmp);
      g_free(tmp);
    }

    g_free(str);
    cur = (cur-1) & LOGWIN_BUF;
  }
}




// Text input "widget"
// TODO: history and tab completion

#if INTERFACE

struct ui_textinput {
  int pos;
  int len; // number of characters in *str
  gunichar *str; // 0-terminated string
};

#define ui_textinput_get(ti) g_ucs4_to_utf8((ti)->str, -1, NULL, NULL, NULL)

#endif


struct ui_textinput *ui_textinput_create() {
  struct ui_textinput *ti = g_new0(struct ui_textinput, 1);
  ti->str = g_new0(gunichar, 1);
  ti->len = 0;
  return ti;
}


void ui_textinput_free(struct ui_textinput *ti) {
  g_free(ti->str);
  g_free(ti);
}


void ui_textinput_set(struct ui_textinput *ti, char *str) {
  g_free(ti->str);
  glong written;
  ti->str = g_utf8_to_ucs4(str, -1, NULL, &written, NULL);
  assert(ti->str != NULL);
  ti->pos = ti->len = written;
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
  gunichar *str = ti->str;
  for(i=0; i<=ti->pos && str[i]; i++)
    width += g_unichar_width(str[i]);
  int f = width - (col*85)/100;
  if(f < 0)
    f = 0;

  // now print it on the screen, starting from column f in the string and
  // stopping when we're out of screen columns
  mvhline(y, x, ' ', col);
  move(y, x);
  int pos = 0;
  char enc[7];
  i = 0;
  while(*str) {
    f -= g_unichar_width(*str);
    if(f <= -col)
      break;
    if(f < 0) {
      enc[g_unichar_to_utf8(*str, enc)] = 0;
      addstr(enc);
      if(i < ti->pos)
        pos += g_unichar_width(*str);
    }
    i++;
    str++;
  }
  move(y, x+pos);
  curs_set(1);
}


gboolean ui_textinput_key(struct ui_textinput *ti, struct input_key *key) {
  if(key->type == INPT_KEY) {
    switch(key->code) {
      case KEY_LEFT:  if(ti->pos > 0) ti->pos--; break;
      case KEY_RIGHT: if(ti->pos < ti->len) ti->pos++; break;
      case KEY_END:   ti->pos = ti->len; break;
      case KEY_HOME:  ti->pos = 0; break;
      case KEY_BACKSPACE:
        if(ti->pos > 0) {
          memmove(ti->str + ti->pos - 1, ti->str + ti->pos, (ti->len - ti->pos + 1) * sizeof(gunichar));
          ti->pos--;
          ti->len--;
        }
        break;
      case KEY_DC:
        if(ti->pos < ti->len) {
          memmove(ti->str + ti->pos, ti->str + ti->pos + 1, (ti->len - ti->pos) * sizeof(gunichar));
          ti->len--;
        }
        break;
      default:
        return FALSE;
    }
  } else if(key->type == INPT_CHAR) {
    // increase string size by one (not *very* efficient...)
    ti->len++;
    ti->str = g_renew(gunichar, ti->str, ti->len+1);
    // insert character
    memmove(ti->str + ti->pos + 1, ti->str + ti->pos, (ti->len - ti->pos) * sizeof(gunichar));
    ti->str[ti->pos] = key->code;
    ti->pos++;
  } else
    return FALSE;
  return TRUE;
}

