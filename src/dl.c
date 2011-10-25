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
#include <stdlib.h>
#include <errno.h>
#include <unistd.h>

#include <fcntl.h>
#include <gdbm.h>


#if INTERFACE

struct dl_user_dl {
  struct dl *dl;
  struct dl_user *u;
  char error;               // DLE_*
  unsigned short error_sub; // errno or block number (it is assumed that 0 <= errno <= USHRT_MAX)
};


#define DLU_NCO  0 // Not connected, ready for connection
#define DLU_EXP  1 // Expecting a dl connection
#define DLU_IDL  2 // dl connected, idle
#define DLU_ACT  3 // dl connected, downloading
#define DLU_WAI  4 // Not connected, waiting for reconnect timeout

struct dl_user {
  int state;                 // DLU_*
  int timeout;               // source id of the timeout function in DLU_WAI
  guint64 uid;
  struct cc *cc;             // Always when state = IDL or ACT, may be set or NULL in EXP
  GSequence *queue;          // list of struct dl_user_dl, ordered by dl_user_dl_sort()
  struct dl_user_dl *active; // when state = DLU_ACT, the dud that is being downloaded (NULL if it had been removed from the queue while downloading)
};

/* State machine for dl_user.state:
 *
 *           8  /-----\
 *        .<--- | WAI | <-------------------------------.
 *       /      \-----/     |             |             |
 *       |                2 |           4 |  .<------.  | 7
 *       v                  |             | /       6 \ |
 *    /-----\  1         /-----\  3    /-----\  5    /-----\
 * -> | NCO | ---------> | EXP | ----> | IDL | ----> | ACT |
 *    \-----/            \-----/       \-----/       \-----/
 *
 *  1. We're requesting a connect
 *  2. No reply, connection timed out or we lost the $Download game on NMDC
 *  3. Successful connection and handshake
 *  4. Idle timeout / user disconnect
 *  5. Start of download
 *  6. Download (chunk) finished
 *  7. Idle timeout / user disconnect / download aborted / no slots free / error while downloading
 *  8. Reconnect timeout expired
 *     (currently hardcoded to 60 sec, probably want to make this configurable)
 */



// Note: The following numbers are also stored in dl.dat. Keep this in mind
// when changing or extending. (Both DLP_ and DLE_)

#define DLP_ERR   -65 // disabled due to (permanent) error
#define DLP_OFF   -64 // disabled by user
#define DLP_VLOW   -2
#define DLP_LOW    -1
#define DLP_MED     0
#define DLP_HIGH    1
#define DLP_VHIGH   2


#define DLE_NONE    0 // No error
#define DLE_INVTTHL 1 // TTHL data does not match the file root
#define DLE_NOFILE  2 // User does not have the file at all
#define DLE_IO_INC  3 // I/O error with incoming file, error_sub = errno
#define DLE_IO_DEST 4 // I/O error when moving to destination file/dir
#define DLE_HASH    5 // Hash check failed, error_sub = block index of failed hash


struct dl {
  gboolean islist : 1;
  gboolean hastthl : 1;
  gboolean active : 1;      // Whether it is being downloaded by someone
  char prio;                // DLP_*
  char error;               // DLE_*
  unsigned short error_sub; // errno or block number (it is assumed that 0 <= errno <= USHRT_MAX)
  int incfd;                // file descriptor for this file in <incoming_dir>
  char *flsel;              // path to file/dir to select for filelists
  struct ui_tab *flpar;     // parent of the file list browser tab for filelists (might be a dangling pointer!)
  char hash[24];            // TTH for files, tiger(uid) for filelists
  GPtrArray *u;             // list of users who have this file (GSequenceIter pointers into dl_user.queue)
  guint64 size;             // total size of the file
  guint64 have;             // what we have so far
  char *inc;                // path to the incomplete file (<incoming_dir>/<base32-hash>)
  char *dest;               // destination path (must be on same filesystem as the incomplete file)
  guint64 hash_block;       // number of bytes that each block represents
  struct tth_ctx *hash_tth; // TTH state of the last block that we have
  GSequenceIter *iter;      // used by UIT_DL
};

#endif


// Minimum filesize for which we request TTHL data. If a file is smaller than
// this, the TTHL data would simply add more overhead than it is worth.
#define DL_MINTTHLSIZE (512*1024)

// GDBM data file.
static GDBM_FILE dl_dat;

// The 'have' field is currently not saved in the data file, stat() is used on
// startup on the incomplete file to get this information. We'd still need some
// progress indication in the data file in the future, to indicate which parts
// have been TTH-checked, etc.
#define DLDAT_INFO  0 // <8 bytes: size><1 byte: prio><1 byte: error><2 bytes: error_sub>
                      // <4 bytes: reserved><zero-terminated-string: destination>
#define DLDAT_USERS 1 // <8 bytes: amount>
                      // <8 bytes: uid><1 byte: reserved><1 byte: error><2 bytes: error_sub><4 bytes: reserved>
                      // Repeat previous line $amount times
                      // For ncdc <= 1.2: amount=1 and only the 8-byte uid was present
#define DLDAT_TTHL  2 // <24 bytes: hash1>..


// Download queue.
// Key = dl->hash, Value = struct dl
GHashTable *dl_queue = NULL;


// uid -> dl_user lookup table.
static GHashTable *queue_users = NULL;



static gboolean dl_dat_needsync = FALSE;

// Performs a gdbm_sync() in an upcoming iteration of the main loop. This
// delayed write allows doing bulk operations on the data file while avoiding a
// gdbm_sync() on every change.
#define dl_dat_sync()\
  if(!dl_dat_needsync) {\
    dl_dat_needsync = TRUE;\
    g_idle_add(dl_dat_sync_do, NULL);\
  }

static gboolean dl_dat_sync_do(gpointer dat) {
  gdbm_sync(dl_dat);
  dl_dat_needsync = FALSE;
  return FALSE;
}




// Utility function that returns an error string for DLE_* errors.
char *dl_strerror(char err, unsigned short sub) {
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





// struct dl_user related functions

static gboolean dl_user_waitdone(gpointer dat);
static void dl_queue_checkrm(struct dl *dl, gboolean justfin);


// Determine whether a dl_user_dl struct can be considered as "enabled".
#define dl_user_dl_enabled(dud) (\
    !dud->error && dud->dl->prio > DLP_OFF\
    && ((!dud->dl->size && dud->dl->islist) || dud->dl->size != dud->dl->have)\
  )


