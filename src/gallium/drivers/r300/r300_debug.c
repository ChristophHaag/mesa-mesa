/*
 * Copyright 2009 Corbin Simpson <MostAwesomeDude@gmail.com>
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
 * FITNESS FOR A PARTICULAR PURPOSE AND NON-INFRINGEMENT. IN NO EVENT SHALL
 * THE AUTHOR(S) AND/OR THEIR SUPPLIERS BE LIABLE FOR ANY CLAIM,
 * DAMAGES OR OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR
 * OTHERWISE, ARISING FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE
 * USE OR OTHER DEALINGS IN THE SOFTWARE. */

#include "r300_debug.h"

void r3xx_dump_fs(struct r3xx_fragment_shader* fs)
{
    int i;

    for (i = 0; i < fs->alu_instruction_count; i++) {
    }
}

void r5xx_fs_dump(struct r5xx_fragment_shader* fs)
{
    int i;
    uint32_t inst;

    for (i = 0; i < fs->instruction_count; i++) {
        inst = fs->instructions[i].inst0;
        debug_printf("%d:  0: CMN_INST   0x%08x:", i, inst);
        switch (inst & 0x3) {
            case R500_INST_TYPE_ALU:
                debug_printf("ALU ");
                break;
            case R500_INST_TYPE_OUT:
                debug_printf("OUT ");
                break;
            case R500_INST_TYPE_FC:
                debug_printf("FC  ");
                break;
            case R500_INST_TYPE_TEX:
                debug_printf("TEX ");
                break;
        }
        debug_printf("%s %s %s %s ",
                inst & R500_INST_TEX_SEM_WAIT ? "TEX_WAIT" : "",
                inst & R500_INST_LAST ? "LAST" : "",
                inst & R500_INST_NOP ? "NOP" : "",
                inst & R500_INST_ALU_WAIT ? "ALU_WAIT" : "");
        debug_printf("wmask: %s omask: %s\n",
                r5xx_fs_mask[(inst >> 11) & 0xf],
                r5xx_fs_mask[(inst >> 15) & 0xf]);
        switch (inst & 0x3) {
            case R500_INST_TYPE_ALU:
            case R500_INST_TYPE_OUT:
                inst = fs->instructions[i].inst1;
                debug_printf("    1: RGB_ADDR   0x%08x:", inst);
                debug_printf("Addr0: %d%c, Addr1: %d%c, "
                        "Addr2: %d%c, srcp:%d\n",
                        inst & 0xff, (inst & (1 << 8)) ? 'c' : 't',
                        (inst >> 10) & 0xff, (inst & (1 << 18)) ? 'c' : 't',
                        (inst >> 20) & 0xff, (inst & (1 << 28)) ? 'c' : 't',
                        (inst >> 30));

                inst = fs->instructions[i].inst2;
                debug_printf("    2: ALPHA_ADDR 0x%08x:", inst);
                debug_printf("Addr0: %d%c, Addr1: %d%c, "
                        "Addr2: %d%c, srcp:%d\n",
                        inst & 0xff, (inst & (1 << 8)) ? 'c' : 't',
                        (inst >> 10) & 0xff, (inst & (1 << 18)) ? 'c' : 't',
                        (inst >> 20) & 0xff, (inst & (1 << 28)) ? 'c' : 't',
                        (inst >> 30));

                inst = fs->instructions[i].inst3;
                debug_printf("    3: RGB_INST   0x%08x:", inst);
                debug_printf("rgb_A_src:%d %s/%s/%s %d "
                        "rgb_B_src:%d %s/%s/%s %d\n",
                        inst & 0x3, r5xx_fs_swiz[(inst >> 2) & 0x7],
                        r5xx_fs_swiz[(inst >> 5) & 0x7],
                        r5xx_fs_swiz[(inst >> 8) & 0x7],
                        (inst >> 11) & 0x3, (inst >> 13) & 0x3,
                        r5xx_fs_swiz[(inst >> 15) & 0x7],
                        r5xx_fs_swiz[(inst >> 18) & 0x7],
                        r5xx_fs_swiz[(inst >> 21) & 0x7],
                        (inst >> 24) & 0x3);

                inst = fs->instructions[i].inst4;
                debug_printf("    4: ALPHA_INST 0x%08x:", inst);
                debug_printf("%s dest:%d%s alp_A_src:%d %s %d "
                        "alp_B_src:%d %s %d w:%d\n",
                        r5xx_fs_op_alpha[inst & 0xf], (inst >> 4) & 0x7f,
                        inst & (1<<11) ? "(rel)":"", (inst >> 12) & 0x3,
                        r5xx_fs_swiz[(inst >> 14) & 0x7], (inst >> 17) & 0x3,
                        (inst >> 19) & 0x3, r5xx_fs_swiz[(inst >> 21) & 0x7],
                        (inst >> 24) & 0x3, (inst >> 31) & 0x1);

                inst = fs->instructions[i].inst5;
                debug_printf("    5: RGBA_INST  0x%08x:", inst);
                debug_printf("%s dest:%d%s rgb_C_src:%d %s/%s/%s %d "
                        "alp_C_src:%d %s %d\n",
                        r5xx_fs_op_rgb[inst & 0xf], (inst >> 4) & 0x7f,
                        inst & (1 << 11) ? "(rel)":"", (inst >> 12) & 0x3,
                        r5xx_fs_swiz[(inst >> 14) & 0x7],
                        r5xx_fs_swiz[(inst >> 17) & 0x7],
                        r5xx_fs_swiz[(inst >> 20) & 0x7],
                        (inst >> 23) & 0x3, (inst >> 25) & 0x3,
                        r5xx_fs_swiz[(inst >> 27) & 0x7], (inst >> 30) & 0x3);
                break;
            case R500_INST_TYPE_FC:
                /* XXX don't even bother yet */
                break;
            case R500_INST_TYPE_TEX:
                inst = fs->instructions[i].inst1;
                debug_printf("    1: TEX_INST   0x%08x: id: %d "
                        "op:%s, %s, %s %s\n",
                        inst, (inst >> 16) & 0xf,
                        r5xx_fs_tex[(inst >> 22) & 0x7],
                        (inst & (1 << 25)) ? "ACQ" : "",
                        (inst & (1 << 26)) ? "IGNUNC" : "",
                        (inst & (1 << 27)) ? "UNSCALED" : "SCALED");

                inst = fs->instructions[i].inst2;
                debug_printf("    2: TEX_ADDR   0x%08x: "
                        "src: %d%s %s/%s/%s/%s dst: %d%s %s/%s/%s/%s\n",
                        inst, inst & 0x7f, inst & (1 << 7) ? "(rel)" : "",
                        r5xx_fs_swiz[(inst >> 8) & 0x3],
                        r5xx_fs_swiz[(inst >> 10) & 0x3],
                        r5xx_fs_swiz[(inst >> 12) & 0x3],
                        r5xx_fs_swiz[(inst >> 14) & 0x3],
                        (inst >> 16) & 0x7f, inst & (1 << 23) ? "(rel)" : "",
                        r5xx_fs_swiz[(inst >> 24) & 0x3],
                        r5xx_fs_swiz[(inst >> 26) & 0x3],
                        r5xx_fs_swiz[(inst >> 28) & 0x3],
                        r5xx_fs_swiz[(inst >> 30) & 0x3]);
                
                inst = fs->instructions[i].inst3;
                debug_printf("    3: TEX_DXDY   0x%08x\n", inst);
                break;
        }
    }
}

