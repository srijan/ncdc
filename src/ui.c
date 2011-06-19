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
#include <math.h>


#if INTERFACE

// These are assumed to occupy two bits
#define UIP_EMPTY 0 // no change
#define UIP_LOW   1 // system messages
#define UIP_MED   2 // chat messages, or error messages in the main tab
#define UIP_HIGH  3 // direct messages to you (PM or name mentioned)

#define UIT_MAIN     0
#define UIT_HUB      1
#define UIT_USERLIST 2
#define UIT_MSG      3
#define UIT_CONN     4

struct ui_tab {
  int type; // UIT_ type
  int prio; // UIP_ type
  char *name;
  struct ui_logwindow *log;    // MAIN, HUB, MSG
  struct nmdc_hub *hub;        // HUB, USERLIST, MSG
  struct ui_listing *list;     // USERLIST, CONN
  // HUB
  struct ui_tab *userlist_tab;
  gboolean hub_joincomplete;
  // MSG
  struct nmdc_user *msg_user;
  char *msg_uname;
  // USERLIST
  gboolean user_details;
  gboolean user_reverse;
  gboolean user_sort_share;
  gboolean user_opfirst;
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

gboolean ui_beep = FALSE; // set to true anywhere to send a beep




// Main tab


// these is only one main tab, so this can be static
struct ui_tab *ui_main;

static struct ui_tab *ui_main_create() {
  ui_main = g_new0(struct ui_tab, 1);
  ui_main->name = "main";
  ui_main->log = ui_logwindow_create("main");
  ui_main->type = UIT_MAIN;

  ui_mf(ui_main, 0, "Welcome to ncdc %s!", VERSION);
  ui_mf(ui_main, 0, "Using working directory: %s", conf_dir);

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
  if(!ui_logwindow_key(ui_main->log, key, winrows) &&
      ui_textinput_key(ui_global_textinput, key, &str) && str) {
    cmd_handle(str);
    g_free(str);
  }
}





// User message tab

struct ui_tab *ui_msg_create(struct nmdc_hub *hub, struct nmdc_user *user) {
  struct ui_tab *tab = g_new0(struct ui_tab, 1);
  tab->type = UIT_MSG;
  tab->hub = hub;
  tab->msg_user = user;
  tab->msg_uname = g_strdup(user->name);
  tab->name = g_strdup_printf("~%s", user->name);
  tab->log = ui_logwindow_create(tab->name);

  ui_mf(tab, 0, "Chatting with %s on %s.", user->name, hub->tab->name);

