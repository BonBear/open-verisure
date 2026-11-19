/**
 * @file fuzz_jpeg_splice.cpp
 * @brief Fuzz/property tests of jpeg_splice.cpp: jpegCommentId, jpegIsDelta, jpegDeltaId and
 *        jpegApplyDelta with random bytes, valid MD CAM pictures and deltas (checked against a
 *        reference splice), and mutated ones. Every input sits in a heap block of its exact size
 *        and the output buffer too, so AddressSanitizer reports any access past an end.
 */

#include "fuzz.h"
#include "jpeg_splice.h"

namespace {

struct ApplyResult {
    size_t len;
    unsigned int updated;
    std::vector<uint8_t> out;
};

/** jpegApplyDelta() with every buffer exactly sized; checks the length bound. */
ApplyResult apply(const std::vector<uint8_t>& base, const std::vector<uint8_t>& delta, size_t outMax) {
    ExactBuf b(base), d(delta), o(outMax);
    ApplyResult r;
    r.updated = 12345;
    r.len = jpegApplyDelta(b.p, b.n, d.p, d.n, o.p, o.n, &r.updated);
    CHECK(r.len <= outMax);
    if (r.len > outMax) r.len = 0;
    r.out.assign(o.p, o.p + r.len);
    return r;
}

/** The read-only parsers on an exactly sized copy; checks their result ranges. */
void parse(const std::vector<uint8_t>& v) {
    ExactBuf b(v);
    int c = jpegCommentId(b.p, b.n);
    CHECK(c >= -1 && c <= 255);
    bool isDelta = jpegIsDelta(b.p, b.n);
    int id = jpegDeltaId(b.p, b.n);
    CHECK(id >= -1 && id <= 255);
    CHECK_EQ(isDelta, id >= 0);
    if (isDelta) CHECK(b.n >= 7);
}

/** Entry count of a delta blob that jpegApplyDelta() can parse at most. */
unsigned deltaCount(const std::vector<uint8_t>& d) { return d.size() >= 7 ? d[5] | (d[6] << 8) : 0; }

/** Structure every non-empty output must have: SOI at the start, EOI at the end. */
void checkOutputShape(const ApplyResult& r, const std::vector<uint8_t>& delta) {
    if (!r.len) {
        CHECK_EQ(r.updated, 0);
        return;
    }
    REQUIRE(r.len >= 6);
    CHECK(r.out[0] == 0xFF && r.out[1] == 0xD8);
    CHECK(r.out[r.len - 2] == 0xFF && r.out[r.len - 1] == 0xD9);
    CHECK(r.updated <= deltaCount(delta));
}

}  // namespace

TEST(fuzz_splice_random_bytes) {
    Rng r(0x5011CE01);
    const int n = fuzzIters(20000);
    for (int it = 0; it < n; it++) {
        std::vector<uint8_t> a(r.below(r.chance(90) ? 64 : 600)), d(r.below(r.chance(90) ? 64 : 600));
        r.fill(a.data(), a.size());
        r.fill(d.data(), d.size());
        // give the parsers something to chew on: a JPEG start, a COM, a delta start
        if (a.size() >= 2 && r.chance(70)) { a[0] = 0xFF; a[1] = 0xD8; }
        if (a.size() >= 4 && r.chance(30)) { a[2] = 0xFF; a[3] = r.chance(50) ? 0xFE : 0xDA; }
        if (d.size() >= 2 && r.chance(70)) { d[0] = 'D'; d[1] = '1'; }
        if (d.size() >= 5 && r.chance(50)) { d[3] = (uint8_t)r.range(1, 8); d[4] = 0; }
        parse(a);
        parse(d);
        ApplyResult res = apply(a, d, r.below(800));
        checkOutputShape(res, d);
        if (!jpegIsDelta(d.data(), d.size())) CHECK_EQ(res.len, 0);
        if (fuzzStop("fuzz_splice_random_bytes", it)) return;
    }
}

TEST(fuzz_splice_valid_matches_reference) {
    Rng r(0x5011CE02);
    const int n = fuzzIters(600);
    for (int it = 0; it < n; it++) {
        int id = (int)r.below(256);
        unsigned segs = r.chance(5) ? JPEG_SPLICE_MAX_SEGS : r.range(1, 300);
        FuzzJpeg base = fuzzMakeJpeg(r, 320, id, segs, r.chance(20) ? 0 : r.range(1, 50));
        std::vector<uint8_t> baseBytes = fuzzAssemble(base);
        std::vector<FuzzDeltaEntry> e = fuzzDeltaEntries(r, segs, 40, 80);
        std::vector<uint8_t> delta = fuzzMakeDelta(id, segs, e);
        std::vector<uint8_t> want = fuzzAssemble(fuzzSplice(base, e));

        CHECK_EQ(jpegCommentId(baseBytes.data(), baseBytes.size()), id);
        CHECK_EQ(jpegDeltaId(delta.data(), delta.size()), id);

        ApplyResult res = apply(baseBytes, delta, want.size() + r.below(64));
        CHECK_EQ(res.len, want.size());
        CHECK(res.out == want);
        CHECK_EQ(res.updated, e.size());

        // the exact size fits, one byte less does not
        CHECK_EQ(apply(baseBytes, delta, want.size()).len, want.size());
        CHECK_EQ(apply(baseBytes, delta, want.size() - 1).len, 0);
        // a delta for another number of restart intervals does not apply
        std::vector<uint8_t> other = fuzzMakeDelta(id, segs + 1, {});
        CHECK_EQ(apply(baseBytes, other, want.size() + 64).len, 0);
        // the base without its EOI (truncated picture) still splits into the same intervals
        std::vector<uint8_t> cut(baseBytes.begin(), baseBytes.end() - 2);
        if (!base.segs.back().empty()) {
            ApplyResult t = apply(cut, delta, want.size() + 64);
            CHECK(t.out == want);
        }
        if (fuzzStop("fuzz_splice_valid_matches_reference", it)) return;
    }
}

