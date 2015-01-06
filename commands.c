/*
 * Copyright 2015 The Chromium OS Authors. All rights reserved.
 * Use of this source code is governed by a BSD-style license that can be
 * found in the LICENSE file.
 */

#include <dlfcn.h>
#include <sys/socket.h>

#include "commands.h"

#include "image.h"
#include "input.h"
#include "util.h"

typedef struct _options_t {
  struct _options_t *next;
  char* opt;
  char* val;
} options_t;

static
options_t* _parse_options(char* buffer)
{
  options_t *list = NULL;
  char* saved_ptr;
  char* token;
  options_t *entry;

  saved_ptr = NULL;
  token = strtok_r(buffer, " :\t,", &saved_ptr);
  entry = calloc(1, sizeof(options_t));
  entry->opt = malloc(strlen(token) + 1);
  strcpy(entry->opt, token);
  for (;;) {
    token = strtok_r(NULL, " :\t,", &saved_ptr);
    if (!token)
      break;
    if (entry) {
      if (entry->opt) {
        entry->val = malloc(strlen(token) + 1);
        strcpy(entry->val, token);
        entry->next = list;
        list = entry;
        entry = NULL;
      }
    } else {
      entry = calloc(1, sizeof(options_t));
      entry->opt = malloc(strlen(token) + 1);
      strcpy(entry->opt, token);
    }
  }

	return list;
}

static
void _parse_location(char* loc_str, int *x, int *y)
{
	int count = 0;
	char* savedptr;
	char* token;
	char* str;
	int *results[] = {x, y};

	for (token = str = loc_str; token != NULL; str = NULL) {
		if (count > 1)
			break;

		token = strtok_r(str, ",", &savedptr);
		if (token) {
			*(results[count++]) = strtol(token, NULL, 0);
		}
	}
}

static
void _makevt_handler(fsocket_t *fsocket, void* user_data, char* buffer)
{
	int vt;
	terminal_t *terminal;
	socket_interface_t *socket_interface = (socket_interface_t*)user_data;

	vt = strtoul(buffer + strlen(COMMAND_MAKE_VT), NULL, 0);

	if ((vt < 1) || (vt > MAX_TERMINALS)) {
		LOG(ERROR, "makevt: invalid terminal");
		return;
	}

	terminal = input_create_term(vt);
	if (term_is_valid(terminal)) {
		send(socket_interface->socket_get_fd(fsocket),
				term_get_ptsname(terminal),
				strlen(term_get_ptsname(terminal)), 0);
	}

}

static
void _switchvt_handler(fsocket_t *fsocket, void* user_data, char* buffer)
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

static
void _terminate_handler(fsocket_t *fsocket, void *user_data, char* buffer)
{
	exit(EXIT_SUCCESS);
}

static
void _image_handler(fsocket_t *fsocket, void* user_data, char* buffer)
{
	options_t* list;
	options_t* entry;
	int x, y;
	int status;
	bool use_location;
	bool use_offset;
	image_t image;
	terminal_t *terminal;
	buffer_properties_t *bp;
	uint32_t *video_buffer;

	memset(&image, 0, sizeof(image));
	list = _parse_options(buffer + strlen(COMMAND_IMAGE));

	use_location = false;
	use_offset = false;
	while (list) {
		entry = list;
		LOG(ERROR, "option = %s (%s)\n", entry->opt, entry->val);
		if (strcmp(entry->opt, "image") == 0) {
			strncpy(image.filename, entry->val, 100);
		} else if (strcmp(entry->opt, "location") == 0) {
			_parse_location(entry->val, &x, &y);
			use_location = true;
		} else if (strcmp(entry->opt, "offset") == 0) {
			_parse_location(entry->val, &x, &y);
			use_offset = true;
		}
		list = list->next;
		free(entry);
	}

	if (use_location && use_offset)
		use_offset = false;

	status = image_load_image_from_file(&image);
	if (status != 0) {
		LOG(WARNING, "image_load_image_from_file failed: %d\n", status);
		return;
	}

	terminal = input_create_term(0);
	if (!terminal)
		return;

	bp = video_get_buffer_properties(terminal->video);

	if (use_offset) {
		x = (bp->width - image.width - x)/2;
		y = (bp->height - image.height - y)/2;
	} else if (!use_location) {
		x = (bp->width - image.width)/2;
		y = (bp->height - image.height)/2;
	}
	video_buffer = video_lock(terminal->video);
	image_show(terminal->video, &image, video_buffer, bp->pitch, x, y);
	video_unlock(terminal->video);
}

void command_init(dbus_t* dbus, unsigned short tcp_port)
{
	socket_interface_t* socket_interface;
	fsocket_t *fsocket;
	void *handle;

	socket_interface = calloc(1, sizeof(socket_interface_t));
	handle = dlopen("libfsocket.so.0", RTLD_LAZY);
	if (handle != NULL) {
		LOG(ERROR, "library loaded");
		socket_interface->socket_init = dlsym(handle, "fsocket_init");
		socket_interface->socket_add_callback = dlsym(handle,
				"fsocket_add_callback");
		socket_interface->socket_add_fd = dlsym(handle, "fsocket_add_fd");
		socket_interface->socket_get_fd = dlsym(handle, "fsocket_get_fd");
		socket_interface->socket_dispatch_io = dlsym(handle, "fsocket_dispatch_io");
		input_set_socket_interface(socket_interface);

		LOG(ERROR, "fsocket_init = %p\n", socket_interface->socket_init);
	} else {
		LOG(INFO, "Failed to load fsocket library");
	}

	if (socket_interface->socket_init != NULL) {
		fsocket = socket_interface->socket_init(tcp_port);
		input_set_socket(fsocket);
		socket_interface->socket_add_callback(fsocket, COMMAND_MAKE_VT,
				_makevt_handler, &socket_interface);
		socket_interface->socket_add_callback(fsocket, COMMAND_SWITCH_VT,
				_switchvt_handler, &socket_interface);
		socket_interface->socket_add_callback(fsocket, COMMAND_TERMINATE,
				_terminate_handler, &socket_interface);
		socket_interface->socket_add_callback(fsocket, COMMAND_IMAGE,
				_image_handler, &socket_interface);
	} else {
		/*
		 * If splash_run didn't create the dbus object (for example, if
		 * we didn't supply the frame-interval parameter, then go ahead
		 * and create it now
		 */
		if (dbus == NULL) {
			dbus = dbus_init();
		}
		input_set_dbus(dbus);
	}
}

void command_destroy(socket_interface_t* socket_interface)
{
	free(socket_interface);
}

