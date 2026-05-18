#pragma once
#include "GL.h"

namespace huskyfe {

struct Color { float r, g, b, a; };


class Renderer {
public:
    bool init(int screen_w, int screen_h);
    void shutdown();


    void begin(Color clear);


    void begin_pass();


    void draw_rect(float x, float y, float w, float h,
                   Color c, float corner_radius = 0.0f);


    void draw_rect_gradient(float x, float y, float w, float h,
                            Color top, Color bottom,
                            float corner_radius = 0.0f);


    void flush();

    int width()  const { return screen_w_; }
    int height() const { return screen_h_; }


    const float* xform_data() const { return xform_; }

private:
    int    screen_w_   = 0;
    int    screen_h_   = 0;
    GLuint program_    = 0;
    GLuint vbo_        = 0;
    GLint  loc_pos_       = -1;
    GLint  loc_xform_     = -1;
    GLint  loc_rect_      = -1;
    GLint  loc_corner_    = -1;
    GLint  loc_color_     = -1;
    GLint  loc_color_bot_ = -1;
    float  xform_[4]   = {};
};

}