// Sort function for dl_user_dl structs. Items with a higher priority are
// sorted before items with a lower priority. Never returns 0, so the order is
// always predictable even if all items have the same priority. This function
// is used both for sorting the queue of a single user, and to sort users
// itself on their highest-priority file.
// TODO: Give priority to small files (those that can be downloaded using a minislot)
static gint dl_user_dl_sort(gconstpointer a, gconstpointer b, gpointer dat) {
  const struct dl_user_dl *x = a;
  const struct dl_user_dl *y = b;
  const struct dl *dx = x->dl;
  const struct dl *dy = y->dl;
  return
      // Disabled? Always last
      dl_user_dl_enabled(x) && !dl_user_dl_enabled(y) ? -1 : !dl_user_dl_enabled(x) && dl_user_dl_enabled(y) ? 1
      // File lists get higher priority than normal files
    : dx->islist && !dy->islist ? -1 : !dx->islist && dy->islist ? 1
      // Higher priority files get higher priority than lower priority ones (duh)
    : dx->prio > dy->prio ? -1 : dx->prio < dy->prio ? 1
      // For equal priority: download in alphabetical order
    : strcmp(dx->dest, dy->dest);
}


// Frees a dl_user_dl struct
static void dl_user_dl_free(gpointer x) {
  g_slice_free(struct dl_user_dl, x);
}


// Get the highest-priority file in the users' queue that is not already being
// downloaded. This function can be assumed to be relatively fast, in most
// cases the first iteration will be enough, in the worst case it at most
// <download_slots> iterations.
// Returns NULL if there is no dl item in the queue that is enabled and not
// being downloaded.
static struct dl_user_dl *dl_user_getdl(const struct dl_user *du) {
  GSequenceIter *i = g_sequence_get_begin_iter(du->queue);
  for(; !g_sequence_iter_is_end(i); i=g_sequence_iter_next(i)) {
    struct dl_user_dl *dud = g_sequence_get(i);
    if(!dl_user_dl_enabled(dud))
      break;
    if(!dud->dl->active)
      return dud;
  }
  return NULL;
}


// Change the state of a user, use state=-1 when something is removed from
// du->queue.
static void dl_user_setstate(struct dl_user *du, int state) {
  // Handle reconnect timeout
  // x -> WAI
  if(state >= 0 && du->state != DLU_WAI && state == DLU_WAI)
    du->timeout = g_timeout_add_seconds_full(G_PRIORITY_LOW, 60, dl_user_waitdone, du, NULL);
  // WAI -> X
  else if(state >= 0 && du->state == DLU_WAI && state != DLU_WAI)
    g_source_remove(du->timeout);

  // Update dl_user_dl.active, dl.active and dl_user.active if we came from the
  // ACT state. These are set in dl_queue_start_user().
  // ACT -> x
  if(state >= 0 && du->state == DLU_ACT && state != DLU_ACT && du->active) {
    struct dl_user_dl *dud = du->active;
    du->active = NULL;
    dud->dl->active = FALSE;
    dl_queue_checkrm(dud->dl, FALSE);
  }

  // Set state
  //g_debug("dlu:%"G_GINT64_MODIFIER"x: %d -> %d (active = %s)", du->uid, du->state, state, du->active ? "true":"false");
  if(state >= 0)
    du->state = state;

  // Check whether there is any value in keeping this dl_user struct in memory
  if(du->state == DLU_NCO && !g_sequence_get_length(du->queue)) {
    g_hash_table_remove(queue_users, &du->uid);
    g_sequence_free(du->queue);
    g_slice_free(struct dl_user, du);
    return;
  }

  // Check whether we can initiate a download again. (We could be more
  // selective here to possibly decrease CPU usage, but oh well.)
  dl_queue_start();
}


static gboolean dl_user_waitdone(gpointer dat) {
  struct dl_user *du = dat;
  g_return_val_if_fail(du->state == DLU_WAI, FALSE);
  dl_user_setstate(du, DLU_NCO);
  return FALSE;
}


// When called with NULL, this means that a connection attempt failed or we
// somehow disconnected from the user.
// Otherwise, it means that the cc connection with the user went into the IDLE
// state, either after the handshake or after a completed download.
void dl_user_cc(guint64 uid, struct cc *cc) {
  g_debug("dl:%016"G_GINT64_MODIFIER"x: cc = %s", uid, cc?"true":"false");
  struct dl_user *du = g_hash_table_lookup(queue_users, &uid);
  if(!du)
    return;
  g_return_if_fail(!cc || du->state == DLU_NCO || du->state == DLU_EXP || du->state == DLU_ACT);
  du->cc = cc;
  dl_user_setstate(du, cc ? DLU_IDL : DLU_WAI);
}


// To be called when a user joins a hub. Checks whether we have something to
// get from that user. May be called with uid=0 after joining a hub, in which
// case all users in the queue will be checked.
void dl_user_join(guint64 uid) {
  if(!uid || g_hash_table_lookup(queue_users, &uid))
    dl_queue_start();
}


// Adds a user to a dl item, making sure to create the user if it's not in the
// queue yet. For internal use only, does not call dl_dat_saveusers() and
// dl_queue_start().
static void dl_user_add(struct dl *dl, guint64 uid, char error, unsigned short error_sub) {
  g_return_if_fail(!dl->islist || dl->u->len == 0);

  // get or create dl_user struct
  struct dl_user *du = g_hash_table_lookup(queue_users, &uid);
  if(!du) {
    du = g_slice_new0(struct dl_user);
    du->state = DLU_NCO;
    du->uid = uid;
    du->queue = g_sequence_new(dl_user_dl_free);
    g_hash_table_insert(queue_users, &du->uid, du);
  }

  // create and fill dl_user_dl struct
  struct dl_user_dl *dud = g_slice_new0(struct dl_user_dl);
  dud->dl = dl;
  dud->u = du;
  dud->error = error;
  dud->error_sub = error_sub;

  // Add to du->queue and dl->u
  g_ptr_array_add(dl->u, g_sequence_insert_sorted(du->queue, dud, dl_user_dl_sort, NULL));
  if(ui_dl)
    ui_dl_dud_listchange(dud, UIDL_ADD);
}


