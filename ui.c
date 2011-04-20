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
  struct ui_logwindow *log;    // MAIN, HUB
  struct nmdc_hub *hub;        // HUB, USERLIST
  struct ui_tab *userlist_tab; // HUB
  // USERLIST
  GSequence *users;
  GSequenceIter *user_sel;
  GSequenceIter *user_top;
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
  if(tab->userlist_tab)
    ui_userlist_close(tab->userlist_tab);
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


void ui_hub_joinquit(struct ui_tab *tab, gboolean join, struct nmdc_user *user) {
  gboolean log = conf_hub_get(boolean, tab->name, "show_joinquit");
  // TODO: don't show joins when we're still getting the initial user list
  if(join) {
    if(log && (!tab->hub->nick_valid || strcmp(tab->hub->nick_hub, user->name_hub) != 0))
      ui_logwindow_printf(tab->log, "%s has joined.", user->name);
  } else {
    if(log)
      ui_logwindow_printf(tab->log, "%s has quit.", user->name);
  }

  if(tab->userlist_tab)
    ui_userlist_joinquit(tab->userlist_tab, join, user);
}




// Userlist tab

// This function should return 0 if and only if a is exactly equal to b
static gint ui_userlist_sort_func(gconstpointer da, gconstpointer db, gpointer tab) {
  const struct nmdc_user *a = da;
  const struct nmdc_user *b = db;
  // TODO: dynamic ordering (ascending/descending, name/sharesize)
  int o = g_utf8_collate(a->name, b->name);
  if(!o)
    o = strcmp(a->name_hub, b->name_hub); // guaranteed to be different for different users
  return o;
}


struct ui_tab *ui_userlist_create(struct nmdc_hub *hub) {
  struct ui_tab *tab = g_new0(struct ui_tab, 1);
  tab->name = g_strdup_printf("u/%s", hub->tab->name);
  tab->type = UIT_USERLIST;
  tab->hub = hub;
  tab->users = g_sequence_new(NULL);
  // populate the list
  // g_sequence_sort() uses insertion sort? in that case it is faster to insert
  // all items using g_sequence_insert_sorted() rather than inserting them in
  // no particular order and then sorting them in one go. (which is faster for
  // linked lists, since it uses a faster sorting algorithm)
  GHashTableIter iter;
  g_hash_table_iter_init(&iter, hub->users);
  struct nmdc_user *u;
  while(g_hash_table_iter_next(&iter, NULL, (gpointer *)&u))
    g_sequence_insert_sorted(tab->users, u, ui_userlist_sort_func, tab);
  tab->user_sel = tab->user_top = g_sequence_get_begin_iter(tab->users);
  return tab;
}


void ui_userlist_close(struct ui_tab *tab) {
  tab->hub->tab->userlist_tab = NULL;
  ui_tab_remove(tab);
  g_sequence_free(tab->users);
  g_free(tab->name);
  g_free(tab);
}


static char *ui_userlist_title(struct ui_tab *tab) {
  return g_strdup_printf("%s / User list", tab->hub->tab->name);
}


#define DRAW_COL(row, colvar, width, str) do {\
    mvaddnstr(row, colvar, str, str_offset_from_columns(str, width-1));\
    colvar += width;\
  } while(0)

static void ui_userlist_draw(struct ui_tab *tab) {
  // column widths
  // TODO: dynamically show/hide columns (or change widths, but that's more work and perhaps not very intuitive)
  int cw_user = 20;
  int cw_share = 12;
  int cw_conn = 15;
  int i = wincols-cw_user-cw_share-cw_conn;
  int cw_desc = i*3/12;
  int cw_tag = i*6/12;
  int cw_mail = i-cw_desc-cw_tag;
  // header
  i = 0;
  attron(A_BOLD);
  DRAW_COL(1, i, cw_user,  "Username");
  DRAW_COL(1, i, cw_share, "Share");
  DRAW_COL(1, i, cw_desc,  "Description");
  DRAW_COL(1, i, cw_tag,   "Tag");
  DRAW_COL(1, i, cw_mail,  "E-Mail");
  DRAW_COL(1, i, cw_conn,  "Connection");
  attroff(A_BOLD);

  // get or update the top row to make sure sel is visible
  GSequenceIter *n;
  int height = winrows-4;
  int row_last = g_sequence_iter_get_position(g_sequence_get_end_iter(tab->users))-1;
  int row_top = g_sequence_iter_get_position(tab->user_top);
  int row_sel = g_sequence_iter_get_position(tab->user_sel);
  // sel is before top? top = sel!
  if(row_top > row_sel)
    row_top = row_sel;
  // otherwise, is it out of the screen? top = sel - height
  else if(row_top <= row_sel-height)
    row_top = row_sel-height+1;
  // make sure there are no empty lines when len > height
  if(row_top && row_top+height+1 > row_last)
    row_top = MAX(0, row_last-height+1);
  tab->user_top = g_sequence_get_iter_at_pos(tab->users, row_top);

  n = tab->user_top;
  // TODO: status indicator? (OP/connected/active/passive/whatever)
  i = 2;
  while(i <= winrows-3 && !g_sequence_iter_is_end(n)) {
    struct nmdc_user *user = g_sequence_get(n);
    char *tag = user->tag ? g_strdup_printf("<%s>", user->tag) : NULL;
    int j=0;
    if(n == tab->user_sel) {
      attron(A_REVERSE);
      mvhline(i, 0, ' ', wincols);
    }
    DRAW_COL(i, j, cw_user,  user->name);
    DRAW_COL(i, j, cw_share, user->hasinfo ? str_formatsize(user->sharesize) : "");
    DRAW_COL(i, j, cw_desc,  user->desc?user->desc:"");
    DRAW_COL(i, j, cw_tag,   tag?tag:"");
    DRAW_COL(i, j, cw_mail,  user->mail?user->mail:"");
    DRAW_COL(i, j, cw_conn,  user->conn?user->conn:"");
    if(n == tab->user_sel)
      attroff(A_REVERSE);
    g_free(tag);
    i++;
    n = g_sequence_iter_next(n);
  }
}


