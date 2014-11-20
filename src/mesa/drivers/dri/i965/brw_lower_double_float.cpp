/*
 * Copyright © 2014 Intel Corporation
 *
 * Permission is hereby granted, free of charge, to any person obtaining a
 * copy of this software and associated documentation files (the "Software"),
 * to deal in the Software without restriction, including without limitation
 * the rights to use, copy, modify, merge, publish, distribute, sublicense,
 * and/or sell copies of the Software, and to permit persons to whom the
 * Software is furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice (including the next
 * paragraph) shall be included in all copies or substantial portions of the
 * Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.  IN NO EVENT SHALL
 * THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING
 * FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER
 * DEALINGS IN THE SOFTWARE.
 */

/**
 * \file brw_lower_double_float.cpp
 *
 * Breaks operations dealing with double precision floats into two
 * instructions each addressing one half of the channels involved.
 *
 * When hardware operates on double precision floats each channel takes
 * 64-bits. The number of bits processed by an instruction is fixed and
 * hence one gets results only for half the number of channels compared to
 * when operating with 32-bit channels.
 * This lowering pass replaces each double precision operation with two
 * operations offseting the registers accordingly so that execution width
 * many channels get processed in the end.
 */

#include "brw_shader.h"
#include "brw_cfg.h"
#include "brw_fs.h"

class inst_traits {
public:
   virtual bool is_double(const backend_instruction *) const = 0;
   virtual backend_instruction *get_2nd_half(backend_instruction *) const = 0;
   virtual backend_instruction *get_pack_2x32_2nd_half(
                                   backend_instruction *) const = 0;
   virtual void lower_double_conversion(bblock_t *,
                                        backend_instruction *) const = 0;
   virtual void lower_cmp(bblock_t *, backend_instruction *) const = 0;
};

static bool
lower_double_float(const inst_traits& traits,
                   backend_visitor *v,
                   cfg_t *cfg)
{
   bool progress = false;

   foreach_block_and_inst_safe (block, fs_inst, inst, cfg) {
      if (!traits.is_double(inst))
         continue;

      if (inst->opcode == FS_OPCODE_UNIFORM_DOUBLE_LOAD ||
          inst->opcode == FS_OPCODE_UNPACK_DOUBLE_2x32_X ||
          inst->opcode == FS_OPCODE_UNPACK_DOUBLE_2x32_Y)
         continue;

      switch (inst->opcode) {
      case FS_OPCODE_PACK_DOUBLE_2x32:
         inst->insert_after(block, traits.get_pack_2x32_2nd_half(inst));
         break;
      case BRW_OPCODE_MOV:
         /* Conversion from double to single precision writes 64 bits per
          * element leaving the upper 32 bits undefined. Hence there has to
          * be first a separate conversion followed by a copy from the lower
          * 32-bits to the final destination.
          */
         if (inst->dst.type != BRW_REGISTER_TYPE_DF) {
            traits.lower_double_conversion(block, inst);
            break;
         }         
      case BRW_OPCODE_CMP:
         traits.lower_cmp(block, inst);
         break;
      default:
         inst->insert_after(block, traits.get_2nd_half(inst));
      }
      progress = true;
   }

   return progress;
}

class fs_inst_traits : public inst_traits {
public:
   explicit fs_inst_traits(backend_visitor *base_v) :
      v((fs_visitor *)base_v)
   {
   }

   virtual bool is_double(const backend_instruction *base_inst) const;
   virtual backend_instruction *get_2nd_half(backend_instruction *inst) const;
   virtual backend_instruction *get_pack_2x32_2nd_half(
                                   backend_instruction *inst) const;
   virtual void lower_double_conversion(bblock_t *block,
                                        backend_instruction *inst) const;
   virtual void lower_cmp(bblock_t *block, backend_instruction *cmp) const;

private:
   fs_reg get_2nd_half(fs_reg reg) const;

   fs_visitor *v;
};

bool
fs_inst_traits::is_double(const backend_instruction *base_inst) const
{
   const fs_inst *inst = ((const fs_inst *)base_inst);

   if (inst->src[0].type == BRW_REGISTER_TYPE_DF) {
      if (inst->sources > 1)
         assert(inst->src[1].type == BRW_REGISTER_TYPE_DF);
      if (inst->sources > 2)
         assert(inst->src[2].type == BRW_REGISTER_TYPE_DF);
      return true;
   }

   return inst->dst.type == BRW_REGISTER_TYPE_DF;
}

