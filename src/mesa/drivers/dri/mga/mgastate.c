/*
 * Copyright 2000-2001 VA Linux Systems, Inc.
 * All Rights Reserved.
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
 * FITNESS FOR A PARTICULAR PURPOSE AND NON-INFRINGEMENT.  IN NO EVENT SHALL
 * VA LINUX SYSTEMS AND/OR ITS SUPPLIERS BE LIABLE FOR ANY CLAIM, DAMAGES OR
 * OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE,
 * ARISING FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR
 * OTHER DEALINGS IN THE SOFTWARE.
 *
 * Authors:
 *    Keith Whitwell <keith@tungstengraphics.com>
 */
/* $XFree86: xc/lib/GL/mesa/src/drv/mga/mgastate.c,v 1.13 2002/10/30 12:51:36 alanh Exp $ */


#include "mtypes.h"
#include "colormac.h"
#include "dd.h"

#include "mm.h"
#include "mgacontext.h"
#include "mgadd.h"
#include "mgastate.h"
#include "mgatex.h"
#include "mgavb.h"
#include "mgatris.h"
#include "mgaioctl.h"
#include "mgaregs.h"

#include "swrast/swrast.h"
#include "array_cache/acache.h"
#include "tnl/tnl.h"
#include "tnl/t_context.h"
#include "tnl/t_pipeline.h"
#include "swrast_setup/swrast_setup.h"


static void updateSpecularLighting( GLcontext *ctx );


/* Some outstanding problems with accelerating logic ops...
 */
#if defined(ACCEL_ROP)
static const GLuint mgarop_NoBLK[16] = {
   DC_atype_rpl  | 0x00000000, DC_atype_rstr | 0x00080000,
   DC_atype_rstr | 0x00040000, DC_atype_rpl  | 0x000c0000,
   DC_atype_rstr | 0x00020000, DC_atype_rstr | 0x000a0000,
   DC_atype_rstr | 0x00060000, DC_atype_rstr | 0x000e0000,
   DC_atype_rstr | 0x00010000, DC_atype_rstr | 0x00090000,
   DC_atype_rstr | 0x00050000, DC_atype_rstr | 0x000d0000,
   DC_atype_rpl  | 0x00030000, DC_atype_rstr | 0x000b0000,
   DC_atype_rstr | 0x00070000, DC_atype_rpl  | 0x000f0000
};
#endif


/* =============================================================
 * Alpha blending
 */

static void mgaDDAlphaFunc(GLcontext *ctx, GLenum func, GLfloat ref)
{
   mgaContextPtr mmesa = MGA_CONTEXT(ctx);
   GLubyte refByte;
   GLuint  a;
   
   CLAMPED_FLOAT_TO_UBYTE(refByte, ref);

   switch ( func ) {
   case GL_NEVER:
      a = AC_atmode_alt;
      refByte = 0;
      break;
   case GL_LESS:
      a = AC_atmode_alt;
      break;
   case GL_GEQUAL:
      a = AC_atmode_agte;
      break;
   case GL_LEQUAL:
      a = AC_atmode_alte;
      break;
   case GL_GREATER:
      a = AC_atmode_agt;
      break;
   case GL_NOTEQUAL:
      a = AC_atmode_ane;
      break;
   case GL_EQUAL:
      a = AC_atmode_ae;
      break;
   case GL_ALWAYS:
      a = AC_atmode_noacmp;
      break;
   default:
      a = 0;
      break;
   }

   FLUSH_BATCH( mmesa );
   mmesa->hw.alpha_func = a | MGA_FIELD( AC_atref, refByte );
   mmesa->dirty |= MGA_UPLOAD_CONTEXT;
}

static void mgaDDBlendEquation(GLcontext *ctx, GLenum mode)
{
   FLUSH_BATCH( MGA_CONTEXT(ctx) );

   /* BlendEquation sets ColorLogicOpEnabled in an unexpected 
    * manner.  
    */
   FALLBACK( ctx, MGA_FALLBACK_LOGICOP,
	     (ctx->Color.ColorLogicOpEnabled && 
	      ctx->Color.LogicOp != GL_COPY));
}

static void mgaDDBlendFunc(GLcontext *ctx, GLenum sfactor, GLenum dfactor)
{
   mgaContextPtr mmesa = MGA_CONTEXT(ctx);
   __DRIdrawablePrivate *driDrawable = mmesa->driDrawable;
   GLuint   src;
   GLuint   dst;

   switch (ctx->Color.BlendSrcRGB) {
   case GL_ZERO:
      src = AC_src_zero; break;
   case GL_SRC_ALPHA:
      src = AC_src_src_alpha; break;
   case GL_ONE:
   default:		/* never happens */
      src = AC_src_one; break;
   case GL_DST_COLOR:
      src = AC_src_dst_color; break;
   case GL_ONE_MINUS_DST_COLOR:
      src = AC_src_om_dst_color; break;
   case GL_ONE_MINUS_SRC_ALPHA:
      src = AC_src_om_src_alpha; break;
   case GL_DST_ALPHA:
      src = (driDrawable->cpp == 4) 
	  ? AC_src_dst_alpha : AC_src_one;
      break;
   case GL_ONE_MINUS_DST_ALPHA:
      src = (driDrawable->cpp == 4)
	  ? AC_src_om_dst_alpha : AC_src_zero;
      break;
   case GL_SRC_ALPHA_SATURATE:
      src = (ctx->Visual.alphaBits > 0)
	  ? AC_src_src_alpha_sat : AC_src_zero;
      break;
   }

   switch (ctx->Color.BlendDstRGB) {
   case GL_SRC_ALPHA:
      dst = AC_dst_src_alpha; break;
   case GL_ONE_MINUS_SRC_ALPHA:
      dst = AC_dst_om_src_alpha; break;
   default:		/* never happens */
   case GL_ZERO:
      dst = AC_dst_zero; break;
   case GL_ONE:
      dst = AC_dst_one; break;
   case GL_SRC_COLOR:
      dst = AC_dst_src_color; break;
   case GL_ONE_MINUS_SRC_COLOR:
      dst = AC_dst_om_src_color; break;
   case GL_DST_ALPHA:
      dst = (driDrawable->cpp == 4)
	  ? AC_dst_dst_alpha : AC_dst_one;
      break;
   case GL_ONE_MINUS_DST_ALPHA:
      dst = (driDrawable->cpp == 4)
	  ? AC_dst_om_dst_alpha : AC_dst_zero;
      break;
   }

   FLUSH_BATCH( mmesa );
   mmesa->hw.blend_func = (src | dst);
   mmesa->dirty |= MGA_UPLOAD_CONTEXT;
}

