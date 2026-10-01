// Copyright (C) 2018 The Android Open Source Project
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
// http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

#include "gfxstream/host/testing/ShaderUtils.h"

#include "gfxstream/files/PathUtils.h"
#include "gfxstream/Optional.h"
#include "gfxstream/system/System.h"

#include "OpenGLESDispatch/OpenGLDispatchLoader.h"

#include <fstream>
#include <vector>

#include <fcntl.h>
#include <stdio.h>

using gfxstream::base::Optional;
using gfxstream::base::pj;

#define DEBUG 0

#if DEBUG
#define D(fmt,...) fprintf(stderr, "%s: " fmt "\n", __func__, ##__VA_ARGS__);
#else
#define D(fmt,...)
#endif

#define E(fmt,...) fprintf(stderr, "%s: " fmt "\n", __func__, ##__VA_ARGS__);

namespace gfxstream {
namespace host {

GLuint compileShader(GLenum shaderType, const char* src) {
    auto gl = gl::LazyLoadedGLESv2Dispatch::get();

    GLuint shader = gl->glCreateShader(shaderType);
    gl->glShaderSource(shader, 1, (const GLchar* const*)&src, nullptr);
    gl->glCompileShader(shader);

    GLint compileStatus;
    gl->glGetShaderiv(shader, GL_COMPILE_STATUS, &compileStatus);

    if (compileStatus != GL_TRUE) {
        GLsizei infoLogLength = 0;
        gl->glGetShaderiv(shader, GL_INFO_LOG_LENGTH, &infoLogLength);
        std::vector<char> infoLog(infoLogLength + 1, 0);
        gl->glGetShaderInfoLog(shader, infoLogLength, nullptr, &infoLog[0]);
        E("fail to compile. infolog: %s", &infoLog[0]);
    }

    return shader;
}

GLint compileAndLinkShaderProgram(const char* vshaderSrc, const char* fshaderSrc) {
    auto gl = gl::LazyLoadedGLESv2Dispatch::get();

    GLuint vshader = compileShader(GL_VERTEX_SHADER, vshaderSrc);
    GLuint fshader = compileShader(GL_FRAGMENT_SHADER, fshaderSrc);

    GLuint program = gl->glCreateProgram();
    gl->glAttachShader(program, vshader);
    gl->glAttachShader(program, fshader);
    gl->glLinkProgram(program);

    GLint linkStatus;
    gl->glGetProgramiv(program, GL_LINK_STATUS, &linkStatus);

    gl->glClearColor(0.0f, 0.0f, 1.0f, 0.0f);

    if (linkStatus != GL_TRUE) {
        GLsizei infoLogLength = 0;
        gl->glGetProgramiv(program, GL_INFO_LOG_LENGTH, &infoLogLength);
        std::vector<char> infoLog(infoLogLength + 1, 0);
        gl->glGetProgramInfoLog(program, infoLogLength, nullptr, &infoLog[0]);

        E("failed to link. infolog: %s", &infoLog[0]);
    }

    return program;
}

}  // namespace host
}  // namespace gfxstream
