# this isn't the best build system, but it works and keeps our main directory clean


# Order of ${SOURCES} is important
SOURCES=ui_c.c nmdc.ml global.ml commands.ml ui.ml main.ml
LIBS=unix.cmxa str.cmxa dbm.cmxa -cclib -lncursesw

CAMLOPT=ocamlopt.opt
CAMLDEP=ocamldep.opt


MLSRC =$(filter %.ml,$(SOURCES))
CSRC  =$(filter %.c,$(SOURCES))
MLOBJ =$(MLSRC:%.ml=%.cmx)
COBJ  =$(CSRC:%.c=%.o)
MLOBJB=$(MLOBJ:%=_build/%)
COBJB =$(COBJ:%=_build/%)

all: ncdc

ncdc: _build ${COBJB} ${MLOBJB}
	cd _build && ${CAMLOPT} ${LIBS} ${COBJ} ${MLOBJ} -o ../ncdc

# Prevent make from removing our links in _build/
.SECONDARY:

_build:
	mkdir _build
	
clean:
	rm -rf _build ncdc

_build/%.ml: %.ml
	ln $< $@

_build/%.c: %.c
	ln $< $@

_build/%.cmx: _build/%.ml
	cd _build && $(CAMLOPT) -c $*.ml

_build/%.o: _build/%.c
	cd _build && $(CAMLOPT) -c -ccopt -Wall $*.c


# Used to re-generate the .depend file. A working .depend file should be on the
# git repo, so you won't need to use this unless you make changes to the
# codebase that modify the dependencies among the source files.
depend:
	${CAMLDEP} -native ${MLSRC} | perl -e\
		'$$_=join"",grep !/cmo/,<>;print /\./?"_build/$$_":$$_ for(split /( +|\r?\n *)/)' >.depend

include .depend

