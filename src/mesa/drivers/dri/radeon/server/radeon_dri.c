/**
 * \file server/radeon_dri.c
 * \brief File to perform the device-specific initialization tasks typically
 * done in the X server.
 *
 * Here they are converted to run in the client (or perhaps a standalone
 * process), and to work with the frambe buffer device rather than the X
 * server infrastructure.
 */

#include <stdio.h>
#include <errno.h>

#include "miniglxP.h"
#include "dri_util.h"
#include "drm.h"

#include "radeon.h"
#include "radeon_dri.h"
#include "radeon_macros.h"
#include "radeon_reg.h"
#include "radeon_sarea.h"
#include "sarea.h"

#include <unistd.h>



/* HACK - for now, put this here... */
/* Alpha - this may need to be a variable to handle UP1x00 vs TITAN */
#if defined(__alpha__)
# define DRM_PAGE_SIZE 8192
#elif defined(__ia64__)
# define DRM_PAGE_SIZE getpagesize()
#else
# define DRM_PAGE_SIZE 4096
#endif


/**
 * \brief Wait for free FIFO entries.
 *
 * \param ctx display handle.
 * \param entries number of free entries to wait.
 *
 * It polls the free entries from the chip until it reaches the requested value
 * or a timeout (3000 tries) occurs. Aborts the program if the FIFO times out.
 */
static void RADEONWaitForFifo( struct DRIDriverContextRec *ctx,
			       int entries )
{
   unsigned char *RADEONMMIO = ctx->MMIOAddress;
   int i;

   for (i = 0; i < 3000; i++) {
      int fifo_slots =
	 INREG(RADEON_RBBM_STATUS) & RADEON_RBBM_FIFOCNT_MASK;
      if (fifo_slots >= entries) return;
   }

   /* There are recoveries possible, but I haven't seen them work
    * in practice:
    */
   fprintf(stderr, "FIFO timed out: %d entries, stat=0x%08x\n",
	   INREG(RADEON_RBBM_STATUS) & RADEON_RBBM_FIFOCNT_MASK,
	   INREG(RADEON_RBBM_STATUS));
   exit(1);
}

/**
 * \brief Read a PLL register.
 *
 * \param ctx display handle.
 * \param addr PLL register index.
 *
 * \return value of the PLL register.
 */
static unsigned int RADEONINPLL( struct DRIDriverContextRec *ctx, int addr)
{
    unsigned char *RADEONMMIO = ctx->MMIOAddress;
    unsigned int data;

    OUTREG8(RADEON_CLOCK_CNTL_INDEX, addr & 0x3f);
    data = INREG(RADEON_CLOCK_CNTL_DATA);

    return data;
}

/**
 * \brief Reset graphics card to known state.
 *
 * \param ctx display handle.
 *
 * Resets the values of several Radeon registers.
 */
static void RADEONEngineReset( struct DRIDriverContextRec *ctx )
{
   unsigned char *RADEONMMIO = ctx->MMIOAddress;
   unsigned int clock_cntl_index;
   unsigned int mclk_cntl;
   unsigned int rbbm_soft_reset;
   unsigned int host_path_cntl;
   int i;

   OUTREGP(RADEON_RB2D_DSTCACHE_CTLSTAT,
	   RADEON_RB2D_DC_FLUSH_ALL,
	   ~RADEON_RB2D_DC_FLUSH_ALL);
   for (i = 0; i < 512; i++) {
      if (!(INREG(RADEON_RB2D_DSTCACHE_CTLSTAT) & RADEON_RB2D_DC_BUSY))
	 break;
   }

   clock_cntl_index = INREG(RADEON_CLOCK_CNTL_INDEX);

   mclk_cntl = INPLL(ctx, RADEON_MCLK_CNTL);
   OUTPLL(RADEON_MCLK_CNTL, (mclk_cntl |
			     RADEON_FORCEON_MCLKA |
			     RADEON_FORCEON_MCLKB |
			     RADEON_FORCEON_YCLKA |
			     RADEON_FORCEON_YCLKB |
			     RADEON_FORCEON_MC |
			     RADEON_FORCEON_AIC));

   /* Soft resetting HDP thru RBBM_SOFT_RESET register can cause some
    * unexpected behaviour on some machines.  Here we use
    * RADEON_HOST_PATH_CNTL to reset it.
    */
   host_path_cntl = INREG(RADEON_HOST_PATH_CNTL);
   rbbm_soft_reset = INREG(RADEON_RBBM_SOFT_RESET);

   OUTREG(RADEON_RBBM_SOFT_RESET, (rbbm_soft_reset |
				   RADEON_SOFT_RESET_CP |
				   RADEON_SOFT_RESET_HI |
				   RADEON_SOFT_RESET_SE |
				   RADEON_SOFT_RESET_RE |
				   RADEON_SOFT_RESET_PP |
				   RADEON_SOFT_RESET_E2 |
				   RADEON_SOFT_RESET_RB));
   INREG(RADEON_RBBM_SOFT_RESET);
   OUTREG(RADEON_RBBM_SOFT_RESET, (rbbm_soft_reset & 
				   (unsigned int) ~(RADEON_SOFT_RESET_CP |
						    RADEON_SOFT_RESET_HI |
						    RADEON_SOFT_RESET_SE |
						    RADEON_SOFT_RESET_RE |
						    RADEON_SOFT_RESET_PP |
						    RADEON_SOFT_RESET_E2 |
						    RADEON_SOFT_RESET_RB)));
   INREG(RADEON_RBBM_SOFT_RESET);

   OUTREG(RADEON_HOST_PATH_CNTL, host_path_cntl | RADEON_HDP_SOFT_RESET);
   INREG(RADEON_HOST_PATH_CNTL);
   OUTREG(RADEON_HOST_PATH_CNTL, host_path_cntl);

   OUTREG(RADEON_RBBM_SOFT_RESET, rbbm_soft_reset);

   OUTREG(RADEON_CLOCK_CNTL_INDEX, clock_cntl_index);
   OUTPLL(RADEON_MCLK_CNTL, mclk_cntl);
}

