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
#include <stdarg.h>

// TODO: The conf_* stuff in util.c should eventually be merged into this file as well.

// All of the db_* functions can be used from multiple threads. The database is
// only accessed from within the database thread (db_thread_func()). All access
// to the database from other threads is performed via message passing.
//
// Some properties of this implementation:
// - Multiple UPDATE/DELETE/INSERT statements in a short interval are grouped
//   together in a single transaction.
// - All queries are executed in the same order as they are queued.


// TODO: Improve error handling. In the current implementation, if an error
// occurs, the transaction is aborted and none of the queries scheduled for the
// transaction is executed. The only way the user can know that his has
// happened is by looking at stderr.log, it'd be better to provide a notify to
// the UI.


static GAsyncQueue *db_queue = NULL;
static GThread *db_thread = NULL;


static gpointer db_thread_func(gpointer dat);


void db_init() {
  char *dbfn = g_build_filename(conf_dir, "db.sqlite3", NULL);

  if(!sqlite3_threadsafe())
    g_error("sqlite3 has not been compiled with threading support. Please recompile sqlite3 with SQLITE_THREADSAFE enabled.");

  // TODO: should look at the version file instead. This check is simply for debugging purposes.
  gboolean newdb = !g_file_test(dbfn, G_FILE_TEST_EXISTS);
  if(newdb)
    g_error("No db.sqlite3 file present yet. Please run ncdc-db-upgrade.");

  sqlite3 *db;
  if(sqlite3_open(dbfn, &db))
    g_error("Couldn't open `%s': %s", dbfn, sqlite3_errmsg(db));
  g_free(dbfn);

  sqlite3_busy_timeout(db, 10);

  sqlite3_exec(db, "PRAGMA foreign_keys = FALSE", NULL, NULL, NULL);

  // TODO: create SQL schema, if it doesn't exist yet

  // start database thread
  db_queue = g_async_queue_new();
  db_thread = g_thread_create(db_thread_func, db, TRUE, NULL);
}


// A "queue item" is an array of pointers representing an SQL query.
//   q[0] = GINT_TO_POINTER(flags)
//   q[1] = (char *)sql_query. This must be a static string in global memory.
//   q[2..n-1] = bind values (see DBQ_* macros)
//   q[n] = DBQ_END


// Query flags
#define DBF_NEXT    1 // Current query must be in the same transaction as next query in the queue.
#define DBF_SINGLE  2 // Query must not be executed in a transaction (e.g. SELECT, VACUUM)
#define DBF_END   128 // Signal the database thread to close

// Column types
#define DBQ_END    0
#define DBQ_NULL   1 // No arguments
#define DBQ_INT    2 // GINT_TO_POINTER() argument
#define DBQ_INT64  3 // pointer to a slice-allocated gint64 as argument
#define DBQ_TEXT   4 // pointer to a g_malloc()'ed UTF-8 string. NULL is allowed as input
#define DBQ_BLOB   5 // first argument: GINT_TO_POINER(length), second: g_malloc()'d blob


// Free a queue item.
static void db_queue_item_free(void *dat) {
  void **q = dat;
  int t;
  void **a = q+2;

  while((t = GPOINTER_TO_INT(*(a++))) != DBQ_END) {
    switch(t) {
    case DBQ_INT:   a++; break;
    case DBQ_INT64: g_slice_free(gint64, *(a++)); break;
    case DBQ_TEXT:  g_free(*(a++)); break;
    case DBQ_BLOB:  g_free(*(a+1)); a += 2; break;
    }
  }
  g_free(q);
}


