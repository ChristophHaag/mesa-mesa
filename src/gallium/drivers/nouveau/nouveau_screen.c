#include "pipe/p_defines.h"
#include "pipe/p_screen.h"
#include "pipe/p_state.h"

#include "util/u_memory.h"
#include "util/u_inlines.h"
#include "util/u_format.h"

#include <stdio.h>
#include <errno.h>

#include "nouveau/nouveau_bo.h"
#include "nouveau_winsys.h"
#include "nouveau_screen.h"

/* XXX this should go away */
#include "state_tracker/drm_api.h"
#include "util/u_simple_screen.h"

static const char *
nouveau_screen_get_name(struct pipe_screen *pscreen)
{
	struct nouveau_device *dev = nouveau_screen(pscreen)->device;
	static char buffer[128];

	snprintf(buffer, sizeof(buffer), "NV%02X", dev->chipset);
	return buffer;
}

static const char *
nouveau_screen_get_vendor(struct pipe_screen *pscreen)
{
	return "nouveau";
}



struct nouveau_bo *
nouveau_screen_bo_new(struct pipe_screen *pscreen, unsigned alignment,
		      unsigned usage, unsigned size)
{
	struct nouveau_device *dev = nouveau_screen(pscreen)->device;
	struct nouveau_bo *bo = NULL;
	uint32_t flags = NOUVEAU_BO_MAP, tile_mode = 0, tile_flags = 0;
	int ret;

	if (usage & NOUVEAU_BUFFER_USAGE_TRANSFER)
		flags |= NOUVEAU_BO_GART;
	else
	if (usage & NOUVEAU_BUFFER_USAGE_VERTEX) {
		if (pscreen->get_param(pscreen, NOUVEAU_CAP_HW_VTXBUF))
			flags |= NOUVEAU_BO_GART;
	} else
	if (usage & NOUVEAU_BUFFER_USAGE_INDEX) {
		if (pscreen->get_param(pscreen, NOUVEAU_CAP_HW_IDXBUF))
			flags |= NOUVEAU_BO_GART;
	}

	if (usage & NOUVEAU_BUFFER_USAGE_PIXEL) {
		if (usage & NOUVEAU_BUFFER_USAGE_TEXTURE)
			flags |= NOUVEAU_BO_GART;
		if (!(usage & NOUVEAU_BUFFER_USAGE_CPU_READ_WRITE))
			flags |= NOUVEAU_BO_VRAM;

		if (dev->chipset == 0x50 || dev->chipset >= 0x80) {
			if (usage & NOUVEAU_BUFFER_USAGE_ZETA)
				tile_flags = 0x2800;
			else
				tile_flags = 0x7000;
		}
	}

	ret = nouveau_bo_new_tile(dev, flags, alignment, size,
				  tile_mode, tile_flags, &bo);
	if (ret)
		return NULL;

	return bo;
}

struct nouveau_bo *
nouveau_screen_bo_user(struct pipe_screen *pscreen, void *ptr, unsigned bytes)
{
	struct nouveau_device *dev = nouveau_screen(pscreen)->device;
	struct nouveau_bo *bo = NULL;
	int ret;

	ret = nouveau_bo_user(dev, ptr, bytes, &bo);
	if (ret)
		return NULL;

	return bo;
}

static inline uint32_t
nouveau_screen_map_flags(unsigned usage)
{
	uint32_t flags = 0;

	if (usage & PIPE_TRANSFER_READ)
		flags |= NOUVEAU_BO_RD;
	if (usage & PIPE_TRANSFER_WRITE)
		flags |= NOUVEAU_BO_WR;
	if (usage & PIPE_TRANSFER_DISCARD)
		flags |= NOUVEAU_BO_INVAL;
	if (usage & PIPE_TRANSFER_DONTBLOCK)
		flags |= NOUVEAU_BO_NOWAIT;
	else
	if (usage & PIPE_TRANSFER_UNSYNCHRONIZED)
		flags |= NOUVEAU_BO_NOSYNC;

	return flags;
}


void *
nouveau_screen_bo_map(struct pipe_screen *pscreen,
		      struct nouveau_bo *pb,
		      unsigned map_flags)
{
	int ret;

	ret = nouveau_bo_map(bo, map_flags);
	if (ret) {
		debug_printf("map failed: %d\n", ret);
		return NULL;
	}

	return bo->map;
}