/**
 * \brief Restore the drawing engine.
 *
 * \param ctx display handle
 *
 * Resets the graphics card and sets initial values for several registers of
 * the card's drawing engine.
 *
 * Turns on the radeon command processor engine (ie: the ringbuffer).
 */
static int RADEONEngineRestore( struct DRIDriverContextRec *ctx )
{
   RADEONInfoPtr info = ctx->driverPrivate;
   unsigned char *RADEONMMIO = ctx->MMIOAddress;
   int pitch64, datatype, dp_gui_master_cntl, err;

   fprintf(stderr, "%s\n", __FUNCTION__);

   OUTREG(RADEON_RB3D_CNTL, 0);
   RADEONEngineReset( ctx );

   switch (ctx->bpp) {
   case 16: datatype = 4; break;
   case 32: datatype = 6; break;
   default: return 0;
   }

   dp_gui_master_cntl =
      ((datatype << RADEON_GMC_DST_DATATYPE_SHIFT)
       | RADEON_GMC_CLR_CMP_CNTL_DIS);

   pitch64 = ((ctx->shared.virtualWidth * (ctx->bpp / 8) + 0x3f)) >> 6;

   RADEONWaitForFifo(ctx, 1);
   OUTREG(RADEON_DEFAULT_OFFSET, ((INREG(RADEON_DEFAULT_OFFSET) & 0xC0000000)
				  | (pitch64 << 22)));

   RADEONWaitForFifo(ctx, 1);
   OUTREG(RADEON_SURFACE_CNTL, RADEON_SURF_TRANSLATION_DIS); 

   RADEONWaitForFifo(ctx, 1);
   OUTREG(RADEON_DEFAULT_SC_BOTTOM_RIGHT, (RADEON_DEFAULT_SC_RIGHT_MAX
					   | RADEON_DEFAULT_SC_BOTTOM_MAX));

   RADEONWaitForFifo(ctx, 1);
   OUTREG(RADEON_DP_GUI_MASTER_CNTL, (dp_gui_master_cntl
				      | RADEON_GMC_BRUSH_SOLID_COLOR
				      | RADEON_GMC_SRC_DATATYPE_COLOR));

   RADEONWaitForFifo(ctx, 7);
   OUTREG(RADEON_DST_LINE_START,    0);
   OUTREG(RADEON_DST_LINE_END,      0);
   OUTREG(RADEON_DP_BRUSH_FRGD_CLR, 0xffffffff);
   OUTREG(RADEON_DP_BRUSH_BKGD_CLR, 0);
   OUTREG(RADEON_DP_SRC_FRGD_CLR,   0xffffffff);
   OUTREG(RADEON_DP_SRC_BKGD_CLR,   0);
   OUTREG(RADEON_DP_WRITE_MASK,     0xffffffff);
   OUTREG(RADEON_AUX_SC_CNTL,       0);

/*    RADEONWaitForIdleMMIO(ctx); */
   usleep(100); 


   OUTREG(RADEON_GEN_INT_CNTL, info->gen_int_cntl);
   OUTREG(RADEON_CRTC_OFFSET_CNTL, info->crtc_offset_cntl);


   /* Initialize and start the CP if required */
   if ((err = drmCommandNone(ctx->drmFD, DRM_RADEON_CP_START)) != 0) {
      fprintf(stderr, "%s: CP start %d\n", __FUNCTION__, err);
      return 0;
   }

   return True;
}


/**
 * \brief Shutdown the drawing engine.
 *
 * \param ctx display handle
 *
 * Turns off the command processor engine & restores the graphics card
 * to a state that fbdev understands.
 */
static int RADEONEngineShutdown( struct DRIDriverContextRec *ctx )
{
   drmRadeonCPStop  stop;
   int              ret, i;

   stop.flush = 1;
   stop.idle  = 1;

   ret = drmCommandWrite(ctx->drmFD, DRM_RADEON_CP_STOP, &stop, 
			 sizeof(drmRadeonCPStop));

   if (ret == 0) {
      return 0;
   } else if (errno != EBUSY) {
      return -errno;
   }

   stop.flush = 0;
 
   i = 0;
   do {
      ret = drmCommandWrite(ctx->drmFD, DRM_RADEON_CP_STOP, &stop, 
			    sizeof(drmRadeonCPStop));
   } while (ret && errno == EBUSY && i++ < 10);

   if (ret == 0) {
      return 0;
   } else if (errno != EBUSY) {
      return -errno;
   }

   stop.idle = 0;

   if (drmCommandWrite(ctx->drmFD, DRM_RADEON_CP_STOP,
		       &stop, sizeof(drmRadeonCPStop))) {
      return -errno;
   } else {
      return 0;
   }
}

/**
 * \brief Compute base 2 logarithm.
 *
 * \param val value.
 * 
 * \return base 2 logarithm of \p val.
 */
static int RADEONMinBits(int val)
{
   int  bits;

   if (!val) return 1;
   for (bits = 0; val; val >>= 1, ++bits);
   return bits;
}

/**
 * \brief Initialize the AGP state
 *
 * \param ctx display handle.
 * \param info driver private data.
 *
 * \return one on sucess, or zero on failure.
 * 
 * Acquires and enables the AGP device. Reserves memory in the AGP space for
 * the ring buffer, vertex buffers and textures. Initialize the Radeon
 * registers to point to that memory and add client mappings.
 */
