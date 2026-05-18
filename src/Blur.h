#pragma once
#include "GL.h"

namespace huskyfe {


class Blur {
public:
    bool init(int work_w, int work_h);
    void shutdown();


    GLuint draw(GLuint input_tex, int caller_screen_w, int caller_screen_h);

private:
    GLuint vbo_         = 0;
    GLuint program_h_   = 0;
    GLuint program_v_   = 0;

    GLuint fbo_a_       = 0;
    GLuint tex_a_       = 0;
    GLuint fbo_b_       = 0;
    GLuint tex_b_       = 0;

    GLint  hl_pos_      = -1;
    GLint  hl_tex_      = -1;
    GLint  hl_texel_    = -1;
    GLint  vl_pos_      = -1;
    GLint  vl_tex_      = -1;
    GLint  vl_texel_    = -1;

    int    w_ = 0, h_ = 0;
};

}