fs_reg
fs_inst_traits::get_2nd_half(fs_reg reg) const
{
   if (reg.file == GRF &&
       reg.width &&
       reg.type == BRW_REGISTER_TYPE_DF)
      reg.reg_offset += (reg.width / 8);
   else if (reg.type != BRW_REGISTER_TYPE_DF)
      reg = horiz_offset(reg, 4);

   return reg;
}

backend_instruction *
fs_inst_traits::get_2nd_half(backend_instruction *base_inst) const
{
   fs_inst *inst = new(v->mem_ctx) fs_inst(*(const fs_inst *)base_inst);

   for (unsigned i = 0; i < inst->sources; ++i)
      inst->src[i] = get_2nd_half(inst->src[i]);

   inst->dst = get_2nd_half(inst->dst);

   return inst;
}

backend_instruction *
fs_inst_traits::get_pack_2x32_2nd_half(backend_instruction *base_inst) const
{
   fs_inst *inst = new(v->mem_ctx) fs_inst(*(const fs_inst *)base_inst);

   for (unsigned i = 0; i < inst->sources; ++i)
      inst->src[i] = horiz_offset(inst->src[i], inst->exec_size / 2);

   inst->dst = get_2nd_half(inst->dst);

   return inst;
}

void
fs_inst_traits::lower_double_conversion(bblock_t *block,
                                        backend_instruction *base_inst) const
{
   fs_reg x(v, glsl_type::float_type);
   fs_reg y(v, glsl_type::float_type);
   fs_inst *inst_mov_x = ((fs_inst *)base_inst);
   fs_reg orig_dst(inst_mov_x->dst);

   /* HACK: Force the correct type */
   x.type = orig_dst.type;
   y.type = orig_dst.type;

   /* Use the original instruction to convert the first half to a temp. */
   inst_mov_x->dst = x;

   /* Convert the second half. */
   fs_inst *inst_mov_y = new(v->mem_ctx) fs_inst(
      BRW_OPCODE_MOV, y, get_2nd_half(inst_mov_x->src[0]));
   inst_mov_x->insert_after(block, inst_mov_y);

   /* Pack the results. */
   inst_mov_y->insert_after(
      block,
      new(v->mem_ctx) fs_inst(
         SHADER_OPCODE_MOV_LOW_2x32_HALF_EXEC_WIDTH, orig_dst, x, y));
}

void
fs_inst_traits::lower_cmp(bblock_t *block, backend_instruction *cmp) const
{
   fs_inst *cmp_1st_half = ((fs_inst *)cmp);

   if (cmp_1st_half->dst.type == BRW_REGISTER_TYPE_DF) {
      cmp_1st_half->insert_after(block, get_2nd_half(cmp_1st_half));
      return;
   }

   fs_reg orig_dst = cmp_1st_half->dst;
   fs_reg tmp_res(v, glsl_type::double_type);

   cmp_1st_half->dst = tmp_res;
   fs_inst *cmp_2nd_half = (fs_inst *)get_2nd_half(cmp_1st_half);
   cmp_1st_half->insert_after(block, cmp_2nd_half);

   /* Pack the double precision results into single precision - all the
    * 64-bits are simply up or down per channel, and one can simply pick
    * either the high or low 32-bits.
    */
   fs_reg res_f(v, glsl_type::float_type);
   fs_inst *pack = new(v->mem_ctx) fs_inst(
                      SHADER_OPCODE_MOV_LOW_2x32_HALF_EXEC_WIDTH,
                      res_f,
                      retype(tmp_res, BRW_REGISTER_TYPE_F),
                      offset(retype(tmp_res, BRW_REGISTER_TYPE_F), 1));
   cmp_2nd_half->insert_after(block, pack);

   fs_inst *cmp_f = new(v->mem_ctx) fs_inst(BRW_OPCODE_CMP,
                                            orig_dst, res_f, fs_reg(0.0f));
   cmp_f->conditional_mod = BRW_CONDITIONAL_NEQ;
   pack->insert_after(block, cmp_f);
}

bool
fs_visitor::lower_double_float()
{
   return ::lower_double_float(fs_inst_traits(this), this, cfg);
}
