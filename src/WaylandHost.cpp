#include "WaylandHost.h"
#include "ImageRenderer.h"

#include <wayland-server.h>
#include <wayland-server-protocol.h>
#include "xdg-shell-server-protocol.h"
#include "linux-dmabuf-unstable-v1-server-protocol.h"
#include "viewporter-server-protocol.h"
#include "text-input-unstable-v3-server-protocol.h"

#include <EGL/egl.h>
#include <EGL/eglext.h>
#include <GLES2/gl2ext.h>

#include <xkbcommon/xkbcommon.h>
#include <fcntl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>
#include <time.h>
#include <cerrno>

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstring>
#include <cstdlib>
#include <vector>
#include <unordered_map>

namespace huskyfe::wlhost {

namespace {


struct Surface {
    wl_resource* surface_res     = nullptr;
    wl_resource* xdg_surface_res = nullptr;
    wl_resource* toplevel_res    = nullptr;


    wl_resource* pending_buffer  = nullptr;
    bool         has_pending     = false;
    std::vector<wl_resource*> pending_frame_cbs;


    wl_resource* committed_buffer = nullptr;


    wl_listener  buffer_destroy_listener{};
    bool         buffer_destroy_armed = false;


    bool         buffer_dirty     = false;


    GLuint  texture = 0;
    int32_t tex_w   = 0;
    int32_t tex_h   = 0;


    void*   egl_image    = nullptr;
    bool    is_dmabuf_tex = false;


    std::vector<wl_resource*> active_frame_cbs;


    uint32_t last_frame_cb_ms = 0;


    std::vector<wl_resource*> deferred_releases;

    bool is_toplevel = false;
    bool mapped      = false;
    bool auto_focused = false;


    bool is_xwayland = false;


    bool needs_rb_swap = false;


    std::string title;
    std::string app_id;


    int scale = 3;


    bool         has_viewport_dest = false;
    int32_t      viewport_dest_w   = 0;
    int32_t      viewport_dest_h   = 0;


    bool         has_viewport_src  = false;
    double       viewport_src_x    = 0;
    double       viewport_src_y    = 0;
    double       viewport_src_w    = 0;
    double       viewport_src_h    = 0;


    Surface*     parent       = nullptr;
    int32_t      sub_x        = 0;
    int32_t      sub_y        = 0;
    std::vector<Surface*> children;
};


constexpr int DEFAULT_OUTPUT_SCALE = 3;


inline EGLImageKHR (*p_eglCreateImageKHR)(EGLDisplay, EGLContext, EGLenum,
                                          EGLClientBuffer, const EGLint*) = nullptr;
inline EGLBoolean  (*p_eglDestroyImageKHR)(EGLDisplay, EGLImageKHR) = nullptr;
inline void (*p_glEGLImageTargetTexture2DOES)(GLenum, GLeglImageOES) = nullptr;
inline EGLDisplay  (*p_eglGetCurrentDisplay)(void) = nullptr;
inline int egl_funcs_loaded = 0;
void load_egl_image_funcs();


inline bool dbg_dmabuf() {
    static const bool v = getenv("HUSKYFE_DMABUF_DEBUG") != nullptr;
    return v;
}


inline GLuint blit_program = 0;
inline GLuint blit_vbo     = 0;
inline GLuint blit_fbo     = 0;
inline GLint  blit_loc_pos = -1;
inline GLint  blit_loc_tex = -1;
[[maybe_unused]] bool ensure_blit_pipeline();

struct State {
    wl_display*    display     = nullptr;
    wl_event_loop* loop        = nullptr;
    int            socket_fd   = -1;

    std::vector<Surface*> surfaces;


    std::vector<wl_resource*> touches;
    std::vector<wl_resource*> pointers;
    std::vector<wl_resource*> keyboards;


    struct TextInput {
        wl_resource* resource     = nullptr;
        wl_client*   client       = nullptr;
        bool         pending_enabled = false;
        bool         current_enabled = false;
        bool         entered      = false;
        uint32_t     serial       = 0;
    };
    std::vector<TextInput*> text_inputs;


    TextInput* active_ti = nullptr;


    int    keymap_fd      = -1;
    size_t keymap_size    = 0;


    Surface* keyboard_focused = nullptr;


    Surface* touched_surface = nullptr;


    Surface* focused = nullptr;


    Surface* pemu_entered = nullptr;
    bool     pemu_down    = false;


    std::vector<wl_resource*> outputs;


    std::vector<std::pair<std::string, int>> scale_overrides;


