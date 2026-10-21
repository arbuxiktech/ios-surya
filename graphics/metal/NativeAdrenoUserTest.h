// SPDX-License-Identifier: MIT
// The userspace GPU test shared by the Adreno Metal plugin (runs inside whichever system process the original Metal discovery loads it
// into, under that process's sandbox) and the launchd probe daemon (no sandbox profile). It proves the whole userspace path to the GPU:
// open the driver, move every ABI method through an admitted selector (or the mailbox page), map buffer objects write-combined or
// uncached, write an indirect buffer from userspace, submit it, wait for the fence, read the result back, and report one line to the
// kernel through SetMetadata. Progress markers are IOServiceOpen calls with type 0x4147<value>, which the driver counts and refuses.
#pragma once
#include <dlfcn.h>
#include <mach/mach.h>
#include <pthread.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <initializer_list>
#include "NativeAdrenoABI.h"
#include "NativeTurnipSmoke.h"
#if __has_include("TurnipReplay.h")
#include "TurnipReplay.h"
#define NATIVE_TURNIP_REPLAY 1
#endif
extern "C" {
#include "AdrenoPM4.h"
}

namespace {
using IOServiceOpenFn = int (*)(unsigned, unsigned, uint32_t, unsigned *);
using IOServiceCloseFn = int (*)(unsigned);
using CallFn = int (*)(unsigned, uint32_t, const void *, size_t, void *, size_t *);
using MapFn = int (*)(unsigned, uint32_t, unsigned, uint64_t *, uint64_t *, uint32_t);
using UnmapFn = int (*)(unsigned, uint32_t, unsigned, uint64_t);

// The sandbox admits only some external-method selector numbers for this user-client class, and which ones is decided by the
// profile, not by us. Every ABI method therefore travels through one admitted selector as {magic, method, payload}; the driver
// recognises the magic before its own selector table. Probing 0..255 finds an admitted selector without knowing the profile.
constexpr uint32_t kTunnelMagic = 0x4e544e4c, kProbe = 0xffffffffu;
struct Gpu {
    unsigned conn = 0;
    CallFn call = nullptr;
    MapFn map = nullptr;
    UnmapFn unmap = nullptr;
    uint32_t selector = 0;
    unsigned admitted = 0;
    uint64_t lastTicks = 0;
    unsigned port = 0;
    IOServiceOpenFn openFn = nullptr;
    IOServiceCloseFn closeFn = nullptr;
    // Progress marker: an IOServiceOpen with type 0x4147<value>. The driver counts it and refuses it, so it needs no selector, no
    // mapping and no connection, and survives a sandbox that admits nothing else or a crash after it.
    void mark(uint32_t value) {
        unsigned c = 0;
        if (openFn && port && openFn(port, mach_task_self(), 0x41470000u | (value & 0xffff), &c) == 0 && c && closeFn) closeFn(c);
    }
    volatile uint32_t *mailbox = nullptr;
    uint32_t mailboxSeq = 0;
    int mailboxMethod(uint32_t m, const void *in, size_t is, void *out, size_t os) {
        if (!mailbox || is > 0x700 || os > 0x700) return -1;
        if (is) memcpy(const_cast<uint32_t *>(mailbox) + 0x10, in, is);
        mailbox[3] = m;
        mailbox[5] = (uint32_t)is;
        mailbox[6] = (uint32_t)os;
        __sync_synchronize();
        const uint32_t seq = ++mailboxSeq;
        mailbox[1] = seq;
        __sync_synchronize();
        for (unsigned i = 0; i < 6000 && mailbox[2] != seq; ++i) usleep(500);   // the kernel poller runs at about 1 kHz
        if (mailbox[2] != seq) return -2;
        __sync_synchronize();
        const int status = (int)mailbox[4];
        if (!status && os) memcpy(out, const_cast<uint32_t *>(mailbox) + 0x200, os);
        return status;
    }
    int method(uint32_t m, const void *in, size_t is, void *out, size_t os) {
        if (mailbox) return mailboxMethod(m, in, is, out, os);
        uint8_t buffer[8 + sizeof(NativeAdrenoAbiSubmit)];
        if (is > sizeof(NativeAdrenoAbiSubmit)) return -1;
        const uint32_t header[2] = {kTunnelMagic, m};
        memcpy(buffer, header, 8);
        if (is) memcpy(buffer + 8, in, is);
        size_t o = os;
        return call(conn, selector, buffer, 8 + is, os ? out : nullptr, os ? &o : nullptr);
    }
    // No selector is admitted: use the mailbox page (IOConnectMapMemory64 is not covered by the selector filter).
    bool openMailbox() {
        uint64_t address = 0, length = 0;
        if (map(conn, 0xC0DE, mach_task_self(), &address, &length, 1) != 0 || length < 4096) return false;
        mailbox = reinterpret_cast<volatile uint32_t *>(address);
        return mailbox[0] == 0x4d41494c;
    }
    // Returns true when some selector was admitted; the first one becomes the tunnel selector.
    bool probe() {
        const uint32_t header[2] = {kTunnelMagic, kProbe};
        bool found = false;
        for (uint32_t s = 0; s < 256; ++s) {
            if (call(conn, s, header, sizeof header, nullptr, nullptr) == 0) {
                if (!found) selector = s;
                found = true;
                ++admitted;
            }
        }
        return found;
    }
};

void report(Gpu &gpu, const char *process, int step, int error, int verified, int verifiedWc, uint64_t ticks) {
    // SetMetadata with a bogus handle: the driver records the text before it validates the handle.
    NativeAdrenoAbiMetadata meta = {};
    meta.handle = 1;
    char text[NativeAdrenoAbiMetadataBytes] = {};
    snprintf(text, sizeof text, "%.9s s%d e%x v%d/%d t%llx a%u/%u", process, step, error, verified, verifiedWc,
             (unsigned long long)ticks, gpu.mailbox ? 0xffffu : gpu.admitted, gpu.selector);
    meta.bytes = (uint32_t)strlen(text);
    memcpy(meta.text, text, meta.bytes);
    gpu.method(NativeAdrenoMethodSetMetadata, &meta, sizeof meta, nullptr, 0);
}

// Everything the Turnip backend does for a first submission, in the same order; returns the verified dword count or -1.
int runTest(Gpu &gpu, uint32_t mapFlags, int &step, int &error) {
    auto check = [&](int status) {
        if (status && !error) error = status;
        if (!status) ++step;
        gpu.mark(status ? 0x4000 | (status & 0xfff) : 0x3000 | (mapFlags == 0x401 ? 0x100 : 0) | step);
        return status == 0;
    };
    NativeAdrenoAbiParams params = {};
    if (!check(gpu.method(NativeAdrenoMethodQuery, nullptr, 0, &params, sizeof params))) return -1;
    NativeAdrenoAbiDeviceInfo info = {};
    if (!check(gpu.method(NativeAdrenoMethodGetDeviceInfo, nullptr, 0, &info, sizeof info))) return -1;
    NativeAdrenoAbiBoAlloc alloc = {4096, 12, 0};
    NativeAdrenoAbiBoAllocReply ib = {}, target = {};
    if (!check(gpu.method(NativeAdrenoMethodBoAlloc, &alloc, sizeof alloc, &ib, sizeof ib)) ||
        !check(gpu.method(NativeAdrenoMethodBoAlloc, &alloc, sizeof alloc, &target, sizeof target))) return -1;
    const uint64_t ibIova = info.va_start, tgtIova = info.va_start + 0x1000;
    NativeAdrenoAbiBoMap mapIb = {ib.handle, ibIova, 1, 0, 2, 0}, mapTgt = {target.handle, tgtIova, 3, 0, 2, 0};
    NativeAdrenoAbiBoMapReply mr = {};
    if (!check(gpu.method(NativeAdrenoMethodBoMap, &mapIb, sizeof mapIb, &mr, sizeof mr)) ||
        !check(gpu.method(NativeAdrenoMethodBoMap, &mapTgt, sizeof mapTgt, &mr, sizeof mr))) return -1;
    NativeAdrenoAbiBoCpuMap cmIb = {ib.handle}, cmTgt = {target.handle};
    NativeAdrenoAbiBoCpuMapReply crIb = {}, crTgt = {};
    if (!check(gpu.method(NativeAdrenoMethodBoCpuMap, &cmIb, sizeof cmIb, &crIb, sizeof crIb)) ||
        !check(gpu.method(NativeAdrenoMethodBoCpuMap, &cmTgt, sizeof cmTgt, &crTgt, sizeof crTgt))) return -1;
    uint64_t ibAddr = 0, ibLen = 0, tgtAddr = 0, tgtLen = 0;
    if (!check(gpu.map(gpu.conn, crIb.token, mach_task_self(), &ibAddr, &ibLen, mapFlags)) ||
        !check(gpu.map(gpu.conn, crTgt.token, mach_task_self(), &tgtAddr, &tgtLen, mapFlags))) return -1;
    if (ibLen < 4096 || tgtLen < 4096) {error = 0xbad1; return -1;}
    volatile uint32_t *iw = reinterpret_cast<volatile uint32_t *>(ibAddr);
    volatile uint32_t *tw = reinterpret_cast<volatile uint32_t *>(tgtAddr);
    for (unsigned i = 0; i < 1024; ++i) tw[i] = 0xdeadbeef;
    const uint32_t values[3] = {0x0adc0de1u, 0xfeed0002u, 0x600d0003u};
    uint32_t words[12];
    for (unsigned i = 0; i < 3; ++i) {
        if (AdrenoPM4Type7(0x3d, 3, &words[i * 4]) != AdrenoPM4Success) {error = 0xbad2; return -1;}
        words[i * 4 + 1] = uint32_t(tgtIova + i * 4);
        words[i * 4 + 2] = uint32_t(tgtIova >> 32);
        words[i * 4 + 3] = values[i];
    }
    for (unsigned i = 0; i < 12; ++i) iw[i] = words[i];
    __asm__ volatile("dsb sy" ::: "memory");   // the mapping is non-cacheable and the GPU is an outer-shareable observer
    NativeAdrenoAbiSubmit submit = {};
    submit.buffer_count = 1;
    submit.buffers[0] = {ib.handle, ibIova, sizeof words};
    NativeAdrenoAbiSubmitReply sr = {};
    if (!check(gpu.method(NativeAdrenoMethodSubmit, &submit, sizeof submit, &sr, sizeof sr))) return -1;
    NativeAdrenoAbiWaitFence wait = {sr.fence, 200000000ull};
    if (!check(gpu.method(NativeAdrenoMethodWaitFence, &wait, sizeof wait, nullptr, 0))) return -1;
    __asm__ volatile("dsb sy" ::: "memory");
    const int verified = (tw[0] == values[0]) + (tw[1] == values[1]) + (tw[2] == values[2]) + (tw[3] == 0xdeadbeef);
    NativeAdrenoAbiTimestamp ts = {};
    uint64_t ticks = 0;
    if (check(gpu.method(NativeAdrenoMethodGetTimestamp, nullptr, 0, &ts, sizeof ts))) ticks = ts.ticks;
    gpu.unmap(gpu.conn, crIb.token, mach_task_self(), ibAddr);
    gpu.unmap(gpu.conn, crTgt.token, mach_task_self(), tgtAddr);
    NativeAdrenoAbiBoCpuUnmap ui = {ib.handle, crIb.token, 0}, ut = {target.handle, crTgt.token, 0};
    check(gpu.method(NativeAdrenoMethodBoCpuUnmap, &ui, sizeof ui, nullptr, 0));
    check(gpu.method(NativeAdrenoMethodBoCpuUnmap, &ut, sizeof ut, nullptr, 0));
    NativeAdrenoAbiBoFree fi = {ib.handle}, ft = {target.handle};
    check(gpu.method(NativeAdrenoMethodBoFree, &fi, sizeof fi, nullptr, 0));
    check(gpu.method(NativeAdrenoMethodBoFree, &ft, sizeof ft, nullptr, 0));
    gpu.lastTicks = ticks;
    return verified;
}
}

