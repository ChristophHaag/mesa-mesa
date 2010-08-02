/*
 * Copyright 2010 Jerome Glisse <glisse@freedesktop.org>
 *
 * Permission is hereby granted, free of charge, to any person obtaining a
 * copy of this software and associated documentation files (the "Software"),
 * to deal in the Software without restriction, including without limitation
 * on the rights to use, copy, modify, merge, publish, distribute, sub
 * license, and/or sell copies of the Software, and to permit persons to whom
 * the Software is furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice (including the next
 * paragraph) shall be included in all copies or substantial portions of the
 * Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NON-INFRINGEMENT. IN NO EVENT SHALL
 * THE AUTHOR(S) AND/OR THEIR SUPPLIERS BE LIABLE FOR ANY CLAIM,
 * DAMAGES OR OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR
 * OTHERWISE, ARISING FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE
 * USE OR OTHER DEALINGS IN THE SOFTWARE.
 *
 * Authors:
 *      Jerome Glisse
 */
#include <stdio.h>
#include <errno.h>
#include "util/u_inlines.h"
#include "util/u_format.h"
#include "util/u_memory.h"
#include "r600_screen.h"
#include "r600_context.h"
#include "r600_resource.h"
#include "r600d.h"
#include "r600_reg.h"
#include "r600_state_inlines.h"

static void *r600_create_blend_state(struct pipe_context *ctx,
					const struct pipe_blend_state *state)
{
	struct r600_context *rctx = r600_context(ctx);

	return r600_context_state(rctx, pipe_blend_type, state);
}

static void *r600_create_dsa_state(struct pipe_context *ctx,
					const struct pipe_depth_stencil_alpha_state *state)
{
	struct r600_context *rctx = r600_context(ctx);

	return r600_context_state(rctx, pipe_dsa_type, state);
}

static void *r600_create_rs_state(struct pipe_context *ctx,
					const struct pipe_rasterizer_state *state)
{
	struct r600_context *rctx = r600_context(ctx);

	return r600_context_state(rctx, pipe_rasterizer_type, state);
}

static void *r600_create_sampler_state(struct pipe_context *ctx,
					const struct pipe_sampler_state *state)
{
	struct r600_context *rctx = r600_context(ctx);

	return r600_context_state(rctx, pipe_sampler_type, state);
}

static void r600_sampler_view_destroy(struct pipe_context *ctx,
				      struct pipe_sampler_view *state)
{
	struct r600_context_state *rstate = (struct r600_context_state *)state;

	r600_context_state_decref(rstate);
}

static struct pipe_sampler_view *r600_create_sampler_view(struct pipe_context *ctx,
							struct pipe_resource *texture,
							const struct pipe_sampler_view *state)
{
	struct r600_context *rctx = r600_context(ctx);
	struct r600_context_state *rstate;

	rstate = r600_context_state(rctx, pipe_sampler_type, state);
	pipe_reference(NULL, &texture->reference);
	rstate->state.sampler_view.texture = texture;
	rstate->state.sampler_view.reference.count = 1;
	rstate->state.sampler_view.context = ctx;
	return &rstate->state.sampler_view;
}

static void *r600_create_shader_state(struct pipe_context *ctx,
					const struct pipe_shader_state *state)
{
	struct r600_context *rctx = r600_context(ctx);

	return r600_context_state(rctx, pipe_shader_type, state);
}

static void *r600_create_vertex_elements(struct pipe_context *ctx,
				unsigned count,
				const struct pipe_vertex_element *elements)
{
	struct r600_vertex_element *v = CALLOC_STRUCT(r600_vertex_element);

	assert(count < 32);
	v->count = count;
	memcpy(v->elements, elements, count * sizeof(struct pipe_vertex_element));
	v->refcount = 1;
	return v;
}

static void r600_bind_state(struct pipe_context *ctx, void *state)
{
	struct r600_context *rctx = r600_context(ctx);
	struct r600_context_state *rstate = (struct r600_context_state *)state;

	if (state == NULL)
		return;
	switch (rstate->type) {
	case pipe_rasterizer_type:
		rctx->rasterizer = r600_context_state_decref(rctx->rasterizer);
		rctx->rasterizer = r600_context_state_incref(rstate);
		break;
	case pipe_poly_stipple_type:
		rctx->poly_stipple = r600_context_state_decref(rctx->poly_stipple);
		rctx->poly_stipple = r600_context_state_incref(rstate);
		break;
	case pipe_scissor_type:
		rctx->scissor = r600_context_state_decref(rctx->scissor);
		rctx->scissor = r600_context_state_incref(rstate);
		break;
	case pipe_clip_type:
		rctx->clip = r600_context_state_decref(rctx->clip);
		rctx->clip = r600_context_state_incref(rstate);
		break;
	case pipe_depth_type:
		rctx->depth = r600_context_state_decref(rctx->depth);
		rctx->depth = r600_context_state_incref(rstate);
		break;
	case pipe_stencil_type:
		rctx->stencil = r600_context_state_decref(rctx->stencil);
		rctx->stencil = r600_context_state_incref(rstate);
		break;
	case pipe_alpha_type:
		rctx->alpha = r600_context_state_decref(rctx->alpha);
		rctx->alpha = r600_context_state_incref(rstate);
		break;
	case pipe_dsa_type:
		rctx->dsa = r600_context_state_decref(rctx->dsa);
		rctx->dsa = r600_context_state_incref(rstate);
		break;
	case pipe_blend_type:
		rctx->blend = r600_context_state_decref(rctx->blend);
		rctx->blend = r600_context_state_incref(rstate);
		break;
	case pipe_framebuffer_type:
		rctx->framebuffer = r600_context_state_decref(rctx->framebuffer);
		rctx->framebuffer = r600_context_state_incref(rstate);
		break;
	case pipe_stencil_ref_type:
		rctx->stencil_ref = r600_context_state_decref(rctx->stencil_ref);
		rctx->stencil_ref = r600_context_state_incref(rstate);
		break;
	case pipe_viewport_type:
		rctx->viewport = r600_context_state_decref(rctx->viewport);
		rctx->viewport = r600_context_state_incref(rstate);
		break;
	case pipe_shader_type:
	case pipe_sampler_type:
	case pipe_sampler_view_type:
	default:
		R600_ERR("invalid type %d\n", rstate->type);
		return;
	}
}

static void r600_bind_ps_shader(struct pipe_context *ctx, void *state)
{
	struct r600_context *rctx = r600_context(ctx);
	struct r600_context_state *rstate = (struct r600_context_state *)state;

	rctx->ps_shader = r600_context_state_decref(rctx->ps_shader);
	rctx->ps_shader = r600_context_state_incref(rstate);
}

static void r600_bind_vs_shader(struct pipe_context *ctx, void *state)
{
	struct r600_context *rctx = r600_context(ctx);
	struct r600_context_state *rstate = (struct r600_context_state *)state;

	rctx->vs_shader = r600_context_state_decref(rctx->vs_shader);
	rctx->vs_shader = r600_context_state_incref(rstate);
}

static void r600_delete_vertex_element(struct pipe_context *ctx, void *state)
{
	struct r600_vertex_element *v = (struct r600_vertex_element*)state;

	if (v == NULL)
		return;
	if (--v->refcount)
		return;
	free(v);
}

static void r600_bind_vertex_elements(struct pipe_context *ctx, void *state)
{
	struct r600_context *rctx = r600_context(ctx);
	struct r600_vertex_element *v = (struct r600_vertex_element*)state;

	r600_delete_vertex_element(ctx, rctx->vertex_elements);
	rctx->vertex_elements = v;
	if (v) {
		v->refcount++;
	}
}

