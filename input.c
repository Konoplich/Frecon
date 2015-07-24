/*
 * Copyright (c) 2014 The Chromium OS Authors. All rights reserved.
 * Use of this source code is governed by a BSD-style license that can be
 * found in the LICENSE file.
 */

#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <libudev.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/select.h>
#include <unistd.h>

#include "input.h"

#include "dbus_interface.h"
#include "dbus.h"
#include "keysym.h"
#include "util.h"

#define MAX_STD_TERMINALS     (3)
#define NUM_SPLASH_TERMINAL   (1)
#define MAX_TERMINALS         (MAX_STD_TERMINALS + NUM_SPLASH_TERMINAL)
#define SPLASH_TERMINAL       (MAX_TERMINALS - 1)

struct input_dev {
	int fd;
	char *path;
};

struct keyboard_state {
	int shift_state;
	int control_state;
	int alt_state;
	int search_state;
};

/*
 * structure to keep input state:
 *  udev - for udev events.
 *  udev_monitor - used to listen for udev events.
 *  udev_fd - to poll for udev messages.
 *  ndevs - number of input devices.
 *  devs - input devices to listen to.
 *  kbd_state - tracks modifier keys that are pressed.
 *  dbus - where to send dbus events.
 *  current_terminal - the currently selected terminal.
 *  terminals - list of all terminals that have been created.
 */
struct {
	struct udev *udev;
	struct udev_monitor *udev_monitor;
	int udev_fd;
	unsigned int ndevs;
	struct input_dev *devs;
	struct keyboard_state kbd_state;
	dbus_t *dbus;
	uint32_t  current_terminal;
	terminal_t *terminals[MAX_TERMINALS];
} input = {
	.udev = NULL,
	.udev_monitor = NULL,
	.udev_fd = -1,
	.ndevs = 0,
	.devs = NULL,
	.dbus = NULL,
	.current_terminal = 0
};

static void report_user_activity(int activity_type)
{
	dbus_bool_t allow_off = false;
	if (!input.dbus)
		return;

	dbus_method_call1(input.dbus, kPowerManagerServiceName,
			kPowerManagerServicePath,
			kPowerManagerInterface,
			kHandleUserActivityMethod,
			DBUS_TYPE_INT32, &activity_type);

	switch (activity_type) {
		case USER_ACTIVITY_BRIGHTNESS_UP_KEY_PRESS:
				(void)dbus_method_call0(input.dbus,
					kPowerManagerServiceName,
					kPowerManagerServicePath,
					kPowerManagerInterface,
					kIncreaseScreenBrightnessMethod);
				break;
		case USER_ACTIVITY_BRIGHTNESS_DOWN_KEY_PRESS:
				/*
				 * Shouldn't allow the screen to go
				 * completely off while frecon is active
				 * so passing false to allow_off
				 */
				(void)dbus_method_call1(input.dbus,
					kPowerManagerServiceName,
					kPowerManagerServicePath,
					kPowerManagerInterface,
					kDecreaseScreenBrightnessMethod,
					DBUS_TYPE_BOOLEAN, &allow_off);
				break;
	}
}