static void mgaDDBlendFuncSeparate( GLcontext *ctx, GLenum sfactorRGB,
				    GLenum dfactorRGB, GLenum sfactorA,
				    GLenum dfactorA )
{
   mgaDDBlendFunc( ctx, sfactorRGB, dfactorRGB );
}


/* =============================================================
 * Depth testing
 */

static void mgaDDDepthFunc(GLcontext *ctx, GLenum func)
{
   mgaContextPtr mmesa = MGA_CONTEXT( ctx );
   int zmode;

   switch (func) {
   case GL_NEVER:
      /* can't do this in h/w, we'll use a s/w fallback */
      FALLBACK (ctx, MGA_FALLBACK_DEPTH, ctx->Depth.Test);

      /* FALLTHROUGH */
   case GL_ALWAYS:
      zmode = DC_zmode_nozcmp; break;
   case GL_LESS:
      zmode = DC_zmode_zlt; break;
   case GL_LEQUAL:
      zmode = DC_zmode_zlte; break;
   case GL_EQUAL:
      zmode = DC_zmode_ze; break;
   case GL_GREATER:
      zmode = DC_zmode_zgt; break;
   case GL_GEQUAL:
      zmode = DC_zmode_zgte; break;
   case GL_NOTEQUAL:
      zmode = DC_zmode_zne; break;
   default:
      zmode = 0; break;
   }

   FLUSH_BATCH( mmesa );
   mmesa->hw.zmode &= DC_zmode_MASK;
   mmesa->hw.zmode |= zmode;
   mmesa->dirty |= MGA_UPLOAD_CONTEXT;
}

static void mgaDDDepthMask(GLcontext *ctx, GLboolean flag)
{
   mgaContextPtr mmesa = MGA_CONTEXT( ctx );


   FLUSH_BATCH( mmesa );
   mmesa->hw.zmode &= DC_atype_MASK;
   mmesa->hw.zmode |= (flag) ? DC_atype_zi : DC_atype_i;
   mmesa->dirty |= MGA_UPLOAD_CONTEXT;
}


static void mgaDDClearDepth(GLcontext *ctx, GLclampd d)
{
   mgaContextPtr mmesa = MGA_CONTEXT(ctx);

   /* Select the Z depth.  The ~ is used because the _MASK values in the
    * MGA driver are used to mask OFF the selected bits.  In this case,
    * we want to mask off everything except the MA_zwidth bits.
    */
   switch (mmesa->setup.maccess & ~MA_zwidth_MASK) {
   case MA_zwidth_16: mmesa->ClearDepth = d * 0x0000ffff; break;
   case MA_zwidth_24: mmesa->ClearDepth = d * 0xffffff00; break;
   case MA_zwidth_32: mmesa->ClearDepth = d * 0xffffffff; break;
   default: return;
   }
}


/* =============================================================
 * Fog
 */


static void mgaDDFogfv(GLcontext *ctx, GLenum pname, const GLfloat *param)
{
   mgaContextPtr mmesa = MGA_CONTEXT(ctx);

   if (pname == GL_FOG_COLOR) {
      GLuint color = PACK_COLOR_888((GLubyte)(ctx->Fog.Color[0]*255.0F), 
				    (GLubyte)(ctx->Fog.Color[1]*255.0F), 
				    (GLubyte)(ctx->Fog.Color[2]*255.0F));

      MGA_STATECHANGE(mmesa, MGA_UPLOAD_CONTEXT);   
      mmesa->setup.fogcolor = color;
   }
}


/* =============================================================
 * Scissoring
 */


void mgaUpdateClipping(const GLcontext *ctx)
{
   mgaContextPtr mmesa = MGA_CONTEXT(ctx);

   if (mmesa->driDrawable)
   {
      int x1 = mmesa->driDrawable->x + ctx->Scissor.X;
      int y1 = mmesa->driDrawable->y + mmesa->driDrawable->h
	 - (ctx->Scissor.Y + ctx->Scissor.Height);
      int x2 = x1 + ctx->Scissor.Width - 1;
      int y2 = y1 + ctx->Scissor.Height - 1;

      if (x1 < 0) x1 = 0;
      if (y1 < 0) y1 = 0;
      if (x2 < 0) x2 = 0;
      if (y2 < 0) y2 = 0;

      mmesa->scissor_rect.x1 = x1;
      mmesa->scissor_rect.y1 = y1;
      mmesa->scissor_rect.x2 = x2;
      mmesa->scissor_rect.y2 = y2;

      mmesa->dirty |= MGA_UPLOAD_CLIPRECTS;
   }
}


static void mgaDDScissor( GLcontext *ctx, GLint x, GLint y,
			  GLsizei w, GLsizei h )
{
   if ( ctx->Scissor.Enabled ) {
      FLUSH_BATCH( MGA_CONTEXT(ctx) );	/* don't pipeline cliprect changes */
      mgaUpdateClipping( ctx );
   }
}


/* =============================================================
 * Culling
 */


#define _CULL_DISABLE 0
#define _CULL_NEGATIVE ((1<<11)|(1<<5)|(1<<16))
#define _CULL_POSITIVE (1<<11)

