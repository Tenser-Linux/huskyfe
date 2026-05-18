#include "Camera.h"

#include <atomic>
#include <cerrno>
#include <csignal>
#include <cstdio>
#include <cstring>
#include <fcntl.h>
#include <mutex>
#include <spawn.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

namespace huskyfe::camera {

static int           g_fd        = -1;
static void*         g_map       = nullptr;
static size_t        g_map_sz    = 0;
static uint32_t      g_last_seq  = 0;
static pid_t         g_daemon    = -1;


static std::mutex          g_op_mu;
static std::atomic<bool>   g_busy{false};

static const char* SHM_PATH = "/run/husky_camera/preview.shm";

bool init() {
    shutdown();
    g_fd = ::open(SHM_PATH, O_RDONLY | O_CLOEXEC);
    if (g_fd < 0) {

        return false;
    }
    struct stat st{};
    if (::fstat(g_fd, &st) < 0 || st.st_size < (off_t)sizeof(ShmHeader)) {
        ::close(g_fd); g_fd = -1;
        return false;
    }
    g_map_sz = (size_t)st.st_size;
    g_map = ::mmap(nullptr, g_map_sz, PROT_READ, MAP_SHARED, g_fd, 0);
    if (g_map == MAP_FAILED) {
        g_map = nullptr;
        ::close(g_fd); g_fd = -1;
        return false;
    }

    auto* h = static_cast<ShmHeader*>(g_map);
    if (h->magic != 0x48434D31u ) {
        ::munmap(g_map, g_map_sz); g_map = nullptr; g_map_sz = 0;
        ::close(g_fd); g_fd = -1;
        return false;
    }
    g_last_seq = 0;
    return true;
}

void shutdown() {
    if (g_map) { ::munmap(g_map, g_map_sz); g_map = nullptr; g_map_sz = 0; }
    if (g_fd >= 0) { ::close(g_fd); g_fd = -1; }
}

bool is_ready() { return g_map != nullptr; }

ShmHeader header() {
    ShmHeader h{};
    if (g_map) {
        __sync_synchronize();
        std::memcpy(&h, g_map, sizeof(h));
    }
    return h;
}

const uint8_t* frame_data() {
    if (!g_map) return nullptr;
    return static_cast<const uint8_t*>(g_map) + sizeof(ShmHeader);
}

uint32_t last_seen_seq() { return g_last_seq; }
void mark_seq(uint32_t s) { g_last_seq = s; }

bool is_busy() { return g_busy.load(); }

static const char* PIDFILE = "/run/husky_camera/daemon.pid";

static bool send_term_and_wait(pid_t pid, int settle_ms) {
    if (pid <= 1) return false;

    char path[64], comm[256] = {0};
    snprintf(path, sizeof(path), "/proc/%d/cmdline", pid);
    int f = ::open(path, O_RDONLY);
    if (f < 0) return false;
    ssize_t n = ::read(f, comm, sizeof(comm) - 1);
    ::close(f);
    if (n <= 0) return false;

    if (!strstr(comm, "camera_daemon")) return false;
    if (::kill(pid, SIGTERM) != 0) return false;


    for (int i = 0; i < 40; i++) {
        if (::kill(pid, 0) != 0 && errno == ESRCH) {
            ::usleep(settle_ms * 1000);
            return true;
        }
        ::usleep(50 * 1000);
    }
    ::kill(pid, SIGKILL);
    ::usleep(settle_ms * 1000);
    return true;
}

void cleanup_orphan_daemon() {


    int f = ::open(PIDFILE, O_RDONLY);
    if (f < 0) {

        return;
    }
    char buf[32] = {0};
    ssize_t n = ::read(f, buf, sizeof(buf) - 1);
    ::close(f);
    if (n <= 0) {
        ::unlink(PIDFILE);
        return;
    }
    pid_t pid = (pid_t)atoi(buf);
    if (pid <= 1) {
        ::unlink(PIDFILE);
        return;
    }

    char path[64], cmdline[256] = {0};
    snprintf(path, sizeof(path), "/proc/%d/cmdline", pid);
    int cf = ::open(path, O_RDONLY);
    if (cf < 0) {

        ::unlink(PIDFILE);
        ::unlink("/run/husky_camera/preview.shm");
        return;
    }
    ssize_t cn = ::read(cf, cmdline, sizeof(cmdline) - 1);
    ::close(cf);
    if (cn <= 0 || !strstr(cmdline, "camera_daemon")) {

        ::unlink(PIDFILE);
        ::unlink("/run/husky_camera/preview.shm");
        return;
    }


    g_daemon = pid;
}

bool spawn_daemon(const std::string& tsv_path) {
    g_busy.store(true);
    std::lock_guard<std::mutex> lk(g_op_mu);
    struct BusyClear { ~BusyClear() { g_busy.store(false); } } _bc;
    if (g_daemon > 0 && daemon_alive()) return true;
    pid_t pid;
    const char* argv[] = {
        "/usr/local/bin/camera_daemon",
        "--tsv", tsv_path.c_str(),
        "--duration", "0",
        nullptr
    };

    char* args[6];
    for (int i = 0; i < 5; i++) args[i] = const_cast<char*>(argv[i]);
    args[5] = nullptr;
    int rc = posix_spawn(&pid, args[0], nullptr, nullptr, args, nullptr);
    if (rc != 0) {
        fprintf(stderr, "camera_daemon spawn failed: %s\n", strerror(rc));
        return false;
    }
    g_daemon = pid;


    mkdir("/run/husky_camera", 0755);
    int pf = ::open(PIDFILE, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (pf >= 0) {
        char buf[32];
        int n = snprintf(buf, sizeof(buf), "%d\n", (int)pid);
        if (n > 0) (void)::write(pf, buf, n);
        ::close(pf);
    }
    return true;
}

void kill_daemon() {
    g_busy.store(true);
    std::lock_guard<std::mutex> lk(g_op_mu);
    struct BusyClear { ~BusyClear() { g_busy.store(false); } } _bc;
    if (g_daemon <= 0) return;
    ::kill(g_daemon, SIGTERM);
    int st;


    for (int i = 0; i < 40; i++) {
        if (::waitpid(g_daemon, &st, WNOHANG) == g_daemon) { g_daemon = -1; return; }
        ::usleep(50 * 1000);
    }
    ::kill(g_daemon, SIGKILL);
    ::waitpid(g_daemon, &st, 0);
    g_daemon = -1;


    ::usleep(300 * 1000);
    ::unlink(PIDFILE);
}

bool daemon_alive() {
    if (g_daemon <= 0) return false;
    int st;
    pid_t r = ::waitpid(g_daemon, &st, WNOHANG);
    if (r == 0) return true;
    if (r == g_daemon) { g_daemon = -1; return false; }
    return false;
}

}
