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


// currently opened tab, see cmd_handle()
static struct ui_tab *tab;

struct cmd {
  char name[16];
  void (*f)(char *);
  char *args;
  char *sum;
  char *desc;
};
// alias to cmd_list, needed to handle the recursive dependency for c_help()
static struct cmd *cmds;



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
    int i;
    for(i=strlen(val)-1; i>=0; i--)
      if(val[i] == '$' || val[i] == '|')
        break;
    if(i >= 0) {
      ui_logwindow_add(tab->log, "Invalid character.");
      return;
    }
    g_key_file_set_string(conf_file, group, key, val);
    get_string(group, key);
  }
  // TODO: when connected, send update to the hub(s)
}


// the actual settings list
static struct setting settings[] = {
  { "nick",        NULL, get_string, set_nick }, // as a special case, global.nick may not be /unset
  { "email",       NULL, get_string, set_userinfo },
  { "description", NULL, get_string, set_userinfo },
  { "connection",  NULL, get_string, set_userinfo },
  { NULL }
};


// get a setting by name
static inline struct setting *getsetting(const char *name) {
  struct setting *s;
  for(s=settings; s->name; s++)
    if(strcmp(s->name, name) == 0)
      break;
  return s->name ? s : NULL;
}


static inline gboolean parsesetting(char *name, char **group, char **key, struct setting **s, gboolean *checkalt) {
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
    save_config();
  }
}


static void c_unset(char *args) {
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
  save_config();
}




// get a command by name. performs a linear search. can be rewritten to use a
// binary search, but I doubt the performance difference really matters.
static inline struct cmd *getcmd(const char *name) {
  struct cmd *c;
  for(c=cmds; c->f; c++)
    if(strcmp(c->name, name) == 0)
      break;
  return c->f ? c : NULL;
}


static void c_quit(char *args) {
  ui_logwindow_add(tab->log, "Closing ncdc...");
  g_main_loop_quit(main_loop);
}


static void c_say(char *args) {
  ui_logwindow_add(tab->log, "Chatting has not been implemented yet.");
}


static void c_help(char *args) {
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
  if(tab->type != UIT_HUB)
    ui_logwindow_add(tab->log, "This command can only be used on hub tabs.");
  else if(tab->hub->state != HUBS_IDLE)
    ui_logwindow_add(tab->log, "Already connected (or connecting). You may want to /disconnect first.");
  else {
    if(args[0]) {
      g_key_file_set_string(conf_file, tab->name, "hubaddr", args);
      save_config();
    }
    if(!g_key_file_has_key(conf_file, tab->name, "hubaddr", NULL))
      ui_logwindow_add(tab->log, "No hub address configured. Use '/connect <address>' to do so.");
    else
      nmdc_connect(tab->hub);
  }
}


static void c_disconnect(char *args) {
  if(args[0])
    ui_logwindow_add(tab->log, "This command does not accept any arguments.");
  else if(tab->type != UIT_HUB)
    ui_logwindow_add(tab->log, "This command can only be used on hub tabs.");
  else if(tab->hub->state == HUBS_IDLE)
    ui_logwindow_add(tab->log, "Not connected.");
  else
    nmdc_disconnect(tab->hub);
}


static void c_close(char *args) {
  if(args[0])
    ui_logwindow_add(tab->log, "This command does not accept any arguments.");
  else if(tab->type == UIT_MAIN)
    ui_logwindow_add(tab->log, "Main tab cannot be closed.");
  else if(tab->type == UIT_HUB)
    ui_hub_close(tab);
}


// the actual command list (cmds is an alias to this, set by cmd_handle())
static struct cmd cmds_list[] = {
  { "quit", c_quit,
    NULL, "Quit ncdc.",
    "You can also just hit ctrl+c, which is equivalent."
  },
  { "say",  c_say,
    "<message>", "Send a chat message.",
    "Not implemented yet."
  },
  { "help", c_help,
    "[<command>]", "Request information on commands.",
    "Use /help without arguments to list all the available commands.\n"
    "Use /help <command> to get information about a particular command."
  },
  { "open", c_open,
    "<name>", "Open a new hub tab.",
    "Opens a new tab to use for a hub. The name is a (short) personal name you use to identify the hub.\n"
    "If you have previously connected to a hub before from a tab with the same name, /open will automatically connect to the same hub again."
  },
  { "connect", c_connect,
    "[<address>]", "Connect to a hub.",
    "If no address is specified, will connect to the hub you last used on the current tab.\n"
    "The address should be in the form of \"host:port\", where the \":port\" part is optional."
  },
  { "disconnect", c_disconnect,
    NULL, "Disconnect from a hub.",
    "Closes the connection with the hub."
  },
  { "close", c_close,
    NULL, "Close the current tab.",
    "When closing a hub tab, you will be disconnected from the hub."
  },
  { "set", c_set,
    "[<key> [<value>]]", "Get or set configuration variables.",
    "Use /set without arguments to get a list of configuration variables.\n"
    "/set <key> without value will print out the current value."
  },
  { "unset", c_unset,
    "<key>", "Unset a configuration variable.",
    "This command will remove any value set with the specified variable.\n"
    "Can be useful to reset a variable back to its global or default value."
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
  cmds = cmds_list;

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

