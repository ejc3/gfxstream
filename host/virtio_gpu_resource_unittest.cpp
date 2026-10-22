// Copyright (C) 2026 The Android Open Source Project
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

#include "virtio_gpu_resource.h"

#include <cstdint>
#include <limits>
#include <memory>
#include <vector>

#include <gtest/gtest.h>

#include "gfxstream/virtio-gpu-gfxstream-renderer.h"
#include "virtio_gpu_format_utils.h"
#include "virtio_gpu_pipe.h"

namespace gfxstream {
namespace host {
namespace {

TEST(VirtioGpuFormatUtilsSecurityTest, GetTransferOffsetAndSize64BitNoOverflow) {
    // 65536 * 65536 * 4 = 16 GiB (wraps 32-bit uint32_t to 0 without size_t widening)
    const size_t offset = GetTransferOffset(VIRGL_FORMAT_R8G8B8A8_UNORM,
                                            /*totalWidth=*/65536,
                                            /*totalHeight=*/65536,
                                            /*x=*/0,
                                            /*y=*/65535,
                                            /*w=*/65536,
                                            /*h=*/1);
    EXPECT_EQ(offset, static_cast<size_t>(65535ULL) * 65536ULL * 4ULL);

    const size_t rgbSize = GetTransferSize(VIRGL_FORMAT_R8G8B8A8_UNORM,
                                           /*totalWidth=*/65536,
                                           /*totalHeight=*/65536,
                                           /*x=*/0,
                                           /*y=*/0,
                                           /*w=*/65536,
                                           /*h=*/65536);
    EXPECT_EQ(rgbSize, 65536ULL * 65536ULL * 4ULL);

    const size_t yuvSize = GetTransferSize(VIRGL_FORMAT_YV12,
                                           /*totalWidth=*/65536,
                                           /*totalHeight=*/65536,
                                           /*x=*/0,
                                           /*y=*/0,
                                           /*w=*/65536,
                                           /*h=*/65536);
    EXPECT_GT(yuvSize, 0x100000000ULL);

    // Zero width/height must never underflow (h - 1U)
    EXPECT_EQ(GetTransferSize(VIRGL_FORMAT_R8G8B8A8_UNORM, 16, 16, 0, 0, 16, 0), 0U);
    EXPECT_EQ(GetTransferSize(VIRGL_FORMAT_R8G8B8A8_UNORM, 0, 16, 0, 0, 16, 16), 0U);
}

TEST(VirtioGpuResourceSecurityTest, RejectsPipeTransferReadAndWriteOutOfBoundsAndOverflow) {
    struct stream_renderer_resource_create_args args = {
        .handle = 1,
        .target = 0,                      // PIPE_BUFFER
        .format = VIRGL_FORMAT_R8_UNORM,
        .bind = VIRGL_BIND_CUSTOM,        // VirtioGpuResourceType::PIPE
        .width = 0x2000,
        .height = 1,
        .depth = 1,
        .array_size = 1,
    };

    // Attach only 4 KiB linear backing buffer.
    std::vector<uint8_t> backing(4096, 0x41);
    struct iovec iov = {.iov_base = backing.data(), .iov_len = backing.size()};

    auto resOpt = VirtioGpuResource::Create(&args, &iov, 1);
    ASSERT_TRUE(resOpt.has_value());
    VirtioGpuResource& res = *resOpt;

    res.SetHostPipe(std::make_shared<VirtioGpuPipe>(/*renderer=*/nullptr, /*id=*/1));

    // Null box must reject cleanly with -EINVAL.
    EXPECT_EQ(res.TransferRead(0, nullptr, std::nullopt), -EINVAL);
    EXPECT_EQ(res.TransferWrite(0, nullptr, std::nullopt), -EINVAL);

    // 1. Direct OOB offset (box.x = 0x10000000, box.w = 0x1000 > mLinear.size() = 0x1000)
    stream_renderer_box oobBox = {.x = 0x10000000, .y = 0, .z = 0, .w = 0x1000, .h = 1, .d = 1};
    EXPECT_EQ(res.TransferRead(0, &oobBox, std::nullopt), -EINVAL);
    EXPECT_EQ(res.TransferWrite(0, &oobBox, std::nullopt), -EINVAL);

    // 2. 32-bit uint32_t overflow (box.x = 0xFFFFF800, box.w = 0x1000 -> x + w wraps u32 to 0x800!)
    stream_renderer_box overflowBox = {
        .x = 0xFFFFF800U, .y = 0, .z = 0, .w = 0x1000U, .h = 1, .d = 1};
    EXPECT_EQ(res.TransferRead(0, &overflowBox, std::nullopt), -EINVAL);
    EXPECT_EQ(res.TransferWrite(0, &overflowBox, std::nullopt), -EINVAL);

    // 3. Exact boundary OOB (box.x = 2048, box.w = 2049 -> 4097 > 4096)
    stream_renderer_box offByOneBox = {.x = 2048, .y = 0, .z = 0, .w = 2049, .h = 1, .d = 1};
    EXPECT_EQ(res.TransferRead(0, &offByOneBox, std::nullopt), -EINVAL);
    EXPECT_EQ(res.TransferWrite(0, &offByOneBox, std::nullopt), -EINVAL);

    // 4. AttachIov size_t overflow must reject cleanly and clear mLinear/mIovs
    struct iovec overflowIovs[2] = {
        {.iov_base = backing.data(), .iov_len = 4096},
        {.iov_base = backing.data(), .iov_len = std::numeric_limits<size_t>::max()},
    };
    res.AttachIov(overflowIovs, 2);
    stream_renderer_box smallBox = {.x = 0, .y = 0, .z = 0, .w = 16, .h = 1, .d = 1};
    EXPECT_EQ(res.TransferRead(0, &smallBox, std::nullopt), -EINVAL);
    EXPECT_EQ(res.TransferWrite(0, &smallBox, std::nullopt), -EINVAL);
}

#ifdef GFXSTREAM_BUILD_WITH_SNAPSHOT_FRONTEND_SUPPORT
TEST(VirtioGpuResourceSecurityTest, RejectsUndersizedLinearBackingOnBufferAndColorBufferTransfers) {
    std::vector<uint8_t> smallBacking(4096, 0x42);
    struct iovec iov = {.iov_base = smallBacking.data(), .iov_len = smallBacking.size()};
    stream_renderer_box tinyBox = {.x = 0, .y = 0, .z = 0, .w = 16, .h = 1, .d = 1};

    // 1. BUFFER resource with width=65536, height=65536 (4 GiB -> u32 wraps width*height to 0!)
    // Without 64-bit bounds check in BOTH ReadFromBufferToLinear and WriteToBufferFromLinear,
    // step-2 would pass requiredSize=0 / skip check and dereference FrameBuffer::getFB() OOB!
    snapshot::VirtioGpuResourceSnapshot bufSnap;
    bufSnap.set_id(10);
    bufSnap.set_type(static_cast<snapshot::VirtioGpuResourceType>(VirtioGpuResourceType::BUFFER));
    auto* bufCreate = bufSnap.mutable_create_args();
    bufCreate->set_id(10);
    bufCreate->set_target(0);
    bufCreate->set_format(VIRGL_FORMAT_R8_UNORM);
    bufCreate->set_bind(1U << 4);
    bufCreate->set_width(65536);
    bufCreate->set_height(65536);
    bufCreate->set_depth(1);
    bufCreate->set_array_size(1);

    auto bufResOpt = VirtioGpuResource::Restore(bufSnap);
    ASSERT_TRUE(bufResOpt.has_value());
    bufResOpt->AttachIov(&iov, 1);
    EXPECT_EQ(bufResOpt->TransferRead(0, &tinyBox, std::nullopt), -EINVAL);
    EXPECT_EQ(bufResOpt->TransferWrite(0, &tinyBox, std::nullopt), -EINVAL);

    // 2. COLOR_BUFFER resource with width=65536, height=65536 RGBA8 (16 GiB -> u32 wraps to 0!)
    // Without 64-bit GetTransferSize + bounds check in BOTH ReadFromColorBufferToLinear and
    // WriteToColorBufferFromLinear, step-2 would read/write 16 GiB past mLinear!
    snapshot::VirtioGpuResourceSnapshot cbSnap;
    cbSnap.set_id(11);
    cbSnap.set_type(
        static_cast<snapshot::VirtioGpuResourceType>(VirtioGpuResourceType::COLOR_BUFFER));
    auto* cbCreate = cbSnap.mutable_create_args();
    cbCreate->set_id(11);
    cbCreate->set_target(2);
    cbCreate->set_format(VIRGL_FORMAT_R8G8B8A8_UNORM);
    cbCreate->set_bind(VIRGL_BIND_SAMPLER_VIEW);
    cbCreate->set_width(65536);
    cbCreate->set_height(65536);
    cbCreate->set_depth(1);
    cbCreate->set_array_size(1);

    auto cbResOpt = VirtioGpuResource::Restore(cbSnap);
    ASSERT_TRUE(cbResOpt.has_value());
    cbResOpt->AttachIov(&iov, 1);
    EXPECT_EQ(cbResOpt->TransferRead(0, &tinyBox, std::nullopt), -EINVAL);
    EXPECT_EQ(cbResOpt->TransferWrite(0, &tinyBox, std::nullopt), -EINVAL);
}
#endif  // GFXSTREAM_BUILD_WITH_SNAPSHOT_FRONTEND_SUPPORT

}  // namespace
}  // namespace host
}  // namespace gfxstream
