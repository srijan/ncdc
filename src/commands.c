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
#include <limits.h>
#include <stdlib.h>
#include <errno.h>

#define DOC_CMD
#define DOC_KEY
#define DOC_SET
#include "doc.h"

struct cmd {
  char name[16];
  void (*f)(char *);
  void (*suggest)(char *, char **);
  struct doc_cmd *doc;
};
// tentative definition of the cmd list
static struct cmd cmds[];


// get a command by name. performs a linear search. can be rewritten to use a
// binary search, but I doubt the performance difference really matters.
static struct cmd *getcmd(const char *name) {
  struct cmd *c;
  for(c=cmds; *c->name; c++)
    if(strcmp(c->name, name) == 0)
      break;
  return c->f ? c : NULL;
}


// Get documentation for a command. May be slow at first, but caches the doc
// structure later on.
static struct doc_cmd *getdoc(struct cmd *cmd) {
  struct doc_cmd empty = { "", NULL, "No documentation available." };
  if(cmd->doc)
    return cmd->doc;
  struct doc_cmd *i = (struct doc_cmd *)doc_cmds;
  for(; *i->name; i++)
    if(strcmp(i->name, cmd->name) == 0)
      break;
  cmd->doc = *i->name ? i : &empty;
  return cmd->doc;
}



static void c_quit(char *args) {
  ncdc_quit();
}


// handle /say and /me
static void sayme(char *args, gboolean me) {
  struct ui_tab *tab = ui_tab_cur->data;
  if(tab->type != UIT_HUB && tab->type != UIT_MSG)
    ui_m(NULL, 0, "This command can only be used on hub and message tabs.");
  else if(!tab->hub->nick_valid)
    ui_m(NULL, 0, "Not connected or logged in yet.");
  else if(!args[0])
    ui_m(NULL, 0, "Message empty.");
  else if(tab->type == UIT_HUB)
    hub_say(tab->hub, args, me);
  else {
    struct hub_user *u = g_hash_table_lookup(hub_uids, &tab->uid);
    if(!u)
      ui_m(NULL, 0, "User is not online.");
    else
      hub_msg(tab->hub, u, args, me, tab->msg_replyto);
  }
}


static void c_say(char *args) {
  sayme(args, FALSE);
}


static void c_me(char *args) {
  sayme(args, TRUE);
}


static void c_msg(char *args) {
  char *sep = strchr(args, ' ');
  if(sep) {
    *sep = 0;
    while(*(++sep) == ' ');
  }
  struct ui_tab *tab = ui_tab_cur->data;
  if(tab->type != UIT_HUB && tab->type != UIT_MSG)
    ui_m(NULL, 0, "This command can only be used on hub and message tabs.");
  else if(!tab->hub->nick_valid)
    ui_m(NULL, 0, "Not connected or logged in yet.");
  else if(!args[0])
    ui_m(NULL, 0, "No user specified. See `/help msg' for more information.");
  else {
    struct hub_user *u = hub_user_get(tab->hub, args);
    if(!u)
      ui_m(NULL, 0, "No user found with that name. Note that usernames are case-sensitive.");
    else {
      // get or open tab and make sure it's selected
      struct ui_tab *t = g_hash_table_lookup(ui_msg_tabs, &u->uid);
      if(!t)
        ui_tab_open(ui_msg_create(tab->hub, u), TRUE, tab);
      else
        ui_tab_cur = g_list_find(ui_tabs, t);
      // if we need to send something, do so
      if(sep && *sep)
        hub_msg(tab->hub, u, sep, FALSE, 0);
    }
  }
}


static void c_help(char *args) {
  char *sec = strchr(args, ' ');
  if(sec)
    *(sec++) = 0;

  // list available commands
  if(!args[0]) {
    ui_m(NULL, 0, "\nAvailable commands:");
    struct cmd *c = cmds;
    for(; c->f; c++)
      ui_mf(NULL, 0, " /%s - %s", c->name, getdoc(c)->sum);
    ui_m(NULL, 0, "\nFor help on key bindings, use `/help keys'.\n");

  // list information on a setting
  } else if((strcmp(args, "set") == 0 || strcmp(args, "hset") == 0) && sec) {
    sec = strncmp(sec, "color_", 6) == 0 ? "color_*" : sec;
    struct doc_set *s = (struct doc_set *)doc_sets;
    for(; s->name; s++)
      if(strcmp(s->name, sec) == 0)
        break;
    if(!s->name)
      ui_mf(NULL, 0, "\nUnknown setting '%s'.", sec);
    else
      ui_mf(NULL, 0, "\nSetting: %s.%s %s\n\n%s\n", s->hub ? "#hub" : "global", s->name, s->type, s->desc);

  // list available key sections
  } else if(strcmp(args, "keys") == 0 && !sec) {
    ui_m(NULL, 0, "\nAvailable sections:");
    const struct doc_key *k = doc_keys;
    for(; k->sect; k++)
      ui_mf(NULL, 0, " %s - %s", k->sect, k->title);
    ui_m(NULL, 0, "\nUse `/help keys <name>' to get help on the key bindings for the selected section.\n");

  // get information on a particular key section
  } else if(strcmp(args, "keys") == 0 && sec) {
    const struct doc_key *k = doc_keys;
    for(; k->sect; k++)
      if(strcmp(k->sect, sec) == 0)
        break;
    if(!k->sect)
      ui_mf(NULL, 0, "\nUnknown keys section '%s'.", sec);
    else
      ui_mf(NULL, 0, "\nKey bindings for: %s - %s.\n\n%s\n", k->sect, k->title, k->desc);

  // get information on a particular command
  } else if(!sec) {
    if(*args == '/')
      args++;
    struct cmd *c = getcmd(args);
    if(!c)
      ui_mf(NULL, 0, "\nUnknown command '%s'.", args);
    else {
      struct doc_cmd *d = getdoc(c);
      ui_mf(NULL, 0, "\nUsage: /%s %s\n  %s\n", c->name, d->args ? d->args : "", d->sum);
      if(d->desc)
        ui_mf(NULL, 0, "%s\n", d->desc);
    }

  } else
    ui_mf(NULL, 0, "\nUnknown help section `%s'.", args);
}


