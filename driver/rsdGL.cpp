/*
 * Copyright (C) 2011 The Android Open Source Project
 * Copyright (C) 2013 Freescale Semiconductor, Inc.
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *      http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#include <ui/FramebufferNativeWindow.h>
#include <ui/PixelFormat.h>

#include <system/window.h>

#include <sys/types.h>
#include <sys/resource.h>
#include <sched.h>

#include <cutils/properties.h>

#include <GLES/gl.h>
#include <GLES/glext.h>
#include <GLES2/gl2.h>
#include <GLES2/gl2ext.h>

#include <string.h>

#include "rsdCore.h"
#include "rsdGL.h"

#include <malloc.h>
#include "rsContext.h"
#include "rsDevice.h"
#include "rsdShaderCache.h"
#include "rsdVertexArray.h"
#include "rsdFrameBufferObj.h"

#include <gui/Surface.h>
#include <gui/DummyConsumer.h>

using namespace android;
using namespace android::renderscript;

static int32_t gGLContextCount = 0;

static void checkEglError(const char* op, EGLBoolean returnVal = EGL_TRUE) {
    struct EGLUtils {
        static const char *strerror(EGLint err) {
            switch (err){
                case EGL_SUCCESS:           return "EGL_SUCCESS";
                case EGL_NOT_INITIALIZED:   return "EGL_NOT_INITIALIZED";
                case EGL_BAD_ACCESS:        return "EGL_BAD_ACCESS";
                case EGL_BAD_ALLOC:         return "EGL_BAD_ALLOC";
                case EGL_BAD_ATTRIBUTE:     return "EGL_BAD_ATTRIBUTE";
                case EGL_BAD_CONFIG:        return "EGL_BAD_CONFIG";
                case EGL_BAD_CONTEXT:       return "EGL_BAD_CONTEXT";
                case EGL_BAD_CURRENT_SURFACE: return "EGL_BAD_CURRENT_SURFACE";
                case EGL_BAD_DISPLAY:       return "EGL_BAD_DISPLAY";
                case EGL_BAD_MATCH:         return "EGL_BAD_MATCH";
                case EGL_BAD_NATIVE_PIXMAP: return "EGL_BAD_NATIVE_PIXMAP";
                case EGL_BAD_NATIVE_WINDOW: return "EGL_BAD_NATIVE_WINDOW";
                case EGL_BAD_PARAMETER:     return "EGL_BAD_PARAMETER";
                case EGL_BAD_SURFACE:       return "EGL_BAD_SURFACE";
                case EGL_CONTEXT_LOST:      return "EGL_CONTEXT_LOST";
                default: return "UNKNOWN";
            }
        }
    };

    if (returnVal != EGL_TRUE) {
        fprintf(stderr, "%s() returned %d\n", op, returnVal);
    }

    for (EGLint error = eglGetError(); error != EGL_SUCCESS; error
            = eglGetError()) {
        fprintf(stderr, "after %s() eglError %s (0x%x)\n", op, EGLUtils::strerror(error),
                error);
    }
}

static void printEGLConfiguration(EGLDisplay dpy, EGLConfig config) {

#define X(VAL) {VAL, #VAL}
    struct {EGLint attribute; const char* name;} names[] = {
    X(EGL_BUFFER_SIZE),
    X(EGL_ALPHA_SIZE),
    X(EGL_BLUE_SIZE),
    X(EGL_GREEN_SIZE),
    X(EGL_RED_SIZE),
    X(EGL_DEPTH_SIZE),
    X(EGL_STENCIL_SIZE),
    X(EGL_CONFIG_CAVEAT),
    X(EGL_CONFIG_ID),
    X(EGL_LEVEL),
    X(EGL_MAX_PBUFFER_HEIGHT),
    X(EGL_MAX_PBUFFER_PIXELS),
    X(EGL_MAX_PBUFFER_WIDTH),
    X(EGL_NATIVE_RENDERABLE),
    X(EGL_NATIVE_VISUAL_ID),
    X(EGL_NATIVE_VISUAL_TYPE),
    X(EGL_SAMPLES),
    X(EGL_SAMPLE_BUFFERS),
    X(EGL_SURFACE_TYPE),
    X(EGL_TRANSPARENT_TYPE),
    X(EGL_TRANSPARENT_RED_VALUE),
    X(EGL_TRANSPARENT_GREEN_VALUE),
    X(EGL_TRANSPARENT_BLUE_VALUE),
    X(EGL_BIND_TO_TEXTURE_RGB),
    X(EGL_BIND_TO_TEXTURE_RGBA),
    X(EGL_MIN_SWAP_INTERVAL),
    X(EGL_MAX_SWAP_INTERVAL),
    X(EGL_LUMINANCE_SIZE),
    X(EGL_ALPHA_MASK_SIZE),
    X(EGL_COLOR_BUFFER_TYPE),
    X(EGL_RENDERABLE_TYPE),
    X(EGL_CONFORMANT),
   };
#undef X

    for (size_t j = 0; j < sizeof(names) / sizeof(names[0]); j++) {
        EGLint value = -1;
        EGLBoolean returnVal = eglGetConfigAttrib(dpy, config, names[j].attribute, &value);
        if (returnVal) {
            ALOGV(" %s: %d (0x%x)", names[j].name, value, value);
        }
    }
}

static void DumpDebug(RsdHal *dc) {
    ALOGE(" EGL ver %i %i", dc->gl.egl.majorVersion, dc->gl.egl.minorVersion);
    ALOGE(" EGL context %p  surface %p,  Display=%p", dc->gl.egl.context, dc->gl.egl.surface,
         dc->gl.egl.display);
    ALOGE(" GL vendor: %s", dc->gl.gl.vendor);
    ALOGE(" GL renderer: %s", dc->gl.gl.renderer);
    ALOGE(" GL Version: %s", dc->gl.gl.version);
    ALOGE(" GL Extensions: %s", dc->gl.gl.extensions);
    ALOGE(" GL int Versions %i %i", dc->gl.gl.majorVersion, dc->gl.gl.minorVersion);

    ALOGV("MAX Textures %i, %i  %i", dc->gl.gl.maxVertexTextureUnits,
         dc->gl.gl.maxFragmentTextureImageUnits, dc->gl.gl.maxTextureImageUnits);
    ALOGV("MAX Attribs %i", dc->gl.gl.maxVertexAttribs);
    ALOGV("MAX Uniforms %i, %i", dc->gl.gl.maxVertexUniformVectors,
         dc->gl.gl.maxFragmentUniformVectors);
    ALOGV("MAX Varyings %i", dc->gl.gl.maxVaryingVectors);
}

void rsdGLShutdown(const Context *rsc) {
    RsdHal *dc = (RsdHal *)rsc->mHal.drv;

    rsdGLSetSurface(rsc, 0, 0, NULL);
    dc->gl.shaderCache->cleanupAll();
    delete dc->gl.shaderCache;
    delete dc->gl.vertexArrayState;

    if (dc->gl.egl.context != EGL_NO_CONTEXT) {
        RSD_CALL_GL(eglMakeCurrent, dc->gl.egl.display,
                    EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
#ifdef IMX5_RS_FIXUP
        /* Make sure to disconnect the DummyConsumer to clear up the BufferQueue mQueue. */
        /* This is needed for z430 GPU since eglCreateWindowSurface post 2 buffers to */
        /* this default surface BufferQueue. When eglDestroySurface is called it waits for */
        /* BufferQueue mQueue to be empty. Since its a Dummy, there is no one consuming */
        /* the buffers from mQueue, hence Disconnect the consumer explicitly */
        dc->gl.egl.bufferqueue->consumerDisconnect();
