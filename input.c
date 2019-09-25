/*
 * Copyright (c) 2014 The Chromium OS Authors. All rights reserved.
 * Use of this source code is governed by a BSD-style license that can be
 * found in the LICENSE file.
 */

#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/select.h>
#include <unistd.h>

#include "dbus.h"
#include "dbus_interface.h"
#include "input.h"
#include "keysym.h"
#include "main.h"
#include "util.h"

struct input_key_event {
	uint16_t code;
	unsigned char value;
	int keyboard_layout_id;
};

struct input_dev {
	int fd;
	char* path;
	int layout_id;
};

struct keyboard_state {
	bool left_shift_state;
	bool right_shift_state;
	bool left_control_state;
	bool right_control_state;
	bool left_alt_state;
	bool right_alt_state;
	bool search_state;
};

/*
 * structure to keep input state:
 *  ndevs - number of input devices.
 *  devs - input devices to listen to.
 *  kbd_state - tracks modifier keys that are pressed.
 */
struct {
	unsigned int ndevs;
	struct input_dev* devs;
	struct keyboard_state kbd_state;
} input = {
	.ndevs = 0,
	.devs = NULL,
};

static bool is_shift_pressed(struct keyboard_state* k)
{
	return k->left_shift_state || k->right_shift_state;
}

static bool is_control_pressed(struct keyboard_state* k)
{
	return k->left_control_state || k->right_control_state;
}

static bool is_alt_pressed(struct keyboard_state* k)
{
	return k->left_alt_state || k->right_alt_state;
}

static uint16_t input_action_key_to_fn_key(const struct input_key_event* ev)
{
	/* Some devices, such as sarien, emit action key codes by default for the
	 * top-row keys. Mapping these action key codes to Fn key codes is a
	 * convenience. For instance, it allows switching to VT2 terminal with
	 * ctrl+alt+f2 rather than ctrl+alt+Fn+f2.
	 */
	struct {
		uint32_t action_code;
		uint32_t fn_code;
	} layout3_action_to_fn[] = {
		{ KEY_BACK, KEY_F1},
		{ KEY_REFRESH, KEY_F2},
		{ KEY_ZOOM, KEY_F3},
		{ KEY_SCALE, KEY_F4},
		{ KEY_BRIGHTNESSDOWN, KEY_F5},
		{ KEY_BRIGHTNESSUP, KEY_F6},
		{ KEY_MUTE, KEY_F7},
		{ KEY_VOLUMEDOWN, KEY_F8},
		{ KEY_VOLUMEUP, KEY_F9},
		{ KEY_SWITCHVIDEOMODE, KEY_F12},
	};

	if (ev->keyboard_layout_id == 3) {
		for (unsigned i = 0; i < ARRAY_SIZE(layout3_action_to_fn); i++) {
			if (layout3_action_to_fn[i].action_code == ev->code) {
				return layout3_action_to_fn[i].fn_code;
			}
		}
	}
	return ev->code;
}

