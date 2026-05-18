

#include <GLES3/gl3.h>
#include <EGL/egl.h>
#include <xf86drm.h>
#include <xf86drmMode.h>
#include <linux/dma-heap.h>
#include <sys/ioctl.h>
#include <sys/select.h>
#include <fcntl.h>
#include <unistd.h>
#include <signal.h>
#include <execinfo.h>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <cmath>
#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <ctime>
#include <utility>

#include <sys/stat.h>
#include <sys/statvfs.h>
#include <dirent.h>
#include <string>
#include <vector>
#include <sys/sysinfo.h>
#include <sys/utsname.h>

#include "Apps.h"
#include "Background.h"
#include "Blur.h"
#include "Icons.h"
#include "ImageRenderer.h"

#include "third_party/stb_image.h"
#include "Input.h"
#include "Keyboard.h"
#include "Renderer.h"
#include "Spring.h"
#include "Status.h"
#include "Text.h"
#include "WaylandHost.h"
#include "Wifi.h"
#include "Haptics.h"
#include "Flashlight.h"
#include "Camera.h"
#include "Bluetooth.h"
#include "Notifications.h"

#include <sys/socket.h>
#include <sys/un.h>
#include <sys/wait.h>
#include <vector>
#include <sstream>
#include <cctype>
#include <mutex>
#include <thread>
#include <atomic>
#include <functional>

extern "C" {
    int glp_set_output_dmabuf(int fd, uint32_t w, uint32_t h, uint32_t stride, uint32_t fourcc);
    int glp_present(void);
}


static void sd_notify(const char* state) {
    const char* path = getenv("NOTIFY_SOCKET");
    if (!path || !*path) return;
    int s = socket(AF_UNIX, SOCK_DGRAM | SOCK_CLOEXEC, 0);
    if (s < 0) return;
    struct sockaddr_un sa{};
    sa.sun_family = AF_UNIX;
    if (path[0] == '@') {

        sa.sun_path[0] = 0;
        strncpy(sa.sun_path + 1, path + 1, sizeof(sa.sun_path) - 2);
    } else {
        strncpy(sa.sun_path, path, sizeof(sa.sun_path) - 1);
    }
    sendto(s, state, strlen(state), MSG_NOSIGNAL,
           (sockaddr*)&sa, sizeof(sa));
    close(s);
}


static void launch_app(const std::string& exec) {
    if (exec.empty()) return;
    pid_t p = fork();
    if (p < 0) { perror("fork"); return; }
    if (p == 0) {

        if (fork() != 0) _exit(0);
        setsid();

        std::vector<std::string> toks;
        std::istringstream is(exec);
        for (std::string t; is >> t; ) {
            if (!t.empty() && t[0] == '%') continue;
            toks.push_back(std::move(t));
        }
        if (toks.empty()) _exit(127);
        std::vector<char*> argv;
        for (auto& s : toks) argv.push_back(s.data());
        argv.push_back(nullptr);


        {
            std::string base = toks[0];
            if (auto sl = base.rfind('/'); sl != std::string::npos)
                base = base.substr(sl + 1);
            for (auto& c : base) if (c == ' ' || c == '/') c = '_';
            std::string path = "/tmp/huskyfe-app-" + base + ".log";
            int lf = open(path.c_str(), O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC,
                          0644);
            if (lf >= 0) {
                dup2(lf, 1);
                dup2(lf, 2);
                close(lf);
            }
        }


        setenv("XDG_RUNTIME_DIR", "/run/user/0", 1);
        setenv("WAYLAND_DISPLAY", "wayland-1",   1);
        setenv("DBUS_SESSION_BUS_ADDRESS",
               "unix:path=/run/user/0/bus", 1);

        setenv("GDK_BACKEND",  "wayland", 1);
        setenv("QT_QPA_PLATFORM", "wayland", 1);
        setenv("MOZ_ENABLE_WAYLAND", "1", 1);


        setenv("GTK_USE_PORTAL",       "0", 1);
        setenv("GTK_A11Y",              "none", 1);
        setenv("NO_AT_BRIDGE",          "1", 1);
        setenv("GIO_USE_VFS",           "local", 1);


        setenv("QT_ACCESSIBILITY",      "0", 1);
        fprintf(stderr, "=== huskyfe-app launched: %s ===\n", argv[0]);
        execvp(argv[0], argv.data());
        perror("execvp");
        _exit(127);
    }

    int status = 0;
    waitpid(p, &status, 0);
}

namespace {

constexpr uint32_t FOURCC_ABGR8888 = 0x34324241;

volatile sig_atomic_t g_quit = 0;
bool g_blanked = false;
huskyfe::Spring g_fade;
bool g_fading_out = false;
bool g_fading_in  = false;
void on_sigint(int) { g_quit = 1; }

struct Dmabuf {
    int fd = -1;
    uint32_t w = 0, h = 0, stride = 0;
    uint32_t fb_id = 0;
};

bool alloc_dmabuf(uint32_t w, uint32_t h, Dmabuf& out) {
    int heap = open("/dev/dma_heap/system", O_RDWR | O_CLOEXEC);
    if (heap < 0) { perror("open dma_heap/system"); return false; }
    dma_heap_allocation_data data{};
    data.len = static_cast<uint64_t>(w) * h * 4;
    data.fd_flags = O_RDWR | O_CLOEXEC;
    if (ioctl(heap, DMA_HEAP_IOCTL_ALLOC, &data) < 0) { perror("DMA_HEAP_IOCTL_ALLOC"); close(heap); return false; }
    close(heap);
    out.fd = static_cast<int>(data.fd);
    out.w = w; out.h = h; out.stride = w * 4;
    return true;
}


std::atomic<bool> g_thermal_low_request{false};
std::atomic<bool> g_thermal_low_active{false};

int read_zone_mc(const char* path) {
    FILE* f = fopen(path, "r");
    if (!f) return -1;
    int v = -1;
    if (fscanf(f, "%d", &v) != 1) v = -1;
    fclose(f);
    return v;
}


std::string thermal_zone_path(const char* type_name) {
    for (int i = 0; i < 64; i++) {
        char tp[128]; snprintf(tp, sizeof(tp),
                               "/sys/class/thermal/thermal_zone%d/type", i);
        FILE* f = fopen(tp, "r");
        if (!f) continue;
        char buf[64] = {0};
        if (!fgets(buf, sizeof(buf), f)) { fclose(f); continue; }
        fclose(f);
        size_t L = strlen(buf);
        if (L && buf[L - 1] == '\n') buf[L - 1] = 0;
        if (strcmp(buf, type_name) == 0) {
            char rp[128]; snprintf(rp, sizeof(rp),
                                   "/sys/class/thermal/thermal_zone%d/temp", i);
            return std::string(rp);
        }
    }
    return std::string();
}

void send_notify(const char* summary, const char* body) {


    pid_t pid = fork();
    if (pid == 0) {
        execlp("notify-send", "notify-send",
               "-a", "huskyfe",
               "-u", "normal",
               summary, body, (char*)nullptr);
        _exit(127);
    }
    if (pid > 0) {


    }
}

void thermal_monitor_thread() {


    int HOT_MC  = 80000;
    int COOL_MC = 75000;
    int POLL_S  = 5;
    if (const char* v = getenv("HUSKYFE_THERMAL_HOT"))   HOT_MC  = atoi(v);
    if (const char* v = getenv("HUSKYFE_THERMAL_COOL"))  COOL_MC = atoi(v);
    if (const char* v = getenv("HUSKYFE_THERMAL_POLL"))  POLL_S  = atoi(v);
    if (HOT_MC <= COOL_MC) HOT_MC = COOL_MC + 1000;
    if (POLL_S < 1) POLL_S = 1;
    fprintf(stderr, "huskyfe: thermal thresholds hot=%dmC cool=%dmC poll=%ds\n",
            HOT_MC, COOL_MC, POLL_S);

    std::string p_big = thermal_zone_path("BIG");
    std::string p_mid = thermal_zone_path("MID");
    if (p_big.empty() && p_mid.empty()) {
        fprintf(stderr, "huskyfe: thermal zones BIG/MID not found, monitor disabled\n");
        return;
    }
    fprintf(stderr, "huskyfe: thermal monitor active (BIG=%s MID=%s)\n",
            p_big.c_str(), p_mid.c_str());

    while (!g_quit) {
        int t_big = p_big.empty() ? -1 : read_zone_mc(p_big.c_str());
        int t_mid = p_mid.empty() ? -1 : read_zone_mc(p_mid.c_str());
        int t = std::max(t_big, t_mid);
        if (t > 0) {
            bool req = g_thermal_low_request.load();
            if (!req && t >= HOT_MC) {
                g_thermal_low_request.store(true);
                fprintf(stderr, "huskyfe: thermal trip — request low refresh "
                                "(BIG=%dmC MID=%dmC)\n", t_big, t_mid);
                send_notify("Thermal protection",
                            "Display rate reduced (overheating)");
            } else if (req && t <= COOL_MC) {
                g_thermal_low_request.store(false);
                fprintf(stderr, "huskyfe: thermal recovered — request high refresh "
                                "(BIG=%dmC MID=%dmC)\n", t_big, t_mid);
                send_notify("Thermal recovered",
                            "Display rate restored");
            }
        }
        for (int i = 0; i < POLL_S * 10 && !g_quit; i++) {
            usleep(100 * 1000);
        }
    }
}

}


static void crash_handler(int sig) {
    static const char prefix[] = "\n\n*** HUSKYFE CRASH (signal ";
    write(STDERR_FILENO, prefix, sizeof(prefix) - 1);
    char nbuf[8];
    int n = snprintf(nbuf, sizeof(nbuf), "%d) ***\n", sig);
    write(STDERR_FILENO, nbuf, n);
    void* frames[64];
    int n_frames = backtrace(frames, 64);
    backtrace_symbols_fd(frames, n_frames, STDERR_FILENO);
    write(STDERR_FILENO, "*** end backtrace ***\n", 22);

    signal(sig, SIG_DFL);
    raise(sig);
}

