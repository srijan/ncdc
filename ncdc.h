
#ifndef NCDC_H
#define NCDC_H

#include "config.h"
#include <wchar.h>
#include <glib.h>

#ifdef HAVE_NCURSESW_NCURSES_H
#include <ncursesw/ncurses.h>
#else
#include <ncurses.h>
#endif

// Make sure that wchar_t and gunichar are equivalent
// TODO: this should be checked at ./configure time
#ifndef __STDC_ISO_10646__
#error Your wchar_t type is not guaranteed to be UCS-4!
#endif


// keyboard input

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


// ui tabs

#define UIT_MAIN 0

struct ui_tab {
  int type; // UIT_ type
  char *name;
  char *title;
};



// ui.c

void ui_main_create(int);
void ui_init();
void ui_draw();
void ui_input(struct input_key *);



// global variables

#ifndef NCDC_MAIN

extern const char *conf_dir;
extern GMainLoop *main_loop;
extern GArray *ui_tabs;
extern int ui_tab_cur;
extern int wincols, winrows;

#else

const char *conf_dir;
GMainLoop *main_loop;
GArray *ui_tabs;
int ui_tab_cur;
int wincols, winrows;

#endif

#endif

