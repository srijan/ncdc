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
#include <limits.h>
#include <stdlib.h>
#include <sys/stat.h>
#include <fcntl.h>
#ifdef HAVE_LINUX_SENDFILE
# include <sys/sendfile.h>
#elif HAVE_BSD_SENDFILE
# include <sys/socket.h>
# include <sys/uio.h>
#endif



/* High-level connection handling for message-based protocols. With some binary
 * transfer stuff mixed in.
 *
 * Implements the following:
 * - Async connecting to a hostname/ip + port
 * - Async message sending (end-of-message char is added automatically)
 * - Async message receiving ("message" = all bytes until end-of-message char)
 * - Sending a file over a socket
 * - Sending UDP messages
 *
 * Does not use the GIOStream interface, since that is inefficient and has too
 * many limitations to be useful.
 */


// global network stats
struct ratecalc net_in, net_out;

#if INTERFACE


// actions that can fail
#define NETERR_CONN 0
#define NETERR_RECV 1
#define NETERR_SEND 2

struct net {
  GSocketConnection *conn;
  GSocket *sock;
  // input/output buffers
  GString *in, *out;
  GCancellable *cancel; // used to cancel a connect operation
  guint in_src, out_src;
  // sending a file
  int file_fd;
  gint64 file_left;
  guint64 file_offset;
  // receive callback
  void (*cb_rcv)(struct net *, char *);
  // on-connect callback
  void (*cb_con)(struct net *);
  // Error callback. In the case of an error while connecting, cb_con will not
  // be called. Second argument is NETERR_* action. The GError does not have to
  // be freed. Will not be called in the case of G_IO_ERROR_CANCELLED.
  void (*cb_err)(struct net *, int, GError *);
  // message termination character
  char eom[2];
  // Whether this connection should be kept alive or not. When true, keepalive
  // packets will be sent. Otherwise, an error will be generated if there has
  // been no read activity within the last 30 seconds (or so).
  gboolean keepalive;
  // Don't use _set_timeout() on the socket, since that will throw a timeout
  // even when we're actively writing to the socket. So we use our own timeout
  // detection using a 5-second timer and a timestamp of the last action.
  guint timeout_src;
  time_t timeout_last;
  // some pointer for use by the user
  void *handle;
  // reference counter
  int ref;
};


// g_socket_create_source() has a cancellable argument. But I can't tell from
// the documentation that it does exactly what I want it to do, so let's just
// use g_source_remove() manually.
#define net_cancel(n) do {\
    if((n)->in_src) {\
      g_source_remove((n)->in_src);\
      (n)->in_src = 0;\
    }\
    if((n)->out_src) {\
      g_source_remove((n)->out_src);\
      (n)->out_src = 0;\
    }\
    g_cancellable_cancel((n)->cancel);\
    g_object_unref((n)->cancel);\
    (n)->cancel = g_cancellable_new();\
  } while(0)


// does this function block?
#define net_disconnect(n) do {\
    if((n)->conn) {\
      g_debug("%s- Disconnected.", net_remoteaddr(n));\
      net_cancel(n);\
      g_object_unref((n)->conn);\
      (n)->conn = NULL;\
    }\
  } while(0)


#define net_ref(n) g_atomic_int_inc(&((n)->ref))

#define net_unref(n) do {\
    if(g_atomic_int_dec_and_test(&((n)->ref))) {\
      net_disconnect(n);\
      if((n)->timeout_src)\
        g_source_remove((n)->timeout_src);\
      if((n)->file_left)\
        close((n)->file_fd);\
      g_object_unref((n)->cancel);\
      g_string_free((n)->out, TRUE);\
      g_string_free((n)->in, TRUE);\
      g_free(n);\
    }\
  } while(0)


#endif



// Some global stuff for sending UDP packets

struct net_udp { GSocketAddress *dest; char *msg; int msglen; };
static GSocket *net_udp_sock;
static GQueue *net_udp_queue;


// initialize some global structures
void net_init_global() {
  ratecalc_init(&net_in, 10);
  ratecalc_init(&net_out, 10);
  ratecalc_register(&net_in);
  ratecalc_register(&net_out);

  // TODO: IPv6?
  net_udp_sock = g_socket_new(G_SOCKET_FAMILY_IPV4, G_SOCKET_TYPE_DATAGRAM, G_SOCKET_PROTOCOL_UDP, NULL);
  g_socket_set_blocking(net_udp_sock, FALSE);
  net_udp_queue = g_queue_new();
}


static void consume_input(struct net *n) {
  char *str = n->in->str;
  char *sep;
  gssize consumed = 0;

  // we need to be able to access the net object after calling a callback that
  // may do an unref().
  net_ref(n);

  // TODO: set a maximum length on a single message to prevent unbounded buffer growth
  while(n->conn && (sep = strchr(str, n->eom[0]))) {
    consumed += 1 + sep - str;
    *sep = 0;
    g_debug("%s< %s", net_remoteaddr(n), str);
    if(str[0])
      n->cb_rcv(n, str);
    str = sep+1;
  }
  if(consumed)
    g_string_erase(n->in, 0, consumed);
  net_unref(n);
}


