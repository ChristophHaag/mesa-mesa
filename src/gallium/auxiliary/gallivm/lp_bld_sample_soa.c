/**************************************************************************
 *
 * Copyright 2009 VMware, Inc.
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
#include "lp_bld_bitarit.h"
#include "lp_bld_logic.h"
#include "lp_bld_printf.h"
#include "lp_bld_swizzle.h"
#include "lp_bld_flow.h"
#include "lp_bld_gather.h"
#include "lp_bld_format.h"
#include "lp_bld_sample.h"
#include "lp_bld_sample_aos.h"
#include "lp_bld_struct.h"
#include "lp_bld_quad.h"


/**
 * Does the given texture wrap mode allow sampling the texture border color?
 * XXX maybe move this into gallium util code.
 */
static boolean
wrap_mode_uses_border_color(unsigned mode)
{
   switch (mode) {
   case PIPE_TEX_WRAP_REPEAT:
   case PIPE_TEX_WRAP_CLAMP_TO_EDGE:
   case PIPE_TEX_WRAP_MIRROR_REPEAT:
   case PIPE_TEX_WRAP_MIRROR_CLAMP_TO_EDGE:
      return FALSE;
   case PIPE_TEX_WRAP_CLAMP:
   case PIPE_TEX_WRAP_CLAMP_TO_BORDER:
   case PIPE_TEX_WRAP_MIRROR_CLAMP:
   case PIPE_TEX_WRAP_MIRROR_CLAMP_TO_BORDER:
      return TRUE;
   default:
      assert(0 && "unexpected wrap mode");
      return FALSE;
   }
}


/**
 * Generate code to fetch a texel from a texture at int coords (x, y, z).
 * The computation depends on whether the texture is 1D, 2D or 3D.
 * The result, texel, will be float vectors:
 *   texel[0] = red values
 *   texel[1] = green values
 *   texel[2] = blue values
 *   texel[3] = alpha values
 */
static void
lp_build_sample_texel_soa(struct lp_build_sample_context *bld,
                          unsigned unit,
                          LLVMValueRef width,
                          LLVMValueRef height,
                          LLVMValueRef depth,
                          LLVMValueRef x,
                          LLVMValueRef y,
                          LLVMValueRef z,
                          LLVMValueRef y_stride,
                          LLVMValueRef z_stride,
                          LLVMValueRef data_ptr,
                          LLVMValueRef texel_out[4])
{
   const int dims = texture_dims(bld->static_state->target);
   struct lp_build_context *int_coord_bld = &bld->int_coord_bld;
   LLVMValueRef offset;
   LLVMValueRef i, j;
   LLVMValueRef use_border = NULL;

   /* use_border = x < 0 || x >= width || y < 0 || y >= height */
   if (wrap_mode_uses_border_color(bld->static_state->wrap_s)) {
      LLVMValueRef b1, b2;
      b1 = lp_build_cmp(int_coord_bld, PIPE_FUNC_LESS, x, int_coord_bld->zero);
      b2 = lp_build_cmp(int_coord_bld, PIPE_FUNC_GEQUAL, x, width);
      use_border = LLVMBuildOr(bld->builder, b1, b2, "b1_or_b2");
   }

   if (dims >= 2 && wrap_mode_uses_border_color(bld->static_state->wrap_t)) {
      LLVMValueRef b1, b2;
      b1 = lp_build_cmp(int_coord_bld, PIPE_FUNC_LESS, y, int_coord_bld->zero);
      b2 = lp_build_cmp(int_coord_bld, PIPE_FUNC_GEQUAL, y, height);
      if (use_border) {
         use_border = LLVMBuildOr(bld->builder, use_border, b1, "ub_or_b1");
         use_border = LLVMBuildOr(bld->builder, use_border, b2, "ub_or_b2");
      }
      else {
         use_border = LLVMBuildOr(bld->builder, b1, b2, "b1_or_b2");
      }
   }

   if (dims == 3 && wrap_mode_uses_border_color(bld->static_state->wrap_r)) {
      LLVMValueRef b1, b2;
      b1 = lp_build_cmp(int_coord_bld, PIPE_FUNC_LESS, z, int_coord_bld->zero);
      b2 = lp_build_cmp(int_coord_bld, PIPE_FUNC_GEQUAL, z, depth);
      if (use_border) {
         use_border = LLVMBuildOr(bld->builder, use_border, b1, "ub_or_b1");
         use_border = LLVMBuildOr(bld->builder, use_border, b2, "ub_or_b2");
      }
      else {
         use_border = LLVMBuildOr(bld->builder, b1, b2, "b1_or_b2");
      }
   }

   /* convert x,y,z coords to linear offset from start of texture, in bytes */
   lp_build_sample_offset(&bld->uint_coord_bld,
                          bld->format_desc,
                          x, y, z, y_stride, z_stride,
                          &offset, &i, &j);

   if (use_border) {
      /* If we can sample the border color, it means that texcoords may
       * lie outside the bounds of the texture image.  We need to do
       * something to prevent reading out of bounds and causing a segfault.
       *
       * Simply AND the texture coords with !use_border.  This will cause
       * coords which are out of bounds to become zero.  Zero's guaranteed
       * to be inside the texture image.
       */
      offset = lp_build_andnot(&bld->uint_coord_bld, offset, use_border);
   }

   lp_build_fetch_rgba_soa(bld->gallivm,
                           bld->format_desc,
                           bld->texel_type,
                           data_ptr, offset,
                           i, j,
                           texel_out);

   /*
    * Note: if we find an app which frequently samples the texture border
    * we might want to implement a true conditional here to avoid sampling
    * the texture whenever possible (since that's quite a bit of code).
    * Ex:
    *   if (use_border) {
    *      texel = border_color;
    *   }
    *   else {
    *      texel = sample_texture(coord);
    *   }
    * As it is now, we always sample the texture, then selectively replace
    * the texel color results with the border color.
    */

   if (use_border) {
      /* select texel color or border color depending on use_border */
      LLVMValueRef border_color_ptr = 
         bld->dynamic_state->border_color(bld->dynamic_state,
                                          bld->gallivm, unit);
      int chan;
      for (chan = 0; chan < 4; chan++) {
         LLVMValueRef border_chan =
            lp_build_array_get(bld->gallivm, border_color_ptr,
                               lp_build_const_int32(bld->gallivm, chan));
         LLVMValueRef border_chan_vec =
            lp_build_broadcast_scalar(&bld->float_vec_bld, border_chan);
         texel_out[chan] = lp_build_select(&bld->texel_bld, use_border,
                                           border_chan_vec, texel_out[chan]);
      }
   }

   apply_sampler_swizzle(bld, texel_out);
}


