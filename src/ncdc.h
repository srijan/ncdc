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

#include "config.h"
#include <glib.h>
#include <glib/gprintf.h>
#include <gio/gio.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <time.h>
#include <string.h>

#define _XOPEN_SOURCE_EXTENDED
#ifdef HAVE_NCURSESW_NCURSES_H
#include <ncursesw/ncurses.h>
#else
#include <ncurses.h>
#endif

// Use GIT_VERSION, if available
#ifdef GIT_VERSION
# undef VERSION
# define VERSION GIT_VERSION
#endif

#define TLS_SUPPORT     GLIB_CHECK_VERSION(2, 28, 0)
#define TIMEOUT_SUPPORT GLIB_CHECK_VERSION(2, 26, 0)

// forward declaration for data types
// (some of these remain incomplete, others are defined in interfaces)
struct cc;
struct cc_expect;
struct dl;
struct dl_user;
struct dl_user_dl;
struct fl_list;
struct hub;
struct hub_user;
struct logfile;
struct net;
struct search_q;
struct search_r;
struct tiger_ctx;
struct tth_ctx;
struct ui_listing;
struct ui_logwindow;
struct ui_tab;
struct ui_textinput;
struct var;


// include the auto-generated header files
#include "cc.h"
#include "commands.h"
#include "db.h"
#include "dl.h"
#include "fl_local.h"
#include "fl_util.h"
#include "hub.h"
#include "main.h"
#include "net.h"
#include "proto.h"
#include "tth.h"
#include "ui.h"
#include "ui_util.h"
#include "util.h"
#include "vars.h"

