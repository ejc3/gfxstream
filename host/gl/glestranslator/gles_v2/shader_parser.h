/*
* Copyright 2011 The Android Open Source Project
*
* Licensed under the Apache License, Version 2.0 (the "License");
* you may not use this file except in compliance with the License.
* You may obtain a copy of the License at
*
* http://www.apache.org/licenses/LICENSE-2.0
*
* Unless required by applicable law or agreed to in writing, software
* distributed under the License is distributed on an "AS IS" BASIS,
* WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
* See the License for the specific language governing permissions and
* limitations under the License.
*/

#ifndef SHADER_PARSER_H
#define SHADER_PARSER_H

#include "gfxstream/synchronization/Lock.h"
#include "angle_shader_parser.h"
#include "gles_v2_context.h"
#include <string>
#include <common/share_group.h>
#include <functional>
#include <unordered_set>

class ShaderParser : public ObjectData {
public:
    ShaderParser(GLenum type, bool coreProfile);
    ShaderParser(gfxstream::Stream* stream);
    virtual void onSave(gfxstream::Stream* stream, unsigned int globalName) const override;
    virtual void restore(ObjectLocalName localName,
                         const getGlobalName_t& getGlobalName) override;
    void setSrc(GLsizei count, const GLchar* const* strings,
            const GLint* length);
    const std::string& getOriginalSrc() const;
    const GLchar** parsedLines();
    void           clear();
    GLenum         getType();

    // Query whether the shader parsed is valid.
    // Don't trust the value if we did not call setSrc
    bool validShader() const;

    const GLchar* getInfoLog() const;
    void setInfoLog(GLchar * infoLog);

    // If validation fails, add proper error messages
    // to the parser's info log, which is treated
    // as the actual info log from guest POV.
    void setInvalidInfoLog();

    void setCompileStatus(bool val);
    bool getCompileStatus() const { return m_compileStatus; }

    // A restore sets a compiled shader's source on the host and leaves the compile to the first
    // host use that needs it (a link of a program it is attached to, a host query of it): a
    // program restored from its binary needs no compiled shader objects. Compiles it then.
    // Another thread that needs it meanwhile waits for that compile.
    void ensureHostCompiled(GLuint globalName);
    // The guest compiles it again itself, or loads a binary into it.
    void clearHostCompilePending();
    // Whether the host's compile of this shader is of its current source: a link of a program
    // it is attached to runs what was compiled, which differs once new source is set.
    bool currentSourceCompiled() const { return m_compileStatus && m_compiledSrc == m_parsedSrc; }
    // Runs in ensureHostCompiled() between taking a pending compile and compiling. For tests.
    static void setHostCompileHookForTesting(std::function<void()> hook);

    void setDeleteStatus(bool val) { m_deleteStatus = val; }
    bool getDeleteStatus() const { return m_deleteStatus; }

    void attachProgram(GLuint program);
    void detachProgram(GLuint program);
    bool hasAttachedPrograms() const;

#ifdef USE_ANGLE_SHADER_PARSER
    const ANGLEShaderParser::ShaderLinkInfo& getShaderLinkInfo() const { return m_shaderLinkInfo; }
    ANGLEShaderParser::ShaderLinkInfo* shaderLinkInfoPtr() { return &m_shaderLinkInfo; }
#endif

    virtual GenNameInfo getGenNameInfo() const override;
    const char* getCompiledSrc() const { return m_compiledSrc.c_str(); }

private:
    gfxstream::base::Lock m_hostCompileLock;
    bool m_hostCompilePending = false;
    void convertESSLToGLSL();

    std::string m_originalSrc;
    std::string m_src;
    std::string m_parsedSrc;
    GLchar*     m_parsedLines = nullptr;
    std::string m_compiledSrc;
    std::basic_string<GLchar> m_infoLog;
    mutable gfxstream::base::Lock m_programsLock;
    std::unordered_set<GLuint> m_programs;
    GLenum      m_type = 0;
    bool        m_compileStatus = false;
    bool        m_deleteStatus = false;
    bool        m_valid = true;
#ifdef USE_ANGLE_SHADER_PARSER
    ANGLEShaderParser::ShaderLinkInfo m_shaderLinkInfo;
#endif
    bool        m_coreProfile = false;
};
#endif
