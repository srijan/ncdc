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
#include <limits.h>
#include <stdlib.h>
#include <errno.h>


// currently opened tab, see cmd_handle()
static struct ui_tab *tab;

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



// set options
struct setting {
  char *name;
  char *group;      // NULL = hub name or "global" on non-hub-tabs
  void (*get)(char *, char *);
  void (*set)(char *, char *, char *);
  void (*suggest)(char *, char **);
};


static void get_string(char *group, char *key) {
  GError *err = NULL;
  char *str = g_key_file_get_string(conf_file, group, key, &err);
  if(!str) {
    ui_logwindow_printf(tab->log, "%s.%s is not set.", group, key);
    g_error_free(err);
  } else {
    ui_logwindow_printf(tab->log, "%s.%s = %s", group, key, str);
    g_free(str);
  }
}


static void get_bool(char *group, char *key) {
  GError *err = NULL;
  gboolean val = g_key_file_get_boolean(conf_file, group, key, &err);
  if(err) {
    ui_logwindow_printf(tab->log, "%s.%s is not set.", group, key);
    g_error_free(err);
  } else
    ui_logwindow_printf(tab->log, "%s.%s = %s", group, key, val ? "true" : "false");
}


static void get_int(char *group, char *key) {
  GError *err = NULL;
  int val = g_key_file_get_integer(conf_file, group, key, &err);
  if(err) {
    ui_logwindow_printf(tab->log, "%s.%s is not set.", group, key);
    g_error_free(err);
  } else
    ui_logwindow_printf(tab->log, "%s.%s = %d", group, key, val);
}


#define UNSET(group, key) do {\
    g_key_file_remove_key(conf_file, group, key, NULL);\
    ui_logwindow_printf(tab->log, "%s.%s reset.", group, key);\
  } while(0)


static void set_nick(char *group, char *key, char *val) {
  if(!val) {
    if(strcmp(group, "global") == 0) {
      ui_logwindow_add(tab->log, "global.nick may not be unset.");
      return;
    }
    UNSET(group, key);
    return;
  }
  if(strlen(val) > 32) {
    ui_logwindow_add(tab->log, "Too long nick name.");
    return;
  }
  int i;
  for(i=strlen(val)-1; i>=0; i--)
    if(val[i] == '$' || val[i] == '|' || val[i] == ' ' || val[i] == '<' || val[i] == '>')
      break;
  if(i >= 0) {
    ui_logwindow_add(tab->log, "Invalid character in nick name.");
    return;
  }
  g_key_file_set_string(conf_file, group, key, val);
  get_string(group, key);
  ui_logwindow_add(tab->log, "Your new nick will be used for new hub connections.");
}


// set email/description/connection info
static void set_userinfo(char *group, char *key, char *val) {
  if(!val)
    UNSET(group, key);
  else {
    g_key_file_set_string(conf_file, group, key, val);
    get_string(group, key);
  }
  // hub-specific setting, so only one hub to check
  if(group[0] == '#')
    nmdc_send_myinfo(tab->hub);
  // global setting, check affected hubs
  else {
    GList *n;
    for(n=ui_tabs; n; n=n->next) {
      struct ui_tab *t = n->data;
      if(t->type == UIT_HUB && !g_key_file_has_key(conf_file, t->name, key, NULL))
        nmdc_send_myinfo(t->hub);
    }
  }
}


static void set_encoding(char *group, char *key, char *val) {
  GError *err = NULL;
  if(!val)
    UNSET(group, key);
  else if(!str_convert_check(val, &err)) {
    if(err) {
      ui_logwindow_printf(tab->log, "ERROR: Can't use that encoding: %s", err->message);
      g_error_free(err);
    } else {
      ui_logwindow_add(tab->log, "ERROR: Invalid encoding.");
    }
  } else {
    g_key_file_set_string(conf_file, group, key, val);
    get_string(group, key);
  }
  // TODO: note that this only affects new incoming data? and that a reconnect
  // may be necessary to re-convert all names/descriptions/stuff?
}


