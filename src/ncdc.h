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
#include <gio/gio.h>
#include <sys/types.h>
#include <time.h>

#define _XOPEN_SOURCE_EXTENDED
#ifdef HAVE_NCURSESW_NCURSES_H
#include <ncursesw/ncurses.h>
#else
#include <ncurses.h>
#endif


// forward declaration for data types
// (some of these remain incomplete, others are defined in interfaces)
struct fl_list;
struct net;
struct nmdc_cc;
struct nmdc_hub;
struct nmdc_user;
struct tth_ctx;
struct ui_logwindow;
struct ui_tab;
struct ui_textinput;


// include the auto-generated header files
#include "commands.h"
#include "fl_local.h"
#include "fl_util.h"
#include "main.h"
#include "nmdc.h"
#include "nmdc_cc.h"
#include "net.h"
#include "tth.h"
#include "ui.h"
#include "ui_util.h"
#include "util.h"

