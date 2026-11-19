/**
 * @file test_ctr_random.cpp
 * @brief Random boot / packet / power-cut sequences (fixed seeds) against the no-reuse
 *        invariant: no counter value that could have been sent is ever used again.
 *
 * Each sequence starts from a fault-free log (blank, or as after n boots, mostly near a page
 * boundary), then runs 10..59 sessions: a boot, 0..855 packets (often across a block rollover),
 * and in 40 % of the sessions a power cut armed at one of the next 8 flash operations, either as
 * it starts or with a random partial result (half-programmed word, half-erased page). A session
 * without a cut ends with a reset between packets. The model of what may have been sent is
 * Sim's (sim.h): every packet whose counter rule returned true (held packets are not sent).
 *
 * Longer runs: CTR_RANDOM_SCALE=<n> runs n times as many sequences, CTR_RANDOM_SEED=<s> adds s
 * to the seeds (e.g. CTR_RANDOM_SCALE=20 CTR_RANDOM_SEED=7000000 make).
 */

#include <stdlib.h>

#include <random>
#include <string>
#include <vector>

#include "sim.h"
#include "test.h"

namespace {

struct Faults {
    bool worn;      // cells that no longer program (writes do not read back)
    bool crystal;   // clock_xosc() timed out for some packets (no flash operation, no radio)
};

struct Totals {
    unsigned sequences = 0, cuts = 0, erases = 0, words = 0, errors = 0, rewrites = 0;
    unsigned held = 0;        // packets held by the counter rule
    unsigned bad = 0;         // sequences with a reused counter below CTR_BLOCK_MAX
    unsigned saturated = 0;   // sequences that reused a counter only above it (log saturated)
    std::string first, firstSaturated;
};

void runSequence(const ctr_dev& d, uint32_t seed, const Faults& f, Totals& t) {
    std::mt19937 rng(seed);
    auto r = [&](unsigned n) { return (unsigned)(rng() % n); };

    emu_init();
    Sim s(d);
    s.note("[%s, seed %u] ", d.name, seed);
    unsigned n = 0;
    switch (r(4)) {
    case 0: n = 0; break;
    case 1: n = r(2 * d.entries + 2); break;
    default: {
        int m = (int)((1 + r(3)) * d.entries) + (int)r(5) - 2;
        n = m < 0 ? 0 : (unsigned)m;
    }
    }
    uint32_t last = prefill(d, n);
    s.note("[log as after %u boots] ", n);
    if (n)
        s.sentUpTo((last << 8) | 0xFF);

    if (f.worn) {
        unsigned count = 1 + r(12);
        for (unsigned i = 0; i < count; i++) {
            unsigned pg = d.page0 + r(2), slot = r(d.entries), byte = r(4);
            uint8_t bits = r(3) ? (uint8_t)(1u << r(8)) : 0xFF;
            emu_stuck1[pg * EMU_PAGE_SIZE + slot * 4 + byte] |= bits;
            s.note("[worn p%u s%u b%u %02X] ", pg, slot, byte, bits);
        }
        if (r(3) == 0) {   // a worn run of slots
            unsigned pg = d.page0 + r(2), slot = r(d.entries), len = 3 + r(6);
            for (unsigned i = 0; i < len && slot + i < d.entries; i++)
                emu_stuck1[pg * EMU_PAGE_SIZE + (slot + i) * 4 + 3] |= 0x01;
            s.note("[worn p%u s%u..+%u] ", pg, slot, len);
        }
    }

    unsigned sessions = 10 + r(50);
    for (unsigned k = 0; k < sessions; k++) {
        bool armed = r(10) < 4;
        if (armed) {
            unsigned at = emu_stats.ops + r(8);
            bool partial = r(2);
            emu_cut_at(at, partial ? EMU_CUT_PARTIAL : EMU_CUT_BEFORE, (uint32_t)rng());
            s.note("[cut at op %u %s] ", at, partial ? "partial" : "start");
        }
        if (s.boot()) {
            unsigned c = r(10), npk;
            if (c < 5)
                npk = r(5);
            else if (c < 9)
                npk = 256 - (s.ctr & 0xFF) + r(4);   // across the next rollover
            else
                npk = 256 + r(600);
            unsigned i = 0;
            uint32_t from = s.ctr;
            for (; i < npk; i++) {
                bool noXtal = f.crystal && r((s.ctr & 0xFF) == 0 ? 4 : 64) == 0;
                if (noXtal) {
                    if (i > 0)
                        s.note("pkts %08X..%08X ", from, s.ctr - 1);
                    s.note("[no crystal] ");
                    CLKCON |= 0x40;
                }
                bool ok = s.packet(!noXtal);   // no crystal: no radio either, nothing sent
                CLKCON = (uint8_t)(CLKCON & ~0x40);
                if (noXtal) {
                    s.note("pkt %08X%s ", s.ctr - (ok ? 1 : 0), ok ? "" : ":CUT");
                    from = s.ctr;
                }
                if (!ok)
                    break;
            }
            if (i && s.ctr != from && s.on)
                s.note("pkts %08X..%08X ", from, s.ctr - 1);
        }
        if (armed && !emu_cut_armed())
            t.cuts++;
        emu_cut_clear();
    }
    s.boot();
    s.packets(1);

    t.sequences++;
    t.erases += emu_stats.erases;
    t.words += emu_stats.words;
    t.errors += emu_stats.errors;
    t.rewrites += emu_stats.rewrites;
    t.held += s.held;
    if (s.violations > s.saturated) {
        if (!t.bad)
            t.first = s.firstBad;
        t.bad++;
    } else if (s.violations) {
        if (!t.saturated)
            t.firstSaturated = s.firstBad;
        t.saturated++;
    }
}

unsigned envNum(const char* name, unsigned dflt) {
    const char* v = getenv(name);
    return v && *v ? (unsigned)strtoul(v, nullptr, 0) : dflt;
}

Totals runMany(const ctr_dev& d, uint32_t seed0, unsigned count, const Faults& f) {
    Totals t;
    count *= envNum("CTR_RANDOM_SCALE", 1);
    seed0 += envNum("CTR_RANDOM_SEED", 0);
    for (unsigned i = 0; i < count; i++)
        runSequence(d, seed0 + i, f, t);
    printf("    %s: %u sequences, %u power cuts in a flash operation, %u erases, %u words "
           "written, %u packets held; reused counters: %u sequences, %u more after a "
           "saturated log\n",
           d.name, t.sequences, t.cuts, t.erases, t.words, t.held, t.bad, t.saturated);
    if (t.bad)
        printf("      first: %s\n", t.first.c_str());
    if (t.saturated)
        printf("      first with a saturated log: %s\n", t.firstSaturated.c_str());
    if (t.errors)
        printf("      emulator errors: %u (%s)\n", t.errors, emu_last_error);
    return t;
}

/** The power cut runs, made once for the two tests below. */
const std::vector<Totals>& powerCutRuns() {
    static std::vector<Totals> runs;
    if (runs.empty()) {
        const Faults none = {false, false};
        runs.push_back(runMany(dev_door, 1000, 2000, none));
        runs.push_back(runMany(dev_door4, 1000, 4000, none));
        runs.push_back(runMany(dev_cam, 1000, 2000, none));
    }
    return runs;
}

}  // namespace