void *
nouveau_screen_bo_map_range(struct pipe_screen *pscreen, struct nouveau_bo *bo,
			    unsigned offset, unsigned length, unsigned flags)
{
	int ret;

	ret = nouveau_bo_map_range(bo, offset, length, flags);
	if (ret) {
		nouveau_bo_unmap(bo);
		if (!(flags & NOUVEAU_BO_NOWAIT) || ret != -EBUSY)
			debug_printf("map_range failed: %d\n", ret);
		return NULL;
	}

	return (char *)bo->map - offset; /* why gallium? why? */
}

void
nouveau_screen_bo_map_flush_range(struct pipe_screen *pscreen, struct nouveau_bo *bo,
				  unsigned offset, unsigned length)
{
	nouveau_bo_map_flush(bo, offset, length);
}

void
nouveau_screen_bo_unmap(struct pipe_screen *pscreen, struct nouveau_bo *bo)
{
	nouveau_bo_unmap(bo);
}

void
nouveau_screen_bo_release(struct pipe_screen *pscreen, struct nouveau_bo *bo)
{
	nouveau_bo_ref(NULL, &bo);
}

static void
nouveau_screen_fence_ref(struct pipe_screen *pscreen,
			 struct pipe_fence_handle **ptr,
			 struct pipe_fence_handle *pfence)
{
	*ptr = pfence;
}

static int
nouveau_screen_fence_signalled(struct pipe_screen *screen,
			       struct pipe_fence_handle *pfence,
			       unsigned flags)
{
	return 0;
}

static int
nouveau_screen_fence_finish(struct pipe_screen *screen,
			    struct pipe_fence_handle *pfence,
			    unsigned flags)
{
	return 0;
}


struct nouveau_bo *
nouveau_screen_bo_from_handle(struct pipe_screen *pscreen,
			      struct winsys_handle *whandle,
			      unsigned *out_stride)
{
	struct nouveau_device *dev = nouveau_screen(pscreen)->device;
	struct nouveau_bo *bo = 0;
	int ret;
 
	ret = nouveau_bo_handle_ref(dev, whandle->handle, &bo);
	if (ret) {
		debug_printf("%s: ref name 0x%08x failed with %d\n",
			     __func__, whandle->handle, ret);
		return NULL;
	}

	*out_stride = whandle->stride;
	return bo;
}


boolean
nouveau_screen_bo_get_handle(struct pipe_screen *pscreen,
			     struct nouveau_bo *bo,
			     unsigned stride,
			     struct winsys_handle *whandle)
{
	whandle->stride = stride;

	if (whandle->type == DRM_API_HANDLE_TYPE_SHARED) { 
		return nouveau_bo_handle_get(bo, &whandle->handle) == 0;
	} else if (whandle->type == DRM_API_HANDLE_TYPE_KMS) {
		whandle->handle = bo->handle;
		return TRUE;
	} else {
		return FALSE;
	}
}


unsigned int
nouveau_reference_flags(struct nouveau_bo *bo)
{
	uint32_t bo_flags;
	int flags = 0;

	bo_flags = nouveau_bo_pending(bo);
	if (bo_flags & NOUVEAU_BO_RD)
		flags |= PIPE_REFERENCED_FOR_READ;
	if (bo_flags & NOUVEAU_BO_WR)
		flags |= PIPE_REFERENCED_FOR_WRITE;

	return flags;
}





int
nouveau_screen_init(struct nouveau_screen *screen, struct nouveau_device *dev)
{
	struct pipe_screen *pscreen = &screen->base;
	int ret;

	ret = nouveau_channel_alloc(dev, 0xbeef0201, 0xbeef0202,
				    &screen->channel);
	if (ret)
		return ret;
	screen->device = dev;

	pscreen->get_name = nouveau_screen_get_name;
	pscreen->get_vendor = nouveau_screen_get_vendor;

	pscreen->fence_reference = nouveau_screen_fence_ref;
	pscreen->fence_signalled = nouveau_screen_fence_signalled;
	pscreen->fence_finish = nouveau_screen_fence_finish;

	return 0;
}

void
nouveau_screen_fini(struct nouveau_screen *screen)
{
	struct pipe_winsys *ws = screen->base.winsys;
	nouveau_channel_free(&screen->channel);
	ws->destroy(ws);
}

