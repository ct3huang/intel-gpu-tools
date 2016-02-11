/*
 * Copyright © 2009,2012,2013 Intel Corporation
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
 *    Eric Anholt <eric@anholt.net>
 *    Chris Wilson <chris@chris-wilson.co.uk>
 *    Daniel Vetter <daniel.vetter@ffwll.ch>
 *
 */

/** @file gem_concurrent.c
 *
 * This is a test of pread/pwrite/mmap behavior when writing to active
 * buffers.
 *
 * Based on gem_gtt_concurrent_blt.
 */

#include "igt.h"
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <fcntl.h>
#include <inttypes.h>
#include <errno.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <sys/wait.h>

#include <drm.h>

#include "intel_bufmgr.h"

IGT_TEST_DESCRIPTION("Test of pread/pwrite/mmap behavior when writing to active"
		     " buffers.");

int fd, devid, gen;
struct intel_batchbuffer *batch;
int all;
int pass;

struct buffers {
	const struct access_mode *mode;
	drm_intel_bufmgr *bufmgr;
	drm_intel_bo **src, **dst;
	drm_intel_bo *snoop, *spare;
	uint32_t *tmp;
	int width, height, size;
	int count;
};

#define MIN_BUFFERS 3

static void blt_copy_bo(struct buffers *b, drm_intel_bo *dst, drm_intel_bo *src);

static void
nop_release_bo(drm_intel_bo *bo)
{
	drm_intel_bo_unreference(bo);
}

static void
prw_set_bo(struct buffers *b, drm_intel_bo *bo, uint32_t val)
{
	for (int i = 0; i < b->size; i++)
		b->tmp[i] = val;
	drm_intel_bo_subdata(bo, 0, 4*b->size, b->tmp);
}

static void
prw_cmp_bo(struct buffers *b, drm_intel_bo *bo, uint32_t val)
{
	uint32_t *vaddr;

	vaddr = b->tmp;
	do_or_die(drm_intel_bo_get_subdata(bo, 0, 4*b->size, vaddr));
	for (int i = 0; i < b->size; i++)
		igt_assert_eq_u32(vaddr[i], val);
}

#define pixel(y, width) ((y)*(width) + (((y) + pass)%(width)))

static void
partial_set_bo(struct buffers *b, drm_intel_bo *bo, uint32_t val)
{
	for (int y = 0; y < b->height; y++)
		do_or_die(drm_intel_bo_subdata(bo, 4*pixel(y, b->width), 4, &val));
}

static void
partial_cmp_bo(struct buffers *b, drm_intel_bo *bo, uint32_t val)
{
	for (int y = 0; y < b->height; y++) {
		uint32_t buf;
		do_or_die(drm_intel_bo_get_subdata(bo, 4*pixel(y, b->width), 4, &buf));
		igt_assert_eq_u32(buf, val);
	}
}

static drm_intel_bo *
create_normal_bo(drm_intel_bufmgr *bufmgr, uint64_t size)
{
	drm_intel_bo *bo;

	bo = drm_intel_bo_alloc(bufmgr, "bo", size, 0);
	igt_assert(bo);

	return bo;
}

static bool can_create_normal(void)
{
	return true;
}

static drm_intel_bo *
create_private_bo(drm_intel_bufmgr *bufmgr, uint64_t size)
{
	drm_intel_bo *bo;
	uint32_t handle;

	/* XXX gem_create_with_flags(fd, size, I915_CREATE_PRIVATE); */

	handle = gem_create(fd, size);
	bo = gem_handle_to_libdrm_bo(bufmgr, fd, "stolen", handle);
	gem_close(fd, handle);

	return bo;
}

static bool can_create_private(void)
{
	return false;
}

static drm_intel_bo *
create_stolen_bo(drm_intel_bufmgr *bufmgr, uint64_t size)
{
	drm_intel_bo *bo;
	uint32_t handle;

	/* XXX gem_create_with_flags(fd, size, I915_CREATE_STOLEN); */

	handle = gem_create(fd, size);
	bo = gem_handle_to_libdrm_bo(bufmgr, fd, "stolen", handle);
	gem_close(fd, handle);

	return bo;
}

static bool can_create_stolen(void)
{
	/* XXX check num_buffers against available stolen */
	return false;
}

static drm_intel_bo *
(*create_func)(drm_intel_bufmgr *bufmgr, uint64_t size);

static bool create_cpu_require(void)
{
	return create_func != create_stolen_bo;
}

static drm_intel_bo *
unmapped_create_bo(drm_intel_bufmgr *bufmgr, int width, int height)
{
	return create_func(bufmgr, (uint64_t)4*width*height);
}

static bool create_snoop_require(void)
{
	if (!create_cpu_require())
		return false;

	return !gem_has_llc(fd);
}

static drm_intel_bo *
snoop_create_bo(drm_intel_bufmgr *bufmgr, int width, int height)
{
	drm_intel_bo *bo;

	bo = unmapped_create_bo(bufmgr, width, height);
	gem_set_caching(fd, bo->handle, I915_CACHING_CACHED);
	drm_intel_bo_disable_reuse(bo);

	return bo;
}

static bool create_userptr_require(void)
{
	static int found = -1;
	if (found < 0) {
		struct drm_i915_gem_userptr arg;

		found = 0;

		memset(&arg, 0, sizeof(arg));
		arg.user_ptr = -4096ULL;
		arg.user_size = 8192;
		errno = 0;
		drmIoctl(fd, LOCAL_IOCTL_I915_GEM_USERPTR, &arg);
		if (errno == EFAULT) {
			igt_assert(posix_memalign((void **)&arg.user_ptr,
						  4096, arg.user_size) == 0);
			found = drmIoctl(fd,
					 LOCAL_IOCTL_I915_GEM_USERPTR,
					 &arg) == 0;
			free((void *)(uintptr_t)arg.user_ptr);
		}

	}
	return found;
}

static drm_intel_bo *
userptr_create_bo(drm_intel_bufmgr *bufmgr, int width, int height)
{
	struct local_i915_gem_userptr userptr;
	drm_intel_bo *bo;
	void *ptr;

	memset(&userptr, 0, sizeof(userptr));
	userptr.user_size = width * height * 4;
	userptr.user_size = (userptr.user_size + 4095) & -4096;

	ptr = mmap(NULL, userptr.user_size,
		   PROT_READ | PROT_WRITE, MAP_ANON | MAP_SHARED, -1, 0);
	igt_assert(ptr != (void *)-1);
	userptr.user_ptr = (uintptr_t)ptr;

	do_or_die(drmIoctl(fd, LOCAL_IOCTL_I915_GEM_USERPTR, &userptr));
	bo = gem_handle_to_libdrm_bo(bufmgr, fd, "userptr", userptr.handle);
	bo->virtual = (void *)(uintptr_t)userptr.user_ptr;
	gem_close(fd, userptr.handle);

	return bo;
}