static void mgaDDCullFaceFrontFace(GLcontext *ctx, GLenum unused)
{
   mgaContextPtr mmesa = MGA_CONTEXT(ctx);

   FLUSH_BATCH( mmesa );
   if (ctx->Polygon.CullFlag && 
       ctx->Polygon.CullFaceMode != GL_FRONT_AND_BACK) 
   {
      mmesa->hw.cull = _CULL_NEGATIVE;

      if (ctx->Polygon.CullFaceMode == GL_FRONT)
	 mmesa->hw.cull ^= (_CULL_POSITIVE ^ _CULL_NEGATIVE);

      if (ctx->Polygon.FrontFace != GL_CCW)
	 mmesa->hw.cull ^= (_CULL_POSITIVE ^ _CULL_NEGATIVE);

      mmesa->hw.cull_dualtex = mmesa->hw.cull ^
	  (_CULL_POSITIVE ^ _CULL_NEGATIVE); /* warp bug? */
   }
   else {
      mmesa->hw.cull = _CULL_DISABLE;
      mmesa->hw.cull_dualtex = _CULL_DISABLE;
   }

   mmesa->dirty |= MGA_UPLOAD_CONTEXT;
}


/* =============================================================
 * Masks
 */

static void mgaDDColorMask(GLcontext *ctx, 
			   GLboolean r, GLboolean g, 
			   GLboolean b, GLboolean a )
{
   mgaContextPtr mmesa = MGA_CONTEXT( ctx );
   __DRIdrawablePrivate *driDrawable = mmesa->driDrawable;


   GLuint mask = mgaPackColor(driDrawable->cpp,
			      ctx->Color.ColorMask[RCOMP],
			      ctx->Color.ColorMask[GCOMP],
			      ctx->Color.ColorMask[BCOMP],
			      ctx->Color.ColorMask[ACOMP]);

   if (driDrawable->cpp == 2)
      mask = mask | (mask << 16);

   if (mmesa->setup.plnwt != mask) {
      MGA_STATECHANGE( mmesa, MGA_UPLOAD_CONTEXT );
      mmesa->setup.plnwt = mask;      
   }
}


/* =============================================================
 * Polygon state
 */

static int mgaStipples[16] = {
   0xffff1,  /* See above note */
   0xa5a5,
   0x5a5a,
   0xa0a0,
   0x5050,
   0x0a0a,
   0x0505,
   0x8020,
   0x0401,
   0x1040,
   0x0208,
   0x0802,
   0x4010,
   0x0104,
   0x2080,
   0x0000
};

/**
 * The MGA supports a subset of possible 4x4 stipples natively, GL
 * wants 32x32.  Fortunately stipple is usually a repeating pattern.
 *
 * \param ctx GL rendering context to be affected
 * \param mask Pointer to the 32x32 stipple mask
 *
 * \note the fully opaque pattern (0xffff) has been disabled in order
 * to work around a conformance issue.
 */

static void mgaDDPolygonStipple( GLcontext *ctx, const GLubyte *mask )
{
   mgaContextPtr mmesa = MGA_CONTEXT(ctx);
   const GLubyte *m = mask;
   GLubyte p[4];
   int i,j,k;
   int active = (ctx->Polygon.StippleFlag && 
		 mmesa->raster_primitive == GL_TRIANGLES);
   GLuint stipple;

   FLUSH_BATCH(mmesa);
   mmesa->haveHwStipple = 0;

   if (active) {
      mmesa->dirty |= MGA_UPLOAD_CONTEXT;
      mmesa->setup.dwgctl &= ~(0xf<<20);
   }

   p[0] = mask[0] & 0xf; p[0] |= p[0] << 4;
   p[1] = mask[4] & 0xf; p[1] |= p[1] << 4;
   p[2] = mask[8] & 0xf; p[2] |= p[2] << 4;
   p[3] = mask[12] & 0xf; p[3] |= p[3] << 4;

   for (k = 0 ; k < 8 ; k++)
      for (j = 0 ; j < 4; j++)
	 for (i = 0 ; i < 4 ; i++)
	    if (*m++ != p[j]) {
	       return;
	    }

   stipple = ( ((p[0] & 0xf) << 0) |
	       ((p[1] & 0xf) << 4) |
	       ((p[2] & 0xf) << 8) |
	       ((p[3] & 0xf) << 12) );

   for (i = 0 ; i < 16 ; i++)
      if (mgaStipples[i] == stipple) {
	 mmesa->poly_stipple = i<<20;
	 mmesa->haveHwStipple = 1;
	 break;
      }
   
   if (active) {
      mmesa->setup.dwgctl &= ~(0xf<<20);
      mmesa->setup.dwgctl |= mmesa->poly_stipple;
   }
}


/* =============================================================
 * Rendering attributes
 *
 * We really don't want to recalculate all this every time we bind a
 * texture.  These things shouldn't change all that often, so it makes
 * sense to break them out of the core texture state update routines.
 */

static void updateSpecularLighting( GLcontext *ctx )
{
   mgaContextPtr mmesa = MGA_CONTEXT(ctx);
   unsigned int specen;

   specen = (ctx->Light.Model.ColorControl == GL_SEPARATE_SPECULAR_COLOR &&
	     ctx->Light.Enabled) ? TMC_specen_enable : 0;

   if ( specen != mmesa->hw.specen ) {
      mmesa->hw.specen = specen;
      mmesa->dirty |= MGA_UPLOAD_TEX0 | MGA_UPLOAD_TEX1;
      
      mgaChooseVertexState( ctx );
   }
}


/* =============================================================
 * Materials
 */


static void mgaDDLightModelfv(GLcontext *ctx, GLenum pname,
			      const GLfloat *param)
{
   if (pname == GL_LIGHT_MODEL_COLOR_CONTROL) {
      FLUSH_BATCH( MGA_CONTEXT(ctx) );
      updateSpecularLighting( ctx );
   }
}


static void mgaDDShadeModel(GLcontext *ctx, GLenum mode)
{
   /* FIXME: This used to FLUSH_BATCH and set MGA_NEW_TEXTURE in new_state,
    * FIXME: so I'm not sure what to do here now.
    */
}


/* =============================================================
 * Stencil
 */


