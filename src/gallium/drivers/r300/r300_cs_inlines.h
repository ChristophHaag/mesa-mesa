/*
 * Copyright 2008 Corbin Simpson <MostAwesomeDude@gmail.com>
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

/* r300_cs_inlines: This is just a handful of useful inlines for sending
 * (very) common instructions to the CS buffer. Should only be included from
 * r300_cs.h, probably. */

#ifdef R300_CS_H

#define RADEON_ONE_REG_WR        (1 << 15)

#define OUT_CS_ONE_REG(register, count) do { \
    if (VERY_VERBOSE_REGISTERS) \
        debug_printf("r300: writing data sequence of %d to 0x%04X\n", \
            count, register); \
    assert(register); \
    OUT_CS(CP_PACKET0(register, ((count) - 1)) | RADEON_ONE_REG_WR); \
} while (0)

#define R300_PACIFY do { \
    OUT_CS_REG(RADEON_WAIT_UNTIL, (1 << 14) | (1 << 15) | (1 << 16) | (1 << 17) | \
        (1 << 18)); \
} while (0)

#define R300_SCREENDOOR do { \
    OUT_CS_REG(R300_SC_SCREENDOOR, 0x0); \
    R300_PACIFY; \
    OUT_CS_REG(R300_SC_SCREENDOOR, 0xffffff); \
} while (0)

#endif /* R300_CS_H */
