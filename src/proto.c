/* ncdc - NCurses Direct Connect client

  Copyright (c) 2011-2012 Yoran Heling

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
#include <stdlib.h>
#include <string.h>


// This file contains various protocol-related utility functions:
// - Generic support (nmdc_* and adc_*)
// - Performing searches and handling the results (search_*)




// NMDC support

char *charset_convert(struct hub *hub, gboolean to_utf8, const char *str) {
  char *fmt = var_get(hub->id, VAR_encoding);
  char *res = str_convert(to_utf8?"UTF-8":fmt, !to_utf8?"UTF-8":fmt, str);
  return res;
}


char *nmdc_encode_and_escape(struct hub *hub, const char *str) {
  char *enc = charset_convert(hub, FALSE, str);
  GString *dest = g_string_sized_new(strlen(enc));
  char *tmp = enc;
  while(*tmp) {
    if(*tmp == '$')
      g_string_append(dest, "&#36;");
    else if(*tmp == '|')
      g_string_append(dest, "&#124;");
    else if(*tmp == '&' && (strncmp(tmp, "&amp;", 5) == 0 || strncmp(tmp, "&#36;", 5) == 0 || strncmp(tmp, "&#124;", 6) == 0))
      g_string_append(dest, "&amp;");
    else
      g_string_append_c(dest, *tmp);
    tmp++;
  }
  g_free(enc);
  return g_string_free(dest, FALSE);
}


char *nmdc_unescape_and_decode(struct hub *hub, const char *str) {
  GString *dest = g_string_sized_new(strlen(str));
  while(*str) {
    if(strncmp(str, "&#36;", 5) == 0) {
      g_string_append_c(dest, '$');
      str += 5;
    } else if(strncmp(str, "&#124;", 6) == 0) {
      g_string_append_c(dest, '|');
      str += 6;
    } else if(strncmp(str, "&amp;", 5) == 0) {
      g_string_append_c(dest, '&');
      str += 5;
    } else {
      g_string_append_c(dest, *str);
      str++;
    }
  }
  char *dec = charset_convert(hub, TRUE, dest->str);
  g_string_free(dest, TRUE);
  return dec;
}


// Info & algorithm @ http://www.teamfair.info/wiki/index.php?title=Lock_to_key
// This function modifies "lock" in-place for temporary data
char *nmdc_lock2key(char *lock) {
  char n;
  int i;
  int len = strlen(lock);
  if(len < 3)
    return g_strdup("STUPIDKEY!"); // let's not crash on invalid data
  int fst = lock[0] ^ lock[len-1] ^ lock[len-2] ^ 5;
  for(i=len-1; i; i--)
    lock[i] = lock[i] ^ lock[i-1];
  lock[0] = fst;
  for(i=0; i<len; i++)
    lock[i] = ((lock[i]<<4) & 0xF0) | ((lock[i]>>4) & 0x0F);
  GString *key = g_string_sized_new(len+100);
  for(i=0; i<len; i++) {
    n = lock[i];
    if(n == 0 || n == 5 || n == 36 || n == 96 || n == 124 || n == 126)
      g_string_append_printf(key, "/%%DCN%03d%%/", n);
    else
      g_string_append_c(key, n);
  }
  return g_string_free(key, FALSE);
}





// ADC support


// ADC parameter unescaping
char *adc_unescape(const char *str, gboolean nmdc) {
  char *dest = g_new(char, strlen(str)+1);
  char *tmp = dest;
  while(*str) {
    if(*str == '\\') {
      str++;
      if(*str == 's' || (nmdc && *str == ' '))
        *tmp = ' ';
      else if(*str == 'n')
        *tmp = '\n';
      else if(*str == '\\')
        *tmp = '\\';
      else {
        g_free(dest);
        return NULL;
      }
    } else
      *tmp = *str;
    tmp++;
    str++;
  }
  *tmp = 0;
  return dest;
}


// ADC parameter escaping
char *adc_escape(const char *str, gboolean nmdc) {
  GString *dest = g_string_sized_new(strlen(str)+50);
  while(*str) {
    switch(*str) {
    case ' ':  g_string_append(dest, nmdc ? "\\ " : "\\s"); break;
    case '\n': g_string_append(dest, "\\n"); break;
    case '\\': g_string_append(dest, "\\\\"); break;
    default: g_string_append_c(dest, *str); break;
    }
    str++;
  }
  return g_string_free(dest, FALSE);
}


#if INTERFACE

// Only writes to the first 4 bytes of str, does not add a padding \0.
#define ADC_EFCC(sid, str) do {\
    (str)[0] = ((sid)>> 0) & 0xFF;\
    (str)[1] = ((sid)>> 8) & 0xFF;\
    (str)[2] = ((sid)>>16) & 0xFF;\
    (str)[3] = ((sid)>>24) & 0xFF;\
  } while(0)

// and the reverse
#define ADC_DFCC(str) ((str)[0] + ((str)[1]<<8) + ((str)[2]<<16) + ((str)[3]<<24))


#define ADC_TOCMDV(a, b, c) ((a) + ((b)<<8) + ((c)<<16))
#define ADC_TOCMD(str) ADC_TOCMDV((str)[0], (str)[1], (str)[2])

enum adc_commands {
#define C(n, a, b, c) ADCC_##n = ADC_TOCMDV(a,b,c)
  // Base commands (copied from DC++ / AdcCommand.h)
  C(SUP, 'S','U','P'), // F,T,C    - PROTOCOL, NORMAL
  C(STA, 'S','T','A'), // F,T,C,U  - All
  C(INF, 'I','N','F'), // F,T,C    - IDENTIFY, NORMAL
  C(MSG, 'M','S','G'), // F,T      - NORMAL
  C(SCH, 'S','C','H'), // F,T,C    - NORMAL (can be in U, but is discouraged)
  C(RES, 'R','E','S'), // F,T,C,U  - NORMAL
  C(CTM, 'C','T','M'), // F,T      - NORMAL
  C(RCM, 'R','C','M'), // F,T      - NORMAL
  C(GPA, 'G','P','A'), // F        - VERIFY
  C(PAS, 'P','A','S'), // T        - VERIFY
  C(QUI, 'Q','U','I'), // F        - IDENTIFY, VERIFY, NORMAL
  C(GET, 'G','E','T'), // C        - NORMAL (extensions may use in it F/T as well)
  C(GFI, 'G','F','I'), // C        - NORMAL
  C(SND, 'S','N','D'), // C        - NORMAL (extensions may use it in F/T as well)
  C(SID, 'S','I','D')  // F        - PROTOCOL
#undef C
};


struct adc_cmd {
  char type;  // B|C|D|E|F|H|I|U
  int cmd;    // ADCC_*, but can also be something else. Unhandled commands should be ignored anyway.
  int source; // Only when type = B|D|E|F
  int dest;   // Only when type = D|E
  char **argv;
  char argc;
};


// ADC Protocol states.
#define ADC_S_PROTOCOL 0
#define ADC_S_IDENTIFY 1
#define ADC_S_VERIFY   2
#define ADC_S_NORMAL   3
#define ADC_S_DATA     4

#endif


static gboolean int_in_array(const int *arr, int needle) {
  for(; arr&&*arr; arr++)
    if(*arr == needle)
      return TRUE;
  return FALSE;
}


void adc_parse(const char *str, struct adc_cmd *c, int *feats, GError **err) {
  if(!g_utf8_validate(str, -1, NULL)) {
    g_set_error_literal(err, 1, 0, "Invalid encoding.");
    return;
  }

  if(strlen(str) < 4) {
    g_set_error_literal(err, 1, 0, "Message too short.");
    return;
  }

  if(*str != 'B' && *str != 'C' && *str != 'D' && *str != 'E' && *str != 'F' && *str != 'H' && *str != 'I' && *str != 'U') {
    g_set_error_literal(err, 1, 0, "Invalid ADC type");
    return;
  }
  c->type = *str;
  c->cmd = ADC_TOCMD(str+1);

  const char *off = str+4;
  if(off[0] && off[0] != ' ') {
    g_set_error_literal(err, 1, 0, "Invalid characters after command.");
    return;
  }
  off++;

  // type = U, first argument is source CID. But we don't handle that here.

  // type = B|D|E|F, first argument must be the source SID
  if(c->type == 'B' || c->type == 'D' || c->type == 'E' || c->type == 'F') {
    if(strlen(off) < 4) {
      g_set_error_literal(err, 1, 0, "Message too short");
      return;
    }
    c->source = ADC_DFCC(off);
    if(off[4] && off[4] != ' ') {
      g_set_error_literal(err, 1, 0, "Invalid characters after argument.");
      return;
    }
    off += off[4] ? 5 : 4;
  }

  // type = D|E, next argument must be the destination SID
  if(c->type == 'D' || c->type == 'E') {
    if(strlen(off) < 4) {
      g_set_error_literal(err, 1, 0, "Message too short");
      return;
    }
    c->dest = ADC_DFCC(off);
    if(off[4] && off[4] != ' ') {
      g_set_error_literal(err, 1, 0, "Invalid characters after argument.");
      return;
    }
    off += off[4] ? 5 : 4;
  }

  // type = F, next argument must be the feature list. We'll match this with
  // the 'feats' list (space separated list of FOURCCs, to make things easier)
  // to make sure it's correct. Some hubs broadcast F messages without actually
  // checking the listed features. :-/
  if(c->type == 'F') {
    int l = strchr(off, ' ') ? strchr(off, ' ')-off : strlen(off);
    if((l % 5) != 0) {
      g_set_error_literal(err, 1, 0, "Message too short");
      return;
    }
    int i;
    for(i=0; i<l/5; i++) {
      int f = ADC_DFCC(off+i*5+1);
      if(off[i*5] == '+' && !int_in_array(feats, f)) {
        g_set_error_literal(err, 1, 0, "Feature broadcast for a feature we don't have.");
        return;
      }
      if(off[i*5] == '-' && int_in_array(feats, f)) {
        g_set_error_literal(err, 1, 0, "Feature broadcast excluding a feature we have.");
        return;
      }
    }
    off += off[l] ? l+1 : l;
  }

  // parse the rest of the arguments
  char **s = g_strsplit(off, " ", 0);
  c->argc = s ? g_strv_length(s) : 0;
  if(s) {
    char **a = g_new0(char *, c->argc+1);
    int i;
    for(i=0; i<c->argc; i++) {
      a[i] = adc_unescape(s[i], FALSE);
      if(!a[i]) {
        g_set_error_literal(err, 1, 0, "Invalid escape in argument.");
        break;
      }
    }
    g_strfreev(s);
    if(i < c->argc) {
      g_strfreev(a);
      return;
    }
    c->argv = a;
  } else
    c->argv = NULL;
}


char *adc_getparam(char **a, char *name, char ***left) {
  while(a && *a) {
    if(**a && **a == name[0] && (*a)[1] == name[1]) {
      if(left)
        *left = a+1;
      return *a+2;
    }
    a++;
  }
  return NULL;
}


// Get all parameters with the given name. Return value should be g_free()'d,
// NOT g_strfreev()'ed.
char **adc_getparams(char **a, char *name) {
  int n = 10;
  char **res = g_new(char *, n);
  int i = 0;
  while(a && *a) {
    if(**a && **a == name[0] && (*a)[1] == name[1])
      res[i++] = *a+2;
    if(i >= n) {
      n += 10;
      res = g_realloc(res, n*sizeof(char *));
    }
    a++;
  }
  res[i] = NULL;
  if(res[0])
    return res;
  g_free(res);
  return NULL;
}


GString *adc_generate(char type, int cmd, int source, int dest) {
  GString *c = g_string_sized_new(100);
  g_string_append_c(c, type);
  char r[5] = {};
  ADC_EFCC(cmd, r);
  g_string_append(c, r);

  if(source) {
    g_string_append_c(c, ' ');
    ADC_EFCC(source, r);
    g_string_append(c, r);
  }

  if(dest) {
    g_string_append_c(c, ' ');
    ADC_EFCC(dest, r);
    g_string_append(c, r);
  }

  return c;
}


void adc_append(GString *c, const char *name, const char *arg) {
  g_string_append_c(c, ' ');
  if(name)
    g_string_append(c, name);
  char *enc = adc_escape(arg, FALSE);
  g_string_append(c, enc);
  g_free(enc);
}





// Search related helper functions

#if INTERFACE

struct search_q {
  char type;    // NMDC search type (if 9, ignore all fields except tth)
  gboolean ge;  // TRUE -> match >= size; FALSE -> match <= size
  guint64 size; // 0 = disabled.
  char **query; // list of patterns to include
  char tth[24]; // only used when type = 9
};

// Represents a search result, coming from either NMDC $SR or ADC RES.
struct search_r {
  guint64 uid;
  char *file;     // full path + filename. Slashes as path saparator, no trailing slash
  guint64 size;   // file size, G_MAXUINT64 = directory
  int slots;      // free slots
  char tth[24];   // TTH root (for regular files)
}

struct search_type {
  char *name;
  char *exts[25];
};

#endif

// NMDC search types and the relevant ADC SEGA extensions.
struct search_type search_types[] = { {},
  { "any"      }, // 1
  { "audio",   { "ape", "flac", "m4a",  "mid",  "mp3", "mpc",  "ogg",  "ra", "wav",  "wma"                                                                           } },
  { "archive", {  "7z",  "ace", "arj",  "bz2",   "gz", "lha",  "lzh", "rar", "tar",   "tz",   "z",  "zip"                                                            } },
  { "doc",     { "doc", "docx", "htm", "html",  "nfo", "odf",  "odp", "ods", "odt",  "pdf", "ppt", "pptx", "rtf", "txt",  "xls", "xlsx", "xml", "xps"                } },
  { "exe",     { "app",  "bat", "cmd",  "com",  "dll", "exe",  "jar", "msi", "ps1",  "vbs", "wsf"                                                                    } },
  { "img",     { "bmp",  "cdr", "eps",  "gif",  "ico", "img", "jpeg", "jpg", "png",   "ps", "psd",  "sfw", "tga", "tif", "webp"                                      } },
  { "video",   { "3gp",  "asf", "asx",  "avi", "divx", "flv",  "mkv", "mov", "mp4", "mpeg", "mpg",  "ogm", "pxp",  "qt",   "rm", "rmvb", "swf", "vob", "webm", "wmv" } },
  { "dir"      }, // 8
  {}              // 9
};


void search_q_free(struct search_q *q) {
  if(!q)
    return;
  if(q->query)
    g_strfreev(q->query);
  g_slice_free(struct search_q, q);
}


// Can be used as a GDestroyNotify callback
void search_r_free(gpointer data) {
  struct search_r *r = data;
  if(!r)
    return;
  g_free(r->file);
  g_slice_free(struct search_r, r);
}


struct search_r *search_r_copy(struct search_r *r) {
  struct search_r *res = g_slice_dup(struct search_r, r);
  res->file = g_strdup(r->file);
  return res;
}


// Currently requires hub to be valid. Modifies msg in-place for temporary stuff.
struct search_r *search_parse_nmdc(struct hub *hub, char *msg) {
  struct search_r r = {};
  char *tmp, *tmp2;
  gboolean hastth = FALSE;

  // forward search to get the username and offset to the filename
  if(strncmp(msg, "$SR ", 4) != 0)
    return NULL;
  msg += 4;
  char *user = msg;
  msg = strchr(msg, ' ');
  if(!msg)
    return NULL;
  *(msg++) = 0;
  r.file = msg;

  // msg is now searched backwards, because we can't reliably determine the end
  // of the filename otherwise.

  // <space>(hub_ip:hub_port).
  tmp = strrchr(msg, ' ');
  if(!tmp)
    return NULL;
  *(tmp++) = 0;
  if(*(tmp++) != '(')
    return NULL;
  char *hubaddr = tmp;
  tmp = strchr(tmp, ')');
  if(!tmp)
    return NULL;
  *tmp = 0;

  // <0x05>TTH:stuff
  tmp = strrchr(msg, 5);
  if(!tmp)
    return NULL;
  *(tmp++) = 0;
  if(strncmp(tmp, "TTH:", 4) == 0) {
    if(!istth(tmp+4))
      return NULL;
    base32_decode(tmp+4, r.tth);
    hastth = TRUE;
  }

  // <space>free_slots/total_slots. We only care about the free slots.
  tmp = strrchr(msg, ' ');
  if(!tmp)
    return NULL;
  *(tmp++) = 0;
  r.slots = g_ascii_strtoull(tmp, &tmp2, 10);
  if(tmp == tmp2 || !tmp2 || *tmp2 != '/')
    return NULL;

  // At this point, msg contains either "filename<0x05>size" in the case of a
  // file or "path" in the case of a directory.
  tmp = strrchr(msg, 5);
  if(tmp) {
    // files must have a TTH
    if(!hastth)
      return NULL;
    *(tmp++) = 0;
    r.size = g_ascii_strtoull(tmp, &tmp2, 10);
    if(tmp == tmp2 || !tmp2 || *tmp2)
      return NULL;
  } else
    r.size = G_MAXUINT64;

  // \ -> /, and remove trailing slashes
  for(tmp = r.file; *tmp; tmp++)
    if(*tmp == '\\')
      *tmp = '/';
  while(--tmp > r.file && *tmp == '/')
    *tmp = 0;

  // For active search results: figure out the hub
  // TODO: Use the hub list associated with the incoming port of listen.c?
  if(!hub) {
    tmp = strchr(hubaddr, ':') ? g_strdup(hubaddr) : g_strdup_printf("%s:411", hubaddr);
    int colon = strchr(tmp, ':') - tmp;
    GList *n;
    for(n=ui_tabs; n; n=n->next) {
      struct ui_tab *t = n->data;
      if(t->type != UIT_HUB || !t->hub->nick_valid || t->hub->adc)
        continue;
      // Excact hub:ip match, stop searching
      if(strcmp(tmp, net_remoteaddr(t->hub->net)) == 0) {
        hub = t->hub;
        break;
      }
      // Otherwise, try a fuzzy search (ignoring the port)
      tmp[colon] = 0;
      if(strncmp(tmp, net_remoteaddr(t->hub->net), colon) == 0)
        hub = t->hub;
      tmp[colon] = ':';
    }
    g_free(tmp);
    if(!hub)
      return NULL;
  }

  // Figure out r.uid
  struct hub_user *u = g_hash_table_lookup(hub->users, user);
  if(!u)
    return NULL;
  r.uid = u->uid;

  // If we're here, then we can safely copy and return the result.
  struct search_r *res = g_slice_dup(struct search_r, &r);
  res->file = nmdc_unescape_and_decode(hub, r.file);
  return res;
}


struct search_r *search_parse_adc(struct hub *hub, struct adc_cmd *cmd) {
  struct search_r r = {};
  char *tmp, *tmp2;

  // If this came from UDP, fetch the users' CID
  if(!hub && (cmd->type != 'U' || cmd->argc < 1 || !istth(cmd->argv[0])))
    return NULL;
  char cid[24];
  if(!hub)
    base32_decode(cmd->argv[0], cid);
  char **argv = hub ? cmd->argv : cmd->argv+1;

  // file
  r.file = adc_getparam(argv, "FN", NULL);
  if(!r.file)
    return NULL;
  gboolean isfile = TRUE;
  while(strlen(r.file) > 1 && r.file[strlen(r.file)-1] == '/') {
    r.file[strlen(r.file)-1] = 0;
    isfile = FALSE;
  }

  // tth & size
  tmp = isfile ? adc_getparam(argv, "TR", NULL) : NULL;
  if(tmp) {
    if(!istth(tmp))
      return NULL;
    base32_decode(tmp, r.tth);
    tmp = adc_getparam(argv, "SI", NULL);
    if(!tmp)
      return NULL;
    r.size = g_ascii_strtoull(tmp, &tmp2, 10);
    if(tmp == tmp2 || !tmp2 || *tmp2)
      return NULL;
  } else
    r.size = G_MAXUINT64;

  // slots
  tmp = adc_getparam(argv, "SL", NULL);
  if(tmp) {
    r.slots = g_ascii_strtoull(tmp, &tmp2, 10);
    if(tmp == tmp2 || !tmp2 || *tmp2)
      return NULL;
  }

  // uid - passive
  if(hub) {
    struct hub_user *u = g_hash_table_lookup(hub->sessions, GINT_TO_POINTER(cmd->source));
    if(!u)
      return NULL;
    r.uid = u->uid;

  // uid - active. Active responses must have the hubid in the token, from
  // which we can generate the uid.
  } else {
    tmp = adc_getparam(argv, "TO", NULL);
    if(!tmp)
      return NULL;
    guint64 hubid = g_ascii_strtoull(tmp, &tmp2, 10);
    if(tmp == tmp2 || !tmp2 || *tmp2)
      return NULL;
    struct tiger_ctx t;
    tiger_init(&t);
    tiger_update(&t, (char *)&hubid, 8);
    tiger_update(&t, cid, 24);
    char res[24];
    tiger_final(&t, res);
    memcpy(&r.uid, res, 8);
  }

  // If we're here, then we can safely copy and return the result.
  return search_r_copy(&r);
}


// Matches a search result with a query.
gboolean search_match(struct search_q *q, struct search_r *r) {
  // TTH match is fast and easy
  if(q->type == 9)
    return r->size == G_MAXUINT64 ? FALSE : memcmp(q->tth, r->tth, 24) == 0 ? TRUE : FALSE;
  // Match file/dir type
  if(q->type == 8 && r->size != G_MAXUINT64)
    return FALSE;
  if((q->size || (q->type >= 2 && q->type <= 7)) && r->size == G_MAXUINT64)
    return FALSE;
  // Match size
  if(q->size && !(q->ge ? r->size >= q->size : r->size <= q->size))
    return FALSE;
  // Match query
  char **str = q->query;
  for(; str&&*str; str++)
    if(G_LIKELY(!str_casestr(r->file, *str)))
      return FALSE;
  // Match extension
  char **ext = search_types[(int)q->type].exts;
  if(ext && *ext) {
    char *l = strrchr(r->file, '.');
    if(G_UNLIKELY(!l || !l[1]))
      return FALSE;
    l++;
    for(; *ext; ext++)
      if(G_UNLIKELY(g_ascii_strcasecmp(l, *ext) == 0))
        break;
    if(!*ext)
      return FALSE;
  }
  // Okay, we have a match
  return TRUE;
}


// Generate the required /search command for a query.
char *search_command(struct search_q *q, gboolean onhub) {
  GString *str = g_string_new("/search");
  g_string_append(str, onhub ? " -hub" : " -all");
  if(q->type == 9) {
    char tth[40] = {};
    base32_encode(q->tth, tth);
    g_string_append(str, " -tth ");
    g_string_append(str, tth);
  }
  if(q->type != 9) {
    g_string_append(str, " -t ");
    g_string_append(str, search_types[(int)q->type].name);
  }
  if(q->type != 9 && q->size) // TODO: convert back to K/M/G suffix when possible?
    g_string_append_printf(str, " -%s %"G_GUINT64_FORMAT, q->ge ? "ge" : "le", q->size);
  char **query = q->type == 9 ? NULL : q->query;
  char **tmp = query;
  for(; tmp&&*tmp; tmp++)
    if(**tmp == '-')
      break;
  if(tmp&&*tmp)
    g_string_append(str, " --");
  for(tmp=query; tmp&&*tmp; tmp++) {
    g_string_append_c(str, ' ');
    if(strcspn(*tmp, " \\'\"") != strlen(*tmp)) {
      char *s = g_shell_quote(*tmp);
      g_string_append(str, s);
      g_free(s);
    } else
      g_string_append(str, *tmp);
  }
  return g_string_free(str, FALSE);
}


// Performs the search query on the given hub, or on all hubs if hub=NULL.
// Opens the search tab. Returns FALSE on error and throws an error message at
// ui_m(). Ownership of the search_q struct is passed to this function, and
// should not be relied upon after calling.
gboolean search_do(struct search_q *q, struct hub *hub, struct ui_tab *parent) {
  if((!q->query || !*q->query) && q->type != 9) {
    ui_m(NULL, 0, "No search query given.");
    search_q_free(q);
    return FALSE;
  }

  // Search a single hub
  if(hub) {
    if(!hub->nick_valid) {
      ui_m(NULL, 0, "Not connected");
      search_q_free(q);
      return FALSE;
    }
    if(var_get_bool(hub->id, VAR_chat_only))
      ui_m(NULL, 0, "WARNING: Searching on a hub with the `chat_only' setting enabled.");
    hub_search(hub, q);

  // Search all hubs (excluding those with chat_only set)
  } else {
    GList *n;
    gboolean one = FALSE;
    for(n=ui_tabs; n; n=n->next) {
      struct ui_tab *t = n->data;
      if(t->type == UIT_HUB && t->hub->nick_valid && !var_get_bool(t->hub->id, VAR_chat_only)) {
        hub_search(t->hub, q);
        one = TRUE;
      }
    }
    if(!one) {
      ui_m(NULL, 0, "Not connected to any non-chat hubs.");
      search_q_free(q);
      return FALSE;
    }
  }

  // No errors? Then open a search tab and wait for the results.
  ui_tab_open(ui_search_create(hub, q), TRUE, parent);
  return TRUE;
}


// Shortcut for a TTH search_do() on all hubs.
gboolean search_alltth(char *tth, struct ui_tab *parent) {
  struct search_q *q = g_slice_new0(struct search_q);
  memcpy(q->tth, tth, 24);
  q->type = 9;
  return search_do(q, NULL, parent);
}