static int RADEONDRIAgpInit( struct DRIDriverContextRec *ctx, RADEONInfoPtr info)
{
   unsigned char *RADEONMMIO = ctx->MMIOAddress;
   unsigned long  mode;
   int            ret;
   int            s, l;

   if (drmAgpAcquire(ctx->drmFD) < 0) {
      fprintf(stderr, "[agp] AGP not available\n");
      return 0;
   }
    
   /* Workaround for some hardware bugs */
   if (info->ChipFamily < CHIP_FAMILY_R200)
      OUTREG(RADEON_AGP_CNTL, INREG(RADEON_AGP_CNTL) | 0x000e0020);

   /* Modify the mode if the default mode is not appropriate for this
    * particular combination of graphics card and AGP chipset.
    */
   mode   = drmAgpGetMode(ctx->drmFD);	/* Default mode */

   /* Disable fast write entirely - too many lockups.
    */
   mode &= ~RADEON_AGP_MODE_MASK;
   switch (info->agpMode) {
   case 4:          mode |= RADEON_AGP_4X_MODE;
   case 2:          mode |= RADEON_AGP_2X_MODE;
   case 1: default: mode |= RADEON_AGP_1X_MODE;
   }

   if (drmAgpEnable(ctx->drmFD, mode) < 0) {
      fprintf(stderr, "[agp] AGP not enabled\n");
      drmAgpRelease(ctx->drmFD);
      return 0;
   }
    
   info->agpOffset = 0;

   if ((ret = drmAgpAlloc(ctx->drmFD, info->agpSize*1024*1024, 0, NULL,
			  &info->agpMemHandle)) < 0) {
      fprintf(stderr, "[agp] Out of memory (%d)\n", ret);
      drmAgpRelease(ctx->drmFD);
      return 0;
   }
   fprintf(stderr,
	   "[agp] %d kB allocated with handle 0x%08x\n",
	   info->agpSize*1024, (unsigned)info->agpMemHandle);
    
   if (drmAgpBind(ctx->drmFD,
		  info->agpMemHandle, info->agpOffset) < 0) {
      fprintf(stderr, "[agp] Could not bind\n");
      drmAgpFree(ctx->drmFD, info->agpMemHandle);
      drmAgpRelease(ctx->drmFD);
      return 0;
   }

   /* Initialize the CP ring buffer data */
   info->ringStart       = info->agpOffset;
   info->ringMapSize     = info->ringSize*1024*1024 + DRM_PAGE_SIZE;

   info->ringReadOffset  = info->ringStart + info->ringMapSize;
   info->ringReadMapSize = DRM_PAGE_SIZE;

   /* Reserve space for vertex/indirect buffers */
   info->bufStart        = info->ringReadOffset + info->ringReadMapSize;
   info->bufMapSize      = info->bufSize*1024*1024;

   /* Reserve the rest for AGP textures */
   info->agpTexStart     = info->bufStart + info->bufMapSize;
   s = (info->agpSize*1024*1024 - info->agpTexStart);
   l = RADEONMinBits((s-1) / RADEON_NR_TEX_REGIONS);
   if (l < RADEON_LOG_TEX_GRANULARITY) l = RADEON_LOG_TEX_GRANULARITY;
   info->agpTexMapSize   = (s >> l) << l;
   info->log2AGPTexGran  = l;

   if (drmAddMap(ctx->drmFD, info->ringStart, info->ringMapSize,
		 DRM_AGP, DRM_READ_ONLY, &info->ringHandle) < 0) {
      fprintf(stderr, "[agp] Could not add ring mapping\n");
      return 0;
   }
   fprintf(stderr, "[agp] ring handle = 0x%08lx\n", info->ringHandle);
    

   if (drmAddMap(ctx->drmFD, info->ringReadOffset, info->ringReadMapSize,
		 DRM_AGP, DRM_READ_ONLY, &info->ringReadPtrHandle) < 0) {
      fprintf(stderr,
	      "[agp] Could not add ring read ptr mapping\n");
      return 0;
   }
    
   fprintf(stderr,
	   "[agp] ring read ptr handle = 0x%08lx\n",
	   info->ringReadPtrHandle);
    
   if (drmAddMap(ctx->drmFD, info->bufStart, info->bufMapSize,
		 DRM_AGP, 0, &info->bufHandle) < 0) {
      fprintf(stderr,
	      "[agp] Could not add vertex/indirect buffers mapping\n");
      return 0;
   }
   fprintf(stderr,
	   "[agp] vertex/indirect buffers handle = 0x%08lx\n",
	   info->bufHandle);

   if (drmAddMap(ctx->drmFD, info->agpTexStart, info->agpTexMapSize,
		 DRM_AGP, 0, &info->agpTexHandle) < 0) {
      fprintf(stderr,
	      "[agp] Could not add AGP texture map mapping\n");
      return 0;
   }
   fprintf(stderr,
	   "[agp] AGP texture map handle = 0x%08lx\n",
	   info->agpTexHandle);

   /* Initialize Radeon's AGP registers */
   /* Ring buffer is at AGP offset 0 */
   OUTREG(RADEON_AGP_BASE, info->ringHandle);

   return 1;
}


/**
 * \brief Initialize the kernel data structures and enable the CP engine.
 *
 * \param ctx display handle.
 * \param info driver private data.
 *
 * \return non-zero on sucess, or zero on failure.
 *
 * This function is a wrapper around the DRM_RADEON_CP_INIT command, passing
 * all the parameters in a drmRadeonInit structure.
 */
