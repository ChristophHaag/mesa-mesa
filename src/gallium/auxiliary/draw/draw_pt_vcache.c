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

 /*
  * Authors:
  *   Keith Whitwell <keith@tungstengraphics.com>
  */

#include "util/u_memory.h"
#include "util/u_prim.h"
#include "draw/draw_context.h"
#include "draw/draw_private.h"
#include "draw/draw_pt.h"


#define CACHE_MAX 256
#define FETCH_MAX 256
#define DRAW_MAX (16*1024)


struct vcache_frontend {
   struct draw_pt_front_end base;
   struct draw_context *draw;

   unsigned in[CACHE_MAX];
   ushort out[CACHE_MAX];

   ushort draw_elts[DRAW_MAX];
   unsigned fetch_elts[FETCH_MAX];

   unsigned draw_count;
   unsigned fetch_count;
   unsigned fetch_max;

   struct draw_pt_middle_end *middle;

   unsigned input_prim;
   unsigned output_prim;

   unsigned middle_prim;
   unsigned opt;
};


static INLINE void
vcache_flush( struct vcache_frontend *vcache )
{
   if (vcache->middle_prim != vcache->output_prim) {
      vcache->middle_prim = vcache->output_prim;
      vcache->middle->prepare( vcache->middle,
                               vcache->middle_prim,
                               vcache->opt,
                               &vcache->fetch_max );
   }

   if (vcache->draw_count) {
      vcache->middle->run( vcache->middle,
                           vcache->fetch_elts,
                           vcache->fetch_count,
                           vcache->draw_elts,
                           vcache->draw_count );
   }

   memset(vcache->in, ~0, sizeof(vcache->in));
   vcache->fetch_count = 0;
   vcache->draw_count = 0;
}


static INLINE void 
vcache_check_flush( struct vcache_frontend *vcache )
{
   if (vcache->draw_count + 6 >= DRAW_MAX ||
       vcache->fetch_count + 6 >= FETCH_MAX) {
      vcache_flush( vcache );
   }
}


static INLINE void 
vcache_elt( struct vcache_frontend *vcache,
            unsigned felt,
            ushort flags )
{
   unsigned idx = felt % CACHE_MAX;

   if (vcache->in[idx] != felt) {
      assert(vcache->fetch_count < FETCH_MAX);

      vcache->in[idx] = felt;
      vcache->out[idx] = (ushort)vcache->fetch_count;
      vcache->fetch_elts[vcache->fetch_count++] = felt;
   }

   vcache->draw_elts[vcache->draw_count++] = vcache->out[idx] | flags;
}


                   
static INLINE void 
vcache_triangle( struct vcache_frontend *vcache,
                 unsigned i0,
                 unsigned i1,
                 unsigned i2 )
{
   vcache_elt(vcache, i0, 0);
   vcache_elt(vcache, i1, 0);
   vcache_elt(vcache, i2, 0);
   vcache_check_flush(vcache);
}

			  
static INLINE void 
vcache_triangle_flags( struct vcache_frontend *vcache,
                       ushort flags,
                       unsigned i0,
                       unsigned i1,
                       unsigned i2 )
{
   vcache_elt(vcache, i0, flags);
   vcache_elt(vcache, i1, 0);
   vcache_elt(vcache, i2, 0);
   vcache_check_flush(vcache);
}


static INLINE void 
vcache_line( struct vcache_frontend *vcache,
             unsigned i0,
             unsigned i1 )
{
   vcache_elt(vcache, i0, 0);
   vcache_elt(vcache, i1, 0);
   vcache_check_flush(vcache);
}


static INLINE void 
vcache_line_flags( struct vcache_frontend *vcache,
                   ushort flags,
                   unsigned i0,
                   unsigned i1 )
{
   vcache_elt(vcache, i0, flags);
   vcache_elt(vcache, i1, 0);
   vcache_check_flush(vcache);
}


static INLINE void 
vcache_point( struct vcache_frontend *vcache,
              unsigned i0 )
{
   vcache_elt(vcache, i0, 0);
   vcache_check_flush(vcache);
}


