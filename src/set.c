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


// set options
struct setting {
  char *name;
  char *group;      // NULL = hub name or "global" on non-hub-tabs
  void (*get)(char *, char *);
  void (*set)(char *, char *, char *);
  void (*suggest)(char *, char *, char *, char **);
  struct doc_set *doc;
};



static void get_string(char *group, char *key) {
  GError *err = NULL;
  char *str = g_key_file_get_string(conf_file, group, key, &err);
  if(!str) {
    ui_mf(NULL, 0, "%s.%s is not set.", group, key);
    g_error_free(err);
  } else {
    ui_mf(NULL, 0, "%s.%s = %s", group, key, str);
    g_free(str);
  }
}


// not set => false
static void get_bool_f(char *group, char *key) {
  ui_mf(NULL, 0, "%s.%s = %s", group, key, g_key_file_get_boolean(conf_file, group, key, NULL) ? "true" : "false");
}


// not set => true
static void get_bool_t(char *group, char *key) {
  ui_mf(NULL, 0, "%s.%s = %s", group, key,
    !g_key_file_has_key(conf_file, group, key, NULL) || g_key_file_get_boolean(conf_file, group, key, NULL) ? "true" : "false");
}


static void get_int(char *group, char *key) {
  GError *err = NULL;
  int val = g_key_file_get_integer(conf_file, group, key, &err);
  if(err) {
    ui_mf(NULL, 0, "%s.%s is not set.", group, key);
    g_error_free(err);
  } else
    ui_mf(NULL, 0, "%s.%s = %d", group, key, val);
}


static gboolean bool_var(const char *val) {
  if(strcmp(val, "1") == 0 || strcmp(val, "t") == 0 || strcmp(val, "y") == 0
      || strcmp(val, "true") == 0 || strcmp(val, "yes") == 0 || strcmp(val, "on") == 0)
    return TRUE;
  return FALSE;
}


#define UNSET(group, key) do {\
    g_key_file_remove_key(conf_file, group, key, NULL);\
    conf_save();\
    ui_mf(NULL, 0, "%s.%s reset.", group, key);\
  } while(0)


static void set_nick(char *group, char *key, char *val) {
  if(!val) {
    if(strcmp(group, "global") == 0) {
      ui_m(NULL, 0, "global.nick may not be unset.");
      return;
    }
    UNSET(group, key);
    return;
  }
  if(strlen(val) > 32) {
    ui_m(NULL, 0, "Too long nick name.");
    return;
  }
  int i;
  for(i=strlen(val)-1; i>=0; i--)
    if(val[i] == '$' || val[i] == '|' || val[i] == ' ' || val[i] == '<' || val[i] == '>')
      break;
  if(i >= 0) {
    ui_m(NULL, 0, "Invalid character in nick name.");
    return;
  }
  g_key_file_set_string(conf_file, group, key, val);
  conf_save();
  get_string(group, key);
  ui_m(NULL, 0, "Your new nick will be used for new hub connections.");
  // TODO: nick change without reconnect on ADC?
}


// set email/description/connection info
static void set_userinfo(char *group, char *key, char *val) {
  if(!val)
    UNSET(group, key);
  else {
    g_key_file_set_string(conf_file, group, key, val);
    conf_save();
    get_string(group, key);
  }
  hub_global_nfochange();
}


static void get_encoding(char *group, char *key) {
  char *enc = conf_encoding(group);
  ui_mf(NULL, 0, "%s.%s = %s", group, key, enc);
  g_free(enc);
}


static void set_encoding(char *group, char *key, char *val) {
  GError *err = NULL;
  if(!val)
    UNSET(group, key);
  else if(!str_convert_check(val, &err)) {
    if(err) {
      ui_mf(NULL, 0, "ERROR: Can't use that encoding: %s", err->message);
      g_error_free(err);
    } else {
      ui_m(NULL, 0, "ERROR: Invalid encoding.");
    }
  } else {
    g_key_file_set_string(conf_file, group, key, val);
    conf_save();
    get_encoding(group, key);
  }
  // TODO: note that this only affects new incoming data? and that a reconnect
  // may be necessary to re-convert all names/descriptions/stuff?
}