// A report line as a marker stream (0x7fff, then 0x70cc per character): works while another connection holds the driver's single slot.
// Two threads of one process may report (the published mode's status monitor and the scanout client): the driver keeps one text cursor, so a line
// goes out whole.
static pthread_mutex_t gStreamLock = PTHREAD_MUTEX_INITIALIZER;
static void streamText(Gpu &gpu, const char *text) {
    pthread_mutex_lock(&gStreamLock);
    gpu.mark(0x7fff);
    for (unsigned i = 0; i < 63 && text[i]; ++i) gpu.mark(0x7000 | (uint8_t)text[i]);
    pthread_mutex_unlock(&gStreamLock);
}

// Line into report (GPU7) instead of report2 (GPU8).
__attribute__((unused)) static void streamText7(Gpu &gpu, const char *text) {
    pthread_mutex_lock(&gStreamLock);
    gpu.mark(0x7ffe);
    for (unsigned i = 0; i < 63 && text[i]; ++i) gpu.mark(0x7000 | (uint8_t)text[i]);
    pthread_mutex_unlock(&gStreamLock);
}

// Remap probe (int-064): does the CP read the new pages after a GPU address is moved to other physical pages? int-063's W1 run (the
// replay IB on fresh buffers at the same addresses) stopped both CP threads in the SQE's unknown-opcode trap, i.e. on wrong data. A
// CP_MEM_TO_MEM from a remapped page into a result page reports what the CP sees instead of executing it, so no variant can stop the CP:
//   v0 first mapping, v1 remap to a page allocated before the old one is freed (so the old page keeps its old pattern),
//   v2 as v1 with a UCHE invalidate (CP_EVENT_WRITE7 0x31) and CP_WAIT_FOR_IDLE before the copy,
//   v3 as v1 but remapped while the CP is busy spinning on a flag (CP_WAIT_REG_MEM), then released: GX stays up across the TLB invalidate.
// Each result: '1' the new pattern, 'o' the previous mapping's pattern, 'z' zero, 'd' result not written, 'g' other, 'x' setup failed,
// 't' fence timeout. Reported as "P<v0><v1><v2><v3>" in front of the replay line. The addresses (0x5f000000..) are never used elsewhere.
struct ProbeBo {
    uint64_t handle = 0, address = 0;
    uint32_t token = 0;
    volatile uint32_t *words = nullptr;
};
static int probeAlloc(Gpu &gpu, ProbeBo &b, uint32_t fill) {
    NativeAdrenoAbiBoAlloc alloc = {4096, 12, 0};
    NativeAdrenoAbiBoAllocReply reply = {};
    if (int e = gpu.method(NativeAdrenoMethodBoAlloc, &alloc, sizeof alloc, &reply, sizeof reply)) return e;
    b.handle = reply.handle;
    NativeAdrenoAbiBoCpuMap cm = {b.handle};
    NativeAdrenoAbiBoCpuMapReply cr = {};
    if (int e = gpu.method(NativeAdrenoMethodBoCpuMap, &cm, sizeof cm, &cr, sizeof cr)) return e;
    b.token = cr.token;
    uint64_t length = 0;
    if (int e = gpu.map(gpu.conn, cr.token, mach_task_self(), &b.address, &length, 0x101)) return e;
    if (length < 4096) return 0xbad5;
    b.words = reinterpret_cast<volatile uint32_t *>(b.address);
    for (unsigned i = 0; i < 1024; ++i) b.words[i] = fill;
    __asm__ volatile("dsb sy" ::: "memory");
    return 0;
}
static int probeMap(Gpu &gpu, const ProbeBo &b, uint64_t iova) {
    NativeAdrenoAbiBoMap map = {b.handle, iova, 3, 0, 2, 0};
    NativeAdrenoAbiBoMapReply mr = {};
    return gpu.method(NativeAdrenoMethodBoMap, &map, sizeof map, &mr, sizeof mr);
}
static void probeFree(Gpu &gpu, ProbeBo &b) {
    if (!b.handle) return;
    if (b.address) {
        gpu.unmap(gpu.conn, b.token, mach_task_self(), b.address);
        NativeAdrenoAbiBoCpuUnmap cu = {b.handle, b.token, 0};
        gpu.method(NativeAdrenoMethodBoCpuUnmap, &cu, sizeof cu, nullptr, 0);
    }
    NativeAdrenoAbiBoFree f = {b.handle};
    gpu.method(NativeAdrenoMethodBoFree, &f, sizeof f, nullptr, 0);
    b = ProbeBo();
}
static char gProbe[6] = "P----";
__attribute__((unused)) static void remapProbe(Gpu &gpu) {
    constexpr uint64_t kSource = 0x5f000000ull, kResult = 0x5f010000ull, kIb = 0x5f020000ull, kFlag = 0x5f030000ull;
    const uint32_t patterns[4] = {0x0a1a1a10u, 0x0b2b2b20u, 0x0c3c3c30u, 0x0d4d4d40u};
    ProbeBo ib, result, flag, source;
    bool stuck = false;
    if (probeAlloc(gpu, ib, 0) || probeAlloc(gpu, result, 0xdeadbeef) || probeAlloc(gpu, flag, 0) ||
        probeAlloc(gpu, source, patterns[0]) || probeMap(gpu, ib, kIb) || probeMap(gpu, result, kResult) ||
        probeMap(gpu, flag, kFlag) || probeMap(gpu, source, kSource)) {
        for (unsigned v = 1; v < 5; ++v) gProbe[v] = 'x';
        probeFree(gpu, source); probeFree(gpu, flag); probeFree(gpu, result); probeFree(gpu, ib);
        return;
    }
    for (unsigned v = 0; v < 4 && !stuck; ++v) {
        gpu.mark(0x6d00 | v);
        ProbeBo next;
        if (v) {
            // The new page exists before the old one is freed, so the two differ and the old one still holds the old pattern.
            if (probeAlloc(gpu, next, patterns[v])) {gProbe[1 + v] = 'x'; probeFree(gpu, next); continue;}
        }
        uint32_t words[32], n = 0;
        auto packet = [&](uint32_t opcode, std::initializer_list<uint32_t> payload) {
            if (AdrenoPM4Type7(opcode, uint32_t(payload.size()), &words[n]) != AdrenoPM4Success) return false;
            ++n;
            for (uint32_t w : payload) words[n++] = w;
            return true;
        };
        bool ok = true;
        if (v == 3) ok = packet(0x3c, {3u | (1u << 4), uint32_t(kFlag), uint32_t(kFlag >> 32), 1u, 0xffffffffu, 16u});   // wait flag == 1
        if (v == 2) ok = packet(0x46, {0x31u}) && packet(0x26, {});
        ok = ok && packet(0x73, {0u, uint32_t(kResult), uint32_t(kResult >> 32), uint32_t(kSource), uint32_t(kSource >> 32)});
        if (!ok) {gProbe[1 + v] = 'x'; probeFree(gpu, next); continue;}
        for (uint32_t i = 0; i < n; ++i) ib.words[i] = words[i];
        result.words[0] = 0xdeadbeef;
        flag.words[0] = 0;
        __asm__ volatile("dsb sy" ::: "memory");
        if (v && v != 3) {
            probeFree(gpu, source);
            if (probeMap(gpu, next, kSource)) {gProbe[1 + v] = 'x'; probeFree(gpu, next); break;}
            source = next;
        }
        NativeAdrenoAbiSubmit submit = {};
        submit.buffer_count = 1;
        submit.buffers[0] = {ib.handle, kIb, uint64_t(n) * 4};
        NativeAdrenoAbiSubmitReply sr = {};
        if (gpu.method(NativeAdrenoMethodSubmit, &submit, sizeof submit, &sr, sizeof sr)) {gProbe[1 + v] = 'x'; probeFree(gpu, next); continue;}
        if (v == 3) {
            usleep(20000);   // the CP is spinning on the flag
            probeFree(gpu, source);
            if (probeMap(gpu, next, kSource)) {gProbe[1 + v] = 'x'; probeFree(gpu, next); stuck = true; break;}
            source = next;
            flag.words[0] = 1;
            __asm__ volatile("dsb sy" ::: "memory");
        }
        int waited = -1;
        for (unsigned t = 0; t < 10 && waited; ++t) {
            NativeAdrenoAbiWaitFence wait = {sr.fence, 200000000ull};
            waited = gpu.method(NativeAdrenoMethodWaitFence, &wait, sizeof wait, nullptr, 0);
        }
        if (waited) {gProbe[1 + v] = 't'; stuck = true; break;}
        const uint32_t got = result.words[0], want = patterns[v];
        gProbe[1 + v] = got == want ? '1' : (v && got == patterns[v - 1]) ? 'o' : got == 0 ? 'z' : got == 0xdeadbeef ? 'd' : 'g';
    }
    // A stuck CP may still reference the buffers.
    if (!stuck) {probeFree(gpu, source); probeFree(gpu, flag); probeFree(gpu, result); probeFree(gpu, ib);}
}