static void c_help_sug(char *args, char **sug) {
  // help h?set ..
  if(strncmp(args, "set ", 4) == 0 || strncmp(args, "hset ", 5) == 0) {
    char *sec = args + (*args == 'h' ? 5 : 4);
    int i, n=0, len = strlen(sec);
    for(i=0; i<VAR_END && n<20; i++)
      if((vars[i].global || vars[i].hub) && strncmp(vars[i].name, sec, len) == 0 && strlen(vars[i].name) != len)
        sug[n++] = g_strdup(vars[i].name);
    strv_prefix(sug, "set ", NULL);
    return;
  }
  // help keys ..
  if(strncmp(args, "keys ", 5) == 0) {
    int i = 0, len = strlen(args)-5;
    const struct doc_key *k;
    for(k=doc_keys; i<20 && k->sect; k++)
      if(strncmp(k->sect, args+5, len) == 0 && strlen(k->sect) != len)
        sug[i++] = g_strdup(k->sect);
    strv_prefix(sug, "keys ", NULL);
    return;
  }
  // help command
  int i = 0, len = strlen(args);
  gboolean ckeys = FALSE;
  struct cmd *c;
  for(c=cmds; i<20 && c->f; c++) {
    // Somehow merge "keys" into the list
    if(!ckeys && strcmp(c->name, "keys") > 0) {
      if(strncmp("keys", args, len) == 0 && len != 4)
        sug[i++] = g_strdup("keys");
      ckeys = TRUE;
    }
    if(i < 20 && strncmp(c->name, args, len) == 0 && strlen(c->name) != len)
      sug[i++] = g_strdup(c->name);
  }
}


static gboolean c_connect_set_hubaddr(char *addr) {
  // Validate and parse
  GRegex *reg = g_regex_new(
    //   1 - proto                2 - host             3 - port                       4 - kp
    "^(?:(dchub|nmdcs?|adcs?)://)?([^ :/<>\\(\\)]+)(?::([0-9]+))?(?:/|/\\?kp=SHA256\\/([a-zA-Z2-7]{52}))?$",
    0, 0, NULL);
  g_assert(reg);
  GMatchInfo *nfo;
  if(!g_regex_match(reg, addr, 0, &nfo)) {
    ui_m(NULL, 0, "Invalid URL format."); // not very specific
    g_regex_unref(reg);
    return FALSE;
  }
  g_regex_unref(reg);
  char *proto = g_match_info_fetch(nfo, 1);
  char *kp = g_match_info_fetch(nfo, 4);

  if(kp && *kp && strcmp(proto, "adcs") != 0 && strcmp(proto, "nmdcs") != 0) {
    ui_m(NULL, 0, "Keyprint is only valid for adcs:// or nmdcs:// URLs.");
    g_match_info_free(nfo);
    g_free(proto);
    g_free(kp);
    return FALSE;
  }

  char *host = g_match_info_fetch(nfo, 2);
  char *port = g_match_info_fetch(nfo, 3);
  g_match_info_free(nfo);

  struct ui_tab *tab = ui_tab_cur->data;
  char *old = g_strdup(var_get(tab->hub->id, VAR_hubaddr));

  // Reconstruct (without the kp) and save
  GString *a = g_string_new("");
  g_string_printf(a, "%s://%s:%s/", !proto || !*proto ? "dchub" : proto, host, !port || !*port ? "411" : port);
  var_set(tab->hub->id, VAR_hubaddr, a->str, NULL);

  // Save kp if specified, or throw it away if the URL changed
  if(kp && *kp)
    var_set(tab->hub->id, VAR_hubkp, kp, NULL);
  else if(old && strcmp(old, a->str) != 0)
    var_set(tab->hub->id, VAR_hubkp, NULL, NULL);

  g_string_free(a, TRUE);
  g_free(old);
  g_free(proto);
  g_free(kp);
  g_free(host);
  g_free(port);
  return TRUE;
}


static void c_connect(char *args) {
  struct ui_tab *tab = ui_tab_cur->data;
  if(tab->type != UIT_HUB)
    ui_m(NULL, 0, "This command can only be used on hub tabs.");
  else if(tab->hub->net->connecting || tab->hub->net->conn)
    ui_m(NULL, 0, "Already connected (or connecting). You may want to /disconnect first.");
  else {
    if(args[0] && !c_connect_set_hubaddr(args))
      ;
    else if(!var_get(tab->hub->id, VAR_hubaddr))
      ui_m(NULL, 0, "No hub address configured. Use '/connect <address>' to do so.");
    else
      hub_connect(tab->hub);
  }
}


// only autocompletes "dchub://" or the hubaddr, when set
static void c_connect_sug(char *args, char **sug) {
  struct ui_tab *t = ui_tab_cur->data;
  if(t->type != UIT_HUB)
    return;
  int i = 0, len = strlen(args);
  char *addr = var_get(t->hub->id, VAR_hubaddr);
  if(addr && strncmp(addr, args, len) == 0)
    sug[i++] = g_strdup(addr);
  else if(addr) {
    char *naddr = g_strconcat("dchub://", addr, "/", NULL);
    if(strncmp(naddr, args, len) == 0)
      sug[i++] = naddr;
    else
      g_free(naddr);
  }
  if(strncmp("dchub://", args, len) == 0)
    sug[i++] = g_strdup("dchub://");
}


