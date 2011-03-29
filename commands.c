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


// get a command by name. performs a linear search. can be rewritten to use a
// binary search, but I doubt the performance difference really matters.
static inline struct cmd *getcmd(char *name) {
  struct cmd *c = cmds;
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
  { "", NULL }
};


void cmd_handle(char *ostr) {
  // special case: ignore empty commands
  if(!ostr)
    return;
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