// Remove a user (dl->u[i]) from a dl item, making sure to also remove it from
// du->queue and possibly free the dl_user item if it's no longer useful. As
// above, for internal use only. Does not call dl_dat_saveusers().
static void dl_user_rm(struct dl *dl, int i) {
  GSequenceIter *dudi = g_ptr_array_index(dl->u, i);
  struct dl_user_dl *dud = g_sequence_get(dudi);
  struct dl_user *du = dud->u;

  // Make sure to disconnect the user and disable dl->active if we happened to
  // be actively downloading the file from this user.
  if(du->active == dud) {
    cc_disconnect(du->cc);
    du->active = NULL;
    dl->active = FALSE;
    // Note that cc_disconnect() immediately calls dl_user_cc(), causing
    // dl->active to be reset anyway. I'm not sure whether it's a good idea to
    // rely on that, however.
  }

  if(ui_dl)
    ui_dl_dud_listchange(dud, UIDL_DEL);
  g_sequence_remove(dudi); // dl_user_dl_free() will be called implicitely
  g_ptr_array_remove_index_fast(dl->u, i);
  dl_user_setstate(du, -1);
}





// Determining when and what to start downloading

static gboolean dl_queue_needstart = FALSE;

// Determines whether the user is a possible target to either connect to, or to
// initiate a download with.
static gboolean dl_queue_start_istarget(struct dl_user *du) {
  // User must be in the NCO/IDL state and we must have something to download
  // from them.
  if((du->state != DLU_NCO && du->state != DLU_IDL) || !dl_user_getdl(du))
    return FALSE;

  // In the NCO state, the user must also be online, and the hub must be
  // properly logged in. Otherwise we won't be able to connect anyway.
  if(du->state == DLU_NCO) {
    struct hub_user *u = g_hash_table_lookup(hub_uids, &du->uid);
    if(!u || !u->hub->nick_valid)
      return FALSE;
  }

  // If the above holds, we're safe
  return TRUE;
}


// Starts a connection with a user or initiates a download if we're already
// connected.
static gboolean dl_queue_start_user(struct dl_user *du) {
  g_return_val_if_fail(dl_queue_start_istarget(du), FALSE);

  // If we're not connected yet, just connect
  if(du->state == DLU_NCO) {
    g_debug("dl:%016"G_GINT64_MODIFIER"x: trying to open a connection", du->uid);
    struct hub_user *u = g_hash_table_lookup(hub_uids, &du->uid);
    dl_user_setstate(du, DLU_EXP);
    hub_opencc(u->hub, u);
    return FALSE;
  }

  // Otherwise, initiate a download.
  struct dl_user_dl *dud = dl_user_getdl(du);
  g_return_val_if_fail(dud, FALSE);
  struct dl *dl = dud->dl;
  g_debug("dl:%016"G_GINT64_MODIFIER"x: using connection for %s", du->uid, dl->dest);

  // For filelists: Don't allow resuming of the download. It could happen that
  // the client modifies its filelist in between our retries. In that case the
  // downloaded filelist would end up being corrupted. To avoid that: make sure
  // lists are downloaded in one go, and throw away any incomplete data.
  if(dl->islist && dl->have > 0) {
    dl->have = dl->size = 0;
    g_return_val_if_fail(close(dl->incfd) == 0, FALSE);
    dl->incfd = open(dl->inc, O_WRONLY|O_CREAT|O_TRUNC, 0666);
    g_return_val_if_fail(dl->incfd >= 0, FALSE);
  }

  // Update state and connect
  dl->active = TRUE;
  du->active = dud;
  dl_user_setstate(du, DLU_ACT);
  cc_download(du->cc, dl);
  return TRUE;
}


// Compares two dl_user structs by a "priority" to determine from whom to
// download first. Note that users in the IDL state always get priority over
// users in the NCO state, in order to prevent the situation that the
// lower-priority user in the IDL state is connected to anyway in a next
// iteration. Returns -1 if a has a higher priority than b.
// This function assumes dl_queue_start_istarget() for both arguments.
static gint dl_queue_start_cmp(gconstpointer a, gconstpointer b) {
  const struct dl_user *ua = a;
  const struct dl_user *ub = b;
  return -1*(
    ua->state == DLU_IDL && ub->state != DLU_IDL ?  1 :
    ua->state != DLU_IDL && ub->state == DLU_IDL ? -1 :
    dl_user_dl_sort(dl_user_getdl(ub), dl_user_getdl(ua), NULL)
  );
}


// Initiates a new connection to a user or requests a file from an already
// connected user, based on the current state of dl_user and dl structs.  This
// function is relatively slow, so is executed from a timeout to bulk-check
// everything after some state variables have changed. Should not be called
// directly, use dl_queue_start() instead.
static gboolean dl_queue_start_do(gpointer dat) {
  int freeslots = conf_download_slots();

  // Walk through all users in the queue and:
  // - determine possible targets to connect to or to start a transfer from
  // - determine the highest-priority target
  // - calculate freeslots
  GPtrArray *targets = g_ptr_array_new();
  struct dl_user *du, *target = NULL;
  int target_i;
  GHashTableIter iter;
  g_hash_table_iter_init(&iter, queue_users);
  while(g_hash_table_iter_next(&iter, NULL, (gpointer *)&du)) {
    if(du->state == DLU_ACT)
      freeslots--;
    if(dl_queue_start_istarget(du)) {
      if(!target || dl_queue_start_cmp(target, du) > 0) {
        target_i = targets->len;
        target = du;
      }
      g_ptr_array_add(targets, du);
    }
  }

  // Try to connect to the previously found highest-priority target, then go
  // through the list again to eliminate any users that may not be a target
  // anymore and to fetch a new highest-priority target.
  while(freeslots > 0 && target) {
    if(dl_queue_start_user(target) && !--freeslots)
      break;
    g_ptr_array_remove_index_fast(targets, target_i);

    int i = 0;
    target = NULL;
    while(i < targets->len) {
      du = g_ptr_array_index(targets, i);
      if(!dl_queue_start_istarget(du))
        g_ptr_array_remove_index_fast(targets, i);
      else {
        if(!target || dl_queue_start_cmp(target, du) > 0) {
          target_i = i;
          target = du;
        }
        i++;
      }
    }
  }

  g_ptr_array_unref(targets);

  // Reset this value *after* performing all the checks and starts, to ignore
  // any dl_queue_start() calls while this function was working - this function
  // already takes those changes into account anyway.
  dl_queue_needstart = FALSE;
  return FALSE;
}


// Make sure dl_queue_start() can be called at any time that something changed
// that might allow us to initiate a download again. Since this is a relatively
// expensive operation, dl_queue_start() simply queues a dl_queue_start_do()
// from a timer.
// TODO: Make the timeout configurable? It's a tradeoff between download
// management responsiveness and CPU usage.
void dl_queue_start() {
  if(!dl_queue_needstart) {
    dl_queue_needstart = TRUE;
    g_timeout_add(500, dl_queue_start_do, NULL);
  }
}





// Adding/removing stuff from dl.dat