static int input_special_key(struct input_key_event *ev)
{
	unsigned int i;
	terminal_t *terminal;

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

	terminal = input.terminals[input.current_terminal];

	for (i = 0; i < ARRAY_SIZE(ignore_keys); i++)
		if (ev->code == ignore_keys[i])
			return 1;

	switch (ev->code) {
	case KEY_LEFTSHIFT:
	case KEY_RIGHTSHIFT:
		input.kbd_state.shift_state = ! !ev->value;
		return 1;
	case KEY_LEFTCTRL:
	case KEY_RIGHTCTRL:
		input.kbd_state.control_state = ! !ev->value;
		return 1;
	case KEY_LEFTALT:
	case KEY_RIGHTALT:
		input.kbd_state.alt_state = ! !ev->value;
		return 1;
	case KEY_LEFTMETA: // search key
		input.kbd_state.search_state = ! !ev->value;
		return 1;
	}

	if (term_is_active(terminal)) {
		if (input.kbd_state.shift_state && ev->value) {
			switch (ev->code) {
			case KEY_PAGEUP:
				term_page_up(input.terminals[input.current_terminal]);
				return 1;
			case KEY_PAGEDOWN:
				term_page_down(input.terminals[input.current_terminal]);
				return 1;
			case KEY_UP:
				term_line_up(input.terminals[input.current_terminal]);
				return 1;
			case KEY_DOWN:
				term_line_down(input.terminals[input.current_terminal]);
				return 1;
			}
		}

		if (input.kbd_state.search_state && ev->value) {
			switch (ev->code) {
				case KEY_UP:
					term_page_up(input.terminals[input.current_terminal]);
					return 1;
				case KEY_DOWN:
					term_page_down(input.terminals[input.current_terminal]);
					return 1;
			}
		}

		if (!(input.kbd_state.search_state || input.kbd_state.alt_state ||
					input.kbd_state.control_state) &&
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
					report_user_activity(USER_ACTIVITY_BRIGHTNESS_DOWN_KEY_PRESS -
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

	if (input.kbd_state.alt_state && input.kbd_state.control_state && ev->value) {
		/*
		 * Special case for key sequence that is used by external program.   Just
		 * explicitly ignore here and do nothing.
		 */
		if (input.kbd_state.shift_state)
			return 1;

		if (ev->code == KEY_F1) {
			if (term_is_active(terminal)) {
				term_deactivate(terminal);
				if (input.terminals[SPLASH_TERMINAL] != NULL) {
					term_activate(input.terminals[SPLASH_TERMINAL]);
				} else {
					if (input.dbus != NULL)
						(void)dbus_method_call0(input.dbus,
							kLibCrosServiceName,
							kLibCrosServicePath,
							kLibCrosServiceInterface,
							kTakeDisplayOwnership);
				}
			}
		} else if ((ev->code >= KEY_F2) && (ev->code < KEY_F2 + MAX_STD_TERMINALS)) {
			if (input.dbus != NULL)
				(void)dbus_method_call0(input.dbus,
					kLibCrosServiceName,
					kLibCrosServicePath,
					kLibCrosServiceInterface,
					kReleaseDisplayOwnership);
			if (term_is_active(terminal))
				term_deactivate(terminal);
			input.current_terminal = ev->code - KEY_F2;
			terminal = input.terminals[input.current_terminal];
			if (terminal == NULL) {
				input.terminals[input.current_terminal] =
					term_init(true, NULL);
				terminal =
					input.terminals[input.current_terminal];
				term_activate(terminal);
				if (!term_is_valid(terminal)) {
					LOG(ERROR, "Term init failed");
					return 1;
				}
			}
			term_activate(input.terminals[input.current_terminal]);
		}

		return 1;

	}

	return 0;
}

static void input_get_keysym_and_unicode(struct input_key_event *event,
		uint32_t *keysym, uint32_t *unicode)
{
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
		*keysym = keysym_table[event->code * 2 + input.kbd_state.shift_state];
		if ((input.kbd_state.control_state) && isascii(*keysym))
			*keysym = tolower(*keysym) - 'a' + 1;
	}

	*unicode = *keysym;
}

static int input_add(const char *devname)
{
	int ret = 0, fd = -1;
	/* for some reason every device has a null enumerations and notifications
	   of every device come with NULL string first */
	if (!devname) {
		ret = -EINVAL;
		goto errorret;
	}
	ret = fd = open(devname, O_RDONLY);
	if (fd < 0)
		goto errorret;

	struct input_dev *newdevs =
	    realloc(input.devs, (input.ndevs + 1) * sizeof (struct input_dev));
	if (!newdevs) {
		ret = -ENOMEM;
		goto closefd;
	}
	input.devs = newdevs;
	input.devs[input.ndevs].fd = fd;
	input.devs[input.ndevs].path = strdup(devname);
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

static void input_remove(const char *devname)
{
	if (!devname) {
		return;
	}
	unsigned int u;
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
	input.udev = udev_new();
	if (!input.udev)
		return -ENOENT;
	input.udev_monitor = udev_monitor_new_from_netlink(input.udev, "udev");
	if (!input.udev_monitor) {
		udev_unref(input.udev);
		return -ENOENT;
	}
	udev_monitor_filter_add_match_subsystem_devtype(input.udev_monitor, "input",
							NULL);
	udev_monitor_enable_receiving(input.udev_monitor);
	input.udev_fd = udev_monitor_get_fd(input.udev_monitor);

	struct udev_enumerate *udev_enum;
	struct udev_list_entry *devices, *deventry;
	udev_enum = udev_enumerate_new(input.udev);
	udev_enumerate_add_match_subsystem(udev_enum, "input");
	udev_enumerate_scan_devices(udev_enum);
	devices = udev_enumerate_get_list_entry(udev_enum);
	udev_list_entry_foreach(deventry, devices) {
		const char *syspath;
		struct udev_device *dev;
		syspath = udev_list_entry_get_name(deventry);
		dev = udev_device_new_from_syspath(input.udev, syspath);
		input_add(udev_device_get_devnode(dev));
		udev_device_unref(dev);
	}
	udev_enumerate_unref(udev_enum);

	if (!isatty(fileno(stdout)))
		setbuf(stdout, NULL);

	if (input.ndevs == 0) {
		LOG(ERROR, "No valid inputs for terminal");
		exit(EXIT_SUCCESS);
	}

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

	udev_monitor_unref(input.udev_monitor);
	input.udev_monitor = NULL;
	udev_unref(input.udev);
	input.udev = NULL;
	input.udev_fd = -1;

	dbus_destroy(input.dbus);

}

void input_set_dbus(dbus_t* dbus)
{
	input.dbus = dbus;
}

int input_setfds(fd_set * read_set, fd_set * exception_set)
{
	unsigned int u;
	int max = -1;
	for (u = 0; u < input.ndevs; u++) {
		FD_SET(input.devs[u].fd, read_set);
		FD_SET(input.devs[u].fd, exception_set);
		if (input.devs[u].fd > max)
			max = input.devs[u].fd;
	}

	FD_SET(input.udev_fd, read_set);
	FD_SET(input.udev_fd, exception_set);
	if (input.udev_fd > max)
		max = input.udev_fd;
	return max;
}

struct input_key_event *input_get_event(fd_set * read_set,
					fd_set * exception_set)
{
	unsigned int u;
	struct input_event ev;
	int ret;

	if (FD_ISSET(input.udev_fd, exception_set)) {
		/* udev died on us? */
		LOG(ERROR, "Exception on udev fd");
	}

	if (FD_ISSET(input.udev_fd, read_set)
	    && !FD_ISSET(input.udev_fd, exception_set)) {
		/* we got an udev notification */
		struct udev_device *dev =
		    udev_monitor_receive_device(input.udev_monitor);
		if (dev) {
			if (!strcmp("add", udev_device_get_action(dev))) {
				input_add(udev_device_get_devnode(dev));
			} else
			    if (!strcmp("remove", udev_device_get_action(dev)))
			{
				input_remove(udev_device_get_devnode(dev));
			}
			udev_device_unref(dev);
		}
	}

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
				struct input_key_event *event =
				    malloc(sizeof (*event));
				event->code = ev.code;
				event->value = ev.value;
				return event;
			}
		}
	}

	return NULL;
}