static int input_special_key(struct input_key_event* ev)
{
	terminal_t* terminal;

	uint32_t ignore_keys[] = {
		BTN_TOUCH, // touchpad events
		BTN_TOOL_FINGER,
		BTN_TOOL_DOUBLETAP,
		BTN_TOOL_TRIPLETAP,
		BTN_TOOL_QUADTAP,
		BTN_TOOL_QUINTTAP,
		BTN_LEFT, // mouse buttons
		BTN_RIGHT,
		BTN_MIDDLE,
		BTN_SIDE,
		BTN_EXTRA,
		BTN_FORWARD,
		BTN_BACK,
		BTN_TASK
	};

	terminal = term_get_current_terminal();

	for (unsigned int i = 0; i < ARRAY_SIZE(ignore_keys); i++)
		if (ev->code == ignore_keys[i])
			return 1;

	switch (ev->code) {
	case KEY_LEFTSHIFT:
		input.kbd_state.left_shift_state = ! !ev->value;
		return 1;
	case KEY_RIGHTSHIFT:
		input.kbd_state.right_shift_state = ! !ev->value;
		return 1;
	case KEY_LEFTCTRL:
		input.kbd_state.left_control_state = ! !ev->value;
		return 1;
	case KEY_RIGHTCTRL:
		input.kbd_state.right_control_state = ! !ev->value;
		return 1;
	case KEY_LEFTALT:
		input.kbd_state.left_alt_state = ! !ev->value;
		return 1;
	case KEY_RIGHTALT:
		input.kbd_state.right_alt_state = ! !ev->value;
		return 1;
	case KEY_LEFTMETA: // search key
		input.kbd_state.search_state = ! !ev->value;
		return 1;
	}

	if (term_is_active(terminal)) {
		if (is_shift_pressed(&input.kbd_state) && ev->value) {
			switch (ev->code) {
			case KEY_PAGEUP:
				term_page_up(terminal);
				return 1;
			case KEY_PAGEDOWN:
				term_page_down(terminal);
				return 1;
			case KEY_UP:
				if (input.kbd_state.search_state)
					term_page_up(terminal);
				else
					term_line_up(terminal);
				return 1;
			case KEY_DOWN:
				if (input.kbd_state.search_state)
					term_page_down(terminal);
				else
					term_line_down(terminal);
				return 1;
			}
		}

		if (!is_alt_pressed(&input.kbd_state) &&
		    is_control_pressed(&input.kbd_state) &&
		    is_shift_pressed(&input.kbd_state) && ev->value) {
			switch (ev->code) {
			case KEY_MINUS:
				term_zoom(false);
				return 1;
			case KEY_EQUAL:
				term_zoom(true);
				return 1;
			}
		}

		if (!(input.kbd_state.search_state ||
		     is_alt_pressed(&input.kbd_state) ||
		     is_control_pressed(&input.kbd_state)) &&
		    ev->value && (ev->code >= KEY_F1) && (ev->code <= KEY_F10)) {
			switch (ev->code) {
				case KEY_F1:
				case KEY_F2:
				case KEY_F3:
				case KEY_F4:
				case KEY_F5:
					break;
				case KEY_F6:
				case KEY_F7:
					dbus_report_user_activity(USER_ACTIVITY_BRIGHTNESS_DOWN_KEY_PRESS -
								(ev->code - KEY_F6));
					break;
				case KEY_F8:
				case KEY_F9:
				case KEY_F10:
					break;
			}
			return 1;
		}
	}

	if (command_flags.enable_vts &&
	    is_alt_pressed(&input.kbd_state) &&
	    is_control_pressed(&input.kbd_state) &&
	    ev->value) {
		/*
		 * Special case for key sequence that is used by external program.   Just
		 * explicitly ignore here and do nothing.
		 */
		if (is_shift_pressed(&input.kbd_state))
			return 1;

		uint16_t code = input_action_key_to_fn_key(ev);
		if ((code >= KEY_F1) && (code < KEY_F1 + term_num_terminals)) {
			term_switch_to(code - KEY_F1);
		}

		return 1;
	}

	return 0;
}