static void dl_dat_saveinfo(struct dl *dl) {
  char key[25];
  key[0] = DLDAT_INFO;
  memcpy(key+1, dl->hash, 24);
  datum keydat = { key, 25 };

  guint64 size = GINT64_TO_LE(dl->size);
  int len = 16 + strlen(dl->dest) + 1;
  char nfo[len];
  memset(nfo, 0, len);
  memcpy(nfo, &size, 8);
  nfo[8] = dl->prio;
  nfo[9] = dl->error;
  guint16 err_sub = GINT16_TO_LE(dl->error_sub);
  memcpy(nfo+10, &err_sub, 2);
  // bytes 12-15 are reserved, and initialized to zero
  strcpy(nfo+16, dl->dest);
  datum val = { nfo, len };

  gdbm_store(dl_dat, keydat, val, GDBM_REPLACE);
  dl_dat_sync();
}


static void dl_dat_saveusers(struct dl *dl) {
  if(dl->islist)
    return;

  char key[25];
  key[0] = DLDAT_USERS;
  memcpy(key+1, dl->hash, 24);

  int len = 8+16*dl->u->len;
  char nfo[len];
  memset(nfo, 0, len);
  guint64 tmp = dl->u->len;
  tmp = GINT64_TO_LE(tmp);
  memcpy(nfo, &tmp, 8);

  int i;
  for(i=0; i<dl->u->len; i++) {
    char *ptr = nfo+8+i*16;
    struct dl_user_dl *dud = g_sequence_get(g_ptr_array_index(dl->u, i));
    tmp = GINT64_TO_LE(dud->u->uid);
    memcpy(ptr, &tmp, 8);
    ptr[9] = dud->error;
    guint16 sub = GINT16_TO_LE(dud->error_sub);
    memcpy(ptr+10, &sub, 2);
  }

  datum keydat = { key, 25 };
  datum val = { nfo, len };
  gdbm_store(dl_dat, keydat, val, GDBM_REPLACE);
  dl_dat_sync();
}


void dl_dat_rmdl(struct dl *dl) {
  char key[25];
  datum keydat = { key, 25 };
  memcpy(key+1, dl->hash, 24);
  key[0] = DLDAT_INFO;
  gdbm_delete(dl_dat, keydat);
  key[0] = DLDAT_USERS;
  gdbm_delete(dl_dat, keydat);
  key[0] = DLDAT_TTHL;
  gdbm_delete(dl_dat, keydat);
  dl_dat_sync();
}





// Adding stuff to the download queue

// Adds a dl item to the queue. dl->inc will be determined and opened here.
// dl->hastthl will be set if the file is small enough to not need TTHL data.
// dl->u is also created here.
static void dl_queue_insert(struct dl *dl, gboolean init) {
  // Set dl->hastthl for files smaller than MINTTHLSIZE.
  if(!dl->islist && !dl->hastthl && dl->size <= DL_MINTTHLSIZE) {
    dl->hastthl = TRUE;
    dl->hash_block = DL_MINTTHLSIZE;
  }
  // figure out dl->inc
  char hash[40] = {};
  base32_encode(dl->hash, hash);
  char *tmp = conf_incoming_dir();
  dl->inc = g_build_filename(tmp, hash, NULL);
  g_free(tmp);
  // create dl->u
  dl->u = g_ptr_array_new();
  // insert in the global queue
  g_hash_table_insert(dl_queue, dl->hash, dl);
  if(ui_dl)
    ui_dl_listchange(dl, UIDL_ADD);

  // insert in the data file
  if(!dl->islist && !init)
    dl_dat_saveinfo(dl);

  // start download, if possible
  if(!init)
    dl_queue_start();
}


// Add the file list of some user to the queue
void dl_queue_addlist(struct hub_user *u, const char *sel, struct ui_tab *parent) {
  g_return_if_fail(u && u->hasinfo);
  struct dl *dl = g_slice_new0(struct dl);
  dl->islist = TRUE;
  if(sel)
    dl->flsel = g_strdup(sel);
  dl->flpar = parent;
  // figure out dl->hash
  tiger_ctx tg;
  tiger_init(&tg);
  tiger_update(&tg, (char *)&u->uid, 8);
  tiger_final(&tg, dl->hash);
  if(g_hash_table_lookup(dl_queue, dl->hash)) {
    g_warning("dl:%016"G_GINT64_MODIFIER"x: files.xml.bz2 already in the queue.", u->uid);
    g_slice_free(struct dl, dl);
    return;
  }
  // figure out dl->dest
  char *fn = g_strdup_printf("%016"G_GINT64_MODIFIER"x.xml.bz2", u->uid);
  dl->dest = g_build_filename(conf_dir, "fl", fn, NULL);
  g_free(fn);
  // insert & start
  g_debug("dl:%016"G_GINT64_MODIFIER"x: queueing files.xml.bz2", u->uid);
  dl_queue_insert(dl, FALSE);
  dl_user_add(dl, u->uid, 0, 0);
  dl_dat_saveusers(dl);
}


// Add a regular file to the queue. If there is another file in the queue with
// the same filename, something else will be chosen instead.
// Returns true if it was added, false if it was already in the queue.
static gboolean dl_queue_addfile(guint64 uid, char *hash, guint64 size, char *fn) {
  if(g_hash_table_lookup(dl_queue, hash))
    return FALSE;
  g_return_val_if_fail(size >= 0, FALSE);
  struct dl *dl = g_slice_new0(struct dl);
  memcpy(dl->hash, hash, 24);
  dl->size = size;
  // Figure out dl->dest
  char *tmp = conf_download_dir();
  dl->dest = g_build_filename(tmp, fn, NULL);
  g_free(tmp);
  // and add to the queue
  g_debug("dl:%016"G_GINT64_MODIFIER"x: queueing %s", uid, fn);
  dl_queue_insert(dl, FALSE);
  dl_user_add(dl, uid, 0, 0);
  dl_dat_saveusers(dl);
  return TRUE;
}


// Recursively adds a file or directory to the queue. *excl will only be
// checked for files in subdirectories, if *fl is a file it will always be
// added.
void dl_queue_add_fl(guint64 uid, struct fl_list *fl, char *base, GRegex *excl) {
  // check excl
  if(base && excl && g_regex_match(excl, fl->name, 0, NULL)) {
    ui_mf(NULL, 0, "Ignoring `%s': excluded by regex.", fl->name);
    return;
  }

  char *name = base ? g_build_filename(base, fl->name, NULL) : g_strdup(fl->name);
  if(fl->isfile) {
    if(!dl_queue_addfile(uid, fl->tth, fl->size, name))
      ui_mf(NULL, 0, "Ignoring `%s': already queued.", name);
  } else {
    int i;
    for(i=0; i<fl->sub->len; i++)
      dl_queue_add_fl(uid, g_ptr_array_index(fl->sub, i), name, excl);
  }
  if(!base)
    ui_mf(NULL, 0, "%s added to queue.", name);
  g_free(name);
}


