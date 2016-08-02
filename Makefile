# Copyright 2014 The Chromium OS Authors. All rights reserved.
# Use of this source code is governed by a BSD-style license that can be
# found in the LICENSE file.

include common.mk

DBUS ?= 1

$(info $$DBUS is [${DBUS}])

PC_DEPS = libdrm libudev libpng libtsm
TARGET = frecon-lite
ifeq ($(DBUS),1)
PC_DEPS += dbus-1
CPPFLAGS += -DDBUS=1
TARGET = frecon
endif
PC_CFLAGS := $(shell $(PKG_CONFIG) --cflags $(PC_DEPS))
PC_LIBS := $(shell $(PKG_CONFIG) --libs $(PC_DEPS))

CPPFLAGS += -std=c99 -D_GNU_SOURCE=1
CFLAGS += -Wall -Wsign-compare -Wpointer-arith -Wcast-qual -Wcast-align

CPPFLAGS += $(PC_CFLAGS) -I$(OUT)
LDLIBS += $(PC_LIBS)

$(info $$PC_DEPS is [${PC_DEPS}])
$(info $$CPPFLAGS is [${CPPFLAGS}])

$(OUT)glyphs.h: $(SRC)/font_to_c.py $(SRC)/ter-u16n.bdf
	python2 $(SRC)/font_to_c.py $(SRC)/ter-u16n.bdf $(OUT)glyphs.h

font.o.depends: $(OUT)glyphs.h

CC_BINARY($(TARGET)): $(C_OBJECTS)

all: CC_BINARY($(TARGET))

clean: CLEAN($(TARGET))

install: all
	mkdir -p $(DESTDIR)/sbin
	install -m 755 $(OUT)/$(TARGET) $(DESTDIR)/sbin
