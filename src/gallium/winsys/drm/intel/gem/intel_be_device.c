
#include "intel_be_device.h"

#include "pipe/internal/p_winsys_screen.h"
#include "pipe/p_defines.h"
#include "pipe/p_state.h"
#include "pipe/p_inlines.h"
#include "util/u_memory.h"

#include "intel_be_fence.h"

#include "i915simple/i915_screen.h"

#include "intel_be_api.h"

/*
 * Buffer
 */

static void *
intel_be_buffer_map(struct pipe_winsys *winsys,
		    struct pipe_buffer *buf,
		    unsigned flags)
{
	drm_intel_bo *bo = intel_bo(buf);
	int write = 0;
	int ret;

	if (flags & PIPE_BUFFER_USAGE_CPU_WRITE)
		write = 1;

	ret = drm_intel_bo_map(bo, write);

	if (ret)
		return NULL;

	return bo->virtual;
}

static void
intel_be_buffer_unmap(struct pipe_winsys *winsys,
		      struct pipe_buffer *buf)
{
	drm_intel_bo_unmap(intel_bo(buf));
}

static void
intel_be_buffer_destroy(struct pipe_winsys *winsys,
			struct pipe_buffer *buf)
{
	drm_intel_bo_unreference(intel_bo(buf));
	free(buf);
}

static struct pipe_buffer *
intel_be_buffer_create(struct pipe_winsys *winsys,
		       unsigned alignment,
		       unsigned usage,
		       unsigned size)
{
	struct intel_be_buffer *buffer = CALLOC_STRUCT(intel_be_buffer);
	struct intel_be_device *dev = intel_be_device(winsys);
	drm_intel_bufmgr *pool;
	char *name;

	if (!buffer)
		return NULL;

	buffer->base.refcount = 1;
	buffer->base.alignment = alignment;
	buffer->base.usage = usage;
	buffer->base.size = size;

	if (usage & (PIPE_BUFFER_USAGE_VERTEX | PIPE_BUFFER_USAGE_CONSTANT)) {
		/* Local buffer */
		name = "gallium3d_local";
		pool = dev->pools.gem;
	} else if (usage & PIPE_BUFFER_USAGE_CUSTOM) {
		/* For vertex buffers */
		name = "gallium3d_internal_vertex";
		pool = dev->pools.gem;
	} else {
		/* Regular buffers */
		name = "gallium3d_regular";
		pool = dev->pools.gem;
	}

	buffer->bo = drm_intel_bo_alloc(pool, name, size, alignment);

	if (!buffer->bo)
		goto err;

	return &buffer->base;

err:
	free(buffer);
	return NULL;
}

static struct pipe_buffer *
intel_be_user_buffer_create(struct pipe_winsys *winsys, void *ptr, unsigned bytes)
{
	struct intel_be_buffer *buffer = CALLOC_STRUCT(intel_be_buffer);
	struct intel_be_device *dev = intel_be_device(winsys);
	int ret;

	if (!buffer)
		return NULL;

	buffer->base.refcount = 1;
	buffer->base.alignment = 0;
	buffer->base.usage = 0;
	buffer->base.size = bytes;

	buffer->bo = drm_intel_bo_alloc(dev->pools.gem,
	                                "gallium3d_user_buffer",
	                                bytes, 0);

	if (!buffer->bo)
		goto err;

	ret = drm_intel_bo_subdata(buffer->bo,
	                           0, bytes, ptr);

	if (ret)
		goto err;

	return &buffer->base;

err:
	free(buffer);
	return NULL;
}

struct pipe_buffer *
intel_be_buffer_from_handle(struct pipe_winsys *winsys,
                            const char* name, unsigned handle)
{
	struct intel_be_device *dev = intel_be_device(winsys);
	struct intel_be_buffer *buffer = CALLOC_STRUCT(intel_be_buffer);

	if (!buffer)
		return NULL;

	buffer->bo = drm_intel_bo_gem_create_from_name(dev->pools.gem, name, handle);

	if (!buffer->bo)
		goto err;

	buffer->base.refcount = 1;
	buffer->base.alignment = buffer->bo->align;
	buffer->base.usage = PIPE_BUFFER_USAGE_GPU_READ |
	                     PIPE_BUFFER_USAGE_GPU_WRITE |
	                     PIPE_BUFFER_USAGE_CPU_READ |
	                     PIPE_BUFFER_USAGE_CPU_WRITE;
	buffer->base.size = buffer->bo->size;

	return &buffer->base;

err:
	free(buffer);
	return NULL;
}

unsigned
intel_be_handle_from_buffer(struct pipe_winsys *winsys,
                            struct pipe_buffer *buf)
{
	drm_intel_bo *bo = intel_bo(buf);
	return bo->handle;
}

/*
 * Fence
 */

static void
intel_be_fence_refunref(struct pipe_winsys *sws,
			 struct pipe_fence_handle **ptr,
			 struct pipe_fence_handle *fence)
{
	struct intel_be_fence **p = (struct intel_be_fence **)ptr;
	struct intel_be_fence *f = (struct intel_be_fence *)fence;

	assert(p);

	if (f)
		intel_be_fence_reference(f);

	if (*p)
		intel_be_fence_unreference(*p);

	*p = f;
}

static int
intel_be_fence_signalled(struct pipe_winsys *sws,
			 struct pipe_fence_handle *fence,
			 unsigned flag)
{
	assert(0);

	return 0;
}

static int
intel_be_fence_finish(struct pipe_winsys *sws,
		      struct pipe_fence_handle *fence,
		      unsigned flag)
{
	struct intel_be_fence *f = (struct intel_be_fence *)fence;

	/* fence already expired */
	if (!f->bo)
		return 0;

	drm_intel_bo_wait_rendering(f->bo);
	drm_intel_bo_unreference(f->bo);
	f->bo = NULL;

	return 0;
}

/*
 * Misc functions
 */

static void
intel_be_destroy_winsys(struct pipe_winsys *winsys)
{
	struct intel_be_device *dev = intel_be_device(winsys);

	drm_intel_bufmgr_destroy(dev->pools.gem);

	free(dev);
}

boolean
intel_be_init_device(struct intel_be_device *dev, int fd, unsigned id)
{
	dev->fd = fd;
	dev->id = id;
	dev->max_batch_size = 16 * 4096;
	dev->max_vertex_size = 128 * 4096;

	dev->base.buffer_create = intel_be_buffer_create;
	dev->base.user_buffer_create = intel_be_user_buffer_create;
	dev->base.buffer_map = intel_be_buffer_map;
	dev->base.buffer_unmap = intel_be_buffer_unmap;
	dev->base.buffer_destroy = intel_be_buffer_destroy;

	/* Not used anymore */
	dev->base.surface_buffer_create = NULL;

	dev->base.fence_reference = intel_be_fence_refunref;
	dev->base.fence_signalled = intel_be_fence_signalled;
	dev->base.fence_finish = intel_be_fence_finish;

	dev->base.destroy = intel_be_destroy_winsys;

	dev->pools.gem = drm_intel_bufmgr_gem_init(dev->fd, dev->max_batch_size);

	return true;
}

struct pipe_screen *
intel_be_create_screen(int drmFD, int deviceID)
{
	struct intel_be_device *dev;
	struct pipe_screen *screen;

	/* Allocate the private area */
	dev = malloc(sizeof(*dev));
	if (!dev)
		return NULL;
	memset(dev, 0, sizeof(*dev));

	intel_be_init_device(dev, drmFD, deviceID);

	screen = i915_create_screen(&dev->base, deviceID);

	return screen;
}
