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


static GSocketListener *listen = NULL;     // TCP and TLS listen object. NULL if we aren't active.
static GSocket         *listen_udp = NULL; // UDP listen socket.

static guint16 listen_port = 0;   // Port used for both UDP and TCP
static GCancellable *listen_tcp_can = NULL;
static int listen_udp_src = 0;


// Public interface to fetch current listen configuration
// (TODO: These should actually use hub-specific configuration)

gboolean listen_hub_active(guint64 hub) {
  return !!listen;
}

// These all returns 0 if passive or disabled
guint32 listen_hub_ip(guint64 hub) {
  listen_hub_active(hub) ? ip4_pack(var_get(hub, VAR_active_ip)) : 0;
}

guint16 listen_hub_tcp(guint64 hub) {
  return listen_port;
}

guint16 listen_hub_tls(guint64 hub) {
  return var_get_int(hub, VAR_tls_policy) == VAR_TLSP_DISABLE ? 0 : listen_port+1;
}

guint16 listen_hub_udp(guint64 hub) {
  return listen_port;
}




static void listen_stop() {
  if(!listen)
    return;

  g_cancellable_cancel(listen_tcp_can);
  g_object_unref(listen_tcp_can);
  listen_tcp_can = NULL;
  g_socket_listener_close(listen);
  g_object_unref(listen);
  listen = NULL;

  g_source_remove(listen_udp_src);
  g_object_unref(listen_udp);
  listen_udp = NULL;
}


static void listen_tcp_handle(GObject *src, GAsyncResult *res, gpointer dat) {
  GSocketListener *tcp = dat;
  GError *err = NULL;
  GObject *istls = NULL;
  GSocketConnection *s = g_socket_listener_accept_finish(G_SOCKET_LISTENER(src), res, &istls, &err);

  if(!s) {
    if(listen && err->code != G_IO_ERROR_CANCELLED && err->code != G_IO_ERROR_CLOSED) {
      ui_mf(ui_main, 0, "Listen error: %s. Switching to passive mode.", err->message);
      listen_stop();
      hub_global_nfochange();
    }
    g_error_free(err);
  } else {
    cc_incoming(cc_create(NULL), s, istls ? TRUE : FALSE);
    g_socket_listener_accept_async(listen, listen_tcp_can, listen_tcp_handle, tcp);
    g_object_ref(tcp);
  }
  g_object_unref(tcp);
}


static void listen_udp_handle_msg(char *addr, char *msg, gboolean adc) {
  if(!msg[0])
    return;
  struct search_r *r = NULL;

  // ADC
  if(adc) {
    GError *err = NULL;
    struct adc_cmd cmd;
    adc_parse(msg, &cmd, NULL, &err);
    if(err) {
      g_warning("ADC parse error from UDP:%s: %s. --> %s", addr, err->message, msg);
      g_error_free(err);
      return;
    }
    r = search_parse_adc(NULL, &cmd);
    g_strfreev(cmd.argv);

  // NMDC
  } else
    r = search_parse_nmdc(NULL, msg);

  // Handle search result
  if(r) {
    ui_search_global_result(r);
    search_r_free(r);
  } else
    g_warning("Invalid search result from UDP:%s: %s", addr, msg);
}


static gboolean listen_udp_handle(GSocket *sock, GIOCondition cond, gpointer dat) {
  static char buf[5000]; // can be static, this function is only called in the main thread.
  GError *err = NULL;
  GSocketAddress *addr = NULL;
  int r = g_socket_receive_from(sock, &addr, buf, 5000, NULL, &err);

  // handle error
  if(r < 0) {
    if(err->code != G_IO_ERROR_WOULD_BLOCK) {
      ui_mf(ui_main, 0, "UDP read error: %s. Switching to passive mode.", err->message);
      listen_stop();
      hub_global_nfochange();
    }
    g_error_free(err);
    return FALSE;
  }

  // get source ip:port in a readable fashion, for debugging
  char addr_str[100];
  GInetSocketAddress *socka = G_INET_SOCKET_ADDRESS(addr);
  if(!socka)
    strcat(addr_str, "(addr error)");
  else {
    char *ip = g_inet_address_to_string(g_inet_socket_address_get_address(socka));
    g_snprintf(addr_str, 100, "%s:%d", ip, g_inet_socket_address_get_port(socka));
    g_free(ip);
  }
  g_object_unref(addr);

  // check for ADC or NMDC
  gboolean adc = FALSE;
  if(buf[0] == 'U')
    adc = TRUE;
  else if(buf[0] != '$') {
    g_message("CC:UDP:%s: Received invalid message: %s", addr_str, buf);
    return TRUE;
  }

  // handle message. since all we receive is either URES or $SR, we can handle that here
  char *cur = buf, *next = buf;
  while((next = strchr(cur, adc ? '\n' : '|')) != NULL) {
    *(next++) = 0;
    g_debug("UDP:%s< %s", addr_str, cur);
    listen_udp_handle_msg(addr_str, cur, adc);
    cur = next;
  }

  return TRUE;
}


