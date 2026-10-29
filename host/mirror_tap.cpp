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

#if defined(__linux__)

#include <errno.h>
#include <poll.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <time.h>
#include <unistd.h>

#include <algorithm>
#include <atomic>
#include <condition_variable>
#include <mutex>
#include <set>
#include <string>
#include <thread>
#include <vector>

namespace gfxstream {
namespace host {
namespace mirror {
namespace {

uint64_t nowUs() {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000u + (uint64_t)ts.tv_nsec / 1000u;
}

void put32(unsigned char* p, uint32_t v) {
    p[0] = v & 0xff;
    p[1] = (v >> 8) & 0xff;
    p[2] = (v >> 16) & 0xff;
    p[3] = (v >> 24) & 0xff;
}

void put64(unsigned char* p, uint64_t v) {
    put32(p, (uint32_t)v);
    put32(p + 4, (uint32_t)(v >> 32));
}

// One bounded byte ring, many writers (render threads), one reader (the sender thread).
class Tap {
   public:
    // Hooks test this first: false means the copy is stopped and the hook returns at once.
    std::atomic<bool> active{false};
    // Bumped by every start: a channel whose synced epoch differs has not been copied since.
    std::atomic<uint32_t> epoch{0};

    void configure(size_t ringBytes) {
        std::lock_guard<std::mutex> l(mMu);
        mRing.assign(ringBytes, 0);
        mHead = mSize = 0;
    }

    // Starts the copy: a HELLO naming the open channels, then everything that follows.
    void start() {
        std::lock_guard<std::mutex> l(mMu);
        mHead = mSize = 0;
        std::vector<unsigned char> ids(4 + 4 * mOpen.size());
        put32(ids.data(), (uint32_t)mOpen.size());
        size_t i = 4;
        for (uint32_t id : mOpen) {
            put32(ids.data() + i, id);
            i += 4;
        }
        epoch.fetch_add(1, std::memory_order_relaxed);
        appendLocked(kHello, 0, kProtocolVersion, ids.data(), ids.size(), nullptr, 0);
        active.store(true, std::memory_order_release);
        mCv.notify_all();
    }

    void stop() {
        std::lock_guard<std::mutex> l(mMu);
        active.store(false, std::memory_order_release);
        mHead = mSize = 0;
    }

    uint32_t openChannel(bool loaded, uint32_t* syncedEpoch) {
        std::lock_guard<std::mutex> l(mMu);
        // A channel that opens while the copy runs is copied from its first byte.
        *syncedEpoch = epoch.load(std::memory_order_relaxed);
        uint32_t id = ++mNextId;
        mOpen.insert(id);
        if (active.load(std::memory_order_relaxed)) recordLocked(kOpen, id, loaded ? 1 : 0, nullptr, 0, nullptr, 0);
        return id;
    }

    void closeChannel(uint32_t id) {
        std::lock_guard<std::mutex> l(mMu);
        mOpen.erase(id);
        if (active.load(std::memory_order_relaxed)) recordLocked(kClose, id, 0, nullptr, 0, nullptr, 0);
    }

    void record(uint32_t type, uint32_t channel, uint32_t aux, const void* a, size_t an, const void* b, size_t bn) {
        std::lock_guard<std::mutex> l(mMu);
        if (!active.load(std::memory_order_relaxed)) return;
        recordLocked(type, channel, aux, a, an, b, bn);
    }

    // One read's bytes. Under the lock with start(), so a channel's first copy after a start
    // (by the epoch it last copied in) always begins at its oldest unconsumed byte.
    void data(uint32_t channel, uint32_t* syncedEpoch, const unsigned char* start, size_t pending, size_t fresh) {
        std::lock_guard<std::mutex> l(mMu);
        if (!active.load(std::memory_order_relaxed)) return;
        uint32_t e = epoch.load(std::memory_order_relaxed);
        if (*syncedEpoch != e) {
            *syncedEpoch = e;
            recordLocked(kData, channel, 0, start, pending + fresh, nullptr, 0);
            return;
        }
        recordLocked(kData, channel, 0, start + pending, fresh, nullptr, 0);
    }

