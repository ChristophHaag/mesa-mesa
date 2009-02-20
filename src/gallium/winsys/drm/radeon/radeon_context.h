/* 
 * Copyright © 2008 Jérôme Glisse
 * All Rights Reserved.
 * 
 * Permission is hereby granted, free of charge, to any person obtaining
 * a copy of this software and associated documentation files (the
 * "Software"), to deal in the Software without restriction, including
 * without limitation the rights to use, copy, modify, merge, publish,
 * distribute, sub license, and/or sell copies of the Software, and to
 * permit persons to whom the Software is furnished to do so, subject to
 * the following conditions:
 * 
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND,
 * EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES
 * OF MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND
 * NON-INFRINGEMENT. IN NO EVENT SHALL THE COPYRIGHT HOLDERS, AUTHORS
 * AND/OR ITS SUPPLIERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE,
 * ARISING FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE 
 * USE OR OTHER DEALINGS IN THE SOFTWARE.
 *
 * The above copyright notice and this permission notice (including the
 * next paragraph) shall be included in all copies or substantial portions
 * of the Software.
 */
/*
 * Authors:
 *      Jérôme Glisse <glisse@freedesktop.org>
 */
#ifndef RADEON_CONTEXT_H
#define RADEON_CONTEXT_H

#include "dri_util.h"
#include "state_tracker/st_public.h"
#include "state_tracker/st_context.h"
#include "radeon_screen.h"

#include "radeon_r300.h"

struct radeon_framebuffer {
    struct st_framebuffer   *st_framebuffer;
    unsigned                attachments;
};

struct radeon_context {
    /* st */
    struct st_context       *st_context;
    /* pipe */
    struct pipe_screen      *pipe_screen;
    struct pipe_winsys      *pipe_winsys;
    /* DRI */
    __DRIscreenPrivate      *dri_screen;
    __DRIdrawablePrivate    *dri_drawable;
    __DRIdrawablePrivate    *dri_readable;
    /* DRM */
    int                     drm_fd;
   /* RADEON */
    struct radeon_screen       *radeon_screen;
};

GLboolean radeon_context_create(const __GLcontextModes*,
                             __DRIcontextPrivate*,
                             void*);
void radeon_context_destroy(__DRIcontextPrivate*);
GLboolean radeon_context_bind(__DRIcontextPrivate*,
                           __DRIdrawablePrivate*,
                           __DRIdrawablePrivate*);
GLboolean radeon_context_unbind(__DRIcontextPrivate*);

#endif
