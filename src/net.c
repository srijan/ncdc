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
#include <gio/gfiledescriptorbased.h>
#ifdef HAVE_LINUX_SENDFILE
# include <sys/sendfile.h>
#elif HAVE_BSD_SENDFILE
# include <sys/socket.h>
# include <sys/uio.h>
#endif


// global network stats
struct ratecalc net_in, net_out;

#if INTERFACE


// actions that can fail
#define NETERR_CONN 0
#define NETERR_RECV 1
#define NETERR_SEND 2

#define NET_RECV_BUF 8192
#define NET_MAX_CMD  1048576

struct net {
  GIOStream *conn; // either a (raw, not wrapped) GSocketConnection or a GTlsConnection
  char addr[50];
  gboolean tls;

  // Connecting
  gboolean connecting;
  GCancellable *conn_can;
#if TLS_SUPPORT
  gboolean (*conn_accept_cert)(GTlsConnection *, GTlsCertificate *, GTlsCertificateFlags, gpointer);
#endif

  // Message termination character. ([0] = character, [1] = 0)
  char eom[2];

  // Receiving data
  // raw receive buffer for _read_async()
  GInputStream *in;
  GCancellable *in_can;
  char in_buf[NET_RECV_BUF];
  // Regular messages
  GString *in_msg;
  void (*recv_msg_cb)(struct net *, char *);
  // Receiving raw data
  guint64 recv_raw_left;
  void (*recv_raw_cb)(struct net *, char *, int, guint64);
  // special hook that is called when data has arrived but before it is processed.
  void (*recv_datain)(struct net *, char *data, int len);

  // Sending data
  GOutputStream *out;
  GCancellable *out_can;
  // Regular data
  GString *out_buf;
  GString *out_buf_old;
  int out_queue_src;
  // Sending a file.
  // A file upload will start when out_buf->len == 0 && file_left > 0 && !file_busy.
  // file_left will be updated from the file transfer thread.
  int file_left;
  gboolean file_busy;
  guint64 file_offset;
  GFileInputStream *file_in;
  void (*file_cb)(struct net *);

  // In/out rates
  struct ratecalc *rate_in;
  struct ratecalc *rate_out;

  // On-connect callback
  void (*cb_con)(struct net *);

  // Error callback. In the case of an error while connecting, cb_con will not
  // be called. Second argument is NETERR_* action. The GError does not have to
  // be freed. Will not be called in the case of G_IO_ERROR_CANCELLED. All
  // errors are fatal, and net_disconnect() should be called in the callback.
  void (*cb_err)(struct net *, int, GError *);

  // Whether this connection should be kept alive or not. When true, keepalive
  // packets will be sent. Otherwise, an error will be generated if there has
  // been no read activity within the last 30 seconds (or so).
  gboolean keepalive;

  // We use our own timeout detection using a 5-second timer and a timestamp of
  // the last successful action.
  guint timeout_src;
  time_t timeout_last;
  int timeout_left;

  // some pointer for use by the user
  void *handle;
  // reference counter
  int ref;
};


#define net_ref(n) g_atomic_int_inc(&((n)->ref))

#define net_file_left(n) g_atomic_int_get(&(n)->file_left)

#define net_remoteaddr(n) ((n)->addr)

#endif


static gboolean handle_timer(gpointer dat) {
  struct net *n = dat;
  time_t t = time(NULL);

  if(!n->conn)
    return TRUE;

  // if file_left has changed, that means there has been some activity.
  // (timeout_last isn't updated from the file transfer thread)
  if(net_file_left(n) != n->timeout_left) {
    n->timeout_last = t;
    n->timeout_left = net_file_left(n);
  }

  // keepalive? send an empty command every 2 minutes of inactivity
  if(n->keepalive && n->timeout_last < t-120)
    net_send(n, "");

  // not keepalive? give a timeout after 30 seconds of inactivity
  else if(!n->keepalive && n->timeout_last < t-30) {
    GError *err = NULL;
    g_set_error_literal(&err, 1, G_IO_ERROR_TIMED_OUT, "Idle timeout.");
    n->cb_err(n, NETERR_RECV, err); // actually not _RECV, but whatever
    g_error_free(err);
    return FALSE;
  }
  return TRUE;
}


