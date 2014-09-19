/*
 * Copyright (c) 2014 The Chromium OS Authors. All rights reserved.
 * Use of this source code is governed by a BSD-style license that can be
 * found in the LICENSE file.
 */

#include <drm_fourcc.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <sys/mman.h>
#include <unistd.h>
#include <fcntl.h>

#include <math.h>
#include <png.h>

#include "util.h"
#include "video.h"
#include "dbus_interface.h"
#include "dbus.h"

#define   MAX_SPLASH_IMAGES                    (30)
#define   FILENAME_LENGTH                      (100)

#define   MS_PER_SEC                           (1000LL)
#define   NS_PER_SEC                           (1000LL * 1000LL * 1000LL)
#define   NS_PER_MS                            (NS_PER_SEC / MS_PER_SEC);

/* Returns the current CLOCK_MONOTONIC time in milliseconds. */
static int64_t get_monotonic_time_ms() {
  struct timespec spec;
  clock_gettime(CLOCK_MONOTONIC, &spec);
  return MS_PER_SEC * spec.tv_sec + spec.tv_nsec / NS_PER_MS;
}


video_t g_video;

typedef union {
  uint32_t  *as_pixels;
  png_byte  *as_png_bytes;
  char      *address;
} splash_layout_t;

typedef struct {
  char             filename[FILENAME_LENGTH];
  FILE            *fp;
  splash_layout_t  layout;
  png_uint_32      width;
  png_uint_32      height;
  png_uint_32      pitch;
} splash_image_t;

struct _splash_t {
  video_t         *video;
  int              num_images;
  splash_image_t   images[MAX_SPLASH_IMAGES];
  int              frame_interval;
  uint32_t         clear;
  bool             terminated;
  bool             devmode;
  dbus_t           *dbus;
};

splash_t g_splash;


static int splash_load_image_from_file(splash_t* splash, splash_image_t* image);
static int splash_image_show(splash_t *splash, splash_image_t* image,
    uint32_t *video_buffer);
static void splash_rgb(png_struct *png, png_row_info *row_info, png_byte *data);


static int kms_open()
{
	const char *module_list[] = { "cirrus", "exynos", "i915", "rockchip",
				      "tegra" };
	int fd = -1;
	unsigned i;

	for (i = 0; i < ARRAY_SIZE(module_list); i++) {
		fd = drmOpen(module_list[i], NULL);
		if (fd >= 0)
			break;
	}

	return fd;
}

static drmModeCrtc *find_crtc_for_connector(int fd,
					    drmModeRes *resources,
					    drmModeConnector *connector)
{
	int i;
	unsigned encoder_crtc_id = 0;

	/* Find the encoder */
	for (i = 0; i < resources->count_encoders; i++) {
		drmModeEncoder *encoder =
		    drmModeGetEncoder(fd, resources->encoders[i]);

		if (encoder) {
			if (encoder->encoder_id == connector->encoder_id) {
				encoder_crtc_id = encoder->crtc_id;
				drmModeFreeEncoder(encoder);
				break;
			}
			drmModeFreeEncoder(encoder);
		}
	}

	if (!encoder_crtc_id)
		return NULL;

	/* Find the crtc */
	for (i = 0; i < resources->count_crtcs; i++) {
		drmModeCrtc *crtc = drmModeGetCrtc(fd, resources->crtcs[i]);

		if (crtc) {
			if (encoder_crtc_id == crtc->crtc_id)
				return crtc;
			drmModeFreeCrtc(crtc);
		}
	}

	return NULL;
}

static bool is_connector_used(int fd,
			      drmModeRes *resources,
			      drmModeConnector *connector)
{
	bool result = false;
	drmModeCrtc *crtc = find_crtc_for_connector(fd, resources, connector);

	if (crtc) {
		result = crtc->buffer_id != 0;
		drmModeFreeCrtc(crtc);
	}

	return result;
}

static drmModeConnector *find_used_connector_by_type(int fd,
						     drmModeRes *resources,
						     unsigned type)
{
	int i;
	for (i = 0; i < resources->count_connectors; i++) {
		drmModeConnector *connector;

		connector = drmModeGetConnector(fd, resources->connectors[i]);
		if (connector) {
			if ((connector->connector_type == type) &&
			    (is_connector_used(fd, resources, connector)))
				return connector;

			drmModeFreeConnector(connector);
		}
	}
	return NULL;
}

