/* $Id: t_array_import.c,v 1.25.2.1 2002/10/15 16:56:52 keithw Exp $ */

/*
 * Mesa 3-D graphics library
 * Version:  4.1
 *
 * Copyright (C) 1999-2002  Brian Paul   All Rights Reserved.
 *
 * Permission is hereby granted, free of charge, to any person obtaining a
 * copy of this software and associated documentation files (the "Software"),
 * to deal in the Software without restriction, including without limitation
 * the rights to use, copy, modify, merge, publish, distribute, sublicense,
 * and/or sell copies of the Software, and to permit persons to whom the
 * Software is furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included
 * in all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS
 * OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.  IN NO EVENT SHALL
 * BRIAN PAUL BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER IN
 * AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN
 * CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.
 *
 * Authors:
 *    Keith Whitwell <keithw@valinux.com>
 */

#include "glheader.h"
#include "context.h"
#include "macros.h"
#include "mem.h"
#include "mmath.h"
#include "state.h"
#include "mtypes.h"

#include "array_cache/acache.h"
#include "math/m_translate.h"

#include "t_array_import.h"
#include "t_context.h"
#include "t_imm_debug.h"


static void _tnl_import_vertex( GLcontext *ctx,
				GLboolean writeable,
				GLboolean stride )
{
   struct gl_client_array *tmp;
   GLboolean is_writeable = 0;
   struct vertex_arrays *inputs = &TNL_CONTEXT(ctx)->array_inputs;

   tmp = _ac_import_vertex(ctx,
			   GL_FLOAT,
			   stride ? 4*sizeof(GLfloat) : 0,
			   0,
			   writeable,
			   &is_writeable);

   inputs->Obj.data = (GLfloat (*)[4]) tmp->Ptr;
   inputs->Obj.start = (GLfloat *) tmp->Ptr;
   inputs->Obj.stride = tmp->StrideB;
   inputs->Obj.size = tmp->Size;
   inputs->Obj.flags &= ~(VEC_BAD_STRIDE|VEC_NOT_WRITEABLE);
   if (inputs->Obj.stride != 4*sizeof(GLfloat))
      inputs->Obj.flags |= VEC_BAD_STRIDE;
   if (!is_writeable)
      inputs->Obj.flags |= VEC_NOT_WRITEABLE;
}

static void _tnl_import_normal( GLcontext *ctx,
				GLboolean writeable,
				GLboolean stride )
{
   struct gl_client_array *tmp;
   GLboolean is_writeable = 0;
   struct vertex_arrays *inputs = &TNL_CONTEXT(ctx)->array_inputs;

   tmp = _ac_import_normal(ctx, GL_FLOAT,
			   stride ? 3*sizeof(GLfloat) : 0, writeable,
			   &is_writeable);

   inputs->Normal.data = (GLfloat (*)[4]) tmp->Ptr;
   inputs->Normal.start = (GLfloat *) tmp->Ptr;
   inputs->Normal.stride = tmp->StrideB;
   inputs->Normal.flags &= ~(VEC_BAD_STRIDE|VEC_NOT_WRITEABLE);
   if (inputs->Normal.stride != 3*sizeof(GLfloat))
      inputs->Normal.flags |= VEC_BAD_STRIDE;
   if (!is_writeable)
      inputs->Normal.flags |= VEC_NOT_WRITEABLE;
}


static void _tnl_import_color( GLcontext *ctx,
			       GLenum type,
			       GLboolean writeable,
			       GLboolean stride )
{
   struct gl_client_array *tmp;
   GLboolean is_writeable = 0;
   struct vertex_arrays *inputs = &TNL_CONTEXT(ctx)->array_inputs;

   tmp = _ac_import_color(ctx,
			  type,
			  stride ? 4*sizeof(GLfloat) : 0,
			  4,
			  writeable,
			  &is_writeable);

   inputs->Color = *tmp;
}


static void _tnl_import_secondarycolor( GLcontext *ctx,
					GLenum type,
					GLboolean writeable,
					GLboolean stride )
{
   struct gl_client_array *tmp;
   GLboolean is_writeable = 0;
   struct vertex_arrays *inputs = &TNL_CONTEXT(ctx)->array_inputs;

   tmp = _ac_import_secondarycolor(ctx, 
				   type,
				   stride ? 4*sizeof(GLfloat) : 0,
				   4,
				   writeable,
				   &is_writeable);

   inputs->SecondaryColor = *tmp;
}