static void
userptr_set_bo(struct buffers *b, drm_intel_bo *bo, uint32_t val)
{
	int size = b->size;
	uint32_t *vaddr = bo->virtual;

	gem_set_domain(fd, bo->handle,
		       I915_GEM_DOMAIN_CPU, I915_GEM_DOMAIN_CPU);
	while (size--)
		*vaddr++ = val;
}

static void
userptr_cmp_bo(struct buffers *b, drm_intel_bo *bo, uint32_t val)
{
	int size =  b->size;
	uint32_t *vaddr = bo->virtual;

	gem_set_domain(fd, bo->handle,
		       I915_GEM_DOMAIN_CPU, 0);
	while (size--)
		igt_assert_eq_u32(*vaddr++, val);
}

static void
userptr_release_bo(drm_intel_bo *bo)
{
	munmap(bo->virtual, bo->size);
	bo->virtual = NULL;

	drm_intel_bo_unreference(bo);
}

static void
gtt_set_bo(struct buffers *b, drm_intel_bo *bo, uint32_t val)
{
	uint32_t *vaddr = bo->virtual;
	int size = b->size;

	drm_intel_gem_bo_start_gtt_access(bo, true);
	while (size--)
		*vaddr++ = val;
}

static void
gtt_cmp_bo(struct buffers *b, drm_intel_bo *bo, uint32_t val)
{
	uint32_t *vaddr = bo->virtual;

	/* GTT access is slow. So we just compare a few points */
	drm_intel_gem_bo_start_gtt_access(bo, false);
	for (int y = 0; y < b->height; y++)
		igt_assert_eq_u32(vaddr[pixel(y, b->width)], val);
}

static drm_intel_bo *
map_bo(drm_intel_bo *bo)
{
	/* gtt map doesn't have a write parameter, so just keep the mapping
	 * around (to avoid the set_domain with the gtt write domain set) and
	 * manually tell the kernel when we start access the gtt. */
	do_or_die(drm_intel_gem_bo_map_gtt(bo));

	return bo;
}

static drm_intel_bo *
tile_bo(drm_intel_bo *bo, int width)
{
	uint32_t tiling = I915_TILING_X;
	uint32_t stride = width * 4;

	do_or_die(drm_intel_bo_set_tiling(bo, &tiling, stride));

	return bo;
}

static drm_intel_bo *
gtt_create_bo(drm_intel_bufmgr *bufmgr, int width, int height)
{
	return map_bo(unmapped_create_bo(bufmgr, width, height));
}

static drm_intel_bo *
gttX_create_bo(drm_intel_bufmgr *bufmgr, int width, int height)
{
	return tile_bo(gtt_create_bo(bufmgr, width, height), width);
}

static drm_intel_bo *
wc_create_bo(drm_intel_bufmgr *bufmgr, int width, int height)
{
	drm_intel_bo *bo;

	gem_require_mmap_wc(fd);

	bo = unmapped_create_bo(bufmgr, width, height);
	bo->virtual = __gem_mmap__wc(fd, bo->handle, 0, bo->size, PROT_READ | PROT_WRITE);
	return bo;
}

static void
wc_release_bo(drm_intel_bo *bo)
{
	munmap(bo->virtual, bo->size);
	bo->virtual = NULL;

	nop_release_bo(bo);
}

static drm_intel_bo *
gpu_create_bo(drm_intel_bufmgr *bufmgr, int width, int height)
{
	return unmapped_create_bo(bufmgr, width, height);
}

static drm_intel_bo *
gpuX_create_bo(drm_intel_bufmgr *bufmgr, int width, int height)
{
	return tile_bo(gpu_create_bo(bufmgr, width, height), width);
}

static void
cpu_set_bo(struct buffers *b, drm_intel_bo *bo, uint32_t val)
{
	int size = b->size;
	uint32_t *vaddr;

	do_or_die(drm_intel_bo_map(bo, true));
	vaddr = bo->virtual;
	while (size--)
		*vaddr++ = val;
	drm_intel_bo_unmap(bo);
}

static void
cpu_cmp_bo(struct buffers *b, drm_intel_bo *bo, uint32_t val)
{
	int size = b->size;
	uint32_t *vaddr;

	do_or_die(drm_intel_bo_map(bo, false));
	vaddr = bo->virtual;
	while (size--)
		igt_assert_eq_u32(*vaddr++, val);
	drm_intel_bo_unmap(bo);
}

static void
gpu_set_bo(struct buffers *buffers, drm_intel_bo *bo, uint32_t val)
{
	struct drm_i915_gem_relocation_entry reloc[1];
	struct drm_i915_gem_exec_object2 gem_exec[2];
	struct drm_i915_gem_execbuffer2 execbuf;
	uint32_t buf[10], *b;
	uint32_t tiling, swizzle;

	drm_intel_bo_get_tiling(bo, &tiling, &swizzle);

	memset(reloc, 0, sizeof(reloc));
	memset(gem_exec, 0, sizeof(gem_exec));
	memset(&execbuf, 0, sizeof(execbuf));

	b = buf;
	*b++ = XY_COLOR_BLT_CMD_NOLEN |
		((gen >= 8) ? 5 : 4) |
		COLOR_BLT_WRITE_ALPHA | XY_COLOR_BLT_WRITE_RGB;
	if (gen >= 4 && tiling) {
		b[-1] |= XY_COLOR_BLT_TILED;
		*b = buffers->width;
	} else
		*b = buffers->width << 2;
	*b++ |= 0xf0 << 16 | 1 << 25 | 1 << 24;
	*b++ = 0;
	*b++ = buffers->height << 16 | buffers->width;
	reloc[0].offset = (b - buf) * sizeof(uint32_t);
	reloc[0].target_handle = bo->handle;
	reloc[0].read_domains = I915_GEM_DOMAIN_RENDER;
	reloc[0].write_domain = I915_GEM_DOMAIN_RENDER;
	*b++ = 0;
	if (gen >= 8)
		*b++ = 0;
	*b++ = val;
	*b++ = MI_BATCH_BUFFER_END;
	if ((b - buf) & 1)
		*b++ = 0;

	gem_exec[0].handle = bo->handle;
	gem_exec[0].flags = EXEC_OBJECT_NEEDS_FENCE;

	gem_exec[1].handle = gem_create(fd, 4096);
	gem_exec[1].relocation_count = 1;
	gem_exec[1].relocs_ptr = (uintptr_t)reloc;

	execbuf.buffers_ptr = (uintptr_t)gem_exec;
	execbuf.buffer_count = 2;
	execbuf.batch_len = (b - buf) * sizeof(buf[0]);
	if (gen >= 6)
		execbuf.flags = I915_EXEC_BLT;

	gem_write(fd, gem_exec[1].handle, 0, buf, execbuf.batch_len);
	gem_execbuf(fd, &execbuf);

	gem_close(fd, gem_exec[1].handle);
}

