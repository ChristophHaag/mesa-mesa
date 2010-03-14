/**************************************************************************
 * 
 * Copyright 2006 Tungsten Graphics, Inc., Cedar Park, Texas.
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
 /*
  * Authors:
  *   Keith Whitwell <keith@tungstengraphics.com>
  *   Michel Dänzer <michel@tungstengraphics.com>
  */

#include "pipe/p_defines.h"
#include "util/u_inlines.h"

#include "util/u_format.h"
#include "util/u_math.h"
#include "util/u_memory.h"
#include "util/u_transfer.h"

#include "sp_context.h"
#include "sp_texture.h"
#include "sp_screen.h"

#include "state_tracker/sw_winsys.h"


/**
 * Conventional allocation path for non-display textures:
 * Use a simple, maximally packed layout.
 */
static boolean
softpipe_resource_layout(struct pipe_screen *screen,
                        struct softpipe_resource * spt)
{
   struct pipe_resource *pt = &spt->base;
   unsigned level;
   unsigned width = pt->width0;
   unsigned height = pt->height0;
   unsigned depth = pt->depth0;
   unsigned buffer_size = 0;

   for (level = 0; level <= pt->last_level; level++) {
      spt->stride[level] = util_format_get_stride(pt->format, width);

      spt->level_offset[level] = buffer_size;

      buffer_size += (util_format_get_nblocksy(pt->format, height) *
                      ((pt->target == PIPE_TEXTURE_CUBE) ? 6 : depth) *
                      spt->stride[level]);

      width  = u_minify(width, 1);
      height = u_minify(height, 1);
      depth = u_minify(depth, 1);
   }

   spt->data = align_malloc(buffer_size, 16);

   return spt->data != NULL;
}


/**
 * Texture layout for simple color buffers.
 */
static boolean
softpipe_displaytarget_layout(struct pipe_screen *screen,
                              struct softpipe_resource * spt)
{
   struct sw_winsys *winsys = softpipe_screen(screen)->winsys;

   /* Round up the surface size to a multiple of the tile size?
    */
   spt->dt = winsys->displaytarget_create(winsys,
                                          spt->base.format,
                                          spt->base.width0, 
                                          spt->base.height0,
                                          16,
                                          &spt->stride[0] );

   return spt->dt != NULL;
}


/**
 * Create new pipe_resource given the template information.
 */
static struct pipe_resource *
softpipe_resource_create(struct pipe_screen *screen,
                        const struct pipe_resource *template)
{
   struct softpipe_resource *spt = CALLOC_STRUCT(softpipe_resource);
   if (!spt)
      return NULL;

   assert(template->format != PIPE_FORMAT_NONE);

   spt->base = *template;
   pipe_reference_init(&spt->base.reference, 1);
   spt->base.screen = screen;

   spt->pot = (util_is_power_of_two(template->width0) &&
               util_is_power_of_two(template->height0) &&
               util_is_power_of_two(template->depth0));

   if (spt->base.tex_usage & (PIPE_TEXTURE_USAGE_DISPLAY_TARGET |
                              PIPE_TEXTURE_USAGE_SCANOUT |
                              PIPE_TEXTURE_USAGE_SHARED)) {
      if (!softpipe_displaytarget_layout(screen, spt))
         goto fail;
   }
   else {
      if (!softpipe_resource_layout(screen, spt))
         goto fail;
   }
    
   return &spt->base;

 fail:
   FREE(spt);
   return NULL;
}




static void
softpipe_resource_destroy(struct pipe_screen *pscreen,
			  struct pipe_resource *pt)
{
   struct softpipe_screen *screen = softpipe_screen(pscreen);
   struct softpipe_resource *spt = softpipe_resource(pt);

   if (spt->dt) {
      /* display target */
      struct sw_winsys *winsys = screen->winsys;
      winsys->displaytarget_destroy(winsys, spt->dt);
   }
   else if (!spt->userBuffer) {
      /* regular texture */
      align_free(spt->data);
   }