int input_process(terminal_t* splash_term, uint32_t usec)
{
	terminal_t *terminal;
	terminal_t *new_terminal;
	fd_set read_set, exception_set;
	int maxfd;
	int sstat;
	struct timeval tm;
	struct timeval *ptm;

	terminal = input.terminals[input.current_terminal];

	FD_ZERO(&read_set);
	FD_ZERO(&exception_set);

	if (input.dbus) {
		dbus_add_fd(input.dbus, &read_set, &exception_set);
		maxfd = dbus_get_fd(input.dbus) + 1;
	} else {
		maxfd = 0;
	}

	maxfd = MAX(maxfd, input_setfds(&read_set, &exception_set)) + 1;

	for (int i = 0; i < MAX_TERMINALS; i++) {
		if (term_is_valid(input.terminals[i])) {
			term_add_fd(input.terminals[i], &read_set, &exception_set);
			maxfd = MAX(maxfd, term_fd(input.terminals[i])) + 1;
			term_dispatch_io(input.terminals[i], &read_set);
		}
	}

	if (usec) {
		ptm = &tm;
		tm.tv_sec = 0;
		tm.tv_usec = usec;
	} else
		ptm = NULL;

	sstat = select(maxfd, &read_set, NULL, &exception_set, ptm);
	if (sstat == 0)
		return 0;


	if (input.dbus)
		dbus_dispatch_io(input.dbus);

	if (term_exception(terminal, &exception_set))
		return -1;

	struct input_key_event *event;
	event = input_get_event(&read_set, &exception_set);
	if (event) {
		if (!input_special_key(event) && event->value) {
			uint32_t keysym, unicode;
			// current_terminal can possibly change during
			// execution of input_special_key
			terminal = input.terminals[input.current_terminal];
			if (term_is_active(terminal)) {
				// Only report user activity when the terminal is active
				report_user_activity(USER_ACTIVITY_OTHER);
				input_get_keysym_and_unicode(
					event, &keysym, &unicode);
				term_key_event(terminal,
						keysym, unicode);
			}
		}
		input_put_event(event);
	}

	for (int i = 0; i < MAX_TERMINALS; i++) {
		if (term_is_valid(input.terminals[i])) {
			term_add_fd(input.terminals[i], &read_set, &exception_set);
			term_dispatch_io(input.terminals[i], &read_set);
		}
	}

	if (term_is_valid(terminal)) {
		if (term_is_child_done(terminal)) {
			if (terminal == input.terminals[SPLASH_TERMINAL]) {
				/*
				 * Note: reference is not lost because it is still referenced
				 * by the splash_t structure which will ultimately destroy
				 * it, once it's safe to do so
				 */
				input.terminals[SPLASH_TERMINAL] = NULL;
				return -1;
			}
			input.terminals[input.current_terminal] = term_init(true, NULL);
			new_terminal = input.terminals[input.current_terminal];
			if (!term_is_valid(new_terminal)) {
				return -1;
			}
			term_close(terminal);
			term_activate(new_terminal);
		}
	}

	return 0;
}

