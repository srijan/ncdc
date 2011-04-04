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
#include <string.h>


#if INTERFACE

#define HUBS_IDLE       0
#define HUBS_CONNECTING 1
#define HUBS_CONNECTED  2

struct nmdc_hub {
  struct ui_tab *tab; // to get name (for config) and for logging & setting of title
  int state;
  GSocketConnection *conn;
  // Used for all async operations. Cancelling is only used on user-disconnect,
  // and when we disconnect we want to cancel *all* outstanding operations at
  // the same time. The documentation advices against this, but...
  //   http://www.mail-archive.com/gtk-devel-list@gnome.org/msg10628.html
  GCancellable *cancel;
};

#endif


#define hubconf_get(type, hub, key) (\
  g_key_file_has_key(conf_file, get_hub_group(hub->tab->name), (key), NULL)\
    ? g_key_file_get_##type(conf_file, get_hub_group(hub->tab->name), (key), NULL)\
    : g_key_file_get_##type(conf_file, "global", (key), NULL))


struct nmdc_hub *nmdc_create(struct ui_tab *tab) {
  struct nmdc_hub *hub = g_new0(struct nmdc_hub, 1);
  hub->tab = tab;
  hub->cancel = g_cancellable_new();
  return hub;
}


static void handle_connect(GObject *src, GAsyncResult *res, gpointer dat) {
  struct nmdc_hub *hub = dat;

  GError *err = NULL;
  GSocketConnection *conn = g_socket_client_connect_to_uri_finish((GSocketClient *)src, res, &err);
  g_object_unref(src);

  if(!conn) {
    if(err->code != G_IO_ERROR_CANCELLED) {
      ui_logwindow_printf(hub->tab->log, "Could not connect to hub: %s", err->message);
      hub->state = HUBS_IDLE;
    }
    g_error_free(err);
  } else {
    ui_logwindow_add(hub->tab->log, "Connected.");
    hub->state = HUBS_CONNECTED;
    hub->conn = conn;
  }
}


void nmdc_connect(struct nmdc_hub *hub) {
  char *addr = hubconf_get(string, hub, "hubaddr");
  g_assert(addr);

  ui_logwindow_printf(hub->tab->log, "Connecting to %s...", addr);
  hub->state = HUBS_CONNECTING;

  GSocketClient *sc = g_socket_client_new();
#if GLIB_CHECK_VERSION(2, 26, 0)
  g_socket_client_set_timeout(sc, 30);
#endif
  g_socket_client_connect_to_host_async(sc, addr, 411, hub->cancel, handle_connect, hub);

  g_free(addr);
}


void nmdc_disconnect(struct nmdc_hub *hub) {
  // cancel current operations and create a new cancellable object for later operations
  // (_reset() is not a good idea when there are still async jobs using the cancellable object)
  g_cancellable_cancel(hub->cancel);
  g_object_unref(hub->cancel);
  hub->cancel = g_cancellable_new();
  if(hub->conn)
    g_object_unref(hub->conn); // also closes the connection (does this block?)
  hub->state = HUBS_IDLE;
  ui_logwindow_printf(hub->tab->log, "Disconnected.");
}


void nmdc_free(struct nmdc_hub *hub) {
  nmdc_disconnect(hub);
  g_object_unref(hub->cancel);
  g_free(hub);
}

