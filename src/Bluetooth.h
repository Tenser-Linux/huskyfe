#pragma once
#include <string>
#include <vector>

namespace huskyfe::bluetooth {

struct Device {
    std::string mac;
    std::string name;
    int         rssi      = 0;
    bool        paired    = false;
    bool        connected = false;
};

struct Status {
    bool        powered        = false;
    bool        connected      = false;
    std::string connected_name;
};


struct Adapter {
    std::string mac;
    std::string name;
    bool        powered = false;
    bool        active  = false;
};


Status status();


std::vector<Device> devices();


Status              peek_status();
std::vector<Device> peek_devices();

void   trigger_scan();
void   stop_scan();
bool   power_on();
bool   power_off();


bool   pair_and_connect(const std::string& mac);
bool   connect_saved(const std::string& mac);
void   disconnect(const std::string& mac);

bool   is_paired(const std::string& mac);


std::vector<Adapter> adapters();


std::string active_adapter();


void   set_active_adapter(const std::string& mac);

}
