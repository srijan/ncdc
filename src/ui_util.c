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





// colors

#if INTERFACE

#define COLOR_DEFAULT (-1)

//  name            default fg       default bg       default attr
#define UI_COLORS \
  C(list_default,   COLOR_DEFAULT,   COLOR_DEFAULT,   0)\
  C(list_header,    COLOR_DEFAULT,   COLOR_DEFAULT,   A_BOLD)\
  C(list_select,    COLOR_DEFAULT,   COLOR_DEFAULT,   A_BOLD)\
  C(log_default,    COLOR_DEFAULT,   COLOR_DEFAULT,   0)\
  C(log_highlight,  COLOR_YELLOW,    COLOR_DEFAULT,   A_BOLD)\
  C(log_join,       COLOR_CYAN,      COLOR_DEFAULT,   A_BOLD)\
  C(log_nick,       COLOR_DEFAULT,   COLOR_DEFAULT,   0)\
  C(log_ownnick,    COLOR_DEFAULT,   COLOR_DEFAULT,   A_BOLD)\
  C(log_quit,       COLOR_CYAN,      COLOR_DEFAULT,   0)\
  C(log_time,       COLOR_BLACK,     COLOR_DEFAULT,   A_BOLD)\
  C(separator,      COLOR_DEFAULT,   COLOR_DEFAULT,   A_REVERSE)\
  C(tabprio_high,   COLOR_MAGENTA,   COLOR_DEFAULT,   A_BOLD)\
  C(tabprio_low,    COLOR_BLACK,     COLOR_DEFAULT,   A_BOLD)\
  C(tabprio_med,    COLOR_CYAN,      COLOR_DEFAULT,   A_BOLD)\
  C(title,          COLOR_DEFAULT,   COLOR_DEFAULT,   A_REVERSE)


enum ui_coltype {
#define C(n, fg, bg, attr) UIC_##n,
  UI_COLORS
#undef C
  UIC_NONE
};


struct ui_color {
  char name[16];
  short fg, bg, d_fg, d_bg;
  int x, d_x, a;
};

struct ui_attr {
  char name[11];
  gboolean color : 1;
  int attr;
}

#define UIC(n) (ui_colors[UIC_##n].a)

#endif // INTERFACE


struct ui_color ui_colors[] = {
#define C(n, fg, bg, attr) { G_STRINGIFY(n), fg, bg, fg, bg, attr, attr, 0 },
  UI_COLORS
#undef C
  { "" }
};


struct ui_attr ui_attr_names[] = {
  { "black",     TRUE,  COLOR_BLACK   },
  { "blue",      TRUE,  COLOR_BLUE    },
  { "bold",      FALSE, A_BOLD        },
  { "cyan",      TRUE,  COLOR_CYAN    },
  { "default",   TRUE,  COLOR_DEFAULT },
  { "green",     TRUE,  COLOR_GREEN   },
  { "magenta",   TRUE,  COLOR_MAGENTA },
  { "red",       TRUE,  COLOR_RED     },
  { "reverse",   FALSE, A_REVERSE     },
  { "underline", FALSE, A_UNDERLINE   },
  { "white",     TRUE,  COLOR_WHITE   },
  { "yellow",    TRUE,  COLOR_YELLOW  },
  { "" }
};


struct ui_attr *ui_attr_by_name(const char *n) {
  struct ui_attr *a = ui_attr_names;
  for(; *a->name; a++)
    if(strcmp(a->name, n) == 0)
      return a;
  return NULL;
}


char *ui_name_by_attr(int n) {
  struct ui_attr *a = ui_attr_names;
  for(; *a->name; a++)
    if(a->attr == n)
      return a->name;
  return NULL;
}


struct ui_color *ui_color_by_name(const char *n) {
  struct ui_color *c = ui_colors;
  for(; *c->name; c++)
    if(strcmp(c->name, n) == 0)
      return c;
  return NULL;
}


gboolean ui_color_str_parse(const char *str, short *fg, short *bg, int *x, GError **err) {
  int state = 0; // 0 = no fg, 1 = no bg, 2 = both
  short f = COLOR_DEFAULT, b = COLOR_DEFAULT;
  int a = 0;
  char **args = g_strsplit(str, ",", 0);
  char **arg = args;
  for(; arg && *arg; arg++) {
    g_strstrip(*arg);
    if(!**arg)
      continue;
    struct ui_attr *attr = ui_attr_by_name(*arg);
    if(!attr) {
      g_set_error(err, 1, 0, "Unknown color or attribute: %s", *arg);
      g_strfreev(args);
      return FALSE;
    }
    if(!attr->color)
      a |= attr->attr;
    else if(!state) {
      f = attr->attr;
      state++;
    } else if(state == 1) {
      b = attr->attr;
      state++;
    } else {
      g_set_error(err, 1, 0, "Don't know what to do with a third color: %s", *arg);
      g_strfreev(args);
      return FALSE;
    }
  }
  g_strfreev(args);
  if(fg) *fg = f;
  if(bg) *bg = b;
  if(x)  *x  = a;
  return TRUE;
}