#endif
        RSD_CALL_GL(eglDestroySurface, dc->gl.egl.display, dc->gl.egl.surfaceDefault);
        if (dc->gl.egl.surface != EGL_NO_SURFACE) {
            RSD_CALL_GL(eglDestroySurface, dc->gl.egl.display, dc->gl.egl.surface);
        }
        RSD_CALL_GL(eglDestroyContext, dc->gl.egl.display, dc->gl.egl.context);
        checkEglError("eglDestroyContext");
    }

    gGLContextCount--;
    if (!gGLContextCount) {
        RSD_CALL_GL(eglTerminate, dc->gl.egl.display);
    }
}

void getConfigData(const Context *rsc,
                   EGLint *configAttribs, size_t configAttribsLen,
                   uint32_t numSamples) {
    memset(configAttribs, 0, configAttribsLen*sizeof(*configAttribs));

    EGLint *configAttribsPtr = configAttribs;

    configAttribsPtr[0] = EGL_SURFACE_TYPE;
    configAttribsPtr[1] = EGL_WINDOW_BIT;
    configAttribsPtr += 2;

    configAttribsPtr[0] = EGL_RENDERABLE_TYPE;
    configAttribsPtr[1] = EGL_OPENGL_ES2_BIT;
    configAttribsPtr += 2;

    configAttribsPtr[0] = EGL_RED_SIZE;
    configAttribsPtr[1] = 8;
    configAttribsPtr += 2;

    configAttribsPtr[0] = EGL_GREEN_SIZE;
    configAttribsPtr[1] = 8;
    configAttribsPtr += 2;

    configAttribsPtr[0] = EGL_BLUE_SIZE;
    configAttribsPtr[1] = 8;
    configAttribsPtr += 2;

    if (rsc->mUserSurfaceConfig.alphaMin > 0) {
        configAttribsPtr[0] = EGL_ALPHA_SIZE;
        configAttribsPtr[1] = rsc->mUserSurfaceConfig.alphaMin;
        configAttribsPtr += 2;
    }

    if (rsc->mUserSurfaceConfig.depthMin > 0) {
        configAttribsPtr[0] = EGL_DEPTH_SIZE;
        configAttribsPtr[1] = rsc->mUserSurfaceConfig.depthMin;
        configAttribsPtr += 2;
    }

    if (rsc->mDev->mForceSW) {
        configAttribsPtr[0] = EGL_CONFIG_CAVEAT;
        configAttribsPtr[1] = EGL_SLOW_CONFIG;
        configAttribsPtr += 2;
    }

    if (numSamples > 1) {
        configAttribsPtr[0] = EGL_SAMPLE_BUFFERS;
        configAttribsPtr[1] = 1;
        configAttribsPtr[2] = EGL_SAMPLES;
        configAttribsPtr[3] = numSamples;
        configAttribsPtr += 4;
    }

    configAttribsPtr[0] = EGL_NONE;
    rsAssert(configAttribsPtr < (configAttribs + configAttribsLen));
}

