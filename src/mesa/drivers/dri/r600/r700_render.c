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
 *   CooperYuan <cooper.yuan@amd.com>, <cooperyuan@gmail.com>
 */

#include "main/glheader.h"
#include "main/state.h"
#include "main/imports.h"
#include "main/enums.h"
#include "main/macros.h"
#include "main/context.h"
#include "main/dd.h"
#include "main/simple_list.h"
#include "main/api_arrayelt.h"
#include "swrast/swrast.h"
#include "swrast_setup/swrast_setup.h"
#include "vbo/vbo.h"

#include "tnl/tnl.h"
#include "tnl/t_vp_build.h"
#include "tnl/t_context.h"
#include "tnl/t_vertex.h"
#include "tnl/t_pipeline.h"

#include "radeon_mipmap_tree.h"
#include "r600_context.h"
#include "r600_cmdbuf.h"

#include "r600_tex.h"

#include "r700_vertprog.h"
#include "r700_fragprog.h"
#include "r700_state.h"

void r700WaitForIdle(context_t *context);
void r700WaitForIdleClean(context_t *context);
void r700Start3D(context_t *context);
GLboolean r700SendTextureState(context_t *context);
unsigned int r700PrimitiveType(int prim);
void r600UpdateTextureState(GLcontext * ctx);
GLboolean r700SyncSurf(context_t *context,
		       struct radeon_bo *pbo,
		       uint32_t read_domain,
		       uint32_t write_domain,
		       uint32_t sync_type);

void r700WaitForIdle(context_t *context)
{
    BATCH_LOCALS(&context->radeon);
    BEGIN_BATCH_NO_AUTOSTATE(3);

    R600_OUT_BATCH(CP_PACKET3(R600_IT_SET_CONFIG_REG, 1));
    R600_OUT_BATCH(mmWAIT_UNTIL - ASIC_CONFIG_BASE_INDEX);
    R600_OUT_BATCH(WAIT_3D_IDLE_bit);

    END_BATCH();
    COMMIT_BATCH();
}

void r700WaitForIdleClean(context_t *context)
{
    BATCH_LOCALS(&context->radeon);
    BEGIN_BATCH_NO_AUTOSTATE(5);

    R600_OUT_BATCH(CP_PACKET3(R600_IT_EVENT_WRITE, 0));
    R600_OUT_BATCH(CACHE_FLUSH_AND_INV_EVENT);

    R600_OUT_BATCH(CP_PACKET3(R600_IT_SET_CONFIG_REG, 1));
    R600_OUT_BATCH(mmWAIT_UNTIL - ASIC_CONFIG_BASE_INDEX);
    R600_OUT_BATCH(WAIT_3D_IDLE_bit | WAIT_3D_IDLECLEAN_bit);

    END_BATCH();
    COMMIT_BATCH();
}

void r700Start3D(context_t *context)
{
    BATCH_LOCALS(&context->radeon);
    if (context->radeon.radeonScreen->chip_family < CHIP_FAMILY_RV770)
    {
        BEGIN_BATCH_NO_AUTOSTATE(2);
        R600_OUT_BATCH(CP_PACKET3(R600_IT_START_3D_CMDBUF, 0));
        R600_OUT_BATCH(0);
        END_BATCH();
    }

    BEGIN_BATCH_NO_AUTOSTATE(3);
    R600_OUT_BATCH(CP_PACKET3(R600_IT_CONTEXT_CONTROL, 1));
    R600_OUT_BATCH(0x80000000);
    R600_OUT_BATCH(0x80000000);
    END_BATCH();

    COMMIT_BATCH();

    r700WaitForIdleClean(context);
}

