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

/* Authors:  Keith Whitwell <keith@tungstengraphics.com>
 */
#include "imports.h"

#define INTEL_DRAW_PRIVATE
#include "draw/intel_draw.h"

#define INTEL_PRIM_PRIVATE
#include "draw/intel_prim.h"



struct cull_stage {
   struct prim_stage stage;

   GLuint hw_data_offset;
   GLuint mode;
};



static INLINE struct cull_stage *cull_stage( struct prim_stage *stage )
{
   return (struct cull_stage *)stage;
}


static void cull_begin( struct prim_stage *stage )
{
   struct cull_stage *cull = cull_stage(stage);

   if (stage->pipe->draw->vb_state.clipped_prims)
      cull->hw_data_offset = 16;
   else
      cull->hw_data_offset = 0;	

   cull->mode = stage->pipe->draw->state.cull_mode;

   stage->next->begin( stage->next );
}


static void cull_tri( struct prim_stage *stage,
		      struct prim_header *header )
{
   GLuint hw_data_offset = cull_stage(stage)->hw_data_offset;
 
   GLfloat *v0 = (GLfloat *)&(header->v[0]->data[hw_data_offset]);
   GLfloat *v1 = (GLfloat *)&(header->v[1]->data[hw_data_offset]);
   GLfloat *v2 = (GLfloat *)&(header->v[2]->data[hw_data_offset]);

   GLfloat ex = v0[0] - v2[0];
   GLfloat ey = v0[1] - v2[1];
   GLfloat fx = v1[0] - v2[0];
   GLfloat fy = v1[1] - v2[1];
   
   header->det = ex * fy - ey * fx;

   if (header->det != 0) {
      GLuint mode = (header->det < 0) ? WINDING_CW : WINDING_CCW;
   
      if ((mode & cull_stage(stage)->mode) == 0)
	 stage->next->tri( stage->next, header );
   }
}


static void cull_line( struct prim_stage *stage,
		       struct prim_header *header )
{
   stage->next->line( stage->next, header );
}


static void cull_point( struct prim_stage *stage,
			struct prim_header *header )
{
   stage->next->point( stage->next, header );
}

static void cull_end( struct prim_stage *stage )
{
   stage->next->end( stage->next );
}

struct prim_stage *intel_prim_cull( struct prim_pipeline *pipe )
{
   struct cull_stage *cull = CALLOC_STRUCT(cull_stage);

   intel_prim_alloc_tmps( &cull->stage, 0 );

   cull->stage.pipe = pipe;
   cull->stage.next = NULL;
   cull->stage.begin = cull_begin;
   cull->stage.point = cull_point;
   cull->stage.line = cull_line;
   cull->stage.tri = cull_tri;
   cull->stage.reset_tmps = intel_prim_reset_tmps;
   cull->stage.end = cull_end;

   return &cull->stage;
}