static void
gpu_cmp_bo(struct buffers *b, drm_intel_bo *bo, uint32_t val)
{
	blt_copy_bo(b, b->snoop, bo);
	cpu_cmp_bo(b, b->snoop, val);
}

const struct access_mode {
	const char *name;
	bool (*require)(void);
	void (*set_bo)(struct buffers *b, drm_intel_bo *bo, uint32_t val);
	void (*cmp_bo)(struct buffers *b, drm_intel_bo *bo, uint32_t val);
	drm_intel_bo *(*create_bo)(drm_intel_bufmgr *bufmgr, int width, int height);
	void (*release_bo)(drm_intel_bo *bo);
} access_modes[] = {
	{
		.name = "prw",
		.set_bo = prw_set_bo,
		.cmp_bo = prw_cmp_bo,
		.create_bo = unmapped_create_bo,
		.release_bo = nop_release_bo,
	},
	{
		.name = "partial",
		.set_bo = partial_set_bo,
		.cmp_bo = partial_cmp_bo,
		.create_bo = unmapped_create_bo,
		.release_bo = nop_release_bo,
	},
	{
		.name = "cpu",
		.require = create_cpu_require,
		.set_bo = cpu_set_bo,
		.cmp_bo = cpu_cmp_bo,
		.create_bo = unmapped_create_bo,
		.release_bo = nop_release_bo,
	},
	{
		.name = "snoop",
		.require = create_snoop_require,
		.set_bo = cpu_set_bo,
		.cmp_bo = cpu_cmp_bo,
		.create_bo = snoop_create_bo,
		.release_bo = nop_release_bo,
	},
	{
		.name = "userptr",
		.require = create_userptr_require,
		.set_bo = userptr_set_bo,
		.cmp_bo = userptr_cmp_bo,
		.create_bo = userptr_create_bo,
		.release_bo = userptr_release_bo,
	},
	{
		.name = "gtt",
		.set_bo = gtt_set_bo,
		.cmp_bo = gtt_cmp_bo,
		.create_bo = gtt_create_bo,
		.release_bo = nop_release_bo,
	},
	{
		.name = "gttX",
		.set_bo = gtt_set_bo,
		.cmp_bo = gtt_cmp_bo,
		.create_bo = gttX_create_bo,
		.release_bo = nop_release_bo,
	},
	{
		.name = "wc",
		.set_bo = gtt_set_bo,
		.cmp_bo = gtt_cmp_bo,
		.create_bo = wc_create_bo,
		.release_bo = wc_release_bo,
	},
	{
		.name = "gpu",
		.set_bo = gpu_set_bo,
		.cmp_bo = gpu_cmp_bo,
		.create_bo = gpu_create_bo,
		.release_bo = nop_release_bo,
	},
	{
		.name = "gpuX",
		.set_bo = gpu_set_bo,
		.cmp_bo = gpu_cmp_bo,
		.create_bo = gpuX_create_bo,
		.release_bo = nop_release_bo,
	},
};

int num_buffers;
igt_render_copyfunc_t rendercopy;

static void *buffers_init(struct buffers *data,
			  const struct access_mode *mode,
			  int width, int height,
			  int _fd, int enable_reuse)
{
	data->mode = mode;
	data->count = 0;

	data->width = width;
	data->height = height;
	data->size = width * height;
	data->tmp = malloc(4*data->size);
	igt_assert(data->tmp);

	data->bufmgr = drm_intel_bufmgr_gem_init(_fd, 4096);
	igt_assert(data->bufmgr);

	data->src = malloc(2*sizeof(drm_intel_bo *)*num_buffers);
	igt_assert(data->src);
	data->dst = data->src + num_buffers;

	if (enable_reuse)
		drm_intel_bufmgr_gem_enable_reuse(data->bufmgr);
	return intel_batchbuffer_alloc(data->bufmgr, devid);
}

static void buffers_destroy(struct buffers *data)
{
	if (data->count == 0)
		return;

	for (int i = 0; i < data->count; i++) {
		data->mode->release_bo(data->src[i]);
		data->mode->release_bo(data->dst[i]);
	}
	data->mode->release_bo(data->snoop);
	data->mode->release_bo(data->spare);
	data->count = 0;
}

static void buffers_create(struct buffers *data,
			   int count)
{
	int width = data->width, height = data->height;
	igt_assert(data->bufmgr);

	buffers_destroy(data);

	for (int i = 0; i < count; i++) {
		data->src[i] =
			data->mode->create_bo(data->bufmgr, width, height);
		data->dst[i] =
			data->mode->create_bo(data->bufmgr, width, height);
	}
	data->spare = data->mode->create_bo(data->bufmgr, width, height);
	data->snoop = snoop_create_bo(data->bufmgr, width, height);
	data->count = count;
}

static void buffers_fini(struct buffers *data)
{
	if (data->bufmgr == NULL)
		return;

	buffers_destroy(data);

	free(data->tmp);
	free(data->src);
	data->src = NULL;
	data->dst = NULL;

	intel_batchbuffer_free(batch);
	drm_intel_bufmgr_destroy(data->bufmgr);
	data->bufmgr = NULL;
}

typedef void (*do_copy)(struct buffers *b, drm_intel_bo *dst, drm_intel_bo *src);
typedef struct igt_hang_ring (*do_hang)(void);