#ifdef NATIVE_TURNIP_REPLAY
// Prefix replay of Turnip's first submission (recorded on the host by scripts/gen-turnip-replay.py with the tablet's 4 KiB page size, so
// the GPU addresses are the ones Turnip itself uses on the tablet): its buffers are mapped at the recorded GPU addresses and growing
// prefixes of its IB, cut after each TYPE7 packet, are submitted one by one, each waited for up to 2 s. Reported on GPU7 as
// "B <retired>/<prefixes> t<thread variants retired> o<last opcode of the first stuck prefix> c<its dwords> e<error>", markers
// 0x6b00 | thread variant, then 0x6a00 | prefix.
struct Replay {
    static constexpr unsigned kCount = sizeof turnipreplay::kBuffers / sizeof turnipreplay::kBuffers[0];
    uint64_t handles[kCount] = {};
    unsigned ibIndex = kCount;
    bool hung = false;
};
// Allocates and maps every recorded buffer and fills it through a CPU mapping with the given flags: 0x101 (Device, aligned volatile
// stores, as the prefix replay always did) or 0x401 (write combine filled with memcpy/memset, as Turnip fills its BOs).
static int replayLoad(Gpu &gpu, Replay &r, uint32_t mapFlags) {
    using namespace turnipreplay;
    for (unsigned i = 0; i < Replay::kCount; ++i) {
        const Buffer &b = kBuffers[i];
        NativeAdrenoAbiBoAlloc alloc = {b.bytes, 12, 0};
        NativeAdrenoAbiBoAllocReply reply = {};
        if (int e = gpu.method(NativeAdrenoMethodBoAlloc, &alloc, sizeof alloc, &reply, sizeof reply)) return e;
        r.handles[i] = reply.handle;
        NativeAdrenoAbiBoMap map = {reply.handle, b.iova, 3, 0, 2, 0};
        NativeAdrenoAbiBoMapReply mr = {};
        if (int e = gpu.method(NativeAdrenoMethodBoMap, &map, sizeof map, &mr, sizeof mr)) return e;
        NativeAdrenoAbiBoCpuMap cm = {reply.handle};
        NativeAdrenoAbiBoCpuMapReply cr = {};
        if (int e = gpu.method(NativeAdrenoMethodBoCpuMap, &cm, sizeof cm, &cr, sizeof cr)) return e;
        uint64_t address = 0, length = 0;
        if (int e = gpu.map(gpu.conn, cr.token, mach_task_self(), &address, &length, mapFlags)) return e;
        if (length < b.bytes) return 0xbad3;
        if (mapFlags == 0x401) {
            memcpy(reinterpret_cast<void *>(address), b.data, b.words * 4);
            memset(reinterpret_cast<uint8_t *>(address) + b.words * 4, 0, b.bytes - b.words * 4);
        } else {
            volatile uint32_t *w = reinterpret_cast<volatile uint32_t *>(address);
            for (uint64_t k = 0; k < b.bytes / 4; ++k) w[k] = k < b.words ? b.data[k] : 0;
        }
        __asm__ volatile("dsb sy" ::: "memory");
        gpu.unmap(gpu.conn, cr.token, mach_task_self(), address);
        NativeAdrenoAbiBoCpuUnmap cu = {reply.handle, cr.token, 0};
        gpu.method(NativeAdrenoMethodBoCpuUnmap, &cu, sizeof cu, nullptr, 0);
        if (b.iova <= kIbIova && kIbIova < b.iova + b.bytes) r.ibIndex = i;
    }
    return r.ibIndex == Replay::kCount ? 0xbad4 : 0;
}
// Submits the first dwords of the IB and waits up to 2 s; a timeout leaves the buffers referenced by the stuck CP.
static int replayRun(Gpu &gpu, Replay &r, uint32_t dwords) {
    NativeAdrenoAbiSubmit submit = {};
    submit.buffer_count = 1;
    submit.buffers[0] = {r.handles[r.ibIndex], turnipreplay::kIbIova, uint64_t(dwords) * 4};
    NativeAdrenoAbiSubmitReply sr = {};
    if (int e = gpu.method(NativeAdrenoMethodSubmit, &submit, sizeof submit, &sr, sizeof sr)) return e;
    int waited = -1;
    for (unsigned t = 0; t < 10 && waited; ++t) {
        NativeAdrenoAbiWaitFence wait = {sr.fence, 200000000ull};
        waited = gpu.method(NativeAdrenoMethodWaitFence, &wait, sizeof wait, nullptr, 0);
    }
    if (waited) r.hung = true;
    return waited;
}
// A stuck CP still references the buffers; they are only released when everything retired.
static void replayFree(Gpu &gpu, Replay &r) {
    if (r.hung) return;
    for (uint64_t &h : r.handles)
        if (h) {NativeAdrenoAbiBoFree f = {h}; gpu.method(NativeAdrenoMethodBoFree, &f, sizeof f, nullptr, 0); h = 0;}
}
// One whole-IB control run on fresh buffers at the same GPU addresses: '1' retired, '0' stuck, 'x' setup failed. Marker 0x6c00 | tag.
static char replayControl(Gpu &gpu, uint32_t mapFlags, uint32_t tag, bool &hung) {
    Replay r;
    gpu.mark(0x6c00 | tag);
    char result = 'x';
    if (!replayLoad(gpu, r, mapFlags)) result = replayRun(gpu, r, turnipreplay::kCuts[sizeof turnipreplay::kCuts / sizeof turnipreplay::kCuts[0] - 1]) ? (r.hung ? '0' : 'x') : '1';
    hung = r.hung;
    replayFree(gpu, r);
    return result;
}
static char gReplayControls[4] = "---";   // W1 W2 N, appended to the B line
static char gReplayLine[64];
// Whole-IB runs before any prefix (int-065). int-064's remap probe read the new pages in every variant (P1111), yet the whole IB on fresh
// buffers after the prefix series stopped the CP again (W1). These runs separate "a fresh first run stops" (prefix runs had left GPU-written
// memory behind) from "a second whole run stops": E<r1><r2><r3>, r1 the whole IB on fresh buffers before anything else, r2 the same
// buffers again, r3 fresh buffers at the same addresses (W1's case); '1' retired, '0' stuck, 'x' setup failed, '-' not run. After r1
// retires, "d" lists how many words of each buffer differ from the recording and "f" the first changed word outside the fill target, as
// <buffer>@<word offset>=<value>, so the memory the GPU wrote is known.
static char gEarly[48] = "E---";
__attribute__((unused)) static bool earlyRuns(Gpu &gpu) {
    using namespace turnipreplay;
    const uint32_t whole = kCuts[sizeof kCuts / sizeof kCuts[0] - 1];
    char diff[40] = "";
    Replay r;
    gpu.mark(0x6e01);
    if (replayLoad(gpu, r, 0x101)) {gEarly[1] = 'x'; replayFree(gpu, r); return true;}
    gEarly[1] = replayRun(gpu, r, whole) ? (r.hung ? '0' : 'x') : '1';
    if (gEarly[1] == '1') {
        unsigned counts[Replay::kCount] = {}, firstBuffer = 0, firstWord = 0;
        uint32_t firstValue = 0;
        bool found = false;
        for (unsigned i = 0; i < Replay::kCount; ++i) {
            const Buffer &b = kBuffers[i];
            NativeAdrenoAbiBoCpuMap cm = {r.handles[i]};
            NativeAdrenoAbiBoCpuMapReply cr = {};
            if (gpu.method(NativeAdrenoMethodBoCpuMap, &cm, sizeof cm, &cr, sizeof cr)) continue;
            uint64_t address = 0, length = 0;
            if (!gpu.map(gpu.conn, cr.token, mach_task_self(), &address, &length, 0x101)) {
                const volatile uint32_t *w = reinterpret_cast<const volatile uint32_t *>(address);
                for (uint64_t k = 0; k < b.bytes / 4 && k < length / 4; ++k) {
                    const uint32_t want = k < b.words ? b.data[k] : 0;
                    if (w[k] == want) continue;
                    ++counts[i];
                    // The fill target is the recording's empty buffer; its change is expected.
                    if (!found && b.words) {found = true; firstBuffer = i; firstWord = unsigned(k); firstValue = w[k];}
                }
                gpu.unmap(gpu.conn, cr.token, mach_task_self(), address);
            }
            NativeAdrenoAbiBoCpuUnmap cu = {r.handles[i], cr.token, 0};
            gpu.method(NativeAdrenoMethodBoCpuUnmap, &cu, sizeof cu, nullptr, 0);
        }
        int at = snprintf(diff, sizeof diff, " d");
        for (unsigned i = 0; i < Replay::kCount && at < int(sizeof diff); ++i)
            at += snprintf(diff + at, sizeof diff - at, "%s%x", i ? "," : "", counts[i]);
        if (found && at < int(sizeof diff)) snprintf(diff + at, sizeof diff - at, " f%u@%x=%x", firstBuffer, firstWord, firstValue);
    }
    if (gEarly[1] == '1') {gpu.mark(0x6e02); gEarly[2] = replayRun(gpu, r, whole) ? (r.hung ? '0' : 'x') : '1';}
    const bool stuck = r.hung;
    replayFree(gpu, r);
    if (!stuck && gEarly[2] == '1') {
        Replay fresh;
        gpu.mark(0x6e03);
        gEarly[3] = replayLoad(gpu, fresh, 0x101) ? 'x' : replayRun(gpu, fresh, whole) ? (fresh.hung ? '0' : 'x') : '1';
        replayFree(gpu, fresh);
        if (fresh.hung) gEarly[3] = '0';
    }
    snprintf(gEarly + 4, sizeof gEarly - 4, "%s", diff);
    streamText7(gpu, gEarly);
    return gEarly[1] == '1' && gEarly[2] == '1' && gEarly[3] == '1';
}
// Fresh-buffer prefix scan (int-066). int-065: the whole IB on fresh buffers, before any other replay, stopped the CP (E0--), while the
// prefix series on one set of buffers retires every prefix. Here every prefix runs on freshly loaded buffers, so memory written by
// earlier runs cannot help it; the first prefix that stops names the packet that needs that memory. When every prefix retires, CP state
// left by the earlier runs is what helped, and the whole IB then runs once more on fresh buffers (e<result>). Reported on GPU7 as
// "F <retired>/<prefixes> o<opcode ending the stuck prefix> c<its dwords> e<whole run: 1, 0 stuck, - not run>".
static bool freshScan(Gpu &gpu) {
    using namespace turnipreplay;
    constexpr unsigned kPrefixes = sizeof kCuts / sizeof kCuts[0];
    unsigned retired = 0;
    bool stuck = false;
    char whole = '-';
    for (unsigned p = 0; p < kPrefixes && !stuck; ++p) {
        Replay r;
        gpu.mark(0x6f80 | p);
        int e = replayLoad(gpu, r, 0x101);
        if (!e) e = replayRun(gpu, r, kCuts[p]);
        stuck = r.hung;
        replayFree(gpu, r);
        if (e) break;
        ++retired;
    }
    if (retired == kPrefixes) {
        Replay r;
        gpu.mark(0x6fa0);
        whole = replayLoad(gpu, r, 0x101) ? 'x' : replayRun(gpu, r, kCuts[kPrefixes - 1]) ? (r.hung ? '0' : 'x') : '1';
        stuck = r.hung;
        replayFree(gpu, r);
    }
    char text[64];
    snprintf(text, sizeof text, "F %u/%u o%x c%u e%c", retired, kPrefixes, retired < kPrefixes ? kCutOps[retired] : 0,
             retired < kPrefixes ? kCuts[retired] : 0, whole);
    streamText7(gpu, text);
    return !stuck && whole == '1';
}
static bool replayPhase(Gpu &gpu) {
    using namespace turnipreplay;
    constexpr unsigned kPrefixes = sizeof kCuts / sizeof kCuts[0];
    Replay r;
    unsigned retired = 0, variants = 0;
    int error = replayLoad(gpu, r, 0x101);
    // The first packet is CP_THREAD_CONTROL(BOTH) and int-060 stopped on it alone. Before the prefixes, the same two dwords run with
    // BR only (1) and BV only (2), so a hang names the thread: t<variants retired>.
    auto patch = [&](uint32_t value) {
        const Buffer &b = kBuffers[r.ibIndex];
        NativeAdrenoAbiBoCpuMap cm = {r.handles[r.ibIndex]};
        NativeAdrenoAbiBoCpuMapReply cr = {};
        if (int e = gpu.method(NativeAdrenoMethodBoCpuMap, &cm, sizeof cm, &cr, sizeof cr)) return e;
        uint64_t address = 0, length = 0;
        if (int e = gpu.map(gpu.conn, cr.token, mach_task_self(), &address, &length, 0x101)) return e;
        reinterpret_cast<volatile uint32_t *>(address)[(kIbIova - b.iova) / 4 + 1] = value;
        __asm__ volatile("dsb sy" ::: "memory");
        gpu.unmap(gpu.conn, cr.token, mach_task_self(), address);
        NativeAdrenoAbiBoCpuUnmap cu = {r.handles[r.ibIndex], cr.token, 0};
        gpu.method(NativeAdrenoMethodBoCpuUnmap, &cu, sizeof cu, nullptr, 0);
        return 0;
    };
    const uint32_t original = r.ibIndex < Replay::kCount ? kBuffers[r.ibIndex].data[(kIbIova - kBuffers[r.ibIndex].iova) / 4 + 1] : 0;
    for (uint32_t thread = 1; thread <= 2 && !error; ++thread) {
        gpu.mark(0x6b00 | thread);
        if (!(error = patch(thread)) && !(error = replayRun(gpu, r, 2))) ++variants;
    }
    if (!error) error = patch(original);
    for (unsigned p = 0; p < kPrefixes && !error; ++p) {
        gpu.mark(0x6a00 | p);
        if ((error = replayRun(gpu, r, kCuts[p]))) break;
        ++retired;
    }
    replayFree(gpu, r);
    // Turnip's own fill of this IB stopped in int-062 while this replay retired. The control runs separate the remaining differences, each
    // on fresh buffers at the same addresses: W1 Device-mapped stores (address reuse after a free), W2 write-combine memcpy (how Turnip
    // fills its BOs); the caller adds N, a second connection (a second context entry on a CP that already ran work).
    bool stuck = r.hung;
    if (retired == kPrefixes && !stuck) gReplayControls[0] = replayControl(gpu, 0x101, 1, stuck);
    if (retired == kPrefixes && !stuck) gReplayControls[1] = replayControl(gpu, 0x401, 2, stuck);
    snprintf(gReplayLine, sizeof gReplayLine, "%.4s B %u/%u t%u o%x c%u e%x ", gEarly, retired, kPrefixes, variants,
             retired < kPrefixes ? kCutOps[retired] : 0, retired < kPrefixes ? kCuts[retired] : 0, unsigned(error));
    char text[64];
    snprintf(text, sizeof text, "%s%s", gReplayLine, gReplayControls);
    streamText7(gpu, text);
    return retired == kPrefixes && !stuck;
}
// N: the whole IB on a second connection, whose first submission carries the A7XX context entry again (Turnip's connection does that
// after the replay). Returns false when the CP stopped.
static bool replaySecondClient(Gpu &gpu) {
    unsigned conn = 0;
    if (gpu.openFn(gpu.port, mach_task_self(), 0x4147, &conn) != 0 || !conn) {gReplayControls[2] = 'o'; return true;}
    const unsigned first = gpu.conn;
    volatile uint32_t *mailbox = gpu.mailbox;
    gpu.conn = conn;
    gpu.mailbox = nullptr;
    bool ok = !mailbox || gpu.openMailbox();
    bool stuck = false;
    gReplayControls[2] = ok ? replayControl(gpu, 0x101, 3, stuck) : 'm';
    gpu.closeFn(conn);
    gpu.conn = first;
    gpu.mailbox = mailbox;
    char text[64];
    snprintf(text, sizeof text, "%s%s", gReplayLine, gReplayControls);
    streamText7(gpu, text);
    return !stuck;
}
#endif

