# Redis Makefile
# Copyright (C) 2009 Salvatore Sanfilippo <antirez at gmail dot com>
# This file is released under the BSD license, see the COPYING file

uname_S := $(shell sh -c 'uname -s 2>/dev/null || echo not')
#OPTIMIZATION?=-O2

CCCOLOR="\033[34m"
LINKCOLOR="\033[34;1m"
SRCCOLOR="\033[33m"
BINCOLOR="\033[37;1m"
MAKECOLOR="\033[32;1m"
ENDCOLOR="\033[0m"

USE_TCMALLOC=yes

ifndef V
QUIET_CC = @printf '    %b %b\n' $(CCCOLOR)CC$(ENDCOLOR) $(SRCCOLOR)$@$(ENDCOLOR);
QUIET_CPP = @printf '    %b %b\n' $(CCCOLOR)CPP$(ENDCOLOR) $(SRCCOLOR)$@$(ENDCOLOR);
QUIET_LINK = @printf '    %b %b\n' $(LINKCOLOR)LINK$(ENDCOLOR) $(BINCOLOR)$@$(ENDCOLOR);
QUIET_AR = @printf '    %b %b\n' $(LINKCOLOR)AR$(ENDCOLOR) $(BINCOLOR)$@$(ENDCOLOR);
endif

ifeq ($(uname_S),SunOS)
  CFLAGS?= -fPIC -std=c99 -pedantic $(OPTIMIZATION) -Wall -W -D__EXTENSIONS__ -D_XPG6 -DTAIR_STORAGE
  CPPFLAGS?= $(OPTIMIZATION) -g -Wall -W $(ARCH) $(PROF) -DTAIR_STORAGE
  CCLINK?= -ldl -lnsl -lsocket -lm -lpthread
  DEBUG?= -g -ggdb 
else
  CFLAGS?= -fPIC -std=c99 -g -pedantic $(OPTIMIZATION) -Wall -W $(ARCH) $(PROF) -DTAIR_STORAGE
  CPPFLAGS?= $(OPTIMIZATION) -g -Wall -W $(ARCH) $(PROF) -DTAIR_STORAGE
  CCLINK?= -lm -pthread -lstdc++
  DEBUG?= -g -rdynamic -ggdb 
endif

CCOPT= $(CFLAGS) $(CCLINK) $(ARCH) $(PROF)

PREFIX= /usr/local

OBJ = adlist.o dict.o redis.o sds.o zmalloc.o lzf_c.o lzf_d.o pqsort.o zipmap.o ziplist.o networking.o util.o object.o db.o t_string.o t_list.o t_set.o t_zset.o t_hash.o sort.o intset.o value_item_list.o 

all: libredis.a
	@echo "Redis static library build done"

DISTFILES=adlist.c adlist.h command.h config.h db.c dict.c dict.h fmacros.h intset.c intset.h libredis.a lzf_c.c lzf_d.c lzf.h lzfP.h Makefile networking.c object.c pqsort.c pqsort.h redis.c redis.h redislib.h sds.c sds.h sort.c t_hash.c t_list.c t_set.c t_string.c t_zset.c util.c valgrind.sup value_item_list.c ziplist.c ziplist.h zipmap.c zipmap.h zmalloc.c zmalloc.h Makefile

# Deps (use make dep to generate this)
#redis-lib-test.o: redis-lib-test.cpp redis.h
adlist.o: adlist.c adlist.h zmalloc.h
db.o: db.c redis.h fmacros.h sds.h dict.h adlist.h \
  zmalloc.h zipmap.h ziplist.h intset.h
dict.o: dict.c fmacros.h dict.h zmalloc.h
intset.o: intset.c intset.h zmalloc.h
lzf_c.o: lzf_c.c lzfP.h
lzf_d.o: lzf_d.c lzfP.h
networking.o: networking.c redis.h fmacros.h sds.h dict.h \
  adlist.h zmalloc.h zipmap.h ziplist.h intset.h
object.o: object.c redis.h fmacros.h sds.h dict.h adlist.h \
  zmalloc.h zipmap.h ziplist.h intset.h
pqsort.o: pqsort.c
redis.o: redis.c redis.h fmacros.h sds.h dict.h adlist.h \
  zmalloc.h zipmap.h ziplist.h intset.h
sds.o: sds.c sds.h zmalloc.h
sort.o: sort.c redis.h fmacros.h sds.h dict.h adlist.h \
  zmalloc.h zipmap.h ziplist.h intset.h pqsort.h
value_item_list.o: value_item_list.c redis.h
t_hash.o: t_hash.c redis.h fmacros.h sds.h dict.h adlist.h \
  zmalloc.h zipmap.h ziplist.h intset.h
t_list.o: t_list.c redis.h fmacros.h sds.h dict.h adlist.h \
  zmalloc.h zipmap.h ziplist.h intset.h
t_set.o: t_set.c redis.h fmacros.h sds.h dict.h adlist.h \
  zmalloc.h zipmap.h ziplist.h intset.h