static void c_disconnect(char *args) {
  struct ui_tab *tab = ui_tab_cur->data;
  if(args[0])
    ui_m(NULL, 0, "This command does not accept any arguments.");
  else if(tab->type == UIT_HUB) {
    if(!tab->hub->net->conn && !tab->hub->reconnect_timer && !tab->hub->net->connecting)
      ui_m(NULL, 0, "Not connected.");
    else
      hub_disconnect(tab->hub, FALSE);
  } else if(tab->type == UIT_MAIN) {
    ui_m(NULL, 0, "Disconnecting all hubs.");
    GList *n = ui_tabs;
    for(; n; n=n->next) {
      tab = n->data;
      if(tab->type == UIT_HUB && (tab->hub->net->conn || tab->hub->net->connecting || tab->hub->reconnect_timer))
        hub_disconnect(tab->hub, FALSE);
    }
  } else
    ui_m(NULL, 0, "This command can only be used on the main tab or on hub tabs.");
}


static void c_reconnect(char *args) {
  struct ui_tab *tab = ui_tab_cur->data;
  if(args[0])
    ui_m(NULL, 0, "This command does not accept any arguments.");
  else if(tab->type == UIT_HUB) {
    if(tab->hub->net->conn || tab->hub->net->connecting || tab->hub->reconnect_timer)
      hub_disconnect(tab->hub, FALSE);
    c_connect(""); // also checks for the existence of "hubaddr"
  } else if(tab->type == UIT_MAIN) {
    ui_m(NULL, 0, "Reconnecting all hubs.");
    GList *n = ui_tabs;
    for(; n; n=n->next) {
      tab = n->data;
      if(tab->type != UIT_HUB)
        continue;
      if(tab->hub->net->conn || tab->hub->net->connecting || tab->hub->reconnect_timer)
        hub_disconnect(tab->hub, FALSE);
      ui_tab_cur = n;
      c_connect("");
    }
    ui_tab_cur = g_list_find(ui_tabs, ui_main);
  } else
    ui_m(NULL, 0, "This command can only be used on the main tab or on hub tabs.");
}


static void c_accept(char *args) {
  struct ui_tab *tab = ui_tab_cur->data;
  if(args[0])
    ui_m(NULL, 0, "This command does not accept any arguments.");
  else if(tab->type != UIT_HUB)
    ui_m(NULL, 0, "This command can only be used on hub tabs.");
#if TLS_SUPPORT
  else if(!tab->hub->kp)
    ui_m(NULL, 0, "Nothing to accept.");
  else {
    char enc[53] = {};
    base32_encode_dat(tab->hub->kp, enc, 32);
    var_set(tab->hub->id, VAR_hubkp, enc, NULL);
    g_slice_free1(32, tab->hub->kp);
    tab->hub->kp = NULL;
    hub_connect(tab->hub);
  }
#else
  else
    ui_m(NULL, 0, "No TLS support.");
#endif
}


static void c_open(char *args) {
  struct ui_tab *tab = ui_tab_cur->data;
  gboolean conn = TRUE;
  if(strncmp(args, "-n ", 3) == 0) {
    conn = FALSE;
    args += 3;
    g_strstrip(args);
  }
  char *name = args, *addr = strchr(args, ' ');
  if(name[0] == '#')
    name++;
  if(addr)
    *(addr++) = 0;
  if(!name[0]) {
    ui_m(NULL, 0, "No hub name given.");
    return;
  }
  if(!is_valid_hubname(name))
    ui_m(NULL, 0, "Sorry, hub name may only consist of alphanumeric characters, and must not exceed 25 characters.");
  else {
    // Look for existing tab
    GList *n;
    for(n=ui_tabs; n; n=n->next) {
      char *tmp = ((struct ui_tab *)n->data)->name;
      if(tmp[0] == '#' && strcmp(tmp+1, name) == 0)
        break;
    }
    // Open or select tab
    if(!n) {
      tab = ui_hub_create(name, addr ? FALSE : conn);
      ui_tab_open(tab, TRUE, NULL);
      listen_refresh();
    } else if(n != ui_tab_cur) {
      ui_tab_cur = n;
      tab = n->data;
    } else {
      ui_m(NULL, 0, addr ? "Tab already selected, saving new address instead." : "Tab already selected.");
      tab = n->data;
    }
    // Save address and (re)connect when necessary
    if(addr && c_connect_set_hubaddr(addr) && conn)
      c_reconnect("");
  }
}


static void c_open_sug(char *args, char **sug) {
  int len = strlen(args);
  int i = 0;
  char **hub, **hubs = db_vars_hubs();
  for(hub=hubs; i<20 && *hub; hub++)
    if((strncmp(args, *hub, len) == 0 || strncmp(args, *hub+1, len) == 0) && strlen(*hub) != len)
      sug[i++] = g_strdup(*hub);
  g_strfreev(hubs);
}


static void c_close(char *args) {
  if(args[0]) {
    ui_m(NULL, 0, "This command does not accept any arguments.");
    return;
  }
  struct ui_tab *tab = ui_tab_cur->data;
  switch(tab->type) {
  case UIT_MAIN:     ui_m(NULL, 0, "Main tab cannot be closed."); break;
  case UIT_HUB:      ui_hub_close(tab); listen_refresh(); break;
  case UIT_USERLIST: ui_userlist_close(tab); break;
  case UIT_MSG:      ui_msg_close(tab);      break;
  case UIT_CONN:     ui_conn_close();        break;
  case UIT_FL:       ui_fl_close(tab);       break;
  case UIT_DL:       ui_dl_close();          break;
  case UIT_SEARCH:   ui_search_close(tab);   break;
  default:
    g_return_if_reached();
  }
}


static void c_clear(char *args) {
  struct ui_tab *tab = ui_tab_cur->data;
  if(args[0])
    ui_m(NULL, 0, "This command does not accept any arguments.");
  else if(tab->log)
    ui_logwindow_clear(tab->log);
}


