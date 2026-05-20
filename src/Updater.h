#pragma once
#include <cstdint>
#include <string>

namespace huskyfe::updater {

void start();
void stop();

bool update_available();
std::string remote_sha();
std::string local_sha();

void apply();
void skip();

void on_notification_tap(uint32_t id, const std::string& app_name);

}
