
#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <wchar.h>
#include <wctype.h>
#include <locale.h>
#include <caml/mlvalues.h>
#include <caml/memory.h>
#include <caml/alloc.h>
#include <caml/callback.h>
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
   *    char == 127           -> Key KEY_BACKSPACE
   *    char <= 31            -> Ctrl char
   *    !'^['                 -> Char char_encoded
   *    ('^[', !)             -> Esc ""
   *    ('^[', !Char)         -> ignore both characters (1)
   *    ('^[', Char && '[')   -> ignore both characters and the character after that (2)
   *    ('^[', Char && !'[')  -> Esc next_char_encoded
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
    // backspace is often sent as DEL control character, correct this
    if(r != KEY_CODE_YES && c == 127) {
      r = KEY_CODE_YES;
      c = KEY_BACKSPACE;
    }
    int type = r == KEY_CODE_YES ? INPT_KEY : c == 27 ? INPT_ESC : c <= 31 ? INPT_CTRL : INPT_CHAR;
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



CAMLprim value ui_refresh(value unit) {
  CAMLparam1(unit);
  refresh();
  CAMLreturn(Val_unit);
}



CAMLprim value ui_textinput_get(value str) {
  CAMLparam1(str);
  CAMLlocal1(newstr);
  wchar_t *wstr = (wchar_t *) String_val(str);
  int len;
  if((len = wcstombs(NULL, wstr, 0)) < 0)
    failwith("ui_textinput_get: Cannot encode string");
  newstr = caml_alloc_string(len);
  wcstombs(String_val(newstr), wstr, len);
  CAMLreturn(newstr);
}



CAMLprim value ui_textinput_set(value newstr, value str, value curpos) {
  CAMLparam3(newstr, str, curpos);
  CAMLlocal1(dest);
  char *src = String_val(newstr);
  int len;
  if((len = mbstowcs(NULL, src, 0)) < 0)
    failwith("ui_textinput_set: Cannot decode string");
  dest = caml_alloc_string(sizeof(wchar_t)*(len+1));
  src = String_val(newstr);
  mbstowcs((wchar_t *)String_val(dest), src, len+1);
  Store_field(str, 0, dest);
  Store_field(curpos, 0, Val_int(len));
  CAMLreturn(Val_unit);
}



CAMLprim value ui_textinput_draw(value loc, value str, value curpos) {
  CAMLparam3(loc, str, curpos);
  //       |              |
  // "Some random string etc etc"
  //       f         #    l
  // f = function(#, strwidth(upto_#), wincols)
  // if(strwidth(upto_#) < wincols*0.85)
  //   f = 0
  // else
  //   f = strwidth(upto_#) - wincols*0.85
  wchar_t *wstr = (wchar_t *)String_val(str);
  int y = Long_val(Field(loc, 0));
  int x = Long_val(Field(loc, 1));
  int col = Long_val(Field(loc, 2));
  int cur = Long_val(curpos); // character number
  int i;

  // calculate f (in number of columns)
  int width = 0;
  for(i=0; i<=cur && *wstr; i++)
    width += wcwidth(wstr[i]);
  int f = width - (col*85)/100;
  if(f < 0)
    f = 0;

  // now print it on the screen, starting from column f in the string and
  // stopping when we're out of screen columns
  cchar_t t;
  memset(&t, 0, sizeof(cchar_t));
  mvhline(y, x, ' ', col);
  move(y, x);
  int pos = 0;
  i = 0;
  while(*wstr) {
    f -= wcwidth(*wstr);
    if(f < -col)
      break;
    if(f < 0) {
      t.chars[0] = *wstr;
      add_wch(&t);
      if(i < cur)
        pos += wcwidth(*wstr);
    }
    i++;
    wstr++;
  }
  move(y, x+pos);
  curs_set(1);
  refresh();
  CAMLreturn(Val_unit);
}



CAMLprim value ui_textinput_key(value key, value str, value curpos) {
  CAMLparam3(key, str, curpos);
  CAMLlocal1(tmp);
  wchar_t *s = (wchar_t *)String_val(Field(str, 0));
  int len = wcslen(s);
  int pos = Int_val(Field(curpos, 0));
  int handled = 1;
  if(Tag_val(key) == INPT_KEY) {
    switch(Int_val(Field(key, 0))) {
      case KEY_LEFT:  if(pos > 0) pos--; break;
      case KEY_RIGHT: if(pos < len) pos++; break;
      case KEY_END:   pos = len; break;
      case KEY_HOME:  pos = 0; break;
      case KEY_BACKSPACE:
        if(pos > 0) {
          memmove(s+pos-1, s+pos, (len-pos+1)*sizeof(wchar_t));
          pos--;
        }
        break;
      case KEY_DC:
        if(pos < len)
          memmove(s+pos, s+pos+1, (len-pos)*sizeof(wchar_t));
        break;
      default:
        handled = 0;
    }
  } else if(Tag_val(key) == INPT_CHAR) {
    // decode character
    wchar_t c;
    char *newchar = String_val(Field(key, 0));
    int r = mbtowc(&c, newchar, strlen(newchar));
    if(r < 0)
      caml_failwith("ui_textinput_key: Invalid character encoding");
    // make sure we have enough space to store the string
    int buflen = caml_string_length(Field(str, 0)) / sizeof(wchar_t);
    if(buflen <= len+1) {
      tmp = caml_alloc_string((len+1)*sizeof(wchar_t)*2);
      wcscpy((wchar_t *)String_val(tmp), s);
      s = (wchar_t *)String_val(tmp);
      Store_field(str, 0, tmp);
    }
    // insert character
    memmove(s+pos+1, s+pos, (len-pos+1)*sizeof(wchar_t));
    s[pos] = c;
    pos++;
  } else
    handled = 0;
  if(handled) {
    Store_field(curpos, 0, Val_int(pos));
    CAMLreturn(Val_true);
  }
  CAMLreturn(Val_false);
}



