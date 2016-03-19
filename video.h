/*
 * Copyright (c) 2014 The Chromium OS Authors. All rights reserved.
 * Use of this source code is governed by a BSD-style license that can be
 * found in the LICENSE file.
 */

#ifndef VIDEO_H
#define VIDEO_H

#include "drm.h"

typedef struct {
	int32_t width;
	int32_t height;
	int32_t pitch;
	int32_t scaling;
	int32_t size;
} buffer_properties_t;

typedef struct {
	int32_t count;
	uint64_t map_offset;
	uint32_t* map;
} video_lock_t;

typedef struct {
	drm_t *drm;
	buffer_properties_t buffer_properties;
	video_lock_t lock;
	uint32_t buffer_handle;
	uint32_t fb_id;
} video_t;

video_t* video_init(void);
void video_close(video_t* video);
int32_t video_setmode(video_t* video);
int video_buffer_init(video_t* video);
void video_buffer_destroy(video_t* video);
uint32_t* video_lock(video_t* video);
void video_unlock(video_t* video);
int32_t video_getwidth(video_t* video);
int32_t video_getheight(video_t* video);
int32_t video_getpitch(video_t* video);
int32_t video_getscaling(video_t* video);

#endif
