
#ifndef EGLCONTEXT_INCLUDED
#define EGLCONTEXT_INCLUDED


#include "egltypedefs.h"


/**
 * "Base" class for device driver contexts.
 */
struct _egl_context
{
   /* Managed by EGLDisplay for linking */
   _EGLDisplay *Display;
   _EGLContext *Next;

   _EGLConfig *Config;

   _EGLSurface *DrawSurface;
   _EGLSurface *ReadSurface;

   EGLBoolean IsBound;

   EGLint ClientAPI; /**< EGL_OPENGL_ES_API, EGL_OPENGL_API, EGL_OPENVG_API */
   EGLint ClientVersion; /**< 1 = OpenGLES 1.x, 2 = OpenGLES 2.x */
};


extern EGLBoolean
_eglInitContext(_EGLDriver *drv, _EGLContext *ctx,
                _EGLConfig *config, const EGLint *attrib_list);


extern EGLContext
_eglCreateContext(_EGLDriver *drv, EGLDisplay dpy, EGLConfig config, EGLContext share_list, const EGLint *attrib_list);


extern EGLBoolean
_eglDestroyContext(_EGLDriver *drv, EGLDisplay dpy, EGLContext ctx);


extern EGLBoolean
_eglQueryContext(_EGLDriver *drv, EGLDisplay dpy, EGLContext ctx, EGLint attribute, EGLint *value);


extern EGLBoolean
_eglMakeCurrent(_EGLDriver *drv, EGLDisplay dpy, EGLSurface draw, EGLSurface read, EGLContext ctx);


extern EGLBoolean
_eglCopyContextMESA(_EGLDriver *drv, EGLDisplay dpy, EGLContext source, EGLContext dest, EGLint mask);

#endif /* EGLCONTEXT_INCLUDED */
