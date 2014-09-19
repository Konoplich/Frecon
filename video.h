/*
 * Copyright (c) 2014 The Chromium OS Authors. All rights reserved.
 * Use of this source code is governed by a BSD-style license that can be
 * found in the LICENSE file.
 */

#ifndef VIDEO_H
#define VIDEO_H

#include <xf86drm.h>
#include <xf86drmMode.h>
#include "dbus.h"

#define kGammaSize      (256)

typedef struct _buffer_properties_t {
  int32_t width;
  int32_t height;
  int32_t pitch;
  int32_t scaling;
  int32_t size;
} buffer_properties_t;

typedef struct _video_lock_t {
  int32_t count;
  uint32_t *map;
} video_lock_t;;

typedef struct _gamme_ramp_t{
  uint16_t   red[kGammaSize];
  uint16_t   green[kGammaSize];
  uint16_t   blue[kGammaSize];
} gamma_ramp_t;

gamma_ramp_t g_gamma_ramp;

typedef struct _video_t {
  int    fd;
  buffer_properties_t  buffer_properties;
  video_lock_t       lock;
  drmModeRes  *drm_resources;
  drmModeConnector *main_monitor_connector;
  drmModeCrtc *crtc;

  uint32_t fb_id;

  gamma_ramp_t gamma_ramp;
} video_t;

typedef struct _splash_t splash_t;


video_t* video_init(int32_t *width, int32_t *height, int32_t *pitch, int *scaling);
int32_t video_getwidth(video_t* video);
int32_t video_getheight(video_t* video);
int32_t video_getpitch(video_t* video);
int32_t video_getscaling(video_t* video);
int32_t video_setmode(video_t* video);
bool video_set_gamma(video_t* video, const char *filename);
void video_close(video_t*);
uint32_t* video_lock(video_t* video);
buffer_properties_t* video_get_buffer_properties(video_t* video);
void video_unlock(video_t* video);

splash_t* splash_init(video_t *video);
int splash_destroy(splash_t*);
int splash_add_image(splash_t*, const char* path);
int splash_set_frame_rate(splash_t *splash, int32_t rate);
int splash_set_clear(splash_t* splash, int32_t clear_color);
void splash_set_dbus(splash_t* splash, dbus_t* dbus);
void splash_set_devmode(splash_t* splash);
int splash_run(splash_t*, dbus_t **dbus);


#endif