static void c_userlist(char *args) {
  struct ui_tab *tab = ui_tab_cur->data;
  if(args[0])
    ui_m(NULL, 0, "This command does not accept any arguments.");
  else if(tab->type != UIT_HUB)
    ui_m(NULL, 0, "This command can only be used on hub tabs.");
  else
    ui_hub_userlist_open(tab);
}


static void listshares() {
  struct db_share_item *l = db_share_list();
  if(!l->name)
    ui_m(NULL, 0, "Nothing shared.");
  else {
    ui_m(NULL, 0, "");
    for(; l->name; l++) {
      struct fl_list *fl = fl_local_list ? fl_list_file(fl_local_list, l->name) : NULL;
      ui_mf(NULL, 0, " /%s -> %s (%s)", l->name, l->path, fl ? str_formatsize(fl->size) : "-");
    }
    ui_m(NULL, 0, "");
  }
}


static void c_share(char *args) {
  if(!args[0]) {
    listshares();
    return;
  }

  char *first, *second;
  str_arg2_split(args, &first, &second);
  if(!first || !first[0] || !second || !second[0])
    ui_m(NULL, 0, "Error parsing arguments. See \"/help share\" for details.");
  else if(db_share_path(first))
    ui_m(NULL, 0, "You have already shared a directory with that name.");
  else {
    char *path = path_expand(second);
    char *tmp;
    for(tmp = first; *tmp; tmp++)
      if(*tmp == '/' || *tmp == '\\')
        break;
    if(*tmp)
      ui_m(NULL, 0, "Invalid character in share name.");
    else if(!path)
      ui_mf(NULL, 0, "Error obtaining absolute path: %s", g_strerror(errno));
    else if(!g_file_test(path, G_FILE_TEST_IS_DIR))
      ui_m(NULL, 0, "Not a directory.");
    else {
      // Check whether it (or a subdirectory) is already shared
      struct db_share_item *l = db_share_list();
      for(; l->name; l++)
        if(strncmp(l->path, path, MIN(strlen(l->path), strlen(path))) == 0)
          break;
      if(l->name)
        ui_mf(NULL, 0, "Directory already (partly) shared in /%s", l->name);
      else {
        db_share_add(first, path);
        fl_share(first);
        ui_mf(NULL, 0, "Added to share: /%s -> %s", first, path);
      }
    }
    if(path)
      free(path);
  }
  g_free(first);
}


static void c_share_sug(char *args, char **sug) {
  char *first, *second;
  str_arg2_split(args, &first, &second);
  g_free(first);
  if(!first || !second)
    return;
  // we want the escaped first part
  first = g_strndup(args, second-args);
  path_suggest(second, sug);
  strv_prefix(sug, first, NULL);
  g_free(first);
}


static void c_unshare(char *args) {
  if(!args[0]) {
    listshares();
    return;
  // otherwise we may crash
  } else if(fl_refresh_queue && fl_refresh_queue->head) {
    ui_m(NULL, 0, "Sorry, can't remove directories from the share while refreshing.");
    return;
  }

  while(args[0] == '/')
    args++;

  // Remove everything
  if(!args[0]) {
    db_share_rm(NULL);
    fl_unshare(NULL);
    ui_m(NULL, 0, "Removed all directories from share.");

  // Remove a single dir
  } else {
    const char *path = db_share_path(args);
    if(!path)
      ui_m(NULL, 0, "No shared directory with that name.");
    else {
      ui_mf(NULL, 0, "Directory /%s (%s) removed from share.", args, path);
      db_share_rm(args);
      fl_unshare(args);
    }
  }
}


static void c_unshare_sug(char *args, char **sug) {
  int len = strlen(args), i = 0;
  if(args[0] == '/')
    args++;
  struct db_share_item *l = db_share_list();
  for(; l->name; l++)
    if(strncmp(args, l->name, len) == 0 && strlen(l->name) != len)
      sug[i++] = g_strdup(l->name);
}


static void c_refresh(char *args) {
  struct fl_list *n = fl_local_from_path(args);
  if(!n)
    ui_mf(NULL, 0, "Directory `%s' not found.", args);
  else
    fl_refresh(n);
}


static void nick_sug(char *args, char **sug, gboolean append) {
  struct ui_tab *t = ui_tab_cur->data;
  if(!t->hub)
    return;
  // get starting point of the nick
  char *nick = args+strlen(args);
  while(nick > args && *(nick-1) != ' ' && *(nick-1) != ',' && *(nick-1) != ':')
    nick--;
  hub_user_suggest(t->hub, nick, sug);
  // optionally append ": " after the nick
  if(append && nick == args) {
    char **n;
    for(n=sug; *n; n++) {
      char *tmp = *n;
      *n = g_strdup_printf("%s: ", tmp);
      g_free(tmp);
    }
  }
  // prefix
  *nick = 0;
  if(*args)
    strv_prefix(sug, args, NULL);
}


// also used for c_me
static void c_say_sug(char *args, char **sug) {
  nick_sug(args, sug, TRUE);
}


// also used for c_whois, c_grant, c_kick and c_browse
static void c_msg_sug(char *args, char **sug) {
  nick_sug(args, sug, FALSE);
}


static void c_version(char *args) {
  if(args[0])
    ui_m(NULL, 0, "This command does not accept any arguments.");
  else
    ui_mf(NULL, 0, "\n%s\n", ncdc_version());
}


static void c_connections(char *args) {
  if(args[0])
    ui_m(NULL, 0, "This command does not accept any arguments.");
  else {
    if(ui_conn)
      ui_tab_cur = g_list_find(ui_tabs, ui_conn);
    else
      ui_tab_open(ui_conn_create(), TRUE, NULL);
  }
}


