CC=cc


ifeq ($(OS),Windows_NT)
    CCFLAGS += -D WIN32
else
    UNAME_S := $(shell uname -s)
    ifeq ($(UNAME_S),Linux)
	FUSE_FLAGS=$(shell pkg-config fuse --cflags)
	LDFLAGS=-lssl -lpthread -lcrypto -lfuse
    endif
    ifeq ($(UNAME_S),Darwin)
	FUSE_FLAGS=-D_FILE_OFFSET_BITS=64 -DFUSE_USE_VERSION=26 -I/usr/local/include/osxfuse/fuse
        LDFLAGS=-losxfuse -lssl -lpthread -lcrypto
    endif
endif

CFLAGS=-Wall -g -O2 $(FUSE_FLAGS) -Iinclude -D_GNU_SOURCE -DDEBUG


all: mount.pfs

mount.pfs: settings.o pfs.o lib/binapi.o
	$(CC) settings.o pfs.o lib/binapi.o -o mount.pfs $(LDFLAGS)

install: mount.pfs
	install -D mount.pfs $(DESTDIR)/usr/bin/mount.pfs

clean:
	rm -f *~ *.o lib/*o mount.pfs