int input_run(bool standalone)
{
	terminal_t* terminal;
	int status;

	if (standalone) {
		if (input.dbus) {
			(void)dbus_method_call0(input.dbus,
				kLibCrosServiceName,
				kLibCrosServicePath,
				kLibCrosServiceInterface,
				kReleaseDisplayOwnership);
		}

		input.terminals[input.current_terminal] = term_init(true, NULL);
		terminal = input.terminals[input.current_terminal];
		term_activate(terminal);
	}

	while (1) {
		status = input_process(NULL, 0);
		if (status != 0) {
			LOG(ERROR, "input process returned %d", status);
			break;
		}
	}

	return 0;
}

void input_put_event(struct input_key_event *event)
{
	free(event);
}

terminal_t* input_create_term(int vt)
{
	terminal_t* terminal;

	if (vt == 0)
		return input.terminals[input.current_terminal];

	terminal = input.terminals[vt-1];
	if (term_is_active(terminal))
		return terminal;

	if (terminal == NULL) {
		input.terminals[vt-1] = term_init(false, NULL);
		terminal = input.terminals[vt-1];
		if (!term_is_valid(terminal)) {
			LOG(ERROR, "create_term: Term init failed");
		}
	}

	return terminal;
}

terminal_t* input_create_splash_term(video_t* video)
{
	input.terminals[SPLASH_TERMINAL] = term_init(false, video);
	return input.terminals[SPLASH_TERMINAL];
}

void input_destroy_splash_term()
{
	input.terminals[SPLASH_TERMINAL] = NULL;
}

void input_set_current(terminal_t* terminal)
{
	int i;

	if (!terminal) {
		input.terminals[input.current_terminal] = NULL;
		input.current_terminal = 0;
		return;
	}

	for (i = 0; i < MAX_TERMINALS; i++) {
		if (terminal == input.terminals[i]) {
			input.current_terminal = i;
			return;
		}
	}
}

unsigned int input_get_maxterminals()
{
	return MAX_STD_TERMINALS;
}