static int RADEONDRIKernelInit(struct DRIDriverContextRec *ctx,
			       RADEONInfoPtr info)
{
   int cpp = ctx->bpp / 8;
   drmRadeonInit  drmInfo;
   int ret;

   memset(&drmInfo, 0, sizeof(drmRadeonInit));

   if ( (info->ChipFamily == CHIP_FAMILY_R200) ||
	(info->ChipFamily == CHIP_FAMILY_RV250) ||
	(info->ChipFamily == CHIP_FAMILY_M9) )
      drmInfo.func             = DRM_RADEON_INIT_R200_CP;
   else
      drmInfo.func             = DRM_RADEON_INIT_CP;

   /* This is the struct passed to the kernel module for its initialization */
   drmInfo.sarea_priv_offset   = sizeof(XF86DRISAREARec);
   drmInfo.is_pci              = 0;
   drmInfo.cp_mode             = RADEON_DEFAULT_CP_BM_MODE;
   drmInfo.agp_size            = info->agpSize*1024*1024;
   drmInfo.ring_size           = info->ringSize*1024*1024;
   drmInfo.usec_timeout        = 1000;
   drmInfo.fb_bpp              = ctx->bpp;
   drmInfo.depth_bpp           = ctx->bpp;
   drmInfo.front_offset        = info->frontOffset;
   drmInfo.front_pitch         = info->frontPitch * cpp;
   drmInfo.back_offset         = info->backOffset;
   drmInfo.back_pitch          = info->backPitch * cpp;
   drmInfo.depth_offset        = info->depthOffset;
   drmInfo.depth_pitch         = info->depthPitch * cpp;
   drmInfo.fb_offset           = info->LinearAddr;
   drmInfo.mmio_offset         = info->registerHandle;
   drmInfo.ring_offset         = info->ringHandle;
   drmInfo.ring_rptr_offset    = info->ringReadPtrHandle;
   drmInfo.buffers_offset      = info->bufHandle;
   drmInfo.agp_textures_offset = info->agpTexHandle;

   ret = drmCommandWrite(ctx->drmFD, DRM_RADEON_CP_INIT, &drmInfo, 
			 sizeof(drmRadeonInit));

   return ret >= 0;
}


/**
 * \brief Initialize the AGP heap.
 *
 * \param ctx display handle.
 * \param info driver private data.
 *
 * This function is a wrapper around the DRM_RADEON_INIT_HEAP command, passing
 * all the parameters in a drm_radeon_mem_init_heap structure.
 */
static void RADEONDRIAgpHeapInit(struct DRIDriverContextRec *ctx,
				 RADEONInfoPtr info)
{
   drmRadeonMemInitHeap drmHeap;

   /* Start up the simple memory manager for agp space */
   drmHeap.region = RADEON_MEM_REGION_AGP;
   drmHeap.start  = 0;
   drmHeap.size   = info->agpTexMapSize;
    
   if (drmCommandWrite(ctx->drmFD, DRM_RADEON_INIT_HEAP,
		       &drmHeap, sizeof(drmHeap))) {
      fprintf(stderr,
	      "[drm] Failed to initialized agp heap manager\n");
   } else {
      fprintf(stderr,
	      "[drm] Initialized kernel agp heap manager, %d\n",
	      info->agpTexMapSize);
   }
}

/**
 * \brief Add a map for the vertex buffers that will be accessed by any
 * DRI-based clients.
 * 
 * \param ctx display handle.
 * \param info driver private data.
 *
 * \return one on sucess, or zero on failure.
 *
 * Calls drmAddBufs() with the previously allocated vertex buffers.
 */
static int RADEONDRIBufInit( struct DRIDriverContextRec *ctx, RADEONInfoPtr info )
{
   /* Initialize vertex buffers */
   info->bufNumBufs = drmAddBufs(ctx->drmFD,
				 info->bufMapSize / RADEON_BUFFER_SIZE,
				 RADEON_BUFFER_SIZE,
				 DRM_AGP_BUFFER,
				 info->bufStart);

   if (info->bufNumBufs <= 0) {
      fprintf(stderr,
	      "[drm] Could not create vertex/indirect buffers list\n");
      return 0;
   }
   fprintf(stderr,
	   "[drm] Added %d %d byte vertex/indirect buffers\n",
	   info->bufNumBufs, RADEON_BUFFER_SIZE);
   
   return 1;
}

/**
 * \brief Install an IRQ handler.
 * 
 * \param ctx display handle.
 * \param info driver private data.
 *
 * Attemps to install an IRQ handler via drmCtlInstHandler(), falling back to
 * IRQ-free operation on failure.
 */
static void RADEONDRIIrqInit(struct DRIDriverContextRec *ctx,
			     RADEONInfoPtr info)
{
   if (!info->irq) {
      info->irq = drmGetInterruptFromBusID(ctx->drmFD,
					   ctx->pciBus,
					   ctx->pciDevice,
					   ctx->pciFunc);

      if ((drmCtlInstHandler(ctx->drmFD, info->irq)) != 0) {
	 fprintf(stderr,
		 "[drm] failure adding irq handler, "
		 "there is a device already using that irq\n"
		 "[drm] falling back to irq-free operation\n");
	 info->irq = 0;
      }
   }

   if (info->irq)
      fprintf(stderr,
	      "[drm] dma control initialized, using IRQ %d\n",
	      info->irq);
}

static int RADEONCheckDRMVersion( struct DRIDriverContextRec *ctx,
				  RADEONInfoPtr info )
{
   drmVersionPtr  version;

   version = drmGetVersion(ctx->drmFD);
   if (version) {
      int req_minor, req_patch;

      /* Need 1.8.x for proper cleanup-on-client-exit behaviour.
       */
      req_minor = 8;
      req_patch = 0;	

      if (version->version_major != 1 ||
	  version->version_minor < req_minor ||
	  (version->version_minor == req_minor && 
	   version->version_patchlevel < req_patch)) {
	 /* Incompatible drm version */
	 fprintf(stderr,
		 "[dri] RADEONDRIScreenInit failed because of a version "
		 "mismatch.\n"
		 "[dri] radeon.o kernel module version is %d.%d.%d "
		 "but version 1.%d.%d or newer is needed.\n"
		 "[dri] Disabling DRI.\n",
		 version->version_major,
		 version->version_minor,
		 version->version_patchlevel,
		 req_minor,
		 req_patch);
	 drmFreeVersion(version);
	 return 0;
      }

      info->drmMinor = version->version_minor;
      drmFreeVersion(version);
   }

   return 1;
}

