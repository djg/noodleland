#define ATRACE_TAG ATRACE_TAG_ALWAYS
#define LOG_TAG "Noodleland"

#include <string.h>

#include <EGL/egl.h>
#include <EGL/eglext.h>
#include <GLES2/gl2.h>
#include <GLES2/gl2ext.h>

#include <hardware/hwcomposer.h>

#include <ui/FramebufferNativeWindow.h>
#include <ui/GraphicBuffer.h>
#include <ui/PixelFormat.h>
#include <ui/Region.h>

#include <utils/Log.h>

#undef NOODLE_DEBUG_CLEAR
#undef NOODLE_DUMP_HWC_LIST
#define NUMA(a) (sizeof(a) / sizeof(a [0]))

using namespace android;

sp<FramebufferNativeWindow> gNativeWindow;
int gWidth;
int gHeight;

struct HwcLayerCount {
    int NumFBLayers, NumOVLayers;
};


const hw_module_t* gHwcModule;
hwc_composer_device_t* gHwc;
hwc_display_t gHwcDisplay;
hwc_surface_t gHwcSurface;
hwc_procs_t gHwcProcs;
hwc_layer_list_t* gHwcList;

#define SIZE 4096
char tempbuffer[SIZE] = { 0, };

static void
hwc_invalidate_cb(hwc_procs_t*) {
    LOGE(HWC_HARDWARE_COMPOSER " invalidate");
}


static status_t
hwc_create_work_list(size_t aLayerCount) {
    if (!gHwc)
        return NO_ERROR;

    if (!gHwcList || gHwcList->numHwLayers < aLayerCount) {
        free(gHwcList);
        size_t sizeInBytes = sizeof(hwc_layer_list_t) + aLayerCount * sizeof(hwc_layer_t);
        gHwcList = (hwc_layer_list_t*) malloc(sizeInBytes);
    }

    gHwcList->flags = HWC_GEOMETRY_CHANGED;
    gHwcList->numHwLayers = aLayerCount;

    return NO_ERROR;
}


static status_t
hwc_prepare_list(HwcLayerCount* aLayerCount) {
    status_t err = gHwc->prepare(gHwc, gHwcList);
    if (err) {
        LOGE("hwc prepare error: %s", strerror(-err));
        return err;
    }

    if (!aLayerCount)
        return NO_ERROR;

    aLayerCount->NumFBLayers = 0;
    aLayerCount->NumOVLayers = 0;

    const size_t layerCount = gHwcList->numHwLayers;
    for (size_t n = 0; n < layerCount; n++) {
        hwc_layer_t& l = gHwcList->hwLayers[n];
        if (l.flags & HWC_SKIP_LAYER) {
            l.compositionType = HWC_FRAMEBUFFER;
        }

        if (l.compositionType) {
            aLayerCount->NumOVLayers += 1;
        } else {
            aLayerCount->NumFBLayers += 1;
        }
    }

    return NO_ERROR;
}


static status_t
hwc_commit() {
    status_t err = gHwc->set(gHwc, gHwcDisplay, gHwcSurface, gHwcList);
    if (gHwcList) {
        gHwcList->flags &= ~HWC_GEOMETRY_CHANGED;
    }

    return err;
}


static GraphicBuffer*
create_composible_buffer(int width, int height) {
    uint32_t bufferFlags = GraphicBuffer::USAGE_SW_WRITE_RARELY | GraphicBuffer::USAGE_HW_COMPOSER;
    GraphicBuffer* buffer = new GraphicBuffer(width, height, PIXEL_FORMAT_RGBA_8888, bufferFlags);

    status_t err = buffer->initCheck();
    if (err) {
        LOGE("error creating GraphicBuffer: %s", strerror(-err));
        return NULL;
    }

    Rect rect = buffer->getBounds();
    LOGE("GraphicBuffer information");
    LOGE("width : %d", buffer->getWidth());
    LOGE("height: %d", buffer->getHeight());
    LOGE("stride: %d", buffer->getStride());
    LOGE("usage : 0x%08x", buffer->getUsage());
    LOGE("format: %d", buffer->getPixelFormat());
    LOGE("bounds: (%d,%d) - (%d,%d)", rect.left, rect.top, rect.right, rect.bottom);

    return buffer;
}