/**
 * Helper to compute the mirror function for the PIPE_WRAP_MIRROR modes.
 */
static LLVMValueRef
lp_build_coord_mirror(struct lp_build_sample_context *bld,
                      LLVMValueRef coord)
{
   struct lp_build_context *coord_bld = &bld->coord_bld;
   struct lp_build_context *int_coord_bld = &bld->int_coord_bld;
   LLVMValueRef fract, flr, isOdd;

   /* fract = coord - floor(coord) */
   fract = lp_build_sub(coord_bld, coord, lp_build_floor(coord_bld, coord));

   /* flr = ifloor(coord); */
   flr = lp_build_ifloor(coord_bld, coord);

   /* isOdd = flr & 1 */
   isOdd = LLVMBuildAnd(bld->builder, flr, int_coord_bld->one, "");

   /* make coord positive or negative depending on isOdd */
   coord = lp_build_set_sign(coord_bld, fract, isOdd);

   /* convert isOdd to float */
   isOdd = lp_build_int_to_float(coord_bld, isOdd);

   /* add isOdd to coord */
   coord = lp_build_add(coord_bld, coord, isOdd);

   return coord;
}


/**
 * Build LLVM code for texture wrap mode for linear filtering.
 * \param x0_out  returns first integer texcoord
 * \param x1_out  returns second integer texcoord
 * \param weight_out  returns linear interpolation weight
 */