static void set_encoding_sug(char *val, char **sug) {
  // This list is neither complete nor are the entries guaranteed to be
  // available. Just a few commonly used encodings. There does not seem to be a
  // portable way of obtaining the available encodings.
  static char *encodings[] = {
    "CP1250", "CP1251", "CP1252", "ISO-2022-JP", "ISO-8859-2", "ISO-8859-7",
    "ISO-8859-8", "ISO-8859-9", "KOI8-R", "LATIN1", "SJIS", "UTF-8",
    "WINDOWS-1250", "WINDOWS-1251", "WINDOWS-1252", NULL
  };
  int i = 0, len = strlen(val);
  char **enc;
  for(enc=encodings; *enc && i<20; enc++)
    if(g_ascii_strncasecmp(val, *enc, len) == 0 && strlen(*enc) != len)
      sug[i++] = g_strdup(*enc);
}


// generic set function for boolean settings that don't require any special attention
static void set_bool(char *group, char *key, char *val) {
  if(!val)
    UNSET(group, key);
  else {
    gboolean new = FALSE;
    if(strcmp(val, "1") == 0 || strcmp(val, "t") == 0 || strcmp(val, "y") == 0
        || strcmp(val, "true") == 0 || strcmp(val, "yes") == 0 || strcmp(val, "on") == 0)
      new = TRUE;
    g_key_file_set_boolean(conf_file, group, key, new);
    get_bool(group, key);
  }
}


// Only suggests "true" or "false" regardless of the input. There are only two
// states anyway, and one would want to switch between those two without any
// hassle.
static void set_bool_sug(char *val, char **sug) {
  gboolean f = !(val[0] == 0 || val[0] == '1' || val[0] == 't' || val[0] == 'y' || val[0] == 'o');
  sug[f ? 1 : 0] = g_strdup("true");
  sug[f ? 0 : 1] = g_strdup("false");
}


static void set_autoconnect(char *group, char *key, char *val) {
  if(strcmp(group, "global") == 0 || group[0] != '#')
    ui_logwindow_add(tab->log, "ERROR: autoconnect can only be used as hub setting.");
  else
    set_bool(group, key, val);
}


static void set_autorefresh(char *group, char *key, char *val) {
  if(!val)
    UNSET(group, key);
  else {
    long v = strtol(val, NULL, 10);
    if((!v && errno == EINVAL) || v < INT_MIN || v > INT_MAX || v < 0)
      ui_logwindow_add(tab->log, "Invalid number.");
    else if(v > 0 && v < 10)
      ui_logwindow_add(tab->log, "Interval between automatic refreshes should be at least 10 minutes.");
    else {
      g_key_file_set_integer(conf_file, group, key, v);
      get_int(group, key);
    }
  }
}


// the settings list
// TODO: help text / documentation?
static struct setting settings[] = {
  { "autoconnect",   NULL,     get_bool,   set_autoconnect, set_bool_sug     }, // may not be used in "global"
  { "autorefresh",   "global", get_int,    set_autorefresh, NULL             }, // in minutes, 0 = disabled
  { "connection",    NULL,     get_string, set_userinfo,    NULL             },
  { "description",   NULL,     get_string, set_userinfo,    NULL             },
  { "email",         NULL,     get_string, set_userinfo,    NULL             },
  { "encoding",      NULL,     get_string, set_encoding,    set_encoding_sug },
  { "nick",          NULL,     get_string, set_nick,        NULL             }, // global.nick may not be /unset
  { "show_joinquit", NULL,     get_bool,   set_bool,        set_bool_sug     },
  { NULL }
};


// get a setting by name
static struct setting *getsetting(const char *name) {
  struct setting *s;
  for(s=settings; s->name; s++)
    if(strcmp(s->name, name) == 0)
      break;
  return s->name ? s : NULL;
}


