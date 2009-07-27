
/**
 * quad polygon stipple stage
 */

#include "lp_context.h"
#include "lp_quad.h"
#include "lp_quad_pipe.h"
#include "pipe/p_defines.h"
#include "util/u_memory.h"


/**
 * Apply polygon stipple to quads produced by triangle rasterization
 */
static void
stipple_quad(struct quad_stage *qs, struct quad_header *quads[], unsigned nr)
{
   static const uint bit31 = 1 << 31;
   static const uint bit30 = 1 << 30;
   unsigned pass = nr;

   if (quads[0]->input.prim == QUAD_PRIM_TRI) {
      struct llvmpipe_context *llvmpipe = qs->llvmpipe;
      unsigned q;

      pass = 0;

      for (q = 0; q < nr; q++)  {
         struct quad_header *quad = quads[q];

         const int col0 = quad->input.x0 % 32;
         const int y0 = quad->input.y0;
         const int y1 = y0 + 1;
         const uint stipple0 = llvmpipe->poly_stipple.stipple[y0 % 32];
         const uint stipple1 = llvmpipe->poly_stipple.stipple[y1 % 32];

         /* turn off quad mask bits that fail the stipple test */
         if ((stipple0 & (bit31 >> col0)) == 0)
            quad->inout.mask &= ~MASK_TOP_LEFT;

         if ((stipple0 & (bit30 >> col0)) == 0)
            quad->inout.mask &= ~MASK_TOP_RIGHT;

         if ((stipple1 & (bit31 >> col0)) == 0)
            quad->inout.mask &= ~MASK_BOTTOM_LEFT;

         if ((stipple1 & (bit30 >> col0)) == 0)
            quad->inout.mask &= ~MASK_BOTTOM_RIGHT;

         if (quad->inout.mask)
            quads[pass++] = quad;
      }
   }

   qs->next->run(qs->next, quads, pass);
}


static void stipple_begin(struct quad_stage *qs)
{
   qs->next->begin(qs->next);
}


static void stipple_destroy(struct quad_stage *qs)
{
   FREE( qs );
}


struct quad_stage *
lp_quad_polygon_stipple_stage( struct llvmpipe_context *llvmpipe )
{
   struct quad_stage *stage = CALLOC_STRUCT(quad_stage);

   stage->llvmpipe = llvmpipe;
   stage->begin = stipple_begin;
   stage->run = stipple_quad;
   stage->destroy = stipple_destroy;

   return stage;
}
