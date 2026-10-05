// Copyright 2019 The Android Open Source Project
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

#pragma once

#include <climits>
#include <limits>
#include <string>
#include <vector>

#include "gfxstream/TypeTraits.h"
#include "gfxstream/common/logging.h"
#include "render-utils/stream.h"

namespace gfxstream {
namespace host {

//
// Save/load operations for different types.
//

bool saveBufferRaw(Stream* stream, char* buffer, uint32_t len);
bool loadBufferRaw(Stream* stream, char* buffer);

template <class T, class = gfxstream::base::enable_if<std::is_standard_layout<T>>>
bool saveBuffer(Stream* stream, const std::vector<T>& buffer) {
    if (buffer.size() > std::numeric_limits<uint32_t>::max()) {
        GFXSTREAM_ERROR("%s: buffer size %zu exceeds uint32_t max", __FUNCTION__, buffer.size());
        return false;
    }
    stream->putBe32(static_cast<uint32_t>(buffer.size()));
    const size_t bytesToWrite = sizeof(T) * buffer.size();
    ssize_t written = stream->write(buffer.data(), bytesToWrite);
    if (written < 0 || static_cast<size_t>(written) != bytesToWrite) {
        GFXSTREAM_ERROR("%s: saveBuffer failed to write %zu bytes from stream, got %zd",
                        __FUNCTION__, bytesToWrite, written);
        return false;
    }
    return true;
}

template <class T, class = gfxstream::base::enable_if<std::is_standard_layout<T>>>
bool loadBuffer(Stream* stream, std::vector<T>* buffer) {
    const uint32_t len = stream->getBe32();
    const size_t bytesToRead = size_t(len) * sizeof(T);
    buffer->resize(len);
    ssize_t ret = stream->read(buffer->data(), bytesToRead);
    if (ret < 0 || static_cast<size_t>(ret) != bytesToRead) {
        GFXSTREAM_ERROR("%s: loadBuffer failed to read %zu bytes from stream, got %zd",
                        __FUNCTION__, bytesToRead, ret);
        return false;
    }
    return true;
}

template <class Container,
          class = gfxstream::base::enable_if<std::is_standard_layout<typename Container::value_type>>>
bool saveBuffer(Stream* stream, const Container& buffer) {
    if (buffer.size() > std::numeric_limits<uint32_t>::max()) {
        GFXSTREAM_ERROR("%s: buffer size %zu exceeds uint32_t max", __FUNCTION__, buffer.size());
        return false;
    }
    stream->putBe32(static_cast<uint32_t>(buffer.size()));
    const size_t bytesToWrite = sizeof(typename Container::value_type) * buffer.size();
    ssize_t written = stream->write(buffer.data(), bytesToWrite);
    if (written < 0 || static_cast<size_t>(written) != bytesToWrite) {
        GFXSTREAM_ERROR("%s: saveBuffer failed to write %zu bytes from stream, got %zd",
                        __FUNCTION__, bytesToWrite, written);
        return false;
    }
    return true;
}

template <class Container,
          class = gfxstream::base::enable_if<std::is_standard_layout<typename Container::value_type>>>
bool loadBuffer(Stream* stream, Container* buffer) {
    const uint32_t len = stream->getBe32();
    const size_t bytesToRead = size_t(len) * sizeof(typename Container::value_type);
    buffer->clear();
    buffer->resize_noinit(len);
    ssize_t ret = stream->read(buffer->data(), bytesToRead);
    if (ret < 0 || static_cast<size_t>(ret) != bytesToRead) {
        GFXSTREAM_ERROR("%s: loadBuffer failed to read %zu bytes from stream, got %zd",
                        __FUNCTION__, bytesToRead, ret);
        return false;
    }
    return true;
}

template <class T, class SaveFunc>
bool saveBuffer(Stream* stream, const std::vector<T>& buffer, SaveFunc&& saver) {
    if (buffer.size() > std::numeric_limits<uint32_t>::max()) {
        GFXSTREAM_ERROR("%s: buffer size %zu exceeds uint32_t max", __FUNCTION__, buffer.size());
        return false;
    }
    stream->putBe32(static_cast<uint32_t>(buffer.size()));
    for (const auto& val : buffer) {
        saver(stream, val);
    }
    return true;
}

template <class T>
bool saveBuffer(Stream* stream, const T* buffer, size_t numElts) {
    if (numElts > std::numeric_limits<uint32_t>::max()) {
        GFXSTREAM_ERROR("%s: buffer size %zu exceeds uint32_t max", __FUNCTION__, numElts);
        return false;
    }
    stream->putBe32(static_cast<uint32_t>(numElts));
    const size_t bytesToWrite = sizeof(T) * numElts;
    ssize_t written = stream->write(buffer, bytesToWrite);
    if (written < 0 || static_cast<size_t>(written) != bytesToWrite) {
        GFXSTREAM_ERROR("%s: saveBuffer failed to write %zu bytes from stream, got %zd",
                        __FUNCTION__, bytesToWrite, written);
        return false;
    }
    return true;
}

template <class T>
bool loadBufferPtr(Stream* stream, T* out) {
    auto len = stream->getBe32();
    const size_t bytesToRead = len * sizeof(T);
    ssize_t ret = stream->read(out, bytesToRead);
    if (ret < 0 || static_cast<size_t>(ret) != bytesToRead) {
        GFXSTREAM_ERROR("%s: loadBufferPtr failed to read %zu bytes from stream, got %zd",
                        __FUNCTION__, bytesToRead, ret);
        return false;
    }
    return true;
}

template <class T, class LoadFunc>
bool loadBuffer(Stream* stream, std::vector<T>* buffer, LoadFunc&& loader) {
    const uint32_t len = stream->getBe32();
    buffer->clear();
    buffer->reserve(len);
    for (uint32_t i = 0; i < len; i++) {
        buffer->emplace_back(loader(stream));
    }
    return true;
}

template <class Collection, class SaveFunc>
bool saveCollection(Stream* stream, const Collection& c, SaveFunc&& saver) {
    if (c.size() > std::numeric_limits<uint32_t>::max()) {
        GFXSTREAM_ERROR("%s: buffer size %zu exceeds uint32_t max", __FUNCTION__, c.size());
        return false;
    }
    stream->putBe32(static_cast<uint32_t>(c.size()));
    for (const auto& val : c) {
        saver(stream, val);
    }
    return true;
}

template <class Collection, class SaveFunc>
bool trySaveCollection(Stream* stream, const Collection& c, const size_t maxSize, SaveFunc&& saver) {
    if (c.size() > maxSize) {
        return false;
    }
    stream->putBe32(c.size());
    for (const auto& val : c) {
        bool savedOk = saver(stream, val);
        if (!savedOk) {
            return false;
        }
    }
    return true;
}

template <class Collection, class LoadFunc>
bool loadCollection(Stream* stream, Collection* c, LoadFunc&& loader) {
    const uint32_t size = stream->getBe32();
    for (uint32_t i = 0; i < size; ++i) {
        c->emplace(loader(stream));
    }
    return true;
}

template <class Collection, class LoadFunc>
bool tryLoadCollection(Stream* stream, Collection* c, const size_t maxSize, LoadFunc&& loader) {
    const uint32_t size = stream->getBe32();
    if (size > maxSize) {
        return false;
    }
    for (uint32_t i = 0; i < size; ++i) {
        auto loadedVal = loader(stream);
        if (!loadedVal.has_value()) {
            return false;
        }
        c->emplace(std::move(*loadedVal));
    }
    return true;
}

bool saveStringArray(Stream* stream, const char* const* strings, uint32_t count);
std::vector<std::string> loadStringArray(Stream* stream);

}  // namespace host
}  // namespace gfxstream
