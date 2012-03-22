/* ncdc - NCurses Direct Connect client

  Copyright (c) 2011-2012 Yoran Heling

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
#include <math.h>
#include <unistd.h>
#include <libnotify/notify.h>
#include <string.h>


#if INTERFACE

// These are assumed to occupy two bits
#define UIP_EMPTY 0 // no change
#define UIP_LOW   1 // system messages
#define UIP_MED   2 // chat messages, or error messages in the main tab
#define UIP_HIGH  3 // direct messages to you (PM or name mentioned)

#define UIT_MAIN     0
#define UIT_HUB      1 // #hubname
#define UIT_USERLIST 2 // @hubname
#define UIT_MSG      3 // ~username
#define UIT_CONN     4
#define UIT_FL       5 // /username
#define UIT_DL       6
#define UIT_SEARCH   7 // ?query

struct ui_tab {
  int type; // UIT_ type
  int prio; // UIP_ type
  char *name;
  struct ui_tab *parent;       // the tab that opened this tab (may be NULL or dangling)
  struct ui_logwindow *log;    // MAIN, HUB, MSG
  struct hub *hub;             // HUB, USERLIST, MSG, SEARCH
  struct ui_listing *list;     // USERLIST, CONN, FL, DL, SEARCH
  guint64 uid;                 // FL, MSG
  int order : 4;               // USERLIST, SEARCH, FL (has different interpretation per tab)
  gboolean o_reverse : 1;      // USERLIST, SEARCH
  gboolean details : 1;        // USERLIST, CONN, DL
  // USERLIST
  gboolean user_opfirst : 1;
  gboolean user_hide_desc : 1;
  gboolean user_hide_tag : 1;
  gboolean user_hide_mail : 1;
  gboolean user_hide_conn : 1;
  gboolean user_hide_ip : 1;
  // HUB
  gboolean hub_joincomplete : 1;
  GRegex *hub_highlight;
  char *hub_nick;
  struct ui_tab *userlist_tab;
  // FL
  struct fl_list *fl_list;
  char *fl_uname;
  gboolean fl_loading : 1;
  gboolean fl_dirfirst : 1;
  gboolean fl_match : 1;
  GError *fl_err;
  char *fl_sel;
  // DL
  struct dl *dl_cur;
  struct ui_listing *dl_users;
  // SEARCH
  struct search_q *search_q;
  time_t search_t;
  gboolean search_hide_hub : 1;
  // MSG
  int msg_replyto;
};

#endif

GList *ui_tabs = NULL;
GList *ui_tab_cur = NULL;

// screen dimensions
int wincols;
int winrows;

gboolean ui_beep = FALSE; // set to true anywhere to send a beep

// uid -> tab lookup table for MSG tabs.
GHashTable *ui_msg_tabs = NULL;




// Main tab


// these is only one main tab, so this can be static
struct ui_tab *ui_main;

static struct ui_tab *ui_main_create() {
  ui_main = g_new0(struct ui_tab, 1);
  ui_main->name = "main";
  ui_main->log = ui_logwindow_create("main", 0);
  ui_main->type = UIT_MAIN;

  ui_mf(ui_main, 0, "Welcome to ncdc %s!", VERSION);
  ui_m(ui_main, 0,
    "Check out the manual page for a general introduction to ncdc.\n"
    "Make sure you always run the latest version available from http://dev.yorhel.nl/ncdc\n");
  ui_mf(ui_main, 0, "Using working directory: %s", db_dir);

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


// Select the main tab and run `/help keys <s>'.
static void ui_main_keys(const char *s) {
  ui_tab_cur = g_list_find(ui_tabs, ui_main);
  char *c = g_strdup_printf("/help keys %s", s);
  cmd_handle(c);
  g_free(c);
}





// User message tab

struct ui_tab *ui_msg_create(struct hub *hub, struct hub_user *user) {
  g_return_val_if_fail(!g_hash_table_lookup(ui_msg_tabs, &user->uid), NULL);

  struct ui_tab *tab = g_new0(struct ui_tab, 1);
  tab->type = UIT_MSG;
  tab->hub = hub;
  tab->uid = user->uid;
  tab->name = g_strdup_printf("~%s", user->name);
  tab->log = ui_logwindow_create(tab->name, var_get_int(0, VAR_backlog));
  tab->log->handle = tab;
  tab->log->checkchat = ui_hub_log_checkchat;

  ui_mf(tab, 0, "Chatting with %s on %s.", user->name, hub->tab->name);
  g_hash_table_insert(ui_msg_tabs, &tab->uid, tab);

  return tab;
}


void ui_msg_close(struct ui_tab *tab) {
  g_hash_table_remove(ui_msg_tabs, &tab->uid);
  ui_tab_remove(tab);
  ui_logwindow_free(tab->log);
  g_free(tab->name);
  g_free(tab);
}


// *u may be NULL if change = QUIT. A QUIT is always done before the user is
// removed from the hub_uids table.
static void ui_msg_userchange(struct ui_tab *tab, int change, struct hub_user *u) {
  switch(change) {
  case UIHUB_UC_JOIN:
    ui_mf(tab, 0, "--> %s has joined.", u->name);
    break;
  case UIHUB_UC_QUIT:
    if(g_hash_table_lookup(hub_uids, &tab->uid))
      ui_mf(tab, 0, "--< %s has quit.", tab->name+1);
    break;
  case UIHUB_UC_NFO:
    // Detect nick changes.
    // Note: the name of the log file remains the same even after a nick
    // change. This probably isn't a major problem, though. Nick changes are
    // not very common and are only detected on ADC hubs.
    if(strcmp(u->name, tab->name+1) != 0) {
      ui_mf(tab, 0, "%s is now known as %s.", tab->name+1, u->name);
      g_free(tab->name);
      tab->name = g_strdup_printf("~%s", u->name);
    }
    break;
  }
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
    tab->name+1, tab->hub->tab->name, g_hash_table_lookup(hub_uids, &tab->uid) ? "" : " (offline)");
}


static void ui_msg_key(struct ui_tab *tab, guint64 key) {
  char *str = NULL;
  if(!ui_logwindow_key(tab->log, key, winrows) &&
      ui_textinput_key(ui_global_textinput, key, &str) && str) {
    cmd_handle(str);
    g_free(str);
  }
}


static void ui_msg_msg(struct ui_tab *tab, const char *msg, int replyto) {
  ui_m(tab, UIP_HIGH, msg);
  tab->msg_replyto = replyto;
}





// Hub tab

#if INTERFACE
// change types for ui_hub_userchange()
#define UIHUB_UC_JOIN 0
#define UIHUB_UC_QUIT 1
#define UIHUB_UC_NFO 2
#endif


// Also used for ui_msg_*
int ui_hub_log_checkchat(void *dat, char *nick, char *msg) {
  struct ui_tab *tab = dat;
  tab = tab->hub->tab;
  if(!tab->hub_nick)
    return 0;

  if(strcmp(nick, tab->hub_nick) == 0)
    return 2;

  if(!tab->hub_highlight)
    return 0;

  return g_regex_match(tab->hub_highlight, msg, 0, NULL) ? 1 : 0;
}


// Called by hub.c when hub->nick is set or changed. (Not called when hub->nick is reset to NULL)
// A local hub_nick field is kept in the hub tab struct to still provide
// highlighting for it after disconnecting from the hub.
void ui_hub_setnick(struct ui_tab *tab) {
  if(!tab->hub->nick)
    return;
  g_free(tab->hub_nick);
  if(tab->hub_highlight)
    g_regex_unref(tab->hub_highlight);
  tab->hub_nick = g_strdup(tab->hub->nick);
  char *name = g_regex_escape_string(tab->hub->nick, -1);
  char *pattern = g_strdup_printf("\\b%s\\b", name);
  tab->hub_highlight = g_regex_new(pattern, G_REGEX_CASELESS|G_REGEX_OPTIMIZE, 0, NULL);
  g_free(name);
  g_free(pattern);
}


struct ui_tab *ui_hub_create(const char *name, gboolean conn) {
  struct ui_tab *tab = g_new0(struct ui_tab, 1);
  // NOTE: tab name is also used as configuration group
  tab->name = g_strdup_printf("#%s", name);
  tab->type = UIT_HUB;
  tab->hub = hub_create(tab);
  tab->log = ui_logwindow_create(tab->name, var_get_int(tab->hub->id, VAR_backlog));
  tab->log->handle = tab;
  tab->log->checkchat = ui_hub_log_checkchat;
  // already used this name before? open connection again
  if(conn && var_get(tab->hub->id, VAR_hubaddr))
    hub_connect(tab->hub);
  return tab;
}


void ui_hub_close(struct ui_tab *tab) {
  // close the userlist tab
  if(tab->userlist_tab)
    ui_userlist_close(tab->userlist_tab);
  // close msg and search tabs
  GList *n;
  for(n=ui_tabs; n;) {
    struct ui_tab *t = n->data;
    n = n->next;
    if((t->type == UIT_MSG || t->type == UIT_SEARCH) && t->hub == tab->hub) {
      if(t->type == UIT_MSG)
        ui_msg_close(t);
      else
        ui_search_close(t);
    }
  }
  // remove ourself from the list
  ui_tab_remove(tab);

  hub_free(tab->hub);
  ui_logwindow_free(tab->log);
  g_free(tab->hub_nick);
  if(tab->hub_highlight)
    g_regex_unref(tab->hub_highlight);
  g_free(tab->name);
  g_free(tab);
}


static void ui_hub_draw(struct ui_tab *tab) {
  ui_logwindow_draw(tab->log, 1, 0, winrows-5, wincols);

  attron(UIC(separator));
  mvhline(winrows-4, 0, ' ', wincols);
  if(tab->hub->net->connecting)
    mvaddstr(winrows-4, wincols-15, "Connecting...");
  else if(!tab->hub->net->conn)
    mvaddstr(winrows-4, wincols-16, "Not connected.");
  else if(!tab->hub->nick_valid)
    mvaddstr(winrows-4, wincols-15, "Logging in...");
  else {
    char *addr = var_get(tab->hub->id, VAR_hubaddr);
    char *conn = !listen_hub_active(tab->hub->id) ? g_strdup("[passive]")
      : g_strdup_printf("[active: %s]", ip4_unpack(hub_ip4(tab->hub)));
    char *tmp = g_strdup_printf("%s @ %s%s %s", tab->hub->nick, addr,
      tab->hub->isop ? " (operator)" : tab->hub->isreg ? " (registered)" : "", conn);
    g_free(conn);
    mvaddstr(winrows-4, 0, tmp);
    g_free(tmp);
    int count = g_hash_table_size(tab->hub->users);
    tmp = g_strdup_printf("%6d users  %10s%c", count,
      str_formatsize(tab->hub->sharesize), tab->hub->sharecount == count ? ' ' : '+');
    mvaddstr(winrows-4, wincols-26, tmp);
    g_free(tmp);
  }
  attroff(UIC(separator));

  mvaddstr(winrows-3, 0, tab->name);
  addstr("> ");
  int pos = str_columns(tab->name)+2;
  ui_textinput_draw(ui_global_textinput, winrows-3, pos, wincols-pos);
}


static char *ui_hub_title(struct ui_tab *tab) {
  return g_strdup_printf("%s: %s", tab->name,
    tab->hub->net->connecting  ? "Connecting..." :
    !tab->hub->net->conn       ? "Not connected." :
    !tab->hub->nick_valid      ? "Logging in..." :
    tab->hub->hubname          ? tab->hub->hubname : "Connected.");
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


void ui_hub_userchange(struct ui_tab *tab, int change, struct hub_user *user) {
  // notify the userlist, when it is open
  if(tab->userlist_tab)
    ui_userlist_userchange(tab->userlist_tab, change, user);

  // notify the MSG tab, if we have one open for this user
  struct ui_tab *mt = g_hash_table_lookup(ui_msg_tabs, &user->uid);
  if(mt)
    ui_msg_userchange(mt, change, user);

  // display the join/quit, when requested
  gboolean log = var_get_bool(tab->hub->id, VAR_show_joinquit);
  if(change == UIHUB_UC_NFO && !user->isjoined) {
    user->isjoined = TRUE;
    if(log && tab->hub->joincomplete && (!tab->hub->nick_valid
        || (tab->hub->adc ? (tab->hub->sid != user->sid) : (strcmp(tab->hub->nick_hub, user->name_hub) != 0))))
      ui_mf(tab, 0, "--> %s has joined.", user->name);
  } else if(change == UIHUB_UC_QUIT && log)
    ui_mf(tab, 0, "--< %s has quit.", user->name);
}


// Called when the hub is disconnected. Notifies any msg tabs and the userlist
// tab, if there's one open.
void ui_hub_disconnect(struct ui_tab *tab) {
  // userlist
  if(tab->userlist_tab)
    ui_userlist_disconnect(tab->userlist_tab);
  // msg tabs
  GList *n = ui_tabs;
  for(; n; n=n->next) {
    struct ui_tab *t = n->data;
    if(t->type == UIT_MSG && t->hub == tab->hub)
      ui_msg_userchange(t, UIHUB_UC_QUIT, NULL);
  }
}


void ui_hub_msg(struct ui_tab *tab, struct hub_user *user, const char *msg, int replyto) {
  struct ui_tab *t = g_hash_table_lookup(ui_msg_tabs, &user->uid);
  if(!t) {
    t = ui_msg_create(tab->hub, user);
    ui_tab_open(t, FALSE, tab);
  }
  ui_msg_msg(t, msg, replyto);

  // Show notification for PMs.
  if(t->type == UIT_MSG) {
    show_system_notification(user->name,&msg[strlen(user->name)+2]);
  }
}

void show_system_notification(const char * from, const char * msg) {
  notify_init("ncdc");
  NotifyNotification * nMsg = notify_notification_new (from, msg, "mail-message-new");
  notify_notification_show (nMsg, NULL);
}


void ui_hub_userlist_open(struct ui_tab *tab) {
  if(tab->userlist_tab)
    ui_tab_cur = g_list_find(ui_tabs, tab->userlist_tab);
  else {
    tab->userlist_tab = ui_userlist_create(tab->hub);
    ui_tab_open(tab->userlist_tab, TRUE, tab);
  }
}


gboolean ui_hub_finduser(struct ui_tab *tab, guint64 uid, const char *user, gboolean utf8) {
  struct hub_user *u =
    uid ? g_hash_table_lookup(hub_uids, &uid) :
    utf8 ? hub_user_get(tab->hub, user) : g_hash_table_lookup(tab->hub->users, user);
  if(!u || u->hub != tab->hub)
    return FALSE;
  ui_hub_userlist_open(tab);
  // u->iter should be valid at this point.
  tab->userlist_tab->list->sel = u->iter;
  tab->userlist_tab->details = TRUE;
  return TRUE;
}





// Userlist tab

// Columns to sort on
#define UIUL_USER   0
#define UIUL_SHARE  1
#define UIUL_CONN   2
#define UIUL_DESC   3
#define UIUL_MAIL   4
#define UIUL_CLIENT 5
#define UIUL_IP     6


static gint ui_userlist_sort_func(gconstpointer da, gconstpointer db, gpointer dat) {
  const struct hub_user *a = da;
  const struct hub_user *b = db;
  struct ui_tab *tab = dat;
  int p = tab->order;

  if(tab->user_opfirst && !a->isop != !b->isop)
    return a->isop && !b->isop ? -1 : 1;

  // All orders have the username as secondary order.
  int o = p == UIUL_USER ? 0 :
    p == UIUL_SHARE  ? a->sharesize > b->sharesize ? 1 : -1:
    p == UIUL_CONN   ? (tab->hub->adc ? a->conn - b->conn : strcmp(a->conn?a->conn:"", b->conn?b->conn:"")) :
    p == UIUL_DESC   ? g_utf8_collate(a->desc?a->desc:"", b->desc?b->desc:"") :
    p == UIUL_MAIL   ? g_utf8_collate(a->mail?a->mail:"", b->mail?b->mail:"") :
    p == UIUL_CLIENT ? strcmp(a->client?a->client:"", b->client?b->client:"")
                     : ip4_cmp(a->ip4, b->ip4);

  // Username sort
  if(!o)
    o = g_utf8_collate(a->name, b->name);
  if(!o && a->name_hub && b->name_hub)
    o = strcmp(a->name_hub, b->name_hub);
  if(!o)
    o = a - b;
  return tab->o_reverse ? -1*o : o;
}


struct ui_tab *ui_userlist_create(struct hub *hub) {
  struct ui_tab *tab = g_new0(struct ui_tab, 1);
  tab->name = g_strdup_printf("@%s", hub->tab->name+1);
  tab->type = UIT_USERLIST;
  tab->hub = hub;
  tab->user_opfirst = TRUE;
  tab->user_hide_conn = TRUE;
  tab->user_hide_mail = TRUE;
  tab->user_hide_ip = TRUE;
  GSequence *users = g_sequence_new(NULL);
  // populate the list
  // g_sequence_sort() uses insertion sort? in that case it is faster to insert
  // all items using g_sequence_insert_sorted() rather than inserting them in
  // no particular order and then sorting them in one go. (which is faster for
  // linked lists, since it uses a faster sorting algorithm)
  GHashTableIter iter;
  g_hash_table_iter_init(&iter, hub->users);
  struct hub_user *u;
  while(g_hash_table_iter_next(&iter, NULL, (gpointer *)&u))
    u->iter = g_sequence_insert_sorted(users, u, ui_userlist_sort_func, tab);
  tab->list = ui_listing_create(users);
  return tab;
}


void ui_userlist_close(struct ui_tab *tab) {
  tab->hub->tab->userlist_tab = NULL;
  ui_tab_remove(tab);
  // To clean things up, we should also reset all hub_user->iter fields. But
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
  int cw_user, cw_share, cw_conn, cw_desc, cw_mail, cw_tag, cw_ip;
};


static void ui_userlist_draw_row(struct ui_listing *list, GSequenceIter *iter, int row, void *dat) {
  struct hub_user *user = g_sequence_get(iter);
  struct ui_userlist_draw_opts *o = dat;

  char *tag = hub_user_tag(user);
  char *conn = hub_user_conn(user);
  int j=5;

  attron(iter == list->sel ? UIC(list_select) : UIC(list_default));
  mvhline(row, 0, ' ', wincols);
  if(iter == list->sel)
    mvaddstr(row, 0, ">");

  if(user->isop)
    mvaddch(row, 2, 'O');
  if(!user->active)
    mvaddch(row, 3, 'P');
  DRAW_COL(row, j, o->cw_user,  user->name);
  DRAW_COL(row, j, o->cw_share, user->hasinfo ? str_formatsize(user->sharesize) : "");
  DRAW_COL(row, j, o->cw_desc,  user->desc?user->desc:"");
  DRAW_COL(row, j, o->cw_tag,   tag?tag:"");
  DRAW_COL(row, j, o->cw_mail,  user->mail?user->mail:"");
  DRAW_COL(row, j, o->cw_conn,  conn?conn:"");
  DRAW_COL(row, j, o->cw_ip,    user->ip4?ip4_unpack(user->ip4):"");
  g_free(conn);
  g_free(tag);

  attroff(iter == list->sel ? UIC(list_select) : UIC(list_default));
}


/* Distributing a width among several columns with given weights:
 *   w_t = sum(i=c_v; w_i)
 *   w_s = 1 + sum(i=c_h; w_i/w_t)
 *   b_i = w_i*w_s
 * Where:
 *   c_v = set of all visible columns
 *   c_h = set of all hidden columns
 *   w_i = weight of column $i
 *   w_t = sum of the weights of all visible columns
 *   w_s = scale factor
 *   b_i = calculated width of column $i, with 0 < b_i <= 1
 *
 * TODO: abstract this, so that the weights and such don't need repetition.
 */
