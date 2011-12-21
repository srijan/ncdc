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
#include <stdlib.h>
#include <errno.h>



// Internal (low-level) utility functions

#define bool_raw(v) (!v ? FALSE : strcmp(v, "true") == 0 ? TRUE : FALSE)
#define int_raw(v) (!v ? 0 : g_ascii_strtoll(v, NULL, 0))


static gboolean bool_parse(const char *val, GError **err) {
  if(strcmp(val, "1") == 0 || strcmp(val, "t") == 0 || strcmp(val, "y") == 0
      || strcmp(val, "true") == 0 || strcmp(val, "yes") == 0 || strcmp(val, "on") == 0)
    return TRUE;
  if(strcmp(val, "0") == 0 || strcmp(val, "f") == 0 || strcmp(val, "n") == 0
      || strcmp(val, "false") == 0 || strcmp(val, "no") == 0 || strcmp(val, "off") == 0)
    return FALSE;
  g_set_error_literal(err, 1, 0, "Unrecognized boolean value.");
  return FALSE;
}

/*static int int_parse(const char *val, GError **err) {
  long v = strtol(val, NULL, 10);
  if((!v && errno == EINVAL) || v < INT_MIN || v > INT_MAX) {
    g_set_error_literal(err, 1, 0, "Invalid number.");
    return 0;
  }
  return v;
}*/


static char *f_id(const char *val) {
  return g_strdup(val);
}

#define f_bool f_id


static char *p_bool(const char *val, GError **err) {
  GError *e = NULL;
  gboolean b = bool_parse(val, &e);
  if(e) {
    g_propagate_error(err, e);
    return NULL;
  }
  return g_strdup(b ? "true" : "false");
}

// Only suggests "true" or "false" regardless of the input. There are only two
// states anyway, and one would want to switch between those two without any
// hassle.
static void su_bool(const char *old, const char *val, char **sug) {
  gboolean f = !(val[0] == 0 || val[0] == '1' || val[0] == 't' || val[0] == 'y' || val[0] == 'o');
  sug[f ? 1 : 0] = g_strdup("true");
  sug[f ? 0 : 1] = g_strdup("false");
}




// Var definitions
// 
// Each variable is defined in a VAR_* (where * is all-capitals), which "calls"
// a V() macro with the following arguments:
//   V(name, global, hub, format, parse, suggest, getraw, setraw, default)
// "default" does not need to be a run-time constant, it will be evaluated at
// initialization instead (after the database has been initialized). Setting
// this to a function allows it to initialize other stuff as well.


// flush_file_cache

// Special interface to allow quick and threaded access to the current value
#if INTERFACE
#define var_flush_file_cache_get() g_atomic_int_get(&var_flush_file_cache)
#define var_flush_file_cache_set(v) g_atomic_int_set(&var_flush_file_cache, v)
#endif

int var_flush_file_cache = 0;

static char *f_flush_file_cache(const char *raw) {
#if HAVE_POSIX_FADVISE
  return f_id(raw);
#else
  return g_strdup("false (not supported)");
#endif
}

static gboolean s_flush_file_cache(guint64 hub, const char *key, const char *val, GError **err) {
  db_vars_set(hub, key, val);
  var_flush_file_cache_set(bool_raw(val));
  return TRUE;
}

static char *i_flush_file_cache() {
  char *r = db_vars_get(0, "flush_file_cache");
  var_flush_file_cache_set(bool_raw(r));
  return "false";
}

#if INTERFACE
#define VAR_FLUSH_FILE_CACHE V(flush_file_cache, 1, 0, f_flush_file_cache, p_bool, su_bool, NULL, s_flush_file_cache, i_flush_file_cache())
#endif


// log_debug

gboolean var_log_debug = TRUE;

static gboolean s_log_debug(guint64 hub, const char *key, const char *val, GError **err) {
  db_vars_set(hub, key, val);
  var_log_debug = bool_raw(val);
  return TRUE;
}

static char *i_log_debug() {
  char *r = db_vars_get(0, "log_debug");
  var_log_debug = bool_raw(r);
  return "false";
}

