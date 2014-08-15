# Copyright (c) 2014 The Chromium OS Authors. All rights reserved.
# Use of this source code is governed by a BSD-style license that can be
# found in the LICENSE file.

APPNAME = frecon
SOURCES = $(shell echo *.c)
ROOT ?= /build/daisy
PKG_CONFIG ?= PKG_CONFIG_SYSROOT_DIR=$(ROOT) pkg-config
CC ?= armv7a-cros-linux-gnueabi-gcc --sysroot=$(ROOT)
LIB = `$(PKG_CONFIG) --libs libdrm libtsm`
FLAGS = -std=c99 -D_GNU_SOURCE=1 `$(PKG_CONFIG) --cflags libdrm libtsm`
CFLAGS += -Wall -Wsign-compare -Wpointer-arith -Wcast-qual -Wcast-align

all:	$(APPNAME)

.c.o:
	$(CC) $(CFLAGS) $(FLAGS) $(INCLUDES) -c $*.c

OBJECTS = $(SOURCES:.c=.o)

$(APPNAME):$(OBJECTS)
	$(CC) $(CFLAGS) -o $(APPNAME) $(OBJECTS) $(LIB)

depend:
	makedepend $(SOURCES)

clean:
	@rm -f $(APPNAME) $(OBJECTS)

install:
	install -m 0755 $(APPNAME) $(DESTDIR)/sbin

