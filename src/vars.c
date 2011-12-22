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

static char *f_id(const char *val) {
  return g_strdup(val);
}

#define f_bool f_id
#define f_int f_id

static char *f_interval(const char *val) {
  return g_strdup(str_formatinterval(int_raw(val)));
}

static char *p_id(const char *val, GError **err) {
  return g_strdup(val);
}


static char *p_bool(const char *val, GError **err) {
  GError *e = NULL;
  gboolean b = bool_parse(val, &e);
  if(e) {
    g_propagate_error(err, e);
    return NULL;
  }
  return g_strdup(b ? "true" : "false");
}

static char *p_int(const char *val, GError **err) {
  long v = strtol(val, NULL, 10);
  if((!v && errno == EINVAL) || v < INT_MIN || v > INT_MAX || v < 0) {
    g_set_error_literal(err, 1, 0, "Invalid number.");
    return NULL;
  }
  return g_strdup_printf("%d", (int)v);
}

static char *p_int_ge1(const char *val, GError **err) {
  char *r = p_int(val, err);
  if(r && int_raw(r) < 1) {
    g_set_error_literal(err, 1, 0, "Invalid value.");
    g_free(r);
    return NULL;
  }
  return r;
}

static char *p_interval(const char *val, GError **err) {
  int n = str_parseinterval(val);
  if(n < 0) {
    g_set_error_literal(err, 1, 0, "Invalid interval.");
    return NULL;
  }
  return g_strdup_printf("%d", n);
}

static char *p_ip(const char *val, GError **err) {
  GInetAddress *a = g_inet_address_new_from_string(val);
  if(!a) {
    g_set_error_literal(err, 1, 0, "Invalid IP.");
    return NULL;
  }
  g_object_unref(a);
  return g_strdup(val);
}

// Only suggests "true" or "false" regardless of the input. There are only two
// states anyway, and one would want to switch between those two without any
// hassle.
static void su_bool(const char *old, const char *val, char **sug) {
  gboolean f = !(val[0] == 0 || val[0] == '1' || val[0] == 't' || val[0] == 'y' || val[0] == 'o');
  sug[f ? 1 : 0] = g_strdup("true");
  sug[f ? 0 : 1] = g_strdup("false");
}


static void su_old(const char *old, const char *val, char **sug) {
  if(old && strncmp(old, val, strlen(val)) == 0)
    sug[0] = g_strdup(old);
}

static gboolean s_hubinfo(guint64 hub, const char *key, const char *val, GError **err) {
  db_vars_set(hub, key, val);
  hub_global_nfochange();
  return TRUE;
}

static gboolean s_active_conf(guint64 hub, const char *key, const char *val, GError **err) {
  db_vars_set(hub, key, val);
  if(!val && !hub && strcmp(key, "active_ip") == 0)
    var_set_bool(0, VAR_active, FALSE);
  if(!hub)
    cc_listen_start();
  else
    hub_global_nfochange();
  return TRUE;
}




// Var definitions
//
// Each variable is defined in a VAR_* (where * is all-capitals), which "calls"
// a V() macro with the following arguments:
//   V(name, global, hub, format, parse, suggest, getraw, setraw, default)
// "default" does not need to be a run-time constant, it will be evaluated at
// initialization instead (after the database has been initialized). Setting
// this to a function allows it to initialize other stuff as well.


// active

static gboolean s_active(guint64 hub, const char *key, const char *val, GError **err) {
  if(bool_raw(val) && !var_get(0, VAR_active_ip)) {
    g_set_error_literal(err, 1, 0, "No IP address set. Please use `/set active_ip <your_ip>' first.");
    return FALSE;
  }
  return s_active_conf(hub, key, val, err);
}

#if INTERFACE
#define VAR_ACTIVE V(active, 1, 0, f_bool, p_bool, su_bool, NULL, s_active, "false")
#endif


// active_bind

#if INTERFACE
#define VAR_ACTIVE_BIND V(active_bind, 1, 0, f_id, p_ip, su_old, NULL, s_active_conf, NULL)
#endif


// active_ip

#if INTERFACE
#define VAR_ACTIVE_IP V(active_ip, 1, 1, f_id, p_ip, su_old, NULL, s_active_conf, NULL)
#endif


// active_port

static char *p_active_port(const char *val, GError **err) {
  char *r = p_int(val, err);
  if(r && (int_raw(r) < 1024 || int_raw(r) > 65535)) {
    g_set_error_literal(err, 1, 0, "Port number must be between 1024 and 65535.");
    g_free(r);
    return NULL;
  }
  return r;
}

#if INTERFACE
#define VAR_ACTIVE_PORT V(active_port, 1, 0, f_int, p_active_port, NULL, NULL, s_active_conf, NULL)
#endif


// autorefresh

