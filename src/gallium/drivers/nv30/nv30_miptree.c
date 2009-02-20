#include "pipe/p_state.h"
#include "pipe/p_defines.h"
#include "pipe/p_inlines.h"

#include "nv30_context.h"

static void
nv30_miptree_layout(struct nv30_miptree *nv30mt)
{
	struct pipe_texture *pt = &nv30mt->base;
	uint width = pt->width[0], height = pt->height[0], depth = pt->depth[0];
	uint offset = 0;
	int nr_faces, l, f;
	uint wide_pitch = pt->tex_usage & (PIPE_TEXTURE_USAGE_SAMPLER |
		                           PIPE_TEXTURE_USAGE_DEPTH_STENCIL |
		                           PIPE_TEXTURE_USAGE_RENDER_TARGET |
		                           PIPE_TEXTURE_USAGE_DISPLAY_TARGET |
		                           PIPE_TEXTURE_USAGE_PRIMARY);

	if (pt->target == PIPE_TEXTURE_CUBE) {
		nr_faces = 6;
	} else
	if (pt->target == PIPE_TEXTURE_3D) {
		nr_faces = pt->depth[0];
	} else {
		nr_faces = 1;
	}

	for (l = 0; l <= pt->last_level; l++) {
		pt->width[l] = width;
		pt->height[l] = height;
		pt->depth[l] = depth;
		pt->nblocksx[l] = pf_get_nblocksx(&pt->block, width);
		pt->nblocksy[l] = pf_get_nblocksy(&pt->block, height);

		if (wide_pitch && (pt->tex_usage & NOUVEAU_TEXTURE_USAGE_LINEAR))
			nv30mt->level[l].pitch = align(pt->width[0] * pt->block.size, 64);
		else
			nv30mt->level[l].pitch = pt->width[l] * pt->block.size;

		nv30mt->level[l].image_offset =
			CALLOC(nr_faces, sizeof(unsigned));

		width  = MAX2(1, width  >> 1);
		height = MAX2(1, height >> 1);
		depth  = MAX2(1, depth  >> 1);
	}

	for (f = 0; f < nr_faces; f++) {
		for (l = 0; l < pt->last_level; l++) {
			nv30mt->level[l].image_offset[f] = offset;

			if (!(pt->tex_usage & NOUVEAU_TEXTURE_USAGE_LINEAR) &&
			    pt->width[l + 1] > 1 && pt->height[l + 1] > 1)
				offset += align(nv30mt->level[l].pitch * pt->height[l], 64);
			else
				offset += nv30mt->level[l].pitch * pt->height[l];
		}

		nv30mt->level[l].image_offset[f] = offset;
		offset += nv30mt->level[l].pitch * pt->height[l];
	}

	nv30mt->total_size = offset;
}

static struct pipe_texture *
nv30_miptree_create(struct pipe_screen *pscreen, const struct pipe_texture *pt)
{
	struct pipe_winsys *ws = pscreen->winsys;
	struct nv30_miptree *mt;

	mt = MALLOC(sizeof(struct nv30_miptree));
	if (!mt)
		return NULL;
	mt->base = *pt;
	mt->base.refcount = 1;
	mt->base.screen = pscreen;
	mt->shadow_tex = NULL;
	mt->shadow_surface = NULL;

	/* Swizzled textures must be POT */
	if (pt->width[0] & (pt->width[0] - 1) ||
	    pt->height[0] & (pt->height[0] - 1))
		mt->base.tex_usage |= NOUVEAU_TEXTURE_USAGE_LINEAR;
	else
	if (pt->tex_usage & (PIPE_TEXTURE_USAGE_PRIMARY |
	                     PIPE_TEXTURE_USAGE_DISPLAY_TARGET |
	                     PIPE_TEXTURE_USAGE_DEPTH_STENCIL))
		mt->base.tex_usage |= NOUVEAU_TEXTURE_USAGE_LINEAR;
	else
	if (pt->tex_usage & PIPE_TEXTURE_USAGE_DYNAMIC)
		mt->base.tex_usage |= NOUVEAU_TEXTURE_USAGE_LINEAR;
	else {
		switch (pt->format) {
		/* TODO: Figure out which formats can be swizzled */
		case PIPE_FORMAT_A8R8G8B8_UNORM:
		case PIPE_FORMAT_X8R8G8B8_UNORM:
		case PIPE_FORMAT_R16_SNORM:
		{
			if (debug_get_bool_option("NOUVEAU_NO_SWIZZLE", FALSE))
				mt->base.tex_usage |= NOUVEAU_TEXTURE_USAGE_LINEAR;
 			break;
		}
		default:
			mt->base.tex_usage |= NOUVEAU_TEXTURE_USAGE_LINEAR;
		}
	}

	nv30_miptree_layout(mt);

	mt->buffer = ws->buffer_create(ws, 256,
				       PIPE_BUFFER_USAGE_PIXEL |
				       NOUVEAU_BUFFER_USAGE_TEXTURE,
				       mt->total_size);
	if (!mt->buffer) {
		FREE(mt);
		return NULL;
	}

	return &mt->base;
}