static gboolean parsesetting(char *name, char **group, char **key, struct setting **s, gboolean *checkalt) {
  char *sep;

  *key = name;
  *group = NULL; // NULL = figure out automatically
  *checkalt = FALSE;

  // separate key/group
  if((sep = strchr(*key, '.'))) {
    *sep = 0;
    *group = *key;
    *key = sep+1;
  }

  // lookup key and validate or figure out group
  *s = getsetting(*key);
  if(!*s) {
    ui_logwindow_printf(tab->log, "No configuration variable with the name '%s'.", *key);
    return FALSE;
  }
  if(*group && (
      ((*s)->group && strcmp(*group, (*s)->group) != 0) ||
      (!(*s)->group && !g_key_file_has_group(conf_file, *group)))) {
    ui_logwindow_add(tab->log, "Wrong configuration group.");
    return FALSE;
  }
  if(!*group)
    *group = (*s)->group;
  if(!*group) {
    if(tab->type == UIT_HUB) {
      *checkalt = TRUE;
      *group = tab->name;
    } else
      *group = "global";
  }
  return TRUE;
}


static void c_set(char *args) {
  g_return_if_fail(tab->log);

  if(!args[0]) {
    struct setting *s;
    ui_logwindow_add(tab->log, "");
    for(s=settings; s->name; s++)
      c_set(s->name);
    ui_logwindow_add(tab->log, "");
    return;
  }

  char *key;
  char *group; // NULL = figure out automatically
  char *val = NULL; // NULL = get
  char *sep;
  struct setting *s;
  gboolean checkalt;

  // separate key/value
  if((sep = strchr(args, ' '))) {
    *sep = 0;
    val = sep+1;
  }

  // get group and key
  if(!parsesetting(args, &group, &key, &s, &checkalt))
    return;

  // get
  if(!val) {
    if(checkalt && !g_key_file_has_key(conf_file, group, key, NULL))
      group = "global";
    s->get(group, key);

  // set
  } else {
    s->set(group, key, val);
    // set() may not always modify the config, but let's just save anyway
    conf_save();
  }
}


static void c_unset(char *args) {
  g_return_if_fail(tab->log);

  if(!args[0]) {
    c_set("");
    return;
  }

  char *key, *group;
  struct setting *s;
  gboolean checkalt;

  // get group and key
  if(!parsesetting(args, &group, &key, &s, &checkalt))
    return;

  if(checkalt && !g_key_file_has_key(conf_file, group, key, NULL))
    group = "global";
  s->set(group, key, NULL);
  conf_save();
}


// Doesn't provide suggestions for group prefixes (e.g. global.<stuff> or
// #hub.<stuff>), but I don't think that'll be used very often anyway.
static void c_set_sugkey(char *args, char **sug) {
  int i = 0, len = strlen(args);
  struct setting *s;
  for(s=settings; i<20 && s->name; s++)
    if(strncmp(s->name, args, len) == 0 && strlen(s->name) != len)
      sug[i++] = g_strdup(s->name);
}


static void c_set_sug(char *args, char **sug) {
  char *sep = strchr(args, ' ');
  if(!sep)
    c_set_sugkey(args, sug);
  else {
    *sep = 0;
    struct setting *s = getsetting(args);
    if(s && s->suggest) {
      s->suggest(sep+1, sug);
      strv_prefix(sug, args, " ", NULL);
    }
  }
}





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


static void c_say(char *args) {
  g_return_if_fail(tab->log);
  if(tab->type != UIT_HUB && tab->type != UIT_MSG)
    ui_logwindow_add(tab->log, "This command can only be used on hub and message tabs.");
  else if(!tab->hub->nick_valid)
    ui_logwindow_add(tab->log, "Not connected or logged in yet.");
  else if(!args[0])
    ui_logwindow_add(tab->log, "Message empty.");
  else if(tab->type == UIT_HUB)
    nmdc_say(tab->hub, args);
  else if(!tab->msg_user)
    ui_logwindow_add(tab->log, "User is not online.");
  else
    nmdc_msg(tab->hub, tab->msg_user, args);
}


