// Copyright (C) 2024 The Android Open Source Project
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

#include <cstddef>
#include <limits>

#include "gfxstream/common/logging.h"
#include "gfxstream/host/gfxstream_format.h"

namespace gfxstream {
namespace host {

// clang-format off
#define VIRGL_BIND_RENDER_TARGET (1u << 1)
#define VIRGL_BIND_SAMPLER_VIEW  (1u << 3)
#define VIRGL_BIND_CURSOR        (1u << 16)
#define VIRGL_BIND_CUSTOM        (1u << 17)
#define VIRGL_BIND_SCANOUT       (1u << 18)
#define VIRGL_BIND_SHARED        (1u << 20)
#define VIRGL_BIND_LINEAR        (1u << 22)

#define VIRGL_FORMAT_B8G8R8A8_UNORM          1
#define VIRGL_FORMAT_B8G8R8X8_UNORM          2
#define VIRGL_FORMAT_B5G6R5_UNORM            7
#define VIRGL_FORMAT_R10G10B10A2_UNORM       8
#define VIRGL_FORMAT_Z16_UNORM               16
#define VIRGL_FORMAT_Z32_FLOAT               18
#define VIRGL_FORMAT_Z24_UNORM_S8_UINT       19
#define VIRGL_FORMAT_Z24X8_UNORM             21
#define VIRGL_FORMAT_R16_UNORM               48
#define VIRGL_FORMAT_R8_UNORM                64
#define VIRGL_FORMAT_R8G8_UNORM              65
#define VIRGL_FORMAT_R8G8B8_UNORM            66
#define VIRGL_FORMAT_R8G8B8A8_UNORM          67
#define VIRGL_FORMAT_R16G16B16A16_FLOAT      94
#define VIRGL_FORMAT_Z32_FLOAT_S8X24_UINT    126
#define VIRGL_FORMAT_R8G8B8X8_UNORM          134
#define VIRGL_FORMAT_YV12                    163
#define VIRGL_FORMAT_NV12                    166
#define VIRGL_FORMAT_P010                    314
#define VIRGL_FORMAT_P210                    481
// clang-format on

inline std::optional<GfxstreamFormat> ToGfxstreamFormat(uint32_t virglFormat) {
    switch (virglFormat) {
        case VIRGL_FORMAT_B8G8R8X8_UNORM:
        case VIRGL_FORMAT_B8G8R8A8_UNORM:
            return GfxstreamFormat::B8G8R8A8_UNORM;
        case VIRGL_FORMAT_R8G8B8X8_UNORM:
            return GfxstreamFormat::R8G8B8X8_UNORM;
        case VIRGL_FORMAT_R8G8B8A8_UNORM:
            return GfxstreamFormat::R8G8B8A8_UNORM;
        case VIRGL_FORMAT_B5G6R5_UNORM:
            return GfxstreamFormat::R5G6B5_UNORM;
        case VIRGL_FORMAT_R16_UNORM:
            return GfxstreamFormat::R16_UNORM;
        case VIRGL_FORMAT_R16G16B16A16_FLOAT:
            return GfxstreamFormat::R16G16B16A16_FLOAT;
        case VIRGL_FORMAT_R8_UNORM:
            return GfxstreamFormat::R8_UNORM;
        case VIRGL_FORMAT_R8G8_UNORM:
            return GfxstreamFormat::R8G8_UNORM;
        case VIRGL_FORMAT_R8G8B8_UNORM:
            return GfxstreamFormat::R8G8B8_UNORM;
        case VIRGL_FORMAT_NV12:
            return GfxstreamFormat::NV12;
        case VIRGL_FORMAT_P010:
            return GfxstreamFormat::P010;
        case VIRGL_FORMAT_P210:
            return GfxstreamFormat::P210;
        case VIRGL_FORMAT_YV12:
            return GfxstreamFormat::YV12;
        case VIRGL_FORMAT_R10G10B10A2_UNORM:
            return GfxstreamFormat::R10G10B10A2_UNORM;
        case VIRGL_FORMAT_Z16_UNORM:
            return GfxstreamFormat::D16_UNORM;
        case VIRGL_FORMAT_Z24X8_UNORM:
            return GfxstreamFormat::D24_UNORM;
        case VIRGL_FORMAT_Z24_UNORM_S8_UINT:
            return GfxstreamFormat::D24_UNORM_S8_UINT;
        case VIRGL_FORMAT_Z32_FLOAT:
            return GfxstreamFormat::D32_FLOAT;
        case VIRGL_FORMAT_Z32_FLOAT_S8X24_UINT:
            return GfxstreamFormat::D32_FLOAT_S8_UINT;
        default: {
            return std::nullopt;
        }
    }
}

static inline void set_virgl_format_supported(uint32_t* mask, uint32_t virgl_format,
                                              bool supported) {
    uint32_t index = virgl_format / 32;
    uint32_t bit_offset = 1 << (virgl_format & 31);
    if (supported) {
        mask[index] |= bit_offset;
    } else {
        mask[index] &= ~bit_offset;
    }
}

static inline bool SafeMulSizeT(size_t a, size_t b, size_t* out) {
    return !__builtin_mul_overflow(a, b, out);
}

static inline bool SafeAddSizeT(size_t a, size_t b, size_t* out) {
    return !__builtin_add_overflow(a, b, out);
}

static inline size_t GetTransferOffset(uint32_t virglFormat,
                                       uint32_t totalWidth,
                                       uint32_t totalHeight,
                                       uint32_t x,
                                       uint32_t y,
                                       uint32_t w,
                                       uint32_t h) {
    auto formatOpt = ToGfxstreamFormat(virglFormat);
    if (!formatOpt) {
        GFXSTREAM_ERROR("Unhandled format %d", virglFormat);
        return 0;
    }
    auto format = *formatOpt;

    if (IsYuvFormat(format)) {
        return 0;
    }

    auto bppOpt = GetBpp(format);
    if (!bppOpt) {
        const std::string formatString = ToString(format);
        GFXSTREAM_ERROR("Unhandled format %s", formatString.c_str());
        return 0;
    }
    const size_t bpp = *bppOpt;
    size_t stride = 0;
    size_t rowOffset = 0;
    size_t colOffset = 0;
    size_t offset = 0;
    if (!SafeMulSizeT(static_cast<size_t>(totalWidth), bpp, &stride) ||
        !SafeMulSizeT(static_cast<size_t>(y), stride, &rowOffset) ||
        !SafeMulSizeT(static_cast<size_t>(x), bpp, &colOffset) ||
        !SafeAddSizeT(rowOffset, colOffset, &offset)) {
        return std::numeric_limits<size_t>::max();
    }
    return offset;
}

static inline size_t align_up_power_of_2(size_t n, size_t a) {
    return (n + (a - 1)) & ~(a - 1);
}

static inline size_t GetTransferSize(uint32_t virglFormat,
                                     uint32_t totalWidth,
                                     uint32_t totalHeight,
                                     uint32_t x,
                                     uint32_t y,
                                     uint32_t w,
                                     uint32_t h) {
    if (totalWidth == 0 || totalHeight == 0 || w == 0 || h == 0) {
        return 0;
    }

    auto formatOpt = ToGfxstreamFormat(virglFormat);
    if (!formatOpt) {
        GFXSTREAM_ERROR("Unhandled format %d", virglFormat);
        return 0;
    }
    auto format = *formatOpt;

    if (IsYuvFormat(format)) {
        size_t bpp;
        switch (format) {
            case GfxstreamFormat::P010:
            case GfxstreamFormat::P210:
                bpp = 2;
                break;
            case GfxstreamFormat::NV12:
            case GfxstreamFormat::NV21:
            case GfxstreamFormat::YV12:
            case GfxstreamFormat::YV21:
                bpp = 1;
                break;
            default: {
                const std::string formatString = ToString(format);
                GFXSTREAM_ERROR("Unhandled format %s", formatString.c_str());
                return 0;
            }
        }

        size_t yWidth = totalWidth;
        size_t yHeight = totalHeight;
        size_t yStridePixels;
        switch (format) {
            case GfxstreamFormat::NV12:
            case GfxstreamFormat::NV21:
            case GfxstreamFormat::P010:
            case GfxstreamFormat::P210: {
                yStridePixels = yWidth;
                break;
            }
            case GfxstreamFormat::YV12:
            case GfxstreamFormat::YV21: {
                // Guard against `yWidth + 31` overflowing `size_t` inside
                // `align_up_power_of_2(yWidth, 32)` (e.g. on 32-bit targets).
                if (yWidth > std::numeric_limits<size_t>::max() - 31U) {
                    return std::numeric_limits<size_t>::max();
                }
                yStridePixels = align_up_power_of_2(yWidth, 32);
                break;
            }
            default: {
                const std::string formatString = ToString(format);
                GFXSTREAM_ERROR("Unhandled format %s", formatString.c_str());
                return 0;
            }
        }

        size_t yStrideBytes = 0;
        size_t ySize = 0;
        if (!SafeMulSizeT(yStridePixels, bpp, &yStrideBytes) ||
            !SafeMulSizeT(yStrideBytes, yHeight, &ySize)) {
            return std::numeric_limits<size_t>::max();
        }

        size_t uvStridePixels;
        size_t uvPlaneCount;
        switch (format) {
            case GfxstreamFormat::NV12:
            case GfxstreamFormat::NV21:
            case GfxstreamFormat::P010:
            case GfxstreamFormat::P210: {
                uvStridePixels = yStridePixels;
                uvPlaneCount = 1;
                break;
            }
            case GfxstreamFormat::YV12:
            case GfxstreamFormat::YV21: {
                uvStridePixels = yStridePixels / 2;
                uvPlaneCount = 2;
                break;
            }
            default: {
                const std::string formatString = ToString(format);
                GFXSTREAM_ERROR("Unhandled format %s", formatString.c_str());
                return 0;
            }
        }

        size_t verticalSubsampling;
        switch (format) {
            case GfxstreamFormat::P210:
                verticalSubsampling = 1;
                break;
            case GfxstreamFormat::NV12:
            case GfxstreamFormat::NV21:
            case GfxstreamFormat::YV12:
            case GfxstreamFormat::YV21:
            case GfxstreamFormat::P010:
                verticalSubsampling = 2;
                break;
            default: {
                const std::string formatString = ToString(format);
                GFXSTREAM_ERROR("Unhandled format %s", formatString.c_str());
                return 0;
            }
        }
        size_t uvHeight = totalHeight / verticalSubsampling;
        size_t uvStrideBytes = 0;
        size_t uvPlaneSize = 0;
        size_t uvSize = 0;
        size_t dataSize = 0;
        if (!SafeMulSizeT(uvStridePixels, bpp, &uvStrideBytes) ||
            !SafeMulSizeT(uvStrideBytes, uvHeight, &uvPlaneSize) ||
            !SafeMulSizeT(uvPlaneSize, uvPlaneCount, &uvSize) ||
            !SafeAddSizeT(ySize, uvSize, &dataSize)) {
            return std::numeric_limits<size_t>::max();
        }
        return dataSize;
    } else {
        auto bppOpt = GetBpp(format);
        if (!bppOpt) {
            const std::string formatString = ToString(format);
            GFXSTREAM_ERROR("Unhandled format %s", formatString.c_str());
            return 0;
        }
        const size_t bpp = *bppOpt;
        size_t stride = 0;
        size_t rowsBytes = 0;
        size_t lastRowBytes = 0;
        size_t totalBytes = 0;

        // height - 1 in order to treat the (w * bpp) row specially
        // (i.e., the last row does not occupy the full stride)
        if (!SafeMulSizeT(static_cast<size_t>(totalWidth), bpp, &stride) ||
            !SafeMulSizeT(static_cast<size_t>(h - 1U), stride, &rowsBytes) ||
            !SafeMulSizeT(static_cast<size_t>(w), bpp, &lastRowBytes) ||
            !SafeAddSizeT(rowsBytes, lastRowBytes, &totalBytes)) {
            return std::numeric_limits<size_t>::max();
        }
        return totalBytes;
    }
}

}  // namespace host
}  // namespace gfxstream
