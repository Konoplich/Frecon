/*
 * Copyright (c) 2014 The Chromium OS Authors. All rights reserved.
 * Use of this source code is governed by a BSD-style license that can be
 * found in the LICENSE file.
 */

#include <stdbool.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>
#include <fcntl.h>

#include "dbus_interface.h"
#include "image.h"
#include "splash.h"
#include "util.h"

#define  MAX_SPLASH_IMAGES      (30)
#define  MAX_SPLASH_WAITTIME    (8)

struct _splash_t {
	video_t         *video;
	int              num_images;
	image_t          images[MAX_SPLASH_IMAGES];
	uint32_t         clear;
	bool             terminated;
	bool             devmode;
	dbus_t          *dbus;
	int32_t          loop_start;
	uint32_t         loop_duration;
	uint32_t         default_duration;
	int32_t          offset_x;
	int32_t          offset_y;
	int32_t          loop_offset_x;
	int32_t          loop_offset_y;
};


splash_t* splash_init()
{
	splash_t* splash;
	FILE *cookie_fp;

	splash = (splash_t*)calloc(1, sizeof(splash_t));
	if (splash == NULL)
		return NULL;

	splash->video = video_init();
	splash->loop_start = -1;
	splash->default_duration = 25;
	splash->loop_duration = 25;

	cookie_fp = fopen("/tmp/display_info.bin", "wb");
	if (cookie_fp) {
		fwrite(&splash->video->internal_panel, sizeof(char), 1, cookie_fp);
		fwrite(splash->video->edid, EDID_SIZE, 1, cookie_fp);
		fclose(cookie_fp);
	}

	return splash;
}

int splash_destroy(splash_t* splash)
{
	return 0;
}

int splash_set_clear(splash_t *splash, int32_t clear_color)
{
	splash->clear = clear_color;
	return 0;
}

static
void parse_filespec(char* filespec, char *filename,
		int32_t *offset_x, int32_t *offset_y, uint32_t *duration,
		uint32_t default_duration, int32_t default_x, int32_t default_y)
{
	char* saved_ptr;
	char* token;

	// defaults
	*offset_x = default_x;
	*offset_y = default_y;
	*duration = default_duration;

	token = filespec;
	token = strtok_r(token, ":", &saved_ptr);
	if (token)
		strcpy(filename, token);

	LOG(ERROR, "image file: %s", filename);

	token = strtok_r(NULL, ":", &saved_ptr);
	if (token) {
		*duration = strtoul(token, NULL, 0);
		token = strtok_r(NULL, ",", &saved_ptr);
		if (token) {
			token = strtok_r(token, ",", &saved_ptr);
			if (token) {
				*offset_x = strtol(token, NULL, 0);
				token = strtok_r(token, ",", &saved_ptr);
				if (token)
					*offset_y = strtol(token, NULL, 0);
			}
		}
	}
}

int splash_add_image(splash_t* splash, char* filespec)
{
	int32_t offset_x, offset_y;
	uint32_t duration;
	if (splash->num_images >= MAX_SPLASH_IMAGES)
		return 1;

	parse_filespec(filespec,
			splash->images[splash->num_images].filename,
			&offset_x, &offset_y, &duration,
			splash->default_duration,
			splash->loop_offset_x,
			splash->loop_offset_y);
	splash->images[splash->num_images].offset_x = offset_x;
	splash->images[splash->num_images].offset_y = offset_y;
	splash->images[splash->num_images].duration = duration;
	splash->num_images++;
	return 0;
}

static void splash_clear_screen(splash_t *splash, uint32_t *video_buffer)
{
	int i,j;
	buffer_properties_t *bp;

	video_setmode(splash->video);

	bp = video_get_buffer_properties(splash->video);

		for (j = 0; j < bp->height; j++) {
			for (i = 0; i < bp->width; i++) {
				 (video_buffer + bp->pitch/4 * j)[i] = splash->clear;
			}
		}
}

