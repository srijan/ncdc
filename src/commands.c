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


// currently opened tab, see cmd_handle()
static struct ui_tab *tab;

struct cmd {
  char name[16];
  void (*f)(char *);
  void (*suggest)(char *, char **);
  char *args;
  char *sum;
  char *desc;
};
// tentative definition of the cmd list
static struct cmd cmds[];



// set options
struct setting {
  char *name;
  char *group;      // NULL = hub name or "global" on non-hub-tabs
  void (*get)(char *, char *);
  void (*set)(char *, char *, char *);
  void (*suggest)(char *, char **);
};


static gboolean is_valid_hubname(char *name) {
  char *tmp;
  int len = 0;
  for(tmp=name; *tmp; tmp = g_utf8_next_char(tmp))
    if(++len && !g_unichar_isalnum(g_utf8_get_char(tmp)))
      break;
  return !*tmp && len && len <= 25;
}


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


static void set_encoding_sug(char *val, char **sug) {
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
static void set_bool_sug(char *val, char **sug) {
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


static void set_color_sug(char *val, char **sug) {
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
    conf_tlsp_list[conf_tls_policy(group)], have_tls_support ? "" : " (not supported)");
}


static void set_tls_policy(char *group, char *key, char *val) {
  int old = conf_tls_policy(group);
  if(!val)
    UNSET(group, key);
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


static void set_tls_policy_sug(char *val, char **sug) {
  int i = 0, j = 0, len = strlen(val);
  for(i=0; i<=2; i++)
    if(g_ascii_strncasecmp(val, conf_tlsp_list[i], len) == 0 && strlen(conf_tlsp_list[i]) != len)
      sug[j++] = g_strdup(conf_tlsp_list[i]);
}


// the settings list
// TODO: help text / documentation?
static struct setting settings[] = {
  { "active",        "global", get_bool_f,        set_active,        NULL               },
  { "active_ip",     "global", get_string,        set_active_ip,     NULL               },
  { "active_port",   "global", get_int,           set_active_port,   NULL,              },
  { "autoconnect",   NULL,     get_bool_f,        set_autoconnect,   set_bool_sug       }, // may not be used in "global"
  { "autorefresh",   "global", get_autorefresh,   set_autorefresh,   NULL               }, // in minutes, 0 = disabled
  { "backlog",       NULL,     get_backlog,       set_backlog,       NULL,              }, // number of lines, 0 = disabled
#define C(n, a,b,c) { "color_" G_STRINGIFY(n), "color", get_color, set_color, set_color_sug },
  UI_COLORS
#undef C
  { "connection",    NULL,     get_string,        set_userinfo,      NULL               },
  { "description",   NULL,     get_string,        set_userinfo,      NULL               },
  { "download_dir",  "global", get_download_dir,  set_download_dir,  path_suggest       },
  { "download_slots","global", get_download_slots,set_download_slots,NULL,              },
  { "email",         NULL,     get_string,        set_userinfo,      NULL               },
  { "encoding",      NULL,     get_encoding,      set_encoding,      set_encoding_sug   },
  { "hubname",       NULL,     get_hubname,       set_hubname,       NULL               }, // makes no sense in "global"
  { "log_debug",     "log",    get_bool_f,        set_bool_f,        set_bool_sug       },
  { "log_downloads", "log",    get_bool_t,        set_bool_t,        set_bool_sug       },
  { "log_uploads",   "log",    get_bool_t,        set_bool_t,        set_bool_sug       },
  { "minislots",     "global", get_minislots,     set_minislots,     NULL               },
  { "minislot_size", "global", get_minislot_size, set_minislot_size, NULL               },
  { "nick",          NULL,     get_string,        set_nick,          NULL               }, // global.nick may not be /unset
  { "password",      NULL,     get_password,      set_password,      NULL               }, // may not be used in "global" (obviously)
  { "share_hidden",  "global", get_bool_f,        set_bool_f,        set_bool_sug       },
  { "show_joinquit", NULL,     get_bool_f,        set_bool_f,        set_bool_sug       },
  { "slots",         "global", get_slots,         set_slots,         NULL               },
  { "tls_policy",    NULL,     get_tls_policy,    set_tls_policy,    set_tls_policy_sug },
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

  if(checkalt && !g_key_file_has_key(conf_file, group, key, NULL) && strcmp(key, "hubname") != 0)
    group = "global";
  s->set(group, key, NULL);
  conf_save();
}


// Doesn't provide suggestions for group prefixes (e.g. global.<stuff> or
// #hub.<stuff>), but I don't think that'll be used very often anyway.
static void c_set_sugkey(char *args, char **sug) {
  int i = 0, len = strlen(args);
  struct setting *s;
  for(s=settings; i<20 && s->name; s++)
    if(strncmp(s->name, args, len) == 0 && strlen(s->name) != len)
      sug[i++] = g_strdup(s->name);
}


static void c_set_sug(char *args, char **sug) {
  char *sep = strchr(args, ' ');
  if(!sep)
    c_set_sugkey(args, sug);
  else {
    *sep = 0;
    struct setting *s = getsetting(args);
    if(s && s->suggest) {
      s->suggest(sep+1, sug);
      strv_prefix(sug, args, " ", NULL);
    }
  }
}





// get a command by name. performs a linear search. can be rewritten to use a
// binary search, but I doubt the performance difference really matters.
static struct cmd *getcmd(const char *name) {
  struct cmd *c;
  for(c=cmds; c->f; c++)
    if(strcmp(c->name, name) == 0)
      break;
  return c->f ? c : NULL;
}


static void c_quit(char *args) {
  ncdc_quit();
}


// handle /say and /me
static void sayme(char *args, gboolean me) {
  if(tab->type != UIT_HUB && tab->type != UIT_MSG)
    ui_m(NULL, 0, "This command can only be used on hub and message tabs.");
  else if(!tab->hub->nick_valid)
    ui_m(NULL, 0, "Not connected or logged in yet.");
  else if(!args[0])
    ui_m(NULL, 0, "Message empty.");
  else if(tab->type == UIT_HUB)
    hub_say(tab->hub, args, me);
  else {
    struct hub_user *u = g_hash_table_lookup(hub_uids, &tab->uid);
    if(!u)
      ui_m(NULL, 0, "User is not online.");
    else
      hub_msg(tab->hub, u, args, me);
  }
}


static void c_say(char *args) {
  sayme(args, FALSE);
}


static void c_me(char *args) {
  sayme(args, TRUE);
}


static void c_msg(char *args) {
  char *sep = strchr(args, ' ');
  if(sep) {
    *sep = 0;
    while(*(++sep) == ' ');
  }
  if(tab->type != UIT_HUB && tab->type != UIT_MSG)
    ui_m(NULL, 0, "This command can only be used on hub and message tabs.");
  else if(!tab->hub->nick_valid)
    ui_m(NULL, 0, "Not connected or logged in yet.");
  else if(!args[0])
    ui_m(NULL, 0, "No user specified. See `/help msg' for more information.");
  else {
    struct hub_user *u = hub_user_get(tab->hub, args);
    if(!u)
      ui_m(NULL, 0, "No user found with that name. Note that usernames are case-sensitive.");
    else {
      // get or open tab and make sure it's selected
      struct ui_tab *t = ui_hub_getmsg(tab, u);
      if(!t)
        ui_tab_open(ui_msg_create(tab->hub, u), TRUE);
      else
        ui_tab_cur = g_list_find(ui_tabs, t);
      // if we need to send something, do so
      if(sep && *sep)
        hub_msg(tab->hub, u, sep, FALSE);
    }
  }
}


static void c_help(char *args) {
  struct cmd *c;
  // list available commands
  if(!args[0]) {
    ui_m(NULL, 0, "\nAvailable commands:");
    for(c=cmds; c->f; c++)
      ui_mf(NULL, 0, " /%s - %s", c->name, c->sum);
    ui_m(NULL, 0, "");

  // list information on a particular command
  } else {
    c = getcmd(args);
    if(!c)
      ui_mf(NULL, 0, "\nUnknown command '%s'.", args);
    else
      ui_mf(NULL, 0, "\nUsage: /%s %s\n  %s\n\n%s\n", c->name, c->args ? c->args : "", c->sum, c->desc);
  }
}


static void c_help_sug(char *args, char **sug) {
  int i = 0, len = strlen(args);
  struct cmd *c;
  for(c=cmds; i<20 && c->f; c++)
    if(strncmp(c->name, args, len) == 0 && strlen(c->name) != len)
      sug[i++] = g_strdup(c->name);
}


static void c_connect_set_hubaddr(char *addr) {
  // make sure it's a full address in the form of "dchub://hostname:port/"
  // (doesn't check for validness)
  GString *a = g_string_new(addr);
  if(strncmp(a->str, "dchub://", 8) != 0 && strncmp(a->str, "nmdc://", 7) != 0 && strncmp(a->str, "nmdcs://", 8) != 0
      && strncmp(a->str, "adcs://", 7) != 0 && strncmp(a->str, "adc://", 6) != 0)
    g_string_prepend(a, "dchub://");
  if(a->str[a->len-1] != '/')
    g_string_append_c(a, '/');
  g_key_file_set_string(conf_file, tab->name, "hubaddr", a->str);
  conf_save();
  g_string_free(a, TRUE);
}


static void c_connect(char *args) {
  if(tab->type != UIT_HUB)
    ui_m(NULL, 0, "This command can only be used on hub tabs.");
  else if(tab->hub->net->connecting || tab->hub->net->conn)
    ui_m(NULL, 0, "Already connected (or connecting). You may want to /disconnect first.");
  else {
    if(args[0])
      c_connect_set_hubaddr(args);
    if(!g_key_file_has_key(conf_file, tab->name, "hubaddr", NULL))
      ui_m(NULL, 0, "No hub address configured. Use '/connect <address>' to do so.");
    else
      hub_connect(tab->hub);
  }
}


// only autocompletes "dchub://" or the hubaddr, when set
static void c_connect_sug(char *args, char **sug) {
  struct ui_tab *t = ui_tab_cur->data;
  if(t->type != UIT_HUB)
    return;
  int i = 0, len = strlen(args);
  char *addr = g_key_file_get_string(conf_file, t->name, "hubaddr", NULL);
  if(addr && strncmp(addr, args, len) == 0)
    sug[i++] = g_strdup(addr);
  else if(addr) {
    char *naddr = g_strconcat("dchub://", addr, "/", NULL);
    if(strncmp(naddr, args, len) == 0)
      sug[i++] = naddr;
    else
      g_free(naddr);
  }
  if(strncmp("dchub://", args, len) == 0)
    sug[i++] = g_strdup("dchub://");
  g_free(addr);
}


static void c_disconnect(char *args) {
  if(args[0])
    ui_m(NULL, 0, "This command does not accept any arguments.");
  else if(tab->type != UIT_HUB)
    ui_m(NULL, 0, "This command can only be used on hub tabs.");
  else if(!tab->hub->net->conn && !tab->hub->reconnect_timer && !tab->hub->net->connecting)
    ui_m(NULL, 0, "Not connected.");
  else
    hub_disconnect(tab->hub, FALSE);
}


static void c_reconnect(char *args) {
  if(args[0])
    ui_m(NULL, 0, "This command does not accept any arguments.");
  else if(tab->type != UIT_HUB)
    ui_m(NULL, 0, "This command can only be used on hub tabs.");
  else {
    if(tab->hub->net->conn || tab->hub->net->connecting || tab->hub->reconnect_timer)
      hub_disconnect(tab->hub, FALSE);
    c_connect(""); // also checks for the existence of "hubaddr"
  }
}


static void c_open(char *args) {
  gboolean conn = TRUE;
  if(strncmp(args, "-n ", 3) == 0) {
    conn = FALSE;
    args += 3;
    g_strstrip(args);
  }
  char *name = args, *addr = strchr(args, ' ');
  if(name[0] == '#')
    name++;
  if(addr)
    *(addr++) = 0;
  if(!name[0]) {
    ui_m(NULL, 0, "No hub name given.");
    return;
  }
  if(!is_valid_hubname(name))
    ui_m(NULL, 0, "Sorry, tab name may only consist of alphanumeric characters, and must not exceed 25 characters.");
  else {
    // Look for existing tab
    GList *n;
    for(n=ui_tabs; n; n=n->next) {
      char *tmp = ((struct ui_tab *)n->data)->name;
      if(tmp[0] == '#' && strcmp(tmp+1, name) == 0)
        break;
    }
    // Open or select tab
    if(!n) {
      tab = ui_hub_create(name, addr ? FALSE : conn);
      ui_tab_open(tab, TRUE);
    } else if(n != ui_tab_cur) {
      ui_tab_cur = n;
      tab = n->data;
    } else {
      ui_m(NULL, 0, addr ? "Tab already selected, saving new address instead." : "Tab already selected.");
      tab = n->data;
    }
    // Save address and (re)connect when necessary
    if(addr) {
      c_connect_set_hubaddr(addr);
      if(conn)
        c_reconnect("");
    }
  }
}


static void c_open_sug(char *args, char **sug) {
  int len = strlen(args);
  int i = 0;
  char **group, **groups = g_key_file_get_groups(conf_file, NULL);
  for(group=groups; i<20 && *group; group++)
    if(**group == '#' && (strncmp(args, *group, len) == 0 || strncmp(args, *group+1, len) == 0) && strlen(*group) != len)
      sug[i++] = g_strdup(*group);
  g_strfreev(groups);
}


static void c_close(char *args) {
  if(args[0])
    ui_m(NULL, 0, "This command does not accept any arguments.");
  else if(tab->type == UIT_MAIN)
    ui_m(NULL, 0, "Main tab cannot be closed.");
  else if(tab->type == UIT_HUB)
    ui_hub_close(tab);
  else if(tab->type == UIT_USERLIST) {
    ui_tab_cur = g_list_find(ui_tabs, tab->hub->tab);
    ui_userlist_close(tab);
  } else if(tab->type == UIT_MSG) {
    ui_tab_cur = g_list_find(ui_tabs, tab->hub->tab);
    ui_msg_close(tab);
  } else if(tab->type == UIT_CONN)
    ui_conn_close();
  else if(tab->type == UIT_FL) {
    struct hub_user *u = tab->uid ? g_hash_table_lookup(hub_uids, &tab->uid) : NULL;
    if(u)
      ui_tab_cur = g_list_find(ui_tabs, u->hub->tab);
    ui_fl_close(tab);
  } else if(tab->type == UIT_DL)
    ui_dl_close(tab);
  else if(tab->type == UIT_SEARCH) {
    if(tab->hub)
      ui_tab_cur = g_list_find(ui_tabs, tab->hub->tab);
    ui_search_close(tab);
  }
}


static void c_clear(char *args) {
  if(args[0])
    ui_m(NULL, 0, "This command does not accept any arguments.");
  else if(tab->log)
    ui_logwindow_clear(tab->log);
}


static void c_userlist(char *args) {
  if(args[0])
    ui_m(NULL, 0, "This command does not accept any arguments.");
  else if(tab->type != UIT_HUB)
    ui_m(NULL, 0, "This command can only be used on hub tabs.");
  else
    ui_hub_userlist_open(tab);
}


static void listshares() {
  gsize len;
  char **dirs = g_key_file_get_keys(conf_file, "share", &len, NULL);
  if(!dirs || len == 0)
    ui_m(NULL, 0, "Nothing shared.");
  else {
    ui_m(NULL, 0, "");
    char **cur;
    for(cur=dirs; *cur; cur++) {
      char *d = g_key_file_get_string(conf_file, "share", *cur, NULL);
      struct fl_list *fl = fl_list_file(fl_local_list, *cur);
      ui_mf(NULL, 0, " /%s -> %s (%s)", *cur, d, str_formatsize(fl->size));
      g_free(d);
    }
    ui_m(NULL, 0, "");
  }
  if(dirs)
    g_strfreev(dirs);
}


static void c_share(char *args) {
  if(!args[0]) {
    listshares();
    return;
  }

  char *first, *second;
  str_arg2_split(args, &first, &second);
  if(!first || !first[0] || !second || !second[0])
    ui_m(NULL, 0, "Error parsing arguments. See \"/help share\" for details.");
  else if(g_key_file_has_key(conf_file, "share", first, NULL))
    ui_m(NULL, 0, "You have already shared a directory with that name.");
  else {
    char *path = path_expand(second);
    if(!path)
      ui_mf(NULL, 0, "Error obtaining absolute path: %s", g_strerror(errno));
    else if(!g_file_test(path, G_FILE_TEST_IS_DIR))
      ui_m(NULL, 0, "Not a directory.");
    else {
      // Check whether it (or a subdirectory) is already shared
      char **dirs = g_key_file_get_keys(conf_file, "share", NULL, NULL);
      char **dir;
      for(dir=dirs; dirs && *dir; dir++) {
        char *d = g_key_file_get_string(conf_file, "share", *dir, NULL);
        if(strncmp(d, path, MIN(strlen(d), strlen(path))) == 0) {
          g_free(d);
          break;
        }
        g_free(d);
      }
      if(dirs && *dir)
        ui_mf(NULL, 0, "Directory already (partly) shared in /%s", *dir);
      else {
        g_key_file_set_string(conf_file, "share", first, path);
        conf_save();
        fl_share(first);
        ui_mf(NULL, 0, "Added to share: /%s -> %s", first, path);
      }
      if(dirs)
        g_strfreev(dirs);
      free(path);
    }
  }
  g_free(first);
}


static void c_share_sug(char *args, char **sug) {
  char *first, *second;
  str_arg2_split(args, &first, &second);
  g_free(first);
  if(!first || !second)
    return;
  // we want the escaped first part
  first = g_strndup(args, second-args);
  path_suggest(second, sug);
  strv_prefix(sug, first, NULL);
  g_free(first);
}


static void c_unshare(char *args) {
  if(!args[0])
    listshares();
  // otherwise we may crash
  else if(fl_refresh_queue && fl_refresh_queue->head)
    ui_m(NULL, 0, "Sorry, can't remove directories from the share while refreshing.");
  else {
    while(args[0] == '/')
      args++;
    char *path = g_key_file_get_string(conf_file, "share", args, NULL);
    if(!args[0]) {
      g_key_file_remove_group(conf_file, "share", NULL);
      conf_save();
      fl_unshare(NULL);
      ui_m(NULL, 0, "Removed all directories from share.");
    } else if(!path)
      ui_m(NULL, 0, "No shared directory with that name.");
    else {
      g_key_file_remove_key(conf_file, "share", args, NULL);
      conf_save();
      fl_unshare(args);
      ui_mf(NULL, 0, "Directory /%s (%s) removed from share.", args, path);
    }
    g_free(path);
  }
}


static void c_unshare_sug(char *args, char **sug) {
  int len = strlen(args), i = 0;
  if(args[0] == '/')
    args++;
  char **dir, **dirs = g_key_file_get_keys(conf_file, "share", NULL, NULL);
  for(dir=dirs; dir && *dir && i<20; dir++)
    if(strncmp(args, *dir, len) == 0 && strlen(*dir) != len)
      sug[i++] = g_strdup(*dir);
  g_strfreev(dirs);
}


static void c_refresh(char *args) {
  struct fl_list *n = fl_local_from_path(args);
  if(!n)
    ui_mf(NULL, 0, "Directory `%s' not found.", args);
  else
    fl_refresh(n);
}


static void nick_sug(char *args, char **sug, gboolean append) {
  struct ui_tab *t = ui_tab_cur->data;
  if(!t->hub)
    return;
  // get starting point of the nick
  char *nick = args+strlen(args);
  while(nick > args && *(nick-1) != ' ' && *(nick-1) != ',' && *(nick-1) != ':')
    nick--;
  hub_user_suggest(t->hub, nick, sug);
  // optionally append ": " after the nick
  if(append && nick == args) {
    char **n;
    for(n=sug; *n; n++) {
      char *tmp = *n;
      *n = g_strdup_printf("%s: ", tmp);
      g_free(tmp);
    }
  }
  // prefix
  *nick = 0;
  if(*args)
    strv_prefix(sug, args, NULL);
}


// also used for c_me
static void c_say_sug(char *args, char **sug) {
  nick_sug(args, sug, TRUE);
}


// also used for c_whois, c_grant, c_kick and c_browse
static void c_msg_sug(char *args, char **sug) {
  nick_sug(args, sug, FALSE);
}


static void c_version(char *args) {
  if(args[0])
    ui_m(NULL, 0, "This command does not accept any arguments.");
  else
    ui_mf(NULL, 0, "\n%s\n", ncdc_version());
}


static void c_connections(char *args) {
  if(args[0])
    ui_m(NULL, 0, "This command does not accept any arguments.");
  else {
    if(ui_conn)
      ui_tab_cur = g_list_find(ui_tabs, ui_conn);
    else
      ui_tab_open(ui_conn_create(), TRUE);
  }
}


static void c_queue(char *args) {
  if(args[0])
    ui_m(NULL, 0, "This command does not accept any arguments.");
  else {
    if(ui_dl)
      ui_tab_cur = g_list_find(ui_tabs, ui_dl);
    else
      ui_tab_open(ui_dl_create(), TRUE);
  }
}


static void c_gc(char *args) {
  if(args[0])
    ui_m(NULL, 0, "This command does not accept any arguments.");
  else {
    ui_m(NULL, UIM_NOLOG, "Collecting garbage...");
    ui_draw();
    fl_hashdat_gc();
    dl_gc();
    ui_m(NULL, UIM_NOLOG, NULL);
    ui_m(NULL, 0, "Garbage-collection done.");
  }
}


static void c_whois(char *args) {
  struct ui_tab *h = tab;
  char *u = NULL;
  guint64 uid = 0;
  gboolean utf8 = TRUE;
  if(tab->type != UIT_HUB && tab->type != UIT_MSG)
    ui_m(NULL, 0, "This command can only be used on hub and message tabs.");
  else if(!args[0] && tab->type != UIT_MSG)
    ui_m(NULL, 0, "No user specified. See `/help whois' for more information.");
  else if(tab->type == UIT_MSG) {
    h = tab->hub->tab;
    if(args[0])
      u = args;
    else
      uid = tab->uid;
  } else
    u = args;
  if((u || uid) && !ui_hub_finduser(h, uid, u, utf8))
    ui_m(NULL, 0, "No user found with that name.");
}


static void c_grant(char *args) {
  struct hub_user *u = NULL;
  if(tab->type != UIT_HUB && tab->type != UIT_MSG)
    ui_m(NULL, 0, "This command can only be used on hub and message tabs.");
  else if(!args[0] && tab->type != UIT_MSG)
    ui_m(NULL, 0, "No user specified. See `/help grant' for more information.");
  else if(args[0]) {
    u = hub_user_get(tab->hub, args);
    if(!u)
      ui_m(NULL, 0, "No user found with that name.");
  } else {
    u = g_hash_table_lookup(hub_uids, &tab->uid);
    if(!u)
      ui_m(NULL, 0, "User not line.");
  }

  if(u) {
    cc_grant(u);
    ui_m(NULL, 0, "Slot granted.");
  }
}


static void c_password(char *args) {
  if(tab->type != UIT_HUB)
    ui_m(NULL, 0, "This command can only be used on hub tabs.");
  else if(!tab->hub->net->conn)
    ui_m(NULL, 0, "Not connected to a hub. Did you want to use '/set password' instead?");
  else if(tab->hub->nick_valid)
    ui_m(NULL, 0, "Already logged in. Did you want to use '/set password' instead?");
  else
    hub_password(tab->hub, args);
}


static void c_kick(char *args) {
  if(tab->type != UIT_HUB)
    ui_m(NULL, 0, "This command can only be used on hub tabs.");
  else if(!tab->hub->nick_valid)
    ui_m(NULL, 0, "Not connected or logged in yet.");
  else if(!args[0])
    ui_m(NULL, 0, "No user specified.");
  else if(tab->hub->adc)
    ui_m(NULL, 0, "This command only works on NMDC hubs.");
  else {
    struct hub_user *u = hub_user_get(tab->hub, args);
    if(!u)
      ui_m(NULL, 0, "No user found with that name.");
    else
      hub_kick(tab->hub, u);
  }
}


static void c_nick(char *args) {
  // not the most elegant solution, but certainly the most simple.
  char *c = g_strdup_printf("nick %s", args);
  c_set(c);
  g_free(c);
}

static void c_browse(char *args) {
  struct hub_user *u = NULL;
  gboolean force = FALSE;

  if(!args[0] && !fl_local_list) {
    ui_m(NULL, 0, "Nothing shared.");
    return;
  } else if(args[0]) {
    if(tab->type != UIT_HUB && tab->type != UIT_MSG) {
      ui_m(NULL, 0, "This command can only be used on hub and message tabs.");
      return;
    }
    if(strncmp(args, "-f ", 3) == 0) {
      force = TRUE;
      args += 3;
      g_strstrip(args);
    }
    u = hub_user_get(tab->hub, args);
    if(!u) {
      ui_m(NULL, 0, "No user found with that name.");
      return;
    }
  }

  ui_fl_queue(u, force);
}


static void c_search(char *args) {
  // Split arguments
  char **argv;
  int argc;
  GError *err = NULL;
  if(!g_shell_parse_argv(args, &argc, &argv, &err)) {
    ui_mf(NULL, 0, "Error parsing arguments: %s", err->message);
    g_error_free(err);
    return;
  }

  // Create basic query
  gboolean allhubs = FALSE;
  gboolean stoparg = FALSE;
  int qlen = 0;
  struct search_q *q = g_slice_new0(struct search_q);
  q->query = g_new0(char *, argc+1);
  q->type = 1;

  // Loop through arguments. (Later arguments overwrite earlier ones)
  int i;
  for(i=0; i<argc; i++) {
    // query
    if(stoparg || argv[i][0] != '-')
      q->query[qlen++] = g_strdup(argv[i]);
    // --
    else if(strcmp(argv[i], "--") == 0)
      stoparg = TRUE;
    // -hub, -all
    else if(strcmp(argv[i], "-hub") == 0)
      allhubs = FALSE;
    else if(strcmp(argv[i], "-all") == 0)
      allhubs = TRUE;
    // -le, -ge
    else if(strcmp(argv[i], "-le") == 0 || strcmp(argv[i], "-ge") == 0) {
      q->ge = strcmp(argv[i], "-ge") == 0;
      if(++i >= argc) {
        ui_mf(NULL, 0, "Option `%s' expects an argument.", argv[i-1]);
        goto c_search_clean;
      }
      q->size = str_parsesize(argv[i]);
      if(q->size == G_MAXUINT64) {
        ui_mf(NULL, 0, "Invalid size argument for option `%s'.", argv[i-1]);
        goto c_search_clean;
      }
    // -t
    } else if(strcmp(argv[i], "-t") == 0) {
      if(++i >= argc) {
        ui_mf(NULL, 0, "Option `%s' expects an argument.", argv[i-1]);
        goto c_search_clean;
      }
      if('1' <= argv[i][0] && argv[i][0] <= '8' && !argv[i][1])
        q->type = argv[i][0]-'0';
      else if(strcmp(argv[i], "any") == 0)      q->type = 1;
      else if(strcmp(argv[i], "audio") == 0)    q->type = 2;
      else if(strcmp(argv[i], "archive") == 0)  q->type = 3;
      else if(strcmp(argv[i], "doc") == 0)      q->type = 4;
      else if(strcmp(argv[i], "exe") == 0)      q->type = 5;
      else if(strcmp(argv[i], "img") == 0)      q->type = 6;
      else if(strcmp(argv[i], "video") == 0)    q->type = 7;
      else if(strcmp(argv[i], "dir") == 0)      q->type = 8;
      else {
        ui_mf(NULL, 0, "Unknown argument for option `%s'.", argv[i-1]);
        goto c_search_clean;
      }
    // -tth
    } else if(strcmp(argv[i], "-tth") == 0) {
      if(++i >= argc) {
        ui_mf(NULL, 0, "Option `%s' expects an argument.", argv[i-1]);
        goto c_search_clean;
      }
      if(!istth(argv[i])) {
        ui_m(NULL, 0, "Invalid TTH root.");
        goto c_search_clean;
      }
      q->type = 9;
      base32_decode(argv[i], q->tth);
    // oops
    } else {
      ui_mf(NULL, 0, "Unknown option: %s", argv[i]);
      goto c_search_clean;
    }
  }

  // validate & send
  if(!qlen && q->type != 9) {
    ui_m(NULL, 0, "No search query given.");
    goto c_search_clean;
  }
  if(!allhubs) {
    if(tab->type != UIT_HUB && tab->type != UIT_MSG) {
      ui_m(NULL, 0, "This command can only be used on hub tabs. Use the `-all' option to search on all connected hubs.");
      goto c_search_clean;
    }
    if(!tab->hub->nick_valid) {
      ui_m(NULL, 0, "Not connected");
      goto c_search_clean;
    }
    hub_search(tab->hub, q);
  }
  if(allhubs) {
    GList *n;
    gboolean one = FALSE;
    for(n=ui_tabs; n; n=n->next) {
      struct ui_tab *t = n->data;
      if(t->type == UIT_HUB && t->hub->nick_valid) {
        hub_search(t->hub, q);
        one = TRUE;
      }
    }
    if(!one) {
      ui_m(NULL, 0, "Not connected to any hubs.");
      goto c_search_clean;
    }
  }

  // No errors? Then open a search tab and wait for the results.
  ui_tab_open(ui_search_create(allhubs ? NULL : tab->hub, q), TRUE);
  q = NULL; // make sure to not free it

c_search_clean:
  g_strfreev(argv);
  search_q_free(q);
}




// definition of the command list
static struct cmd cmds[] = {
  { "browse", c_browse, c_msg_sug,
    "[[-f] <user>]", "Download and browse someone's file list.",
    "Without arguments, this opens a new tab where you can browse your own file list.\n"
    "Note that changes to your list are not immediately visible in the browser."
    " You need to re-open the tab to get the latest version of your list.\n\n"
    "With arguments, the user list of the specified user will be downloaded (if"
    " it has not been downloaded already) and the browse tab will open once it's"
    " complete. The `-f' flag can be used to force the file list to be (re-)downloaded."
  },
  { "clear", c_clear, NULL,
    NULL, "Clear the display.",
    "Clears the log displayed on the screen. Does not affect the log files in any way.\n"
    "Ctrl+l is a shortcut for this command."
  },
  { "close", c_close, NULL,
    NULL, "Close the current tab.",
    "When closing a hub tab, you will be disconnected from the hub.\n"
    "Alt+c is a shortcut for this command."
  },
  { "connect", c_connect, c_connect_sug,
    "[<address>]", "Connect to a hub.",
    "If no address is specified, will connect to the hub you last used on the current tab.\n"
    "The address should be in the form of `dchub://host:port/' or `host:port'.\n"
    "The `:port' part is in both cases optional and defaults to :411.\n\n"
    "Note that this command can only be used on hub tabs. If you want to open a new"
    " connection to a hub, you need to use /open first. For example:\n"
    "  /open testhub\n"
    "  /connect dchub://dc.some-test-hub.com/\n"
    "See `/help open' for more information."
  },
  { "connections", c_connections, NULL,
    NULL, "Display the connection list.",
    "Opens a new tab with the connection list."
  },
  { "disconnect", c_disconnect, NULL,
    NULL, "Disconnect from a hub.",
    "Closes the connection with the hub."
  },
  { "gc", c_gc, NULL,
    NULL, "Perform some garbage collection.",
    "Cleans up unused data and reorganizes existing data to allow more efficient storage and usage.\n"
    "Currently, the only thing being cleaned up is the hashdata.dat file.\n\n"
    "This command may take some time to complete, and will fully block ncdc while it is running.\n"
    "You won't have to perform this command very often."
  },
  { "grant", c_grant, c_msg_sug,
    "[<user>]", "Grant someone a slot.",
    "Granting a slot to someone allows the user to download from you even if you have no free slots.\n\n"
    "The slot will be granted for as long as ncdc stays open. If you restart"
    " ncdc, the user will have to wait for a regular slot. Unless, of course, you"
    " /grant a slot again.\n\n"
    "Note that a granted slot is specific to a single hub. If the user is also"
    " on other hubs, he/she will not be granted a slot on those hubs."
  },
  { "help", c_help, c_help_sug,
    "[<command>]", "Request information on commands.",
    "Use /help without arguments to list all the available commands.\n"
    "Use /help <command> to get information about a particular command."
  },
  { "kick", c_kick, c_msg_sug,
    "<user>", "Kick a user from the hub.",
    "You need to be an OP to be able to use this command."
  },
  { "me", c_me, c_say_sug,
    "<message>", "Chat in third person.",
    "This allows you to talk in third person. Most clients will display your message as something like:\n"
    "  * Nick is doing something\n\n"
    "Note that this command only works correctly on ADC hubs. The NMDC protocol"
    " does not have this feature, and your message will be sent as-is, including the /me."
  },
  { "msg", c_msg, c_msg_sug,
    "<user> [<message>]", "Send a private message.",
    "Send a private message to a user on the currently opened hub.\n"
    "When no message is given, the tab will be opened but no message will be sent."
  },
  { "nick", c_nick, NULL,
    "[<nick>]", "Alias for `/set nick'.",
    ""
  },
  { "open", c_open, c_open_sug,
    "[-n] <name> [<address>]", "Open a new hub tab and connect to the hub.",
    "Opens a new tab to use for a hub. The name is a (short) personal name you"
    " use to identify the hub, and will be used for storing hub-specific"
    " configuration.\n\n"
    "If you have specified an address or have previously connected to a hub"
    " from a tab with the same name, /open will automatically connect to"
    " the hub. Use the `-n' flag to disable this behaviour.\n\n"
    "See `/help connect' for more information on connecting to a hub."
  },
  { "password", c_password, NULL,
    "<password>", "Send your password to the hub.",
    "The /password command can be used to send a password to the hub without saving it to the config file.\n"
    "If you wish to login automatically without having to type /password every time, use '/set password <password>'."
    " Be warned, however, that your password will be saved unencrypted in this case."
  },
  { "pm", c_msg, c_msg_sug,
    "<user> [<message>]", "Alias for /msg",
    ""
  },
  { "queue", c_queue, NULL,
    NULL, "Open the download queue.",
    ""
  },
  { "quit", c_quit, NULL,
    NULL, "Quit ncdc.",
    "You can also just hit ctrl+c, which is equivalent."
  },
  { "reconnect", c_reconnect, NULL,
    NULL, "Shortcut for /disconnect and /connect",
    "When your nick or the hub encoding have been changed, the new settings will be used after the reconnect."
  },
  { "refresh", c_refresh, fl_local_suggest,
    "[<path>]", "Refresh file list.",
    "Initiates a refresh. If no argument is given, the complete list will be refreshed."
    " Otherwise only the specified directory will be refreshed.\n\n"
    "The path argument can be either an absolute filesystem path or a virtual path within your share."
  },
  { "say", c_say, c_say_sug,
    "<message>", "Send a chat message.",
    "You normally don't have to use the /say command explicitly, any command not staring"
    " with '/' will automatically imply `/say <command>'. For example, typing `hello.'"
    " in the command line is equivalent to `/say hello.'.\n\n"
    "Using the /say command explicitly may be useful to send message starting with '/' to"
    " the chat, for example `/say /help is what you are looking for'."
  },
  { "search", c_search, NULL,
    "[options] <query>", "Search for files.",
    "Performs a file search, opening a new tab with the results.\n\n"
    "Available options:\n"
    "  -hub      Search the current hub only. (default)\n"
    "  -all      Search all connected hubs.\n"
    "  -le  <s>  Size of the file must be less than <s>.\n"
    "  -ge  <s>  Size of the file must be larger than <s>.\n"
    "  -t   <t>  File must be of type <t>. (see below)\n"
    "  -tth <h>  TTH root of this file must match <h>.\n\n"
    "File sizes (<s> above) accept the following suffixes: G (GiB), M (MiB) and K (KiB).\n\n"
    "The following file types can be used with the -t option:\n"
    "  1  any      Any file or directory. (default)\n"
    "  2  audio    Audio files.\n"
    "  3  archive  (Compressed) archives.\n"
    "  4  doc      Text documents.\n"
    "  5  exe      Windows executables.\n"
    "  6  img      Image files.\n"
    "  7  video    Videos files.\n"
    "  8  dir      Directories.\n"
    "Note that file type matching is done using the file extension, and is not very reliable."
  },
  { "set", c_set, c_set_sug,
    "[<key> [<value>]]", "Get or set configuration variables.",
    "Use /set without arguments to get a list of configuration variables.\n"
    "/set <key> without value will print out the current value."
  },
  { "share", c_share, c_share_sug,
    "[<name> <path>]", "Add a directory to your share.",
    "Use /share without arguments to get a list of shared directories.\n"
    "When called with a name and a path, the path will be added to your share.\n"
    "Note that shell escaping may be used in the name. For example, to add a"
    " directory with the name `Fun Stuff', you could do the following:\n"
    "  /share \"Fun Stuff\" /path/to/fun/stuff\n"
    "Or:\n"
    "  /share Fun\\ Stuff /path/to/fun/stuff\n\n"
    "The full path to the directory will not be visible to others, only the name you give it will be public.\n"
    "An initial `/refresh' is done automatically on the added directory."
  },
  { "unset", c_unset, c_set_sugkey,
    "<key>", "Unset a configuration variable.",
    "This command will reset the variable back to its default value."
  },
  { "unshare", c_unshare, c_unshare_sug,
    "[<name>]", "Remove a directory from your share.",
    "Use /unshare without arguments to get a list of shared directories.\n"
    "To remove a single directory from your share, use `/unshare <name>'.\n"
    "To remove all directories from your share, use `/unshare /'.\n\n"
    "Note: All hash data for the removed directories will be thrown away. All"
    " files will be re-hashed again when the directory is later re-added."
  },
  { "userlist", c_userlist, NULL,
    NULL, "Open the user list.",
    "Opens the user list of the currently selected hub. Can also be accessed using Alt+u."
  },
  { "version", c_version, NULL,
    NULL, "Display version information.",
    ""
  },
  { "whois", c_whois, c_msg_sug,
    "<user>", "Locate a user in the user list.",
    "This will open the user list and select the given user."
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
  // "replies" should be sent back to.
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

  // execute command when found, generate an error otherwise
  struct cmd *c = getcmd(cmd);
  if(c)
    c->f(args);
  else
    ui_mf(NULL, 0, "Unknown command '%s'.", cmd);

  g_free(str);
}


void cmd_suggest(char *ostr, char **sug) {
  struct cmd *c;
  char *str = g_strdup(ostr);
  // complete command name
  if(str[0] == '/' && !strchr(str, ' ')) {
    int i = 0;
    int len = strlen(str)-1;
    for(c=cmds; i<20 && c->f; c++)
      if(strncmp(str+1, c->name, len) == 0 && strlen(c->name) != len)
        sug[i++] = g_strconcat("/", c->name, NULL);
  } else {
    if(str[0] != '/')
      getcmd("say")->suggest(str, sug);
    else {
      char *sep = strchr(str, ' ');
      *sep = 0;
      c = getcmd(str+1);
      if(c && c->suggest) {
        c->suggest(sep+1, sug);
        strv_prefix(sug, str, " ", NULL);
      }
    }
  }
  g_free(str);
}