// When len new bytes have been received. We don't have to worry about n->ref
// dropping to 0 within this function, the caller (that is, handle_read())
// makes sure to have a reference.
static void handle_input(struct net *n, char *buf, int len) {
  if(n->recv_datain)
    n->recv_datain(n, buf, len);

  // If we're still receiving raw data, send that to the appropriate callbacks
  // first.
  if(len > 0 && n->recv_raw_left > 0) {
    int w = MIN(len, n->recv_raw_left);
    n->recv_raw_left -= w;
    n->recv_raw_cb(n, buf, w, n->recv_raw_left);
    len -= w;
    buf += w;
  }

  if(!n->conn || len <= 0)
    return;

  // Check that the maximum command length isn't reached.
  // (Actually, this should be checked after this command finishes, but oh well)
  if(n->in_msg->len > NET_MAX_CMD) {
    GError *err = NULL;
    g_set_error_literal(&err, 1, 0, "Buffer overflow.");
    n->cb_err(n, NETERR_RECV, err);
    g_error_free(err);
    return;
  }

  // Now we apparently have some data that needs to be interpreted as messages.
  // Add to the message buffer and interpret it.
  g_string_append_len(n->in_msg, buf, len);

  // Make sure the message is consumed from n->in_msg before the callback is
  // called, otherwise net_recvfile() can't do its job.
  char *sep;
  while(n->conn && (sep = memchr(n->in_msg->str, n->eom[0], n->in_msg->len))) {
    // The n->in->str+1 is a hack to work around a bug in uHub 0.2.8 (possibly
    // also other versions), where it would prefix some messages with a 0-byte.
    char *msg = !n->in_msg->str[0] && sep > n->in_msg->str+1
      ? g_strndup(n->in_msg->str+1, sep - n->in_msg->str - 1)
      : g_strndup(n->in_msg->str, sep - n->in_msg->str);
    g_string_erase(n->in_msg, 0, 1 + sep - n->in_msg->str);
    g_debug("%s%s< %s", net_remoteaddr(n), n->tls ? "S" : "", msg);
    if(msg[0])
      n->recv_msg_cb(n, msg);
    g_free(msg);
  }
}


// Activates an asynchronous read, in case there's none active.
#define setup_read(n) do {\
    if(n->conn && !g_input_stream_has_pending(n->in)) {\
      g_input_stream_read_async(n->in, n->in_buf, NET_RECV_BUF, G_PRIORITY_DEFAULT, n->in_can, handle_read, n);\
      net_ref(n);\
      g_object_ref(n->in);\
    }\
  } while(0)


// Called when an asynchronous read has finished. Checks for errors and calls
// handle_input() on the received data.
static void handle_read(GObject *src, GAsyncResult *res, gpointer dat) {
  struct net *n = dat;

  GError *err = NULL;
  gssize r = g_input_stream_read_finish(G_INPUT_STREAM(src), res, &err);
  g_object_unref(src);

  if(r < 0) {
    if(n->conn && err->code != G_IO_ERROR_CANCELLED)
      n->cb_err(n, NETERR_RECV, err);
    g_error_free(err);
  } else if(n->conn && r == 0) {
    g_set_error_literal(&err, 1, 0, "Remote disconnected.");
    n->cb_err(n, NETERR_RECV, err);
    g_error_free(err);
  } else if(n->conn) {
    time(&(n->timeout_last));
    ratecalc_add(&net_in, r);
    ratecalc_add(n->rate_in, r);
    handle_input(n, n->in_buf, r);
    setup_read(n);
  }
  net_unref(n);
}


struct net *net_create(char term, void *han, gboolean keepalive, void (*rfunc)(struct net *, char *), void (*errfunc)(struct net *, int, GError *)) {
  struct net *n = g_new0(struct net, 1);
  n->ref = 1;
  n->rate_in  = g_slice_new0(struct ratecalc);
  n->rate_out = g_slice_new0(struct ratecalc);
  ratecalc_init(n->rate_in);
  ratecalc_init(n->rate_out);
  n->conn_can = g_cancellable_new();
  n->in_can   = g_cancellable_new();
  n->out_can  = g_cancellable_new();
  n->in_msg  = g_string_sized_new(1024);
  n->out_buf = g_string_sized_new(1024);
  n->eom[0] = term;
  n->handle = han;
  n->keepalive = keepalive;
  n->recv_msg_cb = rfunc;
  n->cb_err = errfunc;
  time(&(n->timeout_last));
  n->timeout_src = g_timeout_add_seconds(5, handle_timer, n);
  strcpy(n->addr, "(not connected)");
  return n;
}