static void r600_bind_ps_sampler(struct pipe_context *ctx,
					unsigned count, void **states)
{
	struct r600_context *rctx = r600_context(ctx);
	struct r600_context_state *rstate;
	unsigned i;

	for (i = 0; i < rctx->ps_nsampler; i++) {
		rctx->ps_sampler[i] = r600_context_state_decref(rctx->ps_sampler[i]);
	}
	for (i = 0; i < count; i++) {
		rstate = (struct r600_context_state *)states[i];
		rctx->ps_sampler[i] = r600_context_state_incref(rstate);
	}
	rctx->ps_nsampler = count;
}

static void r600_bind_vs_sampler(struct pipe_context *ctx,
					unsigned count, void **states)
{
	struct r600_context *rctx = r600_context(ctx);
	struct r600_context_state *rstate;
	unsigned i;

	for (i = 0; i < rctx->vs_nsampler; i++) {
		rctx->vs_sampler[i] = r600_context_state_decref(rctx->vs_sampler[i]);
	}
	for (i = 0; i < count; i++) {
		rstate = (struct r600_context_state *)states[i];
		rctx->vs_sampler[i] = r600_context_state_incref(rstate);
	}
	rctx->vs_nsampler = count;
}

static void r600_delete_state(struct pipe_context *ctx, void *state)
{
	struct r600_context_state *rstate = (struct r600_context_state *)state;

	r600_context_state_decref(rstate);
}

static void r600_set_blend_color(struct pipe_context *ctx,
					const struct pipe_blend_color *color)
{
	struct r600_context *rctx = r600_context(ctx);

	rctx->blend_color = *color;
}

static void r600_set_clip_state(struct pipe_context *ctx,
				const struct pipe_clip_state *state)
{
}

static void r600_set_constant_buffer(struct pipe_context *ctx,
					uint shader, uint index,
					struct pipe_resource *buffer)
{
	struct r600_screen *rscreen = r600_screen(ctx->screen);
	struct r600_context *rctx = r600_context(ctx);
	unsigned nconstant = 0, i, type, id;
	struct radeon_state *rstate;
	struct pipe_transfer *transfer;
	u32 *ptr;

	switch (shader) {
	case PIPE_SHADER_VERTEX:
		id = R600_VS_CONSTANT;
		type = R600_VS_CONSTANT_TYPE;
		break;
	case PIPE_SHADER_FRAGMENT:
		id = R600_PS_CONSTANT;
		type = R600_PS_CONSTANT_TYPE;
		break;
	default:
		fprintf(stderr, "%s:%d unsupported %d\n", __func__, __LINE__, shader);
		return;
	}
	if (buffer && buffer->width0 > 0) {
		nconstant = buffer->width0 / 16;
		ptr = pipe_buffer_map(ctx, buffer, PIPE_TRANSFER_READ, &transfer);
		if (ptr == NULL)
			return;
		for (i = 0; i < nconstant; i++) {
			rstate = radeon_state(rscreen->rw, type, id + i);
			if (rstate == NULL)
				return;
			rstate->states[R600_PS_CONSTANT__SQ_ALU_CONSTANT0_0] = ptr[i * 4 + 0];
			rstate->states[R600_PS_CONSTANT__SQ_ALU_CONSTANT1_0] = ptr[i * 4 + 1];
			rstate->states[R600_PS_CONSTANT__SQ_ALU_CONSTANT2_0] = ptr[i * 4 + 2];
			rstate->states[R600_PS_CONSTANT__SQ_ALU_CONSTANT3_0] = ptr[i * 4 + 3];
			if (radeon_state_pm4(rstate))
				return;
			if (radeon_draw_set_new(rctx->draw, rstate))
				return;
		}
		pipe_buffer_unmap(ctx, buffer, transfer);
	}
}

static void r600_set_ps_sampler_view(struct pipe_context *ctx,
					unsigned count,
					struct pipe_sampler_view **views)
{
	struct r600_context *rctx = r600_context(ctx);
	struct r600_context_state *rstate;
	unsigned i;

	for (i = 0; i < rctx->ps_nsampler_view; i++) {
		rctx->ps_sampler_view[i] = r600_context_state_decref(rctx->ps_sampler_view[i]);
	}
	for (i = 0; i < count; i++) {
		rstate = (struct r600_context_state *)views[i];
		rctx->ps_sampler_view[i] = r600_context_state_incref(rstate);
	}
	rctx->ps_nsampler_view = count;
}

static void r600_set_vs_sampler_view(struct pipe_context *ctx,
					unsigned count,
					struct pipe_sampler_view **views)
{
	struct r600_context *rctx = r600_context(ctx);
	struct r600_context_state *rstate;
	unsigned i;

	for (i = 0; i < rctx->vs_nsampler_view; i++) {
		rctx->vs_sampler_view[i] = r600_context_state_decref(rctx->vs_sampler_view[i]);
	}
	for (i = 0; i < count; i++) {
		rstate = (struct r600_context_state *)views[i];
		rctx->vs_sampler_view[i] = r600_context_state_incref(rstate);
	}
	rctx->vs_nsampler_view = count;
}

static void r600_set_framebuffer_state(struct pipe_context *ctx,
					const struct pipe_framebuffer_state *state)
{
	struct r600_context *rctx = r600_context(ctx);
	struct r600_context_state *rstate;

	rstate = r600_context_state(rctx, pipe_framebuffer_type, state);
	r600_bind_state(ctx, rstate);
}

static void r600_set_polygon_stipple(struct pipe_context *ctx,
					 const struct pipe_poly_stipple *state)
{
}

static void r600_set_sample_mask(struct pipe_context *pipe, unsigned sample_mask)
{
}

static void r600_set_scissor_state(struct pipe_context *ctx,
					const struct pipe_scissor_state *state)
{
	struct r600_context *rctx = r600_context(ctx);
	struct r600_context_state *rstate;

	rstate = r600_context_state(rctx, pipe_scissor_type, state);
	r600_bind_state(ctx, rstate);
}

static void r600_set_stencil_ref(struct pipe_context *ctx,
				const struct pipe_stencil_ref *state)
{
	struct r600_context *rctx = r600_context(ctx);
	struct r600_context_state *rstate;

	rstate = r600_context_state(rctx, pipe_stencil_ref_type, state);
	r600_bind_state(ctx, rstate);
}

static void r600_set_vertex_buffers(struct pipe_context *ctx,
					unsigned count,
					const struct pipe_vertex_buffer *buffers)
{
	struct r600_context *rctx = r600_context(ctx);
	unsigned i;

	for (i = 0; i < rctx->nvertex_buffer; i++) {
		pipe_resource_reference(&rctx->vertex_buffer[i].buffer, NULL);
	}
	memcpy(rctx->vertex_buffer, buffers, sizeof(struct pipe_vertex_buffer) * count);
	for (i = 0; i < count; i++) {
		rctx->vertex_buffer[i].buffer = NULL;
		pipe_resource_reference(&rctx->vertex_buffer[i].buffer, buffers[i].buffer);
	}
	rctx->nvertex_buffer = count;
}

static void r600_set_index_buffer(struct pipe_context *ctx,
				  const struct pipe_index_buffer *ib)
{
	struct r600_context *rctx = r600_context(ctx);

	if (ib) {
		pipe_resource_reference(&rctx->index_buffer.buffer, ib->buffer);
		memcpy(&rctx->index_buffer, ib, sizeof(rctx->index_buffer));
	}
	else {
		pipe_resource_reference(&rctx->index_buffer.buffer, NULL);
		memset(&rctx->index_buffer, 0, sizeof(rctx->index_buffer));
	}

	/* TODO make this more like a state */
}