static void c_queue(char *args) {
  if(args[0])
    ui_m(NULL, 0, "This command does not accept any arguments.");
  else {
    if(ui_dl)
      ui_tab_cur = g_list_find(ui_tabs, ui_dl);
    else
      ui_tab_open(ui_dl_create(), TRUE, NULL);
  }
}


static void c_gc(char *args) {
  if(args[0])
    ui_m(NULL, 0, "This command does not accept any arguments.");
  else {
    ui_m(NULL, UIM_NOLOG, "Collecting garbage...");
    ui_draw();
    if(!fl_gc())
      ui_m(NULL, 0, "Not checking for unused hash data: File list refresh in progress or not performed yet.");
    db_fl_purgedata();
    dl_fl_clean(NULL);
    dl_inc_clean();
    db_vacuum();
    ui_m(NULL, UIM_NOLOG, NULL);
    ui_m(NULL, 0, "Garbage-collection done.");
  }
}


static void c_whois(char *args) {
  struct ui_tab *tab = ui_tab_cur->data;
  char *u = NULL;
  guint64 uid = 0;
  gboolean utf8 = TRUE;
  if(tab->type != UIT_HUB && tab->type != UIT_MSG)
    ui_m(NULL, 0, "This command can only be used on hub and message tabs.");
  else if(!args[0] && tab->type != UIT_MSG)
    ui_m(NULL, 0, "No user specified. See `/help whois' for more information.");
  else if(tab->type == UIT_MSG) {
    if(args[0])
      u = args;
    else
      uid = tab->uid;
    tab = tab->hub->tab;
  } else
    u = args;
  if((u || uid) && !ui_hub_finduser(tab, uid, u, utf8))
    ui_m(NULL, 0, "No user found with that name.");
}


static void listgrants() {
  guint64 *list = cc_grant_list();
  if(!*list)
    ui_m(NULL, 0, "No slots granted to anyone.");
  else {
    ui_m(NULL, 0, "\nGranted slots to:");
    guint64 *n = list;
    for(; *n; n++) {
      struct hub_user *u = g_hash_table_lookup(hub_uids, n);
      if(u)
        ui_mf(NULL, 0, "  %"G_GINT64_MODIFIER"x (%s on %s)", *n, u->name, u->hub->tab->name);
      else
        ui_mf(NULL, 0, "  %"G_GINT64_MODIFIER"x (user offline)", *n);
    }
    ui_m(NULL, 0, "");
  }
  g_free(list);
}


static void c_grant(char *args) {
  struct ui_tab *tab = ui_tab_cur->data;
  struct hub_user *u = NULL;
  if((!*args && tab->type != UIT_MSG) || strcmp(args, "-list") == 0)
    listgrants();
  else if(tab->type != UIT_HUB && tab->type != UIT_MSG)
    ui_m(NULL, 0, "This command can only be used on hub and message tabs.");
  else if(args[0]) {
    u = hub_user_get(tab->hub, args);
    if(!u)
      ui_m(NULL, 0, "No user found with that name.");
  } else {
    u = g_hash_table_lookup(hub_uids, &tab->uid);
    if(!u)
      ui_m(NULL, 0, "User not online.");
  }

  if(u) {
    cc_grant(u);
    ui_m(NULL, 0, "Slot granted.");
  }
}


static void c_ungrant(char *args) {
  struct ui_tab *tab = ui_tab_cur->data;
  guint64 uid = 0;
  if(!*args && tab->type != UIT_MSG) {
    listgrants();
    return;
  } else if(!*args && tab->type == UIT_MSG)
    uid = tab->uid;
  else {
    guint64 *key;
    char id[17] = {};
    GHashTableIter iter;
    g_hash_table_iter_init(&iter, cc_granted);
    while(g_hash_table_iter_next(&iter, (gpointer *)&key, NULL)) {
      struct hub_user *u = g_hash_table_lookup(hub_uids, key);
      g_snprintf(id, 17, "%"G_GINT64_MODIFIER"x", *key);
      if((u && strcasecmp(u->name, args) == 0) || g_ascii_strncasecmp(id, args, strlen(args)) == 0) {
        if(uid) {
          ui_mf(NULL, 0, "Ambiguous user `%s'.", args);
          return;
        }
        uid = *key;
      }
    }
  }

  if(uid && g_hash_table_remove(cc_granted, &uid))
    ui_mf(NULL, 0, "Slot for `%"G_GINT64_MODIFIER"x' revoked.", uid);
  else
    ui_mf(NULL, 0, "No slot granted to `%s'.", !*args && tab->type == UIT_MSG ? tab->name+1 : args);
}


static void c_ungrant_sug(char *args, char **sug) {
  int len = strlen(args);
  char id[17] = {};
  guint64 *list = cc_grant_list();
  guint64 *i = list;
  int n = 0;
  for(; n<20 && *i; i++) {
    struct hub_user *u = g_hash_table_lookup(hub_uids, i);
    g_snprintf(id, 17, "%"G_GINT64_MODIFIER"x", *i);
    if((u && strncasecmp(u->name, args, len) == 0))
      sug[n++] = g_strdup(u->name);
    if(n < 20 && g_ascii_strncasecmp(id, args, len) == 0)
      sug[n++] = g_strdup(id);
  }
  g_free(list);
}


static void c_password(char *args) {
  struct ui_tab *tab = ui_tab_cur->data;
  if(tab->type != UIT_HUB)
    ui_m(NULL, 0, "This command can only be used on hub tabs.");
  else if(!tab->hub->net->conn)
    ui_m(NULL, 0, "Not connected to a hub. Did you want to use '/hset password' instead?");
  else if(tab->hub->nick_valid)
    ui_m(NULL, 0, "Already logged in. Did you want to use '/hset password' instead?");
  else
    hub_password(tab->hub, args);
}


