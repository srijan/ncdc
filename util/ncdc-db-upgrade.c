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

#include <locale.h>
#include <unistd.h>
#include <fcntl.h>

#include <glib.h>
#include <glib/gstdio.h>

static const char *db_dir = NULL;


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
  int ver_fd = g_open(ver_file, O_RDWR, 0600);

  struct flock lck;
  lck.l_type = F_WRLCK;
  lck.l_whence = SEEK_SET;
  lck.l_start = 0;
  lck.l_len = 0;
  if(ver_fd < 0 || fcntl(ver_fd, F_SETLK, &lck) == -1) {
    fprintf(stderr, "Unable to open lock file. Please make sure that no other instance of ncdc is running with the same configuration directory.\n");
    exit(1);
  }

  // get version
  unsigned char dir_ver[2];
  if(read(ver_fd, dir_ver, 2) < 2)
    g_error("Could not read version information from '%s': %s", ver_file, g_strerror(errno));
  g_free(ver_file);
  return (dir_ver[0] << 8) + dir_ver[1];
}



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

  return 0;
}

