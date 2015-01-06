/*
 * Copyright 2015 The Chromium OS Authors. All rights reserved.
 * Use of this source code is governed by a BSD-style license that can be
 * found in the LICENSE file.
 */

#ifndef FSOCKET_H
#define FSOCKET_H

#include <stdbool.h>
#include <stdlib.h>

#define   DEFAULT_TCP_PORT    (6530)

typedef struct _frecon_socket_t frecon_socket_t;
typedef void (*socket_handler_t)(frecon_socket_t *socket, void* user_data, char* buffer);

frecon_socket_t *socket_init(unsigned short tcp_port);
bool socket_add_callback(frecon_socket_t* socket,
		const char* method,
		socket_handler_t handler,
		void *user_data);
void socket_destroy(frecon_socket_t *socket);
void socket_add_fd(frecon_socket_t *socket, fd_set* read_set, fd_set* exception_set);
int socket_get_fd(frecon_socket_t *socket);
void socket_dispatch_io(frecon_socket_t* socket, fd_set* read_set, fd_set* exception_set);


#endif // FSOCKET_H
