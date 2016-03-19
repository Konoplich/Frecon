/*
 * Copyright (c) 2014 The Chromium OS Authors. All rights reserved.
 * Use of this source code is governed by a BSD-style license that can be
 * found in the LICENSE file.
 */

#include <drm_fourcc.h>
#include <errno.h>
#include <fcntl.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <time.h>
#include <unistd.h>

#include "util.h"
#include "video.h"

static int video_buffer_create(video_t* video,
			       int* pitch)
{
	struct drm_mode_create_dumb create_dumb;
	struct drm_mode_destroy_dumb destroy_dumb;
	int ret;

	memset(&create_dumb, 0, sizeof (create_dumb));
	create_dumb.bpp = 32;
	create_dumb.width = drm->crtc->mode.hdisplay;
	create_dumb.height = drm->crtc->mode.vdisplay;

	ret = drmIoctl(drm->fd, DRM_IOCTL_MODE_CREATE_DUMB, &create_dumb);
	if (ret) {
		LOG(ERROR, "CREATE_DUMB failed");
		return ret;
	}

	video->buffer_properties.size = create_dumb.size;
	video->buffer_handle = create_dumb.handle;

	struct drm_mode_map_dumb map_dumb;
	map_dumb.handle = create_dumb.handle;
	ret = drmIoctl(drm->fd, DRM_IOCTL_MODE_MAP_DUMB, &map_dumb);
	if (ret) {
		LOG(ERROR, "MAP_DUMB failed");
		goto destroy_buffer;
	}

	video->lock.map_offset = map_dumb.offset;

	uint32_t offset = 0;
	ret = drmModeAddFB2(drm->fd, drm->crtc->mode.hdisplay, drm->crtc->mode.vdisplay,
			    DRM_FORMAT_XRGB8888, &create_dumb.handle,
			    &create_dumb.pitch, &offset, &video->fb_id, 0);
	if (ret) {
		LOG(ERROR, "drmModeAddFB2 failed");
		goto destroy_buffer;
	}

	*pitch = create_dumb.pitch;

	return ret;

destroy_buffer:
	destroy_dumb.handle = create_dumb.handle;

	drmIoctl(drm->fd, DRM_IOCTL_MODE_DESTROY_DUMB, &destroy_dumb);

	return ret;
}

void video_buffer_destroy(video_t* video)
{
	struct drm_mode_destroy_dumb destroy_dumb;

	if (video->buffer_handle <= 0)
		return;

	drmModeRmFB(drm->fd, video->fb_id);
	video->fb_id = 0;
	destroy_dumb.handle = video->buffer_handle;
	drmIoctl(drm->fd, DRM_IOCTL_MODE_DESTROY_DUMB, &destroy_dumb);
	video->buffer_handle = 0;
	video->lock.map = NULL;
	drm_delref(video->drm);
	video->drm = NULL;
}

static bool parse_edid_dtd(uint8_t* dtd, drmModeModeInfo* mode,
			   int32_t* hdisplay_size, int32_t* vdisplay_size) {
	int32_t clock;
	int32_t hactive, hbl, hso, hsw, hsize;
	int32_t vactive, vbl, vso, vsw, vsize;

	clock = ((int32_t)dtd[DTD_PCLK_HI] << 8) | dtd[DTD_PCLK_LO];
	if (!clock)
		return false;

	hactive = ((int32_t)(dtd[DTD_HABL_HI] & 0xf0) << 4) + dtd[DTD_HA_LO];
	vactive = ((int32_t)(dtd[DTD_VABL_HI] & 0xf0) << 4) + dtd[DTD_VA_LO];
	hbl = ((int32_t)(dtd[DTD_HABL_HI] & 0x0f) << 8) + dtd[DTD_HBL_LO];
	vbl = ((int32_t)(dtd[DTD_VABL_HI] & 0x0f) << 8) + dtd[DTD_VBL_LO];
	hso = ((int32_t)(dtd[DTD_HVSX_HI] & 0xc0) << 2) + dtd[DTD_HSO_LO];
	vso = ((int32_t)(dtd[DTD_HVSX_HI] & 0x0c) << 2) + (dtd[DTD_VSX_LO] >> 4);
	hsw = ((int32_t)(dtd[DTD_HVSX_HI] & 0x30) << 4) + dtd[DTD_HSW_LO];
	vsw = ((int32_t)(dtd[DTD_HVSX_HI] & 0x03) << 4) + (dtd[DTD_VSX_LO] & 0xf);
	hsize = ((int32_t)(dtd[DTD_HVSIZE_HI] & 0xf0) << 4) + dtd[DTD_HSIZE_LO];
	vsize = ((int32_t)(dtd[DTD_HVSIZE_HI] & 0x0f) << 8) + dtd[DTD_VSIZE_LO];

	mode->clock = clock * 10;
	mode->hdisplay = hactive;
	mode->vdisplay = vactive;
	mode->hsync_start = hactive + hso;
	mode->vsync_start = vactive + vso;
	mode->hsync_end = mode->hsync_start + hsw;
	mode->vsync_end = mode->vsync_start + vsw;
	mode->htotal = hactive + hbl;
	mode->vtotal = vactive + vbl;
	*hdisplay_size = hsize;
	*vdisplay_size = vsize;
	return true;
}

