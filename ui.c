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
  gboolean user_reverse;
  gboolean user_sort_share;
  gboolean user_hide_desc;
  gboolean user_hide_tag;
  gboolean user_hide_mail;
  gboolean user_hide_conn;
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


static void ui_main_key(guint64 key) {
  char *str = NULL;
  switch(key) {
  case INPT_KEY(KEY_NPAGE):
    ui_logwindow_scroll(ui_main->log, winrows/2);
    break;
  case INPT_KEY(KEY_PPAGE):
    ui_logwindow_scroll(ui_main->log, -winrows/2);
    break;
  default:
    if(ui_textinput_key(ui_global_textinput, key, &str) && str) {
      cmd_handle(str);
      g_free(str);
    }
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


static void ui_hub_key(struct ui_tab *tab, guint64 key) {
  char *str = NULL;
  switch(key) {
  case INPT_KEY(KEY_NPAGE):
    ui_logwindow_scroll(tab->log, winrows/2);
    break;
  case INPT_KEY(KEY_PPAGE):
    ui_logwindow_scroll(tab->log, -winrows/2);
    break;
  default:
    if(ui_textinput_key(ui_global_textinput, key, &str) && str) {
      cmd_handle(str);
      g_free(str);
    }
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

// TODO: sort OPs before normal users?
static gint ui_userlist_sort_func(gconstpointer da, gconstpointer db, gpointer dat) {
  const struct nmdc_user *a = da;
  const struct nmdc_user *b = db;
  struct ui_tab *tab = dat;
  int o = 0;
  if(!o && tab->user_sort_share)
    o = a->sharesize > b->sharesize ? 1 : -1;
  if(!o) // TODO: this is noticably slow on large hubs, cache g_utf8_collate_key()
    o = g_utf8_collate(a->name, b->name);
  if(!o)
    o = strcmp(a->name_hub, b->name_hub);
  if(!o)
    o = a > b ? 1 : -1;
  return tab->user_reverse ? -1*o : o;
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
    u->iter = g_sequence_insert_sorted(tab->users, u, ui_userlist_sort_func, tab);
  tab->user_sel = tab->user_top = g_sequence_get_begin_iter(tab->users);
  return tab;
}


void ui_userlist_close(struct ui_tab *tab) {
  tab->hub->tab->userlist_tab = NULL;
  ui_tab_remove(tab);
  // To clean things up, we should also reset all nmdc_user->iter fields. But
  // this isn't all that necessary since they won't be used anymore until they
  // get reset in a subsequent ui_userlist_create().
  g_sequence_free(tab->users);
  g_free(tab->name);
  g_free(tab);
}


static char *ui_userlist_title(struct ui_tab *tab) {
  return g_strdup_printf("%s / User list", tab->hub->tab->name);
}


#define DRAW_COL(row, colvar, width, str) do {\
    if(width > 1)\
      mvaddnstr(row, colvar, str, str_offset_from_columns(str, width-1));\
    colvar += width;\
  } while(0)

// TODO: some way of letting the user know what keys can be pressed
static void ui_userlist_draw(struct ui_tab *tab) {
  // column widths (this is a trial-and-error-whatever-looks-right algorithm)
  int num = 2 + (tab->user_hide_conn?0:1) + (tab->user_hide_desc?0:1) + (tab->user_hide_tag?0:1) + (tab->user_hide_mail?0:1);
  int cw_user = MAX(20, (wincols*6)/(num*10));
  int cw_share = 12;
  int i = wincols-cw_user-cw_share; num -= 2; // remaining number of columns
  int cw_conn = tab->user_hide_conn ? 0 : (i*6)/(num*10);
  int cw_desc = tab->user_hide_desc ? 0 : (i*10)/(num*10);
  int cw_mail = tab->user_hide_mail ? 0 : (i*7)/(num*10);
  int cw_tag  = tab->user_hide_tag  ? 0 : i-cw_conn-cw_desc-cw_mail;

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
  int height = winrows-5;
  int row_last = g_sequence_iter_get_position(g_sequence_get_end_iter(tab->users));
  int row_top = g_sequence_iter_get_position(tab->user_top);
  int row_sel = g_sequence_iter_get_position(tab->user_sel);
  // sel is before top? top = sel!
  if(row_top > row_sel)
    row_top = row_sel;
  // otherwise, is it out of the screen? top = sel - height
  else if(row_top <= row_sel-height)
    row_top = row_sel-height+1;
  // make sure there are no empty lines when len > height
  if(row_top && row_top+height > row_last)
    row_top = MAX(0, row_last-height);
  tab->user_top = g_sequence_get_iter_at_pos(tab->users, row_top);

  // TODO: status indicator? (OP/connected/active/passive/whatever)
  n = tab->user_top;
  i = 2;
  while(i <= winrows-4 && !g_sequence_iter_is_end(n)) {
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

  // footer
  attron(A_BOLD);
  int count = g_hash_table_size(tab->hub->users);
  mvaddstr(winrows-3, 0, "Totals:");
  char *tmp = g_strdup_printf("%s%c   %d users",
    str_formatsize(tab->hub->sharesize), tab->hub->sharecount == count ? ' ' : '+', count);
  mvaddstr(winrows-3, cw_user, tmp);
  g_free(tmp);
  tmp = g_strdup_printf("%3d%%", MIN(100, (row_top+height)*100/row_last));
  mvaddstr(winrows-3, wincols-6, tmp);
  g_free(tmp);
  attroff(A_BOLD);
}


static void ui_userlist_key(struct ui_tab *tab, guint64 key) {
  gboolean sort = FALSE;
  switch(key) {
  case INPT_KEY(KEY_NPAGE): // page down
    tab->user_sel = g_sequence_iter_move(tab->user_sel, winrows/2);
    if(g_sequence_iter_is_end(tab->user_sel))
      tab->user_sel = g_sequence_iter_prev(tab->user_sel);
    break;
  case INPT_KEY(KEY_PPAGE): // page up
    tab->user_sel = g_sequence_iter_move(tab->user_sel, -winrows/2);
    // workaround for https://bugzilla.gnome.org/show_bug.cgi?id=648313
    if(g_sequence_iter_is_end(tab->user_sel))
      tab->user_sel = g_sequence_get_begin_iter(tab->users);
    break;
  case INPT_KEY(KEY_DOWN): // item down
  case INPT_CHAR('j'):
    tab->user_sel = g_sequence_iter_next(tab->user_sel);
    if(g_sequence_iter_is_end(tab->user_sel))
      tab->user_sel = g_sequence_iter_prev(tab->user_sel);
    break;
  case INPT_KEY(KEY_UP): // item up
  case INPT_CHAR('k'):
    tab->user_sel = g_sequence_iter_prev(tab->user_sel);
    break;
  case INPT_KEY(KEY_HOME): // home
    tab->user_sel = g_sequence_get_begin_iter(tab->users);
    break;
  case INPT_KEY(KEY_END): // end
    tab->user_sel = g_sequence_iter_prev(g_sequence_get_end_iter(tab->users));
    break;
  case INPT_CHAR('s'): // s (order by share asc/desc)
    if(tab->user_sort_share)
      tab->user_reverse = !tab->user_reverse;
    else
      tab->user_sort_share = tab->user_reverse = TRUE;
    sort = TRUE;
    break;
  case INPT_CHAR('u'): // u (order by username asc/desc)
    if(!tab->user_sort_share)
      tab->user_reverse = !tab->user_reverse;
    else
      tab->user_sort_share = tab->user_reverse = FALSE;
    sort = TRUE;
    break;
  case INPT_CHAR('d'): // d (toggle description visibility)
    tab->user_hide_desc = !tab->user_hide_desc;
    break;
  case INPT_CHAR('t'): // t (toggle tag visibility)
    tab->user_hide_tag = !tab->user_hide_tag;
    break;
  case INPT_CHAR('e'): // e (toggle e-mail visibility)
    tab->user_hide_mail = !tab->user_hide_mail;
    break;
  case INPT_CHAR('c'): // c (toggle connection visibility)
    tab->user_hide_conn = !tab->user_hide_conn;
    break;
  }

  // TODO: some way to save the column visibility? per hub? global default?

  if(sort) {
    gboolean selisbegin = g_sequence_iter_is_begin(tab->user_sel);
    g_sequence_sort(tab->users, ui_userlist_sort_func, tab);
    if(selisbegin)
      tab->user_sel = g_sequence_get_begin_iter(tab->users);
    ui_msgf(FALSE, "Ordering by %s (%s)", tab->user_sort_share ? "share size" : "nick",
      tab->user_reverse ? "descending" : "ascending");
  }
}


void ui_userlist_joinquit(struct ui_tab *tab, gboolean join, struct nmdc_user *user) {
  if(join) {
    gboolean topisbegin = g_sequence_iter_is_begin(tab->user_top);
    gboolean selisbegin = g_sequence_iter_is_begin(tab->user_sel);
    user->iter = g_sequence_insert_sorted(tab->users, user, ui_userlist_sort_func, tab);
    // update top/sel in case they used to be begin but aren't anymore
    if(topisbegin != g_sequence_iter_is_begin(tab->user_top))
      tab->user_top = user->iter;
    if(selisbegin != g_sequence_iter_is_begin(tab->user_sel))
      tab->user_sel = user->iter;
  } else {
    g_assert(g_sequence_get(user->iter) == (gpointer)user);
    // update top/sel in case we are removing one of them
    if(user->iter == tab->user_top)
      tab->user_top = g_sequence_iter_prev(user->iter);
    if(user->iter == tab->user_top)
      tab->user_top = g_sequence_iter_next(user->iter);
    if(user->iter == tab->user_sel) {
      tab->user_sel = g_sequence_iter_next(user->iter);
      if(g_sequence_iter_is_end(tab->user_sel))
        tab->user_sel = g_sequence_iter_prev(user->iter);
    }
    g_sequence_remove(user->iter);
  }
}


void ui_userlist_userupdate(struct ui_tab *tab, struct nmdc_user *user) {
  g_sequence_sort_changed(user->iter, ui_userlist_sort_func, tab);
}





// Generic message displaying thing.

static char *ui_msg_text = NULL;
static guint ui_msg_timer;
static gboolean ui_msg_updated = FALSE;

struct ui_msg_dat { char *msg; gboolean global; };


static gboolean ui_msg_timeout(gpointer data) {
  ui_msg(FALSE, NULL);
  return FALSE;
}


static gboolean ui_msg_mainthread(gpointer dat) {
  struct ui_msg_dat *msg = dat;
  struct ui_tab *tab = ui_tab_cur->data;
  if(ui_msg_text) {
    g_free(ui_msg_text);
    ui_msg_text = NULL;
    g_source_remove(ui_msg_timer);
    ui_msg_updated = TRUE;
  }
  if(msg->msg) {
    if(!msg->global && tab->log)
      ui_logwindow_add(tab->log, msg->msg);
    if(msg->global)
      ui_logwindow_add(ui_main->log, msg->msg);
    if(msg->global || !tab->log) {
      ui_msg_text = g_strdup(msg->msg);
      ui_msg_timer = g_timeout_add(3000, ui_msg_timeout, NULL);
      ui_msg_updated = TRUE;
    }
  }
  g_free(msg->msg);
  g_free(msg);
  return FALSE;
}


// a notication message, either displayed in the log of the current tab or, if
// the hub has no tab, in the "status bar". Calling this function with NULL
// will reset the status bar message. Unlike everything else, this function can
// be called from any thread. (It will queue an idle function, after all)
void ui_msg(gboolean global, char *msg) {
  struct ui_msg_dat *dat = g_new0(struct ui_msg_dat, 1);
  dat->msg = g_strdup(msg);
  dat->global = global;
  g_idle_add_full(G_PRIORITY_HIGH_IDLE, ui_msg_mainthread, dat, NULL);
}


void ui_msgf(gboolean global, const char *fmt, ...) {
  va_list va;
  va_start(va, fmt);
  char *str = g_strdup_vprintf(fmt, va);
  va_end(va);
  ui_msg(global, str);
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


void ui_input(guint64 key) {
  struct ui_tab *curtab = ui_tab_cur->data;

  switch(key) {
  case INPT_CTRL(3): // ctrl+c
    g_main_loop_quit(main_loop);
    break;
  case INPT_ALT('j'): // alt+j (previous tab)
    ui_tab_cur = ui_tab_cur->prev ? ui_tab_cur->prev : g_list_last(ui_tabs);
    break;
  case INPT_ALT('k'): // alt+k (next tab)
    ui_tab_cur = ui_tab_cur->next ? ui_tab_cur->next : ui_tabs;
    break;
  case INPT_ALT('h'): ; // alt+h (swap tab with leff)
    GList *prev = ui_tab_cur->prev;
    ui_tabs = g_list_delete_link(ui_tabs, ui_tab_cur);
    ui_tabs = g_list_insert_before(ui_tabs, prev, curtab);
    ui_tab_cur = prev->prev;
    break;
  case INPT_ALT('l'): ; // alt+l (swap tab with right)
    GList *next = ui_tab_cur->next;
    ui_tabs = g_list_delete_link(ui_tabs, ui_tab_cur);
    ui_tabs = g_list_insert_before(ui_tabs, next->next, curtab);
    ui_tab_cur = next->next;
    break;
  case INPT_ALT('c'): // alt+c (alias for /close)
    cmd_handle("/close");
    break;
  case INPT_CTRL(12): // ctrl+l (alias for /close)
    cmd_handle("/clear");
    break;
  default:
    // alt+num (switch tab)
    if(key >= INPT_ALT('0') && key <= INPT_ALT('9')) {
      GList *n = g_list_nth(ui_tabs, INPT_CODE(key) == '0' ? 9 : INPT_CODE(key)-'1');
      if(n)
        ui_tab_cur = n;
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
}

