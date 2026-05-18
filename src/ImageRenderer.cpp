#include "ImageRenderer.h"

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
uniform   sampler2D  u_tex;
uniform   highp float u_alpha;
uniform   highp float u_swap_rb;
void main() {
    vec4 c = texture2D(u_tex, v_uv);
    vec3 rgb = mix(c.rgb, c.bgr, u_swap_rb);
    gl_FragColor = vec4(rgb, c.a * u_alpha);
}
)";


constexpr const char* FS_EXT = R"(
#extension GL_OES_EGL_image_external : require
precision highp float;
varying   highp vec2 v_uv;
uniform   samplerExternalOES u_tex;
uniform   highp float u_alpha;
uniform   highp float u_swap_rb;
void main() {
    vec4 c = texture2D(u_tex, v_uv);
    vec3 rgb = mix(c.rgb, c.bgr, u_swap_rb);
    gl_FragColor = vec4(rgb, c.a * u_alpha);
}
)";

}

bool ImageRenderer::init() {
    program_ = gl::build_program(VS, FS);
    if (!program_) return false;

    loc_pos_     = glGetAttribLocation (program_, "a_pos");
    loc_uv_      = glGetAttribLocation (program_, "a_uv");
    loc_xform_   = glGetUniformLocation(program_, "u_xform");
    loc_tex_     = glGetUniformLocation(program_, "u_tex");
    loc_alpha_   = glGetUniformLocation(program_, "u_alpha");
    loc_swap_rb_ = glGetUniformLocation(program_, "u_swap_rb");


    program_ext_ = gl::build_program(VS, FS_EXT);
    if (program_ext_) {
        loc_pos_ext_     = glGetAttribLocation (program_ext_, "a_pos");
        loc_uv_ext_      = glGetAttribLocation (program_ext_, "a_uv");
        loc_xform_ext_   = glGetUniformLocation(program_ext_, "u_xform");
        loc_tex_ext_     = glGetUniformLocation(program_ext_, "u_tex");
        loc_alpha_ext_   = glGetUniformLocation(program_ext_, "u_alpha");
        loc_swap_rb_ext_ = glGetUniformLocation(program_ext_, "u_swap_rb");
    }

    glGenBuffers(1, &vbo_);
    return gl::check_error("ImageRenderer::init");
}

void ImageRenderer::shutdown() {
    if (vbo_)         glDeleteBuffers(1, &vbo_);
    if (program_)     glDeleteProgram(program_);
    if (program_ext_) glDeleteProgram(program_ext_);
    vbo_ = 0; program_ = 0; program_ext_ = 0;
}

void ImageRenderer::begin(const float xform[4], GLuint texture, bool swap_rb) {
    in_external_ = false;
    glUseProgram(program_);
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, texture);
    glUniform1i (loc_tex_,    0);
    glUniform4fv(loc_xform_,  1, xform);
    if (loc_swap_rb_ >= 0)
        glUniform1f(loc_swap_rb_, swap_rb ? 1.0f : 0.0f);
    glBindBuffer(GL_ARRAY_BUFFER, vbo_);
    glEnableVertexAttribArray((GLuint)loc_pos_);
    glEnableVertexAttribArray((GLuint)loc_uv_);
    glVertexAttribPointer((GLuint)loc_pos_, 2, GL_FLOAT, GL_FALSE, 16, (void*)0);
    glVertexAttribPointer((GLuint)loc_uv_,  2, GL_FLOAT, GL_FALSE, 16, (void*)8);
}

void ImageRenderer::begin_external(const float xform[4], GLuint texture,
                                   bool swap_rb) {


    if (!program_ext_) { begin(xform, texture); return; }
    constexpr GLenum TEX_EXT = 0x8D65;
    in_external_ = true;
    glUseProgram(program_ext_);
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(TEX_EXT, texture);
    glUniform1i (loc_tex_ext_,    0);
    glUniform4fv(loc_xform_ext_,  1, xform);
    if (loc_swap_rb_ext_ >= 0)
        glUniform1f(loc_swap_rb_ext_, swap_rb ? 1.0f : 0.0f);
    glBindBuffer(GL_ARRAY_BUFFER, vbo_);
    glEnableVertexAttribArray((GLuint)loc_pos_ext_);
    glEnableVertexAttribArray((GLuint)loc_uv_ext_);
    glVertexAttribPointer((GLuint)loc_pos_ext_, 2, GL_FLOAT, GL_FALSE, 16, (void*)0);
    glVertexAttribPointer((GLuint)loc_uv_ext_,  2, GL_FLOAT, GL_FALSE, 16, (void*)8);
}

void ImageRenderer::end() {
    if (in_external_) {
        glDisableVertexAttribArray((GLuint)loc_pos_ext_);
        glDisableVertexAttribArray((GLuint)loc_uv_ext_);
    } else {
        glDisableVertexAttribArray((GLuint)loc_pos_);
        glDisableVertexAttribArray((GLuint)loc_uv_);
    }
    glUseProgram(0);
    in_external_ = false;
}

void ImageRenderer::draw(float x, float y, float w, float h,
                         float u0, float v0, float u1, float v1,
                         float opacity) {
    const float quad[24] = {
        x,     y,     u0, v0,
        x + w, y,     u1, v0,
        x,     y + h, u0, v1,
        x + w, y,     u1, v0,
        x + w, y + h, u1, v1,
        x,     y + h, u0, v1,
    };
    glBufferData(GL_ARRAY_BUFFER, sizeof(quad), quad, GL_STREAM_DRAW);
    glUniform1f(in_external_ ? loc_alpha_ext_ : loc_alpha_, opacity);
    glDrawArrays(GL_TRIANGLES, 0, 6);
}

}
