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
#include <time.h>


#if INTERFACE

#define UIT_MAIN     0
#define UIT_HUB      1
#define UIT_USERLIST 2

struct ui_tab {
  int type; // UIT_ type
  char *name;
  struct ui_logwindow *log; // MAIN, HUB
  struct nmdc_hub *hub;     // HUB, USERLIST
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
  ui_main->log = ui_logwindow_create("main");

  ui_logwindow_printf(ui_main->log, "Welcome to ncdc %s!", VERSION);
  ui_logwindow_printf(ui_main->log, "Using working directory: %s", conf_dir);

  return ui_main;
}


static void ui_main_draw() {
  ui_logwindow_draw(ui_main->log, 1, 0, winrows-4, wincols);

  mvaddstr(winrows-3, 0, "main>");
  ui_textinput_draw(ui_global_textinput, winrows-3, 6, wincols-6);
}


static char *ui_main_title() {
  return g_strdup_printf("Welcome to ncdc %s!", VERSION);
}


static void ui_main_key(struct input_key *key) {
  char *str = NULL;
  if(key->type == INPT_KEY && key->code == KEY_NPAGE)
    ui_logwindow_scroll(ui_main->log, winrows/2);
  else if(key->type == INPT_KEY && key->code == KEY_PPAGE)
    ui_logwindow_scroll(ui_main->log, -winrows/2);
  else if(ui_textinput_key(ui_global_textinput, key, &str) && str) {
    cmd_handle(str);
    g_free(str);
  }
}




// Hub tab

struct ui_tab *ui_hub_create(const char *name) {
  struct ui_tab *tab = g_new0(struct ui_tab, 1);
  // NOTE: tab name is also used as configuration group
  tab->name = g_strdup_printf("#%s", name);
  tab->type = UIT_HUB;
  tab->log = ui_logwindow_create(tab->name);
  tab->hub = nmdc_create(tab);
  // already used this name before? open connection again
  if(g_key_file_has_key(conf_file, tab->name, "hubaddr", NULL))
    nmdc_connect(tab->hub);
  return tab;
}


void ui_hub_close(ui_tab *tab) {
  // also close the userlist tab, if there is one
  GList *n;
  for(n=ui_tabs; n; n=n->next) {
    struct ui_tab *t = n->data;
    if(t->type == UIT_USERLIST && t->hub == tab->hub)
      ui_userlist_close(t);
  }
  ui_tab_remove(tab);

  nmdc_free(tab->hub);
  ui_logwindow_free(tab->log);
  g_free(tab->name);
  g_free(tab);
}


static void ui_hub_draw(struct ui_tab *tab) {
  ui_logwindow_draw(tab->log, 1, 0, winrows-5, wincols);

  attron(A_REVERSE);
  mvhline(winrows-4, 0, ' ', wincols);
  if(tab->hub->state == HUBS_IDLE)
    mvaddstr(winrows-4, wincols-16, "Not connected.");
  else if(tab->hub->state == HUBS_CONNECTING)
    mvaddstr(winrows-4, wincols-15, "Connecting...");
  else if(!tab->hub->nick_valid)
    mvaddstr(winrows-4, wincols-15, "Logging in...");
  else {
    char *addr = conf_hub_get(string, tab->name, "hubaddr");
    char *tmp = g_strdup_printf("%s @ dchub://%s/", tab->hub->nick, addr);
    mvaddstr(winrows-4, 0, tmp);
    g_free(addr);
    g_free(tmp);
    int count = g_hash_table_size(tab->hub->users);
    tmp = g_strdup_printf("%6d users  %10s%c", count,
      str_formatsize(tab->hub->sharesize), tab->hub->sharecount == count ? ' ' : '+');
    mvaddstr(winrows-4, wincols-26, tmp);
    g_free(tmp);
  }
  attroff(A_REVERSE);

  mvaddstr(winrows-3, 0, tab->name);
  addstr("> ");
  int pos = str_columns(tab->name)+2;
  ui_textinput_draw(ui_global_textinput, winrows-3, pos, wincols-pos);
}


static char *ui_hub_title(struct ui_tab *tab) {
  return g_strdup_printf("%s: %s", tab->name,
    tab->hub->state == HUBS_IDLE       ? "Not connected." :
    tab->hub->state == HUBS_CONNECTING ? "Connecting..." :
    !tab->hub->nick_valid              ? "Logging in..." :
    tab->hub->hubname                  ? tab->hub->hubname : "Connected.");
}


static void ui_hub_key(struct ui_tab *tab, struct input_key *key) {
  char *str = NULL;
  if(key->type == INPT_KEY && key->code == KEY_NPAGE)
    ui_logwindow_scroll(tab->log, winrows/2);
  else if(key->type == INPT_KEY && key->code == KEY_PPAGE)
    ui_logwindow_scroll(tab->log, -winrows/2);
  else if(ui_textinput_key(ui_global_textinput, key, &str) && str) {
    cmd_handle(str);
    g_free(str);
  }
}




// Userlist tab

struct ui_tab *ui_userlist_create(struct nmdc_hub *hub) {
  struct ui_tab *tab = g_new0(struct ui_tab, 1);
  tab->name = g_strdup_printf("u/%s", hub->tab->name);
  tab->type = UIT_USERLIST;
  tab->hub = hub;
  return tab;
}


void ui_userlist_close(struct ui_tab *tab) {
  ui_tab_remove(tab);
  g_free(tab->name);
  g_free(tab);
}


