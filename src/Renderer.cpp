#include "Renderer.h"

#include <algorithm>
#include <cstdio>

namespace huskyfe {

namespace {


constexpr const char* VS = R"(
precision highp float;
attribute vec2  a_pos;
uniform   highp vec4 u_xform;
uniform   highp vec4 u_rect;
varying   highp vec2 v_local;
void main() {
    vec2 pos = u_rect.xy + a_pos * u_rect.zw;
    v_local  = a_pos * u_rect.zw;
    gl_Position = vec4(u_xform.x * pos.x + u_xform.y,
                       u_xform.z * pos.y + u_xform.w, 0.0, 1.0);
}
)";


constexpr const char* FS = R"(
precision highp float;
varying   highp vec2  v_local;
uniform   highp vec4  u_rect;
uniform   highp float u_corner;
uniform   highp vec4  u_color;
uniform   highp vec4  u_color_bot;
void main() {
    vec2  size   = u_rect.zw;
    vec2  p      = v_local - size * 0.5;
    vec2  b      = size * 0.5 - vec2(u_corner);
    vec2  d      = abs(p) - b;
    float dist   = length(max(d, 0.0)) + min(max(d.x, d.y), 0.0) - u_corner;
    float alpha  = 1.0 - smoothstep(0.0, 1.5, dist);
    float t      = clamp(v_local.y / size.y, 0.0, 1.0);
    vec4  c      = mix(u_color, u_color_bot, t);
    gl_FragColor = vec4(c.rgb, c.a * alpha);
}
)";

}

bool Renderer::init(int screen_w, int screen_h) {
    screen_w_ = screen_w;
    screen_h_ = screen_h;

    program_ = gl::build_program(VS, FS);
    if (!program_) return false;

    loc_pos_       = glGetAttribLocation (program_, "a_pos");
    loc_xform_     = glGetUniformLocation(program_, "u_xform");
    loc_rect_      = glGetUniformLocation(program_, "u_rect");
    loc_corner_    = glGetUniformLocation(program_, "u_corner");
    loc_color_     = glGetUniformLocation(program_, "u_color");
    loc_color_bot_ = glGetUniformLocation(program_, "u_color_bot");

    static const float quad[] = {
        0.0f, 0.0f,
        1.0f, 0.0f,
        0.0f, 1.0f,
        1.0f, 1.0f,
    };
    glGenBuffers(1, &vbo_);
    glBindBuffer(GL_ARRAY_BUFFER, vbo_);
    glBufferData(GL_ARRAY_BUFFER, sizeof(quad), quad, GL_STATIC_DRAW);


    xform_[0] =  2.0f / (float)screen_w_;
    xform_[1] = -1.0f;
    xform_[2] =  2.0f / (float)screen_h_;
    xform_[3] = -1.0f;

    return gl::check_error("Renderer::init");
}

void Renderer::shutdown() {
    if (vbo_)     glDeleteBuffers(1, &vbo_);
    if (program_) glDeleteProgram(program_);
    vbo_ = 0;
    program_ = 0;
}

void Renderer::begin(Color clear) {
    glClearColor(clear.r, clear.g, clear.b, clear.a);
    glClear(GL_COLOR_BUFFER_BIT);
    begin_pass();
}

void Renderer::begin_pass() {
    glViewport(0, 0, screen_w_, screen_h_);
    glEnable(GL_BLEND);
    glBlendFuncSeparate(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA,
                        GL_ONE,       GL_ONE_MINUS_SRC_ALPHA);

    glUseProgram(program_);
    glBindBuffer(GL_ARRAY_BUFFER, vbo_);
    glEnableVertexAttribArray((GLuint)loc_pos_);
    glVertexAttribPointer((GLuint)loc_pos_, 2, GL_FLOAT, GL_FALSE, 0, nullptr);
    glUniform4fv(loc_xform_, 1, xform_);
}

void Renderer::draw_rect(float x, float y, float w, float h, Color c, float corner_radius) {
    float r = std::min(corner_radius, std::min(w, h) * 0.5f);
    float rect[4]  = { x, y, w, h };
    float color[4] = { c.r, c.g, c.b, c.a };
    glUniform4fv(loc_rect_,      1, rect);
    glUniform4fv(loc_color_,     1, color);
    glUniform4fv(loc_color_bot_, 1, color);
    glUniform1f (loc_corner_, r);
    glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
}

void Renderer::draw_rect_gradient(float x, float y, float w, float h,
                                  Color top, Color bottom, float corner_radius) {
    float r = std::min(corner_radius, std::min(w, h) * 0.5f);
    float rect[4] = { x, y, w, h };
    float c0[4]   = { top.r,    top.g,    top.b,    top.a };
    float c1[4]   = { bottom.r, bottom.g, bottom.b, bottom.a };
    glUniform4fv(loc_rect_,      1, rect);
    glUniform4fv(loc_color_,     1, c0);
    glUniform4fv(loc_color_bot_, 1, c1);
    glUniform1f (loc_corner_, r);
    glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
}

void Renderer::flush() {
    glDisableVertexAttribArray((GLuint)loc_pos_);
    glUseProgram(0);
}

}
