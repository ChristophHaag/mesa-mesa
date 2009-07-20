/*
 * Copyright (C) 2008-2009  Advanced Micro Devices, Inc.
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
 * THE COPYRIGHT HOLDER(S) BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER IN
 * AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN
 * CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.
 */

/*
 * Authors:
 *   Richard Li <RichardZ.Li@amd.com>, <richardradeon@gmail.com>
 */

#include "main/glheader.h"
#include "main/mtypes.h"
#include "main/state.h"
#include "main/imports.h"
#include "main/enums.h"
#include "main/macros.h"
#include "main/dd.h"
#include "main/simple_list.h"

#include "tnl/tnl.h"
#include "tnl/t_pipeline.h"
#include "tnl/t_vp_build.h"
#include "swrast/swrast.h"
#include "swrast_setup/swrast_setup.h"
#include "main/api_arrayelt.h"
#include "main/state.h"
#include "main/framebuffer.h"

#include "shader/prog_parameter.h"
#include "shader/prog_statevars.h"
#include "vbo/vbo.h"
#include "main/texformat.h"

#include "r600_context.h"

#include "r700_state.h"

#include "r700_fragprog.h"
#include "r700_vertprog.h"


void r700SetDefaultStates(context_t *context) //--------------------
{
    
}

void r700UpdateShaders (GLcontext * ctx)  //----------------------------------
{
    context_t *context = R700_CONTEXT(ctx);

    GLvector4f dummy_attrib[_TNL_ATTRIB_MAX];
    GLvector4f *temp_attrib[_TNL_ATTRIB_MAX];

    struct r700_vertex_program *vp;
	int i;

    if (context->radeon.NewGLState) 
    {
        context->radeon.NewGLState = 0;

        for (i = _TNL_FIRST_MAT; i <= _TNL_LAST_MAT; i++) 
        {
            /* mat states from state var not array for sw */
            dummy_attrib[i].stride = 0;

            temp_attrib[i] = TNL_CONTEXT(ctx)->vb.AttribPtr[i];
            TNL_CONTEXT(ctx)->vb.AttribPtr[i] = &(dummy_attrib[i]);
        }

        _tnl_UpdateFixedFunctionProgram(ctx);

        for (i = _TNL_FIRST_MAT; i <= _TNL_LAST_MAT; i++) 
        {
            TNL_CONTEXT(ctx)->vb.AttribPtr[i] = temp_attrib[i];
        }

        r700SelectVertexShader(ctx);
        vp = (struct r700_vertex_program *)ctx->VertexProgram._Current;

        if (vp->translated == GL_FALSE) 
        {
            // TODO
            //fprintf(stderr, "Failing back to sw-tcl\n");
            //hw_tcl_on = future_hw_tcl_on = 0;
            //r300ResetHwState(rmesa);
            //
            r700UpdateStateParameters(ctx, _NEW_PROGRAM);
            return;
        }
    }

    r700UpdateStateParameters(ctx, _NEW_PROGRAM);
}

/*
 * To correctly position primitives:
 */
void r700UpdateViewportOffset(GLcontext * ctx) //------------------
{

	//radeonUpdateScissor(ctx);

    return;
}

/**
 * Tell the card where to render (offset, pitch).
 * Effected by glDrawBuffer, etc
 */
void r700UpdateDrawBuffer(GLcontext * ctx) /* TODO */ //---------------------
{
#if 0 /* to be enabled */
    context_t *context = R700_CONTEXT(ctx);

    switch (ctx->DrawBuffer->_ColorDrawBufferIndexes[0]) 
    {
	case BUFFER_FRONT_LEFT:
	    context->target.rt = context->screen->frontBuffer;
	    break;
	case BUFFER_BACK_LEFT:
	    context->target.rt = context->screen->backBuffer;
	    break;
	default:
	    memset (&context->target.rt, sizeof(context->target.rt), 0);
	}
#endif /* to be enabled */
}

static void r700FetchStateParameter(GLcontext * ctx,
			                        const gl_state_index state[STATE_LENGTH],
			                        GLfloat * value)
{
	context_t *context = R700_CONTEXT(ctx);

    /* TODO */
}

void r700UpdateStateParameters(GLcontext * ctx, GLuint new_state) //--------------------
{
	struct r700_fragment_program *fp;
	struct gl_program_parameter_list *paramList;
	GLuint i;

	if (!(new_state & (_NEW_BUFFERS | _NEW_PROGRAM)))
		return;

	fp = (struct r700_fragment_program *)ctx->FragmentProgram._Current;
	if (!fp)
    {
		return;
    }

	paramList = fp->mesa_program.Base.Parameters;

	if (!paramList)
    {
		return;
    }

	for (i = 0; i < paramList->NumParameters; i++) 
    {
		if (paramList->Parameters[i].Type == PROGRAM_STATE_VAR) 
        {
			r700FetchStateParameter(ctx,
						paramList->Parameters[i].
						StateIndexes,
						paramList->ParameterValues[i]);
		}
	}
}

/**
 * Called by Mesa after an internal state update.
 */
static void r700InvalidateState(GLcontext * ctx, GLuint new_state) //-------------------
{
    context_t *context = R700_CONTEXT(ctx);

    R700_CHIP_CONTEXT *r700 = (R700_CHIP_CONTEXT*)(&context->hw);

    _swrast_InvalidateState(ctx, new_state);
	_swsetup_InvalidateState(ctx, new_state);
	_vbo_InvalidateState(ctx, new_state);
	_tnl_InvalidateState(ctx, new_state);
	_ae_invalidate_state(ctx, new_state);

	if (new_state & (_NEW_BUFFERS | _NEW_COLOR | _NEW_PIXEL)) 
    {
        _mesa_update_framebuffer(ctx);
		/* this updates the DrawBuffer's Width/Height if it's a FBO */
		_mesa_update_draw_buffer_bounds(ctx);

		r700UpdateDrawBuffer(ctx);
	}

	r700UpdateStateParameters(ctx, new_state);

    if(GL_TRUE == r700->bEnablePerspective)
    {
        /* Do scale XY and Z by 1/W0 for perspective correction on pos. For orthogonal case, set both to one. */
        CLEARbit(r700->PA_CL_VTE_CNTL.u32All, VTX_XY_FMT_bit);
        CLEARbit(r700->PA_CL_VTE_CNTL.u32All, VTX_Z_FMT_bit);

        SETbit(r700->PA_CL_VTE_CNTL.u32All, VTX_W0_FMT_bit);

        SETbit(r700->SPI_PS_IN_CONTROL_0.u32All, PERSP_GRADIENT_ENA_bit);
        CLEARbit(r700->SPI_PS_IN_CONTROL_0.u32All, LINEAR_GRADIENT_ENA_bit);
    }
    else
    {
        /* For orthogonal case. */
        SETbit(r700->PA_CL_VTE_CNTL.u32All, VTX_XY_FMT_bit);
        SETbit(r700->PA_CL_VTE_CNTL.u32All, VTX_Z_FMT_bit);

        SETbit(r700->PA_CL_VTE_CNTL.u32All, VTX_W0_FMT_bit);

        CLEARbit(r700->SPI_PS_IN_CONTROL_0.u32All, PERSP_GRADIENT_ENA_bit);
        SETbit(r700->SPI_PS_IN_CONTROL_0.u32All, LINEAR_GRADIENT_ENA_bit);
    }

	context->radeon.NewGLState |= new_state;
}

