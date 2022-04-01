# GNU Makefile for libfaun

#STATIC_LIB=true

CFLAGS=-O3 -DNDEBUG
#CFLAGS=-g -DDEBUG

OPT=-DCAPTURE -DUSE_SFX_GEN -DUSE_FLAC

DESTDIR ?= /usr/local


ifneq (,$(wildcard /usr/lib64/libc.so))
LIB_DIR=$(DESTDIR)/lib64
else ifneq (,$(wildcard /usr/lib/x86_64-linux-gnu/.))
LIB_DIR=$(DESTDIR)/lib/x86_64-linux-gnu
else
LIB_DIR=$(DESTDIR)/lib
endif

DEP_LIB = -lpulse-simple -lpulse -lvorbis -lvorbisfile -lpthread -lm

ifdef STATIC_LIB
FAUN_LIB=libfaun.a
DEP_STATIC=$(DEP_LIB)
else
FAUN_LIB=libfaun.so.0.1.0
FAUN_SO=libfaun.so.0
DEP_STATIC=
endif

.PHONY: all install clean

all: $(FAUN_LIB) faun_test basic

obj:
	mkdir obj

obj/tmsg.o: support/tmsg.c obj
	cc -c -pipe -Wall -W $< $(CFLAGS) -Isupport $(OPT) -fPIC -o $@

obj/faun.o: faun.c support/wav_write.c support/wav_read.c support/flac.c support/sfx_gen.c support/well512.c support/os_thread.h support/tmsg.h support/flac.h support/sfx_gen.h support/well512.h obj
	cc -c -pipe -Wall -W $< $(CFLAGS) -Isupport $(OPT) -fPIC -o $@

$(FAUN_LIB): obj/tmsg.o obj/faun.o
ifdef STATIC_LIB
	ar rc $@ $^
	ranlib $@
	#strip -d $@
else
	cc -o $@ $^ -shared -Wl,-soname,$(FAUN_SO) $(DEP_LIB)
	ln -sf $@ $(FAUN_SO)
	ln -sf $@ libfaun.so
endif

faun_test: faun_test.c $(FAUN_LIB)
	cc -Wall -W $< $(CFLAGS) -I. -L. -lfaun $(DEP_STATIC) -o $@

basic: example/basic.c $(FAUN_LIB)
	cc -Wall -W $< $(CFLAGS) -I. -L. -lfaun $(DEP_STATIC) -o $@

install:
	mkdir -p $(DESTDIR)/include $(LIB_DIR)
	install -m 644 faun.h $(DESTDIR)/include
ifdef STATIC_LIB
	install -m 644 $(FAUN_LIB) $(LIB_DIR)
else
	install -m 755 -s $(FAUN_LIB) $(LIB_DIR)
	ln -s $(FAUN_LIB) $(LIB_DIR)/$(FAUN_SO)
	ln -s $(FAUN_LIB) $(LIB_DIR)/libfaun.so
endif

clean:
	@rm -rf obj libfaun.* faun_test basic