   FREE(spt);
}


/**
 * Get a pipe_surface "view" into a texture.
 */
static struct pipe_surface *
softpipe_get_tex_surface(struct pipe_screen *screen,
                         struct pipe_resource *pt,
                         unsigned face, unsigned level, unsigned zslice,
                         unsigned usage)
{
   struct softpipe_resource *spt = softpipe_resource(pt);
   struct pipe_surface *ps;

   assert(level <= pt->last_level);

   ps = CALLOC_STRUCT(pipe_surface);
   if (ps) {
      pipe_reference_init(&ps->reference, 1);
      pipe_resource_reference(&ps->texture, pt);
      ps->format = pt->format;
      ps->width = u_minify(pt->width0, level);
      ps->height = u_minify(pt->height0, level);
      ps->offset = spt->level_offset[level];
      ps->usage = usage;

      ps->face = face;
      ps->level = level;
      ps->zslice = zslice;

      if (pt->target == PIPE_TEXTURE_CUBE) {
         ps->offset += face * util_format_get_nblocksy(pt->format, u_minify(pt->height0, level)) *
                       spt->stride[level];
      }
      else if (pt->target == PIPE_TEXTURE_3D) {
         ps->offset += zslice * util_format_get_nblocksy(pt->format, u_minify(pt->height0, level)) *
                       spt->stride[level];
      }
      else {
         assert(face == 0);
         assert(zslice == 0);
      }
   }
   return ps;
}


/**
 * Free a pipe_surface which was created with softpipe_get_tex_surface().
 */
static void 
softpipe_tex_surface_destroy(struct pipe_surface *surf)
{
   /* Effectively do the texture_update work here - if texture images
    * needed post-processing to put them into hardware layout, this is
    * where it would happen.  For softpipe, nothing to do.
    */
   assert(surf->texture);
   pipe_resource_reference(&surf->texture, NULL);
   FREE(surf);
}


/**
 * Geta pipe_transfer object which is used for moving data in/out of
 * a texture object.
 * \param face  one of PIPE_TEX_FACE_x or 0
 * \param level  texture mipmap level
 * \param zslice  2D slice of a 3D texture
 * \param usage  one of PIPE_TRANSFER_READ/WRITE/READ_WRITE
 * \param x  X position of region to read/write
 * \param y  Y position of region to read/write
 * \param width  width of region to read/write
 * \param height  height of region to read/write
 */
static struct pipe_transfer *
softpipe_get_transfer(struct pipe_context *pipe,
		      struct pipe_resource *resource,
		      struct pipe_subresource sr,
		      enum pipe_transfer_usage usage,
		      const struct pipe_box *box)
{
   struct softpipe_resource *sptex = softpipe_resource(resource);
   struct softpipe_transfer *spt;

   assert(resource);
   assert(sr.level <= resource->last_level);

   /* make sure the requested region is in the image bounds */
   assert(box->x + box->width <= u_minify(resource->width0, sr.level));
   assert(box->y + box->height <= u_minify(resource->height0, sr.level));
   assert(box->z + box->depth <= u_minify(resource->depth0, sr.level));

   spt = CALLOC_STRUCT(softpipe_transfer);
   if (spt) {
      struct pipe_transfer *pt = &spt->base;
      enum pipe_format format = resource->format;
      int nblocksy = util_format_get_nblocksy(resource->format, 
					      u_minify(resource->height0, sr.level));
      pipe_resource_reference(&pt->resource, resource);
      pt->box = *box;
      pt->stride = sptex->stride[sr.level];
      pt->usage = usage;
      //pt->face = face;
      //pt->level = level;

      spt->offset = sptex->level_offset[sr.level];

      if (resource->target == PIPE_TEXTURE_CUBE) {
         spt->offset += sr.face * nblocksy * pt->stride;
      }
      else if (resource->target == PIPE_TEXTURE_3D) {
         spt->offset += box->z * nblocksy * pt->stride;
      }
      else {
         assert(sr.face == 0);
         assert(box->z == 0);
      }
      
      spt->offset += 
	 box->y / util_format_get_blockheight(format) * spt->base.stride +
	 box->x / util_format_get_blockwidth(format) * util_format_get_blocksize(format);

      return pt;
   }
   return NULL;
}


