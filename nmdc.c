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
#include <stdlib.h>


#if INTERFACE

struct nmdc_user {
  gboolean hasinfo : 1;
  gboolean isop : 1;
  char *name;     // UTF-8
  char *name_hub; // hub-encoded (used as hash key)
  char *desc;
  char *tag;
  char *conn;
  char *mail;
  guint64 sharesize;
  GSequenceIter *iter; // used by ui_userlist_*
}


#define HUBS_IDLE       0 // must be 0
#define HUBS_CONNECTING 1
#define HUBS_CONNECTED  2

struct nmdc_hub {
  struct ui_tab *tab; // to get name (for config) and for logging & setting of title
  struct net *net;
  int state;
  // nick as used in this connection, NULL when no $ValidateNick has been sent yet
  char *nick_hub; // in hub encoding
  char *nick;     // UTF-8
  // TRUE is the above nick has also been validated (and we're properly logged in)
  gboolean nick_valid;
  char *hubname;  // UTF-8, or NULL when unknown
  // user list, key = username (in hub encoding!), value = struct nmdc_user *
  GHashTable *users;
  int sharecount;
  guint64 sharesize;
  // what we and the hub support
  gboolean supports_nogetinfo;
  // MyINFO send timer (event loop source id)
  guint myinfo_timer;
  // last MyINFO string
  char *myinfo_last;
};

#endif




// nmdc utility functions

static char *charset_convert(struct nmdc_hub *hub, gboolean to_utf8, const char *str) {
  char *fmt = conf_hub_get(string, hub->tab->name, "encoding");
  char *res = str_convert(to_utf8||!fmt?"UTF-8":fmt, !to_utf8||!fmt?"UTF-8":fmt, str);
  g_free(fmt);
  return res;
}


static char *encode_and_escape(struct nmdc_hub *hub, const char *str) {
  char *enc = charset_convert(hub, FALSE, str);
  GString *dest = g_string_sized_new(strlen(enc));
  char *tmp = enc;
  while(*tmp) {
    if(*tmp == '$')
      g_string_append(dest, "&#36;");
    else if(*tmp == '|')
      g_string_append(dest, "&#124;");
    else if(*tmp == '&' && (strncmp(tmp, "&amp;", 5) == 0 || strncmp(tmp, "&#36;", 5) == 0 || strncmp(tmp, "&#124;", 6) == 0))
      g_string_append(dest, "&amp;");
    else
      g_string_append_c(dest, *tmp);
    tmp++;
  }
  g_free(enc);
  return g_string_free(dest, FALSE);
}


static char *unescape_and_decode(struct nmdc_hub *hub, const char *str) {
  GString *dest = g_string_sized_new(strlen(str));
  while(*str) {
    if(strncmp(str, "&#36;", 5) == 0) {
      g_string_append_c(dest, '$');
      str += 5;
    } else if(strncmp(str, "&#124;", 6) == 0) {
      g_string_append_c(dest, '|');
      str += 6;
    } else if(strncmp(str, "&amp;", 5) == 0) {
      g_string_append_c(dest, '&');
      str += 5;
    } else {
      g_string_append_c(dest, *str);
      str++;
    }
  }
  char *dec = charset_convert(hub, TRUE, dest->str);
  g_string_free(dest, TRUE);
  return dec;
}


// Info & algorithm @ http://www.teamfair.info/wiki/index.php?title=Lock_to_key
// This function modifies "lock" in-place for temporary data
static char *lock2key(char *lock) {
  char n;
  int i;
  int len = strlen(lock);
  if(len < 3)
    return g_strdup("STUPIDKEY!"); // let's not crash on invalid data
  int fst = lock[0] ^ lock[len-1] ^ lock[len-2] ^ 5;
  for(i=len-1; i; i--)
    lock[i] = lock[i] ^ lock[i-1];
  lock[0] = fst;
  for(i=0; i<len; i++)
    lock[i] = ((lock[i]<<4) & 0xF0) | ((lock[i]>>4) & 0x0F);
  GString *key = g_string_sized_new(len+100);
  for(i=0; i<len; i++) {
    n = lock[i];
    if(n == 0 || n == 5 || n == 36 || n == 96 || n == 124 || n == 126)
      g_string_append_printf(key, "/%%DCN%03d%%/", n);
    else
      g_string_append_c(key, n);
  }
  return g_string_free(key, FALSE);
}





