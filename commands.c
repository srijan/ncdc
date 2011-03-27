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
};


static void c_quit(char *args) {
  g_main_loop_quit(main_loop);
}


static void c_say(char *args) {
  ui_logwindow_add(tab->log, "Chatting has not been implemented yet.");
}


// the command list
static struct cmd cmds[] = {
  { "quit", c_quit },
  { "say",  c_say  },
  { "", NULL }
};


// NOT thread-safe
void cmd_handle(char *ostr) {
  char *str = g_strdup(ostr);
  g_strstrip(str);
  // special case: ignore empty commands
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

  // look up the command in the command list (linear search)
  struct cmd *c;
  for(c=cmds; c->f; c++)
    if(strcmp(c->name, cmd) == 0)
      break;
  
  // execute command if found, generate an error otherwise
  if(c->f)
    c->f(args);
  else
    ui_logwindow_printf(tab->log, "Unknown command '%s'.", cmd);

  g_free(str);
}

