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
#include <limits.h>
#include <stdlib.h>
#include <errno.h>

#define DOC_CMD
#define DOC_KEY
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
      hub_msg(tab->hub, u, args, me);
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
      struct ui_tab *t = ui_hub_getmsg(tab, u);
      if(!t)
        ui_tab_open(ui_msg_create(tab->hub, u), TRUE, tab);
      else
        ui_tab_cur = g_list_find(ui_tabs, t);
      // if we need to send something, do so
      if(sep && *sep)
        hub_msg(tab->hub, u, sep, FALSE);
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
  } else if(strcmp(args, "set") == 0 && sec) {
    c_help_set(sec);

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
  // help set ..
  if(strncmp(args, "set ", 4) == 0) {
    c_set_sugkey(args+4, sug);
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
  char *old = g_key_file_get_string(conf_file, tab->name, "hubaddr", NULL);

  // Reconstruct (without the kp) and save
  GString *a = g_string_new("");
  g_string_printf(a, "%s://%s:%s/", !proto || !*proto ? "dchub" : proto, host, !port || !*port ? "411" : port);
  g_key_file_set_string(conf_file, tab->name, "hubaddr", a->str);

  // Save kp if specified, or throw it away if the URL changed
  if(kp && *kp)
    g_key_file_set_string(conf_file, tab->name, "hubkp", kp);
  else if(old && strcmp(old, a->str) != 0)
    g_key_file_remove_key(conf_file, tab->name, "hubkp", NULL);

  conf_save();

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
    else if(!g_key_file_has_key(conf_file, tab->name, "hubaddr", NULL))
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
  char *addr = g_key_file_get_string(conf_file, t->name, "hubaddr", NULL);
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
  g_free(addr);
}


static void c_disconnect(char *args) {
  struct ui_tab *tab = ui_tab_cur->data;
  if(args[0])
    ui_m(NULL, 0, "This command does not accept any arguments.");
  else if(tab->type != UIT_HUB)
    ui_m(NULL, 0, "This command can only be used on hub tabs.");
  else if(!tab->hub->net->conn && !tab->hub->reconnect_timer && !tab->hub->net->connecting)
    ui_m(NULL, 0, "Not connected.");
  else
    hub_disconnect(tab->hub, FALSE);
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
    g_key_file_set_string(conf_file, tab->name, "hubkp", enc);
    conf_save();
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
    ui_m(NULL, 0, "Sorry, tab name may only consist of alphanumeric characters, and must not exceed 25 characters.");
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
  char **group, **groups = g_key_file_get_groups(conf_file, NULL);
  for(group=groups; i<20 && *group; group++)
    if(**group == '#' && (strncmp(args, *group, len) == 0 || strncmp(args, *group+1, len) == 0) && strlen(*group) != len)
      sug[i++] = g_strdup(*group);
  g_strfreev(groups);
}


static void c_close(char *args) {
  if(args[0]) {
    ui_m(NULL, 0, "This command does not accept any arguments.");
    return;
  }
  struct ui_tab *tab = ui_tab_cur->data;
  switch(tab->type) {
  case UIT_MAIN:     ui_m(NULL, 0, "Main tab cannot be closed."); break;
  case UIT_HUB:      ui_hub_close(tab);      break;
  case UIT_USERLIST: ui_userlist_close(tab); break;
  case UIT_MSG:      ui_msg_close(tab);      break;
  case UIT_CONN:     ui_conn_close();        break;
  case UIT_FL:       ui_fl_close(tab);       break;
  case UIT_DL:       ui_dl_close(tab);       break;
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
  gsize len;
  char **dirs = g_key_file_get_keys(conf_file, "share", &len, NULL);
  if(!dirs || len == 0)
    ui_m(NULL, 0, "Nothing shared.");
  else {
    ui_m(NULL, 0, "");
    char **cur;
    for(cur=dirs; *cur; cur++) {
      char *d = g_key_file_get_string(conf_file, "share", *cur, NULL);
      struct fl_list *fl = fl_list_file(fl_local_list, *cur);
      ui_mf(NULL, 0, " /%s -> %s (%s)", *cur, d, str_formatsize(fl->size));
      g_free(d);
    }
    ui_m(NULL, 0, "");
  }
  if(dirs)
    g_strfreev(dirs);
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
  else if(g_key_file_has_key(conf_file, "share", first, NULL))
    ui_m(NULL, 0, "You have already shared a directory with that name.");
  else {
    char *path = path_expand(second);
    if(!path)
      ui_mf(NULL, 0, "Error obtaining absolute path: %s", g_strerror(errno));
    else if(!g_file_test(path, G_FILE_TEST_IS_DIR))
      ui_m(NULL, 0, "Not a directory.");
    else {
      // Check whether it (or a subdirectory) is already shared
      char **dirs = g_key_file_get_keys(conf_file, "share", NULL, NULL);
      char **dir;
      for(dir=dirs; dirs && *dir; dir++) {
        char *d = g_key_file_get_string(conf_file, "share", *dir, NULL);
        if(strncmp(d, path, MIN(strlen(d), strlen(path))) == 0) {
          g_free(d);
          break;
        }
        g_free(d);
      }
      if(dirs && *dir)
        ui_mf(NULL, 0, "Directory already (partly) shared in /%s", *dir);
      else {
        g_key_file_set_string(conf_file, "share", first, path);
        conf_save();
        fl_share(first);
        ui_mf(NULL, 0, "Added to share: /%s -> %s", first, path);
      }
      if(dirs)
        g_strfreev(dirs);
      free(path);
    }
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
  if(!args[0])
    listshares();
  // otherwise we may crash
  else if(fl_refresh_queue && fl_refresh_queue->head)
    ui_m(NULL, 0, "Sorry, can't remove directories from the share while refreshing.");
  else {
    while(args[0] == '/')
      args++;
    char *path = g_key_file_get_string(conf_file, "share", args, NULL);
    if(!args[0]) {
      g_key_file_remove_group(conf_file, "share", NULL);
      conf_save();
      fl_unshare(NULL);
      ui_m(NULL, 0, "Removed all directories from share.");
    } else if(!path)
      ui_m(NULL, 0, "No shared directory with that name.");
    else {
      g_key_file_remove_key(conf_file, "share", args, NULL);
      conf_save();
      fl_unshare(args);
      ui_mf(NULL, 0, "Directory /%s (%s) removed from share.", args, path);
    }
    g_free(path);
  }
}


static void c_unshare_sug(char *args, char **sug) {
  int len = strlen(args), i = 0;
  if(args[0] == '/')
    args++;
  char **dir, **dirs = g_key_file_get_keys(conf_file, "share", NULL, NULL);
  for(dir=dirs; dir && *dir && i<20; dir++)
    if(strncmp(args, *dir, len) == 0 && strlen(*dir) != len)
      sug[i++] = g_strdup(*dir);
  g_strfreev(dirs);
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
    fl_hashdat_gc();
    dl_gc();
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


static void c_grant(char *args) {
  struct ui_tab *tab = ui_tab_cur->data;
  struct hub_user *u = NULL;
  if(tab->type != UIT_HUB && tab->type != UIT_MSG)
    ui_m(NULL, 0, "This command can only be used on hub and message tabs.");
  else if(!args[0] && tab->type != UIT_MSG)
    ui_m(NULL, 0, "No user specified. See `/help grant' for more information.");
  else if(args[0]) {
    u = hub_user_get(tab->hub, args);
    if(!u)
      ui_m(NULL, 0, "No user found with that name.");
  } else {
    u = g_hash_table_lookup(hub_uids, &tab->uid);
    if(!u)
      ui_m(NULL, 0, "User not line.");
  }

  if(u) {
    cc_grant(u);
    ui_m(NULL, 0, "Slot granted.");
  }
}


static void c_password(char *args) {
  struct ui_tab *tab = ui_tab_cur->data;
  if(tab->type != UIT_HUB)
    ui_m(NULL, 0, "This command can only be used on hub tabs.");
  else if(!tab->hub->net->conn)
    ui_m(NULL, 0, "Not connected to a hub. Did you want to use '/set password' instead?");
  else if(tab->hub->nick_valid)
    ui_m(NULL, 0, "Already logged in. Did you want to use '/set password' instead?");
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
  // not the most elegant solution, but certainly the most simple.
  char *c = g_strdup_printf("nick %s", args);
  c_set(c);
  g_free(c);
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

  ui_fl_queue(u, force, NULL, !args[0] ? NULL : tab);
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
  { "kick",        c_kick,        c_msg_sug        },
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
  { "unset",       c_unset,       c_set_sugkey     },
  { "unshare",     c_unshare,     c_unshare_sug    },
  { "userlist",    c_userlist,    NULL             },
  { "version",     c_version,     NULL             },
  { "whois",       c_whois,       c_msg_sug        },
  { "" }
};


void cmd_handle(char *ostr) {
  // special case: ignore empty commands
  char *str = g_strdup(ostr);
  g_strstrip(str);
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
    args = sep ? sep+1 : "";
  }

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