static INLINE void
vcache_line_adj_flags( struct vcache_frontend *vcache,
                       unsigned flags,
                       unsigned a0, unsigned i0, unsigned i1, unsigned a1 )
{
   vcache_elt(vcache, a0, 0);
   vcache_elt(vcache, i0, flags);
   vcache_elt(vcache, i1, 0);
   vcache_elt(vcache, a1, 0);
   vcache_check_flush(vcache);
}


static INLINE void
vcache_line_adj( struct vcache_frontend *vcache,
                 unsigned a0, unsigned i0, unsigned i1, unsigned a1 )
{
   vcache_elt(vcache, a0, 0);
   vcache_elt(vcache, i0, 0);
   vcache_elt(vcache, i1, 0);
   vcache_elt(vcache, a1, 0);
   vcache_check_flush(vcache);
}


static INLINE void
vcache_triangle_adj_flags( struct vcache_frontend *vcache,
                           unsigned flags,
                           unsigned i0, unsigned a0,
                           unsigned i1, unsigned a1,
                           unsigned i2, unsigned a2 )
{
   vcache_elt(vcache, i0, flags);
   vcache_elt(vcache, a0, 0);
   vcache_elt(vcache, i1, 0);
   vcache_elt(vcache, a1, 0);
   vcache_elt(vcache, i2, 0);
   vcache_elt(vcache, a2, 0);
   vcache_check_flush(vcache);
}


static INLINE void
vcache_triangle_adj( struct vcache_frontend *vcache,
                     unsigned i0, unsigned a0,
                     unsigned i1, unsigned a1,
                     unsigned i2, unsigned a2 )
{
   vcache_elt(vcache, i0, 0);
   vcache_elt(vcache, a0, 0);
   vcache_elt(vcache, i1, 0);
   vcache_elt(vcache, a1, 0);
   vcache_elt(vcache, i2, 0);
   vcache_elt(vcache, a2, 0);
   vcache_check_flush(vcache);
}


/* At least for now, we're back to using a template include file for
 * this.  The two paths aren't too different though - it may be
 * possible to reunify them.
 */
#define TRIANGLE(flags,i0,i1,i2) vcache_triangle_flags(vcache,flags,i0,i1,i2)
#define LINE(flags,i0,i1)        vcache_line_flags(vcache,flags,i0,i1)
#define POINT(i0)                vcache_point(vcache,i0)
#define LINE_ADJ(flags,a0,i0,i1,a1) \
   vcache_line_adj_flags(vcache,flags,a0,i0,i1,a1)
#define TRIANGLE_ADJ(flags,i0,a0,i1,a1,i2,a2) \
   vcache_triangle_adj_flags(vcache,flags,i0,a0,i1,a1,i2,a2)
#define FUNC vcache_run_extras
#include "draw_pt_vcache_tmp.h"

#define TRIANGLE(flags,i0,i1,i2) vcache_triangle(vcache,i0,i1,i2)
#define LINE(flags,i0,i1)        vcache_line(vcache,i0,i1)
#define POINT(i0)                vcache_point(vcache,i0)
#define LINE_ADJ(flags,a0,i0,i1,a1) \
   vcache_line_adj(vcache,a0,i0,i1,a1)
#define TRIANGLE_ADJ(flags,i0,a0,i1,a1,i2,a2) \
   vcache_triangle_adj(vcache,i0,a0,i1,a1,i2,a2)
#define FUNC vcache_run
#include "draw_pt_vcache_tmp.h"

static INLINE void 
rebase_uint_elts( const unsigned *src,
                  unsigned count,
                  int delta,
                  ushort *dest )
{
   unsigned i;

   for (i = 0; i < count; i++) {
      assert(src[i] + delta < DRAW_PIPE_MAX_VERTICES);
      dest[i] = (ushort)(src[i] + delta);
   }
}


static INLINE void 
rebase_ushort_elts( const ushort *src,
                    unsigned count,
                    int delta,
                    ushort *dest )
{
   unsigned i;
   for (i = 0; i < count; i++) 
      dest[i] = (ushort)(src[i] + delta);
}


static INLINE void 
rebase_ubyte_elts( const ubyte *src,
                   unsigned count,
                   int delta,
                   ushort *dest )
{
   unsigned i;
   for (i = 0; i < count; i++) 
      dest[i] = (ushort)(src[i] + delta);
}