void net_setconn(struct net *n, GSocketConnection *conn, gboolean tls, gboolean serv) {
  g_return_if_fail(!G_IS_TCP_WRAPPER_CONNECTION(conn));

  // disable the timeout set in net_connect(), we have our own idle timer
#if TIMEOUT_SUPPORT
  g_socket_set_timeout(g_socket_connection_get_socket(G_SOCKET_CONNECTION(conn)), 0);
#endif
  time(&(n->timeout_last));
  if(n->keepalive)
    g_socket_set_keepalive(g_socket_connection_get_socket(conn), TRUE);

  // get/cache address
  GInetSocketAddress *addr = G_INET_SOCKET_ADDRESS(g_socket_connection_get_remote_address(conn, NULL));
  if(!addr) {
    g_warning("g_socket_connection_get_remote_address() returned NULL");
    strcpy(n->addr, "(not connected)");
  } else {
    char *ip = g_inet_address_to_string(g_inet_socket_address_get_address(addr));
    g_snprintf(n->addr, sizeof(n->addr), "%s:%d",
        strncmp("::ffff:", ip, 7) == 0 ? ip+7 : ip,
        g_inet_socket_address_get_port(addr));
    g_free(ip);
    g_object_unref(addr);
  }

  // Set n->conn and wrap around a TlsConnection when requested
  n->tls = FALSE;
  n->conn = G_IO_STREAM(conn);
#if TLS_SUPPORT
  if(tls && have_tls_support) {
    n->tls = TRUE;
    // Create a tls connection and replace n->conn
    GIOStream *tls = serv
      ? g_tls_server_connection_new(n->conn, conf_certificate, NULL)
      : g_tls_client_connection_new(n->conn, NULL, NULL);
    g_return_if_fail(tls);
    g_object_unref(n->conn);
    n->conn = tls;
    // set TLS options
    g_tls_connection_set_use_system_certdb(G_TLS_CONNECTION(n->conn), FALSE);
    if(!serv) {
      if(n->conn_accept_cert)
        g_signal_connect(n->conn, "accept-certificate", G_CALLBACK(n->conn_accept_cert), n);
      else
        g_tls_client_connection_set_validation_flags(G_TLS_CLIENT_CONNECTION(n->conn), 0);
      // allow the server to fetch our certificate if it wants to
      if(conf_certificate)
        g_tls_connection_set_certificate(G_TLS_CONNECTION(n->conn), conf_certificate);
    }
  }
#endif

  // Setup and initiate read/write streams
  n->in  = g_io_stream_get_input_stream(G_IO_STREAM(n->conn));
  n->out = g_io_stream_get_output_stream(G_IO_STREAM(n->conn));
  setup_read(n);

  // and enable rate calculation
  ratecalc_reset(n->rate_in);
  ratecalc_reset(n->rate_out);
  ratecalc_register(n->rate_in);
  ratecalc_register(n->rate_out);

  g_debug("%s%s- Connected.", net_remoteaddr(n), n->tls ? "S" : "");
}


static void handle_connect(GObject *src, GAsyncResult *res, gpointer dat) {
  struct net *n = dat;

  GError *err = NULL;
  GSocketConnection *conn = g_socket_client_connect_to_host_finish(G_SOCKET_CLIENT(src), res, &err);

  if(!conn) {
    if(n->connecting && err->code != G_IO_ERROR_CANCELLED)
      n->cb_err(n, NETERR_CONN, err);
    g_error_free(err);
  } else if(n->connecting) {
    net_setconn(n, conn, n->tls, FALSE);
    n->cb_con(n);
  }
  n->connecting = FALSE;
  g_object_unref(src);
  net_unref(n);
}


