/*
 * Copyright © 2017 Keith Packard
 *
 * Permission to use, copy, modify, distribute, and sell this software and its
 * documentation for any purpose is hereby granted without fee, provided that
 * the above copyright notice appear in all copies and that both that copyright
 * notice and this permission notice appear in supporting documentation, and
 * that the name of the copyright holders not be used in advertising or
 * publicity pertaining to distribution of the software without specific,
 * written prior permission.  The copyright holders make no representations
 * about the suitability of this software for any purpose.  It is provided "as
 * is" without express or implied warranty.
 *
 * THE COPYRIGHT HOLDERS DISCLAIM ALL WARRANTIES WITH REGARD TO THIS SOFTWARE,
 * INCLUDING ALL IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS, IN NO
 * EVENT SHALL THE COPYRIGHT HOLDERS BE LIABLE FOR ANY SPECIAL, INDIRECT OR
 * CONSEQUENTIAL DAMAGES OR ANY DAMAGES WHATSOEVER RESULTING FROM LOSS OF USE,
 * DATA OR PROFITS, WHETHER IN AN ACTION OF CONTRACT, NEGLIGENCE OR OTHER
 * TORTIOUS ACTION, ARISING OUT OF OR IN CONNECTION WITH THE USE OR PERFORMANCE
 * OF THIS SOFTWARE.
 */

#include "util/macros.h"
#include <stdlib.h>
#include <stdio.h>
#include <unistd.h>
#include <errno.h>
#include <string.h>
#include <fcntl.h>
#include <poll.h>
#include <stdbool.h>
#include <math.h>
#include <xf86drm.h>
#include <xf86drmMode.h>
#include <drm_fourcc.h>
#ifdef VK_USE_PLATFORM_XLIB_XRANDR_EXT
#include <xcb/randr.h>
#include <X11/Xlib-xcb.h>
#endif
#include "util/hash_table.h"
#include "util/list.h"

#include "vk_util.h"
#include "wsi_common_private.h"
#include "wsi_common_display.h"
#include "wsi_common_queue.h"

#if 0
#define wsi_display_debug(...) fprintf(stderr, __VA_ARGS__)
#define wsi_display_debug_code(...)     __VA_ARGS__
#else
#define wsi_display_debug(...)
#define wsi_display_debug_code(...)
#endif

/* These have lifetime equal to the instance, so they effectively
 * never go away. This means we must keep track of them separately
 * from all other resources.
 */
typedef struct wsi_display_mode {
   struct list_head             list;
   struct wsi_display_connector *connector;
   bool                         valid;          /* was found in most recent poll */
   bool                         preferred;
   uint32_t                     clock;          /* in kHz */
   uint16_t                     hdisplay, hsync_start, hsync_end, htotal, hskew;
   uint16_t                     vdisplay, vsync_start, vsync_end, vtotal, vscan;
   uint32_t                     flags;
} wsi_display_mode;

typedef struct wsi_display_connector {
   struct list_head             list;
   struct wsi_display           *wsi;
   uint32_t                     id;
   uint32_t                     crtc_id;
   char                         *name;
   bool                         connected;
   bool                         active;
   struct list_head             display_modes;
   wsi_display_mode             *current_mode;
   drmModeModeInfo              current_drm_mode;
#ifdef VK_USE_PLATFORM_XLIB_XRANDR_EXT
   xcb_randr_output_t           output;
#endif
} wsi_display_connector;

struct wsi_display {
   struct wsi_interface         base;

   const VkAllocationCallbacks  *alloc;

   int                          fd;

   pthread_mutex_t              wait_mutex;
   pthread_cond_t               wait_cond;
   pthread_t                    wait_thread;

   struct list_head             connectors;
};

enum wsi_image_state {
   WSI_IMAGE_IDLE,
   WSI_IMAGE_DRAWING,
   WSI_IMAGE_QUEUED,
   WSI_IMAGE_FLIPPING,
   WSI_IMAGE_DISPLAYING
};

struct wsi_display_image {
   struct wsi_image             base;
   struct wsi_display_swapchain *chain;
   enum wsi_image_state         state;
   uint32_t                     fb_id;
   uint32_t                     buffer[4];
   uint64_t                     flip_sequence;
};

struct wsi_display_swapchain {
   struct wsi_swapchain         base;
   struct wsi_display           *wsi;
   VkIcdSurfaceDisplay          *surface;
   uint64_t                     flip_sequence;
   VkResult                     status;
   struct wsi_display_image     images[0];
};

ICD_DEFINE_NONDISP_HANDLE_CASTS(wsi_display_mode, VkDisplayModeKHR)
ICD_DEFINE_NONDISP_HANDLE_CASTS(wsi_display_connector, VkDisplayKHR)

static bool
wsi_display_mode_matches_drm(wsi_display_mode   *wsi,
                             drmModeModeInfoPtr drm)
{
   return wsi->clock == drm->clock &&
      wsi->hdisplay == drm->hdisplay &&
      wsi->hsync_start == drm->hsync_start &&
      wsi->hsync_end == drm->hsync_end &&
      wsi->htotal == drm->htotal &&
      wsi->hskew == drm->hskew &&
      wsi->vdisplay == drm->vdisplay &&
      wsi->vsync_start == drm->vsync_start &&
      wsi->vsync_end == drm->vsync_end &&
      wsi->vtotal == drm->vtotal &&
      wsi->vscan == drm->vscan &&
      wsi->flags == drm->flags;
}

static double
wsi_display_mode_refresh(struct wsi_display_mode        *wsi)
{
   return (double) wsi->clock * 1000.0 / ((double) wsi->htotal * (double) wsi->vtotal * (double) (wsi->vscan + 1));
}

static uint64_t wsi_get_current_monotonic(void)
{
   struct timespec tv;

   clock_gettime(CLOCK_MONOTONIC, &tv);
   return tv.tv_nsec + tv.tv_sec*1000000000ull;
}

static struct wsi_display_mode *
wsi_display_find_drm_mode(struct wsi_device                 *wsi_device,
                          struct wsi_display_connector      *connector,
                          drmModeModeInfoPtr                mode)
{
   struct wsi_display_mode      *display_mode;

   LIST_FOR_EACH_ENTRY(display_mode, &connector->display_modes, list) {
      if (wsi_display_mode_matches_drm(display_mode, mode))
         return display_mode;
   }
   return NULL;
}

static void
wsi_display_invalidate_connector_modes(struct wsi_device            *wsi_device,
                                       struct wsi_display_connector *connector)
{
   struct wsi_display_mode      *display_mode;

   LIST_FOR_EACH_ENTRY(display_mode, &connector->display_modes, list)
      display_mode->valid = false;
}

static VkResult
wsi_display_register_drm_mode(struct wsi_device            *wsi_device,
                              struct wsi_display_connector *connector,
                              drmModeModeInfoPtr           drm_mode)
{
   struct wsi_display           *wsi = (struct wsi_display *) wsi_device->wsi[VK_ICD_WSI_PLATFORM_DISPLAY];
   struct wsi_display_mode      *display_mode;

   display_mode = wsi_display_find_drm_mode(wsi_device, connector, drm_mode);

   if (display_mode) {
      display_mode->valid = true;
      return VK_SUCCESS;
   }

   display_mode = vk_zalloc(wsi->alloc, sizeof (struct wsi_display_mode), 8, VK_SYSTEM_ALLOCATION_SCOPE_INSTANCE);
   if (!display_mode)
      return VK_ERROR_OUT_OF_HOST_MEMORY;

   display_mode->connector = connector;
   display_mode->valid = true;
   display_mode->preferred = (drm_mode->type & DRM_MODE_TYPE_PREFERRED) != 0;
   display_mode->clock = drm_mode->clock; /* kHz */
   display_mode->hdisplay = drm_mode->hdisplay;
   display_mode->hsync_start = drm_mode->hsync_start;
   display_mode->hsync_end = drm_mode->hsync_end;
   display_mode->htotal = drm_mode->htotal;
   display_mode->hskew = drm_mode->hskew;
   display_mode->vdisplay = drm_mode->vdisplay;
   display_mode->vsync_start = drm_mode->vsync_start;
   display_mode->vsync_end = drm_mode->vsync_end;
   display_mode->vtotal = drm_mode->vtotal;
   display_mode->vscan = drm_mode->vscan;
   display_mode->flags = drm_mode->flags;

   LIST_ADDTAIL(&display_mode->list, &connector->display_modes);
   return VK_SUCCESS;
}

/*
 * Update our information about a specific connector
 */