char *ui_color_str_gen(int fd, int bg, int x) {
  static char buf[100]; // must be smaller than (max_color_name * 2) + (max_attr_name * 3) + 6
  strcpy(buf, ui_name_by_attr(fd));
  if(bg != COLOR_DEFAULT) {
    strcat(buf, ",");
    strcat(buf, ui_name_by_attr(bg));
  }
  struct ui_attr *attr = ui_attr_names;
  for(; attr->name[0]; attr++)
    if(!attr->color && x & attr->attr) {
      strcat(buf, ",");
      strcat(buf, attr->name);
    }
  return buf;
}


// TODO: re-use color pairs when we have too many (>64) color groups
void ui_colors_update() {
  char confname[50] = "color_";
  int pair = 0;
  struct ui_color *c = ui_colors;
  for(; *c->name; c++) {
    strcpy(confname+6, c->name);
    char *conf = g_key_file_get_string(conf_file, "color", confname, NULL);
    if(!conf || !ui_color_str_parse(conf, &c->fg, &c->bg, &c->x, NULL)) {
      c->fg = c->d_fg;
      c->bg = c->d_bg;
      c->x = c->d_x;
    }
    g_free(conf);
    init_pair(++pair, c->fg, c->bg);
    c->a = c->x | COLOR_PAIR(pair);
  }
}


void ui_colors_init() {
  if(!has_colors())
    return;

  start_color();
  use_default_colors();

  ui_colors_update();
}





// Log window "widget"


/* Log format of some special messages:
 *   Chat message:  "<nick> msg"
 *   Chat /me:      "** nick msg"
 *   User joined    "--> nick has joined." (may be internationalized,
 *   User quit      "--< nick has quit."    don't depend on the actual message)
 * Anything else is a system / help message.
 */

#if INTERFACE

#define LOGWIN_BUF 1023 // must be 2^x-1

struct ui_logwindow {
  int lastlog;
  int lastvis;
  struct logfile *logfile;
  char *buf[LOGWIN_BUF+1];
  gboolean updated;
  int (*checkchat)(void *, char *, char *);
  void *handle;
};

#endif


void ui_logwindow_addline(struct ui_logwindow *lw, const char *msg, gboolean raw, gboolean nolog) {
  if(lw->lastlog == lw->lastvis)
    lw->lastvis = lw->lastlog + 1;
  lw->lastlog++;
  lw->updated = TRUE;

  time_t tm = time(NULL);
  char ts[50];
  strftime(ts, 10, "%H:%M:%S ", localtime(&tm));
  lw->buf[lw->lastlog & LOGWIN_BUF] = raw ? g_strdup(msg) : g_strconcat(ts, msg, NULL);

  if(!nolog && lw->logfile)
    logfile_add(lw->logfile, msg);

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
    lw->logfile = logfile_create(file);

    if(load)
      ui_logwindow_load(lw, lw->logfile->path, load);
  }
  return lw;
}


