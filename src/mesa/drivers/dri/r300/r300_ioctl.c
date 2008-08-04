/*
Copyright (C) The Weather Channel, Inc.  2002.
Copyright (C) 2004 Nicolai Haehnle.
All Rights Reserved.

The Weather Channel (TM) funded Tungsten Graphics to develop the
initial release of the Radeon 8500 driver under the XFree86 license.
This notice must be preserved.

Permission is hereby granted, free of charge, to any person obtaining
a copy of this software and associated documentation files (the
"Software"), to deal in the Software without restriction, including
without limitation the rights to use, copy, modify, merge, publish,
distribute, sublicense, and/or sell copies of the Software, and to
permit persons to whom the Software is furnished to do so, subject to
the following conditions:

The above copyright notice and this permission notice (including the
next paragraph) shall be included in all copies or substantial
portions of the Software.

THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND,
EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF
MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.
IN NO EVENT SHALL THE COPYRIGHT OWNER(S) AND/OR ITS SUPPLIERS BE
LIABLE FOR ANY CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER IN AN ACTION
OF CONTRACT, TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN CONNECTION
WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.

**************************************************************************/

/**
 * \file
 *
 * \author Keith Whitwell <keith@tungstengraphics.com>
 *
 * \author Nicolai Haehnle <prefect_@gmx.net>
 */

#include <sched.h>
#include <errno.h>

#include "glheader.h"
#include "imports.h"
#include "macros.h"
#include "context.h"
#include "swrast/swrast.h"

#include "r300_context.h"
#include "radeon_ioctl.h"
#include "r300_ioctl.h"
#include "r300_cmdbuf.h"
#include "r300_state.h"
#include "r300_vertprog.h"
#include "radeon_reg.h"
#include "r300_emit.h"
#include "r300_fragprog.h"
#include "r300_mem.h"

#include "vblank.h"

#define CLEARBUFFER_COLOR	0x1
#define CLEARBUFFER_DEPTH	0x2
#define CLEARBUFFER_STENCIL	0x4

