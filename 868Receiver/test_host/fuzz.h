/**
 * @file fuzz.h
 * @brief Helpers of the deterministic fuzz/property tests (fuzz_*.cpp): a seeded PRNG, the
 *        iteration budget, and builders of MD CAM style JPEGs and delta blobs (see jpeg_splice.h).
 *
 * The tests are deterministic: every test has a fixed seed, so a failure repeats on every run.
 * FUZZ_SCALE=<n> runs n times as many iterations, FUZZ_SEED=<n> varies the seeds (both from the
 * environment, for longer local runs: FUZZ_SCALE=50 FUZZ_SEED=7 make -C 868Receiver/test_host fuzz).
 */

#pragma once

#include "test.h"
#include <stdint.h>
#include <stdlib.h>
#include <vector>

/* ---- PRNG and budget ------------------------------------------------------------------------- */
inline uint64_t fuzzSeedOffset() {
    const char* s = getenv("FUZZ_SEED");
    return s ? strtoull(s, nullptr, 0) * 0x9E3779B97F4A7C15ULL : 0;
}

/** base iterations times FUZZ_SCALE (default 1). */
inline int fuzzIters(int base) {
    const char* s = getenv("FUZZ_SCALE");
    long k = s ? strtol(s, nullptr, 0) : 1;
    return (int)(base * (k > 0 ? k : 1));
}

/** splitmix64: small, fast and good enough for test input. */
struct Rng {
    uint64_t s;
    explicit Rng(uint64_t seed) : s(seed ^ fuzzSeedOffset()) {}
    uint64_t next() {
        uint64_t z = (s += 0x9E3779B97F4A7C15ULL);
        z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ULL;
        z = (z ^ (z >> 27)) * 0x94D049BB133111EBULL;
        return z ^ (z >> 31);
    }
    /** 0 .. n-1 (0 if n is 0). */
    uint32_t below(uint32_t n) { return n ? (uint32_t)(next() % n) : 0; }
    /** lo .. hi inclusive. */
    uint32_t range(uint32_t lo, uint32_t hi) { return lo + below(hi - lo + 1); }
    bool chance(unsigned percent) { return below(100) < percent; }
    uint8_t byte() { return (uint8_t)next(); }
    void fill(uint8_t* p, size_t n) {
        for (size_t i = 0; i < n; i++) p[i] = byte();
    }
};

/** Report the seed and iteration of the first failure of a fuzz loop; true = stop the loop. */
inline bool fuzzStop(const char* test, int iter) {
    if (!testCurrentFailed()) return false;
    printf("    %s: first failure in iteration %d (FUZZ_SEED=%s)\n", test, iter,
           getenv("FUZZ_SEED") ? getenv("FUZZ_SEED") : "0");
    return true;
}

/* ---- Mutations ------------------------------------------------------------------------------- */
/** One random change: bit flip, byte set, insertion, deletion or truncation. */
inline void fuzzMutate(Rng& r, std::vector<uint8_t>& v) {
    static const uint8_t interesting[] = {0x00, 0x01, 0x02, 0x04, 0x7F, 0x80, 0xD0, 0xD7,
                                          0xD8, 0xD9, 0xDA, 0xFE, 0xFF, 'D', '1', 'I'};
    switch (r.below(6)) {
    case 0:
        if (!v.empty()) v[r.below((uint32_t)v.size())] ^= (uint8_t)(1u << r.below(8));
        break;
    case 1:
        if (!v.empty()) v[r.below((uint32_t)v.size())] = interesting[r.below(sizeof(interesting))];
        break;
    case 2:
        v.insert(v.begin() + r.below((uint32_t)v.size() + 1), interesting[r.below(sizeof(interesting))]);
        break;
    case 3:
        if (!v.empty()) v.erase(v.begin() + r.below((uint32_t)v.size()));
        break;
    case 4:
        v.resize(r.below((uint32_t)v.size() + 1));
        break;
    default:
        if (!v.empty()) v[r.below((uint32_t)v.size())] = r.byte();
        break;
    }
}

/* ---- MD CAM style JPEG and delta builders (structure as described in jpeg_splice.h) ---------- */
struct FuzzJpeg {
    std::vector<uint8_t> header;                // SOI .. SOS segment
    std::vector<std::vector<uint8_t>> segs;     // entropy-coded data of each restart interval
};

/** Entropy-coded bytes: never a marker, 0xFF is stuffed as FF 00 (n bytes before stuffing). */
inline std::vector<uint8_t> fuzzEntropy(Rng& r, size_t n) {
    std::vector<uint8_t> v;
    for (size_t i = 0; i < n; i++) {
        uint8_t b = r.byte();
        v.push_back(b);
        if (b == 0xFF) v.push_back(0x00);
    }
    return v;
}

