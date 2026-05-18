#include "Text.h"

#include <ft2build.h>
#include FT_FREETYPE_H

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <vector>

namespace huskyfe {

namespace {

constexpr const char* VS = R"(
precision highp float;
attribute vec2 a_pos;
attribute vec2 a_uv;
uniform   highp vec4 u_xform;
varying   highp vec2 v_uv;
void main() {
    gl_Position = vec4(u_xform.x * a_pos.x + u_xform.y,
                       u_xform.z * a_pos.y + u_xform.w, 0.0, 1.0);
    v_uv = a_uv;
}
)";

constexpr const char* FS = R"(
precision highp float;
varying   highp vec2 v_uv;
uniform   sampler2D  u_atlas;
uniform   highp vec4 u_color;
void main() {
    float a = texture2D(u_atlas, v_uv).r;
    gl_FragColor = vec4(u_color.rgb, u_color.a * a);
}
)";

}

bool TextRenderer::init(const char* font_path, int pixel_size) {
    FT_Library lib = nullptr;
    if (FT_Init_FreeType(&lib)) {
        fprintf(stderr, "huskyfe: FT_Init_FreeType failed\n"); return false;
    }
    FT_Face face = nullptr;
    if (FT_New_Face(lib, font_path, 0, &face)) {
        fprintf(stderr, "huskyfe: FT_New_Face %s failed\n", font_path);
        FT_Done_FreeType(lib); return false;
    }
    FT_Set_Pixel_Sizes(face, 0, (FT_UInt)pixel_size);


    const int atlas_w     = 1024;
    const int approx_w    = pixel_size + 4;
    const int per_row     = std::max(1, atlas_w / approx_w);
    const int rows_needed = (96 + per_row - 1) / per_row;
    const int atlas_h     = std::max(256, rows_needed * (pixel_size + 4) + pixel_size);

    std::vector<unsigned char> bitmap((size_t)atlas_w * atlas_h, 0);

    int x = 0, y = 0, row_h = 0;
    for (int c = 32; c < 128; c++) {
        if (FT_Load_Char(face, (FT_ULong)c, FT_LOAD_RENDER)) continue;
        const FT_Bitmap& bm = face->glyph->bitmap;
        if (x + (int)bm.width + 1 >= atlas_w) {
            x = 0; y += row_h + 1; row_h = 0;
        }

        if (y + (int)bm.rows > atlas_h) {
            fprintf(stderr, "huskyfe: text atlas overflow at glyph %d (size %d)\n",
                    c, pixel_size);
            break;
        }
        for (int yy = 0; yy < (int)bm.rows; yy++)
            for (int xx = 0; xx < (int)bm.width; xx++)
                bitmap[(size_t)(y + yy) * atlas_w + (x + xx)] =
                    bm.buffer[yy * bm.pitch + xx];
        Glyph& g     = glyphs_[c - 32];
        g.u0         = (float) x                / atlas_w;
        g.v0         = (float) y                / atlas_h;
        g.u1         = (float)(x + (int)bm.width)  / atlas_w;
        g.v1         = (float)(y + (int)bm.rows)   / atlas_h;
        g.w          = (float) bm.width;
        g.h          = (float) bm.rows;
        g.bearing_x  = (float) face->glyph->bitmap_left;
        g.bearing_y  = (float) face->glyph->bitmap_top;
        g.advance    = (float)(face->glyph->advance.x >> 6);
        x += (int)bm.width + 1;
        if ((int)bm.rows > row_h) row_h = (int)bm.rows;
    }

    ascent_  = (float)(face->size->metrics.ascender  >> 6);
    descent_ = (float)(face->size->metrics.descender >> 6);

    FT_Done_Face(face);
    FT_Done_FreeType(lib);

    program_ = gl::build_program(VS, FS);
    if (!program_) return false;

    loc_pos_   = glGetAttribLocation (program_, "a_pos");
    loc_uv_    = glGetAttribLocation (program_, "a_uv");
    loc_xform_ = glGetUniformLocation(program_, "u_xform");
    loc_atlas_ = glGetUniformLocation(program_, "u_atlas");
    loc_color_ = glGetUniformLocation(program_, "u_color");

    glGenTextures(1, &atlas_tex_);
    glBindTexture(GL_TEXTURE_2D, atlas_tex_);
    glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_R8, atlas_w, atlas_h, 0,
                 GL_RED, GL_UNSIGNED_BYTE, bitmap.data());
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S,     GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T,     GL_CLAMP_TO_EDGE);

    glGenBuffers(1, &vbo_);
    return gl::check_error("TextRenderer::init");
}

void TextRenderer::shutdown() {
    if (vbo_)        glDeleteBuffers (1, &vbo_);
    if (atlas_tex_)  glDeleteTextures(1, &atlas_tex_);
    if (program_)    glDeleteProgram(program_);
    vbo_ = 0; atlas_tex_ = 0; program_ = 0;
}

void TextRenderer::begin(const float xform[4]) {
    glUseProgram(program_);
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, atlas_tex_);
    glUniform1i  (loc_atlas_, 0);
    glUniform4fv (loc_xform_, 1, xform);
    glBindBuffer (GL_ARRAY_BUFFER, vbo_);
    glEnableVertexAttribArray((GLuint)loc_pos_);
    glEnableVertexAttribArray((GLuint)loc_uv_);
    glVertexAttribPointer((GLuint)loc_pos_, 2, GL_FLOAT, GL_FALSE, 16, (void*)0);
    glVertexAttribPointer((GLuint)loc_uv_,  2, GL_FLOAT, GL_FALSE, 16, (void*)8);
}

void TextRenderer::end() {
    glDisableVertexAttribArray((GLuint)loc_pos_);
    glDisableVertexAttribArray((GLuint)loc_uv_);
    glUseProgram(0);
}

void TextRenderer::draw(float x, float y_baseline, const char* text, Color color, float scale) {
    if (!text || !*text) return;
    std::vector<float> v;
    v.reserve(strlen(text) * 24);


    float cur = 0.0f;
    for (const unsigned char* p = (const unsigned char*)text; *p; ++p) {
        if (*p < 32 || *p >= 128) continue;
        const Glyph& g = glyphs_[*p - 32];
        if (g.w == 0.0f || g.h == 0.0f) { cur += g.advance; continue; }
        float gx = x + (cur + g.bearing_x) * scale;
        float gy = y_baseline - g.bearing_y * scale;
        float gw = g.w * scale, gh = g.h * scale;
        const float quad[24] = {
            gx,      gy,      g.u0, g.v0,
            gx + gw, gy,      g.u1, g.v0,
            gx,      gy + gh, g.u0, g.v1,
            gx + gw, gy,      g.u1, g.v0,
            gx + gw, gy + gh, g.u1, g.v1,
            gx,      gy + gh, g.u0, g.v1,
        };
        v.insert(v.end(), quad, quad + 24);
        cur += g.advance;
    }
    if (v.empty()) return;

    glBufferData(GL_ARRAY_BUFFER, (GLsizeiptr)(v.size() * sizeof(float)),
                 v.data(), GL_STREAM_DRAW);
    glUniform4f(loc_color_, color.r, color.g, color.b, color.a);
    glDrawArrays(GL_TRIANGLES, 0, (GLsizei)(v.size() / 4));
}

float TextRenderer::measure_width(const char* text) const {
    if (!text) return 0.0f;
    float w = 0.0f;
    for (const unsigned char* p = (const unsigned char*)text; *p; ++p)
        if (*p >= 32 && *p < 128) w += glyphs_[*p - 32].advance;
    return w;
}

}