// Add a search result to the queue. (Only for files)
void dl_queue_add_res(struct search_r *r) {
  char *name = strrchr(r->file, '/');
  if(name)
    name++;
  else
    name = r->file;
  if(dl_queue_addfile(r->uid, r->tth, r->size, name))
    ui_mf(NULL, 0, "%s added to queue.", name);
  else
    ui_m(NULL, 0, "Already queued.");
}


// Add a user to a dl item, if the file is in the queue and the user hasn't
// been added yet. Returns:
//  -1  Not found in queue
//   0  Found, but user already queued
//   1  Found and user added to the queue
int dl_queue_matchfile(guint64 uid, char *tth) {
  struct dl *dl = g_hash_table_lookup(dl_queue, tth);
  if(!dl)
    return -1;
  int i;
  for(i=0; i<dl->u->len; i++)
    if(((struct dl_user_dl *)g_sequence_get(g_ptr_array_index(dl->u, i)))->u->uid == uid)
      return 0;
  dl_user_add(dl, uid, 0, 0);
  dl_dat_saveusers(dl);
  dl_queue_start();
  return 1;
}


// Recursively walks through the file list and adds the user to matching dl
// items. Returns the number of items found, and the number of items for which
// the user was added is stored in *added (should be initialized to zero).
int dl_queue_match_fl(guint64 uid, struct fl_list *fl, int *added) {
  if(fl->isfile && fl->hastth) {
    int r = dl_queue_matchfile(uid, fl->tth);
    if(r == 1)
      (*added)++;
    return r >= 0 ? 1 : 0;

  } else {
    int n = 0;
    int i;
    for(i=0; i<fl->sub->len; i++)
      n += dl_queue_match_fl(uid, g_ptr_array_index(fl->sub, i), added);
    return n;
  }
}





// Removing stuff from the queue and changing priorities

// removes an item from the queue
void dl_queue_rm(struct dl *dl) {
  // close the incomplete file, in case it's still open
  if(dl->incfd > 0)
    g_warn_if_fail(close(dl->incfd) == 0);
  // remove the incomplete file, in case we still have it
  if(dl->inc && g_file_test(dl->inc, G_FILE_TEST_EXISTS))
    unlink(dl->inc);
  // free dl->inc while we're at it. this is used as detection by dl_queue_checkrm() to prevent recursion
  g_free(dl->inc);
  dl->inc = NULL;
  // remove from the user info
  while(dl->u->len > 0)
    dl_user_rm(dl, 0);
  // remove from the data file
  if(!dl->islist)
    dl_dat_rmdl(dl);
  // free and remove dl struct
  if(ui_dl)
    ui_dl_listchange(dl, UIDL_DEL);
  g_hash_table_remove(dl_queue, dl->hash);
  if(dl->hash_tth)
    g_slice_free(struct tth_ctx, dl->hash_tth);
  g_ptr_array_unref(dl->u);
  g_free(dl->flsel);
  g_free(dl->dest);
  g_slice_free(struct dl, dl);
}


// Called when dl->active is reset or when the download has finished. If both
// actions are satisfied, we can _rm() the dl item. This makes sure that a dl
// struct is not removed while active is true. Otherwise the user will be
// forcibly disconnected in dl_user_rm(). (Which is what you want if you remove
// it while downloading, not if it's removed after finishing the download).
static void dl_queue_checkrm(struct dl *dl, gboolean justfin) {
  // prevent recursion
  if(!dl->inc)
    return;

  if(justfin) {
    // If the download just finished, we might as well remove it from dl.dat
    // immediately. Makes sure we won't load it on the next startup.
    dl_dat_rmdl(dl);
    // Since the dl item is now considered as "disabled" by the download
    // management code, make sure it is also last in every user's download
    // queue. For performance reasons, we can also already remove the users who
    // aren't active.
    int i = 0;
    while(i<dl->u->len) {
      GSequenceIter *iter = g_ptr_array_index(dl->u, i);
      struct dl_user_dl *dud = g_sequence_get(iter);
      if(dud != dud->u->active)
        dl_user_rm(dl, i);
      else {
        g_sequence_sort_changed(iter, dl_user_dl_sort, NULL);
        i++;
      }
    }
  }
  if(!dl->active && (dl->size || !dl->islist) && dl->have == dl->size)
    dl_queue_rm(dl);
}


void dl_queue_setprio(struct dl *dl, char prio) {
  gboolean enabled = dl->prio <= DLP_OFF && prio > DLP_OFF;
  dl->prio = prio;
  dl_dat_saveinfo(dl);
  // Make sure the dl_user.queue lists are still in the correct order
  int i;
  for(i=0; i<dl->u->len; i++)
    g_sequence_sort_changed(g_ptr_array_index(dl->u, i), dl_user_dl_sort, NULL);
  // Start the download if it is enabled
  if(enabled)
    dl_queue_start();
}


#define dl_queue_seterr(dl, e, sub) do {\
    (dl)->error = e;\
    (dl)->error_sub = sub;\
    dl_queue_setprio(dl, DLP_ERR);\
    ui_mf(ui_main, 0, "Download of `%s' failed: %s", (dl)->dest, dl_strerror(e, sub));\
  } while(0)


// Set a user-specific error
void dl_queue_setuerr(guint64 uid, char *tth, char e, unsigned short sub) {
  struct dl *dl = g_hash_table_lookup(dl_queue, tth);
  struct dl_user *du = g_hash_table_lookup(queue_users, &uid);
  if(!dl || !du)
    return;
  int i;
  for(i=0; i<dl->u->len; i++) {
    GSequenceIter *iter = g_ptr_array_index(dl->u, i);
    struct dl_user_dl *dud = g_sequence_get(iter);
    if(dud->u == du) {
      dud->error = e;
      dud->error_sub = sub;
      g_sequence_sort_changed(iter, dl_user_dl_sort, NULL);
      break;
    }
  }
  dl_dat_saveusers(dl);
}


