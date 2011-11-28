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


#include "config.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <stdarg.h>

#include <locale.h>
#include <unistd.h>
#include <fcntl.h>

#include <glib.h>
#include <glib/gstdio.h>
#include <sqlite3.h>
#include <gdbm.h>
#include <bzlib.h>
#include <libxml/xmlreader.h>


static const char *db_dir = NULL;
static int db_verfd = -1;



// Handly utility functions

static void confirm(const char *msg, ...) {
  va_list va;
  va_start(va, msg);
  vprintf(msg, va);
  va_end(va);
  fputs("\n\nContinue? (y/N): ", stdout);

  char reply[20];
  char *r = fgets(reply, 20, stdin);
  if(!r || (strcasecmp(r, "y\n") != 0 && strcasecmp(r, "yes\n") != 0)) {
    puts("Aborted.");
    exit(0);
  }
}


static void base32_encode(const char *from, char *to, int len) {
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


guint64 rand_64() {
  GRand *r = g_rand_new();
  guint32 r1 = g_rand_int(r);
  g_rand_free(r);
  r = g_rand_new();
  g_rand_set_seed(r, g_rand_int(r));
  guint32 r2 = g_rand_int(r);
  g_rand_free(r);
  return (((guint64)r1)<<32) + r2;
}


#define isbase32(s) (strspn(s, "abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ234567") == strlen(s))
#define istth(s) (strlen(s) == 39 && isbase32(s))



// Sets db_dir and returns its current version
static int db_getversion() {
  // get location of ncdc's directory
  if(!db_dir && (db_dir = g_getenv("NCDC_DIR")))
    db_dir = g_strdup(db_dir);
  if(!db_dir)
    db_dir = g_build_filename(g_get_home_dir(), ".ncdc", NULL);

  if(g_access(db_dir, F_OK | R_OK | X_OK | W_OK) < 0) {
    fprintf(stderr, "Directory '%s' does not exist or is not writable.\n", db_dir);
    exit(1);
  }
  printf("Using directory: %s\n", db_dir);

  // get database version and check that the dir isn't locked
  char *ver_file = g_build_filename(db_dir, "version", NULL);
  db_verfd = g_open(ver_file, O_RDWR, 0600);

  struct flock lck;
  lck.l_type = F_WRLCK;
  lck.l_whence = SEEK_SET;
  lck.l_start = 0;
  lck.l_len = 0;
  if(db_verfd < 0 || fcntl(db_verfd, F_SETLK, &lck) == -1) {
    fprintf(stderr, "Unable to open lock file. Please make sure that no other instance of ncdc is running with the same configuration directory.\n");
    exit(1);
  }

  // get version
  unsigned char dir_ver[2];
  if(read(db_verfd, dir_ver, 2) < 2)
    g_error("Could not read version information from '%s': %s", ver_file, g_strerror(errno));
  g_free(ver_file);
  return (dir_ver[0] << 8) + dir_ver[1];
}






// Upgrades the directory from 1.0 to 2.0

static char *u20_sql_fn;
static char *u20_hashdat_fn;
static char *u20_dl_fn;
static char *u20_files_fn;
static char *u20_config_fn;
static GKeyFile *u20_conf;
static sqlite3 *u20_sql;

static void u20_revert(const char *msg, ...) {
  puts(" error.");
  puts("");
  va_list va;
  va_start(va, msg);
  vprintf(msg, va);
  va_end(va);

  puts("");
  fputs("-- Reverting changes...", stdout);
  fflush(stdout);

  // clean up
  unlink(u20_sql_fn);

  puts(" done.");
  exit(1);
}




// Reads files.xml.bz2 and creates a hash table with TTH as keys and a
// GPtrArray of strings (absolute paths) as values.

static GHashTable *u20_filenames = NULL;


static int u20_loadfiles_input(void *context, char *buf, int len) {
  static int stream_end = 0;
  if(stream_end)
    return 0;
  int bzerr;
  int r = BZ2_bzRead(&bzerr, (BZFILE *)context, buf, len);
  if(bzerr != BZ_OK && bzerr != BZ_STREAM_END)
    u20_revert("bzip2 decompression error. (%d)", bzerr);
  stream_end = bzerr == BZ_STREAM_END;
  return r;
}


static void u20_loadfiles_err(void *arg, const char *msg, xmlParserSeverities severity, xmlTextReaderLocatorPtr locator) {
  if(severity == XML_PARSER_SEVERITY_VALIDITY_WARNING || severity == XML_PARSER_SEVERITY_WARNING)
    printf("XML parse warning on line %d: %s", xmlTextReaderLocatorLineNumber(locator), msg);
  else
    u20_revert("XML parse error on input line %d: %s", xmlTextReaderLocatorLineNumber(locator), msg);
}


static gint u20_loadfiles_equal(gconstpointer a, gconstpointer b) {
  return memcmp(a, b, 24) == 0 ? TRUE : FALSE;
}


static void u20_loadfiles_handle(xmlTextReaderPtr reader, char **root, char **path) {
  char name[50], *tmp, *tmp2;

  tmp = (char *)xmlTextReaderName(reader);
  strncpy(name, tmp, 50);
  name[49] = 0;
  free(tmp);

  switch(xmlTextReaderNodeType(reader)) {
  case XML_READER_TYPE_ELEMENT:
    // <Directory ..>
    if(strcmp(name, "Directory") == 0) {
      if(!(tmp = (char *)xmlTextReaderGetAttribute(reader, (xmlChar *)"Name")))
        u20_revert("<Directory> element found without `Name' attribute.");
      if(!*root) {
        char **names = g_key_file_get_keys(u20_conf, "share", NULL, NULL);
        int i, len = names ? g_strv_length(names) : 0;
        for(i=0; i<len; i++) {
          if(strcmp(names[i], tmp) == 0)
            *root = g_key_file_get_string(u20_conf, "share", names[i], NULL);
        }
        g_strfreev(names);
        if(*root)
          *path = g_strdup("/");
        else
          g_warning("%s", tmp);
      } else {
        tmp2 = g_build_path("/", *path, tmp, NULL);
        g_free(*path);
        *path = tmp2;
      }
      free(tmp);

    // <File .. />
    } else if(strcmp(name, "File") == 0 && *root) {

      if(!(tmp2 = (char *)xmlTextReaderGetAttribute(reader, (xmlChar *)"Name")))
        u20_revert("<File> element found without `Name' attribute.");
      tmp = g_build_path("/", *root, *path, tmp2, NULL);
      free(tmp2);
      tmp2 = g_filename_from_utf8(tmp, -1, NULL, NULL, NULL);
      g_free(tmp);
      tmp = realpath(tmp2, NULL);
      g_free(tmp2);
      tmp2 = tmp ? g_filename_to_utf8(tmp, -1, NULL, NULL, NULL) : NULL;
      if(tmp)
        free(tmp);

      tmp = (char *)xmlTextReaderGetAttribute(reader, (xmlChar *)"TTH");
      if(!tmp || !istth(tmp))
        u20_revert("<File> element found with invalid or missing `TTH' attribute.");
      char enc[24];
      base32_decode(tmp, enc);
      free(tmp);

      // add to hash table
      if(tmp2) {
        GPtrArray *a = g_hash_table_lookup(u20_filenames, enc);
        if(a)
          g_ptr_array_add(a, tmp2);
        else {
          a = g_ptr_array_new_with_free_func(g_free);
          g_ptr_array_add(a, tmp2);
          g_hash_table_insert(u20_filenames, g_memdup(enc, 24), a);
        }
      }
    }
    break;

  case XML_READER_TYPE_END_ELEMENT:
    // </Directory>
    if(strcmp(name, "Directory") == 0 && *root) {
      if(strcmp(*path, "/") == 0) {
        g_free(*path);
        g_free(*root);
        *path = *root = NULL;
      } else {
        tmp = strrchr(*path, '/');
        *(tmp == *path ? tmp+1 : tmp) = 0;
      }
    }
    break;
  }
}


static void u20_loadfiles() {
  printf("-- Scanning share...");
  fflush(stdout);

  // Open file
  FILE *f = fopen(u20_files_fn, "r");
  if(!f)
    u20_revert("Error opening file list: %s", strerror(errno));

  BZFILE *bzf = NULL;
  int bzerr;
  bzf = BZ2_bzReadOpen(&bzerr, f, 0, 0, NULL, 0);
  if(bzerr != BZ_OK)
    u20_revert("Error opening bz2 stream (%d)", bzerr);

  // create reader
  xmlTextReaderPtr reader = xmlReaderForIO(u20_loadfiles_input, NULL, bzf, NULL, NULL, XML_PARSE_NOENT);
  if(!reader)
    u20_revert("Error opening XML stream.");
  xmlTextReaderSetErrorHandler(reader, u20_loadfiles_err, NULL);

  // start reading
  u20_filenames = g_hash_table_new_full(g_int64_hash, u20_loadfiles_equal, g_free, (GDestroyNotify)g_ptr_array_unref);
  char *root = NULL;
  char *path = NULL;
  int ret;
  while((ret = xmlTextReaderRead(reader)) == 1)
    u20_loadfiles_handle(reader, &root, &path);
  g_free(root);
  g_free(path);

  if(ret < 0)
    u20_revert("Error reading XML stream.");

  // close
  xmlFreeTextReader(reader);
  BZ2_bzReadClose(&bzerr, bzf);
  fclose(f);

  printf(" %d unique files found.\n", g_hash_table_size(u20_filenames));
}




static void u20_initsqlite() {
  printf("-- Creating `%s'...", u20_sql_fn);
  fflush(stdout);

  if(sqlite3_open(u20_sql_fn, &u20_sql))
    u20_revert("%s", sqlite3_errmsg(u20_sql));

  char *err = NULL;
  if(sqlite3_exec(u20_sql,
      "PRAGMA user_version = 1;"

      "CREATE TABLE hashdata ("
      "  root TEXT NOT NULL PRIMARY KEY,"
      "  size INTEGER NOT NULL,"
      "  tthl BLOB NOT NULL"
      ");"

      "CREATE TABLE hashfiles ("
      "  id INTEGER PRIMARY KEY,"
      "  filename TEXT NOT NULL UNIQUE,"
      "  tth TEXT NOT NULL,"
      "  lastmod INTEGER NOT NULL"
      ");"

      "CREATE TABLE dl ("
      "  tth TEXT NOT NULL PRIMARY KEY,"
      "  size INTEGER NOT NULL,"
      "  dest TEXT NOT NULL,"
      "  priority INTEGER NOT NULL DEFAULT 0,"
      "  error INTEGER NOT NULL DEFAULT 0,"
      "  error_msg TEXT,"
      "  tthl BLOB"
      ");"

      "CREATE TABLE dl_users ("
      "  tth TEXT NOT NULL,"
      "  uid INTEGER NOT NULL,"
      "  error INTEGER NOT NULL DEFAULT 0,"
      "  error_msg TEXT,"
      "  PRIMARY KEY(tth, uid)"
      ");"

      "CREATE TABLE share ("
      "  name TEXT NOT NULL PRIMARY KEY,"
      "  path TEXT NOT NULL"
      ");"

      "CREATE TABLE vars ("
      "  name TEXT NOT NULL,"
      "  hub INTEGER NOT NULL DEFAULT 0,"
      "  value TEXT NOT NULL,"
      "  PRIMARY KEY(name, hub)"
      ");"

    , NULL, NULL, &err))
    u20_revert("%s", err?err:sqlite3_errmsg(u20_sql));

  puts(" done.");
}


static void u20_hashdata_item(GDBM_FILE dat, datum key, sqlite3_stmt *data, sqlite3_stmt *files) {
  char *k = key.dptr;

  char hash[40] = {};
  base32_encode(k+1, hash, 24);
  sqlite3_bind_text(data, 1, hash, -1, SQLITE_STATIC);
  sqlite3_bind_text(files, 1, hash, -1, SQLITE_STATIC);

  // fetch info
  datum res = gdbm_fetch(dat, key);
  if(res.dsize != 3*8) {
    printf("WARNING: Invalid TTH data for `%s' - ignoring.\n", hash);
    if(res.dsize > 0)
      free(res.dptr);
    return;
  }

  // lastmod, filesize, blocksize (ignored)
  guint64 *r = (guint64 *)res.dptr;
  sqlite3_bind_int64(files, 2, GINT64_FROM_LE(r[0]));
  sqlite3_bind_int64(data, 2, GINT64_FROM_LE(r[1]));
  free(res.dptr);

  // fetch tthl data
  k[0] = 1;
  res = gdbm_fetch(dat, key);
  k[0] = 0;
  if(res.dsize < 0) {
    printf("WARNING: Invalid TTH data for `%s' - ignoring.\n", hash);
    if(res.dsize > 0)
      free(res.dptr);
    return;
  }

  GPtrArray *a = g_hash_table_lookup(u20_filenames, k+1);
  if(!a) {
    printf("WARNING: No file found for `%s' - ignoring.\n", hash);
    free(res.dptr);
    return;
  }

  // let sqlite3 free the data when it's done with it.
  sqlite3_bind_blob(data, 3, res.dptr, res.dsize, free);
  if(sqlite3_step(data) != SQLITE_DONE || sqlite3_reset(data))
    u20_revert("%s", sqlite3_errmsg(u20_sql));

  int i;
  for(i=0; i<a->len; i++) {
    sqlite3_bind_text(files, 3, g_ptr_array_index(a, i), -1, SQLITE_STATIC);
    if(sqlite3_step(files) != SQLITE_DONE || sqlite3_reset(files))
      u20_revert("%s", sqlite3_errmsg(u20_sql));
  }
}


// TODO: The `done' flag in hashdata.dat should also be transferred to somewhere.
static void u20_hashdata() {
  printf("-- Converting hashdata.dat...");
  fflush(stdout);

  GDBM_FILE dat = gdbm_open(u20_hashdat_fn, 0, GDBM_READER, 0600, NULL);
  if(!dat)
    u20_revert("%s", gdbm_strerror(gdbm_errno));

  sqlite3_stmt *data, *files;
  if(sqlite3_prepare_v2(u20_sql, "INSERT INTO hashdata (root, size, tthl) VALUES(?, ?, ?)", -1, &data, NULL))
    u20_revert("%s", sqlite3_errmsg(u20_sql));
  if(sqlite3_prepare_v2(u20_sql, "INSERT INTO hashfiles (tth, lastmod, filename) VALUES(?, ?, ?)", -1, &files, NULL))
    u20_revert("%s", sqlite3_errmsg(u20_sql));

  // walk through the keys
  datum key = gdbm_firstkey(dat);
  char *freethis = NULL;
  for(; key.dptr; key=gdbm_nextkey(dat, key)) {
    if(freethis)
      free(freethis);
    freethis = key.dptr;

    // Only check for INFO keys
    if(key.dsize != 25 || *((char *)key.dptr) != 0)
      continue;
    u20_hashdata_item(dat, key, data, files);
  }

  if(freethis)
    free(freethis);

  if(sqlite3_finalize(data) || sqlite3_finalize(files))
    u20_revert("%s", sqlite3_errmsg(u20_sql));

  gdbm_close(dat);

  puts(" done.");
}


#define DLDAT_INFO  0 // <8 bytes: size><1 byte: prio><1 byte: error><2 bytes: error_sub>
                      // <4 bytes: reserved><zero-terminated-string: destination>
#define DLDAT_USERS 1 // <8 bytes: amount>
                      // <8 bytes: uid><1 byte: reserved><1 byte: error><2 bytes: error_sub><4 bytes: reserved>
                      // Repeat previous line $amount times
                      // For ncdc <= 1.2: amount=1 and only the 8-byte uid was present
#define DLDAT_TTHL  2 // <24 bytes: hash1>..


#define DLE_NONE    0 // No error
#define DLE_INVTTHL 1 // TTHL data does not match the file root
#define DLE_NOFILE  2 // User does not have the file at all
#define DLE_IO_INC  3 // I/O error with incoming file, error_sub = errno
#define DLE_IO_DEST 4 // I/O error when moving to destination file/dir
#define DLE_HASH    5 // Hash check failed, error_sub = block index of failed hash


static char *u20_dl_strerror(char err, unsigned short sub) {
  static char buf[200];
  switch(err) {
    case DLE_NONE:    strcpy(buf, "No error."); break;
    case DLE_INVTTHL: strcpy(buf, "TTHL data does not match TTH root."); break;
    case DLE_NOFILE:  strcpy(buf, "File not available from this user."); break;
    case DLE_IO_INC:  g_snprintf(buf, 200, "Error writing to temporary file: %s", g_strerror(sub)); break;
    case DLE_IO_DEST:
      if(!sub)
        strcpy(buf, "Error moving file to destination.");
      else
        g_snprintf(buf, 200, "Error moving file to destination: %s", g_strerror(sub));
      break;
    case DLE_HASH:    g_snprintf(buf, 200, "Hash chunk %d does not match downloaded data.", sub); break;
    default:          strcpy(buf, "Unknown error.");
  }
  return buf;
}


static void u20_dl_item(GDBM_FILE dat, datum key, sqlite3_stmt *dl, sqlite3_stmt *dlu) {
  char *k = key.dptr;

  char hash[40] = {};
  base32_encode(k+1, hash, 24);
  sqlite3_bind_text(dl, 1, hash, -1, SQLITE_STATIC);
  sqlite3_bind_text(dlu, 1, hash, -1, SQLITE_STATIC);

  // fetch info
  datum res = gdbm_fetch(dat, key);
  if(res.dsize < 17) {
    printf("WARNING: Invalid DL data for `%s' - ignoring.\n", hash);
    if(res.dsize > 0)
      free(res.dptr);
    return;
  }
  // size
  sqlite3_bind_int64(dl, 2, GINT64_FROM_LE(*((guint64 *)res.dptr)));
  // dest
  sqlite3_bind_text(dl, 3, ((char *)res.dptr)+16, -1, SQLITE_TRANSIENT);
  // priority
  sqlite3_bind_int(dl, 4, *(((char *)res.dptr)+8));
  // error
  char err = *(((char *)res.dptr)+9);
  sqlite3_bind_int(dl, 5, err);
  // error_msg
  if(!err)
    sqlite3_bind_null(dl, 6);
  else
    sqlite3_bind_text(dl, 6, u20_dl_strerror(err, GINT16_FROM_LE(*(((guint16 *)res.dptr)+5))), -1, SQLITE_STATIC);
  free(res.dptr);

  // fetch tthl
  k[0] = DLDAT_TTHL;
  res = gdbm_fetch(dat, key);
  if(res.dsize > 0)
    sqlite3_bind_blob(dl, 7, res.dptr, res.dsize, free);
  else
    sqlite3_bind_null(dl, 7);

  // insert dl item
  if(sqlite3_step(dl) != SQLITE_DONE || sqlite3_reset(dl))
    u20_revert("%s", sqlite3_errmsg(u20_sql));

  // now fetch the users
  k[0] = DLDAT_USERS;
  res = gdbm_fetch(dat, key);
  k[0] = DLDAT_INFO;
  if(res.dsize < 16) {
    if(res.dsize > 0)
      free(res.dptr);
    return;
  }
  guint64 num = *((guint64 *)res.dptr);
  int i;
  for(i=0; i<num; i++) {
    char *ptr = res.dptr+8+16*i;
    // uid
    sqlite3_bind_int64(dlu, 2, GINT64_FROM_LE(*((guint64 *)ptr)));
    // error, error_msg
    if(res.dsize > 16) { // post-multisource
      char err = ptr[9];
      sqlite3_bind_int(dlu, 3, err);
      if(!err)
        sqlite3_bind_null(dlu, 4);
      else
        sqlite3_bind_text(dlu, 4, u20_dl_strerror(err, GINT16_FROM_LE(*(((guint16 *)ptr)+5))), -1, SQLITE_STATIC);
    } else { // pre-multisource
      sqlite3_bind_int(dlu, 3, 0);
      sqlite3_bind_null(dlu, 4);
    }
    // insert dlu item
    if(sqlite3_step(dlu) != SQLITE_DONE || sqlite3_reset(dlu))
      u20_revert("%s", sqlite3_errmsg(u20_sql));
  }
  free(res.dptr);
}


static void u20_dl() {
  printf("-- Converting dl.dat...");
  fflush(stdout);

  GDBM_FILE dat = gdbm_open(u20_dl_fn, 0, GDBM_READER, 0600, NULL);
  if(!dat)
    u20_revert("%s", gdbm_strerror(gdbm_errno));

  sqlite3_stmt *dl, *dlu;
  if(sqlite3_prepare_v2(u20_sql,
      "INSERT INTO dl (tth, size, dest, priority, error, error_msg, tthl)"
      " VALUES(?, ?, ?, ?, ?, ?, ?)", -1, &dl, NULL))
    u20_revert("%s", sqlite3_errmsg(u20_sql));
  if(sqlite3_prepare_v2(u20_sql,
      "INSERT INTO dl_users (tth, uid, error, error_msg) VALUES(?, ?, ?, ?)", -1, &dlu, NULL))
    u20_revert("%s", sqlite3_errmsg(u20_sql));

  // walk through the keys
  datum key = gdbm_firstkey(dat);
  char *freethis = NULL;
  for(; key.dptr; key=gdbm_nextkey(dat, key)) {
    if(freethis)
      free(freethis);
    freethis = key.dptr;

    // Only check for INFO keys
    if(key.dsize != 25 || *((char *)key.dptr) != DLDAT_INFO)
      continue;
    u20_dl_item(dat, key, dl, dlu);
  }

  if(freethis)
    free(freethis);

  if(sqlite3_finalize(dl) || sqlite3_finalize(dlu))
    u20_revert("%s", sqlite3_errmsg(u20_sql));

  gdbm_close(dat);

  puts(" done.");
};


static void u20_config_group(const char *group, sqlite3_stmt *s) {
  gint64 id = 0;
  // This is a hub group
  if(*group == '#') {
    // Get or create hubid
    char *tmp = g_key_file_get_string(u20_conf, group, "hubid", NULL);
    id = tmp ? (gint64)g_ascii_strtoull(tmp, NULL, 10) : 0;
    if(!id)
      id = (gint64)rand_64();
    g_free(tmp);
    // set hubname
    sqlite3_bind_text(s, 1, "hubname", -1, SQLITE_STATIC);
    sqlite3_bind_int64(s, 2, id);
    sqlite3_bind_text(s, 3, group, -1, SQLITE_STATIC);
    if(sqlite3_step(s) != SQLITE_DONE || sqlite3_reset(s))
      u20_revert("%s", sqlite3_errmsg(u20_sql));

  // Ignore groups we don't know
  } else if(strcmp(group, "global") != 0 && strcmp(group, "log") != 0 && strcmp(group, "color") != 0)
    return;

  // Convert the keys
  sqlite3_bind_int64(s, 2, id);
  char **keys = g_key_file_get_keys(u20_conf, group, NULL, NULL);
  char **key = keys;
  for(; key&&*key; key++) {
    // Ignore `hubid'
    if(strcmp(*key, "hubid") == 0)
      continue;
    // Get value and convert
    char *v = g_key_file_get_string(u20_conf, group, *key, NULL);
    sqlite3_bind_text(s, 1, *key, -1, SQLITE_STATIC);
    sqlite3_bind_text(s, 3, v, -1, SQLITE_STATIC);
    if(sqlite3_step(s) != SQLITE_DONE || sqlite3_reset(s))
      u20_revert("%s", sqlite3_errmsg(u20_sql));
    g_free(v);
  }
  g_strfreev(keys);
}


static void u20_config() {
  printf("-- Converting configuration...");
  fflush(stdout);

  // Convert [share]
  sqlite3_stmt *s;
  if(sqlite3_prepare_v2(u20_sql,
      "INSERT INTO share (name, path) VALUES(?, ?)", -1, &s, NULL))
    u20_revert("%s", sqlite3_errmsg(u20_sql));
  char **dirs = g_key_file_get_keys(u20_conf, "share", NULL, NULL);
  char **dir;
  for(dir=dirs; dirs && *dir; dir++) {
    char *d = g_key_file_get_string(u20_conf, "share", *dir, NULL);
    if(!d)
      continue;
    sqlite3_bind_text(s, 1, *dir, -1, SQLITE_STATIC);
    sqlite3_bind_text(s, 2, d, -1, SQLITE_STATIC);
    if(sqlite3_step(s) != SQLITE_DONE || sqlite3_reset(s))
      u20_revert("%s", sqlite3_errmsg(u20_sql));
    g_free(d);
  }
  g_strfreev(dirs);
  if(sqlite3_finalize(s))
    u20_revert("%s", sqlite3_errmsg(u20_sql));

  // vars
  if(sqlite3_prepare_v2(u20_sql,
      "INSERT INTO vars (name, hub, value) VALUES(?, ?, ?)", -1, &s, NULL))
    u20_revert("%s", sqlite3_errmsg(u20_sql));

  char **groups = g_key_file_get_groups(u20_conf, NULL);
  char **group = groups;
  for(; group&&*group; group++)
    u20_config_group(*group, s);
  g_strfreev(groups);

  if(sqlite3_finalize(s))
    u20_revert("%s", sqlite3_errmsg(u20_sql));

  puts(" done.");
}


static void u20_final() {
  printf("-- Finalizing...");
  fflush(stdout);

  char *er;
  if(sqlite3_exec(u20_sql, "COMMIT", NULL, NULL, &er))
    u20_revert("%s", er?er:sqlite3_errmsg(u20_sql));

  if(sqlite3_close(u20_sql))
    u20_revert("%s", sqlite3_errmsg(u20_sql));

  // TODO: update version and unlink old files

  puts(" done.");
}


static void u20() {
  u20_sql_fn     = g_build_filename(db_dir, "db.sqlite3", NULL);
  u20_hashdat_fn = g_build_filename(db_dir, "hashdata.dat", NULL);
  u20_dl_fn      = g_build_filename(db_dir, "dl.dat", NULL);
  u20_files_fn   = g_build_filename(db_dir, "files.xml.bz2", NULL);
  u20_config_fn  = g_build_filename(db_dir, "config.ini", NULL);

  u20_conf = g_key_file_new();
  GError *err = NULL;
  if(!g_key_file_load_from_file(u20_conf, u20_config_fn, G_KEY_FILE_KEEP_COMMENTS, &err))
    u20_revert("Could not load `%s': %s", u20_config_fn, err->message);

  u20_loadfiles();
  u20_initsqlite();

  // Start a transaction (committed in _final())
  char *er = NULL;
  if(sqlite3_exec(u20_sql, "BEGIN EXCLUSIVE TRANSACTION", NULL, NULL, &er))
    u20_revert("%s", er?er:sqlite3_errmsg(u20_sql));

  u20_hashdata();
  u20_dl();
  u20_config();
  u20_final();

  g_key_file_free(u20_conf);
  g_hash_table_unref(u20_filenames);
  g_free(u20_sql_fn);
  g_free(u20_hashdat_fn);
  g_free(u20_dl_fn);
  g_free(u20_files_fn);
  g_free(u20_config_fn);
}






// The main program

static gboolean print_version(const gchar *name, const gchar *val, gpointer dat, GError **err) {
  printf("ncdc-db-upgrade %s\n", VERSION);
  exit(0);
}

static GOptionEntry cli_options[] = {
  { "version", 'v', G_OPTION_FLAG_NO_ARG, G_OPTION_ARG_CALLBACK, print_version,
      "Print version and compilation information.", NULL },
  { "session-dir", 'c', 0, G_OPTION_ARG_FILENAME, &db_dir,
      "Use a different session directory. Default: `$NCDC_DIR' or `$HOME/.ncdc'.", "<dir>" },
  { NULL }
};


int main(int argc, char **argv) {
  setlocale(LC_ALL, "");

  // parse commandline options
  GOptionContext *optx = g_option_context_new(" - Ncdc Database Upgrade Utility");
  g_option_context_add_main_entries(optx, cli_options, NULL);
  GError *err = NULL;
  if(!g_option_context_parse(optx, &argc, &argv, &err)) {
    puts(err->message);
    exit(1);
  }
  g_option_context_free(optx);

  // not finished...
  confirm(
    "*WARNING*: This utility is not finished yet! You WILL screw up your\n"
    "session directory if you run this program now. Don't do this unless\n"
    "you know what you're doing!"
  );

  // get version
  int ver = db_getversion();
  printf("Detected version: %d.%d (%s)\n", ver>>8, ver&0xFF, (ver>>8)<=1 ? "ncdc 1.5 or earlier" : "ncdc 1.6 or later");

  // TODO: There is a nasty situation that occurs when ncdc 1.4 or earlier is
  // run on a version 2 directory - in this case both db.sqlite3 and the old
  // hashdata.dat and dl.dat are present, and 'version' will be 1. This should
  // be detected.
  if((ver>>8) == 2) {
    printf("Database already updated to the latest version.\n");
    exit(1);
  }
  if((ver>>8) > 2) {
    printf("Error: unrecognized database version. You should probably upgrade this utility.\n");
    exit(1);
  }

  // We've now determined that we have a version 1 directory, ask whether we can upgrade this.
  confirm("\n"
    "The directory will be upgraded for use with ncdc 1.6 or later. This\n"
    "action is NOT reversible! You are encouraged to make a backup of the\n"
    "directory, so that you can revert back to an older version in case\n"
    "something goes wrong."
  );
  u20();

  return 0;
}