static void
lp_build_sample_wrap_linear(struct lp_build_sample_context *bld,
                            LLVMValueRef coord,
                            LLVMValueRef length,
                            boolean is_pot,
                            unsigned wrap_mode,
                            LLVMValueRef *x0_out,
                            LLVMValueRef *x1_out,
                            LLVMValueRef *weight_out)
{
   struct lp_build_context *coord_bld = &bld->coord_bld;
   struct lp_build_context *int_coord_bld = &bld->int_coord_bld;
   struct lp_build_context *uint_coord_bld = &bld->uint_coord_bld;
   LLVMValueRef half = lp_build_const_vec(bld->gallivm, coord_bld->type, 0.5);
   LLVMValueRef length_f = lp_build_int_to_float(coord_bld, length);
   LLVMValueRef length_minus_one = lp_build_sub(uint_coord_bld, length, uint_coord_bld->one);
   LLVMValueRef coord0, coord1, weight;

   switch(wrap_mode) {
   case PIPE_TEX_WRAP_REPEAT:
      /* mul by size and subtract 0.5 */
      coord = lp_build_mul(coord_bld, coord, length_f);
      coord = lp_build_sub(coord_bld, coord, half);
      /* convert to int */
      coord0 = lp_build_ifloor(coord_bld, coord);
      coord1 = lp_build_add(uint_coord_bld, coord0, uint_coord_bld->one);
      /* compute lerp weight */
      weight = lp_build_fract(coord_bld, coord);
      /* repeat wrap */
      if (is_pot) {
         coord0 = LLVMBuildAnd(bld->builder, coord0, length_minus_one, "");
         coord1 = LLVMBuildAnd(bld->builder, coord1, length_minus_one, "");
      }
      else {
         /* Add a bias to the texcoord to handle negative coords */
         LLVMValueRef bias = lp_build_mul_imm(uint_coord_bld, length, 1024);
         coord0 = LLVMBuildAdd(bld->builder, coord0, bias, "");
         coord1 = LLVMBuildAdd(bld->builder, coord1, bias, "");
         coord0 = LLVMBuildURem(bld->builder, coord0, length, "");
         coord1 = LLVMBuildURem(bld->builder, coord1, length, "");
      }
      break;

   case PIPE_TEX_WRAP_CLAMP:
      if (bld->static_state->normalized_coords) {
         /* scale coord to length */
         coord = lp_build_mul(coord_bld, coord, length_f);
      }

      /* clamp to [0, length] */
      coord = lp_build_clamp(coord_bld, coord, coord_bld->zero, length_f);

      coord = lp_build_sub(coord_bld, coord, half);

      weight = lp_build_fract(coord_bld, coord);
      coord0 = lp_build_ifloor(coord_bld, coord);
      coord1 = lp_build_add(int_coord_bld, coord0, int_coord_bld->one);
      break;

   case PIPE_TEX_WRAP_CLAMP_TO_EDGE:
      if (bld->static_state->normalized_coords) {
         /* clamp to [0,1] */
         coord = lp_build_clamp(coord_bld, coord, coord_bld->zero, coord_bld->one);
         /* mul by tex size and subtract 0.5 */
         coord = lp_build_mul(coord_bld, coord, length_f);
         coord = lp_build_sub(coord_bld, coord, half);
      }
      else {
         LLVMValueRef min, max;
         /* clamp to [0.5, length - 0.5] */
         min = half;
         max = lp_build_sub(coord_bld, length_f, min);
         coord = lp_build_clamp(coord_bld, coord, min, max);
      }
      /* compute lerp weight */
      weight = lp_build_fract(coord_bld, coord);
      /* coord0 = floor(coord); */
      coord0 = lp_build_ifloor(coord_bld, coord);
      coord1 = lp_build_add(int_coord_bld, coord0, int_coord_bld->one);
      /* coord0 = max(coord0, 0) */
      coord0 = lp_build_max(int_coord_bld, coord0, int_coord_bld->zero);
      /* coord1 = min(coord1, length-1) */
      coord1 = lp_build_min(int_coord_bld, coord1, length_minus_one);
      break;

   case PIPE_TEX_WRAP_CLAMP_TO_BORDER:
      {
         LLVMValueRef min, max;
         if (bld->static_state->normalized_coords) {
            /* scale coord to length */
            coord = lp_build_mul(coord_bld, coord, length_f);
         }
         /* clamp to [-0.5, length + 0.5] */
         min = lp_build_const_vec(bld->gallivm, coord_bld->type, -0.5F);
         max = lp_build_sub(coord_bld, length_f, min);
         coord = lp_build_clamp(coord_bld, coord, min, max);
         coord = lp_build_sub(coord_bld, coord, half);
         /* compute lerp weight */
         weight = lp_build_fract(coord_bld, coord);
         /* convert to int */
         coord0 = lp_build_ifloor(coord_bld, coord);
         coord1 = lp_build_add(int_coord_bld, coord0, int_coord_bld->one);
      }
      break;

   case PIPE_TEX_WRAP_MIRROR_REPEAT:
      /* compute mirror function */
      coord = lp_build_coord_mirror(bld, coord);

      /* scale coord to length */
      coord = lp_build_mul(coord_bld, coord, length_f);
      coord = lp_build_sub(coord_bld, coord, half);

      /* compute lerp weight */
      weight = lp_build_fract(coord_bld, coord);

      /* convert to int coords */
      coord0 = lp_build_ifloor(coord_bld, coord);
      coord1 = lp_build_add(int_coord_bld, coord0, int_coord_bld->one);

      /* coord0 = max(coord0, 0) */
      coord0 = lp_build_max(int_coord_bld, coord0, int_coord_bld->zero);
      /* coord1 = min(coord1, length-1) */
      coord1 = lp_build_min(int_coord_bld, coord1, length_minus_one);
      break;

   case PIPE_TEX_WRAP_MIRROR_CLAMP:
      coord = lp_build_abs(coord_bld, coord);

      if (bld->static_state->normalized_coords) {
         /* scale coord to length */
         coord = lp_build_mul(coord_bld, coord, length_f);
      }

      /* clamp to [0, length] */
      coord = lp_build_min(coord_bld, coord, length_f);

      coord = lp_build_sub(coord_bld, coord, half);

      weight = lp_build_fract(coord_bld, coord);
      coord0 = lp_build_ifloor(coord_bld, coord);
      coord1 = lp_build_add(int_coord_bld, coord0, int_coord_bld->one);
      break;

   case PIPE_TEX_WRAP_MIRROR_CLAMP_TO_EDGE:
      {
         LLVMValueRef min, max;

         coord = lp_build_abs(coord_bld, coord);

         if (bld->static_state->normalized_coords) {
            /* scale coord to length */
            coord = lp_build_mul(coord_bld, coord, length_f);
         }

         /* clamp to [0.5, length - 0.5] */
         min = half;
         max = lp_build_sub(coord_bld, length_f, min);
         coord = lp_build_clamp(coord_bld, coord, min, max);

         coord = lp_build_sub(coord_bld, coord, half);

         weight = lp_build_fract(coord_bld, coord);
         coord0 = lp_build_ifloor(coord_bld, coord);
         coord1 = lp_build_add(int_coord_bld, coord0, int_coord_bld->one);
      }
      break;

   case PIPE_TEX_WRAP_MIRROR_CLAMP_TO_BORDER:
      {
         LLVMValueRef min, max;

         coord = lp_build_abs(coord_bld, coord);

         if (bld->static_state->normalized_coords) {
            /* scale coord to length */
            coord = lp_build_mul(coord_bld, coord, length_f);
         }

         /* clamp to [-0.5, length + 0.5] */
         min = lp_build_negate(coord_bld, half);
         max = lp_build_sub(coord_bld, length_f, min);
         coord = lp_build_clamp(coord_bld, coord, min, max);

         coord = lp_build_sub(coord_bld, coord, half);

         weight = lp_build_fract(coord_bld, coord);
         coord0 = lp_build_ifloor(coord_bld, coord);
         coord1 = lp_build_add(int_coord_bld, coord0, int_coord_bld->one);
      }
      break;

   default:
      assert(0);
      coord0 = NULL;
      coord1 = NULL;
      weight = NULL;
   }

   *x0_out = coord0;
   *x1_out = coord1;
   *weight_out = weight;
}