static void ui_userlist_calc_widths(struct ui_tab *tab, struct ui_userlist_draw_opts *o) {
  // available width
  int w = wincols-5;

  // share has a fixed size
  o->cw_share = 12;
  w -= 12;

  // IP column as well
  o->cw_ip = tab->user_hide_ip ? 0 : 16;
  w -= o->cw_ip;

  // User column has a minimum size (but may grow a bit later on, so will still be counted as a column)
  o->cw_user = 15;
  w -= 15;

  // Total weight (first one is for the user column)
  double wt = 0.02
    + (tab->user_hide_conn ? 0.0 : 0.16)
    + (tab->user_hide_desc ? 0.0 : 0.32)
    + (tab->user_hide_mail ? 0.0 : 0.18)
    + (tab->user_hide_tag  ? 0.0 : 0.32);

  // Scale factor
  double ws = 1.0 + (
    + (tab->user_hide_conn ? 0.16: 0.0)
    + (tab->user_hide_desc ? 0.32: 0.0)
    + (tab->user_hide_mail ? 0.18: 0.0)
    + (tab->user_hide_tag  ? 0.32: 0.0))/wt;
  // scale to available width
  ws *= w;

  // Get the column widths. Note the use of floor() here, this prevents that
  // the total width exceeds the available width. The remaining columns will be
  // given to the user column, which is always present anyway.
  o->cw_conn = tab->user_hide_conn ? 0 : floor(0.16*ws);
  o->cw_desc = tab->user_hide_desc ? 0 : floor(0.32*ws);
  o->cw_mail = tab->user_hide_mail ? 0 : floor(0.18*ws);
  o->cw_tag  = tab->user_hide_tag  ? 0 : floor(0.32*ws);
  o->cw_user += w - o->cw_conn - o->cw_desc - o->cw_mail - o->cw_tag;
}


static void ui_userlist_draw(struct ui_tab *tab) {
  struct ui_userlist_draw_opts o;
  ui_userlist_calc_widths(tab, &o);

  // header
  int i = 5;
  attron(UIC(list_header));
  mvhline(1, 0, ' ', wincols);
  mvaddstr(1, 2, "OP");
  DRAW_COL(1, i, o.cw_user,  "Username");
  DRAW_COL(1, i, o.cw_share, "Share");
  DRAW_COL(1, i, o.cw_desc,  "Description");
  DRAW_COL(1, i, o.cw_tag,   "Tag");
  DRAW_COL(1, i, o.cw_mail,  "E-Mail");
  DRAW_COL(1, i, o.cw_conn,  "Connection");
  DRAW_COL(1, i, o.cw_ip,    "IP");
  attroff(UIC(list_header));

  // rows
  int bottom = tab->details ? winrows-7 : winrows-3;
  int pos = ui_listing_draw(tab->list, 2, bottom-1, ui_userlist_draw_row, &o);

  // footer
  attron(UIC(separator));
  mvhline(bottom, 0, ' ', wincols);
  int count = g_hash_table_size(tab->hub->users);
  mvaddstr(bottom, 0, "Totals:");
  mvprintw(bottom, o.cw_user+5, "%s%c   %d users",
    str_formatsize(tab->hub->sharesize), tab->hub->sharecount == count ? ' ' : '+', count);
  mvprintw(bottom, wincols-6, "%3d%%", pos);
  attroff(UIC(separator));

  // detailed info box
  if(!tab->details)
    return;
  if(g_sequence_iter_is_end(tab->list->sel))
    mvaddstr(bottom+1, 2, "No user selected.");
  else {
    struct hub_user *u = g_sequence_get(tab->list->sel);
    attron(A_BOLD);
    mvaddstr(bottom+1,  4, "Username:");
    mvaddstr(bottom+1, 41, "Share:");
    mvaddstr(bottom+2,  2, "Connection:");
    mvaddstr(bottom+2, 40, "E-Mail:");
    mvaddstr(bottom+3, 10, "IP:");
    mvaddstr(bottom+3, 43, "Tag:");
    mvaddstr(bottom+4,  1, "Description:");
    attroff(A_BOLD);
    mvaddstr(bottom+1, 14, u->name);
    if(u->hasinfo)
      mvprintw(bottom+1, 48, "%s (%s bytes)", str_formatsize(u->sharesize), str_fullsize(u->sharesize));
    else
      mvaddstr(bottom+1, 48, "-");
    char *conn = hub_user_conn(u);
    mvaddstr(bottom+2, 14, conn?conn:"-");
    g_free(conn);
    mvaddstr(bottom+2, 48, u->mail?u->mail:"-");
    mvaddstr(bottom+3, 14, u->ip4?ip4_unpack(u->ip4):"-");
    char *tag = hub_user_tag(u);
    mvaddstr(bottom+3, 48, tag?tag:"-");
    g_free(tag);
    mvaddstr(bottom+4, 14, u->desc?u->desc:"-");
    // TODO: CID?
  }
}
#undef DRAW_COL


