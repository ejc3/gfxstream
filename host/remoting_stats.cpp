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

#include "remoting_stats.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/syscall.h>
#include <time.h>
#include <unistd.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

namespace gfxstream {
namespace host {
namespace remoting_stats {
namespace {

struct OpStat {
    uint64_t calls = 0;
    uint64_t bytes = 0;
    uint64_t replies = 0;
    uint64_t replyBytes = 0;
};

struct ThreadStats {
    std::mutex mu;
    uint64_t id = 0;
    long tid = 0;
    uint64_t puid = 0;
    bool done = false;
    // this second's deltas
    uint64_t read = 0;
    uint64_t decoded[static_cast<int>(Decoder::kCount)] = {};
    uint64_t replies = 0;
    uint64_t replyBytes = 0;
    std::unordered_map<uint32_t, OpStat> ops;
};

struct Registry {
    std::mutex mu;
    std::vector<std::shared_ptr<ThreadStats>> threads;
    uint64_t nextId = 1;
};

Registry& registry() {
    static Registry* r = new Registry();
    return *r;
}

thread_local std::shared_ptr<ThreadStats> tStats;
std::atomic<uint64_t> sFenceSignals{0};

// The raw dump of decoded guest->host bytes.
std::atomic<bool> sDumpOn{false};
std::mutex sDumpMu;
FILE* sDumpFile = nullptr;
uint64_t sDumpBytes = 0;
uint64_t sDumpCap = 512ull << 20;

const char* statsPath() {
    static const char* p = [] {
        const char* v = getenv("GFXSTREAM_STATS");
        return (v && *v) ? v : nullptr;
    }();
    return p;
}

const char* dumpPath() {
    static const char* p = [] {
        const char* v = getenv("GFXSTREAM_STATS_DUMP");
        return (v && *v) ? v : nullptr;
    }();
    return p;
}

uint64_t nowMs() {
    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);
    return (uint64_t)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

void appendU64(std::string* s, const char* key, uint64_t v) {
    char b[64];
    snprintf(b, sizeof(b), "\"%s\":%llu,", key, (unsigned long long)v);
    *s += b;
}

void loggerLoop() {
    FILE* out = fopen(statsPath(), "a");
    if (!out) {
        fprintf(stderr, "remoting_stats: cannot open %s\n", statsPath());
        return;
    }
    const char* dp = dumpPath();
    std::string onFile = dp ? std::string(dp) + ".on" : std::string();
    if (const char* cap = getenv("GFXSTREAM_STATS_DUMP_BYTES")) {
        if (atoll(cap) > 0) sDumpCap = (uint64_t)atoll(cap);
    }
    auto next = std::chrono::steady_clock::now();
    std::string line;
    for (;;) {
        next += std::chrono::seconds(1);
        std::this_thread::sleep_until(next);
        if (dp) {
            bool on = access(onFile.c_str(), F_OK) == 0;
            sDumpOn.store(on, std::memory_order_relaxed);
            if (!on) {
                // A dump ends when its ".on" file goes: the file is closed, so the caller may
                // move it once the next stats line is written, and the next dump starts anew.
                std::lock_guard<std::mutex> l(sDumpMu);
                if (sDumpFile) {
                    fclose(sDumpFile);
                    sDumpFile = nullptr;
                    sDumpBytes = 0;
                }
            }
        }

        std::vector<std::shared_ptr<ThreadStats>> threads;
        {
            std::lock_guard<std::mutex> l(registry().mu);
            threads = registry().threads;
        }
        line.clear();
        line += "{";
        appendU64(&line, "t_ms", nowMs());
        appendU64(&line, "fence_signals", sFenceSignals.exchange(0));
        {
            std::lock_guard<std::mutex> l(sDumpMu);
            if (sDumpFile) fflush(sDumpFile);
            appendU64(&line, "dump_bytes", sDumpBytes);
        }
        line += "\"threads\":[";
        bool firstThread = true;
        std::vector<ThreadStats*> finished;
        for (auto& t : threads) {
            std::lock_guard<std::mutex> l(t->mu);
            if (t->done) finished.push_back(t.get());
            if (!t->read && !t->replies && t->ops.empty()) continue;
            if (!firstThread) line += ",";
            firstThread = false;
            line += "{";
            appendU64(&line, "id", t->id);
            appendU64(&line, "tid", (uint64_t)t->tid);
            appendU64(&line, "puid", t->puid);
            appendU64(&line, "read", t->read);
            appendU64(&line, "gles1", t->decoded[0]);
            appendU64(&line, "gles2", t->decoded[1]);
            appendU64(&line, "rc", t->decoded[2]);
            appendU64(&line, "vk", t->decoded[3]);
            appendU64(&line, "replies", t->replies);
            appendU64(&line, "reply_bytes", t->replyBytes);
            line += "\"ops\":{";
            bool firstOp = true;
            for (auto& [op, s] : t->ops) {
                char b[160];
                snprintf(b, sizeof(b), "%s\"%u\":[%llu,%llu,%llu,%llu]", firstOp ? "" : ",", op,
                         (unsigned long long)s.calls, (unsigned long long)s.bytes,
                         (unsigned long long)s.replies, (unsigned long long)s.replyBytes);
                line += b;
                firstOp = false;
            }
            line += "}}";
            t->read = 0;
            for (auto& d : t->decoded) d = 0;
            t->replies = 0;
            t->replyBytes = 0;
            t->ops.clear();
        }
        line += "]}\n";
        fputs(line.c_str(), out);
        fflush(out);
        if (!finished.empty()) {
            std::lock_guard<std::mutex> l(registry().mu);
            auto& v = registry().threads;
            for (size_t i = 0; i < v.size();) {
                bool drop = false;
                for (auto* f : finished) drop |= (v[i].get() == f);
                if (drop) {
                    v.erase(v.begin() + i);
                } else {
                    ++i;
                }
            }
        }
    }
}

void startLogger() {
    static std::once_flag once;
    std::call_once(once, [] { std::thread(loggerLoop).detach(); });
}

}  // namespace

bool enabled() { return statsPath() != nullptr; }

void threadStart() {
    if (!enabled()) return;
    startLogger();
    auto t = std::make_shared<ThreadStats>();
    t->tid = syscall(SYS_gettid);
    {
        std::lock_guard<std::mutex> l(registry().mu);
        t->id = registry().nextId++;
        registry().threads.push_back(t);
    }
    tStats = std::move(t);
}

void threadEnd() {
    if (!tStats) return;
    {
        std::lock_guard<std::mutex> l(tStats->mu);
        tStats->done = true;
    }
    tStats.reset();
}

void setPuid(uint64_t puid) {
    ThreadStats* t = tStats.get();
    if (!t || t->puid == puid) return;
    std::lock_guard<std::mutex> l(t->mu);
    t->puid = puid;
}

void onRead(size_t bytes) {
    ThreadStats* t = tStats.get();
    if (!t) return;
    std::lock_guard<std::mutex> l(t->mu);
    t->read += bytes;
}

void onConsumed(Decoder decoder, const uint8_t* buf, size_t len) {
    ThreadStats* t = tStats.get();
    if (!t || !len) return;
    {
        std::lock_guard<std::mutex> l(t->mu);
        t->decoded[static_cast<int>(decoder)] += len;
        size_t off = 0;
        while (len - off >= 8) {
            uint32_t op, plen;
            memcpy(&op, buf + off, 4);
            memcpy(&plen, buf + off + 4, 4);
            if (plen < 8 || plen > len - off) {
                // not a whole packet: counted, unattributed
                OpStat& s = t->ops[0xffffffffu];
                s.calls++;
                s.bytes += len - off;
                off = len;
                break;
            }
            OpStat& s = t->ops[op];
            s.calls++;
            s.bytes += plen;
            off += plen;
        }
        if (off < len) {
            OpStat& s = t->ops[0xffffffffu];
            s.calls++;
            s.bytes += len - off;
        }
    }
    if (sDumpOn.load(std::memory_order_relaxed)) {
        std::lock_guard<std::mutex> l(sDumpMu);
        if (!sDumpFile) sDumpFile = fopen(dumpPath(), "a");
        // One record per decoder call: the thread's id, the decoder, the byte count, then the
        // bytes (whole packets). A record that would pass the cap is not written.
        if (sDumpFile && sDumpBytes + 12 + len <= sDumpCap) {
            uint32_t hdr[3] = {(uint32_t)t->id, (uint32_t)decoder, (uint32_t)len};
            sDumpBytes += fwrite(hdr, 1, sizeof(hdr), sDumpFile);
            sDumpBytes += fwrite(buf, 1, len, sDumpFile);
        }
    }
}

void onReply(size_t bytes) {
    ThreadStats* t = tStats.get();
    if (!t || !bytes) return;
    std::lock_guard<std::mutex> l(t->mu);
    t->replies++;
    t->replyBytes += bytes;
    OpStat& s = t->ops[tCurrentOpcode];
    s.replies++;
    s.replyBytes += bytes;
}

void onFenceSignal() {
    if (!enabled()) return;
    startLogger();
    sFenceSignals.fetch_add(1, std::memory_order_relaxed);
}

}  // namespace remoting_stats
}  // namespace host
}  // namespace gfxstream
