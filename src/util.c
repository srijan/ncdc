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
#include <unistd.h>
#include <errno.h>
#include <stdlib.h>
#include <glib/gstdio.h>
#include <sys/file.h>

#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>

#if INTERFACE


// Get a string from a glib log level
#define loglevel_to_str(level) (\
  (level) & G_LOG_LEVEL_ERROR    ? "ERROR"    :\
  (level) & G_LOG_LEVEL_CRITICAL ? "CRITICAL" :\
  (level) & G_LOG_LEVEL_WARNING  ? "WARNING"  :\
  (level) & G_LOG_LEVEL_MESSAGE  ? "message"  :\
  (level) & G_LOG_LEVEL_INFO     ? "info"     : "debug")

// number of columns of a gunichar
#define gunichar_width(x) (g_unichar_iswide(x) ? 2 : g_unichar_iszerowidth(x) ? 0 : 1)


#endif





// Configuration handling


// global vars
const char *conf_dir = NULL;
GKeyFile *conf_file;

char conf_cid[24];
char conf_pid[24];

#if INTERFACE

#define conf_hub_get(type, name, key) (\
  g_key_file_has_key(conf_file, name, (key), NULL)\
    ? g_key_file_get_##type(conf_file, name, (key), NULL)\
    : g_key_file_get_##type(conf_file, "global", (key), NULL))

#define conf_encoding(hub) (\
  g_key_file_has_key(conf_file, hub, "encoding", NULL)\
    ? g_key_file_get_string(conf_file, hub, "encoding", NULL) \
    : g_key_file_has_key(conf_file, "global", "encoding", NULL) \
    ? g_key_file_get_string(conf_file, "global", "encoding", NULL) \
    : g_strdup("UTF-8"))

#define conf_autorefresh() (\
  !g_key_file_has_key(conf_file, "global", "autorefresh", NULL) ? 60\
    : g_key_file_get_integer(conf_file, "global", "autorefresh", NULL))

#define conf_slots() (\
  !g_key_file_has_key(conf_file, "global", "slots", NULL) ? 10\
    : g_key_file_get_integer(conf_file, "global", "slots", NULL))

#define conf_minislots() (\
  !g_key_file_has_key(conf_file, "global", "minislots", NULL) ? 3\
    : g_key_file_get_integer(conf_file, "global", "minislots", NULL))

#define conf_minislot_size() (1024*(\
  !g_key_file_has_key(conf_file, "global", "minislot_size", NULL) ? 64\
    : g_key_file_get_integer(conf_file, "global", "minislot_size", NULL)))

#define conf_download_dir() (\
  !g_key_file_has_key(conf_file, "global", "download_dir", NULL) ? g_build_filename(conf_dir, "dl", NULL)\
    : g_key_file_get_string(conf_file, "global", "download_dir", NULL))

#define conf_download_slots() (\
  !g_key_file_has_key(conf_file, "global", "download_slots", NULL) ? 3\
    : g_key_file_get_integer(conf_file, "global", "download_slots", NULL))

// Can be used even before the configuration file is loaded. In which case it
// returns TRUE. Default is otherwise FALSE.
#define conf_log_debug() (\
  !conf_file ? TRUE : !g_key_file_has_key(conf_file, "log", "log_debug", NULL) ? FALSE :\
    g_key_file_get_boolean(conf_file, "log", "log_debug", NULL))

#endif


static void generate_pid() {
  guint64 r = rand_64();

  struct tiger_ctx t;
  char pid[24];
  tiger_init(&t);
  tiger_update(&t, (char *)&r, 8);
  tiger_final(&t, pid);

  // now hash the PID so we have our CID
  char cid[24];
  tiger_init(&t);
  tiger_update(&t, pid, 24);
  tiger_final(&t, cid);

  // encode and save
  char enc[40] = {};
  base32_encode(pid, enc);
  g_key_file_set_string(conf_file, "global", "pid", enc);
  base32_encode(cid, enc);
  g_key_file_set_string(conf_file, "global", "cid", enc);
}