    int bottom_inset = 0;
};

State g;


void text_input_focus_changed(Surface* old_focus, Surface* new_focus);
void keyboard_focus_to(Surface* s, wl_client* client);


void send_activated(Surface* s, bool activated) {
    if (!s || !s->toplevel_res) return;
    int scale = s->scale > 0 ? s->scale : DEFAULT_OUTPUT_SCALE;
    struct wl_array states;
    wl_array_init(&states);
    if (activated) {
        uint32_t* st = static_cast<uint32_t*>(
            wl_array_add(&states, sizeof(uint32_t)));
        if (st) *st = XDG_TOPLEVEL_STATE_ACTIVATED;
    }
    xdg_toplevel_send_configure(s->toplevel_res,
                                1344 / scale, 2992 / scale, &states);
    wl_array_release(&states);
    if (s->xdg_surface_res)
        xdg_surface_send_configure(s->xdg_surface_res,
                                   wl_display_next_serial(g.display));
}

void on_focus_changed(Surface* old_focus, Surface* new_focus) {


    if (old_focus && old_focus != new_focus) send_activated(old_focus, false);
    if (new_focus)                            send_activated(new_focus, true);
    wl_client* nc = new_focus ? wl_resource_get_client(new_focus->surface_res)
                              : nullptr;
    keyboard_focus_to(new_focus, nc);
    text_input_focus_changed(old_focus, new_focus);
}


Surface* surface_of(wl_resource* r) {
    return r ? static_cast<Surface*>(wl_resource_get_user_data(r)) : nullptr;
}

void delete_surface(Surface* s) {
    if (!s) return;


    for (auto* cb : s->pending_frame_cbs) wl_resource_set_user_data(cb, nullptr);
    for (auto* cb : s->active_frame_cbs)  wl_resource_set_user_data(cb, nullptr);
    if (g.focused == s) {


        wl_client* gone_c = wl_resource_get_client(s->surface_res);
        for (auto* ti : g.text_inputs) if (ti->entered && ti->client == gone_c) {
            ti->entered = false;
            ti->pending_enabled = false;
            ti->current_enabled = false;
            if (g.active_ti == ti) g.active_ti = nullptr;
        }
        g.focused = nullptr;
    }
    if (g.touched_surface == s) g.touched_surface = nullptr;
    if (g.pemu_entered == s)    { g.pemu_entered = nullptr; g.pemu_down = false; }
    if (g.keyboard_focused == s) g.keyboard_focused = nullptr;


    if (s->egl_image && !s->is_dmabuf_tex
        && p_eglDestroyImageKHR && p_eglGetCurrentDisplay)
        p_eglDestroyImageKHR(p_eglGetCurrentDisplay(),
                             (EGLImageKHR)s->egl_image);
    s->egl_image = nullptr;


    if (s->buffer_destroy_armed) {
        wl_list_remove(&s->buffer_destroy_listener.link);
        s->buffer_destroy_armed = false;
    }
    if (s->texture && !s->is_dmabuf_tex) glDeleteTextures(1, &s->texture);
    s->texture = 0;

    if (s->parent) {
        auto& v = s->parent->children;
        v.erase(std::remove(v.begin(), v.end(), s), v.end());
        s->parent = nullptr;
    }
    for (auto* c : s->children) c->parent = nullptr;
    s->children.clear();
    auto it = std::find(g.surfaces.begin(), g.surfaces.end(), s);
    if (it != g.surfaces.end()) g.surfaces.erase(it);
    delete s;
}


void surface_destroy(wl_client*, wl_resource* r) {
    wl_resource_destroy(r);
}
void surface_attach(wl_client*, wl_resource* r,
                    wl_resource* buffer, int32_t , int32_t ) {
    Surface* s = surface_of(r);
    s->pending_buffer = buffer;
    s->has_pending    = true;
}
void surface_damage(wl_client*, wl_resource*,
                    int32_t, int32_t, int32_t, int32_t) {}
void surface_damage_buffer(wl_client*, wl_resource*,
                           int32_t, int32_t, int32_t, int32_t) {}


void frame_callback_resource_destroy(wl_resource* cb) {
    Surface* s = static_cast<Surface*>(wl_resource_get_user_data(cb));
    if (!s) return;
    auto scrub = [cb](std::vector<wl_resource*>& v) {
        v.erase(std::remove(v.begin(), v.end(), cb), v.end());
    };
    scrub(s->pending_frame_cbs);
    scrub(s->active_frame_cbs);
}
void surface_frame(wl_client* client, wl_resource* r, uint32_t cb_id) {
    Surface* s = surface_of(r);
    wl_resource* cb = wl_resource_create(client, &wl_callback_interface, 1, cb_id);
    if (!cb) { wl_client_post_no_memory(client); return; }
    wl_resource_set_implementation(cb, nullptr, s,
                                   frame_callback_resource_destroy);
    s->pending_frame_cbs.push_back(cb);
}
void surface_set_opaque_region(wl_client*, wl_resource*, wl_resource*) {}
void surface_set_input_region(wl_client*, wl_resource*, wl_resource*) {}
void surface_set_buffer_transform(wl_client*, wl_resource*, int32_t) {}
void surface_set_buffer_scale(wl_client*, wl_resource*, int32_t) {}
void surface_offset(wl_client*, wl_resource*, int32_t, int32_t) {}


extern const struct wl_buffer_interface dmabuf_buffer_impl;


static void on_committed_buffer_destroyed(wl_listener* l, void* ) {
    Surface* s;
    s = wl_container_of(l, s, buffer_destroy_listener);
    s->committed_buffer    = nullptr;
    s->mapped              = false;
    s->buffer_dirty        = false;
    s->is_dmabuf_tex       = false;
    s->buffer_destroy_armed = false;


}

static void arm_buffer_destroy_listener(Surface* s) {
    if (!s->committed_buffer) return;
    if (s->buffer_destroy_armed) return;
    s->buffer_destroy_listener.notify = on_committed_buffer_destroyed;
    wl_resource_add_destroy_listener(s->committed_buffer,
                                     &s->buffer_destroy_listener);
    s->buffer_destroy_armed = true;
}

static void disarm_buffer_destroy_listener(Surface* s) {
    if (!s->buffer_destroy_armed) return;
    wl_list_remove(&s->buffer_destroy_listener.link);
    s->buffer_destroy_armed = false;
}

void surface_commit(wl_client*, wl_resource* r) {
    Surface* s = surface_of(r);
    if (s->has_pending) {


        if (s->committed_buffer && s->committed_buffer != s->pending_buffer)
            s->deferred_releases.push_back(s->committed_buffer);
        bool was_mapped = s->mapped;


        disarm_buffer_destroy_listener(s);
        s->committed_buffer = s->pending_buffer;
        if (s->committed_buffer) {
            s->mapped = true;
            s->buffer_dirty = true;
            arm_buffer_destroy_listener(s);
        }
        s->has_pending  = false;
        s->pending_buffer = nullptr;


        if (s->is_toplevel && s->mapped && !s->auto_focused) {
            Surface* old = g.focused;
            g.focused = s;
            s->auto_focused = true;
            if (old != s) on_focus_changed(old, s);
        }
        (void)was_mapped;
    }


    for (auto* cb : s->pending_frame_cbs) s->active_frame_cbs.push_back(cb);
    s->pending_frame_cbs.clear();
}

const struct wl_surface_interface surface_impl = {
    surface_destroy, surface_attach, surface_damage, surface_frame,
    surface_set_opaque_region, surface_set_input_region, surface_commit,
    surface_set_buffer_transform, surface_set_buffer_scale,
    surface_damage_buffer, surface_offset,
};

void surface_resource_destroy(wl_resource* r) {
    delete_surface(surface_of(r));
}


void region_destroy(wl_client*, wl_resource* r) { wl_resource_destroy(r); }
void region_add(wl_client*, wl_resource*, int32_t, int32_t, int32_t, int32_t) {}
void region_subtract(wl_client*, wl_resource*, int32_t, int32_t, int32_t, int32_t) {}
const struct wl_region_interface region_impl = {
    region_destroy, region_add, region_subtract,
};


static std::unordered_map<wl_client*, bool> g_client_is_xwayland;

static bool client_is_xwayland(wl_client* c) {
    if (!c) return false;
    auto it = g_client_is_xwayland.find(c);
    if (it != g_client_is_xwayland.end()) return it->second;
    pid_t pid = -1;
    wl_client_get_credentials(c, &pid, nullptr, nullptr);
    bool xw = false;
    char comm[64] = {0};
    if (pid > 0) {
        char path[64];
        snprintf(path, sizeof(path), "/proc/%d/comm", (int)pid);
        FILE* f = fopen(path, "r");
        if (f) {
            if (fgets(comm, sizeof(comm), f)) {
                size_t n = strlen(comm);
                if (n && comm[n-1] == '\n') comm[n-1] = '\0';
                if (strncmp(comm, "Xwayland", 8) == 0) xw = true;
            }
            fclose(f);
        }


        if (!xw) {
            snprintf(path, sizeof(path), "/proc/%d/environ", (int)pid);
            int fd = open(path, O_RDONLY);
            if (fd >= 0) {
                char buf[8192];
                ssize_t n = read(fd, buf, sizeof(buf) - 1);
                if (n > 0) {
                    buf[n] = '\0';
                    for (ssize_t i = 0; i < n; ) {
                        const char* e = buf + i;
                        if (strncmp(e, "WINEPREFIX=", 11) == 0
                            || strncmp(e, "WINELOADER=", 11) == 0) {
                            xw = true; break;
                        }
                        i += strlen(e) + 1;
                    }
                }
                close(fd);
            }
        }
    }
    g_client_is_xwayland[c] = xw;
    fprintf(stderr, "huskyfe-wl: client_is_xwayland: c=%p pid=%d comm='%s' -> %d\n",
            (void*)c, (int)pid, comm, (int)xw);
    return xw;
}


void compositor_create_surface(wl_client* client, wl_resource* r, uint32_t id) {
    wl_resource* sr = wl_resource_create(
        client, &wl_surface_interface, wl_resource_get_version(r), id);
    if (!sr) { wl_client_post_no_memory(client); return; }
    Surface* s = new Surface();
    s->surface_res = sr;
    s->is_xwayland = client_is_xwayland(client);
    g.surfaces.push_back(s);
    wl_resource_set_implementation(sr, &surface_impl, s, surface_resource_destroy);
}
void compositor_create_region(wl_client* client, wl_resource* r, uint32_t id) {
    wl_resource* rr = wl_resource_create(
        client, &wl_region_interface, wl_resource_get_version(r), id);
    if (!rr) { wl_client_post_no_memory(client); return; }
    wl_resource_set_implementation(rr, &region_impl, nullptr, nullptr);
}
const struct wl_compositor_interface compositor_impl = {
    compositor_create_surface, compositor_create_region,
};
void compositor_bind(wl_client* client, void*, uint32_t version, uint32_t id) {
    wl_resource* r = wl_resource_create(client, &wl_compositor_interface,
                                        std::min(version, 4u), id);
    if (!r) { wl_client_post_no_memory(client); return; }
    wl_resource_set_implementation(r, &compositor_impl, nullptr, nullptr);
}


void subsurface_destroy(wl_client*, wl_resource* r) { wl_resource_destroy(r); }
void subsurface_set_position(wl_client*, wl_resource* r, int32_t x, int32_t y) {
    auto* s = static_cast<Surface*>(wl_resource_get_user_data(r));
    if (s) { s->sub_x = x; s->sub_y = y; }
}
void subsurface_place_above(wl_client*, wl_resource*, wl_resource*) {}
void subsurface_place_below(wl_client*, wl_resource*, wl_resource*) {}
void subsurface_set_sync(wl_client*, wl_resource*) {}
void subsurface_set_desync(wl_client*, wl_resource*) {}
const struct wl_subsurface_interface subsurface_impl = {
    subsurface_destroy, subsurface_set_position, subsurface_place_above,
    subsurface_place_below, subsurface_set_sync, subsurface_set_desync,
};
void subcompositor_destroy(wl_client*, wl_resource* r) { wl_resource_destroy(r); }
void subcompositor_get_subsurface(wl_client* client, wl_resource* r,
                                  uint32_t id, wl_resource* surf, wl_resource* parent) {
    wl_resource* sr = wl_resource_create(
        client, &wl_subsurface_interface, wl_resource_get_version(r), id);
    if (!sr) { wl_client_post_no_memory(client); return; }


    Surface* child = static_cast<Surface*>(wl_resource_get_user_data(surf));
    Surface* par   = static_cast<Surface*>(wl_resource_get_user_data(parent));
    if (child && par) {
        child->parent = par;
        par->children.push_back(child);
    }
    wl_resource_set_implementation(sr, &subsurface_impl, child, nullptr);
}
const struct wl_subcompositor_interface subcompositor_impl = {
    subcompositor_destroy, subcompositor_get_subsurface,
};
void subcompositor_bind(wl_client* client, void*, uint32_t version, uint32_t id) {
    wl_resource* r = wl_resource_create(client, &wl_subcompositor_interface,
                                        std::min(version, 1u), id);
    if (!r) { wl_client_post_no_memory(client); return; }
    wl_resource_set_implementation(r, &subcompositor_impl, nullptr, nullptr);
}


struct XdgSurface { Surface* s = nullptr; };

void toplevel_destroy(wl_client*, wl_resource* r) { wl_resource_destroy(r); }
void toplevel_set_parent(wl_client*, wl_resource*, wl_resource*) {}
void toplevel_set_title(wl_client*, wl_resource* r, const char* t) {
    auto* s = static_cast<Surface*>(wl_resource_get_user_data(r));
    if (s && t) s->title = t;
    if (t) fprintf(stderr, "huskyfe-wl: title=%s\n", t);
}


static std::string normalize(std::string s) {
    std::string out;
    out.reserve(s.size());
    for (char c : s) {
        if (c == '-' || c == '_' || c == '.' || c == '/' || c == ' ') continue;
        out.push_back((char)std::tolower((unsigned char)c));
    }
    return out;
}
static int scale_for_app_id(const std::string& app_id) {
    if (g.scale_overrides.empty() || app_id.empty()) return DEFAULT_OUTPUT_SCALE;
    std::string nid = normalize(app_id);
    for (const auto& [key, val] : g.scale_overrides) {
        std::string nk = normalize(key);
        if (!nk.empty() && nid.find(nk) != std::string::npos) return val;
    }
    return DEFAULT_OUTPUT_SCALE;
}

void toplevel_set_app_id(wl_client* client, wl_resource* r, const char* a) {
    auto* s = static_cast<Surface*>(wl_resource_get_user_data(r));
    if (s && a) s->app_id = a;
    if (a) fprintf(stderr, "huskyfe-wl: app_id=%s\n", a);
    if (!s || !a) return;


    int new_scale = scale_for_app_id(a);
    if (new_scale == s->scale) return;
    s->scale = new_scale;
    fprintf(stderr, "huskyfe-wl: app_id=%s -> scale=%d\n", a, new_scale);
    for (wl_resource* out : g.outputs) {
        if (wl_resource_get_client(out) != client) continue;
        if (wl_resource_get_version(out) >= WL_OUTPUT_SCALE_SINCE_VERSION)
            wl_output_send_scale(out, new_scale);
        if (wl_resource_get_version(out) >= WL_OUTPUT_DONE_SINCE_VERSION)
            wl_output_send_done(out);
    }
    if (s->toplevel_res) {
        struct wl_array states;
        wl_array_init(&states);
        uint32_t* st = static_cast<uint32_t*>(
            wl_array_add(&states, sizeof(uint32_t)));
        if (st) *st = XDG_TOPLEVEL_STATE_ACTIVATED;

        xdg_toplevel_send_configure(s->toplevel_res,
                                    1344 / new_scale, 2992 / new_scale,
                                    &states);
        wl_array_release(&states);
    }
    if (s->xdg_surface_res)
        xdg_surface_send_configure(s->xdg_surface_res,
                                   wl_display_next_serial(g.display));
}
void toplevel_show_window_menu(wl_client*, wl_resource*, wl_resource*,
                               uint32_t, int32_t, int32_t) {}
void toplevel_move(wl_client*, wl_resource*, wl_resource*, uint32_t) {}
void toplevel_resize(wl_client*, wl_resource*, wl_resource*, uint32_t, uint32_t) {}
void toplevel_set_max_size(wl_client*, wl_resource*, int32_t, int32_t) {}
void toplevel_set_min_size(wl_client*, wl_resource*, int32_t, int32_t) {}
void toplevel_set_maximized(wl_client*, wl_resource*) {}
void toplevel_unset_maximized(wl_client*, wl_resource*) {}
void toplevel_set_fullscreen(wl_client*, wl_resource*, wl_resource*) {}
void toplevel_unset_fullscreen(wl_client*, wl_resource*) {}
void toplevel_set_minimized(wl_client*, wl_resource*) {}
const struct xdg_toplevel_interface toplevel_impl = {
    toplevel_destroy, toplevel_set_parent, toplevel_set_title, toplevel_set_app_id,
    toplevel_show_window_menu, toplevel_move, toplevel_resize, toplevel_set_max_size,
    toplevel_set_min_size, toplevel_set_maximized, toplevel_unset_maximized,
    toplevel_set_fullscreen, toplevel_unset_fullscreen, toplevel_set_minimized,
};


constexpr int GLP_MAX_PLANES = 4;

struct DmabufBuffer {
    wl_resource* res     = nullptr;
    int32_t      width   = 0;
    int32_t      height  = 0;
    uint32_t     fourcc  = 0;
    int          fds[GLP_MAX_PLANES]   = {-1, -1, -1, -1};
    uint32_t     offsets[GLP_MAX_PLANES] = {0, 0, 0, 0};
    uint32_t     strides[GLP_MAX_PLANES] = {0, 0, 0, 0};
    uint64_t     mods[GLP_MAX_PLANES]    = {0, 0, 0, 0};
    int          n_planes = 0;


