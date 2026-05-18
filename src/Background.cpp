#include "Background.h"

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <sstream>
#include <string>

namespace huskyfe {

namespace {

constexpr const char* VS_COMMON = R"(
precision highp float;
attribute vec2 a_pos;
varying   highp vec2 v_uv;
void main() {
    gl_Position = vec4(a_pos, 0.0, 1.0);
    v_uv = a_pos * 0.5 + 0.5;
}
)";


constexpr const char* FS_BG = R"(
precision highp float;
varying   highp vec2  v_uv;
uniform   highp float u_time;
uniform   highp float u_dim;
uniform   highp vec2  u_res;

vec3 rgb2hsv(vec3 c) {
    vec4 K = vec4(0.0, -1.0/3.0, 2.0/3.0, -1.0);
    vec4 p = mix(vec4(c.bg, K.wz), vec4(c.gb, K.xy), step(c.b, c.g));
    vec4 q = mix(vec4(p.xyw, c.r), vec4(c.r, p.yzx), step(p.x, c.r));
    float d = q.x - min(q.w, q.y);
    float e = 1.0e-10;
    return vec3(abs(q.z + (q.w - q.y) / (6.0 * d + e)),
                d / (q.x + e), q.x);
}

vec3 hsv2rgb(vec3 c) {
    vec4 K = vec4(1.0, 2.0/3.0, 1.0/3.0, 3.0);
    vec3 p = abs(fract(c.xxx + K.xyz) * 6.0 - K.www);
    return c.z * mix(K.xxx, clamp(p - K.xxx, 0.0, 1.0), c.y);
}

void main() {
    vec2 ires       = u_res;
    vec2 frag_coord = vec2(v_uv.x, 1.0 - v_uv.y) * ires;
    vec2 uv         = frag_coord / ires;

    vec3 c = mix(
        mix(vec3(0.70, 0.20, 0.20), vec3(0.40, 0.10, 0.10), uv.x),
        mix(vec3(0.45, 0.10, 0.10), vec3(0.80, 0.30, 0.50), uv.x),
        uv.y);

    vec3 hsv = rgb2hsv(c);
    hsv.x = fract(hsv.x + u_time * 0.02);
    c = hsv2rgb(hsv);

    c *= 1.0 - u_dim * 0.85;

    gl_FragColor = vec4(c, 1.0);
}
)";


constexpr const char* FS_SPHERES = R"(
precision highp float;
precision highp int;
varying   highp vec2  v_uv;
uniform   highp float u_time;
uniform   highp float u_dim;
uniform   highp vec2  u_res;

// Per-pixel cached sphere transforms. Populated once in main().
vec3  g_off[16];
float g_rad[16];

float map(vec3 p) {
    float d = 2.0;
    for (int i = 0; i < 16; i++) {
        float di = length(p + g_off[i]) - g_rad[i];
        // opSmoothUnion(d, di, k=0.4) inlined; 0.5/k = 1.25.
        float h  = clamp(0.5 + 1.25*(d - di), 0.0, 1.0);
        d = mix(d, di, h) - 0.4*h*(1.0 - h);
    }
    return d;
}

vec3 calcNormal(vec3 p) {
    const float e = 1e-3;
    float d0 = map(p);
    return normalize(vec3(
        map(p + vec3(e, 0.0, 0.0)) - d0,
        map(p + vec3(0.0, e, 0.0)) - d0,
        map(p + vec3(0.0, 0.0, e)) - d0));
}

void main() {
    float ts = u_time * 0.4;
    for (int i = 0; i < 16; i++) {
        float fi = float(i);
        float h1 = fract(fi*412.531 + 0.513);
        float h2 = fract(fi*412.531 + 0.5124);
        float t  = ts * (h1 - 0.5) * 2.0;
        g_off[i] = sin(t + fi*vec3(52.5126, 64.6274, 632.25))
                   * vec3(2.0, 2.0, 0.8);
        g_rad[i] = mix(0.5, 1.0, h2);
    }

    vec2 frag_coord = vec2(v_uv.x, 1.0 - v_uv.y) * u_res;
    vec2 uv         = frag_coord / u_res;
    vec3 rayOri = vec3((uv - 0.5) * vec2(u_res.x/u_res.y, 1.0) * 6.0, 3.0);
    vec3 rayDir = vec3(0.0, 0.0, -1.0);

    float depth = 0.0;
    vec3  p     = vec3(0.0);
    for (int i = 0; i < 64; i++) {
        p = rayOri + rayDir * depth;
        float dist = map(p);
        depth += dist;
        if (dist < 1e-3 || depth > 6.0) break;
    }
    depth = min(6.0, depth);

    vec3 col;
    if (depth < 6.0) {
        vec3  n = calcNormal(p);
        float b = max(0.0, dot(n, vec3(0.577)));
        col = (0.5 + 0.5 * cos((b + ts*3.0) + uv.xyx*2.0
                                 + vec3(0.0, 2.0, 4.0)))
              * (0.85 + b*0.35);
    } else {
        // Miss: skip the 4 map() calls a normal would cost; color
        // collapses to the b=0 baseline of the same cosine palette.
        col = (0.5 + 0.5 * cos(ts*3.0 + uv.xyx*2.0
                                 + vec3(0.0, 2.0, 4.0))) * 0.85;
    }
    col *= exp(-depth * 0.15);
    col *= 1.0 - u_dim * 0.85;
    gl_FragColor = vec4(col, 1.0);
}
)";