static void mgaDDStencilFunc(GLcontext *ctx, GLenum func, GLint ref,
			     GLuint mask)
{
   mgaContextPtr mmesa = MGA_CONTEXT(ctx);
   GLuint  stencil;
   GLuint  stencilctl;

   stencil = (ref << S_sref_SHIFT) | (mask << S_smsk_SHIFT);
   switch (func)
   {
   case GL_NEVER:
      stencilctl = SC_smode_snever;
      break;
   case GL_LESS:
      stencilctl = SC_smode_slt;
      break;
   case GL_LEQUAL:
      stencilctl = SC_smode_slte;
      break;
   case GL_GREATER:
      stencilctl = SC_smode_sgt;
      break;
   case GL_GEQUAL:
      stencilctl = SC_smode_sgte;
      break;
   case GL_NOTEQUAL:
      stencilctl = SC_smode_sne;
      break;
   case GL_EQUAL:
      stencilctl = SC_smode_se;
      break;
   case GL_ALWAYS:
   default:
      stencilctl = SC_smode_salways;
      break;
   }

   FLUSH_BATCH( mmesa );
   mmesa->hw.stencil &= (S_sref_MASK & S_smsk_MASK);
   mmesa->hw.stencil |= stencil;
   mmesa->hw.stencilctl &= SC_smode_MASK;
   mmesa->hw.stencilctl |= stencilctl;
   mmesa->dirty |= MGA_UPLOAD_CONTEXT;
}

static void mgaDDStencilMask(GLcontext *ctx, GLuint mask)
{
   mgaContextPtr mmesa = MGA_CONTEXT(ctx);

   FLUSH_BATCH( mmesa );
   mmesa->hw.stencil &= S_swtmsk_MASK;
   mmesa->hw.stencil |= (mask << S_swtmsk_SHIFT);
   mmesa->dirty |= MGA_UPLOAD_CONTEXT;
}

static void mgaDDStencilOp(GLcontext *ctx, GLenum fail, GLenum zfail,
			   GLenum zpass)
{
   mgaContextPtr mmesa = MGA_CONTEXT(ctx);
   GLuint  stencilctl;

   stencilctl = 0;
   switch (ctx->Stencil.FailFunc[0])
   {
   case GL_KEEP:
      stencilctl |= SC_sfailop_keep;
      break;
   case GL_ZERO:
      stencilctl |= SC_sfailop_zero;
      break;
   case GL_REPLACE:
      stencilctl |= SC_sfailop_replace;
      break;
   case GL_INCR:
      stencilctl |= SC_sfailop_incrsat;
      break;
   case GL_DECR:
      stencilctl |= SC_sfailop_decrsat;
      break;
   case GL_INCR_WRAP:
      stencilctl |= SC_sfailop_incr;
      break;
   case GL_DECR_WRAP:
      stencilctl |= SC_sfailop_decr;
      break;
   case GL_INVERT:
      stencilctl |= SC_sfailop_invert;
      break;
   default:
      break;
   }

   switch (ctx->Stencil.ZFailFunc[0])
   {
   case GL_KEEP:
      stencilctl |= SC_szfailop_keep;
      break;
   case GL_ZERO:
      stencilctl |= SC_szfailop_zero;
      break;
   case GL_REPLACE:
      stencilctl |= SC_szfailop_replace;
      break;
   case GL_INCR:
      stencilctl |= SC_szfailop_incrsat;
      break;
   case GL_DECR:
      stencilctl |= SC_szfailop_decrsat;
      break;
   case GL_INCR_WRAP:
      stencilctl |= SC_szfailop_incr;
      break;
   case GL_DECR_WRAP:
      stencilctl |= SC_szfailop_decr;
      break;
   case GL_INVERT:
      stencilctl |= SC_szfailop_invert;
      break;
   default:
      break;
   }

   switch (ctx->Stencil.ZPassFunc[0])
   {
   case GL_KEEP:
      stencilctl |= SC_szpassop_keep;
      break;
   case GL_ZERO:
      stencilctl |= SC_szpassop_zero;
      break;
   case GL_REPLACE:
      stencilctl |= SC_szpassop_replace;
      break;
   case GL_INCR:
      stencilctl |= SC_szpassop_incrsat;
      break;
   case GL_DECR:
      stencilctl |= SC_szpassop_decrsat;
      break;
   case GL_INVERT:
      stencilctl |= SC_szpassop_invert;
      break;
   default:
      break;
   }

   FLUSH_BATCH( mmesa );
   mmesa->hw.stencilctl &= (SC_sfailop_MASK & SC_szfailop_MASK 
			    & SC_szpassop_MASK);
   mmesa->hw.stencilctl |= stencilctl;
   mmesa->dirty |= MGA_UPLOAD_CONTEXT;
}


/* =============================================================
 * Window position and viewport transformation
 */

void mgaCalcViewport( GLcontext *ctx )
{
   mgaContextPtr mmesa = MGA_CONTEXT(ctx);
   const GLfloat *v = ctx->Viewport._WindowMap.m;
   GLfloat *m = mmesa->hw_viewport;

   /* See also mga_translate_vertex.
    */
   m[MAT_SX] =   v[MAT_SX];
   m[MAT_TX] =   v[MAT_TX] + mmesa->drawX + SUBPIXEL_X;
   m[MAT_SY] = - v[MAT_SY];
   m[MAT_TY] = - v[MAT_TY] + mmesa->driDrawable->h + mmesa->drawY + SUBPIXEL_Y;
   m[MAT_SZ] =   v[MAT_SZ] * mmesa->depth_scale;
   m[MAT_TZ] =   v[MAT_TZ] * mmesa->depth_scale;

   mmesa->SetupNewInputs = ~0;
}

static void mgaViewport( GLcontext *ctx, 
			  GLint x, GLint y, 
			  GLsizei width, GLsizei height )
{
   mgaCalcViewport( ctx );
}

static void mgaDepthRange( GLcontext *ctx, 
			    GLclampd nearval, GLclampd farval )
{
   mgaCalcViewport( ctx );
}


