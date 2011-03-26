

#define NCDC_MAIN
#include "ncdc.h"

#include <unistd.h>
#include <locale.h>
#include <glib.h>


static gboolean handle_input(GIOChannel *src, GIOCondition cond, gpointer dat) {
  /* Mapping from get_wch() to struct input_key:
   *  KEY_CODE_YES -> KEY
   *  KEY_CODE_NO:
   *    char == 127           -> KEY = KEY_BACKSPACE
   *    char <= 31            -> CTRL
   *    !'^['                 -> CHAR
   *    ('^[', !)             -> KEY = KEY_ESCAPE
   *    ('^[', !CHAR)         -> ignore both characters (1)
   *    ('^[', CHAR && '[')   -> ignore both characters and the character after that (2)
   *    ('^[', CHAR && !'[')  -> ALT = second char
   *
   * 1. this is something like ctrl+alt+X, which we won't use
   * 2. these codes indicate a 'Key' that somehow wasn't captured with
   *    KEY_CODE_YES. We won't attempt to interpret these ourselves.
   *
   * There are still several unhandled issues:
   * - Ncurses does not catch all key codes, and there is no way of knowing how
   *   many bytes a key code spans. Things like ^[[1;3C won't be handled correctly. :-(
   * - Ncurses can actually return key codes > KEY_MAX, but does not provide
   *   any mechanism for figuring out which key it actually was.
   * - It may be useful to use define_key() for some special (and common) codes
   * - Modifier keys will always be a problem. Most alt+key things work, except
   *   for those that may start a control code. alt+[ is a famous one, but
   *   there are others (like alt+O on my system). This is system-dependend,
   *   and again we have no way of knowing these things. (except perhaps by
   *   reading termcap entries on our own?)
   */

  GArray *keys = g_array_new(FALSE, FALSE, sizeof(struct input_key));
  struct input_key key;
  int r;
  int lastesc = 0, curignore = 0;
  doupdate(); // WHY!? http://www.webservertalk.com/archive107-2005-1-896232.html
  while((r = get_wch(&(key.code))) != ERR) {
    if(curignore) {
      curignore = 0;
      continue;
    }
    // backspace is often sent as DEL control character, correct this
    if(r != KEY_CODE_YES && key.code == 127) {
      r = KEY_CODE_YES;
      key.code = KEY_BACKSPACE;
    }
    key.type = r == KEY_CODE_YES ? INPT_KEY : key.code == 27 ? INPT_ALT : key.code <= 31 ? INPT_CTRL : INPT_CHAR;
    // do something with key.encoded
    if(key.type == INPT_CHAR) {
      if((r = wctomb(key.encoded, key.code)) < 0)
        g_warning("Cannot encode character 0x%X", key.code);
      key.encoded[r] = 0;
    } else if(key.type != INPT_KEY) {
      // if it's not a "character" nor a key, then it very likely fits in a single byte
      key.encoded[0] = key.code;
      key.encoded[1] = 0;
    }
    // check for escape sequence
    if(lastesc) {
      lastesc = 0;
      if(key.type != INPT_CHAR)
        continue;
      if(key.code == '[') {
        curignore = 0;
        continue;
      }
      key.type = INPT_ALT;
      g_array_append_val(keys, key);
      continue;
    }
    if(key.type == INPT_ALT) {
      lastesc = 1;
      continue;
    }
    // no escape sequence? just insert it into the array
    if(key.type == INPT_CHAR || key.type == INPT_KEY)
      g_array_append_val(keys, key);
    if(key.type == INPT_CTRL)
      g_array_append_val(keys, key); // TODO: control code to printable char?
  }
  if(lastesc) {
    key.type = INPT_KEY;
    key.code = KEY_ESCAPE;
  }

  ui_input(keys);
  return TRUE;
}


static gboolean screen_update_timer(gpointer dat) {
  ui_draw();
  return TRUE;
}


int main(int argc, char **argv) {
  setlocale(LC_ALL, "");

  // init stuff
  ui_tabs = g_array_new(FALSE, FALSE, sizeof(struct ui_tab));
  ui_main_create(0);
  ui_tab_cur = 0;

  // init curses
  initscr();
  raw();
  noecho();
  curs_set(0);
  keypad(stdscr, 1);
  nodelay(stdscr, 1);
  getmaxyx(stdscr, winrows, wincols);
  ui_draw();

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

