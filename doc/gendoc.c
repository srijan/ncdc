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


#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "config.h"

#define DOC_CMD
#define DOC_SET
#define DOC_KEY
#include "../src/doc.h"


static void gen_cmd() {
  const struct doc_cmd *c = doc_cmds;
  printf("=over\n\n");
  for(; *c->name; c++) {
    printf("=item B</%s>", c->name);
    if(c->args)
      printf(" %s", c->args);
    fputs("\n\n", stdout);
    fputs(c->desc ? c->desc : c->sum, stdout);
    fputs("\n\n", stdout);
  }
  printf("=back\n\n");
}


static void gen_set() {
  const struct doc_set *s = doc_sets;
  printf("=over\n\n");
  for(; s->name; s++) {
    printf("=item B<%s> %s\n\n", s->name, s->type);
    fputs(s->desc, stdout);
    fputs("\n\n", stdout);
  }
  printf("=back\n\n");
}


static void gen_key() {
  const struct doc_key *k = doc_keys;
  printf("=over\n\n");
  for(; k->sect; k++) {
    printf("=item B<%s>\n\n  ", k->title);
    // TODO: It would be nicer to have this in POD rather than as verbatim paragraphs
    const char *m = k->desc;
    for(; *m; m++) {
      if(*m == '\n')
        fputs("\n  ", stdout);
      else
        fputc(*m, stdout);
    }
    fputs("\n\n", stdout);
  }
  printf("=back\n\n");
}


int main(int argc, char **argv) {
  if(argc != 1) {
    fprintf(stderr, "This command does not accept any commandline arguments.");
    return 1;
  }
  char line[4096];
  while(fgets(line, sizeof(line), stdin) != NULL) {
    char *t = line;
    char *m;
    while((m = strchr(t, '@')) != NULL) {
      fwrite(t, 1, m-t, stdout);
      t = m;
      if(strncmp(m, "@commands@", 10) == 0) {
        gen_cmd();
        t += 10;
      } else if(strncmp(m, "@settings@", 10) == 0) {
        gen_set();
        t += 10;
      } else if(strncmp(m, "@keys@", 6) == 0) {
        gen_key();
        t += 6;
      } else {
        fputc('@', stdout);
        t++;
      }
    }
    fputs(t, stdout);
  }

  return 0;
}