/**
 * Build LLVM code for texture wrap mode for nearest filtering.
 * \param coord  the incoming texcoord (nominally in [0,1])
 * \param length  the texture size along one dimension, as int vector
 * \param is_pot  if TRUE, length is a power of two
 * \param wrap_mode  one of PIPE_TEX_WRAP_x
 */
static LLVMValueRef
lp_build_sample_wrap_nearest(struct lp_build_sample_context *bld,
                             LLVMValueRef coord,
                             LLVMValueRef length,
                             boolean is_pot,
                             unsigned wrap_mode)
{
   struct lp_build_context *coord_bld = &bld->coord_bld;
   struct lp_build_context *int_coord_bld = &bld->int_coord_bld;
   struct lp_build_context *uint_coord_bld = &bld->uint_coord_bld;
   LLVMValueRef length_f = lp_build_int_to_float(coord_bld, length);
   LLVMValueRef length_minus_one = lp_build_sub(uint_coord_bld, length, uint_coord_bld->one);
   LLVMValueRef icoord;
   
   switch(wrap_mode) {
   case PIPE_TEX_WRAP_REPEAT:
      coord = lp_build_mul(coord_bld, coord, length_f);
      icoord = lp_build_ifloor(coord_bld, coord);
      if (is_pot)
         icoord = LLVMBuildAnd(bld->builder, icoord, length_minus_one, "");
      else {
         /* Add a bias to the texcoord to handle negative coords */
         LLVMValueRef bias = lp_build_mul_imm(uint_coord_bld, length, 1024);
         icoord = LLVMBuildAdd(bld->builder, icoord, bias, "");
         icoord = LLVMBuildURem(bld->builder, icoord, length, "");
      }
      break;

   case PIPE_TEX_WRAP_CLAMP:
   case PIPE_TEX_WRAP_CLAMP_TO_EDGE:
      if (bld->static_state->normalized_coords) {
         /* scale coord to length */
         coord = lp_build_mul(coord_bld, coord, length_f);
      }

      /* floor */
      icoord = lp_build_ifloor(coord_bld, coord);

      /* clamp to [0, length - 1]. */
      icoord = lp_build_clamp(int_coord_bld, icoord, int_coord_bld->zero,
                              length_minus_one);
      break;

   case PIPE_TEX_WRAP_CLAMP_TO_BORDER:
      /* Note: this is the same as CLAMP_TO_EDGE, except min = -min */
      {
         LLVMValueRef min, max;

         if (bld->static_state->normalized_coords) {
            /* scale coord to length */
            coord = lp_build_mul(coord_bld, coord, length_f);
         }

         icoord = lp_build_ifloor(coord_bld, coord);

         /* clamp to [-1, length] */
         min = lp_build_negate(int_coord_bld, int_coord_bld->one);
         max = length;
         icoord = lp_build_clamp(int_coord_bld, icoord, min, max);
      }
      break;

   case PIPE_TEX_WRAP_MIRROR_REPEAT:
      /* compute mirror function */
      coord = lp_build_coord_mirror(bld, coord);

      /* scale coord to length */
      assert(bld->static_state->normalized_coords);
      coord = lp_build_mul(coord_bld, coord, length_f);

      icoord = lp_build_ifloor(coord_bld, coord);

      /* clamp to [0, length - 1] */
      icoord = lp_build_min(int_coord_bld, icoord, length_minus_one);
      break;

   case PIPE_TEX_WRAP_MIRROR_CLAMP:
   case PIPE_TEX_WRAP_MIRROR_CLAMP_TO_EDGE:
      coord = lp_build_abs(coord_bld, coord);

      if (bld->static_state->normalized_coords) {
         /* scale coord to length */
         coord = lp_build_mul(coord_bld, coord, length_f);
      }

      icoord = lp_build_ifloor(coord_bld, coord);

      /* clamp to [0, length - 1] */
      icoord = lp_build_min(int_coord_bld, icoord, length_minus_one);
      break;

   case PIPE_TEX_WRAP_MIRROR_CLAMP_TO_BORDER:
      coord = lp_build_abs(coord_bld, coord);

      if (bld->static_state->normalized_coords) {
         /* scale coord to length */
         coord = lp_build_mul(coord_bld, coord, length_f);
      }

      icoord = lp_build_ifloor(coord_bld, coord);

      /* clamp to [0, length] */
      icoord = lp_build_min(int_coord_bld, icoord, length);
      break;

   default:
      assert(0);
      icoord = NULL;
   }

   return icoord;
}


/**
 * Generate code to sample a mipmap level with nearest filtering.
 * If sampling a cube texture, r = cube face in [0,5].
 */
