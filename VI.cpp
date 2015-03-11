#include "GLideN64.h"
#include <math.h>
#include "Types.h"
#include "VI.h"
#include "OpenGL.h"
#include "N64.h"
#include "gSP.h"
#include "gDP.h"
#include "RSP.h"
#include "FrameBuffer.h"
#include "DepthBuffer.h"
#include "Config.h"
#include "Debug.h"

using namespace std;

VIInfo VI;

void VI_UpdateSize()
{
	const f32 xScale = _FIXED2FLOAT( _SHIFTR( *REG.VI_X_SCALE, 0, 12 ), 10 );
//	f32 xOffset = _FIXED2FLOAT( _SHIFTR( *REG.VI_X_SCALE, 16, 12 ), 10 );

	const u32 vScale = _SHIFTR(*REG.VI_Y_SCALE, 0, 12);
//	f32 yOffset = _FIXED2FLOAT( _SHIFTR( *REG.VI_Y_SCALE, 16, 12 ), 10 );

	const u32 hEnd = _SHIFTR( *REG.VI_H_START, 0, 10 );
	const u32 hStart = _SHIFTR( *REG.VI_H_START, 16, 10 );

	// These are in half-lines, so shift an extra bit
	const u32 vEnd = _SHIFTR( *REG.VI_V_START, 0, 10 );
	const u32 vStart = _SHIFTR( *REG.VI_V_START, 16, 10 );
	const bool interlacedPrev = VI.interlaced;
	const u32 widthPrev = VI.width;
	const u32 heightPrev = VI.height;

	VI.real_height = vEnd > vStart ? (((vEnd - vStart) >> 1) * vScale) >> 10 : 0;
	VI.width = *REG.VI_WIDTH;
	VI.interlaced = (*REG.VI_STATUS & 0x40) != 0;
	if (VI.interlaced) {
		u32 fullWidth = (u32)(640.0f*xScale);
		if (*REG.VI_WIDTH > fullWidth) {
			const u32 scale = *REG.VI_WIDTH / fullWidth;
			VI.width /= scale;
			VI.real_height *= scale;
		}
		if (VI.real_height % 2 == 1)
			--VI.real_height;
	} else if (hEnd != 0 && *REG.VI_WIDTH != 0)
		VI.width = min((u32)((hEnd - hStart)*xScale), *REG.VI_WIDTH);

	const bool isPAL = (*REG.VI_V_SYNC & 0x3ff) > 550;
	if (isPAL && (vEnd - vStart) > 480) {
		VI.height = (u32)(VI.real_height*1.0041841f);
		if (VI.height > 576)
			VI.height = VI.real_height = 576;
	}
	else {
		VI.height = (u32)(VI.real_height*1.0126582f);
		if (VI.height > 480)
			VI.height = VI.real_height = 480;
	}
	if (VI.height % 2 == 1)
		--VI.height;

//	const int fsaa = ((*REG.VI_STATUS) >> 8) & 3;
//	const int divot = ((*REG.VI_STATUS) >> 4) & 1;

	if (config.frameBufferEmulation.enable && (interlacedPrev != VI.interlaced || widthPrev != VI.width || heightPrev != VI.height)) {
		frameBufferList().removeBuffers(widthPrev, heightPrev);
		frameBufferList().removeBuffers(VI.width, VI.height);
		depthBufferList().destroy();
		depthBufferList().init();
	}

	VI.rwidth = VI.width != 0 ? 1.0f / VI.width : 0.0f;
	VI.rheight = VI.height != 0 ? 1.0f / VI.height : 0.0f;
}

void VI_UpdateScreen()
{
	static u32 uNumCurFrameIsShown = 0;

	if (VI.lastOrigin == -1) // Workaround for Mupen64Plus issue with initialization
		isGLError();

	glFinish();

	OGLVideo & ogl = video();
	if (ogl.changeWindow())
		return;
	if (ogl.resizeWindow())
		return;
	ogl.saveScreenshot();

	bool bVIUpdated = false;
	if (*REG.VI_ORIGIN != VI.lastOrigin) {
		VI_UpdateSize();
		bVIUpdated = true;
		ogl.updateScale();
	}

	if (config.frameBufferEmulation.enable) {
		const bool bCFB = !config.frameBufferEmulation.ignoreCFB && (gSP.changed&CHANGED_CPU_FB_WRITE) == CHANGED_CPU_FB_WRITE;
		const bool bNeedUpdate = gDP.colorImage.changed != 0 || (bCFB ? true : (*REG.VI_ORIGIN != VI.lastOrigin));

		if (bNeedUpdate) {
			if ((gSP.changed&CHANGED_CPU_FB_WRITE) == CHANGED_CPU_FB_WRITE) {
				FrameBuffer * pBuffer = frameBufferList().findBuffer(*REG.VI_ORIGIN);
				if (pBuffer == NULL || pBuffer->m_width != VI.width) {
					if (!bVIUpdated) {
						VI_UpdateSize();
						ogl.updateScale();
						bVIUpdated = true;
					}
					const u32 size = *REG.VI_STATUS & 3;
					if (VI.height > 0 && size > G_IM_SIZ_8b  && VI.width > 0)
						frameBufferList().saveBuffer(*REG.VI_ORIGIN, G_IM_FMT_RGBA, size, VI.width, VI.height, true);
				}
			}
			if ((((*REG.VI_STATUS) & 3) > 0) && ((config.frameBufferEmulation.copyFromRDRAM && gDP.colorImage.changed) || bCFB)) {
				if (!bVIUpdated) {
					VI_UpdateSize();
					bVIUpdated = true;
				}
				FrameBuffer_CopyFromRDRAM(*REG.VI_ORIGIN, config.frameBufferEmulation.copyFromRDRAM && !bCFB);
			}
			frameBufferList().renderBuffer(*REG.VI_ORIGIN);

			if (gDP.colorImage.changed)
				uNumCurFrameIsShown = 0;
			else {
				uNumCurFrameIsShown++;
				if (uNumCurFrameIsShown > 25)
					gSP.changed |= CHANGED_CPU_FB_WRITE;
			}
			frameBufferList().clearBuffersChanged();
			VI.lastOrigin = *REG.VI_ORIGIN;
#ifdef DEBUG
			while (Debug.paused && !Debug.step);
			Debug.step = FALSE;
#endif
		} else {
			uNumCurFrameIsShown++;
			if (uNumCurFrameIsShown > 25)
				gSP.changed |= CHANGED_CPU_FB_WRITE;
		}
	}
	else {
		if (gSP.changed & CHANGED_COLORBUFFER) {
			ogl.swapBuffers();
			gSP.changed &= ~CHANGED_COLORBUFFER;
#ifdef DEBUG
			while (Debug.paused && !Debug.step);
			Debug.step = FALSE;
#endif
			VI.lastOrigin = *REG.VI_ORIGIN;
		}
	}

	if (VI.lastOrigin == -1) { // Workaround for Mupen64Plus issue with initialization
		glClearColor(0.0f, 0.0f, 0.0f, 0.0f);
		glClear(GL_COLOR_BUFFER_BIT);
	}

	glFinish();
}
