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


// generic set function for boolean settings that don't require any special attention
static void set_bool(char *group, char *key, char *val) {
  if(!val)
    UNSET(group, key);
  else {
    gboolean new = FALSE;
    if(strcmp(val, "1") == 0 || strcmp(val, "t") == 0 || strcmp(val, "y") == 0 || strcmp(val, "true") == 0 || strcmp(val, "yes") == 0)
      new = TRUE;
    g_key_file_set_boolean(conf_file, group, key, new);
    get_bool(group, key);
  }
}


static void set_autoconnect(char *group, char *key, char *val) {
  if(strcmp(group, "global") == 0 || group[0] != '#')
    ui_logwindow_add(tab->log, "ERROR: autoconnect can only be used as hub setting.");
  else
    set_bool(group, key, val);
}


// the settings list
// TODO: help text / documentation?
static struct setting settings[] = {
  { "autoconnect",   NULL, get_bool,   set_autoconnect }, // may not be used in "global"
  { "connection",    NULL, get_string, set_userinfo },
  { "description",   NULL, get_string, set_userinfo },
  { "email",         NULL, get_string, set_userinfo },
  { "encoding",      NULL, get_string, set_encoding },
  { "nick",          NULL, get_string, set_nick },        // global.nick may not be /unset
  { "show_joinquit", NULL, get_bool,   set_bool },
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

  // seperate key/value
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
  g_main_loop_quit(main_loop);
}


static void c_say(char *args) {
  g_return_if_fail(tab->log);
  if(tab->type != UIT_HUB)
    ui_logwindow_add(tab->log, "Chatting only works on hub tabs.");
  else if(!tab->hub->nick_valid)
    ui_logwindow_add(tab->log, "Not connected or logged in yet.");
  else
    nmdc_say(tab->hub, args);
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


static void c_open(char *args) {
  g_return_if_fail(tab->log);
  if(!args[0]) {
    ui_logwindow_add(tab->log, "No hub name given.");
    return;
  }
  char *tmp;
  int len = 0;
  GList *n;
  for(tmp=args; *tmp; tmp = g_utf8_next_char(tmp))
    if(++len && !g_unichar_isalnum(g_utf8_get_char(tmp)))
      break;
  if(*tmp || len > 25)
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
    c_connect(""); // also checks for the existance of "hubaddr"
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


static void c_share(char *args) {
  g_return_if_fail(tab->log);
  // No arguments: list currently shared dirs
  if(!args[0]) {
    gsize len;
    char **dirs = g_key_file_get_keys(conf_file, "share", &len, NULL);
    if(!dirs || len == 0)
      ui_logwindow_add(tab->log, "Nothing shared.");
    else {
      ui_logwindow_add(tab->log, "");
      char **cur;
      for(cur=dirs; *cur; cur++) {
        // TODO: display size (and hash status?)
        char *d = g_key_file_get_string(conf_file, "share", *cur, NULL);
        ui_logwindow_printf(tab->log, " /%s -> %s", *cur, d);
        g_free(d);
      }
      ui_logwindow_add(tab->log, "");
    }
    if(dirs)
      g_strfreev(dirs);
    return;
  }
  // use g_shell_parse_argv() to allow the name to contain a space. (e.g. /share "Kewl Share!" ~/blah)
  // TODO: only use that quoting stuff for the first argument, and allow the second argument to be unquoted?
  GError *err = NULL;
  int argc;
  char **argv;
  if(!g_shell_parse_argv(args, &argc, &argv, &err)) {
    ui_logwindow_printf(tab->log, "Unable to parse arguments: %s", err->message);
    g_error_free(err);
    return;
  }
  if(argc != 2)
    ui_logwindow_add(tab->log, "This command requires two arguments. See \"/help share\" for details.");
  else if(g_key_file_has_key(conf_file, "share", argv[0], NULL))
    ui_logwindow_add(tab->log, "You have already shared a directory with that name.");
  else {
    // TODO: ~ substitution with $HOME?
    char *path = realpath(argv[1], NULL); // TODO: how many systems support this?
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
        g_key_file_set_string(conf_file, "share", argv[0], path);
        conf_save();
        //TODO: fl_refresh(argv[0]);
        ui_logwindow_printf(tab->log, "Added to share: /%s -> %s", argv[0], path);
        ui_logwindow_add(tab->log, "Note: Your file list is not refreshed automatically. Use /refresh to update your list after you are done.");
      }
      if(dirs)
        g_strfreev(dirs);
      free(path);
    }
  }
  g_strfreev(argv);
}