void conf_init() {
  // get location of the configuration directory
  if(!conf_dir)
    conf_dir = g_getenv("NCDC_DIR");
  if(!conf_dir)
    conf_dir = g_build_filename(g_get_home_dir(), ".ncdc", NULL);

  // try to create it (ignoring errors if it already exists)
  g_mkdir(conf_dir, 0700);
  if(g_access(conf_dir, F_OK | R_OK | X_OK | W_OK) < 0)
    g_error("Directory '%s' does not exist or is not writable.", conf_dir);

  // make sure some subdirectories exist and are writable
#define cdir(d) do {\
    char *tmp = g_build_filename(conf_dir, d, NULL);\
    g_mkdir(tmp, 0777);\
    if(g_access(conf_dir, F_OK | R_OK | X_OK | W_OK) < 0)\
      g_error("Directory '%s' does not exist or is not writable.", tmp);\
    g_free(tmp);\
  } while(0)
  cdir("logs");
  cdir("inc");
  cdir("fl");
  cdir("dl");
#undef cdir

  // make sure that there is no other ncdc instance working with the same config directory
  char *ver_file = g_build_filename(conf_dir, "version", NULL);
  int ver_fd = g_open(ver_file, O_WRONLY|O_CREAT, 0600);
  if(ver_fd < 0 || flock(ver_fd, LOCK_EX|LOCK_NB))
    g_error("Unable to open lock file. Is another instance of ncdc running with the same configuration directory?");

  // check data directory version
  // version = major, minor
  //   minor = forward & backward compatible, major only backward.
  char dir_ver[2] = {1, 0};
  if(read(ver_fd, dir_ver, 2) < 2)
    if(write(ver_fd, dir_ver, 2) < 2)
      g_error("Could not write to '%s': %s", ver_file, g_strerror(errno));
  g_free(ver_file);
  // Don't close the above file. Keep it open and let the OS close it (and free
  // the lock) when ncdc is closed, was killed or has crashed.
  if(dir_ver[0] > 1)
    g_error("Incompatible data directory. Please upgrade ncdc or use a different directory.");

  // load config file (or create it)
  conf_file = g_key_file_new();
  char *cf = g_build_filename(conf_dir, "config.ini", NULL);
  GError *err = NULL;
  if(g_file_test(cf, G_FILE_TEST_EXISTS)) {
    if(!g_key_file_load_from_file(conf_file, cf, G_KEY_FILE_KEEP_COMMENTS, &err))
      g_error("Could not load '%s': %s", cf, err->message);
  }
  g_free(cf);
  // always set the initial comment
  g_key_file_set_comment(conf_file, NULL, NULL,
    "This file is automatically managed by ncdc.\n"
    "While you could edit it yourself, doing so is highly discouraged.\n"
    "It is better to use the respective commands to change something.\n"
    "Warning: Editing this file while ncdc is running may result in your changes getting lost!", NULL);
  // make sure a nick is set
  if(!g_key_file_has_key(conf_file, "global", "nick", NULL)) {
    char *nick = g_strdup_printf("ncdc_%d", g_random_int_range(1, 9999));
    g_key_file_set_string(conf_file, "global", "nick", nick);
    g_free(nick);
  }
  // make sure we have a PID and CID
  if(!g_key_file_has_key(conf_file, "global", "pid", NULL))
    generate_pid();
  conf_save();

  // load conf_pid and conf_cid
  char *tmp = g_key_file_get_string(conf_file, "global", "pid", NULL);
  base32_decode(tmp, conf_pid);
  g_free(tmp);
  tmp = g_key_file_get_string(conf_file, "global", "cid", NULL);
  base32_decode(tmp, conf_cid);
  g_free(tmp);
}


void conf_save() {
  char *dat = g_key_file_to_data(conf_file, NULL, NULL);
  char *cf = g_build_filename(conf_dir, "config.ini", NULL);
  FILE *f = fopen(cf, "w");
  if(!f || fputs(dat, f) < 0 || fclose(f))
    g_critical("Cannot save config file '%s': %s", cf, g_strerror(errno));
  g_free(dat);
  g_free(cf);
}


