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
#define  MAX_SPLASH_WAITTIME    (5)

struct _splash_t {
	video_t         *video;
	int              num_images;
	image_t          images[MAX_SPLASH_IMAGES];
	int              x;  // offset or location
	int              y;  // offset or location
	int              frame_interval;
	uint32_t         clear;
	bool             terminated;
	bool             devmode;
	dbus_t          *dbus;
};


splash_t* splash_init()
{
	splash_t* splash;
	FILE *cookie_fp;

	splash = (splash_t*)calloc(1, sizeof(splash_t));
	if (splash == NULL)
		return NULL;

	splash->video = video_init();

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

int splash_set_frame_rate(splash_t *splash, int32_t rate)
{
	if (rate <= 0 || rate > 120)
		return 1;

	splash->frame_interval = rate;
	return 0;
}

int splash_set_clear(splash_t *splash, int32_t clear_color)
{
	splash->clear = clear_color;
	return 0;
}

int splash_add_image(splash_t* splash, const char* filename)
{
	if (splash->num_images >= MAX_SPLASH_IMAGES)
		return 1;

	strcpy(splash->images[splash->num_images].filename, filename);
	splash->num_images++;
	return 0;
}

static void splash_clear_screen(splash_t *splash, uint32_t *video_buffer)
{
	int i,j;
	buffer_properties_t *bp;

	video_setmode(splash->video);

	/* After the mode is set, there is nothing splash
	 * needs master for
	 */
	video_release(splash->video);

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
			status = image_load_image_from_file(&splash->images[i]);
			if (status != 0) {
				LOG(WARNING, "image_load_image_from_file failed: %d\n", status);
				break;
			}

			now_ms = get_monotonic_time_ms();
			if (last_show_ms > 0) {
				sleep_ms = splash->frame_interval - (now_ms - last_show_ms);
				if (sleep_ms > 0) {
					sleep_spec.tv_sec = sleep_ms / MS_PER_SEC;
					sleep_spec.tv_nsec = (sleep_ms % MS_PER_SEC) * NS_PER_MS;
					nanosleep(&sleep_spec, NULL);
				}
			}

			now_ms = get_monotonic_time_ms();

			startx = (bp->width - splash->images[i].width + splash->x)/2;
			starty = (bp->height - splash->images[i].height + splash->y)/2;
			status = image_show(splash->video, &splash->images[i],
					video_buffer, bp->pitch, startx, starty);
			if (status != 0) {
				LOG(WARNING, "image_show failed: %d", status);
				break;
			}
			last_show_ms = now_ms;
		}

		if (dbus) {
			/*
			 * The dbus check is just a proxy for letting us
			 * know whether or not we want a terminal after
			 * the splash.  If so, then clear the screen to
			 * help transition to the terminal
			 */
			splash_clear_screen(splash, video_buffer);
		}
		video_unlock(splash->video);

		if (dbus != NULL) {
			do {
				*dbus = dbus_init();
				usleep(50000);
			} while (*dbus == NULL);
			splash_set_dbus(splash, *dbus);
		} else {
			sleep(5);
			exit(EXIT_SUCCESS);
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
			 * image.  If we are not in dev mode, just exit
			 */
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

void splash_set_dbus(splash_t* splash, dbus_t* dbus)
{
	splash->dbus = dbus;
}

void splash_set_devmode(splash_t* splash)
{
	splash->devmode = true;
}

void splash_set_offset(splash_t* splash, int x, int y)
{
	splash->x = x;
	splash->y = y;
}