static void
lp_build_sample_image_nearest(struct lp_build_sample_context *bld,
                              unsigned unit,
                              LLVMValueRef width_vec,
                              LLVMValueRef height_vec,
                              LLVMValueRef depth_vec,
                              LLVMValueRef row_stride_vec,
                              LLVMValueRef img_stride_vec,
                              LLVMValueRef data_ptr,
                              LLVMValueRef s,
                              LLVMValueRef t,
                              LLVMValueRef r,
                              LLVMValueRef colors_out[4])
{
   const int dims = texture_dims(bld->static_state->target);
   LLVMValueRef x, y, z;

   /*
    * Compute integer texcoords.
    */
   x = lp_build_sample_wrap_nearest(bld, s, width_vec,
                                    bld->static_state->pot_width,
                                    bld->static_state->wrap_s);
   lp_build_name(x, "tex.x.wrapped");

   if (dims >= 2) {
      y = lp_build_sample_wrap_nearest(bld, t, height_vec,
                                       bld->static_state->pot_height,
                                       bld->static_state->wrap_t);
      lp_build_name(y, "tex.y.wrapped");

      if (dims == 3) {
         z = lp_build_sample_wrap_nearest(bld, r, depth_vec,
                                          bld->static_state->pot_depth,
                                          bld->static_state->wrap_r);
         lp_build_name(z, "tex.z.wrapped");
      }
      else if (bld->static_state->target == PIPE_TEXTURE_CUBE) {
         z = r;
      }
      else {
         z = NULL;
      }
   }
   else {
      y = z = NULL;
   }

   /*
    * Get texture colors.
    */
   lp_build_sample_texel_soa(bld, unit,
                             width_vec, height_vec, depth_vec,
                             x, y, z,
                             row_stride_vec, img_stride_vec,
                             data_ptr, colors_out);
}


/**
 * Generate code to sample a mipmap level with linear filtering.
 * If sampling a cube texture, r = cube face in [0,5].
 */
static void
lp_build_sample_image_linear(struct lp_build_sample_context *bld,
                             unsigned unit,
                             LLVMValueRef width_vec,
                             LLVMValueRef height_vec,
                             LLVMValueRef depth_vec,
                             LLVMValueRef row_stride_vec,
                             LLVMValueRef img_stride_vec,
                             LLVMValueRef data_ptr,
                             LLVMValueRef s,
                             LLVMValueRef t,
                             LLVMValueRef r,
                             LLVMValueRef colors_out[4])
{
   const int dims = texture_dims(bld->static_state->target);
   LLVMValueRef x0, y0, z0, x1, y1, z1;
   LLVMValueRef s_fpart, t_fpart, r_fpart;
   LLVMValueRef neighbors[2][2][4];
   int chan;

   /*
    * Compute integer texcoords.
    */
   lp_build_sample_wrap_linear(bld, s, width_vec,
                               bld->static_state->pot_width,
                               bld->static_state->wrap_s,
                               &x0, &x1, &s_fpart);
   lp_build_name(x0, "tex.x0.wrapped");
   lp_build_name(x1, "tex.x1.wrapped");

   if (dims >= 2) {
      lp_build_sample_wrap_linear(bld, t, height_vec,
                                  bld->static_state->pot_height,
                                  bld->static_state->wrap_t,
                                  &y0, &y1, &t_fpart);
      lp_build_name(y0, "tex.y0.wrapped");
      lp_build_name(y1, "tex.y1.wrapped");

      if (dims == 3) {
         lp_build_sample_wrap_linear(bld, r, depth_vec,
                                     bld->static_state->pot_depth,
                                     bld->static_state->wrap_r,
                                     &z0, &z1, &r_fpart);
         lp_build_name(z0, "tex.z0.wrapped");
         lp_build_name(z1, "tex.z1.wrapped");
      }
      else if (bld->static_state->target == PIPE_TEXTURE_CUBE) {
         z0 = z1 = r;  /* cube face */
         r_fpart = NULL;
      }
      else {
         z0 = z1 = NULL;
         r_fpart = NULL;
      }
   }
   else {
      y0 = y1 = t_fpart = NULL;
      z0 = z1 = r_fpart = NULL;
   }

   /*
    * Get texture colors.
    */
   /* get x0/x1 texels */
   lp_build_sample_texel_soa(bld, unit,
                             width_vec, height_vec, depth_vec,
                             x0, y0, z0,
                             row_stride_vec, img_stride_vec,
                             data_ptr, neighbors[0][0]);
   lp_build_sample_texel_soa(bld, unit,
                             width_vec, height_vec, depth_vec,
                             x1, y0, z0,
                             row_stride_vec, img_stride_vec,
                             data_ptr, neighbors[0][1]);

   if (dims == 1) {
      /* Interpolate two samples from 1D image to produce one color */
      for (chan = 0; chan < 4; chan++) {
         colors_out[chan] = lp_build_lerp(&bld->texel_bld, s_fpart,
                                          neighbors[0][0][chan],
                                          neighbors[0][1][chan]);
      }
   }
   else {
      /* 2D/3D texture */
      LLVMValueRef colors0[4];

      /* get x0/x1 texels at y1 */
      lp_build_sample_texel_soa(bld, unit,
                                width_vec, height_vec, depth_vec,
                                x0, y1, z0,
                                row_stride_vec, img_stride_vec,
                                data_ptr, neighbors[1][0]);
      lp_build_sample_texel_soa(bld, unit,
                                width_vec, height_vec, depth_vec,
                                x1, y1, z0,
                                row_stride_vec, img_stride_vec,
                                data_ptr, neighbors[1][1]);

      /* Bilinear interpolate the four samples from the 2D image / 3D slice */
      for (chan = 0; chan < 4; chan++) {
         colors0[chan] = lp_build_lerp_2d(&bld->texel_bld,
                                          s_fpart, t_fpart,
                                          neighbors[0][0][chan],
                                          neighbors[0][1][chan],
                                          neighbors[1][0][chan],
                                          neighbors[1][1][chan]);
      }

      if (dims == 3) {
         LLVMValueRef neighbors1[2][2][4];
         LLVMValueRef colors1[4];

         /* get x0/x1/y0/y1 texels at z1 */
         lp_build_sample_texel_soa(bld, unit,
                                   width_vec, height_vec, depth_vec,
                                   x0, y0, z1,
                                   row_stride_vec, img_stride_vec,
                                   data_ptr, neighbors1[0][0]);
         lp_build_sample_texel_soa(bld, unit,
                                   width_vec, height_vec, depth_vec,
                                   x1, y0, z1,
                                   row_stride_vec, img_stride_vec,
                                   data_ptr, neighbors1[0][1]);
         lp_build_sample_texel_soa(bld, unit,
                                   width_vec, height_vec, depth_vec,
                                   x0, y1, z1,
                                   row_stride_vec, img_stride_vec,
                                   data_ptr, neighbors1[1][0]);
         lp_build_sample_texel_soa(bld, unit,
                                   width_vec, height_vec, depth_vec,
                                   x1, y1, z1,
                                   row_stride_vec, img_stride_vec,
                                   data_ptr, neighbors1[1][1]);

         /* Bilinear interpolate the four samples from the second Z slice */
         for (chan = 0; chan < 4; chan++) {
            colors1[chan] = lp_build_lerp_2d(&bld->texel_bld,
                                             s_fpart, t_fpart,
                                             neighbors1[0][0][chan],
                                             neighbors1[0][1][chan],
                                             neighbors1[1][0][chan],
                                             neighbors1[1][1][chan]);
         }

         /* Linearly interpolate the two samples from the two 3D slices */
         for (chan = 0; chan < 4; chan++) {
            colors_out[chan] = lp_build_lerp(&bld->texel_bld,
                                             r_fpart,
                                             colors0[chan], colors1[chan]);
         }
      }
      else {
         /* 2D tex */
         for (chan = 0; chan < 4; chan++) {
            colors_out[chan] = colors0[chan];
         }
      }
   }
}


