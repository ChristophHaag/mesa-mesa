#ifndef EGLDISPLAY_INCLUDED
#define EGLDISPLAY_INCLUDED

#ifdef _EGL_PLATFORM_X
#include <X11/Xlib.h>
#endif

#include "egltypedefs.h"


struct _egl_display 
{
   EGLNativeDisplayType NativeDisplay;
   EGLDisplay Handle;

   const char *DriverName;
   const char *DriverArgs;
   _EGLDriver *Driver;

   EGLint NumScreens;
   _EGLScreen **Screens;  /* array [NumScreens] */

   EGLint NumConfigs;
   _EGLConfig **Configs;  /* array [NumConfigs] of ptr to _EGLConfig */

   /* lists of linked contexts and surface */
   _EGLContext *ContextList;
   _EGLSurface *SurfaceList;
#ifdef _EGL_PLATFORM_X
   Display *Xdpy;
#endif
};


extern _EGLDisplay *
_eglNewDisplay(NativeDisplayType displayName);


extern EGLDisplay
_eglLinkDisplay(_EGLDisplay *dpy);


extern void
_eglUnlinkDisplay(_EGLDisplay *dpy);


extern EGLDisplay
_eglGetDisplayHandle(_EGLDisplay *display);


extern _EGLDisplay *
_eglLookupDisplay(EGLDisplay dpy);


/**
 * Return true if the display is linked.
 */
static INLINE EGLBoolean
_eglIsDisplayLinked(_EGLDisplay *dpy)
{
   return (EGLBoolean) (_eglGetDisplayHandle(dpy) != EGL_NO_DISPLAY);
}


extern _EGLDisplay *
_eglFindDisplay(NativeDisplayType nativeDisplay);


extern void
_eglReleaseDisplayResources(_EGLDriver *drv, EGLDisplay dpy);


extern void
_eglCleanupDisplay(_EGLDisplay *disp);


extern EGLContext
_eglLinkContext(_EGLContext *ctx, _EGLDisplay *dpy);


extern void
_eglUnlinkContext(_EGLContext *ctx);


extern EGLContext
_eglGetContextHandle(_EGLContext *ctx);


extern _EGLContext *
_eglLookupContext(EGLContext ctx);


/**
 * Return true if the context is linked to a display.
 */
static INLINE EGLBoolean
_eglIsContextLinked(_EGLContext *ctx)
{
   return (EGLBoolean) (_eglGetContextHandle(ctx) != EGL_NO_CONTEXT);
}

extern EGLSurface
_eglLinkSurface(_EGLSurface *surf, _EGLDisplay *dpy);


extern void
_eglUnlinkSurface(_EGLSurface *surf);


extern EGLSurface
_eglGetSurfaceHandle(_EGLSurface *);


extern _EGLSurface *
_eglLookupSurface(EGLSurface surf);


/**
 * Return true if the surface is linked to a display.
 */
static INLINE EGLBoolean
_eglIsSurfaceLinked(_EGLSurface *surf)
{
   return (EGLBoolean) (_eglGetSurfaceHandle(surf) != EGL_NO_SURFACE);
}


#endif /* EGLDISPLAY_INCLUDED */