static void r700SetDepthState(GLcontext * ctx)
{
	context_t *context = R700_CONTEXT(ctx);

    R700_CHIP_CONTEXT *r700 = (R700_CHIP_CONTEXT*)(&context->hw);

    if (ctx->Depth.Test)
    {
        SETbit(r700->DB_DEPTH_CONTROL.u32All, Z_ENABLE_bit);
        if (ctx->Depth.Mask)
        {
            SETbit(r700->DB_DEPTH_CONTROL.u32All, Z_WRITE_ENABLE_bit);
        }
        else
        {
            CLEARbit(r700->DB_DEPTH_CONTROL.u32All, Z_WRITE_ENABLE_bit);
        }

        switch (ctx->Depth.Func)
        {
        case GL_NEVER:            
            SETfield(r700->DB_DEPTH_CONTROL.u32All, FRAG_NEVER, 
                     ZFUNC_shift, ZFUNC_mask);
            break;
        case GL_LESS:
            SETfield(r700->DB_DEPTH_CONTROL.u32All, FRAG_LESS, 
                     ZFUNC_shift, ZFUNC_mask);            
            break;
        case GL_EQUAL:
            SETfield(r700->DB_DEPTH_CONTROL.u32All, FRAG_EQUAL, 
                     ZFUNC_shift, ZFUNC_mask);
            break;
        case GL_LEQUAL:
            SETfield(r700->DB_DEPTH_CONTROL.u32All, FRAG_LEQUAL,  
                     ZFUNC_shift, ZFUNC_mask);
            break;
        case GL_GREATER:
            SETfield(r700->DB_DEPTH_CONTROL.u32All, FRAG_GREATER,  
                     ZFUNC_shift, ZFUNC_mask);           
            break;
        case GL_NOTEQUAL:
            SETfield(r700->DB_DEPTH_CONTROL.u32All, FRAG_NOTEQUAL,  
                     ZFUNC_shift, ZFUNC_mask);
            break;
        case GL_GEQUAL:
            SETfield(r700->DB_DEPTH_CONTROL.u32All, FRAG_GEQUAL,  
                     ZFUNC_shift, ZFUNC_mask);
            break;
        case GL_ALWAYS:
            SETfield(r700->DB_DEPTH_CONTROL.u32All, FRAG_ALWAYS,  
                     ZFUNC_shift, ZFUNC_mask);
            break;
        default:
            SETfield(r700->DB_DEPTH_CONTROL.u32All, FRAG_ALWAYS,  
                     ZFUNC_shift, ZFUNC_mask);
            break;
        }
    }
    else
    {
        CLEARbit(r700->DB_DEPTH_CONTROL.u32All, Z_ENABLE_bit);
        CLEARbit(r700->DB_DEPTH_CONTROL.u32All, Z_WRITE_ENABLE_bit);
    }
}

static void r700AlphaFunc(GLcontext * ctx, GLenum func, GLfloat ref) //---------------
{
}


static void r700BlendColor(GLcontext * ctx, const GLfloat cf[4]) //----------------
{
}

static void r700BlendEquationSeparate(GLcontext * ctx,
				                      GLenum modeRGB, GLenum modeA) //-----------------
{
}

static void r700BlendFuncSeparate(GLcontext * ctx,
				  GLenum sfactorRGB, GLenum dfactorRGB,
				  GLenum sfactorA, GLenum dfactorA) //------------------------
{
}

static void r700UpdateCulling(GLcontext * ctx)
{
    R700_CHIP_CONTEXT *r700 = (R700_CHIP_CONTEXT*)(&R700_CONTEXT(ctx)->hw);

    CLEARbit(r700->PA_SU_SC_MODE_CNTL.u32All, FACE_bit);
    CLEARbit(r700->PA_SU_SC_MODE_CNTL.u32All, CULL_FRONT_bit);
    CLEARbit(r700->PA_SU_SC_MODE_CNTL.u32All, CULL_BACK_bit);

    if (ctx->Polygon.CullFlag) 
    {
        switch (ctx->Polygon.CullFaceMode) 
        {
        case GL_FRONT:            
            SETbit(r700->PA_SU_SC_MODE_CNTL.u32All, CULL_FRONT_bit);
            CLEARbit(r700->PA_SU_SC_MODE_CNTL.u32All, CULL_BACK_bit);
            break;
        case GL_BACK:
            CLEARbit(r700->PA_SU_SC_MODE_CNTL.u32All, CULL_FRONT_bit);
            SETbit(r700->PA_SU_SC_MODE_CNTL.u32All, CULL_BACK_bit);
            break;
        case GL_FRONT_AND_BACK:
            SETbit(r700->PA_SU_SC_MODE_CNTL.u32All, CULL_FRONT_bit);
            SETbit(r700->PA_SU_SC_MODE_CNTL.u32All, CULL_BACK_bit);
            break;
        default:
            CLEARbit(r700->PA_SU_SC_MODE_CNTL.u32All, CULL_FRONT_bit);
            CLEARbit(r700->PA_SU_SC_MODE_CNTL.u32All, CULL_BACK_bit);
            break;
        }
    }

    switch (ctx->Polygon.FrontFace) 
    {
        case GL_CW:
            SETbit(r700->PA_SU_SC_MODE_CNTL.u32All, FACE_bit);
            break;
        case GL_CCW:
            CLEARbit(r700->PA_SU_SC_MODE_CNTL.u32All, FACE_bit); 
            break;
        default:
            CLEARbit(r700->PA_SU_SC_MODE_CNTL.u32All, FACE_bit); /* default: ccw */
            break;
    }
}

static void r700UpdateLineStipple(GLcontext * ctx)
{
    R700_CHIP_CONTEXT *r700 = (R700_CHIP_CONTEXT*)(&R700_CONTEXT(ctx)->hw);
    if (ctx->Line.StippleFlag)
    {
	SETbit(r700->PA_SC_MODE_CNTL.u32All, LINE_STIPPLE_ENABLE_bit);
    }
    else
    {
	CLEARbit(r700->PA_SC_MODE_CNTL.u32All, LINE_STIPPLE_ENABLE_bit);
    }
}

static void r700Enable(GLcontext * ctx, GLenum cap, GLboolean state) //------------------
{
	context_t *context = R700_CONTEXT(ctx);

	switch (cap) {
	case GL_TEXTURE_1D:
	case GL_TEXTURE_2D:
	case GL_TEXTURE_3D:
		/* empty */
		break;
	case GL_FOG:
		/* empty */
		break;
	case GL_ALPHA_TEST:
		//r700SetAlphaState(ctx);
		break;
	case GL_COLOR_LOGIC_OP:
		//r700SetLogicOpState(ctx);
		/* fall-through, because logic op overrides blending */
	case GL_BLEND:
		//r700SetBlendState(ctx);
		break;
	case GL_CLIP_PLANE0:
	case GL_CLIP_PLANE1:
	case GL_CLIP_PLANE2:
	case GL_CLIP_PLANE3:
	case GL_CLIP_PLANE4:
	case GL_CLIP_PLANE5:
		//r700SetClipPlaneState(ctx, cap, state);
		break;
	case GL_DEPTH_TEST:
		r700SetDepthState(ctx);
		break;
	case GL_STENCIL_TEST:
		//r700SetStencilState(ctx, state);
		break;
	case GL_CULL_FACE:
		r700UpdateCulling(ctx);
		break;
	case GL_POLYGON_OFFSET_POINT:
	case GL_POLYGON_OFFSET_LINE:
	case GL_POLYGON_OFFSET_FILL:
		//r700SetPolygonOffsetState(ctx, state);
		break;
	case GL_SCISSOR_TEST:
		radeon_firevertices(&context->radeon);
		context->radeon.state.scissor.enabled = state;
		radeonUpdateScissor(ctx);
		break;
	case GL_LINE_STIPPLE:
		r700UpdateLineStipple(ctx);
		break;	
	default:
		break;
	}

}

/**
 * Handle glColorMask()
 */
