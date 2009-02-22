/**************************************************************************
 *
 * Copyright 2006, 2008 Tungsten Graphics, Inc., Cedar Park, Texas.
 * All Rights Reserved.
 * Copyright (c) 2009 VMware, Inc., Palo Alto, CA., USA.
 * All Rights Reserved.
 *
 * Permission is hereby granted, free of charge, to any person obtaining a
 * copy of this software and associated documentation files (the
 * "Software"), to deal in the Software without restriction, including
 * without limitation the rights to use, copy, modify, merge, publish,
 * distribute, sub license, and/or sell copies of the Software, and to
 * permit persons to whom the Software is furnished to do so, subject to
 * the following conditions:
 *
 * The above copyright notice and this permission notice (including the
 * next paragraph) shall be included in all copies or substantial portions
 * of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS
 * OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF
 * MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND NON-INFRINGEMENT.
 * IN NO EVENT SHALL TUNGSTEN GRAPHICS AND/OR ITS SUPPLIERS BE LIABLE FOR
 * ANY CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT,
 * TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN CONNECTION WITH THE
 * SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.
 *
 **************************************************************************/

#include "main/glheader.h"
#include "main/imports.h"
#include "main/context.h"
#include "main/depthstencil.h"
#include "main/fbobject.h"
#include "main/framebuffer.h"
#include "main/hash.h"
#include "main/mtypes.h"
#include "main/renderbuffer.h"

#include "via_context.h"
#include "via_fbo.h"
#include "via_depthstencil.h"
#include "wsbm_manager.h"

/**
 * The GL_EXT_framebuffer_object allows the user to create their own
 * framebuffer objects consisting of color renderbuffers (0 or more),
 * depth renderbuffers (0 or 1) and stencil renderbuffers (0 or 1).
 *
 * The spec considers depth and stencil renderbuffers to be totally independent
 * buffers.  In reality, most graphics hardware today uses a combined
 * depth+stencil buffer (one 32-bit pixel = 24 bits of Z + 8 bits of stencil).
 *
 * This causes difficulty because the user may create some number of depth
 * renderbuffers and some number of stencil renderbuffers and bind them
 * together in framebuffers in any combination.
 *
 * This code manages all that.
 *
 * 1. Depth renderbuffers are always allocated in hardware as 32bpp
 *    GL_DEPTH24_STENCIL8 buffers.
 *
 * 2. Stencil renderbuffers are initially allocated in software as 8bpp
 *    GL_STENCIL_INDEX8 buffers.
 *
 * 3. Depth and Stencil renderbuffers use the PairedStencil and PairedDepth
 *    fields (respectively) to indicate if the buffer's currently paired
 *    with another stencil or depth buffer (respectively).
 *
 * 4. When a depth and stencil buffer are initially both attached to the
 *    current framebuffer, we merge the stencil buffer values into the
 *    depth buffer (really a depth+stencil buffer).  The then hardware uses
 *    the combined buffer.
 *
 * 5. Whenever a depth or stencil buffer is reallocated (with
 *    glRenderbufferStorage) we undo the pairing and copy the stencil values
 *    from the combined depth/stencil buffer back to the stencil-only buffer.
 *
 * 6. We also undo the pairing when we find a change in buffer bindings.
 *
 * 7. If a framebuffer is only using a depth renderbuffer (no stencil), we
 *    just use the combined depth/stencil buffer and ignore the stencil values.
 *
 * 8. If a framebuffer is only using a stencil renderbuffer (no depth) we have
 *    to promote the 8bpp software stencil buffer to a 32bpp hardware
 *    depth+stencil buffer.
 *
 */

static int
map_buffers(GLcontext * ctx,
	    struct via_renderbuffer *depthRb,
	    struct via_renderbuffer *stencilRb)
{
    int ret;

    if (depthRb && depthRb->buf) {
	depthRb->map = wsbmBOMap(depthRb->buf,
				 WSBM_ACCESS_READ | WSBM_ACCESS_WRITE);
	if (!depthRb->map)
	    return -ENOMEM;
	ret = wsbmBOSyncForCpu(depthRb->buf,
			       WSBM_SYNCCPU_READ | WSBM_SYNCCPU_WRITE);
	if (ret)
	    goto out_err0;
    }

    if (stencilRb && stencilRb->buf) {
	stencilRb->map = wsbmBOMap(stencilRb->buf, WSBM_ACCESS_READ |
				   WSBM_ACCESS_WRITE);
	if (!stencilRb->map) {
	    ret = -ENOMEM;
	    goto out_err1;
	}

	ret = wsbmBOSyncForCpu(stencilRb->buf,
			       WSBM_SYNCCPU_READ | WSBM_SYNCCPU_WRITE);
	if (ret)
	    goto out_err2;
    }

    return 0;

