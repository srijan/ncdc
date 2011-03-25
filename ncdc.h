
#ifndef NCDC_H
#define NCDC_H

#include <glib.h>


#define UIT_MAIN 0

struct ui_tab {
  int type; // UIT_ type
  char *name;
  char *title;
  // more stuff(?)
};


#ifndef NCDC_MAIN

extern GMainLoop *main_loop;
extern GArray *ui_tabs;
extern int ui_tab_cur;
extern int wincols, winrows;

#else

GMainLoop *main_loop;
GArray *ui_tabs;
int ui_tab_cur;
int wincols, winrows;

#endif

#endif

