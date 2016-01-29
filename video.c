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

static drmModeConnector* find_first_connected_connector(int fd,
							drmModeRes* resources)
{
	for (int i = 0; i < resources->count_connectors; i++) {
		drmModeConnector* connector;

		connector = drmModeGetConnector(fd, resources->connectors[i]);
		if (connector) {
			if ((connector->count_modes > 0) &&
					(connector->connection == DRM_MODE_CONNECTED))
				return connector;

			drmModeFreeConnector(connector);
		}
	}
	return NULL;
}

static int kms_open(video_t* video)
{
	int fd;
	unsigned i;
	char* dev_name;
	drmModeRes* res = NULL;
	int ret;
	drmVersionPtr version;

	for (i = 0; i < DRM_MAX_MINOR; i++) {
		ret = asprintf(&dev_name, DRM_DEV_NAME, DRM_DIR_NAME, i);
		if (ret < 0)
			continue;

		LOG(INFO, "trying %s", dev_name);
		fd = open(dev_name, O_RDWR, 0);
		free(dev_name);
		if (fd < 0)
			continue;

		res = drmModeGetResources(fd);
		if (!res) {
			LOG(ERROR, "Unable to get resources for card%d", i);
			drmClose(fd);
			continue;
		}

		/* expect at least one crtc so we do not try to run on VGEM */
		if (res->count_crtcs > 0 && res->count_connectors)
			break;

		drmClose(fd);
		drmModeFreeResources(res);
		res = NULL;
	}

	if (fd < 0 || res == NULL)
		return -1;

	video->drm_resources = res;
	version = drmGetVersion(fd);
	if (version) {
		LOG(INFO,
		    "Frecon using drm driver %s, version %d.%d, date(%s), desc(%s)",
		    version->name,
		    version->version_major,
		    version->version_minor,
		    version->date,
		    version->desc);
		drmFreeVersion(version);
	}

	video->drm_plane_resources = drmModeGetPlaneResources(fd);

	return fd;
}

static drmModeCrtc* find_crtc_for_connector(int fd,
					    drmModeRes* resources,
					    drmModeConnector* connector)
{
	int i, j;
	drmModeEncoder* encoder;
	int32_t crtc;

	if (connector->encoder_id)
		encoder = drmModeGetEncoder(fd, connector->encoder_id);
	else
		encoder = NULL;

	if (encoder && encoder->crtc_id) {
		crtc = encoder->crtc_id;
		drmModeFreeEncoder(encoder);
		return drmModeGetCrtc(fd, crtc);
	}

	crtc = -1;
	for (i = 0; i < connector->count_encoders; i++) {
		encoder = drmModeGetEncoder(fd, connector->encoders[i]);

		if (encoder) {
			for (j = 0; j < resources->count_crtcs; j++) {
				if (!(encoder->possible_crtcs & (1 << j)))
					continue;
				crtc = resources->crtcs[j];
				break;
			}
			if (crtc >= 0) {
				drmModeFreeEncoder(encoder);
				return drmModeGetCrtc(fd, crtc);
			}
		}
	}

	return NULL;
}

static drmModeConnector* find_used_connector_by_type(int fd,
						     drmModeRes* resources,
						     unsigned type)
{
	for (int i = 0; i < resources->count_connectors; i++) {
		drmModeConnector* connector;

		connector = drmModeGetConnector(fd, resources->connectors[i]);
		if (connector) {
			if ((connector->connector_type == type) &&
					(connector->connection == DRM_MODE_CONNECTED) &&
					(connector->count_modes > 0))
				return connector;

			drmModeFreeConnector(connector);
		}
	}
	return NULL;
}

