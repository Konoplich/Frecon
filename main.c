/*
 * Copyright 2014 The Chromium OS Authors. All rights reserved.
 * Use of this source code is governed by a BSD-style license that can be
 * found in the LICENSE file.
 */

#include <assert.h>
#include <errno.h>
#include <fcntl.h>
#include <getopt.h>
#include <libtsm.h>
#include <memory.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <sys/stat.h>
#include <sys/types.h>

#include "dbus.h"
#include "dbus_interface.h"
#include "dev.h"
#include "input.h"
#include "main.h"
#include "splash.h"
#include "term.h"
#include "util.h"

#define  FLAG_CLEAR                        'c'
#define  FLAG_DAEMON                       'd'
#define  FLAG_ENABLE_GFX                   'G'
#define  FLAG_ENABLE_VT1                   '1'
#define  FLAG_ENABLE_VTS                   'e'
#define  FLAG_FRAME_INTERVAL               'f'
#define  FLAG_HELP                         'h'
#define  FLAG_IMAGE                        'i'
#define  FLAG_IMAGE_HIRES                  'I'
#define  FLAG_LOOP_COUNT                   'C'
#define  FLAG_LOOP_START                   'l'
#define  FLAG_LOOP_INTERVAL                'L'
#define  FLAG_LOOP_OFFSET                  'o'
#define  FLAG_NUM_VTS                      'N'
#define  FLAG_NO_LOGIN                     'n'
#define  FLAG_OFFSET                       'O'
#define  FLAG_PRE_CREATE_VTS               'P'
#define  FLAG_PRINT_RESOLUTION             'p'
#define  FLAG_SCALE                        'S'
#define  FLAG_SPLASH_ONLY                  's'

static const struct option command_options[] = {
	{ "clear", required_argument, NULL, FLAG_CLEAR },
	{ "daemon", no_argument, NULL, FLAG_DAEMON },
	{ "dev-mode", no_argument, NULL, FLAG_ENABLE_VTS },
	{ "enable-gfx", no_argument, NULL, FLAG_ENABLE_GFX },
	{ "enable-vt1", no_argument, NULL, FLAG_ENABLE_VT1 },
	{ "enable-vts", no_argument, NULL, FLAG_ENABLE_VTS },
	{ "frame-interval", required_argument, NULL, FLAG_FRAME_INTERVAL },
	{ "help", no_argument, NULL, FLAG_HELP },
	{ "image", required_argument, NULL, FLAG_IMAGE },
	{ "image-hires", required_argument, NULL, FLAG_IMAGE_HIRES },
	{ "loop-count", required_argument, NULL, FLAG_LOOP_COUNT },
	{ "loop-start", required_argument, NULL, FLAG_LOOP_START },
	{ "loop-interval", required_argument, NULL, FLAG_LOOP_INTERVAL },
	{ "loop-offset", required_argument, NULL, FLAG_LOOP_OFFSET },
	{ "num-vts", required_argument, NULL, FLAG_NUM_VTS },
	{ "no-login", no_argument, NULL, FLAG_NO_LOGIN },
	{ "offset", required_argument, NULL, FLAG_OFFSET },
	{ "print-resolution", no_argument, NULL, FLAG_PRINT_RESOLUTION },
	{ "pre-create-vts", no_argument, NULL, FLAG_PRE_CREATE_VTS },
	{ "scale", required_argument, NULL, FLAG_SCALE },
	{ "splash-only", no_argument, NULL, FLAG_SPLASH_ONLY },
	{ NULL, 0, NULL, 0 }
};
static const char * const command_help[] = {
	"Splash screen clear color.",
	"Daemonize frecon.",
	"Force dev mode behavior (same as --enable-vts).",
	"Enable image and box drawing OSC escape codes.",
	"Enable switching to VT1 and keep a terminal on it.",
	"Enable additional terminals beyond VT1.",
	"Default time (in msecs) between splash animation frames.",
	"This help screen!",
	"Image (low res) to use for splash animation.",
	"Image (hi res) to use for splash animation.",
	"Number of times to loop splash animations (0 = forever).",
	"First frame to start the splash animation loop (and enable looping).",
	"Pause time (in msecs) between splash animation frames.",
	"Offset (as x,y) for centering looped image.",
	"Number of enabled VTs. The default is 4, the maximum is 12.",
	"Do not display login prompt on additional VTs.",
	"Absolute location of the splash image on screen (as x,y).",
	"(Deprecated) Print detected screen resolution and exit.",
	"Create all VTs immediately instead of on-demand.",
	"Default scale for splash screen images.",
	"Exit immediately after finishing splash animation.",
};