    void*        cached_egl_image = nullptr;
    GLuint       cached_ext_tex   = 0;
};


void load_egl_image_funcs() {
    if (egl_funcs_loaded) return;
    p_eglCreateImageKHR =
        (EGLImageKHR(*)(EGLDisplay,EGLContext,EGLenum,EGLClientBuffer,const EGLint*))
        eglGetProcAddress("eglCreateImageKHR");
    p_eglDestroyImageKHR =
        (EGLBoolean(*)(EGLDisplay,EGLImageKHR))
        eglGetProcAddress("eglDestroyImageKHR");
    p_glEGLImageTargetTexture2DOES =
        (void(*)(GLenum,GLeglImageOES))
        eglGetProcAddress("glEGLImageTargetTexture2DOES");
    p_eglGetCurrentDisplay =
        (EGLDisplay(*)(void))
        eglGetProcAddress("eglGetCurrentDisplay");
    egl_funcs_loaded = 1;
    if (!p_eglCreateImageKHR || !p_eglDestroyImageKHR
     || !p_glEGLImageTargetTexture2DOES || !p_eglGetCurrentDisplay)
        fprintf(stderr, "huskyfe-wl: WARNING: EGL image extensions missing\n");
}


DmabufBuffer* dmabuf_of(wl_resource* r) {
    return r ? static_cast<DmabufBuffer*>(wl_resource_get_user_data(r)) : nullptr;
}
void dmabuf_buffer_destroy(wl_client*, wl_resource* r) { wl_resource_destroy(r); }
const struct wl_buffer_interface dmabuf_buffer_impl = { dmabuf_buffer_destroy };

void dmabuf_buffer_resource_destroyed(wl_resource* r) {
    DmabufBuffer* b = dmabuf_of(r);
    if (!b) return;
    if (b->cached_ext_tex) {
        glDeleteTextures(1, &b->cached_ext_tex);
        b->cached_ext_tex = 0;
    }
    if (b->cached_egl_image && p_eglDestroyImageKHR && p_eglGetCurrentDisplay) {
        p_eglDestroyImageKHR(p_eglGetCurrentDisplay(),
                             (EGLImageKHR)b->cached_egl_image);
        b->cached_egl_image = nullptr;
    }


    for (int i = 0; i < GLP_MAX_PLANES; i++)
        if (b->fds[i] >= 0) close(b->fds[i]);
    delete b;
}


void params_destroy(wl_client*, wl_resource* r) { wl_resource_destroy(r); }
void params_resource_destroyed(wl_resource* r) {
    DmabufBuffer* b = static_cast<DmabufBuffer*>(wl_resource_get_user_data(r));
    if (!b) return;

    if (b->res == nullptr) {
        for (int i = 0; i < GLP_MAX_PLANES; i++)
            if (b->fds[i] >= 0) close(b->fds[i]);
        delete b;
    }
}

void params_add(wl_client*, wl_resource* r, int32_t fd, uint32_t plane,
                uint32_t offset, uint32_t stride,
                uint32_t mod_hi, uint32_t mod_lo) {
    DmabufBuffer* b = static_cast<DmabufBuffer*>(wl_resource_get_user_data(r));
    if (!b || plane >= GLP_MAX_PLANES) { close(fd); return; }
    if (b->fds[plane] >= 0) close(b->fds[plane]);
    b->fds[plane]     = fd;
    b->offsets[plane] = offset;
    b->strides[plane] = stride;
    b->mods[plane]    = ((uint64_t)mod_hi << 32) | (uint64_t)mod_lo;
    if ((int)plane + 1 > b->n_planes) b->n_planes = (int)plane + 1;
}

wl_resource* params_finalize(wl_client* client, wl_resource* params_res,
                             uint32_t buffer_id, int32_t width, int32_t height,
                             uint32_t format, uint32_t ) {
    DmabufBuffer* b = static_cast<DmabufBuffer*>(wl_resource_get_user_data(params_res));
    if (!b || b->n_planes < 1 || b->fds[0] < 0) {
        zwp_linux_buffer_params_v1_send_failed(params_res);
        return nullptr;
    }
    b->width  = width;
    b->height = height;
    b->fourcc = format;
    wl_resource* br;
    if (buffer_id == 0) {
        br = wl_resource_create(client, &wl_buffer_interface, 1, 0);
    } else {
        br = wl_resource_create(client, &wl_buffer_interface, 1, buffer_id);
    }
    if (!br) {
        zwp_linux_buffer_params_v1_send_failed(params_res);
        return nullptr;
    }
    wl_resource_set_implementation(br, &dmabuf_buffer_impl, b,
                                   dmabuf_buffer_resource_destroyed);
    b->res = br;
    return br;
}

void params_create(wl_client* client, wl_resource* r,
                   int32_t width, int32_t height,
                   uint32_t format, uint32_t flags) {
    wl_resource* buf = params_finalize(client, r, 0, width, height, format, flags);
    if (buf)
        zwp_linux_buffer_params_v1_send_created(r, buf);
}

void params_create_immed(wl_client* client, wl_resource* r,
                         uint32_t buffer_id,
                         int32_t width, int32_t height,
                         uint32_t format, uint32_t flags) {
    params_finalize(client, r, buffer_id, width, height, format, flags);
}

const struct zwp_linux_buffer_params_v1_interface params_impl = {
    params_destroy, params_add, params_create, params_create_immed,
};


void dmabuf_destroy(wl_client*, wl_resource* r) { wl_resource_destroy(r); }
void dmabuf_create_params(wl_client* client, wl_resource* r, uint32_t id) {
    wl_resource* pr = wl_resource_create(client,
        &zwp_linux_buffer_params_v1_interface,
        wl_resource_get_version(r), id);
    if (!pr) { wl_client_post_no_memory(client); return; }
    auto* b = new DmabufBuffer();
    wl_resource_set_implementation(pr, &params_impl, b, params_resource_destroyed);
}


struct DmabufFeedback {
    int     fd          = -1;
    size_t  size        = 0;
    dev_t   main_device = 0;
};
static DmabufFeedback g_feedback;

void build_dmabuf_feedback() {
    if (g_feedback.fd >= 0) return;

    struct __attribute__((packed)) Entry {
        uint32_t format;
        uint32_t pad;
        uint64_t modifier;
    };
    static_assert(sizeof(Entry) == 16);

    constexpr uint32_t FORMATS[] = {
        0x34324241,
        0x34325241,
        0x34325258,
        0x34324258,
    };
    constexpr size_t N = sizeof(FORMATS) / sizeof(FORMATS[0]);

    int fd = memfd_create("huskyfe-dmabuf-formats", MFD_CLOEXEC);
    if (fd < 0) {
        fprintf(stderr, "huskyfe-wl: memfd_create for dmabuf feedback failed\n");
        return;
    }
    size_t size = N * sizeof(Entry);
    if (ftruncate(fd, (off_t)size) != 0) { close(fd); return; }
    Entry* table = (Entry*)mmap(nullptr, size, PROT_READ | PROT_WRITE,
                                MAP_SHARED, fd, 0);
    if (table == MAP_FAILED) { close(fd); return; }
    for (size_t i = 0; i < N; i++) {
        table[i].format   = FORMATS[i];
        table[i].pad      = 0;
        table[i].modifier = 0;
    }
    munmap(table, size);


    dev_t devid = 0;
    struct stat st;
    if      (stat("/dev/dri/renderD128", &st) == 0) devid = st.st_rdev;
    else if (stat("/dev/dri/card0",      &st) == 0) devid = st.st_rdev;

    g_feedback.fd          = fd;
    g_feedback.size        = size;
    g_feedback.main_device = devid;
}

void feedback_destroy(wl_client*, wl_resource* r) { wl_resource_destroy(r); }
const struct zwp_linux_dmabuf_feedback_v1_interface feedback_impl = {
    feedback_destroy,
};
void dmabuf_send_feedback(wl_client* client, wl_resource* parent, uint32_t id) {
    build_dmabuf_feedback();
    wl_resource* fr = wl_resource_create(client,
        &zwp_linux_dmabuf_feedback_v1_interface,
        wl_resource_get_version(parent), id);
    if (!fr) { wl_client_post_no_memory(client); return; }
    wl_resource_set_implementation(fr, &feedback_impl, nullptr, nullptr);

    if (g_feedback.fd < 0) {


        zwp_linux_dmabuf_feedback_v1_send_done(fr);
        return;
    }


    zwp_linux_dmabuf_feedback_v1_send_format_table(
        fr, g_feedback.fd, (uint32_t)g_feedback.size);


    struct wl_array dev_arr;
    wl_array_init(&dev_arr);
    void* p = wl_array_add(&dev_arr, sizeof(dev_t));
    if (p) memcpy(p, &g_feedback.main_device, sizeof(dev_t));
    zwp_linux_dmabuf_feedback_v1_send_main_device(fr, &dev_arr);


    zwp_linux_dmabuf_feedback_v1_send_tranche_target_device(fr, &dev_arr);
    struct wl_array idx_arr;
    wl_array_init(&idx_arr);
    size_t n_entries = g_feedback.size / 16;
    for (uint16_t i = 0; i < n_entries; i++) {
        uint16_t* slot = (uint16_t*)wl_array_add(&idx_arr, sizeof(uint16_t));
        if (slot) *slot = i;
    }
    zwp_linux_dmabuf_feedback_v1_send_tranche_formats(fr, &idx_arr);
    zwp_linux_dmabuf_feedback_v1_send_tranche_flags(fr, 0);
    zwp_linux_dmabuf_feedback_v1_send_tranche_done(fr);
    wl_array_release(&idx_arr);


    zwp_linux_dmabuf_feedback_v1_send_done(fr);
    wl_array_release(&dev_arr);
}
void dmabuf_get_default_feedback(wl_client* c, wl_resource* r, uint32_t id) {
    dmabuf_send_feedback(c, r, id);
}
void dmabuf_get_surface_feedback(wl_client* c, wl_resource* r,
                                 uint32_t id, wl_resource* ) {
    dmabuf_send_feedback(c, r, id);
}
const struct zwp_linux_dmabuf_v1_interface dmabuf_impl = {
    dmabuf_destroy, dmabuf_create_params,
    dmabuf_get_default_feedback, dmabuf_get_surface_feedback,
};


constexpr uint32_t DMABUF_FORMATS[] = {
    0x34324241,
    0x34325241,
    0x34325258,
    0x34324258,
};

void dmabuf_bind(wl_client* client, void*, uint32_t version, uint32_t id) {
    wl_resource* r = wl_resource_create(client, &zwp_linux_dmabuf_v1_interface,
                                        std::min(version, 4u), id);
    if (!r) { wl_client_post_no_memory(client); return; }
    wl_resource_set_implementation(r, &dmabuf_impl, nullptr, nullptr);


    for (uint32_t f : DMABUF_FORMATS) {
        zwp_linux_dmabuf_v1_send_format(r, f);


        if (wl_resource_get_version(r) >= 3) {
            zwp_linux_dmabuf_v1_send_modifier(r, f, 0, 0);
        }
    }
}


void viewport_destroy(wl_client*, wl_resource* r) { wl_resource_destroy(r); }

void viewport_set_source(wl_client*, wl_resource* r,
                         wl_fixed_t x, wl_fixed_t y,
                         wl_fixed_t w, wl_fixed_t h) {
    Surface* s = static_cast<Surface*>(wl_resource_get_user_data(r));
    if (!s) return;
    if (x < 0 && y < 0 && w < 0 && h < 0) {
        s->has_viewport_src = false;
        return;
    }
    s->has_viewport_src = true;
    s->viewport_src_x = wl_fixed_to_double(x);
    s->viewport_src_y = wl_fixed_to_double(y);
    s->viewport_src_w = wl_fixed_to_double(w);
    s->viewport_src_h = wl_fixed_to_double(h);
}

void viewport_set_destination(wl_client*, wl_resource* r,
                              int32_t w, int32_t h) {
    Surface* s = static_cast<Surface*>(wl_resource_get_user_data(r));
    if (!s) return;
    if (w == -1 && h == -1) {
        s->has_viewport_dest = false;
        return;
    }
    s->has_viewport_dest = true;
    s->viewport_dest_w = w;
    s->viewport_dest_h = h;
}

const struct wp_viewport_interface viewport_impl = {
    viewport_destroy, viewport_set_source, viewport_set_destination,
};
void viewporter_destroy(wl_client*, wl_resource* r) { wl_resource_destroy(r); }
void viewporter_get_viewport(wl_client* client, wl_resource* r,
                             uint32_t id, wl_resource* surface) {
    wl_resource* vr = wl_resource_create(client, &wp_viewport_interface,
                                         wl_resource_get_version(r), id);
    if (!vr) { wl_client_post_no_memory(client); return; }


    wl_resource_set_implementation(vr, &viewport_impl,
                                   surface_of(surface), nullptr);
}
const struct wp_viewporter_interface viewporter_impl = {
    viewporter_destroy, viewporter_get_viewport,
};
void viewporter_bind(wl_client* client, void*, uint32_t version, uint32_t id) {
    wl_resource* r = wl_resource_create(client, &wp_viewporter_interface,
                                        std::min(version, 1u), id);
    if (!r) { wl_client_post_no_memory(client); return; }
    wl_resource_set_implementation(r, &viewporter_impl, nullptr, nullptr);
}


void popup_destroy(wl_client*, wl_resource* r) { wl_resource_destroy(r); }
void popup_grab(wl_client*, wl_resource*, wl_resource*, uint32_t) {}
void popup_reposition(wl_client*, wl_resource*, wl_resource*, uint32_t) {}
const struct xdg_popup_interface popup_impl = {
    popup_destroy, popup_grab, popup_reposition,
};


void positioner_destroy(wl_client*, wl_resource* r) { wl_resource_destroy(r); }
void positioner_set_size(wl_client*, wl_resource*, int32_t, int32_t) {}
void positioner_set_anchor_rect(wl_client*, wl_resource*,
                                int32_t, int32_t, int32_t, int32_t) {}
void positioner_set_anchor(wl_client*, wl_resource*, uint32_t) {}
void positioner_set_gravity(wl_client*, wl_resource*, uint32_t) {}
void positioner_set_constraint_adjustment(wl_client*, wl_resource*, uint32_t) {}
void positioner_set_offset(wl_client*, wl_resource*, int32_t, int32_t) {}
void positioner_set_reactive(wl_client*, wl_resource*) {}
void positioner_set_parent_size(wl_client*, wl_resource*, int32_t, int32_t) {}
void positioner_set_parent_configure(wl_client*, wl_resource*, uint32_t) {}
const struct xdg_positioner_interface positioner_impl = {
    positioner_destroy, positioner_set_size, positioner_set_anchor_rect,
    positioner_set_anchor, positioner_set_gravity,
    positioner_set_constraint_adjustment, positioner_set_offset,
    positioner_set_reactive, positioner_set_parent_size,
    positioner_set_parent_configure,
};

void xdg_surface_destroy(wl_client*, wl_resource* r) { wl_resource_destroy(r); }
void xdg_surface_get_toplevel(wl_client* client, wl_resource* r, uint32_t id) {
    auto* xs = static_cast<XdgSurface*>(wl_resource_get_user_data(r));
    wl_resource* tr = wl_resource_create(client, &xdg_toplevel_interface,
                                         wl_resource_get_version(r), id);
    if (!tr) { wl_client_post_no_memory(client); return; }
    wl_resource_set_implementation(tr, &toplevel_impl, xs->s, nullptr);
    if (xs->s) {
        xs->s->toplevel_res = tr;
        xs->s->is_toplevel  = true;


        if (xs->s->mapped && !xs->s->auto_focused) {
            Surface* old = g.focused;
            g.focused = xs->s;
            xs->s->auto_focused = true;
            if (old != xs->s) on_focus_changed(old, xs->s);
        }
    }


    int scale = xs->s ? xs->s->scale : DEFAULT_OUTPUT_SCALE;
    struct wl_array states;
    wl_array_init(&states);
    uint32_t* st = static_cast<uint32_t*>(wl_array_add(&states, sizeof(uint32_t)));
    if (st) *st = XDG_TOPLEVEL_STATE_ACTIVATED;
    xdg_toplevel_send_configure(tr, 1344 / scale, 2992 / scale, &states);
    wl_array_release(&states);
    xdg_surface_send_configure(r, wl_display_next_serial(g.display));
}
void xdg_surface_get_popup(wl_client* client, wl_resource* r, uint32_t id,
                           wl_resource* , wl_resource* ) {


    wl_resource* pr = wl_resource_create(client, &xdg_popup_interface,
                                         wl_resource_get_version(r), id);
    if (!pr) { wl_client_post_no_memory(client); return; }
    wl_resource_set_implementation(pr, &popup_impl, nullptr, nullptr);
    xdg_popup_send_configure(pr, 0, 0, 100, 100);
    xdg_surface_send_configure(r, wl_display_next_serial(g.display));
}
void xdg_surface_set_window_geometry(wl_client*, wl_resource*,
                                     int32_t, int32_t, int32_t, int32_t) {}
void xdg_surface_ack_configure(wl_client*, wl_resource*, uint32_t) {}
const struct xdg_surface_interface xdg_surface_impl = {
    xdg_surface_destroy, xdg_surface_get_toplevel, xdg_surface_get_popup,
    xdg_surface_set_window_geometry, xdg_surface_ack_configure,
};
void xdg_surface_resource_destroy(wl_resource* r) {
    delete static_cast<XdgSurface*>(wl_resource_get_user_data(r));
}

void wm_base_destroy(wl_client*, wl_resource* r) { wl_resource_destroy(r); }
void wm_base_create_positioner(wl_client* client, wl_resource* r, uint32_t id) {
    wl_resource* p = wl_resource_create(client, &xdg_positioner_interface,
                                        wl_resource_get_version(r), id);
    if (!p) { wl_client_post_no_memory(client); return; }
    wl_resource_set_implementation(p, &positioner_impl, nullptr, nullptr);
}
void wm_base_get_xdg_surface(wl_client* client, wl_resource* r,
                             uint32_t id, wl_resource* surface) {
    Surface* s = surface_of(surface);
    wl_resource* xr = wl_resource_create(client, &xdg_surface_interface,
                                         wl_resource_get_version(r), id);
    if (!xr) { wl_client_post_no_memory(client); return; }
    auto* xs = new XdgSurface();
    xs->s = s;
    if (s) s->xdg_surface_res = xr;
    wl_resource_set_implementation(xr, &xdg_surface_impl, xs,
                                   xdg_surface_resource_destroy);
}
void wm_base_pong(wl_client*, wl_resource*, uint32_t) {}
const struct xdg_wm_base_interface wm_base_impl = {
    wm_base_destroy, wm_base_create_positioner,
    wm_base_get_xdg_surface, wm_base_pong,
};
void wm_base_bind(wl_client* client, void*, uint32_t version, uint32_t id) {
    wl_resource* r = wl_resource_create(client, &xdg_wm_base_interface,
                                        std::min(version, 3u), id);
    if (!r) { wl_client_post_no_memory(client); return; }
    wl_resource_set_implementation(r, &wm_base_impl, nullptr, nullptr);
}


void output_send_attrs(wl_resource* r, int scale) {
    wl_output_send_geometry(r, 0, 0, 70, 155,
                            WL_OUTPUT_SUBPIXEL_UNKNOWN,
                            "Google", "Pixel 8 Pro panel",
                            WL_OUTPUT_TRANSFORM_NORMAL);
    wl_output_send_mode(r,
                        WL_OUTPUT_MODE_CURRENT | WL_OUTPUT_MODE_PREFERRED,
                        1344, 2992, 60000);
    if (wl_resource_get_version(r) >= WL_OUTPUT_SCALE_SINCE_VERSION)
        wl_output_send_scale(r, scale);
    if (wl_resource_get_version(r) >= WL_OUTPUT_DONE_SINCE_VERSION)
        wl_output_send_done(r);
}

void output_release(wl_client*, wl_resource* r) { wl_resource_destroy(r); }
void output_resource_destroyed(wl_resource* r) {
    auto it = std::find(g.outputs.begin(), g.outputs.end(), r);
    if (it != g.outputs.end()) g.outputs.erase(it);
}
const struct wl_output_interface output_impl = { output_release };

void output_bind(wl_client* client, void*, uint32_t version, uint32_t id) {
    wl_resource* r = wl_resource_create(client, &wl_output_interface,
                                        std::min(version, 3u), id);
    if (!r) { wl_client_post_no_memory(client); return; }
    wl_resource_set_implementation(r, &output_impl, nullptr,
                                   output_resource_destroyed);
    g.outputs.push_back(r);


    output_send_attrs(r, DEFAULT_OUTPUT_SCALE);
}


void data_device_start_drag(wl_client*, wl_resource*, wl_resource*,
                            wl_resource*, wl_resource*, uint32_t) {}
void data_device_set_selection(wl_client*, wl_resource*, wl_resource*, uint32_t) {}
void data_device_release(wl_client*, wl_resource* r) { wl_resource_destroy(r); }
const struct wl_data_device_interface data_device_impl = {
    data_device_start_drag, data_device_set_selection, data_device_release,
};

void data_source_offer(wl_client*, wl_resource*, const char*) {}
void data_source_destroy(wl_client*, wl_resource* r) { wl_resource_destroy(r); }
void data_source_set_actions(wl_client*, wl_resource*, uint32_t) {}
const struct wl_data_source_interface data_source_impl = {
    data_source_offer, data_source_destroy, data_source_set_actions,
};

void ddm_create_data_source(wl_client* c, wl_resource* r, uint32_t id) {
    wl_resource* s = wl_resource_create(c, &wl_data_source_interface,
                                        wl_resource_get_version(r), id);
    if (!s) { wl_client_post_no_memory(c); return; }
    wl_resource_set_implementation(s, &data_source_impl, nullptr, nullptr);
}
void ddm_get_data_device(wl_client* c, wl_resource* r,
                         uint32_t id, wl_resource* ) {
    wl_resource* d = wl_resource_create(c, &wl_data_device_interface,
                                        wl_resource_get_version(r), id);
    if (!d) { wl_client_post_no_memory(c); return; }
    wl_resource_set_implementation(d, &data_device_impl, nullptr, nullptr);
}
const struct wl_data_device_manager_interface ddm_impl = {
    ddm_create_data_source, ddm_get_data_device,
};
void ddm_bind(wl_client* client, void*, uint32_t version, uint32_t id) {
    wl_resource* r = wl_resource_create(client, &wl_data_device_manager_interface,
                                        std::min(version, 3u), id);
    if (!r) { wl_client_post_no_memory(client); return; }
    wl_resource_set_implementation(r, &ddm_impl, nullptr, nullptr);
}


void pointer_resource_destroyed(wl_resource* r) {
    auto it = std::find(g.pointers.begin(), g.pointers.end(), r);
    if (it != g.pointers.end()) g.pointers.erase(it);
}
void pointer_set_cursor(wl_client*, wl_resource*, uint32_t,
                        wl_resource*, int32_t, int32_t) {}
void pointer_release(wl_client*, wl_resource* r) { wl_resource_destroy(r); }
const struct wl_pointer_interface pointer_impl = {
    pointer_set_cursor, pointer_release,
};
void seat_get_pointer(wl_client* c, wl_resource* seat, uint32_t id) {


    int v = wl_resource_get_version(seat);
    wl_resource* r = wl_resource_create(c, &wl_pointer_interface, v, id);
    if (!r) return;
    wl_resource_set_implementation(r, &pointer_impl, nullptr, pointer_resource_destroyed);
    g.pointers.push_back(r);
}
void keyboard_release(wl_client*, wl_resource* r) { wl_resource_destroy(r); }
const struct wl_keyboard_interface keyboard_impl = { keyboard_release };
void keyboard_resource_destroyed(wl_resource* r) {
    auto it = std::find(g.keyboards.begin(), g.keyboards.end(), r);
    if (it != g.keyboards.end()) g.keyboards.erase(it);
}
void seat_get_keyboard(wl_client* c, wl_resource* seat, uint32_t id) {
    int v = wl_resource_get_version(seat);
    wl_resource* r = wl_resource_create(c, &wl_keyboard_interface, v, id);
    if (!r) return;
    wl_resource_set_implementation(r, &keyboard_impl, nullptr,
                                   keyboard_resource_destroyed);
    g.keyboards.push_back(r);

    if (g.keymap_fd >= 0)
        wl_keyboard_send_keymap(r,
            WL_KEYBOARD_KEYMAP_FORMAT_XKB_V1,
            g.keymap_fd, (uint32_t)g.keymap_size);
    if (wl_resource_get_version(r) >= WL_KEYBOARD_REPEAT_INFO_SINCE_VERSION)
        wl_keyboard_send_repeat_info(r, 25, 600);
}
void touch_resource_destroyed(wl_resource* r) {
    auto it = std::find(g.touches.begin(), g.touches.end(), r);
    if (it != g.touches.end()) g.touches.erase(it);
}
void touch_release(wl_client*, wl_resource* r) { wl_resource_destroy(r); }
const struct wl_touch_interface touch_impl = { touch_release };
void seat_get_touch(wl_client* c, wl_resource* seat, uint32_t id) {
    int v = wl_resource_get_version(seat);
    wl_resource* r = wl_resource_create(c, &wl_touch_interface, v, id);
    if (!r) return;
    wl_resource_set_implementation(r, &touch_impl, nullptr, touch_resource_destroyed);
    g.touches.push_back(r);
}
void seat_release(wl_client*, wl_resource* r) { wl_resource_destroy(r); }
const struct wl_seat_interface seat_impl = {
    seat_get_pointer, seat_get_keyboard, seat_get_touch, seat_release,
};
void seat_bind(wl_client* client, void*, uint32_t version, uint32_t id) {
    uint32_t v = std::min(version, 5u);
    wl_resource* r = wl_resource_create(client, &wl_seat_interface, v, id);
    if (!r) { wl_client_post_no_memory(client); return; }
    wl_resource_set_implementation(r, &seat_impl, nullptr, nullptr);
    wl_seat_send_capabilities(r,
        WL_SEAT_CAPABILITY_TOUCH | WL_SEAT_CAPABILITY_POINTER
      | WL_SEAT_CAPABILITY_KEYBOARD);


    if (v >= 2) wl_seat_send_name(r, "huskyfe");
}


void ti_destroy(wl_client*, wl_resource* r) { wl_resource_destroy(r); }
void ti_enable(wl_client*, wl_resource* r) {
    auto* ti = static_cast<State::TextInput*>(wl_resource_get_user_data(r));
    if (ti) ti->pending_enabled = true;
}
void ti_disable(wl_client*, wl_resource* r) {
    auto* ti = static_cast<State::TextInput*>(wl_resource_get_user_data(r));
    if (ti) ti->pending_enabled = false;
}
void ti_set_surrounding_text(wl_client*, wl_resource*,
                             const char*, int32_t, int32_t) {}
void ti_set_text_change_cause(wl_client*, wl_resource*, uint32_t) {}
void ti_set_content_type(wl_client*, wl_resource*, uint32_t, uint32_t) {}
void ti_set_cursor_rectangle(wl_client*, wl_resource*,
                             int32_t, int32_t, int32_t, int32_t) {}
void ti_commit(wl_client*, wl_resource* r) {
    auto* ti = static_cast<State::TextInput*>(wl_resource_get_user_data(r));
    if (!ti) return;
    bool was = ti->current_enabled;
    ti->current_enabled = ti->pending_enabled;
    ti->serial++;
    if (ti->current_enabled) {
        g.active_ti = ti;
    } else if (g.active_ti == ti) {
        g.active_ti = nullptr;
    }
    fprintf(stderr,
        "huskyfe-wl: text_input commit enabled=%d (was %d) entered=%d active=%p\n",
        (int)ti->current_enabled, (int)was, (int)ti->entered, (void*)g.active_ti);
    zwp_text_input_v3_send_done(r, ti->serial);
}
const struct zwp_text_input_v3_interface ti_impl = {
    ti_destroy, ti_enable, ti_disable,
    ti_set_surrounding_text, ti_set_text_change_cause,
    ti_set_content_type, ti_set_cursor_rectangle, ti_commit,
};

void ti_resource_destroyed(wl_resource* r) {
    auto* ti = static_cast<State::TextInput*>(wl_resource_get_user_data(r));
    if (!ti) return;
    if (g.active_ti == ti) g.active_ti = nullptr;
    auto it = std::find(g.text_inputs.begin(), g.text_inputs.end(), ti);
    if (it != g.text_inputs.end()) g.text_inputs.erase(it);
    delete ti;
}

void tim_destroy(wl_client*, wl_resource* r) { wl_resource_destroy(r); }
void tim_get_text_input(wl_client* client, wl_resource* r,
                        uint32_t id, wl_resource* ) {
    wl_resource* tr = wl_resource_create(client, &zwp_text_input_v3_interface,
                                         wl_resource_get_version(r), id);
    if (!tr) { wl_client_post_no_memory(client); return; }
    auto* ti = new State::TextInput{tr, client, false, false, false, 0};
    wl_resource_set_implementation(tr, &ti_impl, ti, ti_resource_destroyed);
    g.text_inputs.push_back(ti);
    fprintf(stderr, "huskyfe-wl: text_input created (client=%p, focused_match=%d)\n",
            (void*)client,
            (int)(g.focused && wl_resource_get_client(g.focused->surface_res) == client));


    if (g.focused && wl_resource_get_client(g.focused->surface_res) == client) {
        zwp_text_input_v3_send_enter(tr, g.focused->surface_res);
        ti->entered = true;
    }
}
const struct zwp_text_input_manager_v3_interface tim_impl = {
    tim_destroy, tim_get_text_input,
};
void tim_bind(wl_client* client, void*, uint32_t version, uint32_t id) {
    wl_resource* r = wl_resource_create(client,
                                        &zwp_text_input_manager_v3_interface,
                                        std::min(version, 1u), id);
    if (!r) { wl_client_post_no_memory(client); return; }
    wl_resource_set_implementation(r, &tim_impl, nullptr, nullptr);
}


void text_input_focus_changed(Surface* old_focus, Surface* new_focus) {
    wl_client* old_c = old_focus ? wl_resource_get_client(old_focus->surface_res) : nullptr;
    wl_client* new_c = new_focus ? wl_resource_get_client(new_focus->surface_res) : nullptr;
    for (auto* ti : g.text_inputs) {
        if (ti->entered && ti->client == old_c) {
            zwp_text_input_v3_send_leave(ti->resource, old_focus->surface_res);
            ti->entered = false;

            ti->pending_enabled = false;
            ti->current_enabled = false;
            if (g.active_ti == ti) g.active_ti = nullptr;
        }
        if (!ti->entered && ti->client == new_c && new_focus) {
            zwp_text_input_v3_send_enter(ti->resource, new_focus->surface_res);
            ti->entered = true;
        }
    }
}

}


static void build_keymap() {
    struct xkb_context* ctx = xkb_context_new(XKB_CONTEXT_NO_FLAGS);
    if (!ctx) return;
    struct xkb_rule_names names = {};
    struct xkb_keymap* km = xkb_keymap_new_from_names(ctx, &names,
        XKB_KEYMAP_COMPILE_NO_FLAGS);
    if (!km) { xkb_context_unref(ctx); return; }
    char* str = xkb_keymap_get_as_string(km, XKB_KEYMAP_FORMAT_TEXT_V1);
    size_t len = strlen(str);
    int fd = memfd_create("huskyfe-keymap", MFD_CLOEXEC);
    if (fd >= 0) {
        if (ftruncate(fd, (off_t)(len + 1)) == 0) {
            void* m = mmap(nullptr, len + 1, PROT_READ | PROT_WRITE,
                           MAP_SHARED, fd, 0);
            if (m != MAP_FAILED) {
                memcpy(m, str, len + 1);
                munmap(m, len + 1);
                g.keymap_fd   = fd;
                g.keymap_size = len + 1;
            } else { close(fd); }
        } else { close(fd); }
    }
    free(str);
    xkb_keymap_unref(km);
    xkb_context_unref(ctx);
}

bool init(const char* socket_name) {
    g.display = wl_display_create();
    if (!g.display) { fprintf(stderr, "huskyfe-wl: wl_display_create failed\n"); return false; }
    g.loop = wl_display_get_event_loop(g.display);

    build_keymap();
    if (g.keymap_fd < 0)
        fprintf(stderr, "huskyfe-wl: WARNING: keymap build failed\n");


    if (FILE* f = fopen("/var/lib/huskyfe/app_scales.conf", "r")) {
        char line[256];
        while (fgets(line, sizeof(line), f)) {
            char* p = line;
            while (*p == ' ' || *p == '\t') p++;
            if (*p == '#' || *p == '\n' || *p == 0) continue;
            char key[128]; int val = 0;
            if (sscanf(p, "%127s %d", key, &val) == 2 && val > 0 && val <= 8) {
                g.scale_overrides.emplace_back(key, val);
                fprintf(stderr, "huskyfe-wl: scale override '%s' -> %d\n",
                        key, val);
            }
        }
        fclose(f);
    }

    if (wl_display_init_shm(g.display) < 0) {
        fprintf(stderr, "huskyfe-wl: wl_display_init_shm failed\n");
        return false;
    }

    if (!wl_global_create(g.display, &wl_compositor_interface, 4,
                          nullptr, compositor_bind)) return false;
    if (!wl_global_create(g.display, &wl_subcompositor_interface, 1,
                          nullptr, subcompositor_bind)) return false;
    if (!wl_global_create(g.display, &xdg_wm_base_interface, 3,
                          nullptr, wm_base_bind)) return false;
    if (!wl_global_create(g.display, &wl_seat_interface, 5,
                          nullptr, seat_bind)) return false;
    if (!wl_global_create(g.display, &wl_data_device_manager_interface, 3,
                          nullptr, ddm_bind)) return false;
    if (!wl_global_create(g.display, &wl_output_interface, 3,
                          nullptr, output_bind)) return false;


    if (!wl_global_create(g.display, &zwp_linux_dmabuf_v1_interface, 4,
                          nullptr, dmabuf_bind)) return false;
    if (!wl_global_create(g.display, &wp_viewporter_interface, 1,
                          nullptr, viewporter_bind)) return false;
    if (!wl_global_create(g.display, &zwp_text_input_manager_v3_interface, 1,
                          nullptr, tim_bind)) return false;

    if (wl_display_add_socket(g.display, socket_name) != 0) {
        fprintf(stderr, "huskyfe-wl: add_socket(%s) failed\n", socket_name);
        return false;
    }
    g.socket_fd = wl_event_loop_get_fd(g.loop);
    fprintf(stderr, "huskyfe-wl: listening on %s (fd=%d)\n", socket_name, g.socket_fd);
    return true;
}

void shutdown() {
    if (g.display) wl_display_destroy(g.display);
    g.display = nullptr;
    g.loop = nullptr;
    g.socket_fd = -1;
}

void dispatch() {
    if (!g.display) return;
    wl_event_loop_dispatch(g.loop, 0);
    wl_display_flush_clients(g.display);
}

bool has_active_surface() {
    return g.focused
        && g.focused->is_toplevel
        && g.focused->mapped
        && g.focused->committed_buffer;
}

namespace {


[[maybe_unused]] bool ensure_blit_pipeline() {
    if (blit_program) return true;
    static const char* VS = R"(
attribute vec2 a_pos;
varying   vec2 v_uv;
void main() {
    gl_Position = vec4(a_pos, 0.0, 1.0);
    v_uv = a_pos * 0.5 + 0.5;
}
)";
    static const char* FS = R"(
#extension GL_OES_EGL_image_external : require
precision highp float;
varying   vec2 v_uv;
uniform   samplerExternalOES u_tex;
/* Dither via a cheap pseudo-random hash of pixel coords. Adds ±0.5/255
 * of high-frequency noise — enough to break the visible banding on
 * 8-bit color transitions without making flat regions look noisy.
 * Hash from "Hash without Sine" (David Hoskins, 2014, MIT). */
float hash12(vec2 p) {
    vec3 p3 = fract(vec3(p.xyx) * 0.1031);
    p3 += dot(p3, p3.yzx + 33.33);
    return fract((p3.x + p3.y) * p3.z);
}
void main() {
    vec3 c = texture2D(u_tex, v_uv).rgb;
    float t = hash12(gl_FragCoord.xy) - 0.5;   /* [-0.5, +0.5) */
    c += t / 255.0;
    gl_FragColor = vec4(c, 1.0);
}
)";
    GLuint vs = glCreateShader(GL_VERTEX_SHADER);
    glShaderSource(vs, 1, &VS, nullptr); glCompileShader(vs);
    GLint ok = 0; glGetShaderiv(vs, GL_COMPILE_STATUS, &ok);
    if (!ok) { fprintf(stderr, "huskyfe-wl: blit VS compile failed\n"); return false; }
    GLuint fs = glCreateShader(GL_FRAGMENT_SHADER);
    glShaderSource(fs, 1, &FS, nullptr); glCompileShader(fs);
    glGetShaderiv(fs, GL_COMPILE_STATUS, &ok);
    if (!ok) { fprintf(stderr, "huskyfe-wl: blit FS compile failed\n"); return false; }
    blit_program = glCreateProgram();
    glAttachShader(blit_program, vs);
    glAttachShader(blit_program, fs);
    glBindAttribLocation(blit_program, 0, "a_pos");
    glLinkProgram(blit_program);
    glGetProgramiv(blit_program, GL_LINK_STATUS, &ok);
    if (!ok) { fprintf(stderr, "huskyfe-wl: blit link failed\n"); return false; }
    glDeleteShader(vs); glDeleteShader(fs);
    blit_loc_pos = 0;
    blit_loc_tex = glGetUniformLocation(blit_program, "u_tex");