static void input_get_keysym_and_unicode(struct input_key_event* event,
					 uint32_t* keysym, uint32_t* unicode)
{
	struct {
		uint32_t code;
		uint32_t keysym;
	} search_keys[] = {
		{ KEY_F1, KEYSYM_F1},
		{ KEY_F2, KEYSYM_F2},
		{ KEY_F3, KEYSYM_F3},
		{ KEY_F4, KEYSYM_F4},
		{ KEY_F5, KEYSYM_F5},
		{ KEY_F6, KEYSYM_F6},
		{ KEY_F7, KEYSYM_F7},
		{ KEY_F8, KEYSYM_F8},
		{ KEY_F9, KEYSYM_F8},
		{ KEY_F10, KEYSYM_F10},
		{ KEY_UP, KEYSYM_PAGEUP},
		{ KEY_DOWN, KEYSYM_PAGEDOWN},
		{ KEY_LEFT, KEYSYM_HOME},
		{ KEY_RIGHT, KEYSYM_END},
	};

	struct {
		uint32_t code;
		uint32_t keysym;
	} non_ascii_keys[] = {
		{ KEY_ESC, KEYSYM_ESC},
		{ KEY_HOME, KEYSYM_HOME},
		{ KEY_LEFT, KEYSYM_LEFT},
		{ KEY_UP, KEYSYM_UP},
		{ KEY_RIGHT, KEYSYM_RIGHT},
		{ KEY_DOWN, KEYSYM_DOWN},
		{ KEY_PAGEUP, KEYSYM_PAGEUP},
		{ KEY_PAGEDOWN, KEYSYM_PAGEDOWN},
		{ KEY_END, KEYSYM_END},
		{ KEY_INSERT, KEYSYM_INSERT},
		{ KEY_DELETE, KEYSYM_DELETE},
	};

	if (input.kbd_state.search_state) {
		for (unsigned i = 0; i < ARRAY_SIZE(search_keys); i++) {
			if (search_keys[i].code == event->code) {
				*keysym = search_keys[i].keysym;
				*unicode = -1;
				return;
			}
		}
	}

	for (unsigned i = 0; i < ARRAY_SIZE(non_ascii_keys); i++) {
		if (non_ascii_keys[i].code == event->code) {
			*keysym = non_ascii_keys[i].keysym;
			*unicode = -1;
			return;
		}
	}

	if (event->code >= ARRAY_SIZE(keysym_table) / 2) {
		*keysym = '?';
	} else {
		*keysym = keysym_table[event->code * 2 + is_shift_pressed(&input.kbd_state)];
		if (is_control_pressed(&input.kbd_state) && isascii(*keysym))
			*keysym = tolower(*keysym) - 'a' + 1;
	}

	*unicode = *keysym;
}

int input_add(const char* devname, int layout_id)
{
	int ret = 0, fd = -1;

	/* for some reason every device has a null enumerations and notifications
	   of every device come with NULL string first */
	if (!devname) {
		ret = -EINVAL;
		goto errorret;
	}
	/* check for duplicates */
	for (unsigned int i = 0; i < input.ndevs; ++i) {
		if (strcmp(devname, input.devs[i].path) == 0) {
			LOG(INFO, "Skipping duplicate input device %s", devname);
			ret = -EINVAL;
			goto errorret;
		}
	}
	ret = fd = open(devname, O_RDONLY);
	if (fd < 0)
		goto errorret;

	ret = ioctl(fd, EVIOCGRAB, (void*) 1);
	if (!ret) {
		ret = ioctl(fd, EVIOCGRAB, (void*) 0);
		if (ret)
			LOG(ERROR,
				"EVIOCGRAB succeeded but the corresponding ungrab failed: %m");
	} else {
		LOG(ERROR, "Evdev device %s grabbed by another process",
			devname);
		ret = -EBUSY;
		goto closefd;
	}

	struct input_dev* newdevs =
	    realloc(input.devs, (input.ndevs + 1) * sizeof (struct input_dev));
	if (!newdevs) {
		ret = -ENOMEM;
		goto closefd;
	}
	input.devs = newdevs;
	input.devs[input.ndevs].fd = fd;
	input.devs[input.ndevs].path = strdup(devname);
	input.devs[input.ndevs].layout_id = layout_id;
	if (!input.devs[input.ndevs].path) {
		ret = -ENOMEM;
		goto closefd;
	}
	input.ndevs++;

	return fd;

closefd:
	close(fd);
errorret:
	return ret;
}

void input_remove(const char* devname)
{
	unsigned int u;

	if (!devname)
		return;

	for (u = 0; u < input.ndevs; u++) {
		if (!strcmp(devname, input.devs[u].path)) {
			free(input.devs[u].path);
			close(input.devs[u].fd);
			input.ndevs--;
			if (u != input.ndevs) {
				input.devs[u] = input.devs[input.ndevs];
			}
			return;
		}
	}
}

int input_init()
{
	if (!isatty(fileno(stdout)))
		setbuf(stdout, NULL);
	return 0;
}

void input_close()
{
	unsigned int u;

	for (u = 0; u < input.ndevs; u++) {
		free(input.devs[u].path);
		close(input.devs[u].fd);
	}
	free(input.devs);
	input.devs = NULL;
	input.ndevs = 0;
}