static void r300ClearBuffer(r300ContextPtr r300, int flags, int buffer)
{
	BATCH_LOCALS(r300);
	GLcontext *ctx = r300->radeon.glCtx;
	__DRIdrawablePrivate *dPriv = r300->radeon.dri.drawable;
	GLuint cboffset, cbpitch;
	r300ContextPtr rmesa = r300;

	if (RADEON_DEBUG & DEBUG_IOCTL)
		fprintf(stderr, "%s: %s buffer (%i,%i %ix%i)\n",
			__FUNCTION__, buffer ? "back" : "front",
			dPriv->x, dPriv->y, dPriv->w, dPriv->h);

	if (buffer) {
		cboffset = r300->radeon.radeonScreen->backOffset;
		cbpitch = r300->radeon.radeonScreen->backPitch;
	} else {
		cboffset = r300->radeon.radeonScreen->frontOffset;
		cbpitch = r300->radeon.radeonScreen->frontPitch;
	}

	cboffset += r300->radeon.radeonScreen->fbLocation;

	if (r300->radeon.radeonScreen->cpp == 4)
		cbpitch |= R300_COLOR_FORMAT_ARGB8888;
	else
		cbpitch |= R300_COLOR_FORMAT_RGB565;

	if (r300->radeon.sarea->tiling_enabled)
		cbpitch |= R300_COLOR_TILE_ENABLE;

	cp_wait(r300, R300_WAIT_3D | R300_WAIT_3D_CLEAN);
	end_3d(rmesa);

	BEGIN_BATCH(19);
	OUT_BATCH_REGVAL(R300_RB3D_COLOROFFSET0, cboffset);
	OUT_BATCH_REGVAL(R300_RB3D_COLORPITCH0, cbpitch);

	OUT_BATCH_REGSEQ(RB3D_COLOR_CHANNEL_MASK, 1);
	if (flags & CLEARBUFFER_COLOR) {
		OUT_BATCH((ctx->Color.ColorMask[BCOMP] ? RB3D_COLOR_CHANNEL_MASK_BLUE_MASK0 : 0) |
			  (ctx->Color.ColorMask[GCOMP] ? RB3D_COLOR_CHANNEL_MASK_GREEN_MASK0 : 0) |
			  (ctx->Color.ColorMask[RCOMP] ? RB3D_COLOR_CHANNEL_MASK_RED_MASK0 : 0) |
			  (ctx->Color.ColorMask[ACOMP] ? RB3D_COLOR_CHANNEL_MASK_ALPHA_MASK0 : 0));
	} else {
		OUT_BATCH(0);
	}

	OUT_BATCH_REGSEQ(R300_ZB_CNTL, 3);

	{
		uint32_t t1, t2;

		t1 = 0x0;
		t2 = 0x0;

		if (flags & CLEARBUFFER_DEPTH) {
			t1 |= R300_Z_ENABLE | R300_Z_WRITE_ENABLE;
			t2 |=
			    (R300_ZS_ALWAYS << R300_Z_FUNC_SHIFT);
		}

		if (flags & CLEARBUFFER_STENCIL) {
			t1 |= R300_STENCIL_ENABLE;
			t2 |=
			    (R300_ZS_ALWAYS <<
			     R300_S_FRONT_FUNC_SHIFT) |
			    (R300_ZS_REPLACE <<
			     R300_S_FRONT_SFAIL_OP_SHIFT) |
			    (R300_ZS_REPLACE <<
			     R300_S_FRONT_ZPASS_OP_SHIFT) |
			    (R300_ZS_REPLACE <<
			     R300_S_FRONT_ZFAIL_OP_SHIFT);
		}

		OUT_BATCH(t1);
		OUT_BATCH(t2);
		OUT_BATCH(((ctx->Stencil.WriteMask[0] & R300_STENCILREF_MASK) << R300_STENCILWRITEMASK_SHIFT) |
			  (ctx->Stencil.Clear & R300_STENCILREF_MASK));
	}

	OUT_BATCH(cmdpacket3(R300_CMD_PACKET3_CLEAR));
	OUT_BATCH_FLOAT32(dPriv->w / 2.0);
	OUT_BATCH_FLOAT32(dPriv->h / 2.0);
	OUT_BATCH_FLOAT32(ctx->Depth.Clear);
	OUT_BATCH_FLOAT32(1.0);
	OUT_BATCH_FLOAT32(ctx->Color.ClearColor[0]);
	OUT_BATCH_FLOAT32(ctx->Color.ClearColor[1]);
	OUT_BATCH_FLOAT32(ctx->Color.ClearColor[2]);
	OUT_BATCH_FLOAT32(ctx->Color.ClearColor[3]);
	END_BATCH();

	r300EmitCacheFlush(rmesa);
	cp_wait(rmesa, R300_WAIT_3D | R300_WAIT_3D_CLEAN);

	R300_STATECHANGE(r300, cb);
	R300_STATECHANGE(r300, cmk);
	R300_STATECHANGE(r300, zs);
}

