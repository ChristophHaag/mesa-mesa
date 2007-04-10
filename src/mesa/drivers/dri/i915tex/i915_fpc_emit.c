/**************************************************************************
 * 
 * Copyright 2003 Tungsten Graphics, Inc., Cedar Park, Texas.
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

#include <strings.h>

#include "glheader.h"
#include "macros.h"
#include "enums.h"

#include "i915_reg.h"
#include "i915_context.h"
#include "i915_fpc.h"


#define A0_DEST( reg ) (((reg)&UREG_TYPE_NR_MASK)>>UREG_A0_DEST_SHIFT_LEFT)
#define D0_DEST( reg ) (((reg)&UREG_TYPE_NR_MASK)>>UREG_A0_DEST_SHIFT_LEFT)
#define T0_DEST( reg ) (((reg)&UREG_TYPE_NR_MASK)>>UREG_A0_DEST_SHIFT_LEFT)
#define A0_SRC0( reg ) (((reg)&UREG_MASK)>>UREG_A0_SRC0_SHIFT_LEFT)
#define A1_SRC0( reg ) (((reg)&UREG_MASK)<<UREG_A1_SRC0_SHIFT_RIGHT)
#define A1_SRC1( reg ) (((reg)&UREG_MASK)>>UREG_A1_SRC1_SHIFT_LEFT)
#define A2_SRC1( reg ) (((reg)&UREG_MASK)<<UREG_A2_SRC1_SHIFT_RIGHT)
#define A2_SRC2( reg ) (((reg)&UREG_MASK)>>UREG_A2_SRC2_SHIFT_LEFT)

/* These are special, and don't have swizzle/negate bits.
 */
#define T0_SAMPLER( reg )     (GET_UREG_NR(reg)<<T0_SAMPLER_NR_SHIFT)
#define T1_ADDRESS_REG( reg ) ((GET_UREG_NR(reg)<<T1_ADDRESS_REG_NR_SHIFT) | \
			       (GET_UREG_TYPE(reg)<<T1_ADDRESS_REG_TYPE_SHIFT))


/* Macros for translating UREG's into the various register fields used
 * by the I915 programmable unit.
 */
#define UREG_A0_DEST_SHIFT_LEFT  (UREG_TYPE_SHIFT - A0_DEST_TYPE_SHIFT)
#define UREG_A0_SRC0_SHIFT_LEFT  (UREG_TYPE_SHIFT - A0_SRC0_TYPE_SHIFT)
#define UREG_A1_SRC0_SHIFT_RIGHT (A1_SRC0_CHANNEL_W_SHIFT - UREG_CHANNEL_W_SHIFT)
#define UREG_A1_SRC1_SHIFT_LEFT  (UREG_TYPE_SHIFT - A1_SRC1_TYPE_SHIFT)
#define UREG_A2_SRC1_SHIFT_RIGHT (A2_SRC1_CHANNEL_W_SHIFT - UREG_CHANNEL_W_SHIFT)
#define UREG_A2_SRC2_SHIFT_LEFT  (UREG_TYPE_SHIFT - A2_SRC2_TYPE_SHIFT)

#define UREG_MASK         0xffffff00
#define UREG_TYPE_NR_MASK ((REG_TYPE_MASK << UREG_TYPE_SHIFT) | \
  			   (REG_NR_MASK << UREG_NR_SHIFT))


#define I915_CONSTFLAG_PARAM 0x1f

GLuint
i915_get_temp(struct i915_fp_compile *p)
{
   int bit = ffs(~p->temp_flag);
   if (!bit) {
      i915_program_error(p, "i915_get_temp: out of temporaries\n");
      return 0;
   }

   p->temp_flag |= 1 << (bit - 1);
   return UREG(REG_TYPE_R, (bit - 1));
}


GLuint
i915_get_utemp(struct i915_fp_compile * p)
{
   int bit = ffs(~p->utemp_flag);
   if (!bit) {
      i915_program_error(p, "i915_get_utemp: out of temporaries\n");
      return 0;
   }

   p->utemp_flag |= 1 << (bit - 1);
   return UREG(REG_TYPE_U, (bit - 1));
}

void
i915_release_utemps(struct i915_fp_compile *p)
{
   p->utemp_flag = ~0x7;
}