static void set_encoding_sug(char *group, char *key, char *val, char **sug) {
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
static void set_bool_f(char *group, char *key, char *val) {
  if(!val)
    UNSET(group, key);
  else {
    g_key_file_set_boolean(conf_file, group, key, bool_var(val));
    conf_save();
    get_bool_f(group, key);
  }
}


static void set_bool_t(char *group, char *key, char *val) {
  if(!val)
    UNSET(group, key);
  else {
    g_key_file_set_boolean(conf_file, group, key, bool_var(val));
    conf_save();
    get_bool_t(group, key);
  }
}


// Only suggests "true" or "false" regardless of the input. There are only two
// states anyway, and one would want to switch between those two without any
// hassle.
static void set_bool_sug(char *group, char *key, char *val, char **sug) {
  gboolean f = !(val[0] == 0 || val[0] == '1' || val[0] == 't' || val[0] == 'y' || val[0] == 'o');
  sug[f ? 1 : 0] = g_strdup("true");
  sug[f ? 0 : 1] = g_strdup("false");
}


static void set_autoconnect(char *group, char *key, char *val) {
  if(strcmp(group, "global") == 0 || group[0] != '#')
    ui_m(NULL, 0, "ERROR: autoconnect can only be used as hub setting.");
  else
    set_bool_f(group, key, val);
}


static void set_active(char *group, char *key, char *val) {
  if(!val)
    UNSET(group, key);
  else if(bool_var(val) && !g_key_file_has_key(conf_file, "global", "active_ip", NULL)) {
    ui_m(NULL, 0, "ERROR: No IP address set. Please use `/set active_ip <your_ip>' first.");
    return;
  }
  set_bool_f(group, key, val);
  cc_listen_start();
}


static void set_active_ip(char *group, char *key, char *val) {
  if(!val) {
    UNSET(group, key);
    set_active(group, key, NULL);
  }
  // TODO: IPv6?
  if(!g_regex_match_simple("^[0-9]{1,3}\\.[0-9]{1,3}\\.[0-9]{1,3}\\.[0-9]{1,3}$", val, 0, 0)
      || strncmp("127.", val, 4) == 0 || strncmp("0.", val, 2) == 0) {
    ui_m(NULL, 0, "ERROR: Invalid IP.");
    return;
  }
  g_key_file_set_string(conf_file, group, key, val);
  conf_save();
  get_string(group, key);
  cc_listen_start();
}


static void set_active_port(char *group, char *key, char *val) {
  long v = -1;
  if(!val)
    UNSET(group, key);
  else {
    v = strtol(val, NULL, 10);
    if((!v && errno == EINVAL) || v < 0 || v > 65535)
      ui_m(NULL, 0, "Invalid port number.");
    g_key_file_set_integer(conf_file, group, key, v);
    conf_save();
    get_int(group, key);
  }
  cc_listen_start();
}


static void get_autorefresh(char *group, char *key) {
  int a = conf_autorefresh();
  ui_mf(NULL, 0, "%s.%s = %d%s", group, key, a, !a ? " (disabled)" : "");
}


static void set_autorefresh(char *group, char *key, char *val) {
  if(!val)
    UNSET(group, key);
  else {
    long v = strtol(val, NULL, 10);
    if((!v && errno == EINVAL) || v < INT_MIN || v > INT_MAX || v < 0)
      ui_m(NULL, 0, "Invalid number.");
    else if(v > 0 && v < 10)
      ui_m(NULL, 0, "Interval between automatic refreshes should be at least 10 minutes.");
    else {
      g_key_file_set_integer(conf_file, group, key, v);
      conf_save();
      get_autorefresh(group, key);
    }
  }
}


static void get_slots(char *group, char *key) {
  ui_mf(NULL, 0, "%s.%s = %d", group, key, conf_slots());
}


static void set_slots(char *group, char *key, char *val) {
  if(!val)
    UNSET(group, key);
  else {
    long v = strtol(val, NULL, 10);
    if((!v && errno == EINVAL) || v < INT_MIN || v > INT_MAX || v < 0)
      ui_m(NULL, 0, "Invalid number.");
    else {
      g_key_file_set_integer(conf_file, group, key, v);
      conf_save();
      get_slots(group, key);
      hub_global_nfochange();
    }
  }
}


static void get_minislot_size(char *group, char *key) {
  ui_mf(NULL, 0, "%s.%s = %d KiB", group, key, conf_minislot_size() / 1024);
}


static void set_minislot_size(char *group, char *key, char *val) {
  if(!val)
    UNSET(group, key);
  else {
    long v = strtol(val, NULL, 10);
    if((!v && errno == EINVAL) || v < INT_MIN || v > INT_MAX/1024 || v < 0)
      ui_m(NULL, 0, "Invalid number.");
    else if(v < 64)
      ui_m(NULL, 0, "Minislot size must be at least 64 KiB.");
    else {
      g_key_file_set_integer(conf_file, group, key, v);
      conf_save();
      get_minislot_size(group, key);
    }
  }
}


static void get_minislots(char *group, char *key) {
  ui_mf(NULL, 0, "%s.%s = %d", group, key, conf_minislots());
}


static void set_minislots(char *group, char *key, char *val) {
  if(!val)
    UNSET(group, key);
  else {
    long v = strtol(val, NULL, 10);
    if((!v && errno == EINVAL) || v < INT_MIN || v > INT_MAX || v < 0)
      ui_m(NULL, 0, "Invalid number.");
    else if(v < 1)
      ui_m(NULL, 0, "You must have at least 1 minislot.");
    else {
      g_key_file_set_integer(conf_file, group, key, v);
      conf_save();
      get_minislots(group, key);
    }
  }
}


static void get_password(char *group, char *key) {
  ui_mf(NULL, 0, "%s.%s is %s", group, key, g_key_file_has_key(conf_file, group, key, NULL) ? "set" : "not set");
}


static void set_password(char *group, char *key, char *val) {
  if(strcmp(group, "global") == 0 || group[0] != '#')
    ui_m(NULL, 0, "ERROR: password can only be used as hub setting.");
  else if(!val)
    UNSET(group, key);
  else {
    g_key_file_set_string(conf_file, group, key, val);
    conf_save();
    struct ui_tab *tab = ui_tab_cur->data;
    if(tab->type == UIT_HUB && tab->hub->net->conn && !tab->hub->nick_valid)
      hub_password(tab->hub, NULL);
    ui_m(NULL, 0, "Password saved.");
  }
}


// a bit pointless, but for consistency's sake
static void get_hubname(char *group, char *key) {
  if(group[0] != '#')
    ui_mf(NULL, 0, "%s.%s is not set.", group, key);
  else
    ui_mf(NULL, 0, "%s.%s = %s", group, key, group+1);
}


static void set_hubname(char *group, char *key, char *val) {
  if(group[0] != '#')
    ui_mf(NULL, 0, "ERROR: hubname can only be used as hub setting.");
  else if(!val[0])
    ui_mf(NULL, 0, "%s.%s may not be unset.", group, key);
  else {
    if(val[0] == '#')
      val++;
    char *g = g_strdup_printf("#%s", val);
    if(!is_valid_hubname(val))
      ui_mf(NULL, 0, "Invalid name.");
    else if(g_key_file_has_group(conf_file, g))
      ui_mf(NULL, 0, "Name already used.");
    else {
      conf_group_rename(group, g);
      GList *n;
      for(n=ui_tabs; n; n=n->next) {
        struct ui_tab *t = n->data;
        if(t->type == UIT_HUB && strcmp(t->name, group) == 0) {
          g_free(t->name);
          t->name = g_strdup(g);
        }
      }
      get_hubname(g, key);
    }
    g_free(g);
  }
}


static void set_hubname_sug(char *group, char *key, char *val, char **sug) {
  if(group && *group == '#')
    sug[0] = g_strdup(group);
}


static void get_download_dir(char *group, char *key) {
  char *d = conf_download_dir();
  ui_mf(NULL, 0, "%s.%s = %s", group, key, d);
  g_free(d);
}


static void set_download_dir(char *group, char *key, char *val) {
  char *nval = val ? g_strdup(val) : g_build_filename(conf_dir, "dl", NULL);
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
    struct stat inc, dest;
    char *incd = g_build_filename(conf_dir, "inc", NULL);
    if(stat(incd, &inc) < 0) {
      ui_mf(NULL, 0, "Error stat'ing %s: %s", incd, g_strerror(errno));
      cont = FALSE;
    }
    g_free(incd);
    if(cont && stat(nval, &dest) < 0) {
      ui_mf(NULL, 0, "Error stat'ing %s: %s", nval, g_strerror(errno));
      cont = FALSE;
    }
    if(cont && inc.st_dev != dest.st_dev)
      warn = TRUE;
  }
  // no errors? save.
  if(cont) {
    if(!val)
      UNSET(group, key);
    else {
      g_key_file_set_string(conf_file, group, key, val);
      get_download_dir(group, key);
      conf_save();
    }
  }
  if(warn)
    ui_m(NULL, 0, "WARNING: The download directory is not on the same filesystem as the incoming"
                  " directory. This may cause the program to hang when downloading large files.");
  g_free(nval);
}


