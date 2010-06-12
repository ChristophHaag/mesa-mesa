/**************************************************************************
 *
 * Copyright 2009 VMware, Inc.  All Rights Reserved.
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
 * IN NO EVENT SHALL VMWARE AND/OR ITS SUPPLIERS BE LIABLE FOR
 * ANY CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT,
 * TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN CONNECTION WITH THE
 * SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.
 *
 **************************************************************************/

/**
 * @file
 * Surface utility functions.
 *  
 * @author Brian Paul
 */


#include "pipe/p_defines.h"
#include "pipe/p_screen.h"
#include "pipe/p_state.h"

#include "util/u_format.h"
#include "util/u_inlines.h"
#include "util/u_rect.h"
#include "util/u_surface.h"
#include "util/u_pack_color.h"


/**
 * Helper to quickly create an RGBA rendering surface of a certain size.
 * \param textureOut  returns the new texture
 * \param surfaceOut  returns the new surface
 * \return TRUE for success, FALSE if failure
 */
boolean
util_create_rgba_surface(struct pipe_context *pipe,
                         uint width, uint height,
                         uint bind,
                         struct pipe_resource **textureOut,
                         struct pipe_surface **surfaceOut)
{
   static const enum pipe_format rgbaFormats[] = {
      PIPE_FORMAT_B8G8R8A8_UNORM,
      PIPE_FORMAT_A8R8G8B8_UNORM,
      PIPE_FORMAT_A8B8G8R8_UNORM,
      PIPE_FORMAT_NONE
   };
   const uint target = PIPE_TEXTURE_2D;
   enum pipe_format format = PIPE_FORMAT_NONE;
   struct pipe_resource templ;
   struct pipe_screen *screen = pipe->screen;
   uint i;

   /* Choose surface format */
   for (i = 0; rgbaFormats[i]; i++) {
      if (screen->is_format_supported(screen, rgbaFormats[i],
                                      target, 0, bind, 0)) {
         format = rgbaFormats[i];
         break;
      }
   }
   if (format == PIPE_FORMAT_NONE)
      return FALSE;  /* unable to get an rgba format!?! */

   /* create texture */
   memset(&templ, 0, sizeof(templ));
   templ.target = target;
   templ.format = format;
   templ.last_level = 0;
   templ.width0 = width;
   templ.height0 = height;
   templ.depth0 = 1;
   templ.bind = bind;

   *textureOut = screen->resource_create(screen, &templ);
   if (!*textureOut)
      return FALSE;

   /* create surface / view into texture */
   *surfaceOut = pipe->create_surface(pipe,
                                      *textureOut,
                                      0, 0, 0,
                                      bind);
   if (!*surfaceOut) {
      pipe_resource_reference(textureOut, NULL);
      return FALSE;
   }

   return TRUE;
}


/**
 * Release the surface and texture from util_create_rgba_surface().
 */
void
util_destroy_rgba_surface(struct pipe_resource *texture,
                          struct pipe_surface *surface)
{
   pipe_surface_reference(&surface, NULL);
   pipe_resource_reference(&texture, NULL);
}



/**
 * Fallback function for pipe->resource_copy_region().
 * Note: (X,Y)=(0,0) is always the upper-left corner.
 */