TEST(fuzz_splice_limits) {
    Rng r(0x5011CE03);
    // more restart intervals than the splicer keeps track of
    FuzzJpeg many = fuzzMakeJpeg(r, 320, 1, JPEG_SPLICE_MAX_SEGS + 1, 2);
    std::vector<uint8_t> manyBytes = fuzzAssemble(many);
    CHECK_EQ(apply(manyBytes, fuzzMakeDelta(1, JPEG_SPLICE_MAX_SEGS + 1, {}), 1 << 17).len, 0);
    CHECK_EQ(apply(manyBytes, fuzzMakeDelta(1, JPEG_SPLICE_MAX_SEGS, {}), 1 << 17).len, 0);
    // an interval longer than 0xFFFF bytes (16-bit length) is refused, with and without EOI
    FuzzJpeg big;
    big.header = fuzzJpegHeader(320, 240, 2);
    big.segs.push_back(std::vector<uint8_t>(0x10000, 0x11));
    big.segs.push_back(std::vector<uint8_t>(10, 0x22));
    std::vector<uint8_t> bigBytes = fuzzAssemble(big);
    CHECK_EQ(apply(bigBytes, fuzzMakeDelta(2, 2, {}), 1 << 18).len, 0);
    std::vector<uint8_t> tail = fuzzJpegHeader(320, 240, 2);
    tail.insert(tail.end(), 0x10000, 0x33);
    CHECK_EQ(apply(tail, fuzzMakeDelta(2, 1, {}), 1 << 18).len, 0);
    tail.resize(tail.size() - 1);            // 0xFFFF bytes: the limit
    CHECK_EQ(apply(tail, fuzzMakeDelta(2, 1, {}), 1 << 18).len, tail.size() + 2);
    // an entry with the largest length that the blob does not hold stops the parsing
    FuzzJpeg one = fuzzMakeJpeg(r, 320, 3, 4, 10);
    std::vector<uint8_t> d = fuzzMakeDelta(3, 4, {{1, {0x44}}});
    d[5] = 2;                                // claims a second entry ...
    d.insert(d.end(), {0x02, 0x00, 0xFF, 0xFF, 0x55});   // ... of 65535 bytes, 1 present
    ApplyResult res = apply(fuzzAssemble(one), d, 4096);
    CHECK_EQ(res.updated, 1);
    CHECK(res.out == fuzzAssemble(fuzzSplice(one, {{1, {0x44}}})));
    // empty and tiny inputs
    CHECK_EQ(apply({}, {}, 0).len, 0);
    CHECK_EQ(apply({0xFF, 0xD8}, {'D', '1', 0, 1, 0, 0, 0}, 16).len, 0);
    CHECK_EQ(apply({0xFF, 0xD8, 0xFF, 0xDA}, {'D', '1', 0, 1, 0, 0}, 16).len, 0);
}

TEST(fuzz_splice_mutated_structures) {
    Rng r(0x5011CE04);
    const int n = fuzzIters(6000);
    for (int it = 0; it < n; it++) {
        int id = (int)r.below(256);
        unsigned segs = r.range(1, 120);
        FuzzJpeg base = fuzzMakeJpeg(r, r.chance(80) ? 320 : 640, r.chance(90) ? id : -1, segs, r.range(0, 30));
        std::vector<uint8_t> b = fuzzAssemble(base);
        std::vector<uint8_t> d = fuzzMakeDelta(id, segs, fuzzDeltaEntries(r, segs, 20, 40));
        unsigned mb = r.chance(50) ? r.range(1, 8) : 0, md = r.chance(70) ? r.range(1, 8) : 0;
        if (!mb && !md) mb = 1;
        for (unsigned k = 0; k < mb; k++) fuzzMutate(r, b);
        for (unsigned k = 0; k < md; k++) fuzzMutate(r, d);
        parse(b);
        parse(d);
        size_t outMax = r.chance(80) ? b.size() + d.size() + 2 * segs + 16 : r.below((uint32_t)b.size() + 64);
        ApplyResult res = apply(b, d, outMax);
        checkOutputShape(res, d);
        if (fuzzStop("fuzz_splice_mutated_structures", it)) return;
    }
}
