/**
 * \file dri_util.c
 * \brief DRI utility functions.
 *
 * This module acts as glue between GLX and the actual hardware driver.  A DRI
 * driver doesn't really \e have to use any of this - it's optional.  But, some
 * useful stuff is done here that otherwise would have to be duplicated in most
 * drivers.
 * 
 * Basically, these utility functions take care of some of the dirty details of
 * screen initialization, context creation, context binding, DRM setup, etc.
 *
 * These functions are compiled into each DRI driver so libGL.so knows nothing
 * about them.
 *
 */

#include <assert.h>
#include <stdarg.h>
#include <unistd.h>
#include <stdio.h>
#include <linux/fb.h>
#include "GL/miniglx.h"
#include "miniglxP.h"
#include <linux/vt.h>
#include <sys/ioctl.h>
#include <sys/mman.h>


#include "sarea.h"
#include "dri_util.h"


/**
 * \brief Print message to \c stderr if the \c LIBGL_DEBUG environment variable
 * is set.
 * 
 * Is called from the drivers.
 * 
 * \param f \e printf like format.
 * 
 * \internal
 * This function is a wrapper around vfprintf().
 */
void
__driUtilMessage(const char *f, ...)
{
    va_list args;

    if (getenv("LIBGL_DEBUG")) {
        fprintf(stderr, "libGL error: \n");
        va_start(args, f);
        vfprintf(stderr, f, args);
        va_end(args);
        fprintf(stderr, "\n");
    }
}


/*****************************************************************/
/** \name Visual utility functions                               */
/*****************************************************************/
/*@{*/


/**
 * \brief Find a visual.
 * 
 * \param dpy the display handle.
 * \param scrn the screen number. It is currently ignored and should be zero.
 * \param vid visual ID.
 * 
 * \return pointer to the wanted __GLXvisualConfigRec if found, or NULL otherwise.
 * 
 * \internal
 * This functions walks through the list of visuals in
 * MiniGLXDisplayRec::configs until finding one with a matching visual ID.
 */
static __GLXvisualConfig *
__driFindGlxConfig(Display *dpy, int scrn, VisualID vid)
{
    __GLXvisualConfig *glxConfigs;
    int numConfigs, i;

    numConfigs = dpy->numConfigs;
    glxConfigs = dpy->configs;

    for (i = 0; i < numConfigs; i++) {
        if (glxConfigs[i].vid == vid) {
            return glxConfigs + i;
        }
    }
    return NULL;
}


/**
 * \brief Convert a __GLXvisualConfigRec structure into a __GLcontextModesRec
 * structure.
 * 
 * \param modes pointer to the destination __GLcontextModesRec structure.
 * \param config pointer to the source __GLXvisualConfigRec structure.
 * 
 * \internal
 * This function comes from xc/programs/Xserver/GL/glx/glxcmds.c
 *
 * It translates the necessary data bits from \p config to \p modes.
 */
static void
__glXFormatGLModes(__GLcontextModes *modes, const __GLXvisualConfig *config)
{
    memset(modes, 0, sizeof(__GLcontextModes));

    modes->rgbMode = (config->rgba != 0);
    modes->colorIndexMode = !(modes->rgbMode);
    modes->doubleBufferMode = (config->doubleBuffer != 0);
    modes->stereoMode = (config->stereo != 0);

    modes->haveAccumBuffer = ((config->accumRedSize +
			       config->accumGreenSize +
			       config->accumBlueSize +
			       config->accumAlphaSize) > 0);
    modes->haveDepthBuffer = (config->depthSize > 0);
    modes->haveStencilBuffer = (config->stencilSize > 0);

    modes->redBits = config->redSize;
    modes->greenBits = config->greenSize;
    modes->blueBits = config->blueSize;
    modes->alphaBits = config->alphaSize;
    modes->redMask = config->redMask;
    modes->greenMask = config->greenMask;
    modes->blueMask = config->blueMask;
    modes->alphaMask = config->alphaMask;
    modes->rgbBits = config->bufferSize;
    modes->indexBits = config->bufferSize;

    modes->accumRedBits = config->accumRedSize;
    modes->accumGreenBits = config->accumGreenSize;
    modes->accumBlueBits = config->accumBlueSize;
    modes->accumAlphaBits = config->accumAlphaSize;
    modes->depthBits = config->depthSize;
    modes->stencilBits = config->stencilSize;

    modes->numAuxBuffers = 0;	/* XXX: should be picked up from the visual */

    modes->level = config->level;
}