bool rsdGLInit(const Context *rsc) {
    RsdHal *dc = (RsdHal *)rsc->mHal.drv;

    dc->gl.egl.numConfigs = -1;

    EGLint configAttribs[128];
    EGLint context_attribs2[] = { EGL_CONTEXT_CLIENT_VERSION, 2, EGL_NONE };

    ALOGV("%p initEGL start", rsc);
    rsc->setWatchdogGL("eglGetDisplay", __LINE__, __FILE__);
    dc->gl.egl.display = eglGetDisplay(EGL_DEFAULT_DISPLAY);
    checkEglError("eglGetDisplay");

    RSD_CALL_GL(eglInitialize, dc->gl.egl.display,
                &dc->gl.egl.majorVersion, &dc->gl.egl.minorVersion);
    checkEglError("eglInitialize");

    EGLBoolean ret;

    EGLint numConfigs = -1, n = 0;
    rsc->setWatchdogGL("eglChooseConfig", __LINE__, __FILE__);

    // Try minding a multisample config that matches the user request
    uint32_t minSample = rsc->mUserSurfaceConfig.samplesMin;
    uint32_t prefSample = rsc->mUserSurfaceConfig.samplesPref;
    for (uint32_t sampleCount = prefSample; sampleCount >= minSample; sampleCount--) {
        getConfigData(rsc, configAttribs, (sizeof(configAttribs) / sizeof(EGLint)), sampleCount);
        ret = eglChooseConfig(dc->gl.egl.display, configAttribs, 0, 0, &numConfigs);
        checkEglError("eglGetConfigs", ret);
        if (numConfigs > 0) {
            break;
        }
    }

    if (numConfigs) {
        EGLConfig* const configs = new EGLConfig[numConfigs];

        rsc->setWatchdogGL("eglChooseConfig", __LINE__, __FILE__);
        ret = eglChooseConfig(dc->gl.egl.display,
                configAttribs, configs, numConfigs, &n);
        if (!ret || !n) {
            checkEglError("eglChooseConfig", ret);
            ALOGE("%p, couldn't find an EGLConfig matching the screen format\n", rsc);
        }

        // The first config is guaranteed to over-satisfy the constraints
        dc->gl.egl.config = configs[0];

        // go through the list and skip configs that over-satisfy our needs
        for (int i=0 ; i<n ; i++) {
            if (rsc->mUserSurfaceConfig.alphaMin <= 0) {
                EGLint alphaSize;
                eglGetConfigAttrib(dc->gl.egl.display,
                        configs[i], EGL_ALPHA_SIZE, &alphaSize);
                if (alphaSize > 0) {
                    continue;
                }
            }

            if (rsc->mUserSurfaceConfig.depthMin <= 0) {
                EGLint depthSize;
                eglGetConfigAttrib(dc->gl.egl.display,
                        configs[i], EGL_DEPTH_SIZE, &depthSize);
                if (depthSize > 0) {
                    continue;
                }
            }

            // Found one!
            dc->gl.egl.config = configs[i];
            break;
        }

        delete [] configs;
    }

    //if (props.mLogVisual) {
    if (0) {
        printEGLConfiguration(dc->gl.egl.display, dc->gl.egl.config);
    }
    //}

    rsc->setWatchdogGL("eglCreateContext", __LINE__, __FILE__);
    dc->gl.egl.context = eglCreateContext(dc->gl.egl.display, dc->gl.egl.config,
                                          EGL_NO_CONTEXT, context_attribs2);
    checkEglError("eglCreateContext");
    if (dc->gl.egl.context == EGL_NO_CONTEXT) {
        ALOGE("%p, eglCreateContext returned EGL_NO_CONTEXT", rsc);
        rsc->setWatchdogGL(NULL, 0, NULL);
        return false;
    }
    gGLContextCount++;

    // Create a BufferQueue with a fake consumer
    sp<BufferQueue> bq = new BufferQueue();
    bq->consumerConnect(new DummyConsumer());
    sp<Surface> stc(new Surface(static_cast<sp<IGraphicBufferProducer> >(bq)));

#ifdef IMX5_RS_FIXUP
    /* Store the BufferQueue to disconnect the dummy consumer before destroying surfaceDefault */
    dc->gl.egl.bufferqueue = bq;
    /* Set the MaxBuffer Count to 3 as the opengl needs 3 buffers */
    dc->gl.egl.bufferqueue->setDefaultMaxBufferCount(3);
#endif

    dc->gl.egl.surfaceDefault = eglCreateWindowSurface(dc->gl.egl.display, dc->gl.egl.config,
                                                       static_cast<ANativeWindow*>(stc.get()),
                                                       NULL);

    checkEglError("eglCreateWindowSurface");
    if (dc->gl.egl.surfaceDefault == EGL_NO_SURFACE) {
        ALOGE("eglCreateWindowSurface returned EGL_NO_SURFACE");
        rsdGLShutdown(rsc);
        rsc->setWatchdogGL(NULL, 0, NULL);
        return false;
    }

    rsc->setWatchdogGL("eglMakeCurrent", __LINE__, __FILE__);
    ret = eglMakeCurrent(dc->gl.egl.display, dc->gl.egl.surfaceDefault,
                         dc->gl.egl.surfaceDefault, dc->gl.egl.context);
    if (ret == EGL_FALSE) {
        ALOGE("eglMakeCurrent returned EGL_FALSE");
        checkEglError("eglMakeCurrent", ret);
        rsdGLShutdown(rsc);
        rsc->setWatchdogGL(NULL, 0, NULL);
        return false;
    }

    dc->gl.gl.version = glGetString(GL_VERSION);
    dc->gl.gl.vendor = glGetString(GL_VENDOR);
    dc->gl.gl.renderer = glGetString(GL_RENDERER);
    dc->gl.gl.extensions = glGetString(GL_EXTENSIONS);

    //ALOGV("EGL Version %i %i", mEGL.mMajorVersion, mEGL.mMinorVersion);
    //ALOGV("GL Version %s", mGL.mVersion);
    //ALOGV("GL Vendor %s", mGL.mVendor);
    //ALOGV("GL Renderer %s", mGL.mRenderer);
    //ALOGV("GL Extensions %s", mGL.mExtensions);

    const char *verptr = NULL;
    if (strlen((const char *)dc->gl.gl.version) > 9) {
        if (!memcmp(dc->gl.gl.version, "OpenGL ES-CM", 12)) {
            verptr = (const char *)dc->gl.gl.version + 12;
        }
        if (!memcmp(dc->gl.gl.version, "OpenGL ES ", 10)) {
            verptr = (const char *)dc->gl.gl.version + 9;
        }
    }

    if (!verptr) {
        ALOGE("Error, OpenGL ES Lite not supported");
        rsdGLShutdown(rsc);
        rsc->setWatchdogGL(NULL, 0, NULL);
        return false;
    } else {
        sscanf(verptr, " %i.%i", &dc->gl.gl.majorVersion, &dc->gl.gl.minorVersion);
    }

    glGetIntegerv(GL_MAX_VERTEX_ATTRIBS, &dc->gl.gl.maxVertexAttribs);
    glGetIntegerv(GL_MAX_VERTEX_UNIFORM_VECTORS, &dc->gl.gl.maxVertexUniformVectors);
    glGetIntegerv(GL_MAX_VERTEX_TEXTURE_IMAGE_UNITS, &dc->gl.gl.maxVertexTextureUnits);

    glGetIntegerv(GL_MAX_VARYING_VECTORS, &dc->gl.gl.maxVaryingVectors);
    glGetIntegerv(GL_MAX_COMBINED_TEXTURE_IMAGE_UNITS, &dc->gl.gl.maxTextureImageUnits);

    glGetIntegerv(GL_MAX_TEXTURE_IMAGE_UNITS, &dc->gl.gl.maxFragmentTextureImageUnits);
    glGetIntegerv(GL_MAX_FRAGMENT_UNIFORM_VECTORS, &dc->gl.gl.maxFragmentUniformVectors);

    dc->gl.gl.OES_texture_npot = NULL != strstr((const char *)dc->gl.gl.extensions,
                                                "GL_OES_texture_npot");
    dc->gl.gl.IMG_texture_npot = NULL != strstr((const char *)dc->gl.gl.extensions,
                                                   "GL_IMG_texture_npot");
    dc->gl.gl.NV_texture_npot_2D_mipmap = NULL != strstr((const char *)dc->gl.gl.extensions,
                                                            "GL_NV_texture_npot_2D_mipmap");
    dc->gl.gl.EXT_texture_max_aniso = 1.0f;
    bool hasAniso = NULL != strstr((const char *)dc->gl.gl.extensions,
                                   "GL_EXT_texture_filter_anisotropic");
    if (hasAniso) {
        glGetFloatv(GL_MAX_TEXTURE_MAX_ANISOTROPY_EXT, &dc->gl.gl.EXT_texture_max_aniso);
    }

    if (0) {
        DumpDebug(dc);
    }

    dc->gl.shaderCache = new RsdShaderCache();
    dc->gl.vertexArrayState = new RsdVertexArrayState();
    dc->gl.vertexArrayState->init(dc->gl.gl.maxVertexAttribs);
    dc->gl.currentFrameBuffer = NULL;
    dc->mHasGraphics = true;

    ALOGV("%p initGLThread end", rsc);
    rsc->setWatchdogGL(NULL, 0, NULL);
    return true;
}


