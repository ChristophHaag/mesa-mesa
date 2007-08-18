#include "utils.h"
#include "framebuffer.h"
#include "renderbuffer.h"
#include "fbobject.h"

#include "nouveau_context.h"
#include "nouveau_fbo.h"
#include "nouveau_fifo.h"
#include "nouveau_msg.h"
#include "nouveau_object.h"
#include "nouveau_reg.h"

static GLboolean
nouveau_renderbuffer_pixelformat(nouveau_renderbuffer_t * nrb,
				 GLenum internalFormat)
{
	nrb->mesa.InternalFormat = internalFormat;

	/*TODO: We probably want to extend this a bit, and maybe make
	 *      card-specific? 
	 */
	switch (internalFormat) {
	case GL_RGBA:
	case GL_RGBA8:
		nrb->mesa._BaseFormat = GL_RGBA;
		nrb->mesa._ActualFormat = GL_RGBA8;
		nrb->mesa.DataType = GL_UNSIGNED_BYTE;
		nrb->mesa.RedBits = 8;
		nrb->mesa.GreenBits = 8;
		nrb->mesa.BlueBits = 8;
		nrb->mesa.AlphaBits = 8;
		nrb->cpp = 4;
		break;
	case GL_RGB:
	case GL_RGB5:
		nrb->mesa._BaseFormat = GL_RGB;
		nrb->mesa._ActualFormat = GL_RGB5;
		nrb->mesa.DataType = GL_UNSIGNED_BYTE;
		nrb->mesa.RedBits = 5;
		nrb->mesa.GreenBits = 6;
		nrb->mesa.BlueBits = 5;
		nrb->mesa.AlphaBits = 0;
		nrb->cpp = 2;
		break;
	case GL_DEPTH_COMPONENT16:
		nrb->mesa._BaseFormat = GL_DEPTH_COMPONENT;
		nrb->mesa._ActualFormat = GL_DEPTH_COMPONENT16;
		nrb->mesa.DataType = GL_UNSIGNED_SHORT;
		nrb->mesa.DepthBits = 16;
		nrb->cpp = 2;
		break;
	case GL_DEPTH_COMPONENT24:
		nrb->mesa._BaseFormat = GL_DEPTH_COMPONENT;
		nrb->mesa._ActualFormat = GL_DEPTH24_STENCIL8_EXT;
		nrb->mesa.DataType = GL_UNSIGNED_INT_24_8_EXT;
		nrb->mesa.DepthBits = 24;
		nrb->cpp = 4;
		break;
	case GL_STENCIL_INDEX8_EXT:
		nrb->mesa._BaseFormat = GL_STENCIL_INDEX;
		nrb->mesa._ActualFormat = GL_DEPTH24_STENCIL8_EXT;
		nrb->mesa.DataType = GL_UNSIGNED_INT_24_8_EXT;
		nrb->mesa.StencilBits = 8;
		nrb->cpp = 4;
		break;
	case GL_DEPTH24_STENCIL8_EXT:
		nrb->mesa._BaseFormat = GL_DEPTH_STENCIL_EXT;
		nrb->mesa._ActualFormat = GL_DEPTH24_STENCIL8_EXT;
		nrb->mesa.DataType = GL_UNSIGNED_INT_24_8_EXT;
		nrb->mesa.DepthBits = 24;
		nrb->mesa.StencilBits = 8;
		nrb->cpp = 4;
		break;
	default:
		return GL_FALSE;
		break;
	}

	return GL_TRUE;
}

static GLboolean
nouveau_renderbuffer_storage(GLcontext * ctx, struct gl_renderbuffer *rb,
			     GLenum internalFormat,
			     GLuint width, GLuint height)
{
	nouveau_renderbuffer_t *nrb = (nouveau_renderbuffer_t *) rb;

	if (!nouveau_renderbuffer_pixelformat(nrb, internalFormat)) {
		fprintf(stderr, "%s: unknown internalFormat\n", __func__);
		return GL_FALSE;
	}

	/* If this buffer isn't statically alloc'd, we may need to ask the
	 * drm for more memory */
	if (rb->Width != width || rb->Height != height) {
		GLuint pitch;

		/* align pitches to 64 bytes */
		pitch = ((width * nrb->cpp) + 63) & ~63;

		if (nrb->mem)
			nouveau_mem_free(ctx, nrb->mem);
		nrb->mem = nouveau_mem_alloc(ctx,
					     NOUVEAU_MEM_FB |
					     NOUVEAU_MEM_MAPPED,
					     pitch * height, 0);
		if (!nrb->mem)
			return GL_FALSE;

		/* update nouveau_renderbuffer info */
		nrb->offset = nouveau_mem_gpu_offset_get(ctx, nrb->mem);
		nrb->pitch = pitch;
	}

	rb->Width = width;
	rb->Height = height;
	rb->InternalFormat = internalFormat;

	nouveauSpanSetFunctions(nrb);

	return GL_TRUE;
}

static void
nouveau_renderbuffer_delete(struct gl_renderbuffer *rb)
{
	GET_CURRENT_CONTEXT(ctx);
	nouveau_renderbuffer_t *nrb = (nouveau_renderbuffer_t *) rb;

	if (nrb->mem)
		nouveau_mem_free(ctx, nrb->mem);
	FREE(nrb);
}