// struct nmdc_user related functions

static struct nmdc_user *user_add(struct nmdc_hub *hub, const char *name) {
  struct nmdc_user *u = g_hash_table_lookup(hub->users, name);
  if(u)
    return u;
  u = g_slice_new0(struct nmdc_user);
  u->name_hub = g_strdup(name);
  u->name = charset_convert(hub, TRUE, name);
  g_hash_table_insert(hub->users, u->name_hub, u);
  ui_hub_joinquit(hub->tab, TRUE, u);
  return u;
}


static void user_free(gpointer dat) {
  struct nmdc_user *u = dat;
  g_free(u->name_hub);
  g_free(u->name);
  g_free(u->desc);
  g_free(u->tag);
  g_free(u->conn);
  g_free(u->mail);
  g_slice_free(struct nmdc_user, u);
}


// Get a user by a UTF-8 string. May fail if the UTF-8 -> hub encoding is not
// really one-to-one
struct nmdc_user *nmdc_user_get(struct nmdc_hub *hub, const char *name) {
  char *name_hub = charset_convert(hub, FALSE, name);
  struct nmdc_user *u = g_hash_table_lookup(hub->users, name_hub);
  g_free(name_hub);
  return u;
}


// Auto-complete suggestions for nmdc_user_get()
void nmdc_user_suggest(struct nmdc_hub *hub, char *str, char **sug) {
  GHashTableIter iter;
  struct nmdc_user *u;
  int i=0, len = strlen(str);
  g_hash_table_iter_init(&iter, hub->users);
  while(i<20 && g_hash_table_iter_next(&iter, NULL, (gpointer *)&u))
    if(g_ascii_strncasecmp(u->name, str, len) == 0 && strlen(u->name) != len)
      sug[i++] = g_strdup(u->name);
  qsort(sug, i, sizeof(char *), cmpstringp);
}


#define ASSIGN_OR_FREE(lval, rval) do { if(rval[0]) lval = rval; else { lval =  NULL; g_free(rval); } } while(0)

static void user_myinfo(struct nmdc_hub *hub, struct nmdc_user *u, const char *str) {
  static GRegex *nfo_reg = NULL;
  static GRegex *nfo_notag = NULL;
  if(!nfo_reg) //          desc   tag               conn   flag   email     share
    nfo_reg = g_regex_new("([^$]*)<([^>$]+)>\\$.\\$([^$]*)(\\C)\\$([^$]*)\\$([0-9]+)\\$", G_REGEX_OPTIMIZE|G_REGEX_RAW, 0, NULL);
  if(!nfo_notag) //          desc   tag      conn   flag    email     share
    nfo_notag = g_regex_new("([^$]*)()\\$.\\$([^$]*)(\\C)\\$([^$]*)\\$([0-9]+)\\$", G_REGEX_OPTIMIZE|G_REGEX_RAW, 0, NULL);

  GMatchInfo *nfo = NULL;
  gboolean match = g_regex_match(nfo_reg, str, 0, &nfo);
  if(!match) {
    g_match_info_free(nfo);
    match = g_regex_match(nfo_notag, str, 0, &nfo);
  }
  if(match) {
    g_free(u->desc);
    g_free(u->tag);
    g_free(u->conn);
    g_free(u->mail);
    char *desc = g_match_info_fetch(nfo, 1);
    char *tag = g_match_info_fetch(nfo, 2);
    char *conn = g_match_info_fetch(nfo, 3);
    //char *flag = g_match_info_fetch(nfo, 4); // currently ignored
    char *mail = g_match_info_fetch(nfo, 5);
    char *share = g_match_info_fetch(nfo, 6);
    u->desc = desc[0] ? unescape_and_decode(hub, desc) : NULL;
    g_free(desc);
    ASSIGN_OR_FREE(u->tag, tag);
    ASSIGN_OR_FREE(u->conn, conn);
    ASSIGN_OR_FREE(u->mail, mail);
    u->sharesize = g_ascii_strtoull(share, NULL, 10);
    g_free(share);
    u->hasinfo = TRUE;
    if(hub->tab->userlist_tab)
      ui_userlist_userupdate(hub->tab->userlist_tab, u);
  } else
    g_critical("Don't understand MyINFO string: %s", str);
  g_match_info_free(nfo);
}





// hub stuff


