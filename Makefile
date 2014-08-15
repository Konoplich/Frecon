# Copyright (c) 2014 The Chromium OS Authors. All rights reserved.
# Use of this source code is governed by a BSD-style license that can be
# found in the LICENSE file.

include common.mk

SOURCES = $(shell echo *.c)
OBJS = $(SOURCES:.c=.o)
PKG_CONFIG ?= pkg-config
INSTALL ?= install

PC_DEPS = libdrm libtsm
PC_CFLAGS := $(shell $(PKG_CONFIG) --cflags $(PC_DEPS))
PC_LIBS := $(shell $(PKG_CONFIG) --libs $(PC_DEPS))

CFLAGS += -std=c99 -D_GNU_SOURCE=1 -Wall -Wsign-compare -Wpointer-arith -Wcast-qual -Wcast-align

CPPFLAGS += $(PC_CFLAGS)
LDLIBS += $(PC_LIBS)

CC_BINARY(frecon): $(OBJS)

all: CC_BINARY(frecon)

clean: CLEAN(frecon)

install: all
	$(INSTALL) -m 744 frecon /sbin