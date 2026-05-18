#include "Status.h"
#include "Bluetooth.h"

#include <arpa/inet.h>
#include <ifaddrs.h>
#include <netinet/in.h>
#include <sys/types.h>
#include <dirent.h>
#include <fcntl.h>
#include <unistd.h>
#include <cstdio>
#include <cstring>
#include <chrono>
#include <thread>

namespace huskyfe::status {
namespace {

Snapshot g_snap;
std::chrono::steady_clock::time_point g_last_refresh{};
constexpr int kRefreshMs = 500;

std::string slurp(const char* path) {
    int fd = open(path, O_RDONLY | O_CLOEXEC);
    if (fd < 0) return {};
    char buf[256];
    ssize_t n = ::read(fd, buf, sizeof(buf) - 1);
    close(fd);
    if (n <= 0) return {};
    while (n > 0 && (buf[n-1] == '\n' || buf[n-1] == ' ' || buf[n-1] == '\r')) n--;
    buf[n] = '\0';
    return std::string(buf, (size_t)n);
}


void read_battery(int& pct, bool& charging) {
    pct = -1;
    charging = false;
    DIR* d = opendir("/sys/class/power_supply");
    if (!d) return;
    std::string node;
    while (dirent* e = readdir(d)) {
        if (e->d_name[0] == '.') continue;
        char tpath[512];
        snprintf(tpath, sizeof(tpath), "/sys/class/power_supply/%s/type", e->d_name);
        if (slurp(tpath) == "Battery") { node = e->d_name; break; }
    }
    closedir(d);
    if (node.empty()) return;

    char cpath[512], spath[512];
    snprintf(cpath, sizeof(cpath), "/sys/class/power_supply/%s/capacity", node.c_str());
    snprintf(spath, sizeof(spath), "/sys/class/power_supply/%s/status",   node.c_str());
    std::string c = slurp(cpath);
    if (!c.empty()) pct = atoi(c.c_str());
    std::string s = slurp(spath);

    charging = (s == "Charging" || s == "Full");
}

bool read_wifi_up() {
    return slurp("/sys/class/net/wlan0/operstate") == "up";
}


bool read_bt_acl_link() {
    DIR* d = opendir("/sys/class/bluetooth");
    if (!d) return false;
    bool any = false;
    while (dirent* e = readdir(d)) {
        if (strncmp(e->d_name, "hci0:", 5) == 0) { any = true; break; }
    }
    closedir(d);
    return any;
}


struct ThermalCache {
    std::string p_big;
    std::string p_mid;
    bool        resolved = false;
};
ThermalCache g_th;

void resolve_thermal_paths() {
    if (g_th.resolved) return;
    g_th.resolved = true;
    for (int i = 0; i < 64; i++) {
        char tp[128]; snprintf(tp, sizeof(tp),
                               "/sys/class/thermal/thermal_zone%d/type", i);
        std::string t = slurp(tp);
        if (t == "BIG" || t == "MID") {
            char rp[128]; snprintf(rp, sizeof(rp),
                                   "/sys/class/thermal/thermal_zone%d/temp", i);
            if (t == "BIG") g_th.p_big = rp;
            else            g_th.p_mid = rp;
        }
    }
}

int read_cpu_temp_c() {
    resolve_thermal_paths();
    int best_mc = -1;
    auto sample = [&](const std::string& p) {
        if (p.empty()) return;
        std::string v = slurp(p.c_str());
        if (v.empty()) return;
        int mc = atoi(v.c_str());
        if (mc > best_mc) best_mc = mc;
    };
    sample(g_th.p_big);
    sample(g_th.p_mid);
    if (best_mc <= 0) return -1;
    return (best_mc + 500) / 1000;
}

std::string read_ipv4_wlan0() {
    ifaddrs* ifs = nullptr;
    if (getifaddrs(&ifs) < 0) return {};
    std::string out;
    for (ifaddrs* a = ifs; a; a = a->ifa_next) {
        if (!a->ifa_addr || a->ifa_addr->sa_family != AF_INET) continue;
        if (!a->ifa_name || strcmp(a->ifa_name, "wlan0") != 0) continue;
        char buf[INET_ADDRSTRLEN]{};
        sockaddr_in* sin = (sockaddr_in*)a->ifa_addr;
        if (inet_ntop(AF_INET, &sin->sin_addr, buf, sizeof(buf))) out = buf;
        break;
    }
    freeifaddrs(ifs);
    return out;
}

}

const Snapshot& read() {
    auto now = std::chrono::steady_clock::now();
    int elapsed_ms = (int)std::chrono::duration_cast<std::chrono::milliseconds>(
                          now - g_last_refresh).count();
    bool first_call = g_last_refresh.time_since_epoch().count() == 0;
    if (first_call || elapsed_ms >= kRefreshMs) {
        read_battery(g_snap.battery_pct, g_snap.charging);
        g_snap.wifi_up    = read_wifi_up();
        g_snap.ipv4       = read_ipv4_wlan0();
        g_snap.cpu_temp_c = read_cpu_temp_c();


        auto bt = huskyfe::bluetooth::peek_status();
        g_snap.bt_powered   = bt.powered;
        g_snap.bt_connected = bt.connected || read_bt_acl_link();
        g_last_refresh = now;


        if (first_call) {
            std::thread([]() { huskyfe::bluetooth::status(); }).detach();
        }
    }
    return g_snap;
}

}