static void r600_set_viewport_state(struct pipe_context *ctx,
					const struct pipe_viewport_state *state)
{
	struct r600_context *rctx = r600_context(ctx);
	struct r600_context_state *rstate;

	rstate = r600_context_state(rctx, pipe_viewport_type, state);
	r600_bind_state(ctx, rstate);
}

void r600_init_state_functions(struct r600_context *rctx)
{
	rctx->context.create_blend_state = r600_create_blend_state;
	rctx->context.create_depth_stencil_alpha_state = r600_create_dsa_state;
	rctx->context.create_fs_state = r600_create_shader_state;
	rctx->context.create_rasterizer_state = r600_create_rs_state;
	rctx->context.create_sampler_state = r600_create_sampler_state;
	rctx->context.create_sampler_view = r600_create_sampler_view;
	rctx->context.create_vertex_elements_state = r600_create_vertex_elements;
	rctx->context.create_vs_state = r600_create_shader_state;
	rctx->context.bind_blend_state = r600_bind_state;
	rctx->context.bind_depth_stencil_alpha_state = r600_bind_state;
	rctx->context.bind_fragment_sampler_states = r600_bind_ps_sampler;
	rctx->context.bind_fs_state = r600_bind_ps_shader;
	rctx->context.bind_rasterizer_state = r600_bind_state;
	rctx->context.bind_vertex_elements_state = r600_bind_vertex_elements;
	rctx->context.bind_vertex_sampler_states = r600_bind_vs_sampler;
	rctx->context.bind_vs_state = r600_bind_vs_shader;
	rctx->context.delete_blend_state = r600_delete_state;
	rctx->context.delete_depth_stencil_alpha_state = r600_delete_state;
	rctx->context.delete_fs_state = r600_delete_state;
	rctx->context.delete_rasterizer_state = r600_delete_state;
	rctx->context.delete_sampler_state = r600_delete_state;
	rctx->context.delete_vertex_elements_state = r600_delete_vertex_element;
	rctx->context.delete_vs_state = r600_delete_state;
	rctx->context.set_blend_color = r600_set_blend_color;
	rctx->context.set_clip_state = r600_set_clip_state;
	rctx->context.set_constant_buffer = r600_set_constant_buffer;
	rctx->context.set_fragment_sampler_views = r600_set_ps_sampler_view;
	rctx->context.set_framebuffer_state = r600_set_framebuffer_state;
	rctx->context.set_polygon_stipple = r600_set_polygon_stipple;
	rctx->context.set_sample_mask = r600_set_sample_mask;
	rctx->context.set_scissor_state = r600_set_scissor_state;
	rctx->context.set_stencil_ref = r600_set_stencil_ref;
	rctx->context.set_vertex_buffers = r600_set_vertex_buffers;
	rctx->context.set_index_buffer = r600_set_index_buffer;
	rctx->context.set_vertex_sampler_views = r600_set_vs_sampler_view;
	rctx->context.set_viewport_state = r600_set_viewport_state;
	rctx->context.sampler_view_destroy = r600_sampler_view_destroy;
}

struct r600_context_state *r600_context_state_incref(struct r600_context_state *rstate)
{
	if (rstate == NULL)
		return NULL;
	rstate->refcount++;
	return rstate;
}

struct r600_context_state *r600_context_state_decref(struct r600_context_state *rstate)
{
	unsigned i;

	if (rstate == NULL)
		return NULL;
	if (--rstate->refcount)
		return NULL;
	switch (rstate->type) {
	case pipe_sampler_view_type:
		pipe_resource_reference(&rstate->state.sampler_view.texture, NULL);
		break;
	case pipe_framebuffer_type:
		for (i = 0; i < rstate->state.framebuffer.nr_cbufs; i++) {
			pipe_surface_reference(&rstate->state.framebuffer.cbufs[i], NULL);
		}
		pipe_surface_reference(&rstate->state.framebuffer.zsbuf, NULL);
		break;
	case pipe_viewport_type:
	case pipe_depth_type:
	case pipe_rasterizer_type:
	case pipe_poly_stipple_type:
	case pipe_scissor_type:
	case pipe_clip_type:
	case pipe_stencil_type:
	case pipe_alpha_type:
	case pipe_dsa_type:
	case pipe_blend_type:
	case pipe_stencil_ref_type:
	case pipe_shader_type:
	case pipe_sampler_type:
		break;
	default:
		R600_ERR("invalid type %d\n", rstate->type);
		return NULL;
	}
	radeon_state_decref(rstate->rstate);
	FREE(rstate);
	return NULL;
}

struct r600_context_state *r600_context_state(struct r600_context *rctx, unsigned type, const void *state)
{
	struct r600_context_state *rstate = CALLOC_STRUCT(r600_context_state);
	const union pipe_states *states = state;
	unsigned i;
	int r;

	if (rstate == NULL)
		return NULL;
	rstate->type = type;
	rstate->refcount = 1;

	switch (rstate->type) {
	case pipe_sampler_view_type:
		rstate->state.sampler_view = (*states).sampler_view;
		rstate->state.sampler_view.texture = NULL;
		break;
	case pipe_framebuffer_type:
		rstate->state.framebuffer = (*states).framebuffer;
		for (i = 0; i < rstate->state.framebuffer.nr_cbufs; i++) {
			pipe_surface_reference(&rstate->state.framebuffer.cbufs[i],
						(*states).framebuffer.cbufs[i]);
		}
		pipe_surface_reference(&rstate->state.framebuffer.zsbuf,
					(*states).framebuffer.zsbuf);
		break;
	case pipe_viewport_type:
		rstate->state.viewport = (*states).viewport;
		break;
	case pipe_depth_type:
		rstate->state.depth = (*states).depth;
		break;
	case pipe_rasterizer_type:
		rstate->state.rasterizer = (*states).rasterizer;
		break;
	case pipe_poly_stipple_type:
		rstate->state.poly_stipple = (*states).poly_stipple;
		break;
	case pipe_scissor_type:
		rstate->state.scissor = (*states).scissor;
		break;
	case pipe_clip_type:
		rstate->state.clip = (*states).clip;
		break;
	case pipe_stencil_type:
		rstate->state.stencil = (*states).stencil;
		break;
	case pipe_alpha_type:
		rstate->state.alpha = (*states).alpha;
		break;
	case pipe_dsa_type:
		rstate->state.dsa = (*states).dsa;
		break;
	case pipe_blend_type:
		rstate->state.blend = (*states).blend;
		break;
	case pipe_stencil_ref_type:
		rstate->state.stencil_ref = (*states).stencil_ref;
		break;
	case pipe_shader_type:
		rstate->state.shader = (*states).shader;
		r =  r600_pipe_shader_create(&rctx->context, rstate, rstate->state.shader.tokens);
		if (r) {
			r600_context_state_decref(rstate);
			return NULL;
		}
		break;
	case pipe_sampler_type:
		rstate->state.sampler = (*states).sampler;
		break;
	default:
		R600_ERR("invalid type %d\n", rstate->type);
		FREE(rstate);
		return NULL;
	}
	return rstate;
}

static struct radeon_state *r600_blend(struct r600_context *rctx)
{
	struct r600_screen *rscreen = rctx->screen;
	struct radeon_state *rstate;
	const struct pipe_blend_state *state = &rctx->blend->state.blend;
	int i;