static INLINE void 
translate_uint_elts( const unsigned *src,
                     unsigned count,
                     ushort *dest )
{
   unsigned i;

   for (i = 0; i < count; i++) {
      assert(src[i] < DRAW_PIPE_MAX_VERTICES);
      dest[i] = (ushort)(src[i]);
   }
}


static INLINE void 
translate_ushort_elts( const ushort *src,
                       unsigned count,
                       ushort *dest )
{
   unsigned i;
   for (i = 0; i < count; i++) 
      dest[i] = (ushort)(src[i]);
}


static INLINE void 
translate_ubyte_elts( const ubyte *src,
                      unsigned count,
                      ushort *dest )
{
   unsigned i;
   for (i = 0; i < count; i++) 
      dest[i] = (ushort)(src[i]);
}




#if 0
static INLINE enum pipe_format 
format_from_get_elt( pt_elt_func get_elt )
{
   switch (draw->pt.user.eltSize) {
   case 1: return PIPE_FORMAT_R8_UNORM;
   case 2: return PIPE_FORMAT_R16_UNORM;
   case 4: return PIPE_FORMAT_R32_UNORM;
   default: return PIPE_FORMAT_NONE;
   }
}
#endif


/**
 * Check if any vertex attributes use instance divisors.
 * Note that instance divisors complicate vertex fetching so we need
 * to take the vcache path when they're in use.
 */
static boolean
any_instance_divisors(const struct draw_context *draw)
{
   uint i;

   for (i = 0; i < draw->pt.nr_vertex_elements; i++) {
      uint div = draw->pt.vertex_element[i].instance_divisor;
      if (div)
         return TRUE;
   }
   return FALSE;
}


static INLINE void 
vcache_check_run( struct draw_pt_front_end *frontend, 
                  pt_elt_func get_elt,
                  const void *elts,
                  int elt_bias,
                  unsigned draw_count )
{
   struct vcache_frontend *vcache = (struct vcache_frontend *)frontend; 
   struct draw_context *draw = vcache->draw;
   const unsigned min_index = draw->pt.user.min_index;
   const unsigned max_index = draw->pt.user.max_index;
   const unsigned index_size = draw->pt.user.eltSize;
   unsigned fetch_count;
   const ushort *transformed_elts;
   ushort *storage = NULL;
   boolean ok = FALSE;

   /* debug: verify indexes are in range [min_index, max_index] */
   if (0) {
      unsigned i;
      for (i = 0; i < draw_count; i++) {
         if (index_size == 1) {
            assert( ((const ubyte *) elts)[i] >= min_index);
            assert( ((const ubyte *) elts)[i] <= max_index);
         }
         else if (index_size == 2) {
            assert( ((const ushort *) elts)[i] >= min_index);
            assert( ((const ushort *) elts)[i] <= max_index);
         }
         else {
            assert(index_size == 4);
            assert( ((const uint *) elts)[i] >= min_index);
            assert( ((const uint *) elts)[i] <= max_index);
         }
      }
   }

   /* Note: max_index is frequently 0xffffffff so we have to be sure
    * that any arithmetic involving max_index doesn't overflow!
    */
   if (max_index >= (unsigned) DRAW_PIPE_MAX_VERTICES)
      goto fail;

   if (any_instance_divisors(draw))
      goto fail;

   fetch_count = max_index + 1 - min_index;

   if (0)
      debug_printf("fetch_count %d fetch_max %d draw_count %d\n", fetch_count, 
                   vcache->fetch_max,
                   draw_count);

   if (elt_bias + max_index >= DRAW_PIPE_MAX_VERTICES ||
       fetch_count >= UNDEFINED_VERTEX_ID ||
       fetch_count > draw_count) {
      if (0) debug_printf("fail\n");
      goto fail;
   }

   if (vcache->middle_prim != vcache->input_prim) {
      vcache->middle_prim = vcache->input_prim;
      vcache->middle->prepare( vcache->middle,
                               vcache->middle_prim,
                               vcache->opt,
                               &vcache->fetch_max );
   }

   assert((elt_bias >= 0 && min_index + elt_bias >= min_index) ||
          (elt_bias <  0 && min_index + elt_bias <  min_index));

   if (min_index == 0 &&
       index_size == 2) {
      transformed_elts = (const ushort *)elts;
   }
   else {
      storage = MALLOC( draw_count * sizeof(ushort) );
      if (!storage)
         goto fail;
      
      if (min_index == 0) {
         switch(index_size) {
         case 1:
            translate_ubyte_elts( (const ubyte *)elts,
                                  draw_count,
                                  storage );
            break;

         case 2:
            translate_ushort_elts( (const ushort *)elts,
                                   draw_count,
                                   storage );
            break;

         case 4:
            translate_uint_elts( (const uint *)elts,
                                 draw_count,
                                 storage );
            break;

         default:
            assert(0);
            FREE(storage);
            return;
         }
      }
      else {
         switch(index_size) {
         case 1:
            rebase_ubyte_elts( (const ubyte *)elts,
                               draw_count,
                               0 - (int)min_index,
                               storage );
            break;

         case 2:
            rebase_ushort_elts( (const ushort *)elts,
                                draw_count,
                                0 - (int)min_index,
                                storage );
            break;

         case 4:
            rebase_uint_elts( (const uint *)elts,
                              draw_count,
                              0 - (int)min_index,
                              storage );
            break;

         default:
            assert(0);
            FREE(storage);
            return;
         }
      }
      transformed_elts = storage;
   }

   if (fetch_count < UNDEFINED_VERTEX_ID)
      ok = vcache->middle->run_linear_elts( vcache->middle,
                                            min_index + elt_bias, /* start */
                                            fetch_count,
                                            transformed_elts,
                                            draw_count );
   
   FREE(storage);

   if (ok)
      return;

   debug_printf("failed to execute atomic draw elts for %d/%d, splitting up\n",
                fetch_count, draw_count);

fail:
   vcache_run( frontend, get_elt, elts, elt_bias, draw_count );
}




