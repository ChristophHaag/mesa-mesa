/*
 * Copyright © 2017 Intel Corporation
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
 * FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS
 * IN THE SOFTWARE.
 */

#include "wsi_common_private.h"
#include "drm_fourcc.h"
#include "util/macros.h"
#include "vk_util.h"

#include <unistd.h>

VkResult
wsi_device_init(struct wsi_device *wsi,
                VkPhysicalDevice pdevice,
                WSI_FN_GetPhysicalDeviceProcAddr proc_addr,
                const VkAllocationCallbacks *alloc,
                int display_fd)
{
   VkResult result;

   memset(wsi, 0, sizeof(*wsi));

   wsi->pdevice = pdevice;

#define WSI_GET_CB(func) \
   PFN_vk##func func = (PFN_vk##func)proc_addr(pdevice, "vk" #func)
   WSI_GET_CB(GetPhysicalDeviceMemoryProperties);
   WSI_GET_CB(GetPhysicalDeviceQueueFamilyProperties);
   WSI_GET_CB(GetPhysicalDeviceProperties);
#undef WSI_GET_CB

   GetPhysicalDeviceMemoryProperties(pdevice, &wsi->memory_props);
   GetPhysicalDeviceQueueFamilyProperties(pdevice, &wsi->queue_family_count, NULL);

   VkPhysicalDeviceProperties properties;
   GetPhysicalDeviceProperties(pdevice, &properties);
   wsi->timestamp_period = properties.limits.timestampPeriod;

#define WSI_GET_CB(func) \
   wsi->func = (PFN_vk##func)proc_addr(pdevice, "vk" #func)
   WSI_GET_CB(AllocateMemory);
   WSI_GET_CB(AllocateCommandBuffers);
   WSI_GET_CB(BindBufferMemory);
   WSI_GET_CB(BindImageMemory);
   WSI_GET_CB(BeginCommandBuffer);
   WSI_GET_CB(CmdCopyImageToBuffer);
   WSI_GET_CB(CmdResetQueryPool);
   WSI_GET_CB(CmdWriteTimestamp);
   WSI_GET_CB(CreateBuffer);
   WSI_GET_CB(CreateCommandPool);
   WSI_GET_CB(CreateFence);
   WSI_GET_CB(CreateImage);
   WSI_GET_CB(CreateQueryPool);
   WSI_GET_CB(DestroyBuffer);
   WSI_GET_CB(DestroyCommandPool);
   WSI_GET_CB(DestroyFence);
   WSI_GET_CB(DestroyImage);
   WSI_GET_CB(DestroyQueryPool);
   WSI_GET_CB(EndCommandBuffer);
   WSI_GET_CB(FreeMemory);
   WSI_GET_CB(FreeCommandBuffers);
   WSI_GET_CB(GetBufferMemoryRequirements);
   WSI_GET_CB(GetImageMemoryRequirements);
   WSI_GET_CB(GetImageSubresourceLayout);
   WSI_GET_CB(GetMemoryFdKHR);
   WSI_GET_CB(GetPhysicalDeviceProperties);
   WSI_GET_CB(GetPhysicalDeviceFormatProperties);
   WSI_GET_CB(GetPhysicalDeviceFormatProperties2KHR);
   WSI_GET_CB(GetPhysicalDeviceQueueFamilyProperties);
   WSI_GET_CB(GetQueryPoolResults);
   WSI_GET_CB(ResetFences);
   WSI_GET_CB(QueueSubmit);
   WSI_GET_CB(QueryCurrentTimestampMESA);
   WSI_GET_CB(WaitForFences);
#undef WSI_GET_CB

#ifdef VK_USE_PLATFORM_XCB_KHR
   result = wsi_x11_init_wsi(wsi, alloc);
   if (result != VK_SUCCESS)
      goto fail;
#endif

#ifdef VK_USE_PLATFORM_WAYLAND_KHR
   result = wsi_wl_init_wsi(wsi, alloc, pdevice);
   if (result != VK_SUCCESS)
      goto fail;
#endif

#ifdef VK_USE_PLATFORM_DISPLAY_KHR
   result = wsi_display_init_wsi(wsi, alloc, display_fd);
   if (result != VK_SUCCESS) {
#ifdef VK_USE_PLATFORM_WAYLAND_KHR
      wsi_wl_finish_wsi(wsi, alloc);
#endif
#ifdef VK_USE_PLATFORM_XCB_KHR
      wsi_x11_finish_wsi(wsi, alloc);
#endif
      return result;
   }
#endif

   return VK_SUCCESS;

fail:
   wsi_device_finish(wsi, alloc);
   return result;
}

void
wsi_device_finish(struct wsi_device *wsi,
                  const VkAllocationCallbacks *alloc)
{
#ifdef VK_USE_PLATFORM_DISPLAY_KHR
   wsi_display_finish_wsi(wsi, alloc);
#endif
#ifdef VK_USE_PLATFORM_WAYLAND_KHR
   wsi_wl_finish_wsi(wsi, alloc);
#endif
#ifdef VK_USE_PLATFORM_XCB_KHR
   wsi_x11_finish_wsi(wsi, alloc);
#endif
}

VkResult
wsi_swapchain_init(const struct wsi_device *wsi,
                   struct wsi_swapchain *chain,
                   VkDevice device,
                   const VkSwapchainCreateInfoKHR *pCreateInfo,
                   const VkAllocationCallbacks *pAllocator)
{
   VkResult result;

   memset(chain, 0, sizeof(*chain));

   chain->wsi = wsi;
   chain->device = device;
   chain->alloc = *pAllocator;
   chain->use_prime_blit = false;
   chain->timing_insert = 0;
   chain->timing_count = 0;

   chain->cmd_pools =
      vk_zalloc(pAllocator, sizeof(VkCommandPool) * wsi->queue_family_count, 8,
                VK_SYSTEM_ALLOCATION_SCOPE_OBJECT);
   if (!chain->cmd_pools)
      return VK_ERROR_OUT_OF_HOST_MEMORY;

   for (uint32_t i = 0; i < wsi->queue_family_count; i++) {
      const VkCommandPoolCreateInfo cmd_pool_info = {
         .sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO,
         .pNext = NULL,
         .flags = 0,
         .queueFamilyIndex = i,
      };
      result = wsi->CreateCommandPool(device, &cmd_pool_info, &chain->alloc,
                                      &chain->cmd_pools[i]);
      if (result != VK_SUCCESS)
         goto fail;
   }

   return VK_SUCCESS;

fail:
   wsi_swapchain_finish(chain);
   return result;
}

void
wsi_swapchain_finish(struct wsi_swapchain *chain)
{
   for (unsigned i = 0; i < ARRAY_SIZE(chain->fences); i++)
      chain->wsi->DestroyFence(chain->device, chain->fences[i], &chain->alloc);

   for (uint32_t i = 0; i < chain->wsi->queue_family_count; i++) {
      chain->wsi->DestroyCommandPool(chain->device, chain->cmd_pools[i],
                                     &chain->alloc);
   }
   vk_free(&chain->alloc, chain->cmd_pools);
}

static uint32_t
select_memory_type(const struct wsi_device *wsi,
                   VkMemoryPropertyFlags props,
                   uint32_t type_bits)
{
   for (uint32_t i = 0; i < wsi->memory_props.memoryTypeCount; i++) {
       const VkMemoryType type = wsi->memory_props.memoryTypes[i];
       if ((type_bits & (1 << i)) && (type.propertyFlags & props) == props)
         return i;
   }

   unreachable("No memory type found");
}

static uint32_t
vk_format_size(VkFormat format)
{
   switch (format) {
   case VK_FORMAT_B8G8R8A8_UNORM:
   case VK_FORMAT_B8G8R8A8_SRGB:
      return 4;
   default:
      unreachable("Unknown WSI Format");
   }
}

static inline uint32_t
align_u32(uint32_t v, uint32_t a)
{
   assert(a != 0 && a == (a & -a));
   return (v + a - 1) & ~(a - 1);
}

static VkResult
wsi_image_init_timestamp(const struct wsi_swapchain *chain,
                         struct wsi_image *image)
{
   const struct wsi_device *wsi = chain->wsi;
   VkResult result;
   /* Set up command buffer to get timestamp info */

   result = wsi->CreateQueryPool(chain->device,
                                 &(const VkQueryPoolCreateInfo) {
                                    .sType = VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO,
                                       .queryType = VK_QUERY_TYPE_TIMESTAMP,
                                       .queryCount = 1,
                                       },
                                 NULL,
                                 &image->query_pool);

   if (result != VK_SUCCESS)
      goto fail;

   result = wsi->AllocateCommandBuffers(chain->device,
                                        &(const VkCommandBufferAllocateInfo) {
                                           .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO,
                                              .pNext = NULL,
                                              .commandPool = chain->cmd_pools[0],
                                              .level = VK_COMMAND_BUFFER_LEVEL_PRIMARY,
                                              .commandBufferCount = 1,
                                              },
                                        &image->timestamp_buffer);
   if (result != VK_SUCCESS)
      goto fail;

   wsi->BeginCommandBuffer(image->timestamp_buffer,
                           &(VkCommandBufferBeginInfo) {
                              .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO,
                                 .flags = 0
                                 });

   wsi->CmdResetQueryPool(image->timestamp_buffer,
                          image->query_pool,
                          0, 1);

   wsi->CmdWriteTimestamp(image->timestamp_buffer,
                          VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,
                          image->query_pool,
                          0);

   wsi->EndCommandBuffer(image->timestamp_buffer);

   return VK_SUCCESS;
fail:
   return result;
}

VkResult
wsi_create_native_image(const struct wsi_swapchain *chain,
                        const VkSwapchainCreateInfoKHR *pCreateInfo,
                        uint32_t num_modifier_lists,
                        const uint32_t *num_modifiers,
                        const uint64_t *const *modifiers,
                        struct wsi_image *image)
{
   const struct wsi_device *wsi = chain->wsi;
   VkResult result;

   memset(image, 0, sizeof(*image));
   for (int i = 0; i < ARRAY_SIZE(image->fds); i++)
      image->fds[i] = -1;

   struct wsi_image_create_info image_wsi_info = {
      .sType = VK_STRUCTURE_TYPE_WSI_IMAGE_CREATE_INFO_MESA,
      .pNext = NULL,
   };

   uint32_t image_modifier_count = 0, modifier_prop_count = 0;
   struct wsi_format_modifier_properties *modifier_props = NULL;
   uint64_t *image_modifiers = NULL;
   if (num_modifier_lists == 0) {
      /* If we don't have modifiers, fall back to the legacy "scanout" flag */
      image_wsi_info.scanout = true;
   } else {
      /* The winsys can't request modifiers if we don't support them. */
      assert(wsi->supports_modifiers);
      struct wsi_format_modifier_properties_list modifier_props_list = {
         .sType = VK_STRUCTURE_TYPE_WSI_FORMAT_MODIFIER_PROPERTIES_LIST_MESA,
         .pNext = NULL,
      };
      VkFormatProperties2KHR format_props = {
         .sType = VK_STRUCTURE_TYPE_FORMAT_PROPERTIES_2_KHR,
         .pNext = &modifier_props_list,
      };
      wsi->GetPhysicalDeviceFormatProperties2KHR(wsi->pdevice,
                                                 pCreateInfo->imageFormat,
                                                 &format_props);
      assert(modifier_props_list.modifier_count > 0);
      modifier_props = vk_alloc(&chain->alloc,
                                sizeof(*modifier_props) *
                                modifier_props_list.modifier_count,
                                8,
                                VK_SYSTEM_ALLOCATION_SCOPE_COMMAND);
      if (!modifier_props) {
         result = VK_ERROR_OUT_OF_HOST_MEMORY;
         goto fail;
      }

      modifier_props_list.modifier_properties = modifier_props;
      wsi->GetPhysicalDeviceFormatProperties2KHR(wsi->pdevice,
                                                 pCreateInfo->imageFormat,
                                                 &format_props);
      modifier_prop_count = modifier_props_list.modifier_count;

      uint32_t max_modifier_count = 0;
      for (uint32_t l = 0; l < num_modifier_lists; l++)
         max_modifier_count = MAX2(max_modifier_count, num_modifiers[l]);

      image_modifiers = vk_alloc(&chain->alloc,
                                 sizeof(*image_modifiers) *
                                 max_modifier_count,
                                 8,
                                 VK_SYSTEM_ALLOCATION_SCOPE_COMMAND);
      if (!image_modifiers) {
         result = VK_ERROR_OUT_OF_HOST_MEMORY;
         goto fail;
      }

      image_modifier_count = 0;
      for (uint32_t l = 0; l < num_modifier_lists; l++) {
         /* Walk the modifier lists and construct a list of supported
          * modifiers.
          */
         for (uint32_t i = 0; i < num_modifiers[l]; i++) {
            for (uint32_t j = 0; j < modifier_prop_count; j++) {
               if (modifier_props[j].modifier == modifiers[l][i])
                  image_modifiers[image_modifier_count++] = modifiers[l][i];
            }
         }

         /* We only want to take the modifiers from the first list */
         if (image_modifier_count > 0)
            break;
      }

      if (image_modifier_count > 0) {
         image_wsi_info.modifier_count = image_modifier_count;
         image_wsi_info.modifiers = image_modifiers;
      } else {
         /* TODO: Add a proper error here */
         assert(!"Failed to find a supported modifier!  This should never "
                 "happen because LINEAR should always be available");
         result = VK_ERROR_OUT_OF_HOST_MEMORY;
         goto fail;
      }
   }

   const VkImageCreateInfo image_info = {
      .sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO,
      .pNext = &image_wsi_info,
      .flags = 0,
      .imageType = VK_IMAGE_TYPE_2D,
      .format = pCreateInfo->imageFormat,
      .extent = {
         .width = pCreateInfo->imageExtent.width,
         .height = pCreateInfo->imageExtent.height,
         .depth = 1,
      },
      .mipLevels = 1,
      .arrayLayers = 1,
      .samples = VK_SAMPLE_COUNT_1_BIT,
      .tiling = VK_IMAGE_TILING_OPTIMAL,
      .usage = pCreateInfo->imageUsage,
      .sharingMode = pCreateInfo->imageSharingMode,
      .queueFamilyIndexCount = pCreateInfo->queueFamilyIndexCount,
      .pQueueFamilyIndices = pCreateInfo->pQueueFamilyIndices,
      .initialLayout = VK_IMAGE_LAYOUT_UNDEFINED,
   };
   result = wsi->CreateImage(chain->device, &image_info,
                             &chain->alloc, &image->image);
   if (result != VK_SUCCESS)
      goto fail;

   VkMemoryRequirements reqs;
   wsi->GetImageMemoryRequirements(chain->device, image->image, &reqs);

   const struct wsi_memory_allocate_info memory_wsi_info = {
      .sType = VK_STRUCTURE_TYPE_WSI_MEMORY_ALLOCATE_INFO_MESA,
      .pNext = NULL,
      .implicit_sync = true,
   };
   const VkExportMemoryAllocateInfoKHR memory_export_info = {
      .sType = VK_STRUCTURE_TYPE_EXPORT_MEMORY_ALLOCATE_INFO_KHR,
      .pNext = &memory_wsi_info,
      .handleTypes = VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT,
   };
   const VkMemoryDedicatedAllocateInfoKHR memory_dedicated_info = {
      .sType = VK_STRUCTURE_TYPE_MEMORY_DEDICATED_ALLOCATE_INFO_KHR,
      .pNext = &memory_export_info,
      .image = image->image,
      .buffer = VK_NULL_HANDLE,
   };
   const VkMemoryAllocateInfo memory_info = {
      .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
      .pNext = &memory_dedicated_info,
      .allocationSize = reqs.size,
      .memoryTypeIndex = select_memory_type(wsi, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT,
                                            reqs.memoryTypeBits),
   };
   result = wsi->AllocateMemory(chain->device, &memory_info,
                                &chain->alloc, &image->memory);
   if (result != VK_SUCCESS)
      goto fail;

   result = wsi->BindImageMemory(chain->device, image->image,
                                 image->memory, 0);
   if (result != VK_SUCCESS)
      goto fail;

   const VkMemoryGetFdInfoKHR memory_get_fd_info = {
      .sType = VK_STRUCTURE_TYPE_MEMORY_GET_FD_INFO_KHR,
      .pNext = NULL,
      .memory = image->memory,
      .handleType = VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT,
   };
   int fd;
   result = wsi->GetMemoryFdKHR(chain->device, &memory_get_fd_info, &fd);
   if (result != VK_SUCCESS)
      goto fail;

   result = wsi_image_init_timestamp(chain, image);

   if (result != VK_SUCCESS)
      goto fail;

   if (num_modifier_lists > 0) {
      image->drm_modifier = wsi->image_get_modifier(image->image);
      assert(image->drm_modifier != DRM_FORMAT_MOD_INVALID);

      for (uint32_t j = 0; j < modifier_prop_count; j++) {
         if (modifier_props[j].modifier == image->drm_modifier) {
            image->num_planes = modifier_props[j].modifier_plane_count;
            break;
         }
      }

      for (uint32_t p = 0; p < image->num_planes; p++) {
         const VkImageSubresource image_subresource = {
            .aspectMask = VK_IMAGE_ASPECT_PLANE_0_BIT_KHR << p,
            .mipLevel = 0,
            .arrayLayer = 0,
         };
         VkSubresourceLayout image_layout;
         wsi->GetImageSubresourceLayout(chain->device, image->image,
                                        &image_subresource, &image_layout);
         image->sizes[p] = image_layout.size;
         image->row_pitches[p] = image_layout.rowPitch;
         image->offsets[p] = image_layout.offset;
         if (p == 0) {
            image->fds[p] = fd;
         } else {
            image->fds[p] = dup(fd);
            if (image->fds[p] == -1) {
               for (uint32_t i = 0; i < p; i++)
                  close(image->fds[p]);

               goto fail;
            }
         }
      }
   } else {
      const VkImageSubresource image_subresource = {
         .aspectMask = VK_IMAGE_ASPECT_COLOR_BIT,
         .mipLevel = 0,
         .arrayLayer = 0,
      };
      VkSubresourceLayout image_layout;
      wsi->GetImageSubresourceLayout(chain->device, image->image,
                                     &image_subresource, &image_layout);

      image->drm_modifier = DRM_FORMAT_MOD_INVALID;
      image->num_planes = 1;
      image->sizes[0] = reqs.size;
      image->row_pitches[0] = image_layout.rowPitch;
      image->offsets[0] = 0;
      image->fds[0] = fd;
   }

   vk_free(&chain->alloc, modifier_props);
   vk_free(&chain->alloc, image_modifiers);

   return VK_SUCCESS;

fail:
   vk_free(&chain->alloc, modifier_props);
   vk_free(&chain->alloc, image_modifiers);
   wsi_destroy_image(chain, image);

   return result;
}

#define WSI_PRIME_LINEAR_STRIDE_ALIGN 256

VkResult
wsi_create_prime_image(const struct wsi_swapchain *chain,
                       const VkSwapchainCreateInfoKHR *pCreateInfo,
                       struct wsi_image *image)
{
   const struct wsi_device *wsi = chain->wsi;
   VkResult result;

   memset(image, 0, sizeof(*image));

   const uint32_t cpp = vk_format_size(pCreateInfo->imageFormat);
   const uint32_t linear_stride = align_u32(pCreateInfo->imageExtent.width * cpp,
                                            WSI_PRIME_LINEAR_STRIDE_ALIGN);

   uint32_t linear_size = linear_stride * pCreateInfo->imageExtent.height;
   linear_size = align_u32(linear_size, 4096);

   const VkExternalMemoryBufferCreateInfoKHR prime_buffer_external_info = {
      .sType = VK_STRUCTURE_TYPE_EXTERNAL_MEMORY_BUFFER_CREATE_INFO_KHR,
      .pNext = NULL,
      .handleTypes = VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT,
   };
   const VkBufferCreateInfo prime_buffer_info = {
      .sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,
      .pNext = &prime_buffer_external_info,
      .size = linear_size,
      .usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT,
      .sharingMode = VK_SHARING_MODE_EXCLUSIVE,
   };
   result = wsi->CreateBuffer(chain->device, &prime_buffer_info,
                              &chain->alloc, &image->prime.buffer);
   if (result != VK_SUCCESS)
      goto fail;

   VkMemoryRequirements reqs;
   wsi->GetBufferMemoryRequirements(chain->device, image->prime.buffer, &reqs);
   assert(reqs.size <= linear_size);

   const struct wsi_memory_allocate_info memory_wsi_info = {
      .sType = VK_STRUCTURE_TYPE_WSI_MEMORY_ALLOCATE_INFO_MESA,
      .pNext = NULL,
      .implicit_sync = true,
   };
   const VkExportMemoryAllocateInfoKHR prime_memory_export_info = {
      .sType = VK_STRUCTURE_TYPE_EXPORT_MEMORY_ALLOCATE_INFO_KHR,
      .pNext = &memory_wsi_info,
      .handleTypes = VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT,
   };
   const VkMemoryDedicatedAllocateInfoKHR prime_memory_dedicated_info = {
      .sType = VK_STRUCTURE_TYPE_MEMORY_DEDICATED_ALLOCATE_INFO_KHR,
      .pNext = &prime_memory_export_info,
      .image = VK_NULL_HANDLE,
      .buffer = image->prime.buffer,
   };
   const VkMemoryAllocateInfo prime_memory_info = {
      .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
      .pNext = &prime_memory_dedicated_info,
      .allocationSize = linear_size,
      .memoryTypeIndex = select_memory_type(wsi, 0, reqs.memoryTypeBits),
   };
   result = wsi->AllocateMemory(chain->device, &prime_memory_info,
                                &chain->alloc, &image->prime.memory);
   if (result != VK_SUCCESS)
      goto fail;

   result = wsi->BindBufferMemory(chain->device, image->prime.buffer,
                                  image->prime.memory, 0);
   if (result != VK_SUCCESS)
      goto fail;

   const VkImageCreateInfo image_info = {
      .sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO,
      .pNext = NULL,
      .flags = 0,
      .imageType = VK_IMAGE_TYPE_2D,
      .format = pCreateInfo->imageFormat,
      .extent = {
         .width = pCreateInfo->imageExtent.width,
         .height = pCreateInfo->imageExtent.height,
         .depth = 1,
      },
      .mipLevels = 1,
      .arrayLayers = 1,
      .samples = VK_SAMPLE_COUNT_1_BIT,
      .tiling = VK_IMAGE_TILING_OPTIMAL,
      .usage = pCreateInfo->imageUsage | VK_IMAGE_USAGE_TRANSFER_SRC_BIT,
      .sharingMode = pCreateInfo->imageSharingMode,
      .queueFamilyIndexCount = pCreateInfo->queueFamilyIndexCount,
      .pQueueFamilyIndices = pCreateInfo->pQueueFamilyIndices,
      .initialLayout = VK_IMAGE_LAYOUT_UNDEFINED,
   };
   result = wsi->CreateImage(chain->device, &image_info,
                             &chain->alloc, &image->image);
   if (result != VK_SUCCESS)
      goto fail;

   wsi->GetImageMemoryRequirements(chain->device, image->image, &reqs);

   const VkMemoryDedicatedAllocateInfoKHR memory_dedicated_info = {
      .sType = VK_STRUCTURE_TYPE_MEMORY_DEDICATED_ALLOCATE_INFO_KHR,
      .pNext = NULL,
      .image = image->image,
      .buffer = VK_NULL_HANDLE,
   };
   const VkMemoryAllocateInfo memory_info = {
      .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
      .pNext = &memory_dedicated_info,
      .allocationSize = reqs.size,
      .memoryTypeIndex = select_memory_type(wsi, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT,
                                            reqs.memoryTypeBits),
   };
   result = wsi->AllocateMemory(chain->device, &memory_info,
                                &chain->alloc, &image->memory);
   if (result != VK_SUCCESS)
      goto fail;

   result = wsi->BindImageMemory(chain->device, image->image,
                                 image->memory, 0);
   if (result != VK_SUCCESS)
      goto fail;

   image->prime.blit_cmd_buffers =
      vk_zalloc(&chain->alloc,
                sizeof(VkCommandBuffer) * wsi->queue_family_count, 8,
                VK_SYSTEM_ALLOCATION_SCOPE_OBJECT);
   if (!image->prime.blit_cmd_buffers) {
      result = VK_ERROR_OUT_OF_HOST_MEMORY;
      goto fail;
   }

   for (uint32_t i = 0; i < wsi->queue_family_count; i++) {
      const VkCommandBufferAllocateInfo cmd_buffer_info = {
         .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO,
         .pNext = NULL,
         .commandPool = chain->cmd_pools[i],
         .level = VK_COMMAND_BUFFER_LEVEL_PRIMARY,
         .commandBufferCount = 1,
      };
      result = wsi->AllocateCommandBuffers(chain->device, &cmd_buffer_info,
                                           &image->prime.blit_cmd_buffers[i]);
      if (result != VK_SUCCESS)
         goto fail;

      const VkCommandBufferBeginInfo begin_info = {
         .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO,
      };
      wsi->BeginCommandBuffer(image->prime.blit_cmd_buffers[i], &begin_info);

      struct VkBufferImageCopy buffer_image_copy = {
         .bufferOffset = 0,
         .bufferRowLength = linear_stride / cpp,
         .bufferImageHeight = 0,
         .imageSubresource = {
            .aspectMask = VK_IMAGE_ASPECT_COLOR_BIT,
            .mipLevel = 0,
            .baseArrayLayer = 0,
            .layerCount = 1,
         },
         .imageOffset = { .x = 0, .y = 0, .z = 0 },
         .imageExtent = {
            .width = pCreateInfo->imageExtent.width,
            .height = pCreateInfo->imageExtent.height,
            .depth = 1,
         },
      };
      wsi->CmdCopyImageToBuffer(image->prime.blit_cmd_buffers[i],
                                image->image,
                                VK_IMAGE_LAYOUT_PRESENT_SRC_KHR,
                                image->prime.buffer,
                                1, &buffer_image_copy);

      result = wsi->EndCommandBuffer(image->prime.blit_cmd_buffers[i]);
      if (result != VK_SUCCESS)
         goto fail;
   }

   result = wsi_image_init_timestamp(chain, image);

   if (result != VK_SUCCESS)
      goto fail;

   const VkMemoryGetFdInfoKHR linear_memory_get_fd_info = {
      .sType = VK_STRUCTURE_TYPE_MEMORY_GET_FD_INFO_KHR,
      .pNext = NULL,
      .memory = image->prime.memory,
      .handleType = VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT,
   };
   int fd;
   result = wsi->GetMemoryFdKHR(chain->device, &linear_memory_get_fd_info, &fd);
   if (result != VK_SUCCESS)
      goto fail;

   image->drm_modifier = DRM_FORMAT_MOD_LINEAR;
   image->num_planes = 1;
   image->sizes[0] = linear_size;
   image->row_pitches[0] = linear_stride;
   image->offsets[0] = 0;
   image->fds[0] = fd;

   return VK_SUCCESS;

fail:
   wsi_destroy_image(chain, image);

   return result;
}

void
wsi_destroy_image(const struct wsi_swapchain *chain,
                  struct wsi_image *image)
{
   const struct wsi_device *wsi = chain->wsi;

   if (image->prime.blit_cmd_buffers) {
      for (uint32_t i = 0; i < wsi->queue_family_count; i++) {
         wsi->FreeCommandBuffers(chain->device, chain->cmd_pools[i],
                                 1, &image->prime.blit_cmd_buffers[i]);
      }
      vk_free(&chain->alloc, image->prime.blit_cmd_buffers);
   }

   wsi->FreeMemory(chain->device, image->memory, &chain->alloc);
   wsi->DestroyImage(chain->device, image->image, &chain->alloc);
   wsi->FreeMemory(chain->device, image->prime.memory, &chain->alloc);
   wsi->DestroyBuffer(chain->device, image->prime.buffer, &chain->alloc);
}

VkResult
wsi_common_get_surface_support(struct wsi_device *wsi_device,
                               int local_fd,
                               uint32_t queueFamilyIndex,
                               VkSurfaceKHR _surface,
                               const VkAllocationCallbacks *alloc,
                               VkBool32* pSupported)
{
   ICD_FROM_HANDLE(VkIcdSurfaceBase, surface, _surface);
   struct wsi_interface *iface = wsi_device->wsi[surface->platform];

   return iface->get_support(surface, wsi_device, alloc,
                             queueFamilyIndex, local_fd, pSupported);
}

VkResult
wsi_common_get_surface_capabilities(struct wsi_device *wsi_device,
                                    VkSurfaceKHR _surface,
                                    VkSurfaceCapabilitiesKHR *pSurfaceCapabilities)
{
   ICD_FROM_HANDLE(VkIcdSurfaceBase, surface, _surface);
   struct wsi_interface *iface = wsi_device->wsi[surface->platform];

   return iface->get_capabilities(surface, pSurfaceCapabilities);
}

VkResult
wsi_common_get_surface_capabilities2(struct wsi_device *wsi_device,
                                     const VkPhysicalDeviceSurfaceInfo2KHR *pSurfaceInfo,
                                     VkSurfaceCapabilities2KHR *pSurfaceCapabilities)
{
   ICD_FROM_HANDLE(VkIcdSurfaceBase, surface, pSurfaceInfo->surface);
   struct wsi_interface *iface = wsi_device->wsi[surface->platform];

   return iface->get_capabilities2(surface, pSurfaceInfo->pNext,
                                   pSurfaceCapabilities);
}

VkResult
wsi_common_get_surface_capabilities2ext(struct wsi_device *wsi_device,
                                        VkSurfaceKHR _surface,
                                        VkSurfaceCapabilities2EXT *pSurfaceCapabilities)
{
   ICD_FROM_HANDLE(VkIcdSurfaceBase, surface, _surface);
   struct wsi_interface *iface = wsi_device->wsi[surface->platform];

   return iface->get_capabilities2ext(surface, pSurfaceCapabilities);
}

VkResult
wsi_common_get_surface_formats(struct wsi_device *wsi_device,
                               VkSurfaceKHR _surface,
                               uint32_t *pSurfaceFormatCount,
                               VkSurfaceFormatKHR *pSurfaceFormats)
{
   ICD_FROM_HANDLE(VkIcdSurfaceBase, surface, _surface);
   struct wsi_interface *iface = wsi_device->wsi[surface->platform];

   return iface->get_formats(surface, wsi_device,
                             pSurfaceFormatCount, pSurfaceFormats);
}

VkResult
wsi_common_get_surface_formats2(struct wsi_device *wsi_device,
                                const VkPhysicalDeviceSurfaceInfo2KHR *pSurfaceInfo,
                                uint32_t *pSurfaceFormatCount,
                                VkSurfaceFormat2KHR *pSurfaceFormats)
{
   ICD_FROM_HANDLE(VkIcdSurfaceBase, surface, pSurfaceInfo->surface);
   struct wsi_interface *iface = wsi_device->wsi[surface->platform];

   return iface->get_formats2(surface, wsi_device, pSurfaceInfo->pNext,
                              pSurfaceFormatCount, pSurfaceFormats);
}

VkResult
wsi_common_get_surface_present_modes(struct wsi_device *wsi_device,
                                     VkSurfaceKHR _surface,
                                     uint32_t *pPresentModeCount,
                                     VkPresentModeKHR *pPresentModes)
{
   ICD_FROM_HANDLE(VkIcdSurfaceBase, surface, _surface);
   struct wsi_interface *iface = wsi_device->wsi[surface->platform];

   return iface->get_present_modes(surface, pPresentModeCount,
                                   pPresentModes);
}

VkResult
wsi_common_create_swapchain(struct wsi_device *wsi,
                            VkDevice device,
                            int fd,
                            const VkSwapchainCreateInfoKHR *pCreateInfo,
                            const VkAllocationCallbacks *pAllocator,
                            VkSwapchainKHR *pSwapchain)
{
   ICD_FROM_HANDLE(VkIcdSurfaceBase, surface, pCreateInfo->surface);
   struct wsi_interface *iface = wsi->wsi[surface->platform];
   struct wsi_swapchain *swapchain;

   VkResult result = iface->create_swapchain(surface, device, wsi, fd,
                                             pCreateInfo, pAllocator,
                                             &swapchain);
   if (result != VK_SUCCESS)
      return result;

   *pSwapchain = wsi_swapchain_to_handle(swapchain);

   return VK_SUCCESS;
}

void
wsi_common_destroy_swapchain(VkDevice device,
                             VkSwapchainKHR _swapchain,
                             const VkAllocationCallbacks *pAllocator)
{
   WSI_FROM_HANDLE(wsi_swapchain, swapchain, _swapchain);
   if (!swapchain)
      return;

   swapchain->destroy(swapchain, pAllocator);
}

VkResult
wsi_common_get_images(VkSwapchainKHR _swapchain,
                      uint32_t *pSwapchainImageCount,
                      VkImage *pSwapchainImages)
{
   WSI_FROM_HANDLE(wsi_swapchain, swapchain, _swapchain);
   VK_OUTARRAY_MAKE(images, pSwapchainImages, pSwapchainImageCount);

   for (uint32_t i = 0; i < swapchain->image_count; i++) {
      vk_outarray_append(&images, image) {
         *image = swapchain->get_wsi_image(swapchain, i)->image;
      }
   }

   return vk_outarray_status(&images);
}

VkResult
wsi_common_acquire_next_image(const struct wsi_device *wsi,
                              VkDevice device,
                              VkSwapchainKHR _swapchain,
                              uint64_t timeout,
                              VkSemaphore semaphore,
                              uint32_t *pImageIndex)
{
   WSI_FROM_HANDLE(wsi_swapchain, swapchain, _swapchain);

   return swapchain->acquire_next_image(swapchain, timeout,
                                        semaphore, pImageIndex);
}

static struct wsi_timing *
wsi_get_timing(struct wsi_swapchain *chain, uint32_t i)
{
   uint32_t j = WSI_TIMING_HISTORY + chain->timing_insert - chain->timing_count + i;

   if (j >= WSI_TIMING_HISTORY)
      j -= WSI_TIMING_HISTORY;
   return &chain->timing[j];
}

static struct wsi_timing *
wsi_next_timing(struct wsi_swapchain *chain, int image_index)
{
   uint32_t j;
   struct wsi_timing *timing;

   j = chain->timing_insert;
   ++chain->timing_insert;
   if (chain->timing_insert >= WSI_TIMING_HISTORY)
      chain->timing_insert = 0;
   if (chain->timing_count < WSI_TIMING_HISTORY)
      ++chain->timing_count;
   timing = &chain->timing[j];
   memset(timing, '\0', sizeof (*timing));
   return timing;
}

void
wsi_mark_timing(struct wsi_swapchain *swapchain,
                struct wsi_image *image,
                uint64_t ust,
                uint64_t msc)
{
   const struct wsi_device *wsi = swapchain->wsi;
   struct wsi_timing *timing = image->timing;
   VkResult result;

   if (!timing)
      return;

   uint64_t     render_timestamp;

   result = wsi->GetQueryPoolResults(swapchain->device, image->query_pool,
                                     0, 1, sizeof(render_timestamp), &render_timestamp,
                                     sizeof (uint64_t),
                                     VK_QUERY_RESULT_64_BIT|VK_QUERY_RESULT_WAIT_BIT);

   if (result != VK_SUCCESS)
      return;

   uint64_t     current_gpu_timestamp;
   result = wsi->QueryCurrentTimestampMESA(swapchain->device, &current_gpu_timestamp);
   if (result != VK_SUCCESS)
      return;
   uint64_t     current_time = swapchain->get_current_time();

   VkRefreshCycleDurationGOOGLE display_timings;
   swapchain->get_refresh_cycle_duration(swapchain, &display_timings);

   uint64_t     refresh_duration = display_timings.refreshDuration;

   /* When did drawing complete (in nsec) */

   int64_t since_render = (int64_t) (current_gpu_timestamp - render_timestamp) * wsi->timestamp_period;;
   uint64_t render_time = current_time - since_render;

   if (render_time > ust)
      render_time = ust;

   uint64_t     render_frames = (ust - render_time) / refresh_duration;

   uint64_t     earliest_time = ust - render_frames * refresh_duration;

   /* Use the presentation mode to figure out when the image could have been
    * displayed. It couldn't have been displayed before the previous image, so
    * use that as a lower bound. If we're in FIFO mode, then it couldn't have
    * been displayed before one frame *after* the previous image
    */
   uint64_t     possible_frame = swapchain->frame_ust;

   switch (swapchain->present_mode) {
   case VK_PRESENT_MODE_FIFO_KHR:
   case VK_PRESENT_MODE_FIFO_RELAXED_KHR:
      possible_frame += refresh_duration;
      break;
   default:
      break;
   }
   if (earliest_time < possible_frame)
      earliest_time = possible_frame;

   if (earliest_time > ust)
      earliest_time = ust;

   timing->timing.actualPresentTime = ust;
   timing->timing.earliestPresentTime = earliest_time;
   timing->timing.presentMargin = earliest_time - render_time;
   timing->complete = true;

   swapchain->frame_msc = msc;
   swapchain->frame_ust = ust;
}

VkResult
wsi_common_queue_present(const struct wsi_device *wsi,
                         VkDevice device,
                         VkQueue queue,
                         int queue_family_index,
                         const VkPresentInfoKHR *pPresentInfo)
{
   VkResult final_result = VK_SUCCESS;

   const VkPresentRegionsKHR *regions =
      vk_find_struct_const(pPresentInfo->pNext, PRESENT_REGIONS_KHR);
   const VkPresentTimesInfoGOOGLE *present_times_info =
      vk_find_struct_const(pPresentInfo->pNext, PRESENT_TIMES_INFO_GOOGLE);

   for (uint32_t i = 0; i < pPresentInfo->swapchainCount; i++) {
      WSI_FROM_HANDLE(wsi_swapchain, swapchain, pPresentInfo->pSwapchains[i]);
      VkResult result;
      struct wsi_timing *timing = NULL;

      if (swapchain->fences[0] == VK_NULL_HANDLE) {
         const VkFenceCreateInfo fence_info = {
            .sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO,
            .pNext = NULL,
            .flags = 0,
         };
         result = wsi->CreateFence(device, &fence_info,
                                   &swapchain->alloc,
                                   &swapchain->fences[0]);
         if (result != VK_SUCCESS)
            goto fail_present;
      } else {
         wsi->ResetFences(device, 1, &swapchain->fences[0]);
      }

      VkCommandBuffer submit_buffers[2];
      VkSubmitInfo submit_info = {
         .sType = VK_STRUCTURE_TYPE_SUBMIT_INFO,
         .pNext = NULL,
         .pCommandBuffers = submit_buffers,
         .commandBufferCount = 0
      };

      VkPipelineStageFlags *stage_flags = NULL;
      if (i == 0) {
         /* We only need/want to wait on semaphores once.  After that, we're
          * guaranteed ordering since it all happens on the same queue.
          */
         submit_info.waitSemaphoreCount = pPresentInfo->waitSemaphoreCount,
         submit_info.pWaitSemaphores = pPresentInfo->pWaitSemaphores,

         /* Set up the pWaitDstStageMasks */
         stage_flags = vk_alloc(&swapchain->alloc,
                                sizeof(VkPipelineStageFlags) *
                                pPresentInfo->waitSemaphoreCount,
                                8,
                                VK_SYSTEM_ALLOCATION_SCOPE_COMMAND);
         if (!stage_flags) {
            result = VK_ERROR_OUT_OF_HOST_MEMORY;
            goto fail_present;
         }
         for (uint32_t s = 0; s < pPresentInfo->waitSemaphoreCount; s++)
            stage_flags[s] = VK_PIPELINE_STAGE_ALL_GRAPHICS_BIT;

         submit_info.pWaitDstStageMask = stage_flags;
      }

      if (swapchain->use_prime_blit) {
         /* If we are using prime blits, we need to perform the blit now.  The
          * command buffer is attached to the image.
          */
         struct wsi_image *image =
            swapchain->get_wsi_image(swapchain, pPresentInfo->pImageIndices[i]);
         submit_buffers[submit_info.commandBufferCount++] = 
            image->prime.blit_cmd_buffers[queue_family_index];
      }

      /* Set up GOOGLE_display_timing bits */
      if (present_times_info &&
          present_times_info->pTimes != NULL &&
          i < present_times_info->swapchainCount)
      {
         const VkPresentTimeGOOGLE *present_time = &present_times_info->pTimes[i];

         struct wsi_image *image = swapchain->get_wsi_image(swapchain, pPresentInfo->pImageIndices[i]);

         timing = wsi_next_timing(swapchain, pPresentInfo->pImageIndices[i]);
         timing->timing.presentID = present_time->presentID;
         timing->timing.desiredPresentTime = present_time->desiredPresentTime;
         timing->target_msc = 0;
         image->timing = timing;

         if (present_time->desiredPresentTime != 0)
         {
            int64_t delta_nsec = (int64_t) (present_time->desiredPresentTime - swapchain->frame_ust);

            /* Set the target msc only if it's no more than two seconds from
             * now, and not stale
             */
            if (0 <= delta_nsec && delta_nsec <= 2000000000ul) {
               VkRefreshCycleDurationGOOGLE refresh_timing;

               swapchain->get_refresh_cycle_duration(swapchain, &refresh_timing);

               int64_t refresh = (int64_t) refresh_timing.refreshDuration;
               int64_t frames = (delta_nsec + refresh/2) / refresh;
               timing->target_msc = swapchain->frame_msc + frames;
            }
         }

         submit_buffers[submit_info.commandBufferCount++] = image->timestamp_buffer;
      }

      result = wsi->QueueSubmit(queue, 1, &submit_info, swapchain->fences[0]);
      vk_free(&swapchain->alloc, stage_flags);
      if (result != VK_SUCCESS)
         goto fail_present;

      const VkPresentRegionKHR *region = NULL;
      if (regions && regions->pRegions)
         region = &regions->pRegions[i];

      result = swapchain->queue_present(swapchain,
                                        pPresentInfo->pImageIndices[i],
                                        region);
      if (result != VK_SUCCESS)
         goto fail_present;

      VkFence last = swapchain->fences[2];
      swapchain->fences[2] = swapchain->fences[1];
      swapchain->fences[1] = swapchain->fences[0];
      swapchain->fences[0] = last;

      if (last != VK_NULL_HANDLE) {
         wsi->WaitForFences(device, 1, &last, true, 1);
      }

   fail_present:
      if (pPresentInfo->pResults != NULL)
         pPresentInfo->pResults[i] = result;

      /* Let the final result be our first unsuccessful result */
      if (final_result == VK_SUCCESS)
         final_result = result;
   }

   return final_result;
}

VkResult
wsi_common_get_refresh_cycle_duration(const struct wsi_device *wsi,
                                      VkDevice device_h,
                                      VkSwapchainKHR _swapchain,
                                      VkRefreshCycleDurationGOOGLE *pDisplayTimingProperties)
{
   WSI_FROM_HANDLE(wsi_swapchain, swapchain, _swapchain);

   if (!swapchain->get_refresh_cycle_duration)
      return VK_ERROR_EXTENSION_NOT_PRESENT;
   return swapchain->get_refresh_cycle_duration(swapchain, pDisplayTimingProperties);
}


VkResult
wsi_common_get_past_presentation_timing(const struct wsi_device *wsi,
                                        VkDevice device_h,
                                        VkSwapchainKHR _swapchain,
                                        uint32_t *count,
                                        VkPastPresentationTimingGOOGLE *timings)
{
   WSI_FROM_HANDLE(wsi_swapchain, swapchain, _swapchain);
   uint32_t timing_count_requested = *count;
   uint32_t timing_count_available = 0;
   uint32_t t;

   /* Count the number of completed entries, copy */
   for (t = 0; t < swapchain->timing_count; t++) {
      struct wsi_timing *timing = wsi_get_timing(swapchain, t);

      if (timing->complete && !timing->consumed) {
         if (timings && timing_count_available < timing_count_requested) {
            timings[timing_count_available] = timing->timing;
            timing->consumed = true;
         }
         timing_count_available++;
      }
   }

   *count = timing_count_available;

   if (timing_count_available > timing_count_requested && timings != NULL)
      return VK_INCOMPLETE;

   return VK_SUCCESS;
}
