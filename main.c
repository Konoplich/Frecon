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

#include "input.h"
#include "term.h"
#include "video.h"
#include "dbus.h"
#include "util.h"
#include "splash.h"
#include "main.h"

#define  FLAG_CLEAR                        'c'
#define  FLAG_DAEMON                       'd'
#define  FLAG_DEV_MODE                     'e'
#define  FLAG_EXEC                         'x'
#define  FLAG_EXEC0                        '0'
#define  FLAG_EXEC1                        '1'
#define  FLAG_EXEC2                        '2'
#define  FLAG_EXEC3                        '3'
#define  FLAG_EXEC4                        '4'
#define  FLAG_FRAME_INTERVAL               'f'
#define  FLAG_GAMMA                        'g'
#define  FLAG_PRINT_RESOLUTION             'p'

static struct option command_options[] = {
	{ "clear", required_argument, NULL, FLAG_CLEAR },
	{ "daemon", no_argument, NULL, FLAG_DAEMON },
	{ "dev-mode", no_argument, NULL, FLAG_DEV_MODE },
	{ "exec", required_argument, NULL, FLAG_EXEC },
	{ "exec0", required_argument, NULL, FLAG_EXEC0 },
	{ "exec1", required_argument, NULL, FLAG_EXEC1 },
	{ "exec2", required_argument, NULL, FLAG_EXEC2 },
	{ "exec3", required_argument, NULL, FLAG_EXEC3 },
	{ "exec4", required_argument, NULL, FLAG_EXEC4 },
	{ "frame-interval", required_argument, NULL, FLAG_FRAME_INTERVAL },
	{ "gamma", required_argument, NULL, FLAG_GAMMA },
	{ "print-resolution", no_argument, NULL, FLAG_PRINT_RESOLUTION },
	{ NULL, 0, NULL, 0 }
};

commandflags_t flags;

static void freecmdline(char **argv)
{
	int i;
	if (!argv)
		return;
	for (i = 0; argv[i]; i++) {
		free(argv[i]);
	}
	free(argv);
}

static char **splitcmdline(const char *cmdline)
{
	bool inside = false, inquotes = false, escape = false;
	size_t rescount = 0;
	char *buf = NULL, *out;
	char **res = calloc(rescount + 1, sizeof(*res));

	if (!res)
		goto done;

	if (!cmdline)
		goto err;

	while (1) {
		if (!inside && *cmdline != ' ') {
			inside = true;
			if (!buf)
				buf = calloc(strlen(cmdline) + 1, sizeof(*buf));
			if (!buf)
				goto err;
			out = buf;
		}
		if (inside) {
			bool skip = false;
			if (*cmdline == '\\') {
				if (escape) {
					escape = false;
				} else {
					escape = true;
					skip = true;
				}
			} else
			if (*cmdline == '"') {
				if (escape) {
					escape = false;
				} else {
					if (inquotes) {
						inquotes = false;
					} else {
						inquotes = true;
					}
					skip = true;
				}
			} else
			if (*cmdline == ' '
			    || *cmdline == '\0') {
				if (inquotes) {
					if (*cmdline == '\0')
						goto err;
				} else
				if (escape && *cmdline == ' ') {
					escape = false;
				} else {
					char **nres;
					inside = false;
					*out++ = '\0';
					nres = realloc(res, (rescount + 2) * sizeof(*res));
					if (!res)
						goto err;
					res = nres;
					res[rescount] = strdup(buf);
					if (!res[rescount])
						goto err;
					rescount++;
					res[rescount] = NULL;
					skip = true;
				}
			} else {
				escape = false;
			}
			if (!skip) {
				*out++ = *cmdline;
			}
		}
		if (*cmdline)
			cmdline++;
		else
			break;
	}

	goto done;

err:
	freecmdline(res);
	res = NULL;
done:
	free(buf);
	return res;
}

int main(int argc, char* argv[])
{
	int ret;
	int c;
	int i;
	splash_t *splash;
	video_t  *video;
	dbus_t *dbus;

	memset(&flags, 0, sizeof(flags));
	flags.standalone = true;

	ret = input_init();
	if (ret) {
		LOG(ERROR, "Input init failed");
		return EXIT_FAILURE;
	}

	splash = splash_init();
	if (splash == NULL) {
		LOG(ERROR, "splash init failed");
		return EXIT_FAILURE;
	}

	for (i = 0; i < MAX_TERMINALS; i++)
		flags.exec[i] = splitcmdline("/sbin/agetty - 9600 xterm");

	for (;;) {
		c = getopt_long(argc, argv, "", command_options, NULL);

		if (c == -1)
			break;

		switch (c) {
			case FLAG_CLEAR:
				splash_set_clear(splash, strtoul(optarg, NULL, 0));
				break;

			case FLAG_DAEMON:
				daemonize();
				flags.standalone = false;
				break;

			case FLAG_DEV_MODE:
				splash_set_devmode(splash);
				break;

			case FLAG_EXEC:
				for (i = 0; i < MAX_TERMINALS; i++) {
					freecmdline(flags.exec[i]);
					flags.exec[i] = splitcmdline(optarg);
					if (!flags.exec[i]) {
						LOG(ERROR, "Parsing exec command line failed");
						return EXIT_FAILURE;
					}
				}
				break;

			case FLAG_EXEC0:
			case FLAG_EXEC1:
			case FLAG_EXEC2:
			case FLAG_EXEC3:
			case FLAG_EXEC4:
				freecmdline(flags.exec[c - FLAG_EXEC0]);
				flags.exec[c - FLAG_EXEC0] = splitcmdline(optarg);
				if (!flags.exec[c - FLAG_EXEC0]) {
					LOG(ERROR, "Parsing exec command line failed");
					return EXIT_FAILURE;
				}
				break;

			case FLAG_PRINT_RESOLUTION:
				flags.print_resolution = true;
				break;

			case FLAG_FRAME_INTERVAL:
				splash_set_frame_rate(splash, strtoul(optarg, NULL, 0));
				for (i = optind; i < argc; i++) {
					 splash_add_image(splash, argv[i]);
				}
				flags.frame_interval = true;
				break;
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
		video = video_init();
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
	ret = input_run(flags.standalone);

	input_close();

	for (i = 0; i < MAX_TERMINALS; i++)
		freecmdline(flags.exec[i]);

	return ret;
}