GLuint
i915_emit_decl(struct i915_fp_compile *p,
               GLuint type, GLuint nr, GLuint d0_flags)
{
   GLuint reg = UREG(type, nr);

   if (type == REG_TYPE_T) {
      if (p->decl_t & (1 << nr))
         return reg;

      p->decl_t |= (1 << nr);
   }
   else if (type == REG_TYPE_S) {
      if (p->decl_s & (1 << nr))
         return reg;

      p->decl_s |= (1 << nr);
   }
   else
      return reg;

   *(p->decl++) = (D0_DCL | D0_DEST(reg) | d0_flags);
   *(p->decl++) = D1_MBZ;
   *(p->decl++) = D2_MBZ;

   p->nr_decl_insn++;
   return reg;
}

GLuint
i915_emit_arith(struct i915_fp_compile * p,
                GLuint op,
                GLuint dest,
                GLuint mask,
                GLuint saturate, GLuint src0, GLuint src1, GLuint src2)
{
   GLuint c[3];
   GLuint nr_const = 0;

   assert(GET_UREG_TYPE(dest) != REG_TYPE_CONST);
   dest = UREG(GET_UREG_TYPE(dest), GET_UREG_NR(dest));
   assert(dest);

   if (GET_UREG_TYPE(src0) == REG_TYPE_CONST)
      c[nr_const++] = 0;
   if (GET_UREG_TYPE(src1) == REG_TYPE_CONST)
      c[nr_const++] = 1;
   if (GET_UREG_TYPE(src2) == REG_TYPE_CONST)
      c[nr_const++] = 2;

   /* Recursively call this function to MOV additional const values
    * into temporary registers.  Use utemp registers for this -
    * currently shouldn't be possible to run out, but keep an eye on
    * this.
    */
   if (nr_const > 1) {
      GLuint s[3], first, i, old_utemp_flag;

      s[0] = src0;
      s[1] = src1;
      s[2] = src2;
      old_utemp_flag = p->utemp_flag;

      first = GET_UREG_NR(s[c[0]]);
      for (i = 1; i < nr_const; i++) {
         if (GET_UREG_NR(s[c[i]]) != first) {
            GLuint tmp = i915_get_utemp(p);

            i915_emit_arith(p, A0_MOV, tmp, A0_DEST_CHANNEL_ALL, 0,
                            s[c[i]], 0, 0);
            s[c[i]] = tmp;
         }
      }

      src0 = s[0];
      src1 = s[1];
      src2 = s[2];
      p->utemp_flag = old_utemp_flag;   /* restore */
   }

   *(p->csr++) = (op | A0_DEST(dest) | mask | saturate | A0_SRC0(src0));
   *(p->csr++) = (A1_SRC0(src0) | A1_SRC1(src1));
   *(p->csr++) = (A2_SRC1(src1) | A2_SRC2(src2));

   p->nr_alu_insn++;
   return dest;
}

GLuint i915_emit_texld( struct i915_fp_compile *p,
			GLuint dest,
			GLuint destmask,
			GLuint sampler,
			GLuint coord,
			GLuint op )
{
   if (coord != UREG(GET_UREG_TYPE(coord), GET_UREG_NR(coord))) {
      /* No real way to work around this in the general case - need to
       * allocate and declare a new temporary register (a utemp won't
       * do).  Will fallback for now.
       */
      i915_program_error(p, "Can't (yet) swizzle TEX arguments");
      return 0;
   }

   /* Don't worry about saturate as we only support  
    */
   if (destmask != A0_DEST_CHANNEL_ALL) {
      GLuint tmp = i915_get_utemp(p);
      i915_emit_texld( p, tmp, A0_DEST_CHANNEL_ALL, sampler, coord, op );
      i915_emit_arith( p, A0_MOV, dest, destmask, 0, tmp, 0, 0 );
      return dest;
   }
   else {
      assert(GET_UREG_TYPE(dest) != REG_TYPE_CONST);
      assert(dest = UREG(GET_UREG_TYPE(dest), GET_UREG_NR(dest)));

      if (GET_UREG_TYPE(coord) != REG_TYPE_T) {
	 p->nr_tex_indirect++;
      }

      *(p->csr++) = (op | 
		     T0_DEST( dest ) |
		     T0_SAMPLER( sampler ));

      *(p->csr++) = T1_ADDRESS_REG( coord );
      *(p->csr++) = T2_MBZ;

      p->nr_tex_insn++;
      return dest;
   }
}