// catches and handles any errors from g_socket_receive or g_socket_send in a
// input/output handler.
#define handle_ioerr(n, src, ret, err, action) do {\
    if(err && err->code == G_IO_ERROR_WOULD_BLOCK) {\
      g_error_free(err);\
      return TRUE;\
    }\
    if(err) {\
      n->cb_err(n, action, err);\
      g_error_free(err);\
      src = 0;\
      return FALSE;\
    }\
    if(ret == 0) {\
      g_set_error_literal(&err, 1, 0, "Remote disconnected.");\
      n->cb_err(n, action, err);\
      g_error_free(err);\
      src = 0;\
      return FALSE;\
    }\
  } while(0)


static gboolean handle_input(GSocket *sock, GIOCondition cond, gpointer dat) {
  struct net *n = dat;
  time(&(n->timeout_last));

  // make sure enough space is available in the input buffer (ugly hack, GString has no simple grow function)
  if(n->in->allocated_len - n->in->len < 1024) {
    gsize oldlen = n->in->len;
    g_string_set_size(n->in, n->in->len+1024);
    n->in->len = oldlen;
  }

  GError *err = NULL;
  gssize read = g_socket_receive(n->sock, n->in->str + n->in->len, n->in->allocated_len - n->in->len - 1, NULL, &err);
  handle_ioerr(n, n->in_src, read, err, NETERR_RECV);
  ratecalc_add(&net_in, read);
  n->in->len += read;
  n->in->str[n->in->len] = 0;
  consume_input(n);
  return TRUE;
}


// TODO: do this in a separate thread to avoid blocking on HDD reads
static gboolean handle_sendfile(struct net *n) {
#ifdef HAVE_SENDFILE

#ifdef HAVE_LINUX_SENDFILE
  off_t off = n->file_offset;
  ssize_t r = sendfile(g_socket_get_fd(n->sock), n->file_fd, &off, MIN((size_t)n->file_left, INT_MAX));
  if(r >= 0)
    n->file_offset = off;
#elif HAVE_BSD_SENDFILE
  off_t len = 0;
  gint64 r = sendfile(n->file_fd, g_socket_get_fd(n->sock), (off_t)n->file_offset, MIN((size_t)n->file_left, INT_MAX), NULL, &len, 0);
  // a partial write results in an EAGAIN error on BSD, even though this isn't
  // really an error condition at all.
  if(r != -1 || (r == -1 && errno == EAGAIN)) {
    r = len;
    n->file_offset += r;
  }
#endif

  if(r >= 0) {
    n->file_left -= r;
    ratecalc_add(&net_out, r);
    return TRUE;

  } else if(errno == EAGAIN || errno == EINTR)
    return TRUE;

  // non-sendfile() fallback
  else if(errno == ENOTSUP || errno == ENOSYS || errno == EINVAL) {
#endif // HAVE_SENDFILE
    GError *err = NULL;
    char buf[10240];
    g_return_val_if_fail(lseek(n->file_fd, n->file_offset, SEEK_SET) != (off_t)-1, FALSE);
    int r = read(n->file_fd, buf, 10240);
    g_return_val_if_fail(r >= 0, FALSE);
    gssize written = g_socket_send(n->sock, buf, r, NULL, &err);
    handle_ioerr(n, n->out_src, written, err, NETERR_SEND);
    ratecalc_add(&net_out, r);
    n->file_offset += r;
    n->file_left -= r;
    return TRUE;

#ifdef HAVE_SENDFILE
  }
  g_return_val_if_reached(FALSE);
#endif
}


static gboolean handle_output(GSocket *sock, GIOCondition cond, gpointer dat) {
  struct net *n = dat;
  time(&(n->timeout_last));

  // send our buffer
  if(n->out->len) {
    GError *err = NULL;
    gssize written = g_socket_send(n->sock, n->out->str, n->out->len, NULL, &err);
    handle_ioerr(n, n->out_src, written, err, NETERR_SEND);
    ratecalc_add(&net_out, written);
    g_string_erase(n->out, 0, written);
    if(n->out->len || n->file_left)
      return TRUE;

  // send a file
  } else if(n->file_left) {
    gboolean c = handle_sendfile(n) && (n->out->len || n->file_left);
    if(!n->file_left)
      close(n->file_fd);
    if(c)
      return TRUE;
  }

  n->out_src = 0;
  return FALSE;
}


static gboolean handle_timer(gpointer dat) {
  struct net *n = dat;
  time_t t = time(NULL);

  // keepalive? send an empty command every 2 minutes of inactivity
  if(n->keepalive && n->timeout_last < t-120)
    net_send(n, "");

  // not keepalive? give a timeout after 30 seconds of inactivity
  else if(!n->keepalive && n->timeout_last < t-30) {
    GError *err = NULL;
    g_set_error_literal(&err, 1, G_IO_ERROR_TIMED_OUT, "No activity for a too long time period.");
    n->cb_err(n, NETERR_RECV, err); // actually not _RECV, but whatever
    g_error_free(err);
    return FALSE;
  }
  return TRUE;
}