static void render_copy_bo(struct buffers *b, drm_intel_bo *dst, drm_intel_bo *src)
{
	struct igt_buf d = {
		.bo = dst,
		.size = b->size * 4,
		.num_tiles = b->size * 4,
		.stride = b->width * 4,
	}, s = {
		.bo = src,
		.size = b->size * 4,
		.num_tiles = b->size * 4,
		.stride = b->width * 4,
	};
	uint32_t swizzle;

	drm_intel_bo_get_tiling(dst, &d.tiling, &swizzle);
	drm_intel_bo_get_tiling(src, &s.tiling, &swizzle);

	rendercopy(batch, NULL,
		   &s, 0, 0,
		   b->width, b->height,
		   &d, 0, 0);
}

static void blt_copy_bo(struct buffers *b, drm_intel_bo *dst, drm_intel_bo *src)
{
	intel_blt_copy(batch,
		       src, 0, 0, 4*b->width,
		       dst, 0, 0, 4*b->width,
		       b->width, b->height, 32);
}

static void cpu_copy_bo(struct buffers *b, drm_intel_bo *dst, drm_intel_bo *src)
{
	const int size = b->size * sizeof(uint32_t);
	void *d, *s;

	gem_set_domain(fd, src->handle, I915_GEM_DOMAIN_CPU, 0);
	gem_set_domain(fd, dst->handle, I915_GEM_DOMAIN_CPU, I915_GEM_DOMAIN_CPU);
	s = gem_mmap__cpu(fd, src->handle, 0, size, PROT_READ);
	d = gem_mmap__cpu(fd, dst->handle, 0, size, PROT_WRITE);

	memcpy(d, s, size);

	munmap(d, size);
	munmap(s, size);
}

static void gtt_copy_bo(struct buffers *b, drm_intel_bo *dst, drm_intel_bo *src)
{
	const int size = b->size * sizeof(uint32_t);
	void *d, *s;

	gem_set_domain(fd, src->handle, I915_GEM_DOMAIN_GTT, 0);
	gem_set_domain(fd, dst->handle, I915_GEM_DOMAIN_GTT, I915_GEM_DOMAIN_GTT);

	s = gem_mmap__gtt(fd, src->handle, size, PROT_READ);
	d = gem_mmap__gtt(fd, dst->handle, size, PROT_WRITE);

	memcpy(d, s, size);

	munmap(d, size);
	munmap(s, size);
}

static void wc_copy_bo(struct buffers *b, drm_intel_bo *dst, drm_intel_bo *src)
{
	const int size = b->width * sizeof(uint32_t);
	void *d, *s;

	gem_set_domain(fd, src->handle, I915_GEM_DOMAIN_GTT, 0);
	gem_set_domain(fd, dst->handle, I915_GEM_DOMAIN_GTT, I915_GEM_DOMAIN_GTT);

	s = gem_mmap__wc(fd, src->handle, 0, size, PROT_READ);
	d = gem_mmap__wc(fd, dst->handle, 0, size, PROT_WRITE);

	memcpy(d, s, size);

	munmap(d, size);
	munmap(s, size);
}

static struct igt_hang_ring no_hang(void)
{
	return (struct igt_hang_ring){0, 0};
}

static struct igt_hang_ring bcs_hang(void)
{
	return igt_hang_ring(fd, I915_EXEC_BLT);
}

static struct igt_hang_ring rcs_hang(void)
{
	return igt_hang_ring(fd, I915_EXEC_RENDER);
}

static void do_basic0(struct buffers *buffers,
		      do_copy do_copy_func,
		      do_hang do_hang_func)
{
	gem_quiescent_gpu(fd);

	buffers->mode->set_bo(buffers, buffers->src[0], 0xdeadbeef);
	for (int i = 0; i < buffers->count; i++) {
		struct igt_hang_ring hang = do_hang_func();

		do_copy_func(buffers, buffers->dst[i], buffers->src[0]);
		buffers->mode->cmp_bo(buffers, buffers->dst[i], 0xdeadbeef);

		igt_post_hang_ring(fd, hang);
	}
}

static void do_basic1(struct buffers *buffers,
		      do_copy do_copy_func,
		      do_hang do_hang_func)
{
	gem_quiescent_gpu(fd);

	for (int i = 0; i < buffers->count; i++) {
		struct igt_hang_ring hang = do_hang_func();

		buffers->mode->set_bo(buffers, buffers->src[i], i);
		buffers->mode->set_bo(buffers, buffers->dst[i], ~i);

		do_copy_func(buffers, buffers->dst[i], buffers->src[i]);
		usleep(0); /* let someone else claim the mutex */
		buffers->mode->cmp_bo(buffers, buffers->dst[i], i);

		igt_post_hang_ring(fd, hang);
	}
}

static void do_basicN(struct buffers *buffers,
		      do_copy do_copy_func,
		      do_hang do_hang_func)
{
	struct igt_hang_ring hang;

	gem_quiescent_gpu(fd);

	for (int i = 0; i < buffers->count; i++) {
		buffers->mode->set_bo(buffers, buffers->src[i], i);
		buffers->mode->set_bo(buffers, buffers->dst[i], ~i);
	}

	hang = do_hang_func();

	for (int i = 0; i < buffers->count; i++) {
		do_copy_func(buffers, buffers->dst[i], buffers->src[i]);
		usleep(0); /* let someone else claim the mutex */
	}

	for (int i = 0; i < buffers->count; i++)
		buffers->mode->cmp_bo(buffers, buffers->dst[i], i);

	igt_post_hang_ring(fd, hang);
}

static void do_overwrite_source(struct buffers *buffers,
				do_copy do_copy_func,
				do_hang do_hang_func)
{
	struct igt_hang_ring hang;
	int i;

	gem_quiescent_gpu(fd);
	for (i = 0; i < buffers->count; i++) {
		buffers->mode->set_bo(buffers, buffers->src[i], i);
		buffers->mode->set_bo(buffers, buffers->dst[i], ~i);
	}
	for (i = 0; i < buffers->count; i++)
		do_copy_func(buffers, buffers->dst[i], buffers->src[i]);
	hang = do_hang_func();
	for (i = buffers->count; i--; )
		buffers->mode->set_bo(buffers, buffers->src[i], 0xdeadbeef);
	for (i = 0; i < buffers->count; i++)
		buffers->mode->cmp_bo(buffers, buffers->dst[i], i);
	igt_post_hang_ring(fd, hang);
}