void nmdc_send_myinfo(struct nmdc_hub *hub) {
  if(!hub->nick_valid)
    return;
  char *tmp;
  tmp = conf_hub_get(string, hub->tab->name, "description"); char *desc = encode_and_escape(hub, tmp?tmp:""); g_free(tmp);
  tmp = conf_hub_get(string, hub->tab->name, "connection");  char *conn = encode_and_escape(hub, tmp?tmp:""); g_free(tmp);
  tmp = conf_hub_get(string, hub->tab->name, "email");       char *mail = encode_and_escape(hub, tmp?tmp:""); g_free(tmp);

  // TODO: differentiate between normal/passworded/OP
  int hubs = 0;
  GList *n;
  for(n=ui_tabs; n; n=n->next) {
    struct ui_tab *t = n->data;
    if(t->type == UIT_HUB && t->hub->nick_valid)
      hubs++;
  }

  // TODO: sharesize, mode, slots and "open new slot when upload is slower than.." stuff. When implemented.
  tmp = g_strdup_printf("$MyINFO $ALL %s %s<ncdc V:%s,M:P,H:%d/0/0,S:1>$ $%s\01$%s$0$",
    hub->nick_hub, desc, VERSION, hubs, conn, mail);
  g_free(desc);
  g_free(conn);
  g_free(mail);

  // send the MyINFO command only when it's different from the last one we sent
  if(!hub->myinfo_last || strcmp(tmp, hub->myinfo_last) != 0) {
    g_free(hub->myinfo_last);
    hub->myinfo_last = tmp;
    net_send(hub->net, tmp);
  } else
    g_free(tmp);
}


void nmdc_say(struct nmdc_hub *hub, const char *str) {
  if(!hub->nick_valid)
    return;
  char *msg = encode_and_escape(hub, str);
  net_sendf(hub->net, "<%s> %s", hub->nick_hub, msg);
  g_free(msg);
}


void nmdc_msg(struct nmdc_hub *hub, struct nmdc_user *user, const char *str) {
  char *msg = encode_and_escape(hub, str);
  net_sendf(hub->net, "$To: %s From: %s $<%s> %s", user->name_hub, hub->nick_hub, hub->nick_hub, msg);
  g_free(msg);
  // emulate protocol echo
  msg = g_strdup_printf("<%s> %s", hub->nick, str);
  ui_hub_msg(hub->tab, user, msg);
  g_free(msg);
}