int main() {
    signal(SIGINT,  on_sigint);
    signal(SIGTERM, on_sigint);
    {
        struct sigaction sa{};
        sa.sa_handler = crash_handler;
        sa.sa_flags   = SA_RESETHAND;
        sigemptyset(&sa.sa_mask);
        for (int s : {SIGSEGV, SIGBUS, SIGABRT, SIGFPE, SIGILL}) sigaction(s, &sa, nullptr);
    }

    if (!huskyfe::input_open()) {
        fprintf(stderr, "huskyfe: input_open failed (continuing without input)\n");
    }

    int drm_fd = open("/dev/dri/card0", O_RDWR | O_CLOEXEC);
    if (drm_fd < 0) { perror("open /dev/dri/card0"); return 1; }
    if (drmSetMaster(drm_fd) < 0) {
        perror("drmSetMaster — kill compositor first (wcomp/cage/phoc/phosh/weston/modetest)");
        return 1;
    }

    drmModeRes* res = drmModeGetResources(drm_fd);
    if (!res) { perror("drmModeGetResources"); return 1; }

    drmModeConnector* conn = nullptr;
    for (int i = 0; i < res->count_connectors; i++) {
        drmModeConnector* c = drmModeGetConnector(drm_fd, res->connectors[i]);
        if (c && c->connection == DRM_MODE_CONNECTED && c->count_modes > 0) { conn = c; break; }
        if (c) drmModeFreeConnector(c);
    }
    if (!conn) { fprintf(stderr, "huskyfe: no connected connector\n"); return 1; }


    drmModeModeInfo mode = conn->modes[0];
    for (int i = 1; i < conn->count_modes; i++) {
        const drmModeModeInfo& m = conn->modes[i];
        int cur = (int)mode.hdisplay * (int)mode.vdisplay;
        int chk = (int)m.hdisplay    * (int)m.vdisplay;
        if (chk > cur || (chk == cur && m.vrefresh > mode.vrefresh)) mode = m;
    }
    fprintf(stderr, "huskyfe: %ux%u@%uHz on connector %u\n",
            mode.hdisplay, mode.vdisplay, mode.vrefresh, conn->connector_id);


    drmModeModeInfo mode_low = mode;
    for (int i = 0; i < conn->count_modes; i++) {
        const drmModeModeInfo& m = conn->modes[i];
        if (m.hdisplay == mode.hdisplay && m.vdisplay == mode.vdisplay
            && m.vrefresh < mode_low.vrefresh) mode_low = m;
    }
    fprintf(stderr, "huskyfe: thermal-low mode = %ux%u@%uHz\n",
            mode_low.hdisplay, mode_low.vdisplay, mode_low.vrefresh);


    drmModeEncoder* enc = nullptr;
    uint32_t crtc_id = 0;
    for (int i = 0; i < conn->count_encoders && !crtc_id; i++) {
        drmModeEncoder* e = drmModeGetEncoder(drm_fd, conn->encoders[i]);
        if (!e) continue;
        if (e->crtc_id) {
            enc = e; crtc_id = e->crtc_id; break;
        }
        for (int j = 0; j < res->count_crtcs; j++) {
            if (e->possible_crtcs & (1u << j)) {
                enc = e; crtc_id = res->crtcs[j]; break;
            }
        }
        if (!crtc_id) drmModeFreeEncoder(e);
    }
    if (!crtc_id) { fprintf(stderr, "huskyfe: no CRTC\n"); return 1; }
    fprintf(stderr, "huskyfe: encoder %u, crtc %u\n",
            enc ? enc->encoder_id : 0, crtc_id);


    uint32_t dpms_prop_id = 0;
    if (drmModeObjectProperties* props = drmModeObjectGetProperties(
            drm_fd, conn->connector_id, DRM_MODE_OBJECT_CONNECTOR)) {
        for (uint32_t i = 0; i < props->count_props; i++) {
            if (drmModePropertyRes* p = drmModeGetProperty(drm_fd, props->props[i])) {
                if (strcmp(p->name, "DPMS") == 0) dpms_prop_id = p->prop_id;
                drmModeFreeProperty(p);
            }
        }
        drmModeFreeObjectProperties(props);
    }
    fprintf(stderr, "huskyfe: DPMS prop_id=%u\n", dpms_prop_id);


    auto set_bl_power = [](int v) {
        int fd = open("/sys/class/backlight/panel0-backlight/bl_power",
                      O_WRONLY | O_CLOEXEC);
        if (fd < 0) return;
        char b[8];
        int n = snprintf(b, sizeof(b), "%d\n", v);
        (void)::write(fd, b, (size_t)n);
        close(fd);
    };


    auto gpu_stack_signal = [](const char* sig) {
        char cmd[128];
        snprintf(cmd, sizeof(cmd),
                 "pkill -%s -f 'glproxy-srv|mali_compositor' >/dev/null 2>&1 &",
                 sig);
        std::system(cmd);
    };


    auto set_cpu_governor = [](const char* gov) {
        char cmd[256];
        snprintf(cmd, sizeof(cmd),
                 "for p in /sys/devices/system/cpu/cpufreq/policy*; do "
                 "echo %s > $p/scaling_governor 2>/dev/null; done",
                 gov);
        std::system(cmd);
    };


    auto set_perf_mon = [](int on) {
        char cmd[256];
        snprintf(cmd, sizeof(cmd),
                 "echo %d > /sys/module/gs_perf_mon/parameters/gs_perf_mon_param_on 2>/dev/null",
                 on);
        std::system(cmd);
    };


    gpu_stack_signal("CONT");


    Dmabuf bufs[2];
    for (int i = 0; i < 2; i++) {
        if (!alloc_dmabuf(mode.hdisplay, mode.vdisplay, bufs[i])) return 1;
        uint32_t handle = 0;
        if (drmPrimeFDToHandle(drm_fd, bufs[i].fd, &handle) < 0) { perror("PrimeFDToHandle"); return 1; }
        uint32_t handles[4] = { handle, 0, 0, 0 };
        uint32_t pitches[4] = { bufs[i].stride, 0, 0, 0 };
        uint32_t offsets[4] = { 0, 0, 0, 0 };
        if (drmModeAddFB2(drm_fd, bufs[i].w, bufs[i].h, FOURCC_ABGR8888,
                          handles, pitches, offsets, &bufs[i].fb_id, 0) < 0) {
            perror("drmModeAddFB2"); return 1;
        }
    }

    EGLDisplay dpy = eglGetDisplay(EGL_DEFAULT_DISPLAY);
    if (!eglInitialize(dpy, nullptr, nullptr)) { fprintf(stderr, "eglInitialize failed\n"); return 1; }
    eglBindAPI(EGL_OPENGL_ES_API);

    EGLConfig cfg = nullptr;
    EGLint nc = 0;
    EGLint cattr[] = {
        EGL_SURFACE_TYPE,    EGL_PBUFFER_BIT,
        EGL_RENDERABLE_TYPE, EGL_OPENGL_ES2_BIT,
        EGL_NONE,
    };
    eglChooseConfig(dpy, cattr, &cfg, 1, &nc);
    EGLint ctxa[] = { EGL_CONTEXT_CLIENT_VERSION, 3, EGL_NONE };
    EGLContext ctx = eglCreateContext(dpy, cfg, EGL_NO_CONTEXT, ctxa);
    EGLint pba[] = { EGL_WIDTH, 16, EGL_HEIGHT, 16, EGL_NONE };
    EGLSurface surf = eglCreatePbufferSurface(dpy, cfg, pba);
    eglMakeCurrent(dpy, surf, surf, ctx);

    int back = 0, front = 1;
    glp_set_output_dmabuf(bufs[back].fd, bufs[back].w, bufs[back].h, bufs[back].stride, FOURCC_ABGR8888);

    huskyfe::Renderer renderer;
    if (!renderer.init((int)mode.hdisplay, (int)mode.vdisplay)) {
        fprintf(stderr, "huskyfe: renderer init failed\n"); return 1;
    }

    constexpr const char* FONT = "/usr/share/fonts/truetype/dejavu/DejaVuSans.ttf";
    huskyfe::TextRenderer text;
    huskyfe::TextRenderer label_text;
    huskyfe::TextRenderer huge_text;
    if (!text.init(FONT, 40))        { fprintf(stderr, "huskyfe: text init failed\n");       return 1; }
    if (!label_text.init(FONT, 26))  { fprintf(stderr, "huskyfe: label_text init failed\n"); return 1; }
    if (!huge_text.init(FONT, 140))  { fprintf(stderr, "huskyfe: huge_text init failed\n");  return 1; }


    auto installed = huskyfe::apps::scan(0);
    {


        std::vector<std::string> hidden;
        if (FILE* f = fopen("/var/lib/huskyfe/hidden_apps", "r")) {
            char line[1024];
            while (fgets(line, sizeof(line), f)) {
                size_t n = strlen(line);
                while (n > 0 && (line[n-1] == '\n' || line[n-1] == '\r')) line[--n] = 0;
                if (n) hidden.emplace_back(line);
            }
            fclose(f);
        }
        if (!hidden.empty()) {
            installed.erase(std::remove_if(installed.begin(), installed.end(),
                [&](const huskyfe::apps::AppEntry& a) {
                    return std::find(hidden.begin(), hidden.end(), a.desktop_path)
                        != hidden.end();
                }), installed.end());
        }

        if (installed.size() > 23) installed.resize(23);


        huskyfe::apps::AppEntry settings;
        settings.name         = "Settings";
        settings.exec         = "";
        settings.icon         = "preferences-system";
        settings.desktop_path = "<built-in>";
        installed.insert(installed.begin(), std::move(settings));
    }
    fprintf(stderr, "huskyfe: scanned %zu app(s) for grid (incl. Settings)\n", installed.size());


    huskyfe::icons::Atlas icon_atlas;
    huskyfe::ImageRenderer image_r;
    {
        std::vector<std::string> names;
        names.reserve(installed.size());
        for (const auto& a : installed) names.push_back(a.icon);
        if (!icon_atlas.build(names, 256, 4, 6)) {
            fprintf(stderr, "huskyfe: icon atlas build failed\n"); return 1;
        }
    }
    if (!image_r.init()) {
        fprintf(stderr, "huskyfe: image renderer init failed\n"); return 1;
    }


    if (!huskyfe::wlhost::init("wayland-1")) {
        fprintf(stderr, "huskyfe: Wayland host init failed (continuing without)\n");
    }

    huskyfe::Background bg;


    if (!bg.init((int)mode.hdisplay, (int)mode.vdisplay, 2, 120.0f)) {
        fprintf(stderr, "huskyfe: background init failed\n"); return 1;
    }
    auto bg_t0 = std::chrono::steady_clock::now();


    constexpr const char* THEME_DIR    = "/var/lib/huskyfe/theme";
    constexpr const char* SHADER_DIR   = "/var/lib/huskyfe/shaders";
    constexpr const char* IRIS_DIR     = "/var/lib/huskyfe/iris_masks";
    constexpr const char* BG_SEL_PATH    = "/var/lib/huskyfe/theme/background";
    constexpr const char* IRIS_SEL_PATH  = "/var/lib/huskyfe/theme/iris_mask";
    constexpr const char* UNLOCK_SEL_PATH= "/var/lib/huskyfe/theme/unlock_mask";

    auto read_one_line = [](const char* path, std::string& out) -> bool {
        FILE* f = fopen(path, "r");
        if (!f) return false;
        char buf[512]; buf[0] = 0;
        if (!fgets(buf, sizeof(buf), f)) { fclose(f); return false; }
        fclose(f);
        size_t n = strlen(buf);
        while (n > 0 && (buf[n-1] == '\n' || buf[n-1] == '\r' || buf[n-1] == ' '))
            buf[--n] = 0;
        out.assign(buf);
        return !out.empty();
    };
    auto write_one_line = [](const char* path, const std::string& v) {
        mkdir("/var/lib/huskyfe", 0755);
        mkdir(THEME_DIR, 0755);
        FILE* f = fopen(path, "w");
        if (!f) return;
        fwrite(v.data(), 1, v.size(), f);
        fputc('\n', f);
        fclose(f);
    };


    {
        std::string sel;
        if (read_one_line(BG_SEL_PATH, sel)) {
            std::string arg = sel;
            if (arg != "none" && arg != "rgb" && arg != "spheres"
                && arg != "fractal"
                && arg.find('/') == std::string::npos) {
                arg = std::string(SHADER_DIR) + "/" + arg;
            }
            if (!bg.set_shader(arg)) {
                fprintf(stderr, "huskyfe: bg shader '%s' rejected, keeping default\n",
                        sel.c_str());
            } else {
                fprintf(stderr, "huskyfe: bg shader = %s\n", sel.c_str());
            }
        }
    }


    std::string iris_choice   = "builtin:circle";
    std::string unlock_choice = "builtin:circle";
    {
        std::string sel;
        if (read_one_line(IRIS_SEL_PATH,   sel)) iris_choice   = sel;
        if (read_one_line(UNLOCK_SEL_PATH, sel)) unlock_choice = sel;
    }


    GLuint      iris_prog        = 0;
    GLuint      iris_vbo         = 0;
    GLuint      iris_mask_tex    = 0;
    GLuint      unlock_mask_tex  = 0;
    GLint       iris_loc_pos     = -1;
    GLint       iris_loc_center  = -1;
    GLint       iris_loc_radius  = -1;
    GLint       iris_loc_edge    = -1;
    GLint       iris_loc_mode    = -1;
    GLint       iris_loc_mask    = -1;
    GLint       iris_loc_t       = -1;
    GLint       iris_loc_dir     = -1;
    std::string iris_loaded;
    std::string unlock_loaded;


    auto load_mask_into = [&](GLuint& tex, std::string& loaded,
                              std::string& choice_var, const std::string& c) {
        choice_var = c;
        if (c.rfind("builtin:", 0) == 0) return;
        std::string path = c;
        if (path.find('/') == std::string::npos)
            path = std::string(IRIS_DIR) + "/" + path;
        if (path == loaded) return;
        int mw = 0, mh = 0, mch = 0;
        unsigned char* mpx = stbi_load(path.c_str(), &mw, &mh, &mch, 4);
        if (!mpx) {
            fprintf(stderr, "huskyfe: mask load failed: %s\n", path.c_str());
            choice_var = "builtin:circle";
            return;
        }
        if (!tex) glGenTextures(1, &tex);
        glBindTexture(GL_TEXTURE_2D, tex);
        glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
        glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, mw, mh, 0,
                     GL_RGBA, GL_UNSIGNED_BYTE, mpx);
        glPixelStorei(GL_UNPACK_ALIGNMENT, 4);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
        loaded = path;
        fprintf(stderr, "huskyfe: mask loaded %dx%d from %s\n",
                mw, mh, path.c_str());
        stbi_image_free(mpx);
    };
    auto apply_iris_choice = [&](const std::string& c) {
        load_mask_into(iris_mask_tex, iris_loaded, iris_choice, c);
    };
    auto apply_unlock_choice = [&](const std::string& c) {
        load_mask_into(unlock_mask_tex, unlock_loaded, unlock_choice, c);
    };
    auto iris_mode_int = [](const std::string& c) -> int {
        if (c == "builtin:circle")    return 0;
        if (c == "builtin:square")    return 1;
        if (c == "builtin:heart")     return 2;
        if (c == "builtin:star")      return 3;
        if (c == "builtin:blackhole") return 5;
        return 4;
    };
    apply_iris_choice(iris_choice);
    apply_unlock_choice(unlock_choice);


    struct ThemeOption { std::string label; std::string value; };
    std::vector<ThemeOption> bg_options;
    std::vector<ThemeOption> iris_options;
    int theme_pressed = -1;

    auto rescan_theme_options = [&]() {
        bg_options.clear();
        bg_options.push_back({ "None",     "none"    });
        bg_options.push_back({ "RGB Wave", "rgb"     });
        bg_options.push_back({ "Spheres",  "spheres" });
        bg_options.push_back({ "Fractal",  "fractal" });
        if (DIR* d = opendir(SHADER_DIR)) {
            while (struct dirent* de = readdir(d)) {
                if (de->d_name[0] == '.') continue;
                std::string n = de->d_name;
                if (n.size() < 6 || n.substr(n.size() - 5) != ".frag") continue;
                bg_options.push_back({ n.substr(0, n.size() - 5), n });
            }
            closedir(d);
        }

        iris_options.clear();
        iris_options.push_back({ "Circle", "builtin:circle" });
        iris_options.push_back({ "Square", "builtin:square" });
        iris_options.push_back({ "Heart",  "builtin:heart"  });
        iris_options.push_back({ "Star",   "builtin:star"   });
        iris_options.push_back({ "Black hole", "builtin:blackhole" });


        {
            struct stat st;
            if (::stat("/var/lib/huskyfe/iris_mask.png", &st) == 0)
                iris_options.push_back({ "Iris", "/var/lib/huskyfe/iris_mask.png" });
        }
        if (DIR* d = opendir(IRIS_DIR)) {
            while (struct dirent* de = readdir(d)) {
                if (de->d_name[0] == '.') continue;
                std::string n = de->d_name;
                if (n.size() < 5 || n.substr(n.size() - 4) != ".png") continue;
                iris_options.push_back({ n.substr(0, n.size() - 4), n });
            }
            closedir(d);
        }
    };


    huskyfe::Blur blur;
    if (!blur.init(168, 374)) {
        fprintf(stderr, "huskyfe: blur init failed\n"); return 1;
    }

    drmModeSetCrtc(drm_fd, crtc_id, bufs[front].fb_id, 0, 0, &conn->connector_id, 1, &mode);


    constexpr int   cols           = 4;
    constexpr int   rows           = 6;
    constexpr int   ncells         = cols * rows;
    constexpr float side_pad       = 80.0f;
    constexpr float status_h       = 130.0f;
    constexpr float top_gap        = 110.0f;
    constexpr float bottom_reserve = 180.0f;
    constexpr float icon_size      = 200.0f;
    constexpr float icon_r         = 56.0f;

    const float msw = (float)mode.hdisplay;
    const float msh = (float)mode.vdisplay;
    const float top_offset = status_h + top_gap;
    const float grid_h     = msh - top_offset - bottom_reserve;
    const float row_stride = grid_h / (float)rows;
    const float col_stride = (msw - 2.0f * side_pad) / (float)cols;

    struct CellRect { float x, y, w, h; };
    std::vector<CellRect> cells; cells.reserve(ncells);
    for (int row = 0; row < rows; row++) {
        for (int col = 0; col < cols; col++) {
            float cx = side_pad + col * col_stride + col_stride * 0.5f;
            float cy = top_offset + row * row_stride + row_stride * 0.5f;
            cells.push_back({ cx - icon_size * 0.5f, cy - icon_size * 0.5f,
                              icon_size, icon_size });
        }
    }

    std::vector<huskyfe::Spring> cell_scales(ncells);
    int hit_cell = -1;


    std::chrono::steady_clock::time_point hit_cell_at{};
    bool ctx_menu_open    = false;
    int  ctx_menu_cell    = -1;
    int  ctx_menu_pressed = -1;
    huskyfe::Spring ctx_menu_anim;
    ctx_menu_anim.snap_to(0.0f);
    auto open_ctx_menu = [&](int cell) {
        ctx_menu_open    = true;
        ctx_menu_cell    = cell;
        ctx_menu_pressed = -1;
        ctx_menu_anim.stiffness = 280.0f;
        ctx_menu_anim.damping   =  32.0f;
        ctx_menu_anim.set(1.0f);
    };
    auto close_ctx_menu = [&]() {
        ctx_menu_open    = false;
        ctx_menu_cell    = -1;
        ctx_menu_pressed = -1;
        ctx_menu_anim.stiffness = 280.0f;
        ctx_menu_anim.damping   =  36.0f;
        ctx_menu_anim.set(0.0f);
    };


    constexpr float menu_w   = 880.0f;
    constexpr float menu_h   = 580.0f;
    constexpr float menu_pad = 56.0f;
    constexpr float btn_w    = menu_w - 2.0f * menu_pad;
    constexpr float btn_h    = 132.0f;
    constexpr float btn_gap  = 28.0f;
    constexpr float btn_radius  = btn_h * 0.5f;
    constexpr float menu_radius = 44.0f;
    const float menu_x = (msw - menu_w) * 0.5f;
    const float menu_y = (msh - menu_h) * 0.5f;
    struct ButtonRect { float x, y, w, h; };
    ButtonRect menu_buttons[3];
    for (int i = 0; i < 3; i++) {
        menu_buttons[i] = {
            menu_x + menu_pad,
            menu_y + menu_pad + i * (btn_h + btn_gap),
            btn_w, btn_h
        };
    }
    static const char* btn_labels[3] = { "Shutdown", "Reboot", "Cancel" };
    static const huskyfe::Color btn_colors[3] = {
        { 0.93f, 0.32f, 0.32f, 1.0f },
        { 0.93f, 0.62f, 0.28f, 1.0f },
        { 0.28f, 0.28f, 0.32f, 1.0f },
    };

    bool   menu_open        = false;
    int    menu_pressed_btn = -1;
    huskyfe::Spring menu_anim;
    menu_anim.snap_to(0.0f);


    bool   quick_open = false;
    huskyfe::Spring quick_anim;
    quick_anim.snap_to(0.0f);
    g_fade.snap_to(0.0f);
    bool   dragging_quick_brightness = false;
    int    quick_pressed = -1;

    int    edge_swipe_start_x = -1;
    int    edge_swipe_start_y = -1;

    constexpr float QUICK_H = 1540.0f;


    auto normalize_id = [](std::string s) -> std::string {
        std::string out;
        out.reserve(s.size());
        for (char c : s) {
            if (c == '-' || c == '_' || c == '.' || c == '/' || c == ' ') continue;
            out.push_back((char)std::tolower((unsigned char)c));
        }
        return out;
    };
    auto derive_app_key = [&](const std::string& exec) -> std::string {
        std::string k = exec;
        if (auto sp = k.find(' ');  sp != std::string::npos) k = k.substr(0, sp);
        if (auto sl = k.rfind('/'); sl != std::string::npos) k = k.substr(sl + 1);
        return normalize_id(k);
    };
    auto find_running_for_cell = [&](const std::string& exec)
                                     -> huskyfe::wlhost::AppHandle {
        std::string key = derive_app_key(exec);
        if (key.empty()) return 0;
        for (const auto& r : huskyfe::wlhost::running()) {
            std::string id = normalize_id(r.app_id);
            if (!id.empty() && (id == key
                                || id.find(key) != std::string::npos
                                || key.find(id) != std::string::npos))
                return r.handle;
        }
        return 0;
    };


    auto hidden_path = std::string("/var/lib/huskyfe/hidden_apps");
    auto append_hidden = [&](const std::string& dp) {
        std::system("mkdir -p /var/lib/huskyfe");
        FILE* f = fopen(hidden_path.c_str(), "a");
        if (!f) return;
        fprintf(f, "%s\n", dp.c_str());
        fclose(f);
    };
    auto is_hidden = [&](const std::string& dp) -> bool {
        FILE* f = fopen(hidden_path.c_str(), "r");
        if (!f) return false;
        char line[1024];
        bool found = false;
        while (fgets(line, sizeof(line), f)) {
            size_t n = strlen(line);
            while (n > 0 && (line[n-1] == '\n' || line[n-1] == '\r')) line[--n] = 0;
            if (dp == line) { found = true; break; }
        }
        fclose(f);
        return found;
    };
    (void)is_hidden;


    bool battery_saver = false;
    {
        FILE* f = fopen("/var/lib/huskyfe/battery_saver", "r");
        if (f) { int v = 0; if (fscanf(f, "%d", &v) == 1) battery_saver = (v != 0); fclose(f); }
    }
    auto save_battery_saver = [&]() {
        std::system("mkdir -p /var/lib/huskyfe");
        FILE* f = fopen("/var/lib/huskyfe/battery_saver", "w");
        if (f) { fprintf(f, "%d\n", battery_saver ? 1 : 0); fclose(f); }
    };
    auto apply_active_power_profile = [&]() {


        set_cpu_governor("schedutil");
        set_perf_mon(battery_saver ? 0 : 1);
    };
    apply_active_power_profile();


    bool dnd = false;
    {
        FILE* f = fopen("/var/lib/huskyfe/dnd", "r");
        if (f) { int v = 0; if (fscanf(f, "%d", &v) == 1) dnd = (v != 0); fclose(f); }
    }
    auto save_dnd = [&]() {
        std::system("mkdir -p /var/lib/huskyfe");
        FILE* f = fopen("/var/lib/huskyfe/dnd", "w");
        if (f) { fprintf(f, "%d\n", dnd ? 1 : 0); fclose(f); }
    };
    huskyfe::notifications::set_muted(dnd);


    bool haptic_enabled = true;


    int  haptic_pressed = -1;
    constexpr int haptic_strength = 22000;
    {
        FILE* f = fopen("/var/lib/huskyfe/haptic_enabled", "r");
        if (f) {
            int v = 1;
            if (fscanf(f, "%d", &v) == 1) haptic_enabled = (v != 0);
            fclose(f);
        }
    }
    auto save_haptic_enabled = [&]() {
        mkdir("/var/lib/huskyfe", 0755);
        FILE* f = fopen("/var/lib/huskyfe/haptic_enabled", "w");
        if (f) { fprintf(f, "%d\n", haptic_enabled ? 1 : 0); fclose(f); }
    };
    huskyfe::haptics::init();
    huskyfe::flashlight::init();


    std::thread([]() {
        huskyfe::camera::cleanup_orphan_daemon();
    }).detach();


    bool     camera_preview_open = false;
    GLuint   cam_texture         = 0;
    uint32_t cam_last_seq        = 0;
    int      cam_tex_w           = 0;
    int      cam_tex_h           = 0;


    bool flashlight_on = false;


    int  flashlight_level = 0x40;
    {
        FILE* f = fopen("/var/lib/huskyfe/flashlight_level", "r");
        if (f) {
            int v = 0;
            if (fscanf(f, "%d", &v) == 1) flashlight_level = std::clamp(v, 0, 0x7F);
            fclose(f);
        }
    }
    auto save_flashlight_level = [&]() {
        mkdir("/var/lib/huskyfe", 0755);
        FILE* f = fopen("/var/lib/huskyfe/flashlight_level", "w");
        if (f) { fprintf(f, "%d\n", flashlight_level); fclose(f); }
    };

    bool flash_slider_open      = false;
    bool dragging_flash_level   = false;
    std::chrono::steady_clock::time_point quick_flash_press_at{};


    std::mutex                        ipc_mu;
    std::vector<std::function<void()>> ipc_pending;
    auto ipc_submit = [&](std::function<void()> fn) {
        std::lock_guard<std::mutex> lk(ipc_mu);
        ipc_pending.push_back(std::move(fn));
    };
    auto ipc_handle_line = [&](const std::string& line, int reply_fd) {
        std::istringstream iss(line);
        std::vector<std::string> tok;
        for (std::string t; iss >> t; ) tok.push_back(t);
        if (tok.size() >= 2 && tok[0] == "flashlight") {
            if (tok[1] == "on") {
                ipc_submit([&]() {
                    int target = flashlight_level > 0 ? flashlight_level : 0x40;
                    if (huskyfe::flashlight::set(true, target)) {
                        flashlight_on    = true;
                        flashlight_level = target;
                        save_flashlight_level();
                    }
                });
            } else if (tok[1] == "off") {
                ipc_submit([&]() {
                    if (huskyfe::flashlight::set(false)) flashlight_on = false;
                });
            } else if (tok[1] == "set" && tok.size() >= 3) {
                int n = std::clamp(std::atoi(tok[2].c_str()), 0, 0x7F);
                ipc_submit([&, n]() {
                    flashlight_level = n;
                    if (n == 0) {
                        if (flashlight_on && huskyfe::flashlight::set(false))
                            flashlight_on = false;
                    } else if (!flashlight_on) {
                        if (huskyfe::flashlight::set(true, n)) flashlight_on = true;
                    } else {
                        huskyfe::flashlight::set_level(n);
                    }
                    save_flashlight_level();
                });
            } else if (tok[1] == "?") {


                char buf[64];
                int  n = snprintf(buf, sizeof(buf), "%s %d\n",
                                  flashlight_on ? "on" : "off",
                                  flashlight_level);
                (void)::write(reply_fd, buf, n);
            }
        }


        else if (!tok.empty() && tok[0] == "camera") {
            const std::string tsv = "/root/oksoko_sensor_only.tsv";
            if (tok.size() >= 2 && tok[1] == "start") {
                ipc_submit([&, tsv]() {
                    huskyfe::camera::spawn_daemon(tsv);

                    for (int i = 0; i < 40 && !huskyfe::camera::is_ready(); i++) {
                        usleep(50 * 1000);
                        huskyfe::camera::init();
                    }
                });
            } else if (tok.size() >= 2 && tok[1] == "stop") {
                ipc_submit([&]() {
                    huskyfe::camera::shutdown();
                    huskyfe::camera::kill_daemon();
                });
            } else if (tok.size() >= 2 && tok[1] == "?") {
                char buf[96];
                int n;
                if (huskyfe::camera::is_ready()) {
                    auto h = huskyfe::camera::header();
                    n = snprintf(buf, sizeof(buf), "running %ux%u seq=%u\n",
                                 h.width, h.height, h.frame_seq);
                } else {
                    n = snprintf(buf, sizeof(buf), "idle\n");
                }
                (void)::write(reply_fd, buf, n);
            }
        }
    };
    std::thread([&]() {
        const char* sock_path = "/run/huskyfe.sock";
        unlink(sock_path);
        int srv = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
        if (srv < 0) { perror("huskyfe: ipc socket"); return; }
        struct sockaddr_un sa{};
        sa.sun_family = AF_UNIX;
        strncpy(sa.sun_path, sock_path, sizeof(sa.sun_path) - 1);
        if (bind(srv, (sockaddr*)&sa, sizeof(sa)) < 0) {
            perror("huskyfe: ipc bind"); close(srv); return;
        }
        chmod(sock_path, 0666);
        if (listen(srv, 4) < 0) { perror("huskyfe: ipc listen"); close(srv); return; }
        fprintf(stderr, "huskyfe: ipc listening at %s\n", sock_path);
        for (;;) {
            int cli = accept4(srv, nullptr, nullptr, SOCK_CLOEXEC);
            if (cli < 0) continue;
            std::thread([cli, &ipc_handle_line]() {
                std::string buf;
                char chunk[256];
                for (;;) {
                    ssize_t n = ::read(cli, chunk, sizeof(chunk));
                    if (n <= 0) break;
                    buf.append(chunk, n);
                    size_t pos;
                    while ((pos = buf.find('\n')) != std::string::npos) {
                        ipc_handle_line(buf.substr(0, pos), cli);
                        buf.erase(0, pos + 1);
                    }
                }
                close(cli);
            }).detach();
        }
    }).detach();


    auto buzz_lock_blackhole = [&]() {
        if (!haptic_enabled)                    return;
        if (iris_choice != "builtin:blackhole") return;
        if (!huskyfe::haptics::ok())            return;
        const int s = haptic_strength;
        std::thread([s]() {
            struct Burst { int dur_ms; int gap_ms; };


            const Burst pattern[] = {
                { 22,   3 },
                { 22,   4 },
                { 22,   5 },
                { 22,   7 },
                { 22,  10 },
                { 22,  13 },
                { 22,  17 },
                { 22,  23 },
                { 22,  31 },
                { 22,  41 },
                { 22,  55 },
                { 22,  74 },
                { 22, 100 },
                { 22,   0 },
            };
            for (const auto& b : pattern) {
                huskyfe::haptics::play(b.dur_ms, s, 16);
                std::this_thread::sleep_for(
                    std::chrono::milliseconds(b.dur_ms + b.gap_ms));
            }
        }).detach();
    };


    auto buzz_unlock_blackhole = [&]() {
        if (!haptic_enabled)                       return;
        if (unlock_choice != "builtin:blackhole")  return;
        if (!huskyfe::haptics::ok())               return;
        const int s = haptic_strength;
        std::thread([s]() {
            struct Burst { int dur_ms; int gap_ms; };
            const Burst pattern[] = {
                { 50, 30 },
                { 35, 55 },
                { 22,  0 },
            };
            for (const auto& b : pattern) {
                huskyfe::haptics::play(b.dur_ms, s, 16);
                std::this_thread::sleep_for(
                    std::chrono::milliseconds(b.dur_ms + b.gap_ms));
            }
        }).detach();
    };

    auto open_quick_settings = [&]() {
        quick_open = true;
        quick_anim.stiffness = 280.0f;
        quick_anim.damping   =  32.0f;
        quick_anim.set(1.0f);


        std::thread([]() { huskyfe::bluetooth::status(); }).detach();
    };
    auto close_quick_settings = [&]() {
        quick_open = false;
        quick_anim.stiffness = 280.0f;
        quick_anim.damping   =  36.0f;
        quick_anim.set(0.0f);
        dragging_quick_brightness = false;
        quick_pressed = -1;
        if (flash_slider_open) save_flashlight_level();
        flash_slider_open    = false;
        dragging_flash_level = false;
    };


    enum class View { LAUNCHER, SETTINGS };
    enum class SubPage { LIST, ABOUT, BRIGHTNESS, DATETIME, WIFI, BLUETOOTH, THEME, GENERAL };
    View view = View::LAUNCHER;
    SubPage settings_sub = SubPage::LIST;
    huskyfe::Spring view_anim;
    huskyfe::Spring sub_anim;
    view_anim.snap_to(0.0f);
    sub_anim.snap_to(0.0f);


    huskyfe::Spring page_anim;
    page_anim.snap_to(0.0f);
    bool dragging_home_page = false;
    int  home_drag_start_x = -1;
    int  home_drag_start_y = -1;
    float page_anim_at_drag_start = 0.0f;
    int  notif_pressed_idx = -1;
    int settings_pressed_row = -1;
    int sub_pressed_back     = 0;


    constexpr int brightness_min_pct = 10;
    int  brightness_max      = 4095;
    int  brightness_val      = 0;
    bool dragging_brightness = false;

    auto brightness_min = [&]() -> int {
        return (brightness_max * brightness_min_pct) / 100;
    };
    auto save_brightness = [&](int v) {
        int lo = brightness_min();
        if (v < lo) v = lo;
        if (v > brightness_max) v = brightness_max;
        brightness_val = v;
        int fd = open("/sys/class/backlight/panel0-backlight/brightness",
                      O_WRONLY | O_CLOEXEC);
        if (fd >= 0) {
            char buf[32];
            int n = snprintf(buf, sizeof(buf), "%d", v);
            (void)::write(fd, buf, (size_t)n);
            close(fd);
        }
    };
    auto load_brightness = [&]() {
        char buf[32];
        int fd = open("/sys/class/backlight/panel0-backlight/max_brightness",
                      O_RDONLY | O_CLOEXEC);
        if (fd >= 0) {
            ssize_t n = ::read(fd, buf, sizeof(buf) - 1);
            close(fd);
            if (n > 0) { buf[n] = 0; int v = atoi(buf); if (v > 0) brightness_max = v; }
        }
        fd = open("/sys/class/backlight/panel0-backlight/brightness",
                  O_RDONLY | O_CLOEXEC);
        if (fd >= 0) {
            ssize_t n = ::read(fd, buf, sizeof(buf) - 1);
            close(fd);
            if (n > 0) { buf[n] = 0; brightness_val = atoi(buf); }
        }


        if (brightness_val < brightness_min()) save_brightness(brightness_min());
    };
    load_brightness();


    huskyfe::wifi::Status               wifi_status;
    std::vector<huskyfe::wifi::Network> wifi_nets;
    auto last_wifi_refresh = std::chrono::steady_clock::now() - std::chrono::seconds(10);

    int wifi_pressed = -1;


    std::string                                connecting_ssid;
    std::chrono::steady_clock::time_point      connect_started_at{};
    std::string                                connect_msg;
    std::chrono::steady_clock::time_point      connect_msg_until{};
    constexpr int                              connect_timeout_s = 15;
    constexpr int                              connect_msg_show_s = 5;


    std::mutex                                async_net_mu;
    bool                                      wifi_inflight = false;
    bool                                      wifi_pending  = false;
    huskyfe::wifi::Status                     async_wifi_status;
    std::vector<huskyfe::wifi::Network>       async_wifi_nets;
    std::chrono::steady_clock::time_point     bt_inflight_since{};
    bool                                      bt_pending  = false;
    huskyfe::bluetooth::Status                async_bt_status;
    std::vector<huskyfe::bluetooth::Device>   async_bt_devices;
    std::vector<huskyfe::bluetooth::Adapter>  async_bt_adapters;


    auto kick_wifi_fetch = [&]() {
        {
            std::lock_guard<std::mutex> lk(async_net_mu);
            if (wifi_inflight) return;
            wifi_inflight = true;
        }
        std::thread([&]() {
            auto s = huskyfe::wifi::status();
            auto n = huskyfe::wifi::scan_results();
            if (n.size() > 8) n.resize(8);
            std::lock_guard<std::mutex> lk(async_net_mu);
            async_wifi_status = std::move(s);
            async_wifi_nets   = std::move(n);
            wifi_pending      = true;
            wifi_inflight     = false;
        }).detach();
    };

    auto refresh_wifi = [&]() {
        {
            std::lock_guard<std::mutex> lk(async_net_mu);
            if (wifi_pending) {
                wifi_status  = std::move(async_wifi_status);
                wifi_nets    = std::move(async_wifi_nets);
                wifi_pending = false;
            }
        }
        auto tnow = std::chrono::steady_clock::now();
        if (!connecting_ssid.empty()) {
            if (wifi_status.connected && wifi_status.ssid == connecting_ssid) {
                connecting_ssid.clear();
            } else {
                int elapsed = (int)std::chrono::duration_cast<std::chrono::seconds>(
                                  tnow - connect_started_at).count();
                if (elapsed >= connect_timeout_s) {
                    connect_msg       = "Failed to connect to " + connecting_ssid;
                    connect_msg_until = tnow + std::chrono::seconds(connect_msg_show_s);
                    connecting_ssid.clear();
                }
            }
        }
        if (!connect_msg.empty() && tnow >= connect_msg_until) connect_msg.clear();
    };
    auto begin_connect = [&](const std::string& ssid) {
        connecting_ssid    = ssid;
        connect_started_at = std::chrono::steady_clock::now();
        connect_msg.clear();
    };


    huskyfe::bluetooth::Status               bt_status;
    std::vector<huskyfe::bluetooth::Device>  bt_devices;
    std::vector<huskyfe::bluetooth::Adapter> bt_adapters;
    auto last_bt_refresh = std::chrono::steady_clock::now() - std::chrono::seconds(10);


    int bt_pressed = -1;

    std::string                                bt_connecting_mac;
    std::string                                bt_connecting_name;
    std::chrono::steady_clock::time_point      bt_connect_started_at{};
    std::string                                bt_msg;
    std::chrono::steady_clock::time_point      bt_msg_until{};
    constexpr int                              bt_connect_timeout_s = 25;
    constexpr int                              bt_msg_show_s        = 5;


    std::chrono::steady_clock::time_point      bt_scan_pending_until{};
    std::chrono::steady_clock::time_point      bt_power_pending_until{};
    bool                                       bt_power_pending_target = false;


    auto kick_bt_fetch = [&](bool ensure_powered) {
        {
            std::lock_guard<std::mutex> lk(async_net_mu);
            auto bt_now = std::chrono::steady_clock::now();
            if (bt_inflight_since != std::chrono::steady_clock::time_point{}
                && std::chrono::duration_cast<std::chrono::seconds>(bt_now - bt_inflight_since).count() < 5)
                return;
            bt_inflight_since = bt_now;
        }
        std::thread([&, ensure_powered]() {
            if (ensure_powered && !huskyfe::bluetooth::status().powered)
                huskyfe::bluetooth::power_on();
            auto s = huskyfe::bluetooth::status();
            auto d = huskyfe::bluetooth::devices();
            auto a = huskyfe::bluetooth::adapters();
            std::lock_guard<std::mutex> lk(async_net_mu);
            async_bt_status   = std::move(s);
            async_bt_devices  = std::move(d);
            async_bt_adapters = std::move(a);
            bt_pending  = true;
            bt_inflight_since = std::chrono::steady_clock::time_point{};
        }).detach();
    };

    auto refresh_bt = [&]() {
        {
            std::lock_guard<std::mutex> lk(async_net_mu);
            if (bt_pending) {
                bt_status   = std::move(async_bt_status);
                bt_devices  = std::move(async_bt_devices);
                bt_adapters = std::move(async_bt_adapters);
                bt_pending  = false;
            }
        }
        auto tnow = std::chrono::steady_clock::now();
        if (!bt_connecting_mac.empty()) {

            bool linked = false;
            for (auto& d : bt_devices)
                if (d.mac == bt_connecting_mac && d.connected) { linked = true; break; }
            if (linked) {
                bt_msg       = "Connected to " + bt_connecting_name;
                bt_msg_until = tnow + std::chrono::seconds(bt_msg_show_s);
                bt_connecting_mac.clear();
                bt_connecting_name.clear();
            } else {
                int elapsed = (int)std::chrono::duration_cast<std::chrono::seconds>(
                                  tnow - bt_connect_started_at).count();
                if (elapsed >= bt_connect_timeout_s) {
                    bt_msg       = "Failed to connect to " + bt_connecting_name;
                    bt_msg_until = tnow + std::chrono::seconds(bt_msg_show_s);
                    bt_connecting_mac.clear();
                    bt_connecting_name.clear();
                }
            }
        }
        if (!bt_msg.empty() && tnow >= bt_msg_until) bt_msg.clear();


        if (bt_power_pending_until != std::chrono::steady_clock::time_point{}) {
            if (tnow >= bt_power_pending_until
                || bt_status.powered == bt_power_pending_target) {
                bt_power_pending_until = std::chrono::steady_clock::time_point{};
            }
        }


        if (bt_scan_pending_until != std::chrono::steady_clock::time_point{}
            && tnow >= bt_scan_pending_until) {
            bt_scan_pending_until = std::chrono::steady_clock::time_point{};
        }


        if (!bt_status.powered) {
            bt_devices.clear();
        }
    };
    auto begin_bt_connect = [&](const std::string& mac, const std::string& name) {
        bt_connecting_mac   = mac;
        bt_connecting_name  = name.empty() ? mac : name;
        bt_connect_started_at = std::chrono::steady_clock::now();
        bt_msg.clear();
    };


    huskyfe::Keyboard keyboard;
    if (!keyboard.init((int)mode.hdisplay, (int)mode.vdisplay)) {
        fprintf(stderr, "huskyfe: keyboard init failed\n"); return 1;
    }


    huskyfe::notifications::init((int)mode.hdisplay, (int)mode.vdisplay);


    std::string keyboard_owner;


    struct AboutInfo {
        std::string hostname, kernel, uptime, mali, ipv4, storage;
    } about{};


    static std::mutex                         about_mu;
    static std::atomic<bool>                  about_quit{false};
    auto gather_about_into = [](AboutInfo& dst) {
        char buf[256];
        if (gethostname(buf, sizeof(buf)) == 0) dst.hostname = buf;
        struct utsname un{};
        if (uname(&un) == 0) dst.kernel = un.release;
        struct sysinfo si{};
        if (sysinfo(&si) == 0) {
            long d = si.uptime / 86400;
            long h = (si.uptime % 86400) / 3600;
            long m = (si.uptime % 3600) / 60;
            char ub[64];
            snprintf(ub, sizeof(ub), "%ldd %ldh %ldm", d, h, m);
            dst.uptime = ub;
        }
        int fd = open("/sys/module/mali_kbase/srcversion", O_RDONLY | O_CLOEXEC);
        if (fd >= 0) {
            ssize_t n = ::read(fd, buf, sizeof(buf) - 1);
            close(fd);
            if (n > 0) {
                while (n > 0 && (buf[n-1] == '\n' || buf[n-1] == ' ')) n--;
                buf[n] = 0;
                dst.mali = buf;
            }
        }
        struct statvfs sv{};
        if (statvfs("/", &sv) == 0) {
            double total_gb = (double)sv.f_blocks * (double)sv.f_frsize / 1e9;
            double avail_gb = (double)sv.f_bavail * (double)sv.f_frsize / 1e9;
            char sb[96];
            snprintf(sb, sizeof(sb), "%.1f GB free of %.1f GB", avail_gb, total_gb);
            dst.storage = sb;
        }
    };

    gather_about_into(about);
    std::thread about_worker([&] {
        while (!about_quit.load(std::memory_order_relaxed)) {
            AboutInfo local{};
            gather_about_into(local);
            {
                std::lock_guard<std::mutex> lk(about_mu);
                about = std::move(local);
            }


            for (int i = 0; i < 50 && !about_quit.load(); i++)
                std::this_thread::sleep_for(std::chrono::milliseconds(20));
        }
    });

    static const char* settings_rows[] = {
        "General", "Wi-Fi", "Bluetooth", "Brightness", "Theme", "Date & Time", "About",
    };
    constexpr int settings_row_count = (int)(sizeof(settings_rows) / sizeof(settings_rows[0]));


    auto open_settings = [&]() {
        view = View::SETTINGS;
        view_anim.stiffness = 220.0f;
        view_anim.damping   =  31.0f;
        view_anim.set(1.0f);
    };
    auto close_settings = [&]() {
        view = View::LAUNCHER;
        view_anim.stiffness = 220.0f;
        view_anim.damping   =  31.0f;
        view_anim.set(0.0f);

        settings_sub = SubPage::LIST;
        sub_anim.snap_to(0.0f);
    };
    auto open_sub = [&](SubPage which) {
        settings_sub = which;
        sub_anim.stiffness = 240.0f;
        sub_anim.damping   =  32.0f;
        sub_anim.set(1.0f);
        if      (which == SubPage::BRIGHTNESS) load_brightness();
        else if (which == SubPage::THEME)      rescan_theme_options();
        else if (which == SubPage::WIFI)       kick_wifi_fetch();
        else if (which == SubPage::BLUETOOTH) {


            std::thread([]() { huskyfe::bluetooth::trigger_scan(); }).detach();
            kick_bt_fetch(true);
        }
    };
    auto close_sub = [&]() {


        settings_sub = SubPage::LIST;
        sub_anim.stiffness = 240.0f;
        sub_anim.damping   =  32.0f;
        sub_anim.set(0.0f);
        dragging_brightness = false;
    };


    auto open_menu = [&]() {
        menu_anim.stiffness = 380.0f;
        menu_anim.damping   =  14.0f;
        menu_anim.set(1.0f);
        menu_open = true;
    };
    auto close_menu = [&]() {


        menu_anim.stiffness = 1200.0f;
        menu_anim.damping   =   72.0f;
        menu_anim.set(0.0f);
        menu_open = false;
        menu_pressed_btn = -1;
    };

    enum class PowerTapAction {
        None,
        FadeOut,
        FadeIn,
    };


    bool pwr_down       = false;
    bool pwr_long_fired = false;
    bool pwr_menu_ok    = false;
    auto pwr_down_at    = std::chrono::steady_clock::now();
    PowerTapAction pwr_tap_action = PowerTapAction::None;
    constexpr int pwr_tap_ms  = 250;
    constexpr int pwr_long_ms = 700;

    auto menu_hit = [&](int x, int y) -> int {
        for (int i = 0; i < 3; i++) {
            const ButtonRect& b = menu_buttons[i];
            if ((float)x >= b.x && (float)x < b.x + b.w
             && (float)y >= b.y && (float)y < b.y + b.h)
                return i;
        }
        return -1;
    };
    auto in_menu = [&](int x, int y) -> bool {
        return (float)x >= menu_x && (float)x < menu_x + menu_w
            && (float)y >= menu_y && (float)y < menu_y + menu_h;
    };
    auto fire_menu_action = [&](int idx) {


        if (idx == 0)      std::system("sync; sleep 0.2; echo o > /proc/sysrq-trigger &");
        else if (idx == 1) std::system("sync; sleep 0.2; echo b > /proc/sysrq-trigger &");

    };

    auto hit_test = [&cells](int tx, int ty) -> int {
        for (size_t i = 0; i < cells.size(); i++) {
            const auto& c = cells[i];
            if ((float)tx >= c.x && (float)tx < c.x + c.w
             && (float)ty >= c.y && (float)ty < c.y + c.h)
                return (int)i;
        }
        return -1;
    };


    static const huskyfe::Color cell_palette[24] = {
        { 0.95f, 0.30f, 0.40f, 1.0f }, { 0.95f, 0.55f, 0.25f, 1.0f },
        { 0.95f, 0.80f, 0.30f, 1.0f }, { 0.55f, 0.85f, 0.35f, 1.0f },
        { 0.30f, 0.85f, 0.65f, 1.0f }, { 0.30f, 0.70f, 0.95f, 1.0f },
        { 0.45f, 0.45f, 0.95f, 1.0f }, { 0.70f, 0.40f, 0.95f, 1.0f },
        { 0.95f, 0.45f, 0.70f, 1.0f }, { 0.65f, 0.45f, 0.30f, 1.0f },
        { 0.50f, 0.50f, 0.55f, 1.0f }, { 0.25f, 0.35f, 0.45f, 1.0f },
        { 0.85f, 0.85f, 0.90f, 1.0f }, { 0.30f, 0.30f, 0.35f, 1.0f },
        { 0.95f, 0.95f, 0.40f, 1.0f }, { 0.30f, 0.95f, 0.95f, 1.0f },
        { 0.85f, 0.40f, 0.55f, 1.0f }, { 0.40f, 0.85f, 0.55f, 1.0f },
        { 0.55f, 0.40f, 0.85f, 1.0f }, { 0.85f, 0.55f, 0.40f, 1.0f },
        { 0.40f, 0.55f, 0.85f, 1.0f }, { 0.85f, 0.85f, 0.55f, 1.0f },
        { 0.55f, 0.85f, 0.85f, 1.0f }, { 0.95f, 0.30f, 0.95f, 1.0f },
    };

    drmEventContext evctx{};
    evctx.version = 2;
    evctx.page_flip_handler = +[](int, unsigned, unsigned, unsigned, void* user) {
        *static_cast<bool*>(user) = false;
    };

    uint64_t frames = 0;
    auto frames_t0 = std::chrono::steady_clock::now();
    auto last_frame = frames_t0;
    bool crtc_active = true;


    sd_notify("READY=1");


    std::thread thermal_thread(thermal_monitor_thread);
    thermal_thread.detach();

    while (!g_quit) {
        sd_notify("WATCHDOG=1");


        {
            bool want_low = g_thermal_low_request.load();
            bool is_low   = g_thermal_low_active.load();
            if (want_low != is_low && crtc_active && !g_blanked) {
                const drmModeModeInfo& target = want_low ? mode_low : mode;
                fprintf(stderr, "huskyfe: thermal switch -> %uHz\n", target.vrefresh);
                drmModeSetCrtc(drm_fd, crtc_id, bufs[front].fb_id, 0, 0,
                               &conn->connector_id, 1,
                               const_cast<drmModeModeInfo*>(&target));
                g_thermal_low_active.store(want_low);
            }
        }


        if (g_blanked) {
            if (crtc_active) {
                drmModeSetCrtc(drm_fd, crtc_id, 0, 0, 0, nullptr, 0, nullptr);
                if (dpms_prop_id) {
                    drmModeConnectorSetProperty(drm_fd, conn->connector_id,
                                                dpms_prop_id, DRM_MODE_DPMS_OFF);
                }
                set_bl_power(4);
                gpu_stack_signal("STOP");
                set_cpu_governor("schedutil");
                set_perf_mon(0);
                crtc_active = false;
                fprintf(stderr, "huskyfe: blanked (DPMS off, bl off, GPU paused, governor=schedutil, perf_mon=off)\n");
            }
            fd_set fds; FD_ZERO(&fds);
            int maxfd = -1;
            int kfd = huskyfe::input_keys_fd();
            int tfd = huskyfe::input_touch_fd();
            if (kfd >= 0) { FD_SET(kfd, &fds); if (kfd > maxfd) maxfd = kfd; }
            if (tfd >= 0) { FD_SET(tfd, &fds); if (tfd > maxfd) maxfd = tfd; }


            timeval tv_blank{ 5, 0 };
            if (select(maxfd + 1, &fds, nullptr, nullptr, &tv_blank) > 0) {

                if (tfd >= 0 && FD_ISSET(tfd, &fds)) {
                    huskyfe::input_drain(tfd, [](const huskyfe::InputEvent&){});
                }
                if (kfd >= 0 && FD_ISSET(kfd, &fds)) {
                    huskyfe::input_drain(kfd, [&](const huskyfe::InputEvent& e) {
                        if (e.kind == huskyfe::InputKind::PowerPressed) {
                            pwr_down = true;
                            pwr_long_fired = false;
                            pwr_menu_ok = true;
                            pwr_down_at = std::chrono::steady_clock::now();
                            pwr_tap_action = PowerTapAction::FadeIn;
                        } else if (e.kind == huskyfe::InputKind::PowerReleased && pwr_down) {
                            int held_ms = (int)std::chrono::duration_cast<std::chrono::milliseconds>(
                                              std::chrono::steady_clock::now() - pwr_down_at).count();
                            g_blanked = false;
                            g_fading_in = true;
                            g_fading_out = false;
                            g_fade.snap_to(1.0f);

                            g_fade.stiffness = 24.0f;
                            g_fade.damping = 10.0f;
                            g_fade.set(0.0f);
                            buzz_unlock_blackhole();
                            if (held_ms > pwr_tap_ms) open_menu();
                            pwr_down = false;
                            pwr_long_fired = false;
                            pwr_menu_ok = false;
                            pwr_tap_action = PowerTapAction::None;
                        } else if (e.kind == huskyfe::InputKind::PowerReleased) {
                            pwr_down = false;
                            pwr_long_fired = false;
                            pwr_menu_ok = false;
                            pwr_tap_action = PowerTapAction::None;
                        }
                    });
                }
            }
            continue;
        }

        if (!crtc_active) {
            apply_active_power_profile();
            gpu_stack_signal("CONT");
            set_bl_power(0);
            if (dpms_prop_id) {
                drmModeConnectorSetProperty(drm_fd, conn->connector_id,
                                            dpms_prop_id, DRM_MODE_DPMS_ON);
            }
            {
                drmModeModeInfo* m_active = g_thermal_low_active.load()
                                                ? &mode_low : &mode;
                drmModeSetCrtc(drm_fd, crtc_id, bufs[front].fb_id, 0, 0,
                               &conn->connector_id, 1, m_active);
            }
            crtc_active = true;


            last_frame = std::chrono::steady_clock::now();
            fprintf(stderr, "huskyfe: unblanked (DPMS on, bl on, GPU resumed)\n");
        }

        const float sw = (float)bufs[back].w;
        const float sh = (float)bufs[back].h;


        auto now = std::chrono::steady_clock::now();
        auto t_frame_top = now;
        float real_dt = std::chrono::duration<float>(now - last_frame).count();
        last_frame = now;


        constexpr float FIXED_DT = 1.0f / 120.0f;
        static float anim_accum = 0.0f;
        anim_accum += real_dt;
        if (anim_accum > FIXED_DT * 4.0f) anim_accum = FIXED_DT * 4.0f;
        while (anim_accum >= FIXED_DT) {
            for (auto& s : cell_scales) s.tick(FIXED_DT);
            menu_anim.tick(FIXED_DT);
            view_anim.tick(FIXED_DT);
            sub_anim.tick(FIXED_DT);
            quick_anim.tick(FIXED_DT);
            keyboard.tick(FIXED_DT);
            g_fade.tick(FIXED_DT);
            ctx_menu_anim.tick(FIXED_DT);
            page_anim.tick(FIXED_DT);
            huskyfe::notifications::tick(FIXED_DT);
            anim_accum -= FIXED_DT;
        }
        if (g_fading_out && g_fade.settled()) {
            g_fading_out = false;
            g_blanked = true;
        } else if (g_fading_in && g_fade.settled()) {
            g_fading_in = false;
        }


        {
            auto kr = keyboard.consume_result();
            if (kr == huskyfe::Keyboard::Result::ACCEPTED) {
                if (keyboard_owner.rfind("wifi:", 0) == 0) {
                    std::string ssid = keyboard_owner.substr(5);
                    std::string psk  = keyboard.text();
                    fprintf(stderr, "huskyfe: connecting to '%s' with password (%zu chars)\n",
                            ssid.c_str(), psk.size());
                    huskyfe::wifi::connect_new(ssid, psk);
                    begin_connect(ssid);
                    refresh_wifi();
                }
                keyboard_owner.clear();
            } else if (kr == huskyfe::Keyboard::Result::CANCELLED) {
                keyboard_owner.clear();
            }
        }


        if (view == View::SETTINGS && settings_sub == SubPage::WIFI
            && std::chrono::duration<float>(now - last_wifi_refresh).count() >= 1.0f) {
            kick_wifi_fetch();
            last_wifi_refresh = now;
        }
        if (view == View::SETTINGS && settings_sub == SubPage::BLUETOOTH
            && std::chrono::duration<float>(now - last_bt_refresh).count() >= 1.5f) {
            kick_bt_fetch(false);
            last_bt_refresh = now;
        }


        refresh_wifi();
        refresh_bt();


        auto t_phase_start = std::chrono::steady_clock::now();


        {
            std::vector<std::function<void()>> pending;
            {
                std::lock_guard<std::mutex> lk(ipc_mu);
                if (!ipc_pending.empty()) pending = std::move(ipc_pending);
            }
            for (auto& f : pending) f();
        }


        if (pwr_down && pwr_menu_ok && !pwr_long_fired) {
            int held_ms = (int)std::chrono::duration_cast<std::chrono::milliseconds>(
                              now - pwr_down_at).count();
            if (held_ms >= pwr_long_ms) {
                pwr_long_fired = true;
                if (hit_cell >= 0) {
                    cell_scales[hit_cell].set(1.0f);
                    hit_cell = -1;
                }
                open_menu();
            }
        }

        const auto& st = huskyfe::status::read();


        huskyfe::wlhost::dispatch();
        auto t_post_dispatch = std::chrono::steady_clock::now();


        glClearColor(0.0f, 0.0f, 0.0f, 1.0f);
        glClear(GL_COLOR_BUFFER_BIT);


        const bool wl_active = huskyfe::wlhost::has_active_surface();


        float bg_seconds = std::chrono::duration<float>(now - bg_t0).count();

        float bg_dim = std::clamp(std::max(menu_anim.value, view_anim.value),
                                  0.0f, 1.0f);


        static const bool no_bg = getenv("HUSKYFE_NO_BG") != nullptr;
        bg.set_paused(battery_saver);
        if (!wl_active && !no_bg) bg.draw(bg_seconds, bg_dim);
        renderer.begin_pass();


        if (wl_active) {
            renderer.flush();
            huskyfe::wlhost::draw(image_r, renderer.xform_data(), sw, sh);
        }

        auto t_post_surface = std::chrono::steady_clock::now();


        const bool osk_wants =
            wl_active && (huskyfe::wlhost::focused_is_terminal()
                       || huskyfe::wlhost::text_input_wanted());
        static bool prev_osk_wants = false;
        const bool rising = osk_wants && !prev_osk_wants;
        if (osk_wants != prev_osk_wants) {
            fprintf(stderr,
                "huskyfe: osk_wants %d->%d rising=%d wl=%d term=%d ti=%d vis=%d\n",
                (int)prev_osk_wants, (int)osk_wants, (int)rising,
                (int)wl_active, (int)huskyfe::wlhost::focused_is_terminal(),
                (int)huskyfe::wlhost::text_input_wanted(),
                (int)keyboard.visible());
        }
        prev_osk_wants = osk_wants;

        if (!wl_active) {
            if (keyboard.visible() && keyboard.is_live()) keyboard.dismiss();
        } else if (rising && !keyboard.visible()) {
            fprintf(stderr, "huskyfe: -> show_live (rising edge)\n");
            keyboard.show_live([](char c) {
                uint32_t t = (uint32_t)std::chrono::duration_cast<std::chrono::milliseconds>(
                    std::chrono::steady_clock::now().time_since_epoch()).count();
                huskyfe::wlhost::keyboard_send_char(t, c);
            });
        }


        const bool draw_launcher = !wl_active && view_anim.value < 0.999f
                                   && !camera_preview_open;


        if (draw_launcher)
        renderer.draw_rect(0.0f, 0.0f, sw, status_h, { 0.0f, 0.0f, 0.0f, 0.35f }, 0.0f);


        const int active_cells = (int)std::min((size_t)ncells, installed.size());
        for (int i = 0; draw_launcher && i < active_cells; i++) {
            const CellRect& c = cells[i];
            float scale  = cell_scales[i].value;
            float sw_px  = icon_size * scale;
            float sh_px  = icon_size * scale;
            float sr     = icon_r * scale;
            float cx     = c.x + c.w * 0.5f;
            float cy     = c.y + c.h * 0.5f;

            huskyfe::Color top, bot;
            if (i == 0) {


                top = { 0.42f, 0.78f, 0.96f, 1.0f };
                bot = { 0.06f, 0.14f, 0.34f, 1.0f };


                float halo = 16.0f * scale;
                renderer.draw_rect(cx - sw_px * 0.5f - halo,
                                   cy - sh_px * 0.5f - halo,
                                   sw_px + 2.0f * halo,
                                   sh_px + 2.0f * halo,
                                   { top.r, top.g, top.b, 0.28f },
                                   (icon_r + halo) * scale);
            } else {
                top = cell_palette[i];
                bot = { top.r * 0.45f, top.g * 0.45f, top.b * 0.45f, top.a };
            }
            renderer.draw_rect_gradient(cx - sw_px * 0.5f, cy - sh_px * 0.5f,
                                        sw_px, sh_px, top, bot, sr);
        }


        char left[128];
        const char* bt_tag = st.bt_connected ? "  BT*"
                           : st.bt_powered   ? "  BT"
                                             : "";
        if (st.wifi_up && !st.ipv4.empty())
            snprintf(left, sizeof(left), "Wi-Fi  %s%s", st.ipv4.c_str(), bt_tag);
        else if (st.wifi_up)
            snprintf(left, sizeof(left), "Wi-Fi%s", bt_tag);
        else
            snprintf(left, sizeof(left), "No Wi-Fi%s", bt_tag);

        char right[16];
        if (st.battery_pct >= 0)
            snprintf(right, sizeof(right), "%d%%", st.battery_pct);
        else
            snprintf(right, sizeof(right), "--");

        char clock_buf[16];
        {
            time_t t = time(nullptr);
            struct tm lt{};
            localtime_r(&t, &lt);

            strftime(clock_buf, sizeof(clock_buf), "%-I:%M %p", &lt);
        }

        const float baseline   = status_h * 0.5f + text.ascent() * 0.35f;
        const float right_w    = text.measure_width(right);
        const float right_x    = sw - 80.0f - right_w;
        const huskyfe::Color fg{ 0.95f, 0.95f, 0.97f, 1.0f };
        const huskyfe::Color label_color{ 0.88f, 0.88f, 0.94f, 0.95f };


        if (draw_launcher && st.charging) {
            constexpr float pill_w = 32.0f;
            constexpr float pill_h = 14.0f;
            constexpr float pill_gap = 14.0f;
            float pill_x = right_x - pill_gap - pill_w;
            float pill_y = baseline - text.ascent() * 0.35f - pill_h * 0.5f;
            renderer.draw_rect(pill_x, pill_y, pill_w, pill_h,
                               { 0.30f, 0.92f, 0.45f, 1.0f }, pill_h * 0.5f);
        }


        if (draw_launcher)
        renderer.draw_rect(sw * 0.5f - 200.0f, sh - 32.0f,
                           400.0f, 8.0f,
                           { 1.0f, 1.0f, 1.0f, 0.7f }, 4.0f);


        if (!wl_active && view == View::LAUNCHER) {
            const float dot_y = sh - 80.0f;
            const float dot_d = 16.0f;
            const float gap   = 22.0f;
            const float a = std::clamp(page_anim.value, 0.0f, 1.0f);
            const float cx = sw * 0.5f;

            float a0 = 1.0f - a;
            renderer.draw_rect(cx - gap - dot_d * 0.5f, dot_y - dot_d * 0.5f,
                               dot_d, dot_d,
                               { 1.0f, 1.0f, 1.0f, 0.35f + 0.55f * a0 },
                               dot_d * 0.5f);

            renderer.draw_rect(cx + gap - dot_d * 0.5f, dot_y - dot_d * 0.5f,
                               dot_d, dot_d,
                               { 1.0f, 1.0f, 1.0f, 0.35f + 0.55f * a },
                               dot_d * 0.5f);
        }

        renderer.flush();


        if (draw_launcher) {
            constexpr float icon_inset = 0.88f;
            image_r.begin(renderer.xform_data(), icon_atlas.texture());
            for (int i = 0; i < active_cells; i++) {
                if (!icon_atlas.has_slot(i)) continue;
                const CellRect& c = cells[i];
                float scale = cell_scales[i].value;
                float iw = icon_size * icon_inset * scale;
                float ih = icon_size * icon_inset * scale;
                float cx = c.x + c.w * 0.5f;
                float cy = c.y + c.h * 0.5f;
                float u0, v0, u1, v1;
                icon_atlas.uv_rect(i, u0, v0, u1, v1);
                image_r.draw(cx - iw * 0.5f, cy - ih * 0.5f, iw, ih,
                             u0, v0, u1, v1);
            }
            image_r.end();
        }


        if (draw_launcher) {
            const huskyfe::Color q{ 1.0f, 1.0f, 1.0f, 0.55f };
            const float qw  = huge_text.measure_width("?");
            huge_text.begin(renderer.xform_data());
            for (int i = 0; i < active_cells; i++) {
                if (icon_atlas.has_slot(i)) continue;
                const CellRect& c = cells[i];
                float scale = cell_scales[i].value;
                float cx = c.x + c.w * 0.5f;
                float cy = c.y + c.h * 0.5f;
                float baseline = cy + huge_text.ascent() * 0.30f * scale;
                huge_text.draw(cx - qw * scale * 0.5f, baseline, "?", q, scale);
            }
            huge_text.end();
        }


        if (draw_launcher) {
            text.begin(renderer.xform_data());
            text.draw(80.0f, baseline, left, fg);
            text.draw(right_x, baseline, right, fg);


            float clock_w = text.measure_width(clock_buf);
            float clock_x = right_x - 60.0f - clock_w;
            text.draw(clock_x, baseline, clock_buf, fg);


            if (st.cpu_temp_c > 0) {
                char temp_buf[12];
                snprintf(temp_buf, sizeof(temp_buf), "%d°C", st.cpu_temp_c);
                float temp_w = text.measure_width(temp_buf);
                float temp_x = clock_x - 60.0f - temp_w;
                huskyfe::Color tc = fg;
                if      (st.cpu_temp_c >= 80) tc = { 1.00f, 0.35f, 0.30f, 1.0f };
                else if (st.cpu_temp_c >= 70) tc = { 1.00f, 0.75f, 0.30f, 1.0f };
                text.draw(temp_x, baseline, temp_buf, tc);
            }
            text.end();
        }


        constexpr float label_pad_top = 16.0f;
        if (draw_launcher) {
        label_text.begin(renderer.xform_data());
        for (int i = 0; i < active_cells; i++) {
            const CellRect& c = cells[i];
            const char* name = installed[i].name.c_str();
            float w  = label_text.measure_width(name);
            float lx = c.x + c.w * 0.5f - w * 0.5f;
            float ly = c.y + c.h + label_pad_top + label_text.ascent();
            label_text.draw(lx, ly, name, label_color);
        }
        label_text.end();
        }


        if (draw_launcher) {
            renderer.flush();
            renderer.begin_pass();
            for (int i = 0; i < active_cells; i++) {
                if (i == 0) continue;
                if (!find_running_for_cell(installed[i].exec)) continue;
                const CellRect& c = cells[i];
                constexpr float dot_d = 24.0f;
                const float dx = c.x + c.w - dot_d - 8.0f;
                const float dy = c.y + 8.0f;
                renderer.draw_rect(dx, dy, dot_d, dot_d,
                                   { 0.45f, 0.95f, 0.55f, 1.0f }, dot_d * 0.5f);
            }
            renderer.flush();
        }


        if (ctx_menu_anim.value > 0.005f && ctx_menu_cell >= 0
            && ctx_menu_cell < (int)installed.size()) {
            float a       = std::clamp(ctx_menu_anim.value, 0.0f, 1.0f);
            float opacity = a;
            const CellRect& cc = cells[ctx_menu_cell];
            constexpr float panel_w   = 520.0f;
            constexpr float panel_h   = 280.0f;
            constexpr float btn_h_loc = 100.0f;
            const float panel_x = cc.x + cc.w * 0.5f - panel_w * 0.5f;
            float panel_y = cc.y + cc.h + 20.0f;
            if (panel_y + panel_h > sh - 100.0f)
                panel_y = cc.y - panel_h - 20.0f;
            const float btn1_y = panel_y + 30.0f;
            const float btn2_y = btn1_y + btn_h_loc + 20.0f;
            const float btn_x  = panel_x + 30.0f;
            const float btn_w_loc = panel_w - 60.0f;


            renderer.flush();
            renderer.begin_pass();
            renderer.draw_rect(0.0f, 0.0f, sw, sh,
                               { 0.0f, 0.0f, 0.0f, 0.35f * opacity }, 0.0f);

            renderer.draw_rect_gradient(panel_x, panel_y, panel_w, panel_h,
                                        { 0.18f, 0.18f, 0.22f, 0.95f * opacity },
                                        { 0.10f, 0.10f, 0.13f, 0.95f * opacity },
                                        24.0f);

            bool running_now = (ctx_menu_cell > 0)
                && find_running_for_cell(installed[ctx_menu_cell].exec) != 0;
            float close_alpha = running_now ? 1.0f : 0.4f;
            huskyfe::Color btn1_top = { 0.55f, 0.18f, 0.18f, opacity * close_alpha };
            huskyfe::Color btn1_bot = { 0.30f, 0.10f, 0.10f, opacity * close_alpha };
            if (ctx_menu_pressed == 0 && running_now) {
                btn1_top.r *= 0.78f; btn1_top.g *= 0.78f; btn1_top.b *= 0.78f;
            }
            renderer.draw_rect_gradient(btn_x, btn1_y, btn_w_loc, btn_h_loc,
                                        btn1_top, btn1_bot, 16.0f);
            huskyfe::Color btn2_top = { 0.18f, 0.18f, 0.22f, opacity };
            huskyfe::Color btn2_bot = { 0.10f, 0.10f, 0.13f, opacity };
            if (ctx_menu_pressed == 1) {
                btn2_top.r *= 0.78f; btn2_top.g *= 0.78f; btn2_top.b *= 0.78f;
            }
            renderer.draw_rect_gradient(btn_x, btn2_y, btn_w_loc, btn_h_loc,
                                        btn2_top, btn2_bot, 16.0f);
            renderer.flush();


            text.begin(renderer.xform_data());
            const huskyfe::Color label_fg{ 0.97f, 0.97f, 0.99f, opacity };
            float bw_close = text.measure_width("Close");
            float bw_hide  = text.measure_width("Hide");
            text.draw(btn_x + (btn_w_loc - bw_close) * 0.5f,
                      btn1_y + btn_h_loc * 0.5f + text.ascent() * 0.32f,
                      "Close", label_fg);
            text.draw(btn_x + (btn_w_loc - bw_hide) * 0.5f,
                      btn2_y + btn_h_loc * 0.5f + text.ascent() * 0.32f,
                      "Hide", label_fg);
            text.end();
        }


        if (page_anim.value > 0.005f && view == View::LAUNCHER && !wl_active) {
            float a       = std::clamp(page_anim.value, 0.0f, 1.0f);
            float offset  = (1.0f - a) * sw;
            float opacity = a;


            auto hist = huskyfe::notifications::history_snapshot();

            constexpr float n_top_safe   = 240.0f;
            constexpr float n_header_h   = 140.0f;
            constexpr float n_card_w     = 1100.0f;
            constexpr float n_card_h     = 200.0f;
            constexpr float n_card_gap   = 18.0f;
            constexpr float n_card_pad_x = 50.0f;

            renderer.begin_pass();

            renderer.draw_rect(offset, 0.0f, sw, sh,
                               { 0.07f, 0.07f, 0.10f, opacity }, 0.0f);
            renderer.flush();


            const huskyfe::Color title_fg{ 1.0f, 1.0f, 1.0f, opacity };
            const huskyfe::Color dim_fg  { 0.65f, 0.65f, 0.72f, opacity };
            text.begin(renderer.xform_data());
            const char* n_title = "Notifications";
            float n_tw = text.measure_width(n_title);
            float n_ty = n_top_safe + n_header_h * 0.5f + text.ascent() * 0.32f;
            text.draw(offset + (sw - n_tw) * 0.5f, n_ty, n_title, title_fg);
            text.end();

            renderer.begin_pass();

            const float n_list_y0 = n_top_safe + n_header_h + 60.0f;
            if (hist.empty()) {


            } else {
                for (size_t i = 0; i < hist.size(); i++) {
                    const auto& h = hist[i];
                    float cy = n_list_y0 + (float)i * (n_card_h + n_card_gap);
                    if (cy > sh) break;
                    float cx = offset + (sw - n_card_w) * 0.5f;


                    if (notif_pressed_idx == (int)i) {
                        renderer.draw_rect_gradient(cx, cy, n_card_w, n_card_h,
                            { 0.10f, 0.11f, 0.14f, 0.97f * opacity },
                            { 0.06f, 0.07f, 0.09f, 0.97f * opacity }, 36.0f);
                    } else {
                        renderer.draw_rect_gradient(cx, cy, n_card_w, n_card_h,
                            { 0.16f, 0.17f, 0.21f, 0.97f * opacity },
                            { 0.10f, 0.11f, 0.14f, 0.97f * opacity }, 36.0f);
                    }
                }
            }
            renderer.flush();


            renderer.begin_pass();
            for (size_t i = 0; i < hist.size(); i++) {
                const auto& h = hist[i];
                float cy = n_list_y0 + (float)i * (n_card_h + n_card_gap);
                if (cy > sh) break;
                float cx = offset + (sw - n_card_w) * 0.5f;
                float bcx = cx + 60.0f;
                float bcy = cy + n_card_h * 0.5f;
                constexpr float D = 64.0f;
                huskyfe::Color base = (h.urgency <= 0)
                    ? huskyfe::Color{ 0.40f, 0.55f, 0.95f, 1.0f }
                    : (h.urgency >= 2)
                        ? huskyfe::Color{ 0.95f, 0.30f, 0.30f, 1.0f }
                        : huskyfe::Color{ 0.30f, 0.80f, 0.50f, 1.0f };
                huskyfe::Color fill{ base.r, base.g, base.b, 0.95f * opacity };
                huskyfe::Color botc{ base.r * 0.55f, base.g * 0.55f, base.b * 0.55f, 0.95f * opacity };
                renderer.draw_rect_gradient(bcx - D * 0.5f, bcy - D * 0.5f,
                                            D, D, fill, botc, D * 0.5f);
                huskyfe::Color ink{ 1.0f, 1.0f, 1.0f, 0.95f * opacity };
                if (h.urgency <= 0) {
                    renderer.draw_rect(bcx - 4.0f, bcy - 14.0f, 8.0f, 8.0f, ink, 4.0f);
                    renderer.draw_rect(bcx - 3.5f, bcy - 1.0f, 7.0f, 22.0f, ink, 3.5f);
                } else if (h.urgency >= 2) {
                    renderer.draw_rect(bcx - 4.0f, bcy - 16.0f, 8.0f, 24.0f, ink, 4.0f);
                    renderer.draw_rect(bcx - 4.0f, bcy + 13.0f, 8.0f, 8.0f, ink, 4.0f);
                } else {

                    renderer.draw_rect(bcx - 2.5f, bcy - 18.0f, 5.0f, 4.0f, ink, 2.5f);
                    renderer.draw_rect(bcx - 15.0f, bcy - 14.0f, 30.0f, 26.0f, ink, 12.6f);
                    renderer.draw_rect(bcx - 18.0f, bcy + 12.0f, 36.0f, 5.0f, ink, 2.5f);
                    renderer.draw_rect(bcx - 4.0f, bcy + 18.0f, 8.0f, 8.0f, ink, 4.0f);
                }
            }
            renderer.flush();


            text.begin(renderer.xform_data());
            for (size_t i = 0; i < hist.size(); i++) {
                const auto& h = hist[i];
                float cy = n_list_y0 + (float)i * (n_card_h + n_card_gap);
                if (cy > sh) break;
                float cx = offset + (sw - n_card_w) * 0.5f;
                constexpr float TEXT_LEFT_PAD = 120.0f;
                float card_cx = cx + TEXT_LEFT_PAD
                              + (n_card_w - TEXT_LEFT_PAD - 30.0f) * 0.5f;
                if (!h.app_name.empty()) {
                    constexpr float app_scale = 0.55f;
                    float aw = text.measure_width(h.app_name.c_str()) * app_scale;
                    float ay = cy + 36.0f + text.ascent() * 0.32f * app_scale;
                    text.draw(card_cx - aw * 0.5f, ay, h.app_name.c_str(), dim_fg, app_scale);
                }
                if (!h.summary.empty()) {
                    constexpr float sum_scale = 0.85f;
                    float sw_text = text.measure_width(h.summary.c_str()) * sum_scale;
                    float sy = cy + n_card_h * 0.5f + text.ascent() * 0.32f * sum_scale;
                    text.draw(card_cx - sw_text * 0.5f, sy, h.summary.c_str(), title_fg, sum_scale);
                }
            }
            text.end();


            label_text.begin(renderer.xform_data());
            for (size_t i = 0; i < hist.size(); i++) {
                const auto& h = hist[i];
                float cy = n_list_y0 + (float)i * (n_card_h + n_card_gap);
                if (cy > sh) break;
                if (h.body.empty()) continue;
                float cx = offset + (sw - n_card_w) * 0.5f;
                std::string line = h.body;
                for (auto& c : line) if (c == '\n' || c == '\r') c = ' ';
                const float max_w = n_card_w - 2.0f * n_card_pad_x;
                while (label_text.measure_width(line.c_str()) > max_w && line.size() > 1) {
                    line.pop_back();
                }
                if (line.size() < h.body.size()) {
                    while (line.size() > 3 && label_text.measure_width((line + "...").c_str()) > max_w) {
                        line.pop_back();
                    }
                    line += "...";
                }
                constexpr float TEXT_LEFT_PAD = 120.0f;
                float card_cx = cx + TEXT_LEFT_PAD
                              + (n_card_w - TEXT_LEFT_PAD - 30.0f) * 0.5f;
                float lw = label_text.measure_width(line.c_str());
                float ly = cy + n_card_h - 38.0f;
                huskyfe::Color body_c{ 0.86f, 0.88f, 0.92f, 0.93f * opacity };
                label_text.draw(card_cx - lw * 0.5f, ly, line.c_str(), body_c);
            }
            label_text.end();


            if (hist.empty()) {
                text.begin(renderer.xform_data());
                const char* msg = "No notifications";
                float mw = text.measure_width(msg);
                float my = sh * 0.5f + text.ascent() * 0.32f;
                huskyfe::Color msg_c{ 0.55f, 0.55f, 0.62f, opacity };
                text.draw(offset + (sw - mw) * 0.5f, my, msg, msg_c);
                text.end();
            }
        }


        if (view_anim.value > 0.005f) {
            float a       = std::clamp(view_anim.value, 0.0f, 1.0f);
            float offset  = (1.0f - a) * sw;
            float opacity = a;


            constexpr float top_safe     = 240.0f;
            constexpr float header_h     = 140.0f;
            constexpr float back_size    = 110.0f;
            constexpr float back_x       = 60.0f;
            const     float back_y       = top_safe + (header_h - back_size) * 0.5f;
            constexpr float row_h        = 140.0f;
            constexpr float row_pad_x    = 80.0f;
            const     float row_first_y  = top_safe + header_h + 80.0f;
            constexpr float row_gap      = 22.0f;

            renderer.begin_pass();


            renderer.draw_rect(offset, 0.0f, sw, sh,
                               { 0.07f, 0.07f, 0.10f, opacity }, 0.0f);


            float div_y = top_safe + header_h + 36.0f;
            renderer.draw_rect(offset + row_pad_x, div_y,
                               sw - 2.0f * row_pad_x, 2.0f,
                               { 1.0f, 1.0f, 1.0f, opacity * 0.06f }, 1.0f);


            huskyfe::Color back_bg{ 0.22f, 0.22f, 0.26f, opacity };
            if (settings_pressed_row == 0) { back_bg.r *= 0.78f; back_bg.g *= 0.78f; back_bg.b *= 0.78f; }
            huskyfe::Color back_bb{ back_bg.r * 0.45f, back_bg.g * 0.45f, back_bg.b * 0.45f, back_bg.a };
            renderer.draw_rect_gradient(offset + back_x, back_y, back_size, back_size,
                                        back_bg, back_bb, back_size * 0.5f);


            float ax            = std::clamp(sub_anim.value, 0.0f, 1.0f);
            float list_inner_x  = -ax * sw;
            float sub_inner_x   = (1.0f - ax) * sw;

            const bool draw_list = ax < 0.999f;
            const bool draw_sub  = ax > 0.001f;
            const bool sub_about      = (settings_sub == SubPage::ABOUT)      || (sub_anim.value > 0.0f && settings_sub == SubPage::ABOUT);
            const bool sub_brightness = (settings_sub == SubPage::BRIGHTNESS) || (sub_anim.value > 0.0f && settings_sub == SubPage::BRIGHTNESS);
            (void)sub_about; (void)sub_brightness;


            if (draw_list) {

                huskyfe::Color back_bg{ 0.22f, 0.22f, 0.26f, opacity };
                if (settings_pressed_row == 0) { back_bg.r *= 0.78f; back_bg.g *= 0.78f; back_bg.b *= 0.78f; }
                huskyfe::Color back_bb{ back_bg.r * 0.45f, back_bg.g * 0.45f, back_bg.b * 0.45f, back_bg.a };
                renderer.draw_rect_gradient(offset + list_inner_x + back_x, back_y,
                                            back_size, back_size,
                                            back_bg, back_bb, back_size * 0.5f);


                for (int i = 0; i < settings_row_count; i++) {
                    float ry = row_first_y + i * (row_h + row_gap);
                    huskyfe::Color top_c{ 0.18f, 0.18f, 0.22f, opacity };
                    if (settings_pressed_row == i + 1) { top_c.r *= 0.78f; top_c.g *= 0.78f; top_c.b *= 0.78f; }
                    huskyfe::Color bot_c{ top_c.r * 0.45f, top_c.g * 0.45f, top_c.b * 0.45f, top_c.a };
                    renderer.draw_rect_gradient(offset + list_inner_x + row_pad_x, ry,
                                                sw - 2.0f * row_pad_x, row_h,
                                                top_c, bot_c, 28.0f);
                }
            }


            if (draw_sub) {

                huskyfe::Color back_bg{ 0.22f, 0.22f, 0.26f, opacity };
                if (sub_pressed_back == 1) { back_bg.r *= 0.78f; back_bg.g *= 0.78f; back_bg.b *= 0.78f; }
                huskyfe::Color back_bb{ back_bg.r * 0.45f, back_bg.g * 0.45f, back_bg.b * 0.45f, back_bg.a };
                renderer.draw_rect_gradient(offset + sub_inner_x + back_x, back_y,
                                            back_size, back_size,
                                            back_bg, back_bb, back_size * 0.5f);


                if (settings_sub == SubPage::WIFI) {
                    constexpr float wifi_action_w   = 440.0f;
                    constexpr float wifi_action_h   = 110.0f;
                    constexpr float wifi_action_gap = 20.0f;
                    const     float wifi_action_y   = top_safe + header_h + 220.0f;
                    const     float wifi_refresh_x  = (sw - 2.0f * wifi_action_w - wifi_action_gap) * 0.5f;
                    const     float wifi_disc_x     = wifi_refresh_x + wifi_action_w + wifi_action_gap;
                    const     float wifi_list_y0    = wifi_action_y + wifi_action_h + 50.0f;
                    constexpr float wifi_row_h      = 110.0f;
                    constexpr float wifi_row_gap    = 18.0f;
                    constexpr float wifi_row_pad_x  = 80.0f;


                    {
                        huskyfe::Color c{ 0.30f, 0.60f, 0.95f, opacity };
                        if (wifi_pressed == 1) { c.r *= 0.78f; c.g *= 0.78f; c.b *= 0.78f; }
                        huskyfe::Color cb{ c.r * 0.45f, c.g * 0.45f, c.b * 0.45f, c.a };
                        renderer.draw_rect_gradient(offset + sub_inner_x + wifi_refresh_x,
                                                    wifi_action_y, wifi_action_w, wifi_action_h,
                                                    c, cb, wifi_action_h * 0.5f);
                    }

                    if (wifi_status.connected) {
                        huskyfe::Color c{ 0.85f, 0.30f, 0.30f, opacity };
                        if (wifi_pressed == 2) { c.r *= 0.78f; c.g *= 0.78f; c.b *= 0.78f; }
                        huskyfe::Color cb{ c.r * 0.45f, c.g * 0.45f, c.b * 0.45f, c.a };
                        renderer.draw_rect_gradient(offset + sub_inner_x + wifi_disc_x,
                                                    wifi_action_y, wifi_action_w, wifi_action_h,
                                                    c, cb, wifi_action_h * 0.5f);
                    }


                    for (size_t i = 0; i < wifi_nets.size(); i++) {
                        const auto& n = wifi_nets[i];
                        bool current = wifi_status.connected && n.ssid == wifi_status.ssid;
                        float ry = wifi_list_y0 + i * (wifi_row_h + wifi_row_gap);
                        huskyfe::Color top_c = current
                            ? huskyfe::Color{ 0.20f, 0.42f, 0.30f, opacity }
                            : huskyfe::Color{ 0.18f, 0.18f, 0.22f, opacity };
                        if (wifi_pressed == 100 + (int)i) {
                            top_c.r *= 0.78f; top_c.g *= 0.78f; top_c.b *= 0.78f;
                        }
                        huskyfe::Color bot_c{ top_c.r * 0.45f, top_c.g * 0.45f, top_c.b * 0.45f, top_c.a };
                        renderer.draw_rect_gradient(offset + sub_inner_x + wifi_row_pad_x, ry,
                                                    sw - 2.0f * wifi_row_pad_x, wifi_row_h,
                                                    top_c, bot_c, 28.0f);


                        int strength = 1;
                        if      (n.signal >= -55) strength = 4;
                        else if (n.signal >= -65) strength = 3;
                        else if (n.signal >= -75) strength = 2;
                        else                      strength = 1;
                        const float bars_x  = sw - wifi_row_pad_x - 32.0f - 4 * (10.0f + 6.0f);
                        const float bars_cy = ry + wifi_row_h * 0.5f;
                        for (int b = 0; b < 4; b++) {
                            float bh = 14.0f + b * 9.0f;
                            float bx = bars_x + b * 16.0f;
                            float by = bars_cy + 22.0f - bh;
                            huskyfe::Color bc = (b < strength)
                                ? huskyfe::Color{ 1.0f, 1.0f, 1.0f, opacity }
                                : huskyfe::Color{ 1.0f, 1.0f, 1.0f, opacity * 0.18f };
                            renderer.draw_rect(offset + sub_inner_x + bx, by, 10.0f, bh, bc, 3.0f);
                        }
                    }
                }


                if (settings_sub == SubPage::BLUETOOTH) {
                    constexpr float bt_action_w    = 440.0f;
                    constexpr float bt_action_h    = 110.0f;
                    constexpr float bt_action_gap  = 20.0f;
                    const     float bt_action_y    = top_safe + header_h + 220.0f;
                    const     float bt_scan_x      = (sw - 2.0f * bt_action_w - bt_action_gap) * 0.5f;
                    const     float bt_power_x     = bt_scan_x + bt_action_w + bt_action_gap;


                    const     bool  bt_show_adp    = bt_adapters.size() >= 2;
                    constexpr float bt_adp_row_h   = 70.0f;
                    constexpr float bt_adp_row_gap = 10.0f;
                    constexpr float bt_adp_pad_x   = 80.0f;
                    const     float bt_adp_y0      = bt_action_y + bt_action_h + 30.0f;
                    const     float bt_adp_total_h = bt_show_adp
                        ? (float)bt_adapters.size() * bt_adp_row_h
                          + ((float)bt_adapters.size() - 1.0f) * bt_adp_row_gap
                        : 0.0f;
                    const     float bt_list_y0     = bt_show_adp
                        ? bt_adp_y0 + bt_adp_total_h + 40.0f
                        : bt_action_y + bt_action_h + 50.0f;
                    constexpr float bt_row_h       = 110.0f;
                    constexpr float bt_row_gap     = 18.0f;
                    constexpr float bt_row_pad_x   = 80.0f;


                    auto bt_pulse = [&](std::chrono::steady_clock::time_point until) {
                        if (until == std::chrono::steady_clock::time_point{}) return 1.0f;
                        float t = std::chrono::duration<float>(now.time_since_epoch()).count();
                        float s = 0.5f + 0.5f * std::sin(t * 9.4f);
                        return 0.65f + 0.35f * s;
                    };


                    {
                        float pm = bt_pulse(bt_scan_pending_until);
                        huskyfe::Color c{ 0.30f * pm, 0.60f * pm, 0.95f * pm, opacity };
                        if (bt_pressed == 1) { c.r *= 0.78f; c.g *= 0.78f; c.b *= 0.78f; }
                        huskyfe::Color cb{ c.r * 0.45f, c.g * 0.45f, c.b * 0.45f, c.a };
                        renderer.draw_rect_gradient(offset + sub_inner_x + bt_scan_x,
                                                    bt_action_y, bt_action_w, bt_action_h,
                                                    c, cb, bt_action_h * 0.5f);
                    }


                    {
                        float pm = bt_pulse(bt_power_pending_until);
                        huskyfe::Color c = bt_status.powered
                            ? huskyfe::Color{ 0.85f * pm, 0.30f * pm, 0.30f * pm, opacity }
                            : huskyfe::Color{ 0.30f * pm, 0.80f * pm, 0.45f * pm, opacity };
                        if (bt_pressed == 2) { c.r *= 0.78f; c.g *= 0.78f; c.b *= 0.78f; }
                        huskyfe::Color cb{ c.r * 0.45f, c.g * 0.45f, c.b * 0.45f, c.a };
                        renderer.draw_rect_gradient(offset + sub_inner_x + bt_power_x,
                                                    bt_action_y, bt_action_w, bt_action_h,
                                                    c, cb, bt_action_h * 0.5f);
                    }


                    if (bt_show_adp) {
                        for (size_t i = 0; i < bt_adapters.size(); i++) {
                            const auto& a = bt_adapters[i];
                            float ry = bt_adp_y0 + i * (bt_adp_row_h + bt_adp_row_gap);
                            huskyfe::Color top_c = a.active
                                ? huskyfe::Color{ 0.20f, 0.42f, 0.30f, opacity }
                                : huskyfe::Color{ 0.18f, 0.18f, 0.22f, opacity };
                            if (bt_pressed == 200 + (int)i) {
                                top_c.r *= 0.78f; top_c.g *= 0.78f; top_c.b *= 0.78f;
                            }
                            huskyfe::Color bot_c{ top_c.r * 0.45f, top_c.g * 0.45f,
                                                  top_c.b * 0.45f, top_c.a };
                            renderer.draw_rect_gradient(offset + sub_inner_x + bt_adp_pad_x, ry,
                                                        sw - 2.0f * bt_adp_pad_x, bt_adp_row_h,
                                                        top_c, bot_c, 22.0f);
                        }
                    }


                    for (size_t i = 0; i < bt_devices.size(); i++) {
                        const auto& d = bt_devices[i];
                        float ry = bt_list_y0 + i * (bt_row_h + bt_row_gap);
                        huskyfe::Color top_c = d.connected
                            ? huskyfe::Color{ 0.20f, 0.42f, 0.30f, opacity }
                            : d.paired
                                ? huskyfe::Color{ 0.22f, 0.26f, 0.34f, opacity }
                                : huskyfe::Color{ 0.18f, 0.18f, 0.22f, opacity };
                        if (bt_pressed == 100 + (int)i) {
                            top_c.r *= 0.78f; top_c.g *= 0.78f; top_c.b *= 0.78f;
                        }
                        huskyfe::Color bot_c{ top_c.r * 0.45f, top_c.g * 0.45f, top_c.b * 0.45f, top_c.a };
                        renderer.draw_rect_gradient(offset + sub_inner_x + bt_row_pad_x, ry,
                                                    sw - 2.0f * bt_row_pad_x, bt_row_h,
                                                    top_c, bot_c, 28.0f);


                        if (d.rssi != 0) {
                            int strength = 1;
                            if      (d.rssi >= -55) strength = 4;
                            else if (d.rssi >= -65) strength = 3;
                            else if (d.rssi >= -75) strength = 2;
                            else                    strength = 1;
                            const float bars_x  = sw - bt_row_pad_x - 32.0f - 4 * (10.0f + 6.0f);
                            const float bars_cy = ry + bt_row_h * 0.5f;
                            for (int b = 0; b < 4; b++) {
                                float bh = 14.0f + b * 9.0f;
                                float bx = bars_x + b * 16.0f;
                                float by = bars_cy + 22.0f - bh;
                                huskyfe::Color bc = (b < strength)
                                    ? huskyfe::Color{ 1.0f, 1.0f, 1.0f, opacity }
                                    : huskyfe::Color{ 1.0f, 1.0f, 1.0f, opacity * 0.18f };
                                renderer.draw_rect(offset + sub_inner_x + bx, by, 10.0f, bh, bc, 3.0f);
                            }
                        }
                    }
                }


                if (settings_sub == SubPage::BRIGHTNESS) {
                    constexpr float slider_w = 900.0f;
                    constexpr float slider_h = 22.0f;
                    const     float slider_x = (sw - slider_w) * 0.5f;
                    const     float slider_y = top_safe + header_h + 360.0f;
                    const     int   lo       = brightness_min();
                    const     float span     = (float)(brightness_max - lo);
                    const     float t        = span > 0.0f
                                                ? (float)(brightness_val - lo) / span
                                                : 0.0f;
                    const     float fill_w   = slider_w * std::clamp(t, 0.0f, 1.0f);
                    constexpr float thumb_d  = 60.0f;
                    const     float thumb_cx = slider_x + fill_w;
                    const     float thumb_cy = slider_y + slider_h * 0.5f;


                    renderer.draw_rect(offset + sub_inner_x + slider_x, slider_y,
                                       slider_w, slider_h,
                                       { 0.22f, 0.22f, 0.26f, opacity }, slider_h * 0.5f);

                    renderer.draw_rect(offset + sub_inner_x + slider_x, slider_y,
                                       fill_w, slider_h,
                                       { 0.95f, 0.95f, 0.97f, opacity }, slider_h * 0.5f);

                    renderer.draw_rect(offset + sub_inner_x + thumb_cx - thumb_d * 0.5f,
                                       thumb_cy - thumb_d * 0.5f,
                                       thumb_d, thumb_d,
                                       { 1.0f, 1.0f, 1.0f, opacity }, thumb_d * 0.5f);
                }


                if (settings_sub == SubPage::THEME) {
                    constexpr float th_row_h   = 92.0f;
                    constexpr float th_row_gap = 12.0f;
                    constexpr float th_pad_x   = 80.0f;
                    constexpr float th_sec_gap = 60.0f;
                    constexpr float th_hdr_h   = 70.0f;
                    const     float th_y0      = top_safe + header_h + 60.0f;

                    auto draw_section = [&](float y0, const std::vector<ThemeOption>& opts,
                                            const std::string& current, int press_base) {
                        for (size_t i = 0; i < opts.size(); i++) {
                            float ry  = y0 + (float)i * (th_row_h + th_row_gap);
                            bool sel  = opts[i].value == current;
                            huskyfe::Color top_c = sel
                                ? huskyfe::Color{ 0.20f, 0.42f, 0.30f, opacity }
                                : huskyfe::Color{ 0.18f, 0.18f, 0.22f, opacity };
                            if (theme_pressed == press_base + (int)i) {
                                top_c.r *= 0.78f; top_c.g *= 0.78f; top_c.b *= 0.78f;
                            }
                            huskyfe::Color bot_c{ top_c.r * 0.45f, top_c.g * 0.45f,
                                                  top_c.b * 0.45f, top_c.a };
                            renderer.draw_rect_gradient(offset + sub_inner_x + th_pad_x, ry,
                                                        sw - 2.0f * th_pad_x, th_row_h,
                                                        top_c, bot_c, 24.0f);
                        }
                    };
                    const float bg_y0     = th_y0 + th_hdr_h;
                    const float bg_h      = (float)bg_options.size()   * (th_row_h + th_row_gap);
                    const float iris_y0   = bg_y0 + bg_h + th_sec_gap + th_hdr_h;
                    const float iris_h    = (float)iris_options.size() * (th_row_h + th_row_gap);
                    const float unlock_y0 = iris_y0 + iris_h + th_sec_gap + th_hdr_h;
                    draw_section(bg_y0,     bg_options,   bg.current_shader(), 1000);
                    draw_section(iris_y0,   iris_options, iris_choice,         2000);
                    draw_section(unlock_y0, iris_options, unlock_choice,       3000);
                }


                if (settings_sub == SubPage::GENERAL) {
                    constexpr float gp_sw_w   = 240.0f;
                    constexpr float gp_sw_h   = 110.0f;
                    constexpr float gp_sw_pad =   8.0f;
                    constexpr float gp_thumb_d = gp_sw_h - 2.0f * gp_sw_pad;
                    const     float gp_sw_x   = (sw - gp_sw_w) * 0.5f;
                    const     float gp_y0     = top_safe + header_h + 80.0f;
                    constexpr float gp_row_dy = 230.0f;

                    auto draw_switch = [&](float sy, bool on,
                                           const huskyfe::Color& on_color,
                                           bool pressed) {
                        huskyfe::Color c = on
                            ? on_color
                            : huskyfe::Color{ 0.30f, 0.30f, 0.34f, opacity };
                        if (pressed) { c.r *= 0.78f; c.g *= 0.78f; c.b *= 0.78f; }
                        huskyfe::Color cb{ c.r * 0.55f, c.g * 0.55f,
                                           c.b * 0.55f, c.a };
                        renderer.draw_rect_gradient(offset + sub_inner_x + gp_sw_x,
                                                    sy, gp_sw_w, gp_sw_h,
                                                    c, cb, gp_sw_h * 0.5f);
                        const float thumb_y = sy + gp_sw_pad;
                        const float thumb_x = on
                            ? gp_sw_x + gp_sw_w - gp_sw_pad - gp_thumb_d
                            : gp_sw_x + gp_sw_pad;
                        renderer.draw_rect(offset + sub_inner_x + thumb_x,
                                           thumb_y, gp_thumb_d, gp_thumb_d,
                                           { 1.0f, 1.0f, 1.0f, opacity },
                                           gp_thumb_d * 0.5f);
                    };

                    const huskyfe::Color c_green { 0.30f, 0.80f, 0.45f, opacity };
                    const huskyfe::Color c_purple{ 0.55f, 0.30f, 0.80f, opacity };

                    draw_switch(gp_y0 + 0.0f * gp_row_dy, battery_saver,  c_green,  haptic_pressed == 1);
                    draw_switch(gp_y0 + 1.0f * gp_row_dy, dnd,            c_purple, haptic_pressed == 2);
                    draw_switch(gp_y0 + 2.0f * gp_row_dy, haptic_enabled, c_green,  haptic_pressed == 3);


                    constexpr float gp_test_w = 440.0f;
                    constexpr float gp_test_h = 110.0f;
                    const     float gp_test_x = (sw - gp_test_w) * 0.5f;
                    const     float gp_test_y = gp_y0 + 3.0f * gp_row_dy;
                    {
                        huskyfe::Color c{ 0.30f, 0.60f, 0.95f, opacity };
                        if (haptic_pressed == 4) {
                            c.r *= 0.78f; c.g *= 0.78f; c.b *= 0.78f;
                        }
                        huskyfe::Color cb{ c.r * 0.45f, c.g * 0.45f,
                                           c.b * 0.45f, c.a };
                        renderer.draw_rect_gradient(offset + sub_inner_x + gp_test_x,
                                                    gp_test_y, gp_test_w, gp_test_h,
                                                    c, cb, gp_test_h * 0.5f);
                    }
                }

            }
            renderer.flush();


            text.begin(renderer.xform_data());
            const huskyfe::Color title_fg{ 1.0f, 1.0f, 1.0f, opacity };
            const huskyfe::Color dim_fg  { 0.65f, 0.65f, 0.72f, opacity };

            const char* chev = "<";
            float cw = text.measure_width(chev);
            float cy = back_y + back_size * 0.5f + text.ascent() * 0.32f;
            float ty = top_safe + header_h * 0.5f + text.ascent() * 0.32f;


            if (draw_list) {
                const char* title = "Settings";
                float tw = text.measure_width(title);
                text.draw(offset + list_inner_x + (sw - tw) * 0.5f, ty, title, title_fg);
                text.draw(offset + list_inner_x + back_x + (back_size - cw) * 0.5f,
                          cy, chev, title_fg);
                for (int i = 0; i < settings_row_count; i++) {
                    float ry = row_first_y + i * (row_h + row_gap);
                    float by = ry + row_h * 0.5f + text.ascent() * 0.32f;
                    text.draw(offset + list_inner_x + row_pad_x + 48.0f, by,
                              settings_rows[i], title_fg);
                }
            }


            if (draw_sub) {
                const char* title = (settings_sub == SubPage::ABOUT)      ? "About"
                                  : (settings_sub == SubPage::BRIGHTNESS) ? "Brightness"
                                  : (settings_sub == SubPage::DATETIME)   ? "Date & Time"
                                  : (settings_sub == SubPage::WIFI)       ? "Wi-Fi"
                                  : (settings_sub == SubPage::BLUETOOTH)  ? "Bluetooth"
                                  : (settings_sub == SubPage::THEME)      ? "Theme"
                                  : (settings_sub == SubPage::GENERAL)    ? "General"
                                                                          : "";
                float tw = text.measure_width(title);
                text.draw(offset + sub_inner_x + (sw - tw) * 0.5f, ty, title, title_fg);
                text.draw(offset + sub_inner_x + back_x + (back_size - cw) * 0.5f,
                          cy, chev, title_fg);
            }
            text.end();


            if (draw_sub && settings_sub == SubPage::ABOUT) {


                AboutInfo about_snap;
                { std::lock_guard<std::mutex> lk(about_mu); about_snap = about; }
                struct Row { const char* label; const std::string* value; };
                std::string ipv4_str = huskyfe::status::read().ipv4.empty()
                                            ? std::string("(offline)")
                                            : huskyfe::status::read().ipv4;
                Row rows[] = {
                    { "Device",    &about_snap.hostname },
                    { "IP",        &ipv4_str            },
                    { "Storage",   &about_snap.storage  },
                    { "Kernel",    &about_snap.kernel   },
                    { "Uptime",    &about_snap.uptime   },
                    { "Mali",      &about_snap.mali     },
                };
                const int nrows  = (int)(sizeof(rows) / sizeof(rows[0]));
                const float info_first_y = top_safe + header_h + 80.0f;
                const float info_pad_x   = row_pad_x + 12.0f;
                const float info_row_h   = 110.0f;

                label_text.begin(renderer.xform_data());
                for (int i = 0; i < nrows; i++) {
                    float ry = info_first_y + i * info_row_h;
                    float lab_y = ry + label_text.ascent();
                    label_text.draw(offset + sub_inner_x + info_pad_x,
                                    lab_y, rows[i].label, dim_fg);
                }
                label_text.end();

                text.begin(renderer.xform_data());
                for (int i = 0; i < nrows; i++) {
                    float ry  = info_first_y + i * info_row_h;
                    float val_y = ry + label_text.ascent() + 8.0f + text.ascent();
                    text.draw(offset + sub_inner_x + info_pad_x,
                              val_y, rows[i].value->c_str(), title_fg);
                }
                text.end();
            }


            if (draw_sub && settings_sub == SubPage::GENERAL) {
                const     float gp_y0     = top_safe + header_h + 80.0f;
                constexpr float gp_row_dy = 230.0f;
                constexpr float gp_test_w = 440.0f;
                constexpr float gp_test_h = 110.0f;
                const     float gp_test_x = (sw - gp_test_w) * 0.5f;
                const     float gp_test_y = gp_y0 + 3.0f * gp_row_dy;

                text.begin(renderer.xform_data());
                auto label_above = [&](const char* lab, float sy) {
                    float w = text.measure_width(lab);
                    float y = sy - 30.0f + text.ascent() * 0.32f;
                    text.draw(offset + sub_inner_x + (sw - w) * 0.5f, y, lab, title_fg);
                };
                label_above("Battery saver",  gp_y0 + 0.0f * gp_row_dy);
                label_above("Do not disturb", gp_y0 + 1.0f * gp_row_dy);
                label_above("Vibration",      gp_y0 + 2.0f * gp_row_dy);
                {
                    const char* lab = "Test";
                    float w = text.measure_width(lab);
                    float y = gp_test_y + gp_test_h * 0.5f + text.ascent() * 0.32f;
                    text.draw(offset + sub_inner_x + gp_test_x + (gp_test_w - w) * 0.5f,
                              y, lab, title_fg);
                }
                text.end();
            }


            if (draw_sub && settings_sub == SubPage::BRIGHTNESS) {
                int pct = brightness_max > 0
                            ? (int)((float)brightness_val * 100.0f / (float)brightness_max + 0.5f)
                            : 0;
                char pct_buf[16];
                snprintf(pct_buf, sizeof(pct_buf), "%d%%", pct);

                const float pct_y = top_safe + header_h + 220.0f;
                huge_text.begin(renderer.xform_data());
                float pw = huge_text.measure_width(pct_buf);
                huge_text.draw(offset + sub_inner_x + (sw - pw) * 0.5f,
                               pct_y, pct_buf, title_fg);
                huge_text.end();
            }


            if (draw_sub && settings_sub == SubPage::WIFI) {
                constexpr float wifi_action_w   = 440.0f;
                constexpr float wifi_action_h   = 110.0f;
                constexpr float wifi_action_gap = 20.0f;
                const     float wifi_action_y   = top_safe + header_h + 220.0f;
                const     float wifi_refresh_x  = (sw - 2.0f * wifi_action_w - wifi_action_gap) * 0.5f;
                const     float wifi_disc_x     = wifi_refresh_x + wifi_action_w + wifi_action_gap;
                const     float wifi_list_y0    = wifi_action_y + wifi_action_h + 50.0f;
                constexpr float wifi_row_h      = 110.0f;
                constexpr float wifi_row_gap    = 18.0f;
                constexpr float wifi_row_pad_x  = 80.0f;


                char status_line[192];
                if (!connecting_ssid.empty()) {
                    snprintf(status_line, sizeof(status_line),
                             "Connecting to %s...", connecting_ssid.c_str());
                } else if (!connect_msg.empty()) {
                    snprintf(status_line, sizeof(status_line), "%s", connect_msg.c_str());
                } else if (wifi_status.connected) {
                    snprintf(status_line, sizeof(status_line),
                             "Connected to %s   %s",
                             wifi_status.ssid.c_str(), wifi_status.ipv4.c_str());
                } else {
                    snprintf(status_line, sizeof(status_line), "Not connected");
                }

                text.begin(renderer.xform_data());
                {
                    float w = text.measure_width(status_line);
                    float y = top_safe + header_h + 100.0f + text.ascent();
                    text.draw(offset + sub_inner_x + (sw - w) * 0.5f, y, status_line, title_fg);
                }


                {
                    const char* label = "Refresh";
                    float w = text.measure_width(label);
                    float y = wifi_action_y + wifi_action_h * 0.5f + text.ascent() * 0.32f;
                    text.draw(offset + sub_inner_x + wifi_refresh_x + (wifi_action_w - w) * 0.5f,
                              y, label, title_fg);
                }
                if (wifi_status.connected) {
                    const char* label = "Disconnect";
                    float w = text.measure_width(label);
                    float y = wifi_action_y + wifi_action_h * 0.5f + text.ascent() * 0.32f;
                    text.draw(offset + sub_inner_x + wifi_disc_x + (wifi_action_w - w) * 0.5f,
                              y, label, title_fg);
                }


                for (size_t i = 0; i < wifi_nets.size(); i++) {
                    const auto& n = wifi_nets[i];
                    float ry = wifi_list_y0 + i * (wifi_row_h + wifi_row_gap);
                    float by = ry + wifi_row_h * 0.5f + text.ascent() * 0.32f;
                    char ssid_disp[80];
                    snprintf(ssid_disp, sizeof(ssid_disp), "%s%s",
                             n.ssid.c_str(), n.encrypted ? " *" : "");
                    text.draw(offset + sub_inner_x + wifi_row_pad_x + 32.0f,
                              by, ssid_disp, title_fg);
                }
                text.end();
            }


            if (draw_sub && settings_sub == SubPage::BLUETOOTH) {
                constexpr float bt_action_w    = 440.0f;
                constexpr float bt_action_h    = 110.0f;
                constexpr float bt_action_gap  = 20.0f;
                const     float bt_action_y    = top_safe + header_h + 220.0f;
                const     float bt_scan_x      = (sw - 2.0f * bt_action_w - bt_action_gap) * 0.5f;
                const     float bt_power_x     = bt_scan_x + bt_action_w + bt_action_gap;
                const     bool  bt_show_adp    = bt_adapters.size() >= 2;
                constexpr float bt_adp_row_h   = 70.0f;
                constexpr float bt_adp_row_gap = 10.0f;
                constexpr float bt_adp_pad_x   = 80.0f;
                const     float bt_adp_y0      = bt_action_y + bt_action_h + 30.0f;
                const     float bt_adp_total_h = bt_show_adp
                    ? (float)bt_adapters.size() * bt_adp_row_h
                      + ((float)bt_adapters.size() - 1.0f) * bt_adp_row_gap
                    : 0.0f;
                const     float bt_list_y0     = bt_show_adp
                    ? bt_adp_y0 + bt_adp_total_h + 40.0f
                    : bt_action_y + bt_action_h + 50.0f;
                constexpr float bt_row_h       = 110.0f;
                constexpr float bt_row_gap     = 18.0f;
                constexpr float bt_row_pad_x   = 80.0f;


                char status_line[192];
                if (!bt_connecting_mac.empty()) {
                    snprintf(status_line, sizeof(status_line),
                             "Connecting to %s...", bt_connecting_name.c_str());
                } else if (!bt_msg.empty()) {
                    snprintf(status_line, sizeof(status_line), "%s", bt_msg.c_str());
                } else if (!bt_status.powered) {
                    snprintf(status_line, sizeof(status_line), "Bluetooth off");
                } else if (bt_status.connected) {
                    snprintf(status_line, sizeof(status_line),
                             "Connected to %s", bt_status.connected_name.c_str());
                } else {
                    snprintf(status_line, sizeof(status_line), "Not connected");
                }

                text.begin(renderer.xform_data());
                {
                    float w = text.measure_width(status_line);
                    float y = top_safe + header_h + 100.0f + text.ascent();
                    text.draw(offset + sub_inner_x + (sw - w) * 0.5f, y, status_line, title_fg);
                }

                {
                    const char* label = (bt_scan_pending_until != std::chrono::steady_clock::time_point{})
                                        ? "Scanning..." : "Scan";
                    float w = text.measure_width(label);
                    float y = bt_action_y + bt_action_h * 0.5f + text.ascent() * 0.32f;
                    text.draw(offset + sub_inner_x + bt_scan_x + (bt_action_w - w) * 0.5f,
                              y, label, title_fg);
                }
                {
                    const char* label;
                    if (bt_power_pending_until != std::chrono::steady_clock::time_point{})
                        label = bt_power_pending_target ? "Turning on..." : "Turning off...";
                    else
                        label = bt_status.powered ? "Turn Off" : "Turn On";
                    float w = text.measure_width(label);
                    float y = bt_action_y + bt_action_h * 0.5f + text.ascent() * 0.32f;
                    text.draw(offset + sub_inner_x + bt_power_x + (bt_action_w - w) * 0.5f,
                              y, label, title_fg);
                }

                if (bt_show_adp) {
                    for (size_t i = 0; i < bt_adapters.size(); i++) {
                        const auto& a = bt_adapters[i];
                        float ry = bt_adp_y0 + i * (bt_adp_row_h + bt_adp_row_gap);
                        float by = ry + bt_adp_row_h * 0.5f + text.ascent() * 0.32f;
                        char  buf[160];
                        const char* tag = a.active ? "  *active" : "";
                        snprintf(buf, sizeof(buf), "%s  %s%s",
                                 a.name.empty() ? a.mac.c_str() : a.name.c_str(),
                                 a.mac.c_str(), tag);
                        text.draw(offset + sub_inner_x + bt_adp_pad_x + 32.0f,
                                  by, buf, title_fg);
                    }
                }


                for (size_t i = 0; i < bt_devices.size(); i++) {
                    const auto& d = bt_devices[i];
                    float ry = bt_list_y0 + i * (bt_row_h + bt_row_gap);
                    float by = ry + bt_row_h * 0.5f + text.ascent() * 0.32f;
                    char name_disp[120];
                    const char* tag = d.connected ? "  *connected"
                                    : d.paired    ? "  *paired"
                                                  : "";
                    snprintf(name_disp, sizeof(name_disp), "%s%s",
                             d.name.c_str(), tag);
                    text.draw(offset + sub_inner_x + bt_row_pad_x + 32.0f,
                              by, name_disp, title_fg);
                }
                text.end();
            }


            if (draw_sub && settings_sub == SubPage::DATETIME) {
                char time_buf[24], date_buf[64], tz_buf[96];
                time_t tnow = time(nullptr);
                struct tm lt{};
                localtime_r(&tnow, &lt);
                strftime(time_buf, sizeof(time_buf), "%-I:%M:%S %p",       &lt);
                strftime(date_buf, sizeof(date_buf), "%A, %B %-d, %Y",     &lt);

                tz_buf[0] = 0;
                int tz_fd = open("/etc/timezone", O_RDONLY | O_CLOEXEC);
                if (tz_fd >= 0) {
                    ssize_t n = ::read(tz_fd, tz_buf, sizeof(tz_buf) - 1);
                    close(tz_fd);
                    if (n > 0) {
                        while (n > 0 && (tz_buf[n-1] == '\n' || tz_buf[n-1] == ' ')) n--;
                        tz_buf[n] = 0;
                    }
                }

                const float time_y = top_safe + header_h + 240.0f;
                const float date_y = time_y + 110.0f;
                const float tz_y   = date_y + 70.0f;

                huge_text.begin(renderer.xform_data());
                float tw2 = huge_text.measure_width(time_buf);
                huge_text.draw(offset + sub_inner_x + (sw - tw2) * 0.5f,
                               time_y, time_buf, title_fg);
                huge_text.end();

                text.begin(renderer.xform_data());
                float dw = text.measure_width(date_buf);
                text.draw(offset + sub_inner_x + (sw - dw) * 0.5f,
                          date_y, date_buf, title_fg);
                text.end();

                if (tz_buf[0]) {
                    label_text.begin(renderer.xform_data());
                    float zw = label_text.measure_width(tz_buf);
                    label_text.draw(offset + sub_inner_x + (sw - zw) * 0.5f,
                                    tz_y, tz_buf, dim_fg);
                    label_text.end();
                }
            }


            if (draw_sub && settings_sub == SubPage::THEME) {
                constexpr float th_row_h   = 92.0f;
                constexpr float th_row_gap = 12.0f;
                constexpr float th_pad_x   = 80.0f;
                constexpr float th_sec_gap = 60.0f;
                constexpr float th_hdr_h   = 70.0f;
                const     float th_y0      = top_safe + header_h + 60.0f;
                const     float bg_y0      = th_y0 + th_hdr_h;
                const     float bg_h        = (float)bg_options.size()   * (th_row_h + th_row_gap);
                const     float iris_y0     = bg_y0 + bg_h + th_sec_gap + th_hdr_h;
                const     float iris_h      = (float)iris_options.size() * (th_row_h + th_row_gap);
                const     float unlock_y0   = iris_y0 + iris_h + th_sec_gap + th_hdr_h;


                label_text.begin(renderer.xform_data());
                {
                    float by = th_y0 + label_text.ascent() + 8.0f;
                    label_text.draw(offset + sub_inner_x + th_pad_x, by, "Background", dim_fg);
                    float by2 = bg_y0 + bg_h + th_sec_gap + label_text.ascent() + 8.0f;
                    label_text.draw(offset + sub_inner_x + th_pad_x, by2, "Lock animation", dim_fg);
                    float by3 = iris_y0 + iris_h + th_sec_gap + label_text.ascent() + 8.0f;
                    label_text.draw(offset + sub_inner_x + th_pad_x, by3, "Unlock animation", dim_fg);
                }
                label_text.end();


                text.begin(renderer.xform_data());
                for (size_t i = 0; i < bg_options.size(); i++) {
                    float ry = bg_y0 + (float)i * (th_row_h + th_row_gap);
                    float by = ry + th_row_h * 0.5f + text.ascent() * 0.32f;
                    text.draw(offset + sub_inner_x + th_pad_x + 32.0f, by,
                              bg_options[i].label.c_str(), title_fg);
                }
                for (size_t i = 0; i < iris_options.size(); i++) {
                    float ry = iris_y0 + (float)i * (th_row_h + th_row_gap);
                    float by = ry + th_row_h * 0.5f + text.ascent() * 0.32f;
                    text.draw(offset + sub_inner_x + th_pad_x + 32.0f, by,
                              iris_options[i].label.c_str(), title_fg);
                }
                for (size_t i = 0; i < iris_options.size(); i++) {
                    float ry = unlock_y0 + (float)i * (th_row_h + th_row_gap);
                    float by = ry + th_row_h * 0.5f + text.ascent() * 0.32f;
                    text.draw(offset + sub_inner_x + th_pad_x + 32.0f, by,
                              iris_options[i].label.c_str(), title_fg);
                }
                text.end();
            }
        }


        bool cam_drew_this_frame = false;
        if (camera_preview_open) {
            if (!huskyfe::camera::is_ready()) huskyfe::camera::init();
            if (huskyfe::camera::is_ready()) do {
                auto h = huskyfe::camera::header();
                const uint8_t* fdata = huskyfe::camera::frame_data();
                if (!fdata) {
                    fprintf(stderr, "[cam] frame_data NULL despite is_ready\n");
                    break;
                }
                if (h.width == 0 || h.height == 0 || h.stride == 0 || h.width > 8192 || h.height > 8192) {
                    fprintf(stderr, "[cam] bad header w=%u h=%u stride=%u\n",
                            h.width, h.height, h.stride);
                    break;
                }
                if (cam_texture == 0) {
                    glGenTextures(1, &cam_texture);
                    glBindTexture(GL_TEXTURE_2D, cam_texture);
                    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
                    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
                    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
                    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
                    cam_tex_w = 0; cam_tex_h = 0;
                }
                glBindTexture(GL_TEXTURE_2D, cam_texture);


                if ((int)h.width != cam_tex_w || (int)h.height != cam_tex_h) {
                    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA,
                                 h.width, h.height, 0,
                                 GL_RGBA, GL_UNSIGNED_BYTE,
                                 fdata);
                    cam_tex_w = h.width;
                    cam_tex_h = h.height;
                    cam_last_seq = h.frame_seq;
                } else if (h.frame_seq != cam_last_seq) {
                    glPixelStorei(GL_UNPACK_ROW_LENGTH, h.stride / 4);
                    glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0,
                                    h.width, h.height,
                                    GL_RGBA, GL_UNSIGNED_BYTE,
                                    fdata);
                    glPixelStorei(GL_UNPACK_ROW_LENGTH, 0);
                    auto h2 = huskyfe::camera::header();
                    if (h2.frame_seq != h.frame_seq) {


                    } else {
                        cam_last_seq = h.frame_seq;
                    }
                }
                cam_drew_this_frame = true;
            } while (0);

            if (cam_drew_this_frame && cam_texture != 0) {
                constexpr float bottom_band_h = 480.0f;
                const     float preview_h     = sh - bottom_band_h;
                glDisable(GL_SCISSOR_TEST);
                glClearColor(0.0f, 0.0f, 0.0f, 1.0f);
                glClear(GL_COLOR_BUFFER_BIT);
                image_r.begin(renderer.xform_data(), cam_texture);
                image_r.draw(0.0f, 0.0f, sw, preview_h, 0.0f, 0.0f, 1.0f, 1.0f, 1.0f);
                image_r.end();
            } else {
                renderer.draw_rect(0.0f, 0.0f, sw, sh,
                                   { 0.05f, 0.07f, 0.09f, 1.0f }, 0.0f);
                renderer.flush();
            }
        }


        if (quick_anim.value > 0.005f) {
            float a       = std::clamp(quick_anim.value, 0.0f, 1.0f);
            float opacity = a;
            float qy      = -QUICK_H * (1.0f - a);

            constexpr float quick_brightness_w     = 1100.0f;
            constexpr float quick_brightness_h     =   22.0f;
            constexpr float quick_brightness_y_off = 700.0f;
            constexpr float quick_wifi_y_off       = 870.0f;
            constexpr float quick_wifi_h           = 130.0f;
            constexpr float quick_wifi_pad_x       =   80.0f;
            constexpr float quick_bt_y_off         = 1020.0f;
            constexpr float quick_bt_h             =  130.0f;
            constexpr float quick_flash_y_off      = 1170.0f;
            constexpr float quick_flash_h          =  130.0f;
            constexpr float quick_cam_y_off        = 1320.0f;
            constexpr float quick_cam_h            =  130.0f;


            GLuint blurred = blur.draw(bg.texture(), (int)sw, (int)sh);


            image_r.begin(renderer.xform_data(), blurred);
            image_r.draw(0.0f, qy, sw, QUICK_H,
                         0.0f, 0.0f, 1.0f, 1.0f, opacity);
            image_r.end();

            renderer.begin_pass();


            renderer.draw_rect_gradient(0.0f, qy, sw, QUICK_H,
                                        { 0.10f, 0.12f, 0.18f, 0.32f * opacity },
                                        { 0.03f, 0.05f, 0.10f, 0.42f * opacity },
                                        0.0f);


            renderer.draw_rect(0.0f, qy + QUICK_H - 2.0f, sw, 2.0f,
                               { 1.0f, 1.0f, 1.0f, 0.10f * opacity }, 0.0f);


            const float bx0 = (sw - quick_brightness_w) * 0.5f;
            const int   lo  = brightness_min();
            const float btsz = (brightness_max - lo) > 0
                                ? (float)(brightness_val - lo) / (float)(brightness_max - lo)
                                : 0.0f;
            const float bfill = quick_brightness_w * std::clamp(btsz, 0.0f, 1.0f);
            constexpr float bthumb_d = 60.0f;
            const float bthumb_cx = bx0 + bfill;
            const float bthumb_cy = qy + quick_brightness_y_off + quick_brightness_h * 0.5f;
            renderer.draw_rect(bx0, qy + quick_brightness_y_off, quick_brightness_w, quick_brightness_h,
                               { 0.22f, 0.22f, 0.26f, opacity }, quick_brightness_h * 0.5f);
            renderer.draw_rect(bx0, qy + quick_brightness_y_off, bfill, quick_brightness_h,
                               { 0.95f, 0.95f, 0.97f, opacity }, quick_brightness_h * 0.5f);
            renderer.draw_rect(bthumb_cx - bthumb_d * 0.5f, bthumb_cy - bthumb_d * 0.5f,
                               bthumb_d, bthumb_d,
                               { 1.0f, 1.0f, 1.0f, opacity }, bthumb_d * 0.5f);


            {
                huskyfe::Color top_c{ 0.18f, 0.18f, 0.22f, opacity };
                if (quick_pressed == 0) { top_c.r *= 0.78f; top_c.g *= 0.78f; top_c.b *= 0.78f; }
                huskyfe::Color bot_c{ top_c.r * 0.45f, top_c.g * 0.45f, top_c.b * 0.45f, top_c.a };
                renderer.draw_rect_gradient(quick_wifi_pad_x, qy + quick_wifi_y_off,
                                            sw - 2.0f * quick_wifi_pad_x, quick_wifi_h,
                                            top_c, bot_c, 28.0f);
            }


            {
                const auto& st_bt = huskyfe::status::read();
                huskyfe::Color top_c = st_bt.bt_powered
                    ? huskyfe::Color{ 0.18f, 0.32f, 0.55f, opacity }
                    : huskyfe::Color{ 0.18f, 0.18f, 0.22f, opacity };
                if (quick_pressed == 2) { top_c.r *= 0.78f; top_c.g *= 0.78f; top_c.b *= 0.78f; }
                huskyfe::Color bot_c{ top_c.r * 0.45f, top_c.g * 0.45f, top_c.b * 0.45f, top_c.a };
                renderer.draw_rect_gradient(quick_wifi_pad_x, qy + quick_bt_y_off,
                                            sw - 2.0f * quick_wifi_pad_x, quick_bt_h,
                                            top_c, bot_c, 28.0f);
                const float ind_d  = 56.0f;
                const float ind_cy = qy + quick_bt_y_off + quick_bt_h * 0.5f;
                const float ind_cx = sw - quick_wifi_pad_x - 36.0f - ind_d * 0.5f;
                renderer.draw_rect(ind_cx - ind_d * 0.5f, ind_cy - ind_d * 0.5f,
                                   ind_d, ind_d,
                                   st_bt.bt_powered
                                     ? huskyfe::Color{ 0.55f, 0.85f, 1.0f, opacity }
                                     : huskyfe::Color{ 0.30f, 0.30f, 0.34f, opacity },
                                   ind_d * 0.5f);
            }


            {
                huskyfe::Color top_c = flashlight_on
                    ? huskyfe::Color{ 0.55f, 0.40f, 0.10f, opacity }
                    : huskyfe::Color{ 0.18f, 0.18f, 0.22f, opacity };
                if (quick_pressed == 1 && !flash_slider_open) {
                    top_c.r *= 0.78f; top_c.g *= 0.78f; top_c.b *= 0.78f;
                }
                huskyfe::Color bot_c{ top_c.r * 0.45f, top_c.g * 0.45f, top_c.b * 0.45f, top_c.a };
                renderer.draw_rect_gradient(quick_wifi_pad_x, qy + quick_flash_y_off,
                                            sw - 2.0f * quick_wifi_pad_x, quick_flash_h,
                                            top_c, bot_c, 28.0f);

                if (flash_slider_open) {


                    constexpr float trk_h     = 16.0f;
                    const     float trk_x0    = quick_wifi_pad_x + 360.0f;
                    const     float trk_x1    = sw - quick_wifi_pad_x - 60.0f;
                    const     float trk_w     = trk_x1 - trk_x0;
                    const     float trk_y     = qy + quick_flash_y_off
                                              + quick_flash_h * 0.5f - trk_h * 0.5f;
                    const     float t01       = (float)flashlight_level / (float)0x7F;
                    const     float fill_w    = trk_w * std::clamp(t01, 0.0f, 1.0f);
                    constexpr float thumb_d   = 48.0f;
                    const     float thumb_cx  = trk_x0 + fill_w;
                    const     float thumb_cy  = trk_y + trk_h * 0.5f;

                    renderer.draw_rect(trk_x0, trk_y, trk_w, trk_h,
                                       { 0.22f, 0.22f, 0.26f, opacity }, trk_h * 0.5f);
                    renderer.draw_rect(trk_x0, trk_y, fill_w, trk_h,
                                       { 1.0f, 0.85f, 0.40f, opacity }, trk_h * 0.5f);
                    renderer.draw_rect(thumb_cx - thumb_d * 0.5f,
                                       thumb_cy - thumb_d * 0.5f,
                                       thumb_d, thumb_d,
                                       { 1.0f, 1.0f, 1.0f, opacity }, thumb_d * 0.5f);
                } else {
                    const float ind_d  = 56.0f;
                    const float ind_cy = qy + quick_flash_y_off + quick_flash_h * 0.5f;
                    const float ind_cx = sw - quick_wifi_pad_x - 36.0f - ind_d * 0.5f;
                    renderer.draw_rect(ind_cx - ind_d * 0.5f, ind_cy - ind_d * 0.5f,
                                       ind_d, ind_d,
                                       flashlight_on
                                         ? huskyfe::Color{ 1.0f, 0.85f, 0.40f, opacity }
                                         : huskyfe::Color{ 0.30f, 0.30f, 0.34f, opacity },
                                       ind_d * 0.5f);
                }
            }


            renderer.draw_rect(sw * 0.5f - 90.0f, qy + QUICK_H - 28.0f,
                               180.0f, 8.0f,
                               { 1.0f, 1.0f, 1.0f, opacity * 0.45f }, 4.0f);
            renderer.flush();


            const huskyfe::Color title_fg{ 1.0f, 1.0f, 1.0f, opacity };
            const huskyfe::Color dim_fg  { 0.7f, 0.7f, 0.78f, opacity };


            char qtime_buf[16], qdate_buf[64];
            {
                time_t tnow = time(nullptr);
                struct tm lt{};
                localtime_r(&tnow, &lt);
                strftime(qtime_buf, sizeof(qtime_buf), "%-I:%M %p",        &lt);
                strftime(qdate_buf, sizeof(qdate_buf), "%a, %b %-d  %Y",   &lt);
            }
            huge_text.begin(renderer.xform_data());
            float qt_w = huge_text.measure_width(qtime_buf);
            huge_text.draw((sw - qt_w) * 0.5f, qy + 280.0f, qtime_buf, title_fg);
            huge_text.end();

            text.begin(renderer.xform_data());
            float qd_w = text.measure_width(qdate_buf);
            text.draw((sw - qd_w) * 0.5f, qy + 380.0f, qdate_buf, dim_fg);


            char wlabel[160];
            if (wifi_status.connected)
                snprintf(wlabel, sizeof(wlabel), "Wi-Fi  •  %s", wifi_status.ssid.c_str());
            else
                snprintf(wlabel, sizeof(wlabel), "Wi-Fi  •  Not connected");
            float wb_y = qy + quick_wifi_y_off + quick_wifi_h * 0.5f + text.ascent() * 0.32f;
            text.draw(quick_wifi_pad_x + 36.0f, wb_y, wlabel, title_fg);


            char btlabel[160];
            {
                const auto& st_bt = huskyfe::status::read();
                if (!st_bt.bt_powered)
                    snprintf(btlabel, sizeof(btlabel), "Bluetooth  •  off");
                else if (st_bt.bt_connected && !bt_status.connected_name.empty())
                    snprintf(btlabel, sizeof(btlabel), "Bluetooth  •  %s",
                             bt_status.connected_name.c_str());
                else if (st_bt.bt_connected)
                    snprintf(btlabel, sizeof(btlabel), "Bluetooth  •  connected");
                else
                    snprintf(btlabel, sizeof(btlabel), "Bluetooth  •  on");
            }
            float btb_y = qy + quick_bt_y_off + quick_bt_h * 0.5f + text.ascent() * 0.32f;
            text.draw(quick_wifi_pad_x + 36.0f, btb_y, btlabel, title_fg);


            char flabel[64];
            if (flash_slider_open) {
                int pct = (int)((float)flashlight_level / (float)0x7F * 100.0f + 0.5f);
                snprintf(flabel, sizeof(flabel), "Flashlight  %d%%", pct);
            } else {
                snprintf(flabel, sizeof(flabel), "Flashlight  \xe2\x80\xa2  %s",
                         flashlight_on ? "on" : "off");
            }
            float fl_y = qy + quick_flash_y_off + quick_flash_h * 0.5f + text.ascent() * 0.32f;
            text.draw(quick_wifi_pad_x + 36.0f, fl_y, flabel, title_fg);


            char qbatt[16];
            snprintf(qbatt, sizeof(qbatt), "%d%%",
                     huskyfe::status::read().battery_pct);
            float qbw = text.measure_width(qbatt);
            text.draw(sw - quick_wifi_pad_x - qbw,
                      qy + 200.0f + text.ascent(), qbatt, title_fg);

            int pct = brightness_max > 0
                        ? (int)((float)brightness_val * 100.0f / (float)brightness_max + 0.5f)
                        : 0;
            char qpct[16];
            snprintf(qpct, sizeof(qpct), "Brightness  %d%%", pct);
            text.draw(quick_wifi_pad_x, qy + 620.0f + text.ascent(), qpct, dim_fg);
            text.end();
        }


        if (menu_anim.value > 0.005f) {
            float a       = std::clamp(menu_anim.value, 0.0f, 1.0f);
            float scale   = 0.85f + 0.15f * a;
            float opacity = a;

            renderer.begin_pass();

            renderer.draw_rect(0.0f, 0.0f, sw, sh,
                               { 0.0f, 0.0f, 0.0f, 0.30f * opacity }, 0.0f);

            float scaled_w = menu_w * scale;
            float scaled_h = menu_h * scale;
            float scaled_x = menu_x + (menu_w - scaled_w) * 0.5f;
            float scaled_y = menu_y + (menu_h - scaled_h) * 0.5f;

            renderer.draw_rect(scaled_x, scaled_y, scaled_w, scaled_h,
                               { 0.13f, 0.13f, 0.16f, 0.85f * opacity },
                               menu_radius * scale);

            for (int i = 0; i < 3; i++) {
                const ButtonRect& b = menu_buttons[i];
                float bx = scaled_x + (b.x - menu_x) * scale;
                float by = scaled_y + (b.y - menu_y) * scale;
                float bw = b.w * scale;
                float bh = b.h * scale;
                huskyfe::Color bc = btn_colors[i];
                bc.a *= opacity;
                if (i == menu_pressed_btn) {
                    bc.r *= 0.78f; bc.g *= 0.78f; bc.b *= 0.78f;
                }
                huskyfe::Color bbot = { bc.r * 0.45f, bc.g * 0.45f, bc.b * 0.45f, bc.a };
                renderer.draw_rect_gradient(bx, by, bw, bh, bc, bbot, btn_radius * scale);
            }
            renderer.flush();

            text.begin(renderer.xform_data());
            for (int i = 0; i < 3; i++) {
                const ButtonRect& b = menu_buttons[i];
                float bx = scaled_x + (b.x - menu_x) * scale;
                float by = scaled_y + (b.y - menu_y) * scale;
                float bw = b.w * scale;
                float bh = b.h * scale;
                const char* label = btn_labels[i];
                float tw = text.measure_width(label);
                float tx = bx + (bw - tw) * 0.5f;
                float ty = by + bh * 0.5f + text.ascent() * 0.32f;
                text.draw(tx, ty, label, { 1.0f, 1.0f, 1.0f, opacity });
            }
            text.end();
        }


        {
            static int last_vis = -1;
            int v = keyboard.visible() ? 1 : 0;
            if (v != last_vis) {
                fprintf(stderr,
                    "huskyfe: keyboard render path vis=%d wl_active=%d\n",
                    v, (int)wl_active);
                last_vis = v;
            }
        }
        keyboard.render(renderer, text, label_text, text);


        renderer.begin_pass();
        huskyfe::notifications::render(renderer, image_r, text, label_text);
        renderer.flush();


        if (g_fade.value > 0.005f) {
            if (!iris_prog) {
                const char* VS = R"(
precision highp float;
attribute vec2 a_pos;
void main() { gl_Position = vec4(a_pos, 0.0, 1.0); }
)";


                const char* FS = R"(
precision highp float;
precision highp int;
uniform highp vec2   u_center;
uniform highp float  u_radius;
uniform highp float  u_edge;
uniform int          u_mode;
uniform sampler2D    u_mask;
uniform highp float  u_t;
uniform highp float  u_dir;

float sd_box(vec2 p, vec2 b) {
    vec2 d = abs(p) - b;
    return length(max(d, 0.0)) + min(max(d.x, d.y), 0.0);
}
// Quilez heart: works in y-up local space scaled to ~[-1,1].
float sd_heart(vec2 p) {
    p.x = abs(p.x);
    if (p.y + p.x > 1.0)
        return sqrt(dot(p - vec2(0.25, 0.75), p - vec2(0.25, 0.75))) - sqrt(2.0)/4.0;
    return sqrt(min(dot(p - vec2(0.00, 1.00), p - vec2(0.00, 1.00)),
                    dot(p - 0.5*max(p.x + p.y, 0.0), p - 0.5*max(p.x + p.y, 0.0)))) * sign(p.x - p.y);
}
// 5-point star SDF (Quilez), n=5, inset m.
float sd_star5(vec2 p, float r, float rf) {
    const vec2 k1 = vec2(0.809016994, -0.587785252);
    const vec2 k2 = vec2(-k1.x, k1.y);
    p.x = abs(p.x);
    p -= 2.0 * max(dot(k1, p), 0.0) * k1;
    p -= 2.0 * max(dot(k2, p), 0.0) * k2;
    p.x = abs(p.x);
    p.y -= r;
    vec2 ba = rf * vec2(-k1.y, k1.x) - vec2(0.0, 1.0);
    float h = clamp(dot(p, ba) / dot(ba, ba), 0.0, r);
    return length(p - ba * h) * sign(p.y * ba.x - p.x * ba.y);
}

void main() {
    vec2 d = gl_FragCoord.xy - u_center;
    float black;
    if (u_mode == 4) {
        vec2 uv = d / u_radius * 0.5 + 0.5;
        if (uv.x < 0.0 || uv.x > 1.0 || uv.y < 0.0 || uv.y > 1.0) {
            black = 1.0;
        } else {
            // No Y flip: the panel's scanout already inverts framebuffer-Y
            // to physical-Y (same convention used by the bg pass), so
            // sampling with raw uv keeps the silhouette right-side-up.
            float a = texture2D(u_mask, uv).a;
            black = 1.0 - smoothstep(0.40, 0.60, a);
        }
    } else if (u_mode == 1) {
        float s = sd_box(d, vec2(u_radius));
        black = smoothstep(-u_edge, u_edge, s);
    } else if (u_mode == 2) {
        // Heart: gl_FragCoord is y-up but the panel scanout flips it so
        // physical y is down — invert here so the V-dip lands at the top
        // of the panel. Quilez's heart spans y∈[0,1.7] with point at y=0;
        // bias by +0.85 so the heart sits centered on u_center.
        vec2 hp;
        hp.x = d.x / u_radius;
        hp.y = -d.y / u_radius * 0.85 + 0.85;
        float s = sd_heart(hp) * u_radius;
        black = smoothstep(-u_edge, u_edge, s);
    } else if (u_mode == 3) {
        // Star: same y-flip as heart so the top vertex points up on the
        // panel. sd_star5 expects unit space; feed it normalized coords
        // and scale the returned distance back to pixels.
        vec2 sp = vec2(d.x, -d.y) / u_radius;
        float s = sd_star5(sp, 1.0, 0.45) * u_radius;
        black = smoothstep(-u_edge, u_edge, s);
    } else if (u_mode == 5) {
        float dist = length(d);
        if (u_dir < 0.5) {
            // Unlock: snap the curtain open with a clean circle.
            // No swirl, no glow — fast reveal.
            float black = smoothstep(u_radius - u_edge,
                                     u_radius + u_edge, dist);
            gl_FragColor = vec4(0.0, 0.0, 0.0, black);
            return;
        }
        // Lock: full ''Fortnite'' purple-storm black hole.
        float angle  = atan(d.y, d.x);
        float ld     = log(max(dist, 1.0));
        // Three octaves of swirl tendrils — the boundary shreds into
        // overlapping spiral hairs that read as matter being dragged in.
        float t1 = sin(angle *  5.0 + ld *  6.0 - u_t * 24.0);
        float t2 = sin(angle *  9.0 - ld *  3.0 + u_t * 14.0) * 0.55;
        float t3 = sin(angle * 17.0 + ld *  9.0 - u_t * 38.0) * 0.30;
        float warp   = (t1 + t2 + t3) * 18.0 * (1.0 - 0.3 * u_t);
        float warped = dist + warp;
        float curtain = smoothstep(u_radius - u_edge - 6.0,
                                   u_radius + u_edge + 6.0, warped);
        // Logarithmic spiral arms that wind into the singularity.
        float spiral = angle * 4.0 + ld * 3.5 - u_t * 12.0;
        float arms   = pow(0.5 + 0.5 * sin(spiral), 6.0);
        float arm_fall = 1.0 / (1.0 + dist * 0.0015);
        float arm_glow = arms * arm_fall * (0.35 + 1.4 * u_t) * curtain;
        // Accretion ring — twin halo (inner core + outer fringe) so
        // the boundary reads as a chromatic-edge violet/cyan rim.
        float ring_d = abs(warped - u_radius);
        float ring   = exp(-ring_d * 0.022) * (0.55 + 1.7 * u_t);
        float fringe = exp(-abs(warped - u_radius - 16.0) * 0.045)
                       * (0.35 + u_t);
        // Lightning crackle: hash-jittered radial bolts near the rim.
        float jitter = fract(sin(angle * 47.0 + u_t * 53.0) * 437.5453);
        float bolt   = step(0.93, jitter)
                       * exp(-ring_d * 0.08) * (0.5 + u_t);
        // Final spark — bright violet-white burst as it pinches shut.
        float spark_t = smoothstep(0.88, 0.985, u_t)
                      * (1.0 - smoothstep(0.985, 1.0, u_t));
        float spark   = spark_t * exp(-dist * 0.008);
        // Palette: deep violet -> magenta -> cyan edge, white spark.
        vec3 violet  = vec3(0.32, 0.05, 0.62);
        vec3 magenta = vec3(0.92, 0.22, 0.96);
        vec3 cyan    = vec3(0.45, 0.85, 1.00);
        vec3 white   = vec3(1.00, 0.92, 1.00);
        vec3 ring_c  = mix(violet, magenta, smoothstep(0.0, 1.0, u_t));
        vec3 col = ring_c   * ring
                 + cyan     * fringe
                 + magenta  * arm_glow * 0.8
                 + cyan     * bolt    * 1.4
                 + white    * spark   * 6.0;
        // Global blackout ramp: by u_t=1.0 every pixel must be
        // fully opaque AND fully black so the panel handoff is clean.
        // black_floor lifts alpha to 1, color_fade kills the glow so the
        // final frame isn't a strobe of leftover spark/ring color.
        float black_floor = smoothstep(0.80, 1.00, u_t);
        float color_fade  = 1.0 - smoothstep(0.92, 1.00, u_t);
        col *= color_fade;
        float a = clamp(max(curtain, black_floor)
                        + ring * 0.9   * color_fade
                        + fringe * 0.6 * color_fade
                        + arm_glow * 0.6 * color_fade
                        + bolt           * color_fade
                        + spark * 1.5    * color_fade,
                        0.0, 1.0);
        gl_FragColor = vec4(col, a);
        return;
    } else {
        // Default / mode 0: analytic circle.
        float dist = length(d);
        black = smoothstep(u_radius - u_edge, u_radius + u_edge, dist);
    }
    gl_FragColor = vec4(0.0, 0.0, 0.0, black);
}
)";
                iris_prog = huskyfe::gl::build_program(VS, FS);
                if (iris_prog) {
                    iris_loc_pos    = glGetAttribLocation (iris_prog, "a_pos");
                    iris_loc_center = glGetUniformLocation(iris_prog, "u_center");
                    iris_loc_radius = glGetUniformLocation(iris_prog, "u_radius");
                    iris_loc_edge   = glGetUniformLocation(iris_prog, "u_edge");
                    iris_loc_mode   = glGetUniformLocation(iris_prog, "u_mode");
                    iris_loc_mask   = glGetUniformLocation(iris_prog, "u_mask");
                    iris_loc_t      = glGetUniformLocation(iris_prog, "u_t");
                    iris_loc_dir    = glGetUniformLocation(iris_prog, "u_dir");
                    static const float quad[] = {
                        -1.0f, -1.0f, 1.0f, -1.0f, -1.0f, 1.0f, 1.0f, 1.0f,
                    };
                    glGenBuffers(1, &iris_vbo);
                    glBindBuffer(GL_ARRAY_BUFFER, iris_vbo);
                    glBufferData(GL_ARRAY_BUFFER, sizeof(quad),
                                 quad, GL_STATIC_DRAW);
                }
            }
            if (iris_prog) {
                float v = std::clamp(g_fade.value, 0.0f, 1.0f);
                float t = v * v * v;
                float diag = std::sqrt(sw * sw + sh * sh) * 0.5f;
                float max_radius = diag + 6.0f;
                float radius     = (1.0f - t) * max_radius;
                glEnable(GL_BLEND);
                glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
                glUseProgram(iris_prog);
                glBindBuffer(GL_ARRAY_BUFFER, iris_vbo);
                glEnableVertexAttribArray((GLuint)iris_loc_pos);
                glVertexAttribPointer((GLuint)iris_loc_pos, 2, GL_FLOAT,
                                      GL_FALSE, 0, nullptr);
                glUniform2f(iris_loc_center, sw * 0.5f, sh * 0.5f);
                glUniform1f(iris_loc_radius, radius);
                glUniform1f(iris_loc_edge,   2.0f);


                const std::string& ch      = g_fading_in ? unlock_choice : iris_choice;
                GLuint              tex_now = g_fading_in ? unlock_mask_tex : iris_mask_tex;
                int mode_now = iris_mode_int(ch);
                glUniform1i(iris_loc_mode, mode_now);
                glUniform1f(iris_loc_t, t);
                glUniform1f(iris_loc_dir, g_fading_in ? 0.0f : 1.0f);
                if (mode_now == 4 && tex_now) {
                    glActiveTexture(GL_TEXTURE0);
                    glBindTexture(GL_TEXTURE_2D, tex_now);
                    glUniform1i(iris_loc_mask, 0);
                }
                glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
                glDisableVertexAttribArray((GLuint)iris_loc_pos);
                glUseProgram(0);
                glBindBuffer(GL_ARRAY_BUFFER, 0);
            }
        }

        auto t_pre_present = std::chrono::steady_clock::now();
        glp_present();
        auto t_post_present = std::chrono::steady_clock::now();


        {
            uint32_t now_ms = (uint32_t)std::chrono::duration_cast<std::chrono::milliseconds>(
                std::chrono::steady_clock::now().time_since_epoch()).count();
            huskyfe::wlhost::send_frame_callbacks(now_ms);
        }


        {
            using msd = std::chrono::duration<double, std::milli>;
            static double sp_pre = 0, sp_dispatch = 0, sp_surface = 0,
                          sp_ui = 0, sp_present = 0, sp_post = 0;
            static int    sp_count    = 0;
            static auto   sp_window   = std::chrono::steady_clock::now();
            auto t_post_callbacks = std::chrono::steady_clock::now();
            sp_pre      += msd(t_phase_start    - t_frame_top    ).count();
            sp_dispatch += msd(t_post_dispatch  - t_phase_start  ).count();
            sp_surface  += msd(t_post_surface   - t_post_dispatch).count();
            sp_ui       += msd(t_pre_present    - t_post_surface ).count();
            sp_present  += msd(t_post_present   - t_pre_present  ).count();
            sp_post     += msd(t_post_callbacks - t_post_present ).count();
            sp_count++;
            if (std::chrono::duration<double>(t_post_callbacks - sp_window).count() >= 5.0) {
                fprintf(stderr,
                    "huskyfe: subphase pre=%.2fms dispatch=%.2fms surface=%.2fms "
                    "ui=%.2fms present=%.2fms post=%.2fms (n=%d)\n",
                    sp_pre      / sp_count,
                    sp_dispatch / sp_count,
                    sp_surface  / sp_count,
                    sp_ui       / sp_count,
                    sp_present  / sp_count,
                    sp_post     / sp_count,
                    sp_count);
                sp_window   = t_post_callbacks;
                sp_pre = sp_dispatch = sp_surface = sp_ui = sp_present = sp_post = 0;
                sp_count    = 0;
            }
        }


        static auto window_start = std::chrono::steady_clock::now();
        static double accum_render_ms = 0.0;
        static double accum_wait_ms   = 0.0;
        static int    accum_count     = 0;


        static auto t_prev_flip = std::chrono::steady_clock::now();
        auto t_render_done = std::chrono::steady_clock::now();

        bool flip_pending = true;
        if (drmModePageFlip(drm_fd, crtc_id, bufs[back].fb_id,
                            DRM_MODE_PAGE_FLIP_EVENT, &flip_pending) < 0) {
            perror("drmModePageFlip"); break;
        }


        std::swap(back, front);
        auto t_set_dmabuf_start = std::chrono::steady_clock::now();
        glp_set_output_dmabuf(bufs[back].fd, bufs[back].w, bufs[back].h,
                              bufs[back].stride, FOURCC_ABGR8888);
        {
            using msd = std::chrono::duration<double, std::milli>;
            static double sd_total = 0.0;
            static int    sd_count = 0;
            static auto   sd_window = std::chrono::steady_clock::now();
            auto t_set_dmabuf_end = std::chrono::steady_clock::now();
            sd_total += msd(t_set_dmabuf_end - t_set_dmabuf_start).count();
            sd_count++;
            if (std::chrono::duration<double>(t_set_dmabuf_end - sd_window).count() >= 5.0) {
                fprintf(stderr,
                    "huskyfe: glp_set_output_dmabuf avg=%.2fms (n=%d)\n",
                    sd_total / sd_count, sd_count);
                sd_window = t_set_dmabuf_end;
                sd_total = 0; sd_count = 0;
            }
        }

        while (flip_pending && !g_quit) {
            fd_set fds; FD_ZERO(&fds);
            FD_SET(drm_fd, &fds);
            int maxfd = drm_fd;
            int kfd = huskyfe::input_keys_fd();
            int tfd = huskyfe::input_touch_fd();
            if (kfd >= 0) { FD_SET(kfd, &fds); if (kfd > maxfd) maxfd = kfd; }
            if (tfd >= 0) { FD_SET(tfd, &fds); if (tfd > maxfd) maxfd = tfd; }
            timeval tv{ 0, 100000 };
            int rc = select(maxfd + 1, &fds, nullptr, nullptr, &tv);
            if (rc <= 0) continue;
            if (FD_ISSET(drm_fd, &fds)) drmHandleEvent(drm_fd, &evctx);
            if (kfd >= 0 && FD_ISSET(kfd, &fds)) {
                huskyfe::input_drain(kfd, [&](const huskyfe::InputEvent& e) {
                    if (e.kind == huskyfe::InputKind::PowerPressed) {
                        if (menu_open) {

                            close_menu();
                        } else {

                            pwr_down       = true;
                            pwr_long_fired = false;
                            pwr_menu_ok    = !g_fading_out && !g_fading_in;
                            pwr_down_at    = std::chrono::steady_clock::now();
                            if (g_fading_out)      pwr_tap_action = PowerTapAction::FadeIn;
                            else if (g_fading_in)  pwr_tap_action = PowerTapAction::FadeOut;
                            else                   pwr_tap_action = PowerTapAction::FadeOut;
                        }
                    } else if (e.kind == huskyfe::InputKind::PowerReleased) {
                        if (pwr_down && !pwr_long_fired) {
                            int held_ms = (int)std::chrono::duration_cast<std::chrono::milliseconds>(
                                              std::chrono::steady_clock::now() - pwr_down_at).count();
                            if (held_ms <= pwr_tap_ms) {


                                if (pwr_tap_action == PowerTapAction::FadeOut) {
                                    g_fading_in = false;
                                    g_fading_out = true;
                                    g_fade.stiffness = 24.0f;
                                    g_fade.damping = 10.0f;
                                    g_fade.set(1.0f);
                                    buzz_lock_blackhole();
                                } else if (pwr_tap_action == PowerTapAction::FadeIn) {
                                    g_fading_out = false;
                                    g_fading_in = true;
                                    g_fade.stiffness = 24.0f;
                                    g_fade.damping = 10.0f;
                                    g_fade.set(0.0f);
                                    buzz_unlock_blackhole();
                                }
                            } else if (pwr_menu_ok) {

                                open_menu();
                            }
                        }
                        pwr_down = false;
                        pwr_long_fired = false;
                        pwr_menu_ok = false;
                        pwr_tap_action = PowerTapAction::None;
                    } else if (e.kind == huskyfe::InputKind::VolUp) {
                        fprintf(stderr, "huskyfe: vol+\n");
                    } else if (e.kind == huskyfe::InputKind::VolDown) {
                        fprintf(stderr, "huskyfe: vol-\n");
                    }
                });
            }
            if (tfd >= 0 && FD_ISSET(tfd, &fds)) {
                bool any_touch_event = false;
                huskyfe::input_drain(tfd, [&](const huskyfe::InputEvent& e) {


                    if (e.kind == huskyfe::InputKind::TouchDown) {
                        if (huskyfe::notifications::on_touch_down(e.x, e.y)) return;
                    } else if (e.kind == huskyfe::InputKind::TouchUp) {
                        if (huskyfe::notifications::on_touch_up(e.x, e.y)) return;
                    }


                    if (camera_preview_open && e.kind == huskyfe::InputKind::TouchDown) {
                        camera_preview_open = false;
                        return;
                    }


                    if (keyboard.visible()) {
                        if      (e.kind == huskyfe::InputKind::TouchDown) keyboard.on_touch_down(e.x, e.y);
                        else if (e.kind == huskyfe::InputKind::TouchMove) keyboard.on_touch_move(e.x, e.y);
                        else if (e.kind == huskyfe::InputKind::TouchUp)   keyboard.on_touch_up  (e.x, e.y);
                        return;
                    }


                    static int bottom_swipe_start_x = -1;
                    static int bottom_swipe_start_y = -1;
                    const bool wl_owns_input =
                        huskyfe::wlhost::has_active_surface()
                        && !menu_open && !quick_open && !ctx_menu_open
                        && view == View::LAUNCHER
                        && view_anim.value < 0.005f;
                    if (wl_owns_input) {
                        if (e.kind == huskyfe::InputKind::TouchDown
                            && (float)e.y > (float)bufs[back].h - 80.0f) {
                            bottom_swipe_start_x = e.x;
                            bottom_swipe_start_y = e.y;
                            return;
                        }
                        if (bottom_swipe_start_y >= 0) {
                            if (e.kind == huskyfe::InputKind::TouchUp) {
                                int dy = e.y - bottom_swipe_start_y;
                                int dx = std::abs(e.x - bottom_swipe_start_x);
                                if (dy < -240 && dx < 220) {
                                    fprintf(stderr, "huskyfe: dismiss app via swipe-up\n");
                                    huskyfe::wlhost::unfocus();
                                }
                                bottom_swipe_start_x = -1;
                                bottom_swipe_start_y = -1;
                            }
                            return;
                        }
                    } else {
                        bottom_swipe_start_x = -1;
                        bottom_swipe_start_y = -1;
                    }


                    static bool slot_routed_to_app[16] = {0};
                    if (wl_owns_input) {
                        const int slot_idx = (e.slot >= 0 && e.slot < 16) ? e.slot : 0;
                        if (e.kind == huskyfe::InputKind::TouchDown)
                            slot_routed_to_app[slot_idx] = (e.y >= 80);
                        if (slot_routed_to_app[slot_idx]) {
                            uint32_t t = (uint32_t)std::chrono::duration_cast<std::chrono::milliseconds>(
                                std::chrono::steady_clock::now().time_since_epoch()).count();
                            if (e.kind == huskyfe::InputKind::TouchDown) {
                                huskyfe::wlhost::touch_down(t, e.slot, (float)e.x, (float)e.y);


                                if (huskyfe::wlhost::text_input_wanted()
                                    && !keyboard.visible()) {
                                    keyboard.show_live([](char c) {
                                        uint32_t tt = (uint32_t)std::chrono::duration_cast<std::chrono::milliseconds>(
                                            std::chrono::steady_clock::now().time_since_epoch()).count();
                                        huskyfe::wlhost::keyboard_send_char(tt, c);
                                    });
                                }
                            }
                            else if (e.kind == huskyfe::InputKind::TouchMove)
                                huskyfe::wlhost::touch_motion(t, e.slot, (float)e.x, (float)e.y);
                            else if (e.kind == huskyfe::InputKind::TouchUp) {
                                huskyfe::wlhost::touch_up(t, e.slot);
                                slot_routed_to_app[slot_idx] = false;
                            }
                            any_touch_event = true;
                            return;
                        }
                    }


                    constexpr float quick_brightness_w     = 1100.0f;
                    constexpr float quick_brightness_h     =   22.0f;
                    constexpr float quick_brightness_y_off = 700.0f;
                    constexpr float quick_wifi_y_off       = 870.0f;
                    constexpr float quick_wifi_h           = 130.0f;
                    constexpr float quick_bt_y_off         = 1020.0f;
                    constexpr float quick_bt_h             =  130.0f;
                    constexpr float quick_flash_y_off      = 1170.0f;
                    constexpr float quick_flash_h          =  130.0f;
                    constexpr float quick_cam_y_off        = 1320.0f;
                    constexpr float quick_cam_h            =  130.0f;
                    constexpr float quick_wifi_pad_x       =   80.0f;

                    if (quick_open || quick_anim.value > 0.005f) {

                        const float qy_max = QUICK_H * quick_anim.value;
                        const float bx0 = (msw - quick_brightness_w) * 0.5f;
                        const float bx1 = bx0 + quick_brightness_w;
                        const float by  = quick_brightness_y_off;


                        const float flash_trk_x0 = quick_wifi_pad_x + 360.0f;
                        const float flash_trk_x1 = msw - quick_wifi_pad_x - 60.0f;
                        const float flash_trk_w  = flash_trk_x1 - flash_trk_x0;
                        auto level_at_x = [&](int x) -> int {


                            float t = std::clamp(((float)x - flash_trk_x0) / flash_trk_w, 0.0f, 1.0f);
                            int   v = (int)(t * (float)0x7F + 0.5f);
                            return std::clamp(v, 0, 0x7F);
                        };
                        auto apply_flash_level = [&](int nv) {
                            if (nv == flashlight_level) return;
                            flashlight_level = nv;
                            if (nv == 0) {
                                if (flashlight_on && huskyfe::flashlight::set(false))
                                    flashlight_on = false;
                            } else if (!flashlight_on) {
                                if (huskyfe::flashlight::set(true, nv))
                                    flashlight_on = true;
                            } else {
                                huskyfe::flashlight::set_level(nv);
                            }
                        };
                        auto in_flash_row = [&](int y) {
                            return (float)y >= quick_flash_y_off
                                && (float)y <  quick_flash_y_off + quick_flash_h;
                        };

                        if (e.kind == huskyfe::InputKind::TouchDown) {

                            if ((float)e.y > qy_max) {
                                flash_slider_open = false;
                                close_quick_settings();
                                return;
                            }


                            if ((float)e.x >= bx0 - 60.0f && (float)e.x <= bx1 + 60.0f
                             && (float)e.y >= by   - 70.0f && (float)e.y <= by   + quick_brightness_h + 70.0f) {
                                dragging_quick_brightness = true;
                                float t = std::clamp(((float)e.x - bx0) / quick_brightness_w, 0.0f, 1.0f);
                                int lo = brightness_min();
                                save_brightness((int)((float)lo + t * (float)(brightness_max - lo) + 0.5f));
                                return;
                            }


                            if (flash_slider_open) {
                                if (in_flash_row(e.y)) {
                                    dragging_flash_level = true;
                                    apply_flash_level(level_at_x(e.x));
                                } else {
                                    flash_slider_open = false;
                                    save_flashlight_level();
                                }
                                return;
                            }


                            if ((float)e.y >= quick_wifi_y_off && (float)e.y < quick_wifi_y_off + quick_wifi_h) {
                                quick_pressed = 0;
                                return;
                            }

                            if ((float)e.y >= quick_bt_y_off && (float)e.y < quick_bt_y_off + quick_bt_h) {
                                quick_pressed = 2;
                                return;
                            }

                            if (in_flash_row(e.y)) {
                                quick_pressed = 1;
                                quick_flash_press_at = std::chrono::steady_clock::now();
                                return;
                            }
                            return;
                        } else if (e.kind == huskyfe::InputKind::TouchMove) {
                            if (dragging_quick_brightness) {
                                float t = std::clamp(((float)e.x - bx0) / quick_brightness_w, 0.0f, 1.0f);
                                int lo = brightness_min();
                                save_brightness((int)((float)lo + t * (float)(brightness_max - lo) + 0.5f));
                            } else if (dragging_flash_level) {
                                apply_flash_level(level_at_x(e.x));
                            }
                            return;
                        } else if (e.kind == huskyfe::InputKind::TouchUp) {
                            if (dragging_flash_level) {
                                save_flashlight_level();
                                dragging_flash_level = false;
                                quick_pressed = -1;
                                return;
                            }
                            if (quick_pressed == 0
                             && (float)e.y >= quick_wifi_y_off
                             && (float)e.y < quick_wifi_y_off + quick_wifi_h) {


                                close_quick_settings();
                                view = View::SETTINGS;
                                view_anim.stiffness = 220.0f; view_anim.damping = 31.0f;
                                view_anim.set(1.0f);
                                open_sub(SubPage::WIFI);
                            } else if (quick_pressed == 1 && in_flash_row(e.y)) {


                                int held_ms = (int)std::chrono::duration_cast<std::chrono::milliseconds>(
                                    std::chrono::steady_clock::now() - quick_flash_press_at).count();
                                if (held_ms >= 500) {
                                    flash_slider_open = true;


                                    if (!flashlight_on && flashlight_level > 0
                                        && huskyfe::flashlight::set(true, flashlight_level)) {
                                        flashlight_on = true;
                                    }
                                } else {
                                    bool target = !flashlight_on;
                                    if (huskyfe::flashlight::set(target, flashlight_level))
                                        flashlight_on = target;
                                }
                            } else if (quick_pressed == 2
                             && (float)e.y >= quick_bt_y_off
                             && (float)e.y < quick_bt_y_off + quick_bt_h) {


                                bool turn_on = !huskyfe::status::read().bt_powered;
                                std::thread([turn_on]() {
                                    if (turn_on) huskyfe::bluetooth::power_on();
                                    else         huskyfe::bluetooth::power_off();
                                }).detach();
                            }
                            quick_pressed = -1;
                            dragging_quick_brightness = false;
                            return;
                        }
                    }


                    {
                        const bool home_eligible =
                            !menu_open && !ctx_menu_open && !quick_open
                            && view == View::LAUNCHER
                            && view_anim.value < 0.005f
                            && !wl_active;
                        constexpr int   PAGE_THRESH_PX  = 240;
                        constexpr float PAGE_DRAG_DENOM = 1344.0f;
                        if (home_eligible) {
                            if (e.kind == huskyfe::InputKind::TouchDown
                                && e.y >= 90 && e.y < (int)bufs[back].h - 80) {
                                home_drag_start_x = e.x;
                                home_drag_start_y = e.y;
                                page_anim_at_drag_start = page_anim.value;
                                dragging_home_page = false;


                            } else if (e.kind == huskyfe::InputKind::TouchMove
                                       && home_drag_start_x >= 0) {
                                int dx = e.x - home_drag_start_x;
                                int dy = e.y - home_drag_start_y;
                                if (!dragging_home_page) {


                                    if (std::abs(dx) > 60 && std::abs(dx) > std::abs(dy) * 2) {
                                        dragging_home_page = true;

                                        if (hit_cell >= 0) {
                                            cell_scales[hit_cell].set(1.0f);
                                            hit_cell = -1;
                                        }
                                    }
                                }
                                if (dragging_home_page) {


                                    float v = page_anim_at_drag_start
                                            + (-(float)dx) / PAGE_DRAG_DENOM;
                                    page_anim.snap_to(std::clamp(v, 0.0f, 1.0f));
                                    return;
                                }
                            } else if (e.kind == huskyfe::InputKind::TouchUp
                                       && home_drag_start_x >= 0) {
                                int dx = e.x - home_drag_start_x;
                                if (dragging_home_page) {


                                    float v = page_anim.value;
                                    bool to_notif = (v > 0.5f) || (-dx > PAGE_THRESH_PX);
                                    if ((page_anim_at_drag_start < 0.5f) && (-dx > PAGE_THRESH_PX)) to_notif = true;
                                    if ((page_anim_at_drag_start > 0.5f) && ( dx > PAGE_THRESH_PX)) to_notif = false;
                                    page_anim.stiffness = 240.0f;
                                    page_anim.damping   =  31.0f;
                                    if (to_notif) {
                                        page_anim.set(1.0f);


                                        if (page_anim_at_drag_start < 0.5f) {
                                            huskyfe::notifications::clear_low();
                                        }
                                    } else {
                                        page_anim.set(0.0f);
                                    }
                                    home_drag_start_x = -1;
                                    home_drag_start_y = -1;
                                    dragging_home_page = false;
                                    return;
                                }
                                home_drag_start_x = -1;
                                home_drag_start_y = -1;
                            }
                        }
                    }


                    if (view == View::LAUNCHER && page_anim.value > 0.5f
                        && !dragging_home_page) {
                        constexpr float n_top_safe = 240.0f;
                        constexpr float n_header_h = 140.0f;
                        constexpr float n_card_w   = 1100.0f;
                        constexpr float n_card_h   = 200.0f;
                        constexpr float n_card_gap =  18.0f;
                        const float n_list_y0 = n_top_safe + n_header_h + 60.0f;
                        const float n_x0 = (msw - n_card_w) * 0.5f;
                        auto hist_now = huskyfe::notifications::history_snapshot();

                        auto hit_card = [&](int x, int y) -> int {
                            if ((float)x < n_x0 || (float)x > n_x0 + n_card_w) return -1;
                            float yy = (float)y - n_list_y0;
                            if (yy < 0) return -1;
                            int idx = (int)(yy / (n_card_h + n_card_gap));
                            float lo = idx * (n_card_h + n_card_gap);
                            if (yy < lo + n_card_h && idx >= 0 && idx < (int)hist_now.size())
                                return idx;
                            return -1;
                        };

                        if (e.kind == huskyfe::InputKind::TouchDown) {
                            notif_pressed_idx = hit_card(e.x, e.y);
                            if (notif_pressed_idx >= 0) return;
                        } else if (e.kind == huskyfe::InputKind::TouchUp) {
                            if (notif_pressed_idx >= 0) {
                                int up = hit_card(e.x, e.y);
                                if (up == notif_pressed_idx
                                    && (size_t)up < hist_now.size()) {
                                    huskyfe::notifications::clear_one(hist_now[up].id);
                                }
                                notif_pressed_idx = -1;
                                return;
                            }
                        }
                    }


                    if (!menu_open) {
                        if (e.kind == huskyfe::InputKind::TouchDown && e.y < 90) {
                            edge_swipe_start_x = e.x;
                            edge_swipe_start_y = e.y;
                            return;
                        }
                        if (edge_swipe_start_y >= 0) {
                            if (e.kind == huskyfe::InputKind::TouchUp) {
                                int dy = e.y - edge_swipe_start_y;
                                int dx = std::abs(e.x - edge_swipe_start_x);
                                if (dy > 280 && dx < 220) open_quick_settings();
                                edge_swipe_start_y = -1;
                                edge_swipe_start_x = -1;
                            }
                            return;
                        }
                    }

                    if (menu_open) {


                        if (e.kind == huskyfe::InputKind::TouchDown) {
                            int btn = menu_hit(e.x, e.y);
                            if (btn >= 0) {
                                menu_pressed_btn = btn;
                            } else if (!in_menu(e.x, e.y)) {

                                close_menu();
                            }
                        } else if (e.kind == huskyfe::InputKind::TouchUp) {
                            if (menu_pressed_btn >= 0) {
                                int btn = menu_hit(e.x, e.y);
                                if (btn == menu_pressed_btn) fire_menu_action(btn);
                                menu_pressed_btn = -1;
                                close_menu();
                            }
                        }
                        return;
                    }

                    if (view == View::SETTINGS) {

                        constexpr float top_safe     = 240.0f;
                        constexpr float header_h     = 140.0f;
                        constexpr float back_size    = 110.0f;
                        constexpr float back_x       = 60.0f;
                        const     float back_y       = top_safe + (header_h - back_size) * 0.5f;
                        constexpr float row_h        = 140.0f;
                        constexpr float row_pad_x    = 80.0f;
                        const     float row_first_y  = top_safe + header_h + 80.0f;
                        constexpr float row_gap      = 22.0f;

                        auto in_back = [&](int x, int y) -> bool {
                            return (float)x >= back_x && (float)x < back_x + back_size
                                && (float)y >= back_y && (float)y < back_y + back_size;
                        };
                        auto hit_row = [&](int x, int y) -> int {
                            if ((float)x < row_pad_x || (float)x >= msw - row_pad_x) return -1;
                            float yy = (float)y - row_first_y;
                            if (yy < 0) return -1;
                            int idx = (int)(yy / (row_h + row_gap));
                            float lo = idx * (row_h + row_gap);
                            if (yy < lo + row_h && idx >= 0 && idx < settings_row_count) return idx;
                            return -1;
                        };


                        constexpr float slider_w = 900.0f;
                        constexpr float slider_h = 22.0f;
                        const     float slider_x = (msw - slider_w) * 0.5f;
                        const     float slider_y = top_safe + header_h + 360.0f;
                        constexpr float slider_grab_pad = 60.0f;
                        auto in_slider_grab = [&](int x, int y) -> bool {
                            return (float)x >= slider_x - slider_grab_pad
                                && (float)x <= slider_x + slider_w + slider_grab_pad
                                && (float)y >= slider_y - slider_grab_pad
                                && (float)y <= slider_y + slider_h + slider_grab_pad;
                        };
                        auto slider_value_at = [&](int x) -> int {
                            float t = ((float)x - slider_x) / slider_w;
                            if (t < 0.0f) t = 0.0f;
                            if (t > 1.0f) t = 1.0f;


                            int lo = brightness_min();
                            return (int)((float)lo + t * (float)(brightness_max - lo) + 0.5f);
                        };

                        const bool on_list = (settings_sub == SubPage::LIST);


                        constexpr float wifi_action_w   = 440.0f;
                        constexpr float wifi_action_h   = 110.0f;
                        constexpr float wifi_action_gap = 20.0f;
                        const     float wifi_action_y   = top_safe + header_h + 220.0f;
                        const     float wifi_refresh_x  = (msw - 2.0f * wifi_action_w - wifi_action_gap) * 0.5f;
                        const     float wifi_disc_x     = wifi_refresh_x + wifi_action_w + wifi_action_gap;
                        const     float wifi_list_y0    = wifi_action_y + wifi_action_h + 50.0f;
                        constexpr float wifi_row_h      = 110.0f;
                        constexpr float wifi_row_gap    = 18.0f;
                        constexpr float wifi_row_pad_x  = 80.0f;

                        auto wifi_in_refresh = [&](int x, int y) -> bool {
                            return (float)x >= wifi_refresh_x && (float)x < wifi_refresh_x + wifi_action_w
                                && (float)y >= wifi_action_y  && (float)y < wifi_action_y  + wifi_action_h;
                        };
                        auto wifi_in_disc = [&](int x, int y) -> bool {
                            if (!wifi_status.connected) return false;
                            return (float)x >= wifi_disc_x  && (float)x < wifi_disc_x  + wifi_action_w
                                && (float)y >= wifi_action_y && (float)y < wifi_action_y + wifi_action_h;
                        };
                        auto wifi_hit_row = [&](int x, int y) -> int {
                            if ((float)x < wifi_row_pad_x || (float)x >= msw - wifi_row_pad_x) return -1;
                            float yy = (float)y - wifi_list_y0;
                            if (yy < 0) return -1;
                            int idx = (int)(yy / (wifi_row_h + wifi_row_gap));
                            float lo = idx * (wifi_row_h + wifi_row_gap);
                            if (yy < lo + wifi_row_h && idx >= 0 && idx < (int)wifi_nets.size())
                                return idx;
                            return -1;
                        };


                        constexpr float bt_action_w    = 440.0f;
                        constexpr float bt_action_h    = 110.0f;
                        constexpr float bt_action_gap  = 20.0f;
                        const     float bt_action_y    = top_safe + header_h + 220.0f;
                        const     float bt_scan_x      = (msw - 2.0f * bt_action_w - bt_action_gap) * 0.5f;
                        const     float bt_power_x     = bt_scan_x + bt_action_w + bt_action_gap;
                        const     bool  bt_show_adp    = bt_adapters.size() >= 2;
                        constexpr float bt_adp_row_h   = 70.0f;
                        constexpr float bt_adp_row_gap = 10.0f;
                        constexpr float bt_adp_pad_x   = 80.0f;
                        const     float bt_adp_y0      = bt_action_y + bt_action_h + 30.0f;
                        const     float bt_adp_total_h = bt_show_adp
                            ? (float)bt_adapters.size() * bt_adp_row_h
                              + ((float)bt_adapters.size() - 1.0f) * bt_adp_row_gap
                            : 0.0f;
                        const     float bt_list_y0     = bt_show_adp
                            ? bt_adp_y0 + bt_adp_total_h + 40.0f
                            : bt_action_y + bt_action_h + 50.0f;
                        constexpr float bt_row_h       = 110.0f;
                        constexpr float bt_row_gap     = 18.0f;
                        constexpr float bt_row_pad_x   = 80.0f;


                        constexpr float th_row_h   = 92.0f;
                        constexpr float th_row_gap = 12.0f;
                        constexpr float th_pad_x   = 80.0f;
                        constexpr float th_sec_gap = 60.0f;
                        constexpr float th_hdr_h   = 70.0f;
                        const     float th_y0      = top_safe + header_h + 60.0f;
                        const     float bg_y0      = th_y0 + th_hdr_h;
                        const     float bg_h        = (float)bg_options.size()   * (th_row_h + th_row_gap);
                        const     float iris_y0     = bg_y0 + bg_h + th_sec_gap + th_hdr_h;
                        const     float iris_h      = (float)iris_options.size() * (th_row_h + th_row_gap);
                        const     float unlock_y0   = iris_y0 + iris_h + th_sec_gap + th_hdr_h;
                        auto theme_hit = [&](int x, int y, int& sect, int& idx) -> bool {

                            if ((float)x < th_pad_x || (float)x >= msw - th_pad_x) return false;
                            float yy = (float)y;
                            for (size_t i = 0; i < bg_options.size(); i++) {
                                float ry = bg_y0 + (float)i * (th_row_h + th_row_gap);
                                if (yy >= ry && yy < ry + th_row_h) {
                                    sect = 0; idx = (int)i; return true;
                                }
                            }
                            for (size_t i = 0; i < iris_options.size(); i++) {
                                float ry = iris_y0 + (float)i * (th_row_h + th_row_gap);
                                if (yy >= ry && yy < ry + th_row_h) {
                                    sect = 1; idx = (int)i; return true;
                                }
                            }
                            for (size_t i = 0; i < iris_options.size(); i++) {
                                float ry = unlock_y0 + (float)i * (th_row_h + th_row_gap);
                                if (yy >= ry && yy < ry + th_row_h) {
                                    sect = 2; idx = (int)i; return true;
                                }
                            }
                            return false;
                        };

                        auto bt_in_scan = [&](int x, int y) -> bool {
                            return (float)x >= bt_scan_x && (float)x < bt_scan_x + bt_action_w
                                && (float)y >= bt_action_y && (float)y < bt_action_y + bt_action_h;
                        };
                        auto bt_in_power = [&](int x, int y) -> bool {
                            return (float)x >= bt_power_x && (float)x < bt_power_x + bt_action_w
                                && (float)y >= bt_action_y && (float)y < bt_action_y + bt_action_h;
                        };
                        auto bt_hit_row = [&](int x, int y) -> int {
                            if ((float)x < bt_row_pad_x || (float)x >= msw - bt_row_pad_x) return -1;
                            float yy = (float)y - bt_list_y0;
                            if (yy < 0) return -1;
                            int idx = (int)(yy / (bt_row_h + bt_row_gap));
                            float lo = idx * (bt_row_h + bt_row_gap);
                            if (yy < lo + bt_row_h && idx >= 0 && idx < (int)bt_devices.size())
                                return idx;
                            return -1;
                        };
                        auto bt_hit_adapter = [&](int x, int y) -> int {
                            if (!bt_show_adp) return -1;
                            if ((float)x < bt_adp_pad_x || (float)x >= msw - bt_adp_pad_x) return -1;
                            float yy = (float)y - bt_adp_y0;
                            if (yy < 0) return -1;
                            int idx = (int)(yy / (bt_adp_row_h + bt_adp_row_gap));
                            float lo = idx * (bt_adp_row_h + bt_adp_row_gap);
                            if (yy < lo + bt_adp_row_h && idx >= 0 && idx < (int)bt_adapters.size())
                                return idx;
                            return -1;
                        };

                        if (e.kind == huskyfe::InputKind::TouchDown) {
                            if (settings_sub == SubPage::BRIGHTNESS) {
                                if (in_back(e.x, e.y)) {
                                    sub_pressed_back = 1;
                                } else if (in_slider_grab(e.x, e.y)) {
                                    dragging_brightness = true;
                                    save_brightness(slider_value_at(e.x));
                                }
                            } else if (settings_sub == SubPage::WIFI) {
                                if (in_back(e.x, e.y))           wifi_pressed = 0;
                                else if (wifi_in_refresh(e.x, e.y)) wifi_pressed = 1;
                                else if (wifi_in_disc(e.x, e.y))    wifi_pressed = 2;
                                else {
                                    int r = wifi_hit_row(e.x, e.y);
                                    wifi_pressed = (r >= 0) ? (100 + r) : -1;
                                }
                            } else if (settings_sub == SubPage::BLUETOOTH) {
                                if (in_back(e.x, e.y))            bt_pressed = 0;
                                else if (bt_in_scan(e.x, e.y))    bt_pressed = 1;
                                else if (bt_in_power(e.x, e.y))   bt_pressed = 2;
                                else {
                                    int a = bt_hit_adapter(e.x, e.y);
                                    if (a >= 0) {
                                        bt_pressed = 200 + a;
                                    } else {
                                        int r = bt_hit_row(e.x, e.y);
                                        bt_pressed = (r >= 0) ? (100 + r) : -1;
                                    }
                                }
                            } else if (settings_sub == SubPage::THEME) {
                                if (in_back(e.x, e.y)) {
                                    theme_pressed = 0; sub_pressed_back = 1;
                                } else {
                                    int sect = -1, idx = -1;
                                    if (theme_hit(e.x, e.y, sect, idx)) {
                                        int base = (sect == 0) ? 1000
                                                 : (sect == 1) ? 2000 : 3000;
                                        theme_pressed = base + idx;
                                    } else {
                                        theme_pressed = -1;
                                    }
                                }
                            } else if (settings_sub == SubPage::GENERAL) {
                                constexpr float gp_sw_w    = 240.0f;
                                constexpr float gp_sw_h    = 110.0f;
                                constexpr float gp_test_w  = 440.0f;
                                constexpr float gp_test_h  = 110.0f;
                                constexpr float gp_row_dy  = 230.0f;
                                const     float gp_sw_x    = (msw - gp_sw_w) * 0.5f;
                                const     float gp_y0      = top_safe + header_h + 80.0f;
                                const     float gp_test_x  = (msw - gp_test_w) * 0.5f;
                                const     float gp_test_y  = gp_y0 + 3.0f * gp_row_dy;
                                auto in_switch = [&](int x, int y, float sy) {
                                    return (float)x >= gp_sw_x - 20.0f
                                        && (float)x <  gp_sw_x + gp_sw_w + 20.0f
                                        && (float)y >= sy - 20.0f
                                        && (float)y <  sy + gp_sw_h + 20.0f;
                                };
                                auto in_test = [&](int x, int y) {
                                    return (float)x >= gp_test_x
                                        && (float)x <  gp_test_x + gp_test_w
                                        && (float)y >= gp_test_y
                                        && (float)y <  gp_test_y + gp_test_h;
                                };
                                if (in_back(e.x, e.y)) {
                                    haptic_pressed = 0; sub_pressed_back = 1;
                                } else if (in_switch(e.x, e.y, gp_y0 + 0.0f * gp_row_dy)) {
                                    haptic_pressed = 1;
                                } else if (in_switch(e.x, e.y, gp_y0 + 1.0f * gp_row_dy)) {
                                    haptic_pressed = 2;
                                } else if (in_switch(e.x, e.y, gp_y0 + 2.0f * gp_row_dy)) {
                                    haptic_pressed = 3;
                                } else if (in_test(e.x, e.y)) {
                                    haptic_pressed = 4;
                                } else {
                                    haptic_pressed = -1;
                                }
                            } else if (!on_list) {

                                sub_pressed_back = in_back(e.x, e.y) ? 1 : 0;
                            } else {
                                if (in_back(e.x, e.y))      settings_pressed_row = 0;
                                else {
                                    int r = hit_row(e.x, e.y);
                                    settings_pressed_row = (r >= 0) ? (1 + r) : -1;
                                }
                            }
                        } else if (e.kind == huskyfe::InputKind::TouchMove) {
                            if (settings_sub == SubPage::BRIGHTNESS && dragging_brightness) {
                                save_brightness(slider_value_at(e.x));
                            }
                        } else if (e.kind == huskyfe::InputKind::TouchUp) {
                            if (settings_sub == SubPage::BRIGHTNESS) {
                                if (sub_pressed_back == 1 && in_back(e.x, e.y)) close_sub();
                                sub_pressed_back = 0;
                                dragging_brightness = false;
                            } else if (settings_sub == SubPage::WIFI) {
                                if (wifi_pressed == 0 && in_back(e.x, e.y)) {
                                    close_sub();
                                } else if (wifi_pressed == 1 && wifi_in_refresh(e.x, e.y)) {
                                    std::thread([]() { huskyfe::wifi::trigger_scan(); }).detach();
                                    kick_wifi_fetch();
                                    last_wifi_refresh = std::chrono::steady_clock::now();
                                } else if (wifi_pressed == 2 && wifi_in_disc(e.x, e.y)) {
                                    std::thread([]() { huskyfe::wifi::disconnect(); }).detach();
                                    kick_wifi_fetch();
                                } else if (wifi_pressed >= 100) {
                                    int idx = wifi_pressed - 100;
                                    int hit = wifi_hit_row(e.x, e.y);
                                    if (hit == idx && idx >= 0 && idx < (int)wifi_nets.size()) {
                                        const auto& n = wifi_nets[idx];
                                        if (huskyfe::wifi::is_known(n.ssid) || !n.encrypted) {
                                            if (huskyfe::wifi::is_known(n.ssid))
                                                huskyfe::wifi::connect_saved(n.ssid);
                                            else
                                                huskyfe::wifi::connect_new(n.ssid, "");
                                            begin_connect(n.ssid);
                                            fprintf(stderr, "huskyfe: connecting to '%s'\n", n.ssid.c_str());
                                        } else {

                                            keyboard_owner = "wifi:" + n.ssid;
                                            keyboard.show("Wi-Fi password", n.ssid, "", true);
                                        }
                                    }
                                }
                                wifi_pressed = -1;
                            } else if (settings_sub == SubPage::BLUETOOTH) {
                                if (bt_pressed == 0 && in_back(e.x, e.y)) {
                                    close_sub();
                                } else if (bt_pressed == 1 && bt_in_scan(e.x, e.y)) {
                                    std::thread([]() { huskyfe::bluetooth::trigger_scan(); }).detach();

                                    bt_scan_pending_until = std::chrono::steady_clock::now()
                                                          + std::chrono::seconds(10);
                                    kick_bt_fetch(false);
                                    last_bt_refresh = std::chrono::steady_clock::now();
                                } else if (bt_pressed == 2 && bt_in_power(e.x, e.y)) {
                                    bool turn_on = !bt_status.powered;
                                    bt_power_pending_target = turn_on;
                                    bt_power_pending_until  = std::chrono::steady_clock::now()
                                                            + std::chrono::seconds(3);


                                    std::thread([&, turn_on]() {
                                        if (turn_on) huskyfe::bluetooth::power_on();
                                        else         huskyfe::bluetooth::power_off();
                                        auto s = huskyfe::bluetooth::status();
                                        auto d = huskyfe::bluetooth::devices();
                                        auto a = huskyfe::bluetooth::adapters();
                                        std::lock_guard<std::mutex> lk(async_net_mu);
                                        async_bt_status   = std::move(s);
                                        async_bt_devices  = std::move(d);
                                        async_bt_adapters = std::move(a);
                                        bt_pending = true;
                                    }).detach();


                                    last_bt_refresh = std::chrono::steady_clock::now();
                                } else if (bt_pressed >= 200) {
                                    int idx = bt_pressed - 200;
                                    int hit = bt_hit_adapter(e.x, e.y);
                                    if (hit == idx && idx >= 0 && idx < (int)bt_adapters.size()) {
                                        const auto& a = bt_adapters[idx];
                                        if (!a.active) {
                                            std::string mac = a.mac;
                                            std::thread([mac]() {
                                                huskyfe::bluetooth::set_active_adapter(mac);


                                                if (!huskyfe::bluetooth::status().powered)
                                                    huskyfe::bluetooth::power_on();
                                            }).detach();
                                            kick_bt_fetch(false);
                                            last_bt_refresh = std::chrono::steady_clock::now();
                                        }
                                    }
                                } else if (bt_pressed >= 100) {
                                    int idx = bt_pressed - 100;
                                    int hit = bt_hit_row(e.x, e.y);
                                    if (hit == idx && idx >= 0 && idx < (int)bt_devices.size()) {
                                        const auto& d = bt_devices[idx];
                                        if (d.connected) {
                                            huskyfe::bluetooth::disconnect(d.mac);
                                        } else if (d.paired) {
                                            std::string mac = d.mac, name = d.name;
                                            std::thread([mac, name]() {
                                                huskyfe::bluetooth::connect_saved(mac);
                                            }).detach();
                                            begin_bt_connect(d.mac, d.name);
                                        } else {
                                            std::string mac = d.mac, name = d.name;
                                            std::thread([mac, name]() {
                                                huskyfe::bluetooth::pair_and_connect(mac);
                                            }).detach();
                                            begin_bt_connect(d.mac, d.name);
                                        }
                                        last_bt_refresh = std::chrono::steady_clock::now()
                                                          - std::chrono::seconds(10);
                                    }
                                }
                                bt_pressed = -1;
                            } else if (settings_sub == SubPage::THEME) {
                                if (theme_pressed == 0 && in_back(e.x, e.y)) {
                                    close_sub();
                                } else if (theme_pressed >= 1000) {
                                    int sect = -1, idx = -1;
                                    if (theme_hit(e.x, e.y, sect, idx)) {
                                        int press_sect = (theme_pressed >= 3000) ? 2
                                                       : (theme_pressed >= 2000) ? 1 : 0;
                                        int press_base = (press_sect == 2) ? 3000
                                                       : (press_sect == 1) ? 2000 : 1000;
                                        int press_idx  = theme_pressed - press_base;
                                        if (sect == press_sect && idx == press_idx) {
                                            if (sect == 0 && idx >= 0
                                                && idx < (int)bg_options.size()) {
                                                const auto& v = bg_options[idx].value;
                                                std::string arg = v;
                                                if (arg != "none" && arg != "rgb"
                                                    && arg != "spheres"
                                                    && arg != "fractal"
                                                    && arg.find('/') == std::string::npos)
                                                    arg = std::string(SHADER_DIR) + "/" + arg;
                                                if (bg.set_shader(arg))
                                                    write_one_line(BG_SEL_PATH, v);
                                            } else if (sect == 1 && idx >= 0
                                                       && idx < (int)iris_options.size()) {
                                                const auto& v = iris_options[idx].value;
                                                apply_iris_choice(v);
                                                write_one_line(IRIS_SEL_PATH, iris_choice);
                                            } else if (sect == 2 && idx >= 0
                                                       && idx < (int)iris_options.size()) {
                                                const auto& v = iris_options[idx].value;
                                                apply_unlock_choice(v);
                                                write_one_line(UNLOCK_SEL_PATH, unlock_choice);
                                            }
                                        }
                                    }
                                }
                                theme_pressed = -1;
                                sub_pressed_back = 0;
                            } else if (settings_sub == SubPage::GENERAL) {
                                constexpr float gp_sw_w    = 240.0f;
                                constexpr float gp_sw_h    = 110.0f;
                                constexpr float gp_test_w  = 440.0f;
                                constexpr float gp_test_h  = 110.0f;
                                constexpr float gp_row_dy  = 230.0f;
                                const     float gp_sw_x    = (msw - gp_sw_w) * 0.5f;
                                const     float gp_y0      = top_safe + header_h + 80.0f;
                                const     float gp_test_x  = (msw - gp_test_w) * 0.5f;
                                const     float gp_test_y  = gp_y0 + 3.0f * gp_row_dy;
                                auto in_switch = [&](int x, int y, float sy) {
                                    return (float)x >= gp_sw_x - 20.0f
                                        && (float)x <  gp_sw_x + gp_sw_w + 20.0f
                                        && (float)y >= sy - 20.0f
                                        && (float)y <  sy + gp_sw_h + 20.0f;
                                };
                                auto in_test = [&](int x, int y) {
                                    return (float)x >= gp_test_x
                                        && (float)x <  gp_test_x + gp_test_w
                                        && (float)y >= gp_test_y
                                        && (float)y <  gp_test_y + gp_test_h;
                                };
                                if (haptic_pressed == 0 && in_back(e.x, e.y)) {
                                    close_sub();
                                } else if (haptic_pressed == 1
                                           && in_switch(e.x, e.y, gp_y0 + 0.0f * gp_row_dy)) {
                                    battery_saver = !battery_saver;
                                    save_battery_saver();
                                    apply_active_power_profile();
                                } else if (haptic_pressed == 2
                                           && in_switch(e.x, e.y, gp_y0 + 1.0f * gp_row_dy)) {
                                    dnd = !dnd;
                                    save_dnd();
                                    huskyfe::notifications::set_muted(dnd);
                                } else if (haptic_pressed == 3
                                           && in_switch(e.x, e.y, gp_y0 + 2.0f * gp_row_dy)) {
                                    haptic_enabled = !haptic_enabled;
                                    save_haptic_enabled();
                                } else if (haptic_pressed == 4 && in_test(e.x, e.y)) {
                                    if (huskyfe::haptics::ok())
                                        huskyfe::haptics::play(680, haptic_strength, 20,
                                                               600, haptic_strength / 8);
                                }
                                haptic_pressed = -1;
                                sub_pressed_back = 0;
                            } else if (!on_list) {
                                if (sub_pressed_back == 1 && in_back(e.x, e.y)) close_sub();
                                sub_pressed_back = 0;
                            } else {
                                if (settings_pressed_row == 0 && in_back(e.x, e.y)) {
                                    close_settings();
                                } else if (settings_pressed_row > 0) {
                                    int r = hit_row(e.x, e.y);
                                    if (r == settings_pressed_row - 1) {
                                        const char* name = settings_rows[r];
                                        fprintf(stderr, "huskyfe: settings row '%s' tapped\n", name);
                                        if      (r == 0 ) open_sub(SubPage::GENERAL);
                                        else if (r == 1 ) open_sub(SubPage::WIFI);
                                        else if (r == 2 ) open_sub(SubPage::BLUETOOTH);
                                        else if (r == 3 ) open_sub(SubPage::BRIGHTNESS);
                                        else if (r == 4 ) open_sub(SubPage::THEME);
                                        else if (r == 5 ) open_sub(SubPage::DATETIME);
                                        else if (r == 6 ) open_sub(SubPage::ABOUT);
                                    }
                                }
                                settings_pressed_row = -1;
                            }
                        }
                        return;
                    }


                    if (ctx_menu_open || ctx_menu_anim.value > 0.005f) {


                        const CellRect& cc = cells[ctx_menu_cell >= 0
                                                   ? ctx_menu_cell : 0];
                        constexpr float panel_w   = 520.0f;
                        constexpr float panel_h   = 280.0f;
                        constexpr float btn_h_loc = 100.0f;
                        const float panel_x = cc.x + cc.w * 0.5f - panel_w * 0.5f;
                        float panel_y = cc.y + cc.h + 20.0f;
                        if (panel_y + panel_h > sh - 100.0f)
                            panel_y = cc.y - panel_h - 20.0f;
                        const float btn1_y = panel_y + 30.0f;
                        const float btn2_y = btn1_y + btn_h_loc + 20.0f;
                        const float btn_x  = panel_x + 30.0f;
                        const float btn_w_loc = panel_w - 60.0f;
                        auto in_panel = [&](int x, int y) {
                            return (float)x >= panel_x && (float)x < panel_x + panel_w
                                && (float)y >= panel_y && (float)y < panel_y + panel_h;
                        };
                        auto in_btn = [&](int x, int y, float by) {
                            return (float)x >= btn_x && (float)x < btn_x + btn_w_loc
                                && (float)y >= by && (float)y < by + btn_h_loc;
                        };
                        if (e.kind == huskyfe::InputKind::TouchDown) {
                            if (!in_panel(e.x, e.y)) {
                                close_ctx_menu();
                                return;
                            }
                            if      (in_btn(e.x, e.y, btn1_y)) ctx_menu_pressed = 0;
                            else if (in_btn(e.x, e.y, btn2_y)) ctx_menu_pressed = 1;
                            return;
                        } else if (e.kind == huskyfe::InputKind::TouchUp) {
                            int p = ctx_menu_pressed;
                            ctx_menu_pressed = -1;
                            if (p == 0 && in_btn(e.x, e.y, btn1_y)) {

                                if (ctx_menu_cell > 0
                                    && ctx_menu_cell < (int)installed.size()) {
                                    auto h = find_running_for_cell(installed[ctx_menu_cell].exec);
                                    if (h) {
                                        fprintf(stderr, "huskyfe: closing '%s' via ctx menu\n",
                                                installed[ctx_menu_cell].name.c_str());
                                        huskyfe::wlhost::close(h);
                                    }
                                }
                                close_ctx_menu();
                            } else if (p == 1 && in_btn(e.x, e.y, btn2_y)) {


                                if (ctx_menu_cell > 0
                                    && ctx_menu_cell < (int)installed.size()) {
                                    const auto& a = installed[ctx_menu_cell];
                                    fprintf(stderr, "huskyfe: hiding '%s' (%s)\n",
                                            a.name.c_str(), a.desktop_path.c_str());
                                    append_hidden(a.desktop_path);
                                    installed.erase(installed.begin() + ctx_menu_cell);


                                    std::vector<std::string> names;
                                    names.reserve(installed.size());
                                    for (const auto& e2 : installed) names.push_back(e2.icon);
                                    icon_atlas.build(names, 256,
                                                     4, 6);


                                    for (auto& s : cell_scales) s.snap_to(1.0f);
                                    hit_cell = -1;
                                }
                                close_ctx_menu();
                            }
                            return;
                        }
                        return;
                    }


                    const bool launcher_input_active =
                        page_anim.value < 0.5f && !dragging_home_page;
                    if (e.kind == huskyfe::InputKind::TouchDown && launcher_input_active) {
                        int idx = hit_test(e.x, e.y);
                        if (idx >= 0) {
                            hit_cell = idx;
                            hit_cell_at = std::chrono::steady_clock::now();
                            cell_scales[idx].set(0.90f);
                        }
                    } else if (e.kind == huskyfe::InputKind::TouchUp && launcher_input_active) {
                        if (hit_cell >= 0) {
                            int held_ms = (int)std::chrono::duration_cast<std::chrono::milliseconds>(
                                std::chrono::steady_clock::now() - hit_cell_at).count();
                            cell_scales[hit_cell].set(1.0f);
                            const int idx_up = hit_cell;
                            hit_cell = -1;


                            if (held_ms >= 600 && idx_up > 0
                                && idx_up < (int)installed.size()) {
                                open_ctx_menu(idx_up);
                                return;
                            }

                            if (idx_up == 0) {
                                open_settings();
                            } else if (idx_up < (int)installed.size()) {
                                const auto& a = installed[idx_up];
                                auto existing = find_running_for_cell(a.exec);
                                if (existing) {
                                    fprintf(stderr, "huskyfe: refocusing '%s' (already running)\n",
                                            a.name.c_str());
                                    huskyfe::wlhost::focus(existing);
                                } else {
                                    fprintf(stderr, "huskyfe: launching '%s' (%s)\n",
                                            a.name.c_str(), a.exec.c_str());
                                    launch_app(a.exec);
                                }
                            }
                        }
                    }
                });
                if (any_touch_event) huskyfe::wlhost::touch_frame();
            }
        }


        huskyfe::wlhost::flush_deferred_releases();

        auto t_flip_done = std::chrono::steady_clock::now();
        accum_render_ms += std::chrono::duration<double, std::milli>(
                               t_render_done - t_prev_flip).count();
        accum_wait_ms   += std::chrono::duration<double, std::milli>(
                               t_flip_done   - t_render_done).count();
        accum_count++;
        if (std::chrono::duration<double>(t_flip_done - window_start).count() >= 5.0) {
            fprintf(stderr,
                "huskyfe: phase avg render=%.2fms wait=%.2fms total=%.2fms (n=%d)\n",
                accum_render_ms / accum_count,
                accum_wait_ms   / accum_count,
                (accum_render_ms + accum_wait_ms) / accum_count,
                accum_count);
            window_start = t_flip_done;
            accum_render_ms = 0.0;
            accum_wait_ms   = 0.0;
            accum_count = 0;
        }
        t_prev_flip = t_flip_done;


        frames++;


        {
            static auto last_t = std::chrono::steady_clock::now();
            static unsigned long long last_frames = 0;
            auto now2 = std::chrono::steady_clock::now();
            float dt = std::chrono::duration<float>(now2 - last_t).count();
            if (dt >= 5.0f) {
                fprintf(stderr,
                    "huskyfe: %.1f fps (wl_active=%d)\n",
                    (frames - last_frames) / dt, (int)wl_active);
                last_t = now2;
                last_frames = frames;
            }
        }
    }

    float secs = std::chrono::duration<float>(std::chrono::steady_clock::now() - frames_t0).count();
    fprintf(stderr, "huskyfe: %llu frames in %.2fs = %.1f fps\n",
            (unsigned long long)frames, secs, secs > 0 ? frames / secs : 0.0f);

    keyboard.shutdown();
    blur.shutdown();
    bg.shutdown();
    image_r.shutdown();
    icon_atlas.shutdown();
    huge_text.shutdown();
    label_text.shutdown();
    text.shutdown();
    renderer.shutdown();
    eglMakeCurrent(dpy, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
    eglDestroySurface(dpy, surf);
    eglDestroyContext(dpy, ctx);
    eglTerminate(dpy);


    gpu_stack_signal("CONT");

    set_cpu_governor("schedutil");
    set_perf_mon(1);
    huskyfe::wlhost::shutdown();
    about_quit.store(true, std::memory_order_relaxed);
    if (about_worker.joinable()) about_worker.join();

    huskyfe::input_close();
    drmDropMaster(drm_fd);
    if (enc) drmModeFreeEncoder(enc);
    drmModeFreeConnector(conn);
    drmModeFreeResources(res);
    close(drm_fd);
    return 0;
}
