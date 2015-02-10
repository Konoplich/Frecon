/*
 * Copyright (c) 2014 The Chromium OS Authors. All rights reserved.
 * Use of this source code is governed by a BSD-style license that can be
 * found in the LICENSE file.
 */

#ifndef INPUT_H
#define INPUT_H

#include <linux/input.h>
#include <fsocket.h>

#include "dbus.h"
#include "term.h"
#include "video.h"

struct input_key_event {
	uint16_t code;
	unsigned char value;
};

int input_init();
int input_run(bool standalone);
void input_set_terminal(terminal_t*);
void input_close();
void input_set_dbus(dbus_t* dbus);
void input_set_socket(fsocket_t* socket);
int input_setfds(fd_set *read_set, fd_set *exception_set);
struct input_key_event *input_get_event(fd_set *read_fds, fd_set *exception_set);
void input_put_event(struct input_key_event *event);
void input_grab();
void input_ungrab();
terminal_t* input_create_term(int vt);
void input_set_socket_interface(socket_interface_t*);

#endif
