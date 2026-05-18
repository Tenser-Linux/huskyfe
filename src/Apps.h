#pragma once
#include <string>
#include <vector>

namespace huskyfe::apps {

struct AppEntry {
    std::string name;
    std::string exec;
    std::string icon;
    std::string desktop_path;
};


std::vector<AppEntry> scan(int max_count);

}