void input_add_fds(fd_set* read_set, fd_set* exception_set, int *maxfd)
{
	unsigned int u;

	for (u = 0; u < input.ndevs; u++) {
		FD_SET(input.devs[u].fd, read_set);
		FD_SET(input.devs[u].fd, exception_set);
		if (input.devs[u].fd > *maxfd)
			*maxfd = input.devs[u].fd;
	}
}

struct input_key_event* input_get_event(fd_set* read_set,
					fd_set* exception_set)
{
	unsigned int u;
	struct input_event ev;
	int ret;

	for (u = 0; u < input.ndevs; u++) {
		if (FD_ISSET(input.devs[u].fd, read_set)
		    && !FD_ISSET(input.devs[u].fd, exception_set)) {
			ret =
			    read(input.devs[u].fd, &ev, sizeof (struct input_event));
			if (ret < 0) {
				if (errno == EINTR || errno == EAGAIN)
					continue;
				if (errno != ENODEV) {
					LOG(ERROR, "read: %s: %s", input.devs[u].path,
						strerror(errno));
				}
				input_remove(input.devs[u].path);
				return NULL;
			} else if (ret < (int) sizeof (struct input_event)) {
				LOG(ERROR, "expected %d bytes, got %d",
				       (int) sizeof (struct input_event), ret);
				return NULL;
			}

			if (ev.type == EV_KEY) {
				struct input_key_event* event =
				    malloc(sizeof (*event));
				event->code = ev.code;
				event->value = ev.value;
				event->keyboard_layout_id = input.devs[u].layout_id;
				return event;
			} else if (ev.type == EV_SW && ev.code == SW_LID) {
				/* TODO(dbehr), abstract this in input_key_event if we ever parse more than one */
				term_monitor_hotplug();
			}
		}
	}

	return NULL;
}

void input_put_event(struct input_key_event* event)
{
	free(event);
}

void input_dispatch_io(fd_set* read_set, fd_set* exception_set)
{
	terminal_t* terminal;
	struct input_key_event* event;

	event = input_get_event(read_set, exception_set);
	if (event) {
		if (!input_special_key(event) && event->value) {
			uint32_t keysym, unicode;
			// current_terminal can possibly change during
			// execution of input_special_key
			terminal = term_get_current_terminal();
			if (term_is_active(terminal)) {
				// Only report user activity when the terminal is active
				dbus_report_user_activity(USER_ACTIVITY_OTHER);
				input_get_keysym_and_unicode(
					event, &keysym, &unicode);
				term_key_event(terminal,
						keysym, unicode);
			}
		}
		input_put_event(event);
	}
}

#define BITS_PER_LONG (sizeof(long) * 8)
#define BITS_TO_LONGS(bits) (((bits) - 1) / BITS_PER_LONG + 1)
#define BITMASK_GET_BIT(bitmask, bit) \
    ((bitmask[bit / BITS_PER_LONG] >> (bit % BITS_PER_LONG)) & 1)

static const int kMaxBit = MAX(MAX(EV_MAX, KEY_MAX), SW_MAX);

static bool has_event_bit(int fd, int event_type, int bit)
{
	unsigned long bitmask[BITS_TO_LONGS(kMaxBit+1)];
	memset(bitmask, 0, sizeof(bitmask));

	if (ioctl(fd, EVIOCGBIT(event_type, sizeof(bitmask)), bitmask) < 0)
		return false;

	return BITMASK_GET_BIT(bitmask, bit);
}

static int get_switch_bit(int fd, int bit) {
	unsigned long bitmask[BITS_TO_LONGS(SW_MAX+1)];
	memset(bitmask, 0, sizeof(bitmask));
	if (ioctl(fd, EVIOCGSW(sizeof(bitmask)), bitmask) < 0)
		return -1;

	return BITMASK_GET_BIT(bitmask, bit);
}

static bool is_lid_switch(int fd)
{
	return has_event_bit(fd, 0, EV_SW) && has_event_bit(fd, EV_SW, SW_LID);
}

int input_check_lid_state(void)
{
	unsigned int u;

	for (u = 0; u < input.ndevs; u++) {
		if (is_lid_switch(input.devs[u].fd)) {
			return get_switch_bit(input.devs[u].fd, SW_LID);
		}
	}
	return -ENODEV;
}
