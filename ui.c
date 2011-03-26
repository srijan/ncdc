
#include <glib.h>
#include <curses.h>

#include "ncdc.h"


void ui_main_create(int idx) {
  struct ui_tab tab;
  tab.name = "main";
  tab.title = "Welcome to ncdc 0.1-alpha!";
  g_array_insert_val(ui_tabs, idx, tab);
}


void ui_draw() {
  struct ui_tab *curtab = &g_array_index(ui_tabs, struct ui_tab, ui_tab_cur);

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

  // TODO: draw currently opened tab

  refresh();
}


void ui_input(GArray *keys) {
  // something happened!
  g_array_free(keys, TRUE);
  g_main_loop_quit(main_loop);
}