// Executes a single query.
// TODO: support for fetching and passing query results
static gboolean db_queue_process_one(sqlite3 *db, void **q, gboolean transaction) {
  char *query = (char *)q[1];

  // Would be nice to have the parameters logged
  g_debug("db: Executing \"%s\" (%s transaction)", query, transaction ? "inside" : "outside");

  // TODO: use cached prepared statements
  sqlite3_stmt *s;
  if(sqlite3_prepare_v2(db, query, -1, &s, NULL)) {
    g_critical("SQLite3 error preparing `%s': %s", query, sqlite3_errmsg(db));
    return FALSE;
  }
  void **a = q+2;
  int i = 1;

  int t;
  while((t = GPOINTER_TO_INT(*(a++))) != DBQ_END) {
    switch(t) {
    case DBQ_NULL:
      sqlite3_bind_null(s, i);
      break;
    case DBQ_INT:
      sqlite3_bind_int(s, i, GPOINTER_TO_INT(*(a++)));
      break;
    case DBQ_INT64:
      sqlite3_bind_int64(s, i, *((gint64 *)*(a++)));
      break;
    case DBQ_TEXT:
      if(*a)
        sqlite3_bind_text(s, i, (char *)*a, -1, SQLITE_STATIC);
      else
        sqlite3_bind_null(s, i);
      a++;
      break;
    case DBQ_BLOB:
      if(*(a+1))
        sqlite3_bind_blob(s, i, (char *)*(a+1), GPOINTER_TO_INT(*a), SQLITE_STATIC);
      else
        sqlite3_bind_null(s, i);
      a += 2;
      break;
    }
    i++;
  }

  int r;
  if(transaction)
    r = sqlite3_step(s);
  else
    while((r = sqlite3_step(s)) == SQLITE_BUSY)
      ;
  if(r != SQLITE_DONE) {
    g_critical("SQLite3 error on step() of `%s': %s", query, sqlite3_errmsg(db));
    return FALSE;
  }

  sqlite3_finalize(s);
  return TRUE;
}


static void db_queue_process_flush(sqlite3 *db, GPtrArray *q) {
  // start transaction
  char *err = NULL;
  if(sqlite3_exec(db, "BEGIN", NULL, NULL, &err)) {
    g_critical("SQLite3 error starting transaction: %s", err?err:sqlite3_errmsg(db));
    g_ptr_array_set_size(q, 0);
    if(err)
      sqlite3_free(err);
    return;
  }

  // execute queries
  int i;
  for(i=0; i<q->len; i++)
    if(!db_queue_process_one(db, g_ptr_array_index(q, i), TRUE))
      break;
  gboolean error = i < q->len ? TRUE : FALSE;
  g_ptr_array_set_size(q, 0);

  // rollback on error
  if(error) {
    err = NULL;
    if(sqlite3_exec(db, "ROLLBACK", NULL, NULL, &err) && err)
      sqlite3_free(err);
    return;
  }

  // otherwise, commit transaction
  sqlite3_stmt *s;
  int r;
  if(sqlite3_prepare_v2(db, "COMMIT", -1, &s, NULL)) {
    g_critical("SQLite3 error committing transaction: %s", sqlite3_errmsg(db));
    return;
  }
  while((r = sqlite3_step(s)) == SQLITE_BUSY)
    ;
  if(r != SQLITE_DONE) {
    sqlite3_finalize(s);
    g_critical("SQLite3 error committing transaction: %s", sqlite3_errmsg(db));
  }
  sqlite3_finalize(s);
}