static struct wsi_display_connector *
wsi_display_find_connector(struct wsi_device    *wsi_device,
                          uint32_t              connector_id)
{
   struct wsi_display           *wsi = (struct wsi_display *) wsi_device->wsi[VK_ICD_WSI_PLATFORM_DISPLAY];
   struct wsi_display_connector *connector;

   connector = NULL;
   LIST_FOR_EACH_ENTRY(connector, &wsi->connectors, list) {
      if (connector->id == connector_id)
         return connector;
   }

   return NULL;
}

static struct wsi_display_connector *
wsi_display_alloc_connector(struct wsi_display  *wsi,
                            uint32_t            connector_id)
{
   struct wsi_display_connector *connector;

   connector = vk_zalloc(wsi->alloc, sizeof (struct wsi_display_connector), 8, VK_SYSTEM_ALLOCATION_SCOPE_INSTANCE);
   connector->id = connector_id;
   connector->wsi = wsi;
   connector->active = false;
   /* XXX use EDID name */
   connector->name = "monitor";
   LIST_INITHEAD(&connector->display_modes);
   return connector;
}

static struct wsi_display_connector *
wsi_display_get_connector(struct wsi_device             *wsi_device,
                          uint32_t                      connector_id)
{
   struct wsi_display           *wsi = (struct wsi_display *) wsi_device->wsi[VK_ICD_WSI_PLATFORM_DISPLAY];
   struct wsi_display_connector *connector;
   drmModeConnectorPtr          drm_connector;
   VkResult                     result;
   int                          m;

   if (wsi->fd < 0)
      return NULL;

   drm_connector = drmModeGetConnector(wsi->fd, connector_id);
   if (!drm_connector)
      return NULL;

   connector = wsi_display_find_connector(wsi_device, connector_id);

   if (!connector) {
      connector = wsi_display_alloc_connector(wsi, connector_id);
      if (!connector) {
         drmModeFreeConnector(drm_connector);
         return NULL;
      }
      LIST_ADDTAIL(&connector->list, &wsi->connectors);
   }

   connector->connected = drm_connector->connection != DRM_MODE_DISCONNECTED;

   /* Mark all connector modes as invalid */
   wsi_display_invalidate_connector_modes(wsi_device, connector);

   /*
    * List current modes, adding new ones and marking existing ones as
    * valid
    */
   for (m = 0; m < drm_connector->count_modes; m++) {
      result = wsi_display_register_drm_mode(wsi_device,
                                             connector,
                                             &drm_connector->modes[m]);
      if (result != VK_SUCCESS) {
         drmModeFreeConnector(drm_connector);
         return NULL;
      }
   }

   drmModeFreeConnector(drm_connector);

   return connector;
}

#define MM_PER_PIXEL     (1.0/96.0 * 25.4)

static uint32_t
mode_size(struct wsi_display_mode *mode) {
   /* fortunately, these are both uint16_t, so this is easy */
   return (uint32_t) mode->hdisplay * (uint32_t) mode->vdisplay;
}

static void
wsi_display_fill_in_display_properties(struct wsi_device                *wsi_device,
                                       struct wsi_display_connector     *connector,
                                       VkDisplayPropertiesKHR           *properties)
{
   struct wsi_display_mode      *display_mode, *preferred_mode = NULL, *largest_mode = NULL;

   properties->display = wsi_display_connector_to_handle(connector);
   properties->displayName = connector->name;

   /* Find the preferred mode and assume that's the physical resolution. If
    * there isn't a preferred mode, find the largest mode and use that.
    */

   LIST_FOR_EACH_ENTRY(display_mode, &connector->display_modes, list) {
      if (display_mode->valid && (largest_mode == NULL || mode_size(display_mode) > mode_size(largest_mode)))
         largest_mode = display_mode;
      if (display_mode->valid && display_mode->preferred) {
         preferred_mode = display_mode;
         break;
      }
   }

   if (preferred_mode) {
      properties->physicalResolution.width = preferred_mode->hdisplay;
      properties->physicalResolution.height = preferred_mode->vdisplay;
   } else if (largest_mode) {
      properties->physicalResolution.width = largest_mode->hdisplay;
      properties->physicalResolution.height = largest_mode->vdisplay;
   } else {
      properties->physicalResolution.width = 1024;
      properties->physicalResolution.height = 768;
   }

   /* Make up physical size based on 96dpi */
   properties->physicalDimensions.width = floor(properties->physicalResolution.width * MM_PER_PIXEL + 0.5);
   properties->physicalDimensions.height = floor(properties->physicalResolution.height * MM_PER_PIXEL + 0.5);

   properties->supportedTransforms = VK_SURFACE_TRANSFORM_IDENTITY_BIT_KHR;
   properties->persistentContent = 0;
}

/*
 * Implement vkGetPhysicalDeviceDisplayPropertiesKHR (VK_KHR_display)
 */
VkResult
wsi_display_get_physical_device_display_properties(VkPhysicalDevice             physical_device,
                                                   struct wsi_device            *wsi_device,
                                                   uint32_t                     *property_count,
                                                   VkDisplayPropertiesKHR       *properties)
{
   struct wsi_display           *wsi = (struct wsi_display *) wsi_device->wsi[VK_ICD_WSI_PLATFORM_DISPLAY];
   drmModeResPtr                mode_res;

   if (wsi->fd < 0)
      goto bail;

   mode_res = drmModeGetResources(wsi->fd);

   if (!mode_res)
      goto bail;

   VK_OUTARRAY_MAKE(conn, properties, property_count);

   /* Get current information */

   for (int c = 0; c < mode_res->count_connectors; c++) {
      struct wsi_display_connector *connector =
         wsi_display_get_connector(wsi_device, mode_res->connectors[c]);

      if (!connector) {
         drmModeFreeResources(mode_res);
         return VK_ERROR_OUT_OF_HOST_MEMORY;
      }

      if (connector->connected) {
         vk_outarray_append(&conn, prop) {
            wsi_display_fill_in_display_properties(wsi_device,
                                                   connector,
                                                   prop);
         }
      }
   }

   drmModeFreeResources(mode_res);

   return vk_outarray_status(&conn);

bail:
   *property_count = 0;
   return VK_SUCCESS;
}

/*
 * Implement vkGetPhysicalDeviceDisplayPlanePropertiesKHR (VK_KHR_display
 */
VkResult
wsi_display_get_physical_device_display_plane_properties(VkPhysicalDevice               physical_device,
                                                         struct wsi_device              *wsi_device,
                                                         uint32_t                       *property_count,
                                                         VkDisplayPlanePropertiesKHR    *properties)
{
   struct wsi_display           *wsi = (struct wsi_display *) wsi_device->wsi[VK_ICD_WSI_PLATFORM_DISPLAY];
   struct wsi_display_connector *connector;

   VK_OUTARRAY_MAKE(conn, properties, property_count);

   LIST_FOR_EACH_ENTRY(connector, &wsi->connectors, list) {
      vk_outarray_append(&conn, prop) {
         if (connector && connector->active) {
            prop->currentDisplay = wsi_display_connector_to_handle(connector);
            prop->currentStackIndex = 0;
         } else {
            prop->currentDisplay = NULL;
            prop->currentStackIndex = 0;
         }
      }
   }
   return vk_outarray_status(&conn);
}

/*
 * Implement vkGetDisplayPlaneSupportedDisplaysKHR (VK_KHR_display)
 */

VkResult
wsi_display_get_display_plane_supported_displays(VkPhysicalDevice               physical_device,
                                                 struct wsi_device              *wsi_device,
                                                 uint32_t                       plane_index,
                                                 uint32_t                       *display_count,
                                                 VkDisplayKHR                   *displays)
{
   struct wsi_display           *wsi = (struct wsi_display *) wsi_device->wsi[VK_ICD_WSI_PLATFORM_DISPLAY];
   struct wsi_display_connector *connector;

   VK_OUTARRAY_MAKE(conn, displays, display_count);

   int c = 0;

   LIST_FOR_EACH_ENTRY(connector, &wsi->connectors, list) {
      if (c == plane_index) {
         vk_outarray_append(&conn, display) {
            *display = wsi_display_connector_to_handle(connector);
         }
      }
      c++;
   }
   return vk_outarray_status(&conn);
}

/*
 * Implement vkGetDisplayModePropertiesKHR (VK_KHR_display)
 */