	rstate = radeon_state(rscreen->rw, R600_BLEND_TYPE, R600_BLEND);
	if (rstate == NULL)
		return NULL;
	rstate->states[R600_BLEND__CB_BLEND_RED] = fui(rctx->blend_color.color[0]);
	rstate->states[R600_BLEND__CB_BLEND_GREEN] = fui(rctx->blend_color.color[1]);
	rstate->states[R600_BLEND__CB_BLEND_BLUE] = fui(rctx->blend_color.color[2]);
	rstate->states[R600_BLEND__CB_BLEND_ALPHA] = fui(rctx->blend_color.color[3]);
	rstate->states[R600_BLEND__CB_BLEND0_CONTROL] = 0x00000000;
	rstate->states[R600_BLEND__CB_BLEND1_CONTROL] = 0x00000000;
	rstate->states[R600_BLEND__CB_BLEND2_CONTROL] = 0x00000000;
	rstate->states[R600_BLEND__CB_BLEND3_CONTROL] = 0x00000000;
	rstate->states[R600_BLEND__CB_BLEND4_CONTROL] = 0x00000000;
	rstate->states[R600_BLEND__CB_BLEND5_CONTROL] = 0x00000000;
	rstate->states[R600_BLEND__CB_BLEND6_CONTROL] = 0x00000000;
	rstate->states[R600_BLEND__CB_BLEND7_CONTROL] = 0x00000000;
	rstate->states[R600_BLEND__CB_BLEND_CONTROL] = 0x00000000;

	for (i = 0; i < 8; i++) {

		unsigned eqRGB = state->rt[i].rgb_func;
		unsigned srcRGB = state->rt[i].rgb_src_factor;
		unsigned dstRGB = state->rt[i].rgb_dst_factor;
		
		unsigned eqA = state->rt[i].alpha_func;
		unsigned srcA = state->rt[i].alpha_src_factor;
		unsigned dstA = state->rt[i].alpha_dst_factor;
		uint32_t bc = 0;

		if (!state->rt[i].blend_enable)
			continue;

		bc |= r600_translate_blend_function(eqRGB) << CB_BLEND_COLOR_COMB_FCN_SHIFT;
		bc |= r600_translate_blend_factor(srcRGB) << CB_BLEND_COLOR_SRCBLEND_SHIFT;
		bc |= r600_translate_blend_factor(dstRGB) << CB_BLEND_COLOR_DESTBLEND_SHIFT;

		if (srcA != srcRGB || dstA != dstRGB || eqA != eqRGB) {
			bc |= CB_BLEND_SEPARATE_ALPHA_BLEND;
			bc |= r600_translate_blend_function(eqA) << CB_BLEND_ALPHA_COMB_FCN_SHIFT;
			bc |= r600_translate_blend_factor(srcA) << CB_BLEND_ALPHA_SRCBLEND_SHIFT;
			bc |= r600_translate_blend_factor(dstA) << CB_BLEND_ALPHA_DESTBLEND_SHIFT;
		}

		rstate->states[R600_BLEND__CB_BLEND0_CONTROL + i] = bc;
		if (i == 0)
			rstate->states[R600_BLEND__CB_BLEND_CONTROL] = bc;
	}

	if (radeon_state_pm4(rstate)) {
		radeon_state_decref(rstate);
		return NULL;
	}
	return rstate;
}

static struct radeon_state *r600_cb0(struct r600_context *rctx)
{
	struct r600_screen *rscreen = rctx->screen;
	struct r600_resource_texture *rtex;
	struct r600_resource *rbuffer;
	struct radeon_state *rstate;
	const struct pipe_framebuffer_state *state = &rctx->framebuffer->state.framebuffer;
	unsigned level = state->cbufs[0]->u.tex.level;
	unsigned pitch, slice;

	rstate = radeon_state(rscreen->rw, R600_CB0_TYPE, R600_CB0);
	if (rstate == NULL)
		return NULL;
	rtex = (struct r600_resource_texture*)state->cbufs[0]->texture;
	rbuffer = &rtex->resource;
	rstate->bo[0] = radeon_bo_incref(rscreen->rw, rbuffer->bo);
	rstate->bo[1] = radeon_bo_incref(rscreen->rw, rbuffer->bo);
	rstate->bo[2] = radeon_bo_incref(rscreen->rw, rbuffer->bo);
	rstate->placement[0] = RADEON_GEM_DOMAIN_GTT;
	rstate->placement[2] = RADEON_GEM_DOMAIN_GTT;
	rstate->placement[4] = RADEON_GEM_DOMAIN_GTT;
	rstate->nbo = 3;
	pitch = (rtex->pitch[level] / rtex->bpt) / 8 - 1;
	slice = (rtex->pitch[level] / rtex->bpt) * state->cbufs[0]->height / 64 - 1;
	rstate->states[R600_CB0__CB_COLOR0_BASE] = 0x00000000;
	rstate->states[R600_CB0__CB_COLOR0_INFO] = 0x08110068;
	rstate->states[R600_CB0__CB_COLOR0_SIZE] = S_028060_PITCH_TILE_MAX(pitch) |
						S_028060_SLICE_TILE_MAX(slice);
	rstate->states[R600_CB0__CB_COLOR0_VIEW] = 0x00000000;
	rstate->states[R600_CB0__CB_COLOR0_FRAG] = 0x00000000;
	rstate->states[R600_CB0__CB_COLOR0_TILE] = 0x00000000;
	rstate->states[R600_CB0__CB_COLOR0_MASK] = 0x00000000;
	if (radeon_state_pm4(rstate)) {
		radeon_state_decref(rstate);
		return NULL;
	}
	return rstate;
}

static int r600_db_format(unsigned pformat, unsigned *format)
{
	switch (pformat) {
	case PIPE_FORMAT_Z24X8_UNORM:
		*format = V_028010_DEPTH_X8_24;
		return 0;
	case PIPE_FORMAT_Z24_UNORM_S8_USCALED:
		*format = V_028010_DEPTH_8_24;
		return 0;
	default:
		*format = V_028010_DEPTH_INVALID;
		R600_ERR("unsupported %d\n", pformat);
		return -EINVAL;
	}
}

static struct radeon_state *r600_db(struct r600_context *rctx)
{
	struct r600_screen *rscreen = rctx->screen;
	struct r600_resource_texture *rtex;
	struct r600_resource *rbuffer;
	struct radeon_state *rstate;
	const struct pipe_framebuffer_state *state = &rctx->framebuffer->state.framebuffer;
	unsigned level;
	unsigned pitch, slice, format;

	if (state->zsbuf == NULL)
		return NULL;

	level = state->zsbuf->u.tex.level;
	rstate = radeon_state(rscreen->rw, R600_DB_TYPE, R600_DB);
	if (rstate == NULL)
		return NULL;

	rtex = (struct r600_resource_texture*)state->zsbuf->texture;
	rbuffer = &rtex->resource;
	rstate->bo[0] = radeon_bo_incref(rscreen->rw, rbuffer->bo);
	rstate->nbo = 1;
	rstate->placement[0] = RADEON_GEM_DOMAIN_VRAM;
	level = state->zsbuf->u.tex.level;
	pitch = (rtex->pitch[level] / rtex->bpt) / 8 - 1;
	slice = (rtex->pitch[level] / rtex->bpt) * state->zsbuf->height / 64 - 1;
	if (r600_db_format(state->zsbuf->texture->format, &format)) {
		radeon_state_decref(rstate);
		return NULL;
	}
	rstate->states[R600_DB__DB_DEPTH_BASE] = 0x00000000;
	rstate->states[R600_DB__DB_DEPTH_INFO] = 0x00010000 |
					S_028010_FORMAT(format);
	rstate->states[R600_DB__DB_DEPTH_VIEW] = 0x00000000;
	rstate->states[R600_DB__DB_PREFETCH_LIMIT] = (state->zsbuf->height / 8) -1;
	rstate->states[R600_DB__DB_DEPTH_SIZE] = S_028000_PITCH_TILE_MAX(pitch) |
						S_028000_SLICE_TILE_MAX(slice);
	if (radeon_state_pm4(rstate)) {
		radeon_state_decref(rstate);
		return NULL;
	}
	return rstate;
}

