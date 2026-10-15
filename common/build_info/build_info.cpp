// Copyright 2026 Google Inc.
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//     http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

#include "gfxstream/common/build_info.h"

#include <string>

#if defined(GFXSTREAM_BUILD_INFO_USE_ANDROID_LIBBUILDVERSION)
#include <build/version.h>
#elif defined(GFXSTREAM_BUILD_INFO_USE_CUSTOM_LIBRARY)
GFXSTREAM_BUILD_INFO_USE_LIBRARY_INCLUDE_PLACEHOLDER
#endif

namespace gfxstream {

std::string GetGfxstreamBuildId() {
#if defined(GFXSTREAM_BUILD_INFO_USE_ANDROID_LIBBUILDVERSION)
    return std::string("Android Build ") + android::build::GetBuildNumber();
#elif defined(GFXSTREAM_BUILD_INFO_USE_CUSTOM_LIBRARY)
    GFXSTREAM_BUILD_INFO_USE_LIBRARY_IMPL_PLACEHOLDER
#elif defined(GFXSTREAM_BUILD_INFO_USE_CUSTOM_STRING)
    return GFXSTREAM_BUILD_INFO_CUSTOM_STRING_VALUE;
#else
    return "UNKNOWN";
#endif
}

} // namespace gfxstream