/**
 * Sample the texture/mipmap using given image filter and mip filter.
 * data0_ptr and data1_ptr point to the two mipmap levels to sample
 * from.  width0/1_vec, height0/1_vec, depth0/1_vec indicate their sizes.
 * If we're using nearest miplevel sampling the '1' values will be null/unused.
 */
static void
lp_build_sample_mipmap(struct lp_build_sample_context *bld,
                       unsigned unit,
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
                       LLVMValueRef *colors_out)
{
   LLVMValueRef colors0[4], colors1[4];
   int chan;

   if (img_filter == PIPE_TEX_FILTER_NEAREST) {
      /* sample the first mipmap level */
      lp_build_sample_image_nearest(bld, unit,
                                    width0_vec, height0_vec, depth0_vec,
                                    row_stride0_vec, img_stride0_vec,
                                    data_ptr0, s, t, r, colors0);

      if (mip_filter == PIPE_TEX_MIPFILTER_LINEAR) {
         /* sample the second mipmap level */
         lp_build_sample_image_nearest(bld, unit,
                                       width1_vec, height1_vec, depth1_vec,
                                       row_stride1_vec, img_stride1_vec,
                                       data_ptr1, s, t, r, colors1);
      }
   }
   else {
      assert(img_filter == PIPE_TEX_FILTER_LINEAR);

      /* sample the first mipmap level */
      lp_build_sample_image_linear(bld, unit,
                                   width0_vec, height0_vec, depth0_vec,
                                   row_stride0_vec, img_stride0_vec,
                                   data_ptr0, s, t, r, colors0);

      if (mip_filter == PIPE_TEX_MIPFILTER_LINEAR) {
         /* sample the second mipmap level */
         lp_build_sample_image_linear(bld, unit,
                                      width1_vec, height1_vec, depth1_vec,
                                      row_stride1_vec, img_stride1_vec,
                                      data_ptr1, s, t, r, colors1);
      }
   }

   if (mip_filter == PIPE_TEX_MIPFILTER_LINEAR) {
      /* interpolate samples from the two mipmap levels */
      for (chan = 0; chan < 4; chan++) {
         colors_out[chan] = lp_build_lerp(&bld->texel_bld, lod_fpart,
                                          colors0[chan], colors1[chan]);
      }
   }
   else {
      /* use first/only level's colors */
      for (chan = 0; chan < 4; chan++) {
         colors_out[chan] = colors0[chan];
      }
   }
}



/**
 * General texture sampling codegen.
 * This function handles texture sampling for all texture targets (1D,
 * 2D, 3D, cube) and all filtering modes.
 */
