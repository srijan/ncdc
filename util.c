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
#include <string.h>
#include <glib/gstdio.h>
#include <sys/file.h>


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
const char *conf_dir;
GKeyFile *conf_file;


#if INTERFACE

#define conf_hub_get(type, name, key) (\
  g_key_file_has_key(conf_file, name, (key), NULL)\
    ? g_key_file_get_##type(conf_file, name, (key), NULL)\
    : g_key_file_get_##type(conf_file, "global", (key), NULL))

#endif


void conf_init() {
  // get location of the configuration directory
  conf_dir = g_getenv("NCDC_DIR");
  if(!conf_dir)
    conf_dir = g_build_filename(g_get_home_dir(), ".ncdc", NULL);

  // try to create it (ignoring errors if it already exists)
  g_mkdir(conf_dir, 0700);
  if(g_access(conf_dir, F_OK | R_OK | X_OK | W_OK) < 0)
    g_error("Directory '%s' does not exist or is not writable.", conf_dir);

  // we should also have a logs/ subdirectory
  char *logs = g_build_filename(conf_dir, "logs", NULL);
  g_mkdir(logs, 0777);
  if(g_access(conf_dir, F_OK | R_OK | X_OK | W_OK) < 0)
    g_error("Directory '%s' does not exist or is not writable.", logs);
  g_free(logs);

  // make sure that there is no other ncdc instance working with the same config directory
  char *lock_file = g_build_filename(conf_dir, "lock", NULL);
  int lock_fd = g_open(lock_file, O_WRONLY|O_CREAT, 0600);
  if(lock_fd < 0 || flock(lock_fd, LOCK_EX|LOCK_NB))
    g_error("Unable to open lock file. Is another instance of ncdc running with the same configuration directory?");
  // Don't close the above file. Keep it open and let the OS close it (and free
  // the lock) when ncdc is closed, was killed or has crashed.

  // load config file (or create it)
  conf_file = g_key_file_new();
  char *cf = g_build_filename(conf_dir, "config.ini", NULL);
  GError *err = NULL;
  if(g_file_test(cf, G_FILE_TEST_EXISTS)) {
    if(!g_key_file_load_from_file(conf_file, cf, G_KEY_FILE_KEEP_COMMENTS, &err))
      g_error("Could not load '%s': %s", cf, err->message);
  }
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
  conf_save();
  g_free(cf);
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
 * conversion, nor has it been thouroughly tested.
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
      g_assert_not_reached();
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
  sprintf(dat, "%6.2f %c%cB", r, c, c == ' ' ? ' ' : 'i');
  return dat;
}

