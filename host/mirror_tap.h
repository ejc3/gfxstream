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

// The mirror tap: a copy of every render channel's guest-to-host bytes, and of the start of
// every reply, for one consumer on a Unix socket, so a viewer can replay the guest's GL
// command stream on a GPU of its own while this renderer keeps answering every call.
//
// Linux only: elsewhere the hooks do nothing. Off unless GFXSTREAM_MIRROR_SOCKET names a socket path; then a consumer connects there
// and sends 'S' to start the copy, 'J' to start it with a join snapshot, and 'X' to stop it. While stopped, each hook costs one
// relaxed atomic load. While started, a hook copies its bytes into one bounded ring
// (GFXSTREAM_MIRROR_RING_BYTES, default 32 MiB) under a mutex held for that copy only, and
// a sender thread writes the ring to the socket: a render thread never waits on the
// consumer. When a record does not fit the copy stops, an OVERFLOW record follows what the
// ring already holds, and the consumer starts it again with 'S' (and a fresh HELLO).
//
// Records, little-endian, a 24-byte header then `len` payload bytes:
//   u32 type, u32 channel, u32 len, u32 aux, u64 monotonic time in microseconds
//   HELLO    1  channel 0; aux the protocol version (1); payload u32 n, then n u32 ids of
//               the channels open when the copy started (their state is unknown to the
//               consumer; their first DATA starts at a packet boundary)
//   OPEN     2  a render channel started; aux 1 when its state was loaded from a snapshot
//   CLOSE    3  a render channel ended
//   DATA     4  bytes the guest sent on the channel, in the order they arrived
//   REPLY    5  a reply to the guest; aux its full size, payload its first 4096 bytes at most
//               (every name a reply returns fits: a reply cut short is a readback)
//   OVERFLOW 6  the ring was full; aux the size of the record that did not fit. The copy
//               has stopped.
//   JOIN     7  the renderer's state when the copy started ('J'), in parts: aux the part's
//               kind. Every JOIN record of a start comes after its HELLO and before any other
//               record, and the last is an END.
//
// A join ('J') runs the join handler the renderer set: it pauses every render thread, starts
// the copy (JoinWriter::begin) while they are paused, saves its state as JOIN parts, resumes
// them and writes END. Records that render threads make meanwhile wait in the ring until the
// handler returns. The JOIN parts are not bounded by the ring: they are sent as they are
// written. With no handler, or one that refuses before it began, 'J' is 'S'.
//
// JOIN parts, little-endian u32s:
//   BEGIN        1  version (1)
//   COLOR_BUFFER 2  handle, width, height, format (gfxstream's GfxstreamFormat), refcount,
//                   flags (bit 0: opened); then width * height * 4 bytes of RGBA8: its rows from
//                   GL's row 0 (glReadPixels' order)
//   END          3  status (0: every part above is whole; else the join failed and the state
//                   is incomplete), the render threads' pause in microseconds, the color buffer
//                   posted last (0: none), the color buffers skipped (an unreadable format)
#pragma once

#include <stddef.h>
#include <stdint.h>

#include <functional>

namespace gfxstream {
namespace host {
namespace mirror {

enum RecordType : uint32_t {
    kHello = 1,
    kOpen = 2,
    kClose = 3,
    kData = 4,
    kReply = 5,
    kOverflow = 6,
    kJoin = 7,
};

enum JoinPart : uint32_t {
    kJoinBegin = 1,
    kJoinColorBuffer = 2,
    kJoinEnd = 3,
};

constexpr uint32_t kJoinVersion = 1;
constexpr uint32_t kJoinOk = 0;
constexpr uint32_t kJoinFailed = 1;

constexpr uint32_t kProtocolVersion = 1;
constexpr size_t kHeaderSize = 24;
constexpr size_t kReplyPrefix = 4096;

// Starts the listener if GFXSTREAM_MIRROR_SOCKET is set; idempotent.
void initFromEnv();

// The calling render thread's channel: from its start until its end.
void registerChannel(bool loadedFromSnapshot);
void unregisterChannel();

// ReadBuffer::getData: `pending` bytes the decoders had not consumed yet, then `fresh` bytes
// just read, contiguous from `start`. The first copy of a channel that was open before the
// copy started includes `pending`, so it starts at a packet boundary.
void onGuestData(const unsigned char* start, size_t pending, size_t fresh);

// ChannelStream::commitBuffer: a reply to the guest.
void onReply(const void* data, size_t size);

// Where a join handler writes the state it saves.
class JoinWriter {
   public:
    virtual ~JoinWriter() = default;
    // Starts the copy (a HELLO naming the open channels): call it with every render thread
    // paused. Until the handler returns, other records wait behind the JOIN parts.
    virtual void begin() = 0;
    // One JOIN record: `kind` its JoinPart, the payload `a` then `b`.
    virtual void part(uint32_t kind, const void* a, size_t an, const void* b, size_t bn) = 0;
};

// Runs on the consumer's thread for a 'J'. Returns false when it could not save the state: if
// it began, the tap ends the join with a failed END; if not, the copy starts as for 'S'.
using JoinHandler = std::function<bool(JoinWriter&)>;

// The renderer's join handler (an empty function: none). Blocks until a running join ends.
void setJoinHandler(JoinHandler handler);

// For tests: serves `fd` as the consumer, with a ring of `ringBytes`.
void startForTest(int fd, size_t ringBytes);
void stopForTest();
// Whether the consumer served by startForTest has finished on its own.
bool consumerDoneForTest();

}  // namespace mirror
}  // namespace host
}  // namespace gfxstream
