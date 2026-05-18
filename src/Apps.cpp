#include "Apps.h"

#include <dirent.h>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <algorithm>
#include <set>

namespace huskyfe::apps {
namespace {

bool parse_desktop(const std::string& path, AppEntry& out) {
    std::ifstream f(path);
    if (!f.is_open()) return false;

    std::string line, section;
    bool nodisplay = false, hidden = false;
    std::string type, name, exec, icon;

    while (std::getline(f, line)) {
        while (!line.empty() &&
               (line.back() == '\r' || line.back() == ' ' || line.back() == '\t'))
            line.pop_back();
        if (line.empty() || line[0] == '#') continue;
        if (line[0] == '[') { section = line; continue; }
        if (section != "[Desktop Entry]") continue;

        size_t eq = line.find('=');
        if (eq == std::string::npos) continue;
        std::string key = line.substr(0, eq);
        std::string val = line.substr(eq + 1);


        if (key.find('[') != std::string::npos) continue;

        if      (key == "Type")      type      = val;
        else if (key == "Name")      name      = val;
        else if (key == "Exec")      exec      = val;
        else if (key == "Icon")      icon      = val;
        else if (key == "NoDisplay") nodisplay = (val == "true");
        else if (key == "Hidden")    hidden    = (val == "true");
    }

    if (type != "Application" || nodisplay || hidden) return false;
    if (name.empty() || exec.empty()) return false;


    if (name.size() > 14) {
        size_t sp = name.find_first_of(" \t");
        if (sp != std::string::npos) name.resize(sp);
    }


    std::string clean;
    for (size_t i = 0; i < exec.size(); i++) {
        if (exec[i] == '%' && i + 1 < exec.size()) {
            if (exec[i + 1] == '%') { clean += '%'; i++; continue; }
            i++;
            continue;
        }
        clean += exec[i];
    }
    while (!clean.empty() && (clean.back() == ' ' || clean.back() == '\t')) clean.pop_back();

    out.name         = std::move(name);
    out.exec         = std::move(clean);
    out.icon         = std::move(icon);
    out.desktop_path = path;
    return true;
}

void scan_dir(const std::string& dir,
              std::vector<AppEntry>& out,
              std::set<std::string>& seen_ids) {
    DIR* d = opendir(dir.c_str());
    if (!d) return;
    while (dirent* e = readdir(d)) {
        if (e->d_name[0] == '.') continue;
        size_t nl = strlen(e->d_name);
        if (nl < 8 || strcmp(e->d_name + nl - 8, ".desktop") != 0) continue;

        std::string id(e->d_name);
        if (seen_ids.count(id)) continue;
        seen_ids.insert(id);

        AppEntry ae;
        if (parse_desktop(dir + "/" + e->d_name, ae)) out.push_back(std::move(ae));
    }
    closedir(d);
}

}

std::vector<AppEntry> scan(int max_count) {
    std::vector<AppEntry> apps;
    std::set<std::string> seen;

    if (const char* home = getenv("HOME")) {
        scan_dir(std::string(home) + "/.local/share/applications", apps, seen);
    }
    scan_dir("/usr/local/share/applications", apps, seen);
    scan_dir("/usr/share/applications",       apps, seen);

    std::sort(apps.begin(), apps.end(),
              [](const AppEntry& a, const AppEntry& b) { return a.name < b.name; });

    if (max_count > 0 && (int)apps.size() > max_count) apps.resize((size_t)max_count);
    return apps;
}

}
