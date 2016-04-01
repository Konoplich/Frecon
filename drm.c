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
#include <time.h>
#include <unistd.h>

#include "drm.h"
#include "input.h"
#include "util.h"

static drm_t* drm = NULL;

static void drm_disable_crtc(drm_t* drm, drmModeCrtc* crtc)
{
	if (crtc) {
		drmModeSetCrtc(drm->fd, crtc->crtc_id, 0, // buffer_id
			       0, 0,  // x,y
			       NULL,  // connectors
			       0,     // connector_count
			       NULL); // mode
	}
}

static drmModeCrtc* find_crtc_for_connector(drm_t* drm, drmModeConnector* connector)
{
	int i, j;
	drmModeEncoder* encoder;
	int32_t crtc_id;

	if (connector->encoder_id)
		encoder = drmModeGetEncoder(drm->fd, connector->encoder_id);
	else
		encoder = NULL;

	if (encoder && encoder->crtc_id) {
		crtc_id = encoder->crtc_id;
		drmModeFreeEncoder(encoder);
		return drmModeGetCrtc(drm->fd, crtc_id);
	}

	crtc_id = -1;
	for (i = 0; i < connector->count_encoders; i++) {
		encoder = drmModeGetEncoder(drm->fd, connector->encoders[i]);

		if (encoder) {
			for (j = 0; j < drm->resources->count_crtcs; j++) {
				if (!(encoder->possible_crtcs & (1 << j)))
					continue;
				crtc_id = drm->resources->crtcs[j];
				break;
			}
			if (crtc_id >= 0) {
				drmModeFreeEncoder(encoder);
				return drmModeGetCrtc(drm->fd, crtc_id);
			}
		}
	}

	return NULL;
}

static void drm_disable_non_main_crtcs(drm_t* drm)
{
	int i;
	drmModeCrtc* crtc;

	for (i = 0; i < drm->resources->count_connectors; i++) {
		drmModeConnector* connector;

		connector = drmModeGetConnector(drm->fd, drm->resources->connectors[i]);
		crtc = find_crtc_for_connector(drm, connector);
		if (crtc->crtc_id != drm->crtc->crtc_id)
			drm_disable_crtc(drm, crtc);
		drmModeFreeCrtc(crtc);
	}
}

