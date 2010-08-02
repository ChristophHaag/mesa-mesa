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
 *      Corbin Simpson
 */
#include <stdio.h>
#include "util/u_inlines.h"
#include "util/u_format.h"
#include "util/u_memory.h"
#include "r600_screen.h"
#include "r600_context.h"
#include "r600_public.h"
#include "r600_resource.h"

static const char* r600_get_vendor(struct pipe_screen* pscreen)
{
	return "X.Org";
}

static const char* r600_get_name(struct pipe_screen* pscreen)
{
	struct r600_screen *screen = r600_screen(pscreen);
	enum radeon_family family = radeon_get_family(screen->rw);

	if (family >= CHIP_R600 && family < CHIP_RV770)
		return "R600 (HD2XXX,HD3XXX)";
	else
		return "R700 (HD4XXX)";
}

static int r600_get_param(struct pipe_screen* pscreen, enum pipe_cap param)
{
	switch (param) {
	case PIPE_CAP_MAX_TEXTURE_IMAGE_UNITS:
	case PIPE_CAP_MAX_COMBINED_SAMPLERS:
		return 16;
	case PIPE_CAP_NPOT_TEXTURES:
		return 1;
	case PIPE_CAP_TWO_SIDED_STENCIL:
		return 1;
	case PIPE_CAP_GLSL:
		return 1;
	case PIPE_CAP_DUAL_SOURCE_BLEND:
		return 1;
	case PIPE_CAP_ANISOTROPIC_FILTER:
		return 1;
	case PIPE_CAP_POINT_SPRITE:
		return 1;
	case PIPE_CAP_MAX_RENDER_TARGETS:
		/* FIXME some r6xx are buggy and can only do 4 */
		return 8;
	case PIPE_CAP_OCCLUSION_QUERY:
		return 1;
	case PIPE_CAP_TEXTURE_SHADOW_MAP:
		return 1;
	case PIPE_CAP_MAX_TEXTURE_2D_LEVELS:
	case PIPE_CAP_MAX_TEXTURE_3D_LEVELS:
	case PIPE_CAP_MAX_TEXTURE_CUBE_LEVELS:
		/* FIXME not sure here */
		return 13;
	case PIPE_CAP_TEXTURE_MIRROR_CLAMP:
		return 1;
	case PIPE_CAP_TEXTURE_MIRROR_REPEAT:
		return 1;
	case PIPE_CAP_MAX_VERTEX_TEXTURE_UNITS:
		/* FIXME allow this once infrastructure is there */
		return 0;
	case PIPE_CAP_TGSI_CONT_SUPPORTED:
		return 0;
	case PIPE_CAP_BLEND_EQUATION_SEPARATE:
		return 1;
	case PIPE_CAP_SM3:
		return 1;
	case PIPE_CAP_INDEP_BLEND_ENABLE:
		return 1;
	case PIPE_CAP_INDEP_BLEND_FUNC:
		/* FIXME allow this */
		return 0;
	case PIPE_CAP_DEPTHSTENCIL_CLEAR_SEPARATE:
		return 1;
	case PIPE_CAP_TGSI_FS_COORD_ORIGIN_UPPER_LEFT:
	case PIPE_CAP_TGSI_FS_COORD_PIXEL_CENTER_HALF_INTEGER:
		return 1;
	case PIPE_CAP_TGSI_FS_COORD_ORIGIN_LOWER_LEFT:
	case PIPE_CAP_TGSI_FS_COORD_PIXEL_CENTER_INTEGER:
		return 0;
	default:
		debug_printf("r600: unknown param %d\n", param);
		return 0;
	}
}