VkResult
wsi_display_get_display_mode_properties(VkPhysicalDevice               physical_device,
                                        struct wsi_device              *wsi_device,
                                        VkDisplayKHR                   display,
                                        uint32_t                       *property_count,
                                        VkDisplayModePropertiesKHR     *properties)
{
   struct wsi_display_connector *connector = wsi_display_connector_from_handle(display);
   struct wsi_display_mode      *display_mode;

   VK_OUTARRAY_MAKE(conn, properties, property_count);

   LIST_FOR_EACH_ENTRY(display_mode, &connector->display_modes, list) {
      if (display_mode->valid) {
         vk_outarray_append(&conn, prop) {
            prop->displayMode = wsi_display_mode_to_handle(display_mode);
            prop->parameters.visibleRegion.width = display_mode->hdisplay;
            prop->parameters.visibleRegion.height = display_mode->vdisplay;
            prop->parameters.refreshRate = (uint32_t) (wsi_display_mode_refresh(display_mode) * 1000 + 0.5);
         }
      }
   }
   return vk_outarray_status(&conn);
}

/*
 * Implement vkGetDisplayPlaneCapabilities
 */
VkResult
wsi_get_display_plane_capabilities(VkPhysicalDevice                     physical_device,
                                   struct wsi_device                    *wsi_device,
                                   VkDisplayModeKHR                     mode_khr,
                                   uint32_t                             plane_index,
                                   VkDisplayPlaneCapabilitiesKHR        *capabilities)
{
   struct wsi_display_mode      *mode = wsi_display_mode_from_handle(mode_khr);

   /* XXX use actual values */
   capabilities->supportedAlpha = VK_DISPLAY_PLANE_ALPHA_OPAQUE_BIT_KHR;
   capabilities->minSrcPosition.x = 0;
   capabilities->minSrcPosition.y = 0;
   capabilities->maxSrcPosition.x = 0;
   capabilities->maxSrcPosition.y = 0;
   capabilities->minSrcExtent.width = mode->hdisplay;
   capabilities->minSrcExtent.height = mode->vdisplay;
   capabilities->maxSrcExtent.width = mode->hdisplay;
   capabilities->maxSrcExtent.height = mode->vdisplay;
   capabilities->minDstPosition.x = 0;
   capabilities->minDstPosition.y = 0;
   capabilities->maxDstPosition.x = 0;
   capabilities->maxDstPosition.y = 0;
   capabilities->minDstExtent.width = mode->hdisplay;
   capabilities->minDstExtent.height = mode->vdisplay;
   capabilities->maxDstExtent.width = mode->hdisplay;
   capabilities->maxDstExtent.height = mode->vdisplay;
   return VK_SUCCESS;
}

VkResult
wsi_create_display_surface(VkInstance instance,
                           const VkAllocationCallbacks   *allocator,
                           const VkDisplaySurfaceCreateInfoKHR *create_info,
                           VkSurfaceKHR *surface_khr)
{
   VkIcdSurfaceDisplay *surface;

   surface = vk_zalloc(allocator, sizeof *surface, 8,
                       VK_SYSTEM_ALLOCATION_SCOPE_OBJECT);
   if (surface == NULL)
      return VK_ERROR_OUT_OF_HOST_MEMORY;

   surface->base.platform = VK_ICD_WSI_PLATFORM_DISPLAY;

   surface->displayMode = create_info->displayMode;
   surface->planeIndex = create_info->planeIndex;
   surface->planeStackIndex = create_info->planeStackIndex;
   surface->transform = create_info->transform;
   surface->globalAlpha = create_info->globalAlpha;
   surface->alphaMode = create_info->alphaMode;
   surface->imageExtent = create_info->imageExtent;

   *surface_khr = VkIcdSurfaceBase_to_handle(&surface->base);
   return VK_SUCCESS;
}


static VkResult
wsi_display_surface_get_support(VkIcdSurfaceBase *surface,
                                struct wsi_device *wsi_device,
                                const VkAllocationCallbacks *allocator,
                                uint32_t queueFamilyIndex,
                                int local_fd,
                                VkBool32* pSupported)
{
   *pSupported = VK_TRUE;
   return VK_SUCCESS;
}

static VkResult
wsi_display_surface_get_capabilities(VkIcdSurfaceBase *surface_base,
                                     VkSurfaceCapabilitiesKHR* caps)
{
   VkIcdSurfaceDisplay *surface = (VkIcdSurfaceDisplay *) surface_base;
   wsi_display_mode *mode = wsi_display_mode_from_handle(surface->displayMode);

   caps->currentExtent.width = mode->hdisplay;
   caps->currentExtent.height = mode->vdisplay;

   /* XXX Figure out extents based on driver capabilities */
   caps->maxImageExtent = caps->minImageExtent = caps->currentExtent;

   caps->supportedCompositeAlpha = VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR;

   caps->minImageCount = 2;
   caps->maxImageCount = 0;

   caps->supportedTransforms = VK_SURFACE_TRANSFORM_IDENTITY_BIT_KHR;
   caps->currentTransform = VK_SURFACE_TRANSFORM_IDENTITY_BIT_KHR;
   caps->maxImageArrayLayers = 1;
   caps->supportedUsageFlags =
      VK_IMAGE_USAGE_TRANSFER_SRC_BIT |
      VK_IMAGE_USAGE_SAMPLED_BIT |
      VK_IMAGE_USAGE_TRANSFER_DST_BIT |
      VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT;

   return VK_SUCCESS;
}

static VkResult
wsi_display_surface_get_capabilities2(VkIcdSurfaceBase *icd_surface,
                                      const void *info_next,
                                      VkSurfaceCapabilities2KHR *caps)
{
   assert(caps->sType == VK_STRUCTURE_TYPE_SURFACE_CAPABILITIES_2_KHR);

   return wsi_display_surface_get_capabilities(icd_surface, &caps->surfaceCapabilities);
}

static VkResult
wsi_display_surface_get_capabilities2ext(VkIcdSurfaceBase *icd_surface,
                                          VkSurfaceCapabilities2EXT *caps)
{
   VkSurfaceCapabilitiesKHR     khr_caps;
   VkResult                     ret;

   assert(caps->sType == VK_STRUCTURE_TYPE_SURFACE_CAPABILITIES_2_EXT);
   ret = wsi_display_surface_get_capabilities(icd_surface, &khr_caps);
   if (ret)
      return ret;

   caps->minImageCount = khr_caps.minImageCount;
   caps->maxImageCount = khr_caps.maxImageCount;
   caps->currentExtent = khr_caps.currentExtent;
   caps->minImageExtent = khr_caps.minImageExtent;
   caps->maxImageExtent = khr_caps.maxImageExtent;
   caps->maxImageArrayLayers = khr_caps.maxImageArrayLayers;
   caps->supportedTransforms = khr_caps.supportedTransforms;
   caps->currentTransform = khr_caps.currentTransform;
   caps->supportedCompositeAlpha = khr_caps.supportedCompositeAlpha;
   caps->supportedUsageFlags = khr_caps.supportedUsageFlags;
   caps->supportedSurfaceCounters = 0;
   return ret;
}

static const struct {
   VkFormat     format;
   uint32_t     drm_format;
} available_surface_formats[] = {
   { .format = VK_FORMAT_B8G8R8A8_SRGB, .drm_format = DRM_FORMAT_XRGB8888 },
   { .format = VK_FORMAT_B8G8R8A8_UNORM, .drm_format = DRM_FORMAT_XRGB8888 },
};

static VkResult
wsi_display_surface_get_formats(VkIcdSurfaceBase        *icd_surface,
                                struct wsi_device       *wsi_device,
                                uint32_t                *surface_format_count,
                                VkSurfaceFormatKHR      *surface_formats)
{
   VK_OUTARRAY_MAKE(out, surface_formats, surface_format_count);

   for (unsigned i = 0; i < ARRAY_SIZE(available_surface_formats); i++) {
      vk_outarray_append(&out, f) {
         f->format = available_surface_formats[i].format;
         f->colorSpace = VK_COLORSPACE_SRGB_NONLINEAR_KHR;
      }
   }

   return vk_outarray_status(&out);
}

static VkResult
wsi_display_surface_get_formats2(VkIcdSurfaceBase *surface,
                                 struct wsi_device *wsi_device,
                                 const void *info_next,
                                 uint32_t *surface_format_count,
                                 VkSurfaceFormat2KHR *surface_formats)
{
   VK_OUTARRAY_MAKE(out, surface_formats, surface_format_count);

   for (unsigned i = 0; i < ARRAY_SIZE(available_surface_formats); i++) {
      vk_outarray_append(&out, f) {
         assert(f->sType == VK_STRUCTURE_TYPE_SURFACE_FORMAT_2_KHR);
         f->surfaceFormat.format = available_surface_formats[i].format;
         f->surfaceFormat.colorSpace = VK_COLORSPACE_SRGB_NONLINEAR_KHR;
      }
   }

   return vk_outarray_status(&out);
}

