# this isn't really a good build system...

# ocamlbuild is faster... but since the project is still quite small it's
# easier to just do everything in a single ocamlopt run

# order matters
SOURCES=ui_c.c global.ml ui.ml main.ml

CLEAN=*.o *.cmi *.cmx

all:
	ocamlopt -cclib -lncursesw -ccopt -Wall unix.cmxa str.cmxa ${SOURCES} -o main
	rm -f ${CLEAN}

clean:
	rm -f ${CLEAN}

