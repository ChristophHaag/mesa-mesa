/**************************************************************************
 * 
 * Copyright � 2007 Intel Corporation
 * Copyright 2006 Tungsten Graphics, Inc., Bismarck, ND., USA
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
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NON-INFRINGEMENT. IN NO EVENT SHALL
 * THE COPYRIGHT HOLDERS, AUTHORS AND/OR ITS SUPPLIERS BE LIABLE FOR ANY CLAIM,
 * DAMAGES OR OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR 
 * OTHERWISE, ARISING FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE 
 * USE OR OTHER DEALINGS IN THE SOFTWARE.
 *
 * The above copyright notice and this permission notice (including the
 * next paragraph) shall be included in all copies or substantial portions
 * of the Software.
 * 
 * 
 **************************************************************************/
/*
 * Authors: Thomas Hellstr�m <thomas-at-tungstengraphics-dot-com>
 *          Keith Whitwell <keithw-at-tungstengraphics-dot-com>
 *	    Eric Anholt <eric@anholt.net>
 */

#ifndef _DRI_BUFMGR_H_
#define _DRI_BUFMGR_H_
#include <xf86drm.h>

typedef struct _dri_bufmgr dri_bufmgr;
typedef struct _dri_bo dri_bo;
typedef struct _dri_fence dri_fence;

struct _dri_bo {
   /** Size in bytes of the buffer object. */
   unsigned long size;
   /**
    * Card virtual address (offset from the beginning of the aperture) for the
    * object.  Only valid while validated.
    */
   unsigned long offset;
   /**
    * Virtual address for accessing the buffer data.  Only valid while mapped.
    */
   void *virtual;
   /** Buffer manager context associated with this buffer object */
   dri_bufmgr *bufmgr;
};

struct _dri_fence {
   /**
    * This is an ORed mask of DRM_BO_FLAG_READ, DRM_BO_FLAG_WRITE, and
    * DRM_FLAG_EXE indicating the operations associated with this fence.
    *
    * It is constant for the life of the fence object.
    */
   unsigned int type;
   /** Buffer manager context associated with this fence */
   dri_bufmgr *bufmgr;
};

/**
 * Context for a buffer manager instance.
 *
 * Contains public methods followed by private storage for the buffer manager.
 */
struct _dri_bufmgr {
   /**
    * Allocate a buffer object.
    *
    * Buffer objects are not necessarily initially mapped into CPU virtual
    * address space or graphics device aperture.  They must be mapped using
    * bo_map() to be used by the CPU, and validated for use using bo_validate()
    * to be used from the graphics device.
    */
   dri_bo *(*bo_alloc)(dri_bufmgr *bufmgr_ctx, const char *name,
		       unsigned long size, unsigned int alignment,
		       unsigned int location_mask);

   /**
    * Allocates a buffer object for a static allocation.
    *
    * Static allocations are ones such as the front buffer that are offered by
    * the X Server, which are never evicted and never moved.
    */
   dri_bo *(*bo_alloc_static)(dri_bufmgr *bufmgr_ctx, const char *name,
			      unsigned long offset, unsigned long size,
			      void *virtual, unsigned int location_mask);

   /** Takes a reference on a buffer object */
   void (*bo_reference)(dri_bo *bo);

   /**
    * Releases a reference on a buffer object, freeing the data if
    * rerefences remain.
    */
   void (*bo_unreference)(dri_bo *bo);

   /**
    * Maps the buffer into userspace.
    *
    * This function will block waiting for any existing fence on the buffer to
    * clear, first.  The resulting mapping is available at buf->virtual.
\    */
   int (*bo_map)(dri_bo *buf, GLboolean write_enable);

   /** Reduces the refcount on the userspace mapping of the buffer object. */
   int (*bo_unmap)(dri_bo *buf);

   /** Takes a reference on a fence object */
   void (*fence_reference)(dri_fence *fence);

   /**
    * Releases a reference on a fence object, freeing the data if
    * rerefences remain.
    */
   void (*fence_unreference)(dri_fence *fence);

