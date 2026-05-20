#include "Wifi.h"

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <sstream>
#include <thread>
#include <algorithm>

namespace huskyfe::wifi {
namespace {


std::string run_cmd(const char* cmd) {
    std::string out;
    FILE* p = popen(cmd, "r");
    if (!p) return out;
    char buf[2048];
    size_t n;
    while ((n = fread(buf, 1, sizeof(buf), p)) > 0) out.append(buf, n);
    pclose(p);
    return out;
}


std::vector<std::string> tabsplit(const std::string& s) {
    std::vector<std::string> out;
    size_t pos = 0;
    while (pos <= s.size()) {
        size_t tab = s.find('\t', pos);
        if (tab == std::string::npos) { out.push_back(s.substr(pos)); break; }
        out.push_back(s.substr(pos, tab - pos));
        pos = tab + 1;
    }
    return out;
}

}

Status status() {
    Status s;
    std::string out = run_cmd("wpa_cli -i wlan0 status 2>/dev/null");
    std::istringstream ss(out);
    std::string line, state;
    while (std::getline(ss, line)) {
        size_t eq = line.find('=');
        if (eq == std::string::npos) continue;
        std::string k = line.substr(0, eq), v = line.substr(eq + 1);
        if      (k == "ssid")       s.ssid = v;
        else if (k == "ip_address") s.ipv4 = v;
        else if (k == "wpa_state")  state  = v;
    }
    s.connected = (state == "COMPLETED" && !s.ssid.empty());
    return s;
}

std::vector<Network> scan_results() {
    std::vector<Network> nets;
    std::string out = run_cmd("wpa_cli -i wlan0 scan_results 2>/dev/null");
    std::istringstream ss(out);
    std::string line;
    bool first = true;
    while (std::getline(ss, line)) {
        if (first) { first = false; continue; }
        if (line.empty()) continue;
        auto cols = tabsplit(line);
        if (cols.size() < 5) continue;
        Network n;
        n.bssid     = cols[0];
        n.signal    = atoi(cols[2].c_str());
        n.encrypted = (cols[3].find("WPA") != std::string::npos)
                   || (cols[3].find("WEP") != std::string::npos);
        n.ssid      = cols[4];
        if (n.ssid.empty()) continue;


        if (n.ssid.compare(0, 4, "\\x00") == 0) continue;
        nets.push_back(std::move(n));
    }

    std::sort(nets.begin(), nets.end(),
              [](const Network& a, const Network& b) { return a.signal > b.signal; });
    std::vector<Network> deduped;
    deduped.reserve(nets.size());
    for (auto& n : nets) {
        bool seen = false;
        for (auto& d : deduped) if (d.ssid == n.ssid) { seen = true; break; }
        if (!seen) deduped.push_back(std::move(n));
    }
    return deduped;
}

void trigger_scan() {
    run_cmd("wpa_cli -i wlan0 scan >/dev/null 2>&1");
}

namespace {


constexpr const char* kFreq5GHz =
    "5180 5200 5220 5240 5260 5280 5300 5320 "
    "5500 5520 5540 5560 5580 5600 5620 5640 5660 5680 5700 "
    "5745 5765 5785 5805 5825";

void pin_5ghz_only(int id) {
    char cmd[512];
    snprintf(cmd, sizeof(cmd),
             "wpa_cli -i wlan0 set_network %d freq_list \"%s\" >/dev/null 2>&1",
             id, kFreq5GHz);
    run_cmd(cmd);
}

int find_network_id(const std::string& ssid) {
    std::string out = run_cmd("wpa_cli -i wlan0 list_networks 2>/dev/null");
    std::istringstream ss(out);
    std::string line;
    bool first = true;
    while (std::getline(ss, line)) {
        if (first) { first = false; continue; }
        if (line.empty()) continue;
        auto cols = tabsplit(line);
        if (cols.size() < 2) continue;
        if (cols[1] == ssid) return atoi(cols[0].c_str());
    }
    return -1;
}
}

bool is_known(const std::string& ssid) {
    return find_network_id(ssid) >= 0;
}

bool connect_saved(const std::string& ssid) {
    int id = find_network_id(ssid);
    if (id < 0) return false;


    pin_5ghz_only(id);
    char cmd[128];
    snprintf(cmd, sizeof(cmd),
             "wpa_cli -i wlan0 select_network %d >/dev/null 2>&1", id);
    run_cmd(cmd);
    run_cmd("wpa_cli -i wlan0 save_config >/dev/null 2>&1");
    return true;
}

namespace {


std::string sanitize(const std::string& s) {
    std::string o;
    o.reserve(s.size());
    for (char c : s) if (c != '"' && c != '\\' && c != '\n' && c != '\r') o.push_back(c);
    return o;
}
}

bool connect_new(const std::string& ssid, const std::string& psk) {

    std::string out = run_cmd("wpa_cli -i wlan0 add_network 2>/dev/null");
    int id = atoi(out.c_str());
    if (id < 0) return false;

    char cmd[1024];
    std::string s_ssid = sanitize(ssid);
    std::string s_psk  = sanitize(psk);

    snprintf(cmd, sizeof(cmd),
             "wpa_cli -i wlan0 set_network %d ssid '\"%s\"' >/dev/null 2>&1",
             id, s_ssid.c_str());
    run_cmd(cmd);

    if (psk.empty()) {
        snprintf(cmd, sizeof(cmd),
                 "wpa_cli -i wlan0 set_network %d key_mgmt NONE >/dev/null 2>&1", id);
        run_cmd(cmd);
    } else {
        snprintf(cmd, sizeof(cmd),
                 "wpa_cli -i wlan0 set_network %d psk '\"%s\"' >/dev/null 2>&1",
                 id, s_psk.c_str());
        run_cmd(cmd);
    }

    pin_5ghz_only(id);

    snprintf(cmd, sizeof(cmd),
             "wpa_cli -i wlan0 enable_network %d >/dev/null 2>&1", id);
    run_cmd(cmd);
    snprintf(cmd, sizeof(cmd),
             "wpa_cli -i wlan0 select_network %d >/dev/null 2>&1", id);
    run_cmd(cmd);
    run_cmd("wpa_cli -i wlan0 save_config >/dev/null 2>&1");
    return true;
}

void disconnect() {
    run_cmd("wpa_cli -i wlan0 disconnect >/dev/null 2>&1");
}


namespace {

std::thread             g_auto_thread;
std::atomic<bool>       g_auto_stop{false};
std::mutex              g_auto_mu;
std::condition_variable g_auto_cv;

std::vector<std::pair<int, std::string>> known_networks() {
    std::vector<std::pair<int, std::string>> out;
    std::string s = run_cmd("wpa_cli -i wlan0 list_networks 2>/dev/null");
    std::istringstream ss(s);
    std::string line;
    bool first = true;
    while (std::getline(ss, line)) {
        if (first) { first = false; continue; }
        if (line.empty()) continue;
        auto cols = tabsplit(line);
        if (cols.size() < 2) continue;
        if (cols.size() >= 4 && cols[3].find("DISABLED") != std::string::npos) continue;
        out.emplace_back(atoi(cols[0].c_str()), cols[1]);
    }
    return out;
}

void auto_thread_main() {
    using namespace std::chrono_literals;
    while (!g_auto_stop.load()) {
        Status st = status();
        if (!st.connected) {
            auto known = known_networks();
            if (!known.empty()) {
                run_cmd("wpa_cli -i wlan0 scan >/dev/null 2>&1");
                {
                    std::unique_lock<std::mutex> lk(g_auto_mu);
                    g_auto_cv.wait_for(lk, 2s, []{ return g_auto_stop.load(); });
                }
                if (g_auto_stop.load()) break;

                auto seen = scan_results();
                for (auto& [id, ssid] : known) {
                    bool in_range = false;
                    for (auto& n : seen) if (n.ssid == ssid) { in_range = true; break; }
                    if (!in_range) continue;
                    char cmd[128];
                    snprintf(cmd, sizeof(cmd),
                             "wpa_cli -i wlan0 select_network %d >/dev/null 2>&1", id);
                    run_cmd(cmd);
                    break;
                }
            }
        }
        std::unique_lock<std::mutex> lk(g_auto_mu);
        g_auto_cv.wait_for(lk, 5s, []{ return g_auto_stop.load(); });
    }
}

}  // namespace

void start_auto_connect() {
    if (g_auto_thread.joinable()) return;
    g_auto_stop.store(false);
    g_auto_thread = std::thread(auto_thread_main);
}

void stop_auto_connect() {
    if (!g_auto_thread.joinable()) return;
    g_auto_stop.store(true);
    g_auto_cv.notify_all();
    g_auto_thread.join();
}

}