void net_connect(struct net *n, const char *addr, unsigned short defport, gboolean tls, void (*cb)(struct net *)) {
  g_return_if_fail(!n->conn);
  n->cb_con = cb;
  n->tls = tls;

  // From g_socket_client_connect_to_host() documentation:
  //   "In general, host_and_port is expected to be provided by the user"
  // But it doesn't properly give an error when the URL contains a space.
  if(strchr(addr, ' ')) {
    GError *err = NULL;
    g_set_error_literal(&err, 1, G_IO_ERROR_INVALID_ARGUMENT, "Address may not contain a space.");
    n->cb_err(n, NETERR_CONN, err);
    g_error_free(err);
    return;
  }

  GSocketClient *sc = g_socket_client_new();
  // set a timeout on the connect, regardless of the value of keepalive
#if TIMEOUT_SUPPORT
  g_socket_client_set_timeout(sc, 30);
#endif
  g_socket_client_connect_to_host_async(sc, addr, defport, n->conn_can, handle_connect, n);
  n->connecting = TRUE;
  net_ref(n);
  time(&(n->timeout_last));
}


// Calls when an async close() has finished. This doesn't do anything but
// release the resources of the connection. This function is also unaware of
// any net struct.
static void handle_close(GObject *src, GAsyncResult *res, gpointer dat) {
  g_io_stream_close_finish(G_IO_STREAM(src), res, NULL);
  g_object_unref(src);
}


void net_disconnect(struct net *n) {
  if(!n->conn && !n->connecting)
    return;

  n->connecting = FALSE;
  if(n->out_queue_src) {
    g_source_remove(n->out_queue_src);
    n->out_queue_src = 0;
  }

#define cancel_and_reset(c) if(c) {\
    g_cancellable_cancel(c);\
    g_object_unref(c);\
    c = g_cancellable_new();\
  }
  cancel_and_reset(n->conn_can);
  cancel_and_reset(n->in_can);
  cancel_and_reset(n->out_can);
#undef cancel_and_reset

  g_debug("%s%s- Disconnected.", net_remoteaddr(n), n->tls ? "S" : "");
  strcpy(n->addr, "(not connected)");
  if(n->conn) {
    // There is a bug in the gnutls backend of glib-networking (2.28.7) that is
    // triggered when the cancellable argument here is NULL when n->conn is a
    // GTlsConnection object. Use some cancellable object to avoid it.
    GCancellable *can = g_cancellable_new();
    g_io_stream_close_async(n->conn, G_PRIORITY_DEFAULT, can, handle_close, NULL);
    g_object_unref(can);
  }
  n->out = NULL;
  n->in = NULL;
  n->conn = NULL;
  g_string_truncate(n->in_msg, 0);
  n->recv_raw_left = 0;
  n->recv_raw_cb = NULL;
  g_string_truncate(n->out_buf, 0);
  if(n->out_buf_old) {
    g_string_free(n->out_buf_old, TRUE);
    n->out_buf_old = NULL;
  }
  if(n->file_in) {
    g_object_unref(n->file_in);
    n->file_in = NULL;
  }
  n->file_left = 0;
  n->file_busy = FALSE;
  ratecalc_unregister(n->rate_in);
  ratecalc_unregister(n->rate_out);
  time(&n->timeout_last);
}


void net_unref(struct net *n) {
  if(!g_atomic_int_dec_and_test(&((n)->ref)))
    return;

  net_disconnect(n);
  g_source_remove(n->timeout_src);
  g_object_unref(n->conn_can);
  g_object_unref(n->in_can);
  g_object_unref(n->out_can);
  g_string_free(n->in_msg, TRUE);
  g_string_free(n->out_buf, TRUE);
  g_slice_free(struct ratecalc, n->rate_in);
  g_slice_free(struct ratecalc, n->rate_out);
  g_free(n);
}


// Receives `length' bytes from the socket and calls cb() on every read.
void net_recvraw(struct net *n, guint64 length, void (*cb)(struct net *, char *, int, guint64)) {
  n->recv_raw_left = length;
  n->recv_raw_cb = cb;
  // read stuff from the message buffer in case it's not empty.
  if(n->in_msg->len >= 0) {
    int w = MIN(n->in_msg->len, length);
    n->recv_raw_left -= w;
    n->recv_raw_cb(n, n->in_msg->str, w, n->recv_raw_left);
    g_string_erase(n->in_msg, 0, w);
  }
  if(!n->recv_raw_left)
    n->recv_raw_cb = NULL;
}





