

#define NCDC_MAIN
#include "ncdc.h"

#include <unistd.h>
#include <curses.h>
#include <locale.h>
#include <glib.h>


static void draw_screen() {
  curs_set(0); // may be overridden later on by a textinput widget
  erase();

  // first line
  attron(A_REVERSE);
  mvhline(0, 0, ' ', wincols);
  // TODO: replace title with tab title
  mvaddstr(0, 0, "Welcome to ncdc 0.1-alpa!");
  attroff(A_REVERSE);

  // tabs
  attron(A_REVERSE);
  mvhline(winrows-2, 0, ' ', wincols);
  move(winrows-2, 0);
  // TODO: list opened tabs
  attroff(A_REVERSE);

  // last line
  mvaddstr(winrows-1, 0, "Here be general info and real-time stats");

  // TODO: draw currently opened tab

  refresh();
}


static gboolean handle_input(GIOChannel *src, GIOCondition cond, gpointer dat) {
  // something happened!
  g_main_loop_quit(main_loop);
  return TRUE;
}


static gboolean screen_update_timer(gpointer dat) {
  draw_screen();
  return TRUE;
}


int main(int argc, char **argv) {
  setlocale(LC_ALL, "");

  // init stuff
  ui_tabs = g_array_new(FALSE, FALSE, sizeof(struct ui_tab));
  ui_tab_cur = 0;

  // init curses
  initscr();
  raw();
  noecho();
  curs_set(0);
  keypad(stdscr, 1);
  nodelay(stdscr, 1);
  getmaxyx(stdscr, winrows, wincols);
  draw_screen();

  // init and start main loop
  main_loop = g_main_loop_new(NULL, FALSE);

  GIOChannel *in = g_io_channel_unix_new(STDIN_FILENO);
  g_io_add_watch(in, G_IO_IN, handle_input, NULL);

  g_timeout_add_seconds(1, screen_update_timer, NULL);

  g_main_loop_run(main_loop);

  // cleanup
  erase();
  refresh();
  endwin();

  return 0;
}