static void _tnl_import_fogcoord( GLcontext *ctx,
				  GLboolean writeable,
				  GLboolean stride )
{
   struct vertex_arrays *inputs = &TNL_CONTEXT(ctx)->array_inputs;
    struct gl_client_array *tmp;
   GLboolean is_writeable = 0;

   tmp = _ac_import_fogcoord(ctx, GL_FLOAT,
			     stride ? sizeof(GLfloat) : 0, writeable,
			     &is_writeable);

   inputs->FogCoord.data = (GLfloat (*)[4]) tmp->Ptr;
   inputs->FogCoord.start = (GLfloat *) tmp->Ptr;
   inputs->FogCoord.stride = tmp->StrideB;
   inputs->FogCoord.flags &= ~(VEC_BAD_STRIDE|VEC_NOT_WRITEABLE);
   if (inputs->FogCoord.stride != sizeof(GLfloat))
      inputs->FogCoord.flags |= VEC_BAD_STRIDE;
   if (!is_writeable)
      inputs->FogCoord.flags |= VEC_NOT_WRITEABLE;
}

static void _tnl_import_index( GLcontext *ctx,
			       GLboolean writeable,
			       GLboolean stride )
{
   struct vertex_arrays *inputs = &TNL_CONTEXT(ctx)->array_inputs;
   struct gl_client_array *tmp;
   GLboolean is_writeable = 0;

   tmp = _ac_import_index(ctx, GL_UNSIGNED_INT,
			  stride ? sizeof(GLuint) : 0, writeable,
			  &is_writeable);

   inputs->Index.data = (GLuint *) tmp->Ptr;
   inputs->Index.start = (GLuint *) tmp->Ptr;
   inputs->Index.stride = tmp->StrideB;
   inputs->Index.flags &= ~(VEC_BAD_STRIDE|VEC_NOT_WRITEABLE);
   if (inputs->Index.stride != sizeof(GLuint))
      inputs->Index.flags |= VEC_BAD_STRIDE;
   if (!is_writeable)
      inputs->Index.flags |= VEC_NOT_WRITEABLE;
}


static void _tnl_import_texcoord( GLcontext *ctx,
				  GLuint unit,
				  GLboolean writeable,
				  GLboolean stride )
{
   struct vertex_arrays *inputs = &TNL_CONTEXT(ctx)->array_inputs;
   struct gl_client_array *tmp;
   GLboolean is_writeable = 0;

   tmp = _ac_import_texcoord(ctx, unit, GL_FLOAT,
			     stride ? 4 * sizeof(GLfloat) : 0,
			     0,
			     writeable,
			     &is_writeable);

   inputs->TexCoord[unit].data = (GLfloat (*)[4]) tmp->Ptr;
   inputs->TexCoord[unit].start = (GLfloat *) tmp->Ptr;
   inputs->TexCoord[unit].stride = tmp->StrideB;
   inputs->TexCoord[unit].size = tmp->Size;
   inputs->TexCoord[unit].flags &= ~(VEC_BAD_STRIDE|VEC_NOT_WRITEABLE);
   if (inputs->TexCoord[unit].stride != 4*sizeof(GLfloat))
      inputs->TexCoord[unit].flags |= VEC_BAD_STRIDE;
   if (!is_writeable)
      inputs->TexCoord[unit].flags |= VEC_NOT_WRITEABLE;
}


static void _tnl_import_edgeflag( GLcontext *ctx,
				  GLboolean writeable,
				  GLboolean stride )
{
   struct vertex_arrays *inputs = &TNL_CONTEXT(ctx)->array_inputs;
   struct gl_client_array *tmp;
   GLboolean is_writeable = 0;

   tmp = _ac_import_edgeflag(ctx, GL_UNSIGNED_BYTE,
			     stride ? sizeof(GLubyte) : 0,
			     0,
			     &is_writeable);

   inputs->EdgeFlag.data = (GLubyte *) tmp->Ptr;
   inputs->EdgeFlag.start = (GLubyte *) tmp->Ptr;
   inputs->EdgeFlag.stride = tmp->StrideB;
   inputs->EdgeFlag.flags &= ~(VEC_BAD_STRIDE|VEC_NOT_WRITEABLE);
   if (inputs->EdgeFlag.stride != sizeof(GLubyte))
      inputs->EdgeFlag.flags |= VEC_BAD_STRIDE;
   if (!is_writeable)
      inputs->EdgeFlag.flags |= VEC_NOT_WRITEABLE;
}



static void _tnl_import_attrib( GLcontext *ctx,
                                GLuint index,
                                GLboolean writeable,
                                GLboolean stride )
{
   struct vertex_arrays *inputs = &TNL_CONTEXT(ctx)->array_inputs;
   struct gl_client_array *tmp;
   GLboolean is_writeable = 0;

   tmp = _ac_import_attrib(ctx, index, GL_FLOAT,
                           stride ? 4 * sizeof(GLfloat) : 0,
                           4,  /* want GLfloat[4] */
                           writeable,
                           &is_writeable);

   inputs->Attribs[index].data = (GLfloat (*)[4]) tmp->Ptr;
   inputs->Attribs[index].start = (GLfloat *) tmp->Ptr;
   inputs->Attribs[index].stride = tmp->StrideB;
   inputs->Attribs[index].size = tmp->Size;
   inputs->Attribs[index].flags &= ~(VEC_BAD_STRIDE|VEC_NOT_WRITEABLE);
   if (inputs->Attribs[index].stride != 4 * sizeof(GLfloat))
      inputs->Attribs[index].flags |= VEC_BAD_STRIDE;
   if (!is_writeable)
      inputs->Attribs[index].flags |= VEC_NOT_WRITEABLE;
}