// The following functions are somewhat similar to g_output_stream_splice(),
// except that this solution DOES, in fact, allow fetching of transfer
// progress. It updates the rate calculation objects and writes to n->file_left.

static GThreadPool *file_pool = NULL; // initialized in net_init_global();


struct file_ctx {
  struct net *n;          // only for access to rate_out and file_left (atomic int)
  GOutputStream *out;     // ref of n->out
  GCancellable *can;      // ref of n->out_can
  GSocket *sock;          // NULL if sendfile() shouldn't be used
  GFileInputStream *file; // file stream to send
  guint64 offset;
  GError *err;
};


static gboolean file_done(gpointer dat) {
  struct file_ctx *c = dat;

  if(!g_cancellable_is_cancelled(c->can)) {
    c->n->file_left = 0;
    c->n->file_busy = FALSE;
    if(c->err)
      c->n->cb_err(c->n, NETERR_SEND, c->err);
    else if(c->n->file_cb)
      c->n->file_cb(c->n);
  }

  g_object_unref(c->can);
  g_object_unref(c->out);
  g_object_unref(c->file);
  if(c->sock)
    g_object_unref(c->sock);
  if(c->err)
    g_error_free(c->err);
  net_unref(c->n);
  g_slice_free(struct file_ctx, c);
  return FALSE;
}


// Inspired by glib:gio/goutputstream.c:g_output_stream_real_splice().
static void file_thread(gpointer dat, gpointer udat) {
  struct file_ctx *c = dat;
  char buf[8192];

  if(g_cancellable_is_cancelled(c->can)) {
    g_set_error_literal(&c->err, 1, G_IO_ERROR_CANCELLED, "Operation cancelled");
    goto file_thread_done;
  }
  int total = g_atomic_int_get(&c->n->file_left);
  int left = total;

  // sendfile()-based sending
#ifdef HAVE_SENDFILE
  int fd = c->sock && G_IS_FILE_DESCRIPTOR_BASED(c->file)
    ? g_file_descriptor_based_get_fd(G_FILE_DESCRIPTOR_BASED(c->file)) : -1;
  while(fd > 0 && left > 0) {
    // Wait for the socket to be writable, to ensure that sendfile() won't
    // block for too long. sendfile() isn't cancellable, after all.
    if(!g_socket_condition_wait(c->sock, G_IO_OUT, c->can, &c->err))
      break;

    // call sendfile()
    off_t off = c->offset+(total-left);
#ifdef HAVE_LINUX_SENDFILE
    ssize_t r = sendfile(g_socket_get_fd(c->sock), fd, &off, left);
#elif HAVE_BSD_SENDFILE
    off_t len = 0;
    gint64 r = sendfile(fd, g_socket_get_fd(c->sock), off, (size_t)left, NULL, &len, 0);
    // a partial write results in an EAGAIN error on BSD, even though this isn't
    // really an error condition at all.
    if(r != -1 || (r == -1 && errno == EAGAIN))
      r = len;
#endif

    // check for errors
    if(r >= 0) {
      left -= r;
      ratecalc_add(&net_out, r);
      ratecalc_add(c->n->rate_out, r);
      g_atomic_int_set(&c->n->file_left, left);
      continue;
    } else if(errno == EAGAIN || errno == EINTR) {
      continue;
    } else if(errno == EPIPE || errno == ECONNRESET) {
      g_set_error_literal(&c->err, 1, 0, "Remote disconnected.");
      break;
    } else if(errno == ENOTSUP || errno == ENOSYS || errno == EINVAL) {
      g_message("sendfile() failed with `%s', using fallback.", g_strerror(errno));
      // Don't set n->file_err here, let the fallback handle the rest
      break;
    } else {
      g_critical("sendfile() returned an unknown error: %d (%s)", errno, g_strerror(errno));
      g_set_error_literal(&c->err, 1, 0, "Sendfile() error.");
      break;
    }
  }
#endif

  // non-sendfile() fallback
  gboolean res = left > 0 && !c->err;
  if(res && !g_seekable_seek(G_SEEKABLE(c->file), c->offset+(total-left), G_SEEK_SET, NULL, &c->err))
    res = FALSE;
  int r, w;
  while(res && left > 0) {
    r = g_input_stream_read(G_INPUT_STREAM(c->file), buf, MIN(left, sizeof(buf)), c->can, &c->err);
    if(r < 0)
      break;

    if(r == 0) {
      g_set_error_literal(&c->err, 1, 0, "Unexpected EOF.");
      break;
    }

    // Don't use write_all() here, screws up the granularity of the rate
    // calculation and file_left.
    char *p = buf;
    while(r > 0) {
      w = g_output_stream_write(c->out, p, r, c->can, &c->err);
      if(w <= 0) {
        res = FALSE;
        break;
      }
      r -= w;
      left -= w;
      p += w;
      ratecalc_add(&net_out, w);
      ratecalc_add(c->n->rate_out, w);
      g_atomic_int_set(&c->n->file_left, left);
    }
  }

file_thread_done:
  g_idle_add(file_done, c);
}