GLuint
i915_emit_const1f(struct i915_fp_compile * p, GLfloat c0)
{
   GLint reg, idx;

   if (c0 == 0.0)
      return swizzle(UREG(REG_TYPE_R, 0), ZERO, ZERO, ZERO, ZERO);
   if (c0 == 1.0)
      return swizzle(UREG(REG_TYPE_R, 0), ONE, ONE, ONE, ONE);

   for (reg = 0; reg < I915_MAX_CONSTANT; reg++) {
      if (p->constant_flags[reg] == I915_CONSTFLAG_PARAM)
         continue;
      for (idx = 0; idx < 4; idx++) {
         if (!(p->constant_flags[reg] & (1 << idx)) ||
             p->fp->constant[reg][idx] == c0) {
            p->fp->constant[reg][idx] = c0;
            p->constant_flags[reg] |= 1 << idx;
            if (reg + 1 > p->fp->nr_constants)
               p->fp->nr_constants = reg + 1;
            return swizzle(UREG(REG_TYPE_CONST, reg), idx, ZERO, ZERO, ONE);
         }
      }
   }

   i915_program_error(p, "i915_emit_const1f: out of constants\n");
   return 0;
}

GLuint
i915_emit_const2f(struct i915_fp_compile * p, GLfloat c0, GLfloat c1)
{
   GLint reg, idx;

   if (c0 == 0.0)
      return swizzle(i915_emit_const1f(p, c1), ZERO, X, Z, W);
   if (c0 == 1.0)
      return swizzle(i915_emit_const1f(p, c1), ONE, X, Z, W);

   if (c1 == 0.0)
      return swizzle(i915_emit_const1f(p, c0), X, ZERO, Z, W);
   if (c1 == 1.0)
      return swizzle(i915_emit_const1f(p, c0), X, ONE, Z, W);

   for (reg = 0; reg < I915_MAX_CONSTANT; reg++) {
      if (p->constant_flags[reg] == 0xf ||
          p->constant_flags[reg] == I915_CONSTFLAG_PARAM)
         continue;
      for (idx = 0; idx < 3; idx++) {
         if (!(p->constant_flags[reg] & (3 << idx))) {
            p->fp->constant[reg][idx] = c0;
            p->fp->constant[reg][idx + 1] = c1;
            p->constant_flags[reg] |= 3 << idx;
            if (reg + 1 > p->fp->nr_constants)
               p->fp->nr_constants = reg + 1;
            return swizzle(UREG(REG_TYPE_CONST, reg), idx, idx + 1, ZERO,
                           ONE);
         }
      }
   }

   i915_program_error(p, "i915_emit_const2f: out of constants\n");
   return 0;
}



GLuint
i915_emit_const4f(struct i915_fp_compile * p,
                  GLfloat c0, GLfloat c1, GLfloat c2, GLfloat c3)
{
   GLint reg;

   for (reg = 0; reg < I915_MAX_CONSTANT; reg++) {
      if (p->constant_flags[reg] == 0xf &&
          p->fp->constant[reg][0] == c0 &&
          p->fp->constant[reg][1] == c1 &&
          p->fp->constant[reg][2] == c2 && 
	  p->fp->constant[reg][3] == c3) {
         return UREG(REG_TYPE_CONST, reg);
      }
      else if (p->constant_flags[reg] == 0) {
         p->fp->constant[reg][0] = c0;
         p->fp->constant[reg][1] = c1;
         p->fp->constant[reg][2] = c2;
         p->fp->constant[reg][3] = c3;
         p->constant_flags[reg] = 0xf;
         if (reg + 1 > p->fp->nr_constants)
            p->fp->nr_constants = reg + 1;
         return UREG(REG_TYPE_CONST, reg);
      }
   }

   i915_program_error(p, "i915_emit_const4f: out of constants\n");
   return 0;
}


GLuint
i915_emit_const4fv(struct i915_fp_compile * p, const GLfloat * c)
{
   return i915_emit_const4f(p, c[0], c[1], c[2], c[3]);
}

/* Reserve a slot in the constant file for a Mesa state parameter.
 * These will later need to be tracked on statechanges, but that is
 * done elsewhere.
 */
GLuint
i915_emit_param4fv(struct i915_fp_compile * p, const GLfloat * values)
{
   struct i915_fragment_program *fp = p->fp;
   GLint i;

   for (i = 0; i < fp->nr_params; i++) {
      if (fp->param[i].values == values)
         return UREG(REG_TYPE_CONST, fp->param[i].reg);
   }

   if (fp->nr_constants == I915_MAX_CONSTANT ||
       fp->nr_params == I915_MAX_CONSTANT) {
      i915_program_error(p, "i915_emit_param4fv: out of constants\n");
      return 0;
   }

   {
      GLint reg = fp->nr_constants++;
      GLint i = fp->nr_params++;

      assert (p->constant_flags[reg] == 0);
      p->constant_flags[reg] = I915_CONSTFLAG_PARAM;

      fp->param[i].values = values;
      fp->param[i].reg = reg;

      return UREG(REG_TYPE_CONST, reg);
   }
}