/*@}*/


/*****************************************************************/
/** \name Context (un)binding functions                          */
/*****************************************************************/
/*@{*/


/**
 * \brief Unbind context.
 * 
 * \param dpy the display handle.
 * \param scrn the screen number.
 * \param draw drawable.
 * \param gc context.
 * \param will_rebind not used.
 *
 * \return GL_TRUE on success, or GL_FALSE on failure.
 * 
 * \internal
 * This function calls __DriverAPIRec::UnbindContext, and then decrements
 * __DRIdrawablePrivateRec::refcount which must be non-zero for a successful
 * return.
 * 
 * While casting the opaque private pointers associated with the parameters into their
 * respective real types it also assures they are not null. 
 */
static Bool driUnbindContext(Display *dpy, int scrn,
                             GLXDrawable draw, GLXContext gc,
                             int will_rebind)
{
    __DRIscreen *pDRIScreen;
    __DRIdrawable *pdraw;
    __DRIcontextPrivate *pcp;
    __DRIscreenPrivate *psp;
    __DRIdrawablePrivate *pdp;

    if (gc == NULL || draw == None) 
	return GL_FALSE;

    if (!(pDRIScreen = __glXFindDRIScreen(dpy, scrn))) 
	return GL_FALSE;

    if (!(psp = (__DRIscreenPrivate *)pDRIScreen->private)) 
	return GL_FALSE;

    if (!(pdraw = &draw->driDrawable))
	return GL_FALSE;

    pcp = (__DRIcontextPrivate *)gc->driContext.private;
    pdp = (__DRIdrawablePrivate *)pdraw->private;

    /* Let driver unbind drawable from context */
    (*psp->DriverAPI.UnbindContext)(pcp);

    if (pdp->refcount == 0) 
	return GL_FALSE;

    --pdp->refcount;

    return GL_TRUE;
}


/**
 * \brief Unbind context.
 * 
 * \param dpy the display handle.
 * \param scrn the screen number.
 * \param draw drawable.
 * \param gc context.
 *
 * \internal
 * This function and increments __DRIdrawablePrivateRec::refcount and calls
 * __DriverAPIRec::MakeCurrent to binds the drawable.
 * 
 * While casting the opaque private pointers into their
 * respective real types it also assures they are not null. 
 */
static Bool driBindContext(Display *dpy, int scrn,
                            GLXDrawable draw,
                            GLXContext gc)
{
    __DRIscreen *pDRIScreen;
    __DRIdrawable *pdraw;
    __DRIdrawablePrivate *pdp;
    __DRIscreenPrivate *psp;
    __DRIcontextPrivate *pcp;

    /*
     * Assume error checking is done properly in glXMakeCurrent before
     * calling driBindContext.
     */
    if (gc == NULL || draw == None) 
	return GL_FALSE;

    if (!(pDRIScreen = __glXFindDRIScreen(dpy, scrn))) 
	return GL_FALSE;

    if (!(psp = (__DRIscreenPrivate *)pDRIScreen->private)) 
	return GL_FALSE;


    pdraw = &draw->driDrawable;
    pdp = (__DRIdrawablePrivate *) pdraw->private;

    /* Bind the drawable to the context */
    pcp = (__DRIcontextPrivate *)gc->driContext.private;
    pcp->driDrawablePriv = pdp;
    pdp->driContextPriv = pcp;
    pdp->refcount++;

    /* Call device-specific MakeCurrent */
    (*psp->DriverAPI.MakeCurrent)(pcp, pdp, pdp);

    return GL_TRUE;
}

/*@}*/


/*****************************************************************/
/** \name Drawable handling functions                            */
/*****************************************************************/
/*@{*/


/**
 * \brief Update private drawable information.
 *
 * \param pdp pointer to the private drawable information to update.
 * 
 * \internal
 * This function is a no-op. Should never be called but is referenced as an
 * external symbol from client drivers.
 */
void __driUtilUpdateDrawableInfo(__DRIdrawablePrivate *pdp)
{
   __DRIscreenPrivate *psp = pdp->driScreenPriv;

   pdp->numClipRects = psp->pSAREA->drawableTable[pdp->index].flags ? 1 : 0;
   pdp->lastStamp = *(pdp->pStamp);
}


