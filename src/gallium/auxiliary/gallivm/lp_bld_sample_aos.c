/**************************************************************************
 *
 * Copyright 2010 VMware, Inc.
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
 * IN NO EVENT SHALL VMWARE AND/OR ITS SUPPLIERS BE LIABLE FOR
 * ANY CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT,
 * TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN CONNECTION WITH THE
 * SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.
 *
 **************************************************************************/

/**
 * @file
 * Texture sampling -- SoA.
 *
 * @author Jose Fonseca <jfonseca@vmware.com>
 * @author Brian Paul <brianp@vmware.com>
 */

#include "pipe/p_defines.h"
#include "pipe/p_state.h"
#include "util/u_debug.h"
#include "util/u_dump.h"
#include "util/u_memory.h"
#include "util/u_math.h"
#include "util/u_format.h"
#include "lp_bld_debug.h"
#include "lp_bld_type.h"
#include "lp_bld_const.h"
#include "lp_bld_conv.h"
#include "lp_bld_arit.h"
#include "lp_bld_logic.h"
#include "lp_bld_swizzle.h"
#include "lp_bld_pack.h"
#include "lp_bld_flow.h"
#include "lp_bld_gather.h"
#include "lp_bld_format.h"
#include "lp_bld_sample.h"
#include "lp_bld_sample_aos.h"
#include "lp_bld_quad.h"


/**
 * Build LLVM code for texture coord wrapping, for nearest filtering,
 * for scaled integer texcoords.
 * \param block_length  is the length of the pixel block along the
 *                      coordinate axis
 * \param coord  the incoming texcoord (s,t,r or q) scaled to the texture size
 * \param length  the texture size along one dimension
 * \param stride  pixel stride along the coordinate axis (in bytes)
 * \param is_pot  if TRUE, length is a power of two
 * \param wrap_mode  one of PIPE_TEX_WRAP_x
 * \param out_offset  byte offset for the wrapped coordinate
 * \param out_i  resulting sub-block pixel coordinate for coord0
 */
static void
lp_build_sample_wrap_nearest_int(struct lp_build_sample_context *bld,
                                 unsigned block_length,
                                 LLVMValueRef coord,
                                 LLVMValueRef length,
                                 LLVMValueRef stride,
                                 boolean is_pot,
                                 unsigned wrap_mode,
                                 LLVMValueRef *out_offset,
                                 LLVMValueRef *out_i)
{
   struct lp_build_context *uint_coord_bld = &bld->uint_coord_bld;
   struct lp_build_context *int_coord_bld = &bld->int_coord_bld;
   LLVMValueRef length_minus_one;

   length_minus_one = lp_build_sub(uint_coord_bld, length, uint_coord_bld->one);

   switch(wrap_mode) {
   case PIPE_TEX_WRAP_REPEAT:
      if(is_pot)
         coord = LLVMBuildAnd(bld->builder, coord, length_minus_one, "");
      else {
         /* Add a bias to the texcoord to handle negative coords */
         LLVMValueRef bias = lp_build_mul_imm(uint_coord_bld, length, 1024);
         coord = LLVMBuildAdd(bld->builder, coord, bias, "");
         coord = LLVMBuildURem(bld->builder, coord, length, "");
      }
      break;

   case PIPE_TEX_WRAP_CLAMP_TO_EDGE:
      coord = lp_build_max(int_coord_bld, coord, int_coord_bld->zero);
      coord = lp_build_min(int_coord_bld, coord, length_minus_one);
      break;

   case PIPE_TEX_WRAP_CLAMP:
   case PIPE_TEX_WRAP_CLAMP_TO_BORDER:
   case PIPE_TEX_WRAP_MIRROR_REPEAT:
   case PIPE_TEX_WRAP_MIRROR_CLAMP:
   case PIPE_TEX_WRAP_MIRROR_CLAMP_TO_EDGE:
   case PIPE_TEX_WRAP_MIRROR_CLAMP_TO_BORDER:
   default:
      assert(0);
   }

   lp_build_sample_partial_offset(uint_coord_bld, block_length, coord, stride,
                                  out_offset, out_i);
}


/**
 * Build LLVM code for texture coord wrapping, for linear filtering,
 * for scaled integer texcoords.
 * \param block_length  is the length of the pixel block along the
 *                      coordinate axis
 * \param coord0  the incoming texcoord (s,t,r or q) scaled to the texture size
 * \param length  the texture size along one dimension
 * \param stride  pixel stride along the coordinate axis (in bytes)
 * \param is_pot  if TRUE, length is a power of two
 * \param wrap_mode  one of PIPE_TEX_WRAP_x
 * \param offset0  resulting relative offset for coord0
 * \param offset1  resulting relative offset for coord0 + 1
 * \param i0  resulting sub-block pixel coordinate for coord0
 * \param i1  resulting sub-block pixel coordinate for coord0 + 1
 */