// Remove a user from the queue for a certain file. If tth = NULL, the user
// will be removed from the queue entirely.
void dl_queue_rmuser(guint64 uid, char *tth) {
  struct dl *dl = tth ? g_hash_table_lookup(dl_queue, tth) : NULL;
  struct dl_user *du = g_hash_table_lookup(queue_users, &uid);
  if(!du)
    return;

  // from a single dl item
  if(dl) {
    int i;
    for(i=0; i<dl->u->len; i++) {
      if(((struct dl_user_dl *)g_sequence_get(g_ptr_array_index(dl->u, i)))->u == du) {
        dl_user_rm(dl, i);
        break;
      }
    }
    if(dl->islist && !dl->u->len)
      dl_queue_rm(dl);
    else
      dl_dat_saveusers(dl);

  // from all dl items (may be fairly slow)
  } else {
    // The loop is written in this way because after calling dl_user_rm():
    // 1. The current GSequenceIter is freed.
    // 2. The entire du struct and the GSequence may have been freed as well,
    //    if there were no other items left in its queue.
    GSequenceIter *n, *i = g_sequence_get_begin_iter(du->queue);
    gboolean run = !g_sequence_iter_is_end(i);
    while(run) {
      n = g_sequence_iter_next(i);
      run = !g_sequence_iter_is_end(n);
      struct dl *dl = ((struct dl_user_dl *)g_sequence_get(i))->dl;
      int j;
      for(j=0; j<dl->u->len; j++) {
        if(g_ptr_array_index(dl->u, j) == i) {
          dl_user_rm(dl, j);
          break;
        }
      }
      if(dl->islist && !dl->u->len)
        dl_queue_rm(dl);
      else
        dl_dat_saveusers(dl);
      i = n;
    }
  }
}





// Managing of active downloads

// Called when we've got a complete file
static void dl_finished(struct dl *dl) {
  g_debug("dl: download of `%s' finished, removing from queue", dl->dest);
  // close
  if(dl->incfd > 0)
    g_warn_if_fail(close(dl->incfd) == 0);
  dl->incfd = 0;
  // Create destination directory, if it does not exist yet.
  char *parent = g_path_get_dirname(dl->dest);
  if(g_mkdir_with_parents(parent, 0755) < 0)
    dl_queue_seterr(dl, DLE_IO_DEST, errno);
  g_free(parent);

  // Prevent overwiting other files by appending a prefix to the destination if
  // it already exists. It is assumed that fn + any dupe-prevention-extension
  // does not exceed NAME_MAX. (Not that checking against NAME_MAX is really
  // reliable - some filesystems have an even more strict limit)
  int num = 1;
  char *dest = g_strdup(dl->dest);
  while(!dl->islist && g_file_test(dest, G_FILE_TEST_EXISTS)) {
    g_free(dest);
    dest = g_strdup_printf("%s.%d", dl->dest, num++);
  }

  // Move the file to the destination.
  // TODO: this may block for a while if they are not on the same filesystem,
  // do this in a separate thread?
  GError *err = NULL;
  if(dl->prio != DLP_ERR) {
    GFile *src = g_file_new_for_path(dl->inc);
    GFile *dst = g_file_new_for_path(dest);
    g_file_move(src, dst, dl->islist ? G_FILE_COPY_OVERWRITE : G_FILE_COPY_BACKUP, NULL, NULL, NULL, &err);
    g_object_unref(src);
    g_object_unref(dst);
    if(err) {
      g_warning("Error moving `%s' to `%s': %s", dl->inc, dest, err->message);
      dl_queue_seterr(dl, DLE_IO_DEST, 0); // g_file_move does not give the value of errno :(
      g_error_free(err);
    }
  }
  g_free(dest);

  // open the file list
  if(dl->prio != DLP_ERR && dl->islist) {
    g_return_if_fail(dl->u->len == 1);
    ui_tab_open(ui_fl_create(((struct dl_user_dl *)g_sequence_get(g_ptr_array_index(dl->u, 0)))->u->uid, dl->flsel), FALSE, dl->flpar);
  }
  // and check whether we can remove this item from the queue
  dl_queue_checkrm(dl, TRUE);
}


static gboolean dl_hash_check(struct dl *dl, int num, char *tth) {
  // We don't have TTHL data for small files, so check against the root hash instead.
  if(dl->size < dl->hash_block) {
    g_return_val_if_fail(num == 0, FALSE);
    return memcmp(tth, dl->hash, 24) == 0 ? TRUE : FALSE;
  }
  // Otherwise, fetch the TTHL data and check against the right block hash.
  // It is probably faster to keep the TTHL data in memory, but since this data
  // may be around 200KiB and we can have a large number of dl structs, let's
  // just hope GDBM has a sensible caching mechanism.
  char key[25] = { DLDAT_TTHL };
  memcpy(key+1, dl->hash, 24);
  datum keydat = { key, 25 };
  datum val = gdbm_fetch(dl_dat, keydat);
  g_return_val_if_fail(val.dsize >= (num+1)*24, FALSE);
  gboolean r = memcmp(tth, val.dptr+(num*24), 24) == 0 ? TRUE : FALSE;
  free(val.dptr);
  return r;
}


// (Incrementally) hashes the incoming data and checks that what we have is
// still correct. When this function is called, dl->length should refer to the
// point where the newly received data will be written to.
// Returns -1 if nothing went wrong, any other number to indicate which block
// failed the hash check.
static int dl_hash_update(struct dl *dl, int length, char *buf) {
  g_return_val_if_fail(dl->hastthl, 0);

  int block = dl->have / dl->hash_block;
  guint64 cur = dl->have % dl->hash_block;

  if(!dl->hash_tth) {
    g_return_val_if_fail(!cur, FALSE);
    dl->hash_tth = g_slice_new(struct tth_ctx);
    tth_init(dl->hash_tth);
  }

  char tth[24];
  while(length > 0) {
    int w = MIN(dl->hash_block - cur, length);
    tth_update(dl->hash_tth, buf, w);
    length -= w;
    cur += w;
    buf += w;
    // we have a complete block, validate it.
    if(cur == dl->hash_block || (!length && dl->size == (block*dl->hash_block)+cur)) {
      tth_final(dl->hash_tth, tth);
      tth_init(dl->hash_tth);
      if(!dl_hash_check(dl, block, tth))
        return block;
      cur = 0;
      block++;
    }
  }

  return -1;
}


