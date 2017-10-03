/*
 * Copyright (c) 2014 The Chromium OS Authors. All rights reserved.
 * Use of this source code is governed by a BSD-style license that can be
 * found in the LICENSE file.
 */

#include <errno.h>
#include <fcntl.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>

#include "dbus.h"
#include "dbus_interface.h"
#include "image.h"
#include "input.h"
#include "main.h"
#include "splash.h"
#include "term.h"
#include "util.h"

#define  MAX_SPLASH_IMAGES      (30)
#define  MAX_SPLASH_WAITTIME    (8)

typedef struct {
	image_t* image;
	uint32_t duration;
} splash_frame_t;

struct _splash_t {
	int num_images;
	uint32_t clear;
	splash_frame_t image_frames[MAX_SPLASH_IMAGES];
	bool terminated;
	int32_t loop_start;
	int32_t loop_count;
	uint32_t loop_duration;
	uint32_t default_duration;
	int32_t offset_x;
	int32_t offset_y;
	int32_t loop_offset_x;
	int32_t loop_offset_y;
	uint32_t scale;
	char info_cmd[MAX_CMD_LEN];
};


splash_t* splash_init(int pts_fd)
{
	splash_t* splash;

	splash = (splash_t*)calloc(1, sizeof(splash_t));
	if (!splash)
		return NULL;

	term_create_splash_term(pts_fd);
	splash->loop_start = -1;
	splash->loop_count = -1;
	splash->default_duration = 25;
	splash->loop_duration = 25;
	splash->scale = 1;
	splash->info_cmd[0]='\0';

	return splash;
}

int splash_destroy(splash_t* splash)
{
	free(splash);
	term_destroy_splash_term();
	return 0;
}

int splash_set_clear(splash_t* splash, uint32_t clear_color)
{
	splash->clear = clear_color;
	return 0;
}

int splash_add_image(splash_t* splash, char* filespec)
{
	image_t* image;
	int32_t offset_x, offset_y;
	char* filename;
	uint32_t duration;
	if (splash->num_images >= MAX_SPLASH_IMAGES)
		return 1;

	filename = (char*)malloc(strlen(filespec) + 1);
	parse_filespec(filespec,
			filename,
			&offset_x, &offset_y, &duration,
			splash->default_duration,
			splash->offset_x,
			splash->offset_y);

	image = image_create();
	image_set_filename(image, filename);
	image_set_offset(image, offset_x, offset_y);
	if (splash->scale == 0)
		image_set_scale(image, splash_is_hires(splash) ? 2 : 1);
	else
		image_set_scale(image, splash->scale);
	splash->image_frames[splash->num_images].image = image;
	splash->image_frames[splash->num_images].duration = duration;
	splash->num_images++;

	free(filename);
	return 0;
}

void splash_show_cmd(terminal_t *terminal, char *command)
{
	FILE *cmd;
	int w, h, i;
	term_get_dimensions(terminal, &w, &h);

	static int *row_lengths;
	if (!row_lengths)
		row_lengths = calloc(h, sizeof(int));

	int *new_lengths = calloc(h, sizeof(int));
	/*
	 * row_text length is the number of columns of displayable text plus two
	 * per row, to have room for the newline character and null termination.
	 */
	char *row_text = calloc((w + 2), sizeof(char));

	/*
	 * Start at the top left.
	 */
	int row = 0;
	term_set_cursor_position(terminal, 0, 0);

	cmd = popen(command, "r");
	if (cmd == NULL) {
		LOG(ERROR, "Failure executing command: %s\n", command);
		snprintf(row_text, w+1, "Failure executing command: %s\n", command);
		term_write_message(terminal, row_text);
	} else {
		while (fgets(row_text, w+1, cmd)) {
			new_lengths[row] = strlen(row_text);
			/*
			 * Add additional space characters for rows that ended up being
			 * shorter than the previous iterations' row was, to cover the
			 * remaining characters.
			 */
			if (new_lengths[row] < row_lengths[row]) {
				for (i = new_lengths[row] - 1; i < row_lengths[row]; i++) {
					row_text[i] = ' ';
				}
				row_text[i] = '\n';
			}
			/*
			 * In the case where we read 'w' characters from the command, we
			 * need to make sure that the text is terminated with a newline.
			 *
			 * In the case that the output was exactly w displayable characters
			 * and the w+1th character was a \n already, we're just overwriting
			 * the newline with a newline.  If the case that we read w chars
			 * and the source line was longer than w before a newline, we'll be
			 * overwriting a null terminator, which is why row_text is allocated
			 * as w+2 in length.
			 */
			if (new_lengths[row] == w)
				row_text[w] = '\n';

			term_write_message(terminal, row_text);
			/*
			 * Clear the buffer for next lap.
			 */
			memset(row_text, 0, w+2);
			row++;
			/*
			 * Ignore the last line on the screen, as printing to it triggers
			 * a framebuffer scroll.
			 */
			if (row == (h - 1))
				break;
		}
		/*
		 * Clear any rows that are below the last line, but were written to
		 * during the previous execution.  Ignore the bottom line on the screen
		 * as printing to it causes scrolling.
		 */
		for (; row < h - 1; row++) {
			if(row_lengths[row]) {
				memset(row_text, ' ', row_lengths[row]);
			}
			row_text[row_lengths[row]] = '\n';
			term_write_message(terminal, row_text);
			memset(row_text, 0, w+2);
		}
		pclose(cmd);
	}
	free(row_text);
	free(row_lengths);
	row_lengths = new_lengths;
}

