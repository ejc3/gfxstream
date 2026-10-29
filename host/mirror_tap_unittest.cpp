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

#include "mirror_tap.h"

#include <gtest/gtest.h>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>

#include <chrono>
#include <string>
#include <thread>
#include <vector>

namespace gfxstream {
namespace host {
namespace mirror {
namespace {

struct Record {
    uint32_t type, channel, len, aux;
    uint64_t t;
    std::string payload;
};

uint32_t get32(const unsigned char* p) { return p[0] | p[1] << 8 | p[2] << 16 | (uint32_t)p[3] << 24; }

// The consumer end of a socketpair: every record it has received within `ms`.
class Consumer {
   public:
    Consumer() {
        int sv[2];
        EXPECT_EQ(socketpair(AF_UNIX, SOCK_STREAM, 0, sv), 0);
        mFd = sv[0];
        mTapFd = sv[1];
    }
    ~Consumer() { close(mFd); }
    int tapFd() const { return mTapFd; }
    void send(char c) { ASSERT_EQ(::write(mFd, &c, 1), 1); }
    void hangUp() { ::shutdown(mFd, SHUT_WR); }
    // Reads until `n` records arrived or `ms` passed.
    std::vector<Record> read(size_t n, int ms = 2000) {
        auto end = std::chrono::steady_clock::now() + std::chrono::milliseconds(ms);
        while (parse() < n && std::chrono::steady_clock::now() < end) {
            pollfd p{mFd, POLLIN, 0};
            if (poll(&p, 1, 20) <= 0) continue;
            unsigned char b[65536];
            ssize_t r = ::read(mFd, b, sizeof b);
            if (r <= 0) break;
            mBuf.append((const char*)b, (size_t)r);
        }
        std::vector<Record> out;
        out.swap(mRecords);
        return out;
    }