static const VkPresentModeKHR available_present_modes[] = {
   VK_PRESENT_MODE_FIFO_KHR,
};

static VkResult
wsi_display_surface_get_present_modes(VkIcdSurfaceBase  *surface,
                                      uint32_t          *present_mode_count,
                                      VkPresentModeKHR  *present_modes)
{
   VK_OUTARRAY_MAKE(conn, present_modes, present_mode_count);

   for (unsigned int c = 0; c < ARRAY_SIZE(available_present_modes); c++) {
      vk_outarray_append(&conn, present) {
         *present = available_present_modes[c];
      }
   }

   return vk_outarray_status(&conn);
}

static void
wsi_display_destroy_buffer(struct wsi_display *wsi,
                           uint32_t buffer)
{
   (void) drmIoctl(wsi->fd, DRM_IOCTL_MODE_DESTROY_DUMB,
                   &((struct drm_mode_destroy_dumb) { .handle = buffer }));
}

static VkResult
wsi_display_image_init(VkDevice                         device_h,
                       struct wsi_swapchain             *drv_chain,
                       const VkSwapchainCreateInfoKHR   *create_info,
                       const VkAllocationCallbacks      *allocator,
                       struct wsi_display_image         *image)
{
   struct wsi_display_swapchain *chain = (struct wsi_display_swapchain *) drv_chain;
   struct wsi_display           *wsi = chain->wsi;
   VkResult                     result;
   int                          ret;
   uint32_t                     drm_format = 0;

   for (unsigned i = 0; i < ARRAY_SIZE(available_surface_formats); i++) {
      if (create_info->imageFormat == available_surface_formats[i].format) {
         drm_format = available_surface_formats[i].drm_format;
         break;
      }
   }

   /* the application provided an invalid format, bail */
   if (drm_format == 0)
      return VK_ERROR_DEVICE_LOST;

   result = wsi_create_native_image(&chain->base, create_info,
                                    0, NULL, NULL,
                                    &image->base);
   if (result != VK_SUCCESS)
      return result;

   memset(image->buffer, 0, sizeof (image->buffer));

   for (unsigned int i = 0; i < image->base.num_planes; i++) {
      ret = drmPrimeFDToHandle(wsi->fd, image->base.fds[i], &image->buffer[i]);

      close(image->base.fds[i]);
      image->base.fds[i] = -1;
      if (ret < 0)
         goto fail_handle;
   }

   image->chain = chain;
   image->state = WSI_IMAGE_IDLE;
   image->fb_id = 0;

   ret = drmModeAddFB2(wsi->fd,
                       create_info->imageExtent.width,
                       create_info->imageExtent.height,
                       drm_format,
                       image->buffer,
                       image->base.row_pitches,
                       image->base.offsets,
                       &image->fb_id, 0);

   if (ret)
      goto fail_fb;

   return VK_SUCCESS;

fail_fb:
fail_handle:
   for (unsigned int i = 0; i < image->base.num_planes; i++) {
      if (image->buffer[i])
         wsi_display_destroy_buffer(wsi, image->buffer[i]);
      if (image->base.fds[i] != -1) {
         close(image->base.fds[i]);
         image->base.fds[i] = -1;
      }
   }

   wsi_destroy_image(&chain->base, &image->base);

   return VK_ERROR_OUT_OF_HOST_MEMORY;
}

static void
wsi_display_image_finish(struct wsi_swapchain           *drv_chain,
                         const VkAllocationCallbacks    *allocator,
                         struct wsi_display_image       *image)
{
   struct wsi_display_swapchain *chain = (struct wsi_display_swapchain *) drv_chain;
   struct wsi_display           *wsi = chain->wsi;

   drmModeRmFB(wsi->fd, image->fb_id);
   for (unsigned int i = 0; i < image->base.num_planes; i++)
      wsi_display_destroy_buffer(wsi, image->buffer[i]);
   wsi_destroy_image(&chain->base, &image->base);
}

static VkResult
wsi_display_swapchain_destroy(struct wsi_swapchain              *drv_chain,
                              const VkAllocationCallbacks       *allocator)
{
   struct wsi_display_swapchain *chain = (struct wsi_display_swapchain *) drv_chain;

   for (uint32_t i = 0; i < chain->base.image_count; i++)
      wsi_display_image_finish(drv_chain, allocator, &chain->images[i]);
   vk_free(allocator, chain);
   return VK_SUCCESS;
}

static struct wsi_image *
wsi_display_get_wsi_image(struct wsi_swapchain  *drv_chain,
                          uint32_t              image_index)
{
   struct wsi_display_swapchain *chain = (struct wsi_display_swapchain *) drv_chain;

   return &chain->images[image_index].base;
}

static void
wsi_display_idle_old_displaying(struct wsi_display_image *active_image)
{
   struct wsi_display_swapchain *chain = active_image->chain;

   wsi_display_debug("idle everyone but %ld\n", active_image - &(chain->images[0]));
   for (uint32_t i = 0; i < chain->base.image_count; i++)
      if (chain->images[i].state == WSI_IMAGE_DISPLAYING && &chain->images[i] != active_image) {
         wsi_display_debug("idle %d\n", i);
         chain->images[i].state = WSI_IMAGE_IDLE;
      }
}

static VkResult
_wsi_display_queue_next(struct wsi_swapchain     *drv_chain);

static void
wsi_display_page_flip_handler2(int              fd,
                               unsigned int     frame,
                               unsigned int     sec,
                               unsigned int     usec,
                               uint32_t         crtc_id,
                               void             *data)
{
   struct wsi_display_image     *image = data;
   struct wsi_display_swapchain *chain = image->chain;
   VkResult                     status;

   wsi_display_debug("image %ld displayed at %d\n", image - &(image->chain->images[0]), frame);
   image->state = WSI_IMAGE_DISPLAYING;
   wsi_display_idle_old_displaying(image);
   status = _wsi_display_queue_next(&(chain->base));
   if (status != VK_SUCCESS)
      chain->status = status;
}

static void wsi_display_page_flip_handler(int fd, unsigned int frame,
                                          unsigned int sec, unsigned int usec, void *data)
{
   wsi_display_page_flip_handler2(fd, frame, sec, usec, 0, data);
}

static drmEventContext event_context = {
   .version = DRM_EVENT_CONTEXT_VERSION,
   .page_flip_handler = wsi_display_page_flip_handler,
#if DRM_EVENT_CONTEXT_VERSION >= 3
   .page_flip_handler2 = wsi_display_page_flip_handler2,
#endif
};

static void *
wsi_display_wait_thread(void *data)
{
   struct wsi_display   *wsi = data;
   struct pollfd pollfd = {
      .fd = wsi->fd,
      .events = POLLIN
   };
   int ret;

   pthread_setcanceltype(PTHREAD_CANCEL_ASYNCHRONOUS, NULL);
   for (;;) {
      ret = poll(&pollfd, 1, -1);
      if (ret > 0) {
         pthread_mutex_lock(&wsi->wait_mutex);
         (void) drmHandleEvent(wsi->fd, &event_context);
         pthread_mutex_unlock(&wsi->wait_mutex);
         pthread_cond_broadcast(&wsi->wait_cond);
      }
   }
   return NULL;
}

static int
wsi_display_start_wait_thread(struct wsi_display        *wsi)
{
   if (!wsi->wait_thread) {
      int ret = pthread_create(&wsi->wait_thread, NULL, wsi_display_wait_thread, wsi);
      if (ret)
         return ret;
   }
   return 0;
}

/*
 * Wait for at least one event from the kernel to be processed.
 * Call with wait_mutex held
 */
static int
wsi_display_wait_for_event(struct wsi_display           *wsi,
                           uint64_t                     timeout_ns)
{
   int ret;

   ret = wsi_display_start_wait_thread(wsi);

   if (ret)
      return ret;

   struct timespec abs_timeout = {
      .tv_sec = timeout_ns / ((uint64_t) 1000 * (uint64_t) 1000 * (uint64_t) 1000),
      .tv_nsec = timeout_ns % ((uint64_t) 1000 * (uint64_t) 1000 * (uint64_t) 1000)
   };

   ret = pthread_cond_timedwait(&wsi->wait_cond, &wsi->wait_mutex, &abs_timeout);

   wsi_display_debug("%9ld done waiting for event %d\n", pthread_self(), ret);
   return ret;
}

