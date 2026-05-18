#include "Blur.h"

#include <cstdio>

namespace huskyfe {

namespace {

constexpr const char* VS = R"(
precision highp float;
attribute vec2 a_pos;
varying   highp vec2 v_uv;
void main() {
    gl_Position = vec4(a_pos, 0.0, 1.0);
    v_uv = a_pos * 0.5 + 0.5;
}
)";


constexpr const char* FS = R"(
precision highp float;
varying   highp vec2 v_uv;
uniform   sampler2D  u_tex;
uniform   highp vec2 u_texel;
void main() {
    vec4 s = vec4(0.0);
    s += texture2D(u_tex, v_uv - 4.0 * u_texel) * 0.05;
    s += texture2D(u_tex, v_uv - 3.0 * u_texel) * 0.09;
    s += texture2D(u_tex, v_uv - 2.0 * u_texel) * 0.12;
    s += texture2D(u_tex, v_uv - 1.0 * u_texel) * 0.15;
    s += texture2D(u_tex, v_uv               ) * 0.18;
    s += texture2D(u_tex, v_uv + 1.0 * u_texel) * 0.15;
    s += texture2D(u_tex, v_uv + 2.0 * u_texel) * 0.12;
    s += texture2D(u_tex, v_uv + 3.0 * u_texel) * 0.09;
    s += texture2D(u_tex, v_uv + 4.0 * u_texel) * 0.05;
    gl_FragColor = s;
}
)";

bool make_attachment(int w, int h, GLuint& fbo, GLuint& tex) {
    glGenTextures(1, &tex);
    glBindTexture(GL_TEXTURE_2D, tex);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, w, h, 0,
                 GL_RGBA, GL_UNSIGNED_BYTE, nullptr);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S,     GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T,     GL_CLAMP_TO_EDGE);
    glGenFramebuffers(1, &fbo);
    glBindFramebuffer(GL_FRAMEBUFFER, fbo);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0,
                           GL_TEXTURE_2D, tex, 0);
    GLenum st = glCheckFramebufferStatus(GL_FRAMEBUFFER);
    if (st != GL_FRAMEBUFFER_COMPLETE) {
        fprintf(stderr, "huskyfe: blur FBO incomplete 0x%x\n", st);
        return false;
    }
    return true;
}

}

bool Blur::init(int work_w, int work_h) {
    w_ = work_w;
    h_ = work_h;

    program_h_ = gl::build_program(VS, FS);
    program_v_ = gl::build_program(VS, FS);
    if (!program_h_ || !program_v_) return false;

    hl_pos_   = glGetAttribLocation (program_h_, "a_pos");
    hl_tex_   = glGetUniformLocation(program_h_, "u_tex");
    hl_texel_ = glGetUniformLocation(program_h_, "u_texel");
    vl_pos_   = glGetAttribLocation (program_v_, "a_pos");
    vl_tex_   = glGetUniformLocation(program_v_, "u_tex");
    vl_texel_ = glGetUniformLocation(program_v_, "u_texel");

    static const float quad[] = {
        -1.0f, -1.0f,
         1.0f, -1.0f,
        -1.0f,  1.0f,
         1.0f,  1.0f,
    };
    glGenBuffers(1, &vbo_);
    glBindBuffer(GL_ARRAY_BUFFER, vbo_);
    glBufferData(GL_ARRAY_BUFFER, sizeof(quad), quad, GL_STATIC_DRAW);

    if (!make_attachment(w_, h_, fbo_a_, tex_a_)) return false;
    if (!make_attachment(w_, h_, fbo_b_, tex_b_)) return false;
    glBindFramebuffer(GL_FRAMEBUFFER, 0);

    return gl::check_error("Blur::init");
}

void Blur::shutdown() {
    if (fbo_a_)     glDeleteFramebuffers(1, &fbo_a_);
    if (tex_a_)     glDeleteTextures(1, &tex_a_);
    if (fbo_b_)     glDeleteFramebuffers(1, &fbo_b_);
    if (tex_b_)     glDeleteTextures(1, &tex_b_);
    if (vbo_)       glDeleteBuffers(1, &vbo_);
    if (program_h_) glDeleteProgram(program_h_);
    if (program_v_) glDeleteProgram(program_v_);
    fbo_a_ = tex_a_ = fbo_b_ = tex_b_ = vbo_ = 0;
    program_h_ = program_v_ = 0;
}

GLuint Blur::draw(GLuint input_tex, int caller_w, int caller_h) {
    GLint prev_fbo = 0;
    glGetIntegerv(GL_FRAMEBUFFER_BINDING, &prev_fbo);
    glDisable(GL_BLEND);
    glBindBuffer(GL_ARRAY_BUFFER, vbo_);


    glBindFramebuffer(GL_FRAMEBUFFER, fbo_a_);
    glViewport(0, 0, w_, h_);
    glUseProgram(program_h_);
    glEnableVertexAttribArray((GLuint)hl_pos_);
    glVertexAttribPointer((GLuint)hl_pos_, 2, GL_FLOAT, GL_FALSE, 0, nullptr);
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, input_tex);
    glUniform1i(hl_tex_, 0);
    glUniform2f(hl_texel_, 1.0f / (float)w_, 0.0f);
    glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
    glDisableVertexAttribArray((GLuint)hl_pos_);


    glBindFramebuffer(GL_FRAMEBUFFER, fbo_b_);
    glViewport(0, 0, w_, h_);
    glUseProgram(program_v_);
    glEnableVertexAttribArray((GLuint)vl_pos_);
    glVertexAttribPointer((GLuint)vl_pos_, 2, GL_FLOAT, GL_FALSE, 0, nullptr);
    glBindTexture(GL_TEXTURE_2D, tex_a_);
    glUniform1i(vl_tex_, 0);
    glUniform2f(vl_texel_, 0.0f, 1.0f / (float)h_);
    glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
    glDisableVertexAttribArray((GLuint)vl_pos_);


    glBindFramebuffer(GL_FRAMEBUFFER, (GLuint)prev_fbo);
    glViewport(0, 0, caller_w, caller_h);
    return tex_b_;
}

}