static void do_overwrite_source_read(struct buffers *buffers,
				     do_copy do_copy_func,
				     do_hang do_hang_func,
				     int do_rcs)
{
	const int half = buffers->count/2;
	struct igt_hang_ring hang;
	int i;

	gem_quiescent_gpu(fd);
	for (i = 0; i < half; i++) {
		buffers->mode->set_bo(buffers, buffers->src[i], i);
		buffers->mode->set_bo(buffers, buffers->dst[i], ~i);
		buffers->mode->set_bo(buffers, buffers->dst[i+half], ~i);
	}
	for (i = 0; i < half; i++) {
		do_copy_func(buffers, buffers->dst[i], buffers->src[i]);
		if (do_rcs)
			render_copy_bo(buffers, buffers->dst[i+half], buffers->src[i]);
		else
			blt_copy_bo(buffers, buffers->dst[i+half], buffers->src[i]);
	}
	hang = do_hang_func();
	for (i = half; i--; )
		buffers->mode->set_bo(buffers, buffers->src[i], 0xdeadbeef);
	for (i = 0; i < half; i++) {
		buffers->mode->cmp_bo(buffers, buffers->dst[i], i);
		buffers->mode->cmp_bo(buffers, buffers->dst[i+half], i);
	}
	igt_post_hang_ring(fd, hang);
}

static void do_overwrite_source_read_bcs(struct buffers *buffers,
					 do_copy do_copy_func,
					 do_hang do_hang_func)
{
	do_overwrite_source_read(buffers, do_copy_func, do_hang_func, 0);
}

static void do_overwrite_source_read_rcs(struct buffers *buffers,
					 do_copy do_copy_func,
					 do_hang do_hang_func)
{
	do_overwrite_source_read(buffers, do_copy_func, do_hang_func, 1);
}

static void do_overwrite_source__rev(struct buffers *buffers,
				     do_copy do_copy_func,
				     do_hang do_hang_func)
{
	struct igt_hang_ring hang;
	int i;

	gem_quiescent_gpu(fd);
	for (i = 0; i < buffers->count; i++) {
		buffers->mode->set_bo(buffers, buffers->src[i], i);
		buffers->mode->set_bo(buffers, buffers->dst[i], ~i);
	}
	for (i = 0; i < buffers->count; i++)
		do_copy_func(buffers, buffers->dst[i], buffers->src[i]);
	hang = do_hang_func();
	for (i = 0; i < buffers->count; i++)
		buffers->mode->set_bo(buffers, buffers->src[i], 0xdeadbeef);
	for (i = buffers->count; i--; )
		buffers->mode->cmp_bo(buffers, buffers->dst[i], i);
	igt_post_hang_ring(fd, hang);
}

static void do_overwrite_source__one(struct buffers *buffers,
				     do_copy do_copy_func,
				     do_hang do_hang_func)
{
	struct igt_hang_ring hang;

	gem_quiescent_gpu(fd);
	buffers->mode->set_bo(buffers, buffers->src[0], 0);
	buffers->mode->set_bo(buffers, buffers->dst[0], ~0);
	do_copy_func(buffers, buffers->dst[0], buffers->src[0]);
	hang = do_hang_func();
	buffers->mode->set_bo(buffers, buffers->src[0], 0xdeadbeef);
	buffers->mode->cmp_bo(buffers, buffers->dst[0], 0);
	igt_post_hang_ring(fd, hang);
}

static void do_intermix(struct buffers *buffers,
			do_copy do_copy_func,
			do_hang do_hang_func,
			int do_rcs)
{
	const int half = buffers->count/2;
	struct igt_hang_ring hang;
	int i;

	gem_quiescent_gpu(fd);
	for (i = 0; i < buffers->count; i++) {
		buffers->mode->set_bo(buffers, buffers->src[i], 0xdeadbeef^~i);
		buffers->mode->set_bo(buffers, buffers->dst[i], i);
	}
	for (i = 0; i < half; i++) {
		if (do_rcs == 1 || (do_rcs == -1 && i & 1))
			render_copy_bo(buffers, buffers->dst[i], buffers->src[i]);
		else
			blt_copy_bo(buffers, buffers->dst[i], buffers->src[i]);

		do_copy_func(buffers, buffers->dst[i+half], buffers->src[i]);

		if (do_rcs == 1 || (do_rcs == -1 && (i & 1) == 0))
			render_copy_bo(buffers, buffers->dst[i], buffers->dst[i+half]);
		else
			blt_copy_bo(buffers, buffers->dst[i], buffers->dst[i+half]);

		do_copy_func(buffers, buffers->dst[i+half], buffers->src[i+half]);
	}
	hang = do_hang_func();
	for (i = 0; i < 2*half; i++)
		buffers->mode->cmp_bo(buffers, buffers->dst[i], 0xdeadbeef^~i);
	igt_post_hang_ring(fd, hang);
}

static void do_intermix_rcs(struct buffers *buffers,
			    do_copy do_copy_func,
			    do_hang do_hang_func)
{
	do_intermix(buffers, do_copy_func, do_hang_func, 1);
}

static void do_intermix_bcs(struct buffers *buffers,
			    do_copy do_copy_func,
			    do_hang do_hang_func)
{
	do_intermix(buffers, do_copy_func, do_hang_func, 0);
}

static void do_intermix_both(struct buffers *buffers,
			     do_copy do_copy_func,
			     do_hang do_hang_func)
{
	do_intermix(buffers, do_copy_func, do_hang_func, -1);
}

static void do_early_read(struct buffers *buffers,
			  do_copy do_copy_func,
			  do_hang do_hang_func)
{
	struct igt_hang_ring hang;
	int i;

	gem_quiescent_gpu(fd);
	for (i = buffers->count; i--; )
		buffers->mode->set_bo(buffers, buffers->src[i], 0xdeadbeef);
	for (i = 0; i < buffers->count; i++)
		do_copy_func(buffers, buffers->dst[i], buffers->src[i]);
	hang = do_hang_func();
	for (i = buffers->count; i--; )
		buffers->mode->cmp_bo(buffers, buffers->dst[i], 0xdeadbeef);
	igt_post_hang_ring(fd, hang);
}