static void get_download_slots(char *group, char *key) {
  ui_mf(NULL, 0, "%s.%s = %d", group, key, conf_download_slots());
}


static void set_download_slots(char *group, char *key, char *val) {
  int oldval = conf_download_slots();
  if(!val)
    UNSET(group, key);
  else {
    long v = strtol(val, NULL, 10);
    if((!v && errno == EINVAL) || v < INT_MIN || v > INT_MAX || v <= 0)
      ui_m(NULL, 0, "Invalid number.");
    else {
      g_key_file_set_integer(conf_file, group, key, v);
      conf_save();
      get_download_slots(group, key);
    }
  }
  if(conf_download_slots() > oldval)
    dl_queue_startany();
}


static void get_backlog(char *group, char *key) {
  int n = g_key_file_get_integer(conf_file, group, key, NULL);
  ui_mf(NULL, 0, "%s.%s = %d%s", group, key, n, !n ? " (disabled)" : "");
}


static void set_backlog(char *group, char *key, char *val) {
  if(!val)
    UNSET(group, key);
  else {
    long v = strtol(val, NULL, 10);
    if((!v && errno == EINVAL) || v < INT_MIN || v > INT_MAX || v < 0)
      ui_m(NULL, 0, "Invalid number.");
    else if(v >= LOGWIN_BUF)
      ui_mf(NULL, 0, "Maximum value is %d.", LOGWIN_BUF-1);
    else {
      g_key_file_set_integer(conf_file, group, key, v);
      conf_save();
      get_backlog(group, key);
    }
  }
}