static VkResult
wsi_display_acquire_next_image(struct wsi_swapchain     *drv_chain,
                               uint64_t                 timeout,
                               VkSemaphore              semaphore,
                               uint32_t                 *image_index)
{
   struct wsi_display_swapchain *chain = (struct wsi_display_swapchain *)drv_chain;
   struct wsi_display           *wsi = chain->wsi;
   int                          ret = 0;
   VkResult                     result = VK_SUCCESS;

   /* Bail early if the swapchain is broken */
   if (chain->status != VK_SUCCESS)
      return chain->status;

   if (timeout != 0 && timeout != UINT64_MAX)
      timeout += wsi_get_current_monotonic();

   pthread_mutex_lock(&wsi->wait_mutex);
   for (;;) {
      for (uint32_t i = 0; i < chain->base.image_count; i++) {
         if (chain->images[i].state == WSI_IMAGE_IDLE) {
            *image_index = i;
            wsi_display_debug("image %d available\n", i);
            chain->images[i].state = WSI_IMAGE_DRAWING;
            result = VK_SUCCESS;
            goto done;
         }
         wsi_display_debug("image %d state %d\n", i, chain->images[i].state);
      }

      if (ret == ETIMEDOUT) {
         result = VK_TIMEOUT;
         goto done;
      }

      ret = wsi_display_wait_for_event(wsi, timeout);

      if (ret && ret != ETIMEDOUT) {
         result = VK_ERROR_OUT_OF_DATE_KHR;
         goto done;
      }
   }
done:
   pthread_mutex_unlock(&wsi->wait_mutex);

   if (result != VK_SUCCESS)
      return result;

   return chain->status;
}

/*
 * Check whether there are any other connectors driven by this crtc
 */
static bool
wsi_display_crtc_solo(struct wsi_display        *wsi,
                      drmModeResPtr             mode_res,
                      drmModeConnectorPtr       connector,
                      uint32_t                  crtc_id)
{
   int                  c, e;

   /* See if any other connectors share the same encoder */
   for (c = 0; c < mode_res->count_connectors; c++) {
      if (mode_res->connectors[c] == connector->connector_id)
         continue;

      drmModeConnectorPtr other_connector = drmModeGetConnector(wsi->fd, mode_res->connectors[c]);
      if (other_connector) {
         bool match = (other_connector->encoder_id == connector->encoder_id);
         drmModeFreeConnector(other_connector);
         if (match)
            return false;
      }
   }

   /* See if any other encoders share the same crtc */
   for (e = 0; e < mode_res->count_encoders; e++) {
      if (mode_res->encoders[e] == connector->encoder_id)
         continue;

      drmModeEncoderPtr other_encoder = drmModeGetEncoder(wsi->fd, mode_res->encoders[e]);
      if (other_encoder) {
         bool match = (other_encoder->crtc_id == crtc_id);
         drmModeFreeEncoder(other_encoder);
         if (match)
            return false;
      }
   }
   return true;
}

/*
 * Pick a suitable CRTC to drive this connector. Prefer a CRTC which is
 * currently driving this connector and not any others. Settle for a CRTC
 * which is currently idle.
 */
static uint32_t
wsi_display_select_crtc(struct wsi_display_connector    *connector,
                        drmModeResPtr                   mode_res,
                        drmModeConnectorPtr             drm_connector)
{
   struct wsi_display   *wsi = connector->wsi;
   int                  c;
   uint32_t             crtc_id;

   /* See what CRTC is currently driving this connector */
   if (drm_connector->encoder_id) {
      drmModeEncoderPtr encoder = drmModeGetEncoder(wsi->fd, drm_connector->encoder_id);
      if (encoder) {
         crtc_id = encoder->crtc_id;
         drmModeFreeEncoder(encoder);
         if (crtc_id) {
            if (wsi_display_crtc_solo(wsi, mode_res, drm_connector, crtc_id))
               return crtc_id;
         }
      }
   }
   crtc_id = 0;
   for (c = 0; crtc_id == 0 && c < mode_res->count_crtcs; c++) {
      drmModeCrtcPtr crtc = drmModeGetCrtc(wsi->fd, mode_res->crtcs[c]);
      if (crtc && crtc->buffer_id == 0)
         crtc_id = crtc->crtc_id;
      drmModeFreeCrtc(crtc);
   }
   return crtc_id;
}

static VkResult
wsi_display_setup_connector(wsi_display_connector       *connector,
                            wsi_display_mode            *display_mode)
{
   struct wsi_display   *wsi = connector->wsi;
   drmModeModeInfoPtr   drm_mode;
   drmModeConnectorPtr  drm_connector;
   drmModeResPtr        mode_res;
   VkResult             result;
   int                  m;

   if (connector->current_mode == display_mode && connector->crtc_id)
      return VK_SUCCESS;

   mode_res = drmModeGetResources(wsi->fd);
   if (!mode_res) {
      if (errno == ENOMEM)
         result = VK_ERROR_OUT_OF_HOST_MEMORY;
      else
         result = VK_ERROR_OUT_OF_DATE_KHR;
      goto bail;
   }

   drm_connector = drmModeGetConnectorCurrent(wsi->fd, connector->id);
   if (!drm_connector) {
      if (errno == ENOMEM)
         result = VK_ERROR_OUT_OF_HOST_MEMORY;
      else
         result = VK_ERROR_OUT_OF_DATE_KHR;
      goto bail_mode_res;
   }

   /* Pick a CRTC if we don't have one */
   if (!connector->crtc_id) {
      connector->crtc_id = wsi_display_select_crtc(connector, mode_res, drm_connector);
      if (!connector->crtc_id) {
         result = VK_ERROR_OUT_OF_DATE_KHR;
         goto bail_connector;
      }
   }

   if (connector->current_mode != display_mode) {

      /* Find the drm mode cooresponding to the requested VkDisplayMode */
      drm_mode = NULL;
      for (m = 0; m < drm_connector->count_modes; m++) {
         drm_mode = &drm_connector->modes[m];
         if (wsi_display_mode_matches_drm(display_mode, drm_mode))
            break;
         drm_mode = NULL;
      }

      if (!drm_mode) {
         result = VK_ERROR_OUT_OF_DATE_KHR;
         goto bail_connector;
      }

      connector->current_mode = display_mode;
      connector->current_drm_mode = *drm_mode;
   }

   result = VK_SUCCESS;

bail_connector:
   drmModeFreeConnector(drm_connector);
bail_mode_res:
   drmModeFreeResources(mode_res);
bail:
   return result;

}

/*
 * Check to see if the kernel has no flip queued and if there's an image
 * waiting to be displayed.
 */
static VkResult
_wsi_display_queue_next(struct wsi_swapchain     *drv_chain)
{
   struct wsi_display_swapchain *chain = (struct wsi_display_swapchain *) drv_chain;
   struct wsi_display           *wsi = chain->wsi;
   uint32_t                     i;
   struct wsi_display_image     *image = NULL;
   VkIcdSurfaceDisplay          *surface = chain->surface;
   wsi_display_mode             *display_mode = wsi_display_mode_from_handle(surface->displayMode);
   wsi_display_connector        *connector = display_mode->connector;
   int                          ret;
   VkResult                     result;

   if (wsi->fd < 0)
      return VK_ERROR_INITIALIZATION_FAILED;

   if (display_mode != connector->current_mode)
      connector->active = false;

   for (;;) {
      /* Check to see if there is an image to display, or if some image is already queued */

      for (i = 0; i < chain->base.image_count; i++) {
         struct wsi_display_image  *tmp_image = &chain->images[i];

         switch (tmp_image->state) {
         case WSI_IMAGE_FLIPPING:
            /* already flipping, don't send another to the kernel yet */
            return VK_SUCCESS;
         case WSI_IMAGE_QUEUED:
            /* find the oldest queued */
            if (!image || tmp_image->flip_sequence < image->flip_sequence)
               image = tmp_image;
            break;
         default:
            break;
         }
      }

      if (!image)
         return VK_SUCCESS;

      if (connector->active) {
         ret = drmModePageFlip(wsi->fd, connector->crtc_id, image->fb_id,
                               DRM_MODE_PAGE_FLIP_EVENT, image);
         if (ret == 0) {
            image->state = WSI_IMAGE_FLIPPING;
            return VK_SUCCESS;
         }
         wsi_display_debug("page flip err %d %s\n", ret, strerror(-ret));
      } else {
         ret = -EINVAL;
      }

      if (ret) {
         switch(-ret) {
         case EINVAL:

            result = wsi_display_setup_connector(connector, display_mode);

            if (result != VK_SUCCESS) {
               image->state = WSI_IMAGE_IDLE;
               return result;
            }

            /* XXX allow setting of position */

            ret = drmModeSetCrtc(wsi->fd, connector->crtc_id, image->fb_id, 0, 0,
                                 &connector->id, 1, &connector->current_drm_mode);

            if (ret == 0) {
               image->state = WSI_IMAGE_DISPLAYING;

               /* Assume that the mode set is synchronous and that any
                * previous image is now idle.
                */
               wsi_display_idle_old_displaying(image);
               connector->active = true;
               return VK_SUCCESS;
            }
            break;
         case EACCES:

            /* Some other VT is currently active. Sit here waiting for
             * our VT to become active again by polling once a second
             */
            usleep(1000 * 1000);
            connector->active = false;
            break;
         default:
            connector->active = false;
            image->state = WSI_IMAGE_IDLE;
            return VK_ERROR_OUT_OF_DATE_KHR;
         }
      }
   }
}