static void db_queue_process(sqlite3 *db) {
  GTimeVal queue_end = {}; // tv_sec = 0 if nothing is queued
  GTimeVal cur_time;
  GPtrArray *queue = g_ptr_array_new_with_free_func(db_queue_item_free);
  gboolean next = FALSE;

  while(1) {
    void **q = (void **)(
                  next ? g_async_queue_try_pop(db_queue) :
      queue_end.tv_sec ? g_async_queue_timed_pop(db_queue, &queue_end) :
                         g_async_queue_pop(db_queue));

    // Shouldn't happen if items are correctly added to the queue.
    if(next && !q) {
      g_warn_if_reached();
      next = FALSE;
      continue;
    }
    next = FALSE;

    // We had something queued and the timeout has elapsed
    if(queue_end.tv_sec && !q) {
      g_debug("db: Flushing after timeout");
      db_queue_process_flush(db, queue);
      queue_end.tv_sec = 0;
      continue;
    }

    // "handle" the item
    int flags = *((int *)*q);

    // signal that the database should be closed
    if(flags == DBF_END) {
      g_debug("db: Flushing and closing");
      db_queue_process_flush(db, queue);
      g_free(q);
      break;
    }

    // query must be done outside of a transaction (e.g. SELECT, VACUUM)
    if(flags & DBF_SINGLE) {
      if(queue->len) {
        g_debug("db: Flushing to process SINGLE query.");
        db_queue_process_flush(db, queue);
        queue_end.tv_sec = 0;
      }
      db_queue_process_one(db, q, FALSE);
      continue;
    }

    // Add item to the queue
    g_ptr_array_add(queue, q);

    // If the item has to be performed in the same transaction as the next one, fetch next
    if(flags & DBF_NEXT) {
      next = TRUE;
      continue;
    }

    // - If the queue has somehow grown beyond 50 items, flush it.
    // - If the time for receiving the queue has elapsed, flush it.
    g_get_current_time(&cur_time);
    if(queue->len > 50 || (cur_time.tv_sec > queue_end.tv_sec
        || (cur_time.tv_sec == queue_end.tv_sec && cur_time.tv_usec > queue_end.tv_usec))){
      g_debug("db: Flushing after timeout or long queue");
      db_queue_process_flush(db, queue);
      queue_end.tv_sec = 0;
    }

    // If we're here and no queue has started yet, start one
    if(!queue_end.tv_sec) {
      queue_end = cur_time;
      g_time_val_add(&queue_end, 1000000); // queue queries for 1 second
    }
  }

  g_ptr_array_unref(queue);
}


static gpointer db_thread_func(gpointer dat) {
  sqlite3 *db = dat;
  db_queue_process(db);
  sqlite3_close(db);
  return NULL;
}


// Flushes the queue, blocks until all queries are processed and then performs
// a little cleanup.
void db_close() {
  // Send a END message to the database thread
  void **q = g_new0(void *, 1);
  q[0] = GINT_TO_POINTER(DBF_END);
  g_async_queue_push(db_queue, q);
  // And wait for it to quit
  g_thread_join(db_thread);
  g_async_queue_unref(db_queue);
  db_queue = NULL;
}


// The query is assumed to be a static string that is not freed or modified.
// Any BLOB or TEXT arguments will be passed directly to the processing thread
// and will be g_free()'d there.
static void *db_queue_create(int flags, const char *q, ...) {
  GPtrArray *a = g_ptr_array_new();
  g_ptr_array_add(a, GINT_TO_POINTER(flags));
  g_ptr_array_add(a, (void *)q);

  int t;
  gint64 *n;
  va_list va;
  va_start(va, q);
  while((t = va_arg(va, int)) != DBQ_END) {
    g_ptr_array_add(a, GINT_TO_POINTER(t));
    switch(t) {
    case DBQ_NULL: break;
    case DBQ_INT:
      g_ptr_array_add(a, GINT_TO_POINTER(va_arg(va, int)));
      break;
    case DBQ_INT64:
      n = g_slice_new(gint64);
      *n = va_arg(va, gint64);
      g_ptr_array_add(a, n);
      break;
    case DBQ_TEXT:
      g_ptr_array_add(a, va_arg(va, char *));
      break;
    case DBQ_BLOB:
      g_ptr_array_add(a, GINT_TO_POINTER(va_arg(va, int)));
      g_ptr_array_add(a, va_arg(va, char *));
      break;
    default:
      g_return_val_if_reached(NULL);
    }
  }
  va_end(va);
  g_ptr_array_add(a, GINT_TO_POINTER(DBQ_END));

  return g_ptr_array_free(a, FALSE);
}


#define db_queue_lock() g_async_queue_lock(db_queue)
#define db_queue_unlock() g_async_queue_unlock(db_queue)
#define db_queue_push(...) g_async_queue_push(db_queue, db_queue_create(__VA_ARGS__))
#define db_queue_push_unlocked(...) g_async_queue_push_unlocked(db_queue, db_queue_create(__VA_ARGS__))



// OLD MACROS! Functions that use these should be rewritten to communicate with
// the database thread instead.
#define db_lock(x) return x
#define db_unlock() {}
#define db_err(msg, x) return x
#define db_begin(x) return x
#define db_step(s, r, x) return x
#define db_commit(x) return x
static sqlite3 *db;