static int RADEONMemoryInit( struct DRIDriverContextRec *ctx, RADEONInfoPtr info )
{
   int        width_bytes = ctx->shared.virtualWidth * ctx->cpp;
   int        cpp         = ctx->cpp;
   int        bufferSize  = ((ctx->shared.virtualHeight * width_bytes
			      + RADEON_BUFFER_ALIGN)
			     & ~RADEON_BUFFER_ALIGN);
   int        depthSize   = ((((ctx->shared.virtualHeight+15) & ~15) * width_bytes
			      + RADEON_BUFFER_ALIGN)
			     & ~RADEON_BUFFER_ALIGN);
   int        l;

   info->frontOffset = 0;
   info->frontPitch = ctx->shared.virtualWidth;

   fprintf(stderr, 
	   "Using %d MB AGP aperture\n", info->agpSize);
   fprintf(stderr, 
	   "Using %d MB for the ring buffer\n", info->ringSize);
   fprintf(stderr, 
	   "Using %d MB for vertex/indirect buffers\n", info->bufSize);
   fprintf(stderr, 
	   "Using %d MB for AGP textures\n", info->agpTexSize);

   /* Front, back and depth buffers - everything else texture??
    */
   info->textureSize = ctx->shared.fbSize - 2 * bufferSize - depthSize;

   if (info->textureSize < 0) 
      return 0;

   l = RADEONMinBits((info->textureSize-1) / RADEON_NR_TEX_REGIONS);
   if (l < RADEON_LOG_TEX_GRANULARITY) l = RADEON_LOG_TEX_GRANULARITY;

   /* Round the texture size up to the nearest whole number of
    * texture regions.  Again, be greedy about this, don't
    * round down.
    */
   info->log2TexGran = l;
   info->textureSize = (info->textureSize >> l) << l;

   /* Set a minimum usable local texture heap size.  This will fit
    * two 256x256x32bpp textures.
    */
   if (info->textureSize < 512 * 1024) {
      info->textureOffset = 0;
      info->textureSize = 0;
   }

   /* Reserve space for textures */
   info->textureOffset = ((ctx->shared.fbSize - info->textureSize +
			   RADEON_BUFFER_ALIGN) &
			  ~RADEON_BUFFER_ALIGN);

   /* Reserve space for the shared depth
    * buffer.
    */
   info->depthOffset = ((info->textureOffset - depthSize +
			 RADEON_BUFFER_ALIGN) &
			~RADEON_BUFFER_ALIGN);
   info->depthPitch = ctx->shared.virtualWidth;

   info->backOffset = ((info->depthOffset - bufferSize +
			RADEON_BUFFER_ALIGN) &
		       ~RADEON_BUFFER_ALIGN);
   info->backPitch = ctx->shared.virtualWidth;


   fprintf(stderr, 
	   "Will use back buffer at offset 0x%x\n",
	   info->backOffset);
   fprintf(stderr, 
	   "Will use depth buffer at offset 0x%x\n",
	   info->depthOffset);
   fprintf(stderr, 
	   "Will use %d kb for textures at offset 0x%x\n",
	   info->textureSize/1024, info->textureOffset);

   info->frontPitchOffset = (((info->frontPitch * cpp / 64) << 22) |
			     (info->frontOffset >> 10));

   info->backPitchOffset = (((info->backPitch * cpp / 64) << 22) |
			    (info->backOffset >> 10));

   info->depthPitchOffset = (((info->depthPitch * cpp / 64) << 22) |
			     (info->depthOffset >> 10));

   return 1;
} 



/**
 * Called at the start of each server generation.
 *
 * \param ctx display handle.
 * \param info driver private data.
 *
 * \return non-zero on sucess, or zero on failure.
 *
 * Performs static frame buffer allocation. Opens the DRM device and add maps
 * to the SAREA, framebuffer and MMIO regions. Fills in \p info with more
 * information. Creates a \e server context to grab the lock for the
 * initialization ioctls and calls the other initliaztion functions in this
 * file. Starts the CP engine via the DRM_RADEON_CP_START command.
 *
 * Setups a RADEONDRIRec structure to be passed to radeon_dri.so for its
 * initialization.
 */