/**
 * Free a pipe_transfer object which was created with
 * softpipe_get_transfer().
 */
static void 
softpipe_transfer_destroy(struct pipe_context *pipe,
                              struct pipe_transfer *transfer)
{
   pipe_resource_reference(&transfer->resource, NULL);
   FREE(transfer);
}


/**
 * Create memory mapping for given pipe_transfer object.
 */
static void *
softpipe_transfer_map( struct pipe_context *pipe,
                       struct pipe_transfer *transfer )
{
   struct softpipe_transfer *sp_transfer = softpipe_transfer(transfer);
   struct softpipe_resource *sp_resource = softpipe_resource(transfer->resource);
   struct sw_winsys *winsys = softpipe_screen(pipe->screen)->winsys;
   uint8_t *map;
   
   /* resources backed by display target treated specially:
    */
   if (sp_resource->dt) {
      map = winsys->displaytarget_map(winsys,
				      sp_resource->dt,
                                      transfer->usage);
   }
   else {
      map = sp_resource->data;
   }

   if (map == NULL)
      return NULL;
   else
      return map + sp_transfer->offset;
}


/**
 * Unmap memory mapping for given pipe_transfer object.
 */
static void
softpipe_transfer_unmap(struct pipe_context *pipe,
                        struct pipe_transfer *transfer)
{
   struct softpipe_resource *spt;

   assert(transfer->resource);
   spt = softpipe_resource(transfer->resource);

   if (spt->dt) {
      /* display target */
      struct sw_winsys *winsys = softpipe_screen(pipe->screen)->winsys;
      winsys->displaytarget_unmap(winsys, spt->dt);
   }

   if (transfer->usage & PIPE_TRANSFER_WRITE) {
      /* Mark the texture as dirty to expire the tile caches. */
      spt->timestamp++;
   }
}

/**
 * Create buffer which wraps user-space data.
 */
static struct pipe_resource *
softpipe_user_buffer_create(struct pipe_screen *screen,
                            void *ptr,
                            unsigned bytes,
			    unsigned usage)
{
   struct softpipe_resource *buffer;

   buffer = CALLOC_STRUCT(softpipe_resource);
   if(!buffer)
      return NULL;

   pipe_reference_init(&buffer->base.reference, 1);
   buffer->base.screen = screen;
   buffer->base.format = PIPE_FORMAT_R8_UNORM; /* ?? */
   buffer->base.usage = usage;
   buffer->base.width0 = bytes;
   buffer->base.height0 = 1;
   buffer->base.depth0 = 1;
   buffer->userBuffer = TRUE;
   buffer->data = ptr;

   return &buffer->base;
}





void
softpipe_init_texture_funcs(struct pipe_context *pipe)
{
   pipe->get_transfer = softpipe_get_transfer;
   pipe->transfer_destroy = softpipe_transfer_destroy;
   pipe->transfer_map = softpipe_transfer_map;
   pipe->transfer_unmap = softpipe_transfer_unmap;

   pipe->transfer_flush_region = u_default_transfer_flush_region;
   pipe->transfer_inline_write = u_default_transfer_inline_write;
}

void
softpipe_init_screen_texture_funcs(struct pipe_screen *screen)
{
   screen->resource_create = softpipe_resource_create;
   screen->resource_destroy = softpipe_resource_destroy;
   screen->user_buffer_create = softpipe_user_buffer_create;

   screen->get_tex_surface = softpipe_get_tex_surface;
   screen->tex_surface_destroy = softpipe_tex_surface_destroy;
}