void
util_resource_copy_region(struct pipe_context *pipe,
                          struct pipe_resource *dst,
                          unsigned dst_level,
                          unsigned dst_x, unsigned dst_y, unsigned dst_z,
                          struct pipe_resource *src,
                          unsigned src_level,
                          const struct pipe_box *src_box)
{
   struct pipe_transfer *src_trans, *dst_trans;
   void *dst_map;
   const void *src_map;
   enum pipe_format src_format, dst_format;
   unsigned w = src_box->width;
   unsigned h = src_box->height;

   assert(src && dst);
   if (!src || !dst)
      return;

   src_format = src->format;
   dst_format = dst->format;

   src_trans = pipe_get_transfer(pipe,
                                 src,
                                 src_level,
                                 src_box->z,
                                 PIPE_TRANSFER_READ,
                                 src_box->x, src_box->y, w, h);

   dst_trans = pipe_get_transfer(pipe,
                                 dst,
                                 dst_level,
                                 dst_z,
                                 PIPE_TRANSFER_WRITE,
                                 dst_x, dst_y, w, h);

   assert(util_format_get_blocksize(dst_format) == util_format_get_blocksize(src_format));
   assert(util_format_get_blockwidth(dst_format) == util_format_get_blockwidth(src_format));
   assert(util_format_get_blockheight(dst_format) == util_format_get_blockheight(src_format));

   src_map = pipe->transfer_map(pipe, src_trans);
   dst_map = pipe->transfer_map(pipe, dst_trans);

   assert(src_map);
   assert(dst_map);

   if (src_map && dst_map) {
      util_copy_rect(dst_map,
                     dst_format,
                     dst_trans->stride,
                     0, 0,
                     w, h,
                     src_map,
                     src_trans->stride,
                     0,
                     0);
   }

   pipe->transfer_unmap(pipe, src_trans);
   pipe->transfer_unmap(pipe, dst_trans);

   pipe->transfer_destroy(pipe, src_trans);
   pipe->transfer_destroy(pipe, dst_trans);
}



#define UBYTE_TO_USHORT(B) ((B) | ((B) << 8))


/**
 * Fallback for pipe->clear_render_target() function.
 * XXX this looks too hackish to be really useful.
 * cpp > 4 looks like a gross hack at best...
 * and we're missing the equivalent clear_depth_stencil fallback.
 * Plus can't use these transfer fallbacks when clearing
 * multisampled surfaces for instance.
 */
void
util_clear_render_target(struct pipe_context *pipe,
                         struct pipe_surface *dst,
                         const float *rgba,
                         unsigned dstx, unsigned dsty,
                         unsigned width, unsigned height)
{
   struct pipe_transfer *dst_trans;
   void *dst_map;
   union util_color uc;

   assert(dst->texture);
   if (!dst->texture)
      return;
   util_pack_color(rgba, dst->texture->format, &uc);
   /* XXX: should handle multiple layers */
   dst_trans = pipe_get_transfer(pipe,
                                 dst->texture,
                                 dst->level,
                                 dst->first_layer,
                                 PIPE_TRANSFER_WRITE,
                                 dstx, dsty, width, height);

   dst_map = pipe->transfer_map(pipe, dst_trans);

   assert(dst_map);

   if (dst_map) {
      assert(dst_trans->stride > 0);

      switch (util_format_get_blocksize(dst->texture->format)) {
      case 1:
      case 2:
      case 4:
         util_pack_color(rgba, dst->texture->format, &uc);
         util_fill_rect(dst_map, dst->texture->format,
                        dst_trans->stride,
                        0, 0, width, height, uc.ui);
         break;
      case 8:
      {
	 /* expand the 4-byte clear value to an 8-byte value */
	 /* should probably not convert back from ubyte but not
	    sure what this code really achieved since it doesn't even
	    check for format type... */
	 ushort *row = (ushort *) dst_map;
	 ushort val0 = UBYTE_TO_USHORT((uc.ui >>  0) & 0xff);
	 ushort val1 = UBYTE_TO_USHORT((uc.ui >>  8) & 0xff);
	 ushort val2 = UBYTE_TO_USHORT((uc.ui >> 16) & 0xff);
	 ushort val3 = UBYTE_TO_USHORT((uc.ui >> 24) & 0xff);
	 unsigned i, j;
	 val0 = (val0 << 8) | val0;
	 val1 = (val1 << 8) | val1;
	 val2 = (val2 << 8) | val2;
	 val3 = (val3 << 8) | val3;
	 for (i = 0; i < height; i++) {
	    for (j = 0; j < width; j++) {
	       row[j*4+0] = val0;
	       row[j*4+1] = val1;
	       row[j*4+2] = val2;
	       row[j*4+3] = val3;
	    }
	    row += dst_trans->stride/2;
	 }
      }
      break;
      default:
         assert(0);
         break;
      }
   }

   pipe->transfer_unmap(pipe, dst_trans);
   pipe->transfer_destroy(pipe, dst_trans);
}