    static const GLfloat quad[] = { -1,-1, 1,-1, -1,1, 1,1 };
    glGenBuffers(1, &blit_vbo);
    glBindBuffer(GL_ARRAY_BUFFER, blit_vbo);
    glBufferData(GL_ARRAY_BUFFER, sizeof(quad), quad, GL_STATIC_DRAW);
    glBindBuffer(GL_ARRAY_BUFFER, 0);

    glGenFramebuffers(1, &blit_fbo);
    return true;
}

static void upload_committed_dmabuf(Surface* s, DmabufBuffer* b) {
    load_egl_image_funcs();
    if (!p_eglCreateImageKHR || !p_glEGLImageTargetTexture2DOES) return;
    EGLDisplay dpy = p_eglGetCurrentDisplay
                       ? p_eglGetCurrentDisplay() : EGL_NO_DISPLAY;


    if (b->cached_ext_tex && b->cached_egl_image) {
        if (s->texture && !s->is_dmabuf_tex) {
            glDeleteTextures(1, &s->texture);
            s->texture = 0;
        }
        if (s->egl_image && s->egl_image != b->cached_egl_image
            && p_eglDestroyImageKHR) {
            p_eglDestroyImageKHR(dpy, (EGLImageKHR)s->egl_image);
        }
        s->egl_image     = b->cached_egl_image;
        s->texture       = b->cached_ext_tex;
        s->is_dmabuf_tex = true;
        s->tex_w         = b->width;
        s->tex_h         = b->height;
        s->buffer_dirty  = false;


        static unsigned hits = 0;
        hits++;
        return;
    }
    {
        static unsigned misses = 0;
        static struct timespec last_log = {0,0};
        misses++;
        struct timespec now;
        clock_gettime(CLOCK_MONOTONIC, &now);
        if (now.tv_sec - last_log.tv_sec >= 1) {
            fprintf(stderr,
                "huskyfe-wl: dmabuf cache slow-path imports/sec=%u "
                "(this means: every commit is a fresh wl_buffer, no caching)\n",
                misses);
            misses = 0;
            last_log = now;
        }
    }


    if (s->texture && !s->is_dmabuf_tex) {
        glDeleteTextures(1, &s->texture);
        s->texture = 0;
    }

    if (s->egl_image && p_eglDestroyImageKHR) {
        p_eglDestroyImageKHR(dpy, (EGLImageKHR)s->egl_image);
        s->egl_image = nullptr;
    }


    uint32_t use_fourcc = b->fourcc;
    if (const char* over = getenv("HUSKYFE_DMABUF_FOURCC")) {
        uint32_t v = (uint32_t)strtoul(over, nullptr, 0);
        if (v) use_fourcc = v;
    }
    EGLint attr[32]; int n = 0;
    attr[n++] = EGL_WIDTH;                     attr[n++] = b->width;
    attr[n++] = EGL_HEIGHT;                    attr[n++] = b->height;
    attr[n++] = EGL_LINUX_DRM_FOURCC_EXT;      attr[n++] = (EGLint)use_fourcc;
    attr[n++] = EGL_DMA_BUF_PLANE0_FD_EXT;     attr[n++] = b->fds[0];
    attr[n++] = EGL_DMA_BUF_PLANE0_OFFSET_EXT; attr[n++] = (EGLint)b->offsets[0];
    attr[n++] = EGL_DMA_BUF_PLANE0_PITCH_EXT;  attr[n++] = (EGLint)b->strides[0];


    constexpr uint64_t MOD_INVALID = 0x00ffffffffffffffULL;
    if (b->mods[0] && b->mods[0] != MOD_INVALID) {
        attr[n++] = EGL_DMA_BUF_PLANE0_MODIFIER_LO_EXT;
        attr[n++] = (EGLint)(b->mods[0] & 0xffffffffu);
        attr[n++] = EGL_DMA_BUF_PLANE0_MODIFIER_HI_EXT;
        attr[n++] = (EGLint)(b->mods[0] >> 32);
    }
    attr[n++] = EGL_NONE;

    if (dbg_dmabuf()) {
        struct stat st = {};
        int fr = fstat(b->fds[0], &st);
        const char* mod_note =
            (b->mods[0] == 0)             ? "0=LINEAR-implied (omitted)"
          : (b->mods[0] == MOD_INVALID)   ? "INVALID (omitted)"
                                          : "explicit";
        fprintf(stderr,
                "huskyfe-wl: dmabuf: import surf=%p fd=%d (fstat rc=%d "
                "ino=%llu size=%llu) %dx%d fourcc=0x%08x "
                "use_fourcc=0x%08x stride=%u offset=%u mod=0x%016llx (%s)\n",
                (void*)s, b->fds[0], fr,
                (unsigned long long)st.st_ino,
                (unsigned long long)st.st_size,
                b->width, b->height, b->fourcc, use_fourcc,
                b->strides[0], b->offsets[0],
                (unsigned long long)b->mods[0], mod_note);


        size_t map_len = (size_t)st.st_size;
        if (map_len > 0) {
            void* m = mmap(nullptr, map_len, PROT_READ, MAP_SHARED,
                           b->fds[0], 0);
            if (m != MAP_FAILED) {
                const uint8_t* p = (const uint8_t*)m;


                size_t row_bytes = (size_t)b->strides[0];
                size_t row_top   = 0;
                size_t row_mid   = (size_t)(b->height / 2) * row_bytes;
                size_t row_bot   = (size_t)(b->height - 1) * row_bytes;
                if (row_bot + 32 > map_len) row_bot = map_len > 32 ? map_len - 32 : 0;
                auto dump_row = [&](const char* tag, size_t off) {
                    char hex[16*3+1] = {0};
                    size_t lim = off + 16 <= map_len ? 16 : 0;
                    for (size_t i = 0; i < lim; i++)
                        snprintf(hex + i*3, 4, "%02x ", p[off + i]);
                    fprintf(stderr,
                            "huskyfe-wl: dmabuf:   mmap %s@%zu: %s\n",
                            tag, off, hex);
                };
                dump_row("top",    row_top);
                dump_row("middle", row_mid);
                dump_row("bottom", row_bot);
                size_t nz = 0;
                for (size_t i = 0; i < map_len; i++) if (p[i]) nz++;
                fprintf(stderr,
                        "huskyfe-wl: dmabuf:   nonzero bytes: %zu/%zu (%.1f%%)\n",
                        nz, map_len, 100.0 * (double)nz / (double)map_len);
                munmap(m, map_len);
            } else {
                fprintf(stderr, "huskyfe-wl: dmabuf:   mmap failed: %s\n",
                        strerror(errno));
            }
        }
    }

    EGLImageKHR img = p_eglCreateImageKHR(dpy, EGL_NO_CONTEXT,
                                          EGL_LINUX_DMA_BUF_EXT, NULL, attr);
    if (dbg_dmabuf()) {
        EGLint ee = eglGetError();
        fprintf(stderr,
                "huskyfe-wl: dmabuf:   eglCreateImageKHR -> %p "
                "eglGetError=0x%04x\n", (void*)img, ee);
    }
    if (img == EGL_NO_IMAGE_KHR) {
        fprintf(stderr, "huskyfe-wl: dmabuf import failed (%dx%d fourcc=0x%x)\n",
                b->width, b->height, b->fourcc);
        return;
    }


    constexpr GLenum TEX_EXT = 0x8D65;
    GLuint ext_tex = 0;
    glGenTextures(1, &ext_tex);
    glBindTexture(TEX_EXT, ext_tex);
    glTexParameteri(TEX_EXT, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(TEX_EXT, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(TEX_EXT, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(TEX_EXT, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    p_glEGLImageTargetTexture2DOES(TEX_EXT, (GLeglImageOES)img);
    if (dbg_dmabuf()) {
        GLenum ge = glGetError();
        fprintf(stderr,
                "huskyfe-wl: dmabuf:   glEGLImageTargetTexture2DOES(EXTERNAL) "
                "img=%p glGetError=0x%04x\n", (void*)img, ge);
    }
    glBindTexture(TEX_EXT, 0);

    b->cached_egl_image = (void*)img;
    b->cached_ext_tex   = ext_tex;

    s->egl_image     = b->cached_egl_image;
    s->texture       = b->cached_ext_tex;
    s->is_dmabuf_tex = true;
    s->tex_w         = b->width;
    s->tex_h         = b->height;
    s->buffer_dirty  = false;


    s->needs_rb_swap = (b->fourcc == 0x34325241
                     || b->fourcc == 0x34325258 );
}

static void upload_committed(Surface* s) {
    if (!s->committed_buffer) return;
    if (!s->buffer_dirty && s->texture) return;


    if (wl_resource_instance_of(s->committed_buffer,
                                &wl_buffer_interface, &dmabuf_buffer_impl)) {
        DmabufBuffer* b = dmabuf_of(s->committed_buffer);
        if (b) upload_committed_dmabuf(s, b);
        return;
    }

    wl_shm_buffer* sb = wl_shm_buffer_get(s->committed_buffer);
    if (!sb) return;

    int32_t  w      = wl_shm_buffer_get_width(sb);
    int32_t  h      = wl_shm_buffer_get_height(sb);
    int32_t  stride = wl_shm_buffer_get_stride(sb);
    uint32_t fmt    = wl_shm_buffer_get_format(sb);

    wl_shm_buffer_begin_access(sb);
    void* data = wl_shm_buffer_get_data(sb);


    if (s->is_dmabuf_tex && s->texture) {
        glDeleteTextures(1, &s->texture);
        s->texture = 0;
        s->is_dmabuf_tex = false;
    }
    if (s->egl_image && p_eglDestroyImageKHR && p_eglGetCurrentDisplay) {
        p_eglDestroyImageKHR(p_eglGetCurrentDisplay(), (EGLImageKHR)s->egl_image);
        s->egl_image = nullptr;
    }
    if (s->texture == 0) glGenTextures(1, &s->texture);
    glBindTexture(GL_TEXTURE_2D, s->texture);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);


    s->needs_rb_swap = (fmt == 0
                     || fmt == 1 );
    glPixelStorei(GL_UNPACK_ROW_LENGTH, stride / 4);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, w, h, 0,
                 GL_RGBA, GL_UNSIGNED_BYTE, data);
    glPixelStorei(GL_UNPACK_ROW_LENGTH, 0);
    wl_shm_buffer_end_access(sb);

    s->tex_w = w;
    s->tex_h = h;
    s->buffer_dirty = false;
}
}

void draw(ImageRenderer& ir, const float xform[4],
          float screen_w, float screen_h) {
    if (!has_active_surface()) return;
    Surface* active = g.focused;
    upload_committed(active);
    if (!active->texture) return;

    {
        static int log_n = 0;
        if (log_n < 4) {
            fprintf(stderr, "huskyfe-wl: draw active surf=%p dmabuf_tex=%d "
                    "is_xwayland=%d kids=%zu\n",
                    (void*)active, (int)active->is_dmabuf_tex,
                    (int)active->is_xwayland, active->children.size());
            log_n++;
        }
    }
    if (active->is_dmabuf_tex)
        ir.begin_external(xform, active->texture, active->needs_rb_swap);
    else
        ir.begin(xform, active->texture, active->needs_rb_swap);
    ir.draw(0.0f, 0.0f, screen_w, screen_h, 0.0f, 0.0f, 1.0f, 1.0f, 1.0f);
    ir.end();


    if (!active->children.empty() && active->tex_w > 0 && active->tex_h > 0) {
        const float sx = screen_w / (float)active->tex_w;
        const float sy = screen_h / (float)active->tex_h;
        for (Surface* c : active->children) {
            if (!c) continue;
            upload_committed(c);
            if (!c->texture || c->tex_w <= 0 || c->tex_h <= 0) continue;
            float cx = (float)c->sub_x * sx;
            float cy = (float)c->sub_y * sy;
            float cw = (float)c->tex_w * sx;
            float ch = (float)c->tex_h * sy;
            {
                static int log_n = 0;
                if (log_n < 4) {
                    fprintf(stderr, "huskyfe-wl: draw child surf=%p dmabuf_tex=%d "
                            "is_xwayland=%d\n",
                            (void*)c, (int)c->is_dmabuf_tex, (int)c->is_xwayland);
                    log_n++;
                }
            }
            if (c->is_dmabuf_tex)
                ir.begin_external(xform, c->texture, c->needs_rb_swap);
            else
                ir.begin(xform, c->texture, c->needs_rb_swap);
            ir.draw(cx, cy, cw, ch, 0.0f, 0.0f, 1.0f, 1.0f, 1.0f);
            ir.end();
        }
    }


    glUseProgram(0);
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, 0);
    glBindBuffer(GL_ARRAY_BUFFER, 0);
    glEnable(GL_BLEND);
    glBlendFunc(GL_ONE, GL_ONE_MINUS_SRC_ALPHA);
}