static drmModeConnector *find_first_used_connector(int fd,
						   drmModeRes *resources)
{
	int i;
	for (i = 0; i < resources->count_connectors; i++) {
		drmModeConnector *connector;

		connector = drmModeGetConnector(fd, resources->connectors[i]);
		if (connector) {
			if (is_connector_used(fd, resources, connector))
				return connector;

			drmModeFreeConnector(connector);
		}
	}
	return NULL;
}

static drmModeConnector *find_main_monitor(int fd, drmModeRes *resources)
{
	unsigned i = 0;
	/*
	 * Find the LVDS/eDP/DSI connectors. Those are the main screens.
	 */
	unsigned kConnectorPriority[] = {
		DRM_MODE_CONNECTOR_LVDS,
		DRM_MODE_CONNECTOR_eDP,
		/*
		 * XXX update the kernel headers to support DSI
		 * see crbug.com/402127
		 */
		/* DRM_MODE_CONNECTOR_DSI, */
	};

	drmModeConnector *main_monitor_connector = NULL;
	do {
		main_monitor_connector = find_used_connector_by_type(fd,
								     resources,
								     kConnectorPriority[i]);
		i++;
	} while (!main_monitor_connector && i < ARRAY_SIZE(kConnectorPriority));

	/*
	 * If we didn't find a connector, grab the first one in use.
	 */
	if (!main_monitor_connector)
		main_monitor_connector =
		    find_first_used_connector(fd, resources);

	return main_monitor_connector;
}

static void disable_connector(int fd,
			      drmModeRes *resources,
			      drmModeConnector *connector)
{
	drmModeCrtc *crtc = find_crtc_for_connector(fd, resources, connector);

	if (crtc) {
		drmModeSetCrtc(fd, crtc->crtc_id, 0,	// buffer_id
			       0, 0,	// x,y
			       NULL,	// connectors
			       0,	// connector_count
			       NULL);	// mode
		drmModeFreeCrtc(crtc);
	}
}

static void disable_non_main_connectors(int fd,
					drmModeRes *resources,
					drmModeConnector *main_connector)
{
	int i;

	for (i = 0; i < resources->count_connectors; i++) {
		drmModeConnector *connector;

		connector = drmModeGetConnector(fd, resources->connectors[i]);
		if (connector->connector_id != main_connector->connector_id)
			disable_connector(fd, resources, connector);

		drmModeFreeConnector(connector);
	}
}

static int video_buffer_create(video_t *video, drmModeCrtc *crtc, drmModeConnector *connector,
			       int *pitch)
{
	struct drm_mode_create_dumb create_dumb;
	int ret;

	memset(&create_dumb, 0, sizeof (create_dumb));
	create_dumb.bpp = 32;
	create_dumb.width = crtc->mode.hdisplay;
	create_dumb.height = crtc->mode.vdisplay;

	ret = drmIoctl(video->fd, DRM_IOCTL_MODE_CREATE_DUMB, &create_dumb);
	if (ret) {
    LOG(ERROR, "CREATE_DUMB failed");
		return ret;
  }

	struct drm_mode_map_dumb map_dumb;
	map_dumb.handle = create_dumb.handle;
	ret = drmIoctl(video->fd, DRM_IOCTL_MODE_MAP_DUMB, &map_dumb);
	if (ret)  {
    LOG(ERROR, "MAP_DUMB failed");
		goto destroy_buffer;
  }

	video->lock.map =
	    mmap(0, create_dumb.size, PROT_READ | PROT_WRITE, MAP_SHARED, video->fd,
		 map_dumb.offset);
	if (!video->lock.map) {
    LOG(ERROR, "mmap failed");
		goto destroy_buffer;
  }

	uint32_t offset = 0;
	ret = drmModeAddFB2(video->fd, crtc->mode.hdisplay, crtc->mode.vdisplay,
			    DRM_FORMAT_XRGB8888, &create_dumb.handle,
			    &create_dumb.pitch, &offset, &video->fb_id, 0);
	if (ret) {
    LOG(ERROR, "drmModeAddFB2 failed");
		goto unmap_buffer;
  }

	*pitch = create_dumb.pitch;

	return ret;

unmap_buffer:
	munmap(video->lock.map, create_dumb.size);

destroy_buffer:
	;
	struct drm_mode_destroy_dumb destroy_dumb;
	destroy_dumb.handle = create_dumb.handle;

	drmIoctl(video->fd, DRM_IOCTL_MODE_DESTROY_DUMB, &destroy_dumb);

	return ret;
}

