/*
 * Copyright © 2015 Intel Corporation
 *
 * Permission is hereby granted, free of charge, to any person obtaining a
 * copy of this software and associated documentation files (the "Software"),
 * to deal in the Software without restriction, including without limitation
 * the rights to use, copy, modify, merge, publish, distribute, sublicense,
 * and/or sell copies of the Software, and to permit persons to whom the
 * Software is furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice (including the next
 * paragraph) shall be included in all copies or substantial portions of the
 * Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.  IN NO EVENT SHALL
 * THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING
 * FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS
 * IN THE SOFTWARE.
 *
 * Authors:
 *    Tiago Vignatti <tiago.vignatti at intel.com>
 *
 */

/* This program can detect when the writes don't land in scanout, due cache
 * incoherency. */

#include "drmtest.h"
#include "igt_debugfs.h"
#include "igt_kms.h"

#define MAX_CONNECTORS 32

struct modeset_params {
	uint32_t crtc_id;
	uint32_t connector_id;
	drmModeModeInfoPtr mode;
};

int drm_fd;
drmModeResPtr drm_res;
drmModeConnectorPtr drm_connectors[MAX_CONNECTORS];
drm_intel_bufmgr *bufmgr;
igt_pipe_crc_t *pipe_crc;

struct modeset_params ms;

static void find_modeset_params(void)
{
	int i;
	uint32_t connector_id = 0, crtc_id;
	drmModeModeInfoPtr mode = NULL;

	for (i = 0; i < drm_res->count_connectors; i++) {
		drmModeConnectorPtr c = drm_connectors[i];

		if (c->count_modes) {
			connector_id = c->connector_id;
			mode = &c->modes[0];
			break;
		}
	}
	igt_require(connector_id);

	crtc_id = drm_res->crtcs[0];
	igt_assert(crtc_id);
	igt_assert(mode);

	ms.connector_id = connector_id;
	ms.crtc_id = crtc_id;
	ms.mode = mode;

}

#define BO_SIZE (16*1024)

char pattern[] = {0xff, 0x00, 0x00, 0x00,
	0x00, 0xff, 0x00, 0x00,
	0x00, 0x00, 0xff, 0x00,
	0x00, 0x00, 0x00, 0xff};

static void mess_with_coherency(char *ptr)
{
	off_t i;

	for (i = 0; i < BO_SIZE; i+=sizeof(pattern)) {
		memcpy(ptr + i, pattern, sizeof(pattern));
	}
//	munmap(ptr, BO_SIZE);
//	close(dma_buf_fd);
}

static char *dmabuf_mmap_framebuffer(struct igt_fb *fb)
{
	int dma_buf_fd;
	char *ptr = NULL;

	dma_buf_fd = prime_handle_to_fd(drm_fd, fb->gem_handle);
	igt_assert(errno == 0);

	ptr = mmap(NULL, BO_SIZE, PROT_READ | PROT_WRITE, MAP_SHARED, dma_buf_fd, 0);
	igt_assert(ptr != MAP_FAILED);

	return ptr;
}

static void get_method_crc(uint64_t tiling, igt_crc_t *crc, bool mess)
{
	struct igt_fb fb;
	int rc;
	char *ptr;

	igt_create_fb(drm_fd, ms.mode->hdisplay, ms.mode->vdisplay,
		      DRM_FORMAT_XRGB8888, tiling, &fb);

	if (mess)
		ptr = dmabuf_mmap_framebuffer(&fb);

	rc = drmModeSetCrtc(drm_fd, ms.crtc_id, fb.fb_id, 0, 0,
			    &ms.connector_id, 1, ms.mode);
	igt_assert(rc == 0);

	if (mess)
		mess_with_coherency(ptr);

	igt_pipe_crc_collect_crc(pipe_crc, crc);

	kmstest_unset_all_crtcs(drm_fd, drm_res);
	igt_remove_fb(drm_fd, &fb);
}

static void draw_method_subtest(uint64_t tiling)
{
	igt_crc_t reference_crc, crc;

	kmstest_unset_all_crtcs(drm_fd, drm_res);

	find_modeset_params();

	get_method_crc(tiling, &reference_crc, false);
	get_method_crc(tiling, &crc, true);

	// XXX: IIUC if we mess up with the scanout device, through a dma-buf mmap'ed
	// pointer, then both the reference crc and the messed up one should be equal
	// because the latter wasn't flushed. That's the theory, but it's not what's
	// happening and the following is not passing.
	igt_assert_crc_equal(&reference_crc, &crc);
}

static void setup_environment(void)
{
	int i;

	drm_fd = drm_open_any_master();
	igt_require(drm_fd >= 0);

	drm_res = drmModeGetResources(drm_fd);
	igt_assert(drm_res->count_connectors <= MAX_CONNECTORS);

	for (i = 0; i < drm_res->count_connectors; i++)
		drm_connectors[i] = drmModeGetConnector(drm_fd,
							drm_res->connectors[i]);

	kmstest_set_vt_graphics_mode();

	bufmgr = drm_intel_bufmgr_gem_init(drm_fd, 4096);
	igt_assert(bufmgr);
	drm_intel_bufmgr_gem_enable_reuse(bufmgr);

	pipe_crc = igt_pipe_crc_new(0, INTEL_PIPE_CRC_SOURCE_AUTO);
}

static void teardown_environment(void)
{
	int i;

	igt_pipe_crc_free(pipe_crc);

	drm_intel_bufmgr_destroy(bufmgr);

	for (i = 0; i < drm_res->count_connectors; i++)
		drmModeFreeConnector(drm_connectors[i]);

	drmModeFreeResources(drm_res);
	close(drm_fd);
}

igt_main
{
	igt_fixture
		setup_environment();

	igt_subtest_f("draw-method-tiled")
		draw_method_subtest(LOCAL_I915_FORMAT_MOD_X_TILED);

	igt_fixture
		teardown_environment();
}