static void r300_vs_op_dump(uint32_t op)
{
    debug_printf(" dst: %d%s op: ",
            (op >> 13) & 0x7f, r300_vs_dst_debug[(op >> 8) & 0x7]);
    if (op & 0x80) {
        if (op & 0x1) {
            debug_printf("PVS_MACRO_OP_2CLK_M2X_ADD\n");
        } else {
            debug_printf("   PVS_MACRO_OP_2CLK_MADD\n");
        }
    } else if (op & 0x40) {
        debug_printf("%s\n", r300_vs_me_ops[op & 0x1f]);
    } else {
        debug_printf("%s\n", r300_vs_ve_ops[op & 0x1f]);
    }
}

void r300_vs_src_dump(uint32_t src)
{
    debug_printf(" reg: %d%s swiz: %s%s/%s%s/%s%s/%s%s\n",
            (src >> 5) & 0x7f, r300_vs_src_debug[src & 0x3],
            src & (1 << 25) ? "-" : " ",
            r300_vs_swiz_debug[(src >> 13) & 0x7],
            src & (1 << 26) ? "-" : " ",
            r300_vs_swiz_debug[(src >> 16) & 0x7],
            src & (1 << 27) ? "-" : " ",
            r300_vs_swiz_debug[(src >> 19) & 0x7],
            src & (1 << 28) ? "-" : " ",
            r300_vs_swiz_debug[(src >> 22) & 0x7]);
}

void r300_vs_dump(struct r300_vertex_shader* vs)
{
    int i;

    for (i = 0; i < vs->instruction_count; i++) {
        debug_printf("%d: op: 0x%08x", i, vs->instructions[i].inst0);
        r300_vs_op_dump(vs->instructions[i].inst0);
        debug_printf(" src0: 0x%08x", vs->instructions[i].inst1);
        r300_vs_src_dump(vs->instructions[i].inst1);
        debug_printf(" src1: 0x%08x", vs->instructions[i].inst2);
        r300_vs_src_dump(vs->instructions[i].inst2);
        debug_printf(" src2: 0x%08x", vs->instructions[i].inst3);
        r300_vs_src_dump(vs->instructions[i].inst3);
    }
}
