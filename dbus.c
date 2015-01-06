/*
 * Copyright (c) 2014 The Chromium OS Authors. All rights reserved.
 * Use of this source code is governed by a BSD-style license that can be
 * found in the LICENSE file.
 */

#include <stdlib.h>

#include "dbus.h"

#include "commands.h"
#include "dbus_interface.h"
#include "input.h"
#include "term.h"
#include "util.h"

struct _dbus_t {
	DBusConnection *conn;
	int terminate;
	DBusWatch *watch;
	int fd;
	struct {
		DBusObjectPathVTable vtable;
		const char* interface;
		const char* signal;
		const char* rule;
		void* user_data;
		dbus_message_handler_t signal_handler;
	} signal;
};

static DBusHandlerResult
_handle_switchvt(DBusConnection *connection, DBusMessage *message)
{
	DBusMessage *reply;
	DBusMessage *msg;
	DBusError error;
	dbus_bool_t stat;
	terminal_t *terminal;
	int vt;

	dbus_error_init(&error);
	stat = dbus_message_get_args(message, &error, DBUS_TYPE_INT32,
			&vt, DBUS_TYPE_INVALID);

	if (!stat) {
		LOG(ERROR, "SwitchVT method error, not VT argument");
		return DBUS_HANDLER_RESULT_NOT_YET_HANDLED;
	}

	if ((vt < 0) || (vt > MAX_TERMINALS)) {
		LOG(ERROR, "SwtichVT: invalid terminal");
		return DBUS_HANDLER_RESULT_NOT_YET_HANDLED;
	}

	if (vt == 0) {
		terminal = input_create_term(vt);
		if (term_is_active(terminal)) {
			input_ungrab();
			terminal->active = false;
			video_release(terminal->video);
			msg = dbus_message_new_method_call(
				kLibCrosServiceName,
				kLibCrosServicePath,
				kLibCrosServiceInterface,
				kTakeDisplayOwnership);
			dbus_connection_send_with_reply_and_block(connection, msg, 3000, NULL);
		}
		reply = dbus_message_new_method_return(message);
		dbus_connection_send(connection, reply, NULL);
		return DBUS_HANDLER_RESULT_HANDLED;
	} else {
		terminal = input_create_term(vt);
		if (term_is_valid(terminal)) {
			term_activate(terminal);

			reply = dbus_message_new_method_return(message);
			dbus_connection_send(connection, reply, NULL);
			return DBUS_HANDLER_RESULT_HANDLED;
		}
	}

	return DBUS_HANDLER_RESULT_NOT_YET_HANDLED;
}

static DBusHandlerResult
_handle_makevt(DBusConnection *connection, DBusMessage *message)
{
	DBusMessage *reply;
	DBusError error;
	dbus_bool_t stat;
	terminal_t *terminal;
	int vt;
	const char *reply_str;

	dbus_error_init(&error);
	stat = dbus_message_get_args(message, &error, DBUS_TYPE_INT32,
			&vt, DBUS_TYPE_INVALID);

	if (!stat) {
		LOG(ERROR, "SwitchVT method error, not VT argument");
		return DBUS_HANDLER_RESULT_NOT_YET_HANDLED;
	}

	if ((vt < 1) || (vt > MAX_TERMINALS)) {
		LOG(ERROR, "SwtichVT: invalid terminal");
		return DBUS_HANDLER_RESULT_NOT_YET_HANDLED;
	}

	terminal = input_create_term(vt);
	reply_str = term_get_ptsname(terminal);

	reply = dbus_message_new_method_return(message);
	dbus_message_append_args(reply,
			DBUS_TYPE_STRING, &reply_str,
			DBUS_TYPE_INVALID);
	dbus_connection_send(connection, reply, NULL);

	return DBUS_HANDLER_RESULT_HANDLED;
}

static DBusHandlerResult
_handle_terminate(DBusConnection *connection, DBusMessage *message)
{
	DBusMessage *reply;

	reply = dbus_message_new_method_return(message);
	dbus_connection_send(connection, reply, NULL);
	exit(EXIT_SUCCESS);
}

static void
_frecon_dbus_unregister(DBusConnection *connection, void* user_data)
{
}


static DBusHandlerResult
_frecon_dbus_message_handler(DBusConnection *connection, DBusMessage *message, void* user_data)
{
	if (dbus_message_is_method_call(message,
				kFreconDbusInterface, COMMAND_SWITCH_VT)) {
		return _handle_switchvt(connection, message);
	} else if (dbus_message_is_method_call(message,
				kFreconDbusInterface, COMMAND_MAKE_VT)) {
		return _handle_makevt(connection, message);
	}
	else if (dbus_message_is_method_call(message,
				kFreconDbusInterface, COMMAND_TERMINATE)) {
		return _handle_terminate(connection, message);
	}

	return DBUS_HANDLER_RESULT_NOT_YET_HANDLED;

}

static DBusObjectPathVTable
frecon_vtable = {
	_frecon_dbus_unregister,
	_frecon_dbus_message_handler,
	NULL
};

dbus_bool_t add_watch(DBusWatch *w, void* data)
{
	dbus_t *dbus = (dbus_t*)data;
	dbus->watch = w;

	return TRUE;
}

void remove_watch(DBusWatch *w, void* data)
{
}

void toggle_watch(DBusWatch *w, void* data)
{
}