void _tnl_vb_bind_arrays( GLcontext *ctx, GLint start, GLsizei count )
{
   TNLcontext *tnl = TNL_CONTEXT(ctx);
   struct vertex_buffer *VB = &tnl->vb;
   GLuint inputs = tnl->pipeline.inputs;
   struct vertex_arrays *tmp = &tnl->array_inputs;

/*        _mesa_debug(ctx, "%s %d..%d // %d..%d\n", __FUNCTION__, */
/*  	      start, count, ctx->Array.LockFirst, ctx->Array.LockCount);  */
/*        _tnl_print_vert_flags("    inputs", inputs);  */
/*        _tnl_print_vert_flags("    _Enabled", ctx->Array._Enabled); */

   VB->Count = count - start;
   VB->FirstClipped = VB->Count;
   VB->Elts = NULL;
   VB->MaterialMask = NULL;
   VB->Material = NULL;
   VB->Flag = NULL;
   VB->Primitive = tnl->tmp_primitive;
   VB->PrimitiveLength = tnl->tmp_primitive_length;

   if (ctx->Array.LockCount) {
      ASSERT(start == (GLint) ctx->Array.LockFirst);
      ASSERT(count == (GLint) ctx->Array.LockCount);
   }

   _ac_import_range( ctx, start, count );

   if (inputs & VERT_BIT_POS) {
      _tnl_import_vertex( ctx, 0, 0 );
      tmp->Obj.count = VB->Count;
      VB->ObjPtr = &tmp->Obj;
   }

   if (inputs & VERT_BIT_NORMAL) {
      _tnl_import_normal( ctx, 0, 0 );
      tmp->Normal.count = VB->Count;
      VB->NormalPtr = &tmp->Normal;
   }

   if (inputs & VERT_BIT_COLOR0) {
      _tnl_import_color( ctx, 0, 0, 0 );
      VB->ColorPtr[0] = &tmp->Color;
      VB->ColorPtr[1] = 0;
   }

   if (inputs & VERT_BITS_TEX_ANY) {
      GLuint unit;
      for (unit = 0; unit < ctx->Const.MaxTextureUnits; unit++) {
	 if (inputs & VERT_BIT_TEX(unit)) {
	    _tnl_import_texcoord( ctx, unit, GL_FALSE, GL_FALSE );
	    tmp->TexCoord[unit].count = VB->Count;
	    VB->TexCoordPtr[unit] = &tmp->TexCoord[unit];
	 }
      }
   }

   if (inputs & (VERT_BIT_INDEX | VERT_BIT_FOG |
                 VERT_BIT_EDGEFLAG | VERT_BIT_COLOR1)) {
      if (inputs & VERT_BIT_INDEX) {
	 _tnl_import_index( ctx, 0, 0 );
	 tmp->Index.count = VB->Count;
	 VB->IndexPtr[0] = &tmp->Index;
	 VB->IndexPtr[1] = 0;
      }

      if (inputs & VERT_BIT_FOG) {
	 _tnl_import_fogcoord( ctx, 0, 0 );
	 tmp->FogCoord.count = VB->Count;
	 VB->FogCoordPtr = &tmp->FogCoord;
      }

      if (inputs & VERT_BIT_EDGEFLAG) {
	 _tnl_import_edgeflag( ctx, GL_TRUE, sizeof(GLboolean) );
	 VB->EdgeFlag = (GLboolean *) tmp->EdgeFlag.data;
      }

      if (inputs & VERT_BIT_COLOR1) {
	 _tnl_import_secondarycolor( ctx, 0, 0, 0 );
	 VB->SecondaryColorPtr[0] = &tmp->SecondaryColor;
	 VB->SecondaryColorPtr[1] = 0;
      }
   }

   /* XXX not 100% sure this is finished.  Keith should probably inspect. */
   if (ctx->VertexProgram.Enabled) {
      GLuint index;
      for (index = 0; index < VERT_ATTRIB_MAX; index++) {
         /* XXX check program->InputsRead to reduce work here */
         _tnl_import_attrib( ctx, index, GL_FALSE, GL_TRUE );
         VB->AttribPtr[index] = &tmp->Attribs[index];
      }
   }
}
