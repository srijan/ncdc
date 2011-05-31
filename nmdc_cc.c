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

struct nmdc_cc {
  struct net *net;
  struct nmdc_hub *hub;
  char *nick_raw; // hub encoding
  char *nick;     // UTF-8
};

#endif

// TODO: keep a list/hashtable/sequence of nmdc_cc objects



static void handle_error(struct net *n, int action, GError *err) {
  // TODO: report error somewhere?
  nmdc_cc_disconnect(n->handle);
}


static void handle_cmd(struct net *n, char *cmd) {
  struct nmdc_cc *cc = n->handle;
  GMatchInfo *nfo;

  // create regexes (declared statically, allocated/compiled on first call)
#define CMDREGEX(name, regex) \
  static GRegex * name = NULL;\
  if(!name) name = g_regex_new("\\$" regex, G_REGEX_OPTIMIZE|G_REGEX_ANCHORED|G_REGEX_DOTALL|G_REGEX_RAW, 0, NULL)

  CMDREGEX(mynick, "MyNick ([^ $]+)");
  CMDREGEX(lock, "Lock ([^ $]+) Pk=[^ $]+");

  // $MyNick
  if(g_regex_match(mynick, cmd, 0, &nfo)) { // 1 = nick
    cc->nick_raw = g_match_info_fetch(nfo, 1);
    // TODO: if !hub, figure out on which hub this user belongs
    cc->nick = nmdc_charset_convert(cc->hub, TRUE, cc->nick_raw);
  }
  g_match_info_free(nfo);

  // $Lock
  if(g_regex_match(lock, cmd, 0, &nfo)) { // 1 = lock
    char *lock = g_match_info_fetch(nfo, 1);
    // we don't implement the classic NMDC get, so we can't talk with non-EXTENDEDPROTOCOL clients
    if(strncmp(lock, "EXTENDEDPROTOCOL", 16) != 0) {
      g_warning("C-C connection with %s (%s), but it does not support EXTENDEDPROTOCOL.", net_remoteaddr(cc->net), cc->nick);
      nmdc_cc_disconnect(cc);
    } else {
      net_send(cc->net, "$Supports "); // TODO: actually support something
      char *key = nmdc_lock2key(lock);
      net_sendf(cc->net, "$Key %s", key);
      g_free(key);
      g_free(lock);
    }
  }
  g_match_info_free(nfo);
}


// Hub may be unknown when we start listening on incoming connections.
struct nmdc_cc *nmdc_cc_create(struct nmdc_hub *hub) {
  struct nmdc_cc *cc = g_new0(struct nmdc_cc, 1);
  cc->net = net_create('|', cc, handle_cmd, handle_error);
  cc->hub = hub;
  return cc;
}


static void handle_connect(struct net *n) {
  struct nmdc_cc *cc = n->handle;
  g_assert(cc->hub);
  net_sendf(n, "$MyNick %s", cc->hub->nick_hub);
  net_sendf(n, "$Lock EXTENDEDPROTOCOL/wut? Pk=%s-%s", PACKAGE_NAME, PACKAGE_VERSION);
}


void nmdc_cc_connect(struct nmdc_cc *cc, const char *addr) {
  net_connect(cc->net, addr, 0, handle_connect);
}


// this is a disconnect-and-free
void nmdc_cc_disconnect(struct nmdc_cc *cc) {
  if(cc->net->conn)
    net_disconnect(cc->net);
  net_free(cc->net);
  g_free(cc->nick_raw);
  g_free(cc->nick);
  g_free(cc);
}