static void r300EmitClearState(GLcontext * ctx)
{
	r300ContextPtr r300 = R300_CONTEXT(ctx);
	BATCH_LOCALS(r300);
	__DRIdrawablePrivate *dPriv = r300->radeon.dri.drawable;
	int i;
	int has_tcl = 1;
	int is_r500 = 0;
	GLuint vap_cntl;

	if (!(r300->radeon.radeonScreen->chip_flags & RADEON_CHIPSET_TCL))
		has_tcl = 0;

	if (r300->radeon.radeonScreen->chip_family >= CHIP_FAMILY_RV515)
		is_r500 = 1;

	/* State atom dirty tracking is a little subtle here.
	 *
	 * On the one hand, we need to make sure base state is emitted
	 * here if we start with an empty batch buffer, otherwise clear
	 * works incorrectly with multiple processes. Therefore, the first
	 * BEGIN_BATCH cannot be a BEGIN_BATCH_NO_AUTOSTATE.
	 *
	 * On the other hand, implicit state emission clears the state atom
	 * dirty bits, so we have to call R300_STATECHANGE later than the
	 * first BEGIN_BATCH.
	 *
	 * The final trickiness is that, because we change state, we need
	 * to ensure that any stored swtcl primitives are flushed properly
	 * before we start changing state. See the R300_NEWPRIM in r300Clear
	 * for this.
	 */
	BEGIN_BATCH(31);
	OUT_BATCH_REGSEQ(R300_VAP_PROG_STREAM_CNTL_0, 1);
	if (!has_tcl)
		OUT_BATCH(((((0 << R300_DST_VEC_LOC_SHIFT) | R300_DATA_TYPE_FLOAT_4) << R300_DATA_TYPE_0_SHIFT) |
		 ((R300_LAST_VEC | (2 << R300_DST_VEC_LOC_SHIFT) | R300_DATA_TYPE_FLOAT_4) << R300_DATA_TYPE_1_SHIFT)));
	else
		OUT_BATCH(((((0 << R300_DST_VEC_LOC_SHIFT) | R300_DATA_TYPE_FLOAT_4) << R300_DATA_TYPE_0_SHIFT) |
		 ((R300_LAST_VEC | (1 << R300_DST_VEC_LOC_SHIFT) | R300_DATA_TYPE_FLOAT_4) << R300_DATA_TYPE_1_SHIFT)));

	OUT_BATCH_REGVAL(R300_FG_FOG_BLEND, 0);
	OUT_BATCH_REGVAL(R300_VAP_PROG_STREAM_CNTL_EXT_0,
	   ((((R300_SWIZZLE_SELECT_X << R300_SWIZZLE_SELECT_X_SHIFT) |
	       (R300_SWIZZLE_SELECT_Y << R300_SWIZZLE_SELECT_Y_SHIFT) |
	       (R300_SWIZZLE_SELECT_Z << R300_SWIZZLE_SELECT_Z_SHIFT) |
	       (R300_SWIZZLE_SELECT_W << R300_SWIZZLE_SELECT_W_SHIFT) |
	       ((R300_WRITE_ENA_X | R300_WRITE_ENA_Y | R300_WRITE_ENA_Z | R300_WRITE_ENA_W) << R300_WRITE_ENA_SHIFT))
	      << R300_SWIZZLE0_SHIFT) |
	     (((R300_SWIZZLE_SELECT_X << R300_SWIZZLE_SELECT_X_SHIFT) |
	       (R300_SWIZZLE_SELECT_Y << R300_SWIZZLE_SELECT_Y_SHIFT) |
	       (R300_SWIZZLE_SELECT_Z << R300_SWIZZLE_SELECT_Z_SHIFT) |
	       (R300_SWIZZLE_SELECT_W << R300_SWIZZLE_SELECT_W_SHIFT) |
	       ((R300_WRITE_ENA_X | R300_WRITE_ENA_Y | R300_WRITE_ENA_Z | R300_WRITE_ENA_W) << R300_WRITE_ENA_SHIFT))
	      << R300_SWIZZLE1_SHIFT)));

	/* R300_VAP_INPUT_CNTL_0, R300_VAP_INPUT_CNTL_1 */
	OUT_BATCH_REGSEQ(R300_VAP_VTX_STATE_CNTL, 2);
	OUT_BATCH((R300_SEL_USER_COLOR_0 << R300_COLOR_0_ASSEMBLY_SHIFT));
	OUT_BATCH(R300_INPUT_CNTL_POS | R300_INPUT_CNTL_COLOR | R300_INPUT_CNTL_TC0);

	/* comes from fglrx startup of clear */
	OUT_BATCH_REGSEQ(R300_SE_VTE_CNTL, 2);
	OUT_BATCH(R300_VTX_W0_FMT | R300_VPORT_X_SCALE_ENA |
		  R300_VPORT_X_OFFSET_ENA | R300_VPORT_Y_SCALE_ENA |
		  R300_VPORT_Y_OFFSET_ENA | R300_VPORT_Z_SCALE_ENA |
		  R300_VPORT_Z_OFFSET_ENA);
	OUT_BATCH(0x8);

	OUT_BATCH_REGVAL(R300_VAP_PSC_SGN_NORM_CNTL, 0xaaaaaaaa);

	OUT_BATCH_REGSEQ(R300_VAP_OUTPUT_VTX_FMT_0, 2);
	OUT_BATCH(R300_VAP_OUTPUT_VTX_FMT_0__POS_PRESENT |
		  R300_VAP_OUTPUT_VTX_FMT_0__COLOR_0_PRESENT);
	OUT_BATCH(0); /* no textures */

	OUT_BATCH_REGVAL(R300_TX_ENABLE, 0);

	OUT_BATCH_REGSEQ(R300_SE_VPORT_XSCALE, 6);
	OUT_BATCH_FLOAT32(1.0);
	OUT_BATCH_FLOAT32(dPriv->x);
	OUT_BATCH_FLOAT32(1.0);
	OUT_BATCH_FLOAT32(dPriv->y);
	OUT_BATCH_FLOAT32(1.0);
	OUT_BATCH_FLOAT32(0.0);

	OUT_BATCH_REGVAL(R300_FG_ALPHA_FUNC, 0);

	OUT_BATCH_REGSEQ(R300_RB3D_CBLEND, 2);
	OUT_BATCH(0x0);
	OUT_BATCH(0x0);
	END_BATCH();

	R300_STATECHANGE(r300, vir[0]);
	R300_STATECHANGE(r300, fogs);
	R300_STATECHANGE(r300, vir[1]);
	R300_STATECHANGE(r300, vic);
	R300_STATECHANGE(r300, vte);
	R300_STATECHANGE(r300, vof);
	R300_STATECHANGE(r300, txe);
	R300_STATECHANGE(r300, vpt);
	R300_STATECHANGE(r300, at);
	R300_STATECHANGE(r300, bld);
	R300_STATECHANGE(r300, ps);

	if (has_tcl) {
		R300_STATECHANGE(r300, vap_clip_cntl);

		BEGIN_BATCH_NO_AUTOSTATE(2);
		OUT_BATCH_REGVAL(R300_VAP_CLIP_CNTL, R300_PS_UCP_MODE_CLIP_AS_TRIFAN | R300_CLIP_DISABLE);
		END_BATCH();
        }

	BEGIN_BATCH_NO_AUTOSTATE(2);
	OUT_BATCH_REGVAL(R300_GA_POINT_SIZE,
		((dPriv->w * 6) << R300_POINTSIZE_X_SHIFT) |
		((dPriv->h * 6) << R300_POINTSIZE_Y_SHIFT));
	END_BATCH();

	if (!is_r500) {
		R300_STATECHANGE(r300, ri);
		R300_STATECHANGE(r300, rc);
		R300_STATECHANGE(r300, rr);

		BEGIN_BATCH(14);
		OUT_BATCH_REGSEQ(R300_RS_IP_0, 8);
		for (i = 0; i < 8; ++i)
			OUT_BATCH(R300_RS_SEL_T(1) | R300_RS_SEL_R(2) | R300_RS_SEL_Q(3));

		OUT_BATCH_REGSEQ(R300_RS_COUNT, 2);
		OUT_BATCH((1 << R300_IC_COUNT_SHIFT) | R300_HIRES_EN);
		OUT_BATCH(0x0);

		OUT_BATCH_REGVAL(R300_RS_INST_0, R300_RS_INST_COL_CN_WRITE);
		END_BATCH();
	} else {
		R300_STATECHANGE(r300, ri);
		R300_STATECHANGE(r300, rc);
		R300_STATECHANGE(r300, rr);

		BEGIN_BATCH(14);
		OUT_BATCH_REGSEQ(R500_RS_IP_0, 8);
		for (i = 0; i < 8; ++i) {
			OUT_BATCH((R500_RS_IP_PTR_K0 << R500_RS_IP_TEX_PTR_S_SHIFT) |
				  (R500_RS_IP_PTR_K0 << R500_RS_IP_TEX_PTR_T_SHIFT) |
				  (R500_RS_IP_PTR_K0 << R500_RS_IP_TEX_PTR_R_SHIFT) |
				  (R500_RS_IP_PTR_K1 << R500_RS_IP_TEX_PTR_Q_SHIFT));
		}

		OUT_BATCH_REGSEQ(R300_RS_COUNT, 2);
		OUT_BATCH((1 << R300_IC_COUNT_SHIFT) | R300_HIRES_EN);
		OUT_BATCH(0x0);

		OUT_BATCH_REGVAL(R500_RS_INST_0, R500_RS_INST_COL_CN_WRITE);
		END_BATCH();
	}

	if (!is_r500) {
		R300_STATECHANGE(r300, fp);
		R300_STATECHANGE(r300, fpi[0]);
		R300_STATECHANGE(r300, fpi[1]);
		R300_STATECHANGE(r300, fpi[2]);
		R300_STATECHANGE(r300, fpi[3]);

		BEGIN_BATCH(17);
		OUT_BATCH_REGSEQ(R300_US_CONFIG, 3);
		OUT_BATCH(0x0);
		OUT_BATCH(0x0);
		OUT_BATCH(0x0);
		OUT_BATCH_REGSEQ(R300_US_CODE_ADDR_0, 4);
		OUT_BATCH(0x0);
		OUT_BATCH(0x0);
		OUT_BATCH(0x0);
		OUT_BATCH(R300_RGBA_OUT);

		OUT_BATCH_REGVAL(R300_US_ALU_RGB_INST_0,
			FP_INSTRC(MAD, FP_ARGC(SRC0C_XYZ), FP_ARGC(ONE), FP_ARGC(ZERO)));
		OUT_BATCH_REGVAL(R300_US_ALU_RGB_ADDR_0,
			FP_SELC(0, NO, XYZ, FP_TMP(0), 0, 0));
		OUT_BATCH_REGVAL(R300_US_ALU_ALPHA_INST_0,
			FP_INSTRA(MAD, FP_ARGA(SRC0A), FP_ARGA(ONE), FP_ARGA(ZERO)));
		OUT_BATCH_REGVAL(R300_US_ALU_ALPHA_ADDR_0,
			FP_SELA(0, NO, W, FP_TMP(0), 0, 0));
		END_BATCH();
	} else {
		R300_STATECHANGE(r300, fp);
		R300_STATECHANGE(r300, r500fp);

		BEGIN_BATCH(14);
		OUT_BATCH_REGSEQ(R500_US_CONFIG, 2);
		OUT_BATCH(R500_ZERO_TIMES_ANYTHING_EQUALS_ZERO);
		OUT_BATCH(0x0);
		OUT_BATCH_REGSEQ(R500_US_CODE_ADDR, 3);
		OUT_BATCH(R500_US_CODE_START_ADDR(0) | R500_US_CODE_END_ADDR(1));
		OUT_BATCH(R500_US_CODE_RANGE_ADDR(0) | R500_US_CODE_RANGE_SIZE(1));
		OUT_BATCH(R500_US_CODE_OFFSET_ADDR(0));

		OUT_BATCH(cmdr500fp(0, 1, 0, 0));
		OUT_BATCH(R500_INST_TYPE_OUT |
			  R500_INST_TEX_SEM_WAIT |
			  R500_INST_LAST |
			  R500_INST_RGB_OMASK_R |
			  R500_INST_RGB_OMASK_G |
			  R500_INST_RGB_OMASK_B |
			  R500_INST_ALPHA_OMASK |
			  R500_INST_RGB_CLAMP |
			  R500_INST_ALPHA_CLAMP);
		OUT_BATCH(R500_RGB_ADDR0(0) |
			  R500_RGB_ADDR1(0) |
			  R500_RGB_ADDR1_CONST |
			  R500_RGB_ADDR2(0) |
			  R500_RGB_ADDR2_CONST);
		OUT_BATCH(R500_ALPHA_ADDR0(0) |
			  R500_ALPHA_ADDR1(0) |
			  R500_ALPHA_ADDR1_CONST |
			  R500_ALPHA_ADDR2(0) |
			  R500_ALPHA_ADDR2_CONST);
		OUT_BATCH(R500_ALU_RGB_SEL_A_SRC0 |
			  R500_ALU_RGB_R_SWIZ_A_R |
			  R500_ALU_RGB_G_SWIZ_A_G |
			  R500_ALU_RGB_B_SWIZ_A_B |
			  R500_ALU_RGB_SEL_B_SRC0 |
			  R500_ALU_RGB_R_SWIZ_B_R |
			  R500_ALU_RGB_B_SWIZ_B_G |
			  R500_ALU_RGB_G_SWIZ_B_B);
		OUT_BATCH(R500_ALPHA_OP_CMP |
			  R500_ALPHA_SWIZ_A_A |
			  R500_ALPHA_SWIZ_B_A);
		OUT_BATCH(R500_ALU_RGBA_OP_CMP |
			  R500_ALU_RGBA_R_SWIZ_0 |
			  R500_ALU_RGBA_G_SWIZ_0 |
			  R500_ALU_RGBA_B_SWIZ_0 |
			  R500_ALU_RGBA_A_SWIZ_0);
		END_BATCH();
	}

	BEGIN_BATCH(2);
	OUT_BATCH_REGVAL(R300_VAP_PVS_STATE_FLUSH_REG, 0);
	END_BATCH();

	if (has_tcl) {
		vap_cntl = ((10 << R300_PVS_NUM_SLOTS_SHIFT) |
			(5 << R300_PVS_NUM_CNTLRS_SHIFT) |
			(12 << R300_VF_MAX_VTX_NUM_SHIFT));
		if (r300->radeon.radeonScreen->chip_family >= CHIP_FAMILY_RV515)
			vap_cntl |= R500_TCL_STATE_OPTIMIZATION;
	} else {
		vap_cntl = ((10 << R300_PVS_NUM_SLOTS_SHIFT) |
			(5 << R300_PVS_NUM_CNTLRS_SHIFT) |
			(5 << R300_VF_MAX_VTX_NUM_SHIFT));
	}

	if (r300->radeon.radeonScreen->chip_family == CHIP_FAMILY_RV515)
		vap_cntl |= (2 << R300_PVS_NUM_FPUS_SHIFT);
	else if ((r300->radeon.radeonScreen->chip_family == CHIP_FAMILY_RV530) ||
		 (r300->radeon.radeonScreen->chip_family == CHIP_FAMILY_RV560) ||
		 (r300->radeon.radeonScreen->chip_family == CHIP_FAMILY_RV570))
		vap_cntl |= (5 << R300_PVS_NUM_FPUS_SHIFT);
	else if ((r300->radeon.radeonScreen->chip_family == CHIP_FAMILY_RV410) ||
		 (r300->radeon.radeonScreen->chip_family == CHIP_FAMILY_R420))
		vap_cntl |= (6 << R300_PVS_NUM_FPUS_SHIFT);
	else if ((r300->radeon.radeonScreen->chip_family == CHIP_FAMILY_R520) ||
		 (r300->radeon.radeonScreen->chip_family == CHIP_FAMILY_R580))
		vap_cntl |= (8 << R300_PVS_NUM_FPUS_SHIFT);
	else
		vap_cntl |= (4 << R300_PVS_NUM_FPUS_SHIFT);

	R300_STATECHANGE(r300, vap_cntl);

	BEGIN_BATCH(2);
	OUT_BATCH_REGVAL(R300_VAP_CNTL, vap_cntl);
	END_BATCH();

	if (has_tcl) {
		R300_STATECHANGE(r300, pvs);
		R300_STATECHANGE(r300, vpi);

		BEGIN_BATCH(13);
		OUT_BATCH_REGSEQ(R300_VAP_PVS_CODE_CNTL_0, 3);
		OUT_BATCH((0 << R300_PVS_FIRST_INST_SHIFT) |
			  (0 << R300_PVS_XYZW_VALID_INST_SHIFT) |
			  (1 << R300_PVS_LAST_INST_SHIFT));
		OUT_BATCH((0 << R300_PVS_CONST_BASE_OFFSET_SHIFT) |
			  (0 << R300_PVS_MAX_CONST_ADDR_SHIFT));
		OUT_BATCH(1 << R300_PVS_LAST_VTX_SRC_INST_SHIFT);

		OUT_BATCH(cmdvpu(0, 2));
		OUT_BATCH(PVS_OP_DST_OPERAND(VE_ADD, GL_FALSE, GL_FALSE, 0, 0xf, PVS_DST_REG_OUT));
		OUT_BATCH(PVS_SRC_OPERAND(0, PVS_SRC_SELECT_X, PVS_SRC_SELECT_Y, PVS_SRC_SELECT_Z, PVS_SRC_SELECT_W, PVS_SRC_REG_INPUT, VSF_FLAG_NONE));
		OUT_BATCH(PVS_SRC_OPERAND(0, PVS_SRC_SELECT_FORCE_0, PVS_SRC_SELECT_FORCE_0, PVS_SRC_SELECT_FORCE_0, PVS_SRC_SELECT_FORCE_0, PVS_SRC_REG_INPUT, VSF_FLAG_NONE));
		OUT_BATCH(0x0);

		OUT_BATCH(PVS_OP_DST_OPERAND(VE_ADD, GL_FALSE, GL_FALSE, 1, 0xf, PVS_DST_REG_OUT));
		OUT_BATCH(PVS_SRC_OPERAND(1, PVS_SRC_SELECT_X, PVS_SRC_SELECT_Y, PVS_SRC_SELECT_Z, PVS_SRC_SELECT_W, PVS_SRC_REG_INPUT, VSF_FLAG_NONE));
		OUT_BATCH(PVS_SRC_OPERAND(1, PVS_SRC_SELECT_FORCE_0, PVS_SRC_SELECT_FORCE_0, PVS_SRC_SELECT_FORCE_0, PVS_SRC_SELECT_FORCE_0, PVS_SRC_REG_INPUT, VSF_FLAG_NONE));
		OUT_BATCH(0x0);
		END_BATCH();
	}
}

