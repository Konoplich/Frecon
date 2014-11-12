/*
 * Copyright (c) 2014 The Chromium OS Authors. All rights reserved.
 * Use of this source code is governed by a BSD-style license that can be
 * found in the LICENSE file.
 */

#include <ctype.h>
#include <fcntl.h>
#include <libtsm.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/select.h>
#include <errno.h>
#include <libudev.h>
#include "input.h"
#include "dbus_interface.h"
#include "dbus.h"
#include "keysym.h"
#include "shl_pty.h"
#include "util.h"

#define MAX_TERMINALS    (5)

struct input_dev {
	int fd;
	char *path;
};

struct keyboard_state {
	int shift_state;
	int control_state;
	int alt_state;
};

struct {
	struct udev *udev;
	struct udev_monitor *udev_monitor;
	int udev_fd;
	unsigned int ndevs;
	struct input_dev *devs;
	struct keyboard_state kbd_state;
	dbus_t *dbus;
	video_t  *video;
	uint32_t  current_terminal;
	terminal_t *terminals[MAX_TERMINALS];
} input = {
	.udev = NULL,
	.udev_monitor = NULL,
	.udev_fd = -1,
	.ndevs = 0,
	.devs = NULL,
	.dbus = NULL,
	.video = NULL,
	.current_terminal = 0
};

static int input_special_key(struct input_key_event *ev);
static void input_get_keysym_and_unicode(struct input_key_event *event,
		uint32_t *keysym, uint32_t *unicode);

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

	ret = ioctl(fd, EVIOCGRAB, (void *) 1);

	if (!ret) {
		ioctl(fd, EVIOCGRAB, (void *) 0);
	} else {
		LOG(ERROR, "Evdev device %s grabbed by another process",
			devname);
		ret = -EBUSY;
		goto closefd;
	}

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

static void report_user_activity(void)
{
	int activity_type = USER_ACTIVITY_OTHER;

	dbus_method_call1(input.dbus, kPowerManagerServiceName,
			kPowerManagerServicePath,
			kPowerManagerInterface,
			kHandleUserActivityMethod,
			&activity_type);

	(void)dbus_message_new_method_call(kPowerManagerServiceName,
					   kPowerManagerServicePath,
					   kPowerManagerInterface,
					   kHandleUserActivityMethod);

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
			if (ret < (int) sizeof (struct input_event)) {
				LOG(ERROR, "expected %d bytes, got %d",
				       (int) sizeof (struct input_event), ret);
				return NULL;
			}

			if (ev.type == EV_KEY) {
				struct input_key_event *event =
				    malloc(sizeof (*event));
				event->code = ev.code;
				event->value = ev.value;
				report_user_activity();
				return event;
			}
		}
	}

	return NULL;
}

int input_run(video_t *video, bool standalone)
{
	fd_set read_set, exception_set;
	int pty_fd = -1;

	input.video = video;
	if (standalone) {
		(void)dbus_method_call0(input.dbus,
			kLibCrosServiceName,
			kLibCrosServicePath,
			kLibCrosServiceInterface,
			kReleaseDisplayOwnership);

		input.terminals[input.current_terminal] = term_init();
		input.terminals[input.current_terminal]->active = true;
		input_grab();
		video_setmode(input.terminals[input.current_terminal]->video);
		term_redraw(input.terminals[input.current_terminal]);
	}
	
	while (1) {
		if (input.terminals[input.current_terminal] &&
				input.terminals[input.current_terminal]->term)
			pty_fd = input.terminals[input.current_terminal]->term->pty_bridge;

		FD_ZERO(&read_set);
		FD_ZERO(&exception_set);
		if (pty_fd >= 0) {
			FD_SET(pty_fd, &read_set);
			FD_SET(pty_fd, &exception_set);
		}
		int maxfd = input_setfds(&read_set, &exception_set);

		maxfd = MAX(maxfd, pty_fd) + 1;

		select(maxfd, &read_set, NULL, &exception_set, NULL);

		if (pty_fd >= 0) {
			if (FD_ISSET(pty_fd, &exception_set)) {
				return -1;
			}
		}

		struct input_key_event *event;
		event = input_get_event(&read_set, &exception_set);
		if (event) {
			if (!input_special_key(event) && event->value) {
				uint32_t keysym, unicode;
				if (input.terminals[input.current_terminal] &&
						input.terminals[input.current_terminal]->active) {
					input_get_keysym_and_unicode(event, &keysym, &unicode);
					term_key_event(input.terminals[input.current_terminal],
							keysym, unicode);
				}
			}

			input_put_event(event);
		}

		if (pty_fd >= 0) {
			if (FD_ISSET(pty_fd, &read_set)) {
				shl_pty_bridge_dispatch(input.terminals[input.current_terminal]->
						term->pty_bridge, 0);
			}
		}

		if (input.terminals[input.current_terminal]) {
			if (term_is_child_done(input.terminals[input.current_terminal])) {
				term_close(input.terminals[input.current_terminal]);
				input.terminals[input.current_terminal] = term_init();
				input.terminals[input.current_terminal]->active = true;
				video_setmode(input.terminals[input.current_terminal]->video);
				term_redraw(input.terminals[input.current_terminal]);
			}
		}
	}
}

