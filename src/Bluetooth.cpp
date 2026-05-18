#include "Bluetooth.h"

#include <atomic>
#include <cctype>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <mutex>
#include <sstream>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <thread>
#include <algorithm>
#include <unistd.h>

namespace huskyfe::bluetooth {
namespace {

constexpr const char* ACTIVE_ADAPTER_PATH = "/var/lib/huskyfe/bt_adapter";
constexpr const char* HUSKYBT_SOCK        = "/run/huskybt.sock";


bool hb_available() {
    struct stat st;
    return ::stat(HUSKYBT_SOCK, &st) == 0;
}


std::string hb_request(const std::string& json_line) {
    int fd = ::socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
    if (fd < 0) return {};
    sockaddr_un sa{};
    sa.sun_family = AF_UNIX;
    std::strncpy(sa.sun_path, HUSKYBT_SOCK, sizeof(sa.sun_path) - 1);
    if (::connect(fd, reinterpret_cast<sockaddr*>(&sa), sizeof(sa)) < 0) {
        ::close(fd); return {};
    }

    std::string req = json_line;
    if (req.empty() || req.back() != '\n') req += '\n';

    timeval tv{12, 0};
    ::setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
    ssize_t w = ::send(fd, req.data(), req.size(), 0);
    if (w < 0) { ::close(fd); return {}; }
    std::string out;
    char buf[4096];
    for (;;) {
        ssize_t r = ::recv(fd, buf, sizeof(buf), 0);
        if (r <= 0) break;
        out.append(buf, r);
        if (out.find('\n') != std::string::npos) break;
    }
    ::close(fd);
    auto nl = out.find('\n');
    if (nl != std::string::npos) out.resize(nl);
    return out;
}


std::string hb_field(const std::string& obj, const std::string& key) {
    std::string needle = "\"" + key + "\":";
    size_t p = obj.find(needle);
    if (p == std::string::npos) return {};
    p += needle.size();
    while (p < obj.size() && (obj[p] == ' ' || obj[p] == '\t')) p++;
    if (p >= obj.size()) return {};
    if (obj[p] == '"') {
        size_t q = p + 1;
        std::string v;
        while (q < obj.size() && obj[q] != '"') {
            if (obj[q] == '\\' && q + 1 < obj.size()) { v.push_back(obj[q+1]); q += 2; }
            else v.push_back(obj[q++]);
        }
        return v;
    }
    if (obj[p] == '{' || obj[p] == '[') {
        char open = obj[p], close = (open == '{') ? '}' : ']';
        int depth = 0;
        bool in_str = false;
        size_t q = p;
        for (; q < obj.size(); q++) {
            char c = obj[q];
            if (in_str) {
                if (c == '\\' && q + 1 < obj.size()) { q++; continue; }
                if (c == '"') in_str = false;
                continue;
            }
            if (c == '"') in_str = true;
            else if (c == open) depth++;
            else if (c == close) { depth--; if (!depth) { q++; break; } }
        }
        return obj.substr(p, q - p);
    }
    size_t q = p;
    while (q < obj.size() && obj[q] != ',' && obj[q] != '}' && obj[q] != ']') q++;
    std::string v = obj.substr(p, q - p);
    while (!v.empty() && (v.back() == ' ' || v.back() == '\t')) v.pop_back();
    return v;
}

bool hb_bool(const std::string& obj, const std::string& key) {
    return hb_field(obj, key) == "true";
}


std::vector<std::string> hb_split_array(const std::string& arr) {
    std::vector<std::string> out;
    if (arr.size() < 2 || arr.front() != '[') return out;
    int depth = 0;
    bool in_str = false;
    size_t start = 0;
    for (size_t i = 1; i + 1 < arr.size(); i++) {
        char c = arr[i];
        if (in_str) {
            if (c == '\\' && i + 1 < arr.size()) { i++; continue; }
            if (c == '"') in_str = false;
            continue;
        }
        if (c == '"') in_str = true;
        else if (c == '{') { if (depth++ == 0) start = i; }
        else if (c == '}') { if (--depth == 0) out.push_back(arr.substr(start, i - start + 1)); }
    }
    return out;
}


std::mutex   g_hb_addr_mu;
std::string  g_hb_addr;
std::chrono::steady_clock::time_point g_hb_addr_at{};

std::string hb_self_addr() {
    {
        std::lock_guard<std::mutex> lk(g_hb_addr_mu);
        auto age = std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now() - g_hb_addr_at).count();
        if (!g_hb_addr.empty() && age < 30'000) return g_hb_addr;
    }
    std::string r = hb_request("{\"cmd\":\"status\"}");
    std::string a = hb_field(r, "addr");
    std::lock_guard<std::mutex> lk(g_hb_addr_mu);
    g_hb_addr = a;
    g_hb_addr_at = std::chrono::steady_clock::now();
    return a;
}

bool use_huskybt(const std::string& active_mac) {
    if (!hb_available()) return false;
    if (active_mac.empty()) return true;       // default adapter
    std::string self = hb_self_addr();
    return !self.empty() && active_mac == self;
}

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

// ---- Active adapter selection ------------------------------------------
//
// bluetoothctl on the CLI runs each subcommand against bluez's "default"


std::mutex   g_adapter_mu;
std::string  g_active_mac;
bool         g_active_loaded = false;

bool valid_mac_str(const std::string& s);

void load_active_locked() {
    if (g_active_loaded) return;
    g_active_loaded = true;
    FILE* f = fopen(ACTIVE_ADAPTER_PATH, "r");
    if (!f) return;
    char buf[64] = {0};
    if (fgets(buf, sizeof(buf), f)) {
        size_t n = strlen(buf);
        while (n > 0 && (buf[n-1] == '\n' || buf[n-1] == '\r' || buf[n-1] == ' ' || buf[n-1] == '\t'))
            buf[--n] = 0;
        std::string s(buf);
        if (valid_mac_str(s)) g_active_mac = s;
    }
    fclose(f);
}

std::string get_active_mac() {
    std::lock_guard<std::mutex> lk(g_adapter_mu);
    load_active_locked();
    return g_active_mac;
}


std::string btctl_pipeline(const std::string& cmds, const char* extra_args) {
    std::string sel = get_active_mac();
    std::string script;
    if (!sel.empty()) script += "select " + sel + "\n";
    script += cmds;
    if (!script.empty() && script.back() != '\n') script += '\n';

    std::string esc;
    esc.reserve(script.size());
    for (char c : script) {
        if (c == '\'') esc += "'\\''";
        else            esc += c;
    }
    std::string pipeline = "printf '%s' '" + esc + "' | bluetoothctl";
    if (extra_args && *extra_args) {
        pipeline += ' ';
        pipeline += extra_args;
    }
    return pipeline;
}


std::string btctl_run(const std::string& cmd, const char* extra_args = "") {
    std::string p = btctl_pipeline(cmd, extra_args);
    p += " 2>/dev/null";
    return run_cmd(p.c_str());
}


int btctl_system(const std::string& cmd, const char* extra_args = "") {
    std::string p = btctl_pipeline(cmd, extra_args);
    p += " >/dev/null 2>&1";
    return system(p.c_str());
}

std::string lstrip(const std::string& s);
bool        valid_mac(const std::string& s);


std::string mac_to_hci(const std::string& mac) {
    if (mac.empty()) return {};
    std::string ls = run_cmd("ls /sys/class/bluetooth/ 2>/dev/null");
    std::istringstream ss(ls);
    std::string node;
    while (std::getline(ss, node)) {
        while (!node.empty() && (node.back() == '\r' || node.back() == '\n'
                              || node.back() == ' '  || node.back() == '\t'))
            node.pop_back();
        if (node.size() < 4 || node.compare(0, 3, "hci") != 0) continue;
        if (node.find(':') != std::string::npos) continue;
        std::string out = run_cmd(("hciconfig " + node + " 2>/dev/null").c_str());
        std::istringstream is(out);
        std::string l;
        while (std::getline(is, l)) {
            std::string ls = lstrip(l);
            if (ls.compare(0, 12, "BD Address: ") == 0) {
                std::string m = ls.substr(12, 17);
                if (m == mac) return node;
                break;
            }
        }
    }
    return {};
}


std::mutex                                            g_manages_mu;
std::map<std::string, std::pair<bool,
        std::chrono::steady_clock::time_point>>       g_manages_cache;
constexpr int kManagesCacheMs = 5000;

bool bluez_manages(const std::string& mac) {
    if (mac.empty()) return true;
    {
        std::lock_guard<std::mutex> lk(g_manages_mu);
        auto it = g_manages_cache.find(mac);
        if (it != g_manages_cache.end()) {
            auto age = std::chrono::duration_cast<std::chrono::milliseconds>(
                std::chrono::steady_clock::now() - it->second.second).count();
            if (age < kManagesCacheMs) return it->second.first;
        }
    }


    std::string out = run_cmd(
        ("bluetoothctl show " + mac + " 2>/dev/null").c_str());
    bool managed = out.find("Controller " + mac) != std::string::npos
                && out.find("Powered:") != std::string::npos;
    {
        std::lock_guard<std::mutex> lk(g_manages_mu);
        g_manages_cache[mac] = {managed, std::chrono::steady_clock::now()};
    }
    return managed;
}

void invalidate_manages_cache() {
    std::lock_guard<std::mutex> lk(g_manages_mu);
    g_manages_cache.clear();
}


constexpr const char* HCI_SCAN_LOG = "/run/huskyfe/hci-scan.log";


std::vector<Device> parse_btmgmt_log(const std::string& path) {
    FILE* f = fopen(path.c_str(), "r");
    if (!f) return {};
    std::string text;
    char buf[4096];
    size_t n;
    while ((n = fread(buf, 1, sizeof(buf), f)) > 0) text.append(buf, n);
    fclose(f);


    std::map<std::string, Device> by_mac;
    std::istringstream ss(text);
    std::string line;
    Device cur;
    bool have = false;
    auto flush = [&]() {
        if (have && !cur.mac.empty()) by_mac[cur.mac] = cur;
        cur = {};
        have = false;
    };
    while (std::getline(ss, line)) {

        size_t p = line.find("dev_found:");
        if (p != std::string::npos) {
            flush();
            std::string rest = lstrip(line.substr(p + 10));
            if (rest.size() >= 17) {
                std::string mac = rest.substr(0, 17);
                if (valid_mac(mac)) { cur.mac = mac; have = true; }
            }
            size_t r = line.find("rssi ");
            if (r != std::string::npos)
                cur.rssi = atoi(line.c_str() + r + 5);
            continue;
        }
        if (!have) continue;
        std::string ls = lstrip(line);
        if (ls.compare(0, 5, "name ") == 0) cur.name = ls.substr(5);
    }
    flush();

    std::vector<Device> out;
    out.reserve(by_mac.size());
    for (auto& kv : by_mac) out.push_back(std::move(kv.second));

    std::sort(out.begin(), out.end(), [](const Device& a, const Device& b) {
        int ra = a.rssi == 0 ? -200 : a.rssi;
        int rb = b.rssi == 0 ? -200 : b.rssi;
        return ra > rb;
    });
    return out;
}


bool hci_powered(const std::string& hci) {
    std::string out = run_cmd(("hciconfig " + hci + " 2>/dev/null").c_str());
    std::istringstream is(out);
    std::string l;
    while (std::getline(is, l)) {
        std::string ls = lstrip(l);
        if (ls.compare(0, 2, "UP") == 0)   return true;
        if (ls.compare(0, 4, "DOWN") == 0) return false;
    }
    return false;
}


std::string lstrip(const std::string& s) {
    size_t i = 0;
    while (i < s.size() && (s[i] == ' ' || s[i] == '\t')) i++;
    return s.substr(i);
}

bool valid_mac(const std::string& s) {
    if (s.size() != 17) return false;
    for (size_t i = 0; i < s.size(); i++) {
        if (i % 3 == 2) { if (s[i] != ':') return false; }
        else            { if (!isxdigit((unsigned char)s[i])) return false; }
    }
    return true;
}

bool valid_mac_str(const std::string& s) { return valid_mac(s); }


bool looks_like_mac_name(const std::string& s) {
    if (s.size() != 17) return false;
    for (size_t i = 0; i < s.size(); i++) {
        if (i % 3 == 2) { if (s[i] != ':' && s[i] != '-') return false; }
        else            { if (!isxdigit((unsigned char)s[i])) return false; }
    }
    return true;
}


bool parse_info(const std::string& mac, Device& d) {
    std::string out = btctl_run("info " + mac);
    if (out.find("Device " + mac) == std::string::npos
        && out.find("Missing device") != std::string::npos) {
        return false;
    }
    if (out.empty()) return false;
    d.mac = mac;
    std::istringstream ss(out);
    std::string line;
    while (std::getline(ss, line)) {
        line = lstrip(line);
        auto eat = [&](const char* key) -> std::string {
            size_t kl = strlen(key);
            if (line.size() > kl && line.compare(0, kl, key) == 0) {
                return lstrip(line.substr(kl));
            }
            return {};
        };
        std::string v;
        if (!(v = eat("Name: ")).empty()) {


            if (!looks_like_mac_name(v)) d.name = v;
        }
        else if (!(v = eat("Alias: ")).empty()) {


            if (d.name.empty() && !looks_like_mac_name(v)) d.name = v;
        }
        else if (!(v = eat("Paired: ")).empty())   d.paired = (v == "yes");
        else if (!(v = eat("Connected: ")).empty()) d.connected = (v == "yes");
        else if (!(v = eat("RSSI: ")).empty())     d.rssi = atoi(v.c_str());
    }
    return !d.mac.empty();
}


constexpr int kCacheMs = 2500;

std::mutex                                g_cache_mu;
Status                                    g_cached_status;
std::vector<Device>                       g_cached_devices;
std::chrono::steady_clock::time_point     g_status_at{};
std::chrono::steady_clock::time_point     g_devices_at{};

bool fresh(std::chrono::steady_clock::time_point at) {
    if (at.time_since_epoch().count() == 0) return false;
    auto age = std::chrono::duration_cast<std::chrono::milliseconds>(
                   std::chrono::steady_clock::now() - at).count();
    return age < kCacheMs;
}

void invalidate_cache() {
    std::lock_guard<std::mutex> lk(g_cache_mu);
    g_status_at  = {};
    g_devices_at = {};
}

}

Status peek_status() {
    std::lock_guard<std::mutex> lk(g_cache_mu);
    return g_cached_status;
}

std::vector<Device> peek_devices() {
    std::lock_guard<std::mutex> lk(g_cache_mu);
    return g_cached_devices;
}

Status status() {
    {
        std::lock_guard<std::mutex> lk(g_cache_mu);
        if (fresh(g_status_at)) return g_cached_status;
    }
    Status s;
    std::string active = get_active_mac();
    bool used_kernel = false;
    if (use_huskybt(active)) {


        std::string st = hb_request("{\"cmd\":\"status\"}");
        s.powered = hb_bool(st, "powered");
        if (s.powered) {
            std::string dl = hb_request("{\"cmd\":\"devices\"}");
            std::string arr = hb_field(dl, "devices");
            for (auto& d : hb_split_array(arr)) {
                if (hb_bool(d, "connected")) {
                    s.connected = true;
                    std::string nm = hb_field(d, "name");
                    s.connected_name = nm.empty() ? hb_field(d, "addr") : nm;
                    break;
                }
            }
        }
        {
            std::lock_guard<std::mutex> lk(g_cache_mu);
            g_cached_status = s;
            g_status_at     = std::chrono::steady_clock::now();
        }
        return s;
    }
    if (!active.empty() && !bluez_manages(active)) {

        std::string hci = mac_to_hci(active);
        if (!hci.empty()) {
            s.powered = hci_powered(hci);
            used_kernel = true;
        }
    }
    if (!used_kernel) {
        std::string out = btctl_run("show");
        std::istringstream ss(out);
        std::string line;
        bool got_powered_line = false;
        while (std::getline(ss, line)) {
            line = lstrip(line);
            if (line.compare(0, 9, "Powered: ") == 0) {
                s.powered = (lstrip(line.substr(9)) == "yes");
                got_powered_line = true;
            }
        }


        if (!got_powered_line) {
            std::string ls = run_cmd("ls /sys/class/bluetooth/ 2>/dev/null");
            std::istringstream is(ls);
            std::string node;
            while (std::getline(is, node)) {
                while (!node.empty() && (node.back() == '\n' || node.back() == '\r'
                                      || node.back() == ' '  || node.back() == '\t'))
                    node.pop_back();
                if (node.size() < 4 || node.compare(0, 3, "hci") != 0) continue;
                if (node.find(':') != std::string::npos) continue;
                if (hci_powered(node)) { s.powered = true; break; }
            }
        }
    }
    if (s.powered && !used_kernel) {

        std::string list = btctl_run("devices");
        std::istringstream ls(list);
        std::string line;
        while (std::getline(ls, line)) {

            if (line.size() < 25 || line.compare(0, 7, "Device ") != 0) continue;
            std::string mac = line.substr(7, 17);
            if (!valid_mac(mac)) continue;
            Device d;
            if (parse_info(mac, d) && d.connected) {
                s.connected      = true;
                s.connected_name = d.name.empty() ? mac : d.name;
                break;
            }
        }
    }
    {
        std::lock_guard<std::mutex> lk(g_cache_mu);
        g_cached_status = s;
        g_status_at     = std::chrono::steady_clock::now();
    }
    return s;
}

std::vector<Device> devices() {
    {
        std::lock_guard<std::mutex> lk(g_cache_mu);
        if (fresh(g_devices_at)) return g_cached_devices;
    }
    std::vector<Device> out;
    std::string active = get_active_mac();
    if (use_huskybt(active)) {
        std::string dl = hb_request("{\"cmd\":\"devices\"}");
        std::string arr = hb_field(dl, "devices");
        for (auto& d : hb_split_array(arr)) {
            Device r;
            r.mac = hb_field(d, "addr");
            r.name = hb_field(d, "name");
            std::string rs = hb_field(d, "rssi");
            if (!rs.empty()) r.rssi = atoi(rs.c_str());
            r.paired    = hb_bool(d, "paired");
            r.connected = hb_bool(d, "connected");
            if (r.mac.empty() || !valid_mac(r.mac)) continue;

            if (r.name.empty() || r.name == r.mac) continue;
            if (looks_like_mac_name(r.name)) continue;
            out.push_back(std::move(r));
        }
        std::sort(out.begin(), out.end(), [](const Device& a, const Device& b) {
            if (a.connected != b.connected) return a.connected;
            if (a.paired    != b.paired)    return a.paired;
            int ra = a.rssi == 0 ? -200 : a.rssi;
            int rb = b.rssi == 0 ? -200 : b.rssi;
            return ra > rb;
        });
        if (out.size() > 10) out.resize(10);
        std::lock_guard<std::mutex> lk(g_cache_mu);
        g_cached_devices = out;
        g_devices_at     = std::chrono::steady_clock::now();
        return out;
    }
    if (!active.empty() && !bluez_manages(active)) {


        out = parse_btmgmt_log(HCI_SCAN_LOG);
        for (auto& d : out) if (d.name.empty()) d.name = d.mac;
        if (out.size() > 25) out.resize(25);
        std::lock_guard<std::mutex> lk(g_cache_mu);
        g_cached_devices = out;
        g_devices_at     = std::chrono::steady_clock::now();
        return out;
    }
    std::string list = btctl_run("devices");
    std::istringstream ss(list);
    std::string line;
    while (std::getline(ss, line)) {
        if (line.size() < 25 || line.compare(0, 7, "Device ") != 0) continue;
        std::string mac = line.substr(7, 17);
        if (!valid_mac(mac)) continue;
        Device d;
        if (parse_info(mac, d)) {

            if (d.name.empty() || d.name == mac) continue;
            if (looks_like_mac_name(d.name)) continue;
            out.push_back(std::move(d));
        }
    }

    std::sort(out.begin(), out.end(), [](const Device& a, const Device& b) {
        if (a.connected != b.connected) return a.connected;
        if (a.paired    != b.paired)    return a.paired;
        int ra = a.rssi == 0 ? -200 : a.rssi;
        int rb = b.rssi == 0 ? -200 : b.rssi;
        return ra > rb;
    });
    if (out.size() > 10) out.resize(10);
    {
        std::lock_guard<std::mutex> lk(g_cache_mu);
        g_cached_devices = out;
        g_devices_at     = std::chrono::steady_clock::now();
    }
    return out;
}

void trigger_scan() {
    std::string active = get_active_mac();
    if (use_huskybt(active)) {
        hb_request("{\"cmd\":\"scan\",\"action\":\"start\"}");
        invalidate_cache();
        return;
    }
    bool kernel_path = !active.empty() && !bluez_manages(active);
    if (kernel_path) {
        std::string hci = mac_to_hci(active);
        if (!hci.empty()) {


            std::string idx = hci.substr(3);
            std::string p =
                "mkdir -p /run/huskyfe; "
                ": > " + std::string(HCI_SCAN_LOG) + "; "
                "pkill -f 'btmgmt --index " + idx + " find' >/dev/null 2>&1; "
                "setsid bash -c \"timeout 12 btmgmt --index " + idx
                + " find -l > " + HCI_SCAN_LOG + " 2>&1\" >/dev/null 2>&1 &";
            run_cmd(p.c_str());
            invalidate_cache();
            return;
        }
    }


    std::string inner = btctl_pipeline("scan on", "--timeout 12");
    std::string p = "setsid bash -c \"" + inner + "\" >/dev/null 2>&1 &";
    run_cmd(p.c_str());
    invalidate_cache();
}

void stop_scan() {
    std::string active = get_active_mac();
    if (use_huskybt(active)) {
        hb_request("{\"cmd\":\"scan\",\"action\":\"stop\"}");
        invalidate_cache();
        return;
    }
    if (!active.empty() && !bluez_manages(active)) {
        std::string hci = mac_to_hci(active);
        if (!hci.empty()) {
            std::string idx = hci.substr(3);
            std::string p =
                "pkill -f 'btmgmt --index " + idx + " find' >/dev/null 2>&1";
            (void)system(p.c_str());
            invalidate_cache();
            return;
        }
    }
    btctl_system("scan off");
    invalidate_cache();
}

bool power_on() {
    std::string active = get_active_mac();
    bool used_kernel = false;
    if (use_huskybt(active)) {


        std::string r = hb_request("{\"cmd\":\"power\",\"action\":\"on\"}");
        invalidate_cache();
        if (hb_bool(r, "ok")) return hb_bool(r, "powered");
        std::string self = active.empty() ? hb_self_addr() : active;
        std::string hci = mac_to_hci(self);
        if (!hci.empty()) {
            std::string p = "hciconfig " + hci + " up >/dev/null 2>&1";
            (void)system(p.c_str());
            return status().powered;
        }
    }
    if (!active.empty() && !bluez_manages(active)) {
        std::string hci = mac_to_hci(active);
        if (!hci.empty()) {
            std::string p = "hciconfig " + hci + " up >/dev/null 2>&1";
            if (system(p.c_str()) != 0) {


                std::string idx = hci.substr(3);
                std::string seq =
                    "btmgmt --index " + idx + " power off >/dev/null 2>&1; "
                    "btmgmt --index " + idx + " static-addr "
                        "C0:DE:CA:FE:BA:BE >/dev/null 2>&1; "
                    "btmgmt --index " + idx + " power on  >/dev/null 2>&1";
                (void)system(seq.c_str());
            }
            used_kernel = true;
        }
    }
    if (!used_kernel) btctl_system("power on");
    invalidate_cache();
    invalidate_manages_cache();
    return status().powered;
}

bool power_off() {
    std::string active = get_active_mac();
    bool used_kernel = false;
    if (use_huskybt(active)) {
        std::string r = hb_request("{\"cmd\":\"power\",\"action\":\"off\"}");
        invalidate_cache();
        if (hb_bool(r, "ok")) return !hb_bool(r, "powered");
        std::string self = active.empty() ? hb_self_addr() : active;
        std::string hci = mac_to_hci(self);
        if (!hci.empty()) {
            std::string p = "hciconfig " + hci + " down >/dev/null 2>&1";
            (void)system(p.c_str());
            return !status().powered;
        }
    }
    if (!active.empty() && !bluez_manages(active)) {
        std::string hci = mac_to_hci(active);
        if (!hci.empty()) {
            std::string p = "hciconfig " + hci + " down >/dev/null 2>&1";
            (void)system(p.c_str());
            used_kernel = true;
        }
    }
    if (!used_kernel) btctl_system("power off");
    invalidate_cache();
    return !status().powered;
}

bool is_paired(const std::string& mac) {
    if (use_huskybt(get_active_mac())) {
        std::string dl = hb_request("{\"cmd\":\"devices\"}");
        for (auto& d : hb_split_array(hb_field(dl, "devices"))) {
            if (hb_field(d, "addr") == mac) return hb_bool(d, "paired");
        }
        return false;
    }
    Device d;
    return parse_info(mac, d) && d.paired;
}


static int btctl_timeout(int seconds, const std::string& cmd) {
    char tbuf[16]; snprintf(tbuf, sizeof(tbuf), "timeout %d", seconds);
    std::string inner = btctl_pipeline(cmd, "");
    std::string p = std::string(tbuf) + " bash -c \"" + inner + "\" >/dev/null 2>&1";
    return system(p.c_str());
}


static uint8_t hb_addr_type(const std::string& mac) {
    std::string dl = hb_request("{\"cmd\":\"devices\"}");
    for (auto& d : hb_split_array(hb_field(dl, "devices"))) {
        if (hb_field(d, "addr") == mac) {
            std::string t = hb_field(d, "type");
            return t.empty() ? 0 : (uint8_t)atoi(t.c_str());
        }
    }
    return 0;
}

bool connect_saved(const std::string& mac) {
    if (use_huskybt(get_active_mac())) {
        std::string req = "{\"cmd\":\"connect\",\"addr\":\"" + mac
                        + "\",\"profile\":\"a2dp\"}";
        std::string r = hb_request(req);
        invalidate_cache();
        return hb_bool(r, "ok");
    }
    int rc = btctl_timeout(10, "connect " + mac);
    invalidate_cache();
    return rc == 0;
}

bool pair_and_connect(const std::string& mac) {
    if (use_huskybt(get_active_mac())) {
        uint8_t t = hb_addr_type(mac);
        if (!is_paired(mac)) {
            char tbuf[4]; snprintf(tbuf, sizeof(tbuf), "%u", t);
            std::string req = "{\"cmd\":\"pair\",\"addr\":\"" + mac
                            + "\",\"type\":" + tbuf + "}";
            hb_request(req);


            for (int i = 0; i < 24; i++) {
                std::this_thread::sleep_for(std::chrono::milliseconds(500));
                if (is_paired(mac)) break;
            }
        }
        std::string creq = "{\"cmd\":\"connect\",\"addr\":\"" + mac
                         + "\",\"profile\":\"a2dp\"}";
        std::string r = hb_request(creq);
        invalidate_cache();
        return hb_bool(r, "ok");
    }
    if (!is_paired(mac)) btctl_timeout(12, "pair " + mac);
    btctl_system("trust " + mac);
    int rc = btctl_timeout(10, "connect " + mac);
    invalidate_cache();
    return rc == 0;
}

void disconnect(const std::string& mac) {
    if (use_huskybt(get_active_mac())) {
        std::string req = "{\"cmd\":\"disconnect\",\"addr\":\"" + mac + "\"}";
        hb_request(req);
        invalidate_cache();
        return;
    }
    btctl_system("disconnect " + mac);
    invalidate_cache();
}

std::string active_adapter() {
    return get_active_mac();
}

void set_active_adapter(const std::string& mac) {
    {
        std::lock_guard<std::mutex> lk(g_adapter_mu);
        load_active_locked();
        if (!mac.empty() && !valid_mac(mac)) return;
        g_active_mac = mac;


        ::mkdir("/var/lib/huskyfe", 0755);
        FILE* f = fopen(ACTIVE_ADAPTER_PATH, "w");
        if (f) {
            if (!mac.empty()) {
                fwrite(mac.data(), 1, mac.size(), f);
                fputc('\n', f);
            }
            fclose(f);
        }
    }

    invalidate_cache();
    invalidate_manages_cache();
}

std::vector<Adapter> adapters() {
    std::vector<Adapter> out;


    std::string ls = run_cmd("ls /sys/class/bluetooth/ 2>/dev/null");
    std::istringstream ss(ls);
    std::string node;
    while (std::getline(ss, node)) {

        while (!node.empty() && (node.back() == ' ' || node.back() == '\t'
                              || node.back() == '\r' || node.back() == '\n'))
            node.pop_back();


        if (node.size() < 4 || node.compare(0, 3, "hci") != 0) continue;
        if (node.find(':') != std::string::npos) continue;
        bool all_digits_after_hci = true;
        for (size_t i = 3; i < node.size(); i++)
            if (!isdigit((unsigned char)node[i])) { all_digits_after_hci = false; break; }
        if (!all_digits_after_hci) continue;


        std::string out_text = run_cmd(
            ("hciconfig " + node + " 2>/dev/null").c_str());
        if (out_text.empty()) continue;

        Adapter a;
        a.name = node;
        std::istringstream is(out_text);
        std::string l;
        while (std::getline(is, l)) {
            std::string ls = lstrip(l);
            if (ls.compare(0, 12, "BD Address: ") == 0) {
                std::string mac = ls.substr(12, 17);
                if (mac.size() == 17) a.mac = mac;
            }

            if (ls.compare(0, 2, "UP") == 0)   a.powered = true;
            if (ls.compare(0, 4, "DOWN") == 0) a.powered = false;
        }
        if (a.mac.empty() || a.mac.size() != 17) continue;


        if (a.mac != "00:00:00:00:00:00") {
            std::string info = run_cmd(
                ("bluetoothctl show " + a.mac + " 2>/dev/null").c_str());
            std::istringstream nis(info);
            std::string nl;
            while (std::getline(nis, nl)) {
                std::string ns = lstrip(nl);
                if (ns.compare(0, 6, "Name: ") == 0) {
                    std::string n = lstrip(ns.substr(6));
                    if (!n.empty()) a.name = n;
                    break;
                }
            }
        }
        out.push_back(std::move(a));
    }
    std::sort(out.begin(), out.end(),
              [](const Adapter& a, const Adapter& b) { return a.mac < b.mac; });

    std::string sel = active_adapter();
    if (!sel.empty()) {
        for (auto& a : out) if (a.mac == sel) a.active = true;
    }
    return out;
}

}