video_t* video_init(int32_t *width, int32_t *height, int32_t *pitch, int32_t *scaling)
{
  video_t *ret = (video_t*)malloc(sizeof(video_t));

  memset(ret, 0, sizeof(*ret));
  ret->fd = -1;

	ret->fd = kms_open();

	if (ret->fd < 0) {
		LOG(ERROR, "Unable to open a KMS module");
		return NULL;
	}

	ret->drm_resources = drmModeGetResources(ret->fd);
	if (!ret->drm_resources) {
		LOG(ERROR, "Unable to get mode resources");
		goto fail;
	}

	ret->main_monitor_connector = find_main_monitor(ret->fd, ret->drm_resources);

	if (!ret->main_monitor_connector) {
    LOG(ERROR, "main_monitor_connector is nil");
		goto fail;
	}

	disable_non_main_connectors(ret->fd,
      ret->drm_resources, ret->main_monitor_connector);

	ret->crtc = find_crtc_for_connector(ret->fd,
      ret->drm_resources, ret->main_monitor_connector);

	if (!ret->crtc) {
    LOG(ERROR, "unable to find a crtc");
		goto fail;
	}

	if (video_buffer_create(ret, ret->crtc,
        ret->main_monitor_connector, pitch)) {
    LOG(ERROR, "video_buffer_create failed");
		goto fail;
	}

	*width = ret->crtc->mode.hdisplay;
	*height = ret->crtc->mode.vdisplay;

	if (!ret->main_monitor_connector->mmWidth)
		*scaling = 1;
	else {
		int dots_per_cm = *width * 10 / ret->main_monitor_connector->mmWidth;
		if (dots_per_cm > 133)
			*scaling = 4;
		else if (dots_per_cm > 100)
			*scaling = 3;
		else if (dots_per_cm > 67)
			*scaling = 2;
		else
			*scaling = 1;
	}

  ret->buffer_properties.width = *width;
  ret->buffer_properties.height = *height;
  ret->buffer_properties.pitch = *pitch;
  ret->buffer_properties.scaling = *scaling;

	return ret;

fail:
  if (ret->drm_resources)
    drmModeFreeResources(ret->drm_resources);

  if (ret->main_monitor_connector)
    drmModeFreeConnector(ret->main_monitor_connector);

  if (ret->crtc)
    drmModeFreeCrtc(ret->crtc);

  if (ret->fd >= 0)
    drmClose(ret->fd);

	return NULL;
}


int32_t video_setmode(video_t* video)
{
  int32_t ret;

  drmSetMaster(video->fd);
  ret = drmModeSetCrtc(video->fd, video->crtc->crtc_id,
           video->fb_id,
			     0, 0,	// x,y
			     &video->main_monitor_connector->connector_id,
			     1,	// connector_count
			     &video->crtc->mode);	// mode

  drmDropMaster(video->fd);

  return ret;
}

void video_close(video_t *video)
{
	if (video->fd >= 0) {
		drmClose(video->fd);
		video->fd = -1;
	}
}


uint32_t* video_lock(video_t *video)
{
  if (video->lock.count == 0) {
    video->lock.count++;
    return video->lock.map;
  }

  return NULL;
}

void video_unlock(video_t *video)
{
	/* XXX Cache flush maybe? */
  if (video->lock.count > 0) {
    drmDropMaster(video->fd);
    video->lock.count--;
  }
}

bool video_load_gamma_ramp(video_t *video, const char* filename)
{
  int i;
  int r = 0;
  unsigned char red[kGammaSize];
  unsigned char green[kGammaSize];
  unsigned char blue[kGammaSize];
  gamma_ramp_t *ramp;

  FILE* f = fopen(filename, "rb");
  if (f == NULL)
    return false;

  ramp = &video->gamma_ramp;

  r += fread(red, sizeof(red), 1, f);
  r += fread(green, sizeof(green), 1, f);
  r += fread(blue, sizeof(blue), 1, f);
  fclose(f);

  if (r != 3)
    return false;

  for (i = 0; i < kGammaSize; ++i) {
    ramp->red[i]   = (uint16_t)red[i] * 257;
    ramp->green[i] = (uint16_t)green[i] * 257;
    ramp->blue[i]  = (uint16_t)blue[i] * 257;
  }

  return true;
}