static void usage(int status)
{
	FILE *out = status ? stderr : stdout;

	static_assert(ARRAY_SIZE(command_help) == ARRAY_SIZE(command_options) - 1,
		"The help & option arrays need resyncing");

	fprintf(out,
		"Frecon: The Freon based console daemon.\n"
		"\n"
		"Usage: frecon [options] [splash images]\n"
		"\n"
		"Options:\n"
	);

	/* Output all the options & help text, and auto-align them. */
	int len;
	for (int i = 0; command_options[i].name; ++i) {
		len = fprintf(out, "  -%c, --%s ",
			command_options[i].val, command_options[i].name);
		if (command_options[i].has_arg == required_argument)
			len += fprintf(out, "<arg> ");
		fprintf(out, "%*s %s\n", (30 - len), "", command_help[i]);
	}

	fprintf(out, "\nFor more detailed documentation, visit:\n"
		"https://chromium.googlesource.com/chromiumos/platform/frecon/+/master\n");

	exit(status);
}

commandflags_t command_flags = { 0 };

static void parse_offset(char* param, int32_t* x, int32_t* y)
{
	char* token;
	char* saveptr;

	token = strtok_r(param, ",", &saveptr);
	if (token)
		*x = strtol(token, NULL, 0);

	token = strtok_r(NULL, ",", &saveptr);
	if (token)
		*y = strtol(token, NULL, 0);
}

int main_process_events(uint32_t usec)
{
	terminal_t* terminal;
	terminal_t* new_terminal;
	fd_set read_set, exception_set;
	int maxfd = -1;
	int sstat;
	struct timeval tm;
	struct timeval* ptm;

	terminal = term_get_current_terminal();

	FD_ZERO(&read_set);
	FD_ZERO(&exception_set);

	dbus_add_fds(&read_set, &exception_set, &maxfd);
	input_add_fds(&read_set, &exception_set, &maxfd);
	dev_add_fds(&read_set, &exception_set, &maxfd);

	for (unsigned i = 0; i < term_num_terminals; i++) {
		terminal_t* current_term = term_get_terminal(i);
		if (term_is_valid(current_term))
			term_add_fds(current_term, &read_set, &exception_set, &maxfd);
	}

	if (usec) {
		ptm = &tm;
		tm.tv_sec = 0;
		tm.tv_usec = usec;
	} else
		ptm = NULL;

	sstat = select(maxfd + 1, &read_set, NULL, &exception_set, ptm);
	if (sstat == 0)
		return 0;

	dbus_dispatch_io();

	if (term_exception(terminal, &exception_set))
		return -1;

	dev_dispatch_io(&read_set, &exception_set);
	input_dispatch_io(&read_set, &exception_set);

	for (unsigned i = 0; i < term_num_terminals; i++) {
		terminal_t* current_term = term_get_terminal(i);
		if (term_is_valid(current_term))
			term_dispatch_io(current_term, &read_set);
	}

	/* Could have changed in input dispatch. */
	terminal = term_get_current_terminal();

	/* Restart terminal on which child has exited. We don't want possible garbage settings from previous session to remain. */
	if (term_is_valid(terminal)) {
		if (term_is_child_done(terminal)) {
			if (terminal == term_get_terminal(TERM_SPLASH_TERMINAL) && !command_flags.enable_vt1) {
				/* Let the old term be, splash_destroy will clean it up. */
				return 0;
			}
			term_set_current_terminal(term_init(term_get_current(), -1));
			new_terminal = term_get_current_terminal();
			if (!term_is_valid(new_terminal)) {
				return -1;
			}
			term_activate(new_terminal);
			term_close(terminal);
		}
	}

	return 0;
}

