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

#include "r300_state_tcl.h"

static void r300_vs_declare(struct r300_vs_asm* assembler,
                            struct tgsi_full_declaration* decl)
{
    switch (decl->Declaration.File) {
        case TGSI_FILE_INPUT:
            switch (decl->Semantic.SemanticName) {
                case TGSI_SEMANTIC_COLOR:
                    assembler->color_count++;
                    break;
                case TGSI_SEMANTIC_GENERIC:
                    assembler->tex_count++;
                    break;
                default:
                    debug_printf("r300: vs: Bad semantic declaration %d\n",
                        decl->Semantic.SemanticName);
                    break;
            }
            break;
        case TGSI_FILE_OUTPUT:
        case TGSI_FILE_CONSTANT:
            break;
        case TGSI_FILE_TEMPORARY:
            assembler->temp_count++;
            break;
        default:
            debug_printf("r300: vs: Bad file %d\n", decl->Declaration.File);
            break;
    }

    assembler->temp_offset = assembler->color_count + assembler->tex_count;
}

static INLINE unsigned r300_vs_src(struct r300_vs_asm* assembler,
                                   struct tgsi_src_register* src)
{
    switch (src->File) {
        case TGSI_FILE_NULL:
            return 0;
        case TGSI_FILE_INPUT:
            /* XXX may be wrong */
            return src->Index;
            break;
        case TGSI_FILE_TEMPORARY:
            return src->Index + assembler->temp_offset;
            break;
        case TGSI_FILE_IMMEDIATE:
            return (src->Index + assembler->imm_offset) | (1 << 8);
            break;
        case TGSI_FILE_CONSTANT:
            /* XXX magic */
            return src->Index | (1 << 8);
            break;
        default:
            debug_printf("r300: vs: Unimplemented src %d\n", src->File);
            break;
    }
    return 0;
}

static INLINE unsigned r300_vs_dst(struct r300_vs_asm* assembler,
                                   struct tgsi_dst_register* dst)
{
    switch (dst->File) {
        case TGSI_FILE_NULL:
            /* This happens during KIL instructions. */
            return 0;
            break;
        case TGSI_FILE_OUTPUT:
            return 0;
            break;
        case TGSI_FILE_TEMPORARY:
            return dst->Index + assembler->temp_offset;
            break;
        default:
            debug_printf("r300: vs: Unimplemented dst %d\n", dst->File);
            break;
    }
    return 0;
}

static void r300_vs_emit_inst(struct r300_vertex_shader* vs,
                              struct r300_vs_asm* assembler,
                              struct tgsi_full_src_register* src,
                              struct tgsi_full_dst_register* dst)
{
    int i = vs->instruction_count;
    vs->instructions[i].inst0 = R300_PVS_DST_OPCODE(R300_VE_ADD) |
        R300_PVS_DST_REG_TYPE(R300_PVS_DST_REG_OUT) |
        R300_PVS_DST_OFFSET(dst->DstRegister.Index);
}

static void r300_vs_instruction(struct r300_vertex_shader* vs,
                                struct r300_vs_asm* assembler,
                                struct tgsi_full_instruction* inst)
{
    switch (inst->Instruction.Opcode) {
        case TGSI_OPCODE_MOV:
            r300_vs_emit_inst(vs, assembler, inst->FullSrcRegisters,
                    &inst->FullDstRegisters[0]);
            break;
        case TGSI_OPCODE_END:
            break;
        default:
            debug_printf("r300: vs: Bad opcode %d\n",
                    inst->Instruction.Opcode);
            break;
    }
}

void r300_translate_vertex_shader(struct r300_context* r300,
                                  struct r300_vertex_shader* vs)
{
    struct tgsi_parse_context parser;
    int i;
    struct r300_constant_buffer* consts =
        &r300->shader_constants[PIPE_SHADER_VERTEX];

    struct r300_vs_asm* assembler = CALLOC_STRUCT(r300_vs_asm);
    if (assembler == NULL) {
        return;
    }
    /* Setup starting offset for immediates. */
    assembler->imm_offset = consts->user_count;

    tgsi_parse_init(&parser, vs->state.tokens);

    while (!tgsi_parse_end_of_tokens(&parser)) {
        tgsi_parse_token(&parser);

        /* This is seriously the lamest way to create fragment programs ever.
         * I blame TGSI. */
        switch (parser.FullToken.Token.Type) {
            case TGSI_TOKEN_TYPE_DECLARATION:
                /* Allocated registers sitting at the beginning
                 * of the program. */
                r300_vs_declare(assembler, &parser.FullToken.FullDeclaration);
                break;
            case TGSI_TOKEN_TYPE_IMMEDIATE:
                debug_printf("r300: Emitting immediate to constant buffer, "
                        "position %d\n",
                        assembler->imm_offset + assembler->imm_count);
                /* I am not amused by the length of these. */
                for (i = 0; i < 4; i++) {
                    consts->constants[assembler->imm_offset +
                        assembler->imm_count][i] =
                        parser.FullToken.FullImmediate.u.ImmediateFloat32[i]
                        .Float;
                }
                assembler->imm_count++;
                break;
            case TGSI_TOKEN_TYPE_INSTRUCTION:
                r300_vs_instruction(vs, assembler,
                        &parser.FullToken.FullInstruction);
                break;
        }
    }

    debug_printf("r300: vs: %d texs and %d colors, first free reg is %d\n",
            assembler->tex_count, assembler->color_count,
            assembler->tex_count + assembler->color_count);

    consts->count = consts->user_count + assembler->imm_count;
    debug_printf("r300: vs: %d total constants, "
            "%d from user and %d from immediates\n", consts->count,
            consts->user_count, assembler->imm_count);

    tgsi_dump(vs->state.tokens);
    /* XXX finish r300 vertex shader dumper */

    tgsi_parse_free(&parser);
    FREE(assembler);
}