static int RADEONScreenInit( struct DRIDriverContextRec *ctx, RADEONInfoPtr info )
{
   RADEONDRIPtr   pRADEONDRI;
   int err;

   usleep(100);
   //assert(!ctx->IsClient);

   {
      int  width_bytes = (ctx->shared.virtualWidth * ctx->cpp);
      int  maxy        = ctx->shared.fbSize / width_bytes;


      if (maxy <= ctx->shared.virtualHeight * 3) {
	 fprintf(stderr, 
		 "Static buffer allocation failed -- "
		 "need at least %d kB video memory (have %d kB)\n",
		 (ctx->shared.virtualWidth * ctx->shared.virtualHeight *
		  ctx->cpp * 3 + 1023) / 1024,
		 ctx->shared.fbSize / 1024);
	 return 0;
      } 
   }


   if (info->ChipFamily >= CHIP_FAMILY_R300) {
      fprintf(stderr, 
	      "Direct rendering not yet supported on "
	      "Radeon 9700 and newer cards\n");
      return 0;
   }

   info->registerSize = ctx->MMIOSize;
   ctx->shared.SAREASize = DRM_PAGE_SIZE;

   /* Note that drmOpen will try to load the kernel module, if needed. */
   ctx->drmFD = drmOpen("radeon", NULL );
   if (ctx->drmFD < 0) {
      fprintf(stderr, "[drm] drmOpen failed\n");
      return 0;
   }

   if ((err = drmSetBusid(ctx->drmFD, ctx->pciBusID)) < 0) {
      fprintf(stderr, "[drm] drmSetBusid failed (%d, %s), %s\n",
	      ctx->drmFD, ctx->pciBusID, strerror(-err));
      return 0;
   }

   if (drmAddMap( ctx->drmFD,
		  0,
		  ctx->shared.SAREASize,
		  DRM_SHM,
		  DRM_CONTAINS_LOCK,
		  &ctx->shared.hSAREA) < 0)
   {
      fprintf(stderr, "[drm] drmAddMap failed\n");
      return 0;
   }
   fprintf(stderr, "[drm] added %d byte SAREA at 0x%08lx\n",
	   ctx->shared.SAREASize, ctx->shared.hSAREA);

   if (drmMap( ctx->drmFD,
	       ctx->shared.hSAREA,
	       ctx->shared.SAREASize,
	       (drmAddressPtr)(&ctx->pSAREA)) < 0)
   {
      fprintf(stderr, "[drm] drmMap failed\n");
      return 0;
   }
   memset(ctx->pSAREA, 0, ctx->shared.SAREASize);
   fprintf(stderr, "[drm] mapped SAREA 0x%08lx to %p, size %d\n",
	   ctx->shared.hSAREA, ctx->pSAREA, ctx->shared.SAREASize);
   
   /* Need to AddMap the framebuffer and mmio regions here:
    */
   if (drmAddMap( ctx->drmFD,
		  (drmHandle)ctx->FBStart,
		  ctx->FBSize,
		  DRM_FRAME_BUFFER,
#if _HAVE_FULL_GL
		  0,
#else
		  DRM_READ_ONLY,
#endif
		  &ctx->shared.hFrameBuffer) < 0)
   {
      fprintf(stderr, "[drm] drmAddMap framebuffer failed\n");
      return 0;
   }

   fprintf(stderr, "[drm] framebuffer handle = 0x%08lx\n",
	   ctx->shared.hFrameBuffer);



   if (drmAddMap(ctx->drmFD, 
		 ctx->MMIOStart,
		 ctx->MMIOSize,
		 DRM_REGISTERS, 
		 DRM_READ_ONLY, 
		 &info->registerHandle) < 0) {
      fprintf(stderr, "[drm] drmAddMap mmio failed\n");	
      return 0;
   }
   fprintf(stderr,
	   "[drm] register handle = 0x%08lx\n", info->registerHandle);

   /* Check the radeon DRM version */
   if (!RADEONCheckDRMVersion(ctx, info)) {
      return 0;
   }

   /* Initialize AGP */
   if (!RADEONDRIAgpInit(ctx, info)) {
      return 0;
   }


   /* Memory manager setup */
   if (!RADEONMemoryInit(ctx, info)) {
      return 0;
   }

   /* Create a 'server' context so we can grab the lock for
    * initialization ioctls.
    */
   if ((err = drmCreateContext(ctx->drmFD, &ctx->serverContext)) != 0) {
      fprintf(stderr, "%s: drmCreateContext failed %d\n", __FUNCTION__, err);
      return 0;
   }

   DRM_LOCK(ctx->drmFD, ctx->pSAREA, ctx->serverContext, 0); 

   /* Initialize the kernel data structures */
   if (!RADEONDRIKernelInit(ctx, info)) {
      fprintf(stderr, "RADEONDRIKernelInit failed\n");
      DRM_UNLOCK(ctx->drmFD, ctx->pSAREA, ctx->serverContext);
      return 0;
   }

   /* Initialize the vertex buffers list */
   if (!RADEONDRIBufInit(ctx, info)) {
      fprintf(stderr, "RADEONDRIBufInit failed\n");
      DRM_UNLOCK(ctx->drmFD, ctx->pSAREA, ctx->serverContext);
      return 0;
   }

   /* Initialize IRQ */
   RADEONDRIIrqInit(ctx, info);

   /* Initialize kernel agp memory manager */
   RADEONDRIAgpHeapInit(ctx, info);

   /* Initialize the SAREA private data structure */
   {
      RADEONSAREAPrivPtr pSAREAPriv;
      pSAREAPriv = (RADEONSAREAPrivPtr)(((char*)ctx->pSAREA) + 
					sizeof(XF86DRISAREARec));
      memset(pSAREAPriv, 0, sizeof(*pSAREAPriv));
      pSAREAPriv->pfAllowPageFlip = 1;
   }


   /* Quick hack to clear the front & back buffers.  Could also use
    * the clear ioctl to do this, but would need to setup hw state
    * first.
    */
   memset((char *)ctx->FBAddress + info->frontOffset,
	  0,
	  info->frontPitch * ctx->cpp * ctx->shared.virtualHeight );

   memset((char *)ctx->FBAddress + info->backOffset,
	  0,
	  info->backPitch * ctx->cpp * ctx->shared.virtualHeight );


   /* This is the struct passed to radeon_dri.so for its initialization */
   ctx->driverClientMsg = malloc(sizeof(RADEONDRIRec));
   ctx->driverClientMsgSize = sizeof(RADEONDRIRec);
   pRADEONDRI                    = (RADEONDRIPtr)ctx->driverClientMsg;
   pRADEONDRI->deviceID          = info->Chipset;
   pRADEONDRI->width             = ctx->shared.virtualWidth;
   pRADEONDRI->height            = ctx->shared.virtualHeight;
   pRADEONDRI->depth             = ctx->bpp; /* XXX: depth */
   pRADEONDRI->bpp               = ctx->bpp;
   pRADEONDRI->IsPCI             = 0;
   pRADEONDRI->AGPMode           = info->agpMode;
   pRADEONDRI->frontOffset       = info->frontOffset;
   pRADEONDRI->frontPitch        = info->frontPitch;
   pRADEONDRI->backOffset        = info->backOffset;
   pRADEONDRI->backPitch         = info->backPitch;
   pRADEONDRI->depthOffset       = info->depthOffset;
   pRADEONDRI->depthPitch        = info->depthPitch;
   pRADEONDRI->textureOffset     = info->textureOffset;
   pRADEONDRI->textureSize       = info->textureSize;
   pRADEONDRI->log2TexGran       = info->log2TexGran;
   pRADEONDRI->registerHandle    = info->registerHandle;
   pRADEONDRI->registerSize      = info->registerSize; 
   pRADEONDRI->statusHandle      = info->ringReadPtrHandle;
   pRADEONDRI->statusSize        = info->ringReadMapSize;
   pRADEONDRI->agpTexHandle      = info->agpTexHandle;
   pRADEONDRI->agpTexMapSize     = info->agpTexMapSize;
   pRADEONDRI->log2AGPTexGran    = info->log2AGPTexGran;
   pRADEONDRI->agpTexOffset      = info->agpTexStart;
   pRADEONDRI->sarea_priv_offset = sizeof(XF86DRISAREARec);

   /* Don't release the lock now - let the VT switch handler do it. */

   return 1;
}


