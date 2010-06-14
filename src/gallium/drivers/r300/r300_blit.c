/*
 * Copyright 2009 Marek Olšák <maraeo@gmail.com>
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
 * USE OR OTHER DEALINGS IN THE SOFTWARE. */

#include "r300_context.h"
#include "r300_texture.h"

#include "util/u_format.h"

enum r300_blitter_op
{
    R300_CLEAR,
    R300_CLEAR_SURFACE,
    R300_COPY
};

static void r300_blitter_begin(struct r300_context* r300, enum r300_blitter_op op)
{
    if (r300->query_current) {
        r300->blitter_saved_query = r300->query_current;
        r300_stop_query(r300);
    }

    /* Yeah we have to save all those states to ensure the blitter operation
     * is really transparent. The states will be restored by the blitter once
     * copying is done. */
    util_blitter_save_blend(r300->blitter, r300->blend_state.state);
    util_blitter_save_depth_stencil_alpha(r300->blitter, r300->dsa_state.state);
    util_blitter_save_stencil_ref(r300->blitter, &(r300->stencil_ref));
    util_blitter_save_rasterizer(r300->blitter, r300->rs_state.state);
    util_blitter_save_fragment_shader(r300->blitter, r300->fs.state);
    util_blitter_save_vertex_shader(r300->blitter, r300->vs_state.state);
    util_blitter_save_viewport(r300->blitter, &r300->viewport);
    util_blitter_save_clip(r300->blitter, (struct pipe_clip_state*)r300->clip_state.state);
    util_blitter_save_vertex_elements(r300->blitter, r300->velems);
    util_blitter_save_vertex_buffers(r300->blitter, r300->vertex_buffer_count,
                                     r300->vertex_buffer);

    if (op & (R300_CLEAR_SURFACE | R300_COPY))
        util_blitter_save_framebuffer(r300->blitter, r300->fb_state.state);

    if (op & R300_COPY) {
        struct r300_textures_state* state =
            (struct r300_textures_state*)r300->textures_state.state;

        util_blitter_save_fragment_sampler_states(
            r300->blitter, state->sampler_state_count,
            (void**)state->sampler_states);

        util_blitter_save_fragment_sampler_views(
            r300->blitter, state->sampler_view_count,
            (struct pipe_sampler_view**)state->sampler_views);
    }
}

static void r300_blitter_end(struct r300_context *r300)
{
    if (r300->blitter_saved_query) {
        r300_resume_query(r300, r300->blitter_saved_query);
        r300->blitter_saved_query = NULL;
    }
}

/* Clear currently bound buffers. */
static void r300_clear(struct pipe_context* pipe,
                       unsigned buffers,
                       const float* rgba,
                       double depth,
                       unsigned stencil)
{
    /* XXX Implement fastfill.
     *
     * If fastfill is enabled, a few facts should be considered:
     *
     * 1) Zbuffer must be micro-tiled and whole microtiles must be
     *    written.
     *
     * 2) ZB_DEPTHCLEARVALUE is used to clear a zbuffer and Z Mask must be
     *    equal to 0.
     *
     * 3) For 16-bit integer buffering, compression causes a hung with one or
     *    two samples and should not be used.
     *
     * 4) Fastfill must not be used if reading of compressed Z data is disabled
     *    and writing of compressed Z data is enabled (RD/WR_COMP_ENABLE),
     *    i.e. it cannot be used to compress the zbuffer.
     *    (what the hell does that mean and how does it fit in clearing
     *    the buffers?)
     *
     * - Marek
     */

    struct r300_context* r300 = r300_context(pipe);
    struct pipe_framebuffer_state* fb =
        (struct pipe_framebuffer_state*)r300->fb_state.state;

    r300_blitter_begin(r300, R300_CLEAR);
    util_blitter_clear(r300->blitter,
                       fb->width,
                       fb->height,
                       fb->nr_cbufs,
                       buffers, rgba, depth, stencil);
    r300_blitter_end(r300);
}

/* Clear a region of a color surface to a constant value. */
static void r300_clear_render_target(struct pipe_context *pipe,
                                     struct pipe_surface *dst,
                                     const float *rgba,
                                     unsigned dstx, unsigned dsty,
                                     unsigned width, unsigned height)
{
    struct r300_context *r300 = r300_context(pipe);

    r300_blitter_begin(r300, R300_CLEAR_SURFACE);
    util_blitter_clear_render_target(r300->blitter, dst, rgba,
                                     dstx, dsty, width, height);
    r300_blitter_end(r300);
}