void conf_group_rename(const char *from, const char *to) {
  g_return_if_fail(!g_key_file_has_group(conf_file, to));
  char **keys = g_key_file_get_keys(conf_file, from, NULL, NULL);
  char **key = keys;
  for(; key&&*key; key++) {
    char *v = g_key_file_get_value(conf_file, from, *key, NULL);
    g_key_file_set_value(conf_file, to, *key, v);
    g_free(v);
    v = g_key_file_get_comment(conf_file, from, *key, NULL);
    if(v)
      g_key_file_set_comment(conf_file, to, *key, v, NULL);
    g_free(v);
  }
  g_strfreev(keys);
  char *c = g_key_file_get_comment(conf_file, from, NULL, NULL);
  if(c)
    g_key_file_set_comment(conf_file, to, NULL, c, NULL);
  g_free(c);
  g_key_file_remove_group(conf_file, from, NULL);
}





/* A best-effort character conversion function.
 *
 * If, for whatever reason, a character could not be converted, a question mark
 * will be inserted instead. Unlike g_convert_with_fallback(), this function
 * does not fail on invalid byte sequences in the input string, either. Those
 * will simply be replaced with question marks as well.
 *
 * The character sets in 'to' and 'from' are assumed to form a valid conversion
 * according to your iconv implementation.
 *
 * Modifying this function to not require glib, but instead use the iconv and
 * memory allocation functions provided by your system, should be trivial.
 *
 * This function does not correctly handle character sets that may use zeroes
 * in the middle of a string (e.g. UTF-16).
 *
 * This function may not represent best practice with respect to character set
 * conversion, nor has it been thoroughly tested.
 */
char *str_convert(const char *to, const char *from, const char *str) {
  GIConv cd = g_iconv_open(to, from);
  if(cd == (GIConv)-1) {
    g_critical("No conversion from '%s' to '%s': %s", from, to, g_strerror(errno));
    return g_strdup("<encoding-error>");
  }
  gsize inlen = strlen(str);
  gsize outlen = inlen+96;
  gsize outsize = inlen+100;
  char *inbuf = (char *)str;
  char *dest = g_malloc(outsize);
  char *outbuf = dest;
  while(inlen > 0) {
    gsize r = g_iconv(cd, &inbuf, &inlen, &outbuf, &outlen);
    if(r != (gsize)-1)
      continue;
    if(errno == E2BIG) {
      gsize used = outsize - outlen - 4;
      outlen += outsize;
      outsize += outsize;
      dest = g_realloc(dest, outsize);
      outbuf = dest + used;
    } else if(errno == EILSEQ || errno == EINVAL) {
      // skip this byte from the input
      inbuf++;
      inlen--;
      // Only output question mark if we happen to have enough space, otherwise
      // it's too much of a hassle...  (In most (all?) cases we do have enough
      // space, otherwise we'd have gotten E2BIG anyway)
      if(outlen >= 1) {
        *outbuf = '?';
        outbuf++;
        outlen--;
      }
    } else
      g_warn_if_reached();
  }
  memset(outbuf, 0, 4);
  g_iconv_close(cd);
  return dest;
}


// Test that conversion is possible from UTF-8 to fmt and backwards.  Not a
// very comprehensive test, but ensures str_convert() can do its job.
// The reason for this test is to make sure the conversion *exists*,
// whether it makes sense or not can't easily be determined. Note that my
// code currently can't handle zeroes in encoded strings, which is why this
// is also tested (though, again, not comprehensive. But at least it does
// not allow UTF-16)
// Returns FALSE if the encoding can't be used, optionally setting err when it
// has something useful to say.
gboolean str_convert_check(const char *fmt, GError **err) {
  GError *l_err = NULL;
  gsize read, written, written2;
  char *enc = g_convert("abc", -1, "UTF-8", fmt, &read, &written, &l_err);
  if(l_err) {
    g_propagate_error(err, l_err);
    return FALSE;
  } else if(!enc || read != 3 || strlen(enc) != written) {
    g_free(enc);
    return FALSE;
  } else {
    char *dec = g_convert(enc, written, fmt, "UTF-8", &read, &written2, &l_err);
    g_free(enc);
    if(l_err) {
      g_propagate_error(err, l_err);
      return FALSE;
    } else if(!dec || read != written || written2 != 3 || strcmp(dec, "abc") != 0) {
      g_free(dec);
      return FALSE;
    } else {
      g_free(dec);
      return TRUE;
    }
  }
}