static void
lp_build_sample_wrap_linear_int(struct lp_build_sample_context *bld,
                                unsigned block_length,
                                LLVMValueRef coord0,
                                LLVMValueRef length,
                                LLVMValueRef stride,
                                boolean is_pot,
                                unsigned wrap_mode,
                                LLVMValueRef *offset0,
                                LLVMValueRef *offset1,
                                LLVMValueRef *i0,
                                LLVMValueRef *i1)
{
   struct lp_build_context *uint_coord_bld = &bld->uint_coord_bld;
   struct lp_build_context *int_coord_bld = &bld->int_coord_bld;
   LLVMValueRef length_minus_one;
   LLVMValueRef lmask, umask, mask;

   if (block_length != 1) {
      /*
       * If the pixel block covers more than one pixel then there is no easy
       * way to calculate offset1 relative to offset0. Instead, compute them
       * independently.
       */

      LLVMValueRef coord1;

      lp_build_sample_wrap_nearest_int(bld,
                                       block_length,
                                       coord0,
                                       length,
                                       stride,
                                       is_pot,
                                       wrap_mode,
                                       offset0, i0);

      coord1 = lp_build_add(int_coord_bld, coord0, int_coord_bld->one);

      lp_build_sample_wrap_nearest_int(bld,
                                       block_length,
                                       coord1,
                                       length,
                                       stride,
                                       is_pot,
                                       wrap_mode,
                                       offset1, i1);

      return;
   }

   /*
    * Scalar pixels -- try to compute offset0 and offset1 with a single stride
    * multiplication.
    */

   *i0 = uint_coord_bld->zero;
   *i1 = uint_coord_bld->zero;

   length_minus_one = lp_build_sub(int_coord_bld, length, int_coord_bld->one);

   switch(wrap_mode) {
   case PIPE_TEX_WRAP_REPEAT:
      if (is_pot) {
         coord0 = LLVMBuildAnd(bld->builder, coord0, length_minus_one, "");
      }
      else {
         /* Add a bias to the texcoord to handle negative coords */
         LLVMValueRef bias = lp_build_mul_imm(uint_coord_bld, length, 1024);
         coord0 = LLVMBuildAdd(bld->builder, coord0, bias, "");
         coord0 = LLVMBuildURem(bld->builder, coord0, length, "");
      }

      mask = lp_build_compare(bld->builder, int_coord_bld->type,
                              PIPE_FUNC_NOTEQUAL, coord0, length_minus_one);

      *offset0 = lp_build_mul(uint_coord_bld, coord0, stride);
      *offset1 = LLVMBuildAnd(bld->builder,
                              lp_build_add(uint_coord_bld, *offset0, stride),
                              mask, "");
      break;

   case PIPE_TEX_WRAP_CLAMP_TO_EDGE:
      lmask = lp_build_compare(int_coord_bld->builder, int_coord_bld->type,
                               PIPE_FUNC_GEQUAL, coord0, int_coord_bld->zero);
      umask = lp_build_compare(int_coord_bld->builder, int_coord_bld->type,
                               PIPE_FUNC_LESS, coord0, length_minus_one);

      coord0 = lp_build_select(int_coord_bld, lmask, coord0, int_coord_bld->zero);
      coord0 = lp_build_select(int_coord_bld, umask, coord0, length_minus_one);

      mask = LLVMBuildAnd(bld->builder, lmask, umask, "");

      *offset0 = lp_build_mul(uint_coord_bld, coord0, stride);
      *offset1 = lp_build_add(uint_coord_bld,
                              *offset0,
                              LLVMBuildAnd(bld->builder, stride, mask, ""));
      break;

   case PIPE_TEX_WRAP_CLAMP:
   case PIPE_TEX_WRAP_CLAMP_TO_BORDER:
   case PIPE_TEX_WRAP_MIRROR_REPEAT:
   case PIPE_TEX_WRAP_MIRROR_CLAMP:
   case PIPE_TEX_WRAP_MIRROR_CLAMP_TO_EDGE:
   case PIPE_TEX_WRAP_MIRROR_CLAMP_TO_BORDER:
   default:
      assert(0);
      *offset0 = uint_coord_bld->zero;
      *offset1 = uint_coord_bld->zero;
      break;
   }
}


/**
 * Sample a single texture image with nearest sampling.
 * If sampling a cube texture, r = cube face in [0,5].
 * Return filtered color as two vectors of 16-bit fixed point values.
 */