dbus_t* dbus_init()
{
	dbus_t* new_dbus;
	DBusError err;
	int result;
	dbus_bool_t stat;

	dbus_error_init(&err);

	new_dbus = (dbus_t*)calloc(1, sizeof(*new_dbus));
	new_dbus->fd = -1;

	new_dbus->conn = dbus_bus_get(DBUS_BUS_SYSTEM, &err);
	if (dbus_error_is_set(&err)) {
		LOG(ERROR, "Cannot get dbus connection");
		free(new_dbus);
		return NULL;
	}

	result = dbus_bus_request_name(new_dbus->conn, kFreconDbusInterface,
			DBUS_NAME_FLAG_DO_NOT_QUEUE, &err);

	if (result <= 0) {
		LOG(ERROR, "Unable to get name for server");
	}

	stat = dbus_connection_register_object_path(new_dbus->conn,
			kFreconDbusPath,
			&frecon_vtable,
			NULL);

	if (!stat) {
		LOG(ERROR, "failed to register object path");
	}

	stat = dbus_connection_set_watch_functions(new_dbus->conn,
			add_watch, remove_watch, toggle_watch,
			new_dbus, NULL);

	if (!stat) {
		LOG(ERROR, "Failed to set watch functions");
	}

	dbus_connection_set_exit_on_disconnect(new_dbus->conn, FALSE);

	return new_dbus;
}


bool dbus_method_call0(dbus_t* dbus, const char* service_name,
		const char* service_path, const char* service_interface,
		const char* method)
{
	DBusMessage *msg = NULL;

	msg = dbus_message_new_method_call(service_name,
			service_path, service_interface, method);

	if (!msg)
		return false;

	if (!dbus_connection_send_with_reply_and_block(dbus->conn,
				msg, 3000, NULL)) {
		dbus_message_unref(msg);
		return false;
	}

	dbus_connection_flush(dbus->conn);
	dbus_message_unref(msg);

	return true;
}

bool dbus_method_call1(dbus_t* dbus, const char* service_name,
		const char* service_path, const char* service_interface,
		const char* method, int* param)
{
	DBusMessage *msg = NULL;

	msg = dbus_message_new_method_call(service_name,
			service_path, service_interface, method);

	if (!msg)
		return false;

	if (!dbus_message_append_args(msg,
				DBUS_TYPE_INT32, param, DBUS_TYPE_INVALID)) {
		dbus_message_unref(msg);
		return false;
	}

	if (!dbus_connection_send_with_reply_and_block(dbus->conn,
				msg, 3000, NULL)) {
		dbus_message_unref(msg);
		return false;
	}

	dbus_connection_flush(dbus->conn);
	dbus_message_unref(msg);

	return true;
}

static void
dbus_path_unregister_function(DBusConnection *connection, void *user_data)
{
}

static DBusHandlerResult
dbus_message_function(DBusConnection *connection,
		DBusMessage *message, void* user_data)
{
	dbus_t* dbus = (dbus_t*)user_data;

	if (dbus_message_is_signal(message, dbus->signal.interface,
				dbus->signal.signal)) {
		dbus->signal.signal_handler(dbus, dbus->signal.user_data);
	}
	return DBUS_HANDLER_RESULT_HANDLED;
}

bool dbus_signal_match_handler(
		dbus_t* dbus,
		const char* signal,
		const char* path,
		const char* interface,
		const char* rule,
		dbus_message_handler_t handler,
		void *user_data)
{
	DBusError	 err;
	dbus->signal.vtable.unregister_function = dbus_path_unregister_function;
	dbus->signal.vtable.message_function = dbus_message_function;
	dbus->signal.signal_handler = handler;
	dbus->signal.signal = signal;
	dbus->signal.user_data = user_data;
	dbus->signal.interface = interface;

	if (!dbus_connection_register_object_path(dbus->conn, path,
				&dbus->signal.vtable, dbus)) {
		LOG(ERROR, "register_object_path failed");
		return false;
	}

	dbus_error_init(&err);
	dbus_bus_add_match(dbus->conn, rule, &err);
	if (dbus_error_is_set(&err)) {
		LOG(ERROR, "add_match failed: %s", err.message);
		return false;
	}

	return true;
}

void dbus_destroy(dbus_t* dbus)
{
	/* FIXME - not sure what the right counterpart to
		dbus_bus_get() is, unref documentation is rather
		unclear. Not a big issue but it would be nice to
		clean up properly here */
	/* dbus_connection_unref(dbus->conn); */
	if (dbus)
		free(dbus);
}

void dbus_add_fd(dbus_t* dbus, fd_set* read_set, fd_set* exception_set)
{
	if (dbus->fd < 0)
		dbus->fd = dbus_watch_get_unix_fd(dbus->watch);

	if (dbus->fd >= 0) {
		FD_SET(dbus->fd, read_set);
		FD_SET(dbus->fd, exception_set);
	}
}

int dbus_get_fd(dbus_t* dbus)
{
	if (dbus->fd < 0)
		dbus->fd = dbus_watch_get_unix_fd(dbus->watch);

	return dbus->fd;
}


void dbus_dispatch_io(dbus_t* dbus)
{
	dbus_watch_handle(dbus->watch, DBUS_WATCH_READABLE);
	while (dbus_connection_get_dispatch_status(dbus->conn)
			== DBUS_DISPATCH_DATA_REMAINS) {
		dbus_connection_dispatch(dbus->conn);
	}
}