static void c_msg(char *args) {
  g_return_if_fail(tab->log);
  char *sep = strchr(args, ' ');
  if(sep) {
    *sep = 0;
    while(*(++sep) == ' ');
  }
  if(tab->type != UIT_HUB && tab->type != UIT_MSG)
    ui_logwindow_add(tab->log, "This command can only be used on hub and message tabs.");
  else if(!tab->hub->nick_valid)
    ui_logwindow_add(tab->log, "Not connected or logged in yet.");
  else if(!args[0])
    ui_logwindow_add(tab->log, "No user specified. See `/help msg' for more information.");
  else {
    struct nmdc_user *u = nmdc_user_get(tab->hub, args);
    if(!u)
      ui_logwindow_add(tab->log, "No user found with that name. Note that usernames are case-sensitive.");
    else {
      // get or open tab and make sure it's selected
      struct ui_tab *t = ui_hub_getmsg(tab, u);
      if(!t) {
        t = ui_msg_create(tab->hub, u);
        ui_tab_open(t);
      } else
        ui_tab_cur = g_list_find(ui_tabs, t);
      // if we need to send something, do so
      if(sep && *sep)
        nmdc_msg(tab->hub, t->msg_user, sep);
    }
  }
}


static void c_help(char *args) {
  g_return_if_fail(tab->log);
  struct cmd *c;
  // list available commands
  if(!args[0]) {
    ui_logwindow_add(tab->log, "");
    ui_logwindow_add(tab->log, "Available commands:");
    for(c=cmds; c->f; c++)
      ui_logwindow_printf(tab->log, " /%s - %s", c->name, c->sum);
    ui_logwindow_add(tab->log, "");

  // list information on a particular command
  } else {
    c = getcmd(args);
    ui_logwindow_add(tab->log, "");
    if(!c)
      ui_logwindow_printf(tab->log, "Unknown command '%s'.", args);
    else {
      ui_logwindow_printf(tab->log, "Usage: /%s %s", c->name, c->args ? c->args : "");
      ui_logwindow_printf(tab->log, "  %s", c->sum);
      ui_logwindow_add(tab->log, "");
      ui_logwindow_add(tab->log, c->desc);
    }
    ui_logwindow_add(tab->log, "");
  }
}


static void c_help_sug(char *args, char **sug) {
  int i = 0, len = strlen(args);
  struct cmd *c;
  for(c=cmds; i<20 && c->f; c++)
    if(strncmp(c->name, args, len) == 0 && strlen(c->name) != len)
      sug[i++] = g_strdup(c->name);
}


static void c_open(char *args) {
  g_return_if_fail(tab->log);
  if(!args[0]) {
    ui_logwindow_add(tab->log, "No hub name given.");
    return;
  }
  char *tmp;
  int len = 0;
  GList *n;
  if(args[0] == '#')
    args++;
  for(tmp=args; *tmp; tmp = g_utf8_next_char(tmp))
    if(++len && !g_unichar_isalnum(g_utf8_get_char(tmp)))
      break;
  if(*tmp || !len || len > 25)
    ui_logwindow_add(tab->log, "Sorry, tab name may only consist of alphanumeric characters, and must not exceed 25 characters.");
  else {
    for(n=ui_tabs; n; n=n->next) {
      tmp = ((struct ui_tab *)n->data)->name;
      if(tmp[0] == '#' && strcmp(tmp+1, args) == 0)
        break;
    }
    if(!n)
      ui_tab_open(ui_hub_create(args));
    else if(n != ui_tab_cur)
      ui_tab_cur = n;
    else
      ui_logwindow_add(tab->log, "Tab already selected.");
  }
}


static void c_open_sug(char *args, char **sug) {
  int len = strlen(args);
  int i = 0;
  char **group, **groups = g_key_file_get_groups(conf_file, NULL);
  for(group=groups; i<20 && *group; group++)
    if(**group == '#' && (strncmp(args, *group, len) == 0 || strncmp(args, *group+1, len) == 0) && strlen(*group) != len)
      sug[i++] = g_strdup(*group);
}


