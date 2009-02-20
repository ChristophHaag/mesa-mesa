#include "pipe/p_state.h"
#include "pipe/p_defines.h"
#include "pipe/p_inlines.h"

#include "nv10_context.h"
#include "nv10_screen.h"

static void
nv10_miptree_layout(struct nv10_miptree *nv10mt)
{
	struct pipe_texture *pt = &nv10mt->base;
	boolean swizzled = FALSE;
	uint width = pt->width[0], height = pt->height[0];
	uint offset = 0;
	int nr_faces, l, f;

	if (pt->target == PIPE_TEXTURE_CUBE) {
		nr_faces = 6;
	} else {
		nr_faces = 1;
	}
	
	for (l = 0; l <= pt->last_level; l++) {
		pt->width[l] = width;
		pt->height[l] = height;
		pt->nblocksx[l] = pf_get_nblocksx(&pt->block, width);
		pt->nblocksy[l] = pf_get_nblocksy(&pt->block, height);

		if (swizzled)
			nv10mt->level[l].pitch = pt->nblocksx[l] * pt->block.size;
		else
			nv10mt->level[l].pitch = pt->nblocksx[0] * pt->block.size;
		nv10mt->level[l].pitch = (nv10mt->level[l].pitch + 63) & ~63;

		nv10mt->level[l].image_offset =
			CALLOC(nr_faces, sizeof(unsigned));

		width  = MAX2(1, width  >> 1);
		height = MAX2(1, height >> 1);

	}

	for (f = 0; f < nr_faces; f++) {
		for (l = 0; l <= pt->last_level; l++) {
			nv10mt->level[l].image_offset[f] = offset;
			offset += nv10mt->level[l].pitch * pt->height[l];
		}
	}

	nv10mt->total_size = offset;
}

static struct pipe_texture *
nv10_miptree_blanket(struct pipe_screen *pscreen, const struct pipe_texture *pt,
		     const unsigned *stride, struct pipe_buffer *pb)
{
	struct nv10_miptree *mt;

	/* Only supports 2D, non-mipmapped textures for the moment */
	if (pt->target != PIPE_TEXTURE_2D || pt->last_level != 0 ||
	    pt->depth[0] != 1)
		return NULL;

	mt = CALLOC_STRUCT(nv10_miptree);
	if (!mt)
		return NULL;

	mt->base = *pt;
	mt->base.refcount = 1;
	mt->base.screen = pscreen;
	mt->level[0].pitch = stride[0];
	mt->level[0].image_offset = CALLOC(1, sizeof(unsigned));

	pipe_buffer_reference(pscreen, &mt->buffer, pb);
	return &mt->base;
}

static struct pipe_texture *
nv10_miptree_create(struct pipe_screen *screen, const struct pipe_texture *pt)
{
	struct pipe_winsys *ws = screen->winsys;
	struct nv10_miptree *mt;

	mt = MALLOC(sizeof(struct nv10_miptree));
	if (!mt)
		return NULL;
	mt->base = *pt;
	mt->base.refcount = 1;
	mt->base.screen = screen;

	nv10_miptree_layout(mt);

	mt->buffer = ws->buffer_create(ws, 256, PIPE_BUFFER_USAGE_PIXEL,
					   mt->total_size);
	if (!mt->buffer) {
		FREE(mt);
		return NULL;
	}
	
	return &mt->base;
}

static void
nv10_miptree_release(struct pipe_screen *screen, struct pipe_texture **pt)
{
	struct pipe_texture *mt = *pt;

	*pt = NULL;
	if (--mt->refcount <= 0) {
		struct nv10_miptree *nv10mt = (struct nv10_miptree *)mt;
		int l;

		pipe_buffer_reference(screen, &nv10mt->buffer, NULL);
		for (l = 0; l <= mt->last_level; l++) {
			if (nv10mt->level[l].image_offset)
				FREE(nv10mt->level[l].image_offset);
		}
		FREE(nv10mt);
	}
}

static void
nv10_miptree_update(struct pipe_context *pipe, struct pipe_texture *mt,
		    uint face, uint levels)
{
}


static struct pipe_surface *
nv10_miptree_surface_get(struct pipe_screen *screen, struct pipe_texture *pt,
			 unsigned face, unsigned level, unsigned zslice,
			 unsigned flags)
{
	struct pipe_winsys *ws = screen->winsys;
	struct nv10_miptree *nv10mt = (struct nv10_miptree *)pt;
	struct pipe_surface *ps;

	ps = CALLOC_STRUCT(pipe_surface);
	if (!ps)
		return NULL;
	pipe_texture_reference(&ps->texture, pt);
	ps->format = pt->format;
	ps->width = pt->width[level];
	ps->height = pt->height[level];
	ps->block = pt->block;
	ps->nblocksx = pt->nblocksx[level];
	ps->nblocksy = pt->nblocksy[level];
	ps->stride = nv10mt->level[level].pitch;
	ps->refcount = 1;

	if (pt->target == PIPE_TEXTURE_CUBE) {
		ps->offset = nv10mt->level[level].image_offset[face];
	} else {
		ps->offset = nv10mt->level[level].image_offset[0];
	}

	return ps;
}

static void
nv10_miptree_surface_release(struct pipe_screen *screen,
			     struct pipe_surface **surface)
{
}

void nv10_screen_init_miptree_functions(struct pipe_screen *pscreen)
{
	pscreen->texture_create = nv10_miptree_create;
	pscreen->texture_blanket = nv10_miptree_blanket;
	pscreen->texture_release = nv10_miptree_release;
	pscreen->get_tex_surface = nv10_miptree_surface_get;
	pscreen->tex_surface_release = nv10_miptree_surface_release;
}