static struct radeon_state *r600_rasterizer(struct r600_context *rctx)
{
	const struct pipe_rasterizer_state *state = &rctx->rasterizer->state.rasterizer;
	struct r600_screen *rscreen = rctx->screen;
	struct radeon_state *rstate;

	rctx->flat_shade = state->flatshade;
	rstate = radeon_state(rscreen->rw, R600_RASTERIZER_TYPE, R600_RASTERIZER);
	if (rstate == NULL)
		return NULL;
	rstate->states[R600_RASTERIZER__SPI_INTERP_CONTROL_0] = 0x00000001;
	rstate->states[R600_RASTERIZER__PA_CL_CLIP_CNTL] = 0x00000000;
	rstate->states[R600_RASTERIZER__PA_SU_SC_MODE_CNTL] = 0x00080000 |
			S_028814_CULL_FRONT((state->cull_face & PIPE_FACE_FRONT) ? 1 : 0) |
			S_028814_CULL_BACK((state->cull_face & PIPE_FACE_BACK) ? 1 : 0) |
			S_028814_FACE(!state->front_ccw);
	rstate->states[R600_RASTERIZER__PA_CL_VS_OUT_CNTL] = 0x00000000;
	rstate->states[R600_RASTERIZER__PA_CL_NANINF_CNTL] = 0x00000000;
	rstate->states[R600_RASTERIZER__PA_SU_POINT_SIZE] = 0x00080008;
	rstate->states[R600_RASTERIZER__PA_SU_POINT_MINMAX] = 0x00000000;
	rstate->states[R600_RASTERIZER__PA_SU_LINE_CNTL] = 0x00000008;
	rstate->states[R600_RASTERIZER__PA_SC_LINE_STIPPLE] = 0x00000005;
	rstate->states[R600_RASTERIZER__PA_SC_MPASS_PS_CNTL] = 0x00000000;
	rstate->states[R600_RASTERIZER__PA_SC_LINE_CNTL] = 0x00000400;
	rstate->states[R600_RASTERIZER__PA_CL_GB_VERT_CLIP_ADJ] = 0x3F800000;
	rstate->states[R600_RASTERIZER__PA_CL_GB_VERT_DISC_ADJ] = 0x3F800000;
	rstate->states[R600_RASTERIZER__PA_CL_GB_HORZ_CLIP_ADJ] = 0x3F800000;
	rstate->states[R600_RASTERIZER__PA_CL_GB_HORZ_DISC_ADJ] = 0x3F800000;
	rstate->states[R600_RASTERIZER__PA_SU_POLY_OFFSET_DB_FMT_CNTL] = 0x00000000;
	rstate->states[R600_RASTERIZER__PA_SU_POLY_OFFSET_CLAMP] = 0x00000000;
	rstate->states[R600_RASTERIZER__PA_SU_POLY_OFFSET_FRONT_SCALE] = 0x00000000;
	rstate->states[R600_RASTERIZER__PA_SU_POLY_OFFSET_FRONT_OFFSET] = 0x00000000;
	rstate->states[R600_RASTERIZER__PA_SU_POLY_OFFSET_BACK_SCALE] = 0x00000000;
	rstate->states[R600_RASTERIZER__PA_SU_POLY_OFFSET_BACK_OFFSET] = 0x00000000;
	if (radeon_state_pm4(rstate)) {
		radeon_state_decref(rstate);
		return NULL;
	}
	return rstate;
}

static struct radeon_state *r600_scissor(struct r600_context *rctx)
{
	const struct pipe_scissor_state *state = &rctx->scissor->state.scissor;
	struct r600_screen *rscreen = rctx->screen;
	struct radeon_state *rstate;
	u32 tl, br;

	tl = S_028240_TL_X(state->minx) | S_028240_TL_Y(state->miny) | S_028240_WINDOW_OFFSET_DISABLE(1);
	br = S_028244_BR_X(state->maxx) | S_028244_BR_Y(state->maxy);
	rstate = radeon_state(rscreen->rw, R600_SCISSOR_TYPE, R600_SCISSOR);
	if (rstate == NULL)
		return NULL;
	rstate->states[R600_SCISSOR__PA_SC_SCREEN_SCISSOR_TL] = tl;
	rstate->states[R600_SCISSOR__PA_SC_SCREEN_SCISSOR_BR] = br;
	rstate->states[R600_SCISSOR__PA_SC_WINDOW_OFFSET] = 0x00000000;
	rstate->states[R600_SCISSOR__PA_SC_WINDOW_SCISSOR_TL] = tl;
	rstate->states[R600_SCISSOR__PA_SC_WINDOW_SCISSOR_BR] = br;
	rstate->states[R600_SCISSOR__PA_SC_CLIPRECT_RULE] = 0x0000FFFF;
	rstate->states[R600_SCISSOR__PA_SC_CLIPRECT_0_TL] = tl;
	rstate->states[R600_SCISSOR__PA_SC_CLIPRECT_0_BR] = br;
	rstate->states[R600_SCISSOR__PA_SC_CLIPRECT_1_TL] = tl;
	rstate->states[R600_SCISSOR__PA_SC_CLIPRECT_1_BR] = br;
	rstate->states[R600_SCISSOR__PA_SC_CLIPRECT_2_TL] = tl;
	rstate->states[R600_SCISSOR__PA_SC_CLIPRECT_2_BR] = br;
	rstate->states[R600_SCISSOR__PA_SC_CLIPRECT_3_TL] = tl;
	rstate->states[R600_SCISSOR__PA_SC_CLIPRECT_3_BR] = br;
	rstate->states[R600_SCISSOR__PA_SC_EDGERULE] = 0xAAAAAAAA;
	rstate->states[R600_SCISSOR__PA_SC_GENERIC_SCISSOR_TL] = tl;
	rstate->states[R600_SCISSOR__PA_SC_GENERIC_SCISSOR_BR] = br;
	rstate->states[R600_SCISSOR__PA_SC_VPORT_SCISSOR_0_TL] = tl;
	rstate->states[R600_SCISSOR__PA_SC_VPORT_SCISSOR_0_BR] = br;
	if (radeon_state_pm4(rstate)) {
		radeon_state_decref(rstate);
		return NULL;
	}
	return rstate;
}

static struct radeon_state *r600_viewport(struct r600_context *rctx)
{
	const struct pipe_viewport_state *state = &rctx->viewport->state.viewport;
	struct r600_screen *rscreen = rctx->screen;
	struct radeon_state *rstate;

	rstate = radeon_state(rscreen->rw, R600_VIEWPORT_TYPE, R600_VIEWPORT);
	if (rstate == NULL)
		return NULL;
	rstate->states[R600_VIEWPORT__PA_SC_VPORT_ZMIN_0] = 0x00000000;
	rstate->states[R600_VIEWPORT__PA_SC_VPORT_ZMAX_0] = 0x3F800000;
	rstate->states[R600_VIEWPORT__PA_CL_VPORT_XSCALE_0] = fui(state->scale[0]);
	rstate->states[R600_VIEWPORT__PA_CL_VPORT_YSCALE_0] = fui(state->scale[1]);
	rstate->states[R600_VIEWPORT__PA_CL_VPORT_ZSCALE_0] = fui(state->scale[2]);
	rstate->states[R600_VIEWPORT__PA_CL_VPORT_XOFFSET_0] = fui(state->translate[0]);
	rstate->states[R600_VIEWPORT__PA_CL_VPORT_YOFFSET_0] = fui(state->translate[1]);
	rstate->states[R600_VIEWPORT__PA_CL_VPORT_ZOFFSET_0] = fui(state->translate[2]);
	rstate->states[R600_VIEWPORT__PA_CL_VTE_CNTL] = 0x0000043F;
	if (radeon_state_pm4(rstate)) {
		radeon_state_decref(rstate);
		return NULL;
	}
	return rstate;
}