static void ui_userlist_key(struct ui_tab *tab, guint64 key) {
  if(ui_listing_key(tab->list, key, winrows/2))
    return;

  struct hub_user *sel = g_sequence_iter_is_end(tab->list->sel) ? NULL : g_sequence_get(tab->list->sel);
  gboolean sort = FALSE;
  switch(key) {
  case INPT_CHAR('?'):
    ui_main_keys("userlist");
    break;

  // Sorting
#define SETSORT(c) \
  tab->o_reverse = tab->order == c ? !tab->o_reverse : FALSE;\
  tab->order = c;\
  sort = TRUE;

  case INPT_CHAR('s'): // s/S - sort on share size
  case INPT_CHAR('S'):
    SETSORT(UIUL_SHARE);
    break;
  case INPT_CHAR('u'): // u/U - sort on username
  case INPT_CHAR('U'):
    SETSORT(UIUL_USER)
    break;
  case INPT_CHAR('D'): // D - sort on description
    SETSORT(UIUL_DESC)
    break;
  case INPT_CHAR('T'): // T - sort on client (= tag)
    SETSORT(UIUL_CLIENT)
    break;
  case INPT_CHAR('E'): // E - sort on email
    SETSORT(UIUL_MAIL)
    break;
  case INPT_CHAR('C'): // C - sort on connection
    SETSORT(UIUL_CONN)
    break;
  case INPT_CHAR('P'): // P - sort on IP
    SETSORT(UIUL_IP)
    break;
  case INPT_CHAR('o'): // o - toggle sorting OPs before others
    tab->user_opfirst = !tab->user_opfirst;
    sort = TRUE;
    break;
#undef SETSORT

  // Column visibility
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
  case INPT_CHAR('p'): // p (toggle IP visibility)
    tab->user_hide_ip = !tab->user_hide_ip;
    break;

  case INPT_CTRL('j'): // newline
  case INPT_CHAR('i'): // i       (toggle user info)
    tab->details = !tab->details;
    break;
  case INPT_CHAR('m'): // m (/msg user)
    if(!sel)
      ui_m(NULL, 0, "No user selected.");
    else {
      struct ui_tab *t = g_hash_table_lookup(ui_msg_tabs, &sel->uid);
      if(!t) {
        t = ui_msg_create(tab->hub, sel);
        ui_tab_open(t, TRUE, tab);
      } else
        ui_tab_cur = g_list_find(ui_tabs, t);
    }
    break;
  case INPT_CHAR('g'): // g (grant slot)
    if(!sel)
      ui_m(NULL, 0, "No user selected.");
    else {
      cc_grant(sel);
      ui_m(NULL, 0, "Slot granted.");
    }
    break;
  case INPT_CHAR('b'): // b (/browse userlist)
  case INPT_CHAR('B'): // B (force /browse userlist)
    if(!sel)
      ui_m(NULL, 0, "No user selected.");
    else
      ui_fl_queue(sel->uid, key == INPT_CHAR('B'), NULL, tab, TRUE, FALSE);
    break;
  }

  // TODO: some way to save the column visibility? per hub? global default?

  if(sort) {
    g_sequence_sort(tab->list->list, ui_userlist_sort_func, tab);
    ui_listing_sorted(tab->list);
    ui_mf(NULL, 0, "Ordering by %s (%s%s)",
        tab->order == UIUL_USER  ? "user name" :
        tab->order == UIUL_SHARE ? "share size" :
        tab->order == UIUL_CONN  ? "connection" :
        tab->order == UIUL_DESC  ? "description" :
        tab->order == UIUL_MAIL  ? "e-mail" :
        tab->order == UIUL_CLIENT? "tag" : "IP address",
      tab->o_reverse ? "descending" : "ascending", tab->user_opfirst ? ", OPs first" : "");
  }
}


// Called when the hub is disconnected. All users should be removed in one go,
// this is faster than a _userchange() for every user.
void ui_userlist_disconnect(struct ui_tab *tab) {
  g_sequence_free(tab->list->list);
  ui_listing_free(tab->list);
  tab->list = ui_listing_create(g_sequence_new(NULL));
}