/* =============================================================
 * Miscellaneous
 */

static void mgaDDClearColor(GLcontext *ctx, 
			    const GLfloat color[4] )
{
   mgaContextPtr mmesa = MGA_CONTEXT(ctx);
   GLubyte c[4];
   CLAMPED_FLOAT_TO_UBYTE(c[0], color[0]);
   CLAMPED_FLOAT_TO_UBYTE(c[1], color[1]);
   CLAMPED_FLOAT_TO_UBYTE(c[2], color[2]);
   CLAMPED_FLOAT_TO_UBYTE(c[3], color[3]);

   mmesa->ClearColor = mgaPackColor( mmesa->driDrawable->cpp,
				     c[0], c[1], c[2], c[3]);
}


/* Fallback to swrast for select and feedback.
 */
static void mgaRenderMode( GLcontext *ctx, GLenum mode )
{
   FALLBACK( ctx, MGA_FALLBACK_RENDERMODE, (mode != GL_RENDER) );
}


static void mgaDDLogicOp( GLcontext *ctx, GLenum opcode )
{
   mgaContextPtr mmesa = MGA_CONTEXT( ctx );

   FLUSH_BATCH( mmesa );
#if defined(ACCEL_ROP)
   mmesa->hw.rop = mgarop_NoBLK[ opcode & 0x0f ];
   mmesa->dirty |= MGA_UPLOAD_CONTEXT;
#else
   FALLBACK( ctx, MGA_FALLBACK_LOGICOP,
	     (ctx->Color.ColorLogicOpEnabled && opcode != GL_COPY) );
#endif
}


static void mgaXMesaSetFrontClipRects( mgaContextPtr mmesa )
{
   __DRIdrawablePrivate *driDrawable = mmesa->driDrawable;

   if (driDrawable->numClipRects == 0) {
       static XF86DRIClipRectRec zeroareacliprect = {0,0,0,0};
       mmesa->numClipRects = 1;
       mmesa->pClipRects = &zeroareacliprect;
   } else {
       mmesa->numClipRects = driDrawable->numClipRects;
       mmesa->pClipRects = driDrawable->pClipRects;
   }
   mmesa->drawX = driDrawable->x;
   mmesa->drawY = driDrawable->y;

   mmesa->dirty |= MGA_UPLOAD_CLIPRECTS;
}


static void mgaXMesaSetBackClipRects( mgaContextPtr mmesa )
{
   __DRIdrawablePrivate *driDrawable = mmesa->driDrawable;

   if (driDrawable->numBackClipRects == 0)
   {
      if (driDrawable->numClipRects == 0) {
	  static XF86DRIClipRectRec zeroareacliprect = {0,0,0,0};
	  mmesa->numClipRects = 1;
	  mmesa->pClipRects = &zeroareacliprect;
      } else {
	  mmesa->numClipRects = driDrawable->numClipRects;
	  mmesa->pClipRects = driDrawable->pClipRects;
      }
      mmesa->drawX = driDrawable->x;
      mmesa->drawY = driDrawable->y;
   } else {
      mmesa->numClipRects = driDrawable->numBackClipRects;
      mmesa->pClipRects = driDrawable->pBackClipRects;
      mmesa->drawX = driDrawable->backX;
      mmesa->drawY = driDrawable->backY;
   }

   mmesa->dirty |= MGA_UPLOAD_CLIPRECTS;
}

static void mgaUpdateBuffers(mgaContextPtr mmesa)
{
   __DRIdrawablePrivate *driDrawable = mmesa->driDrawable;

   mmesa->setup.fb_cpp       = driDrawable->cpp;

   mmesa->setup.front_pitch  = driDrawable->frontPitch / driDrawable->cpp;
   mmesa->setup.front_offset = driDrawable->frontOffset;

   mmesa->setup.back_pitch   = driDrawable->backPitch / driDrawable->cpp;
   mmesa->setup.back_offset  = driDrawable->backOffset;

   switch (mmesa->draw_buffer) {
      case MGA_FRONT:
         mmesa->drawOffset  = driDrawable->frontOffset;
         mmesa->readOffset  = driDrawable->frontOffset;

         mmesa->setup.draw_pitch  = mmesa->setup.front_pitch;
         mmesa->setup.draw_offset = mmesa->setup.front_offset;
         break;
      case MGA_BACK:
         mmesa->drawOffset  = driDrawable->backOffset;
         mmesa->readOffset  = driDrawable->backOffset;

         mmesa->setup.draw_pitch  = mmesa->setup.back_pitch;
         mmesa->setup.draw_offset = mmesa->setup.back_offset;
         break;
      default:
         break;
   }

   mmesa->setup.depth_cpp    = driDrawable->depthCpp;

   mmesa->setup.depth_pitch  = driDrawable->depthPitch / driDrawable->depthCpp;
   mmesa->setup.depth_offset = driDrawable->depthOffset;
   
   mmesa->setup.maccess = (MA_memreset_disable |
                           MA_fogen_disable |
                           MA_tlutload_disable |
                           MA_nodither_disable |
                           MA_dit555_disable);

   switch (driDrawable->cpp) {
      case 2:
         mmesa->setup.maccess |= MA_pwidth_16;
         break;
      case 4:
         mmesa->setup.maccess |= MA_pwidth_32;
         break;
      default:
         fprintf( stderr, "Error: unknown cpp %d, exiting...\n",
                  driDrawable->cpp );
   }

   switch (mmesa->glCtx->Visual.depthBits) {
      case 16:
         mmesa->setup.maccess |= MA_zwidth_16;
         break;
      case 24:
         mmesa->setup.maccess |= MA_zwidth_24;
         break;
      case 32:
         mmesa->setup.maccess |= MA_pwidth_32;
         break;
   }

   if (mmesa->glCtx->Fog.Enabled) 
      mmesa->setup.maccess |= MA_fogen_enable;
   
   mmesa->dirty |= MGA_UPLOAD_CONTEXT;
}

