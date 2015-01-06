/*
 * Copyright 2015 The Chromium OS Authors. All rights reserved.
 * Use of this source code is governed by a BSD-style license that can be
 * found in the LICENSE file.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>
#include <fcntl.h>
#include <math.h>

#include "image.h"

#include "util.h"


static void image_rgb(png_struct *png, png_row_info *row_info, png_byte *data)
{
	unsigned int i;

	for (i = 0; i < row_info->rowbytes; i+= 4) {
		uint8_t r, g, b, a;
		uint32_t pixel;

		r = data[i + 0];
		g = data[i + 1];
		b = data[i + 2];
		a = data[i + 3];
		pixel = (a << 24) | (r << 16) | (g << 8) | b;
		memcpy(data + i, &pixel, sizeof(pixel));
	}
}

int image_load_image_from_file(image_t* image)
{
	png_struct   *png;
	png_info     *info;
	png_uint_32   width, height, pitch, row;
	int           bpp, color_type, interlace_mthd;
	png_byte    **rows;

	if (image->fp != NULL)
		return 1;

	if (image->layout.address != NULL)
		return 1;

	image->fp = fopen(image->filename, "rb");
	if (image->fp == NULL)
		return 1;

	png = png_create_read_struct(PNG_LIBPNG_VER_STRING, NULL, NULL, NULL);
	info = png_create_info_struct(png);

	if (info == NULL)
		return 1;

	png_init_io(png, image->fp);

	if (setjmp(png_jmpbuf(png)) != 0) {
		fclose(image->fp);
		return 1;
	}

	png_read_info(png, info);
	png_get_IHDR(png, info, &width, &height, &bpp, &color_type,
			&interlace_mthd, NULL, NULL);

	pitch = 4 * width;

	switch (color_type)
	{
		case PNG_COLOR_TYPE_PALETTE:
			png_set_palette_to_rgb(png);
			break;

		case PNG_COLOR_TYPE_GRAY:
		case PNG_COLOR_TYPE_GRAY_ALPHA:
			png_set_gray_to_rgb(png);
	}

	if (png_get_valid(png, info, PNG_INFO_tRNS))
		png_set_tRNS_to_alpha(png);

	switch (bpp)
	{
		default:
			if (bpp < 8)
				png_set_packing(png);
			break;
		case 16:
			png_set_strip_16(png);
			break;
	}

	if (interlace_mthd != PNG_INTERLACE_NONE)
		png_set_interlace_handling(png);

	png_set_filler(png, 0xff, PNG_FILLER_AFTER);

	png_set_read_user_transform_fn(png, image_rgb);
	png_read_update_info(png, info);

	rows = malloc(height * sizeof(*rows));
	image->layout.address = malloc(height * pitch);

	for (row = 0; row < height; row++) {
		rows[row] = &image->layout.as_png_bytes[row * pitch];
	}

	png_read_image(png, rows);
	free(rows);

	png_read_end(png, info);
	fclose(image->fp);
	image->fp = NULL;
	png_destroy_read_struct(&png, &info, NULL);

	image->width = width;
	image->height = height;
	image->pitch = pitch;

	return 0;
}

int image_show(video_t *video,
		image_t* image,
		uint32_t *video_buffer,
		uint32_t pitch,
		uint32_t startx, uint32_t starty)
{
	uint32_t j;
	uint32_t *buffer;

	buffer = video_lock(video);

	if (buffer != NULL) {
		for (j = starty; j < starty + image->height; j++) {
			memcpy(buffer + j * pitch/4 + startx,
					image->layout.address + (j - starty)*image->pitch, image->pitch);
		}
	}

	video_unlock(video);
	return 0;
}

void image_release(image_t* image)
{
	if (image->layout.address != NULL)
		free(image->layout.address);

	image->layout.address = NULL;
}