static void
lp_build_sample_image_nearest(struct lp_build_sample_context *bld,
                              LLVMValueRef width_vec,
                              LLVMValueRef height_vec,
                              LLVMValueRef depth_vec,
                              LLVMValueRef row_stride_vec,
                              LLVMValueRef img_stride_vec,
                              LLVMValueRef data_ptr,
                              LLVMValueRef s,
                              LLVMValueRef t,
                              LLVMValueRef r,
                              LLVMValueRef *colors_lo,
                              LLVMValueRef *colors_hi)
{
   const int dims = texture_dims(bld->static_state->target);
   LLVMBuilderRef builder = bld->builder;
   struct lp_build_context i32, h16, u8n;
   LLVMTypeRef i32_vec_type, h16_vec_type, u8n_vec_type;
   LLVMValueRef i32_c8;
   LLVMValueRef s_ipart, t_ipart, r_ipart;
   LLVMValueRef x_stride;
   LLVMValueRef x_offset, offset;
   LLVMValueRef x_subcoord, y_subcoord, z_subcoord;

   lp_build_context_init(&i32, builder, lp_type_int_vec(32));
   lp_build_context_init(&h16, builder, lp_type_ufixed(16));
   lp_build_context_init(&u8n, builder, lp_type_unorm(8));

   i32_vec_type = lp_build_vec_type(i32.type);
   h16_vec_type = lp_build_vec_type(h16.type);
   u8n_vec_type = lp_build_vec_type(u8n.type);

   if (bld->static_state->normalized_coords) {
      /* s = s * width, t = t * height */
      LLVMTypeRef coord_vec_type = lp_build_vec_type(bld->coord_type);
      LLVMValueRef fp_width = LLVMBuildSIToFP(bld->builder, width_vec,
                                              coord_vec_type, "");
      s = lp_build_mul(&bld->coord_bld, s, fp_width);
      if (dims >= 2) {
         LLVMValueRef fp_height = LLVMBuildSIToFP(bld->builder, height_vec,
                                                  coord_vec_type, "");
         t = lp_build_mul(&bld->coord_bld, t, fp_height);
         if (dims >= 3) {
            LLVMValueRef fp_depth = LLVMBuildSIToFP(bld->builder, depth_vec,
                                                    coord_vec_type, "");
            r = lp_build_mul(&bld->coord_bld, r, fp_depth);
         }
      }
   }

   /* scale coords by 256 (8 fractional bits) */
   s = lp_build_mul_imm(&bld->coord_bld, s, 256);
   if (dims >= 2)
      t = lp_build_mul_imm(&bld->coord_bld, t, 256);
   if (dims >= 3)
      r = lp_build_mul_imm(&bld->coord_bld, r, 256);

   /* convert float to int */
   s = LLVMBuildFPToSI(builder, s, i32_vec_type, "");
   if (dims >= 2)
      t = LLVMBuildFPToSI(builder, t, i32_vec_type, "");
   if (dims >= 3)
      r = LLVMBuildFPToSI(builder, r, i32_vec_type, "");

   /* compute floor (shift right 8) */
   i32_c8 = lp_build_const_int_vec(i32.type, 8);
   s_ipart = LLVMBuildAShr(builder, s, i32_c8, "");
   if (dims >= 2)
      t_ipart = LLVMBuildAShr(builder, t, i32_c8, "");
   if (dims >= 3)
      r_ipart = LLVMBuildAShr(builder, r, i32_c8, "");

   /* get pixel, row, image strides */
   x_stride = lp_build_const_vec(bld->uint_coord_bld.type,
                                 bld->format_desc->block.bits/8);

   /* Do texcoord wrapping, compute texel offset */
   lp_build_sample_wrap_nearest_int(bld,
                                    bld->format_desc->block.width,
                                    s_ipart, width_vec, x_stride,
                                    bld->static_state->pot_width,
                                    bld->static_state->wrap_s,
                                    &x_offset, &x_subcoord);
   offset = x_offset;
   if (dims >= 2) {
      LLVMValueRef y_offset;
      lp_build_sample_wrap_nearest_int(bld,
                                       bld->format_desc->block.height,
                                       t_ipart, height_vec, row_stride_vec,
                                       bld->static_state->pot_height,
                                       bld->static_state->wrap_t,
                                       &y_offset, &y_subcoord);
      offset = lp_build_add(&bld->uint_coord_bld, offset, y_offset);
      if (dims >= 3) {
         LLVMValueRef z_offset;
         lp_build_sample_wrap_nearest_int(bld,
                                          1, /* block length (depth) */
                                          r_ipart, depth_vec, img_stride_vec,
                                          bld->static_state->pot_height,
                                          bld->static_state->wrap_r,
                                          &z_offset, &z_subcoord);
         offset = lp_build_add(&bld->uint_coord_bld, offset, z_offset);
      }
      else if (bld->static_state->target == PIPE_TEXTURE_CUBE) {
         LLVMValueRef z_offset;
         /* The r coord is the cube face in [0,5] */
         z_offset = lp_build_mul(&bld->uint_coord_bld, r, img_stride_vec);
         offset = lp_build_add(&bld->uint_coord_bld, offset, z_offset);
      }
   }

   /*
    * Fetch the pixels as 4 x 32bit (rgba order might differ):
    *
    *   rgba0 rgba1 rgba2 rgba3
    *
    * bit cast them into 16 x u8
    *
    *   r0 g0 b0 a0 r1 g1 b1 a1 r2 g2 b2 a2 r3 g3 b3 a3
    *
    * unpack them into two 8 x i16:
    *
    *   r0 g0 b0 a0 r1 g1 b1 a1
    *   r2 g2 b2 a2 r3 g3 b3 a3
    *
    * The higher 8 bits of the resulting elements will be zero.
    */
   {
      LLVMValueRef rgba8;

      if (util_format_is_rgba8_variant(bld->format_desc)) {
         /*
          * Given the format is a rgba8, just read the pixels as is,
          * without any swizzling. Swizzling will be done later.
          */
         rgba8 = lp_build_gather(bld->builder,
                                 bld->texel_type.length,
                                 bld->format_desc->block.bits,
                                 bld->texel_type.width,
                                 data_ptr, offset);

         rgba8 = LLVMBuildBitCast(builder, rgba8, u8n_vec_type, "");
      }
      else {
         rgba8 = lp_build_fetch_rgba_aos(bld->builder,
                                         bld->format_desc,
                                         u8n.type,
                                         data_ptr, offset,
                                         x_subcoord,
                                         y_subcoord);
      }

      /* Expand one 4*rgba8 to two 2*rgba16 */
      lp_build_unpack2(builder, u8n.type, h16.type,
                       rgba8,
                       colors_lo, colors_hi);
   }
}


/**
 * Sample a single texture image with (bi-)(tri-)linear sampling.
 * Return filtered color as two vectors of 16-bit fixed point values.
 */
