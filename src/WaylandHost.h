#pragma once


#include <cstdint>
#include <string>
#include <vector>

#include "ImageRenderer.h"

namespace huskyfe::wlhost {


bool init(const char* socket_name = "wayland-1");
void shutdown();


void dispatch();


bool has_active_surface();


void draw(ImageRenderer& ir, const float xform[4],
          float screen_w, float screen_h);


void send_frame_callbacks(uint32_t time_ms);


void flush_deferred_releases();


using AppHandle = uintptr_t;

struct RunningApp {
    AppHandle handle;
    std::string title;
    std::string app_id;
};


std::vector<RunningApp> running();


void focus(AppHandle h);


void unfocus();


void close_focused();


void close(AppHandle h);


void touch_down  (uint32_t time_ms, int32_t slot, float x_screen, float y_screen);
void touch_motion(uint32_t time_ms, int32_t slot, float x_screen, float y_screen);
void touch_up    (uint32_t time_ms, int32_t slot);
void touch_frame ();


void keyboard_send_char(uint32_t time_ms, char c);


bool focused_is_terminal();


bool text_input_wanted();


void set_bottom_inset(int px);

}