static void
vcache_prepare( struct draw_pt_front_end *frontend,
                unsigned in_prim,
                struct draw_pt_middle_end *middle,
                unsigned opt )
{
   struct vcache_frontend *vcache = (struct vcache_frontend *)frontend;

   if (opt & PT_PIPELINE) {
      vcache->base.run = vcache_run_extras;
   }
   else {
      vcache->base.run = vcache_check_run;
   }

   /* VCache will always emit the reduced version of its input
    * primitive, ie STRIP/FANS become TRIS, etc.
    *
    * This is not to be confused with what the GS might be up to,
    * which is a separate issue.
    */
   vcache->input_prim = in_prim;
   switch (in_prim) {
   case PIPE_PRIM_LINES_ADJACENCY:
   case PIPE_PRIM_LINE_STRIP_ADJACENCY:
      vcache->output_prim = PIPE_PRIM_LINES_ADJACENCY;
      break;
   case PIPE_PRIM_TRIANGLES_ADJACENCY:
   case PIPE_PRIM_TRIANGLE_STRIP_ADJACENCY:
      vcache->output_prim = PIPE_PRIM_TRIANGLES_ADJACENCY;
      break;
   default:
      vcache->output_prim = u_reduced_prim(in_prim);
   }

   vcache->middle = middle;
   vcache->opt = opt;

   /* Have to run prepare here, but try and guess a good prim for
    * doing so:
    */
   vcache->middle_prim = (opt & PT_PIPELINE)
      ? vcache->output_prim : vcache->input_prim;

   middle->prepare( middle,
                    vcache->middle_prim,
                    opt, &vcache->fetch_max );
}


static void 
vcache_finish( struct draw_pt_front_end *frontend )
{
   struct vcache_frontend *vcache = (struct vcache_frontend *)frontend;
   vcache->middle->finish( vcache->middle );
   vcache->middle = NULL;
}


static void 
vcache_destroy( struct draw_pt_front_end *frontend )
{
   FREE(frontend);
}


struct draw_pt_front_end *draw_pt_vcache( struct draw_context *draw )
{
   struct vcache_frontend *vcache = CALLOC_STRUCT( vcache_frontend );
   if (vcache == NULL)
      return NULL;
 
   vcache->base.prepare = vcache_prepare;
   vcache->base.run     = NULL;
   vcache->base.finish  = vcache_finish;
   vcache->base.destroy = vcache_destroy;
   vcache->draw = draw;
   
   memset(vcache->in, ~0, sizeof(vcache->in));
  
   return &vcache->base;
}