static void do_read_read_bcs(struct buffers *buffers,
			     do_copy do_copy_func,
			     do_hang do_hang_func)
{
	struct igt_hang_ring hang;
	int i;

	gem_quiescent_gpu(fd);
	for (i = buffers->count; i--; )
		buffers->mode->set_bo(buffers, buffers->src[i], 0xdeadbeef ^ i);
	for (i = 0; i < buffers->count; i++) {
		do_copy_func(buffers, buffers->dst[i], buffers->src[i]);
		blt_copy_bo(buffers, buffers->spare, buffers->src[i]);
	}
	buffers->mode->cmp_bo(buffers, buffers->spare, 0xdeadbeef^(buffers->count-1));
	hang = do_hang_func();
	for (i = buffers->count; i--; )
		buffers->mode->cmp_bo(buffers, buffers->dst[i], 0xdeadbeef ^ i);
	igt_post_hang_ring(fd, hang);
}

static void do_write_read_bcs(struct buffers *buffers,
			      do_copy do_copy_func,
			      do_hang do_hang_func)
{
	struct igt_hang_ring hang;
	int i;

	gem_quiescent_gpu(fd);
	for (i = buffers->count; i--; )
		buffers->mode->set_bo(buffers, buffers->src[i], 0xdeadbeef ^ i);
	for (i = 0; i < buffers->count; i++) {
		blt_copy_bo(buffers, buffers->spare, buffers->src[i]);
		do_copy_func(buffers, buffers->dst[i], buffers->spare);
	}
	hang = do_hang_func();
	for (i = buffers->count; i--; )
		buffers->mode->cmp_bo(buffers, buffers->dst[i], 0xdeadbeef ^ i);
	igt_post_hang_ring(fd, hang);
}

static void do_read_read_rcs(struct buffers *buffers,
			     do_copy do_copy_func,
			     do_hang do_hang_func)
{
	struct igt_hang_ring hang;
	int i;

	gem_quiescent_gpu(fd);
	for (i = buffers->count; i--; )
		buffers->mode->set_bo(buffers, buffers->src[i], 0xdeadbeef ^ i);
	for (i = 0; i < buffers->count; i++) {
		do_copy_func(buffers, buffers->dst[i], buffers->src[i]);
		render_copy_bo(buffers, buffers->spare, buffers->src[i]);
	}
	buffers->mode->cmp_bo(buffers, buffers->spare, 0xdeadbeef^(buffers->count-1));
	hang = do_hang_func();
	for (i = buffers->count; i--; )
		buffers->mode->cmp_bo(buffers, buffers->dst[i], 0xdeadbeef ^ i);
	igt_post_hang_ring(fd, hang);
}

static void do_write_read_rcs(struct buffers *buffers,
			      do_copy do_copy_func,
			      do_hang do_hang_func)
{
	struct igt_hang_ring hang;
	int i;

	gem_quiescent_gpu(fd);
	for (i = buffers->count; i--; )
		buffers->mode->set_bo(buffers, buffers->src[i], 0xdeadbeef ^ i);
	for (i = 0; i < buffers->count; i++) {
		render_copy_bo(buffers, buffers->spare, buffers->src[i]);
		do_copy_func(buffers, buffers->dst[i], buffers->spare);
	}
	hang = do_hang_func();
	for (i = buffers->count; i--; )
		buffers->mode->cmp_bo(buffers, buffers->dst[i], 0xdeadbeef ^ i);
	igt_post_hang_ring(fd, hang);
}

static void do_gpu_read_after_write(struct buffers *buffers,
				    do_copy do_copy_func,
				    do_hang do_hang_func)
{
	struct igt_hang_ring hang;
	int i;

	gem_quiescent_gpu(fd);
	for (i = buffers->count; i--; )
		buffers->mode->set_bo(buffers, buffers->src[i], 0xabcdabcd);
	for (i = 0; i < buffers->count; i++)
		do_copy_func(buffers, buffers->dst[i], buffers->src[i]);
	for (i = buffers->count; i--; )
		do_copy_func(buffers, buffers->spare, buffers->dst[i]);
	hang = do_hang_func();
	for (i = buffers->count; i--; )
		buffers->mode->cmp_bo(buffers, buffers->dst[i], 0xabcdabcd);
	igt_post_hang_ring(fd, hang);
}

typedef void (*do_test)(struct buffers *buffers,
			do_copy do_copy_func,
			do_hang do_hang_func);

typedef void (*run_wrap)(struct buffers *buffers,
			 do_test do_test_func,
			 do_copy do_copy_func,
			 do_hang do_hang_func);

static void run_single(struct buffers *buffers,
		       do_test do_test_func,
		       do_copy do_copy_func,
		       do_hang do_hang_func)
{
	do_test_func(buffers, do_copy_func, do_hang_func);
	igt_assert_eq(intel_detect_and_clear_missed_interrupts(fd), 0);
}

static void run_interruptible(struct buffers *buffers,
			      do_test do_test_func,
			      do_copy do_copy_func,
			      do_hang do_hang_func)
{
	for (pass = 0; pass < 10; pass++)
		do_test_func(buffers, do_copy_func, do_hang_func);
	pass = 0;
	igt_assert_eq(intel_detect_and_clear_missed_interrupts(fd), 0);
}

static void run_child(struct buffers *buffers,
		      do_test do_test_func,
		      do_copy do_copy_func,
		      do_hang do_hang_func)

{
	/* We inherit the buffers from the parent, but the bufmgr/batch
	 * needs to be local as the cache of reusable itself will be COWed,
	 * leading to the child closing an object without the parent knowing.
	 */
	igt_fork(child, 1)
		do_test_func(buffers, do_copy_func, do_hang_func);
	igt_waitchildren();
	igt_assert_eq(intel_detect_and_clear_missed_interrupts(fd), 0);
}

static void __run_forked(struct buffers *buffers,
			 int num_children, int loops,
			 do_test do_test_func,
			 do_copy do_copy_func,
			 do_hang do_hang_func)

{
	const int old_num_buffers = num_buffers;

	num_buffers /= num_children;
	num_buffers += MIN_BUFFERS;

	igt_fork(child, num_children) {
		/* recreate process local variables */
		buffers->count = 0;
		fd = drm_open_driver(DRIVER_INTEL);

		batch = buffers_init(buffers, buffers->mode,
				     buffers->width, buffers->height,
				     fd, true);

		buffers_create(buffers, num_buffers);
		for (pass = 0; pass < loops; pass++)
			do_test_func(buffers, do_copy_func, do_hang_func);
		pass = 0;

		buffers_fini(buffers);
	}

	igt_waitchildren();
	igt_assert_eq(intel_detect_and_clear_missed_interrupts(fd), 0);

	num_buffers = old_num_buffers;
}