static void file_start(struct net *n) {
  g_return_if_fail(n->file_in && !n->file_busy);

  struct file_ctx *c = g_slice_new0(struct file_ctx);
  c->n = n;
  net_ref(n);
  c->can = n->out_can;
  g_object_ref(c->can);
  c->out = n->out;
  g_object_ref(c->out);
  c->file = n->file_in; // we now claim ownership of it
  n->file_in = NULL;
  c->offset = n->file_offset;
  n->file_busy = TRUE;

#if TLS_SUPPORT
  if(!n->tls) {
#endif
    c->sock = g_socket_connection_get_socket(G_SOCKET_CONNECTION(n->conn));
    g_object_ref(c->sock);
#if TLS_SUPPORT
  }
#endif
  g_thread_pool_push(file_pool, c, NULL);
}




static gboolean setup_write(gpointer dat);


static void handle_write(GObject *src, GAsyncResult *res, gpointer dat) {
  struct net *n = dat;

  GError *err = NULL;
  gssize r = g_output_stream_write_finish(G_OUTPUT_STREAM(src), res, &err);
  g_object_unref(src);

  if(r < 0) {
    if(n->conn && err->code != G_IO_ERROR_CANCELLED)
      n->cb_err(n, NETERR_SEND, err);
    g_error_free(err);
  } else if(n->conn) {
    time(&(n->timeout_last));
    ratecalc_add(&net_out, r);
    ratecalc_add(n->rate_out, r);
    if(n->out_buf_old) {
      g_string_free(n->out_buf_old, TRUE);
      n->out_buf_old = NULL;
    } else
      g_string_erase(n->out_buf, 0, r);
    setup_write(n);
  }
  net_unref(n);
}


static gboolean setup_write(gpointer dat) {
  struct net *n = dat;
  if(n->out_queue_src)
    n->out_queue_src = 0;
  if(!n->conn || g_output_stream_has_pending(n->out) || n->file_busy)
    return FALSE;

  if(n->out_buf->len) {
    g_output_stream_write_async(n->out, n->out_buf->str, n->out_buf->len, G_PRIORITY_DEFAULT, n->out_can, handle_write, n);
    g_object_ref(n->out);
    net_ref(n);
  } else if(n->file_left)
    file_start(n);
  return FALSE;
}


void net_sendraw(struct net *n, const char *buf, int len) {
  if(!n->conn)
    return;
  g_return_if_fail(!n->file_left);
  // Don't write to the output buffer when an asynchronous write is in
  // progress. Otherwise we may risk the write thread to read invalid memory.
  if(g_output_stream_has_pending(n->out)) {
    n->out_buf_old = n->out_buf;
    n->out_buf = g_string_sized_new(1024);
  }
  g_string_append_len(n->out_buf, buf, len);

  // Queue a setup_write() from an idle source. This ensures that batch calls
  // to net_sendraw() will combine stuff into a single buffer before writing it.
  if(!g_output_stream_has_pending(n->out) && !n->out_queue_src)
    n->out_queue_src = g_idle_add_full(G_PRIORITY_LOW, setup_write, n, NULL);
}


void net_send(struct net *n, const char *msg) {
  g_debug("%s%s> %s", net_remoteaddr(n), n->tls ? "S" : "", msg);
  net_sendraw(n, msg, strlen(msg));
  net_sendraw(n, n->eom, 1);
}


void net_sendf(struct net *n, const char *fmt, ...) {
  va_list va;
  va_start(va, fmt);
  char *str = g_strdup_vprintf(fmt, va);
  va_end(va);
  net_send(n, str);
  g_free(str);
}


