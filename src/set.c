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


// the settings list
static struct setting settings[] = {
#define C(n, a,b,c) { "color_" G_STRINGIFY(n), get_color, set_color, set_color_sug },
  UI_COLORS
#undef C
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