static void get_color(char *group, char *key) {
  g_return_if_fail(strncmp(key, "color_", 6) == 0);
  struct ui_color *c = ui_color_by_name(key+6);
  g_return_if_fail(c);
  ui_mf(NULL, 0, "%s.%s = %s", group, key, ui_color_str_gen(c->fg, c->bg, c->x));
}


static void set_color(char *group, char *key, char *val) {
  if(!val) {
    UNSET(group, key);
    ui_colors_update();
  } else {
    GError *err = NULL;
    if(!ui_color_str_parse(val, NULL, NULL, NULL, &err)) {
      ui_m(NULL, 0, err->message);
      g_error_free(err);
    } else {
      g_key_file_set_string(conf_file, group, key, val);
      conf_save();
      ui_colors_update();
      get_color(group, key);
    }
  }
}


static void set_color_sug(char *group, char *key, char *val, char **sug) {
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


static void get_tls_policy(char *group, char *key) {
  ui_mf(NULL, 0, "%s.%s = %s%s", group, key,
    conf_tlsp_list[conf_tls_policy(group)], conf_certificate ? "" : " (not supported)");
}


static void set_tls_policy(char *group, char *key, char *val) {
  int old = conf_tls_policy(group);
  if(!val)
    UNSET(group, key);
  else if(!conf_certificate)
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
      g_key_file_set_integer(conf_file, group, key, p);
      conf_save();
      get_tls_policy(group, key);
    }
  }
  if(old != conf_tls_policy(group))
    hub_global_nfochange();
}


