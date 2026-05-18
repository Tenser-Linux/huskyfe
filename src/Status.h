#pragma once
#include <string>

namespace huskyfe::status {

struct Snapshot {
    int  battery_pct = -1;
    bool charging    = false;
    bool wifi_up     = false;
    std::string ipv4;
    bool bt_powered  = false;
    bool bt_connected = false;
    int  cpu_temp_c  = -1;
};


const Snapshot& read();

}