// Called when we've received some file data. Returns TRUE to continue the
// download, FALSE when something went wrong and the transfer should be
// aborted.
// TODO: do this in a separate thread to avoid blocking on HDD writes.
gboolean dl_received(guint64 uid, char *tth, char *buf, int length) {
  struct dl *dl = g_hash_table_lookup(dl_queue, tth);
  struct dl_user *du = g_hash_table_lookup(queue_users, &uid);
  if(!dl || !du)
    return FALSE;
  g_return_val_if_fail(du->state == DLU_ACT && du->active && du->active->dl == dl, FALSE);
  g_return_val_if_fail(dl->have + length <= dl->size, FALSE);

  // open dl->incfd, if it's not open yet
  if(dl->incfd <= 0) {
    dl->incfd = open(dl->inc, O_WRONLY|O_CREAT, 0666);
    if(dl->incfd < 0 || lseek(dl->incfd, dl->have, SEEK_SET) == (off_t)-1) {
      g_warning("Error opening %s: %s", dl->inc, g_strerror(errno));
      dl_queue_seterr(dl, DLE_IO_INC, errno);
      return FALSE;
    }
  }

  // save to the file and update dl->have
  while(length > 0) {
    // write
    int r = write(dl->incfd, buf, length);
    if(r < 0) {
      g_warning("Error writing to %s: %s", dl->inc, g_strerror(errno));
      dl_queue_seterr(dl, DLE_IO_INC, errno);
      return FALSE;
    }

    // check hash
    int fail = dl->islist ? -1 : dl_hash_update(dl, r, buf);
    if(fail >= 0) {
      g_warning("Hash failed for %s (block %d)", dl->inc, fail);
      dl_queue_setuerr(uid, tth, DLE_HASH, fail);
      // Delete the failed block and everything after it, so that a resume is possible.
      dl->have = fail*dl->hash_block;
      // I have no idea what to do when these functions fail. Resuming the
      // download without an ncdc restart won't work if lseek() fails, resuming
      // the download after an ncdc restart may result in a corrupted download
      // if the ftruncate() fails. Either way, resuming isn't possible in a
      // reliable fashion, so perhaps we should throw away the entire inc file?
      lseek(dl->incfd, dl->have, SEEK_SET);
      ftruncate(dl->incfd, dl->have);
      return FALSE;
    }

    // update vars
    length -= r;
    buf += r;
    dl->have += r;
  }

  if(dl->have >= dl->size) {
    g_warn_if_fail(dl->have == dl->size);
    dl_finished(dl);
  }
  return TRUE;
}


// Called when we've received TTHL data. For now we'll just store it dl.dat
// without modifications.
// TODO: combine hashes to remove uneeded granularity? (512kB is probably enough)
void dl_settthl(guint64 uid, char *tth, char *tthl, int len) {
  struct dl *dl = g_hash_table_lookup(dl_queue, tth);
  struct dl_user *du = g_hash_table_lookup(queue_users, &uid);
  if(!dl || !du)
    return;
  g_return_if_fail(du->state == DLU_ACT);
  g_return_if_fail(!dl->islist);
  g_return_if_fail(!dl->have);
  // Ignore this if we already have the TTHL data. Currently, this situation
  // can't happen, but it may be possible with segmented downloading.
  g_return_if_fail(!dl->hastthl);

  g_debug("dl:%016"G_GINT64_MODIFIER"x: Received TTHL data for %s (len = %d, bs = %"G_GUINT64_FORMAT")", uid, dl->dest, len, tth_blocksize(dl->size, len/24));

  // Validate correctness with the root hash
  char root[24];
  tth_root(tthl, len/24, root);
  if(memcmp(root, dl->hash, 24) != 0) {
    g_warning("dl:%016"G_GINT64_MODIFIER"x: Incorrect TTHL for %s.", uid, dl->dest);
    dl_queue_setuerr(uid, tth, DLE_INVTTHL, 0);
    return;
  }

  dl->hastthl = TRUE;
  dl->hash_block = tth_blocksize(dl->size, len/24);

  // save to dl.dat
  char key[25];
  key[0] = DLDAT_TTHL;
  memcpy(key+1, dl->hash, 24);
  datum keydat = { key, 25 };
  datum val = { tthl, len };
  gdbm_store(dl_dat, keydat, val, GDBM_REPLACE);
  dl_dat_sync();
}





// Loading/initializing the download queue on startup

// Calculates dl->hash_block, dl->have and if necessary updates dl->hash_tth
// for the last block that we have.
static void dl_queue_loadpartial(struct dl *dl) {
  // get size of the incomplete file
  char tth[40] = {};
  base32_encode(dl->hash, tth);
  char *tmp = conf_incoming_dir();
  char *fn = g_build_filename(tmp, tth, NULL);
  g_free(tmp);
  struct stat st;
  if(stat(fn, &st) >= 0)
    dl->have = st.st_size;

  // get TTHL info
  if(dl->size < DL_MINTTHLSIZE) {
    dl->hastthl = TRUE;
    dl->hash_block = DL_MINTTHLSIZE;
  } else {
    char key[25] = { DLDAT_TTHL };
    memcpy(key+1, dl->hash, 24);
    datum keydat = { key, 25 };
    datum val = gdbm_fetch(dl_dat, keydat);
    if(!val.dptr) {
      // No TTHL data found? Make sure to redownload the entire file
      dl->have = 0;
      unlink(fn);
    } else {
      dl->hastthl = TRUE;
      dl->hash_block = tth_blocksize(dl->size, val.dsize/24);
      free(val.dptr);
    }
  }

  // If we have already downloaded some data, hash the last block and update
  // dl->hash_tth.
  guint64 left = dl->hash_block ? dl->have % dl->hash_block : 0;
  if(left > 0) {
    dl->have -= left;
    int fd = open(fn, O_RDONLY);
    if(fd < 0 || lseek(fd, dl->have, SEEK_SET) == (off_t)-1) {
      g_warning("Error opening %s: %s. Throwing away last block.", fn, g_strerror(errno));
      left = 0;
      if(fd >= 0)
        close(fd);
    }
    while(left > 0) {
      char buf[1024];
      int r = read(fd, buf, MIN(left, 1024));
      if(r < 0) {
        g_warning("Error reading from %s: %s. Throwing away unreadable data.", fn, g_strerror(errno));
        left = 0;
        break;
      }
      dl_hash_update(dl, r, buf);
      dl->have += r;
      left -= r;
    }
    if(fd >= 0)
      close(fd);
  }

  g_free(fn);
}