void ui_userlist_userchange(struct ui_tab *tab, int change, struct hub_user *user) {
  if(change == UIHUB_UC_JOIN) {
    user->iter = g_sequence_insert_sorted(tab->list->list, user, ui_userlist_sort_func, tab);
    ui_listing_inserted(tab->list);
  } else if(change == UIHUB_UC_QUIT) {
    g_return_if_fail(g_sequence_get(user->iter) == (gpointer)user);
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
  const struct cc *a = da;
  const struct cc *b = db;
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
  g_sequence_sort(cc_list, ui_conn_sort_func, NULL);
  ui_conn->list = ui_listing_create(cc_list);
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
#define UICONN_MOD 2  // when the nick or hub changes
#endif


void ui_conn_listchange(GSequenceIter *iter, int change) {
  g_return_if_fail(ui_conn);
  switch(change) {
  case UICONN_ADD:
    g_sequence_sort_changed(iter, ui_conn_sort_func, NULL);
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
  struct cc *cc = g_sequence_get(iter);

  attron(iter == list->sel ? UIC(list_select) : UIC(list_default));
  mvhline(row, 0, ' ', wincols);
  if(iter == list->sel)
    mvaddstr(row, 0, ">");

  mvaddch(row, 2,
    cc->state == CCS_CONN      ? 'C' :
    cc->state == CCS_DISCONN   ? '-' :
    cc->state == CCS_HANDSHAKE ? 'H' :
    cc->state == CCS_IDLE      ? 'I' : cc->dl ? 'D' : 'U');
  mvaddch(row, 3, cc->net->tls ? 't' : ' ');

  if(cc->nick)
    mvaddnstr(row, 5, cc->nick, str_offset_from_columns(cc->nick, 15));
  else {
    char tmp[30];
    strcpy(tmp, "IP:");
    strcat(tmp, cc->remoteaddr);
    if(strchr(tmp+3, ':'))
      *(strchr(tmp+3, ':')) = 0;
    mvaddstr(row, 5, tmp);
  }

  if(cc->hub)
    mvaddnstr(row, 21, cc->hub->tab->name, str_offset_from_columns(cc->hub->tab->name, 11));


  mvaddstr(row, 33, cc->last_length ? str_formatsize(cc->last_length) : "-");

  if(cc->last_length && !cc->timeout_src) {
    float left = cc->dl ? net_recv_left(cc->net) : net_file_left(cc->net);
    float length = cc->last_length;
    mvprintw(row, 45, "%3.0f%%", (length-left)*100.0f/length);
  } else
    mvaddstr(row, 45, " -");

  if(cc->timeout_src)
    mvaddstr(row, 50, "     -");
  else
    mvprintw(row, 50, "%6d", ratecalc_rate(cc->dl ? cc->net->rate_in : cc->net->rate_out)/1024);

  if(cc->err) {
    mvaddstr(row, 58, "Disconnected: ");
    addnstr(cc->err->message, str_offset_from_columns(cc->err->message, wincols-(58+14)));
  } else if(cc->last_file) {
    char *file = strrchr(cc->last_file, '/');
    if(file)
      file++;
    else
      file = cc->last_file;
      mvaddnstr(row, 58, file, str_offset_from_columns(file, wincols-58));
  }

  attroff(iter == list->sel ? UIC(list_select) : UIC(list_default));
}


static void ui_conn_draw_details(int l) {
  struct cc *cc = g_sequence_iter_is_end(ui_conn->list->sel) ? NULL : g_sequence_get(ui_conn->list->sel);
  if(!cc) {
    mvaddstr(l+1, 0, "Nothing selected.");
    return;
  }

  // labels
  attron(A_BOLD);
  mvaddstr(l+1,  3, "Username:");
  mvaddstr(l+1, 42, "Hub:");
  mvaddstr(l+2,  4, "IP/port:");
  mvaddstr(l+2, 39, "Status:");
  mvaddstr(l+3,  9, "Up:");
  mvaddstr(l+3, 41, "Down:");
  mvaddstr(l+4,  7, "Size:");
  mvaddstr(l+5,  5, "Offset:");
  mvaddstr(l+6,  6, "Chunk:");
  mvaddstr(l+4, 37, "Progress:");
  mvaddstr(l+5, 42, "ETA:");
  mvaddstr(l+6, 41, "Idle:");
  mvaddstr(l+7,  7, "File:");
  mvaddstr(l+8,  1, "Last error:");
  attroff(A_BOLD);

  // line 1
  mvaddstr(l+1, 13, cc->nick ? cc->nick : "Unknown / connecting");
  mvaddstr(l+1, 47, cc->hub ? cc->hub->tab->name : "-");
  // line 2
  mvaddstr(l+2, 13, cc->remoteaddr);
  mvaddstr(l+2, 47,
    cc->state == CCS_CONN      ? "Connecting" :
    cc->state == CCS_DISCONN   ? "Disconnected" :
    cc->state == CCS_HANDSHAKE ? "Handshake" :
    cc->state == CCS_IDLE      ? "Idle" : cc->dl ? "Downloading" : "Uploading");
  // line 3
  mvprintw(l+3, 13, "%d KiB/s (%s)", ratecalc_rate(cc->net->rate_out)/1024, str_formatsize(ratecalc_total(cc->net->rate_out)));
  mvprintw(l+3, 47, "%d KiB/s (%s)", ratecalc_rate(cc->net->rate_in)/1024, str_formatsize(ratecalc_total(cc->net->rate_in)));
  // size / offset / chunk (line 4/5/6)
  mvaddstr(l+4, 13, cc->last_size ? str_formatsize(cc->last_size) : "-");
  mvaddstr(l+5, 13, cc->last_size ? str_formatsize(cc->last_offset) : "-");
  mvaddstr(l+6, 13, cc->last_length ? str_formatsize(cc->last_length) : "-");
  // progress / eta / idle (line 4/5/6)
  int left = cc->dl ? net_recv_left(cc->net) : net_file_left(cc->net);
  if(cc->last_length && !cc->timeout_src) {
    float length = cc->last_length;
    mvprintw(l+4, 47, "%3.0f%%", (length-(float)left)*100.0f/length);
  } else
    mvaddstr(l+4, 47, "-");
  if(cc->last_length && !cc->timeout_src)
    mvaddstr(l+5, 47, ratecalc_eta(cc->dl ? cc->net->rate_in : cc->net->rate_out, left));
  else
    mvaddstr(l+5, 47, "-");
  mvprintw(l+6, 47, "%ds", (int)(time(NULL)-cc->net->timeout_last));
  // line 7
  if(cc->last_file)
    mvaddnstr(l+7, 13, cc->last_file, str_offset_from_columns(cc->last_file, wincols-13));
  else
    mvaddstr(l+7, 13, "None.");
  // line 8
  if(cc->err)
    mvaddnstr(l+8, 13, cc->err->message, str_offset_from_columns(cc->err->message, wincols-13));
  else
    mvaddstr(l+8, 13, "-");
}


static void ui_conn_draw() {
  attron(UIC(list_header));
  mvhline(1, 0, ' ', wincols);
  mvaddstr(1, 2,  "St Username");
  mvaddstr(1, 21, "Hub");
  mvaddstr(1, 33, "Chunk          %");
  mvaddstr(1, 50, " KiB/s");
  mvaddstr(1, 58, "File");
  attroff(UIC(list_header));

  int bottom = ui_conn->details ? winrows-11 : winrows-3;
  int pos = ui_listing_draw(ui_conn->list, 2, bottom-1, ui_conn_draw_row, NULL);

  // footer
  attron(UIC(separator));
  mvhline(bottom, 0, ' ', wincols);
  mvprintw(bottom, wincols-24, "%3d connections    %3d%%", g_sequence_iter_get_position(g_sequence_get_end_iter(ui_conn->list->list)), pos);
  attroff(UIC(separator));

  // detailed info
  if(ui_conn->details)
    ui_conn_draw_details(bottom);
}


static void ui_conn_key(guint64 key) {
  if(ui_listing_key(ui_conn->list, key, (winrows-10)/2))
    return;

  struct cc *cc = g_sequence_iter_is_end(ui_conn->list->sel) ? NULL : g_sequence_get(ui_conn->list->sel);

  switch(key) {
  case INPT_CHAR('?'):
    ui_main_keys("connections");
    break;
  case INPT_CTRL('j'): // newline
  case INPT_CHAR('i'): // i - toggle detailed info
    ui_conn->details = !ui_conn->details;
    break;
  case INPT_CHAR('f'): // f - find user
    if(!cc)
      ui_m(NULL, 0, "Nothing selected.");
    else if(!cc->hub || !cc->uid)
      ui_m(NULL, 0, "User or hub unknown.");
    else if(!ui_hub_finduser(cc->hub->tab, cc->uid, NULL, FALSE))
      ui_m(NULL, 0, "User has left the hub.");
    break;
  case INPT_CHAR('m'): // m - /msg user
    if(!cc)
      ui_m(NULL, 0, "Nothing selected.");
    else if(!cc->hub || !cc->uid)
      ui_m(NULL, 0, "User or hub unknown.");
    else {
      struct ui_tab *t = g_hash_table_lookup(ui_msg_tabs, &cc->uid);
      if(t) {
        ui_tab_cur = g_list_find(ui_tabs, t);
      } else {
        struct hub_user *u = g_hash_table_lookup(hub_uids, &cc->uid);
        if(!u) {
          ui_m(NULL, 0, "User has left the hub.");
        } else {
          t = ui_msg_create(cc->hub, u);
          ui_tab_open(t, TRUE, ui_conn);
        }
      }
    }
    break;
  case INPT_CHAR('d'): // d - disconnect
    if(!cc)
      ui_m(NULL, 0, "Nothing selected.");
    else if(!cc->net->conn && !cc->net->connecting)
      ui_m(NULL, 0, "Not connected.");
    else
      cc_disconnect(cc);
    break;
  case INPT_CHAR('q'): // q - find queue item
    if(!cc)
      ui_m(NULL, 0, "Nothing selected.");
    else if(!cc->dl || !cc->last_file)
      ui_m(NULL, 0, "Not downloading a file.");
    else {
      struct dl *dl = g_hash_table_lookup(dl_queue, cc->last_hash);
      if(!dl)
        ui_m(NULL, 0, "File has been removed from the queue.");
      else {
        if(ui_dl)
          ui_tab_cur = g_list_find(ui_tabs, ui_dl);
        else
          ui_tab_open(ui_dl_create(), TRUE, ui_conn);
        ui_dl_select(dl, cc->uid);
      }
    }
    break;
  }
}







// File list browser (UIT_FL)

// Columns to sort on
#define UIFL_NAME  0
#define UIFL_SIZE  1


static gint ui_fl_sort(gconstpointer a, gconstpointer b, gpointer dat) {
  const struct fl_list *la = a;
  const struct fl_list *lb = b;
  struct ui_tab *tab = dat;

  // dirs before files
  if(tab->fl_dirfirst && !!la->isfile != !!lb->isfile)
    return la->isfile ? 1 : -1;

  int r = tab->order == UIFL_NAME ? fl_list_cmp(la, lb) : (la->size > lb->size ? 1 : la->size < lb->size ? -1 : 0);
  if(!r)
    r = tab->order == UIFL_NAME ? (la->size > lb->size ? 1 : la->size < lb->size ? -1 : 0) : fl_list_cmp(la, lb);
  return tab->o_reverse ? -r : r;
}


static void ui_fl_setdir(struct ui_tab *tab, struct fl_list *fl, struct fl_list *sel) {
  // Free previously opened dir
  if(tab->list) {
    g_sequence_free(tab->list->list);
    ui_listing_free(tab->list);
  }
  // Open this one and select *sel, if set
  tab->fl_list = fl;
  GSequence *seq = g_sequence_new(NULL);
  GSequenceIter *seli = NULL;
  int i;
  for(i=0; i<fl->sub->len; i++) {
    GSequenceIter *iter = g_sequence_insert_sorted(seq, g_ptr_array_index(fl->sub, i), ui_fl_sort, tab);
    if(sel == g_ptr_array_index(fl->sub, i))
      seli = iter;
  }
  tab->list = ui_listing_create(seq);
  if(seli)
    tab->list->sel = seli;
}


static void ui_fl_matchqueue(struct ui_tab *tab, struct fl_list *root) {
  if(!tab->fl_list) {
    tab->fl_match = TRUE;
    return;
  }

  if(!root) {
    root = tab->fl_list;
    while(root->parent)
      root = root->parent;
  }
  int a = 0;
  int n = dl_queue_match_fl(tab->uid, root, &a);
  ui_mf(NULL, 0, "Matched %d files, %d new.", n, a);
  tab->fl_match = FALSE;
}


static void ui_fl_dosel(struct ui_tab *tab, struct fl_list *fl, const char *sel) {
  struct fl_list *root = fl;
  while(root->parent)
    root = root->parent;
  struct fl_list *n = fl_list_from_path(root, sel);
  if(!n)
    ui_mf(tab, 0, "Can't select `%s': item not found.", sel);
  // open the parent directory and select item
  ui_fl_setdir(tab, n?n->parent:fl, n);
}


// Callback function for use in ui_fl_queue() - not associated with any tab.
// Will just match the list against the queue and free it.
static void ui_fl_loadmatch(struct fl_list *fl, GError *err, void *dat) {
  guint64 uid = *(guint64 *)dat;
  g_free(dat);
  struct hub_user *u = g_hash_table_lookup(hub_uids, &uid);
  char *user = u
    ? g_strdup_printf("%s on %s", u->name, u->hub->tab->name)
    : g_strdup_printf("%016"G_GINT64_MODIFIER"x (user offline)", uid);

  if(err) {
    ui_mf(ui_main, 0, "Error opening file list of %s for matching: %s", user, err->message);
    g_error_free(err);
  } else {
    int a = 0;
    int n = dl_queue_match_fl(uid, fl, &a);
    ui_mf(NULL, 0, "Matched queue for %s: %d files, %d new.", user, n, a);
    fl_list_free(fl);
  }
  g_free(user);
}


// Open/match or queue a file list. (Not really a ui_* function, but where else would it belong?)
void ui_fl_queue(guint64 uid, gboolean force, const char *sel, struct ui_tab *parent, gboolean open, gboolean match) {
  if(!open && !match)
    return;

  struct hub_user *u = g_hash_table_lookup(hub_uids, &uid);
  // check for u == we
  if(u && (u->hub->adc ? u->hub->sid == u->sid : u->hub->nick_valid && strcmp(u->hub->nick_hub, u->name_hub) == 0)) {
    u = NULL;
    uid = 0;
  }
  g_warn_if_fail(uid || !match);

  // check for existing tab
  GList *n;
  struct ui_tab *t;
  for(n=ui_tabs; n; n=n->next) {
    t = n->data;
    if(t->type == UIT_FL && t->uid == uid)
      break;
  }
  if(n) {
    if(open)
      ui_tab_cur = n;
    if(sel) {
      if(!t->fl_loading && t->fl_list)
        ui_fl_dosel(n->data, t->fl_list, sel);
      else if(t->fl_loading) {
        g_free(t->fl_sel);
        t->fl_sel = g_strdup(sel);
      }
    }
    if(match)
      ui_fl_matchqueue(t, NULL);
    return;
  }

  // open own list
  if(!uid) {
    if(open)
      ui_tab_open(ui_fl_create(0, sel), TRUE, parent);
    return;
  }

  // check for cached file list, otherwise queue it
  char *tmp = g_strdup_printf("%016"G_GINT64_MODIFIER"x.xml.bz2", uid);
  char *fn = g_build_filename(db_dir, "fl", tmp, NULL);
  g_free(tmp);

  gboolean e = !force;
  if(!force) {
    struct stat st;
    int age = var_get_int(0, VAR_filelist_maxage);
    e = stat(fn, &st) < 0 || st.st_mtime < time(NULL)-MAX(age, 30) ? FALSE : TRUE;
  }
  if(e) {
    if(open) {
      t = ui_fl_create(uid, sel);
      ui_tab_open(t, TRUE, parent);
      if(match)
        ui_fl_matchqueue(t, NULL);
    } else if(match)
      fl_load_async(fn, ui_fl_loadmatch, g_memdup(&uid, 8));
  } else {
    g_return_if_fail(u); // the caller should have checked this
    dl_queue_addlist(u, sel, parent, open, match);
    ui_mf(NULL, 0, "File list of %s added to the download queue.", u->name);
  }

  g_free(fn);
}


static void ui_fl_loaddone(struct fl_list *fl, GError *err, void *dat) {
  // If the tab has been closed, then we can ignore the result
  if(!g_list_find(ui_tabs, dat)) {
    if(fl)
      fl_list_free(fl);
    if(err)
      g_error_free(err);
    return;
  }
  // Otherwise, update state
  struct ui_tab *tab = dat;
  tab->fl_err = err;
  tab->fl_loading = FALSE;
  tab->prio = err ? UIP_HIGH : UIP_MED;
  if(tab->fl_sel) {
    if(fl)
      ui_fl_dosel(tab, fl, tab->fl_sel);
    g_free(tab->fl_sel);
    tab->fl_sel = NULL;
  } else if(fl)
    ui_fl_setdir(tab, fl, NULL);
}


struct ui_tab *ui_fl_create(guint64 uid, const char *sel) {
  // get user
  struct hub_user *u = uid ? g_hash_table_lookup(hub_uids, &uid) : NULL;

  // create tab
  struct ui_tab *tab = g_new0(struct ui_tab, 1);
  tab->type = UIT_FL;
  tab->name = !uid ? g_strdup("/own") : u ? g_strdup_printf("/%s", u->name) : g_strdup_printf("/%016"G_GINT64_MODIFIER"x", uid);
  tab->fl_uname = u ? g_strdup(u->name) : NULL;
  tab->uid = uid;
  tab->fl_dirfirst = TRUE;
  tab->order = UIFL_NAME;

  // get file list
  if(!uid) {
    struct fl_list *fl = fl_local_list ? fl_list_copy(fl_local_list) : NULL;
    tab->prio = UIP_MED;
    if(fl && fl->sub && sel)
      ui_fl_dosel(tab, fl, sel);
    else if(fl && fl->sub)
      ui_fl_setdir(tab, fl, NULL);
  } else {
    char *tmp = g_strdup_printf("%016"G_GINT64_MODIFIER"x.xml.bz2", uid);
    char *fn = g_build_filename(db_dir, "fl", tmp, NULL);
    fl_load_async(fn, ui_fl_loaddone, tab);
    g_free(tmp);
    g_free(fn);
    tab->prio = UIP_LOW;
    tab->fl_loading = TRUE;
    if(sel)
      tab->fl_sel = g_strdup(sel);
  }

  return tab;
}


void ui_fl_close(struct ui_tab *tab) {
  if(tab->list) {
    g_sequence_free(tab->list->list);
    ui_listing_free(tab->list);
  }
  ui_tab_remove(tab);
  struct fl_list *p = tab->fl_list;
  while(p && p->parent)
    p = p->parent;
  if(p)
    fl_list_free(p);
  if(tab->fl_err)
    g_error_free(tab->fl_err);
  g_free(tab->fl_sel);
  g_free(tab->name);
  g_free(tab->fl_uname);
  g_free(tab);
}


static char *ui_fl_title(struct ui_tab *tab) {
  return  !tab->uid ? g_strdup_printf("Browsing own file list.")
    : tab->fl_uname ? g_strdup_printf("Browsing file list of %s (%016"G_GINT64_MODIFIER"x)", tab->fl_uname, tab->uid)
    : g_strdup_printf("Browsing file list of %016"G_GINT64_MODIFIER"x (user offline)", tab->uid);
}


static void ui_fl_draw_row(struct ui_listing *list, GSequenceIter *iter, int row, void *dat) {
  struct fl_list *fl = g_sequence_get(iter);

  attron(iter == list->sel ? UIC(list_select) : UIC(list_default));
  mvhline(row, 0, ' ', wincols);
  if(iter == list->sel)
    mvaddstr(row, 0, ">");

  mvaddch(row, 2, fl->isfile && !fl->hastth ? 'H' :' ');

  mvaddstr(row, 4, str_formatsize(fl->size));
  if(!fl->isfile)
    mvaddch(row, 17, '/');
  mvaddnstr(row, 18, fl->name, str_offset_from_columns(fl->name, wincols-19));

  attroff(iter == list->sel ? UIC(list_select) : UIC(list_default));
}


static void ui_fl_draw(struct ui_tab *tab) {
  // first line
  mvhline(1, 0, ACS_HLINE, wincols);
  mvaddch(1, 3, ' ');
  char *path = tab->fl_list ? fl_list_path(tab->fl_list) : g_strdup("/");
  int c = str_columns(path) - wincols + 8;
  mvaddstr(1, 4, path+str_offset_from_columns(path, MAX(0, c)));
  g_free(path);
  addch(' ');

  // rows
  int pos = -1;
  if(tab->fl_loading)
    mvaddstr(3, 2, "Loading filelist...");
  else if(tab->fl_err)
    mvprintw(3, 2, "Error loading filelist: %s", tab->fl_err->message);
  else if(tab->fl_list && tab->fl_list->sub && tab->fl_list->sub->len)
    pos = ui_listing_draw(tab->list, 2, winrows-4, ui_fl_draw_row, NULL);
  else
    mvaddstr(3, 2, "Directory empty.");

  // footer
  struct fl_list *sel = pos >= 0 && !g_sequence_iter_is_end(tab->list->sel) ? g_sequence_get(tab->list->sel) : NULL;
  attron(UIC(separator));
  mvhline(winrows-3, 0, ' ', wincols);
  if(pos >= 0)
    mvprintw(winrows-3, wincols-34, "%6d items   %s   %3d%%", tab->fl_list->sub->len, str_formatsize(tab->fl_list->size), pos);
  if(sel && sel->isfile) {
    if(!sel->hastth)
      mvaddstr(winrows-3, 0, "Not hashed yet, this file is not visible to others.");
    else {
      char hash[40] = {};
      base32_encode(sel->tth, hash);
      mvaddstr(winrows-3, 0, hash);
      mvprintw(winrows-3, 40, "(%s bytes)", str_fullsize(sel->size));
    }
  }
  if(sel && !sel->isfile) {
    int num = sel->sub ? sel->sub->len : 0;
    if(!num)
      mvaddstr(winrows-3, 0, " Selected directory is empty.");
    else
      mvprintw(winrows-3, 0, " %d items, %s bytes", num, str_fullsize(sel->size));
  }
  attroff(UIC(separator));
}


static void ui_fl_key(struct ui_tab *tab, guint64 key) {
  if(tab->list && ui_listing_key(tab->list, key, winrows/2))
    return;

  struct fl_list *sel = !tab->list || g_sequence_iter_is_end(tab->list->sel) ? NULL : g_sequence_get(tab->list->sel);
  gboolean sort = FALSE;

  switch(key) {
  case INPT_CHAR('?'):
    ui_main_keys("browse");
    break;

  case INPT_CTRL('j'):      // newline
  case INPT_KEY(KEY_RIGHT): // right
  case INPT_CHAR('l'):      // l          open selected directory
    if(sel && !sel->isfile && sel->sub)
      ui_fl_setdir(tab, sel, NULL);
    break;

  case INPT_CTRL('h'):     // backspace
  case INPT_KEY(KEY_LEFT): // left
  case INPT_CHAR('h'):     // h          open parent directory
    if(tab->fl_list && tab->fl_list->parent)
      ui_fl_setdir(tab, tab->fl_list->parent, tab->fl_list);
    break;

  // Sorting
#define SETSORT(c) \
  tab->o_reverse = tab->order == c ? !tab->o_reverse : FALSE;\
  tab->order = c;\
  sort = TRUE;

  case INPT_CHAR('s'): // s - sort on file size
    SETSORT(UIFL_SIZE);
    break;
  case INPT_CHAR('n'): // n - sort on file name
    SETSORT(UIFL_NAME);
    break;
  case INPT_CHAR('t'): // o - toggle sorting dirs before files
    tab->fl_dirfirst = !tab->fl_dirfirst;
    sort = TRUE;
    break;
#undef SETSORT

  case INPT_CHAR('d'): // d (download)
    if(!sel)
      ui_m(NULL, 0, "Nothing selected.");
    else if(!tab->uid)
      ui_m(NULL, 0, "Can't download from yourself.");
    else if(!sel->isfile && fl_list_isempty(sel))
      ui_m(NULL, 0, "Directory empty.");
    else {
      g_return_if_fail(!sel->isfile || sel->hastth);
      char *excl = var_get(0, VAR_download_exclude);
      GRegex *r = excl ? g_regex_new(excl, 0, 0, NULL) : NULL;
      dl_queue_add_fl(tab->uid, sel, NULL, r);
      if(r)
        g_regex_unref(r);
    }
    break;

  case INPT_CHAR('m'): // m - match queue with selected file/dir
  case INPT_CHAR('M'): // M - match queue with entire file list
    if(!tab->fl_list)
      ui_m(NULL, 0, "File list empty.");
    else if(!tab->uid)
      ui_m(NULL, 0, "Can't download from yourself.");
    else if(key == INPT_CHAR('m') && !sel)
      ui_m(NULL, 0, "Nothing selected.");
    else
      ui_fl_matchqueue(tab, key == INPT_CHAR('m') ? sel : NULL);
    break;

  case INPT_CHAR('a'): // a - search for alternative sources
    if(!sel)
      ui_m(NULL, 0, "Nothing selected.");
    else if(!sel->isfile)
      ui_m(NULL, 0, "Can't look for alternative sources for directories.");
    else if(!sel->hastth)
      ui_m(NULL, 0, "No TTH hash known.");
    else
      search_alltth(sel->tth, tab);
    break;
  }

  if(sort && tab->fl_list) {
    g_sequence_sort(tab->list->list, ui_fl_sort, tab);
    ui_listing_sorted(tab->list);
    ui_mf(NULL, 0, "Ordering by %s (%s%s)",
      tab->order == UIFL_NAME  ? "file name" : "file size",
      tab->o_reverse ? "descending" : "ascending", tab->fl_dirfirst ? ", dirs first" : "");
  }
}




// Download queue tab (UIT_DL)

// these can only be one download queue tab, so this can be static
struct ui_tab *ui_dl;


static gint ui_dl_sort_func(gconstpointer da, gconstpointer db, gpointer dat) {
  const struct dl *a = da;
  const struct dl *b = db;
  return a->islist && !b->islist ? -1 : !a->islist && b->islist ? 1 : strcmp(a->dest, b->dest);
}


// Note that we sort on username, uid. But we do not get a notification when a
// user changes offline/online state, thus don't have the ability to keep the
// list sorted reliably. This isn't a huge problem, though, the list is
// removed/recreated every time an other dl item is selected. This sorting is
// just better than having the users in completely random order all the time.
static gint ui_dl_dud_sort_func(gconstpointer da, gconstpointer db, gpointer dat) {
  const struct dl_user_dl *a = da;
  const struct dl_user_dl *b = db;
  struct hub_user *ua = g_hash_table_lookup(hub_uids, &a->u->uid);
  struct hub_user *ub = g_hash_table_lookup(hub_uids, &b->u->uid);
  return !ua && !ub ? (a->u->uid > b->u->uid ? 1 : a->u->uid < b->u->uid ? -1 : 0) :
     ua && !ub ? 1 : !ua && ub ? -1 : g_utf8_collate(ua->name, ub->name);
}


static void ui_dl_setusers(struct dl *dl) {
  if(ui_dl->dl_cur == dl)
    return;
  // free
  if(!dl) {
    if(ui_dl->dl_cur && ui_dl->dl_users) {
      g_sequence_free(ui_dl->dl_users->list);
      ui_listing_free(ui_dl->dl_users);
    }
    ui_dl->dl_users = NULL;
    ui_dl->dl_cur = NULL;
    return;
  }
  // create
  ui_dl_setusers(NULL);
  GSequence *l = g_sequence_new(NULL);
  int i;
  for(i=0; i<dl->u->len; i++)
    g_sequence_insert_sorted(l, g_sequence_get(g_ptr_array_index(dl->u, i)), ui_dl_dud_sort_func, NULL);
  ui_dl->dl_users = ui_listing_create(l);
  ui_dl->dl_cur = dl;
}


struct ui_tab *ui_dl_create() {
  ui_dl = g_new0(struct ui_tab, 1);
  ui_dl->name = "queue";
  ui_dl->type = UIT_DL;
  // create and pupulate the list
  GSequence *l = g_sequence_new(NULL);
  GHashTableIter iter;
  g_hash_table_iter_init(&iter, dl_queue);
  struct dl *dl;
  while(g_hash_table_iter_next(&iter, NULL, (gpointer *)&dl))
    dl->iter = g_sequence_insert_sorted(l, dl, ui_dl_sort_func, NULL);
  ui_dl->list = ui_listing_create(l);
  return ui_dl;
}


void ui_dl_select(struct dl *dl, guint64 uid) {
  g_return_if_fail(ui_dl);

  // dl->iter should always be valid if the dl tab is open
  ui_dl->list->sel = dl->iter;

  // select the right user
  if(uid) {
    ui_dl->details = TRUE;
    ui_dl_setusers(dl);
    GSequenceIter *i = g_sequence_get_begin_iter(ui_dl->dl_users->list);
    for(; !g_sequence_iter_is_end(i); i=g_sequence_iter_next(i))
      if(((struct dl_user_dl *)g_sequence_get(i))->u->uid == uid) {
        ui_dl->dl_users->sel = i;
        break;
      }
  }
}


void ui_dl_close() {
  ui_tab_remove(ui_dl);
  ui_dl_setusers(NULL);
  g_sequence_free(ui_dl->list->list);
  ui_listing_free(ui_dl->list);
  g_free(ui_dl);
  ui_dl = NULL;
}


#if INTERFACE
#define UIDL_ADD 0
#define UIDL_DEL 1
#endif


void ui_dl_listchange(struct dl *dl, int change) {
  g_return_if_fail(ui_dl);
  switch(change) {
  case UICONN_ADD:
    dl->iter = g_sequence_insert_sorted(ui_dl->list->list, dl, ui_dl_sort_func, NULL);
    ui_listing_inserted(ui_dl->list);
    break;
  case UICONN_DEL:
    if(dl == ui_dl->dl_cur)
      ui_dl_setusers(NULL);
    ui_listing_remove(ui_dl->list, dl->iter);
    g_sequence_remove(dl->iter);
    break;
  }
}


void ui_dl_dud_listchange(struct dl_user_dl *dud, int change) {
  g_return_if_fail(ui_dl);
  if(dud->dl != ui_dl->dl_cur || !ui_dl->dl_users)
    return;
  switch(change) {
  case UICONN_ADD:
    // Note that _insert_sorted() may not actually insert the item in the
    // correct position, since the list is not guaranteed to be correctly
    // sorted in the first place.
    g_sequence_insert_sorted(ui_dl->dl_users->list, dud, ui_dl_dud_sort_func, NULL);
    ui_listing_inserted(ui_dl->list);
    break;
  case UICONN_DEL: ;
    GSequenceIter *i;
    for(i=g_sequence_get_begin_iter(ui_dl->dl_users->list); !g_sequence_iter_is_end(i); i=g_sequence_iter_next(i))
      if(g_sequence_get(i) == dud)
        break;
    if(!g_sequence_iter_is_end(i)) {
      ui_listing_remove(ui_dl->dl_users, i);
      g_sequence_remove(i);
    }
    break;
  }
}


static char *ui_dl_title() {
  return g_strdup("Download queue");
}


static void ui_dl_draw_row(struct ui_listing *list, GSequenceIter *iter, int row, void *dat) {
  struct dl *dl = g_sequence_get(iter);

  attron(iter == list->sel ? UIC(list_select) : UIC(list_default));
  mvhline(row, 0, ' ', wincols);
  if(iter == list->sel)
    mvaddstr(row, 0, ">");

  int online = 0;
  int i;
  for(i=0; i<dl->u->len; i++)
    if(g_hash_table_lookup(hub_uids, &(((struct dl_user_dl *)g_sequence_get(g_ptr_array_index(dl->u, i)))->u->uid)))
      online++;
  mvprintw(row, 2, "%2d/%2d", online, dl->u->len);

  mvaddstr(row, 9, str_formatsize(dl->size));
  if(dl->size)
    mvprintw(row, 20, "%3d%%", (int) ((dl->have*100)/dl->size));
  else
    mvaddstr(row, 20, " -");

  if(dl->prio == DLP_ERR)
    mvaddstr(row, 26, " ERR");
  else if(dl->prio == DLP_OFF)
    mvaddstr(row, 26, " OFF");
  else
    mvprintw(row, 26, "%3d", dl->prio);

  if(dl->islist)
    mvaddstr(row, 32, "files.xml.bz2");
  else {
    char *def = var_get(0, VAR_download_dir);
    int len = strlen(def);
    char *dest = strncmp(def, dl->dest, len) == 0 ? dl->dest+len+(dl->dest[len-1] == '/' ? 0 : 1) : dl->dest;
    mvaddnstr(row, 32, dest, str_offset_from_columns(dest, wincols-32));
  }

  attroff(iter == list->sel ? UIC(list_select) : UIC(list_default));
}


static void ui_dl_dud_draw_row(struct ui_listing *list, GSequenceIter *iter, int row, void *dat) {
  struct dl_user_dl *dud = g_sequence_get(iter);

  attron(iter == list->sel ? UIC(list_select) : UIC(list_default));
  mvhline(row, 0, ' ', wincols);
  if(iter == list->sel)
    mvaddstr(row, 0, ">");

  struct hub_user *u = g_hash_table_lookup(hub_uids, &dud->u->uid);
  if(u) {
    mvaddnstr(row, 2, u->name, str_offset_from_columns(u->name, 19));
    mvaddnstr(row, 22, u->hub->tab->name, str_offset_from_columns(u->hub->tab->name, 13));
  } else
    mvprintw(row, 2, "ID:%016"G_GINT64_MODIFIER"x (offline)", dud->u->uid);

  if(dud->error)
    mvprintw(row, 36, "Error: %s", dl_strerror(dud->error, dud->error_msg));
  else if(dud->u->active == dud)
    mvaddstr(row, 36, "Downloading.");
  else if(dud->u->state == DLU_ACT)
    mvaddstr(row, 36, "Downloading an other file.");
  else if(dud->u->state == DLU_EXP)
    mvaddstr(row, 36, "Connecting.");
  else
    mvaddstr(row, 36, "Idle.");

  attroff(iter == list->sel ? UIC(list_select) : UIC(list_default));
}


static void ui_dl_draw() {
  attron(UIC(list_header));
  mvhline(1, 0, ' ', wincols);
  mvaddstr(1, 2,  "Users");
  mvaddstr(1, 9,  "Size");
  mvaddstr(1, 20, "Done");
  mvaddstr(1, 26, "Prio");
  mvaddstr(1, 32, "File");
  attroff(UIC(list_header));

  int bottom = ui_dl->details ? winrows-14 : winrows-4;
  int pos = ui_listing_draw(ui_dl->list, 2, bottom-1, ui_dl_draw_row, NULL);

  struct dl *sel = g_sequence_iter_is_end(ui_dl->list->sel) ? NULL : g_sequence_get(ui_dl->list->sel);

  // footer / separator
  attron(UIC(separator));
  mvhline(bottom, 0, ' ', wincols);
  if(sel) {
    char hash[40] = {};
    base32_encode(sel->hash, hash);
    mvprintw(bottom, 0, hash);
  } else
    mvaddstr(bottom, 0, "Nothing selected.");
  mvprintw(bottom, wincols-19, "%5d files - %3d%%", g_hash_table_size(dl_queue), pos);
  attroff(UIC(separator));

  // error info
  if(sel && sel->prio == DLP_ERR)
    mvprintw(++bottom, 0, "Error: %s", dl_strerror(sel->error, sel->error_msg));

  // user list
  if(sel && ui_dl->details) {
    ui_dl_setusers(sel);
    attron(A_BOLD);
    mvaddstr(bottom+1, 2, "User");
    mvaddstr(bottom+1, 22, "Hub");
    mvaddstr(bottom+1, 36, "Status");
    attroff(A_BOLD);
    if(!ui_dl->dl_users || !g_sequence_get_length(ui_dl->dl_users->list))
      mvaddstr(bottom+3, 0, "  No users for this download.");
    else
      ui_listing_draw(ui_dl->dl_users, bottom+2, winrows-3, ui_dl_dud_draw_row, NULL);
  }
}


static void ui_dl_key(guint64 key) {
  if(ui_listing_key(ui_dl->list, key, (winrows-(ui_dl->details?14:4))/2))
    return;

  struct dl *sel = g_sequence_iter_is_end(ui_dl->list->sel) ? NULL : g_sequence_get(ui_dl->list->sel);
  struct dl_user_dl *usel = NULL;
  if(!ui_dl->details)
    usel = NULL;
  else {
    ui_dl_setusers(sel);
    usel = !ui_dl->dl_users || g_sequence_iter_is_end(ui_dl->dl_users->sel) ? NULL : g_sequence_get(ui_dl->dl_users->sel);
  }

  switch(key) {
  case INPT_CHAR('?'):
    ui_main_keys("queue");
    break;

  case INPT_CHAR('J'): // J - user down
    if(ui_dl->details && ui_dl->dl_users) {
      ui_dl->dl_users->sel = g_sequence_iter_next(ui_dl->dl_users->sel);
      if(g_sequence_iter_is_end(ui_dl->dl_users->sel))
        ui_dl->dl_users->sel = g_sequence_iter_prev(ui_dl->dl_users->sel);
    }
    break;
  case INPT_CHAR('K'): // K - user up
    if(ui_dl->details && ui_dl->dl_users)
      ui_dl->dl_users->sel = g_sequence_iter_prev(ui_dl->dl_users->sel);
    break;

  case INPT_CHAR('f'): // f - find user
    if(!usel)
      ui_m(NULL, 0, "No user selected.");
    else {
      struct hub_user *u = g_hash_table_lookup(hub_uids, &usel->u->uid);
      if(!u)
        ui_m(NULL, 0, "User is not online.");
      else
        ui_hub_finduser(u->hub->tab, u->uid, NULL, FALSE);
    }
    break;
  case INPT_CHAR('d'): // d - remove item
    if(!sel)
      ui_m(NULL, 0, "Nothing selected.");
    else {
      ui_mf(NULL, 0, "Removed `%s' from queue.", sel->dest);
      dl_queue_rm(sel);
    }
    break;
  case INPT_CHAR('c'): // c - find connection
    if(!sel)
      ui_m(NULL, 0, "Nothing selected.");
    else if(!sel->active)
      ui_m(NULL, 0, "Download not in progress.");
    else {
      struct cc *cc = NULL;
      int i;
      for(i=0; i<sel->u->len; i++) {
        struct dl_user_dl *dud = g_sequence_get(g_ptr_array_index(sel->u, i));
        if(dud->u->active == dud) {
          cc = dud->u->cc;
          break;
        }
      }
      if(!cc)
        ui_m(NULL, 0, "Download not in progress.");
      else {
        if(ui_conn)
          ui_tab_cur = g_list_find(ui_tabs, ui_conn);
        else
          ui_tab_open(ui_conn_create(), TRUE, ui_dl);
        // cc->iter should be valid at this point
        ui_conn->list->sel = cc->iter;
      }
    }
    break;
  case INPT_CHAR('a'): // a - search for alternative sources
    if(!sel)
      ui_m(NULL, 0, "Nothing selected.");
    else if(sel->islist)
      ui_m(NULL, 0, "Can't search for alternative sources for file lists.");
    else
      search_alltth(sel->hash, ui_dl);
    break;
  case INPT_CHAR('R'): // R - remove user from all queued files
  case INPT_CHAR('r'): // r - remove user from file
    if(!usel)
      ui_m(NULL, 0, "No user selected.");
    else {
      dl_queue_rmuser(usel->u->uid, key == INPT_CHAR('R') ? NULL : sel->hash);
      ui_m(NULL, 0, key == INPT_CHAR('R') ? "Removed user from the download queue." : "Removed user for this file.");
    }
    break;
  case INPT_CHAR('x'): // x - clear user-specific error state
  case INPT_CHAR('X'): // X - clear user-specific error state for all files
    if(!usel)
      ui_m(NULL, 0, "No user selected.");
    else if(key == INPT_CHAR('x') && !usel->error)
      ui_m(NULL, 0, "Selected user is not in an error state.");
    else
      dl_queue_setuerr(usel->u->uid, key == INPT_CHAR('X') ? NULL : sel->hash, 0, 0);
    break;
  case INPT_CHAR('+'): // +
  case INPT_CHAR('='): // = - increase priority
    if(!sel)
      ui_m(NULL, 0, "Nothing selected.");
    else if(sel->prio >= 2)
      ui_m(NULL, 0, "Already set to highest priority.");
    else
      dl_queue_setprio(sel, sel->prio == DLP_ERR ? 0 : sel->prio == DLP_OFF ? -2 : sel->prio+1);
    break;
  case INPT_CHAR('-'): // - - decrease priority
    if(!sel)
      ui_m(NULL, 0, "Nothing selected.");
    else if(sel->prio <= DLP_OFF)
      ui_m(NULL, 0, "Item already disabled.");
    else
      dl_queue_setprio(sel, sel->prio == -2 ? DLP_OFF : sel->prio-1);
    break;
  case INPT_CTRL('j'): // newline
  case INPT_CHAR('i'): // i       (toggle user list)
    ui_dl->details = !ui_dl->details;
    break;
  }
}




// Search results tab (UIT_SEARCH)


// Columns to sort on
#define UISCH_USER  0
#define UISCH_SIZE  1
#define UISCH_SLOTS 2
#define UISCH_FILE  3


// Note: The ordering of the results partly depends on whether the user is
// online or not (i.e. whether we know its name and hub). However, we do not
// get notified when a user or hub changes state and can therefore not keep the
// ordering of the list correct. This isn't a huge problem, though.


// Compares users, uses a hub comparison as fallback
static int ui_search_cmp_user(guint64 ua, guint64 ub) {
  struct hub_user *a = g_hash_table_lookup(hub_uids, &ua);
  struct hub_user *b = g_hash_table_lookup(hub_uids, &ub);
  int o =
    !a && !b ? (ua > ub ? 1 : ua < ub ? -1 : 0) :
     a && !b ? 1 : !a && b ? -1 : g_utf8_collate(a->name, b->name);
  if(!o && a && b)
    return g_utf8_collate(a->hub->tab->name, b->hub->tab->name);
  return o;
}


static int ui_search_cmp_file(const char *fa, const char *fb) {
  const char *a = strrchr(fa, '/');
  const char *b = strrchr(fb, '/');
  return g_utf8_collate(a?a+1:fa, b?b+1:fb);
}


static gint ui_search_sort_func(gconstpointer da, gconstpointer db, gpointer dat) {
  const struct search_r *a = da;
  const struct search_r *b = db;
  struct ui_tab *tab = dat;
  int p = tab->order;

  /* Sort columns and their alternatives:
   * USER:  user/hub  -> file name -> file size
   * SIZE:  size      -> TTH       -> file name
   * SLOTS: slots     -> user/hub  -> file name
   * FILE:  file name -> size      -> TTH
   */
#define CMP_USER  ui_search_cmp_user(a->uid, b->uid)
#define CMP_SIZE  (a->size == b->size ? 0 : (a->size == G_MAXUINT64 ? 0 : a->size) > (b->size == G_MAXUINT64 ? 0 : b->size) ? 1 : -1)
#define CMP_SLOTS (a->slots > b->slots ? 1 : a->slots < b->slots ? -1 : 0)
#define CMP_FILE  ui_search_cmp_file(a->file, b->file)
#define CMP_TTH   memcmp(a->tth, b->tth, 24)

  // Try 1
  int o = p == UISCH_USER ? CMP_USER : p == UISCH_SIZE ? CMP_SIZE : p == UISCH_SLOTS ? CMP_SLOTS : CMP_FILE;
  // Try 2
  if(!o)
    o = p == UISCH_USER ? CMP_FILE : p == UISCH_SIZE ? CMP_TTH : p == UISCH_SLOTS ? CMP_USER : CMP_SIZE;
  // Try 3
  if(!o)
    o = p == UISCH_USER ? CMP_SIZE : p == UISCH_SIZE ? CMP_FILE : p == UISCH_SLOTS ? CMP_FILE : CMP_TTH;
  return tab->o_reverse ? -o : o;
}


// Called when a new search result has been received. Looks through the opened
// search tabs and adds the result to the list if it matches the query.
void ui_search_global_result(struct search_r *r) {
  GList *n;
  for(n=ui_tabs; n; n=n->next) {
    struct ui_tab *t = n->data;
    if(t->type == UIT_SEARCH && search_match(t->search_q, r)) {
      g_sequence_insert_sorted(t->list->list, search_r_copy(r), ui_search_sort_func, t);
      ui_listing_inserted(t->list);
      t->prio = MAX(t->prio, UIP_LOW);
    }
  }
}


// Ownership of q is passed to the tab, and will be freed on close.
struct ui_tab *ui_search_create(struct hub *hub, struct search_q *q) {
  struct ui_tab *tab = g_new0(struct ui_tab, 1);
  tab->type = UIT_SEARCH;
  tab->search_q = q;
  tab->hub = hub;
  tab->search_hide_hub = hub ? TRUE : FALSE;
  tab->order = UISCH_FILE;
  time(&tab->search_t);

  // figure out a suitable tab->name
  if(q->type == 9) {
    tab->name = g_new0(char, 41);
    tab->name[0] = '?';
    base32_encode(q->tth, tab->name+1);
  } else {
    char *s = g_strjoinv(" ", q->query);
    tab->name = g_strdup_printf("?%s", s);
    g_free(s);
  }
  if(strlen(tab->name) > 15)
    tab->name[15] = 0;
  while(tab->name[strlen(tab->name)-1] == ' ')
    tab->name[strlen(tab->name)-1] = 0;

  // Create an empty list
  tab->list = ui_listing_create(g_sequence_new(search_r_free));
  return tab;
}


void ui_search_close(struct ui_tab *tab) {
  search_q_free(tab->search_q);
  g_sequence_free(tab->list->list);
  ui_listing_free(tab->list);
  ui_tab_remove(tab);
  g_free(tab->name);
  g_free(tab);
}


static char *ui_search_title(struct ui_tab *tab) {
  char *sq = search_command(tab->search_q, tab->hub?TRUE:FALSE);
  char *r = tab->hub
    ? g_strdup_printf("Results on %s for: %s", tab->hub->tab->name, sq)
    : g_strdup_printf("Results for: %s", sq);
  g_free(sq);
  return r;
}


// TODO: mark already shared and queued files?
static void ui_search_draw_row(struct ui_listing *list, GSequenceIter *iter, int row, void *dat) {
  struct search_r *r = g_sequence_get(iter);
  struct ui_tab *tab = dat;

  attron(iter == list->sel ? UIC(list_select) : UIC(list_default));
  mvhline(row, 0, ' ', wincols);
  if(iter == list->sel)
    mvaddstr(row, 0, ">");

  struct hub_user *u = g_hash_table_lookup(hub_uids, &r->uid);
  if(u) {
    mvaddnstr(row, 2, u->name, str_offset_from_columns(u->name, 19));
    if(!tab->search_hide_hub)
      mvaddnstr(row, 22, u->hub->tab->name, str_offset_from_columns(u->hub->tab->name, 13));
  } else
    mvprintw(row, 2, "ID:%016"G_GINT64_MODIFIER"x%s", r->uid, !tab->search_hide_hub ? " (offline)" : "");

  int i = tab->search_hide_hub ? 22 : 36;
  if(r->size == G_MAXUINT64)
    mvaddstr(row, i, "   DIR");
  else
    mvaddstr(row, i, str_formatsize(r->size));

  mvprintw(row, i+12, "%3d/", r->slots);
  if(u)
    mvprintw(row, i+16, "%3d", u->slots);
  else
    mvaddstr(row, i+16, "  -");

  char *fn = strrchr(r->file, '/');
  if(fn)
    fn++;
  else
    fn = r->file;
  mvaddnstr(row, i+21, fn, str_offset_from_columns(fn, wincols-i-21));

  attroff(iter == list->sel ? UIC(list_select) : UIC(list_default));
}


static void ui_search_draw(struct ui_tab *tab) {
  attron(UIC(list_header));
  mvhline(1, 0, ' ', wincols);
  mvaddstr(1,    2, "User");
  if(!tab->search_hide_hub)
    mvaddstr(1, 22, "Hub");
  int i = tab->search_hide_hub ? 22 : 36;
  mvaddstr(1, i,    "Size");
  mvaddstr(1, i+12, "Slots");
  mvaddstr(1, i+21, "File");
  attroff(UIC(list_header));

  int bottom = winrows-4;
  int pos = ui_listing_draw(tab->list, 2, bottom-1, ui_search_draw_row, tab);

  struct search_r *sel = g_sequence_iter_is_end(tab->list->sel) ? NULL : g_sequence_get(tab->list->sel);

  // footer
  attron(UIC(separator));
  mvhline(bottom,   0, ' ', wincols);
  if(!sel)
    mvaddstr(bottom, 0, "Nothing selected.");
  else if(sel->size == G_MAXUINT64)
    mvaddstr(bottom, 0, "Directory.");
  else {
    char tth[40] = {};
    base32_encode(sel->tth, tth);
    mvprintw(bottom, 0, "%s (%s bytes)", tth, str_fullsize(sel->size));
  }
  mvprintw(bottom, wincols-29, "%5d results in%4ds - %3d%%",
    g_sequence_get_length(tab->list->list), time(NULL)-tab->search_t, pos);
  attroff(UIC(separator));
  if(sel)
    mvaddnstr(bottom+1, 3, sel->file, str_offset_from_columns(sel->file, wincols-3));
}


static void ui_search_key(struct ui_tab *tab, guint64 key) {
  if(ui_listing_key(tab->list, key, (winrows-4)/2))
    return;

  struct search_r *sel = g_sequence_iter_is_end(tab->list->sel) ? NULL : g_sequence_get(tab->list->sel);
  gboolean sort = FALSE;

  switch(key) {
  case INPT_CHAR('?'):
    ui_main_keys("search");
    break;

  case INPT_CHAR('f'): // f - find user
    if(!sel)
      ui_m(NULL, 0, "Nothing selected.");
    else {
      struct hub_user *u = g_hash_table_lookup(hub_uids, &sel->uid);
      if(!u)
        ui_m(NULL, 0, "User is not online.");
      else
        ui_hub_finduser(u->hub->tab, u->uid, NULL, FALSE);
    }
    break;
  case INPT_CHAR('b'): // b - /browse userlist
  case INPT_CHAR('B'): // B - /browse -f userlist
    if(!sel)
      ui_m(NULL, 0, "Nothing selected.");
    else {
      struct hub_user *u = g_hash_table_lookup(hub_uids, &sel->uid);
      if(!u)
        ui_m(NULL, 0, "User is not online.");
      else
        ui_fl_queue(u->uid, key == INPT_CHAR('B'), sel->file, tab, TRUE, FALSE);
    }
    break;
  case INPT_CHAR('d'): // d - download file
    if(!sel)
      ui_m(NULL, 0, "Nothing selected.");
    else if(sel->size == G_MAXUINT64)
      ui_m(NULL, 0, "Can't download directories from the search. Use 'b' to browse the file list instead.");
    else
      dl_queue_add_res(sel);
    break;

  case INPT_CHAR('m'): // m - match selected item with queue
    if(!sel)
      ui_m(NULL, 0, "Nothing selected.");
    else if(sel->size == G_MAXUINT64)
      ui_m(NULL, 0, "Can't download directories from the search. Use 'b' to browse the file list instead.");
    else {
      int r = dl_queue_matchfile(sel->uid, sel->tth);
      ui_m(NULL, 0, r < 0 ? "File not in the queue." :
                   r == 0 ? "User already in the queue."
                          : "Added user to queue for the selected file.");
    }
    break;

  case INPT_CHAR('M'):;// M - match all results with queue
    int n = 0, a = 0;
    GSequenceIter *i = g_sequence_get_begin_iter(tab->list->list);
    for(; !g_sequence_iter_is_end(i); i=g_sequence_iter_next(i)) {
      struct search_r *r = g_sequence_get(i);
      int v = dl_queue_matchfile(r->uid, r->tth);
      if(v >= 0)
        n++;
      if(v == 1)
        a++;
    }
    ui_mf(NULL, 0, "Matched %d files, %d new.", n, a);
    break;

  case INPT_CHAR('q'): // q - download filelist and match queue for selected user
    if(!sel)
      ui_m(NULL, 0, "Nothing selected.");
    else
      ui_fl_queue(sel->uid, FALSE, NULL, NULL, FALSE, TRUE);
    break;

  case INPT_CHAR('Q'):{// Q - download filelist and match queue for all results
    GSequenceIter *i = g_sequence_get_begin_iter(tab->list->list);
    // Use a hash table to avoid checking the same filelist more than once
    GHashTable *uids = g_hash_table_new(g_int64_hash, g_int64_equal);
    for(; !g_sequence_iter_is_end(i); i=g_sequence_iter_next(i)) {
      struct search_r *r = g_sequence_get(i);
      // In the case that this wasn't a TTH search, check whether this search
      // result matches the queue before checking the file list.
      if(tab->search_q->type == 9 || dl_queue_matchfile(r->uid, r->tth) >= 0)
        g_hash_table_insert(uids, &r->uid, (void *)1);
    }
    GHashTableIter iter;
    g_hash_table_iter_init(&iter, uids);
    guint64 *uid;
    while(g_hash_table_iter_next(&iter, (gpointer *)&uid, NULL))
      ui_fl_queue(*uid, FALSE, NULL, NULL, FALSE, TRUE);
    ui_mf(NULL, 0, "Matching %d file lists...", g_hash_table_size(uids));
    g_hash_table_unref(uids);
  } break;

  case INPT_CHAR('a'): // a - search for alternative sources
    if(!sel)
      ui_m(NULL, 0, "Nothing selected.");
    else if(sel->size == G_MAXUINT64)
      ui_m(NULL, 0, "Can't look for alternative sources for directories.");
    else
      search_alltth(sel->tth, tab);
    break;
  case INPT_CHAR('h'): // h - show/hide hub column
    tab->search_hide_hub = !tab->search_hide_hub;
    break;
  case INPT_CHAR('u'): // u - sort on username
    tab->o_reverse = tab->order == UISCH_USER ? !tab->o_reverse : FALSE;
    tab->order = UISCH_USER;
    sort = TRUE;
    break;
  case INPT_CHAR('s'): // s - sort on size
    tab->o_reverse = tab->order == UISCH_SIZE ? !tab->o_reverse : FALSE;
    tab->order = UISCH_SIZE;
    sort = TRUE;
    break;
  case INPT_CHAR('l'): // l - sort on slots
    tab->o_reverse = tab->order == UISCH_SLOTS ? !tab->o_reverse : FALSE;
    tab->order = UISCH_SLOTS;
    sort = TRUE;
    break;
  case INPT_CHAR('n'): // n - sort on filename
    tab->o_reverse = tab->order == UISCH_FILE ? !tab->o_reverse : FALSE;
    tab->order = UISCH_FILE;
    sort = TRUE;
    break;
  }

  if(sort) {
    g_sequence_sort(tab->list->list, ui_search_sort_func, tab);
    ui_listing_sorted(tab->list);
    ui_mf(NULL, 0, "Ordering by %s (%s)",
      tab->order == UISCH_USER  ? "user name" :
      tab->order == UISCH_SIZE  ? "file size" :
      tab->order == UISCH_SLOTS ? "free slots" : "filename",
      tab->o_reverse ? "descending" : "ascending");
  }
}






// Generic message displaying thing.

#if INTERFACE

// These flags can be OR'ed together with UIP_ flags. No UIP_ flag or UIP_EMPTY
// implies UIP_LOW. There is no need to set any priority when tab == NULL,
// since it will be displayed right away anyway.

// Message should also be notified in status bar (implied automatically if the
// requested tab has no log window). This also uses UIP_EMPTY if no other UIP_
// flag is set.
#define UIM_NOTIFY  4
// Ownership of the message string is passed to the message handling function.
// (Which fill g_free() it after use)
#define UIM_PASS    8
// This is a chat message, i.e. check to see if your name is part of the
// message, and if so, give it UIP_HIGH.
#define UIM_CHAT   16
// Indicates that ui_m_mainthread() is called directly - without using an idle
// function.
#define UIM_DIRECT 32
// Do not log to the tab. Implies UIM_NOTIFY
#define UIM_NOLOG  (64 | UIM_NOTIFY)

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
  else if(!(msg->flags & UIM_DIRECT) && !g_list_find(ui_tabs, tab))
    goto ui_m_cleanup;

  gboolean notify = (msg->flags & UIM_NOTIFY) || !tab->log;

  if(notify && ui_m_text) {
    g_free(ui_m_text);
    ui_m_text = NULL;
    g_source_remove(ui_m_timer);
    ui_m_updated = TRUE;
  }
  if(notify && msg->msg) {
    ui_m_text = g_strdup(msg->msg);
    ui_m_timer = g_timeout_add(3000, ui_m_timeout, NULL);
    ui_m_updated = TRUE;
  }
  if(tab->log && msg->msg && !(msg->flags & (UIM_NOLOG & ~UIM_NOTIFY))) {
    if((msg->flags & UIM_CHAT) && tab->type == UIT_HUB && tab->hub_highlight
        && g_regex_match(tab->hub_highlight, msg->msg, 0, NULL))
      prio = UIP_HIGH;
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
  // call directly if we're running from the main thread. use an idle function
  // otherwise.
  if((dat->flags & UIM_DIRECT) || g_main_context_is_owner(NULL)) {
    dat->flags |= UIM_DIRECT;
    ui_m_mainthread(dat);
  } else
    g_idle_add_full(G_PRIORITY_HIGH_IDLE, ui_m_mainthread, dat, NULL);
}


// UIM_PASS shouldn't be used here (makes no sense).
void ui_mf(struct ui_tab *tab, int flags, const char *fmt, ...) {
  va_list va;
  va_start(va, fmt);
  ui_m(tab, flags | UIM_PASS, g_strdup_vprintf(fmt, va));
  va_end(va);
}





// Global stuff

struct ui_textinput *ui_global_textinput;

void ui_tab_open(struct ui_tab *tab, gboolean sel, struct ui_tab *parent) {
  ui_tabs = g_list_append(ui_tabs, tab);
  tab->parent = parent;
  if(sel)
    ui_tab_cur = g_list_last(ui_tabs);
}


// to be called from ui_*_close()
void ui_tab_remove(struct ui_tab *tab) {
  // Look for any tabs that have this one as parent, and let those inherit this tab's parent
  GList *n = ui_tabs;
  for(; n; n=n->next) {
    struct ui_tab *t = n->data;
    if(t->parent == tab)
      t->parent = tab->parent;
  }
  // If this tab was selected, select its parent or a neighbour
  GList *cur = g_list_find(ui_tabs, tab);
  if(cur == ui_tab_cur) {
    GList *par = tab->parent ? g_list_find(ui_tabs, tab->parent) : NULL;
    ui_tab_cur = par && par != cur ? par : cur->prev ? cur->prev : cur->next;
  }
  // And remove the tab
  ui_tabs = g_list_delete_link(ui_tabs, cur);
}


void ui_init() {
  ui_msg_tabs = g_hash_table_new(g_int64_hash, g_int64_equal);

  // global textinput field
  ui_global_textinput = ui_textinput_create(TRUE, cmd_suggest);

  // first tab = main tab
  ui_tab_open(ui_main_create(), TRUE, NULL);

  // init curses
  initscr();
  raw();
  noecho();
  curs_set(0);
  keypad(stdscr, 1);
  nodelay(stdscr, 1);

  ui_colors_init();

  // draw
  ui_draw();
}


static void ui_draw_status() {
  if(fl_refresh_queue && fl_refresh_queue->head)
    mvaddstr(winrows-1, 0, "[Refreshing share]");
  else if(fl_hash_queue && g_hash_table_size(fl_hash_queue))
    mvprintw(winrows-1, 0, "[Hashing: %d / %s / %.2f MiB/s]",
      g_hash_table_size(fl_hash_queue), str_formatsize(fl_hash_queue_size), ((float)ratecalc_rate(&fl_hash_rate))/(1024.0f*1024.0f));
  mvprintw(winrows-1, wincols-37, "[U/D:%6d/%6d KiB/s]", ratecalc_rate(&net_out)/1024, ratecalc_rate(&net_in)/1024);
  mvprintw(winrows-1, wincols-11, "[S:%3d/%3d]", cc_slots_in_use(NULL), var_get_int(0, VAR_slots));

  ui_m_updated = FALSE;
  if(ui_m_text) {
    mvaddstr(winrows-1, 0, ui_m_text);
    mvaddstr(winrows-1, str_columns(ui_m_text), "   ");
  }
}


#define tabcol(t, n) (2+ceil(log10((n)+1))+str_columns(((struct ui_tab *)(t)->data)->name))
#define prio2a(p) ((p) == UIP_LOW ? UIC(tabprio_low) : (p) == UIP_MED ? UIC(tabprio_med) : UIC(tabprio_high))

/* All tabs are in one of the following states:
 * - Selected                 (tab == ui_tab_cur->data) = sel    "n:name" in A_BOLD
 * - No change                (!sel && tab->prio == UIP_EMPTY)   "n:name" normal
 * - Change, low priority     (!sel && tab->prio == UIP_LOW)     "n!name", with ! in UIC(tabprio_low)
 * - Change, medium priority  (!sel && tab->prio == UIP_MED)     "n!name", with ! in ^_MED
 * - Change, high priority    (!sel && tab->prio == UIP_HIGH)    "n!name", with ! in ^_HIGH
 *
 * The truncated indicators are in the following states:
 * - No changes    ">>" or "<<"
 * - Change        "!>" or "<!"  with ! in same color as above
 */

static void ui_draw_tablist(int xoffset) {
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
  for(; n; n=n->next) {
    w -= tabcol(n, ++i);
    if(w < 0)
      break;
    struct ui_tab *t = n->data;
    addch(' ');
    if(n == ui_tab_cur)
      attron(A_BOLD);
    printw("%d", i);
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
    curtab->type == UIT_CONN     ? ui_conn_title() :
    curtab->type == UIT_FL       ? ui_fl_title(curtab) :
    curtab->type == UIT_DL       ? ui_dl_title() :
    curtab->type == UIT_SEARCH   ? ui_search_title(curtab) : g_strdup("");
  attron(UIC(title));
  mvhline(0, 0, ' ', wincols);
  mvaddstr(0, 0, title);
  attroff(UIC(title));
  g_free(title);

  // second-last line - time and tab list
  mvhline(winrows-2, 0, ACS_HLINE, wincols);
  // time
  int xoffset = 0;
  char *tfmt = var_get(0, VAR_ui_time_format);
  if(strcmp(tfmt, "-") != 0) {
#if GLIB_CHECK_VERSION(2,26,0)
    GDateTime *tm = g_date_time_new_now_local();
    char *ts = g_date_time_format(tm, tfmt);
    mvaddstr(winrows-2, 1, ts);
    xoffset = 2 + str_columns(ts);
    g_free(ts);
    g_date_time_unref(tm);
#else
    // Pre-2.6 users will have a possible buffer overflow and a slightly
    // different formatting function. Just fucking update your system already!
    time_t tm = time(NULL);
    char ts[250];
    strftime(ts, 11, tfmt, localtime(&tm));
    mvaddstr(winrows-2, 1, ts);
    xoffset = 2 + str_columns(ts);
#endif
  }
  // tabs
  ui_draw_tablist(xoffset);

  // last line - status info or notification
  ui_draw_status();

  // tab contents
  switch(curtab->type) {
  case UIT_MAIN:     ui_main_draw(); break;
  case UIT_HUB:      ui_hub_draw(curtab);  break;
  case UIT_USERLIST: ui_userlist_draw(curtab);  break;
  case UIT_MSG:      ui_msg_draw(curtab);  break;
  case UIT_CONN:     ui_conn_draw(); break;
  case UIT_FL:       ui_fl_draw(curtab); break;
  case UIT_DL:       ui_dl_draw(); break;
  case UIT_SEARCH:   ui_search_draw(curtab); break;
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


// Called when the day has changed. Argument is new date.
void ui_daychange(const char *day) {
  char *msg = g_strdup_printf("Day changed to %s", day);
  GList *n = ui_tabs;
  for(; n; n=n->next) {
    struct ui_tab *t = n->data;
    if(t->log)
      ui_logwindow_addline(t->log, msg, TRUE, TRUE);
  }
  g_free(msg);
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

  case INPT_ALT('r'): // alt+r (alias for /refresh)
    cmd_handle("/refresh");
    break;

  case INPT_ALT('o'): // alt+o (alias for /browse)
    cmd_handle("/browse");
    break;

  case INPT_ALT('n'): // alt+n (alias for /connections)
    cmd_handle("/connections");
    break;

  case INPT_ALT('q'): // alt+q (alias for /queue)
    cmd_handle("/queue");
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
      case UIT_FL:       ui_fl_key(curtab, key); break;
      case UIT_DL:       ui_dl_key(key); break;
      case UIT_SEARCH:   ui_search_key(curtab, key); break;
      }
    }
    // TODO: some user feedback on invalid key
  }
}

