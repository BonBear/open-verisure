/**
 * @file sim.h
 * @brief Helpers of the counter log tests: log entries, a fast fault-free pre-fill, and Sim,
 *        which runs a device's real ctr_boot and packet rule and checks the no-reuse invariant.
 *
 * "Possibly sent": both firmwares send a packet right after the counter rule (door sensor main
 * loop, camera transmit()) whether or not its block could be logged. So once the rule returns
 * without a power cut, its counter value counts as sent (on the air, maybe received by the
 * gateway, maybe recorded), and every later counter value, in this boot or any later one, must
 * be greater. A power cut inside the rule (during a log write) means the packet was not sent.
 */

#pragma once

#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include <string>

#include "ctr_dev.h"
#include "emu.h"
#include "test.h"

/** The 4 bytes of a log entry as ctr_log() writes them. */
inline void entryBytes(uint32_t block, uint8_t e[4]) {
    e[0] = (uint8_t)block;
    e[2] = (uint8_t)(block >> 8);
    e[3] = (uint8_t)(block >> 16);
    e[1] = (uint8_t)(e[0] ^ e[2] ^ e[3] ^ 0xA5);
}

inline uint8_t* entryPtr(unsigned page, unsigned i) { return &emu_flash[page * EMU_PAGE_SIZE + i * 4]; }

/** Put raw bytes into a slot (test set-up, not through the flash controller). */
inline void putRaw(unsigned page, unsigned i, uint8_t b0, uint8_t b1, uint8_t b2, uint8_t b3) {
    uint8_t* p = entryPtr(page, i);
    p[0] = b0;
    p[1] = b1;
    p[2] = b2;
    p[3] = b3;
}

inline void putEntry(unsigned page, unsigned i, uint32_t block) {
    uint8_t e[4];
    entryBytes(block, e);
    putRaw(page, i, e[0], e[1], e[2], e[3]);
}

/** The newest valid block in the log (what ctr_boot() looks for). */
inline bool newestBlock(const ctr_dev& d, uint32_t* best) {
    bool found = false;
    for (unsigned pg = 0; pg < 2; pg++)
        for (unsigned i = 0; i < d.entries; i++) {
            uint32_t v;
            if (d.decode((uint8_t)(d.page0 + pg), (uint16_t)i, &v) && (!found || v > *best)) {
                *best = v;
                found = true;
            }
        }
    return found;
}

inline uint32_t seedBlock(const ctr_dev& d) {
    return (d.seed >= 1 && d.seed <= d.block_max) ? d.seed : 1;
}

/** Fill a blank log as @p n fault-free boots would (seed, then +2 each, page switch with
 *  erase when full). @return the last block (0 if n == 0). */
inline uint32_t prefill(const ctr_dev& d, unsigned n) {
    unsigned pg = 0, next = 0;
    uint32_t block = 0;
    for (unsigned k = 0; k < n; k++) {
        block = k ? block + 2 : seedBlock(d);
        if (next >= d.entries) {
            pg ^= 1;
            memset(entryPtr(d.page0 + pg, 0), 0xFF, EMU_PAGE_SIZE);
            next = 0;
        }
        putEntry(d.page0 + pg, next++, block);
    }
    return block;
}

/** Runs one device through boots and packets and checks that no counter value that could have
 *  been sent is ever used again. */
struct Sim {
    const ctr_dev& d;
    bool have = false;        // some counter value may have been sent
    uint32_t maxSent = 0;     // the highest one
    bool on = false;          // booted, not cut since
    uint32_t ctr = 0;         // counter of the next packet
    uint8_t pkts = 0;         // packets since this boot (the door sensor's boot_pkts)
    unsigned violations = 0;
    unsigned saturated = 0;   // of them: with a block above CTR_BLOCK_MAX (the log is stuck)
    std::string trace;        // what happened, for the report of the first violation
    std::string firstBad;

    explicit Sim(const ctr_dev& dev) : d(dev) {}

    void note(const char* fmt, ...) __attribute__((format(printf, 2, 3))) {
        char b[160];
        va_list ap;
        va_start(ap, fmt);
        vsnprintf(b, sizeof(b), fmt, ap);
        va_end(ap);
        trace += b;
    }

    /** Counter values up to @p c may already have been sent (e.g. before a pre-fill). */
    void sentUpTo(uint32_t c) {
        have = true;
        maxSent = c;
        note("[sent up to %08X] ", c);
    }

    void bad(const char* what, uint32_t c) {
        violations++;
        if ((c >> 8) > d.block_max)
            saturated++;
        if (firstBad.empty()) {
            char b[200];
            snprintf(b, sizeof(b), "\n      -> %s: %08X, but %08X may have been sent already", what,
                     c, maxSent);
            firstBad = trace + b;
        }
    }

    /** A reset and ctr_boot(). @return false if the power was cut during it. */
    bool boot() {
        uint32_t f;
        on = false;
        d.reset();
        if (!d.boot(&f)) {
            note("boot:CUT ");
            return false;
        }
        note("boot:%08X ", f);
        if (have && f <= maxSent)
            bad("a boot starts at", f);
        on = true;
        ctr = f;
        pkts = 0;
        return true;
    }

    /** The counter rule and the packet. @p sent: the radio sends it (false: e.g. no crystal;
     *  its counter value is used up all the same). @return false if the power was cut. */
    bool packet(bool sent = true) {
        if (!d.packet(ctr, pkts)) {
            on = false;
            note("pkt %08X:CUT ", ctr);
            return false;
        }
        if (sent) {
            if (have && ctr <= maxSent)
                bad("a packet uses", ctr);
            have = true;
            maxSent = ctr;
        }
        ctr++;
        if (pkts < 0xFF)
            pkts++;
        return true;
    }

    bool packets(unsigned n) {
        if (!n)
            return true;
        uint32_t from = ctr;
        unsigned i;
        for (i = 0; i < n; i++)
            if (!packet())
                break;
        if (i)
            note("pkts %08X..%08X ", from, from + i - 1);
        return i == n;
    }

    void report() {
        if (violations)
            printf("    %s: %u counter reuse(s); first one after:\n      %s\n", d.name, violations,
                   firstBad.c_str());
    }
};