    // Sender thread: blocks until there are bytes, then moves up to `max` of them into `out`.
    // False once `stopWaiting` is set.
    bool take(std::vector<unsigned char>* out, size_t max, const std::atomic<bool>& stopWaiting) {
        std::unique_lock<std::mutex> l(mMu);
        mCv.wait_for(l, std::chrono::milliseconds(100), [&] { return mSize > 0 || stopWaiting.load(); });
        if (stopWaiting.load()) return false;
        size_t n = std::min(mSize, max);
        out->resize(n);
        if (n == 0) return true;
        size_t first = std::min(n, mRing.size() - mHead);
        memcpy(out->data(), mRing.data() + mHead, first);
        memcpy(out->data() + first, mRing.data(), n - first);
        mHead = (mHead + n) % mRing.size();
        mSize -= n;
        return true;
    }

    // For tests and HELLO: the channels open now.
    size_t openCount() {
        std::lock_guard<std::mutex> l(mMu);
        return mOpen.size();
    }

   private:
    void recordLocked(uint32_t type, uint32_t channel, uint32_t aux, const void* a, size_t an, const void* b,
                      size_t bn) {
        size_t need = kHeaderSize + an + bn;
        // An overflow record must always fit behind what the ring holds.
        if (mRing.empty() || need + kHeaderSize > mRing.size() - mSize) {
            active.store(false, std::memory_order_release);
            appendLocked(kOverflow, channel, (uint32_t)std::min<size_t>(need, UINT32_MAX), nullptr, 0, nullptr, 0);
            mCv.notify_all();
            return;
        }
        appendLocked(type, channel, aux, a, an, b, bn);
        mCv.notify_all();
    }

    // Caller checked room (or this is a HELLO or OVERFLOW into a ring that has room for one).
    void appendLocked(uint32_t type, uint32_t channel, uint32_t aux, const void* a, size_t an, const void* b,
                      size_t bn) {
        unsigned char h[kHeaderSize];
        put32(h, type);
        put32(h + 4, channel);
        put32(h + 8, (uint32_t)(an + bn));
        put32(h + 12, aux);
        put64(h + 16, nowUs());
        if (mRing.size() - mSize < kHeaderSize + an + bn) return;
        copyIn(h, kHeaderSize);
        if (an) copyIn(a, an);
        if (bn) copyIn(b, bn);
    }

    void copyIn(const void* p, size_t n) {
        size_t tail = (mHead + mSize) % mRing.size();
        size_t first = std::min(n, mRing.size() - tail);
        memcpy(mRing.data() + tail, p, first);
        memcpy(mRing.data(), (const unsigned char*)p + first, n - first);
        mSize += n;
    }

