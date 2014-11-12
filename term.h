/*
 * Copyright (c) 2014 The Chromium OS Authors. All rights reserved.
 * Use of this source code is governed by a BSD-style license that can be
 * found in the LICENSE file.
 */

#ifndef TERM_H
#define TERM_H

#include "video.h"
#include "input.h"

typedef struct _terminal_t {
  video_t  *video;
  dbus_t   *dbus;
  struct term *term;
  bool active;
} terminal_t;

struct term {
	struct tsm_screen *screen;
	struct tsm_vte *vte;
	struct shl_pty *pty;
	int pty_bridge;
	int pid;
	tsm_age_t age;
	int char_x, char_y;
	int pitch;
	uint32_t *dst_image;
};

terminal_t *term_init();
void term_close(terminal_t* terminal);
void term_redraw(terminal_t* terminal);
void term_set_dbus(terminal_t* terminal, dbus_t* dbus);
void term_close(terminal_t* terminal);
void term_key_event(terminal_t* terminal, uint32_t keysym, int32_t unicode);
bool term_is_child_done(terminal_t* terminal);

#endif