static int drm_is_primary_plane(drm_t* drm, uint32_t plane_id)
{
	uint32_t p;
	bool found = false;
	int ret = -1;

	drmModeObjectPropertiesPtr props;
	props = drmModeObjectGetProperties(drm->fd,
					   plane_id,
					   DRM_MODE_OBJECT_PLANE);
	if (!props) {
		LOG(ERROR, "Unable to get plane properties: %m");
		return -1;
	}

	for (p = 0; p < props->count_props && !found; p++) {
		drmModePropertyPtr prop;
		prop = drmModeGetProperty(drm->fd, props->props[p]);
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
static void drm_disable_non_primary_planes(drm_t* drm)
{
	int ret;

	if (!drm->plane_resources)
		return;

	for (uint32_t p = 0; p < drm->plane_resources->count_planes; p++) {
		drmModePlanePtr plane;
		plane = drmModeGetPlane(drm->fd,
					drm->plane_resources->planes[p]);
		if (plane) {
			int primary = drm_is_primary_plane(drm, plane->plane_id);
			if (!(plane->crtc_id == drm->crtc->crtc_id && primary != 0)) {
				ret = drmModeSetPlane(drm->fd, plane->plane_id, plane->crtc_id,
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

static bool drm_is_internal(unsigned type)
{
	unsigned t;
	unsigned kInternalConnectors[] = {
		DRM_MODE_CONNECTOR_LVDS,
		DRM_MODE_CONNECTOR_eDP,
		DRM_MODE_CONNECTOR_DSI,
	};
	for (t = 0; t < ARRAY_SIZE(kInternalConnectors); t++)
		if (type == kInternalConnectors[t])
			return true;
	return false;
}

static drmModeConnector* find_first_connected_connector(drm_t* drm, bool internal, bool external)
{
	for (int i = 0; i < drm->resources->count_connectors; i++) {
		drmModeConnector* connector;

		connector = drmModeGetConnector(drm->fd, drm->resources->connectors[i]);
		if (connector) {
			bool is_internal = drm_is_internal(connector->connector_type);
			if (!internal && is_internal)
				continue;
			if (!external && !is_internal)
				continue;
			if ((connector->count_modes > 0) &&
					(connector->connection == DRM_MODE_CONNECTED))
				return connector;

			drmModeFreeConnector(connector);
		}
	}
	return NULL;
}

static drmModeConnector* find_main_monitor(drm_t* drm, uint32_t* mode_index)
{
	int modes;
	int lid_state = input_check_lid_state();
	drmModeConnector* main_monitor_connector = NULL;

	/*
	 * Find the LVDS/eDP/DSI connectors. Those are the main screens.
	 */
	if (lid_state <= 0)
		main_monitor_connector = find_first_connected_connector(drm, true, false);

	/*
	 * Now try external connectors.
	 */
	if (!main_monitor_connector)
		main_monitor_connector =
				find_first_connected_connector(drm, false, true);

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

static void drm_fini(drm_t* drm)
{
	if (!drm)
		return;

	if (drm->fd >= 0) {
		if (drm->crtc) {
			int32_t ret;

			ret = drmSetMaster(drm->fd);
			if (ret)
				LOG(ERROR, "drmSetMaster in fini failed: %m");
			drm_disable_crtc(drm, drm->crtc);
			drmModeFreeCrtc(drm->crtc);
			drm->crtc = NULL;
		}

		if (drm->main_monitor_connector) {
		drmModeFreeConnector(drm->main_monitor_connector);
			drm->main_monitor_connector = NULL;
		}

		if (drm->plane_resources) {
			drmModeFreePlaneResources(drm->plane_resources);
			drm->plane_resources = NULL;
		}

		if (drm->resources) {
			drmModeFreeResources(drm->resources);
			drm->resources = NULL;
		}

		drmClose(drm->fd);
		drm->fd = -1;
	}

	free(drm);
}

static bool drm_equal(drm_t* l, drm_t* r)
{
	if (!l && !r)
		return true;
	if ((!l && r) || (l && !r))
		return false;
	if (!l->crtc && r->crtc)
		return false;
	if (l->crtc && !r->crtc)
		return false;
	if (l->crtc && r->crtc)
		if (l->crtc->crtc_id != r->crtc->crtc_id)
			return false;

	if (!l->main_monitor_connector && r->main_monitor_connector)
		return false;
	if (l->main_monitor_connector && !r->main_monitor_connector)
		return false;
	if (l->main_monitor_connector && r->main_monitor_connector)
		if (l->main_monitor_connector->connector_id != r->main_monitor_connector->connector_id)
			return false;
	return true;
}

static int drm_score(drm_t* drm)
{
	drmVersionPtr version;
	int score = 0;

	if (!drm)
		return -1000000000;

	if (!drm->main_monitor_connector)
		return -1000000000;

	if (drm_is_internal(drm->main_monitor_connector->connector_type))
		score++;

	version = drmGetVersion(drm->fd);
	if (version) {
		/* we would rather use any driver besides UDL */
		if (strcmp("udl", version->name) == 0)
			score--;
		if (strcmp("evdi", version->name) == 0)
			score--;
		/* VGEM should be ignored because it has no displays, but lets make sure */
		if (strcmp("vgem", version->name) == 0)
			score -= 1000000;
		drmFreeVersion(version);
	}
	return score;
}

drm_t* drm_scan(void)
{
	unsigned i;
	char* dev_name;
	int ret;
	drm_t *best_drm = NULL;

	for (i = 0; i < DRM_MAX_MINOR; i++) {
		drm_t* drm = calloc(1, sizeof(drm_t));

		if (!drm)
			return NULL;

		ret = asprintf(&dev_name, DRM_DEV_NAME, DRM_DIR_NAME, i);
		if (ret < 0) {
			drm_fini(drm);
			continue;
		}

		drm->fd = open(dev_name, O_RDWR, 0);
		free(dev_name);
		if (drm->fd < 0) {
			drm_fini(drm);
			continue;
		}

		drm->resources = drmModeGetResources(drm->fd);
		if (!drm->resources) {
			drm_fini(drm);
			continue;
		}

		/* expect at least one crtc so we do not try to run on VGEM */
		if (drm->resources->count_crtcs == 0 || drm->resources->count_connectors == 0) {
			drm_fini(drm);
			continue;
		}

		drm->main_monitor_connector = find_main_monitor(drm, &drm->selected_mode);
		if (!drm->main_monitor_connector) {
			drm_fini(drm);
			continue;
		}

		drm->crtc = find_crtc_for_connector(drm, drm->main_monitor_connector);
		if (!drm->crtc) {
			drm_fini(drm);
			continue;
		}

		drm->crtc->mode = drm->main_monitor_connector->modes[drm->selected_mode];

		drm->plane_resources = drmModeGetPlaneResources(drm->fd);
		drm->refcount = 1;

		if (drm_score(drm) > drm_score(best_drm)) {
			drm_fini(best_drm);
			best_drm = drm;
		}
	}

	if (best_drm) {
		drmVersionPtr version;
		version = drmGetVersion(best_drm->fd);
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
		drmDropMaster(best_drm->fd);
	}

	return best_drm;
}

void drm_set(drm_t* drm_)
{
	if (drm) {
		drm_delref(drm);
		drm = NULL;
	}
	drm = drm_;
}

void drm_close(void)
{
	if (drm) {
		drm_delref(drm);
		drm = NULL;
	}
}

void drm_delref(drm_t *drm)
{
	if (drm->refcount) {
		drm->refcount--;
	} else {
		LOG(ERROR, "Imbalanced drm_close()");
	}
	if (drm->refcount) {
		return;
	}

	drm_fini(drm);
}

drm_t* drm_addref(void)
{
	if (drm) {
		drm->refcount++;
		return drm;
	}

	return NULL;
}

/*
 * returns true if connector/crtc/driver have changed and framebuffer object have to be re-create
 */
bool drm_rescan(void)
{
	drm_t* ndrm;

	ndrm = drm_scan();
	if (ndrm) {
		if (drm_equal(ndrm, drm)) {
			drm_fini(ndrm);
		} else {
			drm_delref(drm);
			drm = ndrm;
			return true;
		}
	} else {
		if (drm) {
			drm_delref(drm); /* no usable monitor/drm object */
			drm = NULL;
			return true;
		}
	}
	return false;
}

bool drm_valid(drm_t* drm) {
	return drm && drm->fd >= 0 && drm->resources && drm->main_monitor_connector && drm->crtc;
}

int32_t drm_setmode(drm_t* drm, uint32_t fb_id)
{
	int32_t ret;

	ret = drmSetMaster(drm->fd);
	if (ret)
		LOG(ERROR, "drmSetMaster failed: %m");

	ret = drmModeSetCrtc(drm->fd, drm->crtc->crtc_id,
			     fb_id,
			     0, 0,  // x,y
			     &drm->main_monitor_connector->connector_id,
			     1,  // connector_count
			     &drm->crtc->mode); // mode

	if (ret) {
		LOG(ERROR, "Unable to set crtc: %m");
		drmDropMaster(drm->fd);
		return ret;
	}

	ret = drmModeSetCursor(drm->fd, drm->crtc->crtc_id,
			0, 0, 0);

	if (ret)
		LOG(ERROR, "Unable to hide cursor");

	drm_disable_non_primary_planes(drm);
	drm_disable_non_main_crtcs(drm);
	drmDropMaster(drm->fd);
	return ret;
}

bool drm_read_edid(drm_t* drm)
{
	if (drm->edid_found) {
		return true;
	}

	for (int i = 0; i < drm->main_monitor_connector->count_props; i++) {
		drmModePropertyPtr prop;
		drmModePropertyBlobPtr blob_ptr;
		prop = drmModeGetProperty(drm->fd, drm->main_monitor_connector->props[i]);
		if (prop) {
			if (strcmp(prop->name, "EDID") == 0) {
				blob_ptr = drmModeGetPropertyBlob(drm->fd,
					drm->main_monitor_connector->prop_values[i]);
				if (blob_ptr) {
					memcpy(&drm->edid, blob_ptr->data, EDID_SIZE);
					drmModeFreePropertyBlob(blob_ptr);
					return (drm->edid_found = true);
				}
			}
		}
	}

	return false;
}

uint32_t drm_gethres(drm_t* drm)
{
	return drm->crtc->mode.hdisplay;
}

uint32_t drm_getvres(drm_t* drm)
{
	return drm->crtc->mode.vdisplay;
}