  out_err2:
    (void)wsbmBOUnmap(stencilRb->buf);
  out_err1:
    (void)wsbmBOReleaseFromCpu(depthRb->buf,
			       WSBM_SYNCCPU_READ | WSBM_SYNCCPU_WRITE);
  out_err0:
    (void)wsbmBOUnmap(depthRb->buf);
    return ret;
}

static void
unmap_buffers(GLcontext * ctx,
	      struct via_renderbuffer *depthRb,
	      struct via_renderbuffer *stencilRb)
{
    if (depthRb && depthRb->buf) {
	(void)wsbmBOReleaseFromCpu(depthRb->buf,
				   WSBM_SYNCCPU_READ | WSBM_SYNCCPU_WRITE);
	wsbmBOUnmap(depthRb->buf);
	depthRb->map = NULL;
    }

    if (stencilRb && stencilRb->buf) {
	(void)wsbmBOReleaseFromCpu(stencilRb->buf,
				   WSBM_SYNCCPU_READ | WSBM_SYNCCPU_WRITE);
	wsbmBOUnmap(stencilRb->buf);
	stencilRb->map = NULL;
    }
}

static int via_extract_stencil(GLcontext * ctx,
			       struct gl_renderbuffer *combinedRb,
			       struct gl_renderbuffer *stencilRb)
{
    struct via_context *vmesa = VIA_CONTEXT(ctx);
    struct via_renderbuffer *viaStencilRb = via_renderbuffer(stencilRb);
    struct via_renderbuffer *viaCombinedRb = via_renderbuffer(combinedRb);
    int ret;

    if (!viaStencilRb || !viaCombinedRb)
	goto out_sw;

    VIA_FLUSH_DMA(vmesa);
    viaBlit(vmesa, 32, viaCombinedRb->buf, viaStencilRb->buf, 0, 0,
	    viaCombinedRb->pitch, viaStencilRb->pitch, 1, 1,
	    combinedRb->Width, combinedRb->Height, VIA_BLIT_COPY,
	    0, 0xe << 28);

    via_execbuf(vmesa, VIA_NO_CLIPRECTS);
    return 0;
  out_sw:
    ret = map_buffers(ctx, viaCombinedRb, viaStencilRb);
    if (!ret) {
	_mesa_extract_stencil(ctx, combinedRb, stencilRb);
	unmap_buffers(ctx, viaCombinedRb, viaStencilRb);
    }
    return ret;
}

static int via_insert_stencil(GLcontext * ctx,
			       struct gl_renderbuffer *combinedRb,
			       struct gl_renderbuffer *stencilRb)
{
    struct via_context *vmesa = VIA_CONTEXT(ctx);
    struct via_renderbuffer *viaStencilRb = via_renderbuffer(stencilRb);
    struct via_renderbuffer *viaCombinedRb = via_renderbuffer(combinedRb);
    int ret;

    if (!viaStencilRb || !viaCombinedRb)
	goto out_sw;

    VIA_FLUSH_DMA(vmesa);
    viaBlit(vmesa, 32, viaStencilRb->buf, viaCombinedRb->buf, 0, 0,
	    viaStencilRb->pitch, viaCombinedRb->pitch, 1, 1,
	    combinedRb->Width, combinedRb->Height, VIA_BLIT_COPY,
	    0, 0xe << 28);
	
    via_execbuf(vmesa, VIA_NO_CLIPRECTS);
    return 0;
  out_sw:
    ret = map_buffers(ctx, viaCombinedRb, viaStencilRb);
    if (!ret) {
	_mesa_insert_stencil(ctx, combinedRb, stencilRb);
	unmap_buffers(ctx, viaCombinedRb, viaStencilRb);
    }
    return ret;
}
	

/**
 * Undo the pairing/interleaving between depth and stencil buffers.
 * viarb should be a depth/stencil or stencil renderbuffer.
 */
void
via_unpair_depth_stencil(GLcontext * ctx, struct via_renderbuffer *viarb)
{
    if (viarb->PairedStencil) {
	/* viarb is a depth/stencil buffer */
	struct gl_renderbuffer *stencilRb;
	struct via_renderbuffer *stencilViarb;

	ASSERT(viarb->Base._ActualFormat == GL_DEPTH24_STENCIL8_EXT);

	stencilRb = _mesa_lookup_renderbuffer(ctx, viarb->PairedStencil);
	stencilViarb = via_renderbuffer(stencilRb);
	if (stencilViarb) {
	    /* need to extract stencil values from the depth buffer */
	    ASSERT(stencilViarb->PairedDepth == viarb->Base.Name);
	    via_extract_stencil(ctx, &viarb->Base, &stencilViarb->Base);
	    stencilViarb->PairedDepth = 0;
	}
	viarb->PairedStencil = 0;
    } else if (viarb->PairedDepth) {
	/* viarb is a stencil buffer */
	struct gl_renderbuffer *depthRb;
	struct via_renderbuffer *depthViarb;

	ASSERT(viarb->Base._ActualFormat == GL_STENCIL_INDEX8_EXT ||
	       viarb->Base._ActualFormat == GL_DEPTH24_STENCIL8_EXT);

	depthRb = _mesa_lookup_renderbuffer(ctx, viarb->PairedDepth);
	depthViarb = via_renderbuffer(depthRb);
	if (depthViarb) {
	    /* need to extract stencil values from the depth buffer */
	    ASSERT(depthViarb->PairedStencil == viarb->Base.Name);
	    via_extract_stencil(ctx, &depthViarb->Base, &viarb->Base);
	    depthViarb->PairedStencil = 0;
	}
	viarb->PairedDepth = 0;
    } else {
	_mesa_problem(ctx, "Problem in undo_depth_stencil_pairing");
    }

    ASSERT(viarb->PairedStencil == 0);
    ASSERT(viarb->PairedDepth == 0);
}