    std::mutex mMu;
    std::condition_variable mCv;
    std::vector<unsigned char> mRing;
    size_t mHead = 0, mSize = 0;
    std::set<uint32_t> mOpen;
    uint32_t mNextId = 0;
};

Tap& tap() {
    static Tap* t = new Tap();  // never destroyed: render threads may outlive static destructors
    return *t;
}

thread_local uint32_t t_channel = 0;
thread_local uint32_t t_syncedEpoch = 0;

bool writeAll(int fd, const unsigned char* p, size_t n) {
    while (n) {
        ssize_t w = ::send(fd, p, n, MSG_NOSIGNAL);
        if (w < 0 && errno == EINTR) continue;
        if (w <= 0) return false;
        p += w;
        n -= (size_t)w;
    }
    return true;
}

// One consumer at a time: reads its commands and writes the ring to it. Returns when it goes.
void serveConsumer(int fd) {
    std::atomic<bool> gone{false};
    std::thread sender([fd, &gone] {
        std::vector<unsigned char> buf;
        while (tap().take(&buf, 1 << 20, gone)) {
            if (!buf.empty() && !writeAll(fd, buf.data(), buf.size())) {
                gone.store(true);
                tap().stop();
                ::shutdown(fd, SHUT_RDWR);
            }
        }
    });
    while (!gone.load()) {
        char c;
        ssize_t r = ::recv(fd, &c, 1, 0);
        if (r < 0 && errno == EINTR) continue;
        if (r <= 0) break;
        if (c == 'S') tap().start();
        if (c == 'X') tap().stop();
    }
    tap().stop();
    gone.store(true);
    // A consumer that stopped reading leaves the sender blocked in send(): end the socket first.
    ::shutdown(fd, SHUT_RDWR);
    sender.join();
    ::close(fd);
}

void listenLoop(std::string path) {
    int lfd = ::socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
    struct sockaddr_un a = {};
    a.sun_family = AF_UNIX;
    if (lfd < 0 || path.size() >= sizeof a.sun_path) {
        fprintf(stderr, "gfxstream mirror tap: cannot listen on %s\n", path.c_str());
        if (lfd >= 0) ::close(lfd);
        return;
    }
    memcpy(a.sun_path, path.c_str(), path.size());
    ::unlink(path.c_str());
    if (::bind(lfd, (struct sockaddr*)&a, sizeof a) != 0 || ::listen(lfd, 1) != 0) {
        fprintf(stderr, "gfxstream mirror tap: cannot listen on %s: %s\n", path.c_str(), strerror(errno));
        ::close(lfd);
        return;
    }
    while (true) {
        int fd = ::accept4(lfd, nullptr, nullptr, SOCK_CLOEXEC);
        if (fd < 0) {
            if (errno == EINTR || errno == ECONNABORTED) continue;
            fprintf(stderr, "gfxstream mirror tap: accept: %s\n", strerror(errno));
            ::sleep(1);
            continue;
        }
        serveConsumer(fd);
    }
}

}  // namespace

void initFromEnv() {
    static std::once_flag once;
    std::call_once(once, [] {
        const char* path = getenv("GFXSTREAM_MIRROR_SOCKET");
        if (!path || !*path) return;
        size_t ring = 32u << 20;
        if (const char* r = getenv("GFXSTREAM_MIRROR_RING_BYTES")) {
            long long v = atoll(r);
            if (v >= (1 << 16)) ring = (size_t)v;
        }
        tap().configure(ring);
        std::thread(listenLoop, std::string(path)).detach();
    });
}

void registerChannel(bool loadedFromSnapshot) {
    initFromEnv();
    t_channel = tap().openChannel(loadedFromSnapshot, &t_syncedEpoch);
}

void unregisterChannel() {
    if (!t_channel) return;
    tap().closeChannel(t_channel);
    t_channel = 0;
}

void onGuestData(const unsigned char* start, size_t pending, size_t fresh) {
    if (!tap().active.load(std::memory_order_acquire) || !t_channel) return;
    tap().data(t_channel, &t_syncedEpoch, start, pending, fresh);
}

void onReply(const void* data, size_t size) {
    if (!tap().active.load(std::memory_order_acquire) || !t_channel) return;
    tap().record(kReply, t_channel, (uint32_t)std::min<size_t>(size, UINT32_MAX), data,
                 std::min(size, kReplyPrefix), nullptr, 0);
}

namespace {
std::thread g_testConsumer;
int g_testFd = -1;
std::atomic<bool> g_testConsumerDone{false};
}  // namespace

void startForTest(int fd, size_t ringBytes) {
    stopForTest();
    tap().configure(ringBytes);
    g_testFd = fd;
    g_testConsumerDone = false;
    g_testConsumer = std::thread([fd] {
        serveConsumer(fd);
        g_testConsumerDone = true;
    });
}

bool consumerDoneForTest() { return g_testConsumerDone.load(); }

void stopForTest() {
    tap().stop();
    if (g_testFd >= 0) ::shutdown(g_testFd, SHUT_RDWR);
    if (g_testConsumer.joinable()) g_testConsumer.join();
    g_testFd = -1;
}

}  // namespace mirror
}  // namespace host
}  // namespace gfxstream

#else  // the tap is Linux only: elsewhere every hook does nothing

namespace gfxstream {
namespace host {
namespace mirror {
void initFromEnv() {}
void registerChannel(bool) {}
void unregisterChannel() {}
void onGuestData(const unsigned char*, size_t, size_t) {}
void onReply(const void*, size_t) {}
void startForTest(int, size_t) {}
void stopForTest() {}
bool consumerDoneForTest() { return true; }
}  // namespace mirror
}  // namespace host
}  // namespace gfxstream

#endif