static void run_forked(struct buffers *buffers,
		       do_test do_test_func,
		       do_copy do_copy_func,
		       do_hang do_hang_func)
{
	__run_forked(buffers, sysconf(_SC_NPROCESSORS_ONLN), 10,
		     do_test_func, do_copy_func, do_hang_func);
}

static void run_bomb(struct buffers *buffers,
		     do_test do_test_func,
		     do_copy do_copy_func,
		     do_hang do_hang_func)
{
	__run_forked(buffers, 8*sysconf(_SC_NPROCESSORS_ONLN), 10,
		     do_test_func, do_copy_func, do_hang_func);
}

static void bit17_require(void)
{
	struct drm_i915_gem_get_tiling2 {
		uint32_t handle;
		uint32_t tiling_mode;
		uint32_t swizzle_mode;
		uint32_t phys_swizzle_mode;
	} arg;
#define DRM_IOCTL_I915_GEM_GET_TILING2	DRM_IOWR (DRM_COMMAND_BASE + DRM_I915_GEM_GET_TILING, struct drm_i915_gem_get_tiling2)

	memset(&arg, 0, sizeof(arg));
	arg.handle = gem_create(fd, 4096);
	gem_set_tiling(fd, arg.handle, I915_TILING_X, 512);

	do_ioctl(fd, DRM_IOCTL_I915_GEM_GET_TILING2, &arg);
	gem_close(fd, arg.handle);
	igt_require(arg.phys_swizzle_mode == arg.swizzle_mode);
}

static void cpu_require(void)
{
	bit17_require();
}

static void gtt_require(void)
{
}

static void wc_require(void)
{
	bit17_require();
	gem_require_mmap_wc(fd);
}

static void bcs_require(void)
{
}

static void rcs_require(void)
{
	igt_require(rendercopy);
}

static void
run_basic_modes(const char *prefix,
		const struct access_mode *mode,
		const char *suffix,
		run_wrap run_wrap_func)
{
	const struct {
		const char *prefix;
		do_copy copy;
		void (*require)(void);
	} pipelines[] = {
		{ "cpu", cpu_copy_bo, cpu_require },
		{ "gtt", gtt_copy_bo, gtt_require },
		{ "wc", wc_copy_bo, wc_require },
		{ "blt", blt_copy_bo, bcs_require },
		{ "render", render_copy_bo, rcs_require },
		{ NULL, NULL }
	}, *pskip = pipelines + 3, *p;
	const struct {
		const char *suffix;
		do_hang hang;
	} hangs[] = {
		{ "", no_hang },
		{ "-hang-blt", bcs_hang },
		{ "-hang-render", rcs_hang },
		{ NULL, NULL },
	}, *h;

	for (h = hangs; h->suffix; h++) {
		if (!all && *h->suffix)
			continue;

		for (p = all ? pipelines : pskip; p->prefix; p++) {
			struct buffers buffers;

			igt_fixture
				batch = buffers_init(&buffers, mode,
						     512, 512, fd,
						     run_wrap_func != run_child);

			igt_subtest_f("%s-%s-%s-sanitycheck0%s%s", prefix, mode->name, p->prefix, suffix, h->suffix) {
				p->require();
				buffers_create(&buffers, num_buffers);
				run_wrap_func(&buffers, do_basic0,
					      p->copy, h->hang);
			}

			igt_subtest_f("%s-%s-%s-sanitycheck1%s%s", prefix, mode->name, p->prefix, suffix, h->suffix) {
				p->require();
				buffers_create(&buffers, num_buffers);
				run_wrap_func(&buffers, do_basic1,
					      p->copy, h->hang);
			}

			igt_subtest_f("%s-%s-%s-sanitycheckN%s%s", prefix, mode->name, p->prefix, suffix, h->suffix) {
				p->require();
				buffers_create(&buffers, num_buffers);
				run_wrap_func(&buffers, do_basicN,
					      p->copy, h->hang);
			}

			/* try to overwrite the source values */
			igt_subtest_f("%s-%s-%s-overwrite-source-one%s%s", prefix, mode->name, p->prefix, suffix, h->suffix) {
				p->require();
				buffers_create(&buffers, num_buffers);
				run_wrap_func(&buffers,
					      do_overwrite_source__one,
					      p->copy, h->hang);
			}

			igt_subtest_f("%s-%s-%s-overwrite-source%s%s", prefix, mode->name, p->prefix, suffix, h->suffix) {
				p->require();
				buffers_create(&buffers, num_buffers);
				run_wrap_func(&buffers,
					      do_overwrite_source,
					      p->copy, h->hang);
			}

			igt_subtest_f("%s-%s-%s-overwrite-source-read-bcs%s%s", prefix, mode->name, p->prefix, suffix, h->suffix) {
				p->require();
				buffers_create(&buffers, num_buffers);
				run_wrap_func(&buffers,
					      do_overwrite_source_read_bcs,
					      p->copy, h->hang);
			}

			igt_subtest_f("%s-%s-%s-overwrite-source-read-rcs%s%s", prefix, mode->name, p->prefix, suffix, h->suffix) {
				p->require();
				igt_require(rendercopy);
				buffers_create(&buffers, num_buffers);
				run_wrap_func(&buffers,
					      do_overwrite_source_read_rcs,
					      p->copy, h->hang);
			}

			igt_subtest_f("%s-%s-%s-overwrite-source-rev%s%s", prefix, mode->name, p->prefix, suffix, h->suffix) {
				p->require();
				buffers_create(&buffers, num_buffers);
				run_wrap_func(&buffers,
					      do_overwrite_source__rev,
					      p->copy, h->hang);
			}

			/* try to intermix copies with GPU copies*/
			igt_subtest_f("%s-%s-%s-intermix-rcs%s%s", prefix, mode->name, p->prefix, suffix, h->suffix) {
				p->require();
				igt_require(rendercopy);
				buffers_create(&buffers, num_buffers);
				run_wrap_func(&buffers,
					      do_intermix_rcs,
					      p->copy, h->hang);
			}
			igt_subtest_f("%s-%s-%s-intermix-bcs%s%s", prefix, mode->name, p->prefix, suffix, h->suffix) {
				p->require();
				igt_require(rendercopy);
				buffers_create(&buffers, num_buffers);
				run_wrap_func(&buffers,
					      do_intermix_bcs,
					      p->copy, h->hang);
			}
			igt_subtest_f("%s-%s-%s-intermix-both%s%s", prefix, mode->name, p->prefix, suffix, h->suffix) {
				p->require();
				igt_require(rendercopy);
				buffers_create(&buffers, num_buffers);
				run_wrap_func(&buffers,
					      do_intermix_both,
					      p->copy, h->hang);
			}

			/* try to read the results before the copy completes */
			igt_subtest_f("%s-%s-%s-early-read%s%s", prefix, mode->name, p->prefix, suffix, h->suffix) {
				p->require();
				buffers_create(&buffers, num_buffers);
				run_wrap_func(&buffers,
					      do_early_read,
					      p->copy, h->hang);
			}

			/* concurrent reads */
			igt_subtest_f("%s-%s-%s-read-read-bcs%s%s", prefix, mode->name, p->prefix, suffix, h->suffix) {
				p->require();
				buffers_create(&buffers, num_buffers);
				run_wrap_func(&buffers,
					      do_read_read_bcs,
					      p->copy, h->hang);
			}
			igt_subtest_f("%s-%s-%s-read-read-rcs%s%s", prefix, mode->name, p->prefix, suffix, h->suffix) {
				p->require();
				igt_require(rendercopy);
				buffers_create(&buffers, num_buffers);
				run_wrap_func(&buffers,
					      do_read_read_rcs,
					      p->copy, h->hang);
			}

			/* split copying between rings */
			igt_subtest_f("%s-%s-%s-write-read-bcs%s%s", prefix, mode->name, p->prefix, suffix, h->suffix) {
				p->require();
				buffers_create(&buffers, num_buffers);
				run_wrap_func(&buffers,
					      do_write_read_bcs,
					      p->copy, h->hang);
			}
			igt_subtest_f("%s-%s-%s-write-read-rcs%s%s", prefix, mode->name, p->prefix, suffix, h->suffix) {
				p->require();
				igt_require(rendercopy);
				buffers_create(&buffers, num_buffers);
				run_wrap_func(&buffers,
					      do_write_read_rcs,
					      p->copy, h->hang);
			}

			/* and finally try to trick the kernel into loosing the pending write */
			igt_subtest_f("%s-%s-%s-gpu-read-after-write%s%s", prefix, mode->name, p->prefix, suffix, h->suffix) {
				p->require();
				buffers_create(&buffers, num_buffers);
				run_wrap_func(&buffers,
					      do_gpu_read_after_write,
					      p->copy, h->hang);
			}

			igt_fixture
				buffers_fini(&buffers);
		}
	}
}

