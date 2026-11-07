/*
* Copyright (C) 2016 The Android Open Source Project
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

#pragma once

#include "render-utils/stream.h"
#include "common/named_object.h"

#include <cstdint>
#include <functional>
#include <memory>

enum ObjectDataType {
    SHADER_DATA,
    PROGRAM_DATA,
    TEXTURE_DATA,
    BUFFER_DATA,
    RENDERBUFFER_DATA,
    FRAMEBUFFER_DATA,
    SAMPLER_DATA,
    TRANSFORMFEEDBACK_DATA,
    UNDEFINED_DATA,
};

class ObjectData;
typedef std::shared_ptr<ObjectData> ObjectDataPtr;

// Where a snapshot restore's time goes on this thread, in microseconds: glCompileShader, and
// glLinkProgram up to the program's first query. ShareGroup reports and
// resets it per restored group.
struct RestoreCost {
    uint64_t compileUs = 0;
    uint64_t linkUs = 0;
    uint64_t programs = 0;
    uint64_t programBinaries = 0;
    uint64_t shaders = 0;
};
RestoreCost& restoreCostOnThisThread();
// The share group and local name of the object a save or a restore on this thread is at:
// what a program's kept binary is filed under, so it only ever restores that same program.
struct ObjectIdentity {
    uint64_t shareGroup = 0;
    uint64_t localName = 0;
};
ObjectIdentity& currentObjectIdentityOnThisThread();
// Programs any thread restored from a kept host binary since the process started. For tests.
uint64_t programBinaryRestores();
void countProgramBinaryRestore();
// Runs when a shader a restore left to compile is compiled on use, between taking the
// compile and compiling it. For tests; set it only while no restore or GL use runs.
void setShaderCompileOnUseHookForTesting(std::function<void()> hook);
const std::function<void()>& shaderCompileOnUseHookForTesting();

extern NamedObjectType ObjectDataType2NamedObjectType(ObjectDataType objDataType);

class ObjectData
{
public:
    ObjectData(ObjectDataType type = UNDEFINED_DATA): m_dataType(type) {}
    ObjectData(gfxstream::Stream* stream, ObjectDataType expectedType = UNDEFINED_DATA);
    ObjectDataType getDataType() { return m_dataType; };
    virtual ~ObjectData() = default;
    virtual void onSave(gfxstream::Stream* stream, unsigned int globalName) const = 0;
    typedef std::function<ObjectDataPtr(NamedObjectType p_type,
            ObjectLocalName p_localName, gfxstream::Stream* stream)>
                loadObject_t;
    typedef std::function<const ObjectDataPtr(NamedObjectType,
            ObjectLocalName)> const getObjDataPtr_t;
    typedef std::function<int(NamedObjectType, ObjectLocalName)> const
            getGlobalName_t;
    // postLoad: setup references after loading all ObjectData from snapshot
    // in one share group
    virtual void postLoad(const getObjDataPtr_t& getObjDataPtr);
    // restore: restore object data back to hardware GPU.
    //          It must be called before restoring hardware context states,
    //          because it messes up context object bindings.
    virtual void restore(ObjectLocalName localName,
                         const getGlobalName_t& getGlobalName) = 0;
    virtual GenNameInfo getGenNameInfo() const;
    bool needRestore() const;
private:
    ObjectDataType m_dataType;
    bool m_needRestore = false;
};