static void set_tls_policy_sug(char *group, char *key, char *val, char **sug) {
  int i = 0, j = 0, len = strlen(val);
  for(i=0; i<=2; i++)
    if(g_ascii_strncasecmp(val, conf_tlsp_list[i], len) == 0 && strlen(conf_tlsp_list[i]) != len)
      sug[j++] = g_strdup(conf_tlsp_list[i]);
}


static void set_regex(char *group, char *key, char *val) {
  if(!val) {
    UNSET(group, key);
    return;
  }
  GError *err = NULL;
  GRegex *r = g_regex_new(val, 0, 0, &err);
  if(!r) {
    ui_m(NULL, 0, err->message);
    g_error_free(err);
  } else {
    g_regex_unref(r);
    g_key_file_set_string(conf_file, group, key, val);
    conf_save();
    get_string(group, key);
  }
}


static void set_path_sug(char *group, char *key, char *val, char **sug) {
  path_suggest(val, sug);
}


// Suggest the current value. Works for both integers and strings. Perhaps also
// for booleans, but set_bool_sug() is more useful there anyway.
// BUG: This does not use a default value, if there is one...
static void set_old_sug(char *group, char *key, char *val, char **sug) {
  char *old = g_key_file_get_string(conf_file, group, key, NULL);
  sug[0] = old;
}


// the settings list
static struct setting settings[] = {
  { "active",           "global", get_bool_f,        set_active,        NULL               },
  { "active_ip",        "global", get_string,        set_active_ip,     set_old_sug        },
  { "active_port",      "global", get_int,           set_active_port,   NULL,              },
  { "autoconnect",      NULL,     get_bool_f,        set_autoconnect,   set_bool_sug       },
  { "autorefresh",      "global", get_autorefresh,   set_autorefresh,   NULL               },
  { "backlog",          NULL,     get_backlog,       set_backlog,       NULL,              },
#define C(n, a,b,c) { "color_" G_STRINGIFY(n), "color", get_color, set_color, set_color_sug },
  UI_COLORS
#undef C
  { "connection",       NULL,     get_string,        set_userinfo,      set_old_sug        },
  { "description",      NULL,     get_string,        set_userinfo,      set_old_sug        },
  { "download_dir",     "global", get_download_dir,  set_download_dir,  set_path_sug       },
  { "download_slots",   "global", get_download_slots,set_download_slots,NULL               },
  { "download_exclude", "global", get_string,        set_regex,         set_old_sug        },
  { "email",            NULL,     get_string,        set_userinfo,      set_old_sug        },
  { "encoding",         NULL,     get_encoding,      set_encoding,      set_encoding_sug   },
  { "hubname",          NULL,     get_hubname,       set_hubname,       set_hubname_sug    },
  { "log_debug",        "log",    get_bool_f,        set_bool_f,        set_bool_sug       },
  { "log_downloads",    "log",    get_bool_t,        set_bool_t,        set_bool_sug       },
  { "log_uploads",      "log",    get_bool_t,        set_bool_t,        set_bool_sug       },
  { "minislots",        "global", get_minislots,     set_minislots,     NULL               },
  { "minislot_size",    "global", get_minislot_size, set_minislot_size, NULL               },
  { "nick",             NULL,     get_string,        set_nick,          set_old_sug        },
  { "password",         NULL,     get_password,      set_password,      NULL               },
  { "share_hidden",     "global", get_bool_f,        set_bool_f,        set_bool_sug       },
  { "share_exclude",    "global", get_string,        set_regex,         set_old_sug        },
  { "show_joinquit",    NULL,     get_bool_f,        set_bool_f,        set_bool_sug       },
  { "slots",            "global", get_slots,         set_slots,         NULL               },
  { "tls_policy",       NULL,     get_tls_policy,    set_tls_policy,    set_tls_policy_sug },
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
    ui_mf(NULL, 0, "No configuration variable with the name '%s'.", *key);
    return FALSE;
  }
  if(*group && (
      ((*s)->group && strcmp(*group, (*s)->group) != 0) ||
      (!(*s)->group && !g_key_file_has_group(conf_file, *group)))) {
    ui_m(NULL, 0, "Wrong configuration group.");
    return FALSE;
  }
  if(!*group)
    *group = (*s)->group;
  if(!*group) {
    struct ui_tab *tab = ui_tab_cur->data;
    if(tab->type == UIT_HUB) {
      *checkalt = TRUE;
      *group = tab->name;
    } else
      *group = "global";
  }
  return TRUE;
}


