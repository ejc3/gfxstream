# Vulkan Headers in gfxstream

This directory (`third_party/vulkan`) contains historical, checked-in Vulkan headers (version 1.4.363). These headers are used by:
- Core gfxstream host and guest components (`:gfxstream_vulkan_headers`)
- Host/guest serialization (`cereal`)
- Non-Bazel build systems (`Android.bp`, `CMakeLists.txt`, `meson.build`)

## External Vulkan Headers (`third_party/vulkan_headers`)

A separate external repository rule is defined in `//third_party/vulkan_headers` (`@vulkan_headers//:vulkan_headers`), pinned to `vulkan-sdk-1.4.357.0`.

This second copy is used by:
1. Vulkan Validation Layers (`vvl`) and Vulkan-Utility-Libraries (`vulkan_utility_libraries`), which are pinned to 1.4.357.
2. Keeping them separate avoids coupling VVL/VUL upgrades to the checked-in headers used across Android and CMake/Meson build systems.
