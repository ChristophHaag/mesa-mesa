#ifndef __NOUVEAU_CONTEXT_H__
#define __NOUVEAU_CONTEXT_H__

#include "nouveau/nouveau_winsys.h"
#include "nouveau_drmif.h"
#include "nouveau_device.h"
#include "nouveau_channel.h"
#include "nouveau_pushbuf.h"
#include "nouveau_bo.h"
#include "nouveau_grobj.h"
#include "nouveau_notifier.h"
#include "nouveau_class.h"
#include "nouveau_local.h"

struct nouveau_channel_context {
	struct pipe_screen *pscreen;
	int refcount;

	unsigned cur_pctx;
	unsigned nr_pctx;
	struct pipe_context **pctx;

	struct nouveau_channel  *channel;
	unsigned next_handle;
};

struct nouveau_context {
	int locked;
	struct nouveau_screen *nv_screen;
	struct pipe_surface *frontbuffer;
	struct pipe_texture *frontbuffer_texture;

	struct {
		int hw_vertex_buffer;
		int hw_index_buffer;
	} cap;

	/* Hardware context */
	struct nouveau_channel_context *nvc;
	int pctx_id;
};

extern int nouveau_context_init(struct nouveau_screen *nv_screen,
                                drm_context_t hHWContext, drmLock *sarea_lock,
                                struct nouveau_context *nv_share,
                                struct nouveau_context *nv);
extern void nouveau_context_cleanup(struct nouveau_context *nv);

extern void LOCK_HARDWARE(struct nouveau_context *);
extern void UNLOCK_HARDWARE(struct nouveau_context *);

extern uint32_t *nouveau_pipe_dma_beginp(struct nouveau_grobj *, int, int);
extern void nouveau_pipe_dma_kickoff(struct nouveau_channel *);

/* Must be provided by clients of common code */
extern void
nouveau_contended_lock(struct nouveau_context *nv);

#endif
