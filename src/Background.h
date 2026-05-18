#pragma once
#include "GL.h"

#include <string>

namespace huskyfe {


class Background {
public:


    bool init(int screen_w, int screen_h, int downscale = 2,
              float max_redraw_hz = 60.0f);
    void shutdown();


    void draw(float t_seconds, float dim = 0.0f);


    GLuint texture() const { return tex_; }


    bool set_shader(const std::string& sel);


    const std::string& current_shader() const { return current_sel_; }


    void set_paused(bool p) { paused_ = p; }

private:
    enum class Mode { NONE, PROGRAM };
    Mode   mode_ = Mode::PROGRAM;
    std::string current_sel_ = "rgb";
    bool   need_clear_ = false;
    bool   paused_     = false;


    GLuint program_bg_   = 0;
    GLint  loc_bg_pos_   = -1;
    GLint  loc_bg_time_  = -1;
    GLint  loc_bg_dim_   = -1;
    GLint  loc_bg_res_   = -1;


    GLuint program_blit_ = 0;
    GLint  loc_blit_pos_ = -1;
    GLint  loc_blit_tex_ = -1;

    GLuint vbo_   = 0;
    GLuint fbo_   = 0;
    GLuint tex_   = 0;

    int    screen_w_       = 0;
    int    screen_h_       = 0;
    int    fbo_w_          = 0;
    int    fbo_h_          = 0;
    float  min_interval_   = 1.0f / 60.0f;
    float  last_redraw_t_  = -1e9f;
    float  last_dim_       = -1.0f;
};

}