// Number of columns required to represent the UTF-8 string.
int str_columns(const char *str) {
  int w = 0;
  while(*str) {
    w += gunichar_width(g_utf8_get_char(str));
    str = g_utf8_next_char(str);
  }
  return w;
}


// returns the byte offset to the last character in str (UTF-8) that does not
// fit within col columns.
int str_offset_from_columns(const char *str, int col) {
  const char *ostr = str;
  int w = 0;
  while(*str && w < col) {
    w += gunichar_width(g_utf8_get_char(str));
    str = g_utf8_next_char(str);
  }
  return str-ostr;
}


// Stolen from ncdu (with small modifications)
// Result is stored in an internal buffer.
char *str_formatsize(guint64 size) {
  static char dat[11]; /* "xxx.xx MiB" */
  double r = size;
  char c = ' ';
  if(r < 1000.0f)      { }
  else if(r < 1023e3f) { c = 'k'; r/=1024.0f; }
  else if(r < 1023e6f) { c = 'M'; r/=1048576.0f; }
  else if(r < 1023e9f) { c = 'G'; r/=1073741824.0f; }
  else if(r < 1023e12f){ c = 'T'; r/=1099511627776.0f; }
  else                 { c = 'P'; r/=1125899906842624.0f; }
  g_snprintf(dat, 11, "%6.2f %c%cB", r, c, c == ' ' ? ' ' : 'i');
  return dat;
}


char *str_fullsize(guint64 size) {
  static char tmp[50];
  static char res[50];
  int i, j;

  /* the K&R method */
  i = 0;
  do {
    tmp[i++] = size % 10 + '0';
  } while((size /= 10) > 0);
  tmp[i] = '\0';

  /* reverse and add thousand seperators */
  j = 0;
  while(i--) {
    res[j++] = tmp[i];
    if(i != 0 && i%3 == 0)
      res[j++] = '.';
  }
  res[j] = '\0';

  return res;
}


// case-insensitive substring match
char *str_casestr(const char *haystack, const char *needle) {
  gsize hlen = strlen(haystack);
  gsize nlen = strlen(needle);

  while(hlen-- >= nlen) {
    if(!g_strncasecmp(haystack, needle, nlen))
      return (char*)haystack;
    haystack++;
  }
  return NULL;
}


// Parses a size string. ('<num>[GMK](iB)?'). Returns G_MAXUINT64 on error.
guint64 str_parsesize(const char *str) {
  char *e = NULL;
  guint64 num = strtoull(str, &e, 10);
  if(e == str)
    return G_MAXUINT64;
  if(!*e)
    return num;
  if(*e == 'G' || *e == 'g')
    num *= 1024*1024*1024;
  else if(*e == 'M' || *e == 'm')
    num *= 1024*1024;
  else if(*e == 'K' || *e == 'K')
    num *= 1024;
  else
    return G_MAXUINT64;
  if(!e[1] || g_strcasecmp(e+1, "b") == 0 || g_strcasecmp(e+1, "ib") == 0)
    return num;
  else
    return G_MAXUINT64;
}


// Prefixes all strings in the array-of-strings with a string, obtained by
// concatenating all arguments together. Last argument must be NULL.
void strv_prefix(char **arr, const char *str, ...) {
  // create the prefix
  va_list va;
  va_start(va, str);
  char *prefix = g_strdup(str);
  const char *c;
  while((c = va_arg(va, const char *))) {
    char *o = prefix;
    prefix = g_strconcat(prefix, c, NULL);
    g_free(o);
  }
  va_end(va);
  // add the prefix to every string
  char **a;
  for(a=arr; *a; a++) {
    char *o = *a;
    *a = g_strconcat(prefix, *a, NULL);
    g_free(o);
  }
  g_free(prefix);
}