static void handle_cmd(struct net *n, char *cmd) {
  struct nmdc_hub *hub = n->handle;
  GMatchInfo *nfo;

  // create regexes (declared statically, allocated/compiled on first call)
#define CMDREGEX(name, regex) \
  static GRegex * name = NULL;\
  if(!name) name = g_regex_new("\\$" regex, G_REGEX_OPTIMIZE|G_REGEX_ANCHORED|G_REGEX_DOTALL|G_REGEX_RAW, 0, NULL)

  CMDREGEX(lock, "Lock ([^ $]+) Pk=[^ $]+");
  CMDREGEX(supports, "Supports (.+)");
  CMDREGEX(hello, "Hello ([^ $]+)");
  CMDREGEX(quit, "Quit ([^ $]+)");
  CMDREGEX(nicklist, "NickList (.+)");
  CMDREGEX(oplist, "OpList (.+)");
  CMDREGEX(myinfo, "MyINFO \\$ALL ([^ $]+) (.+)");
  CMDREGEX(hubname, "HubName (.+)");
  CMDREGEX(to, "To: ([^ $]+) From: ([^ $]+) \\$(.+)");
  CMDREGEX(forcemove, "ForceMove (.+)");

  // $Lock
  if(g_regex_match(lock, cmd, 0, &nfo)) { // 1 = lock
    char *lock = g_match_info_fetch(nfo, 1);
    if(strncmp(lock, "EXTENDEDPROTOCOL", 16) == 0)
      net_send(hub->net, "$Supports NoGetINFO NoHello");
    char *key = lock2key(lock);
    net_sendf(hub->net, "$Key %s", key);
    hub->nick = conf_hub_get(string, hub->tab->name, "nick");
    hub->nick_hub = charset_convert(hub, FALSE, hub->nick);
    net_sendf(hub->net, "$ValidateNick %s", hub->nick_hub);
    g_free(key);
    g_free(lock);
  }
  g_match_info_free(nfo);

  // $Supports
  if(g_regex_match(supports, cmd, 0, &nfo)) { // 1 = list
    char *list = g_match_info_fetch(nfo, 1);
    if(strstr(list, "NoGetINFO"))
      hub->supports_nogetinfo = TRUE;
    // we also support NoHello, but no need to check for that
    g_free(list);
  }
  g_match_info_free(nfo);

  // $Hello
  if(g_regex_match(hello, cmd, 0, &nfo)) { // 1 = nick
    char *nick = g_match_info_fetch(nfo, 1);
    if(strcmp(nick, hub->nick_hub) == 0) {
      // some hubs send our $Hello twice (like verlihub)
      // just ignore the second one
      if(!hub->nick_valid) {
        ui_logwindow_add(hub->tab->log, "Nick validated.");
        hub->nick_valid = TRUE;
        net_send(hub->net, "$Version 1,0091");
        nmdc_send_myinfo(hub);
        net_send(hub->net, "$GetNickList");
      }
    } else {
      struct nmdc_user *u = user_add(hub, nick);
      if(!u->hasinfo && !hub->supports_nogetinfo)
        net_sendf(hub->net, "$GetINFO %s", nick);
    }
    g_free(nick);
  }
  g_match_info_free(nfo);

  // $Quit
  if(g_regex_match(quit, cmd, 0, &nfo)) { // 1 = nick
    char *nick = g_match_info_fetch(nfo, 1);
    struct nmdc_user *u = g_hash_table_lookup(hub->users, nick);
    if(u) {
      hub->sharecount--;
      hub->sharesize -= u->sharesize;
      ui_hub_joinquit(hub->tab, FALSE, u);
      g_hash_table_remove(hub->users, nick);
    }
    g_free(nick);
  }
  g_match_info_free(nfo);

  // $NickList
  if(g_regex_match(nicklist, cmd, 0, &nfo)) { // 1 = list of users
    // not really efficient, but does the trick
    char *str = g_match_info_fetch(nfo, 1);
    char **list = g_strsplit(str, "$$", 0);
    g_free(str);
    char **cur;
    for(cur=list; *cur&&**cur; cur++) {
      struct nmdc_user *u = user_add(hub, *cur);
      if(!u->hasinfo && !hub->supports_nogetinfo)
        net_sendf(hub->net, "$GetINFO %s %s", *cur, hub->nick_hub);
    }
    g_strfreev(list);
  }
  g_match_info_free(nfo);

  // $OpList
  if(g_regex_match(oplist, cmd, 0, &nfo)) { // 1 = list of ops
    // not really efficient, but does the trick
    char *str = g_match_info_fetch(nfo, 1);
    char **list = g_strsplit(str, "$$", 0);
    g_free(str);
    char **cur;
    for(cur=list; *cur&&**cur; cur++)
      user_add(hub, *cur)->isop = TRUE;
    g_strfreev(list);
  }
  g_match_info_free(nfo);

  // $MyINFO
  if(g_regex_match(myinfo, cmd, 0, &nfo)) { // 1 = nick, 2 = info string
    char *nick = g_match_info_fetch(nfo, 1);
    char *str = g_match_info_fetch(nfo, 2);
    struct nmdc_user *u = user_add(hub, nick);
    if(!u->hasinfo)
      hub->sharecount++;
    else
      hub->sharesize -= u->sharesize;
    user_myinfo(hub, u, str);
    if(!u->hasinfo)
      hub->sharecount--;
    else
      hub->sharesize += u->sharesize;
    g_free(str);
    g_free(nick);
  }
  g_match_info_free(nfo);

  // $HubName
  if(g_regex_match(hubname, cmd, 0, &nfo)) { // 1 = name
    char *name = g_match_info_fetch(nfo, 1);
    hub->hubname = unescape_and_decode(hub, name);
    g_free(name);
  }
  g_match_info_free(nfo);

  // $To
  if(g_regex_match(to, cmd, 0, &nfo)) { // 1 = to, 2 = from, 3 = msg
    char *to = g_match_info_fetch(nfo, 1);
    char *from = g_match_info_fetch(nfo, 2);
    char *msg = g_match_info_fetch(nfo, 3);
    struct nmdc_user *u = g_hash_table_lookup(hub->users, from);
    if(!u)
      g_warning("[hub: %s] Got a $To from `%s', who is not on this hub!", hub->tab->name, from);
    else {
      char *msge = unescape_and_decode(hub, msg);
      ui_hub_msg(hub->tab, u, msge);
      g_free(msge);
    }
    g_free(from);
    g_free(to);
    g_free(msg);
  }
  g_match_info_free(nfo);

  // $ForceMove
  if(g_regex_match(forcemove, cmd, 0, &nfo)) { // 1 = addr
    char *addr = g_match_info_fetch(nfo, 1);
    char *eaddr = unescape_and_decode(hub, addr);
    ui_logwindow_printf(hub->tab->log, "\nThe hub is requesting you to move to %s.\nType `/connect %s' to do so.\n", eaddr, eaddr);
    g_free(eaddr);
    g_free(addr);
  }
  g_match_info_free(nfo);

  // $GetPass
  if(strncmp(cmd, "$GetPass", 8) == 0) {
    ui_logwindow_add(hub->tab->log, "Hub requires a password. This version of ncdc does not support passworded login yet.");
    nmdc_disconnect(hub);
  }

  // $ValidateDenide
  if(strncmp(cmd, "$ValidateDenide", 15) == 0) {
    ui_logwindow_add(hub->tab->log, "Username invalid or already taken.");
    nmdc_disconnect(hub);
  }

  // $HubIsFull
  if(strncmp(cmd, "$HubIsFull", 10) == 0) {
    ui_logwindow_add(hub->tab->log, "Hub is full.");
    nmdc_disconnect(hub);
  }

  // global hub message
  if(cmd[0] != '$') {
    char *msg = unescape_and_decode(hub, cmd);
    ui_logwindow_add(hub->tab->log, msg);
    g_free(msg);
  }
}