void mgaUpdateRects( mgaContextPtr mmesa, GLuint buffers )
{
   __DRIdrawablePrivate *driDrawable = mmesa->driDrawable;
   MGASAREAPrivPtr sarea = mmesa->sarea;


   DRI_VALIDATE_DRAWABLE_INFO(mmesa->driScreen, driDrawable); 
   mmesa->dirty_cliprects = 0;	

   mgaUpdateBuffers( mmesa );

   if (mmesa->draw_buffer == MGA_FRONT)
      mgaXMesaSetFrontClipRects( mmesa );
   else
      mgaXMesaSetBackClipRects( mmesa );

   sarea->req_drawable = (unsigned int)driDrawable;//dok ->draw;
   sarea->req_draw_buffer = mmesa->draw_buffer;

   mgaUpdateClipping( mmesa->glCtx );
   mgaCalcViewport( mmesa->glCtx );

   mmesa->dirty |= MGA_UPLOAD_CLIPRECTS;
}


static void mgaDDDrawBuffer(GLcontext *ctx, GLenum mode )
{
   mgaContextPtr mmesa = MGA_CONTEXT(ctx);

   FLUSH_BATCH( mmesa );

   /*
    * _DrawDestMask is easier to cope with than <mode>.
    */
   switch ( ctx->Color._DrawDestMask ) {
   case FRONT_LEFT_BIT:
      mmesa->draw_buffer = MGA_FRONT;
      mgaXMesaSetFrontClipRects( mmesa );
      FALLBACK( ctx, MGA_FALLBACK_DRAW_BUFFER, GL_FALSE );
      break;
   case BACK_LEFT_BIT:
      mmesa->draw_buffer = MGA_BACK;
      mgaXMesaSetBackClipRects( mmesa );
      FALLBACK( ctx, MGA_FALLBACK_DRAW_BUFFER, GL_FALSE );
      break;
   default:
      /* GL_NONE or GL_FRONT_AND_BACK or stereo left&right, etc */
      FALLBACK( ctx, MGA_FALLBACK_DRAW_BUFFER, GL_TRUE );
      return;
   }

   mgaUpdateBuffers( mmesa );

   if (mmesa->draw_buffer == MGA_FRONT)
      mgaXMesaSetFrontClipRects( mmesa );
   else
      mgaXMesaSetBackClipRects( mmesa );
   
   /* We want to update the s/w rast state too so that r200SetBuffer()
    * gets called.
    */
   _swrast_DrawBuffer(ctx, mode);
}


static void mgaDDReadBuffer(GLcontext *ctx, GLenum mode )
{
   /* nothing, until we implement h/w glRead/CopyPixels or CopyTexImage */
}


/* =============================================================
 * State enable/disable
 */


static void mgaDDEnable(GLcontext *ctx, GLenum cap, GLboolean state)
{
   mgaContextPtr mmesa = MGA_CONTEXT( ctx );

   switch(cap) {
   case GL_ALPHA_TEST:
      FLUSH_BATCH( mmesa );
      mmesa->hw.alpha_func_enable = (state) ? ~0 : 0;
      break;
   case GL_BLEND:
      FLUSH_BATCH( mmesa );
      mmesa->hw.blend_func_enable = (state) ? ~0 : 0;

      /* For some reason enable(GL_BLEND) affects ColorLogicOpEnabled.
       */
      FALLBACK( ctx, MGA_FALLBACK_LOGICOP,
		(ctx->Color.ColorLogicOpEnabled && 
		 ctx->Color.LogicOp != GL_COPY));
      break;
   case GL_DEPTH_TEST:
      FLUSH_BATCH( mmesa );
      FALLBACK (ctx, MGA_FALLBACK_DEPTH,
		ctx->Depth.Func == GL_NEVER && ctx->Depth.Test);
      break;

   case GL_SCISSOR_TEST:
      FLUSH_BATCH( mmesa );
      mmesa->scissor = state;
      mgaUpdateClipping( ctx );
      break;

   case GL_FOG:
      MGA_STATECHANGE( mmesa, MGA_UPLOAD_CONTEXT );
      if (ctx->Fog.Enabled) 
	 mmesa->setup.maccess |= MA_fogen_enable;
      else
	 mmesa->setup.maccess &= ~MA_fogen_enable;
      
      mgaChooseVertexState( ctx );
      break;
   case GL_CULL_FACE:
      mgaDDCullFaceFrontFace( ctx, 0 );
      break;
   case GL_TEXTURE_1D:
   case GL_TEXTURE_2D:
   case GL_TEXTURE_3D:
      break;
   case GL_POLYGON_STIPPLE:
      if (mmesa->haveHwStipple && mmesa->raster_primitive == GL_TRIANGLES) {
	 FLUSH_BATCH(mmesa);
	 mmesa->dirty |= MGA_UPLOAD_CONTEXT;
	 mmesa->setup.dwgctl &= ~(0xf<<20);
	 if (state)
	    mmesa->setup.dwgctl |= mmesa->poly_stipple;
      }
      break;
   case GL_COLOR_LOGIC_OP:
      FLUSH_BATCH( mmesa );
#if !defined(ACCEL_ROP)
      FALLBACK( ctx, MGA_FALLBACK_LOGICOP, 
		(state && ctx->Color.LogicOp != GL_COPY));
#endif
      break;
   case GL_STENCIL_TEST:
      FLUSH_BATCH( mmesa );
      if (mmesa->hw_stencil) {
	 mmesa->hw.stencil_enable = ( state ) ? ~0 : 0;
      }
      else {
	 FALLBACK( ctx, MGA_FALLBACK_STENCIL, state );
      }
   default:
      break;
   }
}


/* =============================================================
 */