static struct radeon_state *r600_dsa(struct r600_context *rctx)
{
	const struct pipe_depth_stencil_alpha_state *state = &rctx->dsa->state.dsa;
	struct r600_screen *rscreen = rctx->screen;
	struct radeon_state *rstate;
	unsigned db_depth_control, alpha_test_control, alpha_ref;
	unsigned stencil_ref_mask, stencil_ref_mask_bf;
	
	rstate = radeon_state(rscreen->rw, R600_DSA_TYPE, R600_DSA);
	if (rstate == NULL)
		return NULL;

	stencil_ref_mask = 0;
	stencil_ref_mask_bf = 0;
	db_depth_control = 0x00700700 |
		S_028800_Z_ENABLE(state->depth.enabled) |
		S_028800_Z_WRITE_ENABLE(state->depth.writemask) |
		S_028800_ZFUNC(state->depth.func);
	/* set stencil enable */
	db_depth_control |= S_028800_STENCIL_ENABLE(state->stencil[0].enabled);
	
	if (state->stencil[0].enabled) {

		db_depth_control |= S_028800_STENCILFUNC(r600_translate_ds_func(state->stencil[0].func));
		db_depth_control |= S_028800_STENCILFAIL(r600_translate_stencil_op(state->stencil[0].fail_op));
		db_depth_control |= S_028800_STENCILZPASS(r600_translate_stencil_op(state->stencil[0].zpass_op));
		db_depth_control |= S_028800_STENCILZFAIL(r600_translate_stencil_op(state->stencil[0].zfail_op));
		
		db_depth_control |= S_028800_BACKFACE_ENABLE(state->stencil[1].enabled);

		stencil_ref_mask = (state->stencil[0].valuemask << R600_STENCILMASK_SHIFT) |
			(state->stencil[0].writemask << R600_STENCILWRITEMASK_SHIFT);
		if (state->stencil[1].enabled) {
			db_depth_control |= S_028800_STENCILFUNC_BF(r600_translate_ds_func(state->stencil[1].func));
			db_depth_control |= S_028800_STENCILFAIL_BF(r600_translate_stencil_op(state->stencil[1].fail_op));
			db_depth_control |= S_028800_STENCILZPASS_BF(r600_translate_stencil_op(state->stencil[1].zpass_op));
			db_depth_control |= S_028800_STENCILZFAIL_BF(r600_translate_stencil_op(state->stencil[1].zfail_op));
			stencil_ref_mask_bf = (state->stencil[1].valuemask << R600_STENCILMASK_SHIFT) |
				(state->stencil[1].writemask << R600_STENCILWRITEMASK_SHIFT);
		}
	}

	alpha_test_control = 0;
	alpha_ref = 0;
	if (state->alpha.enabled) {
		alpha_test_control = (state->alpha.func) << 0;
		alpha_test_control |= SX_ALPHA_TEST_ENABLE;
		alpha_ref = fui(state->alpha.ref_value);
	}

	rstate->states[R600_DSA__DB_STENCIL_CLEAR] = 0x00000000;
	rstate->states[R600_DSA__DB_DEPTH_CLEAR] = 0x3F800000;
	rstate->states[R600_DSA__SX_ALPHA_TEST_CONTROL] = alpha_test_control;
	rstate->states[R600_DSA__DB_STENCILREFMASK] = stencil_ref_mask;
	rstate->states[R600_DSA__DB_STENCILREFMASK_BF] = stencil_ref_mask_bf;
	rstate->states[R600_DSA__SX_ALPHA_REF] = alpha_ref;
	rstate->states[R600_DSA__SPI_FOG_FUNC_SCALE] = 0x00000000;
	rstate->states[R600_DSA__SPI_FOG_FUNC_BIAS] = 0x00000000;
	rstate->states[R600_DSA__SPI_FOG_CNTL] = 0x00000000;
	rstate->states[R600_DSA__DB_DEPTH_CONTROL] = db_depth_control;
	rstate->states[R600_DSA__DB_SHADER_CONTROL] = 0x00000210;
	rstate->states[R600_DSA__DB_RENDER_CONTROL] = 0x00000060;
	rstate->states[R600_DSA__DB_RENDER_OVERRIDE] = 0x0000002A;
	rstate->states[R600_DSA__DB_SRESULTS_COMPARE_STATE1] = 0x00000000;
	rstate->states[R600_DSA__DB_PRELOAD_CONTROL] = 0x00000000;
	rstate->states[R600_DSA__DB_ALPHA_TO_MASK] = 0x0000AA00;
	if (radeon_state_pm4(rstate)) {
		radeon_state_decref(rstate);
		return NULL;
	}
	return rstate;
}

static inline unsigned r600_tex_wrap(unsigned wrap)
{
	switch (wrap) {
	default:
	case PIPE_TEX_WRAP_REPEAT:
		return V_03C000_SQ_TEX_WRAP;
	case PIPE_TEX_WRAP_CLAMP:
		return V_03C000_SQ_TEX_CLAMP_LAST_TEXEL;
	case PIPE_TEX_WRAP_CLAMP_TO_EDGE:
		return V_03C000_SQ_TEX_CLAMP_HALF_BORDER;
	case PIPE_TEX_WRAP_CLAMP_TO_BORDER:
		return V_03C000_SQ_TEX_CLAMP_BORDER;
	case PIPE_TEX_WRAP_MIRROR_REPEAT:
		return V_03C000_SQ_TEX_MIRROR;
	case PIPE_TEX_WRAP_MIRROR_CLAMP:
		return V_03C000_SQ_TEX_MIRROR_ONCE_LAST_TEXEL;
	case PIPE_TEX_WRAP_MIRROR_CLAMP_TO_EDGE:
		return V_03C000_SQ_TEX_MIRROR_ONCE_HALF_BORDER;
	case PIPE_TEX_WRAP_MIRROR_CLAMP_TO_BORDER:
		return V_03C000_SQ_TEX_MIRROR_ONCE_BORDER;
	}
}

static inline unsigned r600_tex_filter(unsigned filter)
{
	switch (filter) {
	default:
	case PIPE_TEX_FILTER_NEAREST:
		return V_03C000_SQ_TEX_XY_FILTER_POINT;
	case PIPE_TEX_FILTER_LINEAR:
		return V_03C000_SQ_TEX_XY_FILTER_BILINEAR;
	}
}

static inline unsigned r600_tex_mipfilter(unsigned filter)
{
	switch (filter) {
	case PIPE_TEX_MIPFILTER_NEAREST:
		return V_03C000_SQ_TEX_Z_FILTER_POINT;
	case PIPE_TEX_MIPFILTER_LINEAR:
		return V_03C000_SQ_TEX_Z_FILTER_LINEAR;
	default:
	case PIPE_TEX_MIPFILTER_NONE:
		return V_03C000_SQ_TEX_Z_FILTER_NONE;
	}
}

