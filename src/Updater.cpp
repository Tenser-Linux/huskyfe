#include "Updater.h"
#include "Notifications.h"
#include "Wifi.h"

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <string>
#include <thread>

#ifndef HUSKYFE_GIT_SHA
#define HUSKYFE_GIT_SHA "unknown"
#endif

#define HUSKYFE_NOTIF_APP "huskyfe-updater"

namespace huskyfe::updater {

namespace {

std::thread             g_thread;
std::atomic<bool>       g_stop{false};
std::mutex              g_mu;
std::condition_variable g_cv;

std::string             g_remote_sha;
uint32_t                g_pending_notif_id = 0;
std::atomic<bool>       g_update_available{false};
std::atomic<bool>       g_skipped{false};
std::atomic<bool>       g_applying{false};

std::string run_cmd(const std::string& cmd) {
    std::string out;
    FILE* p = popen(cmd.c_str(), "r");
    if (!p) return out;
    char buf[1024];
    size_t n;
    while ((n = fread(buf, 1, sizeof(buf), p)) > 0) out.append(buf, n);
    pclose(p);
    return out;
}

std::string trim(const std::string& s) {
    size_t a = 0;
    while (a < s.size() && (s[a] == ' ' || s[a] == '\n' || s[a] == '\r' || s[a] == '\t')) a++;
    size_t b = s.size();
    while (b > a && (s[b-1] == ' ' || s[b-1] == '\n' || s[b-1] == '\r' || s[b-1] == '\t')) b--;
    return s.substr(a, b - a);
}

bool wait_for_network(int max_seconds) {
    for (int i = 0; i < max_seconds; i++) {
        if (g_stop.load()) return false;
        if (huskyfe::wifi::status().connected) return true;
        std::unique_lock<std::mutex> lk(g_mu);
        g_cv.wait_for(lk, std::chrono::seconds(1), []{ return g_stop.load(); });
    }
    return huskyfe::wifi::status().connected;
}

std::string fetch_remote_sha() {
    std::string cmd =
        "curl -fsS --max-time 15 "
        "-A 'huskyfe-updater' "
        "-H 'Accept: application/vnd.github.v3.sha' "
        "https://api.github.com/repos/Tenser-Linux/huskyfe/commits/main "
        "2>/dev/null";
    return trim(run_cmd(cmd));
}

void run_build_and_swap() {
    if (g_applying.exchange(true)) return;

    huskyfe::notifications::post_local(
        HUSKYFE_NOTIF_APP,
        "Updating huskyfe",
        "Cloning + building... this can take a few minutes.",
        60000);

    const char* steps =
        "set -e; "
        "rm -rf /tmp/huskyfe-update; "
        "git clone --depth=1 https://github.com/Tenser-Linux/huskyfe /tmp/huskyfe-update; "
        "cd /tmp/huskyfe-update && make -j$(nproc); "
        "install -m0755 /tmp/huskyfe-update/huskyfe /root/huskyfe/huskyfe.new; "
        "mv -f /root/huskyfe/huskyfe.new /root/huskyfe/huskyfe; "
        "rsync -a --delete /tmp/huskyfe-update/src/ /root/huskyfe/src/; "
        "cp -f /tmp/huskyfe-update/Makefile /root/huskyfe/Makefile; ";

    int rc = std::system(steps);
    if (rc != 0) {
        huskyfe::notifications::post_local(
            HUSKYFE_NOTIF_APP,
            "Update failed",
            "Build failed; keeping current binary.",
            10000);
        g_applying.store(false);
        return;
    }

    huskyfe::notifications::post_local(
        HUSKYFE_NOTIF_APP,
        "Update ready",
        "Restarting huskyfe...",
        4000);

    std::thread([]{
        std::this_thread::sleep_for(std::chrono::seconds(2));
        std::system("pkill -TERM -x huskyfe");
    }).detach();
}

void thread_main() {
    if (!wait_for_network(120)) return;
    if (g_stop.load()) return;

    std::string remote = fetch_remote_sha();
    if (remote.empty()) return;

    {
        std::lock_guard<std::mutex> lk(g_mu);
        g_remote_sha = remote;
    }

    const std::string local = HUSKYFE_GIT_SHA;
    bool newer = !remote.empty()
              && local != "unknown"
              && remote.compare(0, local.size(), local) != 0;
    if (!newer) return;

    g_update_available.store(true);

    std::string body = "Tap to install commit ";
    body += remote.substr(0, 7);
    uint32_t id = huskyfe::notifications::post_local(
        HUSKYFE_NOTIF_APP,
        "huskyfe update available",
        body,
        30000);
    {
        std::lock_guard<std::mutex> lk(g_mu);
        g_pending_notif_id = id;
    }
}

}

void start() {
    if (g_thread.joinable()) return;
    g_stop.store(false);
    g_thread = std::thread(thread_main);
}

void stop() {
    if (!g_thread.joinable()) return;
    g_stop.store(true);
    g_cv.notify_all();
    g_thread.join();
}

bool update_available() { return g_update_available.load(); }

std::string remote_sha() {
    std::lock_guard<std::mutex> lk(g_mu);
    return g_remote_sha;
}

std::string local_sha() { return HUSKYFE_GIT_SHA; }

void apply() {
    if (!g_update_available.load()) return;
    if (g_skipped.load())            return;
    std::thread([]{ run_build_and_swap(); }).detach();
}

void skip() {
    g_skipped.store(true);
    g_update_available.store(false);
}

void on_notification_tap(uint32_t id, const std::string& app_name) {
    if (app_name != HUSKYFE_NOTIF_APP) return;
    uint32_t pending = 0;
    {
        std::lock_guard<std::mutex> lk(g_mu);
        pending = g_pending_notif_id;
    }
    if (id != pending) return;
    apply();
}

}