static VkResult
wsi_display_queue_present(struct wsi_swapchain          *drv_chain,
                          uint32_t                      image_index,
                          const VkPresentRegionKHR      *damage)
{
   struct wsi_display_swapchain *chain = (struct wsi_display_swapchain *) drv_chain;
   struct wsi_display           *wsi = chain->wsi;
   struct wsi_display_image     *image = &chain->images[image_index];
   VkResult                     result;

   /* Bail early if the swapchain is broken */
   if (chain->status != VK_SUCCESS)
      return chain->status;

   assert(image->state == WSI_IMAGE_DRAWING);
   wsi_display_debug("present %d\n", image_index);

   pthread_mutex_lock(&wsi->wait_mutex);

   image->flip_sequence = ++chain->flip_sequence;
   image->state = WSI_IMAGE_QUEUED;

   result = _wsi_display_queue_next(drv_chain);
   if (result != VK_SUCCESS)
      chain->status = result;

   pthread_mutex_unlock(&wsi->wait_mutex);

   if (result != VK_SUCCESS)
      return result;

   return chain->status;
}

static VkResult
wsi_display_surface_create_swapchain(VkIcdSurfaceBase                   *icd_surface,
                                     VkDevice                           device,
                                     struct wsi_device                  *wsi_device,
                                     int                                local_fd,
                                     const VkSwapchainCreateInfoKHR     *create_info,
                                     const VkAllocationCallbacks        *allocator,
                                     struct wsi_swapchain               **swapchain_out)
{
   struct wsi_display *wsi = (struct wsi_display *) wsi_device->wsi[VK_ICD_WSI_PLATFORM_DISPLAY];
   VkResult result;

   assert(create_info->sType == VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR);

   struct wsi_display_swapchain *chain;
   const unsigned num_images = create_info->minImageCount;
   size_t size = sizeof(*chain) + num_images * sizeof(chain->images[0]);

   chain = vk_zalloc(allocator, size, 8,
                    VK_SYSTEM_ALLOCATION_SCOPE_OBJECT);

   if (chain == NULL)
      return VK_ERROR_OUT_OF_HOST_MEMORY;

   result = wsi_swapchain_init(wsi_device, &chain->base, device,
                               create_info, allocator);

   chain->base.destroy = wsi_display_swapchain_destroy;
   chain->base.get_wsi_image = wsi_display_get_wsi_image;
   chain->base.acquire_next_image = wsi_display_acquire_next_image;
   chain->base.queue_present = wsi_display_queue_present;
   chain->base.present_mode = create_info->presentMode;
   chain->base.image_count = num_images;

   chain->wsi = wsi;
   chain->status = VK_SUCCESS;

   chain->surface = (VkIcdSurfaceDisplay *) icd_surface;

   for (uint32_t image = 0; image < chain->base.image_count; image++) {
      result = wsi_display_image_init(device, &chain->base, create_info, allocator,
                                      &chain->images[image]);
      if (result != VK_SUCCESS) {
         while (image > 0) {
            --image;
            wsi_display_image_finish(&chain->base, allocator, &chain->images[image]);
         }
         vk_free(allocator, chain);
         goto fail_init_images;
      }
   }

   *swapchain_out = &chain->base;

   return VK_SUCCESS;

fail_init_images:
   return result;
}

VkResult
wsi_display_init_wsi(struct wsi_device *wsi_device,
                     const VkAllocationCallbacks *alloc,
                     int display_fd)
{
   struct wsi_display *wsi;
   VkResult result;

   wsi = vk_zalloc(alloc, sizeof(*wsi), 8,
                   VK_SYSTEM_ALLOCATION_SCOPE_INSTANCE);
   if (!wsi) {
      result = VK_ERROR_OUT_OF_HOST_MEMORY;
      goto fail;
   }

   wsi->fd = display_fd;

   pthread_mutex_init(&wsi->wait_mutex, NULL);
   wsi->alloc = alloc;

   LIST_INITHEAD(&wsi->connectors);

   pthread_condattr_t condattr;
   int ret;

   ret = pthread_mutex_init(&wsi->wait_mutex, NULL);
   if (ret) {
      result = VK_ERROR_OUT_OF_HOST_MEMORY;
      goto fail_mutex;
   }

   ret = pthread_condattr_init(&condattr);
   if (ret) {
      result = VK_ERROR_OUT_OF_HOST_MEMORY;
      goto fail_condattr;
   }

   ret = pthread_condattr_setclock(&condattr, CLOCK_MONOTONIC);
   if (ret) {
      result = VK_ERROR_OUT_OF_HOST_MEMORY;
      goto fail_setclock;
   }

   ret = pthread_cond_init(&wsi->wait_cond, &condattr);
   if (ret) {
      result = VK_ERROR_OUT_OF_HOST_MEMORY;
      goto fail_cond;
   }

   pthread_condattr_destroy(&condattr);

   wsi->base.get_support = wsi_display_surface_get_support;
   wsi->base.get_capabilities = wsi_display_surface_get_capabilities;
   wsi->base.get_capabilities2 = wsi_display_surface_get_capabilities2;
   wsi->base.get_capabilities2ext = wsi_display_surface_get_capabilities2ext;
   wsi->base.get_formats = wsi_display_surface_get_formats;
   wsi->base.get_formats2 = wsi_display_surface_get_formats2;
   wsi->base.get_present_modes = wsi_display_surface_get_present_modes;
   wsi->base.create_swapchain = wsi_display_surface_create_swapchain;

   wsi_device->wsi[VK_ICD_WSI_PLATFORM_DISPLAY] = &wsi->base;

   return VK_SUCCESS;

fail_cond:
fail_setclock:
   pthread_condattr_destroy(&condattr);
fail_condattr:
   pthread_mutex_destroy(&wsi->wait_mutex);
fail_mutex:
   vk_free(alloc, wsi);
fail:
   return result;
}

void
wsi_display_finish_wsi(struct wsi_device *wsi_device,
                       const VkAllocationCallbacks *alloc)
{
   struct wsi_display *wsi = (struct wsi_display *) wsi_device->wsi[VK_ICD_WSI_PLATFORM_DISPLAY];

   if (wsi) {

      struct wsi_display_connector *connector, *connector_storage;
      LIST_FOR_EACH_ENTRY_SAFE(connector, connector_storage, &wsi->connectors, list) {
         struct wsi_display_mode *mode, *mode_storage;
         LIST_FOR_EACH_ENTRY_SAFE(mode, mode_storage, &connector->display_modes, list) {
            vk_free(wsi->alloc, mode);
         }
         vk_free(wsi->alloc, connector);
      }

      pthread_mutex_lock(&wsi->wait_mutex);
      if (wsi->wait_thread) {
         pthread_cancel(wsi->wait_thread);
         pthread_join(wsi->wait_thread, NULL);
      }
      pthread_mutex_unlock(&wsi->wait_mutex);
      pthread_mutex_destroy(&wsi->wait_mutex);
      pthread_cond_destroy(&wsi->wait_cond);

      vk_free(alloc, wsi);
   }
}

/*
 * Implement vkReleaseDisplay
 */
VkResult
wsi_release_display(VkPhysicalDevice            physical_device,
                    struct wsi_device           *wsi_device,
                    VkDisplayKHR                display)
{
   struct wsi_display           *wsi = (struct wsi_display *) wsi_device->wsi[VK_ICD_WSI_PLATFORM_DISPLAY];

   if (wsi->fd >= 0) {
      close(wsi->fd);
      wsi->fd = -1;
   }
#ifdef VK_USE_PLATFORM_XLIB_XRANDR_EXT
   wsi_display_connector_from_handle(display)->output = None;
#endif

   return VK_SUCCESS;
}