// loads a single item from the data file
static void dl_queue_loaditem(char *hash) {
  char key[25];
  memcpy(key+1, hash, 24);
  datum keydat = { key, 25 };

  key[0] = DLDAT_INFO;
  datum nfo = gdbm_fetch(dl_dat, keydat);
  g_return_if_fail(nfo.dsize > 17);
  key[0] = DLDAT_USERS;
  datum users = gdbm_fetch(dl_dat, keydat);
  g_return_if_fail(users.dsize >= 8);

  guint64 numusers;
  memcpy(&numusers, users.dptr, 8);
  numusers = GINT64_FROM_LE(numusers);
  g_return_if_fail(users.dsize == 16 || users.dsize == 8+16*numusers);

  // Fix dl struct
  struct dl *dl = g_slice_new0(struct dl);
  memcpy(dl->hash, hash, 24);
  memcpy(&dl->size, nfo.dptr, 8);
  dl->size = GINT64_FROM_LE(dl->size);
  dl->prio = nfo.dptr[8];
  dl->error = nfo.dptr[9];
  memcpy(&dl->error_sub, nfo.dptr+10, 2);
  dl->error_sub = GINT16_FROM_LE(dl->error_sub);
  dl->dest = g_strdup(nfo.dptr+16);
  dl_queue_insert(dl, TRUE);

  // Fix users
  int i;
  for(i=0; i<numusers; i++) {
    char *ptr = users.dptr+8+16*i;
    guint64 uid;
    memcpy(&uid, ptr, 8);
    uid = GINT64_FROM_LE(uid);
    guint16 errsub = 0;
    char err = 0;
    // Pre-multisource versions only stored one user, and only its uid. (=16 bytes in total)
    if(users.dsize > 16) {
      memcpy(&errsub, ptr+10, 2);
      errsub = GINT16_FROM_LE(errsub);
    }
    dl_user_add(dl, uid, err, errsub);
  }

  // check what we already have
  dl_queue_loadpartial(dl);

  // and insert in the hash tables
  g_debug("dl: load `%s' from data file (sources = %d, size = %"G_GUINT64_FORMAT", have = %"G_GUINT64_FORMAT", bs = %"G_GUINT64_FORMAT")",
    dl->dest, dl->u->len, dl->size, dl->have, dl->hash_block);

  free(nfo.dptr);
  free(users.dptr);
}


// loads the queued items from the data file
static void dl_queue_loaddat() {
  datum key = gdbm_firstkey(dl_dat);
  while(key.dptr) {
    char *str = key.dptr;
    if(key.dsize == 25 && str[0] == DLDAT_INFO)
      dl_queue_loaditem(str+1);
    key = gdbm_nextkey(dl_dat, key);
    free(str);
  }
}


void dl_init_global() {
  queue_users = g_hash_table_new(g_int64_hash, g_int64_equal);
  dl_queue = g_hash_table_new(g_int_hash, tiger_hash_equal);
  // open data file
  char *fn = g_build_filename(conf_dir, "dl.dat", NULL);
  dl_dat = gdbm_open(fn, 0, GDBM_WRCREAT, 0600, NULL);
  g_free(fn);
  if(!dl_dat)
    g_error("Unable to open dl.dat.");
  dl_queue_loaddat();
  // Delete old filelists
  dl_fl_clean(NULL);
}


void dl_close_global() {
  gdbm_close(dl_dat);
  // Delete incomplete file lists. They won't be completed anyway.
  GHashTableIter iter;
  struct dl *dl;
  g_hash_table_iter_init(&iter, dl_queue);
  while(g_hash_table_iter_next(&iter, NULL, (gpointer *)&dl))
    if(dl->islist)
      unlink(dl->inc);
  // Delete old filelists
  dl_fl_clean(NULL);
}






// Various cleanup/gc utilities

// Removes old filelists from /fl/. Can be run from a timer.
gboolean dl_fl_clean(gpointer dat) {
  char *dir = g_build_filename(conf_dir, "fl", NULL);
  GDir *d = g_dir_open(dir, 0, NULL);
  if(!d) {
    g_free(dir);
    return TRUE;
  }

  const char *n;
  time_t ref = time(NULL) - 7*24*3600; // keep lists for one week
  while((n = g_dir_read_name(d))) {
    if(strcmp(n, ".") == 0 || strcmp(n, "..") == 0)
      continue;
    char *fn = g_build_filename(dir, n, NULL);
    struct stat st;
    if(stat(fn, &st) >= 0 && st.st_mtime < ref)
      unlink(fn);
    g_free(fn);
  }
  g_dir_close(d);
  g_free(dir);
  return TRUE;
}


// Removes unused files in <incoming_dir>.
static void dl_inc_clean() {
  char *dir = conf_incoming_dir();
  GDir *d = g_dir_open(dir, 0, NULL);
  if(!d) {
    g_free(dir);
    return;
  }

  const char *n;
  char hash[24];
  while((n = g_dir_read_name(d))) {
    // Only consider files that we have created, which always happen to have a
    // base32-encoded hash as filename.
    if(!istth(n))
      continue;
    base32_decode(n, hash);
    if(g_hash_table_lookup(dl_queue, hash))
      continue;
    // not in the queue? delete.
    char *fn = g_build_filename(dir, n, NULL);
    unlink(fn);
    g_free(fn);
  }
  g_dir_close(d);
  g_free(dir);
}


// Removes unused items from dl.dat, performs a gdbm_reorganize(), removes
// unused files in /inc/ and calls dl_fl_clean().
// The dl.dat cleaning code is based on based on fl_hashdat_gc().
void dl_gc() {
  GSList *rm = NULL;
  char hash[40];
  hash[39] = 0;

  // check for unused keys
  datum key = gdbm_firstkey(dl_dat);
  char *freethis = NULL;
  for(; key.dptr; key=gdbm_nextkey(dl_dat, key)) {
    char *str = key.dptr;
    if(freethis)
      free(freethis);
    // We only touch keys that this or earlier versions of ncdc could have
    // created. Unknown keys are left untouched as a later version could have
    // made these and there is no way to tell whether these need to be cleaned
    // up or not.
    if(key.dsize == 25 && (str[0] == DLDAT_INFO || str[0] == DLDAT_USERS || str[0] == DLDAT_TTHL)
        && !g_hash_table_lookup(dl_queue, str+1)) {
      base32_encode(str+1, hash);
      g_message("Removing unused key in dl.dat: type = %d, hash = %s", str[0], hash);
      rm = g_slist_prepend(rm, str);
      freethis = NULL;
    } else
      freethis = str;
  }
  if(freethis)
    free(freethis);

  // delete the unused keys
  GSList *n = rm;
  key.dsize = 25;
  while(n) {
    rm = n->next;
    key.dptr = n->data;
    gdbm_delete(dl_dat, key);
    free(n->data);
    g_slist_free_1(n);
    n = rm;
  }

  // perform the reorganize
  gdbm_reorganize(dl_dat);

  // remove unused files in /inc/
  dl_inc_clean();

  // remove old files in /fl/
  dl_fl_clean(NULL);
}

