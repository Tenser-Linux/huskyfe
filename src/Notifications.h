#pragma once
#include "Renderer.h"
#include "Text.h"
#include "ImageRenderer.h"
#include "Spring.h"

#include <atomic>
#include <cstdint>
#include <functional>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace huskyfe::notifications {


bool init(int screen_w, int screen_h);
void shutdown();

void tick(float dt);


void render(Renderer& r, ImageRenderer& image_r, TextRenderer& title,
            TextRenderer& body);


bool on_touch_down(int x, int y);
bool on_touch_up(int x, int y);


bool any_visible();


void set_muted(bool muted);
bool is_muted();


struct HistoryEntry {
    uint32_t    id;
    std::string app_name;
    std::string summary;
    std::string body;
    int32_t     urgency;
    int64_t     ts_unix_ms;
};


std::vector<HistoryEntry> history_snapshot();


void clear_one(uint32_t id);


void clear_low();


void clear_all();


uint32_t post_local(const std::string& app_name,
                    const std::string& summary,
                    const std::string& body,
                    int32_t            expire_ms = -1);

using TapHandler = std::function<void(uint32_t id, const std::string& app_name)>;
void set_tap_handler(TapHandler h);

}
