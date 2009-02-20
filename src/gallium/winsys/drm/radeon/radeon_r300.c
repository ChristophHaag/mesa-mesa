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

#include "radeon_r300.h"

static boolean radeon_r300_check_cs(struct radeon_cs* cs, int size)
{
    /* XXX check size here, lazy ass! */
    return TRUE;
}

static void radeon_r300_write_cs_reloc(struct radeon_cs* cs,
                                    struct pipe_buffer* pbuffer,
                                    uint32_t rd,
                                    uint32_t wd,
                                    uint32_t flags)
{
    radeon_cs_write_reloc(cs, ((struct radeon_pipe_buffer*)pbuffer)->bo, rd, wd, flags);
}

static void radeon_r300_flush_cs(struct radeon_cs* cs)
{
    radeon_cs_emit(cs);
    radeon_cs_erase(cs);
}

/* Helper function to do the ioctls needed for setup and init. */
static void do_ioctls(struct r300_winsys* winsys, int fd)
{
    drm_radeon_getparam_t gp;
    uint32_t target;
    int retval;

    /* XXX is this cast safe? */
    gp.value = (int*)&target;

    /* First, get PCI ID */
    gp.param = RADEON_PARAM_DEVICE_ID;
    retval = drmCommandWriteRead(fd, DRM_RADEON_GETPARAM, &gp, sizeof(gp));
    if (retval) {
        fprintf(stderr, "%s: Failed to get PCI ID, error number %d",
                __FUNCTION__, retval);
        exit(1);
    }
    winsys->pci_id = target;

    /* Then, get the number of pixel pipes */
    gp.param = RADEON_PARAM_NUM_GB_PIPES;
    retval = drmCommandWriteRead(fd, DRM_RADEON_GETPARAM, &gp, sizeof(gp));
    if (retval) {
        fprintf(stderr, "%s: Failed to get GB pipe count, error number %d",
                __FUNCTION__, retval);
        exit(1);
    }
    winsys->gb_pipes = target;

}

struct r300_winsys* radeon_create_r300_winsys(int fd)
{
    struct r300_winsys* winsys = calloc(1, sizeof(struct r300_winsys));

    do_ioctls(winsys, fd);

    struct radeon_cs_manager* csm = radeon_cs_manager_gem_ctor(fd);

    winsys->cs = radeon_cs_create(csm, 1024 * 64 / 4);

    winsys->check_cs = radeon_r300_check_cs;
    winsys->begin_cs = radeon_cs_begin;
    winsys->write_cs_dword = radeon_cs_write_dword;
    winsys->write_cs_reloc = radeon_r300_write_cs_reloc;
    winsys->end_cs = radeon_cs_end;
    winsys->flush_cs = radeon_r300_flush_cs;

    return winsys;
}