static void c_unshare(char *args) {
  g_return_if_fail(tab->log);
  if(!args[0])
    c_share("");
  else if(!g_key_file_has_key(conf_file, "share", args, NULL))
    ui_logwindow_add(tab->log, "No shared directory with that name.");
  else {
    g_key_file_remove_key(conf_file, "share", args, NULL);
    conf_save();
    ui_logwindow_printf(tab->log, "Directory unshared: /%s", args);
    ui_logwindow_add(tab->log, "Note: Your file list is not refreshed automatically. Use /refresh to update your list after you are done.");
    // TODO: update share
  }
}


// TODO: refresh a single shared dir
// The ability to refresh a single deep subdirectory would be awesome as well,
// but probably less easy.
static void c_refresh(char *args) {
  g_return_if_fail(tab->log);
  if(args[0])
    ui_logwindow_add(tab->log, "This command does not accept any arguments.");
  else {
    ui_logwindow_add(tab->log, "Refreshing file list...");
    fl_refresh(NULL);
  }
}


// definition of the command list
static struct cmd cmds[] = {
  { "clear", c_clear,
    NULL, "Clear the display",
    "Clears the log displayed on the screen. Does not affect the log files in any way.\n"
    "Ctrl+l is a shortcut for this command."
  },
  { "close", c_close,
    NULL, "Close the current tab.",
    "When closing a hub tab, you will be disconnected from the hub.\n"
    "Alt+c is a shortcut for this command."
  },
  { "connect", c_connect,
    "[<address>]", "Connect to a hub.",
    "If no address is specified, will connect to the hub you last used on the current tab.\n"
    "The address should be in the form of \"dchub://host:port/\" or \"host:port\".\n"
    "The \":port\" part is in both cases optional and defaults to :411.\n\n"
    "Note that this command can only be used on hub tabs. If you want to open a new"
    " connection to a hub, you need to use /open first. For example:\n"
    "  /open testhub\n"
    "  /connect dchub://dc.some-test-hub.com/\n"
    "See \"/help open\" for more information."
  },
  { "disconnect", c_disconnect,
    NULL, "Disconnect from a hub.",
    "Closes the connection with the hub."
  },
  { "help", c_help,
    "[<command>]", "Request information on commands.",
    "Use /help without arguments to list all the available commands.\n"
    "Use /help <command> to get information about a particular command."
  },
  { "open", c_open,
    "<name>", "Open a new hub tab.",
    "Opens a new tab to use for a hub. The name is a (short) personal name you use to"
    " identify the hub, and will be used for storing hub-specific configuration.\n\n"
    "If you have previously connected to a hub from a tab with the same name, /open"
    " will automatically connect to the same hub again."
  },
  { "quit", c_quit,
    NULL, "Quit ncdc.",
    "You can also just hit ctrl+c, which is equivalent."
  },
  { "reconnect", c_reconnect,
    NULL, "Shortcut for /disconnect and /connect",
    "When your nick or the hub encoding have been changed, the new settings will be used after the reconnect."
  },
  { "refresh", c_refresh,
    NULL, "Refresh file list.",
    "" // TODO
  },
  { "say",  c_say,
    "<message>", "Send a chat message.",
    "You normally don't have to use the /say command explicitely, any command not staring"
    " with '/' will automatically imply \"/say <command>\". For example, typing \"hello.\""
    " in the command line is equivalent to \"/say hello.\".\n\n"
    "Using the /say command explicitely may be useful to send message starting with '/' to"
    " the chat, for example \"/say /help is what you are looking for\"."
  },
  { "set", c_set,
    "[<key> [<value>]]", "Get or set configuration variables.",
    "Use /set without arguments to get a list of configuration variables.\n"
    "/set <key> without value will print out the current value."
  },
  { "share", c_share,
    "[<name> <directory>]", "Add a directory to your share.",
    "" // TODO
  },
  { "unset", c_unset,
    "<key>", "Unset a configuration variable.",
    "This command will remove any value set with the specified variable.\n"
    "Can be useful to reset a variable back to its global or default value."
  },
  { "unshare", c_unshare,
    "[<name>]", "Remove a directory from your share.",
    "" // TODO
  },
  { "userlist", c_userlist,
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
  // "replies" should be sent back to. (This tab is assumed to have a logwindow
  // object - though this assumption can be relaxed in the future)
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