static GLboolean r700SetupShaders(GLcontext * ctx)
{
    context_t *context = R700_CONTEXT(ctx);

    R700_CHIP_CONTEXT *r700 = (R700_CHIP_CONTEXT*)(&context->hw);

    GLuint exportCount;

    r700->ps.SQ_PGM_RESOURCES_PS.u32All = 0;
    r700->vs.SQ_PGM_RESOURCES_VS.u32All = 0;

    SETbit(r700->ps.SQ_PGM_RESOURCES_PS.u32All, PGM_RESOURCES__PRIME_CACHE_ON_DRAW_bit);
    SETbit(r700->vs.SQ_PGM_RESOURCES_VS.u32All, PGM_RESOURCES__PRIME_CACHE_ON_DRAW_bit);

    r700SetupVertexProgram(ctx);

    r700SetupFragmentProgram(ctx);

    exportCount = (r700->ps.SQ_PGM_EXPORTS_PS.u32All & EXPORT_MODE_mask) / (1 << EXPORT_MODE_shift);
    r700->CB_SHADER_CONTROL.u32All = (1 << exportCount) - 1;

    return GL_TRUE;
}

GLboolean r700SendTextureState(context_t *context)
{
    unsigned int i;
    R700_CHIP_CONTEXT *r700 = (R700_CHIP_CONTEXT*)(&context->hw);
    offset_modifiers offset_mod = {NO_SHIFT, 0, 0xFFFFFFFF};
    struct radeon_bo *bo = NULL;
    BATCH_LOCALS(&context->radeon);

    for (i=0; i<R700_TEXTURE_NUMBERUNITS; i++) {
	    radeonTexObj *t = r700->textures[i];
	    if (t) {
		    if (!t->image_override)
			    bo = t->mt->bo;
		    else
			    bo = t->bo;
		    if (bo) {

			    r700SyncSurf(context, bo,
					 RADEON_GEM_DOMAIN_GTT|RADEON_GEM_DOMAIN_VRAM,
					 0, TC_ACTION_ENA_bit);

			    BEGIN_BATCH_NO_AUTOSTATE(9);
			    R600_OUT_BATCH(CP_PACKET3(R600_IT_SET_RESOURCE, 7));
			    R600_OUT_BATCH(i * 7);
			    R600_OUT_BATCH(r700->textures[i]->SQ_TEX_RESOURCE0);
			    R600_OUT_BATCH(r700->textures[i]->SQ_TEX_RESOURCE1);
			    R600_OUT_BATCH_RELOC(r700->textures[i]->SQ_TEX_RESOURCE2,
						 bo,
						 0,
						 RADEON_GEM_DOMAIN_GTT|RADEON_GEM_DOMAIN_VRAM, 0, 0, &offset_mod);
			    R600_OUT_BATCH_RELOC(r700->textures[i]->SQ_TEX_RESOURCE3,
						 bo,
						 0,
						 RADEON_GEM_DOMAIN_GTT|RADEON_GEM_DOMAIN_VRAM, 0, 0, &offset_mod);
			    R600_OUT_BATCH(r700->textures[i]->SQ_TEX_RESOURCE4);
			    R600_OUT_BATCH(r700->textures[i]->SQ_TEX_RESOURCE5);
			    R600_OUT_BATCH(r700->textures[i]->SQ_TEX_RESOURCE6);
			    END_BATCH();

			    BEGIN_BATCH_NO_AUTOSTATE(5);
			    R600_OUT_BATCH(CP_PACKET3(R600_IT_SET_SAMPLER, 3));
			    R600_OUT_BATCH(i * 3);
			    R600_OUT_BATCH(r700->textures[i]->SQ_TEX_SAMPLER0);
			    R600_OUT_BATCH(r700->textures[i]->SQ_TEX_SAMPLER1);
			    R600_OUT_BATCH(r700->textures[i]->SQ_TEX_SAMPLER2);
			    END_BATCH();
			    COMMIT_BATCH();
		    }
	    }
    }
    return GL_TRUE;
}