/**
 * \brief Swap buffers.
 *
 * \param dpy the display handle.
 * \param drawablePrivate opaque pointer to the per-drawable private info.
 * 
 * \internal
 * This function calls __DRIdrawablePrivate::swapBuffers.
 * 
 * Is called directly from glXSwapBuffers().
 */
static void driSwapBuffers( Display *dpy, void *drawablePrivate )
{
    __DRIdrawablePrivate *dPriv = (__DRIdrawablePrivate *) drawablePrivate;
    dPriv->swapBuffers(dPriv);
    (void) dpy;
}


/**
 * \brief Destroy per-drawable private information.
 *
 * \param dpy the display handle.
 * \param drawablePrivate opaque pointer to the per-drawable private info.
 *
 * \internal
 * This function calls __DriverAPIRec::DestroyBuffer on \p drawablePrivate,
 * frees the clip rects if any, and finally frees \p drawablePrivate itself.
 */
static void driDestroyDrawable(Display *dpy, void *drawablePrivate)
{
    __DRIdrawablePrivate *pdp = (__DRIdrawablePrivate *) drawablePrivate;
    __DRIscreenPrivate *psp = pdp->driScreenPriv;

    if (pdp) {
        (*psp->DriverAPI.DestroyBuffer)(pdp);
	if (pdp->pClipRects)
	    free(pdp->pClipRects);
	free(pdp);
    }
}


/**
 * \brief Create the per-drawable private driver information.
 * 
 * \param dpy the display handle.
 * \param scrn the screen number.
 * \param draw the GLX drawable info.
 * \param vid visual ID.
 * \param pdraw will receive the drawable dependent methods.
 * 
 *
 * \returns a opaque pointer to the per-drawable private info on success, or NULL
 * on failure.
 * 
 * \internal
 * This function allocates and fills a __DRIdrawablePrivateRec structure,
 * initializing the invariant window dimensions and clip rects.  It obtains the
 * visual config, converts it into a __GLcontextModesRec and passes it to
 * __DriverAPIRec::CreateBuffer to create a buffer.
 */
static void *driCreateDrawable(Display *dpy, int scrn,
                                     GLXDrawable draw,
                                     VisualID vid, __DRIdrawable *pdraw)
{
    __DRIscreen *pDRIScreen;
    __DRIscreenPrivate *psp;
    __DRIdrawablePrivate *pdp;
    __GLXvisualConfig *config;
    __GLcontextModes modes;

    pdp = (__DRIdrawablePrivate *)malloc(sizeof(__DRIdrawablePrivate));
    if (!pdp) {
	return NULL;
    }

    pdp->index = dpy->clientID;
    pdp->draw = draw;
    pdp->refcount = 0;
    pdp->lastStamp = -1;
    pdp->numBackClipRects = 0;
    pdp->pBackClipRects = NULL;
    pdp->display = dpy;
    pdp->screen = scrn;

    /* Initialize with the invariant window dimensions and clip rects here.
     */
    pdp->x = 0;
    pdp->y = 0;
    pdp->w = pdp->draw->w;
    pdp->h = pdp->draw->h;
    pdp->numClipRects = 0;
    pdp->pClipRects = (XF86DRIClipRectPtr) malloc(sizeof(XF86DRIClipRectRec));
    (pdp->pClipRects)[0].x1 = 0;
    (pdp->pClipRects)[0].y1 = 0;
    (pdp->pClipRects)[0].x2 = pdp->draw->w;
    (pdp->pClipRects)[0].y2 = pdp->draw->h;

    if (!(pDRIScreen = __glXFindDRIScreen(dpy, scrn))) {
	free(pdp);
	return NULL;
    } else if (!(psp = (__DRIscreenPrivate *)pDRIScreen->private)) {
	free(pdp);
	return NULL;
    }
    pdp->driScreenPriv = psp;
    pdp->driContextPriv = 0;

    config = __driFindGlxConfig(dpy, scrn, vid);
    if (!config)
        return NULL;

    /* convert GLXvisualConfig struct to GLcontextModes struct */
    __glXFormatGLModes(&modes, config);

    if (!(*psp->DriverAPI.CreateBuffer)(psp, pdp, &modes, GL_FALSE)) {
       free(pdp);
       return NULL;
    }

    pdraw->destroyDrawable = driDestroyDrawable;
    pdraw->swapBuffers = driSwapBuffers;  /* called by glXSwapBuffers() */
    pdp->swapBuffers = psp->DriverAPI.SwapBuffers;

    pdp->pStamp = &(psp->pSAREA->drawableTable[pdp->index].stamp);
    return (void *) pdp;
}

