
bin_PROGRAMS = ncdc
ncdc_SOURCES = main.c filelist.c commands.c util.c ui.c ui_util.c nmdc.c
noinst_HEADERS = ncdc.h
ncdc_CFLAGS = $(DEPS_CFLAGS)
ncdc_LDFLAGS = $(DEPS_LIBS)
auto_headers = $(ncdc_SOURCES:.c=.h)
#BUILT_SOURCES = $(auto_headers)
MOSTLYCLEANFILES = $(auto_headers)

%.h: %.c
	makeheaders $<

${ncdc_OBJECTS}: ${auto_headers}