bool rsdGLSetInternalSurface(const Context *rsc, RsNativeWindow sur) {
    RsdHal *dc = (RsdHal *)rsc->mHal.drv;

    EGLBoolean ret;
    if (dc->gl.egl.surface != NULL) {
        rsc->setWatchdogGL("eglMakeCurrent", __LINE__, __FILE__);
        ret = eglMakeCurrent(dc->gl.egl.display, dc->gl.egl.surfaceDefault,
                             dc->gl.egl.surfaceDefault, dc->gl.egl.context);
        checkEglError("eglMakeCurrent", ret);

        rsc->setWatchdogGL("eglDestroySurface", __LINE__, __FILE__);
        ret = eglDestroySurface(dc->gl.egl.display, dc->gl.egl.surface);
        checkEglError("eglDestroySurface", ret);

        dc->gl.egl.surface = NULL;
    }

    if (dc->gl.currentWndSurface != NULL) {
        dc->gl.currentWndSurface->decStrong(NULL);
    }

    dc->gl.currentWndSurface = (ANativeWindow *)sur;
    if (dc->gl.currentWndSurface != NULL) {
        dc->gl.currentWndSurface->incStrong(NULL);

        rsc->setWatchdogGL("eglCreateWindowSurface", __LINE__, __FILE__);
        dc->gl.egl.surface = eglCreateWindowSurface(dc->gl.egl.display, dc->gl.egl.config,
                                                    dc->gl.currentWndSurface, NULL);
        checkEglError("eglCreateWindowSurface");
        if (dc->gl.egl.surface == EGL_NO_SURFACE) {
            ALOGE("eglCreateWindowSurface returned EGL_NO_SURFACE");
        }

        rsc->setWatchdogGL("eglMakeCurrent", __LINE__, __FILE__);
        ret = eglMakeCurrent(dc->gl.egl.display, dc->gl.egl.surface,
                             dc->gl.egl.surface, dc->gl.egl.context);
        checkEglError("eglMakeCurrent", ret);
    }
    rsc->setWatchdogGL(NULL, 0, NULL);
    return true;
}

