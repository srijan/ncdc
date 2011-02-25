
#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <wchar.h>
#include <locale.h>
#include <caml/mlvalues.h>
#include <caml/memory.h>
#include <caml/fail.h>

// acually the wchar_t variant, but the name and location of this one seems to
// vary a lot among systems. On Arch linux, curses.h includes everything.
#include <curses.h>


int wincols, winrows;


CAMLprim value ui_init(value unit) {
  CAMLparam1(unit); 
  setlocale(LC_ALL, "");
  initscr();
  cbreak();
  noecho();
  curs_set(0);
  keypad(stdscr, 1);
  nodelay(stdscr, 1);
  CAMLreturn(Val_unit);
}


CAMLprim value ui_end(value unit) {
  CAMLparam1(unit);
  erase();
  refresh();
  endwin();
  CAMLreturn(Val_unit);
}


static int check_size() {
  getmaxyx(stdscr, winrows, wincols);
  // TODO: check for minimum size and display warning
  return 0;
}


CAMLprim value ui_global(value title, value tabs) {
  CAMLparam2(title, tabs);
  CAMLlocal1(val);
  int i = 0;

  erase();
  if(check_size())
    CAMLreturn(Val_true);

  // first line
  attron(A_REVERSE);
  mvhline(0, 0, ' ', wincols);
  mvaddstr(0, 0, String_val(title));
  attroff(A_REVERSE);

  // tabs
  attron(A_REVERSE);
  mvhline(winrows-2, 0, ' ', wincols);
  move(winrows-2, 0);
  // TODO:
  // - handle the case when there are too many tabs
  // - Indicate which tab is selected
  while(tabs != Val_emptylist) {
    printw("%d:", ++i);
    addstr(String_val(Field(Field(tabs, 0), 0)));
    tabs = Field(tabs, 1);
  }
  attroff(A_REVERSE);

  // last line
  mvaddstr(winrows-1, 0, "Here be general info and real-time stats");

  CAMLreturn(Val_unit);
}


CAMLprim value ui_tab_main(value log, value last, value scrup) {
  CAMLparam3(log, last, scrup);

  int backlog = Wosize_val(log)-1;
  int top = winrows - 4 + Long_val(scrup);;
  int cur = Long_val(last);
  short lines[51]; // assumes we can't have more than 50 lines per log item
  lines[0] = 0;
  while(top > 0) {
    char *str = String_val(Field(log, cur));
    int curline = 1, curlinecols = 0, i = -1,
        len = strlen(str);
    wchar_t *buffer = malloc(sizeof(wchar_t)*len);
    lines[1] = 0;
    while(*str) {
      // decode character
      int r = mbtowc(buffer+(++i), str, len);
      if(r < 0)
        caml_failwith("ui_tab_main: Invalid character encoding");
      len -= r;
      str += r;
      // decide on which line to put it
      int width = wcwidth(buffer[i]);
      if(curlinecols+width > wincols) {
        if(++curline > 50)
          caml_failwith("ui_tab_main: Too many lines for log entry");
        curlinecols = 0;
      }
      lines[curline] = i;
      curlinecols += width;
    }
    // print the lines
    for(i=curline-1; top>0 && i>=0; i--, top--)
      if(top <= winrows-4)
        mvaddnwstr(top, 0, buffer+lines[i], lines[i+1]-lines[i]+1);

    free(buffer);
    cur = (cur-1) & backlog;
  }
  
  refresh();
  CAMLreturn(Val_unit);
}