// hashdata and hashfiles

#if INTERFACE

// TODO!
#define db_fl_getdone() TRUE
#define db_fl_setdone(v) {}

#endif


// Adds a file to hashfiles and, if not present yet, hashdata. Returns the new hashfiles.id.
gint64 db_fl_addhash(const char *path, guint64 size, time_t lastmod, const char *root, const char *tthl, int tthl_len) {
  char hash[40] = {};
  base32_encode(root, hash);

  db_queue_lock();
  db_queue_push_unlocked(DBF_NEXT,
    "INSERT OR IGNORE INTO hashdata (root, size, tthl) VALUES(?, ?, ?)",
    DBQ_TEXT, g_strdup(hash),
    DBQ_INT64, (gint64)size,
    DBQ_BLOB, tthl_len, g_strdup(tthl),
    DBQ_END
  );

  // hashfiles.
  // Note that it in certain situations it may happen that a row with the same
  // filename is already present. This happens when two files in the share have
  // the same realpath() (e.g. one is a symlink). In such a case it is safe to
  // just do a REPLACE.
  db_queue_push_unlocked(0,
    "INSERT OR REPLACE INTO hashfiles (tth, lastmod, filename) VALUES(?, ?, ?)",
    DBQ_TEXT, g_strdup(hash),
    DBQ_INT64, (gint64)lastmod,
    DBQ_TEXT, g_strdup(path),
    DBQ_END
  );

  // TODO: GET ID! -> sqlite3_last_insert_rowid(db)
  return 0;
}


// Fetch the tthl data associated with a TTH root. Return value must be
// g_free()'d. Returns NULL on error or when it's not in the DB.
char *db_fl_gettthl(const char *root, int *len) {
  db_lock(NULL);

  char hash[40] = {};
  base32_encode(root, hash);
  sqlite3_stmt *s;
  int r;
  int l = 0;
  char *res = NULL;

  if(sqlite3_prepare_v2(db, "SELECT tthl FROM hashfiles WHERE root = ?", -1, &s, NULL))
    db_err(NULL, NULL);
  sqlite3_bind_text(s, 1, hash, -1, SQLITE_STATIC);

  db_step(s, r, NULL);
  if(r == SQLITE_ROW) {
    res = (char *)sqlite3_column_blob(s, 0);
    if(res) {
      l = sqlite3_column_bytes(s, 0);
      res = g_memdup(res, l);
    }
    if(len)
      *len = l;
  }

  sqlite3_finalize(s);
  db_unlock();
  return res;
}


// Get information for a file. Returns 0 if not found or error.
gint64 db_fl_getfile(const char *path, time_t *lastmod, guint64 *size, char *tth) {
  db_lock(0);

  sqlite3_stmt *s;
  int r;
  gint64 id = 0;

  if(sqlite3_prepare_v2(db,
       "SELECT f.id, f.lastmod, f.tth, d.size FROM hashfiles f JOIN hashdata d ON d.root = f.tth WHERE f.filename = ?",
       -1, &s, NULL))
    db_err(NULL, 0);
  sqlite3_bind_text(s, 1, path, -1, SQLITE_STATIC);

  db_step(s, r, 0);
  if(r == SQLITE_ROW) {
    id = sqlite3_column_int64(s, 0);
    if(lastmod)
      *lastmod = sqlite3_column_int64(s, 1);
    if(tth) {
      const char *hash = (const char *)sqlite3_column_text(s, 2);
      base32_decode(hash, tth);
    }
    if(size)
      *size = sqlite3_column_int64(s, 3);
  }

  sqlite3_finalize(s);
  db_unlock();
  return id;
}


// Batch-remove rows from hashfiles.
// TODO: how/when to remove rows from hashdata for which no entry in hashfiles
// exist? A /gc will do this by calling db_fl_purgedata(), but ideally this
// would be done as soon as the hashdata row has become obsolete.
void db_fl_rmfiles(gint64 *ids, int num) {
  int i;
  for(i=0; i<num; i++)
    db_queue_push(0, "DELETE FROM hashfiles WHERE id = ?", DBQ_INT64, ids[i], DBQ_END);
}


