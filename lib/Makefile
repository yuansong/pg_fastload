#
# pg_fastload: lib/Makefile
#
#    Copyright (c) 2007-2026, NTT, Inc.
#
SRCS = \
	binary.c \
	fastload_parallel.c \
	fastload_worker.c \
	logger.c \
	parser_binary.c \
	parser_csv.c \
	parser_function.c \
	parser_tuple.c \
	pg_btree.c \
	pg_fastload.c \
	pg_strutil.c \
	progress.c \
	reader.c \
	source.c \
	writer.c \
	writer_binary.c \
	writer_buffered.c \
	writer_direct.c \
	writer_parallel.c \
	pgut/pgut-be.c \
	pgut/pgut-ipc.c
OBJS = $(SRCS:.c=.o)
MODULE_big = pg_fastload

EXTENSION = pg_fastload

# set pg_fastload version
FASTLOAD_VERSION = $(shell grep "PG_FASTLOAD_VERSION" ../include/common.h | cut -d " " -f 3 | tr -d '"')

#supports EXTENSION (for >=9.1)
DATA_built = pg_fastload--$(FASTLOAD_VERSION).sql \
	pg_fastload.control

PG_CPPFLAGS = -I../include -I$(libpq_srcdir)
SHLIB_LINK = $(libpq) -lpthread

# handle to which function to be published from this shared library
# see src/Makefile.shlib in PostgreSQL
SHLIB_EXPORTS = exports.txt

ifdef USE_PROFILE
PG_CPPFLAGS += -DENABLE_FASTLOAD_PROFILE
endif

ifndef USE_PGXS
top_builddir = ../../..
makefile_global = $(top_builddir)/src/Makefile.global
ifeq "$(wildcard $(makefile_global))" ""
USE_PGXS = 1	# use pgxs if not in contrib directory
endif
endif

ifdef USE_PGXS
PG_CONFIG = pg_config
PGXS := $(shell $(PG_CONFIG) --pgxs)
include $(PGXS)
else
subdir = contrib/$(MODULE_big)
include $(makefile_global)
include $(top_srcdir)/contrib/contrib-global.mk
endif

# remove dependency to libxml2 and libxslt
LIBS := $(filter-out -lxml2, $(LIBS))
LIBS := $(filter-out -lxslt, $(LIBS))

.PHONY: subclean
clean: subclean

subclean:
	rm -f nbtsort.c

LDFLAGS+=-Wl,--build-id

pg_fastload--$(FASTLOAD_VERSION).sql: pg_fastload.sql.in
	sed 's,MODULE_PATHNAME,$$libdir/$(MODULE_big),g' $< \
	| sed 's,FASTLOAD_VERSION,$(FASTLOAD_VERSION),g' > $@

pg_fastload.control: pg_fastload.control.in
	sed 's,FASTLOAD_VERSION,$(FASTLOAD_VERSION),g' $< > $@