int main_loop(void)
{
	int status;

	while (1) {
		status = main_process_events(0);
		if (status != 0) {
			LOG(ERROR, "Input process returned %d.", status);
			break;
		}
	}

	return 0;
}

bool set_drm_master_relax(void)
{
	int fd;
	int num_written;

	/*
	 * Setting drm_master_relax flag in kernel allows us to transfer DRM master
	 * between Chrome and frecon.
	 */
	fd = open("/sys/kernel/debug/dri/drm_master_relax", O_WRONLY);
	if (fd != -1) {
		num_written = write(fd, "Y", 1);
		close(fd);
		if (num_written != 1) {
			LOG(ERROR, "Unable to set drm_master_relax.");
			return false;
		}
	} else {
		LOG(ERROR, "Unable to open drm_master_relax.");
		return false;
	}
	return true;
}

static void main_on_login_prompt_visible(void* ptr)
{
	if (command_flags.daemon && !command_flags.enable_vts) {
		LOG(INFO, "Chrome started, our work is done, exiting.");
		exit(EXIT_SUCCESS);
	} else
	if (ptr) {
		LOG(INFO, "Chrome started, splash screen is not needed anymore.");
		if (command_flags.enable_vt1)
			LOG(WARNING, "VT1 enabled and Chrome is active!");
		splash_destroy((splash_t*)ptr);
	}
}

static void legacy_print_resolution(int argc, char* argv[])
{
	int c;

	optind = 1;
	opterr = 0;
	for (;;) {
		c = getopt_long(argc, argv, "", command_options, NULL);
		if (c == -1) {
			break;
		} else if (c == FLAG_PRINT_RESOLUTION) {
			drm_t *drm = drm_scan();
			if (!drm)
				exit(EXIT_FAILURE);

			printf("%d %d", drm_gethres(drm),
			       drm_getvres(drm));
			drm_delref(drm);
			exit(EXIT_SUCCESS);
		}
	}
}

