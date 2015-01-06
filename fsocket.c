/*
 * Copyright 2015 The Chromium OS Authors. All rights reserved.
 * Use of this source code is governed by a BSD-style license that can be
 * found in the LICENSE file.
 */

#include <stdlib.h>
#include <unistd.h>
#include <memory.h>
#include <sys/socket.h>
#include <arpa/inet.h>
#include <fcntl.h>
#include "fsocket.h"

#include "commands.h"
#include "input.h"
#include "term.h"
#include "util.h"

#define MAX_HANDLERS           (10)
#define COMMAND_BUFFER_SZ      (200)

struct _command_map_t {
	char    command[20];
	socket_handler_t handler;
	void *user_data;
};

struct _frecon_socket_t {
	int    sock;
	int    conn;
	char   recvbuffer[COMMAND_BUFFER_SZ];
	struct _command_map_t handlers[10];
};

static void
_makevt_handler(frecon_socket_t *fsocket, void* user_data, char* buffer)
{
	int vt;
	terminal_t *terminal;

	vt = strtoul(buffer + strlen(COMMAND_MAKE_VT), NULL, 0);

	if ((vt < 1) || (vt > MAX_TERMINALS)) {
		LOG(ERROR, "makevt: invalid terminal");
		return;
	}

	terminal = input_create_term(vt);
	if (term_is_valid(terminal)) {
		send(fsocket->conn, term_get_ptsname(terminal), strlen(term_get_ptsname(terminal)), 0);
	}

}

static void
_switchvt_handler(frecon_socket_t *fsocket, void* user_data, char* buffer)
{
	int vt;
	terminal_t *terminal;

	vt = strtoul(buffer + strlen(COMMAND_SWITCH_VT), NULL, 0);

	if ((vt < 1) || (vt > MAX_TERMINALS)) {
		LOG(ERROR, "switchvt: invalid terminal");
		return;
	}

	terminal = input_create_term(vt);
	if (term_is_valid(terminal)) {
		term_activate(terminal);
	}
}

static void
_terminate_handler(frecon_socket_t *fsocket, void *user_data, char* buffer)
{
	exit(EXIT_SUCCESS);
}

frecon_socket_t *socket_init(unsigned short tcp_port)
{
	frecon_socket_t *fsocket;
	struct sockaddr_in server;
	int status;
	int flags;

	fsocket = (frecon_socket_t*)calloc(1, sizeof(*fsocket));

	fsocket->sock = socket(AF_INET, SOCK_STREAM, 0);
	if (fsocket->sock < 0) {
		LOG(ERROR, "Could not create socket: %m");
		goto fail;
	}

	flags = fcntl(fsocket->sock, F_GETFL, 0);
	if (flags < 0) {
		LOG(ERROR, "unable to get socket flags");
		goto fail;
	}

	flags |= O_NONBLOCK;
	fcntl(fsocket->sock, F_SETFL, flags);

	server.sin_family = AF_INET;
	server.sin_addr.s_addr = INADDR_ANY;
	server.sin_port = htons(tcp_port);

	status = bind(fsocket->sock, &server, sizeof(server));
	if (status < 0) {
		LOG(ERROR, "bind failed");
		goto fail;
	}

	status = listen(fsocket->sock, 5);
	if (status < 0) {
		LOG(ERROR, "listen failed");
		goto fail;
	}

	socket_add_callback(fsocket, COMMAND_MAKE_VT, _makevt_handler, fsocket);
	socket_add_callback(fsocket, COMMAND_SWITCH_VT, _switchvt_handler, fsocket);
	socket_add_callback(fsocket, COMMAND_TERMINATE, _terminate_handler, fsocket);

	return fsocket;

fail:
	if (fsocket->sock >= 0) {
		close(fsocket->sock);
		fsocket->sock = -1;
	}

	return NULL;

}

bool socket_add_callback(frecon_socket_t* socket,
		const char* method,
		socket_handler_t handler,
		void *user_data)
{
	for (int i = 0; i < MAX_HANDLERS; i++) {
		if (socket->handlers[i].handler == NULL) {
			strcpy(socket->handlers[i].command, method);
			socket->handlers[i].handler = handler;
			socket->handlers[i].user_data = user_data;
			return true;
		}
	}

	return false;
}

void socket_destroy(frecon_socket_t *socket)
{
	if (socket->conn > 0)
		close(socket->conn);

	if (socket->sock > 0)
		close(socket->sock);

	free(socket);
}

void socket_add_fd(frecon_socket_t* socket, fd_set* read_set, fd_set *exception_set)
{
	if (socket->conn > 0) {
		FD_SET(socket->conn, read_set);
		FD_SET(socket->conn, exception_set);
	}

	if (socket->sock > 0) {
		FD_SET(socket->sock, read_set);
		FD_SET(socket->sock, exception_set);
	}
}

int socket_get_fd(frecon_socket_t* socket)
{
	return MAX(socket->conn, socket->sock);
}

void socket_process_command(frecon_socket_t *fsocket, int s, char* buffer)
{
	for (int i = 0; i < MAX_HANDLERS; i++) {
		if (fsocket->handlers[i].handler != NULL) {
			if (strlen(buffer) < strlen(fsocket->handlers[i].command)) {
				continue;
			}

			if (strncmp(buffer, fsocket->handlers[i].command, strlen(fsocket->handlers[i].command)) == 0) {
				fsocket->handlers[i].handler(fsocket, fsocket->handlers[i].user_data, buffer);
				return;
			}
		}
	}
}

void socket_dispatch_io(frecon_socket_t *socket, fd_set* read_set, fd_set* exception_set)
{
	struct sockaddr_in addr;
	socklen_t  addrlen;
	int n;

	if (FD_ISSET(socket->sock, read_set)) {
		addrlen = sizeof(struct sockaddr_in);
		memset(&addr, 0, sizeof(struct sockaddr_in));
		socket->conn = accept(socket->sock, &addr, &addrlen);
	} else if ((socket->conn > 0) && (FD_ISSET(socket->conn, read_set))) {
		n = recv(socket->conn, socket->recvbuffer, sizeof(socket->recvbuffer), 0);
		if (n > 0) {
			socket->recvbuffer[n] = '\0';
			socket_process_command(socket, socket->conn, socket->recvbuffer);
		} else if (n <= 0) {
			close(socket->conn);
			socket->conn = -1;
		}
	}
}