void net_sendfile(struct net *n, const char *path, guint64 offset, int length, void (*cb)(struct net *)) {
  if(!length) {
    if(cb)
      cb(n);
    return;
  }
  // Open file
  GError *err = NULL;
  GFile *fn = g_file_new_for_path(path);
  GFileInputStream *fis = g_file_read(fn, NULL, &err);
  g_object_unref(fn);

  if(err) {
    n->cb_err(n, NETERR_SEND, err);
    g_error_free(err);
    if(fis)
      g_object_unref(fis);
    return;
  }

  // Set state and setup write
  n->file_in = fis;
  n->file_left = length;
  n->file_cb = cb;
  n->file_offset = offset;
  setup_write(n);
}






// Some global stuff for sending UDP packets

struct net_udp { GSocketAddress *dest; char *msg; int msglen; };
static GSocket *net_udp_sock;
static GQueue *net_udp_queue;


static gboolean udp_handle_out(GSocket *sock, GIOCondition cond, gpointer dat) {
  struct net_udp *m = g_queue_pop_head(net_udp_queue);
  if(!m)
    return FALSE;
  if(g_socket_send_to(net_udp_sock, m->dest, m->msg, m->msglen, NULL, NULL) != m->msglen)
    g_warning("Short write for UDP message.");
  else {
    ratecalc_add(&net_out, m->msglen);
    char *a = g_inet_address_to_string(g_inet_socket_address_get_address(G_INET_SOCKET_ADDRESS(m->dest)));
    g_debug("UDP:%s:%d> %s", a, g_inet_socket_address_get_port(G_INET_SOCKET_ADDRESS(m->dest)), m->msg);
    g_free(a);
  }
  g_free(m->msg);
  g_object_unref(m->dest);
  g_slice_free(struct net_udp, m);
  return net_udp_queue->head ? TRUE : FALSE;
}


// dest is assumed to be a valid IPv4 address with an optional port ("x.x.x.x" or "x.x.x.x:p")
void net_udp_send_raw(const char *dest, const char *msg, int len) {
  char *destc = g_strdup(dest);
  char *port_str = strchr(destc, ':');
  long port = 412;
  if(port_str) {
    *port_str = 0;
    port_str++;
    port = strtol(port_str, NULL, 10);
    if(port < 0 || port > 0xFFFF) {
      g_free(destc);
      return;
    }
  }

  GInetAddress *iaddr = g_inet_address_new_from_string(destc);
  g_free(destc);
  if(!iaddr)
    return;

  struct net_udp *m = g_slice_new0(struct net_udp);
  m->msg = g_strdup(msg);
  m->msglen = len;
  m->dest = G_SOCKET_ADDRESS(g_inet_socket_address_new(iaddr, port));
  g_object_unref(iaddr);

  g_queue_push_tail(net_udp_queue, m);
  if(net_udp_queue->head == net_udp_queue->tail) {
    GSource *src = g_socket_create_source(net_udp_sock, G_IO_OUT, NULL);
    g_source_set_callback(src, (GSourceFunc)udp_handle_out, NULL, NULL);
    g_source_attach(src, NULL);
    g_source_unref(src);
  }
}


void net_udp_send(const char *dest, const char *msg) {
  net_udp_send_raw(dest, msg, strlen(msg));
}


void net_udp_sendf(const char *dest, const char *fmt, ...) {
  va_list va;
  va_start(va, fmt);
  char *str = g_strdup_vprintf(fmt, va);
  va_end(va);
  net_udp_send_raw(dest, str, strlen(str));
  g_free(str);
}








// initialize some global structures

void net_init_global() {
  ratecalc_init(&net_in);
  ratecalc_init(&net_out);
  ratecalc_register(&net_in);
  ratecalc_register(&net_out);

  file_pool = g_thread_pool_new(file_thread, NULL, -1, FALSE, NULL);

  // TODO: IPv6?
  net_udp_sock = g_socket_new(G_SOCKET_FAMILY_IPV4, G_SOCKET_TYPE_DATAGRAM, G_SOCKET_PROTOCOL_UDP, NULL);
  g_socket_set_blocking(net_udp_sock, FALSE);
  net_udp_queue = g_queue_new();
}