static void
checkerboard_fill(sp<GraphicBuffer>& buffer, int width, int height, uint32_t color) {
    uint32_t* bits = NULL;
    status_t err = buffer->lock(GraphicBuffer::USAGE_SW_WRITE_RARELY, (void**) &bits);

    if (!bits) {
        LOGE("error locking graphic buffer: %s", strerror(-err));
        return;
    }

    for (int row = 0; row < width; row++) {
        for (int col = 0; col < height; col++) {
            uint32_t value = color * (((row & 0x10) == 0) ^ ((col & 0x10) == 0));
            *bits++ = value;
        }
    }

    buffer->unlock();
}


static status_t
selectConfigForPixelFormat(EGLDisplay dpy,
                          EGLint const* attrs,
                          PixelFormat format,
                          EGLConfig* outConfig) {
    EGLConfig config = NULL;
    EGLint numConfigs = -1, n=0;
    eglGetConfigs(dpy, NULL, 0, &numConfigs);
    EGLConfig* const configs = new EGLConfig[numConfigs];
    eglChooseConfig(dpy, attrs, configs, numConfigs, &n);
    for (int i=0 ; i<n ; i++) {
        EGLint nativeVisualId = 0;
        eglGetConfigAttrib(dpy, configs[i], EGL_NATIVE_VISUAL_ID, &nativeVisualId);
        if (nativeVisualId>0 && format == nativeVisualId) {
            *outConfig = configs[i];
            delete [] configs;
            return NO_ERROR;
        }
    }
    delete [] configs;
    return NAME_NOT_FOUND;
}


typedef struct sprite {
    Rect rect;
    int dirX;
    uint32_t color;
    sp<GraphicBuffer> buffer;
} sprite_t;


sprite_t test_sprites[] = {
    { Rect(  0,   0,     128,     128), 1, 0xFF0000FF, NULL },
    { Rect( 64,  64,  64+128,  64+128), 1, 0xFF00FF00, NULL },
    { Rect(128, 128, 128+128, 128+128), 1, 0xFFFF0000, NULL },
    { Rect(192, 192, 192+128, 192+128), 1, 0xFF00FFFF, NULL },
    { Rect(256, 256, 256+128, 256+128), 1, 0xFFFF00FF, NULL },
    { Rect(320, 320, 320+128, 320+128),-1, 0xFFFFFF00, NULL },
    { Rect(256, 384, 256+128, 384+128),-1, 0xFFFFFFFF, NULL },
};


static void
hwc_layer_set_geometry(hwc_layer_t& l, Rect& r) {
    l.sourceCrop.left = 0;
    l.sourceCrop.top = 0;
    l.sourceCrop.right = r.width();
    l.sourceCrop.bottom = r.height();
    l.displayFrame.left = r.left;
    l.displayFrame.top = r.top;
    l.displayFrame.right = r.right;
    l.displayFrame.bottom = r.bottom;
}

static void
create_sprites() {
    for (size_t n = 0; n < NUMA(test_sprites); n++) {
        sprite_t& s = test_sprites[n];

        int w = s.rect.width();
        int h = s.rect.height();
        s.buffer = create_composible_buffer(w, h);
        checkerboard_fill(s.buffer, w, h, s.color);
    }
}

static void
update_sprites() {
    for (size_t n = 0; n < NUMA(test_sprites); n++) {
        sprite_t& s = test_sprites[n];

        hwc_layer_t& l = gHwcList->hwLayers[n];
        l.compositionType = HWC_FRAMEBUFFER;
        l.handle = s.buffer->getNativeBuffer()->handle;
        l.blending = HWC_BLENDING_NONE;
        hwc_layer_set_geometry(l, s.rect);
        l.visibleRegionScreen.numRects = 1;
        l.visibleRegionScreen.rects = &l.displayFrame;

        s.rect.offsetBy(s.dirX, 0);
        if ((s.rect.left < -s.rect.width() && s.dirX < 0) ||
            (s.rect.right >= gWidth + s.rect.width() && s.dirX > 0))
        {
            s.dirX = -s.dirX;
        }
    }

    gHwcList->flags = HWC_GEOMETRY_CHANGED;
}