void flush_deferred_releases() {
    for (auto* s : g.surfaces) {
        for (auto* buf : s->deferred_releases)
            wl_buffer_send_release(buf);
        s->deferred_releases.clear();
    }
    if (g.display) wl_display_flush_clients(g.display);
}

void send_frame_callbacks(uint32_t time_ms) {
    for (auto* s : g.surfaces) {


        bool is_dmabuf = s->committed_buffer
            && wl_resource_instance_of(s->committed_buffer,
                                       &wl_buffer_interface,
                                       &dmabuf_buffer_impl);


        std::vector<wl_resource*> to_fire;
        to_fire.swap(s->active_frame_cbs);
        if (is_dmabuf) {


            uint32_t since = time_ms - s->last_frame_cb_ms;


            if (since < 8) {

                for (auto* cb : to_fire)
                    s->active_frame_cbs.push_back(cb);
                continue;
            }
            s->last_frame_cb_ms = time_ms;
            if (!to_fire.empty()) {
                wl_callback_send_done(to_fire[0], time_ms);
                wl_resource_destroy(to_fire[0]);
                for (size_t i = 1; i < to_fire.size(); i++)
                    s->active_frame_cbs.push_back(to_fire[i]);
            }
        } else {

            for (auto* cb : to_fire) {
                wl_callback_send_done(cb, time_ms);
                wl_resource_destroy(cb);
            }
        }
    }
    if (g.display) wl_display_flush_clients(g.display);
}