// Split a two-argument string into the two arguments.  The first argument
// should be shell-escaped, the second shouldn't. The string should be
// writable. *first should be free()'d, *second refers to a location in str.
void str_arg2_split(char *str, char **first, char **second) {
  GError *err = NULL;
  while(*str == ' ')
    str++;
  char *sep = str;
  gboolean bs = FALSE;
  *first = *second = NULL;
  do {
    if(err)
      g_error_free(err);
    err = NULL;
    sep = strchr(sep+1, ' ');
    if(sep && *(sep-1) == '\\')
      bs = TRUE;
    else {
      if(sep)
        *sep = 0;
      *first = g_shell_unquote(str, &err);
      if(sep)
        *sep = ' ';
      bs = FALSE;
    }
  } while(sep && (err || bs));
  if(sep && sep != str) {
    *second = sep+1;
    while(**second == ' ')
      (*second)++;
  }
}


guint64 rand_64() {
  // g_rand_new() uses four bytes from /dev/urandom when it's available. Doing
  // that twice (i.e. reading 8 bytes) should generate enough randomness for a
  // unique ID. In the case that it uses the current time as fallback, avoid
  // using the same number twice by calling g_rand_set_seed() on the second
  // one.
  GRand *r = g_rand_new();
  guint32 r1 = g_rand_int(r);
  g_rand_free(r);
  r = g_rand_new();
  g_rand_set_seed(r, g_rand_int(r));
  guint32 r2 = g_rand_int(r);
  g_rand_free(r);
  return (((guint64)r1)<<32) + r2;
}


// Equality functions for tiger and TTH hashes. Suitable for use in
// GHashTables.
gboolean tiger_hash_equal(gconstpointer a, gconstpointer b) {
  return memcmp(a, b, 24) == 0;
}



// like realpath(), but also expands ~
char *path_expand(const char *path) {
  char *p = path[0] == '~' ? g_build_filename(g_get_home_dir(), path+1, NULL) : g_strdup(path);
  char *r = realpath(p, NULL);
  g_free(p);
  return r;
}



// String pointer comparison, for use with qsort() on string arrays.
int cmpstringp(const void *p1, const void *p2) {
  return strcmp(* (char * const *) p1, * (char * const *) p2);
}

// Expand and auto-complete a filesystem path
void path_suggest(char *opath, char **sug) {
  char *path = g_strdup(opath);
  char *name, *dir = NULL;

  // special-case ~ and .
  if((path[0] == '~' || path[0] == '.') && (path[1] == 0 || (path[1] == '/' && path[2] == 0))) {
    name = path_expand(path);
    sug[0] = g_strconcat(name, "/", NULL);
    g_free(name);
    goto path_suggest_f;
  }

  char *sep = strrchr(path, '/');
  if(sep) {
    *sep = 0;
    name = sep+1;
    dir = path_expand(path[0] ? path : "/");
    if(!dir)
      goto path_suggest_f;
  } else {
    name = path;
    dir = path_expand(".");
  }
  GDir *d = g_dir_open(dir, 0, NULL);
  if(!d)
    goto path_suggest_f;

  const char *n;
  int i = 0, len = strlen(name);
  while(i<20 && (n = g_dir_read_name(d))) {
    if(strcmp(n, ".") == 0 || strcmp(n, "..") == 0)
      continue;
    char *fn = g_build_filename(dir, n, NULL);
    if(strncmp(n, name, len) == 0 && strlen(n) != len)
      sug[i++] = g_file_test(fn, G_FILE_TEST_IS_DIR) ? g_strconcat(fn, "/", NULL) : g_strdup(fn);
    g_free(fn);
  }
  g_dir_close(d);
  qsort(sug, i, sizeof(char *), cmpstringp);

path_suggest_f:
  g_free(path);
  if(dir)
    free(dir);
}




#if INTERFACE

// Test whether a string is a valid TTH hash. I.e., whether it is a
// base32-encoded 39-character string.
#define istth(s) (strlen(s) == 39 && strspn(s, "abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ234567") == 39)