static gboolean check_myinfo(gpointer data) {
  nmdc_send_myinfo(data);
  return TRUE;
}


static void handle_error(struct net *n, int action, GError *err) {
  struct nmdc_hub *hub = n->handle;

  if(err->code == G_IO_ERROR_CANCELLED)
    return;

  switch(action) {
  case NETERR_CONN:
    ui_logwindow_printf(hub->tab->log, "Could not connect to hub: %s", err->message);
    hub->state = HUBS_IDLE;
    break;
  case NETERR_RECV:
    ui_logwindow_printf(hub->tab->log, "Read error: %s", err->message);
    nmdc_disconnect(hub);
    break;
  case NETERR_SEND:
    ui_logwindow_printf(hub->tab->log, "Write error: %s", err->message);
    nmdc_disconnect(hub);
    break;
  }
}


// TODO: periodically send empty keep-alive commands
struct nmdc_hub *nmdc_create(struct ui_tab *tab) {
  struct nmdc_hub *hub = g_new0(struct nmdc_hub, 1);
  hub->net = net_create('|', hub, handle_cmd, handle_error);
  hub->tab = tab;
  hub->users = g_hash_table_new_full(g_str_hash, g_str_equal, NULL, user_free);
  hub->myinfo_timer = g_timeout_add_seconds(5*60, check_myinfo, hub);
  return hub;
}


static void handle_connect(struct net *n) {
  struct nmdc_hub *hub = n->handle;
  ui_logwindow_printf(hub->tab->log, "Connected to %s.", net_remoteaddr(n));
  hub->state = HUBS_CONNECTED;
}


void nmdc_connect(struct nmdc_hub *hub) {
  char *addr = conf_hub_get(string, hub->tab->name, "hubaddr");
  g_assert(addr);

  ui_logwindow_printf(hub->tab->log, "Connecting to %s...", addr);
  hub->state = HUBS_CONNECTING;
  net_connect(hub->net, addr, 411, handle_connect);
  g_free(addr);
}


void nmdc_disconnect(struct nmdc_hub *hub) {
  net_cancel(hub->net);
  net_disconnect(hub->net);
  g_hash_table_remove_all(hub->users);
  g_free(hub->nick);     hub->nick = NULL;
  g_free(hub->nick_hub); hub->nick_hub = NULL;
  g_free(hub->hubname);  hub->hubname = NULL;
  g_free(hub->myinfo_last); hub->myinfo_last = NULL;
  hub->nick_valid = hub->state = hub->sharecount = hub->sharesize = hub->supports_nogetinfo = 0;
  ui_logwindow_printf(hub->tab->log, "Disconnected.");
}


void nmdc_free(struct nmdc_hub *hub) {
  nmdc_disconnect(hub);
  net_free(hub->net);
  g_hash_table_unref(hub->users);
  g_source_remove(hub->myinfo_timer);
  g_free(hub);
}