static void mgaDDPrintDirty( const char *msg, GLuint state )
{
   fprintf(stderr, "%s (0x%03x): %s%s%s%s%s%s%s\n",
	   msg,
	   (unsigned int) state,
	   (state & MGA_WAIT_AGE)          ? "wait-age " : "",
	   (state & MGA_UPLOAD_TEX0IMAGE)  ? "upload-tex0-img " : "",
	   (state & MGA_UPLOAD_TEX1IMAGE)  ? "upload-tex1-img " : "",
	   (state & MGA_UPLOAD_CONTEXT)    ? "upload-ctx " : "",
	   (state & MGA_UPLOAD_TEX0)       ? "upload-tex0 " : "",
	   (state & MGA_UPLOAD_TEX1)       ? "upload-tex1 " : "",
	   (state & MGA_UPLOAD_PIPE)       ? "upload-pipe " : ""
      );
}

/* Push the state into the sarea and/or texture memory.
 */
void mgaEmitHwStateLocked( mgaContextPtr mmesa )
{
   MGASAREAPrivPtr sarea = mmesa->sarea;
   GLcontext * ctx = mmesa->glCtx;

   if (MGA_DEBUG & DEBUG_VERBOSE_MSG)
      mgaDDPrintDirty( __FUNCTION__, mmesa->dirty );

   if (mmesa->dirty & MGA_UPLOAD_CONTEXT) {
      mmesa->setup.wflag = _CULL_DISABLE;
      if (mmesa->raster_primitive == GL_TRIANGLES) {
	 if ((ctx->Texture.Unit[0]._ReallyEnabled == TEXTURE_2D_BIT &&
	      ctx->Texture.Unit[1]._ReallyEnabled == TEXTURE_2D_BIT)) {
	    mmesa->setup.wflag = mmesa->hw.cull_dualtex;
	 }
	 else {
	    mmesa->setup.wflag = mmesa->hw.cull;
	 }
      }

      mmesa->setup.stencil = mmesa->hw.stencil 
	  & mmesa->hw.stencil_enable;
      mmesa->setup.stencilctl = mmesa->hw.stencilctl
	  & mmesa->hw.stencil_enable;

      /* If depth testing is not enabled, then use the no Z-compare / no
       * Z-write mode.  Otherwise, use whatever is set in hw.zmode.
       */
      mmesa->setup.dwgctl &= (DC_zmode_MASK & DC_atype_MASK);
      mmesa->setup.dwgctl |= (ctx->Depth.Test)
	  ? mmesa->hw.zmode : (DC_zmode_nozcmp | DC_atype_i);

#if defined(ACCEL_ROP)
      mmesa->setup.dwgctl &= DC_bop_MASK;
      mmesa->setup.dwgctl |= (ctx->Color.ColorLogicOpEnabled)
	  ? mmesa->hw.rop : mgarop_NoBLK[ GL_COPY & 0x0f ];
#endif

      mmesa->setup.alphactrl &= AC_src_MASK & AC_dst_MASK & AC_atmode_MASK
	  & AC_atref_MASK;
      mmesa->setup.alphactrl |= 
	  ((mmesa->hw.alpha_func & mmesa->hw.alpha_func_enable)
	   | ((mmesa->hw.blend_func & mmesa->hw.blend_func_enable)
	      | ((AC_src_one | AC_dst_zero) & ~mmesa->hw.blend_func_enable))
	   | mmesa->hw.alpha_sel
	   | (AC_amode_alpha_channel
	      | AC_astipple_disable 
	      | AC_aten_disable 
	      | AC_atmode_noacmp));

      memcpy( &sarea->ContextState, &mmesa->setup, sizeof(mmesa->setup));
   }

   if ((mmesa->dirty & MGA_UPLOAD_TEX0) && mmesa->CurrentTexObj[0]) {
      mmesa->CurrentTexObj[0]->setup.texctl2 &= ~TMC_specen_enable;
      mmesa->CurrentTexObj[0]->setup.texctl2 |= mmesa->hw.specen;

      memcpy(&sarea->TexState[0],
	     &mmesa->CurrentTexObj[0]->setup,
	     sizeof(sarea->TexState[0]));
   }

   if ((mmesa->dirty & MGA_UPLOAD_TEX1) && mmesa->CurrentTexObj[1]) {
      mmesa->CurrentTexObj[1]->setup.texctl2 &= ~TMC_specen_enable;
      mmesa->CurrentTexObj[1]->setup.texctl2 |= mmesa->hw.specen;

      memcpy(&sarea->TexState[1],
	     &mmesa->CurrentTexObj[1]->setup,
	     sizeof(sarea->TexState[1]));
   }

   if (sarea->TexState[0].texctl2 !=
       sarea->TexState[1].texctl2) {
      memcpy(&sarea->TexState[1],
	     &sarea->TexState[0],
	     sizeof(sarea->TexState[0]));
      mmesa->dirty |= MGA_UPLOAD_TEX1|MGA_UPLOAD_TEX0;
   }

   if (mmesa->dirty & MGA_UPLOAD_PIPE) {
/*        mmesa->sarea->wacceptseq = mmesa->hw_primitive; */
      mmesa->sarea->WarpPipe = mmesa->vertex_format;
      mmesa->sarea->vertsize = mmesa->vertex_size;
   }

   mmesa->sarea->dirty |= mmesa->dirty;
   mmesa->dirty &= MGA_UPLOAD_CLIPRECTS;

   /* This is a bit of a hack but seems to be the best place to ensure
    * that separate specular is disabled when not needed.
    */
   if (ctx->Texture._EnabledUnits == 0 ||
       !ctx->Light.Enabled ||
       ctx->Light.Model.ColorControl == GL_SINGLE_COLOR) {
      sarea->TexState[0].texctl2 &= ~TMC_specen_enable;
      sarea->TexState[1].texctl2 &= ~TMC_specen_enable;
   }
}


/* =============================================================
 */


static void mgaDDValidateState( GLcontext *ctx )
{
   mgaContextPtr mmesa = MGA_CONTEXT( ctx );
   int new_state = mmesa->NewGLState;


   FLUSH_BATCH( mmesa );

   if (mmesa->NewGLState & _MGA_NEW_RASTERSETUP) {
      mgaChooseVertexState( ctx );
   }

   if (mmesa->NewGLState & _MGA_NEW_RENDERSTATE) {
      mgaChooseRenderState( ctx );
   }

   if (new_state & _NEW_TEXTURE) {
      mgaUpdateTextureState(ctx);
   }

   mmesa->NewGLState = 0;
}