/**
 * \brief Get the per-drawable dependent methods.
 * 
 * \param dpy the display handle.
 * \param draw the GLX drawable.
 * \param screenPrivate opaque pointer to the per-screen private information.
 *
 * \return pointer to a __DRIdrawableRec structure.
 *
 * \internal
 * This function returns the MiniGLXwindowRec::driDrawable attribute.
 */
static __DRIdrawable *driGetDrawable(Display *dpy, GLXDrawable draw,
					 void *screenPrivate)
{
    return &draw->driDrawable;
}

/*@}*/


/*****************************************************************/
/** \name Context handling functions                             */
/*****************************************************************/
/*@{*/


/**
 * \brief Destroy the per-context private information.
 * 
 * \param dpy the display handle.
 * \param scrn the screen number.
 * \param contextPrivate opaque pointer to the per-drawable private info.
 *
 * \internal
 * This function calls __DriverAPIRec::DestroyContext on \p contextPrivate, calls
 * drmDestroyContext(), and finally frees \p contextPrivate.
 */
static void driDestroyContext(Display *dpy, int scrn, void *contextPrivate)
{
    __DRIcontextPrivate  *pcp   = (__DRIcontextPrivate *) contextPrivate;
    __DRIscreenPrivate   *psp = NULL;

    if (pcp) {
	(*pcp->driScreenPriv->DriverAPI.DestroyContext)(pcp);
        psp = pcp->driDrawablePriv->driScreenPriv;
	if (psp->fd) {
	   printf(">>> drmDestroyContext(0x%x)\n", (int) pcp->hHWContext);
	   drmDestroyContext(psp->fd, pcp->hHWContext);
	}
	free(pcp);
    }
}

/**
 * \brief Create the per-drawable private driver information.
 * 
 * \param dpy the display handle.
 * \param vis the visual information.
 * \param sharedPrivate the shared context dependent methods or NULL if non-existent.
 * \param pctx will receive the context dependent methods.
 *
 * \returns a opaque pointer to the per-context private information on success, or NULL
 * on failure.
 * 
 * \internal
 * This function allocates and fills a __DRIcontextPrivateRec structure.  It
 * gets the visual, converts it into a __GLcontextModesRec and passes it
 * to __DriverAPIRec::CreateContext to create the context.
 */
static void *driCreateContext(Display *dpy, XVisualInfo *vis,
                              void *sharedPrivate,
                              __DRIcontext *pctx)
{
   __DRIscreen *pDRIScreen;
   __DRIcontextPrivate *pcp;
   __DRIcontextPrivate *pshare = (__DRIcontextPrivate *) sharedPrivate;
   __DRIscreenPrivate *psp;
   __GLXvisualConfig *config;
   __GLcontextModes modes;
   void *shareCtx;

   if (!(pDRIScreen = __glXFindDRIScreen(dpy, 0))) 
      return NULL;
    
   if (!(psp = (__DRIscreenPrivate *)pDRIScreen->private)) 
      return NULL;

   pcp = (__DRIcontextPrivate *)malloc(sizeof(__DRIcontextPrivate));
   if (!pcp) {
      return NULL;
   }

   pcp->display = dpy;
   pcp->driScreenPriv = psp;
   pcp->driDrawablePriv = NULL;

   if (psp->fd) {
      if (drmCreateContext(psp->fd, &pcp->hHWContext)) {
	 fprintf(stderr, ">>> drmCreateContext failed\n");
	 free(pcp);
	 return NULL;
      }
   }


   /* Setup a __GLcontextModes struct corresponding to vis->visualid
    * and create the rendering context.
    */
   config = __driFindGlxConfig(dpy, 0, vis->visualid);
   if (!config)
      return NULL;

   __glXFormatGLModes(&modes, config);
   shareCtx = pshare ? pshare->driverPrivate : NULL;
   if (!(*psp->DriverAPI.CreateContext)(&modes, pcp, shareCtx)) {
      if (psp->fd) 
	 (void) drmDestroyContext(psp->fd, pcp->hHWContext);
      free(pcp);
      return NULL;
   }

   pctx->destroyContext = driDestroyContext;
   pctx->bindContext    = driBindContext;
   pctx->unbindContext  = driUnbindContext;

   return pcp;
}

/*@}*/


