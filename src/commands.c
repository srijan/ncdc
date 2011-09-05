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


struct cmd {
  char name[16];
  void (*f)(char *);
  void (*suggest)(char *, char **);
  char *args;
  char *sum;
  char *desc;
};
// tentative definition of the cmd list
static struct cmd cmds[];


// get a command by name. performs a linear search. can be rewritten to use a
// binary search, but I doubt the performance difference really matters.
static struct cmd *getcmd(const char *name) {
  struct cmd *c;
  for(c=cmds; c->f; c++)
    if(strcmp(c->name, name) == 0)
      break;
  return c->f ? c : NULL;
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
        ui_tab_open(ui_msg_create(tab->hub, u), TRUE);
      else
        ui_tab_cur = g_list_find(ui_tabs, t);
      // if we need to send something, do so
      if(sep && *sep)
        hub_msg(tab->hub, u, sep, FALSE);
    }
  }
}


static void c_help(char *args) {
  struct cmd *c;
  // list available commands
  if(!args[0]) {
    ui_m(NULL, 0, "\nAvailable commands:");
    for(c=cmds; c->f; c++)
      ui_mf(NULL, 0, " /%s - %s", c->name, c->sum);
    ui_m(NULL, 0, "");

  // list information on a particular command
  } else {
    c = getcmd(args);
    if(!c)
      ui_mf(NULL, 0, "\nUnknown command '%s'.", args);
    else
      ui_mf(NULL, 0, "\nUsage: /%s %s\n  %s\n\n%s\n", c->name, c->args ? c->args : "", c->sum, c->desc);
  }
}


static void c_help_sug(char *args, char **sug) {
  int i = 0, len = strlen(args);
  struct cmd *c;
  for(c=cmds; i<20 && c->f; c++)
    if(strncmp(c->name, args, len) == 0 && strlen(c->name) != len)
      sug[i++] = g_strdup(c->name);
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
  else if(tab->type != UIT_HUB)
    ui_m(NULL, 0, "This command can only be used on hub tabs.");
  else {
    if(tab->hub->net->conn || tab->hub->net->connecting || tab->hub->reconnect_timer)
      hub_disconnect(tab->hub, FALSE);
    c_connect(""); // also checks for the existence of "hubaddr"
  }
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
      ui_tab_open(tab, TRUE);
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
  struct ui_tab *tab = ui_tab_cur->data;
  if(args[0])
    ui_m(NULL, 0, "This command does not accept any arguments.");
  else if(tab->type == UIT_MAIN)
    ui_m(NULL, 0, "Main tab cannot be closed.");
  else if(tab->type == UIT_HUB)
    ui_hub_close(tab);
  else if(tab->type == UIT_USERLIST) {
    ui_tab_cur = g_list_find(ui_tabs, tab->hub->tab);
    ui_userlist_close(tab);
  } else if(tab->type == UIT_MSG) {
    ui_tab_cur = g_list_find(ui_tabs, tab->hub->tab);
    ui_msg_close(tab);
  } else if(tab->type == UIT_CONN)
    ui_conn_close();
  else if(tab->type == UIT_FL) {
    struct hub_user *u = tab->uid ? g_hash_table_lookup(hub_uids, &tab->uid) : NULL;
    if(u)
      ui_tab_cur = g_list_find(ui_tabs, u->hub->tab);
    ui_fl_close(tab);
  } else if(tab->type == UIT_DL)
    ui_dl_close(tab);
  else if(tab->type == UIT_SEARCH) {
    if(tab->hub)
      ui_tab_cur = g_list_find(ui_tabs, tab->hub->tab);
    ui_search_close(tab);
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
      ui_tab_open(ui_conn_create(), TRUE);
  }
}


