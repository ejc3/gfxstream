# Copyright 2026 Android Open Source Project
# SPDX-License-Identifier: Apache-2.0

inc_host_native_window = include_directories('include')

files_lib_host_native_window = files('native_sub_window_stub.cpp')

if host_machine.system() == 'darwin'
  files_lib_host_native_window += files('native_sub_window_cocoa.mm')
elif host_machine.system() == 'windows'
  files_lib_host_native_window += files('native_sub_window_win32.cpp')
elif host_machine.system() == 'linux' and use_gles
  files_lib_host_native_window += files('native_sub_window_x11.cpp')
elif host_machine.system() == 'qnx'
  files_lib_host_native_window += files(
    'native_sub_window_qnx.cpp',
  )
endif

lib_host_native_window = static_library(
  'host_native_window',
  files_lib_host_native_window,
  include_directories: [
    inc_common_base,
    inc_host_common,
    inc_host_decoder_common,
    inc_host_include,
    inc_host_native_window,
  ],
)