static void
run_modes(const char *style, const struct access_mode *mode, unsigned allow_mem)
{
	if (mode->require && !mode->require())
		return;

	igt_debug("%s: using 2x%d buffers, each 1MiB\n",
			style, num_buffers);
	if (!__intel_check_memory(2*num_buffers, 1024*1024, allow_mem,
				  NULL, NULL))
		return;

	run_basic_modes(style, mode, "", run_single);
	run_basic_modes(style, mode, "-child", run_child);
	run_basic_modes(style, mode, "-forked", run_forked);

	igt_fork_signal_helper();
	run_basic_modes(style, mode, "-interruptible", run_interruptible);
	run_basic_modes(style, mode, "-bomb", run_bomb);
	igt_stop_signal_helper();
}

igt_main
{
	const struct {
		const char *name;
		drm_intel_bo *(*create)(drm_intel_bufmgr *, uint64_t size);
		bool (*require)(void);
	} create[] = {
		{ "", create_normal_bo, can_create_normal},
		{ "private-", create_private_bo, can_create_private },
		{ "stolen-", create_stolen_bo, can_create_stolen },
		{ NULL, NULL }
	}, *c;
	uint64_t pin_sz = 0;
	void *pinned = NULL;
	int i;

	igt_skip_on_simulation();

	if (strstr(igt_test_name(), "all"))
		all = true;

	igt_fixture {
		fd = drm_open_driver(DRIVER_INTEL);
		intel_detect_and_clear_missed_interrupts(fd);
		devid = intel_get_drm_devid(fd);
		gen = intel_gen(devid);
		rendercopy = igt_get_render_copyfunc(devid);
	}

	for (c = create; c->name; c++) {
		char name[80];

		create_func = c->create;

		num_buffers = MIN_BUFFERS;
		if (c->require()) {
			snprintf(name, sizeof(name), "%s%s", c->name, "tiny");
			for (i = 0; i < ARRAY_SIZE(access_modes); i++)
				run_modes(name, &access_modes[i], CHECK_RAM);
		}

		igt_fixture {
			num_buffers = gem_mappable_aperture_size() / (1024 * 1024) / 4;
		}

		if (c->require()) {
			snprintf(name, sizeof(name), "%s%s", c->name, "small");
			for (i = 0; i < ARRAY_SIZE(access_modes); i++)
				run_modes(name, &access_modes[i], CHECK_RAM);
		}

		igt_fixture {
			num_buffers = gem_mappable_aperture_size() / (1024 * 1024);
		}

		if (c->require()) {
			snprintf(name, sizeof(name), "%s%s", c->name, "thrash");
			for (i = 0; i < ARRAY_SIZE(access_modes); i++)
				run_modes(name, &access_modes[i], CHECK_RAM);
		}

		igt_fixture {
			num_buffers = gem_aperture_size(fd) / (1024 * 1024);
		}

		if (c->require()) {
			snprintf(name, sizeof(name), "%s%s", c->name, "full");
			for (i = 0; i < ARRAY_SIZE(access_modes); i++)
				run_modes(name, &access_modes[i], CHECK_RAM);
		}

		igt_fixture {
			num_buffers = gem_mappable_aperture_size() / (1024 * 1024);
			pin_sz = intel_get_avail_ram_mb() - num_buffers;

			igt_debug("Pinning %ld MiB\n", pin_sz);
			pin_sz *= 1024 * 1024;

			if (posix_memalign(&pinned, 4096, pin_sz) ||
			    mlock(pinned, pin_sz) ||
			    madvise(pinned, pin_sz, MADV_DONTFORK)) {
				free(pinned);
				pinned = NULL;
			}
			igt_require(pinned);
		}

		if (c->require()) {
			snprintf(name, sizeof(name), "%s%s", c->name, "swap");
			for (i = 0; i < ARRAY_SIZE(access_modes); i++)
				run_modes(name, &access_modes[i], CHECK_RAM | CHECK_SWAP);
		}

		igt_fixture {
			if (pinned) {
				munlock(pinned, pin_sz);
				free(pinned);
				pinned = NULL;
			}
		}
	}
}