   /**
    * Blocks until the given fence is signaled.
    */
   void (*fence_wait)(dri_fence *fence);

   /**
    * Tears down the buffer manager instance.
    */
   void (*destroy)(dri_bufmgr *bufmgr);

   /**
    * Add relocation entry in reloc_buf, to be set on command submission.
    *
    * \param reloc_buf Buffer to write the relocation into.
    * \param flags BO flags to be used in validating the target buffer.
    *	     Applicable flags include:
    *	     - DRM_BO_FLAG_READ: The buffer will be read in the process of
    *	       command execution.
    *	     - DRM_BO_FLAG_WRITE: The buffer will be written in the process of
    *	       command execution.
    *	     - DRM_BO_FLAG_MEM_TT: The buffer should be validated in TT memory.
    *	     - DRM_BO_FLAG_MEM_VRAM: The buffer should be validated in video
    *	       memory.
    * \param delta Constant value to be added to the relocation
    * \param offset Byte offset within batch_buf of the relocated pointer.
    * \param target Buffer whose offset should be written into the relocation
    *	     entry.
    */
   void (*emit_reloc)(dri_bo *reloc_buf, GLuint flags, GLuint delta,
		      GLuint offset, dri_bo *target);

   /**
    * Processes the relocations, either in userland or by converting the list
    * for use in batchbuffer submission.
    *
    * Kernel-based implementations will return a pointer to the arguments
    * to be handed with batchbuffer submission to the kernel.  The userland
    * implementation performs the buffer validation and emits relocations
    * into them the appopriate order.
    *
    * \param batch_buf buffer at the root of the tree of relocations
    * \param count returns the number of buffers validated.
    * \return relocation record for use in command submission.
    * */
   void *(*process_relocs)(dri_bo *batch_buf, GLuint *count);

   void (*post_submit)(dri_bo *batch_buf, dri_fence **fence);
};

dri_bo *dri_bo_alloc(dri_bufmgr *bufmgr, const char *name, unsigned long size,
		     unsigned int alignment, unsigned int location_mask);
dri_bo *dri_bo_alloc_static(dri_bufmgr *bufmgr, const char *name,
			    unsigned long offset, unsigned long size,
			    void *virtual, unsigned int location_mask);
void dri_bo_reference(dri_bo *bo);
void dri_bo_unreference(dri_bo *bo);
int dri_bo_map(dri_bo *buf, GLboolean write_enable);
int dri_bo_unmap(dri_bo *buf);
void dri_fence_wait(dri_fence *fence);
void dri_fence_reference(dri_fence *fence);
void dri_fence_unreference(dri_fence *fence);

void dri_bo_subdata(dri_bo *bo, unsigned long offset,
		    unsigned long size, const void *data);
void dri_bo_get_subdata(dri_bo *bo, unsigned long offset,
			unsigned long size, void *data);

dri_bufmgr *dri_bufmgr_ttm_init(int fd, unsigned int fence_type,
				unsigned int fence_type_flush);

void dri_bufmgr_fake_contended_lock_take(dri_bufmgr *bufmgr);
dri_bufmgr *dri_bufmgr_fake_init(unsigned long low_offset, void *low_virtual,
				 unsigned long size,
				 unsigned int (*fence_emit)(void *private),
				 int (*fence_wait)(void *private,
						   unsigned int cookie),
				 void *driver_priv);
void dri_bufmgr_destroy(dri_bufmgr *bufmgr);
dri_bo *dri_ttm_bo_create_from_handle(dri_bufmgr *bufmgr, const char *name,
				      unsigned int handle);

void dri_emit_reloc(dri_bo *batch_buf, GLuint flags, GLuint delta, GLuint offset, dri_bo *relocatee);
void *dri_process_relocs(dri_bo *batch_buf, uint32_t *count);
void dri_post_process_relocs(dri_bo *batch_buf);
void dri_post_submit(dri_bo *batch_buf, dri_fence **last_fence);
#endif