bool rsdGLSetSurface(const Context *rsc, uint32_t w, uint32_t h, RsNativeWindow sur) {
    RsdHal *dc = (RsdHal *)rsc->mHal.drv;

    if (dc->gl.wndSurface != NULL) {
        dc->gl.wndSurface->decStrong(NULL);
        dc->gl.wndSurface = NULL;
    }
    if(w && h) {
        // WAR: Some drivers fail to handle 0 size surfaces correctly. Use the
        // pbuffer to avoid this pitfall.
        dc->gl.wndSurface = (ANativeWindow *)sur;
        if (dc->gl.wndSurface != NULL) {
            dc->gl.wndSurface->incStrong(NULL);
        }
    }

    return rsdGLSetInternalSurface(rsc, sur);
}

void rsdGLSwap(const android::renderscript::Context *rsc) {
    RsdHal *dc = (RsdHal *)rsc->mHal.drv;
    RSD_CALL_GL(eglSwapBuffers, dc->gl.egl.display, dc->gl.egl.surface);
}

void rsdGLSetPriority(const Context *rsc, int32_t priority) {
    if (priority > 0) {
        // Mark context as low priority.
        ALOGV("low pri");
    } else {
        ALOGV("normal pri");
    }
}

void rsdGLCheckError(const android::renderscript::Context *rsc,
                     const char *msg, bool isFatal) {
    GLenum err = glGetError();
    if (err != GL_NO_ERROR) {
        char buf[1024];
        snprintf(buf, sizeof(buf), "GL Error = 0x%08x, from: %s", err, msg);

        if (isFatal) {
            rsc->setError(RS_ERROR_FATAL_DRIVER, buf);
        } else {
            switch (err) {
            case GL_OUT_OF_MEMORY:
                rsc->setError(RS_ERROR_OUT_OF_MEMORY, buf);
                break;
            default:
                rsc->setError(RS_ERROR_DRIVER, buf);
                break;
            }
        }

        ALOGE("%p, %s", rsc, buf);
    }

}