static void c_kick(char *args) {
  struct ui_tab *tab = ui_tab_cur->data;
  if(tab->type != UIT_HUB)
    ui_m(NULL, 0, "This command can only be used on hub tabs.");
  else if(!tab->hub->nick_valid)
    ui_m(NULL, 0, "Not connected or logged in yet.");
  else if(!args[0])
    ui_m(NULL, 0, "No user specified.");
  else if(tab->hub->adc)
    ui_m(NULL, 0, "This command only works on NMDC hubs.");
  else {
    struct hub_user *u = hub_user_get(tab->hub, args);
    if(!u)
      ui_m(NULL, 0, "No user found with that name.");
    else
      hub_kick(tab->hub, u);
  }
}


static void c_nick(char *args) {
  struct ui_tab *tab = ui_tab_cur->data;
  guint64 hub = tab->type == UIT_HUB || tab->type == UIT_MSG ? tab->hub->id : 0;
  int v = vars_byname("nick");
  g_return_if_fail(v >= 0);
  GError *err = NULL;
  char *r = vars[v].parse(args, &err);
  if(!r || !var_set(hub, v, r, &err)) {
    ui_mf(NULL, 0, "Error changing nick: %s", err->message);
    g_free(r);
    return;
  }
  g_free(r);
  ui_mf(NULL, 0, "Nick changed.");
}

static void c_browse(char *args) {
  struct ui_tab *tab = ui_tab_cur->data;
  struct hub_user *u = NULL;
  gboolean force = FALSE;

  if(!args[0] && !fl_local_list) {
    ui_m(NULL, 0, "Nothing shared.");
    return;
  } else if(args[0]) {
    if(tab->type != UIT_HUB && tab->type != UIT_MSG) {
      ui_m(NULL, 0, "This command can only be used on hub and message tabs.");
      return;
    }
    if(strncmp(args, "-f ", 3) == 0) {
      force = TRUE;
      args += 3;
      g_strstrip(args);
    }
    u = hub_user_get(tab->hub, args);
    if(!u) {
      ui_m(NULL, 0, "No user found with that name.");
      return;
    }
  }

  ui_fl_queue(u ? u->uid : 0, force, NULL, !args[0] ? NULL : tab, TRUE, FALSE);
}


static void c_search(char *args) {
  // Split arguments
  char **argv;
  int argc;
  GError *err = NULL;
  if(!g_shell_parse_argv(args, &argc, &argv, &err)) {
    ui_mf(NULL, 0, "Error parsing arguments: %s", err->message);
    g_error_free(err);
    return;
  }

  // Create basic query
  gboolean allhubs = FALSE;
  gboolean stoparg = FALSE;
  int qlen = 0;
  struct search_q *q = g_slice_new0(struct search_q);
  q->query = g_new0(char *, argc+1);
  q->type = 1;

  // Loop through arguments. (Later arguments overwrite earlier ones)
  int i;
  for(i=0; i<argc; i++) {
    // query
    if(stoparg || argv[i][0] != '-')
      q->query[qlen++] = g_strdup(argv[i]);
    // --
    else if(strcmp(argv[i], "--") == 0)
      stoparg = TRUE;
    // -hub, -all
    else if(strcmp(argv[i], "-hub") == 0)
      allhubs = FALSE;
    else if(strcmp(argv[i], "-all") == 0)
      allhubs = TRUE;
    // -le, -ge
    else if(strcmp(argv[i], "-le") == 0 || strcmp(argv[i], "-ge") == 0) {
      q->ge = strcmp(argv[i], "-ge") == 0;
      if(++i >= argc) {
        ui_mf(NULL, 0, "Option `%s' expects an argument.", argv[i-1]);
        goto c_search_clean;
      }
      q->size = str_parsesize(argv[i]);
      if(q->size == G_MAXUINT64) {
        ui_mf(NULL, 0, "Invalid size argument for option `%s'.", argv[i-1]);
        goto c_search_clean;
      }
    // -t
    } else if(strcmp(argv[i], "-t") == 0) {
      if(++i >= argc) {
        ui_mf(NULL, 0, "Option `%s' expects an argument.", argv[i-1]);
        goto c_search_clean;
      }
      if('1' <= argv[i][0] && argv[i][0] <= '8' && !argv[i][1])
        q->type = argv[i][0]-'0';
      else if(strcmp(argv[i], "any") == 0)      q->type = 1;
      else if(strcmp(argv[i], "audio") == 0)    q->type = 2;
      else if(strcmp(argv[i], "archive") == 0)  q->type = 3;
      else if(strcmp(argv[i], "doc") == 0)      q->type = 4;
      else if(strcmp(argv[i], "exe") == 0)      q->type = 5;
      else if(strcmp(argv[i], "img") == 0)      q->type = 6;
      else if(strcmp(argv[i], "video") == 0)    q->type = 7;
      else if(strcmp(argv[i], "dir") == 0)      q->type = 8;
      else {
        ui_mf(NULL, 0, "Unknown argument for option `%s'.", argv[i-1]);
        goto c_search_clean;
      }
    // -tth
    } else if(strcmp(argv[i], "-tth") == 0) {
      if(++i >= argc) {
        ui_mf(NULL, 0, "Option `%s' expects an argument.", argv[i-1]);
        goto c_search_clean;
      }
      if(!istth(argv[i])) {
        ui_m(NULL, 0, "Invalid TTH root.");
        goto c_search_clean;
      }
      q->type = 9;
      base32_decode(argv[i], q->tth);
    // oops
    } else {
      ui_mf(NULL, 0, "Unknown option: %s", argv[i]);
      goto c_search_clean;
    }
  }

  // validate & send
  struct ui_tab *tab = ui_tab_cur->data;
  if(!allhubs && tab->type != UIT_HUB && tab->type != UIT_MSG) {
    ui_m(NULL, 0, "This command can only be used on hub tabs. Use the `-all' option to search on all connected hubs.");
    goto c_search_clean;
  }

  search_do(q, allhubs ? NULL : tab->hub, allhubs ? NULL : tab);
  q = NULL; // make sure to not free it

c_search_clean:
  g_strfreev(argv);
  search_q_free(q);
}


