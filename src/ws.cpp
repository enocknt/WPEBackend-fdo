/*
 * Copyright (C) 2017, 2018 Igalia S.L.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in the
 *    documentation and/or other materials provided with the distribution.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
 * "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
 * LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A
 * PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT
 * HOLDER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL,
 * SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT
 * LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE,
 * DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY
 * THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
 * (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
 * OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 */

#include "ws.h"

#include <EGL/egl.h>
#include <EGL/eglext.h>
#include "linux-dmabuf/linux-dmabuf.h"
#include "bridge/wpe-bridge-server-protocol.h"
#include <cassert>
#include <sys/socket.h>
#include <unistd.h>

#ifndef EGL_WL_bind_wayland_display
typedef EGLBoolean (EGLAPIENTRYP PFNEGLBINDWAYLANDDISPLAYWL) (EGLDisplay dpy, struct wl_display *display);
#endif

#ifndef EGL_EXT_image_dma_buf_import
#define EGL_LINUX_DMA_BUF_EXT             0x3270
#define EGL_DMA_BUF_PLANE0_FD_EXT         0x3272
#define EGL_DMA_BUF_PLANE0_OFFSET_EXT     0x3273
#define EGL_DMA_BUF_PLANE0_PITCH_EXT      0x3274
#define EGL_DMA_BUF_PLANE1_FD_EXT         0x3275
#define EGL_DMA_BUF_PLANE1_OFFSET_EXT     0x3276
#define EGL_DMA_BUF_PLANE1_PITCH_EXT      0x3277
#define EGL_DMA_BUF_PLANE2_FD_EXT         0x3278
#define EGL_DMA_BUF_PLANE2_OFFSET_EXT     0x3279
#define EGL_DMA_BUF_PLANE2_PITCH_EXT      0x327A
#endif /* EGL_EXT_image_dma_buf_import */

#ifndef EGL_EXT_image_dma_buf_import_modifiers
#define EGL_DMA_BUF_PLANE3_FD_EXT         0x3440
#define EGL_DMA_BUF_PLANE3_OFFSET_EXT     0x3441
#define EGL_DMA_BUF_PLANE3_PITCH_EXT      0x3442
#define EGL_DMA_BUF_PLANE0_MODIFIER_LO_EXT 0x3443
#define EGL_DMA_BUF_PLANE0_MODIFIER_HI_EXT 0x3444
#define EGL_DMA_BUF_PLANE1_MODIFIER_LO_EXT 0x3445
#define EGL_DMA_BUF_PLANE1_MODIFIER_HI_EXT 0x3446
#define EGL_DMA_BUF_PLANE2_MODIFIER_LO_EXT 0x3447
#define EGL_DMA_BUF_PLANE2_MODIFIER_HI_EXT 0x3448
#define EGL_DMA_BUF_PLANE3_MODIFIER_LO_EXT 0x3449
#define EGL_DMA_BUF_PLANE3_MODIFIER_HI_EXT 0x344A
#endif /* EGL_EXT_image_dma_buf_import_modifiers */

static PFNEGLBINDWAYLANDDISPLAYWL eglBindWaylandDisplayWL;
static PFNEGLCREATEIMAGEKHRPROC eglCreateImageKHR;
static PFNEGLDESTROYIMAGEKHRPROC eglDestroyImageKHR;

namespace WS {

struct Source {
    static GSourceFuncs s_sourceFuncs;

    GSource source;
    GPollFD pfd;
    struct wl_display* display;
};

GSourceFuncs Source::s_sourceFuncs = {
    // prepare
    [](GSource* base, gint* timeout) -> gboolean
    {
        auto& source = *reinterpret_cast<Source*>(base);
        *timeout = -1;
        wl_display_flush_clients(source.display);
        return FALSE;
    },
    // check
    [](GSource* base) -> gboolean
    {
        auto& source = *reinterpret_cast<Source*>(base);
        return !!source.pfd.revents;
    },
    // dispatch
    [](GSource* base, GSourceFunc, gpointer) -> gboolean
    {
        auto& source = *reinterpret_cast<Source*>(base);

        if (source.pfd.revents & G_IO_IN) {
            struct wl_event_loop* eventLoop = wl_display_get_event_loop(source.display);
            wl_event_loop_dispatch(eventLoop, -1);
            wl_display_flush_clients(source.display);
        }

        if (source.pfd.revents & (G_IO_ERR | G_IO_HUP))
            return FALSE;

        source.pfd.revents = 0;
        return TRUE;
    },
    nullptr, // finalize
    nullptr, // closure_callback
    nullptr, // closure_marshall
};

struct Surface {
    uint32_t id { 0 };
    struct wl_client* client { nullptr };