#ifdef VK_USE_PLATFORM_XLIB_XRANDR_EXT

static struct wsi_display_connector *
wsi_display_find_output(struct wsi_device               *wsi_device,
                        RROutput                        output)
{
   struct wsi_display           *wsi = (struct wsi_display *) wsi_device->wsi[VK_ICD_WSI_PLATFORM_DISPLAY];
   struct wsi_display_connector *connector;

   connector = NULL;
   LIST_FOR_EACH_ENTRY(connector, &wsi->connectors, list) {
      if (connector->output == output)
         return connector;
   }

   return NULL;
}

/*
 * Given a RandR output, find the associated kernel connector_id by
 * looking at the CONNECTOR_ID property provided by the X server
 */

static uint32_t
wsi_display_output_to_connector_id(xcb_connection_t     *connection,
                                   xcb_atom_t           *connector_id_atom_p,
                                   RROutput             output)
{
   uint32_t                     connector_id = 0;
   xcb_atom_t                   connector_id_atom = *connector_id_atom_p;

   if (connector_id_atom == 0) {
   /* Go dig out the CONNECTOR_ID property */
      xcb_intern_atom_cookie_t     ia_c = xcb_intern_atom(connection,
                                                          true,
                                                          12,
                                                          "CONNECTOR_ID");
      xcb_intern_atom_reply_t      *ia_r = xcb_intern_atom_reply(connection,
                                                                 ia_c,
                                                                 NULL);
      if (ia_r) {
         *connector_id_atom_p = connector_id_atom = ia_r->atom;
         free(ia_r);
      }
   }

   /* If there's an CONNECTOR_ID atom in the server, then there may be a CONNECTOR_ID property. Otherwise,
    * there will not be and we don't even need to bother.
    */
   if (connector_id_atom) {

      xcb_randr_query_version_cookie_t          qv_c = xcb_randr_query_version(connection, 1, 6);
      xcb_randr_get_output_property_cookie_t    gop_c = xcb_randr_get_output_property(connection,
                                                                                      output,
                                                                                      connector_id_atom,
                                                                                      0,
                                                                                      0,
                                                                                      0xffffffffUL,
                                                                                      0,
                                                                                      0);
      xcb_randr_query_version_reply_t           *qv_r = xcb_randr_query_version_reply(connection, qv_c, NULL);
      free(qv_r);
      xcb_randr_get_output_property_reply_t     *gop_r = xcb_randr_get_output_property_reply(connection,
                                                                                             gop_c,
                                                                                             NULL);
      if (gop_r) {
         if (gop_r->num_items == 1 && gop_r->format == 32)
            memcpy(&connector_id, xcb_randr_get_output_property_data(gop_r), 4);
         free(gop_r);
      }
   }
   return connector_id;
}

static bool
wsi_display_check_randr_version(xcb_connection_t        *connection)
{
   xcb_randr_query_version_cookie_t     qv_c = xcb_randr_query_version(connection, 1, 6);
   xcb_randr_query_version_reply_t      *qv_r = xcb_randr_query_version_reply(connection, qv_c, NULL);
   bool                                 ret = false;

   if (!qv_r)
      return false;

   /* Check for version 1.6 or newer */
   ret = qv_r->major_version > 1 || (qv_r->major_version == 1 && qv_r->minor_version >= 6);

   free(qv_r);
   return ret;
}

/*
 * Given a kernel connector id, find the associated RandR output using the
 * CONNECTOR_ID property
 */

static xcb_randr_output_t
wsi_display_connector_id_to_output(xcb_connection_t     *connection,
                                   uint32_t             connector_id)
{
   if (!wsi_display_check_randr_version(connection))
      return 0;

   const xcb_setup_t                    *setup = xcb_get_setup(connection);

   xcb_atom_t                           connector_id_atom = 0;
   xcb_randr_output_t                   output = 0;

   /* Search all of the screens for the provided output */
   xcb_screen_iterator_t iter;
   for (iter = xcb_setup_roots_iterator(setup); output == 0 && iter.rem; xcb_screen_next(&iter)) {

      xcb_randr_get_screen_resources_cookie_t      gsr_c = xcb_randr_get_screen_resources(connection, iter.data->root);
      xcb_randr_get_screen_resources_reply_t       *gsr_r = xcb_randr_get_screen_resources_reply(connection, gsr_c, NULL);

      if (!gsr_r)
         return 0;

      xcb_randr_output_t        *ro = xcb_randr_get_screen_resources_outputs(gsr_r);
      int                       o;

      for (o = 0; o < gsr_r->num_outputs; o++) {
         if (wsi_display_output_to_connector_id(connection, &connector_id_atom, ro[o]) == connector_id) {
            output = ro[o];
            break;
         }
      }
      free(gsr_r);
   }
   return output;
}

/*
 * Given a RandR output, find out which screen it's associated with
 */
static xcb_window_t
wsi_display_output_to_root(xcb_connection_t   *connection,
                           xcb_randr_output_t output)
{
   if (!wsi_display_check_randr_version(connection))
      return 0;

   const xcb_setup_t                    *setup = xcb_get_setup(connection);
   xcb_window_t                         root = 0;

   /* Search all of the screens for the provided output */
   xcb_screen_iterator_t iter;
   for (iter = xcb_setup_roots_iterator(setup); root == 0 && iter.rem; xcb_screen_next(&iter)) {
      xcb_randr_get_screen_resources_cookie_t      gsr_c = xcb_randr_get_screen_resources(connection, iter.data->root);
      xcb_randr_get_screen_resources_reply_t       *gsr_r = xcb_randr_get_screen_resources_reply(connection, gsr_c, NULL);

      if (!gsr_r)
         return 0;

      xcb_randr_output_t        *ro = xcb_randr_get_screen_resources_outputs(gsr_r);
      int                       o;

      for (o = 0; o < gsr_r->num_outputs; o++) {
         if (ro[o] == output) {
            root = iter.data->root;
            break;
         }
      }
      free(gsr_r);
   }
   return root;
}

static bool
wsi_display_mode_matches_x(struct wsi_display_mode      *wsi,
                           xcb_randr_mode_info_t        *xcb)
{
   return wsi->clock == (xcb->dot_clock + 500) / 1000 &&
      wsi->hdisplay == xcb->width &&
      wsi->hsync_start == xcb->hsync_start &&
      wsi->hsync_end == xcb->hsync_end &&
      wsi->htotal == xcb->htotal &&
      wsi->hskew == xcb->hskew &&
      wsi->vdisplay == xcb->height &&
      wsi->vsync_start == xcb->vsync_start &&
      wsi->vsync_end == xcb->vsync_end &&
      wsi->vtotal == xcb->vtotal &&
      wsi->flags == xcb->mode_flags;
}

static struct wsi_display_mode *
wsi_display_find_x_mode(struct wsi_device                 *wsi_device,
                        struct wsi_display_connector      *connector,
                        xcb_randr_mode_info_t             *mode)
{
   struct wsi_display_mode      *display_mode;

   LIST_FOR_EACH_ENTRY(display_mode, &connector->display_modes, list) {
      if (wsi_display_mode_matches_x(display_mode, mode))
         return display_mode;
   }
   return NULL;
}

static VkResult
wsi_display_register_x_mode(struct wsi_device                   *wsi_device,
                            struct wsi_display_connector        *connector,
                            xcb_randr_mode_info_t               *x_mode,
                            bool                                preferred)
{
   struct wsi_display           *wsi = (struct wsi_display *) wsi_device->wsi[VK_ICD_WSI_PLATFORM_DISPLAY];
   struct wsi_display_mode      *display_mode;

   display_mode = wsi_display_find_x_mode(wsi_device, connector, x_mode);

   if (display_mode) {
      display_mode->valid = true;
      return VK_SUCCESS;
   }

   display_mode = vk_zalloc(wsi->alloc, sizeof (struct wsi_display_mode), 8, VK_SYSTEM_ALLOCATION_SCOPE_INSTANCE);
   if (!display_mode)
      return VK_ERROR_OUT_OF_HOST_MEMORY;

   display_mode->connector = connector;
   display_mode->valid = true;
   display_mode->preferred = preferred;
   display_mode->clock = (x_mode->dot_clock + 500) / 1000; /* kHz */
   display_mode->hdisplay = x_mode->width;
   display_mode->hsync_start = x_mode->hsync_start;
   display_mode->hsync_end = x_mode->hsync_end;
   display_mode->htotal = x_mode->htotal;
   display_mode->hskew = x_mode->hskew;
   display_mode->vdisplay = x_mode->height;
   display_mode->vsync_start = x_mode->vsync_start;
   display_mode->vsync_end = x_mode->vsync_end;
   display_mode->vtotal = x_mode->vtotal;
   display_mode->vscan = 0;
   if (x_mode->mode_flags & XCB_RANDR_MODE_FLAG_DOUBLE_SCAN)
      display_mode->vscan = 1;
   display_mode->flags = x_mode->mode_flags;

   LIST_ADDTAIL(&display_mode->list, &connector->display_modes);
   return VK_SUCCESS;
}