int
main(int argc, char** argv) {
    LOGE("*** Noodleland ***");

    gNativeWindow = new FramebufferNativeWindow();
    const framebuffer_device_t* fbDev = gNativeWindow->getDevice();
    if (!fbDev) {
        LOGE("Display subsystem failed to initialise. check logs. exiting...");
        return 0;
    }

    const ANativeWindow* window = gNativeWindow.get();
    int width = 0, height = 0, format = 0;
    window->query(window, NATIVE_WINDOW_WIDTH, &width);
    window->query(window, NATIVE_WINDOW_HEIGHT, &height);
    window->query(window, NATIVE_WINDOW_FORMAT, &format);
    LOGE("width = %d", width);
    LOGE("height = %d", height);
    LOGE("format = 0x%08X", format);
    LOGE("xdpi = %f", window->xdpi);
    LOGE("ydpi = %f", window->ydpi);

    EGLint numConfigs;
    EGLDisplay display = eglGetDisplay(EGL_DEFAULT_DISPLAY);
    eglInitialize(display, NULL, NULL);
    eglGetConfigs(display, NULL, 0, &numConfigs);

    EGLint attribs[] = {
        EGL_SURFACE_TYPE, EGL_WINDOW_BIT,
        EGL_NONE
    };
    EGLConfig config = NULL;
    status_t err = selectConfigForPixelFormat(display, attribs, format, &config);
    LOGE_IF(err, "couldn't find an EGLConfig matching the screen format");

    EGLint r,g,b,a;
    eglGetConfigAttrib(display, config, EGL_RED_SIZE,   &r);
    eglGetConfigAttrib(display, config, EGL_GREEN_SIZE, &g);
    eglGetConfigAttrib(display, config, EGL_BLUE_SIZE,  &b);
    eglGetConfigAttrib(display, config, EGL_ALPHA_SIZE, &a);
    LOGE("r = %d, g = %d, b = %d, a = %d", r, g, b, a);

    EGLSurface surface = eglCreateWindowSurface(display, config, gNativeWindow.get(), NULL);
    eglQuerySurface(display, surface, EGL_WIDTH, &gWidth);
    eglQuerySurface(display, surface, EGL_HEIGHT, &gHeight);

    if (gNativeWindow->isUpdateOnDemand()) {
        LOGE("*** PARTIAL UPDATES");
        eglSurfaceAttrib(display, surface, EGL_SWAP_BEHAVIOR, EGL_BUFFER_DESTROYED);
    }

    EGLint contextAttribs[] = {
        EGL_NONE
    };
    EGLContext context = eglCreateContext(display, config, NULL, contextAttribs);

    EGLBoolean result;
    result = eglMakeCurrent(display, surface, surface, context);
    if (!result) {
        LOGE("Couldn't create a working GLES context. check logs. exiting...");
        return 0;
    }

    LOGE("EGL information");
    LOGE("# of configs: %d", numConfigs);
    LOGE("vendor      : %s", eglQueryString(display, EGL_VENDOR));
    LOGE("version     : %s", eglQueryString(display, EGL_VERSION));
    LOGE("extensions  : %s", eglQueryString(display, EGL_EXTENSIONS));
    LOGE("client API  : %s", eglQueryString(display, EGL_CLIENT_APIS) ?: "Not Supported");
    LOGE("EGL surface : %d-%d-%d-%d, config=%p", r, g, b, a, config);

    LOGE("OpenGL information");
    LOGE("vendor       : %s", glGetString(GL_VENDOR));
    LOGE("renderer     : %s", glGetString(GL_RENDERER));
    LOGE("version      : %s", glGetString(GL_VERSION));
    LOGE("extensions   : %s", glGetString(GL_EXTENSIONS));

    /* AMD Performance Counters */
    GLint num_groups;
    char buffer[512];
    glGetPerfMonitorGroupsAMD(&num_groups, 0, NULL);
    for (int group = 0; group < num_groups; group++) {
        glGetPerfMonitorGroupStringAMD(group, 512, NULL, buffer);
        LOGE("Perf Monitor Group [%d]: %s", group, buffer);

        GLint num_counters, max_active_counters;
        glGetPerfMonitorCountersAMD(group, &num_counters, &max_active_counters, 0, NULL);
        LOGE("  Counters #: %d, Max Active: %d", num_counters, max_active_counters);
        GLuint* counters = new GLuint[num_counters];
        glGetPerfMonitorCountersAMD(group, NULL, NULL, num_counters, counters);

        for (int c = 0; c < num_counters; c++) {
            GLuint counter = counters[c];
            glGetPerfMonitorCounterStringAMD(group, counter, 512, NULL, buffer);
            LOGE("  %s (%u/0x%08x)", buffer, counter, counter);
        }

        delete [] counters;
    }


    err = hw_get_module(HWC_HARDWARE_MODULE_ID, &gHwcModule);
    if (err) {
        LOGE(HWC_HARDWARE_MODULE_ID " module not found.");
        return 0;
    }

    err = hwc_open(gHwcModule, &gHwc);
    if (err) {
        LOGE(HWC_HARDWARE_COMPOSER " device failed to initialize (%s)", strerror(-err));
        return 0;
    }

    if (gHwc->registerProcs) {
        gHwcProcs.invalidate = hwc_invalidate_cb;
        gHwc->registerProcs(gHwc, &gHwcProcs);
    }

    gHwcDisplay = (hwc_display_t) display;
    gHwcSurface = (hwc_surface_t) surface;


    err = hwc_create_work_list(NUMA(test_sprites));
    if (err)
        return 0;

    create_sprites();

    uint32_t flipCount = 0;

    for (;;)
    {
        hwc_layer_t& ll = gHwcList->hwLayers[0];

        update_sprites();

        HwcLayerCount layerCounts;
        err = hwc_prepare_list(&layerCounts);
        if (err)
            return 0;

#ifdef NOODLE_DUMP_HWC_LIST
        LOGE("HWC state: %d", flipCount);
        LOGE("Num OV: %d", layerCounts.NumOVLayers);
        LOGE("Num FB: %d", layerCounts.NumFBLayers);

        LOGE("  numHwLayers=%u, flags=0x%08x", gHwcList->numHwLayers, gHwcList->flags);
        LOGE("   type   |  handle  |   hints  |   flags  | tr | blend |  format  |       source crop         |           frame           name");
        LOGE("----------+----------+----------+----------+----+-------+----------+---------------------------+--------------------------------");

        uint32_t format = PIXEL_FORMAT_RGBA_8888;
        for (size_t n = 0; n < gHwcList->numHwLayers; n++) {
            const hwc_layer_t& l = gHwcList->hwLayers[n];

            LOGE(" %8s | %08x | %08x | %08x | %02x | %05x | %08x | [%5d,%5d,%5d,%5d] | [%5d,%5d,%5d,%5d] %s",
                 l.compositionType ? "OVERLAY" : "FB",
                 intptr_t(l.handle), l.hints, l.flags, l.transform, l.blending, format,
                 l.sourceCrop.left, l.sourceCrop.top, l.sourceCrop.right, l.sourceCrop.bottom,
                 l.displayFrame.left, l.displayFrame.top, l.displayFrame.right, l.displayFrame.bottom,
                 "buffer");
        }

        if (gHwc->common.version >= 1 && gHwc->dump) {
            gHwc->dump(gHwc, tempbuffer, SIZE);
            LOGE("%s", tempbuffer);
        }
#endif

#ifdef NOODLE_DEBUG_CLEAR
        if (flipCount & 0x1) {
            glClearColor(1,0,0,0);
        } else {
            glClearColor(0,1,0,0);
        }
#else
        glClearColor(0,0,0,0);
#endif
        glClear(GL_COLOR_BUFFER_BIT);

        gNativeWindow->compositionComplete();

        err = hwc_commit(); // Flip
        if (err) {
            LOGE("hwc commit failed: %s", strerror(-err));
            return 0;
        }

        flipCount += 1;
    }

    return 0;
}