static inline unsigned r600_tex_compare(unsigned compare)
{
	switch (compare) {
	default:
	case PIPE_FUNC_NEVER:
		return V_03C000_SQ_TEX_DEPTH_COMPARE_NEVER;
	case PIPE_FUNC_LESS:
		return V_03C000_SQ_TEX_DEPTH_COMPARE_LESS;
	case PIPE_FUNC_EQUAL:
		return V_03C000_SQ_TEX_DEPTH_COMPARE_EQUAL;
	case PIPE_FUNC_LEQUAL:
		return V_03C000_SQ_TEX_DEPTH_COMPARE_LESSEQUAL;
	case PIPE_FUNC_GREATER:
		return V_03C000_SQ_TEX_DEPTH_COMPARE_GREATER;
	case PIPE_FUNC_NOTEQUAL:
		return V_03C000_SQ_TEX_DEPTH_COMPARE_NOTEQUAL;
	case PIPE_FUNC_GEQUAL:
		return V_03C000_SQ_TEX_DEPTH_COMPARE_GREATEREQUAL;
	case PIPE_FUNC_ALWAYS:
		return V_03C000_SQ_TEX_DEPTH_COMPARE_ALWAYS;
	}
}

static INLINE u32 S_FIXED(float value, u32 frac_bits)
{
	return value * (1 << frac_bits);
}

static struct radeon_state *r600_sampler(struct r600_context *rctx,
				const struct pipe_sampler_state *state,
				unsigned id)
{
	struct r600_screen *rscreen = rctx->screen;
	struct radeon_state *rstate;

	rstate = radeon_state(rscreen->rw, R600_PS_SAMPLER_TYPE, id);
	if (rstate == NULL)
		return NULL;
	rstate->states[R600_PS_SAMPLER__SQ_TEX_SAMPLER_WORD0_0] =
			S_03C000_CLAMP_X(r600_tex_wrap(state->wrap_s)) |
			S_03C000_CLAMP_Y(r600_tex_wrap(state->wrap_t)) |
			S_03C000_CLAMP_Z(r600_tex_wrap(state->wrap_r)) |
			S_03C000_XY_MAG_FILTER(r600_tex_filter(state->mag_img_filter)) |
			S_03C000_XY_MIN_FILTER(r600_tex_filter(state->min_img_filter)) |
			S_03C000_MIP_FILTER(r600_tex_mipfilter(state->min_mip_filter)) |
			S_03C000_DEPTH_COMPARE_FUNCTION(r600_tex_compare(state->compare_func));
	/* FIXME LOD it depends on texture base level ... */
	rstate->states[R600_PS_SAMPLER__SQ_TEX_SAMPLER_WORD1_0] =
			S_03C004_MIN_LOD(S_FIXED(CLAMP(state->min_lod, 0, 15), 6)) |
			S_03C004_MAX_LOD(S_FIXED(CLAMP(state->max_lod, 0, 15), 6)) |
			S_03C004_LOD_BIAS(S_FIXED(CLAMP(state->lod_bias, -16, 16), 6));
	rstate->states[R600_PS_SAMPLER__SQ_TEX_SAMPLER_WORD2_0] = S_03C008_TYPE(1);
	if (radeon_state_pm4(rstate)) {
		radeon_state_decref(rstate);
		return NULL;
	}
	return rstate;
}

static inline unsigned r600_tex_swizzle(unsigned swizzle)
{
	switch (swizzle) {
	case PIPE_SWIZZLE_RED:
		return V_038010_SQ_SEL_X;
	case PIPE_SWIZZLE_GREEN:
		return V_038010_SQ_SEL_Y;
	case PIPE_SWIZZLE_BLUE:
		return V_038010_SQ_SEL_Z;
	case PIPE_SWIZZLE_ALPHA:
		return V_038010_SQ_SEL_W;
	case PIPE_SWIZZLE_ZERO:
		return V_038010_SQ_SEL_0;
	default:
	case PIPE_SWIZZLE_ONE:
		return V_038010_SQ_SEL_1;
	}
}

static inline unsigned r600_format_type(unsigned format_type)
{
	switch (format_type) {
	default:
	case UTIL_FORMAT_TYPE_UNSIGNED:
		return V_038010_SQ_FORMAT_COMP_UNSIGNED;
	case UTIL_FORMAT_TYPE_SIGNED:
		return V_038010_SQ_FORMAT_COMP_SIGNED;
	case UTIL_FORMAT_TYPE_FIXED:
		return V_038010_SQ_FORMAT_COMP_UNSIGNED_BIASED;
	}
}

static inline unsigned r600_tex_dim(unsigned dim)
{
	switch (dim) {
	default:
	case PIPE_TEXTURE_1D:
		return V_038000_SQ_TEX_DIM_1D;
	case PIPE_TEXTURE_2D:
		return V_038000_SQ_TEX_DIM_2D;
	case PIPE_TEXTURE_3D:
		return V_038000_SQ_TEX_DIM_3D;
	case PIPE_TEXTURE_CUBE:
		return V_038000_SQ_TEX_DIM_CUBEMAP;
	}
}

static struct radeon_state *r600_resource(struct r600_context *rctx,
					const struct pipe_sampler_view *view,
					unsigned id)
{
	struct r600_screen *rscreen = rctx->screen;
	const struct util_format_description *desc;
	struct r600_resource_texture *tmp;
	struct r600_resource *rbuffer;
	struct radeon_state *rstate;
	unsigned format;

	if (r600_conv_pipe_format(view->texture->format, &format))
		return NULL;
	desc = util_format_description(view->texture->format);
	assert(desc == NULL);
	rstate = radeon_state(rscreen->rw, R600_PS_RESOURCE_TYPE, id);
	if (rstate == NULL) {
		return NULL;
	}
	tmp = (struct r600_resource_texture*)view->texture;
	rbuffer = &tmp->resource;
	rstate->bo[0] = radeon_bo_incref(rscreen->rw, rbuffer->bo);
	rstate->bo[1] = radeon_bo_incref(rscreen->rw, rbuffer->bo);
	rstate->nbo = 2;
	rstate->placement[0] = RADEON_GEM_DOMAIN_GTT;
	rstate->placement[1] = RADEON_GEM_DOMAIN_GTT;
	rstate->placement[2] = RADEON_GEM_DOMAIN_GTT;
	rstate->placement[3] = RADEON_GEM_DOMAIN_GTT;

	/* FIXME properly handle first level != 0 */
	rstate->states[R600_PS_RESOURCE__RESOURCE0_WORD0] =
			S_038000_DIM(r600_tex_dim(view->texture->target)) |
			S_038000_PITCH(((tmp->pitch[0] / tmp->bpt) / 8) - 1) |
			S_038000_TEX_WIDTH(view->texture->width0 - 1);
	rstate->states[R600_PS_RESOURCE__RESOURCE0_WORD1] =
			S_038004_TEX_HEIGHT(view->texture->height0 - 1) |
			S_038004_TEX_DEPTH(view->texture->depth0 - 1) |
			S_038004_DATA_FORMAT(format);
	rstate->states[R600_PS_RESOURCE__RESOURCE0_WORD2] = 0;
	rstate->states[R600_PS_RESOURCE__RESOURCE0_WORD3] = tmp->offset[1] >> 8;
	rstate->states[R600_PS_RESOURCE__RESOURCE0_WORD4] =
			S_038010_FORMAT_COMP_X(r600_format_type(UTIL_FORMAT_TYPE_UNSIGNED)) |
			S_038010_FORMAT_COMP_Y(r600_format_type(UTIL_FORMAT_TYPE_UNSIGNED)) |
			S_038010_FORMAT_COMP_Z(r600_format_type(UTIL_FORMAT_TYPE_UNSIGNED)) |
			S_038010_FORMAT_COMP_W(r600_format_type(UTIL_FORMAT_TYPE_UNSIGNED)) |
			S_038010_NUM_FORMAT_ALL(V_038010_SQ_NUM_FORMAT_NORM) |
			S_038010_SRF_MODE_ALL(V_038010_SFR_MODE_NO_ZERO) |
			S_038010_REQUEST_SIZE(1) |
			S_038010_DST_SEL_X(r600_tex_swizzle(view->swizzle_b)) |
			S_038010_DST_SEL_Y(r600_tex_swizzle(view->swizzle_g)) |
			S_038010_DST_SEL_Z(r600_tex_swizzle(view->swizzle_r)) |
			S_038010_DST_SEL_W(r600_tex_swizzle(view->swizzle_a)) |
			S_038010_BASE_LEVEL(view->u.tex.first_level);
	rstate->states[R600_PS_RESOURCE__RESOURCE0_WORD5] =
			S_038014_LAST_LEVEL(view->u.tex.last_level) |
			S_038014_BASE_ARRAY(0) |
			S_038014_LAST_ARRAY(0);
	rstate->states[R600_PS_RESOURCE__RESOURCE0_WORD6] =
			S_038018_TYPE(V_038010_SQ_TEX_VTX_VALID_TEXTURE);
	if (radeon_state_pm4(rstate)) {
		radeon_state_decref(rstate);
		return NULL;
	}
	return rstate;
}

