/*
 * Copyright 2015 The Chromium OS Authors. All rights reserved.
 * Use of this source code is governed by a BSD-style license that can be
 * found in the LICENSE file.
 */
#ifndef IMAGE_H
#define IMAGE_H

#include <stddef.h>
#include <stdbool.h>
#include <stdint.h>
#include <png.h>

#include "video.h"

#define  FILENAME_LENGTH        (100)

typedef union {
	uint32_t  *as_pixels;
	png_byte  *as_png_bytes;
	char      *address;
} layout_t;

typedef struct {
	char            filename[FILENAME_LENGTH];
	int32_t         offset_x;
	int32_t         offset_y;
	uint32_t        duration;
	FILE           *fp;
	layout_t        layout;
	png_uint_32     width;
	png_uint_32     height;
	png_uint_32     pitch;
} image_t;


int image_load_image_from_file(image_t* image);
int image_show(video_t *video,
		image_t* image,
		uint32_t *video_buffer,
		uint32_t pitch,
		uint32_t startx, uint32_t starty);
void image_release(image_t* image);

#endif