#endif


// from[24] (binary) -> to[39] (ascii - no padding zero will be added)
void base32_encode(const char *from, char *to) {
  static char alphabet[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZ234567";
  int i, bits = 0, idx = 0, value = 0;
  for(i=0; i<24; i++) {
    value = (value << 8) | (unsigned char)from[i];
    bits += 8;
    while(bits > 5) {
      to[idx++] = alphabet[(value >> (bits-5)) & 0x1F];
      bits -= 5;
    }
  }
  if(bits > 0)
    to[idx++] = alphabet[(value << (5-bits)) & 0x1F];
}


// from[n] (ascii) -> to[floor(n*5/8)] (binary)
// from must be zero-terminated.
void base32_decode(const char *from, char *to) {
  int bits = 0, idx = 0, value = 0;
  while(*from) {
    value = (value << 5) | (*from <= '9' ? (26+(*from-'2')) : *from-'A');
    bits += 5;
    while(bits > 8) {
      to[idx++] = (value >> (bits-8)) & 0xFF;
      bits -= 8;
    }
    from++;
  }
}



// Handy wrappers to efficiently store an IPv4 address in an integer. This is
// only used for internal storage, all actual network IO is done using GIO,
// which works with the stringified versions. For portability and future IPv6
// support, it'd help to use GInetAddress objects instead, but that's
// inefficient. (For one thing, a simple pointer already takes twice as much
// space as a compacted IPv4 address). Even for IPv6 it'll be more efficient to
// use this strategy of ip6_pack() and ip6_unpack() with two guint64's.

guint32 ip4_pack(const char *str) {
  struct in_addr n;
  if(!inet_aton(str, &n))
    return 0;
  return n.s_addr; // this is an uint32_t, on linux at least
}


// Returns a static string buffer.
char *ip4_unpack(guint32 ip) {
  struct in_addr n;
  n.s_addr = ip;
  return inet_ntoa(n);
}






// Transfer / hashing rate calculation

/* How to use this:
 * From main thread:
 *   struct ratecalc thing;
 *   ratecalc_init(&thing);
 *   ratecalc_register(&thing);
 * From any thread (usually some worker thread):
 *   ratecalc_add(&thing, bytes);
 * From main thread:
 *   rate = ratecalc_get(&thing);
 *   ratecalc_reset(&thing);
 *   ratecalc_unregister(&thing);
 *
 * ratecalc_calc() should be called with a one-second interval
 */

#if INTERFACE

struct ratecalc {
  int counter;
  int rate;
  guint64 total;
  char isreg;
};

#define ratecalc_add(rc, b) g_atomic_int_add(&((rc)->counter), b)

#define ratecalc_reset(rc) do {\
    g_atomic_int_set(&((rc)->counter), 0);\
    (rc)->rate = (rc)->total = 0;\
  } while(0)

#define ratecalc_init(rc) do {\
    ratecalc_unregister(rc);\
    ratecalc_reset(rc);\
  } while(0)

#define ratecalc_register(rc) do { if(!(rc)->isreg) {\
    ratecalc_list = g_slist_prepend(ratecalc_list, rc);\
    (rc)->isreg = 1;\
  } } while(0)

#define ratecalc_unregister(rc) do {\
    ratecalc_list = g_slist_remove(ratecalc_list, rc);\
    (rc)->isreg = (rc)->rate = 0;\
  } while(0)

#define ratecalc_get(rc) ((rc)->rate)

#define ratecalc_calc() do {\
    GSList *n; int cur; struct ratecalc *rc;\
    for(n=ratecalc_list; n; n=n->next) {\
      rc = n->data;\
      do {\
        cur = g_atomic_int_get(&(rc->counter));\
      } while(!g_atomic_int_compare_and_exchange(&(rc->counter), cur, 0));\
      rc->total += cur;\
      rc->rate = cur + ((rc->rate - cur) / 2);\
    }\
  } while(0)

#endif

GSList *ratecalc_list = NULL;