    ExportableClient* exportableClient { nullptr };

    struct wl_resource* bufferResource { nullptr };
    const struct linux_dmabuf_buffer* dmabufBuffer { nullptr };
};

static const struct wl_surface_interface s_surfaceInterface = {
    // destroy
    [](struct wl_client*, struct wl_resource*) { },
    // attach
    [](struct wl_client*, struct wl_resource* surfaceResource, struct wl_resource* bufferResource, int32_t, int32_t)
    {
        auto& surface = *static_cast<Surface*>(wl_resource_get_user_data(surfaceResource));

        surface.dmabufBuffer = linux_dmabuf_get_buffer(bufferResource);

        if (surface.bufferResource)
            wl_buffer_send_release(surface.bufferResource);
        surface.bufferResource = bufferResource;
    },
    // damage
    [](struct wl_client*, struct wl_resource*, int32_t, int32_t, int32_t, int32_t) { },
    // frame
    [](struct wl_client* client, struct wl_resource* surfaceResource, uint32_t callback)
    {
        auto& surface = *static_cast<Surface*>(wl_resource_get_user_data(surfaceResource));
        if (!surface.exportableClient)
            return;

        struct wl_resource* callbackResource = wl_resource_create(client, &wl_callback_interface, 1, callback);
        if (!callbackResource) {
            wl_resource_post_no_memory(surfaceResource);
            return;
        }

        wl_resource_set_implementation(callbackResource, nullptr, nullptr, nullptr);
        surface.exportableClient->frameCallback(callbackResource);
    },
    // set_opaque_region
    [](struct wl_client*, struct wl_resource*, struct wl_resource*) { },
    // set_input_region
    [](struct wl_client*, struct wl_resource*, struct wl_resource*) { },
    // commit
    [](struct wl_client*, struct wl_resource* surfaceResource)
    {
        auto& surface = *static_cast<Surface*>(wl_resource_get_user_data(surfaceResource));
        if (!surface.exportableClient)
            return;

        if (surface.dmabufBuffer) {
            surface.exportableClient->exportLinuxDmabuf(surface.dmabufBuffer);
        } else {
            struct wl_resource* bufferResource = surface.bufferResource;
            surface.bufferResource = nullptr;
            surface.exportableClient->exportBufferResource(bufferResource);
        }
    },
    // set_buffer_transform
    [](struct wl_client*, struct wl_resource*, int32_t) { },
    // set_buffer_scale
    [](struct wl_client*, struct wl_resource*, int32_t) { },
#ifdef WAYLAND_1_10_OR_GREATER
    // damage_buffer
    [](struct wl_client*, struct wl_resource*, int32_t, int32_t, int32_t, int32_t) { },
#endif
};

static const struct wl_compositor_interface s_compositorInterface = {
    // create_surface
    [](struct wl_client* client, struct wl_resource* resource, uint32_t id)
    {
        struct wl_resource* surfaceResource = wl_resource_create(client, &wl_surface_interface,
            wl_resource_get_version(resource), id);
        if (!surfaceResource) {
            wl_resource_post_no_memory(resource);
            return;
        }

        auto* surface = new Surface;
        surface->client = client;
        surface->id = id;
        wl_resource_set_implementation(surfaceResource, &s_surfaceInterface, surface,
            [](struct wl_resource* resource)
            {
                auto* surface = static_cast<Surface*>(wl_resource_get_user_data(resource));
                delete surface;
            });
    },
    // create_region
    [](struct wl_client*, struct wl_resource*, uint32_t) { },
};

static const struct wpe_bridge_interface s_wpeBridgeInterface = {
    // connect
    [](struct wl_client*, struct wl_resource* resource, struct wl_resource* surfaceResource)
    {
        auto* surface = static_cast<Surface*>(wl_resource_get_user_data(surfaceResource));
        if (!surface)
            return;

        static uint32_t bridgeID = 0;
        ++bridgeID;
        wpe_bridge_send_connected(resource, bridgeID);
        Instance::singleton().createSurface(bridgeID, surface);
    },
};

Instance& Instance::singleton()
{
    static Instance* s_singleton;
    if (!s_singleton)
        s_singleton = new Instance;
    return *s_singleton;
}

Instance::Instance()
    : m_display(wl_display_create())
    , m_source(g_source_new(&Source::s_sourceFuncs, sizeof(Source)))
    , m_eglDisplay(EGL_NO_DISPLAY)
{
    m_compositor = wl_global_create(m_display, &wl_compositor_interface, 3, this,
        [](struct wl_client* client, void*, uint32_t version, uint32_t id)
        {
            struct wl_resource* resource = wl_resource_create(client, &wl_compositor_interface, version, id);
            if (!resource) {
                wl_client_post_no_memory(client);
                return;
            }

            wl_resource_set_implementation(resource, &s_compositorInterface, nullptr, nullptr);
        });
    m_wpeBridge = wl_global_create(m_display, &wpe_bridge_interface, 1, this,
        [](struct wl_client* client, void*, uint32_t version, uint32_t id)
        {
            struct wl_resource* resource = wl_resource_create(client, &wpe_bridge_interface, version, id);
            if (!resource) {
                wl_client_post_no_memory(client);
                return;
            }

            wl_resource_set_implementation(resource, &s_wpeBridgeInterface, nullptr, nullptr);
        });

    auto& source = *reinterpret_cast<Source*>(m_source);

    struct wl_event_loop* eventLoop = wl_display_get_event_loop(m_display);
    source.pfd.fd = wl_event_loop_get_fd(eventLoop);
    source.pfd.events = G_IO_IN | G_IO_ERR | G_IO_HUP;
    source.pfd.revents = 0;
    source.display = m_display;

    g_source_add_poll(m_source, &source.pfd);
    g_source_set_name(m_source, "WPEBackend-fdo::Host");
    g_source_set_priority(m_source, -70);
    g_source_set_can_recurse(m_source, TRUE);
    g_source_attach(m_source, g_main_context_get_thread_default());
}

Instance::~Instance()
{
    linux_dmabuf_teardown();

    if (m_source) {
        g_source_destroy(m_source);
        g_source_unref(m_source);
    }

    if (m_display)
        wl_display_destroy(m_display);
}

static bool isEGLExtensionSupported(const char* extensionList, const char* extension)
{
    if (!extensionList)
        return false;

    int extensionLen = strlen(extension);
    const char* extensionListPtr = extensionList;
    while ((extensionListPtr = strstr(extensionListPtr, extension))) {
        if (extensionListPtr[extensionLen] == ' ' || extensionListPtr[extensionLen] == '\0')
            return true;
	extensionListPtr += extensionLen;
    }
    return false;
}

bool Instance::initialize(EGLDisplay eglDisplay)
{
    if (m_eglDisplay == eglDisplay)
        return true;

    if (m_eglDisplay != EGL_NO_DISPLAY) {
        fprintf(stderr, "WPE fdo doesn't support multiple EGL displays\n");
        return false;
    }

    const char* extensions = eglQueryString(eglDisplay, EGL_EXTENSIONS);
    if (isEGLExtensionSupported(extensions, "EGL_WL_bind_wayland_display"))
        eglBindWaylandDisplayWL = reinterpret_cast<PFNEGLBINDWAYLANDDISPLAYWL>(eglGetProcAddress("eglBindWaylandDisplayWL"));
    if (!eglBindWaylandDisplayWL)
        return false;

    if (isEGLExtensionSupported(extensions, "EGL_KHR_image_base")) {
        eglCreateImageKHR = reinterpret_cast<PFNEGLCREATEIMAGEKHRPROC>(eglGetProcAddress("eglCreateImageKHR"));
        eglDestroyImageKHR = reinterpret_cast<PFNEGLDESTROYIMAGEKHRPROC>(eglGetProcAddress("eglDestroyImageKHR"));
    }
    if (!eglCreateImageKHR || !eglDestroyImageKHR)
        return false;

    if (!eglBindWaylandDisplayWL(eglDisplay, m_display))
        return false;

    m_eglDisplay = eglDisplay;

    /* Initialize Linux dmabuf subsystem. */
    linux_dmabuf_setup(m_display, eglDisplay);

    return true;
}

int Instance::createClient()
{
    if (m_eglDisplay == EGL_NO_DISPLAY)
        return -1;

    int pair[2];
    if (socketpair(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0, pair) < 0)
        return -1;

    int clientFd = dup(pair[1]);
    close(pair[1]);

    wl_client_create(m_display, pair[0]);
    return clientFd;
}

void Instance::createSurface(uint32_t id, Surface* surface)
{
    m_viewBackendMap.insert({ id, surface });
}

EGLImageKHR Instance::createImage(struct wl_resource* resourceBuffer)
{
    if (m_eglDisplay == EGL_NO_DISPLAY)
        return EGL_NO_IMAGE_KHR;
    return eglCreateImageKHR(m_eglDisplay, EGL_NO_CONTEXT, EGL_WAYLAND_BUFFER_WL, resourceBuffer, nullptr);
}

EGLImageKHR Instance::createImage(const struct linux_dmabuf_buffer* dmabufBuffer)
{
    if (m_eglDisplay == EGL_NO_DISPLAY)
        return EGL_NO_IMAGE_KHR;
    const struct linux_dmabuf_attributes* bufAttribs =
        linux_dmabuf_get_buffer_attributes(dmabufBuffer);
    assert(bufAttribs);

    static const struct {
        EGLint fd;
        EGLint offset;
        EGLint pitch;
        EGLint modifierLo;
        EGLint modifierHi;
    } planeEnums[4] = {
        {EGL_DMA_BUF_PLANE0_FD_EXT,
         EGL_DMA_BUF_PLANE0_OFFSET_EXT,
         EGL_DMA_BUF_PLANE0_PITCH_EXT,
         EGL_DMA_BUF_PLANE0_MODIFIER_LO_EXT,
         EGL_DMA_BUF_PLANE0_MODIFIER_HI_EXT},
        {EGL_DMA_BUF_PLANE1_FD_EXT,
         EGL_DMA_BUF_PLANE1_OFFSET_EXT,
         EGL_DMA_BUF_PLANE1_PITCH_EXT,
         EGL_DMA_BUF_PLANE1_MODIFIER_LO_EXT,
         EGL_DMA_BUF_PLANE1_MODIFIER_HI_EXT},
        {EGL_DMA_BUF_PLANE2_FD_EXT,
         EGL_DMA_BUF_PLANE2_OFFSET_EXT,
         EGL_DMA_BUF_PLANE2_PITCH_EXT,
         EGL_DMA_BUF_PLANE2_MODIFIER_LO_EXT,
         EGL_DMA_BUF_PLANE2_MODIFIER_HI_EXT},
        {EGL_DMA_BUF_PLANE3_FD_EXT,
         EGL_DMA_BUF_PLANE3_OFFSET_EXT,
         EGL_DMA_BUF_PLANE3_PITCH_EXT,
         EGL_DMA_BUF_PLANE3_MODIFIER_LO_EXT,
         EGL_DMA_BUF_PLANE3_MODIFIER_HI_EXT},
    };

    EGLint attribs[50];
    int atti = 0;
    attribs[atti++] = EGL_WIDTH;
    attribs[atti++] = bufAttribs->width;
    attribs[atti++] = EGL_HEIGHT;
    attribs[atti++] = bufAttribs->height;
    attribs[atti++] = EGL_LINUX_DRM_FOURCC_EXT;
    attribs[atti++] = bufAttribs->format;

    for (int i = 0; i < bufAttribs->n_planes; i++) {
        attribs[atti++] = planeEnums[i].fd;
        attribs[atti++] = bufAttribs->fd[i];
        attribs[atti++] = planeEnums[i].offset;
        attribs[atti++] = bufAttribs->offset[i];
        attribs[atti++] = planeEnums[i].pitch;
        attribs[atti++] = bufAttribs->stride[i];
        attribs[atti++] = planeEnums[i].modifierLo;
        attribs[atti++] = bufAttribs->modifier[i] & 0xFFFFFFFF;
        attribs[atti++] = planeEnums[i].modifierHi;
        attribs[atti++] = bufAttribs->modifier[i] >> 32;
    }

    attribs[atti++] = EGL_NONE;

    return eglCreateImageKHR(m_eglDisplay, EGL_NO_CONTEXT, EGL_LINUX_DMA_BUF_EXT, nullptr, attribs);
}

void Instance::destroyImage(EGLImageKHR image)
{
    if (m_eglDisplay == EGL_NO_DISPLAY)
        return;
    eglDestroyImageKHR(m_eglDisplay, image);
}

struct wl_client* Instance::registerViewBackend(uint32_t id, ExportableClient& exportableClient)
{
    auto it = m_viewBackendMap.find(id);
    if (it == m_viewBackendMap.end())
        std::abort();

    it->second->exportableClient = &exportableClient;
    return it->second->client;
}

void Instance::unregisterViewBackend(uint32_t id)
{
    auto it = m_viewBackendMap.find(id);
    if (it != m_viewBackendMap.end()) {
        it->second->exportableClient = nullptr;
        m_viewBackendMap.erase(it);
    }
}

} // namespace WS