/**
 * Buffer clear
 */
static void r300Clear(GLcontext * ctx, GLbitfield mask)
{
	r300ContextPtr r300 = R300_CONTEXT(ctx);
	BATCH_LOCALS(r300);
	__DRIdrawablePrivate *dPriv = r300->radeon.dri.drawable;
	int flags = 0;
	int bits = 0;
	int swapped;

	if (RADEON_DEBUG & DEBUG_IOCTL)
		fprintf(stderr, "r300Clear\n");

	{
		LOCK_HARDWARE(&r300->radeon);
		UNLOCK_HARDWARE(&r300->radeon);
		if (dPriv->numClipRects == 0)
			return;
	}

	/* Flush swtcl vertices if necessary, because we will change hardware
	 * state during clear. See also the state-related comment in
	 * r300EmitClearState.
	 */
	R300_NEWPRIM(r300);

	if (mask & BUFFER_BIT_FRONT_LEFT) {
		flags |= BUFFER_BIT_FRONT_LEFT;
		mask &= ~BUFFER_BIT_FRONT_LEFT;
	}

	if (mask & BUFFER_BIT_BACK_LEFT) {
		flags |= BUFFER_BIT_BACK_LEFT;
		mask &= ~BUFFER_BIT_BACK_LEFT;
	}

	if (mask & BUFFER_BIT_DEPTH) {
		bits |= CLEARBUFFER_DEPTH;
		mask &= ~BUFFER_BIT_DEPTH;
	}

	if ((mask & BUFFER_BIT_STENCIL) && r300->state.stencil.hw_stencil) {
		bits |= CLEARBUFFER_STENCIL;
		mask &= ~BUFFER_BIT_STENCIL;
	}

	if (mask) {
		if (RADEON_DEBUG & DEBUG_FALLBACKS)
			fprintf(stderr, "%s: swrast clear, mask: %x\n",
				__FUNCTION__, mask);
		_swrast_Clear(ctx, mask);
	}

	swapped = r300->radeon.sarea->pfCurrentPage == 1;

	/* Make sure it fits there. */
	r300EnsureCmdBufSpace(r300, 421 * 3, __FUNCTION__);
	if (flags || bits)
		r300EmitClearState(ctx);

	if (flags & BUFFER_BIT_FRONT_LEFT) {
		r300ClearBuffer(r300, bits | CLEARBUFFER_COLOR, swapped);
		bits = 0;
	}

	if (flags & BUFFER_BIT_BACK_LEFT) {
		r300ClearBuffer(r300, bits | CLEARBUFFER_COLOR, swapped ^ 1);
		bits = 0;
	}

	if (bits)
		r300ClearBuffer(r300, bits, 0);

	COMMIT_BATCH();
}