// calculates an ETA and formats it into a "?d ?h ?m ?s" thing
char *ratecalc_eta(struct ratecalc *rc, guint64 left) {
  static char buf[100];
  int sec = left / MAX(1, ratecalc_get(rc));
  int l = 0;
  buf[0] = 0;
  if(sec > 356*24*3600)
    return "-";
  if(sec >= 24*3600) {
    l += g_snprintf(buf+l, 99-l, "%dd ", sec/(24*3600));
    sec %= 24*3600;
  }
  if(sec >= 3600) {
    l += g_snprintf(buf+l, 99-l, "%dh ", sec/3600);
    sec %= 3600;
  }
  if(sec >= 60) {
    l += g_snprintf(buf+l, 99-l, "%dm ", sec/60);
    sec %= 60;
  }
  g_snprintf(buf+l, 99-l, "%ds", sec);
  return buf;
}





// Protocol utility functions

char *charset_convert(struct hub *hub, gboolean to_utf8, const char *str) {
  char *fmt = conf_encoding(hub->tab->name);
  char *res = str_convert(to_utf8?"UTF-8":fmt, !to_utf8?"UTF-8":fmt, str);
  g_free(fmt);
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
  char *exts[10];
};

#endif

// NMDC search types and their extensions.
struct search_type search_types[] = { {},
  { "any"      }, // 1
  { "audio",   { "mp3",  "mp2",  "wav",  "au",  "rm", "mid",  "sm"        } },
  { "archive", { "zip",  "arj",  "rar", "lzh",  "gz",   "z", "arc", "pak" } },
  { "doc",     { "doc",  "txt",  "wri", "pdf",  "ps", "tex"               } },
  { "exe",     {  "pm",  "exe",  "bat", "com"                             } },
  { "img",     { "gif",  "jpg", "jpeg", "bmp", "pcx", "png", "wmf", "psd" } },
  { "video",   { "mpg", "mpeg",  "avi", "asf", "mov"                      } },
  { "dir"      }, // 8
  {}              // 9
};


void search_q_free(struct search_q *q) {
  if(!q)
    return;
  g_strfreev(q->query);
  g_slice_free(struct search_q, q);
}


void search_r_free(struct search_r *r) {
  if(!r)
    return;
  g_free(r->file);
  g_slice_free(struct search_r, r);
}


// Currently requires hub to be valid. Modifies msg in-place for temporary stuff.
// TODO: Handle active responses (hub=NULL)
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

  // <space>(hub_ip:hub_port). Also ignored since we only support passive results for now.
  tmp = strrchr(msg, ' ');
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


// TODO: active responses
struct search_r *search_parse_adc(struct hub *hub, struct adc_cmd *cmd) {
  struct search_r r = {};
  char *tmp, *tmp2;

  // file
  r.file = adc_getparam(cmd->argv, "FN", NULL);
  if(!r.file)
    return NULL;
  while(strlen(r.file) > 1 && r.file[strlen(r.file)-1] == '/')
    r.file[strlen(r.file)-1] = 0;

  // tth & size
  tmp = adc_getparam(cmd->argv, "TR", NULL);
  if(tmp) {
    if(!istth(tmp))
      return NULL;
    base32_decode(tmp, r.tth);
    tmp = adc_getparam(cmd->argv, "SI", NULL);
    if(!tmp)
      return NULL;
    r.size = g_ascii_strtoull(tmp, &tmp2, 10);
    if(tmp == tmp2 || !tmp2 || *tmp2)
      return NULL;
  } else
    r.size = G_MAXUINT64;

  // slots
  tmp = adc_getparam(cmd->argv, "SL", NULL);
  if(tmp) {
    r.slots = g_ascii_strtoull(tmp, &tmp2, 10);
    if(tmp == tmp2 || !tmp2 || *tmp2)
      return NULL;
  }

  // uid
  struct hub_user *u = g_hash_table_lookup(hub->sessions, GINT_TO_POINTER(cmd->source));
  if(!u)
    return NULL;
  r.uid = u->uid;

  // If we're here, then we can safely copy and return the result.
  struct search_r *res = g_slice_dup(struct search_r, &r);
  res->file = g_strdup(r.file);
  return res;
}