#define print_var(hub, hubname, var) do {\
    char *raw = var_get(hub, var);\
    if(!raw)\
      ui_mf(NULL, 0, "%s.%s is not set.", hubname, vars[var].name);\
    else {\
      char *fmt = vars[var].format(raw);\
      ui_mf(NULL, 0, "%s.%s = %s", hubname, vars[var].name, fmt);\
      g_free(fmt);\
    }\
  } while(0)


#define check_var(hub, var, key, unset) do {\
    if(var < 0 || (!vars[var].global && !vars[var].hub)) {\
      ui_mf(NULL, 0, "No setting with the name '%s'.", key);\
      return;\
    }\
    if(hub ? !vars[var].hub : !vars[var].global) {\
      ui_mf(NULL, 0,\
        hub ? "`%s' is a global setting, did you mean to use /%s instead?"\
            : "'%s' is a hub setting, did you mean to use /%s instead?",\
        vars[var].name, !hub ? (unset ? "hunset" : "hset") : unset ? "unset" : "set");\
      return;\
    }\
  } while(0)


#define hubandhubname(h) \
  guint64 hub = 0;\
  char *hubname = "global";\
  if(h) {\
    struct ui_tab *tab = ui_tab_cur->data;\
    if(tab->type != UIT_HUB && tab->type != UIT_MSG) {\
      ui_m(NULL, 0, "This command can only be used on hub tabs.");\
      return;\
    }\
    hub = tab->hub->id;\
    hubname = tab->name;\
  }


static gboolean listsettings(guint64 hub, const char *hubname, const char *key) {
  int i, n = 0;
  char *pat = !key || !*key ? NULL : key[strlen(key)-1] == '*' || key[strlen(key)-1] == '?' ? g_strdup(key) : g_strconcat(key, "*", NULL);
  GPatternSpec *p = pat ? g_pattern_spec_new(pat) : NULL;
  g_free(pat);
  for(i=0; i<VAR_END; i++) {
    if((hub ? vars[i].hub : vars[i].global) && (!pat || g_pattern_match_string(p, vars[i].name))) {
      if(n++ == 0)
        ui_m(NULL, 0, "");
      print_var(hub, hubname, i);
    }
  }
  if(pat)
    g_pattern_spec_free(p);
  if(n)
    ui_m(NULL, 0, "");
  return n == 0 ? FALSE : TRUE;
}


// Implements /set and /hset
static void sethset(gboolean h, char *args) {
  hubandhubname(h);
  char *key = args;
  char *val; // NULL = get

  // separate key/value
  if((val = strchr(args, ' '))) {
    *(val++) = 0;
    g_strstrip(val);
    if(!*val)
      val = NULL;
  }

  // Remove hubname. from the key
  if(strncmp(key, hubname, strlen(hubname)) == 0 && key[strlen(hubname)] == '.')
    key += strlen(hubname)+1;

  // Get var, optionally list, and check whether it can be used in this context
  int var = *key ? vars_byname(key) : -1;
  if(var < 0 && !val && listsettings(hub, hubname, key))
    return;
  check_var(hub, var, key, FALSE);

  // get
  if(!val)
    print_var(hub, hubname, var);

  // set
  else {
    GError *err = NULL;
    char *raw = vars[var].parse(val, &err);
    if(err) {
      g_free(raw);
      ui_mf(NULL, 0, "Error setting `%s': %s", vars[var].name, err->message);
      g_error_free(err);
      return;
    }
    var_set(hub, var, raw, &err);
    g_free(raw);
    if(err) {
      ui_mf(NULL, 0, "Error setting `%s': %s", vars[var].name, err->message);
      g_error_free(err);
    } else
      print_var(hub, hubname, var);
  }
}


static void c_set(char *args)  { sethset(FALSE, args); }
static void c_hset(char *args) { sethset(TRUE,  args); }


// Implements /unset and /hunset
static void unsethunset(gboolean h, const char *key) {
  hubandhubname(h);

  // Get var, optionally list, and check whether it can be used in this context
  int var = *key ? vars_byname(key) : -1;
  if(var < 0 && listsettings(hub, hubname, key))
    return;
  check_var(hub, var, key, TRUE);
  GError *err = NULL;

  // Unset
  var_set(hub, var, NULL, &err);
  if(err) {
    ui_mf(NULL, 0, "Error resetting `%s': %s", vars[var].name, err->message);
    g_error_free(err);
  } else
    ui_mf(NULL, 0, "%s.%s reset.", hubname, vars[var].name);
}


static void c_unset(char *args)  { unsethunset(FALSE, args); }
static void c_hunset(char *args) { unsethunset(TRUE,  args); }


// Implementents suggestions for /h?(un)?set
static void setunset_sug(gboolean set, gboolean h, const char *val, char **sug) {
  guint64 hub = 0;
  if(h) {
    struct ui_tab *tab = ui_tab_cur->data;
    if(tab->type != UIT_HUB && tab->type != UIT_MSG)
      return;
    hub = tab->hub->id;
  }

  char *sep = strchr(val, ' ');

  // Suggest var name
  if(!set || !sep) {
    int len = strlen(val);
    int i, n = 0;
    for(i=0; i<VAR_END && n<20; i++)
      if((hub ? vars[i].hub : vars[i].global) && strncmp(vars[i].name, val, len) == 0 && strlen(vars[i].name) != len)
        sug[n++] = g_strdup(vars[i].name);
    return;
  }

  // Suggest value
  *(sep++) = 0;
  g_strstrip(sep);
  int var = vars_byname(val);
  if(var >= 0 && vars[var].sug) {
    vars[var].sug(var_get(hub, var), sep, sug);
    strv_prefix(sug, val, " ", NULL);
  }
}