static void r700ColorMask(GLcontext * ctx,
			  GLboolean r, GLboolean g, GLboolean b, GLboolean a) //------------------
{
	R700_CHIP_CONTEXT *r700 = (R700_CHIP_CONTEXT*)(&R700_CONTEXT(ctx)->hw);
	unsigned int mask = ((r ? 1 : 0) |
			     (g ? 2 : 0) |
			     (b ? 4 : 0) |
			     (a ? 8 : 0));

	if (mask != r700->CB_SHADER_MASK.u32All)
		SETfield(r700->CB_SHADER_MASK.u32All, mask, OUTPUT0_ENABLE_shift, OUTPUT0_ENABLE_mask);
}

/**
 * Change the depth testing function.
 *
 * \note Mesa already filters redundant calls to this function.
 */
static void r700DepthFunc(GLcontext * ctx, GLenum func) //--------------------
{
    r700SetDepthState(ctx);
}

/**
 * Enable/Disable depth writing.
 *
 * \note Mesa already filters redundant calls to this function.
 */
static void r700DepthMask(GLcontext * ctx, GLboolean mask) //------------------
{
    r700SetDepthState(ctx);
}

/**
 * Change the culling mode.
 *
 * \note Mesa already filters redundant calls to this function.
 */
static void r700CullFace(GLcontext * ctx, GLenum mode) //-----------------
{
    r700UpdateCulling(ctx);
}

/* =============================================================
 * Fog
 */
static void r700Fogfv(GLcontext * ctx, GLenum pname, const GLfloat * param) //--------------
{
}

/**
 * Change the polygon orientation.
 *
 * \note Mesa already filters redundant calls to this function.
 */
static void r700FrontFace(GLcontext * ctx, GLenum mode) //------------------
{
    r700UpdateCulling(ctx);
}

static void r700ShadeModel(GLcontext * ctx, GLenum mode) //--------------------
{
	context_t *context = R700_CONTEXT(ctx);
	R700_CHIP_CONTEXT *r700 = (R700_CHIP_CONTEXT*)(&context->hw);

	/* also need to set/clear FLAT_SHADE bit per param in SPI_PS_INPUT_CNTL_[0-31] */
	switch (mode) {
	case GL_FLAT:
		SETbit(r700->SPI_INTERP_CONTROL_0.u32All, FLAT_SHADE_ENA_bit);
		break;
	case GL_SMOOTH:
		CLEARbit(r700->SPI_INTERP_CONTROL_0.u32All, FLAT_SHADE_ENA_bit);
		break;
	default:
		return;
	}
}

static void r700PointParameter(GLcontext * ctx, GLenum pname, const GLfloat * param) //---------------
{
}

static void r700StencilFuncSeparate(GLcontext * ctx, GLenum face,
				    GLenum func, GLint ref, GLuint mask) //---------------------
{
}


static void r700StencilMaskSeparate(GLcontext * ctx, GLenum face, GLuint mask) //--------------
{
}

static void r700StencilOpSeparate(GLcontext * ctx, GLenum face,
				  GLenum fail, GLenum zfail, GLenum zpass) //--------------------
{
}

static void r700UpdateWindow(GLcontext * ctx, int id) //--------------------
{

	context_t *context = R700_CONTEXT(ctx);
	R700_CHIP_CONTEXT *r700 = (R700_CHIP_CONTEXT*)(&context->hw);
	__DRIdrawablePrivate *dPriv = radeon_get_drawable(&context->radeon);
	GLfloat xoffset = dPriv ? (GLfloat) dPriv->x : 0;
	GLfloat yoffset = dPriv ? (GLfloat) dPriv->y + dPriv->h : 0;
	const GLfloat *v = ctx->Viewport._WindowMap.m;
	const GLfloat depthScale = 1.0F / ctx->DrawBuffer->_DepthMaxF;
	const GLboolean render_to_fbo = (ctx->DrawBuffer->Name != 0);
	GLfloat y_scale, y_bias;

	if (render_to_fbo) {
		y_scale = 1.0;
		y_bias = 0;
	} else {
		y_scale = -1.0;
		y_bias = yoffset;
	}

	GLfloat sx = v[MAT_SX];
	GLfloat tx = v[MAT_TX] + xoffset;
	GLfloat sy = v[MAT_SY] * y_scale;
	GLfloat ty = (v[MAT_TY] * y_scale) + y_bias;
	GLfloat sz = v[MAT_SZ] * depthScale;
	GLfloat tz = v[MAT_TZ] * depthScale;

	/* TODO : Need DMA flush as well. */

	r700->viewport[id].PA_CL_VPORT_XSCALE.f32All  = sx;
	r700->viewport[id].PA_CL_VPORT_XOFFSET.f32All = tx;

	r700->viewport[id].PA_CL_VPORT_YSCALE.f32All  = sy;
	r700->viewport[id].PA_CL_VPORT_YOFFSET.f32All = ty;

	r700->viewport[id].PA_CL_VPORT_ZSCALE.f32All  = sz;
	r700->viewport[id].PA_CL_VPORT_ZOFFSET.f32All = tz;

	r700->viewport[id].enabled = GL_TRUE;

	r700SetScissor(context);
}


static void r700Viewport(GLcontext * ctx,
                         GLint x,
                         GLint y,
			 GLsizei width,
                         GLsizei height) //--------------------
{
	r700UpdateWindow(ctx, 0);

	radeon_viewport(ctx, x, y, width, height);
}

static void r700DepthRange(GLcontext * ctx, GLclampd nearval, GLclampd farval) //-------------
{
	r700UpdateWindow(ctx, 0);
}

static void r700PointSize(GLcontext * ctx, GLfloat size) //-------------------
{
}

static void r700LineWidth(GLcontext * ctx, GLfloat widthf) //---------------
{
    context_t *context = R700_CONTEXT(ctx);
    R700_CHIP_CONTEXT *r700 = (R700_CHIP_CONTEXT*)(&context->hw);
    uint32_t lineWidth = (uint32_t)((widthf * 0.5) * (1 << 4));
    if (lineWidth > 0xFFFF)
	lineWidth = 0xFFFF;
    SETfield(r700->PA_SU_LINE_CNTL.u32All,(uint16_t)lineWidth,
	PA_SU_LINE_CNTL__WIDTH_shift, PA_SU_LINE_CNTL__WIDTH_mask);
}

static void r700LineStipple(GLcontext *ctx, GLint factor, GLushort pattern)
{
    context_t *context = R700_CONTEXT(ctx);
    R700_CHIP_CONTEXT *r700 = (R700_CHIP_CONTEXT*)(&context->hw);

    SETfield(r700->PA_SC_LINE_STIPPLE.u32All, pattern, LINE_PATTERN_shift, LINE_PATTERN_mask);
    SETfield(r700->PA_SC_LINE_STIPPLE.u32All, (factor-1), REPEAT_COUNT_shift, REPEAT_COUNT_mask);
    SETfield(r700->PA_SC_LINE_STIPPLE.u32All, 1, AUTO_RESET_CNTL_shift, AUTO_RESET_CNTL_mask);
}

static void r700PolygonOffset(GLcontext * ctx, GLfloat factor, GLfloat units) //--------------
{
}


static void r700PolygonMode(GLcontext * ctx, GLenum face, GLenum mode) //------------------
{
}
 
static void r700RenderMode(GLcontext * ctx, GLenum mode) //---------------------
{
}

static void r700ClipPlane( GLcontext *ctx, GLenum plane, const GLfloat *eq ) //-----------------
{
}

