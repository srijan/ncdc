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
#include <fcntl.h>

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



// UTF-8 aware case-insensitive string comparison.
// This should be somewhat equivalent to
//   strcmp(g_utf8_casefold(a), g_utf8_casefold(b)),
// but hopefully faster by avoiding the memory allocations.
// Note that g_utf8_collate() is not suitable for case-insensitive filename
// comparison. For example, g_utf8_collate('a', 'A') != 0.
int str_casecmp(const char *a, const char *b) {
  int d;
  while(*a && *b) {
    d = g_unichar_tolower(g_utf8_get_char(a)) - g_unichar_tolower(g_utf8_get_char(b));
    if(d)
      return d;
    a = g_utf8_next_char(a);
    b = g_utf8_next_char(b);
  }
  return *a ? 1 : *b ? -1 : 0;
}


// UTF-8 aware case-insensitive substring match.
// This should be somewhat equivalent to
//   strstr(g_utf8_casefold(haystack), g_utf8_casefold(needle))
// If the same needle is used to match against many haystacks, it will be far
// more efficient to use regular expressions instead. Those tend to be around 4
// times faster.
char *str_casestr(const char *haystack, const char *needle) {
  gsize hlen = g_utf8_strlen(haystack, -1);
  gsize nlen = g_utf8_strlen(needle, -1);
  int d, l;
  const char *a, *b;

  while(hlen-- >= nlen) {
    a = haystack;
    b = needle;
    l = nlen;
    d = 0;
    while(l-- > 0 && *b) {
      d = g_unichar_tolower(g_utf8_get_char(a)) - g_unichar_tolower(g_utf8_get_char(b));
      if(d)
        break;
      a = g_utf8_next_char(a);
      b = g_utf8_next_char(b);
    }
    if(!d)
      return (char *)haystack;
    haystack = g_utf8_next_char(haystack);
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


char *str_formatinterval(int sec) {
  static char buf[100];
  int l=0;
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
  if(sec || !l)
    g_snprintf(buf+l, 99-l, "%ds", sec);
  return buf;
}


// Parses an interval string, returns -1 on error.
int str_parseinterval(const char *str) {
  int sec = 0;
  while(*str) {
    if(*str == ' ')
      str++;
    else if(*str >= '0' && *str <= '9') {
      char *e;
      int num = strtoull(str, &e, 0);
      if(!e || e == str)
        return -1;
      if(!*e || *e == ' ' || *e == 's' || *e == 'S')
        sec += num;
      else if(*e == 'm' || *e == 'M')
        sec += num*60;
      else if(*e == 'h' || *e == 'H')
        sec += num*3600;
      else if(*e == 'd' || *e == 'D')
        sec += num*3600*24;
      else
        return -1;
      str = e+1;
    } else
      return -1;
  }
  return sec;
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


// Perform a binary search on a GPtrArray, returning the index of the found
// item. The result is undefined if the array is not sorted according to `cmp'.
// Returns -1 when nothing is found.
int ptr_array_search(GPtrArray *a, gconstpointer v, GCompareFunc cmp) {
  if(!a->len)
    return -1;
  int b = 0;
  int e = a->len-1;
  while(b <= e) {
    int i = b + (e - b)/2;
    int r = cmp(g_ptr_array_index(a, i), v);
    if(r < 0) { // i < v, look into the upper half
      b = i+1;
    } else if(r > 0) { // i > v, look into the lower half
      e = i-1;
    } else // equivalent
      return i;
  }
  return -1;
}


// Adds an element to the array before the specified index. If i >= a->len, it
// will be appended to the array. This function preserves the order of the
// array: all elements after the specified index will be moved.
void ptr_array_insert_before(GPtrArray *a, int i, gpointer v) {
  if(i >= a->len) {
    g_ptr_array_add(a, v);
    return;
  }
  // add dummy element to make sure the array has the correct size. The value
  // will be overwritten in the memmove().
  g_ptr_array_add(a, NULL);
  memmove(a->pdata+i+1, a->pdata+i, sizeof(a->pdata)*(a->len-i-1));
  a->pdata[i] = v;
}


// Validates a hub name
gboolean is_valid_hubname(const char *name) {
  const char *tmp;
  int len = 0;
  for(tmp=name; *tmp; tmp = g_utf8_next_char(tmp))
    if(++len && !g_unichar_isalnum(g_utf8_get_char(tmp)))
      break;
  return !*tmp && len && len <= 25;
}


// Converts the "connection" setting into a speed in bytes/s, returns 0 on error.
guint64 connection_to_speed(const char *conn) {
  if(!conn)
    return 0;
  char *end;
  double val = strtod(conn, &end);
  // couldn't convert
  if(end == conn)
    return 0;
  // raw number, assume mbit/s
  if(!*end)
    return (val*1024.0*1024.0)/8.0;
  // KiB/s, assume KiB/s (heh)
  if(strcasecmp(end, "KiB/s") == 0 || strcasecmp(end, " KiB/s") == 0)
    return val*1024.0;
  // otherwise, no idea what to do with it
  return 0;
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


#if TLS_SUPPORT

// Calculates the SHA-256 digest of a certificate. This digest can be used for
// the KEYP ADC extension and general verification.
void certificate_sha256(GTlsCertificate *cert, char *digest) {
  GValue val = {};
  g_value_init(&val, G_TYPE_BYTE_ARRAY);
  g_object_get_property(G_OBJECT(cert), "certificate", &val);
  GByteArray *dat = g_value_get_boxed(&val);

  GChecksum *ctx = g_checksum_new(G_CHECKSUM_SHA256);
  g_checksum_update(ctx, dat->data, dat->len);
  gsize len = 32;
  g_checksum_get_digest(ctx, (guchar *)digest, &len);
  g_checksum_free(ctx);
  g_boxed_free(G_TYPE_BYTE_ARRAY, dat);
}

#endif


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



// Reads from fd until EOF and returns the number of lines found. Starts
// counting after the first '\n' if start is FALSE. Returns -1 on error.
static int file_count_lines(int fd) {
  char buf[1024];
  int n = 0;
  int r;
  while((r = read(fd, buf, 1024)) > 0)
    while(r--)
      if(buf[r] == '\n')
        n++;
  return r == 0 ? MAX(0, n) : r;
}


// Skips 'skip' lines and reads n lines from fd.
static char **file_read_lines(int fd, int skip, int n) {
  char buf[1024];
  // skip 'skip' lines
  int r = 0;
  while(skip > 0 && (r = read(fd, buf, 1024)) > 0) {
    int i;
    for(i=0; i<r; i++) {
      if(buf[i] == '\n' && !--skip) {
        r -= i+1;
        memmove(buf, buf+i+1, r);
        break;
      }
    }
  }
  if(r < 0)
    return NULL;
  // now read the rest of the lines
  char **res = g_new0(char *, n+1);
  int num = 0;
  GString *cur = g_string_sized_new(1024);
  do {
    char *tmp = buf;
    int left = r;
    char *sep;
    while(num < n && left > 0 && (sep = memchr(tmp, '\n', left)) != NULL) {
      int w = sep - tmp;
      g_string_append_len(cur, tmp, w);
      res[num++] = g_strdup(cur->str);
      g_string_assign(cur, "");
      left -= w+1;
      tmp += w+1;
    }
    g_string_append_len(cur, tmp, left);
  } while(num < n && (r = read(fd, buf, 1024)) > 0);
  g_string_free(cur, TRUE);
  if(r < 0) {
    g_strfreev(res);
    return NULL;
  }
  return res;
}


// Read the last n lines from a file and return them in a string array. The
// file must end with a newline, and only \n is recognized as one.  Returns
// NULL on error, with errno set. Can return an empty string array (result &&
// !*result). This isn't the fastest implementation available, but at least it
// does not have to read the entire file.
char **file_tail(const char *fn, int n) {
  if(n <= 0)
    return g_new0(char *, 1);

  int fd = open(fn, O_RDONLY);
  if(fd < 0)
    return NULL;
  int backbytes = n*128;
  off_t offset;
  while((offset = lseek(fd, -backbytes, SEEK_END)) != (off_t)-1) {
    int lines = file_count_lines(fd);
    if(lines < 0)
      return NULL;
    // not enough lines, try seeking back further
    if(offset > 0 && lines < n)
      backbytes *= 2;
    // otherwise, if we have enough lines seek again and fetch them
    else if(lseek(fd, offset, SEEK_SET) == (off_t)-1)
      return NULL;
    else
      return file_read_lines(fd, MAX(0, lines-n), MIN(lines+1, n));
  }

  // offset is -1 if we reach this. we may have been seeking to a negative
  // offset, so let's try from the beginning.
  if(errno == EINVAL) {
    if(lseek(fd, 0, SEEK_SET) == (off_t)-1)
      return NULL;
    int lines = file_count_lines(fd);
    if(lines < 0 || lseek(fd, 0, SEEK_SET) == (off_t)-1)
      return NULL;
    return file_read_lines(fd, MAX(0, lines-n), MIN(lines+1, n));
  }

  return NULL;
}




#if INTERFACE

// Tests whether a string is a valid base32-encoded string.
#define isbase32(s) (strspn(s, "abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ234567") == strlen(s))

// Test whether a string is a valid TTH hash. I.e., whether it is a
// base32-encoded 39-character string.
#define istth(s) (strlen(s) == 39 && isbase32(s))

#endif


// Generic base32 encoder.
// from[len] (binary) -> to[ceil(len*8/5)] (ascii)
void base32_encode_dat(const char *from, char *to, int len) {
  static char alphabet[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZ234567";
  int i, bits = 0, idx = 0, value = 0;
  for(i=0; i<len; i++) {
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


// from[24] (binary) -> to[39] (ascii - no padding zero will be added)
void base32_encode(const char *from, char *to) {
  base32_encode_dat(from, to, 24);
}


// from[n] (ascii) -> to[floor(n*5/8)] (binary)
// from must be zero-terminated.
void base32_decode(const char *from, char *to) {
  int bits = 0, idx = 0, value = 0;
  while(*from) {
    value = (value << 5) | (*from <= '9' ? (26+(*from-'2')) : *from-'A');
    bits += 5;
    while(bits >= 8) {
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
  unsigned char ipraw[4];
  if(sscanf(str, "%hhu.%hhu.%hhu.%hhu", ipraw, ipraw+1, ipraw+2, ipraw+3) != 4)
    return 0;
  guint32 ip;
  memcpy(&ip, ipraw, 4); // ip is now in network byte order
  return g_ntohl(ip); // convert to host byte order (allows for easy sorting)
}


// Returns a static string buffer.
char *ip4_unpack(guint32 ip) {
  // Don't use inet_ntoa(), not very portable on Solaris.
  static char buf[20];
  unsigned char ipraw[4];
  ip = g_htonl(ip);
  memcpy(ipraw, &ip, 4);
  g_snprintf(buf, 20, "%d.%d.%d.%d", ipraw[0], ipraw[1], ipraw[2], ipraw[3]);
  return buf;
}

#if INTERFACE

// don't use (a-b) here - the result may not fit in a signed integer
#define ip4_cmp(a, b) ((a) > (b) ? 1 : -1)

#endif




// Handy functions to create and read arbitrary data to/from byte arrays. Data
// is written to and read from a byte array sequentially. The data is stored as
// efficient as possible, bit still adds padding to correctly align some values.

// Usage:
//   GByteArray *a = g_byte_array_new();
//   darray_init(a);
//   darray_add_int32(a, 43);
//   darray_add_string(a, "blah");
//   char *v = g_byte_array_free(a, FALSE);
// ...later:
//   int number = darray_get_int32(v);
//   char *thestring = darray_get_string(v);
//   g_free(v);
//
// So it's basically a method to efficiently pass around variable arguments to
// functions without the restrictions imposed by stdarg.h.

#if INTERFACE

// For internal use
#define darray_append_pad(v, a)\
  int darray_pad = (((v)->len + (a)) & ~(a)) - (v)->len;\
  gint64 darray_zero = 0;\
  if(darray_pad)\
    g_byte_array_append(v, (guint8 *)&darray_zero, darray_pad)

// All values (not necessarily the v thing itself) are always evaluated once.
#define darray_add_int32(v, i)   do { guint32 darray_p=i; darray_append_pad(v, 3); g_byte_array_append(v, (guint8 *)&darray_p, 4); } while(0)
#define darray_add_int64(v, i)   do { guint64 darray_p=i; darray_append_pad(v, 7); g_byte_array_append(v, (guint8 *)&darray_p, 8); } while(0)
#define darray_add_ptr(v, p)     do { const void *darray_t=p; darray_append_pad(v, sizeof(void *)-1); g_byte_array_append(v, (guint8 *)&darray_t, sizeof(void *)); } while(0)
#define darray_add_dat(v, b, l)  do { int darray_i=l; darray_add_int32(v, darray_i); g_byte_array_append(v, (guint8 *)(b), darray_i); } while(0)
#define darray_add_string(v, s)  do { const char *darray_t=s; darray_add_dat(v, darray_t, strlen(darray_t)+1); } while(0)
#define darray_init(v)           darray_add_int32(v, 4)

#define darray_get_int32(v)      *((gint32 *)darray_get_raw(v, 4, 3))
#define darray_get_int64(v)      *((gint64 *)darray_get_raw(v, 8, 7))
#define darray_get_ptr(v)        *((void **)darray_get_raw(v, sizeof(void *), sizeof(void *)-1))
#define darray_get_string(v)     darray_get_raw(v, darray_get_int32(v), 0)
#endif


// For use by the macros
char *darray_get_raw(char *v, int i, int a) {
  int *d = (int *)v;
  d[0] += a;
  d[0] &= ~a;
  char *r = v + d[0];
  d[0] += i;
  return r;
}


char *darray_get_dat(char *v, int *l) {
  int n = darray_get_int32(v);
  if(l)
    *l = n;
  return darray_get_raw(v, n, 0);
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
  int sec = left / MAX(1, ratecalc_get(rc));
  return sec > 356*24*3600 ? "-" : str_formatinterval(sec);
}





// Log file writer. Prefixes all messages with a timestamp and allows the logs
// to be rotated.

#if INTERFACE

struct logfile {
  int file;
  char *path;
  struct stat st;
};

#endif


static GSList *logfile_instances = NULL;


// (Re-)opens the log file and checks for inode and file size changes.
static void logfile_checkfile(struct logfile *l) {
  // stat
  gboolean restat = l->file < 0;
  struct stat st;
  if(l->file >= 0 && stat(l->path, &st) < 0) {
    g_warning("Unable to stat log file '%s': %s. Attempting to re-create it.", l->path, g_strerror(errno));
    close(l->file);
    l->file = -1;
    restat = TRUE;
  }

  // if we have the log open, compare inode & size
  if(l->file >= 0 && (l->st.st_ino != st.st_ino || l->st.st_size > st.st_size)) {
    close(l->file);
    l->file = -1;
  }

  // if the log hadn't been opened or has been closed earlier, try to open it again
  if(l->file < 0)
    l->file = open(l->path, O_WRONLY|O_APPEND|O_CREAT, 0666);
  if(l->file < 0)
    g_warning("Unable to open log file '%s' for writing: %s", l->path, g_strerror(errno));

  // stat again if we need to
  if(l->file >= 0 && restat && stat(l->path, &st) < 0) {
    g_warning("Unable to stat log file '%s': %s. Closing.", l->path, g_strerror(errno));
    close(l->file);
    l->file = -1;
  }

  memcpy(&l->st, &st, sizeof(struct stat));
}


struct logfile *logfile_create(const char *name) {
  struct logfile *l = g_slice_new0(struct logfile);

  l->file = -1;
  char *n = g_strconcat(name, ".log", NULL);
  l->path = g_build_filename(db_dir, "logs", n, NULL);
  g_free(n);

  logfile_checkfile(l);
  logfile_instances = g_slist_prepend(logfile_instances, l);
  return l;
}


void logfile_free(struct logfile *l) {
  if(!l)
    return;
  logfile_instances = g_slist_remove(logfile_instances, l);
  if(l->file >= 0)
    close(l->file);
  g_free(l->path);
  g_slice_free(struct logfile, l);
}


void logfile_add(struct logfile *l, const char *msg) {
  logfile_checkfile(l);
  if(l->file < 0)
    return;

  time_t tm = time(NULL);
  char ts[50];
  strftime(ts, 49, "[%F %H:%M:%S %Z]", localtime(&tm));
  char *line = g_strdup_printf("%s %s\n", ts, msg);

  int len = strlen(line);
  int wr = 0;
  int r;
  while(wr < len && (r = write(l->file, line+wr, len-wr)) > 0)
    wr += r;

  g_free(line);
  if(r <= 0 && !strstr(msg, " (LOGERR)"))
    g_warning("Error writing to log file: %s (LOGERR)", g_strerror(errno));
}


// Flush and re-open all opened log files.
void logfile_global_reopen() {
  GSList *n = logfile_instances;
  for(; n; n=n->next) {
    struct logfile *l = n->data;
    if(l->file >= 0) {
      close(l->file);
      l->file = -1;
    }
    logfile_checkfile(l);
  }
}





// OS cache invalidation after reading data from a file. This is pretty much a
// wrapper around posix_fadvise(), but multiple sequential reads are bulked
// together in a single call to posix_fadvise(). This should work a lot better
// than calling the function multiple times on smaller chunks if the OS
// implementation works on page sizes internally.
//
// Usage:
//   int fd = open(..);
//   struct fadv a;
//   fadv_init(&a, fd, offset);
//   while((int len = read(..)) > 0)
//     fadv_purge(&a, len);
//   fadv_close(&a);
//   close(fd);
//
// These functions are thread-safe, as long as they are not used on the same
// struct from multiple threads at the same time.

#if INTERFACE

struct fadv {
  int fd;
  int chunk;
  guint64 offset;
};

#ifdef HAVE_POSIX_FADVISE

#define fadv_init(a, f, o) do {\
    (a)->fd = f;\
    (a)->chunk = 0;\
    (a)->offset = o;\
  } while(0)

#define fadv_close(a) fadv_purge(a, -1)

#else // HAVE_POSIX_FADVISE

// Some pointless assignments to make sure the compiler doesn't complain about
// unused variables.
#define fadv_init(a, f, o) ((a)->fd = 0)
#define fadv_purge(a, l)   ((a)->fd = 0)
#define fadv_close(a)      ((a)->fd = 0)

#endif

#endif

// Enable/disable calling of posix_fadvise(). Use g_atomic_int_() functions to
// read/write this variable!
int fadv_enabled = 0;


#ifdef HAVE_POSIX_FADVISE

// call with length = -1 to force a flush
void fadv_purge(struct fadv *a, int length) {
  if(length > 0)
    a->chunk += length;
  // flush every 5MB. Some magical value, don't think too much into it.
  if(g_atomic_int_get(&fadv_enabled) && (a->chunk > 5*1024*1024 || (length < 0 && a->chunk > 0))) {
    posix_fadvise(a->fd, a->offset, a->chunk, POSIX_FADV_DONTNEED);
    a->offset += a->chunk;
    a->chunk = 0;
  }
}

#endif