static struct wsi_display_connector *
wsi_display_get_output(struct wsi_device        *wsi_device,
                       xcb_connection_t         *connection,
                       RROutput                 output)
{
   struct wsi_display                           *wsi = (struct wsi_display *) wsi_device->wsi[VK_ICD_WSI_PLATFORM_DISPLAY];
   struct wsi_display_connector                 *connector;
   uint32_t                                     connector_id;
   xcb_window_t                                 root;
   xcb_randr_get_screen_resources_cookie_t      src;
   xcb_randr_get_screen_resources_reply_t       *srr;
   xcb_randr_get_output_info_cookie_t           oic;
   xcb_randr_get_output_info_reply_t            *oir;
   xcb_randr_mode_t                             *x_modes;
   int                                          m;

   root = wsi_display_output_to_root(connection, output);
   if (!root)
      return NULL;

   src = xcb_randr_get_screen_resources(connection, root);
   oic = xcb_randr_get_output_info(connection, output, XCB_CURRENT_TIME);
   srr = xcb_randr_get_screen_resources_reply(connection, src, NULL);
   oir = xcb_randr_get_output_info_reply(connection, oic, NULL);

   /* See if we already have a connector for this output */
   connector = wsi_display_find_output(wsi_device, output);

   if (!connector) {
      xcb_atom_t        connector_id_atom = 0;

      /*
       * Go get the kernel connector ID for this X output
       */
      connector_id = wsi_display_output_to_connector_id(connection, &connector_id_atom, output);

      /* Any X server with lease support will have this atom */
      if (!connector_id) {
         free(oir);
         free(srr);
         return NULL;
      }

      if (!connector) {
         /* See if we already have a connector for this id */
         connector = wsi_display_find_connector(wsi_device, connector_id);

         if (connector)
            connector->output = output;
      }
   }

   if (!connector) {
      connector = wsi_display_alloc_connector(wsi, connector_id);
      if (!connector) {
         free(oir);
         free(srr);
         return NULL;
      }
      LIST_ADDTAIL(&connector->list, &wsi->connectors);
      connector->output = output;
   }

   if (oir && srr) {
      /* Get X modes and add them */

      connector->connected = oir->connection != XCB_RANDR_CONNECTION_DISCONNECTED;

      wsi_display_invalidate_connector_modes(wsi_device, connector);

      x_modes = xcb_randr_get_output_info_modes(oir);
      for (m = 0; m < oir->num_modes; m++) {
         xcb_randr_mode_info_iterator_t i = xcb_randr_get_screen_resources_modes_iterator(srr);
         while (i.rem) {
            xcb_randr_mode_info_t *mi = i.data;
            if (mi->id == x_modes[m]) {
               VkResult result = wsi_display_register_x_mode(wsi_device, connector, mi, m < oir->num_preferred);
               if (result != VK_SUCCESS) {
                  free(oir);
                  free(srr);
                  return NULL;
               }
               break;
            }
            xcb_randr_mode_info_next(&i);
         }
      }
   }

   free(oir);
   free(srr);
   return connector;
}

static xcb_randr_crtc_t
wsi_display_find_crtc_for_output(xcb_connection_t       *connection,
                                 xcb_window_t           root,
                                 xcb_randr_output_t     output)
{
   xcb_randr_get_screen_resources_cookie_t      gsr_c = xcb_randr_get_screen_resources(connection, root);
   xcb_randr_get_screen_resources_reply_t       *gsr_r = xcb_randr_get_screen_resources_reply(connection, gsr_c, NULL);

   if (!gsr_r)
      return 0;

   xcb_randr_crtc_t     *rc = xcb_randr_get_screen_resources_crtcs(gsr_r);
   xcb_randr_crtc_t     idle_crtc = 0;
   xcb_randr_crtc_t     active_crtc = 0;

   /* Find either a crtc already connected to the desired output or idle */
   int c;
   for (c = 0; active_crtc == 0 && c < gsr_r->num_crtcs; c++) {
      xcb_randr_get_crtc_info_cookie_t gci_c = xcb_randr_get_crtc_info(connection, rc[c], gsr_r->config_timestamp);
      xcb_randr_get_crtc_info_reply_t *gci_r = xcb_randr_get_crtc_info_reply(connection, gci_c, NULL);
      if (gci_r) {
         if (gci_r->mode) {
            int num_outputs = xcb_randr_get_crtc_info_outputs_length(gci_r);
            xcb_randr_output_t *outputs = xcb_randr_get_crtc_info_outputs(gci_r);
            for (int o = 0; o < num_outputs; o++)
               if (outputs[o] == output && num_outputs == 1) {
                  active_crtc = rc[c];
                  break;
               }
         } else if (idle_crtc == 0) {
            int num_possible = xcb_randr_get_crtc_info_possible_length(gci_r);
            xcb_randr_output_t *possible = xcb_randr_get_crtc_info_possible(gci_r);
            for (int p = 0; p < num_possible; p++)
               if (possible[p] == output) {
                  idle_crtc = rc[c];
                  break;
               }
         }
         free(gci_r);
      }
   }
   free(gsr_r);

   if (active_crtc)
      return active_crtc;
   return idle_crtc;
}

VkResult
wsi_acquire_xlib_display(VkPhysicalDevice       physical_device,
                         struct wsi_device      *wsi_device,
                         Display                *dpy,
                         VkDisplayKHR           display)
{
   struct wsi_display           *wsi = (struct wsi_display *) wsi_device->wsi[VK_ICD_WSI_PLATFORM_DISPLAY];
   xcb_connection_t             *connection = XGetXCBConnection(dpy);
   struct wsi_display_connector *connector = wsi_display_connector_from_handle(display);
   xcb_window_t                 root;

   if (!connector->output) {
      connector->output = wsi_display_connector_id_to_output(connection, connector->id);

      /* Check and see if we found the output */
      if (!connector->output)
         return VK_ERROR_OUT_OF_DATE_KHR;
   }

   root = wsi_display_output_to_root(connection, connector->output);
   if (!root)
      return VK_ERROR_OUT_OF_DATE_KHR;

   xcb_randr_crtc_t                     crtc = wsi_display_find_crtc_for_output(connection,
                                                                                root,
                                                                                connector->output);

   if (!crtc)
      return VK_ERROR_OUT_OF_DATE_KHR;

   xcb_randr_lease_t                    lease = xcb_generate_id(connection);
   xcb_randr_create_lease_cookie_t      cl_c = xcb_randr_create_lease(connection,
                                                                      root,
                                                                      lease,
                                                                      1,
                                                                      1,
                                                                      &crtc,
                                                                      &connector->output);
   xcb_randr_create_lease_reply_t       *cl_r = xcb_randr_create_lease_reply(connection, cl_c, NULL);
   if (!cl_r)
      return VK_ERROR_OUT_OF_DATE_KHR;

   int fd = -1;
   if (cl_r->nfd > 0) {
      int *rcl_f = xcb_randr_create_lease_reply_fds(connection, cl_r);

      fd = rcl_f[0];
   }
   free (cl_r);
   if (fd < 0)
      return VK_ERROR_OUT_OF_DATE_KHR;

   wsi->fd = fd;

   return VK_SUCCESS;
}

VkResult
wsi_get_randr_output_display(VkPhysicalDevice   physical_device,
                             struct wsi_device  *wsi_device,
                             Display            *dpy,
                             RROutput           output,
                             VkDisplayKHR       *display)
{
   xcb_connection_t             *connection = XGetXCBConnection(dpy);
   struct wsi_display_connector *connector = wsi_display_get_output(wsi_device, connection, output);

   if (connector)
      *display = wsi_display_connector_to_handle(connector);
   else
      *display = NULL;
   return VK_SUCCESS;
}

#endif