  return tab;
}


void ui_msg_close(struct ui_tab *tab) {
  ui_tab_remove(tab);
  ui_logwindow_free(tab->log);
  g_free(tab->name);
  g_free(tab->msg_uname);
  g_free(tab);
}


static void ui_msg_draw(struct ui_tab *tab) {
  ui_logwindow_draw(tab->log, 1, 0, winrows-4, wincols);

  mvaddstr(winrows-3, 0, tab->name);
  addstr("> ");
  int pos = str_columns(tab->name)+2;
  ui_textinput_draw(ui_global_textinput, winrows-3, pos, wincols-pos);
}


static char *ui_msg_title(struct ui_tab *tab) {
  return g_strdup_printf("Chatting with %s on %s%s.",
      tab->msg_uname, tab->hub->tab->name, tab->msg_user ? "" : " (disabled)");
}


static void ui_msg_key(struct ui_tab *tab, guint64 key) {
  char *str = NULL;
  if(!ui_logwindow_key(tab->log, key, winrows) &&
      ui_textinput_key(ui_global_textinput, key, &str) && str) {
    cmd_handle(str);
    g_free(str);
  }
}


static void ui_msg_msg(struct ui_tab *tab, const char *msg) {
  ui_m(tab, UIP_HIGH, msg);
}


static void ui_msg_joinquit(struct ui_tab *tab, gboolean join, struct nmdc_user *user) {
  if(join) {
    ui_mf(tab, 0, "%s has joined.", user->name);
    tab->msg_user = user;
  } else {
    ui_mf(tab, 0, "%s has quit.", user->name);
    tab->msg_user = NULL;
  }
}





// Hub tab

#if INTERFACE
// change types for ui_hub_userchange()
#define UIHUB_UC_JOIN 0
#define UIHUB_UC_QUIT 1
#define UIHUB_UC_NFO 2
#endif


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


void ui_hub_close(struct ui_tab *tab) {
  // close the userlist tab
  if(tab->userlist_tab)
    ui_userlist_close(tab->userlist_tab);
  // close msg tabs
  GList *t;
  for(t=ui_tabs; t;) {
    if(((struct ui_tab *)t->data)->type == UIT_MSG && ((struct ui_tab *)t->data)->hub == tab->hub) {
      GList *n = t->next;
      ui_msg_close(t->data);
      t = n;
    } else
      t = t->next;
  }
  // remove ourself from the list
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
  if(!ui_logwindow_key(tab->log, key, winrows) &&
      ui_textinput_key(ui_global_textinput, key, &str) && str) {
    cmd_handle(str);
    g_free(str);
  } else if(key == INPT_ALT('u'))
    ui_hub_userlist_open(tab);
}


struct ui_tab *ui_hub_getmsg(struct ui_tab *tab, struct nmdc_user *user) {
  // This is slow when many tabs are open, should be improved...
  GList *t;
  struct ui_tab *mt;
  for(t=ui_tabs; t; t=t->next) {
    mt = t->data;
    if(mt->type == UIT_MSG && mt->hub == tab->hub && (mt->msg_user == user || strcmp(mt->msg_uname, user->name) == 0))
      return mt;
  }
  return NULL;
}


void ui_hub_userchange(struct ui_tab *tab, int change, struct nmdc_user *user) {
  // notify msg tab, if any
  if(change == UIHUB_UC_JOIN || change == UIHUB_UC_QUIT) {
    struct ui_tab *t = ui_hub_getmsg(tab, user);
    if(t)
      ui_msg_joinquit(t, change == UIHUB_UC_JOIN, user);
  }

  // notify the userlist, when it is open
  if(tab->userlist_tab)
    ui_userlist_userchange(tab->userlist_tab, change, user);

  // display the join/quit, when requested
  gboolean log = conf_hub_get(boolean, tab->name, "show_joinquit");
  if(change == UIHUB_UC_NFO && !user->isjoined) {
    user->isjoined = TRUE;
    if(log && tab->hub->joincomplete
        && (!tab->hub->nick_valid || strcmp(tab->hub->nick_hub, user->name_hub) != 0))
      ui_mf(tab, 0, "%s has joined.", user->name);
  } else if(change == UIHUB_UC_QUIT && log)
    ui_mf(tab, 0, "%s has quit.", user->name);
}


void ui_hub_msg(struct ui_tab *tab, struct nmdc_user *user, const char *msg) {
  struct ui_tab *t = ui_hub_getmsg(tab, user);
  if(!t) {
    t = ui_msg_create(tab->hub, user);
    GList *old = ui_tab_cur;
    ui_tab_open(t);
    ui_tab_cur = old;
  }
  ui_msg_msg(t, msg);
}


void ui_hub_userlist_open(struct ui_tab *tab) {
  if(tab->userlist_tab)
    ui_tab_cur = g_list_find(ui_tabs, tab->userlist_tab);
  else {
    tab->userlist_tab = ui_userlist_create(tab->hub);
    ui_tab_open(tab->userlist_tab);
  }
}





// Userlist tab

static gint ui_userlist_sort_func(gconstpointer da, gconstpointer db, gpointer dat) {
  const struct nmdc_user *a = da;
  const struct nmdc_user *b = db;
  struct ui_tab *tab = dat;
  int o = 0;
  if(!o && tab->user_opfirst && !a->isop != !b->isop)
    return a->isop && !b->isop ? -1 : 1;
  if(!o && tab->user_sort_share)
    o = a->sharesize > b->sharesize ? 1 : a->sharesize < b->sharesize ? -1 : 0;
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
  tab->name = g_strdup_printf("@%s", hub->tab->name+1);
  tab->type = UIT_USERLIST;
  tab->hub = hub;
  tab->user_opfirst = TRUE;
  tab->user_hide_conn = TRUE;
  tab->user_hide_mail = TRUE;
  GSequence *users = g_sequence_new(NULL);
  // populate the list
  // g_sequence_sort() uses insertion sort? in that case it is faster to insert
  // all items using g_sequence_insert_sorted() rather than inserting them in
  // no particular order and then sorting them in one go. (which is faster for
  // linked lists, since it uses a faster sorting algorithm)
  GHashTableIter iter;
  g_hash_table_iter_init(&iter, hub->users);
  struct nmdc_user *u;
  while(g_hash_table_iter_next(&iter, NULL, (gpointer *)&u))
    u->iter = g_sequence_insert_sorted(users, u, ui_userlist_sort_func, tab);
  tab->list = ui_listing_create(users);
  return tab;
}


void ui_userlist_close(struct ui_tab *tab) {
  tab->hub->tab->userlist_tab = NULL;
  ui_tab_remove(tab);
  // To clean things up, we should also reset all nmdc_user->iter fields. But
  // this isn't all that necessary since they won't be used anymore until they
  // get reset in a subsequent ui_userlist_create().
  g_sequence_free(tab->list->list);
  ui_listing_free(tab->list);
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


struct ui_userlist_draw_opts {
  int cw_user, cw_share, cw_conn, cw_desc, cw_mail, cw_tag;
};


static void ui_userlist_draw_row(struct ui_listing *list, GSequenceIter *iter, int row, void *dat) {
  struct nmdc_user *user = g_sequence_get(iter);
  struct ui_userlist_draw_opts *o = dat;

  char *tag = user->tag ? g_strdup_printf("<%s>", user->tag) : NULL;
  int j=2;
  if(iter == list->sel) {
    attron(A_BOLD);
    mvaddstr(row, 0, ">");
    attroff(A_BOLD);
  }
  DRAW_COL(row, j, o->cw_user,  user->name);
  DRAW_COL(row, j, o->cw_share, user->hasinfo ? str_formatsize(user->sharesize) : "");
  DRAW_COL(row, j, o->cw_desc,  user->desc?user->desc:"");
  DRAW_COL(row, j, o->cw_tag,   tag?tag:"");
  DRAW_COL(row, j, o->cw_mail,  user->mail?user->mail:"");
  DRAW_COL(row, j, o->cw_conn,  user->conn?user->conn:"");
  g_free(tag);
}


// TODO: some way of letting the user know what keys can be pressed
static void ui_userlist_draw(struct ui_tab *tab) {
  char tmp[201];
  // column widths (this is a trial-and-error-whatever-looks-right algorithm)
  struct ui_userlist_draw_opts o;
  int num = 2 + (tab->user_hide_conn?0:1) + (tab->user_hide_desc?0:1) + (tab->user_hide_tag?0:1) + (tab->user_hide_mail?0:1);
  o.cw_user = MAX(20, (wincols*6)/(num*10));
  o.cw_share = 12;
  int i = wincols-o.cw_user-o.cw_share-2; num -= 2; // remaining number of columns
  o.cw_conn = tab->user_hide_conn ? 0 : (i*6)/(num*10);
  o.cw_desc = tab->user_hide_desc ? 0 : (i*10)/(num*10);
  o.cw_mail = tab->user_hide_mail ? 0 : (i*7)/(num*10);
  o.cw_tag  = tab->user_hide_tag  ? 0 : i-o.cw_conn-o.cw_desc-o.cw_mail;

  // header
  i = 2;
  attron(A_BOLD);
  DRAW_COL(1, i, o.cw_user,  "Username");
  DRAW_COL(1, i, o.cw_share, "Share");
  DRAW_COL(1, i, o.cw_desc,  "Description");
  DRAW_COL(1, i, o.cw_tag,   "Tag");
  DRAW_COL(1, i, o.cw_mail,  "E-Mail");
  DRAW_COL(1, i, o.cw_conn,  "Connection");
  attroff(A_BOLD);

  // rows
  int bottom = tab->user_details ? winrows-7 : winrows-3;
  int pos = ui_listing_draw(tab->list, 2, bottom-1, ui_userlist_draw_row, &o);

  // footer
  attron(A_REVERSE);
  mvhline(bottom, 0, ' ', wincols);
  int count = g_hash_table_size(tab->hub->users);
  mvaddstr(bottom, 0, "Totals:");
  g_snprintf(tmp, 200, "%s%c   %d users",
    str_formatsize(tab->hub->sharesize), tab->hub->sharecount == count ? ' ' : '+', count);
  mvaddstr(bottom, o.cw_user+2, tmp);
  g_snprintf(tmp, 200, "%3d%%", pos);
  mvaddstr(bottom, wincols-6, tmp);
  attroff(A_REVERSE);

  // detailed info box
  if(!tab->user_details)
    return;
  if(g_sequence_iter_is_end(tab->list->sel))
    mvaddstr(bottom+1, 2, "No user selected.");
  else {
    struct nmdc_user *u = g_sequence_get(tab->list->sel);
    attron(A_BOLD);
    mvaddstr(bottom+1,  8, "Username:");
    mvaddstr(bottom+1, 45, "Share:");
    mvaddstr(bottom+2,  6, "Connection:");
    mvaddstr(bottom+2, 44, "E-Mail:");
    mvaddstr(bottom+3,  1, "Description/tag:");
    attroff(A_BOLD);
    mvaddstr(bottom+1, 18, u->name);
    if(u->hasinfo)
      g_snprintf(tmp, 200, "%s (%s bytes)", str_formatsize(u->sharesize), str_fullsize(u->sharesize));
    else
      strcpy(tmp, "-");
    mvaddstr(bottom+1, 52, tmp);
    mvaddstr(bottom+2, 18, u->hasinfo ? u->conn : "-");
    mvaddstr(bottom+2, 52, u->hasinfo ? u->mail : "-");
    if(u->hasinfo && u->tag)
      g_snprintf(tmp, 200, "%s <%s>", u->desc?u->desc:"", u->tag);
    else if(u->desc)
      strncpy(tmp, u->desc, 200);
    else
      strcpy(tmp, "-");
    mvaddstr(bottom+3, 18, tmp);
  }
}
#undef DRAW_COL


static void ui_userlist_key(struct ui_tab *tab, guint64 key) {
  if(ui_listing_key(tab->list, key, winrows/2))
    return;

  gboolean sort = FALSE;
  switch(key) {
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
  case INPT_CHAR('o'): // o (toggle sorting OPs before others)
    tab->user_opfirst = !tab->user_opfirst;
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
  case INPT_CTRL('j'): // newline
  case INPT_CHAR('i'): // i       (toggle user info)
    tab->user_details = !tab->user_details;
    break;
  case INPT_CHAR('m'): // m (/msg user)
    if(!g_sequence_iter_is_end(tab->list->sel)) {
      struct nmdc_user *u = g_sequence_get(tab->list->sel);
      struct ui_tab *t = ui_hub_getmsg(tab, u);
      if(!t) {
        t = ui_msg_create(tab->hub, u);
        ui_tab_open(t);
      } else
        ui_tab_cur = g_list_find(ui_tabs, t);
    } else
      ui_m(NULL, 0, "No user selected.");
    break;
  }

  // TODO: some way to save the column visibility? per hub? global default?

  if(sort) {
    g_sequence_sort(tab->list->list, ui_userlist_sort_func, tab);
    ui_listing_sorted(tab->list);
    ui_mf(NULL, 0, "Ordering by %s (%s%s)", tab->user_sort_share ? "share size" : "user name",
      tab->user_reverse ? "descending" : "ascending", tab->user_opfirst ? ", OPs first" : "");
  }
}


void ui_userlist_userchange(struct ui_tab *tab, int change, struct nmdc_user *user) {
  if(change == UIHUB_UC_JOIN) {
    user->iter = g_sequence_insert_sorted(tab->list->list, user, ui_userlist_sort_func, tab);
    ui_listing_inserted(tab->list);
  } else if(change == UIHUB_UC_QUIT) {
    g_assert(g_sequence_get(user->iter) == (gpointer)user);
    ui_listing_remove(tab->list, user->iter);
    g_sequence_remove(user->iter);
  } else {
    g_sequence_sort_changed(user->iter, ui_userlist_sort_func, tab);
    ui_listing_sorted(tab->list);
  }
}




// these can only be one connections tab, so this can be static
struct ui_tab *ui_conn;


static gint ui_conn_sort_func(gconstpointer da, gconstpointer db, gpointer dat) {
  const struct nmdc_cc *a = da;
  const struct nmdc_cc *b = db;
  int o = 0;
  if(!o && !a->nick != !b->nick)
    o = a->nick ? 1 : -1;
  if(!o && a->nick && b->nick)
    o = strcmp(a->nick, b->nick);
  if(!o && a->hub && b->hub)
    o = strcmp(a->hub->tab->name, b->hub->tab->name);
  return o;
}


struct ui_tab *ui_conn_create() {
  ui_conn = g_new0(struct ui_tab, 1);
  ui_conn->name = "connections";
  ui_conn->type = UIT_CONN;
  // sort the connection list
  g_sequence_sort(nmdc_cc_list, ui_conn_sort_func, NULL);
  ui_conn->list = ui_listing_create(nmdc_cc_list);
  return ui_conn;
}


void ui_conn_close() {
  ui_tab_remove(ui_conn);
  ui_listing_free(ui_conn->list);
  g_free(ui_conn);
  ui_conn = NULL;
}


#if INTERFACE
#define UICONN_ADD 0
#define UICONN_DEL 1
#define UICONN_MOD 2
#endif


void ui_conn_listchange(GSequenceIter *iter, int change) {
  g_return_if_fail(ui_conn);
  switch(change) {
  case UICONN_ADD:
    ui_listing_inserted(ui_conn->list);
    break;
  case UICONN_DEL:
    ui_listing_remove(ui_conn->list, iter);
    break;
  case UICONN_MOD:
    g_sequence_sort_changed(iter, ui_conn_sort_func, NULL);
    ui_listing_sorted(ui_conn->list);
    break;
  }
}


static char *ui_conn_title() {
  return g_strdup("Connection list");
}


static void ui_conn_draw_row(struct ui_listing *list, GSequenceIter *iter, int row, void *dat) {
  struct nmdc_cc *cc = g_sequence_get(iter);
  char tmp[100];
  if(iter == list->sel) {
    attron(A_BOLD);
    mvaddch(row, 0, '>');
    attroff(A_BOLD);
  }

  mvaddch(row, 2,
    !cc->nick          ? 'C' : // connecting
    cc->timeout_src    ? 'D' : // disconnected
    cc->net->file_left ? 'U' : // uploading
                         'I'); // idle

  if(cc->nick)
    mvaddnstr(row, 4, cc->nick, str_offset_from_columns(cc->nick, 19));
  else {
    g_snprintf(tmp, 99, "IP:%s", cc->remoteaddr);
    mvaddstr(row, 4, tmp);
  }

  if(cc->hub)
    mvaddnstr(row, 24, cc->hub->tab->name, str_offset_from_columns(cc->hub->tab->name, 14));

  if(cc->timeout_src)
    strcpy(tmp, "     -");
  else
    g_snprintf(tmp, 99, "%6d", ratecalc_get(cc->net->rate_out)/1024);
  mvaddstr(row, 46, tmp);

  // TODO: progress bar or something?

  if(cc->err) {
    mvaddstr(row, 57, "Disconnected: ");
    addnstr(cc->err->message, str_offset_from_columns(cc->err->message, wincols-72));
  } else if(cc->last_file) {
    char *file = rindex(cc->last_file, '/');
    if(file)
      file++;
    else
      file = cc->last_file;
      mvaddnstr(row, 57, file, str_offset_from_columns(file, wincols-57));
  }
}


static void ui_conn_draw() {
  char tmp[100];
  attron(A_BOLD);
  mvaddstr(1, 2,  "S Username");
  mvaddstr(1, 24, "Hub");
  mvaddstr(1, 46, "KiB/s");
  mvaddstr(1, 57, "File");
  attroff(A_BOLD);

  int bottom = winrows-11;
  int pos = ui_listing_draw(ui_conn->list, 2, bottom-1, ui_conn_draw_row, NULL);

  // footer
  attron(A_REVERSE);
  mvhline(bottom, 0, ' ', wincols);
  g_snprintf(tmp, 99, "%3d connections    %3d%%", g_sequence_iter_get_position(g_sequence_get_end_iter(ui_conn->list->list)), pos);
  mvaddstr(bottom, wincols-24, tmp);
  attroff(A_REVERSE);

  // detailed info
  struct nmdc_cc *cc = g_sequence_iter_is_end(ui_conn->list->sel) ? NULL : g_sequence_get(ui_conn->list->sel);
  if(!cc) {
    mvaddstr(bottom+1, 0, "Nothing selected.");
    return;
  }
  attron(A_BOLD);
  mvaddstr(bottom+1,  1, "Username:");
  mvaddstr(bottom+1, 40, "Hub:");
  mvaddstr(bottom+2,  2, "IP/port:");
  mvaddstr(bottom+2, 37, "Status:");
  mvaddstr(bottom+3,  7, "Up:");
  mvaddstr(bottom+3, 39, "Down:");
  mvaddstr(bottom+4,  5, "File:");
  attroff(A_BOLD);
  mvaddstr(bottom+1, 11, cc->nick ? cc->nick : "Unknown / connecting");
  mvaddstr(bottom+1, 45, cc->hub ? cc->hub->tab->name : "-");
  mvaddstr(bottom+2, 11, cc->remoteaddr);
  mvaddstr(bottom+2, 45,
    !cc->nick          ? "Connecting" :
    cc->timeout_src    ? "Disconnected" :
    cc->net->file_left ? "Uploading" : "Idle");
  g_snprintf(tmp, 99, "%d KiB/s (%s)", ratecalc_get(cc->net->rate_out)/1024, str_formatsize(cc->net->rate_out->total));
  mvaddstr(bottom+3, 11, tmp);
  g_snprintf(tmp, 99, "%d KiB/s (%s)", ratecalc_get(cc->net->rate_in)/1024, str_formatsize(cc->net->rate_in->total));
  mvaddstr(bottom+3, 45, tmp);
  if(cc->last_file)
    mvaddnstr(bottom+4, 11, cc->last_file, str_offset_from_columns(cc->last_file, wincols-12));
  else
    mvaddstr(bottom+4, 11, "None.");
  // TODO: more info (chunk size, file size, progress, error info, idle time)
}


static void ui_conn_key(guint64 key) {
  if(ui_listing_key(ui_conn->list, key, (winrows-10)/2))
    return;
}






// Generic message displaying thing.

#if INTERFACE

// These flags can be OR'ed together with UIP_ flags. No UIP_ flag or UIP_EMPTY
// implies UIP_LOW. There is no need to set any priority when tab == NULL,
// since it will be displayed right away anyway.

// Message should also be notified in status bar (implied automatically if the
// requested tab has no log window). This also uses UIP_EMPTY if no other UIP_
// flag is set.
#define UIM_NOTIFY 4
// Ownership of the message string is passed to the message handling function.
// (Which fill g_free() it after use)
#define UIM_PASS   8
// This is a chat message, i.e. check to see if your name is part of the
// message, and if so, give it UIP_HIGH.
#define UIM_CHAT  16

#endif


static char *ui_m_text = NULL;
static guint ui_m_timer;
static gboolean ui_m_updated = FALSE;

struct ui_m_dat {
  char *msg;
  struct ui_tab *tab;
  int flags;
};


static gboolean ui_m_timeout(gpointer data) {
  if(ui_m_text) {
    g_free(ui_m_text);
    ui_m_text = NULL;
    g_source_remove(ui_m_timer);
    ui_m_updated = TRUE;
  }
  return FALSE;
}


static gboolean ui_m_mainthread(gpointer dat) {
  struct ui_m_dat *msg = dat;
  struct ui_tab *tab = msg->tab;
  int prio = msg->flags & 3; // lower two bits
  if(!tab)
    tab = ui_tab_cur->data;
  // It can happen that the tab is closed while we were waiting for this idle
  // function to be called, so check whether it's still in the list.
  // TODO: Performance can be improved by using a reference counter or so.
  else if(!g_list_find(ui_tabs, tab))
    goto ui_m_cleanup;

  gboolean notify = (msg->flags & UIM_NOTIFY) || !tab->log;

  if(notify && ui_m_text) {
    g_free(ui_m_text);
    ui_m_text = NULL;
    g_source_remove(ui_m_timer);
    ui_m_updated = TRUE;
  }
  if(notify) {
    ui_m_text = g_strdup(msg->msg);
    ui_m_timer = g_timeout_add(3000, ui_m_timeout, NULL);
    ui_m_updated = TRUE;
  }
  if(tab->log) {
    if((msg->flags & UIM_CHAT) && tab->type == UIT_HUB && tab->hub->nick_valid) {
      char *name = g_regex_escape_string(tab->hub->nick, -1);
      char *pattern = g_strdup_printf("\\b%s\\b", name);
      if(g_regex_match_simple(pattern, msg->msg, G_REGEX_CASELESS, 0))
        prio = UIP_HIGH;
      g_free(name);
      g_free(pattern);
    }
    ui_logwindow_add(tab->log, msg->msg);
    tab->prio = MAX(tab->prio, MAX(prio, notify ? UIP_EMPTY : UIP_LOW));
  }

ui_m_cleanup:
  g_free(msg->msg);
  g_free(msg);
  return FALSE;
}


// a notication message, either displayed in the log of the current tab or, if
// the hub has no tab, in the "status bar". Calling this function with NULL
// will reset the status bar message. Unlike everything else, this function can
// be called from any thread. (It will queue an idle function, after all)
void ui_m(struct ui_tab *tab, int flags, const char *msg) {
  struct ui_m_dat *dat = g_new0(struct ui_m_dat, 1);
  dat->msg = (flags & UIM_PASS) ? (char *)msg : g_strdup(msg);
  dat->tab = tab;
  dat->flags = flags;
  g_idle_add_full(G_PRIORITY_HIGH_IDLE, ui_m_mainthread, dat, NULL);
}


// UIM_PASS shouldn't be used here (makes no sense).
void ui_mf(struct ui_tab *tab, int flags, const char *fmt, ...) {
  va_list va;
  va_start(va, fmt);
  ui_m(tab, flags | UIM_PASS, g_strdup_vprintf(fmt, va));
  va_end(va);
}





// colours

#if INTERFACE

#define UI_COLOURS \
  C(TABPRIO_LOW,  COLOR_BLACK,   -1, A_BOLD)\
  C(TABPRIO_MED,  COLOR_CYAN,    -1, A_BOLD)\
  C(TABPRIO_HIGH, COLOR_MAGENTA, -1, A_BOLD)


enum ui_coltype {
  UIC_UNUSED,
#define C(n, fg, bg, attr) UIC_##n,
  UI_COLOURS
#undef C
  UIC_NONE
}


struct ui_colour {
  short fg, bg;
  int x, a;
};

#define UIC(n) (ui_colours[UIC_##n].a)

#endif

struct ui_colour ui_colours[] = {
  {},
#define C(n, fg, bg, attr) { fg, bg, attr, 0 },
  UI_COLOURS
#undef C
  { -1, -1, 0, 0 }
};

int ui_colours_num = G_N_ELEMENTS(ui_colours);



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
  ui_global_textinput = ui_textinput_create(TRUE, cmd_suggest);

  // first tab = main tab
  ui_tab_open(ui_main_create());

  // init curses
  initscr();
  raw();
  noecho();
  curs_set(0);
  keypad(stdscr, 1);
  nodelay(stdscr, 1);

  // init colours
  if(has_colors()) {
    start_color();
    use_default_colors();

    int i;
    for(i=1; i<ui_colours_num; i++) {
      init_pair(i, ui_colours[i].fg, ui_colours[i].bg);
      ui_colours[i].a = ui_colours[i].x | COLOR_PAIR(i);
    }
  }

  // draw
  ui_draw();
}


static void ui_draw_status() {
  char buf[100];

  if(fl_refresh_queue && fl_refresh_queue->head)
    mvaddstr(winrows-1, 0, "[Refreshing share]");
  else if(fl_hash_queue && g_hash_table_size(fl_hash_queue)) {
    g_snprintf(buf, 100, "[Hashing: %d / %s / %.2f MiB/s]",
      g_hash_table_size(fl_hash_queue), str_formatsize(fl_hash_queue_size), ((float)ratecalc_get(&fl_hash_rate))/(1024.0f*1024.0f));
    mvaddstr(winrows-1, 0, buf);
  }
  // Note: Only upload is displayed, since we can't download yet anyway.
  // And protocol overhead isn't very significant.
  g_snprintf(buf, 100, "[U:%6d KiB/s]", ratecalc_get(&net_out)/1024);
  mvaddstr(winrows-1, wincols-28, buf);
  g_snprintf(buf, 100, "[S:%3d/%3d]", nmdc_cc_slots_in_use(), conf_slots());
  mvaddstr(winrows-1, wincols-11, buf);

  ui_m_updated = FALSE;
  if(ui_m_text) {
    mvaddstr(winrows-1, 0, ui_m_text);
    mvaddstr(winrows-1, str_columns(ui_m_text), "   ");
  }
}


#define tabcol(t, n) (2+ceil(log10((n)+1))+str_columns(((struct ui_tab *)(t)->data)->name))
#define prio2a(p) ((p) == UIP_LOW ? UIC(TABPRIO_LOW) : (p) == UIP_MED ? UIC(TABPRIO_MED) : UIC(TABPRIO_HIGH))

/* All tabs are in one of the following states:
 * - Selected                 (tab == ui_tab_cur->data) = sel    "n:name" in A_BOLD
 * - No change                (!sel && tab->prio == UIP_EMPTY)   "n:name" normal
 * - Change, low priority     (!sel && tab->prio == UIP_LOW)     "n!name", with ! in UIC(TABPRIO_LOW)
 * - Change, medium priority  (!sel && tab->prio == UIP_MED)     "n!name", with ! in ^_MED
 * - Change, high priority    (!sel && tab->prio == UIP_HIGH)    "n!name", with ! in ^_HIGH
 *
 * The truncated indicators are in the following states:
 * - No changes    ">>" or "<<"
 * - Change        "!>" or "<!"  with ! in same colour as above
 */

static void ui_draw_tablist() {
  static const int xoffset = 12;
  static int top = 0;
  int i, w;
  GList *n;

  int cur = g_list_position(ui_tabs, ui_tab_cur);
  int maxw = wincols-xoffset-5;

  // Make sure cur is visible
  if(top > cur)
    top = cur;
  do {
    w = maxw;
    i = top;
    for(n=g_list_nth(ui_tabs, top); n; n=n->next) {
      w -= tabcol(n, ++i);
      if(w < 0 || n == ui_tab_cur)
        break;
    }
  } while(top != cur && w < 0 && ++top);

  // display some more tabs when there is still room left
  while(top > 0 && w > tabcol(g_list_nth(ui_tabs, top-1), top-1)) {
    top--;
    w -= tabcol(g_list_nth(ui_tabs, top), top);
  }

  // check highest priority of hidden tabs before top
  // (This also sets n and i to the start of the visible list)
  char maxprio = 0;
  for(n=ui_tabs,i=0; i<top; i++,n=n->next) {
    struct ui_tab *t = n->data;
    if(t->prio > maxprio)
      maxprio = t->prio;
  }

  // print left truncate indicator
  if(top > 0) {
    mvaddch(winrows-2, xoffset, '<');
    if(!maxprio)
      addch('<');
    else {
      attron(prio2a(maxprio));
      addch('!');
      attroff(prio2a(maxprio));
    }
  } else
    mvaddch(winrows-2, xoffset+1, '[');

  // print the tab list
  w = maxw;
  char num[10];
  for(; n; n=n->next) {
    w -= tabcol(n, ++i);
    if(w < 0)
      break;
    struct ui_tab *t = n->data;
    addch(' ');
    if(n == ui_tab_cur)
      attron(A_BOLD);
    g_snprintf(num, 10, "%d", i);
    addstr(num);
    if(n == ui_tab_cur || !t->prio)
      addch(':');
    else {
      attron(prio2a(t->prio));
      addch('!');
      attroff(prio2a(t->prio));
    }
    addstr(t->name);
    if(n == ui_tab_cur)
      attroff(A_BOLD);
  }

  // check priority of hidden tabs after the last visible one
  GList *last = n;
  maxprio = 0;
  for(; n&&maxprio<UIP_HIGH; n=n->next) {
    struct ui_tab *t = n->data;
    if(t->prio > maxprio)
      maxprio = t->prio;
  }

  // print right truncate indicator
  if(!last)
    addstr(" ]");
  else {
    hline(' ', w + tabcol(last, i));
    if(!maxprio)
      mvaddch(winrows-2, wincols-3, '>');
    else {
      attron(prio2a(maxprio));
      mvaddch(winrows-2, wincols-3, '!');
      attroff(prio2a(maxprio));
    }
    addch('>');
  }
}
#undef tabcol
#undef prio2a


void ui_draw() {
  struct ui_tab *curtab = ui_tab_cur->data;
  curtab->prio = UIP_EMPTY;

  getmaxyx(stdscr, winrows, wincols);
  curs_set(0); // may be overridden later on by a textinput widget
  erase();

  // first line - title
  char *title =
    curtab->type == UIT_MAIN     ? ui_main_title() :
    curtab->type == UIT_HUB      ? ui_hub_title(curtab) :
    curtab->type == UIT_USERLIST ? ui_userlist_title(curtab) :
    curtab->type == UIT_MSG      ? ui_msg_title(curtab) :
    curtab->type == UIT_CONN     ? ui_conn_title() : g_strdup("");
  attron(A_REVERSE);
  mvhline(0, 0, ' ', wincols);
  mvaddstr(0, 0, title);
  attroff(A_REVERSE);
  g_free(title);

  // second-last line - time and tab list
  mvhline(winrows-2, 0, ACS_HLINE, wincols);
  // time
  time_t tm = time(NULL);
  char ts[12];
  strftime(ts, 11, "[%H:%M:%S]", localtime(&tm));
  mvaddstr(winrows-2, 1, ts);
  ui_draw_tablist();

  // last line - status info or notification
  ui_draw_status();

  // tab contents
  switch(curtab->type) {
  case UIT_MAIN:     ui_main_draw(); break;
  case UIT_HUB:      ui_hub_draw(curtab);  break;
  case UIT_USERLIST: ui_userlist_draw(curtab);  break;
  case UIT_MSG:      ui_msg_draw(curtab);  break;
  case UIT_CONN:     ui_conn_draw(); break;
  }

  refresh();
  if(ui_beep) {
    beep();
    ui_beep = FALSE;
  }
}


gboolean ui_checkupdate() {
  struct ui_tab *cur = ui_tab_cur->data;
  return ui_m_updated || ui_beep || (cur->log && cur->log->updated);
}


void ui_input(guint64 key) {
  struct ui_tab *curtab = ui_tab_cur->data;

  switch(key) {
  case INPT_CTRL('c'): // ctrl+c
    ncdc_quit();
    break;
  case INPT_ALT('j'): // alt+j (previous tab)
    ui_tab_cur = ui_tab_cur->prev ? ui_tab_cur->prev : g_list_last(ui_tabs);
    break;
  case INPT_ALT('k'): // alt+k (next tab)
    ui_tab_cur = ui_tab_cur->next ? ui_tab_cur->next : ui_tabs;
    break;
  case INPT_ALT('h'): ; // alt+h (swap tab with left)
    GList *prev = ui_tab_cur->prev;
    if(prev) {
      ui_tabs = g_list_delete_link(ui_tabs, ui_tab_cur);
      ui_tabs = g_list_insert_before(ui_tabs, prev, curtab);
      ui_tab_cur = prev->prev;
    }
    break;
  case INPT_ALT('l'): ; // alt+l (swap tab with right)
    GList *next = ui_tab_cur->next;
    if(next) {
      ui_tabs = g_list_delete_link(ui_tabs, ui_tab_cur);
      ui_tabs = g_list_insert_before(ui_tabs, next->next, curtab);
      ui_tab_cur = next->next;
    }
    break;
  case INPT_ALT('c'): // alt+c (alias for /close)
    cmd_handle("/close");
    break;
  case INPT_CTRL('l'): // ctrl+l (alias for /clear)
    cmd_handle("/clear");
    break;

  case INPT_CTRL('e'): // ctrl+e
  case INPT_CTRL('u'): // ctrl+u (alias for /refresh)
    cmd_handle("/refresh");
    break;

  case INPT_ALT('n'): // alt+n (alias for /connections)
    cmd_handle("/connections");
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
      case UIT_MSG:      ui_msg_key(curtab, key); break;
      case UIT_CONN:     ui_conn_key(key); break;
      }
    }
    // TODO: some user feedback on invalid key
  }
}