static char *ui_userlist_title(struct ui_tab *tab) {
  return g_strdup_printf("%s / User list", tab->hub->tab->name);
}




// Generic message displaying thing.

static char *ui_msg_text = NULL;
static guint ui_msg_timer;
static gboolean ui_msg_updated = FALSE;


static gboolean ui_msg_timeout(gpointer data) {
  ui_msg(NULL);
  return FALSE;
}


// a notication message, either displayed in the log of the current tab or, if
// the hub has no tab, in the "status bar". Calling this function with NULL
// will reset the status bar message.
void ui_msg(char *msg) {
  struct ui_tab *tab = ui_tab_cur->data;
  if(ui_msg_text) {
    g_free(ui_msg_text);
    ui_msg_text = NULL;
    g_source_remove(ui_msg_timer);
    ui_msg_updated = TRUE;
  }
  if(msg && tab->log)
    ui_logwindow_add(tab->log, msg);
  else if(msg) {
    ui_msg_text = g_strdup(msg);
    ui_msg_timer = g_timeout_add(3000, ui_msg_timeout, NULL);
    ui_msg_updated = TRUE;
  }
}


void ui_msgf(const char *fmt, ...) {
  va_list va;
  va_start(va, fmt);
  char *str = g_strdup_vprintf(fmt, va);
  va_end(va);
  ui_msg(str);
  g_free(str);
}



// Global stuff

struct ui_textinput *ui_global_textinput;

void ui_tab_open(struct ui_tab *tab) {
  ui_tabs = g_list_append(ui_tabs, tab);
  ui_tab_cur = g_list_last(ui_tabs);
}


// to be called from ui_*_close()
void ui_tab_remove(struct ui_tab *tab) {
  GList *cur = g_list_find(ui_tabs, tab);
  if(cur == ui_tab_cur)
    ui_tab_cur = cur->prev ? cur->prev : cur->next;
  ui_tabs = g_list_delete_link(ui_tabs, cur);
}


void ui_init() {
  // global textinput field
  ui_global_textinput = ui_textinput_create(TRUE);

  // first tab = main tab
  ui_tab_open(ui_main_create());

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

  // first line - title
  char *title =
    curtab->type == UIT_MAIN     ? ui_main_title() :
    curtab->type == UIT_HUB      ? ui_hub_title(curtab) :
    curtab->type == UIT_USERLIST ? ui_userlist_title(curtab) : g_strdup("");
  attron(A_REVERSE);
  mvhline(0, 0, ' ', wincols);
  mvaddstr(0, 0, title);
  attroff(A_REVERSE);
  g_free(title);

  // second-last line - time and tab list
  attron(A_REVERSE);
  // time
  mvhline(winrows-2, 0, ' ', wincols);
  time_t tm = time(NULL);
  char ts[10];
  strftime(ts, 9, "%H:%M:%S", localtime(&tm));
  mvaddstr(winrows-2, 0, ts);
  addstr(" --");
  // tab list
  // TODO: handle screen overflows
  GList *n;
  int i=0;
  for(n=ui_tabs; n; n=n->next) {
    i++;
    addch(' ');
    if(n == ui_tab_cur)
      attron(A_BOLD);
    char *tmp = g_strdup_printf("%d:%s", i, ((struct ui_tab *)n->data)->name);
    addstr(tmp);
    g_free(tmp);
    if(n == ui_tab_cur)
      attroff(A_BOLD);
  }
  attroff(A_REVERSE);

  // last line - status info or notification
  ui_msg_updated = FALSE;
  if(ui_msg_text)
    mvaddstr(winrows-1, 0, ui_msg_text);
  else
    mvaddstr(winrows-1, wincols-14, "Here be stats");

  // tab contents
  switch(curtab->type) {
  case UIT_MAIN: ui_main_draw(); break;
  case UIT_HUB:  ui_hub_draw(curtab);  break;
  }

  refresh();
}


gboolean ui_checkupdate() {
  struct ui_tab *cur = ui_tab_cur->data;
  return ui_msg_updated || (cur->log && cur->log->updated);
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
    ui_tab_cur = ui_tab_cur->prev ? ui_tab_cur->prev : g_list_last(ui_tabs);
  } else if(key->type == INPT_ALT && key->code == 'k') {
    ui_tab_cur = ui_tab_cur->next ? ui_tab_cur->next : ui_tabs;

  // alt+h and alt+l (swap tab with left/right)
  } else if(key->type == INPT_ALT && key->code == 'h' && ui_tab_cur->prev) {
    GList *prev = ui_tab_cur->prev;
    ui_tabs = g_list_delete_link(ui_tabs, ui_tab_cur);
    ui_tabs = g_list_insert_before(ui_tabs, prev, curtab);
    ui_tab_cur = prev->prev;
  } else if(key->type == INPT_ALT && key->code == 'l' && ui_tab_cur->next) {
    GList *next = ui_tab_cur->next;
    ui_tabs = g_list_delete_link(ui_tabs, ui_tab_cur);
    ui_tabs = g_list_insert_before(ui_tabs, next->next, curtab);
    ui_tab_cur = next->next;

  // alt+c (alias for /close)
  } else if(key->type == INPT_ALT && key->code == 'c') {
    cmd_handle("/close");

  // ctrl+l (alias for /clear)
  } else if(key->type == INPT_CTRL && key->code == 12) {
    cmd_handle("/clear");

  // let tab handle it
  } else {
    switch(curtab->type) {
    case UIT_MAIN: ui_main_key(key); break;
    case UIT_HUB:  ui_hub_key(curtab, key); break;
    }
  }
}

