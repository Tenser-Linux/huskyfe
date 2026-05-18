#pragma once
#include "GL.h"
#include "Renderer.h"

namespace huskyfe {


class TextRenderer {
public:
    bool init(const char* font_path, int pixel_size);
    void shutdown();


    void begin(const float xform[4]);
    void end();


    void draw(float x, float y_baseline, const char* text, Color color, float scale = 1.0f);

    float measure_width(const char* text) const;
    float ascent()      const { return ascent_; }
    float descent()     const { return descent_; }
    float line_height() const { return ascent_ - descent_; }

private:
    struct Glyph {
        float u0, v0, u1, v1;
        float w, h;
        float bearing_x, bearing_y;
        float advance;
    };
    Glyph  glyphs_[96]{};
    GLuint atlas_tex_  = 0;
    GLuint program_    = 0;
    GLuint vbo_        = 0;
    GLint  loc_pos_    = -1;
    GLint  loc_uv_     = -1;
    GLint  loc_xform_  = -1;
    GLint  loc_atlas_  = -1;
    GLint  loc_color_  = -1;
    float  ascent_     = 0.0f;
    float  descent_    = 0.0f;
};

}
