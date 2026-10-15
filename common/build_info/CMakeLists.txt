# Copyright 2026 The Android Open Source Project
#
# Licensed under the Apache License, Version 2.0 (the "License");
# you may not use this file except in compliance with the License.
# You may obtain a copy of the License at
#
# http://www.apache.org/licenses/LICENSE-2.0
#
# Unless required by applicable law or agreed to in writing, software
# distributed under the License is distributed on an "AS IS" BASIS,
# WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
# See the License for the specific language governing permissions and
# limitations under the License.

add_library(
    gfxstream_common_build_info_headers
    INTERFACE)
target_include_directories(
    gfxstream_common_build_info_headers
    INTERFACE
    include)

add_library(
    gfxstream_common_build_info
    STATIC
    build_info.cpp)
target_link_libraries(
    gfxstream_common_build_info
    PUBLIC
    gfxstream_common_build_info_headers)
target_compile_definitions(
    gfxstream_common_build_info
    PRIVATE
    GFXSTREAM_BUILD_INFO_USE_CUSTOM_STRING=1
    GFXSTREAM_BUILD_INFO_CUSTOM_STRING_VALUE="\"CMake Build\"")