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
 */
#ifndef R600_TEXTURE_H
#define R600_TEXTURE_H

#include <pipe/p_state.h>

struct r600_texture {
	struct u_resource		b;
	unsigned long			offset[PIPE_MAX_TEXTURE_LEVELS];
	unsigned long			pitch[PIPE_MAX_TEXTURE_LEVELS];
	unsigned long			stride[PIPE_MAX_TEXTURE_LEVELS];
	unsigned long			layer_size[PIPE_MAX_TEXTURE_LEVELS];
	unsigned long			stride_override;
	unsigned long			size;
	struct pipe_resource		*buffer;
};

struct pipe_resource *r600_texture_create(struct pipe_screen *screen,
					  const struct pipe_resource *templ);
unsigned long r600_texture_get_offset(struct r600_texture *rtex, unsigned level, unsigned layer);
struct pipe_resource *r600_texture_from_handle(struct pipe_screen *screen,
					       const struct pipe_resource *base,
					       struct winsys_handle *whandle);

/* This should be implemented by winsys. */
boolean r600_buffer_get_handle(struct radeon *rw,
			       struct pipe_resource *buf,
			       struct winsys_handle *whandle);


#endif
