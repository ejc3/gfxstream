// Copyright 2026 The Android Open Source Project
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

// Counters for what a remote renderer would have to carry: the bytes each render thread
// receives from the guest, split by decoder and by opcode, and every reply written back
// to the guest, by the opcode being decoded when it was written. A reply is a point where
// the guest's encoder waits for the host (a call with output), so the reply count is the
// guest's blocking round trip count.
//
// Off unless GFXSTREAM_STATS names a file. Then one JSON line a second is appended to it,
// holding that second's deltas per render thread (fleet/phone, docs/fleet/phone-gpu-remoting.md
// in ejc3/fcvm reads them). GFXSTREAM_STATS_DUMP names a file the decoded guest->host bytes
// are appended to while a file of that name plus ".on" exists, up to
// GFXSTREAM_STATS_DUMP_BYTES (default 512 MiB).
#pragma once

#include <stddef.h>
#include <stdint.h>

namespace gfxstream {
namespace host {
namespace remoting_stats {

enum class Decoder { Gles1 = 0, Gles2 = 1, RenderControl = 2, Vulkan = 3, kCount = 4 };

// The opcode the calling thread's decoder is decoding; the decoders set it per packet, and
// a reply written meanwhile is counted against it.
inline thread_local uint32_t tCurrentOpcode = 0;
inline void setOpcode(uint32_t opcode) { tCurrentOpcode = opcode; }

bool enabled();

// A render thread starts and stops counting; between the two every call below is counted
// against it.
void threadStart();
void threadEnd();
// The guest process (puid) the calling render thread serves now.
void setPuid(uint64_t puid);
// Bytes the calling render thread read from its channel.
void onRead(size_t bytes);
// A decoder consumed `len` bytes of whole packets starting at `buf`.
void onConsumed(Decoder decoder, const uint8_t* buf, size_t len);
// The calling thread wrote a reply of `bytes` to the guest.
void onReply(size_t bytes);
// The sync thread was asked to signal a guest fence when a host fence completes.
void onFenceSignal();

}  // namespace remoting_stats
}  // namespace host
}  // namespace gfxstream
