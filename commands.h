/*
 * Copyright 2015 The Chromium OS Authors. All rights reserved.
 * Use of this source code is governed by a BSD-style license that can be
 * found in the LICENSE file.
 */

#ifndef COMMANDS_H
#define COMMANDS_H

#include <fsocket.h>

#include "dbus.h"

#define COMMAND_MAKE_VT               "MakeVT"
#define COMMAND_SWITCH_VT             "SwitchVT"
#define COMMAND_TERMINATE             "Terminate"
#define COMMAND_IMAGE                 "Image"

void command_init(dbus_t* dbus, unsigned short tcp_port);
void command_destroy(socket_interface_t*);

#endif