void rsdGLClearColor(const android::renderscript::Context *rsc,
                     float r, float g, float b, float a) {
    RSD_CALL_GL(glClearColor, r, g, b, a);
    RSD_CALL_GL(glClear, GL_COLOR_BUFFER_BIT);
}

void rsdGLClearDepth(const android::renderscript::Context *rsc, float v) {
    RSD_CALL_GL(glClearDepthf, v);
    RSD_CALL_GL(glClear, GL_DEPTH_BUFFER_BIT);
}

void rsdGLFinish(const android::renderscript::Context *rsc) {
    RSD_CALL_GL(glFinish);
}

void rsdGLDrawQuadTexCoords(const android::renderscript::Context *rsc,
                            float x1, float y1, float z1, float u1, float v1,
                            float x2, float y2, float z2, float u2, float v2,
                            float x3, float y3, float z3, float u3, float v3,
                            float x4, float y4, float z4, float u4, float v4) {

    float vtx[] = {x1,y1,z1, x2,y2,z2, x3,y3,z3, x4,y4,z4};
    const float tex[] = {u1,v1, u2,v2, u3,v3, u4,v4};

    RsdVertexArray::Attrib attribs[2];
    attribs[0].set(GL_FLOAT, 3, 12, false, (uint32_t)vtx, "ATTRIB_position");
    attribs[1].set(GL_FLOAT, 2, 8, false, (uint32_t)tex, "ATTRIB_texture0");

    RsdVertexArray va(attribs, 2);
    va.setup(rsc);

    RSD_CALL_GL(glDrawArrays, GL_TRIANGLE_FAN, 0, 4);
}