void c_set(char *args) {
  if(!args[0]) {
    struct setting *s;
    ui_m(NULL, 0, "");
    for(s=settings; s->name; s++)
      c_set(s->name);
    ui_m(NULL, 0, "");
    return;
  }

  char *key;
  char *group; // NULL = figure out automatically
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

  // get group and key
  if(!parsesetting(args, &group, &key, &s, &checkalt))
    return;

  // get
  if(!val || !val[0]) {
    if(checkalt && !g_key_file_has_key(conf_file, group, key, NULL) && strcmp(key, "hubname") != 0)
      group = "global";
    s->get(group, key);

  // set
  } else {
    s->set(group, key, val);
    // set() may not always modify the config, but let's just save anyway
    conf_save();
  }
}


void c_unset(char *args) {
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

  if(checkalt && !g_key_file_has_key(conf_file, group, key, NULL) && strcmp(key, "hubname") != 0)
    group = "global";
  s->set(group, key, NULL);
  conf_save();
}


// Doesn't provide suggestions for group prefixes (e.g. global.<stuff> or
// #hub.<stuff>), but I don't think that'll be used very often anyway.
void c_set_sugkey(char *args, char **sug) {
  int i = 0, len = strlen(args);
  struct setting *s;
  for(s=settings; i<20 && s->name; s++)
    if(strncmp(s->name, args, len) == 0 && strlen(s->name) != len)
      sug[i++] = g_strdup(s->name);
}


void c_set_sug(char *args, char **sug) {
  char *sep = strchr(args, ' ');
  if(!sep)
    c_set_sugkey(args, sug);
  else {
    *sep = 0;
    char *pre = g_strdup(args);

    // Get group and key
    char *key, *group;
    struct setting *s;
    gboolean checkalt;
    if(parsesetting(pre, &group, &key, &s, &checkalt)) {
      if(checkalt && !g_key_file_has_key(conf_file, group, key, NULL) && strcmp(key, "hubname") != 0)
        group = "global";

      s->suggest(group, key, sep+1, sug);
      strv_prefix(sug, args, " ", NULL);
    }
    g_free(pre);
  }
}


void c_help_set(char *args) {
  struct setting *s = getsetting(args);
  struct doc_set *d = s ? getdoc(s) : NULL;
  if(!s)
    ui_mf(NULL, 0, "\nUnknown setting `%s'.", args);
  else if(!d)
    ui_mf(NULL, 0, "\nNo documentation available for %s.", args);
  else
    ui_mf(NULL, 0, "\nSetting: %s.%s %s\n\n%s\n", !s->group ? "#hub" : s->group, s->name, d->type, d->desc);
}

