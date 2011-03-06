# this isn't the best build system, but it works and keeps our main directory clean


# Order of ${SOURCES} is important; we don't have dependency tracking yet
SOURCES=ui_c.c nmdc.ml global.ml commands.ml ui.ml main.ml
LIBS=unix.cmxa str.cmxa dbm.cmxa -cclib -lncursesw

CAMLOPT=ocamlopt.opt


MLSRC_=$(filter %.ml,$(SOURCES))
CSRC_ =$(filter %.c,$(SOURCES))
MLOBJ =$(MLSRC_:%.ml=%.cmx)
COBJ  =$(CSRC_:%.c=%.o)
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