static void handle_connect(GObject *src, GAsyncResult *res, gpointer dat) {
  struct net *n = dat;

  GError *err = NULL;
  GSocketConnection *conn = g_socket_client_connect_to_host_finish(G_SOCKET_CLIENT(src), res, &err);

  if(!conn) {
    if(err->code != G_IO_ERROR_CANCELLED)
      n->cb_err(n, NETERR_CONN, err);
    g_error_free(err);
  } else {
    n->conn = conn;
    n->sock = g_socket_connection_get_socket(n->conn);
#if GLIB_CHECK_VERSION(2, 26, 0)
    g_socket_set_timeout(n->sock, 0);
#endif
    time(&(n->timeout_last));
    n->timeout_src = g_timeout_add_seconds(5, handle_timer, n);
    if(n->keepalive)
      g_socket_set_keepalive(n->sock, TRUE);
    g_socket_set_blocking(n->sock, FALSE);
    GSource *src = g_socket_create_source(n->sock, G_IO_IN, NULL);
    g_source_set_callback(src, (GSourceFunc)handle_input, n, NULL);
    n->in_src = g_source_attach(src, NULL);
    g_source_unref(src);
    g_debug("%s- Connected.", net_remoteaddr(n));
    n->cb_con(n);
  }
}


void net_connect(struct net *n, const char *addr, unsigned short defport, void (*cb)(struct net *)) {
  n->cb_con = cb;

  GSocketClient *sc = g_socket_client_new();
  // set a timeout on the connect, regardless of the value of keepalive
#if GLIB_CHECK_VERSION(2, 26, 0)
  g_socket_client_set_timeout(sc, 30);
#endif
  g_socket_client_connect_to_host_async(sc, addr, defport, n->cancel, handle_connect, n);
  g_object_unref(sc);
}


char *net_remoteaddr(struct net *n) {
  static char a[100];
  if(!n->conn)
    return "(not connected)";

  GInetSocketAddress *addr = G_INET_SOCKET_ADDRESS(g_socket_connection_get_remote_address(n->conn, NULL));
  g_assert(addr);
  char *ip = g_inet_address_to_string(g_inet_socket_address_get_address(addr));
  sprintf(a, "%s:%d", ip, g_inet_socket_address_get_port(addr));
  g_free(ip);
  g_object_unref(addr);
  return a;
}


struct net *net_create(char term, void *han, gboolean keepalive, void (*rfunc)(struct net *, char *), void (*errfunc)(struct net *, int, GError *)) {
  struct net *n = g_new0(struct net, 1);
  n->ref = 1;
  n->in  = g_string_sized_new(1024);
  n->out = g_string_sized_new(1024);
  n->cancel = g_cancellable_new();
  n->eom[0] = term;
  n->handle = han;
  n->keepalive = keepalive;
  n->cb_rcv = rfunc;
  n->cb_err = errfunc;
  return n;
}

#define send_do(n) do {\
    if(!n->out_src) {\
      GSource *src = g_socket_create_source(n->sock, G_IO_OUT, NULL);\
      g_source_set_callback(src, (GSourceFunc)handle_output, n, NULL);\
      n->out_src = g_source_attach(src, NULL);\
      g_source_unref(src);\
    }\
  } while (0)

void net_send_raw(struct net *n, const char *msg, int len) {
  if(!n->conn)
    return;
  g_string_append_len(n->out, msg, len);
  send_do(n);
}


void net_send(struct net *n, const char *msg) {
  g_debug("%s> %s", net_remoteaddr(n), msg);
  net_send_raw(n, msg, strlen(msg));
  net_send_raw(n, n->eom, 1);
}


void net_sendf(struct net *n, const char *fmt, ...) {
  va_list va;
  va_start(va, fmt);
  char *str = g_strdup_vprintf(fmt, va);
  va_end(va);
  net_send(n, str);
  g_free(str);
}


// TODO: error reporting?
// Note: the net_send() family shouldn't be used while the file is being sent.
void net_sendfile(struct net *n, const char *path, guint64 offset, guint64 length) {
  g_return_if_fail(!n->file_left);
  g_return_if_fail((n->file_fd = open(path, O_RDONLY)) >= 0);
  n->file_offset = offset;
  n->file_left = length;
  send_do(n);
}



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

    g_free(m->msg);
    g_object_unref(m->dest);
    g_slice_free(struct net_udp, m);
  }
  return net_udp_queue->head ? TRUE : FALSE;
}


// dest is assumed to be a valid IPv4 address with an optional port ("x.x.x.x" or "x.x.x.x:p")
void net_udp_send_raw(const char *dest, const char *msg, int len) {
  char *destc = g_strdup(dest);
  char *port_str = index(destc, ':');
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


void net_udp_sendf(const char *dest, const char *fmt, ...) {
  va_list va;
  va_start(va, fmt);
  char *str = g_strdup_vprintf(fmt, va);
  va_end(va);
  net_udp_send_raw(dest, str, strlen(str));
  g_free(str);
}

