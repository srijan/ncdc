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


#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "config.h"

#define DOC_CMD
#define DOC_SET
#define DOC_KEY
#include "../src/doc.h"


static void out_string(const char *str) {
  const char *m;
  for(m=str; *m; m++) {
    if(*m == '\n')
      fputs("\n.br\n", stdout);
    else if(*m == '\\')
      fputs("\\\\", stdout);
    else
      fputc(*m, stdout);
  }
}


static void gen_cmd() {
  const struct doc_cmd *c = doc_cmds;
  for(; *c->name; c++) {
    printf(".TP\n\\fB/%s\\fP %s\n.br\n", c->name, c->args ? c->args : "");
    out_string(c->desc ? c->desc : c->sum);
    printf("\n");
  }
}


static void gen_set() {
  const struct doc_set *s = doc_sets;
  for(; s->name; s++) {
    printf(".TP\n\\fB%s\\fP %s\n.br\n", s->name, s->type);
    out_string(s->desc);
    printf("\n");
  }
}


static void gen_key() {
  const struct doc_key *k = doc_keys;
  for(; k->sect; k++) {
    printf(".TP\n\\fB%s\\fP\n.br\n", k->title);
    out_string(k->desc);
    printf("\n");
  }
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
      } else if(strncmp(m, "@version@", 9) == 0) {
        printf("%s-%s", PACKAGE, VERSION);
        t += 9;
      } else {
        fputc('@', stdout);
        t++;
      }
    }
    fputs(t, stdout);
  }

  return 0;
}