constexpr const char* FS_FRACTAL = R"(
precision highp float;
precision highp int;
varying   highp vec2  v_uv;
uniform   highp float u_time;
uniform   highp float u_dim;
uniform   highp vec2  u_res;

vec2 tanh_emu(vec2 x) {
    vec2 cx = clamp(x, -15.0, 15.0);
    vec2 e  = exp(2.0 * cx);
    return (e - 1.0) / (e + 1.0);
}

void main() {
    vec2 frag_coord = vec2(v_uv.x, 1.0 - v_uv.y) * u_res;
    vec2 u = frag_coord;
    vec2 v = u_res;
    u = 0.2 * (u + u - v) / v.y;

    vec4 o = vec4(1.0, 2.0, 3.0, 0.0);
    vec4 z = o;
    float a = 0.5;
    float t = u_time;

    for (int k = 0; k < 18; k++) {
        float i = float(k) + 1.0;
        t += 1.0;
        a += 0.03;
        v = cos(t - 7.0 * u * pow(a, i)) - 5.0 * u;
        vec4 c = cos((i + 0.02 * t) - z.wxzw * 11.0);
        mat2 m = mat2(c.x, c.y, c.z, c.w);
        u = u * m;
        u += tanh_emu(40.0 * dot(u, u) * cos(100.0 * u.yx + t)) / 200.0
           + 0.2 * a * u
           + cos(4.0 / exp(dot(o, o) / 100.0) + t) / 300.0;
        vec2 s = sin(1.5 * u / (0.5 - dot(u, u)) - 9.0 * u.yx + t);
        o += (1.0 + cos(z + t)) / length((1.0 + i * dot(v, v)) * s);
    }
    o = 25.6 / (min(o, vec4(13.0)) + 164.0 / o) - dot(u, u) / 250.0;

    vec3 col = o.rgb;
    col *= 1.0 - u_dim * 0.85;
    gl_FragColor = vec4(col, 1.0);
}
)";


constexpr const char* FS_BLIT = R"(
precision highp float;
varying   highp vec2  v_uv;
uniform   sampler2D   u_tex;
void main() {
    gl_FragColor = vec4(texture2D(u_tex, v_uv).rgb, 1.0);
}
)";

}

bool Background::init(int screen_w, int screen_h, int downscale, float max_redraw_hz) {
    if (downscale < 1) downscale = 1;
    if (max_redraw_hz < 1.0f) max_redraw_hz = 1.0f;
    screen_w_     = screen_w;
    screen_h_     = screen_h;
    fbo_w_        = screen_w  / downscale;
    fbo_h_        = screen_h  / downscale;
    min_interval_ = 1.0f / max_redraw_hz;

    program_bg_   = gl::build_program(VS_COMMON, FS_BG);
    program_blit_ = gl::build_program(VS_COMMON, FS_BLIT);
    if (!program_bg_ || !program_blit_) return false;
    mode_         = Mode::PROGRAM;
    current_sel_  = "rgb";

    loc_bg_pos_   = glGetAttribLocation (program_bg_, "a_pos");
    loc_bg_time_  = glGetUniformLocation(program_bg_, "u_time");
    loc_bg_dim_   = glGetUniformLocation(program_bg_, "u_dim");
    loc_bg_res_   = glGetUniformLocation(program_bg_, "u_res");

    loc_blit_pos_ = glGetAttribLocation (program_blit_, "a_pos");
    loc_blit_tex_ = glGetUniformLocation(program_blit_, "u_tex");

    static const float quad[] = {
        -1.0f, -1.0f,
         1.0f, -1.0f,
        -1.0f,  1.0f,
         1.0f,  1.0f,
    };
    glGenBuffers(1, &vbo_);
    glBindBuffer(GL_ARRAY_BUFFER, vbo_);
    glBufferData(GL_ARRAY_BUFFER, sizeof(quad), quad, GL_STATIC_DRAW);

    glGenTextures(1, &tex_);
    glBindTexture(GL_TEXTURE_2D, tex_);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, fbo_w_, fbo_h_, 0,
                 GL_RGBA, GL_UNSIGNED_BYTE, nullptr);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);

    glGenFramebuffers(1, &fbo_);
    glBindFramebuffer(GL_FRAMEBUFFER, fbo_);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0,
                           GL_TEXTURE_2D, tex_, 0);
    GLenum st = glCheckFramebufferStatus(GL_FRAMEBUFFER);
    if (st != GL_FRAMEBUFFER_COMPLETE) {
        fprintf(stderr, "huskyfe: bg FBO incomplete 0x%x\n", st);
        return false;
    }
    glBindFramebuffer(GL_FRAMEBUFFER, 0);

    return gl::check_error("Background::init");
}