void r700SetScissor(context_t *context) //---------------
{
	R700_CHIP_CONTEXT *r700 = (R700_CHIP_CONTEXT*)(&context->hw);
	unsigned x1, y1, x2, y2;
	int id = 0;
	struct radeon_renderbuffer *rrb;

	rrb = radeon_get_colorbuffer(&context->radeon);
	if (!rrb || !rrb->bo) {
		return;
	}
	if (context->radeon.state.scissor.enabled) {
		x1 = context->radeon.state.scissor.rect.x1;
		y1 = context->radeon.state.scissor.rect.y1;
		x2 = context->radeon.state.scissor.rect.x2 - 1;
		y2 = context->radeon.state.scissor.rect.y2 - 1;
	} else {
		x1 = rrb->dPriv->x;
		y1 = rrb->dPriv->y;
		x2 = rrb->dPriv->x + rrb->dPriv->w;
		y2 = rrb->dPriv->y + rrb->dPriv->h;
	}

	/* window */
	SETbit(r700->PA_SC_WINDOW_SCISSOR_TL.u32All, WINDOW_OFFSET_DISABLE_bit);
	SETfield(r700->PA_SC_WINDOW_SCISSOR_TL.u32All, x1,
		 PA_SC_WINDOW_SCISSOR_TL__TL_X_shift, PA_SC_WINDOW_SCISSOR_TL__TL_X_mask);
	SETfield(r700->PA_SC_WINDOW_SCISSOR_TL.u32All, y1,
		 PA_SC_WINDOW_SCISSOR_TL__TL_Y_shift, PA_SC_WINDOW_SCISSOR_TL__TL_Y_mask);

	SETfield(r700->PA_SC_WINDOW_SCISSOR_BR.u32All, x2,
		 PA_SC_WINDOW_SCISSOR_BR__BR_X_shift, PA_SC_WINDOW_SCISSOR_BR__BR_X_mask);
	SETfield(r700->PA_SC_WINDOW_SCISSOR_BR.u32All, y2,
		 PA_SC_WINDOW_SCISSOR_BR__BR_Y_shift, PA_SC_WINDOW_SCISSOR_BR__BR_Y_mask);


	SETfield(r700->PA_SC_CLIPRECT_0_TL.u32All, x1,
		 PA_SC_CLIPRECT_0_TL__TL_X_shift, PA_SC_CLIPRECT_0_TL__TL_X_mask);
	SETfield(r700->PA_SC_CLIPRECT_0_TL.u32All, y1,
		 PA_SC_CLIPRECT_0_TL__TL_Y_shift, PA_SC_CLIPRECT_0_TL__TL_Y_mask);
	SETfield(r700->PA_SC_CLIPRECT_0_BR.u32All, x2,
		 PA_SC_CLIPRECT_0_BR__BR_X_shift, PA_SC_CLIPRECT_0_BR__BR_X_mask);
	SETfield(r700->PA_SC_CLIPRECT_0_BR.u32All, y2,
		 PA_SC_CLIPRECT_0_BR__BR_Y_shift, PA_SC_CLIPRECT_0_BR__BR_Y_mask);

	r700->PA_SC_CLIPRECT_1_TL.u32All = r700->PA_SC_CLIPRECT_0_TL.u32All;
	r700->PA_SC_CLIPRECT_1_BR.u32All = r700->PA_SC_CLIPRECT_0_BR.u32All;
	r700->PA_SC_CLIPRECT_2_TL.u32All = r700->PA_SC_CLIPRECT_0_TL.u32All;
	r700->PA_SC_CLIPRECT_2_BR.u32All = r700->PA_SC_CLIPRECT_0_BR.u32All;
	r700->PA_SC_CLIPRECT_3_TL.u32All = r700->PA_SC_CLIPRECT_0_TL.u32All;
	r700->PA_SC_CLIPRECT_3_BR.u32All = r700->PA_SC_CLIPRECT_0_BR.u32All;

	/* more....2d clip */
	SETbit(r700->PA_SC_GENERIC_SCISSOR_TL.u32All, WINDOW_OFFSET_DISABLE_bit);
	SETfield(r700->PA_SC_GENERIC_SCISSOR_TL.u32All, x1,
		 PA_SC_GENERIC_SCISSOR_TL__TL_X_shift, PA_SC_GENERIC_SCISSOR_TL__TL_X_mask);
	SETfield(r700->PA_SC_GENERIC_SCISSOR_TL.u32All, y1,
		 PA_SC_GENERIC_SCISSOR_TL__TL_Y_shift, PA_SC_GENERIC_SCISSOR_TL__TL_Y_mask);
	SETfield(r700->PA_SC_GENERIC_SCISSOR_BR.u32All, x2,
		 PA_SC_GENERIC_SCISSOR_BR__BR_X_shift, PA_SC_GENERIC_SCISSOR_BR__BR_X_mask);
	SETfield(r700->PA_SC_GENERIC_SCISSOR_BR.u32All, y2,
		 PA_SC_GENERIC_SCISSOR_BR__BR_Y_shift, PA_SC_GENERIC_SCISSOR_BR__BR_Y_mask);

	SETbit(r700->viewport[id].PA_SC_VPORT_SCISSOR_0_TL.u32All, WINDOW_OFFSET_DISABLE_bit);
	SETfield(r700->viewport[id].PA_SC_VPORT_SCISSOR_0_TL.u32All, x1,
		 PA_SC_VPORT_SCISSOR_0_TL__TL_X_shift, PA_SC_VPORT_SCISSOR_0_TL__TL_X_mask);
	SETfield(r700->viewport[id].PA_SC_VPORT_SCISSOR_0_TL.u32All, y1,
		 PA_SC_VPORT_SCISSOR_0_TL__TL_Y_shift, PA_SC_VPORT_SCISSOR_0_TL__TL_Y_mask);
	SETfield(r700->viewport[id].PA_SC_VPORT_SCISSOR_0_BR.u32All, x2,
		 PA_SC_VPORT_SCISSOR_0_BR__BR_X_shift, PA_SC_VPORT_SCISSOR_0_BR__BR_X_mask);
	SETfield(r700->viewport[id].PA_SC_VPORT_SCISSOR_0_BR.u32All, y2,
		 PA_SC_VPORT_SCISSOR_0_BR__BR_Y_shift, PA_SC_VPORT_SCISSOR_0_BR__BR_Y_mask);

	r700->viewport[id].PA_SC_VPORT_ZMIN_0.u32All = 0;
	r700->viewport[id].PA_SC_VPORT_ZMAX_0.u32All = 0x3F800000;
	r700->viewport[id].enabled = GL_TRUE;
}