/* Clear a region of a depth stencil surface. */
static void r300_clear_depth_stencil(struct pipe_context *pipe,
                                     struct pipe_surface *dst,
                                     unsigned clear_flags,
                                     double depth,
                                     unsigned stencil,
                                     unsigned dstx, unsigned dsty,
                                     unsigned width, unsigned height)
{
    struct r300_context *r300 = r300_context(pipe);

    r300_blitter_begin(r300, R300_CLEAR_SURFACE);
    util_blitter_clear_depth_stencil(r300->blitter, dst, clear_flags, depth, stencil,
                                     dstx, dsty, width, height);
    r300_blitter_end(r300);
}

/* Copy a block of pixels from one surface to another using HW. */
static void r300_hw_copy_region(struct pipe_context* pipe,
                                struct pipe_resource *dst,
                                unsigned dst_level,
                                unsigned dstx, unsigned dsty, unsigned dstz,
                                struct pipe_resource *src,
                                unsigned src_level,
                                const struct pipe_box *src_box)
{
    struct r300_context* r300 = r300_context(pipe);

    r300_blitter_begin(r300, R300_COPY);

    /* Do a copy */
    util_blitter_copy_region(r300->blitter, dst, dst_level, dstx, dsty, dstz,
                             src, src_level, src_box, TRUE);
    r300_blitter_end(r300);
}

/* Copy a block of pixels from one surface to another. */
static void r300_resource_copy_region(struct pipe_context *pipe,
                                      struct pipe_resource *dst,
                                      unsigned dst_level,
                                      unsigned dstx, unsigned dsty, unsigned dstz,
                                      struct pipe_resource *src,
                                      unsigned src_level,
                                      const struct pipe_box *src_box)
{
    enum pipe_format old_format = dst->format;
    enum pipe_format new_format = old_format;

    if (dst->format != src->format) {
        debug_printf("r300: Implementation error: Format mismatch in %s\n"
            "    : src: %s dst: %s\n", __FUNCTION__,
            util_format_short_name(src->format),
            util_format_short_name(dst->format));
        debug_assert(0);
    }

    if (!pipe->screen->is_format_supported(pipe->screen,
                                           old_format, src->target,
                                           src->nr_samples,
                                           PIPE_BIND_RENDER_TARGET |
                                           PIPE_BIND_SAMPLER_VIEW, 0) &&
        util_format_is_plain(old_format)) {
        switch (util_format_get_blocksize(old_format)) {
            case 1:
                new_format = PIPE_FORMAT_I8_UNORM;
                break;
            case 2:
                new_format = PIPE_FORMAT_B4G4R4A4_UNORM;
                break;
            case 4:
                new_format = PIPE_FORMAT_B8G8R8A8_UNORM;
                break;
            case 8:
                new_format = PIPE_FORMAT_R16G16B16A16_UNORM;
                break;
            default:
                debug_printf("r300: surface_copy: Unhandled format: %s. Falling back to software.\n"
                             "r300: surface_copy: Software fallback doesn't work for tiled textures.\n",
                             util_format_short_name(old_format));
        }
    }

    if (old_format != new_format) {
        dst->format = new_format;
        src->format = new_format;

        r300_texture_reinterpret_format(pipe->screen,
                                        dst, new_format);
        r300_texture_reinterpret_format(pipe->screen,
                                        src, new_format);
    }

    r300_hw_copy_region(pipe, dst, dst_level, dstx, dsty, dstz,
                        src, src_level, src_box);

    if (old_format != new_format) {
        dst->format = old_format;
        src->format = old_format;

        r300_texture_reinterpret_format(pipe->screen,
                                        dst, old_format);
        r300_texture_reinterpret_format(pipe->screen,
                                        src, old_format);
    }
}

void r300_init_blit_functions(struct r300_context *r300)
{
    r300->context.clear = r300_clear;
    r300->context.clear_render_target = r300_clear_render_target;
    r300->context.clear_depth_stencil = r300_clear_depth_stencil;
    r300->context.resource_copy_region = r300_resource_copy_region;
}
