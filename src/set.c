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
#include <limits.h>
#include <stdlib.h>
#include <errno.h>

#define DOC_SET
#include "doc.h"


#define hubname(g) (!(g) ? "global" : db_vars_get((g), "hubname"))


// set options
struct setting {
  char *name;
  void (*get)(guint64, char *);
  void (*set)(guint64, char *, char *);
  void (*suggest)(guint64, char *, char *, char **);
  struct doc_set *doc;
};


static void get_string(guint64 hub, char *key) {
  char *str = db_vars_get(hub, key);
  if(!str)
    ui_mf(NULL, 0, "%s.%s is not set.", hubname(hub), key);
  else
    ui_mf(NULL, 0, "%s.%s = %s", hubname(hub), key, str);
}


// not set => false
static void get_bool_f(guint64 hub, char *key) {
  ui_mf(NULL, 0, "%s.%s = %s", hubname(hub), key, conf_get_bool(hub, key) ? "true" : "false");
}


static void get_int(guint64 hub, char *key) {
  if(!conf_exists(hub, key))
    ui_mf(NULL, 0, "%s.%s is not set.", hubname(hub), key);
  else
    ui_mf(NULL, 0, "%s.%s = %d", hubname(hub), key, conf_get_int(hub, key));
}


static gboolean bool_var(const char *val) {
  if(strcmp(val, "1") == 0 || strcmp(val, "t") == 0 || strcmp(val, "y") == 0
      || strcmp(val, "true") == 0 || strcmp(val, "yes") == 0 || strcmp(val, "on") == 0)
    return TRUE;
  return FALSE;
}


static void get_encoding(guint64 hub, char *key) {
  ui_mf(NULL, 0, "%s.%s = %s", hubname(hub), key, conf_encoding(hub));
}


static void set_encoding(guint64 hub, char *key, char *val) {
  GError *err = NULL;
  if(!val) {
    db_vars_rm(hub, key);
    ui_mf(NULL, 0, "%s.%s reset.", hubname(hub), key);
  } else if(!str_convert_check(val, &err)) {
    if(err) {
      ui_mf(NULL, 0, "ERROR: Can't use that encoding: %s", err->message);
      g_error_free(err);
    } else
      ui_m(NULL, 0, "ERROR: Invalid encoding.");
  } else {
    db_vars_set(hub, key, val);
    get_encoding(hub, key);
  }
  // TODO: note that this only affects new incoming data? and that a reconnect
  // may be necessary to re-convert all names/descriptions/stuff?
}


