

#include "ncdc.h"

#include <string.h>
#include <errno.h>
#include <fcntl.h>
#include <assert.h>



// Log window "widget"


#define LOGWIN_BUF 2047 // must be 2^x-1
#define LOGWIN_PAD 9

// log entries are stored in a circular buffer
struct logwindow {
  int lastlog;
  int lastvis;
  int file;
  char *buf[LOGWIN_BUF+1];
};


static struct logwindow *logwindow_create(const char *file) {
  struct logwindow *lw = g_new0(struct logwindow, 1);
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


static void logwindow_free(struct logwindow *lw) {
  int i;
  if(lw->file >= 0)
    close(lw->file);
  for(i=0; i<LOGWIN_BUF; i++)
    if(lw->buf[i])
      g_free(lw->buf[i]);
  g_free(lw);
}


static void logwindow_add(struct logwindow *lw, const char *msg) {
  if(lw->lastlog == lw->lastvis)
    lw->lastvis = lw->lastlog + 1;
  lw->lastlog++;

  // TODO: convert invalid characters being written to the buffer
  //  logwindow_draw() really requires valid UTF-8
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


static void logwindow_scroll(struct logwindow *lw, int i) {
  lw->lastvis = MIN(lw->lastvis + i, lw->lastlog);
  lw->lastvis = MAX(lw->lastvis - LOGWIN_BUF + 1, MAX(1, lw->lastvis));
}


// TODO: this function is called often and can be optimized.
static void logwindow_draw(struct logwindow *lw, int y, int x, int rows, int cols) {
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
      int width = g_unichar_iswide(str[i]) ? 2 : g_unichar_iszerowidth(str[i]) ? 0 : 1;
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




// Main tab


// these is only one main tab, so these can be static
struct ui_tab main_tab;
struct logwindow *main_log;

void ui_main_create(int idx) {
  main_log = logwindow_create("main.log");

  main_tab.name = "main";
  main_tab.title = "Welcome to ncdc 0.1-alpha!";
  g_array_insert_val(ui_tabs, idx, main_tab);

  logwindow_add(main_log, "Welcome to ncdc 0.1-alpha!");
  char *tmp = g_strconcat("Using working directory: ", conf_dir, NULL);
  logwindow_add(main_log, tmp);
  g_free(tmp);
}


static void ui_main_draw() {
  logwindow_draw(main_log, 1, 0, winrows-3, wincols);
}


static void ui_main_key(struct input_key *key) {
  if(key->type == INPT_KEY) {
    if(key->code == KEY_NPAGE)
      logwindow_scroll(main_log, winrows/2);
    else if(key->code == KEY_PPAGE)
      logwindow_scroll(main_log, -winrows/2);
  }
}




// Global stuff


void ui_draw() {
  struct ui_tab *curtab = &g_array_index(ui_tabs, struct ui_tab, ui_tab_cur);

  getmaxyx(stdscr, winrows, wincols);
  curs_set(0); // may be overridden later on by a textinput widget
  erase();

  // first line
  attron(A_REVERSE);
  mvhline(0, 0, ' ', wincols);
  mvaddstr(0, 0, curtab->title);
  attroff(A_REVERSE);

  // tabs
  attron(A_REVERSE);
  mvhline(winrows-2, 0, ' ', wincols);
  move(winrows-2, 0);
  // TODO: status info
  attroff(A_REVERSE);

  // last line - text input
  mvaddstr(winrows-1, 0, curtab->name);
  addstr("> ");
  // TODO: text input

  if(curtab->type == UIT_MAIN)
    ui_main_draw();

  refresh();
}


void ui_input(struct input_key *key) {
  struct ui_tab *curtab = &g_array_index(ui_tabs, struct ui_tab, ui_tab_cur);

  // ctrl+c
  if(key->type == INPT_CTRL && key->code == 3)
    g_main_loop_quit(main_loop);
  // let tab handle it
  else {
    if(curtab->type == UIT_MAIN)
      ui_main_key(key);
  }
}