void Background::shutdown() {
    if (fbo_)         glDeleteFramebuffers(1, &fbo_);
    if (tex_)         glDeleteTextures(1, &tex_);
    if (vbo_)         glDeleteBuffers(1, &vbo_);
    if (program_bg_)   glDeleteProgram(program_bg_);
    if (program_blit_) glDeleteProgram(program_blit_);
    fbo_ = 0; tex_ = 0; vbo_ = 0;
    program_bg_ = 0; program_blit_ = 0;
}

void Background::draw(float t_seconds, float dim) {
    GLint prev_fbo = 0;
    glGetIntegerv(GL_FRAMEBUFFER_BINDING, &prev_fbo);

    glDisable(GL_BLEND);
    glBindBuffer(GL_ARRAY_BUFFER, vbo_);


    if (mode_ == Mode::NONE) {
        if (need_clear_) {
            glBindFramebuffer(GL_FRAMEBUFFER, fbo_);
            glViewport(0, 0, fbo_w_, fbo_h_);
            glClearColor(0.0f, 0.0f, 0.0f, 1.0f);
            glClear(GL_COLOR_BUFFER_BIT);
            need_clear_ = false;
        }
        glBindFramebuffer(GL_FRAMEBUFFER, (GLuint)prev_fbo);
        glViewport(0, 0, screen_w_, screen_h_);
        glUseProgram(program_blit_);
        glEnableVertexAttribArray((GLuint)loc_blit_pos_);
        glVertexAttribPointer((GLuint)loc_blit_pos_, 2, GL_FLOAT, GL_FALSE, 0, nullptr);
        glActiveTexture(GL_TEXTURE0);
        glBindTexture(GL_TEXTURE_2D, tex_);
        glUniform1i(loc_blit_tex_, 0);
        glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
        glDisableVertexAttribArray((GLuint)loc_blit_pos_);
        return;
    }


    float dt_since   = t_seconds - last_redraw_t_;
    bool  dim_change = std::abs(dim - last_dim_) > 0.01f;
    bool  redraw_bg  = (dt_since >= min_interval_) || dim_change;


    if (paused_ && !dim_change) redraw_bg = false;

    if (redraw_bg) {
        glBindFramebuffer(GL_FRAMEBUFFER, fbo_);
        glViewport(0, 0, fbo_w_, fbo_h_);
        glUseProgram(program_bg_);
        glEnableVertexAttribArray((GLuint)loc_bg_pos_);
        glVertexAttribPointer((GLuint)loc_bg_pos_, 2, GL_FLOAT, GL_FALSE, 0, nullptr);
        glUniform1f(loc_bg_time_, t_seconds);
        glUniform1f(loc_bg_dim_,  dim);
        glUniform2f(loc_bg_res_,  (float)fbo_w_, (float)fbo_h_);
        glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
        glDisableVertexAttribArray((GLuint)loc_bg_pos_);
        last_redraw_t_ = t_seconds;
        last_dim_      = dim;
    }


    glBindFramebuffer(GL_FRAMEBUFFER, (GLuint)prev_fbo);
    glViewport(0, 0, screen_w_, screen_h_);
    glUseProgram(program_blit_);
    glEnableVertexAttribArray((GLuint)loc_blit_pos_);
    glVertexAttribPointer((GLuint)loc_blit_pos_, 2, GL_FLOAT, GL_FALSE, 0, nullptr);
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, tex_);
    glUniform1i(loc_blit_tex_, 0);
    glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
    glDisableVertexAttribArray((GLuint)loc_blit_pos_);
}

bool Background::set_shader(const std::string& sel) {
    if (sel == "none") {
        mode_        = Mode::NONE;
        current_sel_ = "none";
        need_clear_  = true;
        return true;
    }

    const char* fs_src = nullptr;
    std::string file_buf;
    if (sel == "rgb") {
        fs_src = FS_BG;
    } else if (sel == "spheres") {
        fs_src = FS_SPHERES;
    } else if (sel == "fractal") {
        fs_src = FS_FRACTAL;
    } else {
        std::ifstream f(sel);
        if (!f) {
            fprintf(stderr, "huskyfe: bg shader not readable: %s\n", sel.c_str());
            return false;
        }
        std::stringstream ss; ss << f.rdbuf();
        file_buf = ss.str();
        fs_src   = file_buf.c_str();
    }

    GLuint prog = gl::build_program(VS_COMMON, fs_src);
    if (!prog) {
        fprintf(stderr, "huskyfe: bg shader compile failed: %s\n", sel.c_str());
        return false;
    }

    if (program_bg_) glDeleteProgram(program_bg_);
    program_bg_ = prog;


    loc_bg_pos_  = glGetAttribLocation (program_bg_, "a_pos");
    loc_bg_time_ = glGetUniformLocation(program_bg_, "u_time");
    loc_bg_dim_  = glGetUniformLocation(program_bg_, "u_dim");
    loc_bg_res_  = glGetUniformLocation(program_bg_, "u_res");

    mode_           = Mode::PROGRAM;
    current_sel_    = sel;
    last_redraw_t_  = -1e9f;
    last_dim_       = -1.0f;
    return true;
}

}