static void
lp_build_sample_image_linear(struct lp_build_sample_context *bld,
                             LLVMValueRef width_vec,
                             LLVMValueRef height_vec,
                             LLVMValueRef depth_vec,
                             LLVMValueRef row_stride_vec,
                             LLVMValueRef img_stride_vec,
                             LLVMValueRef data_ptr,
                             LLVMValueRef s,
                             LLVMValueRef t,
                             LLVMValueRef r,
                             LLVMValueRef *colors_lo,
                             LLVMValueRef *colors_hi)
{
   const int dims = texture_dims(bld->static_state->target);
   LLVMBuilderRef builder = bld->builder;
   struct lp_build_context i32, h16, u8n;
   LLVMTypeRef i32_vec_type, h16_vec_type, u8n_vec_type;
   LLVMValueRef i32_c8, i32_c128, i32_c255;
   LLVMValueRef s_ipart, s_fpart, s_fpart_lo, s_fpart_hi;
   LLVMValueRef t_ipart, t_fpart, t_fpart_lo, t_fpart_hi;
   LLVMValueRef r_ipart, r_fpart, r_fpart_lo, r_fpart_hi;
   LLVMValueRef x_stride, y_stride, z_stride;
   LLVMValueRef x_offset0, x_offset1;
   LLVMValueRef y_offset0, y_offset1;
   LLVMValueRef z_offset0, z_offset1;
   LLVMValueRef offset[2][2][2]; /* [z][y][x] */
   LLVMValueRef x_subcoord[2], y_subcoord[2], z_subcoord[2];
   LLVMValueRef neighbors_lo[2][2][2]; /* [z][y][x] */
   LLVMValueRef neighbors_hi[2][2][2]; /* [z][y][x] */
   LLVMValueRef packed_lo, packed_hi;
   unsigned x, y, z;
   unsigned i, j, k;
   unsigned numj, numk;

   lp_build_context_init(&i32, builder, lp_type_int_vec(32));
   lp_build_context_init(&h16, builder, lp_type_ufixed(16));
   lp_build_context_init(&u8n, builder, lp_type_unorm(8));

   i32_vec_type = lp_build_vec_type(i32.type);
   h16_vec_type = lp_build_vec_type(h16.type);
   u8n_vec_type = lp_build_vec_type(u8n.type);

   if (bld->static_state->normalized_coords) {
      /* s = s * width, t = t * height */
      LLVMTypeRef coord_vec_type = lp_build_vec_type(bld->coord_type);
      LLVMValueRef fp_width = LLVMBuildSIToFP(bld->builder, width_vec,
                                              coord_vec_type, "");
      s = lp_build_mul(&bld->coord_bld, s, fp_width);
      if (dims >= 2) {
         LLVMValueRef fp_height = LLVMBuildSIToFP(bld->builder, height_vec,
                                                  coord_vec_type, "");
         t = lp_build_mul(&bld->coord_bld, t, fp_height);
      }
      if (dims >= 3) {
         LLVMValueRef fp_depth = LLVMBuildSIToFP(bld->builder, depth_vec,
                                                 coord_vec_type, "");
         r = lp_build_mul(&bld->coord_bld, r, fp_depth);
      }
   }

   /* scale coords by 256 (8 fractional bits) */
   s = lp_build_mul_imm(&bld->coord_bld, s, 256);
   if (dims >= 2)
      t = lp_build_mul_imm(&bld->coord_bld, t, 256);
   if (dims >= 3)
      r = lp_build_mul_imm(&bld->coord_bld, r, 256);

   /* convert float to int */
   s = LLVMBuildFPToSI(builder, s, i32_vec_type, "");
   if (dims >= 2)
      t = LLVMBuildFPToSI(builder, t, i32_vec_type, "");
   if (dims >= 3)
      r = LLVMBuildFPToSI(builder, r, i32_vec_type, "");

   /* subtract 0.5 (add -128) */
   i32_c128 = lp_build_const_int_vec(i32.type, -128);
   s = LLVMBuildAdd(builder, s, i32_c128, "");
   if (dims >= 2) {
      t = LLVMBuildAdd(builder, t, i32_c128, "");
   }
   if (dims >= 3) {
      r = LLVMBuildAdd(builder, r, i32_c128, "");
   }

   /* compute floor (shift right 8) */
   i32_c8 = lp_build_const_int_vec(i32.type, 8);
   s_ipart = LLVMBuildAShr(builder, s, i32_c8, "");
   if (dims >= 2)
      t_ipart = LLVMBuildAShr(builder, t, i32_c8, "");
   if (dims >= 3)
      r_ipart = LLVMBuildAShr(builder, r, i32_c8, "");

   /* compute fractional part (AND with 0xff) */
   i32_c255 = lp_build_const_int_vec(i32.type, 255);
   s_fpart = LLVMBuildAnd(builder, s, i32_c255, "");
   if (dims >= 2)
      t_fpart = LLVMBuildAnd(builder, t, i32_c255, "");
   if (dims >= 3)
      r_fpart = LLVMBuildAnd(builder, r, i32_c255, "");

   /* get pixel, row and image strides */
   x_stride = lp_build_const_vec(bld->uint_coord_bld.type,
                                 bld->format_desc->block.bits/8);
   y_stride = row_stride_vec;
   z_stride = img_stride_vec;

   /* do texcoord wrapping and compute texel offsets */
   lp_build_sample_wrap_linear_int(bld,
                                   bld->format_desc->block.width,
                                   s_ipart, width_vec, x_stride,
                                   bld->static_state->pot_width,
                                   bld->static_state->wrap_s,
                                   &x_offset0, &x_offset1,
                                   &x_subcoord[0], &x_subcoord[1]);
   for (z = 0; z < 2; z++) {
      for (y = 0; y < 2; y++) {
         offset[z][y][0] = x_offset0;
         offset[z][y][1] = x_offset1;
      }
   }

   if (dims >= 2) {
      lp_build_sample_wrap_linear_int(bld,
                                      bld->format_desc->block.height,
                                      t_ipart, height_vec, y_stride,
                                      bld->static_state->pot_height,
                                      bld->static_state->wrap_t,
                                      &y_offset0, &y_offset1,
                                      &y_subcoord[0], &y_subcoord[1]);

      for (z = 0; z < 2; z++) {
         for (x = 0; x < 2; x++) {
            offset[z][0][x] = lp_build_add(&bld->uint_coord_bld,
                                           offset[z][0][x], y_offset0);
            offset[z][1][x] = lp_build_add(&bld->uint_coord_bld,
                                           offset[z][1][x], y_offset1);
         }
      }
   }

   if (dims >= 3) {
      lp_build_sample_wrap_linear_int(bld,
                                      bld->format_desc->block.height,
                                      r_ipart, depth_vec, z_stride,
                                      bld->static_state->pot_depth,
                                      bld->static_state->wrap_r,
                                      &z_offset0, &z_offset1,
                                      &z_subcoord[0], &z_subcoord[1]);
      for (y = 0; y < 2; y++) {
         for (x = 0; x < 2; x++) {
            offset[0][y][x] = lp_build_add(&bld->uint_coord_bld,
                                           offset[0][y][x], z_offset0);
            offset[1][y][x] = lp_build_add(&bld->uint_coord_bld,
                                           offset[1][y][x], z_offset1);
         }
      }
   }
   else if (bld->static_state->target == PIPE_TEXTURE_CUBE) {
      LLVMValueRef z_offset;
      z_offset = lp_build_mul(&bld->uint_coord_bld, r, img_stride_vec);
      for (y = 0; y < 2; y++) {
         for (x = 0; x < 2; x++) {
            /* The r coord is the cube face in [0,5] */
            offset[0][y][x] = lp_build_add(&bld->uint_coord_bld,
                                           offset[0][y][x], z_offset);
         }
      }
   }

   /*
    * Transform 4 x i32 in
    *
    *   s_fpart = {s0, s1, s2, s3}
    *
    * into 8 x i16
    *
    *   s_fpart = {00, s0, 00, s1, 00, s2, 00, s3}
    *
    * into two 8 x i16
    *
    *   s_fpart_lo = {s0, s0, s0, s0, s1, s1, s1, s1}
    *   s_fpart_hi = {s2, s2, s2, s2, s3, s3, s3, s3}
    *
    * and likewise for t_fpart. There is no risk of loosing precision here
    * since the fractional parts only use the lower 8bits.
    */
   s_fpart = LLVMBuildBitCast(builder, s_fpart, h16_vec_type, "");
   if (dims >= 2)
      t_fpart = LLVMBuildBitCast(builder, t_fpart, h16_vec_type, "");
   if (dims >= 3)
      r_fpart = LLVMBuildBitCast(builder, r_fpart, h16_vec_type, "");

   {
      LLVMTypeRef elem_type = LLVMInt32TypeInContext(LC);
      LLVMValueRef shuffles_lo[LP_MAX_VECTOR_LENGTH];
      LLVMValueRef shuffles_hi[LP_MAX_VECTOR_LENGTH];
      LLVMValueRef shuffle_lo;
      LLVMValueRef shuffle_hi;

      for (j = 0; j < h16.type.length; j += 4) {
#ifdef PIPE_ARCH_LITTLE_ENDIAN
         unsigned subindex = 0;
#else
         unsigned subindex = 1;
#endif
         LLVMValueRef index;

         index = LLVMConstInt(elem_type, j/2 + subindex, 0);
         for (i = 0; i < 4; ++i)
            shuffles_lo[j + i] = index;

         index = LLVMConstInt(elem_type, h16.type.length/2 + j/2 + subindex, 0);
         for (i = 0; i < 4; ++i)
            shuffles_hi[j + i] = index;
      }

      shuffle_lo = LLVMConstVector(shuffles_lo, h16.type.length);
      shuffle_hi = LLVMConstVector(shuffles_hi, h16.type.length);

      s_fpart_lo = LLVMBuildShuffleVector(builder, s_fpart, h16.undef,
                                          shuffle_lo, "");
      s_fpart_hi = LLVMBuildShuffleVector(builder, s_fpart, h16.undef,
                                          shuffle_hi, "");
      if (dims >= 2) {
         t_fpart_lo = LLVMBuildShuffleVector(builder, t_fpart, h16.undef,
                                             shuffle_lo, "");
         t_fpart_hi = LLVMBuildShuffleVector(builder, t_fpart, h16.undef,
                                             shuffle_hi, "");
      }
      if (dims >= 3) {
         r_fpart_lo = LLVMBuildShuffleVector(builder, r_fpart, h16.undef,
                                             shuffle_lo, "");
         r_fpart_hi = LLVMBuildShuffleVector(builder, r_fpart, h16.undef,
                                             shuffle_hi, "");
      }
   }

   /*
    * Fetch the pixels as 4 x 32bit (rgba order might differ):
    *
    *   rgba0 rgba1 rgba2 rgba3
    *
    * bit cast them into 16 x u8
    *
    *   r0 g0 b0 a0 r1 g1 b1 a1 r2 g2 b2 a2 r3 g3 b3 a3
    *
    * unpack them into two 8 x i16:
    *
    *   r0 g0 b0 a0 r1 g1 b1 a1
    *   r2 g2 b2 a2 r3 g3 b3 a3
    *
    * The higher 8 bits of the resulting elements will be zero.
    */
   numj = 1 + (dims >= 2);
   numk = 1 + (dims >= 3);

   for (k = 0; k < numk; k++) {
      for (j = 0; j < numj; j++) {
         for (i = 0; i < 2; i++) {
            LLVMValueRef rgba8;

            if (util_format_is_rgba8_variant(bld->format_desc)) {
               /*
                * Given the format is a rgba8, just read the pixels as is,
                * without any swizzling. Swizzling will be done later.
                */
               rgba8 = lp_build_gather(bld->builder,
                                       bld->texel_type.length,
                                       bld->format_desc->block.bits,
                                       bld->texel_type.width,
                                       data_ptr, offset[k][j][i]);

               rgba8 = LLVMBuildBitCast(builder, rgba8, u8n_vec_type, "");
            }
            else {
               rgba8 = lp_build_fetch_rgba_aos(bld->builder,
                                               bld->format_desc,
                                               u8n.type,
                                               data_ptr, offset[k][j][i],
                                               x_subcoord[i],
                                               y_subcoord[j]);
            }

            /* Expand one 4*rgba8 to two 2*rgba16 */
            lp_build_unpack2(builder, u8n.type, h16.type,
                             rgba8,
                             &neighbors_lo[k][j][i], &neighbors_hi[k][j][i]);
         }
      }
   }

   /*
    * Linear interpolation with 8.8 fixed point.
    */
   if (dims == 1) {
      /* 1-D lerp */
      packed_lo = lp_build_lerp(&h16,
				s_fpart_lo,
				neighbors_lo[0][0][0],
				neighbors_lo[0][0][1]);

      packed_hi = lp_build_lerp(&h16,
				s_fpart_hi,
				neighbors_hi[0][0][0],
				neighbors_hi[0][0][1]);
   }
   else {
      /* 2-D lerp */
      packed_lo = lp_build_lerp_2d(&h16,
				   s_fpart_lo, t_fpart_lo,
				   neighbors_lo[0][0][0],
				   neighbors_lo[0][0][1],
				   neighbors_lo[0][1][0],
				   neighbors_lo[0][1][1]);

      packed_hi = lp_build_lerp_2d(&h16,
				   s_fpart_hi, t_fpart_hi,
				   neighbors_hi[0][0][0],
				   neighbors_hi[0][0][1],
				   neighbors_hi[0][1][0],
				   neighbors_hi[0][1][1]);

      if (dims >= 3) {
	 LLVMValueRef packed_lo2, packed_hi2;

	 /* lerp in the second z slice */
	 packed_lo2 = lp_build_lerp_2d(&h16,
				       s_fpart_lo, t_fpart_lo,
				       neighbors_lo[1][0][0],
				       neighbors_lo[1][0][1],
				       neighbors_lo[1][1][0],
				       neighbors_lo[1][1][1]);

	 packed_hi2 = lp_build_lerp_2d(&h16,
				       s_fpart_hi, t_fpart_hi,
				       neighbors_hi[1][0][0],
				       neighbors_hi[1][0][1],
				       neighbors_hi[1][1][0],
				       neighbors_hi[1][1][1]);
	 /* interp between two z slices */
	 packed_lo = lp_build_lerp(&h16, r_fpart_lo,
				   packed_lo, packed_lo2);
	 packed_hi = lp_build_lerp(&h16, r_fpart_hi,
				   packed_hi, packed_hi2);
      }
   }

   *colors_lo = packed_lo;
   *colors_hi = packed_hi;
}