GLboolean r700SyncSurf(context_t *context,
		       struct radeon_bo *pbo,
		       uint32_t read_domain,
		       uint32_t write_domain,
		       uint32_t sync_type)
{
    BATCH_LOCALS(&context->radeon);
    uint32_t cp_coher_size;
    offset_modifiers offset_mod;

    if (pbo->size == 0xffffffff)
	    cp_coher_size = 0xffffffff;
    else
	    cp_coher_size = ((pbo->size + 255) >> 8);

    offset_mod.shift     = NO_SHIFT;
    offset_mod.shiftbits = 0;
    offset_mod.mask      = 0xFFFFFFFF;

    BEGIN_BATCH_NO_AUTOSTATE(5);
    R600_OUT_BATCH(CP_PACKET3(R600_IT_SURFACE_SYNC, 3));
    R600_OUT_BATCH(sync_type);
    R600_OUT_BATCH(cp_coher_size);
    R600_OUT_BATCH_RELOC(0,
			 pbo,
			 0,
			 read_domain, write_domain, 0, &offset_mod); // ???
    R600_OUT_BATCH(10);

    END_BATCH();
    COMMIT_BATCH();

    return GL_TRUE;
}

unsigned int r700PrimitiveType(int prim)
{
    switch (prim & PRIM_MODE_MASK)
    {
    case GL_POINTS:
        return DI_PT_POINTLIST;
        break;
    case GL_LINES:
        return DI_PT_LINELIST;
        break;
    case GL_LINE_STRIP:
        return DI_PT_LINESTRIP;
        break;
    case GL_LINE_LOOP:
        return DI_PT_LINELOOP;
        break;
    case GL_TRIANGLES:
        return DI_PT_TRILIST;
        break;
    case GL_TRIANGLE_STRIP:
        return DI_PT_TRISTRIP;
        break;
    case GL_TRIANGLE_FAN:
        return DI_PT_TRIFAN;
        break;
    case GL_QUADS:
        return DI_PT_QUADLIST;
        break;
    case GL_QUAD_STRIP:
        return DI_PT_QUADSTRIP;
        break;
    case GL_POLYGON:
        return DI_PT_POLYGON;
        break;
    default:
        assert(0);
        return -1;
        break;
    }
}

static GLboolean r700RunRender(GLcontext * ctx,
			                   struct tnl_pipeline_stage *stage)
{
    context_t *context = R700_CONTEXT(ctx);
    R700_CHIP_CONTEXT *r700 = (R700_CHIP_CONTEXT*)(&context->hw);
    int lastIndex = 0;
#if 1
    BATCH_LOCALS(&context->radeon);

    unsigned int i, j;
    TNLcontext *tnl = TNL_CONTEXT(ctx);
    struct vertex_buffer *vb = &tnl->vb;

    struct r700_fragment_program *fp = (struct r700_fragment_program *)
	                                   (ctx->FragmentProgram._Current);
    if (context->radeon.radeonScreen->chip_family < CHIP_FAMILY_RV770)
    {
        fp->r700AsmCode.bR6xx = 1;
    }

    r700Start3D(context); /* TODO : this is too much. */

    r700SendSQConfig(context);

    r700UpdateShaders(ctx);

    r700SetScissor(context);
    r700SetRenderTarget(context, 0);
    r700SetDepthTarget(context);

    if(r700SetupStreams(ctx))
    {
        return GL_TRUE;
    }

    r600UpdateTextureState(ctx);
    r700SendTextureState(context);

    if(GL_FALSE == fp->translated)
    {
        if( GL_FALSE == r700TranslateFragmentShader(fp, &(fp->mesa_program)) )
        {
            return GL_TRUE;
        }
    }

    r700SetupShaders(ctx);

    r700SendFSState(context); // FIXME just a place holder for now
    r700SendPSState(context);
    r700SendVSState(context);

    r700SendContextStates(context);
    r700SendViewportState(context, 0);
    r700SendRenderTargetState(context, 0);
    r700SendDepthTargetState(context);