static drmModeConnector* find_main_monitor(int fd, drmModeRes* resources,
					   uint32_t* mode_index)
{
	unsigned i = 0;
	int modes;
	/*
	 * Find the LVDS/eDP/DSI connectors. Those are the main screens.
	 */
	unsigned kConnectorPriority[] = {
		DRM_MODE_CONNECTOR_LVDS,
		DRM_MODE_CONNECTOR_eDP,
		DRM_MODE_CONNECTOR_DSI,
	};

	drmModeConnector* main_monitor_connector = NULL;
	do {
		main_monitor_connector = find_used_connector_by_type(fd,
								     resources,
								     kConnectorPriority[i]);
		i++;
	} while (!main_monitor_connector && i < ARRAY_SIZE(kConnectorPriority));

	/*
	 * If we didn't find a connector, grab the first one that is connected.
	 */
	if (!main_monitor_connector)
		main_monitor_connector =
				find_first_connected_connector(fd, resources);

	/*
	 * If we still didn't find a connector, give up and return.
	 */
	if (!main_monitor_connector)
		return NULL;

	*mode_index = 0;
	for (modes = 0; modes < main_monitor_connector->count_modes; modes++) {
		if (main_monitor_connector->modes[modes].type &
				DRM_MODE_TYPE_PREFERRED) {
			*mode_index = modes;
			break;
		}
	}

	return main_monitor_connector;
}

static void disable_crtc(int fd,
			 drmModeRes* resources,
			 drmModeCrtc* crtc)
{
	if (crtc) {
		drmModeSetCrtc(fd, crtc->crtc_id, 0, // buffer_id
			       0, 0,  // x,y
			       NULL,  // connectors
			       0,     // connector_count
			       NULL); // mode
	}
}

static void disable_non_main_crtcs(int fd,
				   drmModeRes* resources,
				   drmModeCrtc* main_crtc)
{
	int i;
	drmModeCrtc* crtc;

	for (i = 0; i < resources->count_connectors; i++) {
		drmModeConnector* connector;

		connector = drmModeGetConnector(fd, resources->connectors[i]);
		crtc = find_crtc_for_connector(fd, resources, connector);
		if (crtc->crtc_id != main_crtc->crtc_id)
			disable_crtc(fd, resources, crtc);
		drmModeFreeCrtc(crtc);
	}
}