// Run after a passing Turnip phase by the plugin (the Metal bridge self-test); null in the probe daemon.
static void (*gAfterTurnip)(Gpu &gpu, const char *turnipPath);

// Turnip phase: only where libvulkan_freedreno.dylib sits next to this code (the plugin bundle), never in the probe daemon.
static void turnipPhase(Gpu &gpu) {
    char path[1024];
    if (!turnip::libraryPath(path, sizeof path) || access(path, R_OK) != 0) return;
    gpu.mark(0x6000);
    auto marker = [&](uint32_t value) { gpu.mark(value); };
    const turnip::Result result = turnip::runWatched(marker, [&](const char *text) { streamText(gpu, text); }, path);
    char text[NativeAdrenoAbiMetadataBytes] = {};
    snprintf(text, sizeof text, "T s%d r%d v%d c%d%s g%d/%d%s %.30s", result.stage, result.vkResult, result.verified, result.computeWords,
             result.computeStage ? "!" : "", result.renderTriangle, result.renderOther, result.renderStage ? "!" : "", turnip::gFirstError);
    streamText(gpu, text);
    gpu.mark(result.ok ? 0x6fff : 0x6ffe);
    // Also when results are wrong but every Vulkan step ran: one boot then shows both layers.
    if (result.stage == 0 && result.vkResult == 0 && gAfterTurnip) gAfterTurnip(gpu, path);
}

