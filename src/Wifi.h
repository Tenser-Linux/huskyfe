#pragma once
#include <string>
#include <vector>

namespace huskyfe::wifi {

struct Network {
    std::string ssid;
    std::string bssid;
    int         signal    = -100;
    bool        encrypted = false;
};

struct Status {
    bool        connected = false;
    std::string ssid;
    std::string ipv4;
};


Status status();
std::vector<Network> scan_results();
void   trigger_scan();
bool   is_known(const std::string& ssid);
bool   connect_saved(const std::string& ssid);


bool   connect_new(const std::string& ssid, const std::string& psk);
void   disconnect();

}