static void c_queue(char *args) {
  if(args[0])
    ui_m(NULL, 0, "This command does not accept any arguments.");
  else {
    if(ui_dl)
      ui_tab_cur = g_list_find(ui_tabs, ui_dl);
    else
      ui_tab_open(ui_dl_create(), TRUE);
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

  ui_fl_queue(u, force);
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
  if(!qlen && q->type != 9) {
    ui_m(NULL, 0, "No search query given.");
    goto c_search_clean;
  }
  if(!allhubs) {
    if(tab->type != UIT_HUB && tab->type != UIT_MSG) {
      ui_m(NULL, 0, "This command can only be used on hub tabs. Use the `-all' option to search on all connected hubs.");
      goto c_search_clean;
    }
    if(!tab->hub->nick_valid) {
      ui_m(NULL, 0, "Not connected");
      goto c_search_clean;
    }
    hub_search(tab->hub, q);
  }
  if(allhubs) {
    GList *n;
    gboolean one = FALSE;
    for(n=ui_tabs; n; n=n->next) {
      struct ui_tab *t = n->data;
      if(t->type == UIT_HUB && t->hub->nick_valid) {
        hub_search(t->hub, q);
        one = TRUE;
      }
    }
    if(!one) {
      ui_m(NULL, 0, "Not connected to any hubs.");
      goto c_search_clean;
    }
  }

  // No errors? Then open a search tab and wait for the results.
  ui_tab_open(ui_search_create(allhubs ? NULL : tab->hub, q), TRUE);
  q = NULL; // make sure to not free it

c_search_clean:
  g_strfreev(argv);
  search_q_free(q);
}




// definition of the command list
static struct cmd cmds[] = {
  { "accept", c_accept, NULL,
    NULL, "Accept the TLS certificate of a hub.",
    "This command is used only when the keyprint of the TLS certificate of a hub"
    " does not match the keyprint stored in the configuration file."
  },
  { "browse", c_browse, c_msg_sug,
    "[[-f] <user>]", "Download and browse someone's file list.",
    "Without arguments, this opens a new tab where you can browse your own file list.\n"
    "Note that changes to your list are not immediately visible in the browser."
    " You need to re-open the tab to get the latest version of your list.\n\n"
    "With arguments, the user list of the specified user will be downloaded (if"
    " it has not been downloaded already) and the browse tab will open once it's"
    " complete. The `-f' flag can be used to force the file list to be (re-)downloaded."
  },
  { "clear", c_clear, NULL,
    NULL, "Clear the display.",
    "Clears the log displayed on the screen. Does not affect the log files in any way.\n"
    "Ctrl+l is a shortcut for this command."
  },
  { "close", c_close, NULL,
    NULL, "Close the current tab.",
    "When closing a hub tab, you will be disconnected from the hub.\n"
    "Alt+c is a shortcut for this command."
  },
  { "connect", c_connect, c_connect_sug,
    "[<address>]", "Connect to a hub.",
    "If no address is specified, will connect to the hub you last used on the current tab.\n"
    "The address should be in the form of `dchub://host:port/' or `host:port'.\n"
    "The `:port' part is in both cases optional and defaults to :411.\n\n"
    "Note that this command can only be used on hub tabs. If you want to open a new"
    " connection to a hub, you need to use /open first. For example:\n"
    "  /open testhub\n"
    "  /connect dchub://dc.some-test-hub.com/\n"
    "See `/help open' for more information."
  },
  { "connections", c_connections, NULL,
    NULL, "Display the connection list.",
    "Opens a new tab with the connection list."
  },
  { "disconnect", c_disconnect, NULL,
    NULL, "Disconnect from a hub.",
    "Closes the connection with the hub."
  },
  { "gc", c_gc, NULL,
    NULL, "Perform some garbage collection.",
    "Cleans up unused data and reorganizes existing data to allow more efficient storage and usage.\n"
    "Currently, the only thing being cleaned up is the hashdata.dat file.\n\n"
    "This command may take some time to complete, and will fully block ncdc while it is running.\n"
    "You won't have to perform this command very often."
  },
  { "grant", c_grant, c_msg_sug,
    "[<user>]", "Grant someone a slot.",
    "Granting a slot to someone allows the user to download from you even if you have no free slots.\n\n"
    "The slot will be granted for as long as ncdc stays open. If you restart"
    " ncdc, the user will have to wait for a regular slot. Unless, of course, you"
    " /grant a slot again.\n\n"
    "Note that a granted slot is specific to a single hub. If the user is also"
    " on other hubs, he/she will not be granted a slot on those hubs."
  },
  { "help", c_help, c_help_sug,
    "[<command>]", "Request information on commands.",
    "Use /help without arguments to list all the available commands.\n"
    "Use /help <command> to get information about a particular command."
  },
  { "kick", c_kick, c_msg_sug,
    "<user>", "Kick a user from the hub.",
    "You need to be an OP to be able to use this command."
  },
  { "me", c_me, c_say_sug,
    "<message>", "Chat in third person.",
    "This allows you to talk in third person. Most clients will display your message as something like:\n"
    "  * Nick is doing something\n\n"
    "Note that this command only works correctly on ADC hubs. The NMDC protocol"
    " does not have this feature, and your message will be sent as-is, including the /me."
  },
  { "msg", c_msg, c_msg_sug,
    "<user> [<message>]", "Send a private message.",
    "Send a private message to a user on the currently opened hub.\n"
    "When no message is given, the tab will be opened but no message will be sent."
  },
  { "nick", c_nick, NULL,
    "[<nick>]", "Alias for `/set nick'.",
    ""
  },
  { "open", c_open, c_open_sug,
    "[-n] <name> [<address>]", "Open a new hub tab and connect to the hub.",
    "Opens a new tab to use for a hub. The name is a (short) personal name you"
    " use to identify the hub, and will be used for storing hub-specific"
    " configuration.\n\n"
    "If you have specified an address or have previously connected to a hub"
    " from a tab with the same name, /open will automatically connect to"
    " the hub. Use the `-n' flag to disable this behaviour.\n\n"
    "See `/help connect' for more information on connecting to a hub."
  },
  { "password", c_password, NULL,
    "<password>", "Send your password to the hub.",
    "The /password command can be used to send a password to the hub without saving it to the config file.\n"
    "If you wish to login automatically without having to type /password every time, use '/set password <password>'."
    " Be warned, however, that your password will be saved unencrypted in this case."
  },
  { "pm", c_msg, c_msg_sug,
    "<user> [<message>]", "Alias for /msg",
    ""
  },
  { "queue", c_queue, NULL,
    NULL, "Open the download queue.",
    ""
  },
  { "quit", c_quit, NULL,
    NULL, "Quit ncdc.",
    "You can also just hit ctrl+c, which is equivalent."
  },
  { "reconnect", c_reconnect, NULL,
    NULL, "Shortcut for /disconnect and /connect",
    "When your nick or the hub encoding have been changed, the new settings will be used after the reconnect."
  },
  { "refresh", c_refresh, fl_local_suggest,
    "[<path>]", "Refresh file list.",
    "Initiates a refresh. If no argument is given, the complete list will be refreshed."
    " Otherwise only the specified directory will be refreshed.\n\n"
    "The path argument can be either an absolute filesystem path or a virtual path within your share."
  },
  { "say", c_say, c_say_sug,
    "<message>", "Send a chat message.",
    "You normally don't have to use the /say command explicitly, any command not staring"
    " with '/' will automatically imply `/say <command>'. For example, typing `hello.'"
    " in the command line is equivalent to `/say hello.'.\n\n"
    "Using the /say command explicitly may be useful to send message starting with '/' to"
    " the chat, for example `/say /help is what you are looking for'."
  },
  { "search", c_search, NULL,
    "[options] <query>", "Search for files.",
    "Performs a file search, opening a new tab with the results.\n\n"
    "Available options:\n"
    "  -hub      Search the current hub only. (default)\n"
    "  -all      Search all connected hubs.\n"
    "  -le  <s>  Size of the file must be less than <s>.\n"
    "  -ge  <s>  Size of the file must be larger than <s>.\n"
    "  -t   <t>  File must be of type <t>. (see below)\n"
    "  -tth <h>  TTH root of this file must match <h>.\n\n"
    "File sizes (<s> above) accept the following suffixes: G (GiB), M (MiB) and K (KiB).\n\n"
    "The following file types can be used with the -t option:\n"
    "  1  any      Any file or directory. (default)\n"
    "  2  audio    Audio files.\n"
    "  3  archive  (Compressed) archives.\n"
    "  4  doc      Text documents.\n"
    "  5  exe      Windows executables.\n"
    "  6  img      Image files.\n"
    "  7  video    Videos files.\n"
    "  8  dir      Directories.\n"
    "Note that file type matching is done using the file extension, and is not very reliable."
  },
  { "set", c_set, c_set_sug,
    "[<key> [<value>]]", "Get or set configuration variables.",
    "Use /set without arguments to get a list of configuration variables.\n"
    "/set <key> without value will print out the current value."
  },
  { "share", c_share, c_share_sug,
    "[<name> <path>]", "Add a directory to your share.",
    "Use /share without arguments to get a list of shared directories.\n"
    "When called with a name and a path, the path will be added to your share.\n"
    "Note that shell escaping may be used in the name. For example, to add a"
    " directory with the name `Fun Stuff', you could do the following:\n"
    "  /share \"Fun Stuff\" /path/to/fun/stuff\n"
    "Or:\n"
    "  /share Fun\\ Stuff /path/to/fun/stuff\n\n"
    "The full path to the directory will not be visible to others, only the name you give it will be public.\n"
    "An initial `/refresh' is done automatically on the added directory."
  },
  { "unset", c_unset, c_set_sugkey,
    "<key>", "Unset a configuration variable.",
    "This command will reset the variable back to its default value."
  },
  { "unshare", c_unshare, c_unshare_sug,
    "[<name>]", "Remove a directory from your share.",
    "Use /unshare without arguments to get a list of shared directories.\n"
    "To remove a single directory from your share, use `/unshare <name>'.\n"
    "To remove all directories from your share, use `/unshare /'.\n\n"
    "Note: All hash data for the removed directories will be thrown away. All"
    " files will be re-hashed again when the directory is later re-added."
  },
  { "userlist", c_userlist, NULL,
    NULL, "Open the user list.",
    "Opens the user list of the currently selected hub. Can also be accessed using Alt+u."
  },
  { "version", c_version, NULL,
    NULL, "Display version information.",
    ""
  },
  { "whois", c_whois, c_msg_sug,
    "<user>", "Locate a user in the user list.",
    "This will open the user list and select the given user."
  },
  { "", NULL }
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