int main(int argc, char* argv[])
{
	int ret;
	int c;
	int pts_fd;
	int32_t x, y;
	splash_t* splash;
	drm_t* drm;

	legacy_print_resolution(argc, argv);

	fix_stdio();
	pts_fd =  posix_openpt(O_RDWR | O_NOCTTY | O_CLOEXEC | O_NONBLOCK);

	optind = 1;
	opterr = 1;
	for (;;) {
		c = getopt_long(argc, argv, "", command_options, NULL);

		if (c == -1)
			break;

		switch (c) {
			case FLAG_DAEMON:
				command_flags.daemon = true;
				break;

			case FLAG_ENABLE_GFX:
				command_flags.enable_gfx = true;
				break;

			case FLAG_ENABLE_VT1:
				command_flags.enable_vt1 = true;
				break;

			case FLAG_ENABLE_VTS:
				command_flags.enable_vts = true;;
				break;

			case FLAG_NO_LOGIN:
				command_flags.no_login = true;
				break;

			case FLAG_NUM_VTS:
				term_set_num_terminals(strtoul(optarg, NULL, 0));
				break;

			case FLAG_PRE_CREATE_VTS:
				command_flags.pre_create_vts = true;
				break;

			case FLAG_SPLASH_ONLY:
				command_flags.splash_only = true;
				break;

			case FLAG_HELP:
				usage(0);
				break;

			case '?':
				usage(1);
				break;
		}
	}

	ret = input_init();
	if (ret) {
		LOG(ERROR, "Input init failed.");
		return EXIT_FAILURE;
	}

	ret = dev_init();
	if (ret) {
		LOG(ERROR, "Device management init failed.");
		return EXIT_FAILURE;
	}

	drm_set(drm = drm_scan());

	/* Create VTs before daemonizing */
	if (command_flags.pre_create_vts && command_flags.enable_vts)
		for (unsigned vt = 1; vt < term_num_terminals; vt++)
			term_set_terminal(vt, term_init(vt, -1));

	/* Create splash terminal before daemonizing */
	splash = splash_init(pts_fd);
	if (splash == NULL) {
		LOG(ERROR, "Splash init failed.");
		return EXIT_FAILURE;
	}

	if (command_flags.daemon) {
		int status;
		fprintf(stdout, "%s\n", ptsname(pts_fd));
		daemonize();
		status = mkdir(FRECON_RUN_DIR, S_IRWXU);
		if (status == 0 || (status < 0 && errno == EEXIST)) {
			char pids[32];

			sprintf(pids, "%u", getpid());
			write_string_to_file(FRECON_PID_FILE, pids);
		}
	}

	/* These flags can be only processed after splash object has been created. */
	optind = 1;
	for (;;) {
		c = getopt_long(argc, argv, "", command_options, NULL);

		if (c == -1)
			break;

		switch (c) {
			case FLAG_CLEAR:
				splash_set_clear(splash, strtoul(optarg, NULL, 0));
				break;

			case FLAG_FRAME_INTERVAL:
				splash_set_default_duration(splash, strtoul(optarg, NULL, 0));
				break;

			case FLAG_IMAGE:
				if (!splash_is_hires(splash))
					splash_add_image(splash, optarg);
				break;

			case FLAG_IMAGE_HIRES:
				if (splash_is_hires(splash))
					splash_add_image(splash, optarg);
				break;

			case FLAG_LOOP_COUNT:
				splash_set_loop_count(splash, strtoul(optarg, NULL, 0));
				break;

			case FLAG_LOOP_START:
				splash_set_loop_start(splash, strtoul(optarg, NULL, 0));
				break;

			case FLAG_LOOP_INTERVAL:
				splash_set_loop_duration(splash, strtoul(optarg, NULL, 0));
				break;

			case FLAG_LOOP_OFFSET:
				parse_offset(optarg, &x, &y);
				splash_set_loop_offset(splash, x, y);
				break;

			case FLAG_OFFSET:
				parse_offset(optarg, &x, &y);
				splash_set_offset(splash, x, y);
				break;

			case FLAG_SCALE:
				splash_set_scale(splash, strtoul(optarg, NULL, 0));
				break;
		}
	}

	for (int i = optind; i < argc; i++)
		splash_add_image(splash, argv[i]);

	if (splash_num_images(splash) > 0) {
		ret = splash_run(splash);
		if (ret) {
			LOG(ERROR, "Splash_run failed: %d.", ret);
			return EXIT_FAILURE;
		}
	}

	if (command_flags.splash_only) {
		splash_destroy(splash);
		goto main_done;
	}

	/*
	 * The DBUS service launches later than the boot-splash service, and
	 * as a result, when splash_run starts DBUS is not yet up, but, by
	 * the time splash_run completes, it is running.
	 * We really need DBUS now, so we can interact with Chrome.
	 */
	dbus_init_wait();

	/*
	 * Ask DBUS to call us back so we can destroy splash (or quit) when login
	 * prompt is visible.
	 */
	dbus_set_login_prompt_visible_callback(main_on_login_prompt_visible,
					       (void*)splash);
#if !DBUS
	splash_destroy(splash);
#endif
	/*
	 * Ask DBUS to notify us when suspend has finished so monitors can be reprobed
	 * in case they changed during suspend.
	 */
	dbus_set_suspend_done_callback(term_suspend_done, NULL);

	if (command_flags.daemon) {
		if (command_flags.enable_vts)
			set_drm_master_relax(); /* TODO(dbehr) Remove when Chrome is fixed to actually release master. */
		if (command_flags.enable_vt1)
			term_switch_to(TERM_SPLASH_TERMINAL);
		else
			term_background();
	} else {
		/* Create and switch to first term in interactve mode. */
		set_drm_master_relax(); /* TODO(dbehr) Remove when Chrome is fixed to actually release master. */
		term_switch_to(command_flags.enable_vt1 ? TERM_SPLASH_TERMINAL : 1);
	}

	ret = main_loop();

main_done:
	input_close();
	dev_close();
	dbus_destroy();
	drm_close();
	if (command_flags.daemon)
		unlink(FRECON_PID_FILE);

	return ret;
}