void r700SetRenderTarget(context_t *context, int id)
{
    R700_CHIP_CONTEXT *r700 = (R700_CHIP_CONTEXT*)(&context->hw);

    struct radeon_renderbuffer *rrb;
    unsigned int nPitchInPixel;

    /* screen/window/view */
    SETfield(r700->CB_TARGET_MASK.u32All, 0xF, (4 * id), TARGET0_ENABLE_mask);

    rrb = radeon_get_colorbuffer(&context->radeon);
	if (!rrb || !rrb->bo) {
		fprintf(stderr, "no rrb\n");
		return;
	}

    /* color buffer */
    r700->render_target[id].CB_COLOR0_BASE.u32All = context->radeon.state.color.draw_offset;

    nPitchInPixel = rrb->pitch/rrb->cpp;
    SETfield(r700->render_target[id].CB_COLOR0_SIZE.u32All, (nPitchInPixel/8)-1,
             PITCH_TILE_MAX_shift, PITCH_TILE_MAX_mask);
    SETfield(r700->render_target[id].CB_COLOR0_SIZE.u32All, ( (nPitchInPixel * context->radeon.radeonScreen->driScreen->fbHeight)/64 )-1,
             SLICE_TILE_MAX_shift, SLICE_TILE_MAX_mask);
    r700->render_target[id].CB_COLOR0_BASE.u32All = 0;
    SETfield(r700->render_target[id].CB_COLOR0_INFO.u32All, ENDIAN_NONE, ENDIAN_shift, ENDIAN_mask);
    SETfield(r700->render_target[id].CB_COLOR0_INFO.u32All, ARRAY_LINEAR_GENERAL,
             CB_COLOR0_INFO__ARRAY_MODE_shift, CB_COLOR0_INFO__ARRAY_MODE_mask);
    if(4 == rrb->cpp)
    {
        SETfield(r700->render_target[id].CB_COLOR0_INFO.u32All, COLOR_8_8_8_8,
                 CB_COLOR0_INFO__FORMAT_shift, CB_COLOR0_INFO__FORMAT_mask);
        SETfield(r700->render_target[id].CB_COLOR0_INFO.u32All, SWAP_ALT, COMP_SWAP_shift, COMP_SWAP_mask);
    }
    else
    {
        SETfield(r700->render_target[id].CB_COLOR0_INFO.u32All, COLOR_5_6_5,
                 CB_COLOR0_INFO__FORMAT_shift, CB_COLOR0_INFO__FORMAT_mask);
        SETfield(r700->render_target[id].CB_COLOR0_INFO.u32All, SWAP_ALT_REV,
                 COMP_SWAP_shift, COMP_SWAP_mask);
    }
    SETbit(r700->render_target[id].CB_COLOR0_INFO.u32All, SOURCE_FORMAT_bit);
    SETbit(r700->render_target[id].CB_COLOR0_INFO.u32All, BLEND_CLAMP_bit);
    SETfield(r700->render_target[id].CB_COLOR0_INFO.u32All, NUMBER_UNORM, NUMBER_TYPE_shift, NUMBER_TYPE_mask);

    CLEARfield(r700->render_target[id].CB_BLEND0_CONTROL.u32All, COLOR_SRCBLEND_mask); /* no dst blend */
    CLEARfield(r700->render_target[id].CB_BLEND0_CONTROL.u32All, ALPHA_SRCBLEND_mask); /* no dst blend */

    r700->render_target[id].enabled = GL_TRUE;
}

void r700SetDepthTarget(context_t *context)
{
    R700_CHIP_CONTEXT *r700 = (R700_CHIP_CONTEXT*)(&context->hw);

    struct radeon_renderbuffer *rrb;
    unsigned int nPitchInPixel;

    /* depth buf */
    r700->DB_DEPTH_SIZE.u32All = 0;
    r700->DB_DEPTH_BASE.u32All = 0;
    r700->DB_DEPTH_INFO.u32All = 0;

    r700->DB_DEPTH_CLEAR.u32All     = 0x3F800000;
    r700->DB_DEPTH_VIEW.u32All      = 0;
    r700->DB_RENDER_CONTROL.u32All  = 0;
    SETbit(r700->DB_RENDER_CONTROL.u32All, STENCIL_COMPRESS_DISABLE_bit);
    SETbit(r700->DB_RENDER_CONTROL.u32All, DEPTH_COMPRESS_DISABLE_bit);
    r700->DB_RENDER_OVERRIDE.u32All = 0;
    if (context->radeon.radeonScreen->chip_family < CHIP_FAMILY_RV770)
	    SETbit(r700->DB_RENDER_OVERRIDE.u32All, FORCE_SHADER_Z_ORDER_bit);
    SETfield(r700->DB_RENDER_OVERRIDE.u32All, FORCE_DISABLE, FORCE_HIZ_ENABLE_shift, FORCE_HIZ_ENABLE_mask);
    SETfield(r700->DB_RENDER_OVERRIDE.u32All, FORCE_DISABLE, FORCE_HIS_ENABLE0_shift, FORCE_HIS_ENABLE0_mask);
    SETfield(r700->DB_RENDER_OVERRIDE.u32All, FORCE_DISABLE, FORCE_HIS_ENABLE1_shift, FORCE_HIS_ENABLE1_mask);

    r700->DB_ALPHA_TO_MASK.u32All = 0;
    SETfield(r700->DB_ALPHA_TO_MASK.u32All, 2, ALPHA_TO_MASK_OFFSET0_shift, ALPHA_TO_MASK_OFFSET0_mask);
    SETfield(r700->DB_ALPHA_TO_MASK.u32All, 2, ALPHA_TO_MASK_OFFSET1_shift, ALPHA_TO_MASK_OFFSET1_mask);
    SETfield(r700->DB_ALPHA_TO_MASK.u32All, 2, ALPHA_TO_MASK_OFFSET2_shift, ALPHA_TO_MASK_OFFSET2_mask);
    SETfield(r700->DB_ALPHA_TO_MASK.u32All, 2, ALPHA_TO_MASK_OFFSET3_shift, ALPHA_TO_MASK_OFFSET3_mask);

    rrb = radeon_get_depthbuffer(&context->radeon);
	if (!rrb)
		return;

    nPitchInPixel = rrb->pitch/rrb->cpp;

    SETfield(r700->DB_DEPTH_SIZE.u32All, (nPitchInPixel/8)-1,
             PITCH_TILE_MAX_shift, PITCH_TILE_MAX_mask);
    SETfield(r700->DB_DEPTH_SIZE.u32All, ( (nPitchInPixel * context->radeon.radeonScreen->driScreen->fbHeight)/64 )-1,
             SLICE_TILE_MAX_shift, SLICE_TILE_MAX_mask); /* size in pixel / 64 - 1 */

    if(4 == rrb->cpp)
    {
        switch (GL_CONTEXT(context)->Visual.depthBits)
        {
        case 16:
        case 24:
            SETfield(r700->DB_DEPTH_INFO.u32All, DEPTH_8_24,
                     DB_DEPTH_INFO__FORMAT_shift, DB_DEPTH_INFO__FORMAT_mask);
            break;
        default:
            fprintf(stderr, "Error: Unsupported depth %d... exiting\n",
                GL_CONTEXT(context)->Visual.depthBits);
            _mesa_exit(-1);
        }
    }
    else
    {
        SETfield(r700->DB_DEPTH_INFO.u32All, DEPTH_16,
                     DB_DEPTH_INFO__FORMAT_shift, DB_DEPTH_INFO__FORMAT_mask);
    }
    SETfield(r700->DB_DEPTH_INFO.u32All, ARRAY_2D_TILED_THIN1,
             DB_DEPTH_INFO__ARRAY_MODE_shift, DB_DEPTH_INFO__ARRAY_MODE_mask);
    /* r700->DB_PREFETCH_LIMIT.bits.DEPTH_HEIGHT_TILE_MAX = (context->currentDraw->h >> 3) - 1; */ /* z buffer sie may much bigger than what need, so use actual used h. */
}