TEST(random_power_cuts_never_reuse_a_counter) {
    unsigned total = 0;
    for (const Totals& t : powerCutRuns()) {
        CHECK_EQ(t.bad, 0);
        CHECK_EQ(t.errors, 0);
        CHECK_EQ(t.rewrites, 0);
        total += t.sequences;
    }
    printf("    %u random sequences\n", total);
}

/* Until 2026-11-19 some random cut writes left a valid entry at or near CTR_BLOCK_MAX (XOR
 * check byte), after which the boots reused counter values. */
TEST(random_power_cuts_never_saturate_the_log) {
    unsigned saturated = 0;
    for (const Totals& t : powerCutRuns())
        saturated += t.saturated;
    CHECK_EQ(saturated, 0);
}

/* Until 2026-11-19, with worn cells or a crystal that does not start, a block could be used
 * without being logged (the boot's block, or two blocks in a row), and a skipped erase could
 * lose the newest entry: counter values were reused after a reset. */
TEST(random_worn_cells_and_crystal_failures_never_reuse_a_counter) {
    const Faults faults = {true, true};
    unsigned bad = 0;
    for (const ctr_dev* d : {&dev_door, &dev_door4, &dev_cam}) {
        Totals t = runMany(*d, 50000, 500, faults);
        bad += t.bad + t.saturated;
        CHECK_EQ(t.errors, 0);
        CHECK_EQ(t.rewrites, 0);
    }
    CHECK_EQ(bad, 0);
}
