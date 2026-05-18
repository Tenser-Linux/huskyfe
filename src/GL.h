#pragma once
#include <GLES3/gl3.h>

namespace huskyfe::gl {

GLuint compile_shader(GLenum type, const char* src);
GLuint link_program(GLuint vs, GLuint fs);
GLuint build_program(const char* vs_src, const char* fs_src);
bool   check_error(const char* tag);

}