/*****************************************************************/
/** \name Screen handling functions                              */
/*****************************************************************/
/*@{*/


/**
 * \brief Destroy the per-screen private information.
 * 
 * \param dpy the display handle.
 * \param scrn the screen number.
 * \param screenPrivate opaque pointer to the per-screen private information.
 *
 * \internal
 * This function calls __DriverAPIRec::DestroyScreen on \p screenPrivate, calls
 * drmClose(), and finally frees \p screenPrivate.
 */
static void driDestroyScreen(Display *dpy, int scrn, void *screenPrivate)
{
    __DRIscreenPrivate *psp = (__DRIscreenPrivate *) screenPrivate;
    
    if (psp) {
	if (psp->DriverAPI.DestroyScreen)
	    (*psp->DriverAPI.DestroyScreen)(psp);

	if (psp->fd) 
	   (void)drmClose(psp->fd);

	free(psp->pDevPriv);
	free(psp);
    }
}


/**
 * \brief Create the per-screen private information.
 * 
 * \param dpy the display handle.
 * \param scrn the screen number.
 * \param psc will receive the screen dependent methods.
 * \param numConfigs number of visuals.
 * \param config visuals.
 * \param driverAPI driver callbacks structure.
 *
 * \return a pointer to the per-screen private information.
 * 
 * \internal
 * This function allocates and fills a __DRIscreenPrivateRec structure. It
 * opens the DRM device verifying that the exported version matches the
 * expected.  It copies the driver callback functions and calls
 * __DriverAPIRec::InitDriver.
 *
 * If a client maps the framebuffer and SAREA regions.
 */
__DRIscreenPrivate *
__driUtilCreateScreen(Display *dpy, int scrn, __DRIscreen *psc,
                      int numConfigs, __GLXvisualConfig *config,
                      const struct __DriverAPIRec *driverAPI)
{
   __DRIscreenPrivate *psp;

   psp = (__DRIscreenPrivate *)malloc(sizeof(__DRIscreenPrivate));
   if (!psp) {
      return NULL;
   }

   psp->display = dpy;
   psp->myNum = scrn;

   psp->fd = drmOpen(NULL,dpy->driverContext.pciBusID);
   if (psp->fd < 0) {
      fprintf(stderr, "libGL error: failed to open DRM: %s\n", 
	      strerror(-psp->fd));
      free(psp);
      return NULL;
   }

   {
      drmVersionPtr version = drmGetVersion(psp->fd);
      if (version) {
	 psp->drmMajor = version->version_major;
	 psp->drmMinor = version->version_minor;
	 psp->drmPatch = version->version_patchlevel;
	 drmFreeVersion(version);
      }
      else {
	 fprintf(stderr, "libGL error: failed to get drm version: %s\n", 
		 strerror(-psp->fd));
	 free(psp);
	 return NULL;
      }
   }

   /*
    * Fake various version numbers.
    */
   psp->ddxMajor = 4;
   psp->ddxMinor = 0;
   psp->ddxPatch = 1;
   psp->driMajor = 4;
   psp->driMinor = 1;
   psp->driPatch = 0;

   /* install driver's callback functions */
   memcpy(&psp->DriverAPI, driverAPI, sizeof(struct __DriverAPIRec));

   /*
    * Get device-specific info.  pDevPriv will point to a struct
    * (such as DRIRADEONRec in xfree86/driver/ati/radeon_dri.h)
    * that has information about the screen size, depth, pitch,
    * ancilliary buffers, DRM mmap handles, etc.
    */
   psp->fbOrigin = 0;  
   psp->fbSize = dpy->driverContext.shared.fbSize; 
   psp->fbStride = dpy->driverContext.shared.fbStride;
   psp->devPrivSize = dpy->driverContext.driverClientMsgSize;
   psp->pDevPriv = dpy->driverContext.driverClientMsg;
   psp->fbWidth = dpy->driverContext.shared.virtualWidth;
   psp->fbHeight = dpy->driverContext.shared.virtualHeight;
   psp->fbBPP = dpy->driverContext.bpp;

   if (dpy->IsClient) {
      /*
       * Map the framebuffer region.  
       */
      if (drmMap(psp->fd, dpy->driverContext.shared.hFrameBuffer, psp->fbSize, 
		 (drmAddressPtr)&psp->pFB)) {
	 fprintf(stderr, "libGL error: drmMap of framebuffer failed\n");
	 (void)drmClose(psp->fd);
	 free(psp);
	 return NULL;
      }

      /*
       * Map the SAREA region.  Further mmap regions may be setup in
       * each DRI driver's "createScreen" function.
       */
      if (drmMap(psp->fd, dpy->driverContext.shared.hSAREA,
                 dpy->driverContext.shared.SAREASize, 
		 (drmAddressPtr)&psp->pSAREA)) {
	 fprintf(stderr, "libGL error: drmMap of sarea failed\n");
	 (void)drmUnmap((drmAddress)psp->pFB, psp->fbSize);
	 (void)drmClose(psp->fd);
	 free(psp);
	 return NULL;
      }

#if !_HAVE_FULL_GL
      mprotect(psp->pSAREA, dpy->driverContext.shared.SAREASize, PROT_READ);
#endif

   } else {
      psp->pFB = dpy->driverContext.FBAddress;
      psp->pSAREA = dpy->driverContext.pSAREA;
   }


   /* Initialize the screen specific GLX driver */
   if (psp->DriverAPI.InitDriver) {
      if (!(*psp->DriverAPI.InitDriver)(psp)) {
	 fprintf(stderr, "libGL error: InitDriver failed\n");
	 free(psp->pDevPriv);
	 (void)drmClose(psp->fd);
	 free(psp);
	 return NULL;
      }
   }

   return psp;
}