static GSocket *listen_udp_create(GInetAddress *ia, int port, GError **err) {
  GError *tmperr = NULL;

  // create the socket
  GSocket *s = g_socket_new(G_SOCKET_FAMILY_IPV4, G_SOCKET_TYPE_DATAGRAM, G_SOCKET_PROTOCOL_UDP, NULL);
  g_socket_set_blocking(s, FALSE);

  // bind to the address
  GSocketAddress *saddr = G_SOCKET_ADDRESS(g_inet_socket_address_new(ia, port));
  g_socket_bind(s, saddr, TRUE, &tmperr);
  g_object_unref(saddr);
  if(tmperr) {
    g_propagate_error(err, tmperr);
    g_object_unref(s);
    return NULL;
  }

  return s;
}


static GSocketListener *listen_tcp_create(GInetAddress *ia, int *port, GError **err) {
  GSocketListener *s = g_socket_listener_new();
  GSocketAddress *newaddr = NULL;

  // TCP port
  GSocketAddress *saddr = G_SOCKET_ADDRESS(g_inet_socket_address_new(ia, *port));
  gboolean r = g_socket_listener_add_address(s, saddr, G_SOCKET_TYPE_STREAM, G_SOCKET_PROTOCOL_TCP, NULL, &newaddr, err);
  g_object_unref(saddr);
  if(!r) {
    g_object_unref(s);
    return NULL;
  }

  // Get effective port, in case our requested port was 0
  *port = g_inet_socket_address_get_port(G_INET_SOCKET_ADDRESS(newaddr));
  g_object_unref(newaddr);

  // TLS port (use a bogus GCancellable object to differenciate betwen the two)
  if(s && db_certificate) {
    saddr = G_SOCKET_ADDRESS(g_inet_socket_address_new(ia, *port+1));
    GCancellable *t = g_cancellable_new();
    r = g_socket_listener_add_address(s, saddr, G_SOCKET_TYPE_STREAM, G_SOCKET_PROTOCOL_TCP, G_OBJECT(t), NULL, err);
    g_object_unref(t);
    g_object_unref(saddr);
    if(!r) {
      g_object_unref(s);
      return NULL;
    }
  }
  return s;
}


// more like a "restart()"
gboolean listen_start() {
  GError *err = NULL;

  listen_stop();
  if(!var_get_bool(0, VAR_active)) {
    hub_global_nfochange();
    return FALSE;
  }

  // can be 0, in which case it'll be randomly assigned
  int port = var_get_int(0, VAR_active_port);

  // local addr
  char *bind = var_get(0, VAR_active_bind);
  GInetAddress *laddr = NULL;
  if(bind && *bind && !(laddr = g_inet_address_new_from_string(bind)))
    ui_m(ui_main, 0, "Error parsing `active_bind' setting, binding to all interfaces instead.");
  if(!laddr)
    laddr = g_inet_address_new_any(G_SOCKET_FAMILY_IPV4);

  // Open TCP listen socket (and determine the port if it was 0)
  GSocketListener *tcp = listen_tcp_create(laddr, &port, &err);
  if(!tcp) {
    ui_mf(ui_main, 0, "Error creating TCP listen socket: %s", err->message);
    g_object_unref(laddr);
    g_error_free(err);
    return FALSE;
  }

  // Open UDP listen socket
  GSocket *udp = listen_udp_create(laddr, port, &err);
  if(!udp) {
    ui_mf(ui_main, 0, "Error creating UDP listen socket: %s", err->message);
    g_object_unref(laddr);
    g_object_unref(tcp);
    g_error_free(err);
    return FALSE;
  }

  g_object_unref(laddr);

  // start accepting incoming TCP connections
  listen_tcp_can = g_cancellable_new();
  g_socket_listener_accept_async(tcp, listen_tcp_can, listen_tcp_handle, tcp);
  g_object_ref(tcp);

  // start receiving incoming UDP messages
  GSource *src = g_socket_create_source(udp, G_IO_IN, NULL);
  g_source_set_callback(src, (GSourceFunc)listen_udp_handle, NULL, NULL);
  listen_udp_src = g_source_attach(src, NULL);
  g_source_unref(src);

  // set global variables
  listen = tcp;
  listen_udp = udp;
  listen_port = port;

  if(db_certificate)
    ui_mf(ui_main, 0, "Listening on TCP+UDP port %d and TCP port %d, remote IP is %s.", listen_port, listen_port+1, var_get(0, VAR_active_ip));
  else
    ui_mf(ui_main, 0, "Listening on TCP+UDP port %d, remote IP is %s.", listen_port, var_get(0, VAR_active_ip));
  hub_global_nfochange();
  return TRUE;
}