static bool parse_edid_dtd_display_size(drm_t* drm, int32_t* hsize_mm, int32_t* vsize_mm) {
	drmModeModeInfo* mode = &drm->crtc->mode;

	for (int i = 0; i < EDID_N_DTDS; i++) {
		uint8_t* dtd = (uint8_t*)&drm->edid[EDID_DTD_BASE + i * DTD_SIZE];
		drmModeModeInfo dtd_mode;
		int32_t hdisplay_size, vdisplay_size;
		if (!parse_edid_dtd(dtd, &dtd_mode, &hdisplay_size, &vdisplay_size) ||
				mode->clock != dtd_mode.clock ||
				mode->hdisplay != dtd_mode.hdisplay ||
				mode->vdisplay != dtd_mode.vdisplay ||
				mode->hsync_start != dtd_mode.hsync_start ||
				mode->vsync_start != dtd_mode.vsync_start ||
				mode->hsync_end != dtd_mode.hsync_end ||
				mode->vsync_end != dtd_mode.vsync_end ||
				mode->htotal != dtd_mode.htotal ||
				mode->vtotal != dtd_mode.vtotal)
			continue;
		*hsize_mm = hdisplay_size;
		*vsize_mm = vdisplay_size;
		return true;
	}
	return false;
}

int video_buffer_init(video_t* video)
{
	int32_t width, height, pitch;
	int32_t hsize_mm, vsize_mm;
	int r;

	/* some reasonable defaults */
	video->buffer_properties.width = 640;
	video->buffer_properties.height = 480;
	video->buffer_properties.pitch = 640 * 4;
	video->buffer_properties.scaling = 1;

	video->drm = drm_addref();

	if (!video->drm) {
		LOG(WARNING, "No monitor available, running headless!");
		return -ENODEV;
	}

	width = drm->crtc->mode.hdisplay;
	height = drm->crtc->mode.vdisplay;

	r = video_buffer_create(video, &pitch);
	if (r < 0) {
		LOG(ERROR, "video_buffer_create failed");
		return r;
	}

	video->buffer_properties.width = width;
	video->buffer_properties.height = height;
	video->buffer_properties.pitch = pitch;

	hsize_mm = video->drm->main_monitor_connector->mmWidth;
	vsize_mm = video->drm->main_monitor_connector->mmHeight;
	if (drm_read_edid(drm)) {
		parse_edid_dtd_display_size(drm, &hsize_mm, &vsize_mm);
	}

	if (hsize_mm) {
		int dots_per_cm = width * 10 / hsize_mm;
		if (dots_per_cm > 133)
			video->buffer_properties.scaling = 4;
		else if (dots_per_cm > 100)
			video->buffer_properties.scaling = 3;
		else if (dots_per_cm > 67)
			video->buffer_properties.scaling = 2;
	}

	return 0;
}

video_t* video_init(void)
{
	video_t* video;

	video = (video_t*)calloc(1, sizeof(video_t));
	if (!video)
		return NULL;

	video_buffer_init(video);

	return video;
}

void video_close(video_t* video)
{
	if (!video)
		return;

	video_buffer_destroy(video);

	free(video);
}

int32_t video_setmode(video_t* video)
{
	/* headless mode */
	if (!drm_valid(video->drm))
		return 0;

	return drm_setmode(video->drm, video->fb_id);
}

uint32_t* video_lock(video_t* video)
{
	if (video->lock.count == 0 && video->buffer_handle > 0) {
		video->lock.map =
			mmap(0, video->buffer_properties.size, PROT_READ | PROT_WRITE,
					MAP_SHARED, video->drm->fd, video->lock.map_offset);
		if (video->lock.map == MAP_FAILED) {
			LOG(ERROR, "mmap failed");
			return NULL;
		}
	}
	video->lock.count++;

	return video->lock.map;
}

void video_unlock(video_t* video)
{
	if (video->lock.count > 0)
		video->lock.count--;
	else
		LOG(ERROR, "video locking unbalanced");

	if (video->lock.count == 0 && video->buffer_handle > 0) {
		struct drm_clip_rect clip_rect = {
			0, 0, video->buffer_properties.width, video->buffer_properties.height
		};
		munmap(video->lock.map, video->buffer_properties.size);
		drmModeDirtyFB(video->drm->fd, video->fb_id, &clip_rect, 1);
	}
}

int32_t video_getwidth(video_t* video)
{
	return video->buffer_properties.width;
}

int32_t video_getheight(video_t* video)
{
	return video->buffer_properties.height;
}

int32_t video_getpitch(video_t* video)
{
	return video->buffer_properties.pitch;
}

int32_t video_getscaling(video_t* video)
{
	return video->buffer_properties.scaling;
}

