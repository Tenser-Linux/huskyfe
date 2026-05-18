#pragma once
#include "GL.h"

namespace huskyfe {


class ImageRenderer {
public:
    bool init();
    void shutdown();


    void begin(const float xform[4], GLuint texture, bool swap_rb = false);


    void begin_external(const float xform[4], GLuint texture,
                        bool swap_rb = false);
    void end();

    void draw(float x, float y, float w, float h,
              float u0, float v0, float u1, float v1,
              float opacity = 1.0f);

private:
    GLuint program_     = 0;
    GLuint program_ext_ = 0;
    GLuint vbo_         = 0;
    GLint  loc_pos_     = -1;
    GLint  loc_uv_      = -1;
    GLint  loc_xform_   = -1;
    GLint  loc_tex_     = -1;
    GLint  loc_alpha_   = -1;
    GLint  loc_swap_rb_ = -1;

    GLint  loc_pos_ext_     = -1;
    GLint  loc_uv_ext_      = -1;
    GLint  loc_xform_ext_   = -1;
    GLint  loc_tex_ext_     = -1;
    GLint  loc_alpha_ext_   = -1;
    GLint  loc_swap_rb_ext_ = -1;


    bool   in_external_   = false;
};

}