static void
lp_build_sample_general(struct lp_build_sample_context *bld,
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
                        LLVMValueRef *colors_out)
{
   struct lp_build_context *float_bld = &bld->float_bld;
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
   LLVMValueRef face_ddx[4], face_ddy[4];

   /*
   printf("%s mip %d  min %d  mag %d\n", __FUNCTION__,
          mip_filter, min_filter, mag_filter);
   */

   /*
    * Choose cube face, recompute texcoords and derivatives for the chosen face.
    */
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
    * Compute integer mipmap level(s) to fetch texels from.
    */
   if (mip_filter == PIPE_TEX_MIPFILTER_NONE) {
      /* always use mip level 0 */
      if (bld->static_state->target == PIPE_TEXTURE_CUBE) {
         /* XXX this is a work-around for an apparent bug in LLVM 2.7.
          * We should be able to set ilevel0 = const(0) but that causes
          * bad x86 code to be emitted.
          */
         lod = lp_build_const_elem(bld->gallivm, bld->coord_bld.type, 0.0);
         lp_build_nearest_mip_level(bld, unit, lod, &ilevel0);
      }
      else {
         ilevel0 = lp_build_const_int32(bld->gallivm, 0);
      }
   }
   else {
      assert(lod);
      if (mip_filter == PIPE_TEX_MIPFILTER_NEAREST) {
         lp_build_nearest_mip_level(bld, unit, lod, &ilevel0);
      }
      else {
         assert(mip_filter == PIPE_TEX_MIPFILTER_LINEAR);
         lp_build_linear_mip_levels(bld, unit, lod, &ilevel0, &ilevel1,
                                    &lod_fpart);
         lod_fpart = lp_build_broadcast_scalar(&bld->coord_bld, lod_fpart);
      }
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
      lp_build_sample_mipmap(bld, unit,
                             min_filter, mip_filter, s, t, r, lod_fpart,
                             width0_vec, width1_vec,
                             height0_vec, height1_vec,
                             depth0_vec, depth1_vec,
                             row_stride0_vec, row_stride1_vec,
                             img_stride0_vec, img_stride1_vec,
                             data_ptr0, data_ptr1,
                             colors_out);
   }
   else {
      /* Emit conditional to choose min image filter or mag image filter
       * depending on the lod being >0 or <= 0, respectively.
       */
      struct lp_build_flow_context *flow_ctx;
      struct lp_build_if_state if_ctx;
      LLVMValueRef minify;

      flow_ctx = lp_build_flow_create(bld->gallivm);
      lp_build_flow_scope_begin(flow_ctx);

      lp_build_flow_scope_declare(flow_ctx, &colors_out[0]);
      lp_build_flow_scope_declare(flow_ctx, &colors_out[1]);
      lp_build_flow_scope_declare(flow_ctx, &colors_out[2]);
      lp_build_flow_scope_declare(flow_ctx, &colors_out[3]);

      /* minify = lod > 0.0 */
      minify = LLVMBuildFCmp(bld->builder, LLVMRealUGE,
                             lod, float_bld->zero, "");

      lp_build_if(&if_ctx, flow_ctx, bld->builder, minify);
      {
         /* Use the minification filter */
         lp_build_sample_mipmap(bld, unit,
                                min_filter, mip_filter,
                                s, t, r, lod_fpart,
                                width0_vec, width1_vec,
                                height0_vec, height1_vec,
                                depth0_vec, depth1_vec,
                                row_stride0_vec, row_stride1_vec,
                                img_stride0_vec, img_stride1_vec,
                                data_ptr0, data_ptr1,
                                colors_out);
      }
      lp_build_else(&if_ctx);
      {
         /* Use the magnification filter */
         lp_build_sample_mipmap(bld, unit,
                                mag_filter, mip_filter,
                                s, t, r, lod_fpart,
                                width0_vec, width1_vec,
                                height0_vec, height1_vec,
                                depth0_vec, depth1_vec,
                                row_stride0_vec, row_stride1_vec,
                                img_stride0_vec, img_stride1_vec,
                                data_ptr0, data_ptr1,
                                colors_out);
      }
      lp_build_endif(&if_ctx);

      lp_build_flow_scope_end(flow_ctx);
      lp_build_flow_destroy(flow_ctx);
   }
}


/**
 * Do shadow test/comparison.
 * \param p  the texcoord Z (aka R, aka P) component
 * \param texel  the texel to compare against (use the X channel)
 */
static void
lp_build_sample_compare(struct lp_build_sample_context *bld,
                        LLVMValueRef p,
                        LLVMValueRef texel[4])
{
   struct lp_build_context *texel_bld = &bld->texel_bld;
   LLVMValueRef res;
   const unsigned chan = 0;

   if (bld->static_state->compare_mode == PIPE_TEX_COMPARE_NONE)
      return;

   /* debug code */
   if (0) {
      LLVMValueRef indx = lp_build_const_int32(bld->gallivm, 0);
      LLVMValueRef coord = LLVMBuildExtractElement(bld->builder, p, indx, "");
      LLVMValueRef tex = LLVMBuildExtractElement(bld->builder,
                                                 texel[chan], indx, "");
      lp_build_printf(bld->gallivm, "shadow compare coord %f to texture %f\n",
                      coord, tex);
   }

   /* result = (p FUNC texel) ? 1 : 0 */
   res = lp_build_cmp(texel_bld, bld->static_state->compare_func,
                      p, texel[chan]);
   res = lp_build_select(texel_bld, res, texel_bld->one, texel_bld->zero);

   /* XXX returning result for default GL_DEPTH_TEXTURE_MODE = GL_LUMINANCE */
   texel[0] =
   texel[1] =
   texel[2] = res;
   texel[3] = texel_bld->one;
}


/**
 * Just set texels to white instead of actually sampling the texture.
 * For debugging.
 */
void
lp_build_sample_nop(struct gallivm_state *gallivm, struct lp_type type,
                    LLVMValueRef texel_out[4])
{
   LLVMValueRef one = lp_build_one(gallivm, type);
   unsigned chan;

   for (chan = 0; chan < 4; chan++) {
      texel_out[chan] = one;
   }  
}