void ui_logwindow_free(struct ui_logwindow *lw) {
  logfile_free(lw->logfile);
  ui_logwindow_clear(lw);
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
      (**lines == '*' && lines[0][1] == '*' && lines[0][2] == ' ' && (tmp = strchr(*lines+3, ' ')) != NULL)) { // ** nick
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


// Calculate the wrapping points in a line. Storing the mask in *rows, the row
// where the indent is reset in *ind_row, and returning the number of rows.
static int ui_logwindow_calc_wrap(char *str, int cols, int indent, int *rows, int *ind_row) {
  rows[0] = rows[1] = 0;
  *ind_row = 0;
  int cur = 1, curcols = 0, i = 0;

  // Appends an entity that will not be wrapped (i.e. a single character or a
  // word that isn't too long). Does a 'break' if there are too many lines.
#define append(w, b, ind) \
  int t_w = w;\
  if(curcols+t_w > cols) {\
    if(++cur >= 200)\
      break;\
    if(ind && !*ind_row) {\
      *ind_row = cur-1;\
      indent = 0;\
    }\
    curcols = indent;\
  }\
  if(!(cur > 1 && j == i && curcols == indent))\
    curcols += t_w;\
  i += b;\
  rows[cur] = i;

  while(str[i] && cur < 200) {
    // Determine the width of the current word
    int j = i;
    int width = 0;
    for(; str[j] && str[j] != ' '; j = g_utf8_next_char(str+j)-str)
      width += gunichar_width(g_utf8_get_char(str+j));

    // Special-case the space
    if(j == i) {
      append(1,1, FALSE);

    // If the word still fits on the current line or is smaller than cols*3/4
    // and cols-indent, then consider it as a single entity
    } else if(curcols+width <= cols || width < MIN(cols*3/4, cols-indent)) {
      append(width, j-i, FALSE);

    // Otherwise, wrap on character-boundary and ignore indent
    } else {
      char *tmp = str+i;
      for(; *tmp && *tmp != ' '; tmp = g_utf8_next_char(tmp)) {
        append(gunichar_width(g_utf8_get_char(tmp)), g_utf8_next_char(tmp)-tmp, TRUE);
      }
    }
  }

#undef append
  if(!*ind_row)
    *ind_row = cur;
  return cur-1;
}


// Determines the colors each part of a log line should have. Returns the
// highest index to the attr array.
static int ui_logwindow_calc_color(struct ui_logwindow *lw, char *str, int *sep, int *attr) {
  sep[0] = 0;
  int mask = 0;

  // add a mask
#define addm(from, to, a)\
  int t_f = from;\
  if(sep[mask] != t_f) {\
    sep[mask+1] = t_f;\
    attr[mask] = UIC(log_default);\
    mask++;\
  }\
  sep[mask] = t_f;\
  sep[mask+1] = to;\
  attr[mask] = a;\
  mask++;

  // time
  char *msg = strchr(str, ' ');
  if(msg && msg-str != 8) // Make sure it's not "Day changed to ..", which doesn't have the time prefix
    msg = NULL;
  if(msg) {
    addm(0, msg-str, UIC(log_time));
    msg++;
  }

  // chat messages (<nick> and ** nick)
  char *tmp;
  if(msg && (
      (msg[0] == '<' && (tmp = strchr(msg, '>')) != NULL && tmp[1] == ' ') || // <nick>
      (msg[0] == '*' && msg[1] == '*' && msg[2] == ' ' && (tmp = strchr(msg+3, ' ')) != NULL))) { // ** nick
    int nickstart = (msg-str) + (msg[0] == '<' ? 1 : 3);
    int nickend = tmp-str;
    // check for a highlight or whether it is our own nick
    char old = tmp[0];
    tmp[0] = 0;
    int r = lw->checkchat ? lw->checkchat(lw->handle, str+nickstart, str+nickend+1) : 0;
    tmp[0] = old;
    // and use the correct color
    addm(nickstart, nickend, r == 2 ? UIC(log_ownnick) : r == 1 ? UIC(log_highlight) : UIC(log_nick));
  }

  // join/quits (--> and --<)
  if(msg && msg[0] == '-' && msg[1] == '-' && (msg[2] == '>' || msg[2] == '<')) {
    addm(msg-str, strlen(str), msg[2] == '>' ? UIC(log_join) : UIC(log_quit));
  }

#undef addm
  // make sure the last mask is correct and return
  if(sep[mask+1] != strlen(str)) {
    sep[mask+1] = strlen(str);
    attr[mask] = UIC(log_default);
  }
  return mask;
}


// Draws a line between x and x+cols on row y (continuing on y-1 .. y-(rows+1) for
// multiple rows). Returns the actual number of rows written to.
static int ui_logwindow_drawline(struct ui_logwindow *lw, int y, int x, int nrows, int cols, char *str) {
  g_return_val_if_fail(nrows > 0, 1);

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

  // Convert indent from bytes to columns
  if(indent && indent <= strlen(str)) {
    int i = indent;
    char old = str[i];
    str[i] = 0;
    indent = str_columns(str);
    str[i] = old;
  }

  // Determine the wrapping boundaries.
  // Defines a mask over the string: <#0,#1), <#1,#2), ..
  static int rows[201];
  int ind_row;
  int rmask = ui_logwindow_calc_wrap(str, cols, indent, rows, &ind_row);

  // Determine the colors to give each part
  static int colors_sep[10]; // Mask, similar to the rows array
  static int colors[10];     // Color attribute for each mask
  int cmask = ui_logwindow_calc_color(lw, str, colors_sep, colors);

  // print the rows
  int r = 0, c = 0, lr = 0;
  if(rmask-r < nrows)
    move(y - rmask + r, r == 0 || r >= ind_row ? x : x+indent);
  while(r <= rmask && c <= cmask) {
    int rend = rows[r+1];
    int cend = colors_sep[c+1];
    int rstart = rows[r];
    int cstart = colors_sep[c];
    int start = MAX(rstart, cstart);
    int end = MIN(cend, rend);

    // Ignore spaces at the start of a new line
    while(r > 0 && lr != r && start < end && str[start] == ' ')
      start++;
    if(start < end)
      lr = r;

    if(start != end && rmask-r < nrows) {
      attron(colors[c]);
      addnstr(str+start, end-start);
      attroff(colors[c]);
    }

    if(rend <= cend) {
      r++;
      if(rmask-r < nrows)
        move(y - rmask + r, r == 0 || r >= ind_row ? x : x+indent);
    }
    if(rend >= cend)
      c++;
  }

  return rmask+1;
}


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
  case INPT_KEY(KEY_LEFT): // left  - cursor one character left
    if(ti->pos > 0) ti->pos--;
    break;
  case INPT_KEY(KEY_RIGHT):// right - cursor one character right
    if(ti->pos < chars) ti->pos++;
    break;
  case INPT_KEY(KEY_END):  // end
  case INPT_CTRL('e'):     // C-e   - cursor to end
    ti->pos = chars;
    break;
  case INPT_KEY(KEY_HOME): // home
  case INPT_CTRL('a'):     // C-a   - cursor to begin
    ti->pos = 0;
    break;
  case INPT_ALT('b'):      // Alt+b - cursor one word backward
    if(ti->pos > 0) {
      char *pos = g_utf8_offset_to_pointer(ti->str->str, ti->pos-1);
      while(pos > ti->str->str && *pos == ' ')
        pos--;
      while(pos > ti->str->str && *(pos-1) != ' ')
        pos--;
      ti->pos = g_utf8_strlen(ti->str->str, pos-ti->str->str);
    }
    break;
  case INPT_ALT('f'):      // Alt+f - cursor one word forward
    if(ti->pos < chars) {
      char *pos = g_utf8_offset_to_pointer(ti->str->str, ti->pos);
      while(*pos == ' ')
        pos++;
      while(*pos && *pos != ' ')
        pos++;
      ti->pos = g_utf8_strlen(ti->str->str, pos-ti->str->str);
    }
    break;
  case INPT_KEY(KEY_BACKSPACE): // backspace - delete character before cursor
    if(ti->pos > 0) {
      char *pos = g_utf8_offset_to_pointer(ti->str->str, ti->pos-1);
      g_string_erase(ti->str, pos-ti->str->str, g_utf8_next_char(pos)-pos);
      ti->pos--;
    }
    break;
  case INPT_KEY(KEY_DC):   // del   - delete character under cursor
    if(ti->pos < chars) {
      char *pos = g_utf8_offset_to_pointer(ti->str->str, ti->pos);
      g_string_erase(ti->str, pos-ti->str->str, g_utf8_next_char(pos)-pos);
    }
    break;
  case INPT_CTRL('w'):     // C-w   - delete to previous space
    if(ti->pos > 0) {
      char *end = g_utf8_offset_to_pointer(ti->str->str, ti->pos-1);
      char *begin = end;
      while(begin > ti->str->str && *begin == ' ')
        begin--;
      while(begin > ti->str->str && *(begin-1) != ' ')
        begin--;
      ti->pos -= g_utf8_strlen(begin, g_utf8_next_char(end)-begin);
      g_string_erase(ti->str, begin-ti->str->str, g_utf8_next_char(end)-begin);
    }
    break;
  case INPT_ALT('d'):      // Alt+d - delete to next space
    if(ti->pos < chars) {
      char *begin = g_utf8_offset_to_pointer(ti->str->str, ti->pos);
      char *end = begin;
      while(*end == ' ')
        end++;
      while(*end && *(end+1) && *(end+1) != ' ')
        end++;
      g_string_erase(ti->str, begin-ti->str->str, g_utf8_next_char(end)-begin);
    }
    break;
  case INPT_CTRL('k'):     // C-k   - delete everything after cursor
    if(ti->pos < chars)
      g_string_erase(ti->str, g_utf8_offset_to_pointer(ti->str->str, ti->pos)-ti->str->str, -1);
    break;
  case INPT_CTRL('u'):     // C-u   - delete entire line
    g_string_erase(ti->str, 0, -1);
    ti->pos = 0;
    break;
  case INPT_KEY(KEY_UP):   // up    - history search back
  case INPT_KEY(KEY_DOWN): // down  - history search forward
    if(ti->usehist)
      ui_textinput_search(ti, key == INPT_KEY(KEY_UP));
    else
      return FALSE;
    break;
  case INPT_CTRL('i'):     // tab   - autocomplete
    ui_textinput_complete(ti);
    completereset = FALSE;
    break;
  case INPT_CTRL('j'):     // newline - accept & clear
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


