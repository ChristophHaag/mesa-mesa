/*
 * Copyright (C) 2016 Christian Gmeiner <christian.gmeiner@gmail.com>
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
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 * OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
 * SOFTWARE.
 *
 * Authors:
 *    Christian Gmeiner <christian.gmeiner@gmail.com>
 *    Alexandre Courbot <acourbot@nvidia.com>
 */

#include "tegra_drm_public.h"
#include "nouveau/drm/nouveau_drm_public.h"
#include "renderonly/renderonly.h"

#include <unistd.h>
#include <fcntl.h>
#include <libdrm/tegra_drm.h>
#include <xf86drm.h>

static struct renderonly_scanout *
tegra_create_with_tiling_for_resource(struct pipe_resource *rsc,
                                      struct renderonly *ro)
{
   struct renderonly_scanout *scanout;

   scanout = renderonly_create_gpu_import_for_resource(rsc, ro);
   if (!scanout)
      return NULL;

   struct drm_tegra_gem_set_tiling args = {
      .handle = scanout->handle,
      .mode = DRM_TEGRA_GEM_TILING_MODE_BLOCK,
      .value = 4
   };

   int ret = drmIoctl(ro->kms_fd, DRM_IOCTL_TEGRA_GEM_SET_TILING, &args);
   if (ret)
      return NULL;

   return scanout;
}

struct pipe_screen *tegra_drm_screen_create(int fd)
{
   struct renderonly ro = {
      .create_for_resource = tegra_create_with_tiling_for_resource,
      .kms_fd = fd,
      /* Strangely the X modesetting driver will fail to start *unless*
      * /dev/dri/card1 is opened directly. Even calling drmOpenWithType() with
      * DRM_NODE_PRIMARY will not work (although for a different reason). This is
      * strange since this FD is not supposed to be used directly by X.
      */
      //.gpu_fd = drmOpenWithType("nouveau", NULL, DRM_NODE_RENDER),
      .gpu_fd = open("/dev/dri/card1", O_RDWR),
   };

   if (ro.gpu_fd < 0)
      return NULL;

   struct pipe_screen *screen = nouveau_drm_screen_create_renderonly(ro.gpu_fd, &ro);
   if (!screen)
      drmClose(ro.gpu_fd);

   return screen;
};
