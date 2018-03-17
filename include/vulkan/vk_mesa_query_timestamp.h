/*
 * Copyright © 2018 Keith Packard
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

#ifndef __VK_MESA_QUERY_TIMESTAMP_H__
#define __VK_MESA_QUERY_TIMESTAMP_H__

#ifdef __cplusplus
extern "C" {
#endif

typedef VkResult (VKAPI_PTR *PFN_vkQueryCurrentTimestampMESA)(VkDevice device, uint64_t *timestamp);

VKAPI_ATTR VkResult VKAPI_CALL vkQueryCurrentTimestampMESA(
    VkDevice                                    _device,
    uint64_t                                    *timestamp);

#ifdef __cplusplus
}
#endif

#endif /* __VK_MESA_QUERY_TIMESTAMP_H__ */

