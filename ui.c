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
#define UIT_HUB 1

struct ui_tab {
  int type; // UIT_ type
  char *name;
  char *title;
  struct ui_logwindow *log;
  struct nmdc_hub *hub;
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
  ui_main->log = ui_logwindow_create("main");

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




// Hub tab

struct ui_tab *ui_hub_create(const char *name) {
  struct ui_tab *tab = g_new0(struct ui_tab, 1);
  // NOTE: tab name is also used as configuration group
  tab->name = g_strdup_printf("#%s", name);
  tab->title = "Debugging hub tabs";
  tab->type = UIT_HUB;
  tab->log = ui_logwindow_create(name);
  tab->hub = nmdc_create(tab);
  // already used this name before? open connection again
  if(g_key_file_has_key(conf_file, tab->name, "hubaddr", NULL))
    nmdc_connect(tab->hub);
  return tab;
}


void ui_hub_close(ui_tab *tab) {
  // remove from the ui_tabs list
  GList *cur = g_list_find(ui_tabs, tab);
  ui_tab_cur = cur->prev ? cur->prev : cur->next;
  ui_tabs = g_list_delete_link(ui_tabs, cur);

  // free resources
  nmdc_free(tab->hub);
  ui_logwindow_free(tab->log);
  g_free(tab->name);
  g_free(tab);
}


static void ui_hub_draw(struct ui_tab *tab) {
  ui_logwindow_draw(tab->log, 1, 0, winrows-3, wincols);
}


static void ui_hub_key(struct ui_tab *tab, struct input_key *key) {
  if(key->type == INPT_KEY) {
    if(key->code == KEY_NPAGE)
      ui_logwindow_scroll(tab->log, winrows/2);
    else if(key->code == KEY_PPAGE)
      ui_logwindow_scroll(tab->log, -winrows/2);
  }
}




// Global stuff


static struct ui_textinput *global_textinput;


void ui_tab_open(struct ui_tab *tab) {
  ui_tabs = g_list_append(ui_tabs, tab);
  ui_tab_cur = g_list_last(ui_tabs);
}


void ui_init() {
  // first tab = main tab
  ui_tab_open(ui_main_create());

  // global textinput field
  global_textinput = ui_textinput_create(TRUE);

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

  // second-last line
  attron(A_REVERSE);
  mvhline(winrows-2, 0, ' ', wincols);

  GDateTime *t = g_date_time_new_now_local();
  char *tf = g_date_time_format(t, "%H:%M:%S");
  g_date_time_unref(t);
  mvaddstr(winrows-2, 0, tf);
  g_free(tf);
  // TODO: status info
  attroff(A_REVERSE);

  // tab contents
  switch(curtab->type) {
  case UIT_MAIN: ui_main_draw(); break;
  case UIT_HUB:  ui_hub_draw(curtab);  break;
  }

  // last line - text input
  mvaddstr(winrows-1, 0, curtab->name);
  addch('>');
  int pos = strlen(curtab->name)+2; // TODO: use number of columns, not number of bytes...
  ui_textinput_draw(global_textinput, winrows-1, pos, wincols-pos);

  refresh();
}


gboolean ui_checkupdate() {
  struct ui_tab *cur = ui_tab_cur->data;
  return cur->log->updated;
}


void ui_input(struct input_key *key) {
  struct ui_tab *curtab = ui_tab_cur->data;

  // ctrl+c
  if(key->type == INPT_CTRL && key->code == 3)
    g_main_loop_quit(main_loop);

  // alt+num (switch tab)
  else if(key->type == INPT_ALT && key->code >= '0' && key->code <= '9') {
    GList *n = g_list_nth(ui_tabs, key->code == '0' ? 9 : key->code-'1');
    if(n)
      ui_tab_cur = n;

  // alt+j and alt+k (previous/next tab)
  } else if(key->type == INPT_ALT && key->code == 'j') {
    if(ui_tab_cur->prev)
      ui_tab_cur = ui_tab_cur->prev;
  } else if(key->type == INPT_ALT && key->code == 'k') {
    if(ui_tab_cur->next)
      ui_tab_cur = ui_tab_cur->next;

  // main text input (TODO: in some cases the focus shouldn't be on the text input.)
  } else if(ui_textinput_key(global_textinput, key)) {

  // enter key is pressed while focused on the textinput
  } else if(key->type == INPT_CTRL && key->code == '\n') {
    char *str = ui_textinput_reset(global_textinput);
    cmd_handle(str);
    g_free(str);
    ui_textinput_set(global_textinput, "");

  // let tab handle it
  } else {
    switch(curtab->type) {
    case UIT_MAIN: ui_main_key(key); break;
    case UIT_HUB:  ui_hub_key(curtab, key); break;
    }
  }
}