void r300Flush(GLcontext * ctx)
{
	r300ContextPtr rmesa = R300_CONTEXT(ctx);

	if (RADEON_DEBUG & DEBUG_IOCTL)
		fprintf(stderr, "%s\n", __FUNCTION__);

	if (rmesa->dma.flush)
		rmesa->dma.flush( rmesa );

	if (rmesa->cmdbuf.committed > rmesa->cmdbuf.reemit)
		r300FlushCmdBuf(rmesa, __FUNCTION__);
}

void r300RefillCurrentDmaRegion(r300ContextPtr rmesa, int size)
{
	size = MAX2(size, RADEON_BUFFER_SIZE * 16);

	if (RADEON_DEBUG & (DEBUG_IOCTL | DEBUG_DMA))
		fprintf(stderr, "%s\n", __FUNCTION__);

	if (rmesa->dma.flush) {
		rmesa->dma.flush(rmesa);
	}

	if (rmesa->dma.current) {
		dri_bo_unreference(rmesa->dma.current);
		rmesa->dma.current = 0;
	}
	if (rmesa->dma.nr_released_bufs > 4)
		r300FlushCmdBuf(rmesa, __FUNCTION__);

	rmesa->dma.current = dri_bo_alloc(&rmesa->bufmgr->base, "DMA regions",
		size, 4, DRM_BO_MEM_DMA);
	rmesa->dma.current_used = 0;
	rmesa->dma.current_vertexptr = 0;
}