// Gets the full list of all ids in the hashfiles table, in ascending order.
// *callback is called for every row.
void db_fl_getids(void (*callback)(gint64)) {
  db_lock();
  sqlite3_stmt *s;
  int r;

  // This query is fast: `id' is the SQLite rowid, and has an index that is
  // already ordered.
  if(sqlite3_prepare_v2(db, "SELECT id FROM hashfiles ORDER BY id ASC", -1, &s, NULL))
    db_err(NULL,);

  db_step(s, r,);
  while(r == SQLITE_ROW) {
    callback(sqlite3_column_int64(s, 0));
    db_step(s, r,);
  }

  sqlite3_finalize(s);
  db_unlock();
}


// Remove rows from the hashdata table that are not referenced from the
// hashfiles table.
void db_fl_purgedata() {
  // Since there is no index on hashfiles(tth), one might expect this query to
  // be extremely slow. Luckily sqlite is clever enough to create a temporary
  // index for this query.
  db_queue_push(0, "DELETE FROM hashdata WHERE NOT EXISTS(SELECT 1 FROM hashfiles WHERE tth = root)", DBQ_END);
}





// dl and dl_users


// Fetches everything (except the raw TTHL data) from the dl table in no
// particular order, calls the callback for each row.
void db_dl_getdls(
  void (*callback)(const char *tth, guint64 size, const char *dest, char prio, char error, const char *error_msg, int tthllen)
) {
  db_lock();
  sqlite3_stmt *s;
  char hash[24];
  int r;

  if(sqlite3_prepare_v2(db, "SELECT tth, size, dest, priority, error, error_msg, length(tthl) FROM dl", -1, &s, NULL))
    db_err(NULL,);

  db_step(s, r,);
  while(r == SQLITE_ROW) {
    base32_decode((const char *)sqlite3_column_text(s, 0), hash);
    callback(
      hash,
      (guint64)sqlite3_column_int64(s, 1),
      (const char *)sqlite3_column_text(s, 2),
      sqlite3_column_int(s, 3),
      sqlite3_column_int(s, 4),
      (const char *)sqlite3_column_text(s, 5),
      sqlite3_column_int(s, 6)
    );
    db_step(s, r,);
  }

  sqlite3_finalize(s);
  db_unlock();
}


// Fetches everything from the dl_users table in no particular order, calls the
// callback for each row.
void db_dl_getdlus(void (*callback)(const char *tth, guint64 uid, char error, const char *error_msg)) {
  db_lock();
  sqlite3_stmt *s;
  char hash[24];
  int r;

  if(sqlite3_prepare_v2(db, "SELECT tth, uid, error, error_msg FROM dl_users", -1, &s, NULL))
    db_err(NULL,);

  db_step(s, r,);
  while(r == SQLITE_ROW) {
    base32_decode((const char *)sqlite3_column_text(s, 0), hash);
    callback(
      hash,
      (guint64)sqlite3_column_int64(s, 1),
      sqlite3_column_int(s, 2),
      (const char *)sqlite3_column_text(s, 3)
    );
    db_step(s, r,);
  }

  sqlite3_finalize(s);
  db_unlock();
}


// (queued) Delete a row from dl and any rows from dl_users that reference the row.
void db_dl_rm(const char *tth) {
  char hash[40] = {};
  base32_encode(tth, hash);

  db_queue_lock();
  db_queue_push_unlocked(0, "DELETE FROM dl_users WHERE tth = ?", DBQ_TEXT, g_strdup(hash), DBQ_END);
  db_queue_push_unlocked(0, "DELETE FROM dl WHERE tth = ?", DBQ_TEXT, g_strdup(hash), DBQ_END);
  db_queue_unlock();
}


// (queued) Set the priority, error and error_msg columns of a dl row
void db_dl_setstatus(const char *tth, char priority, char error, const char *error_msg) {
  char hash[40] = {};
  base32_encode(tth, hash);
  db_queue_push(0, "UPDATE dl SET priority = ?, error = ?, error_msg = ? WHERE tth = ?",
    DBQ_INT, (int)priority, DBQ_INT, (int)error,
    DBQ_TEXT, error_msg ? g_strdup(error_msg) : NULL,
    DBQ_TEXT, g_strdup(hash),
    DBQ_END
  );
}


