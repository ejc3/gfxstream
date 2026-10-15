# Copyright 2026 Android Open Source Project
# SPDX-License-Identifier: Apache-2.0

inc_common_build_info = include_directories('include')

files_lib_common_build_info = files(
  'build_info.cpp',
)

lib_common_build_info = static_library(
  'common_build_info',
  files_lib_common_build_info,
  include_directories: [inc_common_build_info],
  cpp_args: [
    '-DGFXSTREAM_BUILD_INFO_USE_CUSTOM_STRING=1',
    '-DGFXSTREAM_BUILD_INFO_CUSTOM_STRING_VALUE="\"Meson Build\""',
  ]
)