int splash_run(splash_t* splash, dbus_t** dbus)
{
	int i;
	uint32_t* video_buffer;
	int status;
	int64_t last_show_ms;
	int64_t now_ms;
	int64_t sleep_ms;
	struct timespec sleep_spec;
	int fd;
	int num_written;
	buffer_properties_t *bp;
	uint32_t startx;
	uint32_t starty;
	int32_t offset_x;
	int32_t offset_y;
	image_t* image;
	uint32_t duration;

	status = 0;

	/*
	 * First draw the actual splash screen
	 */
	video_buffer = video_lock(splash->video);
	if (video_buffer != NULL) {
		splash_clear_screen(splash, video_buffer);
		last_show_ms = -1;
		bp = video_get_buffer_properties(splash->video);
		for (i = 0; i < splash->num_images; i++) {
			image = &splash->images[i];
			status = image_load_image_from_file(image);
			if (status != 0) {
				LOG(WARNING, "image_load_image_from_file failed: %d\n", status);
				break;
			}

			now_ms = get_monotonic_time_ms();
			if (last_show_ms > 0) {
				if (i >= splash->loop_start)
					duration = splash->loop_duration;
				else
					duration = splash->images[i].duration;
				sleep_ms = duration - (now_ms - last_show_ms);
				if (sleep_ms > 0) {
					sleep_spec.tv_sec = sleep_ms / MS_PER_SEC;
					sleep_spec.tv_nsec = (sleep_ms % MS_PER_SEC) * NS_PER_MS;
					nanosleep(&sleep_spec, NULL);
				}
			}

			now_ms = get_monotonic_time_ms();

			if (i >= splash->loop_start) {
				offset_x = splash->loop_offset_x;
				offset_y = splash->loop_offset_y;
			} else {
				offset_x = image->offset_x;
				offset_y = image->offset_y;
			}
			startx = (bp->width - image->width + offset_x)/2;
			starty = (bp->height - image->height + offset_y)/2;
			status = image_show(splash->video, image,
					video_buffer, bp->pitch, startx, starty);
			if (status != 0) {
				LOG(WARNING, "image_show failed: %d", status);
				break;
			}
			last_show_ms = now_ms;

			if ((splash->loop_start >= 0) &&
					(splash->loop_start < splash->num_images)) {
				if (i == splash->num_images - 1)
					i = splash->loop_start;
			}
		}
		video_unlock(splash->video);

		/*
		 * Now Chrome can take over
		 */
		video_release(splash->video);
		sync_lock(false);
		video_unlock(splash->video);

		if (dbus != NULL) {
			do {
				*dbus = dbus_init();
				usleep(50000);
			} while (*dbus == NULL);
			splash_set_dbus(splash, *dbus);
		}

		if (splash->devmode) {
			/*
			 * Now set drm_master_relax so that we can transfer drm_master between
			 * chrome and frecon
			 */
			fd = open("/sys/kernel/debug/dri/drm_master_relax", O_WRONLY);
			if (fd != -1) {
				num_written = write(fd, "Y", 1);
				close(fd);

				/*
				 * If we can't set drm_master relax, then transitions between chrome
				 * and frecon won't work.  No point in having frecon hold any resources
				 */
				if (num_written != 1) {
					LOG(ERROR, "Unable to set drm_master_relax");
					splash->devmode = false;
				}
			} else {
				LOG(ERROR, "unable to open drm_master_relax");
			}
		} else {
			/*
			 * Below, we will wait for Chrome to appear above the splash
			 * image.  If we are not in dev mode, wait and then exit
			 */
			sleep(MAX_SPLASH_WAITTIME);
			exit(EXIT_SUCCESS);
		}
	}

	if (splash->dbus)
		(void)dbus_method_call0(splash->dbus,
			kLibCrosServiceName,
			kLibCrosServicePath,
			kLibCrosServiceInterface,
			kTakeDisplayOwnership);

	/*
	 * Finally, wait until chrome has drawn on top of the splash.  In dev mode,
	 * wait a few seconds for chrome to show up.
	 */
	sleep(MAX_SPLASH_WAITTIME);
	return status;
}

void splash_set_offset(splash_t* splash, int32_t x, int32_t y)
{
	splash->offset_x = x;
	splash->offset_y = y;
}

void splash_set_dbus(splash_t* splash, dbus_t* dbus)
{
	splash->dbus = dbus;
}

void splash_set_devmode(splash_t* splash)
{
	splash->devmode = true;
}

int splash_num_images(splash_t *splash)
{
	if (splash)
		return splash->num_images;

	return 0;
}

void splash_set_default_duration(splash_t* splash, uint32_t duration)
{
	splash->default_duration = duration;
}

void splash_set_loop_start(splash_t* splash, int32_t loop_start)
{
	splash->loop_start = loop_start;
}

void splash_set_loop_duration(splash_t* splash, uint32_t duration)
{
	splash->loop_duration = duration;
}

void splash_set_loop_offset(splash_t* splash, int32_t x, int32_t y)
{
	splash->loop_offset_x = x;
	splash->loop_offset_y = y;
}