bool video_set_gamma(video_t* video, const char *filename)
{
  bool status;
  drmModeCrtcPtr mode;
  int drm_status;

  status = video_load_gamma_ramp(video, filename);
  if (status == false) {
    LOG(WARNING, "Unable to load gamma ramp");
    return false;
  }

  mode = drmModeGetCrtc(video->fd, video->crtc->crtc_id);
  drm_status = drmModeCrtcSetGamma(video->fd,
      mode->crtc_id,
      mode->gamma_size,
      video->gamma_ramp.red,
      video->gamma_ramp.green,
      video->gamma_ramp.blue);

  return drm_status == 0;
}


buffer_properties_t* video_get_buffer_properties(video_t *video)
{
  return &video->buffer_properties;
}

int32_t video_getwidth(video_t *video)
{
  return video->buffer_properties.width;
}


int32_t video_getheight(video_t *video)
{
  return video->buffer_properties.height;
}


int32_t video_getpitch(video_t *video)
{
  return video->buffer_properties.pitch;
}


int32_t video_getscaling(video_t *video)
{
  return video->buffer_properties.scaling;
}

splash_t* splash_init(video_t *video)
{
  splash_t* splash;

  splash = &g_splash;
  if (splash == NULL)
    return NULL;

  memset(splash, 0, sizeof(*splash));
  splash->num_images = 0;
  splash->video = video;


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


static void
frecon_dbus_path_message_func(dbus_t* dbus, void* user_data)
{
  splash_t* splash = (splash_t*)user_data;

  if (!splash->devmode)
    exit(EXIT_SUCCESS);

  dbus_stop_wait(dbus);
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
  bool db_status;
  int64_t last_show_ms;
  int64_t now_ms;
  int64_t sleep_ms;
  struct timespec sleep_spec;
  int fd;
  int num_written;

  status = 0;

  /*
   * First draw the actual splash screen
   */
  video_buffer = video_lock(splash->video);
  if (video_buffer != NULL) {
    splash_clear_screen(splash, video_buffer);
    last_show_ms = -1;
    for (i = 0; i < splash->num_images; i++) {
      status = splash_load_image_from_file(splash, &splash->images[i]);
      if (status != 0) {
        LOG(WARNING, "splash_load_image_from_file failed: %d\n", status);
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

      status = splash_image_show(splash, &splash->images[i], video_buffer);
      if (status != 0) {
        LOG(WARNING, "splash_image_show failed: %d", status);
        break;
      }
      last_show_ms = now_ms;
    }
    video_unlock(splash->video);

    /*
     * Next wait until chrome has drawn on top of the splash.  In dev mode,
     * dbus_wait_for_messages will return when chrome is visible.  In 
     * verified mode, the frecon app will exit before dbus_wait_for_messages
     * returns
     */
    do {
      *dbus = dbus_init();
    } while (*dbus == NULL);

    splash_set_dbus(splash, *dbus);

    db_status = dbus_signal_match_handler(*dbus,
        kLoginPromptVisibleSignal,
        kSessionManagerServicePath,
        kSessionManagerInterface,
        kLoginPromptVisiibleRule,
        frecon_dbus_path_message_func, splash);

    if (db_status)
      dbus_wait_for_messages(*dbus);
    

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
  }
  return status;
}


static int splash_load_image_from_file(splash_t* splash, splash_image_t* image)
{
  png_struct   *png;
  png_info     *info;
  png_uint_32   width, height, pitch, row;
  int           bpp, color_type, interlace_mthd;
  png_byte    **rows;

  if (image->fp != NULL)
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

  png_set_read_user_transform_fn(png, splash_rgb);
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
  png_destroy_read_struct(&png, &info, NULL);

  image->width = width;
  image->height = height;
  image->pitch = pitch;

  return 0;
}

static
int splash_image_show(splash_t *splash,
                      splash_image_t* image,
                      uint32_t *video_buffer)
{
  uint32_t j;
  uint32_t startx, starty;
  buffer_properties_t *bp;
  uint32_t *buffer;


  bp = video_get_buffer_properties(splash->video);
  startx = (bp->width - image->width) / 2;
  starty = (bp->height - image->height) / 2;

  buffer = video_lock(splash->video);

  if (buffer != NULL) {
    for (j = starty; j < starty + image->height; j++) {
      memcpy(buffer + j * bp->pitch/4 + startx,
          image->layout.address + (j - starty)*image->pitch, image->pitch);
    }
  }

  video_unlock(splash->video);
  return 0;
}

void splash_set_dbus(splash_t* splash, dbus_t* dbus)
{
  splash->dbus = dbus;
}

void splash_set_devmode(splash_t* splash)
{
  splash->devmode = true;
}


static 
void splash_rgb(png_struct *png, png_row_info *row_info, png_byte *data)
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