/**
 * Sample the texture/mipmap using given image filter and mip filter.
 * data0_ptr and data1_ptr point to the two mipmap levels to sample
 * from.  width0/1_vec, height0/1_vec, depth0/1_vec indicate their sizes.
 * If we're using nearest miplevel sampling the '1' values will be null/unused.
 */
static void
lp_build_sample_mipmap(struct lp_build_sample_context *bld,
                       unsigned img_filter,
                       unsigned mip_filter,
                       LLVMValueRef s,
                       LLVMValueRef t,
                       LLVMValueRef r,
                       LLVMValueRef lod_fpart,
                       LLVMValueRef width0_vec,
                       LLVMValueRef width1_vec,
                       LLVMValueRef height0_vec,
                       LLVMValueRef height1_vec,
                       LLVMValueRef depth0_vec,
                       LLVMValueRef depth1_vec,
                       LLVMValueRef row_stride0_vec,
                       LLVMValueRef row_stride1_vec,
                       LLVMValueRef img_stride0_vec,
                       LLVMValueRef img_stride1_vec,
                       LLVMValueRef data_ptr0,
                       LLVMValueRef data_ptr1,
                       LLVMValueRef *colors_lo,
                       LLVMValueRef *colors_hi)
{
   LLVMValueRef colors0_lo, colors0_hi;
   LLVMValueRef colors1_lo, colors1_hi;

   if (img_filter == PIPE_TEX_FILTER_NEAREST) {
      /* sample the first mipmap level */
      lp_build_sample_image_nearest(bld,
                                    width0_vec, height0_vec, depth0_vec,
                                    row_stride0_vec, img_stride0_vec,
                                    data_ptr0, s, t, r,
                                    &colors0_lo, &colors0_hi);

      if (mip_filter == PIPE_TEX_MIPFILTER_LINEAR) {
         /* sample the second mipmap level */
         lp_build_sample_image_nearest(bld,
                                       width1_vec, height1_vec, depth1_vec,
                                       row_stride1_vec, img_stride1_vec,
                                       data_ptr1, s, t, r,
                                       &colors1_lo, &colors1_hi);
      }
   }
   else {
      assert(img_filter == PIPE_TEX_FILTER_LINEAR);

      /* sample the first mipmap level */
      lp_build_sample_image_linear(bld,
                                   width0_vec, height0_vec, depth0_vec,
                                   row_stride0_vec, img_stride0_vec,
                                   data_ptr0, s, t, r,
                                   &colors0_lo, &colors0_hi);

      if (mip_filter == PIPE_TEX_MIPFILTER_LINEAR) {
         /* sample the second mipmap level */
         lp_build_sample_image_linear(bld,
                                      width1_vec, height1_vec, depth1_vec,
                                      row_stride1_vec, img_stride1_vec,
                                      data_ptr1, s, t, r,
                                      &colors1_lo, &colors1_hi);
      }
   }

   if (mip_filter == PIPE_TEX_MIPFILTER_LINEAR) {
      /* interpolate samples from the two mipmap levels */
      struct lp_build_context h16;
      lp_build_context_init(&h16, bld->builder, lp_type_ufixed(16));

      *colors_lo = lp_build_lerp(&h16, lod_fpart,
                                 colors0_lo, colors1_lo);
      *colors_hi = lp_build_lerp(&h16, lod_fpart,
                                 colors0_hi, colors1_hi);
   }
   else {
      /* use first/only level's colors */
      *colors_lo = colors0_lo;
      *colors_hi = colors0_hi;
   }
}