static void r700InitSQConfig(GLcontext * ctx)
{
    context_t *context = R700_CONTEXT(ctx);
    R700_CHIP_CONTEXT *r700 = (R700_CHIP_CONTEXT*)(&context->hw);
    int ps_prio;
    int vs_prio;
    int gs_prio;
    int es_prio;
    int num_ps_gprs;
    int num_vs_gprs;
    int num_gs_gprs;
    int num_es_gprs;
    int num_temp_gprs;
    int num_ps_threads;
    int num_vs_threads;
    int num_gs_threads;
    int num_es_threads;
    int num_ps_stack_entries;
    int num_vs_stack_entries;
    int num_gs_stack_entries;
    int num_es_stack_entries;

    // SQ
    ps_prio = 0;
    vs_prio = 1;
    gs_prio = 2;
    es_prio = 3;
    switch (context->radeon.radeonScreen->chip_family) {
    case CHIP_FAMILY_R600:
	    num_ps_gprs = 192;
	    num_vs_gprs = 56;
	    num_temp_gprs = 4;
	    num_gs_gprs = 0;
	    num_es_gprs = 0;
	    num_ps_threads = 136;
	    num_vs_threads = 48;
	    num_gs_threads = 4;
	    num_es_threads = 4;
	    num_ps_stack_entries = 128;
	    num_vs_stack_entries = 128;
	    num_gs_stack_entries = 0;
	    num_es_stack_entries = 0;
	    break;
    case CHIP_FAMILY_RV630:
    case CHIP_FAMILY_RV635:
	    num_ps_gprs = 84;
	    num_vs_gprs = 36;
	    num_temp_gprs = 4;
	    num_gs_gprs = 0;
	    num_es_gprs = 0;
	    num_ps_threads = 144;
	    num_vs_threads = 40;
	    num_gs_threads = 4;
	    num_es_threads = 4;
	    num_ps_stack_entries = 40;
	    num_vs_stack_entries = 40;
	    num_gs_stack_entries = 32;
	    num_es_stack_entries = 16;
	    break;
    case CHIP_FAMILY_RV610:
    case CHIP_FAMILY_RV620:
    case CHIP_FAMILY_RS780:
    default:
	    num_ps_gprs = 84;
	    num_vs_gprs = 36;
	    num_temp_gprs = 4;
	    num_gs_gprs = 0;
	    num_es_gprs = 0;
	    num_ps_threads = 136;
	    num_vs_threads = 48;
	    num_gs_threads = 4;
	    num_es_threads = 4;
	    num_ps_stack_entries = 40;
	    num_vs_stack_entries = 40;
	    num_gs_stack_entries = 32;
	    num_es_stack_entries = 16;
	    break;
    case CHIP_FAMILY_RV670:
	    num_ps_gprs = 144;
	    num_vs_gprs = 40;
	    num_temp_gprs = 4;
	    num_gs_gprs = 0;
	    num_es_gprs = 0;
	    num_ps_threads = 136;
	    num_vs_threads = 48;
	    num_gs_threads = 4;
	    num_es_threads = 4;
	    num_ps_stack_entries = 40;
	    num_vs_stack_entries = 40;
	    num_gs_stack_entries = 32;
	    num_es_stack_entries = 16;
	    break;
    case CHIP_FAMILY_RV770:
	    num_ps_gprs = 192;
	    num_vs_gprs = 56;
	    num_temp_gprs = 4;
	    num_gs_gprs = 0;
	    num_es_gprs = 0;
	    num_ps_threads = 188;
	    num_vs_threads = 60;
	    num_gs_threads = 0;
	    num_es_threads = 0;
	    num_ps_stack_entries = 256;
	    num_vs_stack_entries = 256;
	    num_gs_stack_entries = 0;
	    num_es_stack_entries = 0;
	    break;
    case CHIP_FAMILY_RV730:
    case CHIP_FAMILY_RV740:
	    num_ps_gprs = 84;
	    num_vs_gprs = 36;
	    num_temp_gprs = 4;
	    num_gs_gprs = 0;
	    num_es_gprs = 0;
	    num_ps_threads = 188;
	    num_vs_threads = 60;
	    num_gs_threads = 0;
	    num_es_threads = 0;
	    num_ps_stack_entries = 128;
	    num_vs_stack_entries = 128;
	    num_gs_stack_entries = 0;
	    num_es_stack_entries = 0;
	    break;
    case CHIP_FAMILY_RV710:
	    num_ps_gprs = 192;
	    num_vs_gprs = 56;
	    num_temp_gprs = 4;
	    num_gs_gprs = 0;
	    num_es_gprs = 0;
	    num_ps_threads = 144;
	    num_vs_threads = 48;
	    num_gs_threads = 0;
	    num_es_threads = 0;
	    num_ps_stack_entries = 128;
	    num_vs_stack_entries = 128;
	    num_gs_stack_entries = 0;
	    num_es_stack_entries = 0;
	    break;
    }

    r700->sq_config.SQ_CONFIG.u32All = 0;
    if ((context->radeon.radeonScreen->chip_family == CHIP_FAMILY_RV610) ||
        (context->radeon.radeonScreen->chip_family == CHIP_FAMILY_RV620) ||
	(context->radeon.radeonScreen->chip_family == CHIP_FAMILY_RS780) ||
        (context->radeon.radeonScreen->chip_family == CHIP_FAMILY_RV710))
	    CLEARbit(r700->sq_config.SQ_CONFIG.u32All, VC_ENABLE_bit);
    else
	    SETbit(r700->sq_config.SQ_CONFIG.u32All, VC_ENABLE_bit);
    SETbit(r700->sq_config.SQ_CONFIG.u32All, DX9_CONSTS_bit);
    SETbit(r700->sq_config.SQ_CONFIG.u32All, ALU_INST_PREFER_VECTOR_bit);
    SETfield(r700->sq_config.SQ_CONFIG.u32All, ps_prio, PS_PRIO_shift, PS_PRIO_mask);
    SETfield(r700->sq_config.SQ_CONFIG.u32All, ps_prio, VS_PRIO_shift, VS_PRIO_mask);
    SETfield(r700->sq_config.SQ_CONFIG.u32All, ps_prio, GS_PRIO_shift, GS_PRIO_mask);
    SETfield(r700->sq_config.SQ_CONFIG.u32All, ps_prio, ES_PRIO_shift, ES_PRIO_mask);

    r700->sq_config.SQ_GPR_RESOURCE_MGMT_1.u32All = 0;
    SETfield(r700->sq_config.SQ_GPR_RESOURCE_MGMT_1.u32All, num_ps_gprs, NUM_PS_GPRS_shift, NUM_PS_GPRS_mask);
    SETfield(r700->sq_config.SQ_GPR_RESOURCE_MGMT_1.u32All, num_vs_gprs, NUM_VS_GPRS_shift, NUM_VS_GPRS_mask);
    SETfield(r700->sq_config.SQ_GPR_RESOURCE_MGMT_1.u32All, num_temp_gprs,
	     NUM_CLAUSE_TEMP_GPRS_shift, NUM_CLAUSE_TEMP_GPRS_mask);

    r700->sq_config.SQ_GPR_RESOURCE_MGMT_2.u32All = 0;
    SETfield(r700->sq_config.SQ_GPR_RESOURCE_MGMT_2.u32All, num_gs_gprs, NUM_GS_GPRS_shift, NUM_GS_GPRS_mask);
    SETfield(r700->sq_config.SQ_GPR_RESOURCE_MGMT_2.u32All, num_es_gprs, NUM_ES_GPRS_shift, NUM_ES_GPRS_mask);

    r700->sq_config.SQ_THREAD_RESOURCE_MGMT.u32All = 0;
    SETfield(r700->sq_config.SQ_THREAD_RESOURCE_MGMT.u32All, num_ps_threads,
	     NUM_PS_THREADS_shift, NUM_PS_THREADS_mask);
    SETfield(r700->sq_config.SQ_THREAD_RESOURCE_MGMT.u32All, num_vs_threads,
	     NUM_VS_THREADS_shift, NUM_VS_THREADS_mask);
    SETfield(r700->sq_config.SQ_THREAD_RESOURCE_MGMT.u32All, num_gs_threads,
	     NUM_GS_THREADS_shift, NUM_GS_THREADS_mask);
    SETfield(r700->sq_config.SQ_THREAD_RESOURCE_MGMT.u32All, num_es_threads,
	     NUM_ES_THREADS_shift, NUM_ES_THREADS_mask);

    r700->sq_config.SQ_STACK_RESOURCE_MGMT_1.u32All = 0;
    SETfield(r700->sq_config.SQ_STACK_RESOURCE_MGMT_1.u32All, num_ps_stack_entries,
	     NUM_PS_STACK_ENTRIES_shift, NUM_PS_STACK_ENTRIES_mask);
    SETfield(r700->sq_config.SQ_STACK_RESOURCE_MGMT_1.u32All, num_vs_stack_entries,
	     NUM_VS_STACK_ENTRIES_shift, NUM_VS_STACK_ENTRIES_mask);

    r700->sq_config.SQ_STACK_RESOURCE_MGMT_2.u32All = 0;
    SETfield(r700->sq_config.SQ_STACK_RESOURCE_MGMT_2.u32All, num_gs_stack_entries,
	     NUM_GS_STACK_ENTRIES_shift, NUM_GS_STACK_ENTRIES_mask);
    SETfield(r700->sq_config.SQ_STACK_RESOURCE_MGMT_2.u32All, num_es_stack_entries,
	     NUM_ES_STACK_ENTRIES_shift, NUM_ES_STACK_ENTRIES_mask);

}

