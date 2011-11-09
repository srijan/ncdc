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
#include <sqlite3.h>

// TODO: The conf_* stuff in util.c should eventually be merged into this file as well.

// All of the db_* functions can be used from multiple threads, the database is
// protected by one master lock. The only exception to this is db_init().
// Note that, even though sqlite may do locking internally in SERIALIZED mode,
// ncdc still performs manual locks around all database calls, in order to get
// reliable values from sqlite3_errmsg(). Prepared statements are not visible
// outside of a single transaction, since I don't know how well those work in a
// multithreaded environment.

static sqlite3 *db = NULL;


void db_init() {
  char *dbfn = g_build_filename(conf_dir, "db.sqlite3", NULL);

  if(!sqlite3_threadsafe())
    g_error("sqlite3 has not been compiled with threading support. Please recompile sqlite3 with SQLITE_THREADSAFE enabled.");

  // TODO: should look at the version file instead. This check is simply for debugging purposes.
  gboolean newdb = !g_file_test(dbfn, G_FILE_TEST_EXISTS);
  if(newdb)
    g_error("No db.sqlite3 file present yet. Please run ncdc-db-upgrade.");

  if(sqlite3_open(dbfn, &db))
    g_error("Couldn't open `%s': %s", dbfn, sqlite3_errmsg(db));
  g_free(dbfn);

  sqlite3_busy_timeout(db, 10);

  sqlite3_exec(db, "PRAGMA foreign_keys = FALSE", NULL, NULL, NULL);

  // TODO: create SQL schema, if it doesn't exist yet
}


// Note: db may be NULL after a db_lock(), this happens when ncdc is shutting down.
#define db_lock()   sqlite3_mutex_enter(sqlite3_db_mutex(db))
#define db_unlock() sqlite3_mutex_leave(sqlite3_db_mutex(db))

#define db_err(msg, x) do {\
    g_critical("%s:%d: SQLite3 error: %s", __FILE__, __LINE__, (msg)?(msg):sqlite3_errmsg(db));\
    db_unlock();\
    return x;\
  } while(0)


void db_close() {
  sqlite3 *d = db;
  db_lock();
  db = NULL;
  sqlite3_mutex_leave(sqlite3_db_mutex(d));
  sqlite3_close(d);
}