static struct pipe_texture *
nv30_miptree_blanket(struct pipe_screen *pscreen, const struct pipe_texture *pt,
		     const unsigned *stride, struct pipe_buffer *pb)
{
	struct nv30_miptree *mt;

	/* Only supports 2D, non-mipmapped textures for the moment */
	if (pt->target != PIPE_TEXTURE_2D || pt->last_level != 0 ||
	    pt->depth[0] != 1)
		return NULL;

	mt = CALLOC_STRUCT(nv30_miptree);
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

static void
nv30_miptree_release(struct pipe_screen *pscreen, struct pipe_texture **ppt)
{
	struct pipe_texture *pt = *ppt;
	struct nv30_miptree *mt = (struct nv30_miptree *)pt;
	int l;

	*ppt = NULL;
	if (--pt->refcount)
		return;

	pipe_buffer_reference(pscreen, &mt->buffer, NULL);
	for (l = 0; l <= pt->last_level; l++) {
		if (mt->level[l].image_offset)
			FREE(mt->level[l].image_offset);
	}

	if (mt->shadow_tex) {
		if (mt->shadow_surface)
			pscreen->tex_surface_release(pscreen, &mt->shadow_surface);
		nv30_miptree_release(pscreen, &mt->shadow_tex);
	}

	FREE(mt);
}

static struct pipe_surface *
nv30_miptree_surface_new(struct pipe_screen *pscreen, struct pipe_texture *pt,
			 unsigned face, unsigned level, unsigned zslice,
			 unsigned flags)
{
	struct nv30_miptree *nv30mt = (struct nv30_miptree *)pt;
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
	ps->stride = nv30mt->level[level].pitch;
	ps->usage = flags;
	ps->status = PIPE_SURFACE_STATUS_DEFINED;
	ps->refcount = 1;
	ps->face = face;
	ps->level = level;
	ps->zslice = zslice;

	if (pt->target == PIPE_TEXTURE_CUBE) {
		ps->offset = nv30mt->level[level].image_offset[face];
	} else
	if (pt->target == PIPE_TEXTURE_3D) {
		ps->offset = nv30mt->level[level].image_offset[zslice];
	} else {
		ps->offset = nv30mt->level[level].image_offset[0];
	}

	return ps;
}

static void
nv30_miptree_surface_del(struct pipe_screen *pscreen,
			 struct pipe_surface **psurface)
{
	struct pipe_surface *ps = *psurface;

	*psurface = NULL;
	if (--ps->refcount > 0)
		return;

	pipe_texture_reference(&ps->texture, NULL);
	FREE(ps);
}

void
nv30_screen_init_miptree_functions(struct pipe_screen *pscreen)
{
	pscreen->texture_create = nv30_miptree_create;
	pscreen->texture_blanket = nv30_miptree_blanket;
	pscreen->texture_release = nv30_miptree_release;
	pscreen->get_tex_surface = nv30_miptree_surface_new;
	pscreen->tex_surface_release = nv30_miptree_surface_del;
}