static void c_set_sug(char *args, char **sug)    { setunset_sug(TRUE,  FALSE, args, sug); }
static void c_hset_sug(char *args, char **sug)   { setunset_sug(TRUE,  TRUE,  args, sug); }
static void c_unset_sug(char *args, char **sug)  { setunset_sug(FALSE, FALSE, args, sug); }
static void c_hunset_sug(char *args, char **sug) { setunset_sug(FALSE, TRUE,  args, sug); }


static void c_listen(char *args) {
  if(args[0]) {
    ui_m(NULL, 0, "This command does not accept any arguments.");
    return;
  }
  // TODO: If we're currently passive because of an error, might want to give
  // an overview of the configuration rather than this unhelpful "not active"
  // message.
  if(!listen_binds) {
    ui_m(NULL, 0, "Not active on any hub - no listening sockets enabled.");
    return;
  }
  ui_m(NULL, 0, "");
  ui_m(NULL, 0, "Currently opened ports:");
  // TODO: sort the listen_binds and ->hubs lists
  GList *l;
  for(l=listen_binds; l; l=l->next) {
    struct listen_bind *b = l->data;
    GString *h = g_string_new("");
    GSList *n;
    for(n=b->hubs; n; n=n->next) {
      struct hub *hub = hub_global_byid(((struct listen_hub_bind *)n->data)->hubid);
      if(hub) {
        if(h->len > 0)
          g_string_append(h, ", ");
        g_string_append(h, hub->tab->name);
      }
    }
    ui_mf(NULL, 0, " %s:%d (%s): %s", ip4_unpack(b->ip4), b->port, LBT_STR(b->type), h->str);
    g_string_free(h, TRUE);
  }
  ui_m(NULL, 0, "");
}




// definition of the command list
static struct cmd cmds[] = {
  { "accept",      c_accept,      NULL             },
  { "browse",      c_browse,      c_msg_sug        },
  { "clear",       c_clear,       NULL             },
  { "close",       c_close,       NULL             },
  { "connect",     c_connect,     c_connect_sug    },
  { "connections", c_connections, NULL             },
  { "disconnect",  c_disconnect,  NULL             },
  { "gc",          c_gc,          NULL             },
  { "grant",       c_grant,       c_msg_sug        },
  { "help",        c_help,        c_help_sug       },
  { "hset",        c_hset,        c_hset_sug       },
  { "hunset",      c_hunset,      c_hunset_sug     },
  { "kick",        c_kick,        c_msg_sug        },
  { "listen",      c_listen,      NULL             },
  { "me",          c_me,          c_say_sug        },
  { "msg",         c_msg,         c_msg_sug        },
  { "nick",        c_nick,        NULL             },
  { "open",        c_open,        c_open_sug       },
  { "password",    c_password,    NULL             },
  { "pm",          c_msg,         c_msg_sug        },
  { "queue",       c_queue,       NULL             },
  { "quit",        c_quit,        NULL             },
  { "reconnect",   c_reconnect,   NULL             },
  { "refresh",     c_refresh,     fl_local_suggest },
  { "say",         c_say,         c_say_sug        },
  { "search",      c_search,      NULL             },
  { "set",         c_set,         c_set_sug        },
  { "share",       c_share,       c_share_sug      },
  { "ungrant",     c_ungrant,     c_ungrant_sug    },
  { "unset",       c_unset,       c_unset_sug      },
  { "unshare",     c_unshare,     c_unshare_sug    },
  { "userlist",    c_userlist,    NULL             },
  { "version",     c_version,     NULL             },
  { "whois",       c_whois,       c_msg_sug        },
  { "" }
};


void cmd_handle(char *ostr) {
  // special case: ignore empty commands
  if(strspn(ostr, " \t") == strlen(ostr))
    return;
  char *str = g_strdup(ostr);
  if(!str || !str[0]) {
    g_free(str);
    return;
  }

  // extract the command from the string
  char *cmd, *args;
  // not a command, imply '/say <string>'
  if(str[0] != '/') {
    cmd = "say";
    args = str;
  // it is a command, extract cmd and args
  } else {
    char *sep = strchr(str+1, ' ');
    if(sep)
      *sep = 0;
    cmd = str+1;
    args = sep ? sep+1 : str+strlen(str);
  }

  // Strip whitespace around the argument, unless this is the /say command
  if(strcmp(cmd, "say") != 0)
    g_strstrip(args);

  // execute command when found, generate an error otherwise
  struct cmd *c = getcmd(cmd);
  if(c)
    c->f(args);
  else
    ui_mf(NULL, 0, "Unknown command '%s'.", cmd);

  g_free(str);
}


void cmd_suggest(char *ostr, char **sug) {
  struct cmd *c;
  char *str = g_strdup(ostr);
  // complete command name
  if(str[0] == '/' && !strchr(str, ' ')) {
    int i = 0;
    int len = strlen(str)-1;
    for(c=cmds; i<20 && c->f; c++)
      if(strncmp(str+1, c->name, len) == 0 && strlen(c->name) != len)
        sug[i++] = g_strconcat("/", c->name, NULL);
  } else {
    if(str[0] != '/')
      getcmd("say")->suggest(str, sug);
    else {
      char *sep = strchr(str, ' ');
      *sep = 0;
      c = getcmd(str+1);
      if(c && c->suggest) {
        c->suggest(sep+1, sug);
        strv_prefix(sug, str, " ", NULL);
      }
    }
  }
  g_free(str);
}