static void c_connect(char *args) {
  g_return_if_fail(tab->log);
  if(tab->type != UIT_HUB)
    ui_logwindow_add(tab->log, "This command can only be used on hub tabs.");
  else if(tab->hub->state != HUBS_IDLE)
    ui_logwindow_add(tab->log, "Already connected (or connecting). You may want to /disconnect first.");
  else {
    if(args[0]) {
      if(strncmp(args, "dchub://", 8) == 0)
        args += 8;
      if(args[strlen(args)-1] == '/')
        args[strlen(args)-1] = 0;
      g_key_file_set_string(conf_file, tab->name, "hubaddr", args);
      conf_save();
    }
    if(!g_key_file_has_key(conf_file, tab->name, "hubaddr", NULL))
      ui_logwindow_add(tab->log, "No hub address configured. Use '/connect <address>' to do so.");
    else
      nmdc_connect(tab->hub);
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
  g_return_if_fail(tab->log);
  if(args[0])
    ui_logwindow_add(tab->log, "This command does not accept any arguments.");
  else if(tab->type != UIT_HUB)
    ui_logwindow_add(tab->log, "This command can only be used on hub tabs.");
  else if(tab->hub->state == HUBS_IDLE)
    ui_logwindow_add(tab->log, "Not connected.");
  else
    nmdc_disconnect(tab->hub);
}


static void c_reconnect(char *args) {
  g_return_if_fail(tab->log);
  if(args[0])
    ui_logwindow_add(tab->log, "This command does not accept any arguments.");
  else if(tab->type != UIT_HUB)
    ui_logwindow_add(tab->log, "This command can only be used on hub tabs.");
  else {
    if(tab->hub->state != HUBS_IDLE)
      nmdc_disconnect(tab->hub);
    c_connect(""); // also checks for the existence of "hubaddr"
  }
}


static void c_close(char *args) {
  if(args[0])
    ui_msg(UIMSG_TAB, "This command does not accept any arguments.");
  else if(tab->type == UIT_MAIN)
    ui_msg(UIMSG_TAB, "Main tab cannot be closed.");
  else if(tab->type == UIT_HUB)
    ui_hub_close(tab);
  else if(tab->type == UIT_USERLIST)
    ui_userlist_close(tab);
  else if(tab->type == UIT_MSG)
    ui_msg_close(tab);
}


static void c_clear(char *args) {
  if(args[0])
    ui_msg(UIMSG_TAB, "This command does not accept any arguments.");
  else if(tab->log)
    ui_logwindow_clear(tab->log);
}


static void c_userlist(char *args) {
  g_return_if_fail(tab->log);
  if(args[0])
    ui_logwindow_add(tab->log, "This command does not accept any arguments.");
  else if(tab->type != UIT_HUB)
    ui_logwindow_add(tab->log, "This command can only be used on hub tabs.");
  else if(tab->userlist_tab)
    ui_tab_cur = g_list_find(ui_tabs, tab->userlist_tab);
  else {
    tab->userlist_tab = ui_userlist_create(tab->hub);
    ui_tab_open(tab->userlist_tab);
  }
}


static void listshares() {
  gsize len;
  char **dirs = g_key_file_get_keys(conf_file, "share", &len, NULL);
  if(!dirs || len == 0)
    ui_logwindow_add(tab->log, "Nothing shared.");
  else {
    ui_logwindow_add(tab->log, "");
    char **cur;
    for(cur=dirs; *cur; cur++) {
      char *d = g_key_file_get_string(conf_file, "share", *cur, NULL);
      struct fl_list *fl = fl_list_file(fl_local_list, *cur);
      ui_logwindow_printf(tab->log, " /%s -> %s (%s)", *cur, d, str_formatsize(fl->size));
      g_free(d);
    }
    ui_logwindow_add(tab->log, "");
  }
  if(dirs)
    g_strfreev(dirs);
}


static void c_share(char *args) {
  g_return_if_fail(tab->log);
  if(!args[0]) {
    listshares();
    return;
  }

  char *first, *second;
  str_arg2_split(args, &first, &second);
  if(!first || !first[0] || !second || !second[0])
    ui_logwindow_add(tab->log, "Error parsing arguments. See \"/help share\" for details.");
  else if(g_key_file_has_key(conf_file, "share", first, NULL))
    ui_logwindow_add(tab->log, "You have already shared a directory with that name.");
  else {
    char *path = path_expand(second);
    if(!path)
      ui_logwindow_printf(tab->log, "Error obtaining absolute path: %s", g_strerror(errno));
    else if(!g_file_test(path, G_FILE_TEST_IS_DIR))
      ui_logwindow_add(tab->log, "Not a directory.");
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
        ui_logwindow_printf(tab->log, "Directory already (partly) shared in /%s", *dir);
      else {
        g_key_file_set_string(conf_file, "share", first, path);
        conf_save();
        fl_share(first);
        ui_logwindow_printf(tab->log, "Added to share: /%s -> %s", first, path);
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
  g_return_if_fail(tab->log);
  if(!args[0])
    listshares();
  // otherwise we may crash
  else if(fl_refresh_queue && fl_refresh_queue->head)
    ui_logwindow_add(tab->log, "Sorry, can't remove directories from the share while refreshing.");
  else {
    while(args[0] == '/')
      args++;
    char *path = g_key_file_get_string(conf_file, "share", args, NULL);
    if(!args[0]) {
      g_key_file_remove_group(conf_file, "share", NULL);
      conf_save();
      fl_unshare(NULL);
      ui_logwindow_add(tab->log, "Removed all directories from share.");
    } else if(!path)
      ui_logwindow_add(tab->log, "No shared directory with that name.");
    else {
      g_key_file_remove_key(conf_file, "share", args, NULL);
      conf_save();
      fl_unshare(args);
      ui_logwindow_printf(tab->log, "Directory /%s (%s) removed from share.", args, path);
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
  g_return_if_fail(tab->log);
  struct fl_list *n = fl_local_from_path(args);
  if(!n)
    ui_msgf(UIMSG_TAB, "Directory `%s' not found.", args);
  else
    fl_refresh(n);
}


static void nick_sug(char *args, char **sug) {
  struct ui_tab *t = ui_tab_cur->data;
  if(!t->hub)
    return;
  // get starting point of the nick
  char *nick = args+strlen(args);
  while(nick > args && *(nick-1) != ' ' && *(nick-1) != ',' && *(nick-1) != ':')
    nick--;
  nmdc_user_suggest(t->hub, nick, sug);
  *nick = 0;
  if(*args)
    strv_prefix(sug, args, NULL);
}


// definition of the command list
static struct cmd cmds[] = {
  { "clear", c_clear, NULL,
    NULL, "Clear the display",
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
  { "disconnect", c_disconnect, NULL,
    NULL, "Disconnect from a hub.",
    "Closes the connection with the hub."
  },
  { "help", c_help, c_help_sug,
    "[<command>]", "Request information on commands.",
    "Use /help without arguments to list all the available commands.\n"
    "Use /help <command> to get information about a particular command."
  },
  { "msg", c_msg, nick_sug,
    "<user> [<message>]", "Send a private message.",
    "Send a private message to a user on the currently opened hub.\n"
    "When no message is given, the tab will be opened but no message will be sent."
  },
  { "open", c_open, c_open_sug,
    "<name>", "Open a new hub tab.",
    "Opens a new tab to use for a hub. The name is a (short) personal name you use to"
    " identify the hub, and will be used for storing hub-specific configuration.\n\n"
    "If you have previously connected to a hub from a tab with the same name, /open"
    " will automatically connect to the same hub again."
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
  { "say", c_say, nick_sug,
    "<message>", "Send a chat message.",
    "You normally don't have to use the /say command explicitly, any command not staring"
    " with '/' will automatically imply `/say <command>'. For example, typing `hello.'"
    " in the command line is equivalent to `/say hello.'.\n\n"
    "Using the /say command explicitly may be useful to send message starting with '/' to"
    " the chat, for example `/say /help is what you are looking for'."
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
    "This command will remove any value set with the specified variable.\n"
    "Can be useful to reset a variable back to its global or default value."
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
    "" // TODO?
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

  // the current opened tab is where this command came from, and where the
  // "replies" should be sent back to. (Some commands require this tab to have
  // a logwindow, others report to ui_msg())
  tab = ui_tab_cur->data;

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
    ui_logwindow_printf(tab->log, "Unknown command '%s'.", cmd);

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