// TODO: this function isn't very fast. Can be optimized using some form of caching?
CAMLprim value ui_logwindow_draw(value loc, value log, value lastvisible) {
  CAMLparam3(loc, log, lastvisible);

  int y = Int_val(Field(loc, 0)),
      x = Int_val(Field(loc, 1)),
      rows = Int_val(Field(loc, 2)),
      cols = Int_val(Field(loc, 3)),
      backlog = Wosize_val(log)-1;

  int top = rows + y - 1;
  int cur = Long_val(lastvisible);

  // assumes we can't have more than 100 lines per log item, and that each log
  // item does not exceed 64k characters
  unsigned short lines[101];
  lines[0] = 0;

  while(top >= y) {
    char *str = String_val(Field(Field(log, cur & backlog), 1));
    int curline = 1, curlinecols = 0, i = -1,
        len = strlen(str);
    wchar_t *buffer = malloc(sizeof(wchar_t)*len);
    lines[1] = 0;
    while(*str) {
      // decode character
      int r = mbtowc(buffer+(++i), str, len);
      if(r < 0) {
        //caml_failwith("ui_logwindow_draw: Invalid character encoding");
        // Temporary solution. TODO: make sure all incoming text can be decoded
        buffer[i] = '?';
        r = 1;
      }
      len -= r;
      str += r;
      // decide on which line to put it
      int width = wcwidth(buffer[i]);
      if(curlinecols+width >= cols-9) {
        if(++curline > 100)
          caml_failwith("ui_logwindow_draw: Too many lines for log entry");
        curlinecols = 0;
      }
      lines[curline] = i+1;
      curlinecols += width;
    }
    // print the lines
    for(i=curline-1; top>=y && i>=0; i--, top--) {
      if(i == 0) {
        char ts[10];
        time_t tm = (time_t) Double_val(Field(Field(log, cur & backlog), 0));
        if(tm) {
          strftime(ts, 9, "%H:%M:%S", localtime(&tm));
          mvaddstr(top, x, ts);
        }
      }
      mvaddnwstr(top, x+9, buffer+lines[i], lines[i+1]-lines[i]);
    }

    free(buffer);
    cur = (cur-1) & backlog;
  }

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



CAMLprim value ui_global(value tabs, value seltab) {
  CAMLparam2(tabs, seltab);
  CAMLlocal1(val);
  int i = 0;
  int getName = hash_variant("getName");

  // a textinput may override this when it is drawn
  curs_set(0);

  erase();
  // first line
  attron(A_REVERSE);
  mvhline(0, 0, ' ', wincols);
  val = caml_callback(caml_get_public_method(seltab, hash_variant("getTitle")), seltab);
  mvaddstr(0, 0, String_val(val));
  attroff(A_REVERSE);

  // tabs
  attron(A_REVERSE);
  mvhline(winrows-2, 0, ' ', wincols);
  move(winrows-2, 0);
  // TODO:
  // - handle the case when there are too many tabs
  while(tabs != Val_emptylist) {
    if(Field(tabs, 0) == seltab)
      attron(A_BOLD);
    printw("%d:", ++i);
    val = caml_callback(caml_get_public_method(Field(tabs, 0), getName), Field(tabs, 0));
    addstr(String_val(val));
    if(Field(tabs, 0) == seltab)
      attroff(A_BOLD);
    addch(' ');
    tabs = Field(tabs, 1);
  }
  attroff(A_REVERSE);

  // last line
  mvaddstr(winrows-1, 0, "Here be general info and real-time stats");

  CAMLreturn(Val_unit);
}



CAMLprim value ui_tab_main(value unit) {
  CAMLparam1(unit);
  mvaddstr(winrows-3, 0, "console> ");
  CAMLreturn(Val_unit);
}



CAMLprim value ui_tab_hub(value name, value hub) {
  CAMLparam2(name, hub);
  attron(A_REVERSE);
  mvhline(winrows-4, 0, ' ', wincols);

  // time and username
  char buf[10];
  time_t tm = time(NULL);
  strftime(buf, 9, "%H:%M:%S", localtime(&tm));
  mvprintw(winrows-4, 0, "%s [%s]", buf,
    String_val(caml_callback(caml_get_public_method(hub, hash_variant("getNick")), hub)));

  // connection status or user count and share size
  int count = Int_val(caml_callback(caml_get_public_method(hub, hash_variant("getUserCount")), hub));
  if(count)
    mvprintw(winrows-4, wincols-26, "%6d users  %8.2f TB", count, 123.45);
  else if(Int_val(caml_callback(caml_get_public_method(hub, hash_variant("isConnecting")), hub)))
    mvaddstr(winrows-4, wincols-14, "connecting...");
  else
    mvaddstr(winrows-4, wincols-14, "not connected");

  attroff(A_REVERSE);
  mvprintw(winrows-3, 0, "#%s> ", String_val(name));
  CAMLreturn(Val_unit);
}