/**
 * Texture sampling in AoS format.  Used when sampling common 32-bit/texel
 * formats.  1D/2D/3D/cube texture supported.  All mipmap sampling modes
 * but only limited texture coord wrap modes.
 */
void
lp_build_sample_aos(struct lp_build_sample_context *bld,
                    unsigned unit,
                    LLVMValueRef s,
                    LLVMValueRef t,
                    LLVMValueRef r,
                    const LLVMValueRef *ddx,
                    const LLVMValueRef *ddy,
                    LLVMValueRef lod_bias, /* optional */
                    LLVMValueRef explicit_lod, /* optional */
                    LLVMValueRef width,
                    LLVMValueRef height,
                    LLVMValueRef depth,
                    LLVMValueRef width_vec,
                    LLVMValueRef height_vec,
                    LLVMValueRef depth_vec,
                    LLVMValueRef row_stride_array,
                    LLVMValueRef img_stride_array,
                    LLVMValueRef data_array,
                    LLVMValueRef texel_out[4])
{
   struct lp_build_context *float_bld = &bld->float_bld;
   LLVMBuilderRef builder = bld->builder;
   const unsigned mip_filter = bld->static_state->min_mip_filter;
   const unsigned min_filter = bld->static_state->min_img_filter;
   const unsigned mag_filter = bld->static_state->mag_img_filter;
   const int dims = texture_dims(bld->static_state->target);
   LLVMValueRef lod = NULL, lod_fpart = NULL;
   LLVMValueRef ilevel0, ilevel1 = NULL;
   LLVMValueRef width0_vec = NULL, height0_vec = NULL, depth0_vec = NULL;
   LLVMValueRef width1_vec = NULL, height1_vec = NULL, depth1_vec = NULL;
   LLVMValueRef row_stride0_vec = NULL, row_stride1_vec = NULL;
   LLVMValueRef img_stride0_vec = NULL, img_stride1_vec = NULL;
   LLVMValueRef data_ptr0, data_ptr1 = NULL;
   LLVMValueRef packed, packed_lo, packed_hi;
   LLVMValueRef unswizzled[4];
   LLVMValueRef face_ddx[4], face_ddy[4];
   struct lp_build_context h16;
   LLVMTypeRef h16_vec_type;

   /* we only support the common/simple wrap modes at this time */
   assert(lp_is_simple_wrap_mode(bld->static_state->wrap_s));
   if (dims >= 2)
      assert(lp_is_simple_wrap_mode(bld->static_state->wrap_t));
   if (dims >= 3)
      assert(lp_is_simple_wrap_mode(bld->static_state->wrap_r));


   /* make 16-bit fixed-pt builder context */
   lp_build_context_init(&h16, builder, lp_type_ufixed(16));
   h16_vec_type = lp_build_vec_type(h16.type);


   /* cube face selection, compute pre-face coords, etc. */
   if (bld->static_state->target == PIPE_TEXTURE_CUBE) {
      LLVMValueRef face, face_s, face_t;
      lp_build_cube_lookup(bld, s, t, r, &face, &face_s, &face_t);
      s = face_s; /* vec */
      t = face_t; /* vec */
      /* use 'r' to indicate cube face */
      r = lp_build_broadcast_scalar(&bld->int_coord_bld, face); /* vec */

      /* recompute ddx, ddy using the new (s,t) face texcoords */
      face_ddx[0] = lp_build_ddx(&bld->coord_bld, s);
      face_ddx[1] = lp_build_ddx(&bld->coord_bld, t);
      face_ddx[2] = NULL;
      face_ddx[3] = NULL;
      face_ddy[0] = lp_build_ddy(&bld->coord_bld, s);
      face_ddy[1] = lp_build_ddy(&bld->coord_bld, t);
      face_ddy[2] = NULL;
      face_ddy[3] = NULL;
      ddx = face_ddx;
      ddy = face_ddy;
   }


   /*
    * Compute the level of detail (float).
    */
   if (min_filter != mag_filter ||
       mip_filter != PIPE_TEX_MIPFILTER_NONE) {
      /* Need to compute lod either to choose mipmap levels or to
       * distinguish between minification/magnification with one mipmap level.
       */
      lod = lp_build_lod_selector(bld, unit, ddx, ddy,
                                  lod_bias, explicit_lod,
                                  width, height, depth);
   }

   /*
    * Compute integer mipmap level(s) to fetch texels from: ilevel0, ilevel1
    * If mipfilter=linear, also compute the weight between the two
    * mipmap levels: lod_fpart
    */
   switch (mip_filter) {
   default:
      assert(0 && "bad mip_filter value in lp_build_sample_aos()");
      /* fall-through */
   case PIPE_TEX_MIPFILTER_NONE:
      /* always use mip level 0 */
      if (bld->static_state->target == PIPE_TEXTURE_CUBE) {
         /* XXX this is a work-around for an apparent bug in LLVM 2.7.
          * We should be able to set ilevel0 = const(0) but that causes
          * bad x86 code to be emitted.
          */
         lod = lp_build_const_elem(bld->coord_bld.type, 0.0);
         lp_build_nearest_mip_level(bld, unit, lod, &ilevel0);
      }
      else {
         ilevel0 = lp_build_const_int32(0);
      }
      break;
   case PIPE_TEX_MIPFILTER_NEAREST:
      assert(lod);
      lp_build_nearest_mip_level(bld, unit, lod, &ilevel0);
      break;
   case PIPE_TEX_MIPFILTER_LINEAR:
      {
         LLVMValueRef f256 = lp_build_const_float(256.0);
         LLVMValueRef i255 = lp_build_const_int32(255);
         LLVMTypeRef i16_type = LLVMInt16TypeInContext(LC);

         assert(lod);

         lp_build_linear_mip_levels(bld, unit, lod, &ilevel0, &ilevel1,
                                    &lod_fpart);
         lod_fpart = LLVMBuildFMul(builder, lod_fpart, f256, "");
         lod_fpart = lp_build_ifloor(&bld->float_bld, lod_fpart);
         lod_fpart = LLVMBuildAnd(builder, lod_fpart, i255, "");
         lod_fpart = LLVMBuildTrunc(builder, lod_fpart, i16_type, "");
         lod_fpart = lp_build_broadcast_scalar(&h16, lod_fpart);

         /* the lod_fpart values will be fixed pt values in [0,1) */
      }
      break;
   }

   /* compute image size(s) of source mipmap level(s) */
   lp_build_mipmap_level_sizes(bld, dims, width_vec, height_vec, depth_vec,
                               ilevel0, ilevel1,
                               row_stride_array, img_stride_array,
                               &width0_vec, &width1_vec,
                               &height0_vec, &height1_vec,
                               &depth0_vec, &depth1_vec,
                               &row_stride0_vec, &row_stride1_vec,
                               &img_stride0_vec, &img_stride1_vec);

   /*
    * Get pointer(s) to image data for mipmap level(s).
    */
   data_ptr0 = lp_build_get_mipmap_level(bld, data_array, ilevel0);
   if (mip_filter == PIPE_TEX_MIPFILTER_LINEAR) {
      data_ptr1 = lp_build_get_mipmap_level(bld, data_array, ilevel1);
   }


   /*
    * Get/interpolate texture colors.
    */
   if (min_filter == mag_filter) {
      /* no need to distinquish between minification and magnification */
      lp_build_sample_mipmap(bld, min_filter, mip_filter,
                             s, t, r, lod_fpart,
                             width0_vec, width1_vec,
                             height0_vec, height1_vec,
                             depth0_vec, depth1_vec,
                             row_stride0_vec, row_stride1_vec,
                             img_stride0_vec, img_stride1_vec,
                             data_ptr0, data_ptr1,
                             &packed_lo, &packed_hi);
   }
   else {
      /* Emit conditional to choose min image filter or mag image filter
       * depending on the lod being > 0 or <= 0, respectively.
       */
      struct lp_build_flow_context *flow_ctx;
      struct lp_build_if_state if_ctx;
      LLVMValueRef minify;

      flow_ctx = lp_build_flow_create(builder);
      lp_build_flow_scope_begin(flow_ctx);

      packed_lo = LLVMGetUndef(h16_vec_type);
      packed_hi = LLVMGetUndef(h16_vec_type);

      lp_build_flow_scope_declare(flow_ctx, &packed_lo);
      lp_build_flow_scope_declare(flow_ctx, &packed_hi);

      /* minify = lod > 0.0 */
      minify = LLVMBuildFCmp(builder, LLVMRealUGE,
                             lod, float_bld->zero, "");

      lp_build_if(&if_ctx, flow_ctx, builder, minify);
      {
         /* Use the minification filter */
         lp_build_sample_mipmap(bld, min_filter, mip_filter,
                                s, t, r, lod_fpart,
                                width0_vec, width1_vec,
                                height0_vec, height1_vec,
                                depth0_vec, depth1_vec,
                                row_stride0_vec, row_stride1_vec,
                                img_stride0_vec, img_stride1_vec,
                                data_ptr0, data_ptr1,
                                &packed_lo, &packed_hi);
      }
      lp_build_else(&if_ctx);
      {
         /* Use the magnification filter */
         lp_build_sample_mipmap(bld, mag_filter, mip_filter,
                                s, t, r, lod_fpart,
                                width0_vec, width1_vec,
                                height0_vec, height1_vec,
                                depth0_vec, depth1_vec,
                                row_stride0_vec, row_stride1_vec,
                                img_stride0_vec, img_stride1_vec,
                                data_ptr0, data_ptr1,
                                &packed_lo, &packed_hi);
      }
      lp_build_endif(&if_ctx);

      lp_build_flow_scope_end(flow_ctx);
      lp_build_flow_destroy(flow_ctx);
   }

   /* combine 'packed_lo', 'packed_hi' into 'packed' */
   {
      struct lp_build_context h16, u8n;

      lp_build_context_init(&h16, builder, lp_type_ufixed(16));
      lp_build_context_init(&u8n, builder, lp_type_unorm(8));

      packed = lp_build_pack2(builder, h16.type, u8n.type,
                              packed_lo, packed_hi);
   }

   /*
    * Convert to SoA and swizzle.
    */
   lp_build_rgba8_to_f32_soa(builder,
                             bld->texel_type,
                             packed, unswizzled);

   if (util_format_is_rgba8_variant(bld->format_desc)) {
      lp_build_format_swizzle_soa(bld->format_desc,
                                  &bld->texel_bld,
                                  unswizzled, texel_out);
   }
   else {
      texel_out[0] = unswizzled[0];
      texel_out[1] = unswizzled[1];
      texel_out[2] = unswizzled[2];
      texel_out[3] = unswizzled[3];
   }

   apply_sampler_swizzle(bld, texel_out);
}