static struct radeon_state *r600_cb_cntl(struct r600_context *rctx)
{
	struct r600_screen *rscreen = rctx->screen;
	struct radeon_state *rstate;
	const struct pipe_blend_state *pbs = &rctx->blend->state.blend;
	uint32_t color_control, target_mask;
	int i;

	target_mask = 0;
	color_control = 0;

	if (pbs->logicop_enable) {
		color_control |= (pbs->logicop_func) << 16;
	} else
		color_control |= (0xcc << 16);

	target_mask |= (pbs->rt[0].colormask);
	for (i = 0; i < 8; i++) {
		if (pbs->rt[i].blend_enable) {
			color_control |= (1 << (8 + i));
			target_mask |= (pbs->rt[0].colormask << (4 * i));
		} else if (i == 0)
			target_mask |= 0xf;
	}
	rstate = radeon_state(rscreen->rw, R600_CB_CNTL_TYPE, R600_CB_CNTL);
	rstate->states[R600_CB_CNTL__CB_SHADER_MASK] = 0x0000000F;
	rstate->states[R600_CB_CNTL__CB_TARGET_MASK] = target_mask;
	rstate->states[R600_CB_CNTL__CB_COLOR_CONTROL] = color_control;
	rstate->states[R600_CB_CNTL__PA_SC_AA_CONFIG] = 0x00000000;
	rstate->states[R600_CB_CNTL__PA_SC_AA_SAMPLE_LOCS_MCTX] = 0x00000000;
	rstate->states[R600_CB_CNTL__PA_SC_AA_SAMPLE_LOCS_8S_WD1_MCTX] = 0x00000000;
	rstate->states[R600_CB_CNTL__CB_CLRCMP_CONTROL] = 0x01000000;
	rstate->states[R600_CB_CNTL__CB_CLRCMP_SRC] = 0x00000000;
	rstate->states[R600_CB_CNTL__CB_CLRCMP_DST] = 0x000000FF;
	rstate->states[R600_CB_CNTL__CB_CLRCMP_MSK] = 0xFFFFFFFF;
	rstate->states[R600_CB_CNTL__PA_SC_AA_MASK] = 0xFFFFFFFF;
	if (radeon_state_pm4(rstate)) {
		radeon_state_decref(rstate);
		return NULL;
	}
	return rstate;
}

int r600_context_hw_states(struct r600_context *rctx)
{
	unsigned i;
	int r;

	/* free previous TODO determine what need to be updated, what
	 * doesn't
	 */
	//radeon_state_decref(rctx->hw_states.config);
	radeon_state_decref(rctx->hw_states.cb_cntl);
	radeon_state_decref(rctx->hw_states.db);
	radeon_state_decref(rctx->hw_states.rasterizer);
	radeon_state_decref(rctx->hw_states.scissor);
	radeon_state_decref(rctx->hw_states.dsa);
	radeon_state_decref(rctx->hw_states.blend);
	radeon_state_decref(rctx->hw_states.viewport);
	radeon_state_decref(rctx->hw_states.cb0);
	for (i = 0; i < rctx->hw_states.ps_nresource; i++) {
		radeon_state_decref(rctx->hw_states.ps_resource[i]);
		rctx->hw_states.ps_resource[i] = NULL;
	}
	rctx->hw_states.ps_nresource = 0;
	for (i = 0; i < rctx->hw_states.ps_nsampler; i++) {
		radeon_state_decref(rctx->hw_states.ps_sampler[i]);
		rctx->hw_states.ps_sampler[i] = NULL;
	}
	rctx->hw_states.ps_nsampler = 0;

	/* build new states */
	rctx->hw_states.rasterizer = r600_rasterizer(rctx);
	rctx->hw_states.scissor = r600_scissor(rctx);
	rctx->hw_states.dsa = r600_dsa(rctx);
	rctx->hw_states.blend = r600_blend(rctx);
	rctx->hw_states.viewport = r600_viewport(rctx);
	rctx->hw_states.cb0 = r600_cb0(rctx);
	rctx->hw_states.db = r600_db(rctx);
	rctx->hw_states.cb_cntl = r600_cb_cntl(rctx);

	for (i = 0; i < rctx->ps_nsampler; i++) {
		if (rctx->ps_sampler[i]) {
			rctx->hw_states.ps_sampler[i] = r600_sampler(rctx,
							&rctx->ps_sampler[i]->state.sampler,
							R600_PS_SAMPLER + i);
		}
	}
	rctx->hw_states.ps_nsampler = rctx->ps_nsampler;
	for (i = 0; i < rctx->ps_nsampler_view; i++) {
		if (rctx->ps_sampler_view[i]) {
			rctx->hw_states.ps_resource[i] = r600_resource(rctx,
							&rctx->ps_sampler_view[i]->state.sampler_view,
							R600_PS_RESOURCE + i);
		}
	}
	rctx->hw_states.ps_nresource = rctx->ps_nsampler_view;

	/* bind states */
	r = radeon_draw_set(rctx->draw, rctx->hw_states.db);
	if (r)
		return r;
	r = radeon_draw_set(rctx->draw, rctx->hw_states.rasterizer);
	if (r)
		return r;
	r = radeon_draw_set(rctx->draw, rctx->hw_states.scissor);
	if (r)
		return r;
	r = radeon_draw_set(rctx->draw, rctx->hw_states.dsa);
	if (r)
		return r;
	r = radeon_draw_set(rctx->draw, rctx->hw_states.blend);
	if (r)
		return r;
	r = radeon_draw_set(rctx->draw, rctx->hw_states.viewport);
	if (r)
		return r;
	r = radeon_draw_set(rctx->draw, rctx->hw_states.cb0);
	if (r)
		return r;
	r = radeon_draw_set(rctx->draw, rctx->hw_states.config);
	if (r)
		return r;
	r = radeon_draw_set(rctx->draw, rctx->hw_states.cb_cntl);
	if (r)
		return r;
	for (i = 0; i < rctx->hw_states.ps_nresource; i++) {
		if (rctx->hw_states.ps_resource[i]) {
			r = radeon_draw_set(rctx->draw, rctx->hw_states.ps_resource[i]);
			if (r)
				return r;
		}
	}
	for (i = 0; i < rctx->hw_states.ps_nsampler; i++) {
		if (rctx->hw_states.ps_sampler[i]) {
			r = radeon_draw_set(rctx->draw, rctx->hw_states.ps_sampler[i]);
			if (r)
				return r;
		}
	}
	return 0;
}