std::vector<RunningApp> running() {
    std::vector<RunningApp> out;
    for (auto* s : g.surfaces) {
        if (!s->is_toplevel || !s->mapped) continue;
        out.push_back({(AppHandle)s, s->title, s->app_id});
    }
    return out;
}

static Surface* surface_for_handle(AppHandle h) {
    for (auto* s : g.surfaces) if ((AppHandle)s == h) return s;
    return nullptr;
}

void focus(AppHandle h) {
    Surface* s = surface_for_handle(h);
    if (s && s->is_toplevel && s->mapped) {
        Surface* old = g.focused;
        g.focused = s;
        if (old != s) on_focus_changed(old, s);
    }
}

void unfocus() {
    Surface* old = g.focused;
    g.focused = nullptr;
    g.touched_surface = nullptr;
    if (old) on_focus_changed(old, nullptr);
}


static bool osk_blocked_for_app_id(const std::string& app_id) {
    static const char* kBlocked[] = {
        "sm64",
        "mupen",
        "retroarch",
        "dosbox",
        nullptr,
    };
    if (app_id.empty()) return false;
    std::string nid;
    nid.reserve(app_id.size());
    for (char c : app_id) {
        if (c == '-' || c == '_' || c == '.' || c == '/' || c == ' ') continue;
        nid.push_back((char)std::tolower((unsigned char)c));
    }
    for (const char** p = kBlocked; *p; ++p) {
        if (nid.find(*p) != std::string::npos) return true;
    }
    return false;
}