/**
 * \brief Get Radeon chip family from chipset number.
 * 
 * \param driver private data.
 *
 * \return non-zero on sucess, or zero on failure.
 *
 * Called by radeonInitFBDev() to set RADEONInfoRec::ChipFamily
 * according to the value of RADEONInfoRec::Chipset.  Fails if the
 * chipset is unrecognized or not appropriate for this driver (ie. not
 * an r100 style radeon)
 */
static int get_chipfamily_from_chipset( RADEONInfoPtr info )
{
    switch (info->Chipset) {
    case PCI_CHIP_RADEON_LY:
    case PCI_CHIP_RADEON_LZ:
	info->ChipFamily = CHIP_FAMILY_M6;
	break;

    case PCI_CHIP_RADEON_QY:
    case PCI_CHIP_RADEON_QZ:
	info->ChipFamily = CHIP_FAMILY_VE;
	break;

    case PCI_CHIP_R200_QL:
    case PCI_CHIP_R200_QN:
    case PCI_CHIP_R200_QO:
    case PCI_CHIP_R200_Ql:
    case PCI_CHIP_R200_BB:
	info->ChipFamily = CHIP_FAMILY_R200;
	break;

    case PCI_CHIP_RV200_QW: /* RV200 desktop */
    case PCI_CHIP_RV200_QX:
	info->ChipFamily = CHIP_FAMILY_RV200;
	break;

    case PCI_CHIP_RADEON_LW:
    case PCI_CHIP_RADEON_LX:
	info->ChipFamily = CHIP_FAMILY_M7;
	break;

    case PCI_CHIP_RV250_Id:
    case PCI_CHIP_RV250_Ie:
    case PCI_CHIP_RV250_If:
    case PCI_CHIP_RV250_Ig:
	info->ChipFamily = CHIP_FAMILY_RV250;
	break;

    case PCI_CHIP_RV250_Ld:
    case PCI_CHIP_RV250_Le:
    case PCI_CHIP_RV250_Lf:
    case PCI_CHIP_RV250_Lg:
	info->ChipFamily = CHIP_FAMILY_M9;
	break;

    case PCI_CHIP_R300_ND:
    case PCI_CHIP_R300_NE:
    case PCI_CHIP_R300_NF:
    case PCI_CHIP_R300_NG:
	info->ChipFamily = CHIP_FAMILY_R300;
        break;

    default:
	/* Original Radeon/7200 */
	info->ChipFamily = CHIP_FAMILY_RADEON;
    }

    return 1;
}


/**
 * \brief Establish the set of visuals available for the display.
 *
 * \param ctx display handle.
 * \param numConfigs will receive the number of supported visuals.
 * \param configs will point to the list of supported visuals.
 *
 * \return one on sucess, or zero on failure.
 * 
 * \note Requires the ::__GLXvisualConfig data type.
 *
 * Allocates a single visual and fills it with information according to the
 * display bit depth. Supports only 16 and 32 bpp bit depths, aborting
 * otherwise.
 */
static int radeonInitScreenConfigs( struct DRIDriverContextRec *ctx,
				   int *numConfigs, __GLXvisualConfig **configs)
{
   int i;

   *numConfigs = 1;
   *configs = (__GLXvisualConfig *) calloc(*numConfigs, 
					   sizeof(__GLXvisualConfig));

   switch (ctx->bpp) {
   case 32:
      for (i = 0; i < *numConfigs; i++) {
	 (*configs)[i].vid = 100 + i;
	 (*configs)[i].class = TrueColor;
	 (*configs)[i].rgba = True;
	 (*configs)[i].redSize = 8;
	 (*configs)[i].greenSize = 8;
	 (*configs)[i].blueSize = 8;
	 (*configs)[i].alphaSize = 8;
	 (*configs)[i].redMask = 0xff0000;
	 (*configs)[i].greenMask = 0xff00;
	 (*configs)[i].blueMask = 0xff;
	 (*configs)[i].alphaMask = 0xff000000;
	 (*configs)[i].doubleBuffer = True;
	 (*configs)[i].stereo = False;
	 (*configs)[i].bufferSize = 32;
	 (*configs)[i].depthSize = 24;
	 (*configs)[i].stencilSize = 8;
	 (*configs)[i].auxBuffers = 0;
	 (*configs)[i].level = 0;
	 /* leave remaining fields zero */
      }
      break;

   case 16:
      for (i = 0; i < *numConfigs; i++) {
	 (*configs)[i].vid = 100 + i;
	 (*configs)[i].class = TrueColor;
	 (*configs)[i].rgba = True;
	 (*configs)[i].redSize = 5;
	 (*configs)[i].greenSize = 6;
	 (*configs)[i].blueSize = 5;
	 (*configs)[i].alphaSize = 0;
	 (*configs)[i].redMask = 0xf800;
	 (*configs)[i].greenMask = 0x07e0;
	 (*configs)[i].blueMask = 0x001f;
	 (*configs)[i].alphaMask = 0x0000;
	 (*configs)[i].doubleBuffer = True;
	 (*configs)[i].stereo = False;
	 (*configs)[i].bufferSize = 16;
	 (*configs)[i].depthSize = 16;
	 (*configs)[i].stencilSize = 0;
	 (*configs)[i].auxBuffers = 0;
	 (*configs)[i].level = 0;
	 /* leave remaining fields zero */
      }
      break;

   default:
      fprintf(stderr, "Unknown bpp in %s: %d\n", __FUNCTION__, 
	      ctx->bpp);
      exit(1);
      break;

   }   
   return 1;
}