static int video_buffer_create(video_t* video, drmModeCrtc* crtc, drmModeConnector* connector,
			       int* pitch)
{
	struct drm_mode_create_dumb create_dumb;
	struct drm_mode_destroy_dumb destroy_dumb;
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

	video->buffer_properties.size = create_dumb.size;
	video->buffer_handle = create_dumb.handle;

	struct drm_mode_map_dumb map_dumb;
	map_dumb.handle = create_dumb.handle;
	ret = drmIoctl(video->fd, DRM_IOCTL_MODE_MAP_DUMB, &map_dumb);
	if (ret) {
		LOG(ERROR, "MAP_DUMB failed");
		goto destroy_buffer;
	}

	video->lock.map_offset = map_dumb.offset;

	uint32_t offset = 0;
	ret = drmModeAddFB2(video->fd, crtc->mode.hdisplay, crtc->mode.vdisplay,
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

	drmIoctl(video->fd, DRM_IOCTL_MODE_DESTROY_DUMB, &destroy_dumb);

	return ret;
}

static void video_buffer_destroy(video_t* video)
{
	struct drm_mode_destroy_dumb destroy_dumb;

	if (video->buffer_handle <= 0)
		return;

	drmModeRmFB(video->fd, video->fb_id);
	video->fb_id = 0;
	destroy_dumb.handle = video->buffer_handle;
	drmIoctl(video->fd, DRM_IOCTL_MODE_DESTROY_DUMB, &destroy_dumb);
	video->buffer_handle = 0;
	video->lock.map = NULL;
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

static bool parse_edid_dtd_display_size(video_t* video,
					int32_t* hsize_mm, int32_t* vsize_mm) {
	drmModeModeInfo* mode = &video->crtc->mode;

	for (int i = 0; i < EDID_N_DTDS; i++) {
		uint8_t* dtd = (uint8_t*)&video->edid[EDID_DTD_BASE + i * DTD_SIZE];
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

static void video_close_monitor(video_t* video)
{
	video_buffer_destroy(video);

	if (video->main_monitor_connector) {
		disable_crtc(video->fd, video->drm_resources,
				find_crtc_for_connector(video->fd, video->drm_resources,
					video->main_monitor_connector));
		drmModeFreeConnector(video->main_monitor_connector);
		video->main_monitor_connector = NULL;
	}

	if (video->crtc) {
		drmModeFreeCrtc(video->crtc);
		video->crtc = NULL;
	}
}

/* video_init_connector can be safely called multiple times for instance
   when another monitor is connected. it will pick the best monitor and
   re-initialize required resources.
   return codes:
    negative - error
    0 - OK, no changes
    1 - OK, monitor has changed
*/
int video_init_connector(video_t* video)
{
	int32_t width, height, scaling, pitch;
	bool edid_found = false;
	int32_t hsize_mm, vsize_mm;
	uint32_t selected_mode;
	drmModeConnectorPtr new_mon;
	int ret = 0, r;

	new_mon = find_main_monitor(video->fd,
			video->drm_resources, &selected_mode);

	if (!new_mon) {
		if (video->main_monitor_connector)
			video_close_monitor(video);
		video->buffer_properties.width = 640;
		video->buffer_properties.height = 480;
		video->buffer_properties.pitch = 640 * 4;
		video->buffer_properties.scaling = 1;
		LOG(WARNING, "No monitor available, running headless!");
		return ret;
	}

	if (video->main_monitor_connector
		&& new_mon->connector_id == video->main_monitor_connector->connector_id) {
		drmModeFreeConnector(new_mon);
		return ret;
	}

	ret = 1;
	if (video->main_monitor_connector)
		video_close_monitor(video);
	video->main_monitor_connector = new_mon;

	for (int i = 0; i < video->main_monitor_connector->count_props; i++) {
		drmModePropertyPtr prop;
		drmModePropertyBlobPtr blob_ptr;
		prop = drmModeGetProperty(video->fd, video->main_monitor_connector->props[i]);
		if (prop) {
			if (strcmp(prop->name, "EDID") == 0) {
				blob_ptr = drmModeGetPropertyBlob(video->fd,
					video->main_monitor_connector->prop_values[i]);
				if (blob_ptr) {
					memcpy(&video->edid, blob_ptr->data, EDID_SIZE);
					drmModeFreePropertyBlob(blob_ptr);
					edid_found = true;
				}
			}
		}
	}

	video->crtc = find_crtc_for_connector(video->fd,
			video->drm_resources, video->main_monitor_connector);

	if (!video->crtc) {
		LOG(ERROR, "unable to find a crtc");
		return -ENODEV;
	}

	video->crtc->mode =
		video->main_monitor_connector->modes[selected_mode];

	r = video_buffer_create(video, video->crtc,
				video->main_monitor_connector, &pitch);
	if (r < 0) {
		LOG(ERROR, "video_buffer_create failed");
		return r;
	}

	width = video->crtc->mode.hdisplay;
	height = video->crtc->mode.vdisplay;

	if (!edid_found || !parse_edid_dtd_display_size(
			video, &hsize_mm, &vsize_mm)) {
		hsize_mm = video->main_monitor_connector->mmWidth;
		vsize_mm = video->main_monitor_connector->mmHeight;
	}

	if (!hsize_mm)
		scaling = 1;
	else {
		int dots_per_cm = width * 10 / hsize_mm;
		if (dots_per_cm > 133)
			scaling = 4;
		else if (dots_per_cm > 100)
			scaling = 3;
		else if (dots_per_cm > 67)
			scaling = 2;
		else
			scaling = 1;
	}

	video->buffer_properties.width = width;
	video->buffer_properties.height = height;
	video->buffer_properties.pitch = pitch;
	video->buffer_properties.scaling = scaling;

	return ret;
}

video_t* video_init()
{
	video_t* new_video;

	new_video = (video_t*)calloc(1, sizeof(video_t));
	if (!new_video)
		return NULL;

	new_video->fd = kms_open(new_video);

	if (new_video->fd < 0) {
		LOG(ERROR, "Unable to open a KMS module");
		return NULL;
	}

	if (!new_video->drm_resources) {
		LOG(ERROR, "Unable to get mode resources");
		goto fail;
	}

	if (video_init_connector(new_video) < 0)
		goto fail;

	video_addref(new_video);

	return new_video;

fail:
	if (new_video->drm_resources)
		drmModeFreeResources(new_video->drm_resources);

	if (new_video->main_monitor_connector)
		drmModeFreeConnector(new_video->main_monitor_connector);

	if (new_video->crtc)
		drmModeFreeCrtc(new_video->crtc);

	if (new_video->fd >= 0)
		drmClose(new_video->fd);

	return NULL;
}

static int video_is_primary_plane(video_t* video, uint32_t plane_id)
{
	uint32_t p;
	bool found = false;
	int ret = -1;

	drmModeObjectPropertiesPtr props;
	props = drmModeObjectGetProperties(video->fd,
					   plane_id,
					   DRM_MODE_OBJECT_PLANE);
	if (!props) {
		LOG(ERROR, "Unable to get plane properties: %m");
		return -1;
	}

	for (p = 0; p < props->count_props && !found; p++) {
		drmModePropertyPtr prop;
		prop = drmModeGetProperty(video->fd, props->props[p]);
		if (prop) {
			if (strcmp("type", prop->name) == 0) {
				found = true;
				ret = (props->prop_values[p] == DRM_PLANE_TYPE_PRIMARY);
			}
			drmModeFreeProperty(prop);
		}
	}

	drmModeFreeObjectProperties(props);

	return ret;
}

/* disable all planes except for primary on crtc we use */
static void video_disable_non_primary_planes(video_t* video)
{
	int ret;

	if (!video->drm_plane_resources)
		return;

	for (uint32_t p = 0; p < video->drm_plane_resources->count_planes; p++) {
		drmModePlanePtr plane;
		plane = drmModeGetPlane(video->fd,
					video->drm_plane_resources->planes[p]);
		if (plane) {
			int primary = video_is_primary_plane(video, plane->plane_id);
			if (!(plane->crtc_id == video->crtc->crtc_id && primary != 0)) {
				ret = drmModeSetPlane(video->fd, plane->plane_id, plane->crtc_id,
						      0, 0,
						      0, 0,
						      0, 0,
						      0, 0,
						      0, 0);
				if (ret) {
					LOG(WARNING, "Unable to disable plane: %m");
				}
			}
			drmModeFreePlane(plane);
		}
	}
}

int32_t video_setmode(video_t* video)
{
	int32_t ret;

	/* headless mode */
	if (!video->crtc)
		return 0;

	ret = drmSetMaster(video->fd);
	if (ret)
		LOG(ERROR, "drmSetMaster failed: %m");

	ret = drmModeSetCrtc(video->fd, video->crtc->crtc_id,
			     video->fb_id,
			     0, 0,  // x,y
			     &video->main_monitor_connector->connector_id,
			     1,  // connector_count
			     &video->crtc->mode); // mode

	if (ret) {
		LOG(ERROR, "Unable to set crtc: %m");
		goto done;
	}

	ret = drmModeSetCursor(video->fd, video->crtc->crtc_id,
			0, 0, 0);

	if (ret)
		LOG(ERROR, "Unable to hide cursor");

	video_disable_non_primary_planes(video);
	disable_non_main_crtcs(video->fd,
			video->drm_resources, video->crtc);

done:
	return ret;
}

void video_release(video_t* video)
{
	drmDropMaster(video->fd);
}

void video_close(video_t* video)
{

	if (!video)
		return;

	video_delref(video);
	if (video->ref > 0)
		return;

	video_release(video);

	if (video->fd >= 0) {
		video_close_monitor(video);

		if (video->drm_plane_resources) {
			drmModeFreePlaneResources(video->drm_plane_resources);
			video->drm_plane_resources = NULL;
		}

		if (video->drm_resources) {
			drmModeFreeResources(video->drm_resources);
			video->drm_resources = NULL;
		}

		drmClose(video->fd);
		video->fd = -1;
	}

	free(video);
}

uint32_t* video_lock(video_t* video)
{
	if (video->lock.count == 0 && video->buffer_handle > 0) {
		video->lock.map =
			mmap(0, video->buffer_properties.size, PROT_READ | PROT_WRITE,
					MAP_SHARED, video->fd, video->lock.map_offset);
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
		drmModeDirtyFB(video->fd, video->fb_id, &clip_rect, 1);
	}
}

bool video_load_gamma_ramp(video_t* video, const char* filename)
{
	int r = 0;
	unsigned char red[kGammaSize];
	unsigned char green[kGammaSize];
	unsigned char blue[kGammaSize];
	gamma_ramp_t* ramp;
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

	for (int i = 0; i < kGammaSize; ++i) {
		ramp->red[i]   = (uint16_t)red[i] * 257;
		ramp->green[i] = (uint16_t)green[i] * 257;
		ramp->blue[i]  = (uint16_t)blue[i] * 257;
	}

	return true;
}

bool video_set_gamma(video_t* video, const char* filename)
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

void video_addref(video_t* video)
{
	video->ref++;
}

void video_delref(video_t* video)
{
	video->ref--;
}