/**
 * Examine the depth and stencil renderbuffers which are attached to the
 * framebuffer.  If both depth and stencil are attached, make sure that the
 * renderbuffers are 'paired' (combined).  If only depth or only stencil is
 * attached, undo any previous pairing.
 *
 * Must be called if NewState & _NEW_BUFFER (when renderbuffer attachments
 * change, for example).
 */
void
via_validate_paired_depth_stencil(GLcontext * ctx, struct gl_framebuffer *fb)
{
    struct via_renderbuffer *depthRb, *stencilRb;

    depthRb = via_get_renderbuffer(fb, BUFFER_DEPTH);
    stencilRb = via_get_renderbuffer(fb, BUFFER_STENCIL);

    if (fb->Name == 0)
	return;

    if (depthRb && stencilRb) {
	if (depthRb == stencilRb) {
	    /* Using a user-created combined depth/stencil buffer.
	     * Nothing to do.
	     */
	    ASSERT(depthRb->Base._BaseFormat == GL_DEPTH_STENCIL_EXT);
	    ASSERT(depthRb->Base._ActualFormat == GL_DEPTH24_STENCIL8_EXT);
	} else {
	    /* Separate depth/stencil buffers, need to interleave now */
	    ASSERT(depthRb->Base._BaseFormat == GL_DEPTH_COMPONENT);
	    ASSERT(stencilRb->Base._BaseFormat == GL_STENCIL_INDEX);
	    /* may need to interleave depth/stencil now */
	    if (depthRb->PairedStencil == stencilRb->Base.Name) {
		/* OK, the depth and stencil buffers are already interleaved */
		ASSERT(stencilRb->PairedDepth == depthRb->Base.Name);
	    } else {
		/* need to setup new pairing/interleaving */
		if (depthRb->PairedStencil) {
		    via_unpair_depth_stencil(ctx, depthRb);
		}
		if (stencilRb->PairedDepth) {
		    via_unpair_depth_stencil(ctx, stencilRb);
		}

		ASSERT(depthRb->Base._ActualFormat ==
		       GL_DEPTH24_STENCIL8_EXT);
		ASSERT(stencilRb->Base._ActualFormat == GL_STENCIL_INDEX8_EXT
		       || stencilRb->Base._ActualFormat ==
		       GL_DEPTH24_STENCIL8_EXT);

		/* establish new pairing: interleave stencil into depth buffer */
		via_insert_stencil(ctx, &depthRb->Base, &stencilRb->Base);
		depthRb->PairedStencil = stencilRb->Base.Name;
		stencilRb->PairedDepth = depthRb->Base.Name;
	    }

	}
    } else if (depthRb) {
	/* Depth buffer but no stencil buffer.
	 * We'll use a GL_DEPTH24_STENCIL8 buffer and ignore the stencil bits.
	 */
	/* can't assert this until storage is allocated:
	 * ASSERT(depthRb->Base._ActualFormat == GL_DEPTH24_STENCIL8_EXT);
	 */
	/* via_undo any previous pairing */
	if (depthRb->PairedStencil) {
	    via_unpair_depth_stencil(ctx, depthRb);
	}
    } else if (stencilRb) {
	/* Stencil buffer but no depth buffer.
	 * Since h/w doesn't typically support just 8bpp stencil w/out Z,
	 * we'll use a GL_DEPTH24_STENCIL8 buffer and ignore the depth bits.
	 */
	/* undo any previous pairing */
	if (stencilRb->PairedDepth) {
	    via_unpair_depth_stencil(ctx, stencilRb);
	}
    }

    /* Finally, update the fb->_DepthBuffer and fb->_StencilBuffer fields */
    _mesa_update_depth_buffer(ctx, fb, BUFFER_DEPTH);
    if (depthRb && depthRb->PairedStencil)
	_mesa_update_stencil_buffer(ctx, fb, BUFFER_DEPTH);
    else
	_mesa_update_stencil_buffer(ctx, fb, BUFFER_STENCIL);

    /* The hardware should use fb->Attachment[BUFFER_DEPTH].Renderbuffer
     * first, if present, then fb->Attachment[BUFFER_STENCIL].Renderbuffer
     * if present.
     */
}