   private:
    size_t parse() {
        while (mBuf.size() >= kHeaderSize) {
            auto* p = (const unsigned char*)mBuf.data();
            uint32_t len = get32(p + 8);
            if (mBuf.size() < kHeaderSize + len) break;
            Record r{get32(p), get32(p + 4), len, get32(p + 12), get32(p + 16) | (uint64_t)get32(p + 20) << 32,
                     mBuf.substr(kHeaderSize, len)};
            mRecords.push_back(r);
            mBuf.erase(0, kHeaderSize + len);
        }
        return mRecords.size();
    }
    int mFd = -1, mTapFd = -1;
    std::string mBuf;
    std::vector<Record> mRecords;
};

// Runs `f` on a thread of its own, as a render thread whose channel is registered.
template <typename F>
void onChannel(F f, bool loaded = false) {
    std::thread([&] {
        registerChannel(loaded);
        f();
        unregisterChannel();
    }).join();
}

// A reply that returns names is copied whole: 1024 sampler names are 4096 bytes. One longer
// (a readback) is cut, its full size in aux.
TEST(MirrorTap, RepliesWithNamesAreWholeAndReadbacksAreCut) {
    Consumer c;
    startForTest(c.tapFd(), 1 << 20);
    c.send('S');
    ASSERT_EQ(c.read(1).size(), 1u);
    std::string names(4096, 'n'), pixels(100000, 'p');
    onChannel([&] {
        onReply(names.data(), names.size());
        onReply(pixels.data(), pixels.size());
    });
    auto r = c.read(4);
    ASSERT_EQ(r.size(), 4u);
    EXPECT_EQ(r[1].payload, names);
    EXPECT_EQ(r[2].aux, 100000u);
    EXPECT_EQ(r[2].payload.size(), kReplyPrefix);
    stopForTest();
}

// A consumer that stops reading does not keep the tap from its next consumer: its socket is
// ended before its sender is joined.
TEST(MirrorTap, AConsumerThatStopsReadingIsLetGo) {
    Consumer c;
    startForTest(c.tapFd(), 8 << 20);
    c.send('S');
    ASSERT_EQ(c.read(1).size(), 1u);
    std::string chunk(1 << 20, 'x');
    // more than the socket buffers hold, never read: the sender blocks in send()
    onChannel([&] {
        for (int i = 0; i < 6; i++) onGuestData((const unsigned char*)chunk.data(), 0, chunk.size());
    });
    // the consumer hangs up its side and goes on not reading
    c.hangUp();
    bool done = false;
    for (int i = 0; i < 300 && !done; i++) {
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
        done = consumerDoneForTest();
    }
    EXPECT_TRUE(done) << "the tap still waits on a consumer that went";
    stopForTest();
}

TEST(MirrorTap, StoppedCopiesNothing) {
    Consumer c;
    startForTest(c.tapFd(), 1 << 20);
    onChannel([] {
        const unsigned char b[8] = {1, 2, 3, 4, 8, 0, 0, 0};
        onGuestData(b, 0, sizeof b);
        onReply(b, 4);
    });
    EXPECT_TRUE(c.read(1, 200).empty());
    stopForTest();
}

TEST(MirrorTap, CopiesDataRepliesAndChannelLifetimes) {
    Consumer c;
    startForTest(c.tapFd(), 1 << 20);
    c.send('S');
    auto hello = c.read(1);
    ASSERT_EQ(hello.size(), 1u);
    EXPECT_EQ(hello[0].type, kHello);
    EXPECT_EQ(hello[0].aux, kProtocolVersion);
    std::string big(1000, 'r');
    onChannel(
        [&] {
            const unsigned char b[12] = {9, 0, 0, 0, 12, 0, 0, 0, 7, 7, 7, 7};
            onGuestData(b, 0, sizeof b);
            onReply(big.data(), big.size());
        },
        true);
    auto r = c.read(4);
    ASSERT_EQ(r.size(), 4u);
    EXPECT_EQ(r[0].type, kOpen);
    EXPECT_EQ(r[0].aux, 1u);  // loaded from a snapshot
    EXPECT_EQ(r[1].type, kData);
    EXPECT_EQ(r[1].channel, r[0].channel);
    EXPECT_EQ(r[1].payload.size(), 12u);
    EXPECT_EQ(r[2].type, kReply);
    EXPECT_EQ(r[2].aux, 1000u);                    // the full size
    EXPECT_EQ(r[2].payload, big);                  // whole: it is under the 4096-byte cut
    EXPECT_EQ(r[3].type, kClose);
    EXPECT_LE(r[1].t, r[2].t);
    stopForTest();
}

// A channel open before the copy started is first copied from its oldest unconsumed byte,
// so the consumer starts at a packet boundary; later reads carry only their new bytes.
TEST(MirrorTap, AChannelOpenBeforeTheStartBeginsAtAPacketBoundary) {
    Consumer c;
    startForTest(c.tapFd(), 1 << 20);
    std::thread t([&] {
        registerChannel(false);
        // read before the start: not copied
        const unsigned char before[4] = {1, 1, 1, 1};
        onGuestData(before, 0, 4);
        c.send('S');
        // wait for the HELLO, which lists this channel
        auto hello = c.read(1);
        ASSERT_EQ(hello.size(), 1u);
        ASSERT_EQ(hello[0].type, kHello);
        ASSERT_EQ(hello[0].payload.size(), 8u);
        EXPECT_EQ(get32((const unsigned char*)hello[0].payload.data()), 1u);
        const unsigned char buf[10] = {5, 5, 5, 6, 6, 6, 6, 6, 6, 6};
        onGuestData(buf, 3, 7);       // 3 pending bytes, then 7 new
        onGuestData(buf + 3, 0, 7);   // a later read: only its new bytes
        unregisterChannel();
    });
    t.join();
    auto r = c.read(3);
    ASSERT_GE(r.size(), 2u);
    EXPECT_EQ(r[0].type, kData);
    EXPECT_EQ(r[0].payload.size(), 10u);
    EXPECT_EQ(r[1].type, kData);
    EXPECT_EQ(r[1].payload.size(), 7u);
    stopForTest();
}

// A record that does not fit stops the copy: what the ring held arrives, then OVERFLOW, and
// nothing after it until the consumer starts the copy again.
TEST(MirrorTap, AFullRingStopsTheCopyWithAnOverflowRecord) {
    Consumer c;
    startForTest(c.tapFd(), 1 << 16);
    c.send('S');
    ASSERT_EQ(c.read(1).size(), 1u);
    // a read the ring cannot hold whatever the sender has drained: overflow, deterministically
    std::string small(100, 'd'), big(70000, 'b');
    onChannel([&] {
        onGuestData((const unsigned char*)small.data(), 0, small.size());
        onGuestData((const unsigned char*)big.data(), 0, big.size());
        // after the overflow: not copied
        onGuestData((const unsigned char*)small.data(), 0, 10);
    });
    auto r = c.read(10, 500);
    std::vector<uint32_t> types;
    for (auto& x : r) types.push_back(x.type);
    // OPEN, DATA, OVERFLOW; the CLOSE after the overflow is not copied either
    ASSERT_EQ(types, (std::vector<uint32_t>{kOpen, kData, kOverflow}));
    EXPECT_EQ(r[2].aux, kHeaderSize + big.size());
    c.send('S');
    auto again = c.read(1);
    ASSERT_EQ(again.size(), 1u);
    EXPECT_EQ(again[0].type, kHello);
    stopForTest();
}

}  // namespace
}  // namespace mirror
}  // namespace host
}  // namespace gfxstream