/**
 * \brief Validate the fbdev mode.
 * 
 * \param ctx display handle.
 *
 * \return one on success, or zero on failure.
 *
 * Saves some registers and returns 1.
 *
 * \sa radeonValidateMode().
 */
static int radeonValidateMode( struct DRIDriverContextRec *ctx )
{
   unsigned char *RADEONMMIO = ctx->MMIOAddress;
   RADEONInfoPtr info = ctx->driverPrivate;

   info->gen_int_cntl = INREG(RADEON_GEN_INT_CNTL);
   info->crtc_offset_cntl = INREG(RADEON_CRTC_OFFSET_CNTL);

   return 1;
}


/**
 * \brief Examine mode returned by fbdev.
 * 
 * \param ctx display handle.
 *
 * \return one on success, or zero on failure.
 *
 * Restores registers that fbdev has clobbered and returns 1.
 *
 * \sa radeonValidateMode().
 */
static int radeonPostValidateMode( struct DRIDriverContextRec *ctx )
{
   unsigned char *RADEONMMIO = ctx->MMIOAddress;
   RADEONInfoPtr info = ctx->driverPrivate;

   OUTREG(RADEON_GEN_INT_CNTL, info->gen_int_cntl);
   OUTREG(RADEON_CRTC_OFFSET_CNTL, info->crtc_offset_cntl);

   return 1;
}


/**
 * \brief Initialize the framebuffer device mode
 *
 * \param ctx display handle.
 *
 * \return one on success, or zero on failure.
 *
 * Fills in \p info with some default values and some information from \p ctx
 * and then calls RADEONScreenInit() for the screen initialization.
 * 
 * Before exiting clears the framebuffer memomry accessing it directly.
 */
static int radeonInitFBDev( struct DRIDriverContextRec *ctx )
{
   RADEONInfoPtr info = calloc(1, sizeof(*info));

   {
      int  dummy = ctx->shared.virtualWidth;

      switch (ctx->bpp / 8) {
      case 1: dummy = (ctx->shared.virtualWidth + 127) & ~127; break;
      case 2: dummy = (ctx->shared.virtualWidth +  31) &  ~31; break;
      case 3:
      case 4: dummy = (ctx->shared.virtualWidth +  15) &  ~15; break;
      }

      ctx->shared.virtualWidth = dummy;
   }

   ctx->driverPrivate = (void *)info;
   
   info->agpFastWrite  = RADEON_DEFAULT_AGP_FAST_WRITE;
   info->agpMode       = RADEON_DEFAULT_AGP_MODE;
   info->agpSize       = RADEON_DEFAULT_AGP_SIZE;
   info->agpTexSize    = RADEON_DEFAULT_AGP_TEX_SIZE;
   info->bufSize       = RADEON_DEFAULT_BUFFER_SIZE;
   info->ringSize      = RADEON_DEFAULT_RING_SIZE;
  
   info->Chipset = ctx->chipset;

   if (!get_chipfamily_from_chipset( info )) {
      fprintf(stderr, "Unknown or non-radeon chipset -- cannot continue\n");
      fprintf(stderr, "==> Verify PCI BusID is correct in miniglx.conf\n");
      return 0;
   }

   info->frontPitch = ctx->shared.virtualWidth;
   info->LinearAddr = ctx->FBStart & 0xfc000000;
    

   if (!RADEONScreenInit( ctx, info ))
      return 0;


   return 1;
}


/**
 * \brief The screen is being closed, so clean up any state and free any
 * resources used by the DRI.
 *
 * \param ctx display handle.
 *
 * Unmaps the SAREA, closes the DRM device file descriptor and frees the driver
 * private data.
 */
static void radeonHaltFBDev( struct DRIDriverContextRec *ctx )
{
    drmUnmap( ctx->pSAREA, ctx->shared.SAREASize );
    drmClose(ctx->drmFD);

    if (ctx->driverPrivate) {
       free(ctx->driverPrivate);
       ctx->driverPrivate = 0;
    }

    if (ctx->driverClientMsg) {
       free(ctx->driverClientMsg);
       ctx->driverClientMsg = NULL;
    }
}


extern void radeonNotifyFocus( int );

/**
 * \brief Exported driver interface for Mini GLX.
 *
 * \sa DRIDriverRec.
 */
struct DRIDriverRec __driDriver = {
   radeonInitScreenConfigs,
   radeonValidateMode,
   radeonPostValidateMode,
   radeonInitFBDev,
   radeonHaltFBDev,
   RADEONEngineShutdown,
   RADEONEngineRestore,  
   0,
#if _HAVE_FULL_GL
   0,
#else
   radeonNotifyFocus, 
#endif
};