/**
 * Calculate initial hardware state and register state functions.
 * Assumes that the command buffer and state atoms have been
 * initialized already.
 */
void r700InitState(GLcontext * ctx) //-------------------
{
    context_t *context = R700_CONTEXT(ctx);

    R700_CHIP_CONTEXT *r700 = (R700_CHIP_CONTEXT*)(&context->hw);

    r700->TA_CNTL_AUX.u32All = 0;
    SETfield(r700->TA_CNTL_AUX.u32All, 28, TD_FIFO_CREDIT_shift, TD_FIFO_CREDIT_mask);
    r700->VC_ENHANCE.u32All = 0;
    r700->DB_WATERMARKS.u32All = 0;
    SETfield(r700->DB_WATERMARKS.u32All, 4, DEPTH_FREE_shift, DEPTH_FREE_mask);
    SETfield(r700->DB_WATERMARKS.u32All, 16, DEPTH_FLUSH_shift, DEPTH_FLUSH_mask);
    SETfield(r700->DB_WATERMARKS.u32All, 0, FORCE_SUMMARIZE_shift, FORCE_SUMMARIZE_mask);
    SETfield(r700->DB_WATERMARKS.u32All, 4, DEPTH_PENDING_FREE_shift, DEPTH_PENDING_FREE_mask);
    r700->SQ_DYN_GPR_CNTL_PS_FLUSH_REQ.u32All = 0;
    if (context->radeon.radeonScreen->chip_family < CHIP_FAMILY_RV770) {
	    SETfield(r700->TA_CNTL_AUX.u32All, 3, GRADIENT_CREDIT_shift, GRADIENT_CREDIT_mask);
	    r700->DB_DEBUG.u32All = 0x82000000;
	    SETfield(r700->DB_WATERMARKS.u32All, 16, DEPTH_CACHELINE_FREE_shift, DEPTH_CACHELINE_FREE_mask);
    } else {
	    SETfield(r700->TA_CNTL_AUX.u32All, 2, GRADIENT_CREDIT_shift, GRADIENT_CREDIT_mask);
	    SETfield(r700->DB_WATERMARKS.u32All, 4, DEPTH_CACHELINE_FREE_shift, DEPTH_CACHELINE_FREE_mask);
	    SETbit(r700->SQ_DYN_GPR_CNTL_PS_FLUSH_REQ.u32All, VS_PC_LIMIT_ENABLE_bit);
    }

    /* Turn off vgt reuse */
    r700->VGT_REUSE_OFF.u32All = 0;
    SETbit(r700->VGT_REUSE_OFF.u32All, REUSE_OFF_bit);

    /* Specify offsetting and clamp values for vertices */
    r700->VGT_MAX_VTX_INDX.u32All      = 0xFFFFFF;
    r700->VGT_MIN_VTX_INDX.u32All      = 0;
    r700->VGT_INDX_OFFSET.u32All    = 0;

    /* Specify the number of instances */
    r700->VGT_DMA_NUM_INSTANCES.u32All = 1;

    /* not alpha blend */
    CLEARfield(r700->SX_ALPHA_TEST_CONTROL.u32All, ALPHA_FUNC_mask);
    CLEARbit(r700->SX_ALPHA_TEST_CONTROL.u32All, ALPHA_TEST_ENABLE_bit);

    /* default shader connections. */
    r700->SPI_VS_OUT_ID_0.u32All  = 0x03020100;
    r700->SPI_VS_OUT_ID_1.u32All  = 0x07060504;

    r700->SPI_PS_INPUT_CNTL_0.u32All  = 0x00000800;
    r700->SPI_PS_INPUT_CNTL_1.u32All  = 0x00000801;
    r700->SPI_PS_INPUT_CNTL_2.u32All  = 0x00000802;

    r700->SPI_THREAD_GROUPING.u32All = 0;
    if (context->radeon.radeonScreen->chip_family >= CHIP_FAMILY_RV770)
	    SETfield(r700->SPI_THREAD_GROUPING.u32All, 1, PS_GROUPING_shift, PS_GROUPING_mask);

    SETfield(r700->CB_COLOR_CONTROL.u32All, 0xCC, ROP3_shift, ROP3_mask);
    CLEARbit(r700->CB_COLOR_CONTROL.u32All, PER_MRT_BLEND_bit);

    r700->DB_SHADER_CONTROL.u32All = 0;
    SETbit(r700->DB_SHADER_CONTROL.u32All, DUAL_EXPORT_ENABLE_bit);

    /* Set up the culling control register */
    SETfield(r700->PA_SU_SC_MODE_CNTL.u32All, X_DRAW_TRIANGLES,
             POLYMODE_FRONT_PTYPE_shift, POLYMODE_FRONT_PTYPE_mask);
    SETfield(r700->PA_SU_SC_MODE_CNTL.u32All, X_DRAW_TRIANGLES,
             POLYMODE_BACK_PTYPE_shift, POLYMODE_BACK_PTYPE_mask);

    /* screen */
    r700->PA_SC_SCREEN_SCISSOR_TL.u32All = 0x0;

    SETfield(r700->PA_SC_SCREEN_SCISSOR_BR.u32All,
	     ((RADEONDRIPtr)(context->radeon.radeonScreen->driScreen->pDevPriv))->width,
	     PA_SC_SCREEN_SCISSOR_BR__BR_X_shift, PA_SC_SCREEN_SCISSOR_BR__BR_X_mask);
    SETfield(r700->PA_SC_SCREEN_SCISSOR_BR.u32All,
	     ((RADEONDRIPtr)(context->radeon.radeonScreen->driScreen->pDevPriv))->height,
	     PA_SC_SCREEN_SCISSOR_BR__BR_Y_shift, PA_SC_SCREEN_SCISSOR_BR__BR_Y_mask);

    /* 4 clip rectangles */ /* TODO : set these clip rects according to context->currentDraw->numClipRects */
    r700->PA_SC_CLIPRECT_RULE.u32All = 0;
    SETfield(r700->PA_SC_CLIPRECT_RULE.u32All, CLIP_RULE_mask, CLIP_RULE_shift, CLIP_RULE_mask);

    if (context->radeon.radeonScreen->chip_family < CHIP_FAMILY_RV770)
	    r700->PA_SC_EDGERULE.u32All = 0;
    else
	    r700->PA_SC_EDGERULE.u32All = 0xAAAAAAAA;

    if (context->radeon.radeonScreen->chip_family < CHIP_FAMILY_RV770) {
	    r700->PA_SC_MODE_CNTL.u32All = 0;
	    SETbit(r700->PA_SC_MODE_CNTL.u32All, WALK_ORDER_ENABLE_bit);
	    SETbit(r700->PA_SC_MODE_CNTL.u32All, FORCE_EOV_CNTDWN_ENABLE_bit);
    } else {
	    r700->PA_SC_MODE_CNTL.u32All = 0x00500000;
	    SETbit(r700->PA_SC_MODE_CNTL.u32All, FORCE_EOV_REZ_ENABLE_bit);
	    SETbit(r700->PA_SC_MODE_CNTL.u32All, FORCE_EOV_CNTDWN_ENABLE_bit);
    }

    /* Do scale XY and Z by 1/W0. */
    r700->bEnablePerspective = GL_TRUE;
    CLEARbit(r700->PA_CL_VTE_CNTL.u32All, VTX_XY_FMT_bit);
    CLEARbit(r700->PA_CL_VTE_CNTL.u32All, VTX_Z_FMT_bit);
    SETbit(r700->PA_CL_VTE_CNTL.u32All, VTX_W0_FMT_bit);

    /* Enable viewport scaling for all three axis */
    SETbit(r700->PA_CL_VTE_CNTL.u32All, VPORT_X_SCALE_ENA_bit);
    SETbit(r700->PA_CL_VTE_CNTL.u32All, VPORT_X_OFFSET_ENA_bit);
    SETbit(r700->PA_CL_VTE_CNTL.u32All, VPORT_Y_SCALE_ENA_bit);
    SETbit(r700->PA_CL_VTE_CNTL.u32All, VPORT_Y_OFFSET_ENA_bit);
    SETbit(r700->PA_CL_VTE_CNTL.u32All, VPORT_Z_SCALE_ENA_bit);
    SETbit(r700->PA_CL_VTE_CNTL.u32All, VPORT_Z_OFFSET_ENA_bit);

    /* Set up point sizes and min/max values */
    SETfield(r700->PA_SU_POINT_SIZE.u32All, 0x8,
             PA_SU_POINT_SIZE__HEIGHT_shift, PA_SU_POINT_SIZE__HEIGHT_mask);
    SETfield(r700->PA_SU_POINT_SIZE.u32All, 0x8,
             PA_SU_POINT_SIZE__WIDTH_shift, PA_SU_POINT_SIZE__WIDTH_mask);
    CLEARfield(r700->PA_SU_POINT_MINMAX.u32All, MIN_SIZE_mask);
    SETfield(r700->PA_SU_POINT_MINMAX.u32All, 0x8000, MAX_SIZE_shift, MAX_SIZE_mask);

    /* Set up line control */
    SETfield(r700->PA_SU_LINE_CNTL.u32All, 0x8,
             PA_SU_LINE_CNTL__WIDTH_shift, PA_SU_LINE_CNTL__WIDTH_mask);

    r700->PA_SC_LINE_CNTL.u32All = 0;
    CLEARbit(r700->PA_SC_LINE_CNTL.u32All, EXPAND_LINE_WIDTH_bit);
    SETbit(r700->PA_SC_LINE_CNTL.u32All, LAST_PIXEL_bit);

    /* Set up vertex control */
    r700->PA_SU_VTX_CNTL.u32All = 0;
    CLEARfield(r700->PA_SU_VTX_CNTL.u32All, QUANT_MODE_mask);
    SETbit(r700->PA_SU_VTX_CNTL.u32All, PIX_CENTER_bit);
    SETfield(r700->PA_SU_VTX_CNTL.u32All, X_ROUND_TO_EVEN,
             PA_SU_VTX_CNTL__ROUND_MODE_shift, PA_SU_VTX_CNTL__ROUND_MODE_mask);

    /* to 1.0 = no guard band */
    r700->PA_CL_GB_VERT_CLIP_ADJ.u32All  = 0x3F800000;  /* 1.0 */
    r700->PA_CL_GB_VERT_DISC_ADJ.u32All  = 0x3F800000;
    r700->PA_CL_GB_HORZ_CLIP_ADJ.u32All  = 0x3F800000;
    r700->PA_CL_GB_HORZ_DISC_ADJ.u32All  = 0x3F800000;

    /* CB */
    r700->CB_CLEAR_RED_R6XX.f32All = 1.0; //r6xx only
    r700->CB_CLEAR_GREEN_R6XX.f32All = 0.0; //r6xx only
    r700->CB_CLEAR_BLUE_R6XX.f32All = 1.0; //r6xx only
    r700->CB_CLEAR_ALPHA_R6XX.f32All = 1.0; //r6xx only
    r700->CB_FOG_RED_R6XX.u32All = 0; //r6xx only
    r700->CB_FOG_GREEN_R6XX.u32All = 0; //r6xx only
    r700->CB_FOG_BLUE_R6XX.u32All = 0; //r6xx only

    r700->CB_BLEND_RED.u32All = 0;
    r700->CB_BLEND_GREEN.u32All = 0;
    r700->CB_BLEND_BLUE.u32All = 0;
    r700->CB_BLEND_ALPHA.u32All = 0;

    r700->CB_BLEND_CONTROL.u32All = 0;

    /* Disable color compares */
    SETfield(r700->CB_CLRCMP_CONTROL.u32All, CLRCMP_DRAW_ALWAYS,
             CLRCMP_FCN_SRC_shift, CLRCMP_FCN_SRC_mask);
    SETfield(r700->CB_CLRCMP_CONTROL.u32All, CLRCMP_DRAW_ALWAYS,
             CLRCMP_FCN_DST_shift, CLRCMP_FCN_DST_mask);
    SETfield(r700->CB_CLRCMP_CONTROL.u32All, CLRCMP_SEL_SRC,
             CLRCMP_FCN_SEL_shift, CLRCMP_FCN_SEL_mask);

    /* Zero out source */
    r700->CB_CLRCMP_SRC.u32All = 0x00000000;

    /* Put a compare color in for error checking */
    r700->CB_CLRCMP_DST.u32All = 0x000000FF;

    /* Set up color compare mask */
    r700->CB_CLRCMP_MSK.u32All = 0xFFFFFFFF;

    /* default color mask */
    SETfield(r700->CB_SHADER_MASK.u32All, 0xF, OUTPUT0_ENABLE_shift, OUTPUT0_ENABLE_mask);

    /* Enable all samples for multi-sample anti-aliasing */
    r700->PA_SC_AA_MASK.u32All = 0xFFFFFFFF;
    /* Turn off AA */
    r700->PA_SC_AA_CONFIG.u32All = 0;

    r700->SX_MISC.u32All = 0;

    r700InitSQConfig(ctx);
}