    /* richard test code */
    for (i = 0; i < vb->PrimitiveCount; i++) 
    {
        GLuint prim = _tnl_translate_prim(&vb->Primitive[i]);
        GLuint start = vb->Primitive[i].start;
        GLuint end = vb->Primitive[i].start + vb->Primitive[i].count;
        GLuint numIndices = vb->Primitive[i].count;
        GLuint numEntires;

        unsigned int VGT_DRAW_INITIATOR = 0;
        unsigned int VGT_INDEX_TYPE     = 0;
        unsigned int VGT_PRIMITIVE_TYPE = 0;
        unsigned int VGT_NUM_INDICES    = 0;
        
        numEntires = 2 /* VGT_INDEX_TYPE */
                     + 3 /* VGT_PRIMITIVE_TYPE */
                     + numIndices + 3; /* DRAW_INDEX_IMMD */                  
                     
        BEGIN_BATCH_NO_AUTOSTATE(numEntires);  

        VGT_INDEX_TYPE |= DI_INDEX_SIZE_32_BIT << INDEX_TYPE_shift;

        R600_OUT_BATCH(CP_PACKET3(R600_IT_INDEX_TYPE, 0));
        R600_OUT_BATCH(VGT_INDEX_TYPE);

        VGT_NUM_INDICES = numIndices;

        VGT_PRIMITIVE_TYPE |= r700PrimitiveType(prim) << VGT_PRIMITIVE_TYPE__PRIM_TYPE_shift;
        R600_OUT_BATCH(CP_PACKET3(R600_IT_SET_CONFIG_REG, 1));
        R600_OUT_BATCH(mmVGT_PRIMITIVE_TYPE - ASIC_CONFIG_BASE_INDEX);
        R600_OUT_BATCH(VGT_PRIMITIVE_TYPE);

        VGT_DRAW_INITIATOR |= DI_SRC_SEL_IMMEDIATE << SOURCE_SELECT_shift;
        VGT_DRAW_INITIATOR |= DI_MAJOR_MODE_0 << MAJOR_MODE_shift;

        R600_OUT_BATCH(CP_PACKET3(R600_IT_DRAW_INDEX_IMMD, (numIndices + 1)));
        R600_OUT_BATCH(VGT_NUM_INDICES);
        R600_OUT_BATCH(VGT_DRAW_INITIATOR);

        for (j = lastIndex; j < lastIndex + numIndices; j++)
        {
            R600_OUT_BATCH(j);
        }
        lastIndex += numIndices;

        END_BATCH();
        COMMIT_BATCH();
    }

    /* Flush render op cached for last several quads. */
    r700WaitForIdleClean(context);

    radeonReleaseArrays(ctx, 0);

#endif //0
    rcommonFlushCmdBuf( &context->radeon, __FUNCTION__ );

    return GL_FALSE;
}

static GLboolean r700RunNonTCLRender(GLcontext * ctx,
				     struct tnl_pipeline_stage *stage) /* -------------------- */
{
	GLboolean bRet = GL_TRUE;
	
	return bRet;
}

static GLboolean r700RunTCLRender(GLcontext * ctx,  /*----------------------*/
				  struct tnl_pipeline_stage *stage)
{
	GLboolean bRet = GL_FALSE;

    /* TODO : sw fallback */

    /**
    * Ensure all enabled and complete textures are uploaded along with any buffers being used.
    */
    if(!r600ValidateBuffers(ctx))
    {
        return GL_TRUE;
    }

    context_t *context = R700_CONTEXT(ctx);

    r700UpdateShaders(ctx);

    bRet = r700RunRender(ctx, stage);

    return bRet;
	//GL_FALSE will stop to do other pipe stage in _tnl_run_pipeline
    //The render here DOES finish the whole pipe, so GL_FALSE should be returned for success.
}

const struct tnl_pipeline_stage _r700_render_stage = {
	"r700 Hardware Rasterization",
	NULL,
	NULL,
	NULL,
	NULL,
	r700RunNonTCLRender
};

const struct tnl_pipeline_stage _r700_tcl_stage = {
	"r700 Hardware Transform, Clipping and Lighting",
	NULL,
	NULL,
	NULL,
	NULL,
	r700RunTCLRender
};

const struct tnl_pipeline_stage *r700_pipeline[] = 
{
    &_r700_tcl_stage,
    &_tnl_vertex_transform_stage,
	&_tnl_normal_transform_stage,
	&_tnl_lighting_stage,
	&_tnl_fog_coordinate_stage,
	&_tnl_texgen_stage,
	&_tnl_texture_transform_stage,
	&_tnl_vertex_program_stage,

    &_r700_render_stage,
    &_tnl_render_stage,
    0,
};