static void set_encoding_sug(guint64 hub, char *key, char *val, char **sug) {
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
static void set_bool_f(guint64 hub, char *key, char *val) {
  if(!val) {
    db_vars_rm(hub, key);
    ui_mf(NULL, 0, "%s.%s reset.", hubname(hub), key);
    return;
  }

  conf_set_bool(hub, key, bool_var(val));
  get_bool_f(hub, key);
}


// Only suggests "true" or "false" regardless of the input. There are only two
// states anyway, and one would want to switch between those two without any
// hassle.
static void set_bool_sug(guint64 hub, char *key, char *val, char **sug) {
  gboolean f = !(val[0] == 0 || val[0] == '1' || val[0] == 't' || val[0] == 'y' || val[0] == 'o');
  sug[f ? 1 : 0] = g_strdup("true");
  sug[f ? 0 : 1] = g_strdup("false");
}


static void set_autoconnect(guint64 hub, char *key, char *val) {
  if(!hub)
    ui_m(NULL, 0, "ERROR: autoconnect can only be used as hub setting.");
  else
    set_bool_f(hub, key, val);
}


static void set_active(guint64 hub, char *key, char *val) {
  if(!val) {
    db_vars_rm(0, key);
    ui_mf(NULL, 0, "global.%s reset.", key);
  } else if(bool_var(val) && !conf_exists(0, "active_ip")) {
    ui_m(NULL, 0, "ERROR: No IP address set. Please use `/set active_ip <your_ip>' first (on a non-hub tab).");
    return;
  }
  set_bool_f(0, key, val);
  cc_listen_start();
}


static void set_active_ip(guint64 hub, char *key, char *val) {
  if(!val) {
    db_vars_rm(hub, key);
    ui_mf(NULL, 0, "%s.%s reset.", hubname(hub), key);
    if(!hub)
      set_active(0, "active", NULL);
    else
      hub_global_nfochange();
    return;
  }
  // TODO: IPv6?
  if(!g_regex_match_simple("^[0-9]{1,3}\\.[0-9]{1,3}\\.[0-9]{1,3}\\.[0-9]{1,3}$", val, 0, 0)
      || strncmp("127.", val, 4) == 0 || strncmp("0.", val, 2) == 0) {
    ui_m(NULL, 0, "ERROR: Invalid IP.");
    return;
  }
  db_vars_set(hub, key, val);
  get_string(hub, key);
  if(!hub)
    cc_listen_start();
  else
    hub_global_nfochange();
}


static void set_active_port(guint64 hub, char *key, char *val) {
  if(!val) {
    db_vars_rm(0, key);
    ui_mf(NULL, 0, "global.%s reset.", key);
  } else {
    long v = strtol(val, NULL, 10);
    if((!v && errno == EINVAL) || v < 0 || v > 65535) {
      ui_m(NULL, 0, "Invalid port number.");
      return;
    }
    conf_set_int(0, key, v);
    get_int(0, key);
  }
  cc_listen_start();
}


static void set_active_bind(guint64 hub, char *key, char *val) {
  if(!val) {
    db_vars_rm(0, key);
    ui_mf(NULL, 0, "global.%s reset.", key);
  } else {
    GInetAddress *a = g_inet_address_new_from_string(val);
    if(!a) {
      ui_m(NULL, 0, "Invalid IP.");
      return;
    }
    g_object_unref(a);
    db_vars_set(0, key, val);
    get_string(0, key);
  }
  cc_listen_start();
}


static void get_minislot_size(guint64 hub, char *key) {
  ui_mf(NULL, 0, "global.%s = %d KiB", key, conf_minislot_size() / 1024);
}


static void set_minislot_size(guint64 hub, char *key, char *val) {
  if(!val) {
    db_vars_rm(0, key);
    ui_mf(NULL, 0, "global.%s reset.", key);
    return;
  }

  long v = strtol(val, NULL, 10);
  if((!v && errno == EINVAL) || v < INT_MIN || v > INT_MAX/1024 || v < 0)
    ui_m(NULL, 0, "Invalid number.");
  else if(v < 64)
    ui_m(NULL, 0, "Minislot size must be at least 64 KiB.");
  else {
    conf_set_int(0, key, v*1024);
    get_minislot_size(0, key);
  }
}


static void get_minislots(guint64 hub, char *key) {
  ui_mf(NULL, 0, "global.%s = %d", key, conf_minislots());
}


static void set_minislots(guint64 hub, char *key, char *val) {
  if(!val) {
    db_vars_rm(0, key);
    ui_mf(NULL, 0, "global.%s reset.", key);
    return;
  }

  long v = strtol(val, NULL, 10);
  if((!v && errno == EINVAL) || v < INT_MIN || v > INT_MAX || v < 0)
    ui_m(NULL, 0, "Invalid number.");
  else if(v < 1)
    ui_m(NULL, 0, "You must have at least 1 minislot.");
  else {
    conf_set_int(0, key, v);
    get_minislots(0, key);
  }
}


static void get_password(guint64 hub, char *key) {
  ui_mf(NULL, 0, "%s.%s is %s", hubname(hub), key, conf_exists(hub, key) ? "set" : "not set");
}


static void set_password(guint64 hub, char *key, char *val) {
  if(!hub)
    ui_m(NULL, 0, "ERROR: password can only be used as hub setting.");
  else if(!val) {
    db_vars_rm(hub, key);
    ui_mf(NULL, 0, "%s.%s reset.", hubname(hub), key);
  } else {
    db_vars_set(hub, key, val);
    struct ui_tab *tab = ui_tab_cur->data;
    if(tab->type == UIT_HUB && tab->hub->net->conn && !tab->hub->nick_valid)
      hub_password(tab->hub, NULL);
    ui_m(NULL, 0, "Password saved.");
  }
}


// a bit pointless, but for consistency's sake
static void get_hubname(guint64 hub, char *key) {
  if(!hub)
    ui_mf(NULL, 0, "global.%s is not set.", key);
  else
    ui_mf(NULL, 0, "%s.%s = %s", hubname(hub), key, hubname(hub)+1);
}


static void set_hubname(guint64 hub, char *key, char *val) {
  if(!hub)
    ui_mf(NULL, 0, "ERROR: hubname can only be used as hub setting.");
  else if(!val || !val[0])
    ui_mf(NULL, 0, "%s.%s may not be unset.", hubname(hub), key);
  else {
    if(val[0] == '#')
      val++;
    char *g = g_strdup_printf("#%s", val);
    if(!is_valid_hubname(val))
      ui_mf(NULL, 0, "Invalid name.");
    else if(db_vars_hubid(g))
      ui_mf(NULL, 0, "Name already used.");
    else {
      db_vars_set(hub, key, g);
      GList *n;
      for(n=ui_tabs; n; n=n->next) {
        struct ui_tab *t = n->data;
        if(t->type == UIT_HUB && t->hub->id == hub) {
          g_free(t->name);
          t->name = g_strdup(g);
        }
      }
      get_hubname(hub, key);
    }
    g_free(g);
  }
}


static void set_hubname_sug(guint64 hub, char *key, char *val, char **sug) {
  if(hub)
    sug[0] = g_strdup(hubname(hub));
}


static void get_download_dir(guint64 hub, char *key) {
  char *d = conf_download_dir();
  ui_mf(NULL, 0, "global.%s = %s", key, d);
  g_free(d);
}


static void get_incoming_dir(guint64 hub, char *key) {
  char *d = conf_incoming_dir();
  ui_mf(NULL, 0, "global.%s = %s", key, d);
  g_free(d);
}


static void set_dl_inc_dir(guint64 hub, char *key, char *val) {
  gboolean dl = strcmp(key, "download_dir") == 0 ? TRUE : FALSE;

  // Don't allow changes to incoming_dir when the download queue isn't empty
  if(!dl && g_hash_table_size(dl_queue) > 0) {
    ui_m(NULL, 0, "Can't change the incoming directory unless the download queue is empty.");
    return;
  }

  char *nval = val ? g_strdup(val) : g_build_filename(db_dir, dl ? "dl" : "inc", NULL);
  gboolean cont = FALSE, warn = FALSE;
  // check if it exists
  if(g_file_test(nval, G_FILE_TEST_EXISTS)) {
    if(!g_file_test(nval, G_FILE_TEST_IS_DIR))
      ui_mf(NULL, 0, "%s: Not a directory.", nval);
    else
      cont = TRUE;
  // otherwise, create
  } else {
    GFile *d = g_file_new_for_path(nval);
    GError *err = NULL;
    g_file_make_directory_with_parents(d, NULL, &err);
    g_object_unref(d);
    if(err) {
      ui_mf(NULL, 0, "Error creating `%s': %s", nval, err->message);
      g_error_free(err);
    } else
      cont = TRUE;
  }
  // test whether they are on the same filesystem
  if(cont) {
    struct stat a, b;
    char *bd = dl ? conf_incoming_dir() : conf_download_dir();
    if(stat(bd, &b) < 0) {
      ui_mf(NULL, 0, "Error stat'ing %s: %s.", bd, g_strerror(errno));
      cont = FALSE;
    }
    g_free(bd);
    if(cont && stat(nval, &a) < 0) {
      ui_mf(NULL, 0, "Error stat'ing %s: %s", a, g_strerror(errno));
      cont = FALSE;
    }
    if(!cont || (cont && a.st_dev != b.st_dev))
      warn = TRUE;
    cont = TRUE;
  }
  // no errors? save.
  if(cont) {
    if(!val) {
      db_vars_rm(0, key);
      ui_mf(NULL, 0, "global.%s reset.", key);
    } else {
      db_vars_set(0, key, val);
      if(dl)
        get_download_dir(0, key);
      else
        get_incoming_dir(0, key);
    }
  }
  if(warn)
    ui_m(NULL, 0, "WARNING: The download directory is not on the same filesystem as the incoming"
                  " directory. This may cause the program to hang when downloading large files.");
  g_free(nval);
}


static void get_download_slots(guint64 hub, char *key) {
  ui_mf(NULL, 0, "global.%s = %d", key, conf_download_slots());
}


static void set_download_slots(guint64 hub, char *key, char *val) {
  int oldval = conf_download_slots();
  if(!val) {
    db_vars_rm(0, key);
    ui_mf(NULL, 0, "global.%s reset.", key);
  } else {
    long v = strtol(val, NULL, 10);
    if((!v && errno == EINVAL) || v < INT_MIN || v > INT_MAX || v <= 0)
      ui_m(NULL, 0, "Invalid number.");
    else {
      conf_set_int(0, key, v);
      get_download_slots(0, key);
    }
  }
  if(conf_download_slots() > oldval)
    dl_queue_start();
}


static void get_backlog(guint64 hub, char *key) {
  int n = conf_get_int(hub, key);
  ui_mf(NULL, 0, "%s.%s = %d%s", hubname(hub), key, n, !n ? " (disabled)" : "");
}


static void set_backlog(guint64 hub, char *key, char *val) {
  if(!val) {
    db_vars_rm(hub, key);
    ui_mf(NULL, 0, "%s.%s reset.", hubname(hub), key);
    return;
  }

  errno = 0;
  long v = strtol(val, NULL, 10);
  if((!v && errno == EINVAL) || v < INT_MIN || v > INT_MAX || v < 0)
    ui_m(NULL, 0, "Invalid number.");
  else if(v >= LOGWIN_BUF)
    ui_mf(NULL, 0, "Maximum value is %d.", LOGWIN_BUF-1);
  else {
    conf_set_int(hub, key, v);
    get_backlog(hub, key);
  }
}


static void get_color(guint64 hub, char *key) {
  g_return_if_fail(strncmp(key, "color_", 6) == 0);
  struct ui_color *c = ui_color_by_name(key+6);
  g_return_if_fail(c);
  ui_mf(NULL, 0, "global.%s = %s", key, ui_color_str_gen(c->fg, c->bg, c->x));
}


static void set_color(guint64 hub, char *key, char *val) {
  if(!val) {
    db_vars_rm(0, key);
    ui_mf(NULL, 0, "global.%s reset.", key);
    ui_colors_update();
    return;
  }

  GError *err = NULL;
  if(!ui_color_str_parse(val, NULL, NULL, NULL, &err)) {
    ui_m(NULL, 0, err->message);
    g_error_free(err);
  } else {
    db_vars_set(0, key, val);
    ui_colors_update();
    get_color(0, key);
  }
}


static void set_color_sug(guint64 hub, char *key, char *val, char **sug) {
  char *attr = strrchr(val, ',');
  if(attr)
    *(attr++) = 0;
  else
    attr = val;
  g_strstrip(attr);
  struct ui_attr *a = ui_attr_names;
  int i = 0, len = strlen(attr);
  for(; a->name[0] && i<20; a++)
    if(strncmp(attr, a->name, len) == 0)
      sug[i++] = g_strdup(a->name);
  if(i && attr != val)
    strv_prefix(sug, val, ",", NULL);
}


static void get_tls_policy(guint64 hub, char *key) {
  ui_mf(NULL, 0, "%s.%s = %s%s", hubname(hub), key,
    conf_tlsp_list[conf_tls_policy(hub)], db_certificate ? "" : " (not supported)");
}


static void set_tls_policy(guint64 hub, char *key, char *val) {
  int old = conf_tls_policy(hub);
  if(!val) {
    db_vars_rm(hub, key);
    ui_mf(NULL, 0, "%s.%s reset.", hubname(hub), key);
  } else if(!db_certificate)
    ui_mf(NULL, 0, "This option can't be modified: %s.",
      !have_tls_support ? "no TLS support available" : "no client certificate available");
  else {
    int p =
      strcmp(val, "0") == 0 || strcmp(val, conf_tlsp_list[0]) == 0 ? 0 :
      strcmp(val, "1") == 0 || strcmp(val, conf_tlsp_list[1]) == 0 ? 1 :
      strcmp(val, "2") == 0 || strcmp(val, conf_tlsp_list[2]) == 0 ? 2 : -1;
    if(p < 0)
      ui_m(NULL, 0, "Invalid TLS policy.");
    else {
      conf_set_int(hub, key, p);
      get_tls_policy(hub, key);
    }
  }
  if(old != conf_tls_policy(hub))
    hub_global_nfochange();
}


static void set_tls_policy_sug(guint64 hub, char *key, char *val, char **sug) {
  int i = 0, j = 0, len = strlen(val);
  for(i=0; i<=2; i++)
    if(g_ascii_strncasecmp(val, conf_tlsp_list[i], len) == 0 && strlen(conf_tlsp_list[i]) != len)
      sug[j++] = g_strdup(conf_tlsp_list[i]);
}


static void set_regex(guint64 hub, char *key, char *val) {
  if(!val) {
    db_vars_rm(hub, key);
    ui_mf(NULL, 0, "%s.%s reset.", hubname(hub), key);
    return;
  }
  GError *err = NULL;
  GRegex *r = g_regex_new(val, 0, 0, &err);
  if(!r) {
    ui_m(NULL, 0, err->message);
    g_error_free(err);
  } else {
    g_regex_unref(r);
    db_vars_set(hub, key, val);
    get_string(hub, key);
  }
}


static void get_ui_time_format(guint64 hub, char *key) {
  ui_mf(NULL, 0, "global.%s = %s", key, conf_ui_time_format());
}


static void set_ui_time_format(guint64 hub, char *key, char *val) {
  if(!val) {
    db_vars_rm(0, key);
    ui_mf(NULL, 0, "global.%s reset.", key);\
    return;
  }

  db_vars_set(0, key, val);
  get_ui_time_format(0, key);
}


static void set_path_sug(guint64 hub, char *key, char *val, char **sug) {
  path_suggest(val, sug);
}


// Suggest the current value. Works for both integers and strings. Perhaps also
// for booleans, but set_bool_sug() is more useful there anyway.
// BUG: This does not use a default value, if there is one...
static void set_old_sug(guint64 hub, char *key, char *val, char **sug) {
  char *old = db_vars_get(hub, key);
  if(old && strncmp(old, val, strlen(val)) == 0)
    sug[0] = g_strdup(old);
}


static void get_filelist_maxage(guint64 hub, char *key) {
  ui_mf(NULL, 0, "global.%s = %s", key, str_formatinterval(conf_filelist_maxage()));
}


static void set_filelist_maxage(guint64 hub, char *key, char *val) {
  if(!val) {
    db_vars_rm(0, key);
    ui_mf(NULL, 0, "global.%s reset.", key);\
    return;
  }

  int v = str_parseinterval(val);
  if(v < 0)
    ui_m(NULL, 0, "Invalid number.");
  else {
    conf_set_int(0, key, v);
    get_filelist_maxage(0, key);
  }
}


// the settings list
static struct setting settings[] = {
  { "active",           get_bool_f,          set_active,          set_bool_sug       },
  { "active_bind",      get_string,          set_active_bind,     set_old_sug        },
  { "active_ip",        get_string,          set_active_ip,       set_old_sug        },
  { "active_port",      get_int,             set_active_port,     NULL,              },
  { "autoconnect",      get_bool_f,          set_autoconnect,     set_bool_sug       },
  { "backlog",          get_backlog,         set_backlog,         NULL,              },
  { "chat_only",        get_bool_f,          set_bool_f,          set_bool_sug       },
#define C(n, a,b,c) { "color_" G_STRINGIFY(n), get_color, set_color, set_color_sug },
  UI_COLORS
#undef C
  { "download_dir",     get_download_dir,    set_dl_inc_dir,      set_path_sug       },
  { "download_slots",   get_download_slots,  set_download_slots,  NULL               },
  { "download_exclude", get_string,          set_regex,           set_old_sug        },
  { "encoding",         get_encoding,        set_encoding,        set_encoding_sug   },
  { "filelist_maxage",  get_filelist_maxage, set_filelist_maxage, set_old_sug        },
  { "hubname",          get_hubname,         set_hubname,         set_hubname_sug    },
  { "incoming_dir",     get_incoming_dir,    set_dl_inc_dir,      set_path_sug       },
  { "minislots",        get_minislots,       set_minislots,       NULL               },
  { "minislot_size",    get_minislot_size,   set_minislot_size,   NULL               },
  { "password",         get_password,        set_password,        NULL               },
  { "share_hidden",     get_bool_f,          set_bool_f,          set_bool_sug       },
  { "share_exclude",    get_string,          set_regex,           set_old_sug        },
  { "show_joinquit",    get_bool_f,          set_bool_f,          set_bool_sug       },
  { "tls_policy",       get_tls_policy,      set_tls_policy,      set_tls_policy_sug },
  { "ui_time_format",   get_ui_time_format,  set_ui_time_format,  set_old_sug        },
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


// Get documentation for a setting. May return NULL.
static struct doc_set *getdoc(struct setting *s) {
  // Anything prefixed with `color_' can go to the `color_*' doc
  char *n = strncmp(s->name, "color_", 6) == 0 ? "color_*" : s->name;

  if(s->doc)
    return s->doc;
  struct doc_set *i = (struct doc_set *)doc_sets;
  for(; i->name; i++)
    if(strcmp(i->name, n) == 0)
      return i;
  return NULL;
}


static gboolean parsesetting(char *name, guint64 *hub, char **key, struct setting **s, gboolean *checkalt) {
  char *sep;

  *key = name;
  *hub = 0;
  char *group = NULL;
  *checkalt = FALSE;

  // separate key/group
  if((sep = strchr(*key, '.'))) {
    *sep = 0;
    group = *key;
    *key = sep+1;
  }

  // lookup key and validate or figure out group
  *s = getsetting(*key);
  if(!*s) {
    ui_mf(NULL, 0, "No configuration variable with the name '%s'.", *key);
    return FALSE;
  }
  if(group && strcmp(group, "global") != 0) {
    *hub = db_vars_hubid(group);
    if(!getdoc(*s)->hub || !*hub) {
      ui_m(NULL, 0, "Wrong configuration group.");
      return FALSE;
    }
  }

  if(!group) {
    struct ui_tab *tab = ui_tab_cur->data;
    if(getdoc(*s)->hub && tab->type == UIT_HUB) {
      *checkalt = TRUE;
      *hub = tab->hub->id;
    }
  }
  return TRUE;
}


void c_oset(char *args) {
  if(!args[0]) {
    struct setting *s;
    ui_m(NULL, 0, "");
    for(s=settings; s->name; s++)
      c_oset(s->name);
    ui_m(NULL, 0, "");
    return;
  }

  char *key;
  guint64 hub = 0;
  char *val = NULL; // NULL = get
  char *sep;
  struct setting *s;
  gboolean checkalt;

  // separate key/value
  if((sep = strchr(args, ' '))) {
    *sep = 0;
    val = sep+1;
    g_strstrip(val);
  }

  // get hub and key
  if(!parsesetting(args, &hub, &key, &s, &checkalt))
    return;

  // get
  if(!val || !val[0]) {
    if(checkalt && !conf_exists(hub, key))
      hub = 0;
    s->get(hub, key);

  // set
  } else
    s->set(hub, key, val);
}


void c_ounset(char *args) {
  if(!args[0]) {
    c_oset("");
    return;
  }

  char *key;
  guint64 hub;
  struct setting *s;
  gboolean checkalt;

  // get hub and key
  if(!parsesetting(args, &hub, &key, &s, &checkalt))
    return;

  if(checkalt && !conf_exists(hub, key))
    hub = 0;
  s->set(hub, key, NULL);
}


// Doesn't provide suggestions for group prefixes (e.g. global.<stuff> or
// #hub.<stuff>), but I don't think that'll be used very often anyway.
void c_oset_sugkey(char *args, char **sug) {
  int i = 0, len = strlen(args);
  struct setting *s;
  for(s=settings; i<20 && s->name; s++)
    if(strncmp(s->name, args, len) == 0 && strlen(s->name) != len)
      sug[i++] = g_strdup(s->name);
}


void c_oset_sug(char *args, char **sug) {
  char *sep = strchr(args, ' ');
  if(!sep)
    c_oset_sugkey(args, sug);
  else {
    *sep = 0;
    char *pre = g_strdup(args);

    // Get group and key
    char *key;
    guint64 hub;
    struct setting *s;
    gboolean checkalt;
    if(parsesetting(pre, &hub, &key, &s, &checkalt)) {
      if(checkalt && !conf_exists(hub, key))
        hub = 0;

      if(s->suggest) {
        s->suggest(hub, key, sep+1, sug);
        strv_prefix(sug, args, " ", NULL);
      }
    }
    g_free(pre);
  }
}


void c_help_oset(char *args) {
  struct setting *s = getsetting(args);
  struct doc_set *d = s ? getdoc(s) : NULL;
  if(!s)
    ui_mf(NULL, 0, "\nUnknown setting `%s'.", args);
  else if(!d)
    ui_mf(NULL, 0, "\nNo documentation available for %s.", args);
  else
    ui_mf(NULL, 0, "\nSetting: %s.%s %s\n\n%s\n", d->hub ? "#hub" : "global", s->name, d->type, d->desc);
}