#if INTERFACE
#define VAR_LOG_DEBUG V(log_debug, 1, 0, f_bool, p_bool, su_bool, NULL, s_log_debug, i_log_debug())
#endif


// log_downloads

#if INTERFACE
#define VAR_LOG_DOWNLOADS V(log_downloads, 1, 0, f_bool, p_bool, su_bool, NULL, NULL, "true")
#endif


// log_uploads

#if INTERFACE
#define VAR_LOG_UPLOADS V(log_uploads, 1, 0, f_bool, p_bool, su_bool, NULL, NULL, "true")
#endif




// Exported data

#if INTERFACE

struct var {
  // Name does not necessarily have to correspond to the name in the 'vars'
  // table. Though in that case special getraw() and setraw() functions have to
  // be used.
  const char *name;
  gboolean global : 1;
  gboolean hub : 1;

  // Formats the raw value for human viewing. Returned string will be
  // g_free()'d. May be NULL if !hub && !global.
  char *(*format)(const char *val);

  // Validates and parses a human input string and returns the "raw" string.
  // Returned string will be g_free()'d. May also return an error if the
  // setting can't be set yet (e.g. if some other setting has to be set
  // first.). Will write any warnings or notes to ui_m(NULL, ..).
  // May be NULL if !hub && !global.
  char *(*parse)(const char *val, GError **err);

  // Suggestion function. *old is the old (raw) value. *val the current string
  // on the input line. May be NULL if no suggestions are available.
  void (*sug)(const char *old, const char *val, char **sug);

  // Get the raw value. The returned string will not be freed and may be
  // modified later. When this is NULL, db_vars_get() is used.
  char *(*getraw)(guint64 hub, const char *name);

  // Set the raw value and make sure it's active. val = NULL to unset it. In
  // general, this function should not fail if parse() didn't return an error,
  // but it may still refuse to set the value set *err to indicate failure.
  // (e.g. when trying to unset a var that must always exist).
  gboolean (*setraw)(guint64 hub, const char *name, const char *val, GError **err);

  // Default raw value, to be used when getraw() returns NULL.
  char *def;
};


#define VARS\
  VAR_FLUSH_FILE_CACHE \
  VAR_LOG_DEBUG \
  VAR_LOG_DOWNLOADS \
  VAR_LOG_UPLOADS

enum var_names {
#define V(n, gl, h, f, p, su, g, s, d) VAR_##n,
  VARS
#undef V
  VAR_END
};

#endif


struct var vars[] = {
#define V(n, gl, h, f, p, su, g, s, d)\
  { G_STRINGIFY(n), gl, h, f, p, su, g, s, NULL },
  VARS
#undef V
  { NULL }
};




// Exported functions


// Get a var id by name. Returns -1 if not found.
// TODO: case insensitive? Allow '-' in addition to '_'?
// TODO: binary search?
int vars_byname(const char *n) {
  int i;
  for(i=0; i<VAR_END; i++)
    if(strcmp(vars[i].name, n) == 0)
      break;
  return i==VAR_END ? -1 : i;
}


// Calls setraw() on the specified var
gboolean var_set(guint64 h, int n, const char *v, GError **err) {
  if(vars[n].setraw)
    return vars[n].setraw(h, vars[n].name, v, err);
  db_vars_set(h, vars[n].name, v);
  return FALSE;
}


// Calls getraw() on the specified var. If h != 0 and no value is found for
// that hub, then another getraw() will be called with h = 0. If that fails,
// the default value is returned instead.
char *var_get(guint64 h, int n) {
  char *r = NULL;
  if(vars[n].getraw)
    r = vars[n].getraw(h, vars[n].name);
  else
    r = db_vars_get(h, vars[n].name);
  return r ? r : h ? var_get(0, n) : vars[n].def;
}


#if INTERFACE
#define var_set_bool(h, n, v) var_set(h, n, v ? "true" : "false", NULL)
#endif


gboolean var_get_bool(guint64 h, int n) {
  char *r = var_get(n, h);
  return bool_raw(r);
}




// Initialization

void vars_init() {
  // Set correct default value
  int var_num = 0;
#define V(n, gl, h, f, p, su, g, s, d)\
    { vars[var_num].def = d; };\
    var_num++;
  VARS
#undef V
}