// (queued) Set the error information for a dl_user row (if tth != NULL), or
// all rows for a single user if tth = NULL.
// TODO: tth = NULL is currently not very fast - no index on dl_user(uid).
void db_dl_setuerr(guint64 uid, const char *tth, char error, const char *error_msg) {
  // for a single dl item
  if(tth) {
    char hash[40] = {};
    base32_encode(tth, hash);
    db_queue_push(0, "UPDATE dl_users SET error = ?, error_msg = ? WHERE uid = ? AND tth = ?",
      DBQ_INT, (int)error,
      DBQ_TEXT, error_msg ? g_strdup(error_msg) : NULL,
      DBQ_INT64, (gint64)uid,
      DBQ_TEXT, g_strdup(hash),
      DBQ_END
    );
  // for all dl items
  } else {
    db_queue_push(0, "UPDATE dl_users SET error = ?, error_msg = ? WHERE uid = ?",
      DBQ_INT, (int)error,
      DBQ_TEXT, error_msg ? g_strdup(error_msg) : NULL,
      DBQ_INT64, (gint64)uid,
      DBQ_END
    );
  }
}


// (queued) Remove a dl_user row from the database (if tth != NULL), or all
// rows from a single user if tth = NULL. (Same note as for db_dl_setuerr()
// applies here).
void db_dl_rmuser(guint64 uid, const char *tth) {
  // for a single dl item
  if(tth) {
    char hash[40] = {};
    base32_encode(tth, hash);
    db_queue_push(0, "DELETE FROM dl_users WHERE uid = ? AND tth = ?",
      DBQ_INT64, (gint64)uid,
      DBQ_TEXT, g_strdup(hash),
      DBQ_END
    );
  // for all dl items
  } else {
    db_queue_push(0, "DELETE FROM dl_users WHERE uid = ?",
      DBQ_INT64, (gint64)uid,
      DBQ_END
    );
  }
}


// (queued) Sets the tthl column for a dl row.
void db_dl_settthl(const char *tth, const char *tthl, int len) {
  char hash[40] = {};
  base32_encode(tth, hash);
  db_queue_push(0, "UPDATE dl SET tthl = ? WHERE tth = ?",
    DBQ_BLOB, len, g_memdup(tthl, len),
    DBQ_TEXT, g_strdup(hash),
    DBQ_END
  );
}


// (queued) Adds a new row to the dl table.
void db_dl_insert(const char *tth, guint64 size, const char *dest, char priority, char error, const char *error_msg) {
  char hash[40] = {};
  base32_encode(tth, hash);
  db_queue_push(0, "INSERT OR REPLACE INTO dl (tth, size, dest, priority, error, error_msg) VALUES (?, ?, ?, ?, ?, ?)",
    DBQ_TEXT, g_strdup(hash),
    DBQ_INT64, (gint64)size,
    DBQ_TEXT, g_strdup(dest),
    DBQ_INT, (int)priority,
    DBQ_INT, (int)error,
    DBQ_TEXT, error_msg ? g_strdup(error_msg) : NULL,
    DBQ_END
  );
}


// (queued) Adds a new row to the dl_users table.
void db_dl_adduser(const char *tth, guint64 uid, char error, const char *error_msg) {
  char hash[40] = {};
  base32_encode(tth, hash);
  db_queue_push(0, "INSERT OR REPLACE INTO dl_users (tth, uid, error, error_msg) VALUES (?, ?, ?, ?)",
    DBQ_TEXT, g_strdup(hash),
    DBQ_INT64, (gint64)uid,
    DBQ_INT, (int)error,
    DBQ_TEXT, error_msg ? g_strdup(error_msg) : NULL,
    DBQ_END
  );
}




// Executes a VACUUM
void db_vacuum() {
  db_queue_push(DBF_SINGLE, "VACUUM", DBQ_END);
}