// Returns the GPU-verified dword count of the uncached run, or a negative number; every outcome is also reported to the kernel.
static int adrenoUserTest(unsigned port, const char *process) {
    void *iokit = dlopen("/System/Library/Frameworks/IOKit.framework/IOKit", RTLD_NOW | RTLD_LOCAL);
    if (!iokit) return -1;
    auto open = (IOServiceOpenFn)dlsym(iokit, "IOServiceOpen");
    auto close = (IOServiceCloseFn)dlsym(iokit, "IOServiceClose");
    Gpu gpu;
    gpu.call = (CallFn)dlsym(iokit, "IOConnectCallStructMethod");
    gpu.map = (MapFn)dlsym(iokit, "IOConnectMapMemory64");
    gpu.unmap = (UnmapFn)dlsym(iokit, "IOConnectUnmapMemory64");
    gpu.port = port;
    gpu.openFn = open;
    gpu.closeFn = close;
    gpu.mark(1);
    if (!open || !close || !gpu.call || !gpu.map || !gpu.unmap) return -1;
    gpu.mark(2);
    // 0x4147 ('AG') is the open type the driver serves; the original IOGPU library's type-1 opens are refused.
    const int openResult = open(port, mach_task_self(), 0x4147, &gpu.conn);
    if (openResult != 0 || !gpu.conn) {
        // The exact kern_return_t, as two markers (high and low half), after the failure marker.
        gpu.mark(0x4f01);
        gpu.mark(((uint32_t)openResult >> 16) & 0xffff ? ((uint32_t)openResult >> 16) & 0xffff : 0xffff);
        gpu.mark((uint32_t)openResult & 0xffff ? (uint32_t)openResult & 0xffff : 0xffff);
        return -1;
    }
    gpu.mark(3);
    const bool probed = gpu.probe();
    gpu.mark(0x1000 | (gpu.admitted & 0xfff));
    bool mailboxOk = false;
    if (!probed) {mailboxOk = gpu.openMailbox(); gpu.mark(mailboxOk ? 0x2001 : 0x2000);}
    if (!probed && !mailboxOk) {close(gpu.conn); return -2;}   // neither channel works: the driver's E row shows what reached it
    // mapFlags are what the CLIENT asks for: IOUserClient::mapClientMemory64 lets them override the cache mode the driver returns, so
    // the cache bits must be set here. 0x101 = anywhere + inhibit cache (aligned dword accesses only), 0x401 = anywhere + write
    // combine (Normal non-cacheable; what a Vulkan driver needs, since it uses memcpy and unaligned stores).
    int step = 0, error = 0;
    const int verified = runTest(gpu, 0x101, step, error);
    int stepWc = 0, errorWc = 0;
    const int verifiedWc = error ? -2 : runTest(gpu, 0x401, stepWc, errorWc);
    report(gpu, process, step + stepWc, error ? error : errorWc, verified, verifiedWc, gpu.lastTicks);
    gpu.mark(0x5000 | (verified & 0xff));
    bool replayed = true;
#ifdef NATIVE_TURNIP_REPLAY
    // Plugin only (the Turnip library sits next to it): a stuck replay leaves the CP stopped, so Turnip is not started after it.
    char turnipPath[1024];
    if (verified == 4 && verifiedWc == 4 && turnip::libraryPath(turnipPath, sizeof turnipPath) && access(turnipPath, R_OK) == 0) {
        replayed = freshScan(gpu) && replayPhase(gpu);
    }
#endif
    close(gpu.conn);
#ifdef NATIVE_TURNIP_REPLAY
    if (replayed && verified == 4 && verifiedWc == 4) replayed = replaySecondClient(gpu);
#endif
    if (verified == 4 && verifiedWc == 4 && replayed) turnipPhase(gpu);
    return verified;
}