/** Header: SOI, COM 'I' id (id < 0: none), DQT, SOF0 width x height, SOS. */
inline std::vector<uint8_t> fuzzJpegHeader(unsigned width, unsigned height, int id) {
    std::vector<uint8_t> h = {0xFF, 0xD8};
    if (id >= 0) {
        uint8_t com[] = {0xFF, 0xFE, 0x00, 0x04, 'I', (uint8_t)id};
        h.insert(h.end(), com, com + sizeof(com));
    }
    h.insert(h.end(), {0xFF, 0xDB, 0x00, 0x43, 0x00});
    for (int i = 0; i < 64; i++) h.push_back((uint8_t)(1 + i));
    uint8_t sof[] = {0xFF, 0xC0, 0x00, 0x11, 0x08, (uint8_t)(height >> 8), (uint8_t)height,
                     (uint8_t)(width >> 8), (uint8_t)width, 0x03, 0x01, 0x22, 0x00, 0x02, 0x11, 0x01,
                     0x03, 0x11, 0x01};
    h.insert(h.end(), sof, sof + sizeof(sof));
    uint8_t sos[] = {0xFF, 0xDA, 0x00, 0x0C, 0x03, 0x01, 0x00, 0x02, 0x11, 0x03, 0x11, 0x00, 0x3F, 0x00};
    h.insert(h.end(), sos, sos + sizeof(sos));
    return h;
}

/** A picture of nSegs restart intervals of 0..maxSeg entropy bytes each. */
inline FuzzJpeg fuzzMakeJpeg(Rng& r, unsigned width, int id, unsigned nSegs, unsigned maxSeg) {
    FuzzJpeg j;
    j.header = fuzzJpegHeader(width, width * 3 / 4, id);
    for (unsigned k = 0; k < nSegs; k++) j.segs.push_back(fuzzEntropy(r, r.below(maxSeg + 1)));
    return j;
}

/** Header and intervals joined by RST0..7, ended by EOI (what jpegApplyDelta() writes too). */
inline std::vector<uint8_t> fuzzAssemble(const FuzzJpeg& j) {
    std::vector<uint8_t> v = j.header;
    for (size_t k = 0; k < j.segs.size(); k++) {
        v.insert(v.end(), j.segs[k].begin(), j.segs[k].end());
        v.push_back(0xFF);
        v.push_back(k + 1 < j.segs.size() ? (uint8_t)(0xD0 + (k & 7)) : 0xD9);
    }
    return v;
}

struct FuzzDeltaEntry {
    uint16_t idx;
    std::vector<uint8_t> data;
};

/** Delta blob: 'D' '1' id, total MCUs, entry count, entries (idx, len, data). */
inline std::vector<uint8_t> fuzzMakeDelta(int id, unsigned total, const std::vector<FuzzDeltaEntry>& e) {
    std::vector<uint8_t> d = {'D', '1', (uint8_t)id, (uint8_t)total, (uint8_t)(total >> 8),
                              (uint8_t)e.size(), (uint8_t)(e.size() >> 8)};
    for (const FuzzDeltaEntry& x : e) {
        d.push_back((uint8_t)x.idx);
        d.push_back((uint8_t)(x.idx >> 8));
        d.push_back((uint8_t)x.data.size());
        d.push_back((uint8_t)(x.data.size() >> 8));
        d.insert(d.end(), x.data.begin(), x.data.end());
    }
    return d;
}

/** Random valid delta entries for a picture of 'total' intervals (later entries win). */
inline std::vector<FuzzDeltaEntry> fuzzDeltaEntries(Rng& r, unsigned total, unsigned maxEntries, unsigned maxSeg) {
    std::vector<FuzzDeltaEntry> e;
    unsigned n = r.below(maxEntries + 1);
    for (unsigned i = 0; i < n; i++) {
        FuzzDeltaEntry x;
        x.idx = (uint16_t)r.below(total);
        x.data = fuzzEntropy(r, r.range(1, maxSeg));
        e.push_back(x);
    }
    return e;
}

/** Reference splice: the picture with the delta's intervals replaced. */
inline FuzzJpeg fuzzSplice(const FuzzJpeg& base, const std::vector<FuzzDeltaEntry>& e) {
    FuzzJpeg j = base;
    for (const FuzzDeltaEntry& x : e) j.segs[x.idx] = x.data;
    return j;
}

/** Copy of v in a heap block of exactly v.size() bytes, so ASan sees any read past the end. */
struct ExactBuf {
    uint8_t* p;
    size_t n;
    explicit ExactBuf(const std::vector<uint8_t>& v) : p(new uint8_t[v.size()]), n(v.size()) {
        if (n) memcpy(p, v.data(), n);
    }
    explicit ExactBuf(size_t len) : p(new uint8_t[len]), n(len) {}
    ~ExactBuf() { delete[] p; }
    ExactBuf(const ExactBuf&) = delete;
    ExactBuf& operator=(const ExactBuf&) = delete;
};
