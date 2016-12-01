/**************************************************************************
 *
 * Copyright 2013 Marek Olšák <maraeo@gmail.com>
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
 * IN NO EVENT SHALL THE AUTHORS AND/OR ITS SUPPLIERS BE LIABLE FOR
 * ANY CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT,
 * TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN CONNECTION WITH THE
 * SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.
 *
 **************************************************************************/

/* This file contains code for calculating frametimes for displaying on the HUD.
 */

#include "hud/hud_private.h"
#include "util/os_time.h"
#include "util/u_memory.h"
#include <sys/time.h>
#include <inttypes.h>

struct frametime_info {
   uint64_t last_time;
   uint64_t last_frame_time;
   uint64_t threshold_frametime;
   uint64_t late_frames;
};

static inline uint64_t GetTimeStamp() {
   struct timeval tv;
   gettimeofday(&tv,NULL);
   return tv.tv_sec*(uint64_t)1000000+tv.tv_usec;
}

static void
query_frametime(struct hud_graph *gr)
{
   struct frametime_info *info = gr->query_data;
   uint64_t now =  GetTimeStamp();

   uint64_t this_time_micro = (now - info->last_frame_time);
   //printf("%" PRId64 " %" PRId64 " %" PRId64 " %" PRId64 "\n", now, info->threshold_frametime , this_time_micro, info->threshold_frametime - this_time_micro);

   if (this_time_micro > info->threshold_frametime + 900) //we don't care about delays smaller than 900 microseconds
   { 
      info->late_frames += (this_time_micro - info->threshold_frametime);
      //printf("late frame microseconds %" PRId64 "\n", info->late_frames);
   }

   info->last_frame_time = now;

   if (info->last_time) {
      if (info->last_time + gr->pane->period <= now) {
	 info->last_time = now;
	 hud_graph_add_value(gr, (uint64_t) info->late_frames);
	 info->late_frames = 0;
      }
   }
   else {
      info->last_time = now;
   }
}

static void
free_query_data(void *p)
{
   FREE(p);
}

void
hud_frametime_X_graph_install(struct hud_pane *pane, int fps)
{
   struct hud_graph *gr = CALLOC_STRUCT(hud_graph);
   if (!gr)
      return;
   char desc[1000];
   snprintf(desc, 1000, "frame delay for %d fps", fps);
   strcpy(gr->name, desc);
   gr->query_data = CALLOC_STRUCT(frametime_info);
   struct frametime_info *info = gr->query_data;
   info->threshold_frametime = (1./fps) * 1000 * 1000; //how many microseconds should a frame take
   info->last_frame_time = GetTimeStamp();
   if (!gr->query_data) {
      FREE(gr);
      return;
   }

   gr->query_new_value = query_frametime;

   /* Don't use free() as our callback as that messes up Gallium's
    * memory debugger.  Use simple free_query_data() wrapper.
    */
   gr->free_query_data = free_query_data;
   pane->type = PIPE_DRIVER_QUERY_TYPE_MICROSECONDS;
   
   hud_pane_add_graph(pane, gr);
}