nouveau_renderbuffer_t *
nouveau_renderbuffer_new(GLenum internalFormat)
{
	nouveau_renderbuffer_t *nrb;

	nrb = CALLOC_STRUCT(nouveau_renderbuffer);
	if (!nrb)
		return NULL;

	_mesa_init_renderbuffer(&nrb->mesa, 0);

	if (!nouveau_renderbuffer_pixelformat(nrb, internalFormat)) {
		fprintf(stderr, "%s: unknown internalFormat\n", __func__);
		return GL_FALSE;
	}

	nrb->mesa.AllocStorage = nouveau_renderbuffer_storage;
	nrb->mesa.Delete = nouveau_renderbuffer_delete;

	return nrb;
}

static void
nouveau_cliprects_renderbuffer_set(nouveauContextPtr nmesa,
				   nouveau_renderbuffer_t * nrb)
{
	nmesa->numClipRects = 1;
	nmesa->pClipRects = &nmesa->osClipRect;
	nmesa->osClipRect.x1 = 0;
	nmesa->osClipRect.y1 = 0;
	nmesa->osClipRect.x2 = nrb->mesa.Width;
	nmesa->osClipRect.y2 = nrb->mesa.Height;
	nmesa->drawX = 0;
	nmesa->drawY = 0;
	nmesa->drawW = nrb->mesa.Width;
	nmesa->drawH = nrb->mesa.Height;
}

void
nouveau_window_moved(GLcontext * ctx)
{
	nouveauContextPtr nmesa = NOUVEAU_CONTEXT(ctx);
	nouveau_renderbuffer_t *nrb;

	nrb = (nouveau_renderbuffer_t *)
		ctx->DrawBuffer->_ColorDrawBuffers[0][0];
	if (!nrb)
		return;

	nouveau_cliprects_renderbuffer_set(nmesa, nrb);

	/* Viewport depends on window size/position, nouveauCalcViewport
	 * will take care of calling the hw-specific WindowMoved
	 */
	ctx->Driver.Viewport(ctx, ctx->Viewport.X, ctx->Viewport.Y,
			     ctx->Viewport.Width, ctx->Viewport.Height);
	/* Scissor depends on window position */
	ctx->Driver.Scissor(ctx, ctx->Scissor.X, ctx->Scissor.Y,
			    ctx->Scissor.Width, ctx->Scissor.Height);
}

GLboolean
nouveau_build_framebuffer(GLcontext *ctx, struct gl_framebuffer *fb)
{
	nouveauContextPtr nmesa = NOUVEAU_CONTEXT(ctx);
	nouveau_renderbuffer_t *color[MAX_DRAW_BUFFERS];
	nouveau_renderbuffer_t *depth;

	_mesa_update_framebuffer(ctx);
	_mesa_update_draw_buffer_bounds(ctx);

	color[0] = (nouveau_renderbuffer_t *) fb->_ColorDrawBuffers[0][0];
	if (fb->_DepthBuffer && fb->_DepthBuffer->Wrapped)
		depth = (nouveau_renderbuffer_t *) fb->_DepthBuffer->Wrapped;
	else
		depth = (nouveau_renderbuffer_t *) fb->_DepthBuffer;

	if (!nmesa->hw_func.BindBuffers(nmesa, 1, color, depth))
		return GL_FALSE;
	nouveau_window_moved(ctx);

	return GL_TRUE;
}

static void
nouveauDrawBuffer(GLcontext *ctx, GLenum buffer)
{
	nouveau_build_framebuffer(ctx, ctx->DrawBuffer);
}

static struct gl_framebuffer *
nouveauNewFramebuffer(GLcontext *ctx, GLuint name)
{
	return _mesa_new_framebuffer(ctx, name);
}

static struct gl_renderbuffer *
nouveauNewRenderbuffer(GLcontext *ctx, GLuint name)
{
	nouveau_renderbuffer_t *nrb;

	nrb = CALLOC_STRUCT(nouveau_renderbuffer);
	if (!nrb)
		return NULL;

	_mesa_init_renderbuffer(&nrb->mesa, name);

	nrb->mesa.AllocStorage = nouveau_renderbuffer_storage;
	nrb->mesa.Delete = nouveau_renderbuffer_delete;
	return &nrb->mesa;
}

static void
nouveauBindFramebuffer(GLcontext *ctx, GLenum target,
		       struct gl_framebuffer *fb,
		       struct gl_framebuffer *fbread)
{
	if (target == GL_FRAMEBUFFER_EXT || target == GL_DRAW_FRAMEBUFFER_EXT) {
		nouveau_build_framebuffer(ctx, fb);
	}
}

static void
nouveauFramebufferRenderbuffer(GLcontext *ctx, struct gl_framebuffer *fb,
			       GLenum attachment, struct gl_renderbuffer *rb)
{
	_mesa_framebuffer_renderbuffer(ctx, fb, attachment, rb);
	nouveau_build_framebuffer(ctx, fb);
}

static void
nouveauRenderTexture(GLcontext * ctx, struct gl_framebuffer *fb,
		     struct gl_renderbuffer_attachment *att)
{
}

static void
nouveauFinishRenderTexture(GLcontext * ctx,
			   struct gl_renderbuffer_attachment *att)
{
}

void
nouveauInitBufferFuncs(struct dd_function_table *func)
{
	func->DrawBuffer = nouveauDrawBuffer;

	func->NewFramebuffer = nouveauNewFramebuffer;
	func->NewRenderbuffer = nouveauNewRenderbuffer;
	func->BindFramebuffer = nouveauBindFramebuffer;
	func->FramebufferRenderbuffer = nouveauFramebufferRenderbuffer;
	func->RenderTexture = nouveauRenderTexture;
	func->FinishRenderTexture = nouveauFinishRenderTexture;
}

