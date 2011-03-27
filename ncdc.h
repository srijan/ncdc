
#include "config.h"
#include <wchar.h>
#include <glib.h>

#define _XOPEN_SOURCE_EXTENDED
#ifdef HAVE_NCURSESW_NCURSES_H
#include <ncursesw/ncurses.h>
#else
#include <ncurses.h>
#endif

// Make sure that wchar_t and gunichar are equivalent
// TODO: this should be checked at ./configure time
#ifndef __STDC_ISO_10646__
#error Your wchar_t type is not guaranteed to be UCS-4!
#endif

// include the auto-generated header files
#include "main.h"
#include "ui.h"
#include "ui_util.h"