bool text_input_wanted() {
    if (!g.focused || !g.active_ti) return false;
    if (osk_blocked_for_app_id(g.focused->app_id)) return false;
    wl_client* fc = wl_resource_get_client(g.focused->surface_res);
    return g.active_ti->client == fc
        && g.active_ti->entered
        && g.active_ti->current_enabled;
}

void set_bottom_inset(int px) {
    g.bottom_inset = px;
}

void close_focused() {
    if (g.focused && g.focused->toplevel_res)
        xdg_toplevel_send_close(g.focused->toplevel_res);
}

void close(AppHandle h) {
    Surface* s = surface_for_handle(h);
    if (s && s->toplevel_res) xdg_toplevel_send_close(s->toplevel_res);
}


namespace {

Surface* active_surface() {
    return has_active_surface() ? g.focused : nullptr;
}


void map_to_surface(Surface* s, float x_screen, float y_screen,
                    wl_fixed_t* sx_out, wl_fixed_t* sy_out) {
    constexpr float screen_w = 1344.0f;
    constexpr float screen_h = 2992.0f;
    float buf_x = (s->tex_w > 0) ? x_screen * (float)s->tex_w / screen_w : x_screen;
    float buf_y = (s->tex_h > 0) ? y_screen * (float)s->tex_h / screen_h : y_screen;
    float scale = (s->scale > 0) ? (float)s->scale : (float)DEFAULT_OUTPUT_SCALE;
    *sx_out = wl_fixed_from_double(buf_x / scale);
    *sy_out = wl_fixed_from_double(buf_y / scale);
}


void for_each_client_touch(wl_client* client,
                           void (*fn)(wl_resource*, void*), void* ud) {
    for (auto* tr : g.touches)
        if (wl_resource_get_client(tr) == client) fn(tr, ud);
}
void for_each_client_pointer(wl_client* client,
                             void (*fn)(wl_resource*, void*), void* ud) {
    for (auto* pr : g.pointers)
        if (wl_resource_get_client(pr) == client) fn(pr, ud);
}


#define BTN_LEFT 0x110

}