/**
 * Build texture sampling code.
 * 'texel' will return a vector of four LLVMValueRefs corresponding to
 * R, G, B, A.
 * \param type  vector float type to use for coords, etc.
 * \param ddx  partial derivatives of (s,t,r,q) with respect to x
 * \param ddy  partial derivatives of (s,t,r,q) with respect to y
 */
void
lp_build_sample_soa(struct gallivm_state *gallivm,
                    const struct lp_sampler_static_state *static_state,
                    struct lp_sampler_dynamic_state *dynamic_state,
                    struct lp_type type,
                    unsigned unit,
                    unsigned num_coords,
                    const LLVMValueRef *coords,
                    const LLVMValueRef ddx[4],
                    const LLVMValueRef ddy[4],
                    LLVMValueRef lod_bias, /* optional */
                    LLVMValueRef explicit_lod, /* optional */
                    LLVMValueRef texel_out[4])
{
   struct lp_build_sample_context bld;
   LLVMValueRef width, width_vec;
   LLVMValueRef height, height_vec;
   LLVMValueRef depth, depth_vec;
   LLVMValueRef row_stride_array, img_stride_array;
   LLVMValueRef data_array;
   LLVMValueRef s;
   LLVMValueRef t;
   LLVMValueRef r;
   struct lp_type float_vec_type;

   if (0) {
      enum pipe_format fmt = static_state->format;
      debug_printf("Sample from %s\n", util_format_name(fmt));
   }

   assert(type.floating);

   /* Setup our build context */
   memset(&bld, 0, sizeof bld);
   bld.builder = gallivm->builder;
   bld.gallivm = gallivm;
   bld.static_state = static_state;
   bld.dynamic_state = dynamic_state;
   bld.format_desc = util_format_description(static_state->format);

   bld.float_type = lp_type_float(32);
   bld.int_type = lp_type_int(32);
   bld.coord_type = type;
   bld.uint_coord_type = lp_uint_type(type);
   bld.int_coord_type = lp_int_type(type);
   bld.texel_type = type;

   float_vec_type = lp_type_float_vec(32);

   lp_build_context_init(&bld.float_bld, gallivm, bld.float_type);
   lp_build_context_init(&bld.float_vec_bld, gallivm, float_vec_type);
   lp_build_context_init(&bld.int_bld, gallivm, bld.int_type);
   lp_build_context_init(&bld.coord_bld, gallivm, bld.coord_type);
   lp_build_context_init(&bld.uint_coord_bld, gallivm, bld.uint_coord_type);
   lp_build_context_init(&bld.int_coord_bld, gallivm, bld.int_coord_type);
   lp_build_context_init(&bld.texel_bld, gallivm, bld.texel_type);

   /* Get the dynamic state */
   width = dynamic_state->width(dynamic_state, gallivm, unit);
   height = dynamic_state->height(dynamic_state, gallivm, unit);
   depth = dynamic_state->depth(dynamic_state, gallivm, unit);
   row_stride_array = dynamic_state->row_stride(dynamic_state, gallivm, unit);
   img_stride_array = dynamic_state->img_stride(dynamic_state, gallivm, unit);
   data_array = dynamic_state->data_ptr(dynamic_state, gallivm, unit);
   /* Note that data_array is an array[level] of pointers to texture images */

   s = coords[0];
   t = coords[1];
   r = coords[2];

   /* width, height, depth as uint vectors */
   width_vec = lp_build_broadcast_scalar(&bld.uint_coord_bld, width);
   height_vec = lp_build_broadcast_scalar(&bld.uint_coord_bld, height);
   depth_vec = lp_build_broadcast_scalar(&bld.uint_coord_bld, depth);

   if (0) {
      /* For debug: no-op texture sampling */
      lp_build_sample_nop(gallivm, bld.texel_type, texel_out);
   }
   else if (util_format_fits_8unorm(bld.format_desc) &&
            lp_is_simple_wrap_mode(static_state->wrap_s) &&
            lp_is_simple_wrap_mode(static_state->wrap_t)) {
      /* do sampling/filtering with fixed pt arithmetic */
      lp_build_sample_aos(&bld, unit, s, t, r, ddx, ddy,
                          lod_bias, explicit_lod,
                          width, height, depth,
                          width_vec, height_vec, depth_vec,
                          row_stride_array, img_stride_array,
                          data_array, texel_out);
   }

   else {
      if ((gallivm_debug & GALLIVM_DEBUG_PERF) &&
          util_format_fits_8unorm(bld.format_desc)) {
         debug_printf("%s: using floating point linear filtering for %s\n",
                      __FUNCTION__, bld.format_desc->short_name);
         debug_printf("  min_img %d  mag_img %d  mip %d  wraps %d  wrapt %d\n",
                      static_state->min_img_filter,
                      static_state->mag_img_filter,
                      static_state->min_mip_filter,
                      static_state->wrap_s,
                      static_state->wrap_t);
      }

      lp_build_sample_general(&bld, unit, s, t, r, ddx, ddy,
                              lod_bias, explicit_lod,
                              width, height, depth,
                              width_vec, height_vec, depth_vec,
                              row_stride_array, img_stride_array,
                              data_array,
                              texel_out);
   }

   lp_build_sample_compare(&bld, r, texel_out);
}