static void mgaDDInvalidateState( GLcontext *ctx, GLuint new_state )
{
   _swrast_InvalidateState( ctx, new_state );
   _swsetup_InvalidateState( ctx, new_state );
   _ac_InvalidateState( ctx, new_state );
   _tnl_InvalidateState( ctx, new_state );
   MGA_CONTEXT(ctx)->NewGLState |= new_state;
}


static void mgaRunPipeline( GLcontext *ctx )
{
   mgaContextPtr mmesa = MGA_CONTEXT(ctx);

   if (mmesa->NewGLState) {
      mgaDDValidateState( ctx );
   }

   if (mmesa->dirty) {
       mgaEmitHwStateLocked( mmesa );
   }

   _tnl_run_pipeline( ctx );
}


void mgaInitState( mgaContextPtr mmesa )
{
   GLcontext *ctx = mmesa->glCtx;

   if (ctx->Visual.doubleBufferMode) {
      /* use back buffer by default */
      mmesa->draw_buffer = MGA_BACK;
   } else {
      /* use front buffer by default */
      mmesa->draw_buffer = MGA_FRONT;
   }

   mmesa->hw.zmode = DC_zmode_zlt | DC_atype_zi;
   mmesa->hw.stencil = (0x0ff << S_smsk_SHIFT) | (0x0ff << S_swtmsk_SHIFT);
   mmesa->hw.stencilctl = SC_smode_salways | SC_sfailop_keep 
       | SC_szfailop_keep | SC_szpassop_keep;
   mmesa->hw.stencil_enable = 0;
/*dok   mmesa->hw.cull = _CULL_NEGATIVE;
   mmesa->hw.cull_dualtex = _CULL_POSITIVE;*/
   mmesa->hw.specen = 0;

   mmesa->setup.dwgctl = (DC_opcod_trap |
			  DC_linear_xy |
			  DC_solid_disable |
			  DC_arzero_disable |
			  DC_sgnzero_disable |
			  DC_shftzero_enable |
			  (0xC << DC_bop_SHIFT) |
			  (0x0 << DC_trans_SHIFT) |
			  DC_bltmod_bmonolef |
			  DC_pattern_disable |
			  DC_transc_disable |
			  DC_clipdis_disable);


   mmesa->setup.plnwt = ~0;
   mmesa->setup.alphactrl = ( AC_src_one |
			      AC_dst_zero |
			      AC_amode_FCOL |
			      AC_astipple_disable |
			      AC_aten_disable |
			      AC_atmode_noacmp |
			      AC_alphasel_fromtex );

   mmesa->setup.fogcolor = PACK_COLOR_888((GLubyte)(ctx->Fog.Color[0]*255.0F),
					  (GLubyte)(ctx->Fog.Color[1]*255.0F),
					  (GLubyte)(ctx->Fog.Color[2]*255.0F));

   mmesa->setup.wflag = 0;
   mmesa->setup.tdualstage0 = 0;
   mmesa->setup.tdualstage1 = 0;
   mmesa->setup.fcol = 0;
   mmesa->dirty |= MGA_UPLOAD_CONTEXT;
}


void mgaDDInitStateFuncs( GLcontext *ctx )
{
   ctx->Driver.UpdateState = mgaDDInvalidateState;
   ctx->Driver.Enable = mgaDDEnable;
   ctx->Driver.LightModelfv = mgaDDLightModelfv;
   ctx->Driver.AlphaFunc = mgaDDAlphaFunc;
   ctx->Driver.BlendEquation = mgaDDBlendEquation;
   ctx->Driver.BlendFunc = mgaDDBlendFunc;
   ctx->Driver.BlendFuncSeparate = mgaDDBlendFuncSeparate;
   ctx->Driver.DepthFunc = mgaDDDepthFunc;
   ctx->Driver.DepthMask = mgaDDDepthMask;
   ctx->Driver.Fogfv = mgaDDFogfv;
   ctx->Driver.Scissor = mgaDDScissor;
   ctx->Driver.ShadeModel = mgaDDShadeModel;
   ctx->Driver.CullFace = mgaDDCullFaceFrontFace;
   ctx->Driver.FrontFace = mgaDDCullFaceFrontFace;
   ctx->Driver.ColorMask = mgaDDColorMask;

   ctx->Driver.DrawBuffer = mgaDDDrawBuffer;
   ctx->Driver.ReadBuffer = mgaDDReadBuffer;
   ctx->Driver.ClearColor = mgaDDClearColor;
   ctx->Driver.ClearDepth = mgaDDClearDepth;
   ctx->Driver.LogicOpcode = mgaDDLogicOp;

   ctx->Driver.PolygonStipple = mgaDDPolygonStipple;

   ctx->Driver.StencilFunc = mgaDDStencilFunc;
   ctx->Driver.StencilMask = mgaDDStencilMask;
   ctx->Driver.StencilOp = mgaDDStencilOp;

   ctx->Driver.DepthRange = mgaDepthRange;
   ctx->Driver.Viewport = mgaViewport;
   ctx->Driver.RenderMode = mgaRenderMode;

   ctx->Driver.ClearIndex = 0;
   ctx->Driver.IndexMask = 0;

   /* Swrast hooks for imaging extensions:
    */
   ctx->Driver.CopyColorTable = _swrast_CopyColorTable;
   ctx->Driver.CopyColorSubTable = _swrast_CopyColorSubTable;
   ctx->Driver.CopyConvolutionFilter1D = _swrast_CopyConvolutionFilter1D;
   ctx->Driver.CopyConvolutionFilter2D = _swrast_CopyConvolutionFilter2D;

   TNL_CONTEXT(ctx)->Driver.RunPipeline = mgaRunPipeline;
}
