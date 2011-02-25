
#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <wchar.h>
#include <locale.h>
#include <caml/mlvalues.h>
#include <caml/memory.h>
#include <caml/alloc.h>
#include <caml/fail.h>

// acually the wchar_t variant, but the name and location of this one seems to
// vary a lot among systems. On Arch linux, curses.h includes everything.
#include <curses.h>

// ui.ml:type input_t
#define INPT_KEY  0
#define INPT_CTRL 1
#define INPT_ESC  2
#define INPT_CHAR 3

int wincols, winrows;


CAMLprim value ui_getinput(value unit) {
  CAMLparam1(unit);
  CAMLlocal4(head, k, b, tail);

  head = Val_emptylist;
#define PUSHLIST(t, v) do {\
    b = caml_alloc(1, (t));\
    Store_field(b, 0, (v));\
    k = caml_alloc(2, 0);\
    Store_field(k, 0, b);\
    Store_field(k, 1, Val_emptylist);\
    if(head == Val_emptylist) head = k;\
    else Store_field(tail, 1, k);\
    tail = k;\
  } while(0)

  /* Mapping from get_wch() to input_t:
   *  KEY_CODE_YES -> Key char_encoded
   *  KEY_CODE_NO:
   *    char <= 31 || char == 127 -> Ctrl char
   *    !'^['                     -> Char char_encoded
   *    ('^[', !)                 -> Esc ""
   *    ('^[', !Char)             -> ignore both characters (1)
   *    ('^[', Char && '[')       -> ignore both characters and the character after that (2)
   *    ('^[', Char && !'[')      -> Esc next_char_encoded
   *
   * 1. this is something like ctrl+alt+X, which we won't use
   * 2. these codes indicate a 'Key' that somehow wasn't captured with
   *    KEY_CODE_YES. We won't attempt to interpret these ourselves.
   */

  int r;
  wint_t c;
  int lastesc = 0, curignore = 0;
  char encoded[10]; // should be MB_CUR_MAX+1, but that's not known at compile time
  doupdate(); // WHY!? http://www.webservertalk.com/archive107-2005-1-896232.html
  while((r = get_wch(&c)) != ERR) {
    if(curignore) {
      curignore = 0;
      continue;
    }
    int type = r == KEY_CODE_YES ? INPT_KEY : c == 27 ? INPT_ESC : c <= 31 || c == 127 ? INPT_CTRL : INPT_CHAR;
    if(type == INPT_CHAR) {
      if((r = wctomb(encoded, c)) < 0)
        failwith("ui_getinput: Cannot encode character");
      encoded[r] = 0;
    }
    if(lastesc) {
      lastesc = 0;
      if(type != INPT_CHAR)
        continue;
      if(c == '[') {
        curignore = 0;
        continue;
      }
      PUSHLIST(INPT_ESC, caml_copy_string(encoded));
      continue;
    }
    if(type == INPT_ESC) {
      lastesc = 1;
      continue;
    }
    if(type == INPT_KEY || type == INPT_CTRL)
      PUSHLIST(type, Val_int(c));
    if(type == INPT_CHAR)
      PUSHLIST(type, caml_copy_string(encoded));
  }
  if(lastesc)
    PUSHLIST(INPT_ESC, caml_copy_string(""));

  CAMLreturn(head);
}



CAMLprim value ui_init(value unit) {
  CAMLparam1(unit); 
  setlocale(LC_ALL, "");
  initscr();
  raw();
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



CAMLprim value ui_checksize(value toosmall, value rows, value cols) {
  CAMLparam3(toosmall, rows, cols);
  getmaxyx(stdscr, winrows, wincols);
  Store_field(rows, 0, Val_int(winrows));
  Store_field(cols, 0, Val_int(wincols));
  // TODO: check for minimum size and display warning
  CAMLreturn(Val_unit);
}



CAMLprim value ui_global(value title, value tabs) {
  CAMLparam2(title, tabs);
  CAMLlocal1(val);
  int i = 0;

  erase();
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



CAMLprim value ui_tab_main(value log, value lastvisible) {
  CAMLparam2(log, lastvisible);

  int backlog = Wosize_val(log)-1;
  int top = winrows - 4;
  int cur = Long_val(lastvisible);
  short lines[51]; // assumes we can't have more than 50 lines per log item
  lines[0] = 0;
  while(top > 0) {
    char *str = String_val(Field(log, cur & backlog));
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


