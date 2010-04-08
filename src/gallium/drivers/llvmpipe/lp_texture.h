/**************************************************************************
 * 
 * Copyright 2007 Tungsten Graphics, Inc., Cedar Park, Texas.
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

#ifndef LP_TEXTURE_H
#define LP_TEXTURE_H


#include "pipe/p_state.h"


#define LP_MAX_TEXTURE_2D_LEVELS 12  /* 2K x 2K for now */
#define LP_MAX_TEXTURE_3D_LEVELS 10  /* 512 x 512 x 512 for now */

#define LP_MAX_TEXTURE_LEVELS LP_MAX_TEXTURE_2D_LEVELS


enum lp_texture_usage
{
   LP_TEX_USAGE_READ = 100,
   LP_TEX_USAGE_READ_WRITE,
   LP_TEX_USAGE_WRITE_ALL
};


/** Per-tile layout mode */
enum lp_texture_layout
{
   LP_TEX_LAYOUT_NONE = 0,  /**< no layout for the tile data yet */
   LP_TEX_LAYOUT_TILED,     /**< the tile data is in tiled layout */
   LP_TEX_LAYOUT_LINEAR,    /**< the tile data is in linear layout */
   LP_TEX_LAYOUT_BOTH       /**< the tile data is in both modes */
};


struct pipe_context;
struct pipe_screen;
struct llvmpipe_context;

struct sw_displaytarget;


/**
 * We keep one or two copies of the texture image data:  one in a simple
 * linear layout (for texture sampling) and another in a tiled layout (for
 * render targets).  We keep track of whether each image tile is linear
 * or tiled on a per-tile basis.
 */


/** A 1D/2D/3D image, one mipmap level */
struct llvmpipe_texture_image
{
   void *data;
};


struct llvmpipe_texture
{
   struct pipe_texture base;

   /** Row stride in bytes */
   unsigned stride[LP_MAX_TEXTURE_LEVELS];
   unsigned tiles_per_row[LP_MAX_TEXTURE_LEVELS];

   /**
    * Display target, for textures with the PIPE_TEXTURE_USAGE_DISPLAY_TARGET
    * usage.
    */
   struct sw_displaytarget *dt;

   /**
    * Malloc'ed data for regular textures, or a mapping to dt above.
    */
   struct llvmpipe_texture_image tiled[LP_MAX_TEXTURE_LEVELS];
   struct llvmpipe_texture_image linear[LP_MAX_TEXTURE_LEVELS];

   /** per-tile layout info */
   enum lp_texture_layout *layout[PIPE_TEX_FACE_MAX][LP_MAX_TEXTURE_LEVELS];

   unsigned timestamp;

   unsigned id;  /**< temporary, for debugging */
};


struct llvmpipe_transfer
{
   struct pipe_transfer base;

   unsigned long offset;
};


/** cast wrappers */
static INLINE struct llvmpipe_texture *
llvmpipe_texture(struct pipe_texture *pt)
{
   return (struct llvmpipe_texture *) pt;
}


static INLINE const struct llvmpipe_texture *
llvmpipe_texture_const(const struct pipe_texture *pt)
{
   return (const struct llvmpipe_texture *) pt;
}


static INLINE struct llvmpipe_transfer *
llvmpipe_transfer(struct pipe_transfer *pt)
{
   return (struct llvmpipe_transfer *) pt;
}


static INLINE unsigned
llvmpipe_texture_stride(struct pipe_texture *texture,
                        unsigned level)
{
   struct llvmpipe_texture *lpt = llvmpipe_texture(texture);
   assert(level < LP_MAX_TEXTURE_2D_LEVELS);
   return lpt->stride[level];
}


void *
llvmpipe_texture_map(struct pipe_texture *texture,
                     unsigned face,
                     unsigned level,
                     unsigned zslice,
                     enum lp_texture_usage usage,
                     enum lp_texture_layout layout);

void
llvmpipe_texture_unmap(struct pipe_texture *texture,
                       unsigned face,
                       unsigned level,
                       unsigned zslice);

void *
llvmpipe_get_texture_image_address(struct llvmpipe_texture *lpt,
                                   unsigned face, unsigned level,
                                   enum lp_texture_layout layout);

void *
llvmpipe_get_texture_image(struct llvmpipe_texture *texture,
                           unsigned face, unsigned level,
                           enum lp_texture_usage usage,
                           enum lp_texture_layout layout);


ubyte *
llvmpipe_get_texture_tile_linear(struct llvmpipe_texture *lpt,
                                 unsigned face, unsigned level,
                                 enum lp_texture_usage usage,
                                 unsigned x, unsigned y);

ubyte *
llvmpipe_get_texture_tile(struct llvmpipe_texture *lpt,
                          unsigned face, unsigned level,
                          enum lp_texture_usage usage,
                          unsigned x, unsigned y);



extern void
llvmpipe_init_screen_texture_funcs(struct pipe_screen *screen);

extern void
llvmpipe_init_context_texture_funcs(struct pipe_context *pipe);

#endif /* LP_TEXTURE_H */
