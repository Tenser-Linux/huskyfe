#include "GL.h"

#include <cstdio>
#include <vector>

namespace huskyfe::gl {

GLuint compile_shader(GLenum type, const char* src) {
    GLuint sh = glCreateShader(type);
    glShaderSource(sh, 1, &src, nullptr);
    glCompileShader(sh);
    GLint ok = 0;
    glGetShaderiv(sh, GL_COMPILE_STATUS, &ok);
    if (!ok) {
        GLint len = 0;
        glGetShaderiv(sh, GL_INFO_LOG_LENGTH, &len);
        std::vector<char> log(len > 0 ? len + 1 : 1);
        glGetShaderInfoLog(sh, (GLsizei)log.size(), nullptr, log.data());
        fprintf(stderr, "huskyfe: shader compile failed (%s):\n%s\n",
                type == GL_VERTEX_SHADER ? "VS" : "FS", log.data());
        glDeleteShader(sh);
        return 0;
    }
    return sh;
}

GLuint link_program(GLuint vs, GLuint fs) {
    GLuint p = glCreateProgram();
    glAttachShader(p, vs);
    glAttachShader(p, fs);
    glLinkProgram(p);
    GLint ok = 0;
    glGetProgramiv(p, GL_LINK_STATUS, &ok);
    if (!ok) {
        GLint len = 0;
        glGetProgramiv(p, GL_INFO_LOG_LENGTH, &len);
        std::vector<char> log(len > 0 ? len + 1 : 1);
        glGetProgramInfoLog(p, (GLsizei)log.size(), nullptr, log.data());
        fprintf(stderr, "huskyfe: program link failed:\n%s\n", log.data());
        glDeleteProgram(p);
        return 0;
    }
    return p;
}

GLuint build_program(const char* vs_src, const char* fs_src) {
    GLuint vs = compile_shader(GL_VERTEX_SHADER, vs_src);
    if (!vs) return 0;
    GLuint fs = compile_shader(GL_FRAGMENT_SHADER, fs_src);
    if (!fs) { glDeleteShader(vs); return 0; }
    GLuint p = link_program(vs, fs);
    glDeleteShader(vs);
    glDeleteShader(fs);
    return p;
}

bool check_error(const char* tag) {
    GLenum e = glGetError();
    if (e == GL_NO_ERROR) return true;
    fprintf(stderr, "huskyfe: GL error 0x%x at %s\n", e, tag);
    return false;
}

}
