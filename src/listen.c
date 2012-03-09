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


#if INTERFACE

#define LBT_TLS 0
#define LBT_UDP 1
#define LBT_TCP 2

#define LBT_STR(x) ((x) == LBT_TLS ? "TLS" : (x) == LBT_UDP ? "UDP" : "TCP")

// port + ip4 are "cached" for convenience.
struct listen_bind {
  guint16 type; // LBT_*
  guint16 port;
  guint32 ip4;
  int src; // glib event source
  GSocket *sock;
  GSList *hubs; // hubs that use this bind
};


struct listen_hub_bind {
  guint64 hubid;
  struct listen_bind *tcp, *udp, *tls;
  guint32 ip4; // public IP (stored here to allow it to be different from the active_ip setting)
};

#endif


GList      *listen_binds     = NULL; // List of currently used binds
GHashTable *listen_hub_binds = NULL; // Map of &hubid to listen_hub_bind

// The port to use when active_port hasn't been set. Initialized to a random
// value on startup. Note that the same port is still used for all hubs that
// have the port set to "random". This obviously isn't very "random", but
// avoids a change to the port every time that listen_refresh() is called.
// Also note that this isn't necessarily a "random *free* port" such as what
// the OS would give if you actually specify port=0, so if a port happens to be
// chosen that is already in use, you'll get an error. (This isn't very nice...
// Especially if the port is being used for e.g. an outgoing connection, which
// isn't very uncommon for high ports).
static guint16 random_tcp_port, random_udp_port, random_tls_port;


// Public interface to fetch current listen configuration

gboolean listen_hub_active(guint64 hub) {
  struct listen_hub_bind *b = g_hash_table_lookup(listen_hub_binds, &hub);
  return b && b->tcp;
}

// These all returns 0 if passive or disabled
guint32 listen_hub_ip(guint64 hub) {
  struct listen_hub_bind *b = g_hash_table_lookup(listen_hub_binds, &hub);
  return b && b->tcp ? b->ip4 : 0;
}

guint16 listen_hub_tcp(guint64 hub) {
  struct listen_hub_bind *b = g_hash_table_lookup(listen_hub_binds, &hub);
  return b && b->tcp ? b->tcp->port : 0;
}

guint16 listen_hub_tls(guint64 hub) {
  struct listen_hub_bind *b = g_hash_table_lookup(listen_hub_binds, &hub);
  return b && b->tls ? b->tls->port : 0;
}

guint16 listen_hub_udp(guint64 hub) {
  struct listen_hub_bind *b = g_hash_table_lookup(listen_hub_binds, &hub);
  return b && b->udp ? b->udp->port : 0;
}



void listen_global_init() {
  listen_hub_binds = g_hash_table_new_full(g_int64_hash, g_int64_equal, NULL, g_free);
  random_tcp_port = g_random_int_range(1025, 65535);
  do
    random_tls_port = g_random_int_range(1025, 65535);
  while(random_tls_port == random_tcp_port);
  random_udp_port = g_random_int_range(1025, 65535);
}


// Closes all listen sockets and clears *listen_binds and *listen_hub_binds.
static void listen_stop() {
  g_debug("listen: Stopping.");
  g_hash_table_remove_all(listen_hub_binds);
  GList *n, *b = listen_binds;
  while(b) {
    n = b->next;
    struct listen_bind *lb = b->data;
    if(lb->src)
      g_source_remove(lb->src);
    if(lb->sock) {
      g_socket_close(lb->sock, NULL);
      g_object_unref(lb->sock);
    }
    g_slist_free(lb->hubs);
    g_free(lb);
    g_list_free_1(b);
    b = n;
  }
  listen_binds = NULL;
}


