
#include "ncdc.h"
#include <stdlib.h>
#include <unistd.h>
#include <locale.h>
#include <signal.h>
#include <glib/gstdio.h>
#include <stdio.h>


// input handling declarations

#if INTERFACE

#define KEY_ESCAPE (KEY_MAX+1)
#define INPT_KEY  0
#define INPT_CHAR 1
#define INPT_CTRL 2
#define INPT_ALT  4

struct input_key {
  wchar_t code;    // character code (as given by get_wch())
  char type;       // INPT_ type
  char encoded[7]; // UTF-8 encoded character string (if type != key)
};

#endif


// global variables
const char *conf_dir;
GMainLoop *main_loop;
GArray *ui_tabs;
int ui_tab_cur;
int wincols;
int winrows;



static void handle_input() {
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

  struct input_key key;
  int r;
  int lastesc = 0, curignore = 0;
  while((r = get_wch((wint_t *)&(key.code))) != ERR) {
    if(curignore) {
      curignore = 0;
      continue;
    }
    // we use SIGWINCH, so KEY_RESIZE can be ignored
    if(r == KEY_CODE_YES && key.code == KEY_RESIZE)
      continue;
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
      ui_input(&key);
      continue;
    }
    if(key.type == INPT_ALT) {
      lastesc = 1;
      continue;
    }
    // no escape sequence? just insert it into the array
    if(key.type == INPT_CHAR || key.type == INPT_KEY)
      ui_input(&key);
    if(key.type == INPT_CTRL)
      ui_input(&key); // TODO: control code to printable char?
  }
  if(lastesc) {
    key.type = INPT_KEY;
    key.code = KEY_ESCAPE;
    ui_input(&key);
  }

  ui_draw();
}


static gboolean stdin_read(GIOChannel *src, GIOCondition cond, gpointer dat) {
  handle_input();
  return TRUE;
}


static gboolean screen_update_timer(gpointer dat) {
  ui_draw();
  return TRUE;
}


// Fired when the screen is resized.  Normally I would check for KEY_RESIZE,
// but that doesn't work very nicely together with select(). See
// http://www.webservertalk.com/archive107-2005-1-896232.html
static void catch_sigwinch(int sig) {
  endwin();
  doupdate();
  ui_draw();
}


static void init_config() {
  // get location of the configuration directory
  conf_dir = g_getenv("NCDC_DIR");
  if(!conf_dir)
    conf_dir = g_build_filename(g_get_home_dir(), ".ncdc", NULL);

  // try to create it (ignoring errors if it already exists)
  g_mkdir(conf_dir, 0700);
  if(g_access(conf_dir, F_OK | R_OK | X_OK | W_OK) < 0) {
    fprintf(stderr, "Directory '%s' does not exist or is not writable.", conf_dir);
    exit(1);
  }

  // we should also have a logs/ subdirectory
  char *logs = g_build_filename(conf_dir, "logs", NULL);
  g_mkdir(logs, 0700);
  if(g_access(conf_dir, F_OK | R_OK | X_OK | W_OK) < 0) {
    fprintf(stderr, "Directory '%s' does not exist or is not writable.", logs);
    exit(1);
  }
  g_free(logs);
}


int main(int argc, char **argv) {
  setlocale(LC_ALL, "");

  // init stuff
  init_config();
  ui_init();

  // setup SIGWINCH
  struct sigaction act;
  sigemptyset(&act.sa_mask);
  act.sa_flags = SA_RESTART;
  act.sa_handler = catch_sigwinch;
  if(sigaction(SIGWINCH, &act, NULL) < 0) {
    perror("Error setting up SIGWINCH");
    exit(1);
  }

  // init and start main loop
  main_loop = g_main_loop_new(NULL, FALSE);

  GIOChannel *in = g_io_channel_unix_new(STDIN_FILENO);
  g_io_add_watch(in, G_IO_IN, stdin_read, NULL);

  g_timeout_add_seconds(1, screen_update_timer, NULL);

  g_main_loop_run(main_loop);

  // cleanup
  erase();
  refresh();
  endwin();

  return 0;
}