void input_put_event(struct input_key_event *event)
{
	free(event);
}

void input_grab()
{
	unsigned int i;
	for (i = 0; i < input.ndevs; i++) {
		(void)ioctl(input.devs[i].fd, EVIOCGRAB, (void *) 1);
	}
}

void input_ungrab()
{
	unsigned int i;
	for (i = 0; i < input.ndevs; i++) {
		(void)ioctl(input.devs[i].fd, EVIOCGRAB, (void*) 0);
	}
}

static int input_special_key(struct input_key_event *ev)
{
	unsigned int i;

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
	}

	if (input.kbd_state.shift_state && ev->value) {
		switch (ev->code) {
		case KEY_PAGEUP:
			tsm_screen_sb_page_up(input.terminals[input.current_terminal]->
					term->screen, 1);
			term_redraw(input.terminals[input.current_terminal]);
			return 1;
		case KEY_PAGEDOWN:
			tsm_screen_sb_page_down(input.terminals[input.current_terminal]->
					term->screen, 1);
			term_redraw(input.terminals[input.current_terminal]);
			return 1;
		case KEY_UP:
			tsm_screen_sb_up(input.terminals[input.current_terminal]->
					term->screen, 1);
			term_redraw(input.terminals[input.current_terminal]);
			return 1;
		case KEY_DOWN:
			tsm_screen_sb_down(input.terminals[input.current_terminal]->
					term->screen, 1);
			term_redraw(input.terminals[input.current_terminal]);
			return 1;
		}
	}

	if (input.kbd_state.alt_state && input.kbd_state.control_state && ev->value) {
		switch (ev->code) {
			case KEY_F1:
				input_ungrab();
				input.terminals[input.current_terminal]->active = false;
				(void)dbus_method_call0(input.dbus,
					kLibCrosServiceName,
					kLibCrosServicePath,
					kLibCrosServiceInterface,
					kTakeDisplayOwnership);
				break;
			case KEY_F2:
			case KEY_F3:
			case KEY_F4:
			case KEY_F5:
			case KEY_F6:
			case KEY_F7:
			case KEY_F8:
			case KEY_F9:
			case KEY_F10:
				(void)dbus_method_call0(input.dbus,
					kLibCrosServiceName,
					kLibCrosServicePath,
					kLibCrosServiceInterface,
					kReleaseDisplayOwnership);
				break;
		}

		if (ev->code == KEY_F2) {
			input.current_terminal = 0;
			if (input.terminals[input.current_terminal] == NULL) {
				input.terminals[input.current_terminal] = term_init();
				if (input.terminals[input.current_terminal] == NULL) {
					LOG(ERROR, "Term init failed");
          return 1;
				}
      }
			input.terminals[input.current_terminal]->active = true;
			input_grab();
			video_setmode(input.terminals[input.current_terminal]->video);
			term_redraw(input.terminals[input.current_terminal]);
		}

		if (ev->code == KEY_F3) {
			input.current_terminal = 1;
			if (input.terminals[input.current_terminal] == NULL) {
				input.terminals[input.current_terminal] = term_init();
				if (input.terminals[input.current_terminal] == NULL) {
					LOG(ERROR, "Term init failed");
          return 1;
				}
      }
			input.terminals[input.current_terminal]->active = true;
			input_grab();
			video_setmode(input.terminals[input.current_terminal]->video);
			term_redraw(input.terminals[input.current_terminal]);
		}

		if (ev->code == KEY_F4) {
			input.current_terminal = 2;
			if (input.terminals[input.current_terminal] == NULL) {
				input.terminals[input.current_terminal] = term_init();
				if (input.terminals[input.current_terminal] == NULL) {
					LOG(ERROR, "Term init failed");
          return 1;
				}
      }
			input.terminals[input.current_terminal]->active = true;
			input_grab();
			video_setmode(input.terminals[input.current_terminal]->video);
			term_redraw(input.terminals[input.current_terminal]);
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