static gboolean listen_tcp_handle(GSocket *sock, GIOCondition cond, gpointer dat) {
  struct listen_bind *b = dat;
  GError *err = NULL;
  GSocket *c = g_socket_accept(sock, NULL, &err);

  // handle error
  if(!c) {
    if(err->code == G_IO_ERROR_WOULD_BLOCK) {
      g_error_free(err);
      return TRUE;
    }
    ui_mf(ui_main, 0, "TCP accept error on %s:%d: %s. Switching to passive mode.",
      ip4_unpack(b->ip4), b->port, err->message);
    listen_stop();
    hub_global_nfochange();
    g_error_free(err);
    return FALSE;
  }

  // Create connection
  cc_incoming(cc_create(NULL), b->port, g_socket_connection_factory_create_connection(c), b->type == LBT_TLS);
  g_object_unref(c);
  return TRUE;
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
  struct listen_bind *b = dat;
  GError *err = NULL;
  GSocketAddress *addr = NULL;
  int r = g_socket_receive_from(sock, &addr, buf, 5000, NULL, &err);

  // handle error
  if(r < 0) {
    if(err->code == G_IO_ERROR_WOULD_BLOCK) {
      g_error_free(err);
      return TRUE;
    }
    ui_mf(ui_main, 0, "UDP read error on %s:%d: %s. Switching to passive mode.",
      ip4_unpack(b->ip4), b->port, err->message);
    listen_stop();
    hub_global_nfochange();
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


#define bind_hub_add(lb, h) do {\
    if((lb)->type == LBT_TCP)\
      (h)->tcp = lb;\
    else if((lb)->type == LBT_UDP)\
      (h)->udp = lb;\
    else\
      (h)->tls = lb;\
    (lb)->hubs = g_slist_prepend((lb)->hubs, h);\
  } while(0)


static void bind_add(struct listen_hub_bind *b, int type, guint32 ip, guint16 port) {
  if(!port)
    port = type == LBT_TCP ? random_tcp_port : type == LBT_UDP ? random_udp_port : random_tls_port;
  g_debug("Listen: Adding %s %s:%d", LBT_STR(type), ip4_unpack(ip), port);
  // First: look if we can re-use an existing bind and look for any unresolvable conflicts.
  GList *c;
  for(c=listen_binds; c; c=c->next) {
    struct listen_bind *i = c->data;
    // Same? Just re-use.
    if(i->type == type && (i->ip4 == ip || !i->ip4) && i->port == port) {
      g_debug("Listen: Re-using!");
      bind_hub_add(i, b);
      return;
    }
    // Clashing IP/port but different type? conflict!
    if(((type == LBT_TLS && i->type == LBT_TCP) || (type == LBT_TCP && i->type == LBT_TLS))
        && i->port == port && (!i->ip4 || !ip || i->ip4 == ip)) {
      char tmp[20];
      strcpy(tmp, ip4_unpack(ip));
      ui_mf(ui_main, UIP_MED, "Active configuration error: %s %s:%d conflicts with %s %s:%d. Switching to passive mode.",
        LBT_STR(type), tmp, port, LBT_STR(i->type), ip4_unpack(i->ip4), i->port);
      listen_stop();
      return;
    }
  }

  // Create and add bind item
  struct listen_bind *lb = g_new0(struct listen_bind, 1);
  lb->type = type;
  lb->ip4 = ip;
  lb->port = port;
  bind_hub_add(lb, b);

  // Look for existing binds that should be merged.
  GList *n;
  for(c=listen_binds; !lb->ip4&&c; c=n) {
    n = c->next;
    struct listen_bind *i = c->data;
    if(i->port != lb->port || i->type != lb->type)
      continue;
    g_debug("Listen: Merging!");
    // Move over all hubs to *lb
    GSList *in;
    for(in=i->hubs; in; in=in->next)
      bind_hub_add(lb, (struct listen_hub_bind *)in->data);
    g_slist_free(i->hubs);
    // And remove this bind
    g_free(i);
    listen_binds = g_list_delete_link(listen_binds, c);
  }

  listen_binds = g_list_prepend(listen_binds, lb);
}


static void bind_create(struct listen_bind *b) {
  g_debug("Listen: binding %s %s:%d", LBT_STR(b->type), ip4_unpack(b->ip4), b->port);
  GError *err = NULL;

  // Get local address object
  GInetAddress *laddr = b->ip4
    ? g_inet_address_new_from_string(ip4_unpack(b->ip4))
    : g_inet_address_new_any(G_SOCKET_FAMILY_IPV4);
  g_return_if_fail(laddr != NULL);

  // create the socket
  GSocket *s = b->type == LBT_UDP
    ? g_socket_new(G_SOCKET_FAMILY_IPV4, G_SOCKET_TYPE_DATAGRAM, G_SOCKET_PROTOCOL_UDP, NULL)
    : g_socket_new(G_SOCKET_FAMILY_IPV4, G_SOCKET_TYPE_STREAM, G_SOCKET_PROTOCOL_TCP, NULL);
  g_socket_set_blocking(s, FALSE);

  // bind
  GSocketAddress *saddr = G_SOCKET_ADDRESS(g_inet_socket_address_new(laddr, b->port));
  g_socket_bind(s, saddr, TRUE, &err);
  g_object_unref(laddr);
  g_object_unref(saddr);

  // listen
  if(!err && b->type != LBT_UDP)
    g_socket_listen(s, &err);

  // Bind failed? Abandon ship! (This may be a bit extreme, but at least it
  // avoids any other problems that may arise from a partially activated
  // configuration).
  if(err) {
    ui_mf(ui_main, UIP_MED, "Error binding to %s %s:%d, %s. Switching to passive mode.",
      b->type == LBT_UDP ? "UDP" : "TCP", ip4_unpack(b->ip4), b->port, err->message);
    g_error_free(err);
    g_object_unref(s);
    listen_stop();
    return;
  }
  b->sock = s;

  // Start accepting incoming connections or handling incoming messages
  GSource *src = g_socket_create_source(s, G_IO_IN, NULL);
  g_source_set_callback(src, (GSourceFunc)(b->type == LBT_UDP ? listen_udp_handle : listen_tcp_handle), b, NULL);
  b->src = g_source_attach(src, NULL);
  g_source_unref(src);
}


// Should be called every time a hub is opened/closed or an active_ config
// variable has changed.
void listen_refresh() {
  listen_stop();
  g_debug("listen: Refreshing");

  // Walk through ui_tabs to get a list of hubs and their config
  GList *l;
  for(l=ui_tabs; l; l=l->next) {
    struct ui_tab *t = l->data;
    // We only look at hubs on which we are active
    if(t->type != UIT_HUB || !var_get_bool(t->hub->id, VAR_active))
      continue;
    // Add to listen_hub_binds
    struct listen_hub_bind *b = g_new0(struct listen_hub_bind, 1);
    b->hubid = t->hub->id;
    b->ip4 = ip4_pack(var_get(b->hubid, VAR_active_ip));
    g_hash_table_insert(listen_hub_binds, &b->hubid, b);
    // And add the required binds for this hub (Due to the conflict resolution in binds_add(), this is O(n^2))
    // Note: bind_add() can call listen_stop() on error, detect this on whether listen_hub_binds is empty or not.
    guint32 localip = ip4_pack(var_get(b->hubid, VAR_local_address));
    bind_add(b, LBT_TCP, localip, var_get_int(b->hubid, VAR_active_port));
    if(!g_hash_table_size(listen_hub_binds))
      break;
    bind_add(b, LBT_UDP, localip, var_get_int(b->hubid, VAR_active_udp_port));
    if(!g_hash_table_size(listen_hub_binds))
      break;
    if(var_get_int(b->hubid, VAR_tls_policy) > VAR_TLSP_DISABLE) {
      bind_add(b, LBT_TLS, localip, var_get_int(b->hubid, VAR_active_tls_port));
      if(!g_hash_table_size(listen_hub_binds))
        break;
    }
  }

  // Now walk through *listen_binds and actually create the listen sockets
  for(l=listen_binds; l; listen_binds ? (l=l->next) : (l=NULL))
    bind_create((struct listen_bind *)l->data);
}