static void pointer_send_enter(Surface* s, wl_fixed_t sx, wl_fixed_t sy) {
    if (g.pemu_entered == s) return;
    if (g.pemu_entered) {

        uint32_t serial = wl_display_next_serial(g.display);
        wl_client* prev_client = wl_resource_get_client(g.pemu_entered->surface_res);
        struct Args { uint32_t serial; wl_resource* surface; };
        Args a{serial, g.pemu_entered->surface_res};
        for_each_client_pointer(prev_client, [](wl_resource* pr, void* ud) {
            auto* a = (Args*)ud;
            wl_pointer_send_leave(pr, a->serial, a->surface);
            if (wl_resource_get_version(pr) >= 5) wl_pointer_send_frame(pr);
        }, &a);
    }
    uint32_t serial = wl_display_next_serial(g.display);
    wl_client* client = wl_resource_get_client(s->surface_res);
    struct Args { uint32_t serial; wl_resource* surface; wl_fixed_t sx, sy; };
    Args a{serial, s->surface_res, sx, sy};
    for_each_client_pointer(client, [](wl_resource* pr, void* ud) {
        auto* a = (Args*)ud;
        wl_pointer_send_enter(pr, a->serial, a->surface, a->sx, a->sy);
    }, &a);
    g.pemu_entered = s;
}

void touch_down(uint32_t time_ms, int32_t slot, float x, float y) {
    Surface* s = active_surface();
    if (!s) return;
    g.touched_surface = s;
    wl_fixed_t sx, sy;
    map_to_surface(s, x, y, &sx, &sy);
    uint32_t serial = wl_display_next_serial(g.display);
    wl_client* client = wl_resource_get_client(s->surface_res);
    struct Args { uint32_t serial, time, slot; wl_resource* surface; wl_fixed_t sx, sy; };
    Args a{serial, time_ms, (uint32_t)slot, s->surface_res, sx, sy};
    for_each_client_touch(client, [](wl_resource* tr, void* ud) {
        auto* a = (Args*)ud;
        wl_touch_send_down(tr, a->serial, a->time, a->surface,
                           (int32_t)a->slot, a->sx, a->sy);
    }, &a);

    if (slot == 0) {
        pointer_send_enter(s, sx, sy);
        struct PArgs { uint32_t time; wl_fixed_t sx, sy; uint32_t serial; };
        PArgs p{time_ms, sx, sy, serial};
        for_each_client_pointer(client, [](wl_resource* pr, void* ud) {
            auto* p = (PArgs*)ud;
            wl_pointer_send_motion(pr, p->time, p->sx, p->sy);
            wl_pointer_send_button(pr, p->serial, p->time,
                                   BTN_LEFT, WL_POINTER_BUTTON_STATE_PRESSED);
        }, &p);
        g.pemu_down = true;
    }
}

void touch_motion(uint32_t time_ms, int32_t slot, float x, float y) {
    Surface* s = g.touched_surface;
    if (!s) return;
    wl_fixed_t sx, sy;
    map_to_surface(s, x, y, &sx, &sy);
    wl_client* client = wl_resource_get_client(s->surface_res);
    struct Args { uint32_t time, slot; wl_fixed_t sx, sy; };
    Args a{time_ms, (uint32_t)slot, sx, sy};
    for_each_client_touch(client, [](wl_resource* tr, void* ud) {
        auto* a = (Args*)ud;
        wl_touch_send_motion(tr, a->time, (int32_t)a->slot, a->sx, a->sy);
    }, &a);
    if (slot == 0 && g.pemu_entered == s) {
        struct PArgs { uint32_t time; wl_fixed_t sx, sy; };
        PArgs p{time_ms, sx, sy};
        for_each_client_pointer(client, [](wl_resource* pr, void* ud) {
            auto* p = (PArgs*)ud;
            wl_pointer_send_motion(pr, p->time, p->sx, p->sy);
        }, &p);
    }
}

void touch_up(uint32_t time_ms, int32_t slot) {
    Surface* s = g.touched_surface;
    if (!s) return;
    uint32_t serial = wl_display_next_serial(g.display);
    wl_client* client = wl_resource_get_client(s->surface_res);
    struct Args { uint32_t serial, time, slot; };
    Args a{serial, time_ms, (uint32_t)slot};
    for_each_client_touch(client, [](wl_resource* tr, void* ud) {
        auto* a = (Args*)ud;
        wl_touch_send_up(tr, a->serial, a->time, (int32_t)a->slot);
    }, &a);
    if (slot == 0 && g.pemu_down) {
        struct PArgs { uint32_t time, serial; };
        PArgs p{time_ms, serial};
        for_each_client_pointer(client, [](wl_resource* pr, void* ud) {
            auto* p = (PArgs*)ud;
            wl_pointer_send_button(pr, p->serial, p->time,
                                   BTN_LEFT, WL_POINTER_BUTTON_STATE_RELEASED);
        }, &p);
        g.pemu_down = false;
    }
}

void touch_frame() {
    Surface* s = g.touched_surface;
    if (!s) return;
    wl_client* client = wl_resource_get_client(s->surface_res);


    for_each_client_touch(client, [](wl_resource* tr, void*) {
        if (wl_resource_get_version(tr) >= 5) wl_touch_send_frame(tr);
    }, nullptr);
    for_each_client_pointer(client, [](wl_resource* pr, void*) {
        if (wl_resource_get_version(pr) >= 5) wl_pointer_send_frame(pr);
    }, nullptr);
    if (g.display) wl_display_flush_clients(g.display);
}


namespace {


struct AsciiKey { uint16_t keycode; bool shift; };


constexpr uint8_t LETTER_KC[26] = {
    30,48,46,32,18,33,34,35,23,36,37,38,50,49,24,25,16,19,31,20,
    22,47,17,45,21,44 };

AsciiKey ascii_to_key(char c) {
    switch (c) {
    case ' ':  return {57, false};
    case '\n': return {28, false};
    case '\t': return {15, false};
    case '\b': return {14, false};
    case '\x1b': return {1, false};
    case '0': return {11, false}; case ')': return {11, true};
    case '1': return {2,  false}; case '!': return {2,  true};
    case '2': return {3,  false}; case '@': return {3,  true};
    case '3': return {4,  false}; case '#': return {4,  true};
    case '4': return {5,  false}; case '$': return {5,  true};
    case '5': return {6,  false}; case '%': return {6,  true};
    case '6': return {7,  false}; case '^': return {7,  true};
    case '7': return {8,  false}; case '&': return {8,  true};
    case '8': return {9,  false}; case '*': return {9,  true};
    case '9': return {10, false}; case '(': return {10, true};
    case '-': return {12, false}; case '_': return {12, true};
    case '=': return {13, false}; case '+': return {13, true};
    case '[': return {26, false}; case '{': return {26, true};
    case ']': return {27, false}; case '}': return {27, true};
    case '\\': return {43, false}; case '|': return {43, true};
    case ';': return {39, false}; case ':': return {39, true};
    case '\'': return {40, false}; case '"': return {40, true};
    case '`': return {41, false}; case '~': return {41, true};
    case ',': return {51, false}; case '<': return {51, true};
    case '.': return {52, false}; case '>': return {52, true};
    case '/': return {53, false}; case '?': return {53, true};
    default: break;
    }
    if (c >= 'a' && c <= 'z') return {LETTER_KC[c - 'a'], false};
    if (c >= 'A' && c <= 'Z') return {LETTER_KC[c - 'A'], true};
    return {0, false};
}

void keyboard_focus_to(Surface* s, wl_client* client) {
    if (g.keyboard_focused == s) return;
    if (g.keyboard_focused) {

        wl_client* pc = wl_resource_get_client(g.keyboard_focused->surface_res);
        uint32_t serial = wl_display_next_serial(g.display);
        for (auto* kr : g.keyboards) {
            if (wl_resource_get_client(kr) != pc) continue;
            wl_keyboard_send_leave(kr, serial, g.keyboard_focused->surface_res);
        }
    }
    g.keyboard_focused = s;
    if (!s) return;
    uint32_t serial = wl_display_next_serial(g.display);
    struct wl_array keys;
    wl_array_init(&keys);
    for (auto* kr : g.keyboards) {
        if (wl_resource_get_client(kr) != client) continue;
        wl_keyboard_send_enter(kr, serial, s->surface_res, &keys);

        wl_keyboard_send_modifiers(kr, serial, 0, 0, 0, 0);
    }
    wl_array_release(&keys);
}
}

void keyboard_send_char(uint32_t time_ms, char c) {
    if (!g.focused) return;
    AsciiKey ak = ascii_to_key(c);
    if (ak.keycode == 0) return;
    wl_client* client = wl_resource_get_client(g.focused->surface_res);
    keyboard_focus_to(g.focused, client);

    uint32_t serial_mods = wl_display_next_serial(g.display);
    uint32_t serial_dn   = wl_display_next_serial(g.display);
    uint32_t serial_up   = wl_display_next_serial(g.display);
    constexpr uint32_t MOD_SHIFT = 1u;
    for (auto* kr : g.keyboards) {
        if (wl_resource_get_client(kr) != client) continue;
        if (ak.shift)
            wl_keyboard_send_modifiers(kr, serial_mods, MOD_SHIFT, 0, 0, 0);
        wl_keyboard_send_key(kr, serial_dn, time_ms, ak.keycode,
                             WL_KEYBOARD_KEY_STATE_PRESSED);
        wl_keyboard_send_key(kr, serial_up, time_ms + 1, ak.keycode,
                             WL_KEYBOARD_KEY_STATE_RELEASED);
        if (ak.shift)
            wl_keyboard_send_modifiers(kr, wl_display_next_serial(g.display),
                                       0, 0, 0, 0);
    }
    if (g.display) wl_display_flush_clients(g.display);
}

bool focused_is_terminal() {
    if (!g.focused) return false;
    std::string id = normalize(g.focused->app_id);
    static const char* terms[] = {
        "foot", "alacritty", "kitty", "wezterm",
        "konsole", "gnometerminal", "xtermjs", "weston",
    };
    for (const char* t : terms)
        if (id.find(t) != std::string::npos) return true;
    return false;
}

}