t_string.o: t_string.c redis.h fmacros.h sds.h dict.h \
  adlist.h zmalloc.h zipmap.h ziplist.h intset.h
t_zset.o: t_zset.c redis.h fmacros.h sds.h dict.h adlist.h \
  zmalloc.h zipmap.h ziplist.h intset.h
util.o: util.c redis.h fmacros.h sds.h dict.h adlist.h \
  zmalloc.h zipmap.h ziplist.h intset.h
ziplist.o: ziplist.c zmalloc.h ziplist.h
zipmap.o: zipmap.c zmalloc.h
zmalloc.o: zmalloc.c zmalloc.h

dependencies:
	@printf '%b %b\n' $(MAKECOLOR)MAKE$(ENDCOLOR) $(BINCOLOR)hiredis$(ENDCOLOR)
	cd ../deps/hiredis && $(MAKE) static ARCH="$(ARCH)"
	@printf '%b %b\n' $(MAKECOLOR)MAKE$(ENDCOLOR) $(BINCOLOR)linenoise$(ENDCOLOR)
	cd ../deps/linenoise && $(MAKE) ARCH="$(ARCH)"

libredis.a: $(OBJ)
	$(QUIET_AR)ar cr $@ $(OBJ) 

#redis-lib-test: redis-lib-test.o libredis.a 
#	$(QUIET_LINK)$(CC) -o $(PRGNAME) $(CCOPT) $(DEBUG) $^ -ltcmalloc

.c.o:
	$(QUIET_CC)$(CC) -c $(CFLAGS) $(DEBUG) $(COMPILE_TIME) $<

.cpp.o:
	$(QUIET_CPP)g++ -c $(CPPFLAGS) $(DEBUG) $(COMPILE_TIME) $<

clean:
	rm -rf $(PRGNAME) $(BENCHPRGNAME) $(CLIPRGNAME) $(CHECKDUMPPRGNAME) $(CHECKAOFPRGNAME) *.o *.gcda *.gcno *.gcov

dep:
	$(CC) -MM *.c -I ../deps/hiredis -I ../deps/linenoise

log:
	git log '--pretty=format:%ad %s (%cn)' --date=short > ../Changelog

32bit:
	@echo ""
	@echo "WARNING: if it fails under Linux you probably need to install libc6-dev-i386"
	@echo ""
	$(MAKE) ARCH="-m32"

gprof:
	$(MAKE) PROF="-pg"

gcov:
	$(MAKE) PROF="-fprofile-arcs -ftest-coverage"

noopt:
	$(MAKE) OPTIMIZATION=""

32bitgprof:
	$(MAKE) PROF="-pg" ARCH="-arch i386"
install:
distdir: $(DISTFILES)
	@srcdirstrip=`echo "$(srcdir)" | sed 's|.|.|g'`; \
	topsrcdirstrip=`echo "$(top_srcdir)" | sed 's|.|.|g'`; \
	list='$(DISTFILES)'; for file in $$list; do \
	  case $$file in \
	    $(srcdir)/*) file=`echo "$$file" | sed "s|^$$srcdirstrip/||"`;; \
	    $(top_srcdir)/*) file=`echo "$$file" | sed "s|^$$topsrcdirstrip/|$(top_builddir)/|"`;; \
	  esac; \
	  if test -f $$file || test -d $$file; then d=.; else d=$(srcdir); fi; \
	  dir=`echo "$$file" | sed -e 's,/[^/]*$$,,'`; \
	  if test "$$dir" != "$$file" && test "$$dir" != "."; then \
	    dir="/$$dir"; \
	    $(mkdir_p) "$(distdir)$$dir"; \
	  else \
	    dir=''; \
	  fi; \
	  if test -d $$d/$$file; then \
	    if test -d $(srcdir)/$$file && test $$d != $(srcdir); then \
	      cp -pR $(srcdir)/$$file $(distdir)$$dir || exit 1; \
	    fi; \
	    cp -pR $$d/$$file $(distdir)$$dir || exit 1; \
	  else \
	    test -f $(distdir)/$$file \
	    || cp -p $$d/$$file $(distdir)/$$file \
	    || exit 1; \
	  fi; \
	done
	list='$(DIST_SUBDIRS)'; for subdir in $$list; do \
	  if test "$$subdir" = .; then :; else \
	    test -d "$(distdir)/$$subdir" \
	    || $(mkdir_p) "$(distdir)/$$subdir" \
	    || exit 1; \
	    distdir=`$(am__cd) $(distdir) && pwd`; \
	    top_distdir=`$(am__cd) $(top_distdir) && pwd`; \
	    (cd $$subdir && \
	      $(MAKE) $(AM_MAKEFLAGS) \
	        top_distdir="$$top_distdir" \
	        distdir="$$distdir/$$subdir" \
	        distdir) \
	      || exit 1; \
	  fi; \
	done