static void ui_userlist_key(struct ui_tab *tab, struct input_key *key) {
  // page down
  if(key->type == INPT_KEY && key->code == KEY_NPAGE) {
    tab->user_sel = g_sequence_iter_move(tab->user_sel, winrows/2);
    if(g_sequence_iter_is_end(tab->user_sel))
      tab->user_sel = g_sequence_iter_prev(tab->user_sel);
  // page up
  } else if(key->type == INPT_KEY && key->code == KEY_PPAGE) {
    tab->user_sel = g_sequence_iter_move(tab->user_sel, -winrows/2);
    // workaround for https://bugzilla.gnome.org/show_bug.cgi?id=648313
    if(g_sequence_iter_is_end(tab->user_sel))
      tab->user_sel = g_sequence_get_begin_iter(tab->users);
  // item down
  } else if((key->type == INPT_KEY && key->code == KEY_DOWN)
      || (key->type == INPT_CHAR && key->code == 'j')) {
    tab->user_sel = g_sequence_iter_next(tab->user_sel);
    if(g_sequence_iter_is_end(tab->user_sel))
      tab->user_sel = g_sequence_iter_prev(tab->user_sel);
  // item up
  } else if((key->type == INPT_KEY && key->code == KEY_UP)
      || (key->type == INPT_CHAR && key->code == 'k')) {
    tab->user_sel = g_sequence_iter_prev(tab->user_sel);
  // home
  } else if(key->type == INPT_KEY && key->code == KEY_HOME) {
    tab->user_sel = g_sequence_get_begin_iter(tab->users);
  // end
  } else if(key->type == INPT_KEY && key->code == KEY_END) {
    tab->user_sel = g_sequence_iter_prev(g_sequence_get_end_iter(tab->users));
  }
}


void ui_userlist_joinquit(struct ui_tab *tab, gboolean join, struct nmdc_user *user) {
  if(join) {
    gboolean topisbegin = g_sequence_iter_is_begin(tab->user_top);
    gboolean selisbegin = g_sequence_iter_is_begin(tab->user_sel);
    GSequenceIter *iter = g_sequence_insert_sorted(tab->users, user, ui_userlist_sort_func, tab);
    // update top/sel in case they used to be begin but aren't anymore
    if(topisbegin != g_sequence_iter_is_begin(tab->user_top))
      tab->user_top = iter;
    if(selisbegin != g_sequence_iter_is_begin(tab->user_sel))
      tab->user_sel = iter;
  } else {
    // g_sequence_lookup() is what we want, but it's too new...
    GSequenceIter *iter = g_sequence_iter_prev(g_sequence_search(tab->users, user, ui_userlist_sort_func, tab));
    // update top/sel in case we are removing one of them
    if(iter == tab->user_top)
      tab->user_top = g_sequence_iter_prev(iter);
    if(iter == tab->user_top)
      tab->user_top = g_sequence_iter_next(iter);
    if(iter == tab->user_sel) {
      tab->user_sel = g_sequence_iter_next(iter);
      if(g_sequence_iter_is_end(tab->user_sel))
        tab->user_sel = g_sequence_iter_prev(iter);
    }
    g_sequence_remove(iter);
  }
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
  case UIT_MAIN:     ui_main_draw(); break;
  case UIT_HUB:      ui_hub_draw(curtab);  break;
  case UIT_USERLIST: ui_userlist_draw(curtab);  break;
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
    case UIT_MAIN:     ui_main_key(key); break;
    case UIT_HUB:      ui_hub_key(curtab, key); break;
    case UIT_USERLIST: ui_userlist_key(curtab, key); break;
    }
  }

  // TODO: some user feedback on invalid key
}

