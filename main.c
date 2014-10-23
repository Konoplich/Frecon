/*
 * Copyright (c) 2014 The Chromium OS Authors. All rights reserved.
 * Use of this source code is governed by a BSD-style license that can be
 * found in the LICENSE file.
 */

#include <libtsm.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <memory.h>
#include <getopt.h>
#include <stdbool.h>
#include <fcntl.h>

#include "input.h"
#include "term.h"
#include "video.h"
#include "dbus.h"
#include "util.h"
#include "splash.h"

#define	 FLAG_CLEAR												'c'
#define	 FLAG_DAEMON											'd'
#define	 FLAG_DEV_MODE										'e'
#define	 FLAG_FRAME_INTERVAL							'f'
#define	 FLAG_GAMMA												'g'
#define	 FLAG_PRINT_RESOLUTION						'p'

static struct option command_options[] = {
	{ "clear", required_argument, NULL, FLAG_CLEAR },
	{ "daemon", no_argument, NULL, FLAG_DAEMON },
	{ "dev-mode", no_argument, NULL, FLAG_DEV_MODE },
	{ "frame-interval", required_argument, NULL, FLAG_FRAME_INTERVAL },
	{ "gamma", required_argument, NULL, FLAG_GAMMA },
	{ "print-resolution", no_argument, NULL, FLAG_PRINT_RESOLUTION },
	{ NULL, 0, NULL, 0 }
};

typedef struct {
		bool	 print_resolution;
		bool	 frame_interval;
} commandflags_t;

static void daemonize();

int main(int argc, char* argv[])
{
	int32_t width, height, pitch, scaling;
	int ret;
	int c;
	int i;
	commandflags_t flags;
	splash_t *splash;
	video_t  *video;
	terminal_t *terminal;
	dbus_t *dbus;

	memset(&flags, 0, sizeof(flags));

	video = video_init(&width, &height, &pitch, &scaling);
	if (video == NULL) {
		LOG(ERROR, "Video init failed");
		return EXIT_FAILURE;
	}


	ret = input_init();
	if (ret) {
		LOG(ERROR, "Input init failed");
		video_close(video);
		return EXIT_FAILURE;
	}

	terminal = term_init(video);
	if (ret) {
		LOG(ERROR, "Term init failed");
		input_close();
		video_close(video);
		return EXIT_FAILURE;
	}

	splash = splash_init(video);
	if (splash == NULL) {
		LOG(ERROR, "splash init failed");
		return EXIT_FAILURE;
	}

	for (;;) {
		c = getopt_long(argc, argv, "", command_options, NULL);
		if (c == -1)
			break;

		if (c == FLAG_CLEAR) {
			splash_set_clear(splash, strtoul(optarg, NULL, 0));
		}

		if (c == FLAG_DAEMON) {
			daemonize();
		}

		if (c == FLAG_DEV_MODE) {
			splash_set_devmode(splash);
		}

		if (c == FLAG_PRINT_RESOLUTION) {
			flags.print_resolution = true;
		}

		if (c == FLAG_GAMMA) {
			video_set_gamma(video, optarg);
		}

		if (c == FLAG_FRAME_INTERVAL) {
			splash_set_frame_rate(splash, strtoul(optarg, NULL, 0));
			for (i = optind; i < argc; i++) {
				 splash_add_image(splash, argv[i]);
			}
			flags.frame_interval = true;
		}
	}

	/*
	 * The DBUS service launches later than the boot-splash service, and
	 * as a result, when splash_run starts dbus is not yet up, but, by
	 * the time splash_run completes, it is running.  At the same time,
	 * splash_run needs dbus to determine when chrome is visible.  So,
	 * it creates the dbus object and then passes it back to the caller
	 * who can then pass it to the other objects that need it
	 */
	dbus = NULL;
	if (flags.print_resolution) {
		printf("%d %d", video_getwidth(video), video_getheight(video));
		return EXIT_SUCCESS;
	}
	else if (flags.frame_interval) {
		ret = splash_run(splash, &dbus);
		if (ret) {
				LOG(ERROR, "splash_run failed: %d", ret);
				return EXIT_FAILURE;
		}
	}

	/*
	 * If splash_run didn't create the dbus object (for example, if
	 * we didn't supply the frame-interval parameter, then go ahead
	 * and create it now
	 */
	if (dbus == NULL) {
		dbus = dbus_init();
	}

	input_set_dbus(dbus);
	term_set_dbus(terminal, dbus);

	ret = term_run(terminal);

	input_close();
	term_close(terminal);
	video_close(video);

	return ret;
}

static void daemonize()
{
	pid_t pid;
	int fd;

	pid = fork();
	if (pid == -1)
		return;
	else if (pid != 0)
		exit(EXIT_SUCCESS);

	if (setsid() == -1)
		return;

	// Re-direct stderr/stdout to the system message log
	close(0);
	close(1);
	close(2);

	open("/dev/kmsg", O_RDWR);

	fd = dup(0);
	if (fd != STDOUT_FILENO) {
		close(fd);
		return;
	}
	fd = dup(0);
	if (fd != STDERR_FILENO) {
		close(fd);
		return;
	}
}

#ifdef __clang__
__attribute__((format (__printf__, 2, 0)))
#endif
void LOG(int severity, const char* fmt, ...)
{
	va_list arg_list;
	fprintf(stderr, "frecon: ");
	va_start( arg_list, fmt);
	vfprintf(stderr, fmt, arg_list);
	va_end(arg_list);
	fprintf(stderr, "\n");
}

