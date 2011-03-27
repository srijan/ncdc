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


#if INTERFACE

#define UIT_MAIN 0

struct ui_tab {
  int type; // UIT_ type
  char *name;
  char *title;
  struct ui_logwindow *log;
};

#endif

GList *ui_tabs = NULL;
GList *ui_tab_cur = NULL;

// screen dimensions
int wincols;
int winrows;



// Main tab


// these is only one main tab, so this can be static
struct ui_tab *ui_main;

static struct ui_tab *ui_main_create() {
  ui_main = g_new0(struct ui_tab, 1);
  ui_main->name = "main";
  ui_main->title = "Welcome to ncdc 0.1-alpha!";
  ui_main->log = ui_logwindow_create("main.log");

  ui_logwindow_add(ui_main->log, "Welcome to ncdc 0.1-alpha!");
  ui_logwindow_printf(ui_main->log, "Using working directory: %s", conf_dir);

  return ui_main;
}


static void ui_main_draw() {
  ui_logwindow_draw(ui_main->log, 1, 0, winrows-3, wincols);
}


static void ui_main_key(struct input_key *key) {
  if(key->type == INPT_KEY) {
    if(key->code == KEY_NPAGE)
      ui_logwindow_scroll(ui_main->log, winrows/2);
    else if(key->code == KEY_PPAGE)
      ui_logwindow_scroll(ui_main->log, -winrows/2);
  }
}




// Global stuff


static struct ui_textinput *global_textinput;


void ui_init() {
  // first tab = main tab
  ui_tabs = g_list_append(ui_tabs, ui_main_create());
  ui_tab_cur = ui_tabs;

  // global textinput field
  global_textinput = ui_textinput_create();

  // init curses
  initscr();
  raw();
  noecho();
  curs_set(0);
  keypad(stdscr, 1);
  nodelay(stdscr, 1);

  // draw
  ui_draw();
}


void ui_draw() {
  struct ui_tab *curtab = ui_tab_cur->data;

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

  // tab contents
  if(curtab->type == UIT_MAIN)
    ui_main_draw();

  // last line - text input
  mvaddstr(winrows-1, 0, curtab->name);
  addch('>');
  int pos = strlen(curtab->name)+2;
  ui_textinput_draw(global_textinput, winrows-1, pos, wincols-pos);

  refresh();
}


void ui_input(struct input_key *key) {
  struct ui_tab *curtab = ui_tab_cur->data;

  // ctrl+c
  if(key->type == INPT_CTRL && key->code == 3)
    g_main_loop_quit(main_loop);

  // main text input (TODO: in some cases the focus shouldn't be on the text input.)
  else if(ui_textinput_key(global_textinput, key)) {

  // enter key is pressed while focused on the textinput
  } else if(key->type == INPT_CTRL && key->code == '\n') {
    char *str = ui_textinput_get(global_textinput);
    cmd_handle(str);
    g_free(str);
    ui_textinput_set(global_textinput, "");

  // let tab handle it
  } else {
    if(curtab->type == UIT_MAIN)
      ui_main_key(key);
  }
}