static float r600_get_paramf(struct pipe_screen* pscreen, enum pipe_cap param)
{
	switch (param) {
	case PIPE_CAP_MAX_LINE_WIDTH:
	case PIPE_CAP_MAX_LINE_WIDTH_AA:
	case PIPE_CAP_MAX_POINT_WIDTH:
	case PIPE_CAP_MAX_POINT_WIDTH_AA:
		return 8192.0f;
	case PIPE_CAP_MAX_TEXTURE_ANISOTROPY:
		return 16.0f;
	case PIPE_CAP_MAX_TEXTURE_LOD_BIAS:
		return 16.0f;
	default:
		debug_printf("r600: unsupported paramf %d\n", param);
		return 0.0f;
	}
}

static boolean r600_is_format_supported(struct pipe_screen* screen,
					enum pipe_format format,
					enum pipe_texture_target target,
					unsigned sample_count,
					unsigned bindings,
					unsigned geom_flags)
{
	if (target >= PIPE_MAX_TEXTURE_TYPES) {
		debug_printf("r600: unsupported texture type %d\n", target);
		return FALSE;
	}
	switch (format) {
	case PIPE_FORMAT_B4G4R4A4_UNORM:
	case PIPE_FORMAT_B5G6R5_UNORM:
	case PIPE_FORMAT_B5G5R5A1_UNORM:
	case PIPE_FORMAT_A8_UNORM:
	case PIPE_FORMAT_L8_UNORM:
	case PIPE_FORMAT_A8R8G8B8_SRGB:
	case PIPE_FORMAT_R8G8B8A8_SRGB:
	case PIPE_FORMAT_DXT1_RGB:
	case PIPE_FORMAT_DXT1_RGBA:
	case PIPE_FORMAT_DXT3_RGBA:
	case PIPE_FORMAT_DXT5_RGBA:
	case PIPE_FORMAT_UYVY:
	case PIPE_FORMAT_L8_SRGB:
	case PIPE_FORMAT_L8A8_SRGB:
	case PIPE_FORMAT_L8A8_UNORM:
	case PIPE_FORMAT_A8R8G8B8_UNORM:
	case PIPE_FORMAT_X8R8G8B8_UNORM:
	case PIPE_FORMAT_R8G8B8A8_UNORM:
	case PIPE_FORMAT_R8G8B8X8_UNORM:
	case PIPE_FORMAT_B8G8R8A8_UNORM:
	case PIPE_FORMAT_B8G8R8X8_UNORM:
	case PIPE_FORMAT_A8B8G8R8_SRGB:
	case PIPE_FORMAT_B8G8R8A8_SRGB:
	case PIPE_FORMAT_I8_UNORM:
	case PIPE_FORMAT_Z16_UNORM:
	case PIPE_FORMAT_X8Z24_UNORM:
	case PIPE_FORMAT_S8_USCALED_Z24_UNORM:
	case PIPE_FORMAT_Z32_UNORM:
	case PIPE_FORMAT_Z24_UNORM_S8_USCALED:
	case PIPE_FORMAT_Z24X8_UNORM:
		return TRUE;
	default:
		/* Unknown format... */
		break;
	}
	return FALSE;
}

static void r600_destroy_screen(struct pipe_screen* pscreen)
{
	struct r600_screen* rscreen = r600_screen(pscreen);

	if (rscreen == NULL)
		return;
	FREE(rscreen);
}

struct pipe_screen *r600_screen_create(struct radeon *rw)
{
	struct r600_screen* rscreen;

	rscreen = CALLOC_STRUCT(r600_screen);
	if (rscreen == NULL) {
		return NULL;
	}
	rscreen->rw = rw;
	rscreen->screen.winsys = (struct pipe_winsys*)rw;
	rscreen->screen.destroy = r600_destroy_screen;
	rscreen->screen.get_name = r600_get_name;
	rscreen->screen.get_vendor = r600_get_vendor;
	rscreen->screen.get_param = r600_get_param;
	rscreen->screen.get_paramf = r600_get_paramf;
	rscreen->screen.is_format_supported = r600_is_format_supported;
	rscreen->screen.context_create = r600_create_context;
	r600_init_screen_resource_functions(rscreen);
	return &rscreen->screen;
}