/**
 * \brief Create the per-screen private information.
 *
 * Version for drivers without a DRM module.
 * 
 * \param dpy the display handle.
 * \param scrn the screen number.
 * \param psc pointer to the screen dependent methods structure.
 * \param numConfigs number of visuals.
 * \param config visuals.
 * \param driverAPI driver callbacks structure.
 * 
 * \internal
 * Same as __driUtilCreateScreen() but without opening the DRM device.
 */
__DRIscreenPrivate *
__driUtilCreateScreenNoDRM(Display *dpy, int scrn, __DRIscreen *psc,
			   int numConfigs, __GLXvisualConfig *config,
			   const struct __DriverAPIRec *driverAPI)
{
    __DRIscreenPrivate *psp;

    psp = (__DRIscreenPrivate *)calloc(1, sizeof(__DRIscreenPrivate));
    if (!psp) 
	return NULL;

    psp->ddxMajor = 4;
    psp->ddxMinor = 0;
    psp->ddxPatch = 1;
    psp->driMajor = 4;
    psp->driMinor = 1;
    psp->driPatch = 0;
    psp->display = dpy;
    psp->myNum = scrn;
    psp->fd = 0;
    psp->fbOrigin = 0; 

    psp->fbSize = dpy->driverContext.shared.fbSize; 
    psp->fbStride = dpy->driverContext.shared.fbStride;
    psp->devPrivSize = dpy->driverContext.driverClientMsgSize;
    psp->pDevPriv = dpy->driverContext.driverClientMsg;
    psp->fbWidth = dpy->driverContext.shared.virtualWidth;
    psp->fbHeight = dpy->driverContext.shared.virtualHeight;
    psp->fbBPP = dpy->driverContext.bpp;

    psp->pFB = dpy->driverContext.FBAddress;

    /* install driver's callback functions */
    memcpy(&psp->DriverAPI, driverAPI, sizeof(struct __DriverAPIRec));

    /* Initialize the screen specific GLX driver */
    if (psp->DriverAPI.InitDriver) {
	if (!(*psp->DriverAPI.InitDriver)(psp)) {
	    fprintf(stderr, "libGL error: InitDriver failed\n");
	    free(psp->pDevPriv);
	    free(psp);
	    return NULL;
	}
    }

    return psp;
}


/**
 * \brief Initialize the screen dependent methods.
 *
 * \param dpy the display handle.
 * \param scrn the screen number.
 * \param psc pointer to the screen dependent methods structure.
 *
 * \internal
 * These can be put in place and safely used prior to __driUtilCreateScreen()
 * being called.  This allows glXCreateContext() to be called prior to
 * XCreateWindow(), but still allows XCreateWindow() to determine the virtual
 * resolution (a screen parameter as far as the driver is concerned).
 */
void
__driUtilInitScreen( Display *dpy, int scrn, __DRIscreen *psc )
{
    psc->destroyScreen  = driDestroyScreen;
    psc->createContext  = driCreateContext;
    psc->createDrawable = driCreateDrawable;
    psc->getDrawable    = driGetDrawable;
}




/*@}*/