/* Allocates a region from rmesa->dma.current.  If there isn't enough
 * space in current, grab a new buffer (and discard what was left of current)
 */
void r300AllocDmaRegion(r300ContextPtr rmesa,
			dri_bo **pbo, int *poffset,
			int bytes, int alignment)
{
	if (RADEON_DEBUG & DEBUG_IOCTL)
		fprintf(stderr, "%s %d\n", __FUNCTION__, bytes);

	if (rmesa->dma.flush)
		rmesa->dma.flush(rmesa);

	assert(rmesa->dma.current_used == rmesa->dma.current_vertexptr);

	alignment--;
	rmesa->dma.current_used = (rmesa->dma.current_used + alignment) & ~alignment;

	if (!rmesa->dma.current || rmesa->dma.current_used + bytes > rmesa->dma.current->size)
		r300RefillCurrentDmaRegion(rmesa, (bytes + 15) & ~15);

	*poffset = rmesa->dma.current_used;
	*pbo = rmesa->dma.current;
	dri_bo_reference(*pbo);

	/* Always align to at least 16 bytes */
	rmesa->dma.current_used = (rmesa->dma.current_used + bytes + 15) & ~15;
	rmesa->dma.current_vertexptr = rmesa->dma.current_used;

	assert(rmesa->dma.current_used <= rmesa->dma.current->size);
}

void r300InitIoctlFuncs(struct dd_function_table *functions)
{
	functions->Clear = r300Clear;
	functions->Finish = radeonFinish;
	functions->Flush = r300Flush;
}