static char *f_autorefresh(const char *val) {
  int n = int_raw(val);
  if(!n)
    return g_strconcat(str_formatinterval(n), " (disabled)", NULL);
  return f_interval(val);
}

static char *p_autorefresh(const char *val, GError **err) {
  char *raw = p_interval(val, err);
  if(raw && raw[0] != '0' && int_raw(raw) < 600) {
    g_set_error_literal(err, 1, 0, "Interval between automatic refreshes should be at least 10 minutes.");
    g_free(raw);
    return NULL;
  }
  return raw;
}

#if INTERFACE
#define VAR_AUTOREFRESH V(autorefresh, 1, 0, f_autorefresh, p_autorefresh, NULL, NULL, NULL, "3600")
#endif



// nick
// TODO: nick change without reconnect on ADC?

static char *p_nick(const char *val, GError **err) {
  if(strlen(val) > 32) {
    g_set_error_literal(err, 1, 0, "Too long nick name.");
    return NULL;
  }

  int i;
  for(i=strlen(val)-1; i>=0; i--)
    if(val[i] == '$' || val[i] == '|' || val[i] == ' ' || val[i] == '<' || val[i] == '>')
      break;
  if(i >= 0) {
    g_set_error_literal(err, 1, 0, "Invalid character in nick name.");
    return NULL;
  }

  ui_m(NULL, 0, "Your new nick will be used for new hub connections.");
  return g_strdup(val);
}

static gboolean s_nick(guint64 hub, const char *key, const char *val, GError **err) {
  if(!val && !hub) {
    g_set_error_literal(err, 1, 0, "May not be unset.");
    return FALSE;
  }
  db_vars_set(hub, key, val);
  return TRUE;
}

static char *i_nick() {
  // make sure a nick is set
  if(!db_vars_get(0, "nick")) {
    char *nick = g_strdup_printf("ncdc_%d", g_random_int_range(1, 9999));
    db_vars_set(0, "nick", nick);
    g_free(nick);
  }
  return "ncdc";
}

#if INTERFACE
#define VAR_NICK V(nick, 1, 1, f_id, p_nick, su_old, NULL, s_nick, i_nick())
#endif


// email / description / connection

static char *p_connection(const char *val, GError **err) {
  if(!connection_to_speed(val))
    ui_mf(NULL, 0, "Couldn't convert `%s' to bytes/second, won't broadcast upload speed on ADC. See `/help set connection' for more information.", val);
  return g_strdup(val);
}

#if INTERFACE
#define VAR_EMAIL       V(email,       1, 1, f_id, p_id,         su_old, NULL, s_hubinfo, NULL)
#define VAR_CONNECTION  V(connection,  1, 1, f_id, p_connection, su_old, NULL, s_hubinfo, NULL)
#define VAR_DESCRIPTION V(description, 1, 1, f_id, p_id,         su_old, NULL, s_hubinfo, NULL)
#endif


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


// minislots

#if INTERFACE
#define VAR_MINISLOTS V(minislots, 1, 0, f_int, p_int_ge1, NULL, NULL, NULL, "3")
#endif


// minislot_size

static char *p_minislot_size(const char *val, GError **err) {
  char *r = p_int(val, err);
  int n = r ? int_raw(r) : 0;
  g_free(r);
  if(r && n < 64) {
    g_set_error_literal(err, 1, 0, "Minislot size must be at least 64 KiB.");
    return NULL;
  }
  return r ? g_strdup_printf("%d", MIN(G_MAXINT, n*1024)) : NULL;
}

static char *f_minislot_size(const char *val) {
  return g_strdup_printf("%d KiB", (int)int_raw(val)/1024);
}

#if INTERFACE
#define VAR_MINISLOT_SIZE V(minislot_size, 1, 0, f_minislot_size, p_minislot_size, NULL, NULL, NULL, "65536")
#endif


// slots

#if INTERFACE
#define VAR_SLOTS V(slots, 1, 0, f_int, p_int_ge1, NULL, NULL, s_hubinfo, "10")
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
  VAR_ACTIVE \
  VAR_ACTIVE_BIND \
  VAR_ACTIVE_IP \
  VAR_ACTIVE_PORT \
  VAR_AUTOREFRESH \
  VAR_CONNECTION \
  VAR_DESCRIPTION \
  VAR_EMAIL \
  VAR_FLUSH_FILE_CACHE \
  VAR_LOG_DEBUG \
  VAR_LOG_DOWNLOADS \
  VAR_LOG_UPLOADS \
  VAR_MINISLOTS \
  VAR_MINISLOT_SIZE \
  VAR_NICK \
  VAR_SLOTS

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
  char *r = var_get(h, n);
  return bool_raw(r);
}

int var_get_int(guint64 h, int n) {
  char *r = var_get(h, n);
  return int_raw(r);
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