void r700InitStateFuncs(struct dd_function_table *functions) //-----------------
{
	functions->UpdateState = r700InvalidateState;
	functions->AlphaFunc = r700AlphaFunc;
	functions->BlendColor = r700BlendColor;
	functions->BlendEquationSeparate = r700BlendEquationSeparate;
	functions->BlendFuncSeparate = r700BlendFuncSeparate;
	functions->Enable = r700Enable;
	functions->ColorMask = r700ColorMask;
	functions->DepthFunc = r700DepthFunc;
	functions->DepthMask = r700DepthMask;
	functions->CullFace = r700CullFace;
	functions->Fogfv = r700Fogfv;
	functions->FrontFace = r700FrontFace;
	functions->ShadeModel = r700ShadeModel;

	/* ARB_point_parameters */
	functions->PointParameterfv = r700PointParameter;

	/* Stencil related */
	functions->StencilFuncSeparate = r700StencilFuncSeparate;
	functions->StencilMaskSeparate = r700StencilMaskSeparate;
	functions->StencilOpSeparate = r700StencilOpSeparate;

	/* Viewport related */
	functions->Viewport = r700Viewport;
	functions->DepthRange = r700DepthRange;
	functions->PointSize = r700PointSize;
	functions->LineWidth = r700LineWidth;
	functions->LineStipple = r700LineStipple;

	functions->PolygonOffset = r700PolygonOffset;
	functions->PolygonMode = r700PolygonMode;

	functions->RenderMode = r700RenderMode;

	functions->ClipPlane = r700ClipPlane;

	functions->Scissor = radeonScissor;

	functions->DrawBuffer		= radeonDrawBuffer;
	functions->ReadBuffer		= radeonReadBuffer;

}