int splash_run(splash_t* splash)
{
	int i;
	int status = 0;
	/*
	 * Counters for throttling error messages. Only at most MAX_SPLASH_IMAGES
	 * of each type of error are logged so every frame of animation could log
	 * error message but it wouldn't spam the log.
	 */
	int ec_li = 0, ec_ts = 0, ec_ip = 0;
	int64_t last_show_ms;
	int64_t last_info_ms;
	int64_t now_ms;
	int64_t sleep_ms;
	struct timespec sleep_spec;
	image_t* image;
	uint32_t duration;
	int32_t c, loop_start, loop_count;
	bool active = false;

	terminal_t *terminal = term_get_terminal(TERM_SPLASH_TERMINAL);
	if (!terminal)
		return -ENOENT;

	/*
	 * First draw the actual splash screen
	 */
	term_set_background(terminal, splash->clear);
	term_clear(terminal);
	term_set_current_to(terminal);

	last_show_ms = -1;
	last_info_ms = -1;
	loop_count = (splash->loop_start >= 0 && splash->loop_start < splash->num_images) ? splash->loop_count : 1;
	loop_start = (splash->loop_start >= 0 && splash->loop_start < splash->num_images) ? splash->loop_start : 0;

	for (c = 0; ((loop_count < 0) ? true : (c < loop_count)); c++)
	for (i = (c > 0) ? loop_start : 0; i < splash->num_images; i++) {
		image = splash->image_frames[i].image;
		status = image_load_image_from_file(image);
		if (status != 0 && ec_li < MAX_SPLASH_IMAGES) {
			LOG(WARNING, "image_load_image_from_file %s failed: %d:%s.",
				image_get_filename(image), status, strerror(status));
			ec_li++;
		}
		/*
		 * Check status again after timing code so we preserve animation
		 * frame timings and dont's monopolize CPU time.
		 */
		now_ms = get_monotonic_time_ms();
		if (last_show_ms > 0) {
			if (splash->loop_start >= 0 && i >= splash->loop_start)
				duration = splash->loop_duration;
			else
				duration = splash->image_frames[i].duration;
			sleep_ms = duration - (now_ms - last_show_ms);
			if (sleep_ms > 0) {
				sleep_spec.tv_sec = sleep_ms / MS_PER_SEC;
				sleep_spec.tv_nsec = (sleep_ms % MS_PER_SEC) * NS_PER_MS;
				nanosleep(&sleep_spec, NULL);
			}
		}

		now_ms = get_monotonic_time_ms();
		if (status != 0) {
			goto img_error;
		}

		if (i >= splash->loop_start) {
			image_set_offset(image,
					splash->loop_offset_x,
					splash->loop_offset_y);
		}
		status = term_show_image(terminal, image);
		if (status != 0 && ec_ts < MAX_SPLASH_IMAGES) {
			LOG(WARNING, "term_show_image failed: %d:%s.", status, strerror(status));
			ec_ts++;
			goto img_error;
		}

		if (splash->info_cmd[0] != '\0' && now_ms > last_info_ms + 1000) {
			last_info_ms = now_ms;
			splash_show_cmd(terminal, splash->info_cmd);
		}


		if (!active) {
			/*
			 * Set video mode on first frame so user does not see
			 * us drawing first frame.
			 */
			term_activate(terminal);
			active = true;
		}

		status = main_process_events(1);
		if (status != 0 && ec_ip < MAX_SPLASH_IMAGES) {
			LOG(WARNING, "input_process failed: %d:%s.", status, strerror(status));
			ec_ip++;
		}
img_error:
		last_show_ms = now_ms;

		image_release(image);
		/* see if we can initialize DBUS */
		if (!dbus_is_initialized())
			dbus_init();
		if (status != 0) {
			break;
		}
	}

	for (i = 0; i < splash->num_images; i++) {
		image_destroy(splash->image_frames[i].image);
	}

	return status;
}

void splash_set_offset(splash_t* splash, int32_t x, int32_t y)
{
	if (splash) {
		splash->offset_x = x;
		splash->offset_y = y;
	}
}

int splash_num_images(splash_t* splash)
{
	if (splash)
		return splash->num_images;

	return 0;
}

void splash_set_loop_count(splash_t* splash, int32_t count)
{
	if (splash)
		splash->loop_count = count;
}

void splash_set_default_duration(splash_t* splash, uint32_t duration)
{
	if (splash)
		splash->default_duration = duration;
}

void splash_set_loop_start(splash_t* splash, int32_t loop_start)
{
	if (splash)
		splash->loop_start = loop_start;
}

void splash_set_loop_duration(splash_t* splash, uint32_t duration)
{
	if (splash)
		splash->loop_duration = duration;
}

void splash_set_loop_offset(splash_t* splash, int32_t x, int32_t y)
{
	if (splash) {
		splash->loop_offset_x = x;
		splash->loop_offset_y = y;
	}
}

void splash_set_scale(splash_t* splash, uint32_t scale)
{
	if (scale > MAX_SCALE_FACTOR)
		scale = MAX_SCALE_FACTOR;
	if (splash)
		splash->scale = scale;
}

void splash_set_info_cmd(splash_t* splash, char *cmd)
{
	strncpy(splash->info_cmd, "exec 2>&1;", 10);
	strncpy(splash->info_cmd + 10, cmd, MAX_CMD_LEN - 10);
}

int splash_is_hires(splash_t* splash)
{
	terminal_t *terminal = term_get_terminal(TERM_SPLASH_TERMINAL);
	if (!terminal)
		return 0;

	if (term_getfb(terminal))
		return fb_getwidth(term_getfb(terminal)) > HIRES_THRESHOLD_HR;
	return 0;
}

void splash_redrm(splash_t* splash)
{
	terminal_t *terminal = term_get_terminal(TERM_SPLASH_TERMINAL);
	if (!terminal)
		return;
	term_redrm(terminal);
}
