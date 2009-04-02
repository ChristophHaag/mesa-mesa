/**************************************************************************
 *
 * Copyright 2008 Tungsten Graphics, Inc., Cedar Park, Texas.
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
 * The above copyright notice and this permission notice (including the
 * next paragraph) shall be included in all copies or substantial portions
 * of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS
 * OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF
 * MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND NON-INFRINGEMENT.
 * IN NO EVENT SHALL TUNGSTEN GRAPHICS AND/OR ITS SUPPLIERS BE LIABLE FOR
 * ANY CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT,
 * TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN CONNECTION WITH THE
 * SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.
 *
 **************************************************************************/

#include <windows.h>

#include "glapi/glthread.h"
#include "util/u_debug.h"
#include "pipe/p_screen.h"

#include "shared/stw_device.h"
#include "shared/stw_winsys.h"
#include "shared/stw_pixelformat.h"
#include "shared/stw_public.h"
#include "shared/stw_tls.h"

#ifdef WIN32_THREADS
extern _glthread_Mutex OneTimeLock;
extern void FreeAllTSD(void);
#endif


struct stw_device *stw_dev = NULL;


/**
 * XXX: Dispatch pipe_screen::flush_front_buffer to our 
 * stw_winsys::flush_front_buffer.
 */
static void 
st_flush_frontbuffer(struct pipe_screen *screen,
                     struct pipe_surface *surf,
                     void *context_private )
{
   const struct stw_winsys *stw_winsys = stw_dev->stw_winsys;
   HDC hdc = (HDC)context_private;
   
   stw_winsys->flush_frontbuffer(screen, surf, hdc);
}


boolean
st_init(const struct stw_winsys *stw_winsys)
{
   static struct stw_device stw_dev_storage;

   debug_printf("%s\n", __FUNCTION__);
   
   assert(!stw_dev);

   stw_tls_init();

   stw_dev = &stw_dev_storage;
   memset(stw_dev, 0, sizeof(*stw_dev));

#ifdef DEBUG
   stw_dev->memdbg_no = debug_memory_begin();
#endif
   
   stw_dev->stw_winsys = stw_winsys;

#ifdef WIN32_THREADS
   _glthread_INIT_MUTEX(OneTimeLock);
#endif

   stw_dev->screen = stw_winsys->create_screen();
   if(!stw_dev->screen)
      goto error1;

   stw_dev->screen->flush_frontbuffer = st_flush_frontbuffer;
   
   pipe_mutex_init( stw_dev->mutex );

   stw_dev->ctx_table = handle_table_create();
   if (!stw_dev->ctx_table) {
      goto error1;
   }

   pixelformat_init();

   return TRUE;

error1:
   stw_dev = NULL;
   return FALSE;
}


boolean
st_init_thread(void)
{
   if (!stw_tls_init_thread()) {
      return FALSE;
   }

   return TRUE;
}


void
st_cleanup_thread(void)
{
   stw_tls_cleanup_thread();
}


void
st_cleanup(void)
{
   unsigned i;

   debug_printf("%s\n", __FUNCTION__);

   if (!stw_dev)
      return;
   
   pipe_mutex_lock( stw_dev->mutex );
   {
      /* Ensure all contexts are destroyed */
      i = handle_table_get_first_handle(stw_dev->ctx_table);
      while (i) {
         stw_delete_context(i);
         i = handle_table_get_next_handle(stw_dev->ctx_table, i);
      }
      handle_table_destroy(stw_dev->ctx_table);
   }
   pipe_mutex_unlock( stw_dev->mutex );

   pipe_mutex_destroy( stw_dev->mutex );
   
   stw_dev->screen->destroy(stw_dev->screen);

#ifdef WIN32_THREADS
   _glthread_DESTROY_MUTEX(OneTimeLock);
   FreeAllTSD();
#endif

#ifdef DEBUG
   debug_memory_end(stw_dev->memdbg_no);
#endif

   stw_tls_cleanup();

   stw_dev = NULL;
}


struct stw_context *
stw_lookup_context( UINT_PTR dhglrc )
{
   if (dhglrc == 0)
      return NULL;

   if (stw_dev == NULL)
      return NULL;

   return (struct stw_context *) handle_table_get(stw_dev->ctx_table, dhglrc);
}

