# GNU Makefile for libfaun

-include make.config

ifeq ($(STATIC),1)
    STATIC_LIB=true
endif

CFLAGS+=-O3 -DNDEBUG
#CFLAGS+=-g -DDEBUG

OPT=-DUSE_LOAD_MEM -DUSE_SFX_GEN

DESTDIR ?= /usr/local


ifneq (,$(wildcard /usr/lib64/libc.so))
LIB_DIR=$(DESTDIR)/lib64
else ifneq (,$(wildcard /usr/lib/x86_64-linux-gnu/.))
LIB_DIR=$(DESTDIR)/lib/x86_64-linux-gnu
else
LIB_DIR=$(DESTDIR)/lib
endif

DEP_LIB = -lpulse -lvorbisfile -lpthread -lm

ifeq ($(FLAC),1)
OPT+=-DUSE_FLAC=1
DEP_LIB+=-lFLAC
else ifeq ($(FLAC),2)
OPT+=-DUSE_FLAC=2
endif

ifdef STATIC_LIB
FAUN_LIB=libfaun.a
DEP_STATIC=$(DEP_LIB)
else
FAUN_LIB=libfaun.so.0.2.0
FAUN_SO=libfaun.so.0
DEP_STATIC=
endif

.PHONY: all install clean sdks

ifeq ($(FTEST),1)
OPT+=-DCAPTURE
all: $(FAUN_LIB) basic faun_test
else
all: $(FAUN_LIB) basic
endif

obj:
	mkdir obj

obj/tmsg.o: support/tmsg.c obj
	$(CC) -c -pipe -Wall -W $< $(CFLAGS) -Isupport $(OPT) -fPIC -o $@

obj/faun.o: faun.c support/wav_write.c support/wav_read.c support/flac.c support/sfx_gen.c support/well512.c support/os_thread.h support/tmsg.h support/flac.h support/sfx_gen.h support/well512.h obj
	$(CC) -c -pipe -Wall -W $< $(CFLAGS) -Isupport $(OPT) -fPIC -o $@

$(FAUN_LIB): obj/tmsg.o obj/faun.o
ifdef STATIC_LIB
	ar rc $@ $^
	ranlib $@
	#strip -d $@
else
	$(CC) -o $@ $^ -shared -Wl,-soname,$(FAUN_SO) $(LDFLAGS) $(DEP_LIB)
	ln -sf $@ $(FAUN_SO)
	ln -sf $@ libfaun.so
endif

faun_test: faun_test.c $(FAUN_LIB)
	$(CC) -Wall -W $< $(CFLAGS) -I. -L. -lfaun $(DEP_STATIC) $(LDFLAGS) -o $@

basic: example/basic.c $(FAUN_LIB)
	$(CC) -Wall -W $< $(CFLAGS) -I. -L. -lfaun $(DEP_STATIC) $(LDFLAGS) -o $@

install:
	mkdir -p $(DESTDIR)/include $(LIB_DIR)
	install -m 644 faun.h $(DESTDIR)/include
ifdef STATIC_LIB
	install -m 644 $(FAUN_LIB) $(LIB_DIR)
else
	install -m 755 $(FAUN_LIB) $(LIB_DIR)
	ln -s $(FAUN_LIB) $(LIB_DIR)/$(FAUN_SO)
	ln -s $(FAUN_LIB) $(LIB_DIR)/libfaun.so
endif

clean:
	@rm -rf obj libfaun.* faun_test basic

sdks:
	@rm -f project.tar.gz
	copr -c
	dist/cbuild windows
	dist/cbuild linux
