/**
 * @file test_ctr_log.cpp
 * @brief Directed tests of the persistent packet counter log (ctr_log.c + each ctr_boot) on the
 *        emulated CC1110 flash: seed, boot steps, block rollover, page switch, a power cut at
 *        every flash operation, garbage and worn entries, CTR_BLOCK_MAX, no crystal, and the
 *        packets the counter rule holds (blocks that could not be logged).
 */

#include <string.h>

#include <vector>

#include "sim.h"
#include "test.h"

namespace {

const ctr_dev* const kDevices[] = {&dev_door, &dev_door4, &dev_cam};

void blank(const ctr_dev& d) {
    emu_init();
    d.reset();
}

void checkClean(const ctr_dev& d) {
    if (emu_stats.errors)
        printf("    %s: emulator: %s\n", d.name, emu_last_error);
    CHECK_EQ(emu_stats.errors, 0);
    CHECK_EQ(emu_stats.rewrites, 0);
}

}  // namespace

TEST(fresh_log_starts_at_ctr_seed) {
    struct {
        const ctr_dev* d;
        uint32_t want;
    } cases[] = {
        {&dev_door, 1}, {&dev_door4, 1}, {&dev_cam, 1}, {&dev_door_seed, 0x123456},
        {&dev_cam_seed, 0x123456}, {&dev_door_seed0, 1}, {&dev_door_seedbig, 1},
    };
    for (auto& c : cases) {
        const ctr_dev& d = *c.d;
        uint32_t first = 0, v = 0;
        blank(d);
        REQUIRE(d.boot(&first));
        CHECK_EQ(first, c.want << 8);
        CHECK(d.decode((uint8_t)d.page0, 0, &v));
        CHECK_EQ(v, c.want);
        CHECK_EQ(d.page(), d.page0);
        CHECK_EQ(d.next(), 1);
        CHECK_EQ(d.limit(), c.want + 1);
        CHECK_EQ(emu_stats.erases, 0);
        CHECK_EQ(emu_stats.words, 2);   // one entry, two flash words
        checkClean(d);
    }
}

TEST(each_boot_adds_two_blocks) {
    for (const ctr_dev* dp : kDevices) {
        const ctr_dev& d = *dp;
        blank(d);
        for (unsigned k = 0; k < 2 * d.entries + 5; k++) {
            uint32_t first = 0, newest = 0;
            d.reset();
            REQUIRE(d.boot(&first));
            CHECK_EQ(first, (1 + 2 * k) << 8);
            CHECK(newestBlock(d, &newest));
            CHECK_EQ(newest, 1 + 2 * k);
            CHECK_EQ(d.limit(), 2 + 2 * k);
        }
        // a page switch (erase of the other page) every CTR_ENTRIES boots after the first page
        CHECK_EQ(emu_stats.erases, (2 * d.entries + 4) / d.entries);
        checkClean(d);
    }
}

TEST(rollover_logs_the_next_block_before_its_first_packet) {
    for (const ctr_dev* dp : kDevices) {
        const ctr_dev& d = *dp;
        Sim s(d);
        blank(d);
        REQUIRE(s.boot());
        const uint32_t b = s.ctr >> 8;
        for (unsigned blk = 0; blk < 3; blk++) {
            for (unsigned i = 0; i < 256; i++) {
                unsigned ops = emu_stats.ops;
                REQUIRE(s.packet());
                uint32_t sentBlock = (s.ctr - 1) >> 8, newest = 0;
                REQUIRE(newestBlock(d, &newest));
                // the block of every packet is in the log before the packet goes out
                CHECK_EQ(newest, sentBlock);
                // only the first packet of a new block writes (the boot's block is logged)
                CHECK_EQ(emu_stats.ops - ops, (i == 0 && blk > 0) ? 2u : 0u);
            }
        }
        CHECK_EQ(s.ctr >> 8, b + 3);
        CHECK_EQ(s.violations, 0);
        checkClean(d);
    }
}

TEST(full_page_erases_the_other_page_and_keeps_the_newest_entry) {
    for (const ctr_dev* dp : kDevices) {
        const ctr_dev& d = *dp;
        for (unsigned round = 0; round < 3; round++) {
            // both pages full: the newest entry is the last one of the page written last
            emu_init();
            uint32_t last = prefill(d, 2 * d.entries + round * d.entries);
            unsigned full = d.page0 + ((2 + round) % 2 == 0 ? 1 : 0);
            unsigned other = d.page0 + d.page0 + 1 - full;
            std::vector<uint8_t> fullPage(entryPtr(full, 0), entryPtr(full, 0) + EMU_PAGE_SIZE);
            uint32_t v = 0, first = 0;
            REQUIRE(d.decode((uint8_t)full, (uint16_t)(d.entries - 1), &v));
            REQUIRE(v == last);

            // a cut as the erase starts and a cut in the middle of it: the newest entry stays
            for (int kind = 0; kind < 2; kind++) {
                d.reset();
                emu_cut_at(emu_stats.ops, kind ? EMU_CUT_PARTIAL : EMU_CUT_BEFORE, 7 + round);
                CHECK(!d.boot(&first));
                CHECK(memcmp(fullPage.data(), entryPtr(full, 0), EMU_PAGE_SIZE) == 0);
                CHECK(newestBlock(d, &v) && v >= last);
            }
            emu_cut_clear();
            emu_init();   // the same state again (the cut erase left the other page in pieces)
            prefill(d, 2 * d.entries + round * d.entries);
            d.reset();
            unsigned erases = emu_stats.erases;
            REQUIRE(d.boot(&first));
            CHECK_EQ(emu_stats.erases, erases + 1);
            CHECK(memcmp(fullPage.data(), entryPtr(full, 0), EMU_PAGE_SIZE) == 0);
            CHECK(d.decode((uint8_t)other, 0, &v));
            CHECK_EQ(v, last + 2);
            CHECK_EQ(first, (last + 2) << 8);
            CHECK_EQ(d.page(), other);
            CHECK_EQ(d.next(), 1);
            for (unsigned i = 1; i < d.entries; i++)
                CHECK(d.erased((uint8_t)other, (uint16_t)i));
            checkClean(d);
        }
    }
}

/* A power cut at every flash operation (as it starts, or in the middle with 8 different partial
 * results) of a boot and of the packets after it, from log states around the page boundaries.
 * Then three clean boots, each with a rollover: no counter value may be used twice, and the
 * newest entry from before the cut is never lost. */
TEST(power_cut_at_every_flash_operation_never_reuses_a_counter) {
    static const unsigned kPkts[] = {0, 1, 257, 513};
    unsigned cases = 0, reuse = 0, lost = 0, notCut = 0, errors = 0, rewrites = 0, saturated = 0;
    for (const ctr_dev* dp : kDevices) {
        const ctr_dev& d = *dp;
        std::vector<unsigned> fills;
        if (d.entries <= 8) {
            for (unsigned n = 0; n <= 3 * d.entries + 1; n++)
                fills.push_back(n);
        } else {
            fills = {0, 1, 2, d.entries - 2, d.entries - 1, d.entries, d.entries + 1,
                     2 * d.entries - 1, 2 * d.entries, 2 * d.entries + 1};
        }
        for (unsigned n : fills) {
            for (unsigned pk : kPkts) {
                // a run without a cut counts the flash operations of this boot + packets
                emu_init();
                uint32_t last = prefill(d, n);
                std::vector<uint8_t> snap(emu_state_size());
                emu_save(snap.data());
                {
                    Sim s(d);
                    REQUIRE(s.boot());
                    REQUIRE(s.packets(pk));
                }
                unsigned nops = emu_stats.ops;
                for (unsigned k = 0; k < nops; k++) {
                    for (unsigned variant = 0; variant < 9; variant++) {
                        emu_restore(snap.data());
                        Sim s(d);
                        s.note("[%s, %u boots before, cut at flash op %u (%s)] ", d.name, n, k,
                               variant ? "partial" : "as it starts");
                        if (n)
                            s.sentUpTo((last << 8) | 0xFF);
                        emu_cut_at(k, variant ? EMU_CUT_PARTIAL : EMU_CUT_BEFORE,
                                   0x1000u * k + variant);
                        if (s.boot())
                            s.packets(pk);
                        notCut += emu_cut_armed();
                        emu_cut_clear();
                        uint32_t newest = 0;
                        if (n && !(newestBlock(d, &newest) && newest >= last)) {
                            if (!lost++)
                                printf("    newest entry lost: %s\n", s.trace.c_str());
                        }
                        for (int r = 0; r < 3; r++)
                            if (s.boot())
                                s.packets(257);
                        if (emu_stats.errors && !errors++)
                            printf("    emulator: %s\n", emu_last_error);
                        rewrites += emu_stats.rewrites;
                        saturated += s.saturated > 0;
                        if (s.violations && !reuse++)
                            s.report();
                        cases++;
                    }
                }
            }
        }
    }
    printf("    %u cut cases\n", cases);
    CHECK(cases > 1000);
    CHECK_EQ(reuse, 0);
    CHECK_EQ(saturated, 0);
    CHECK_EQ(lost, 0);
    CHECK_EQ(notCut, 0);
    CHECK_EQ(errors, 0);
    CHECK_EQ(rewrites, 0);
}

TEST(garbage_entries_are_skipped) {
    for (const ctr_dev* dp : kDevices) {
        const ctr_dev& d = *dp;
        const unsigned p0 = d.page0;
        uint32_t first = 0, v = 0;
        uint8_t e[4];

        // valid 5, wrong check byte, write cut after the low word, b2 = 0xFF with a matching
        // check (never written: blocks stop at CTR_BLOCK_MAX), valid 7
        blank(d);
        putEntry(p0, 0, 5);
        entryBytes(0x40, e);
        putRaw(p0, 1, e[0], (uint8_t)(e[1] ^ 0x10), e[2], e[3]);
        entryBytes(0x123456, e);
        putRaw(p0, 2, e[0], e[1], 0xFF, 0xFF);
        putRaw(p0, 3, 0x01, entryCheck(0x01, 0x02, 0xFF), 0x02, 0xFF);
        CHECK(!d.decode((uint8_t)p0, 1, &v));
        CHECK(!d.decode((uint8_t)p0, 2, &v));
        CHECK(!d.decode((uint8_t)p0, 3, &v));
        unsigned nextSlot = 4;
        if (d.entries > 4) {
            putEntry(p0, 4, 7);
            nextSlot = 5;
        }
        REQUIRE(d.boot(&first));
        uint32_t want = d.entries > 4 ? 9 : 7;
        CHECK_EQ(first, want << 8);
        if (d.entries > nextSlot) {
            CHECK(d.decode((uint8_t)p0, (uint16_t)nextSlot, &v));
            CHECK_EQ(v, want);
            CHECK_EQ(d.next(), nextSlot + 1);
        } else {   // the page is full of entries: the other page was erased and used
            CHECK(d.decode((uint8_t)(p0 + 1), 0, &v));
            CHECK_EQ(v, want);
        }
        checkClean(d);

        // garbage after the newest entry: the next entry goes after the garbage
        blank(d);
        putEntry(p0, 0, 5);
        putRaw(p0, 1, 0x12, 0x34, 0x56, 0x78);
        REQUIRE(!d.decode((uint8_t)p0, 1, &v));
        REQUIRE(d.boot(&first));
        CHECK_EQ(first, 7u << 8);
        CHECK(d.decode((uint8_t)p0, 2, &v) && v == 7);
        checkClean(d);

        // a page of nothing but garbage and a blank page: CTR_SEED, in the blank page
        blank(d);
        for (unsigned i = 0; i < d.entries; i++)
            putRaw(p0, i, (uint8_t)i, 0x00, (uint8_t)(i * 7), 0xFF);
        REQUIRE(d.boot(&first));
        CHECK_EQ(first, seedBlock(d) << 8);
        CHECK(d.decode((uint8_t)(p0 + 1), 0, &v) && v == seedBlock(d));
        d.reset();
        REQUIRE(d.boot(&first));
        CHECK_EQ(first, (seedBlock(d) + 2) << 8);
        checkClean(d);
    }
}

TEST(worn_slot_is_skipped_and_the_rule_retries_it) {
    for (const ctr_dev* dp : {&dev_door, &dev_cam}) {
        const ctr_dev& d = *dp;
        const unsigned p0 = d.page0;
        uint32_t first = 0, v = 0;
        bool ok, send;

        blank(d);
        REQUIRE(d.boot(&first));   // block 1 in slot 0
        // slot 1: bit 0 of b2 no longer programs, so block 3 reads back with b2 = 0x01
        emu_stuck1[p0 * EMU_PAGE_SIZE + 1 * 4 + 3] = 0x01;
        d.reset();
        REQUIRE(d.boot(&first));
        CHECK_EQ(first, 3u << 8);
        CHECK(!d.decode((uint8_t)p0, 1, &v));
        CHECK(d.decode((uint8_t)p0, 2, &v) && v == 3);
        CHECK_EQ(d.next(), 3);
        CHECK_EQ(d.limit(), 4);

        // three worn slots: the boot's entry fails, ctr_limit stays at the newest block + 1
        for (unsigned i = 3; i < 6; i++)
            emu_stuck1[p0 * EMU_PAGE_SIZE + i * 4 + 3] = 0x01;
        d.reset();
        REQUIRE(d.boot(&first));
        CHECK_EQ(first, 5u << 8);
        CHECK_EQ(d.limit(), 4);
        CHECK_EQ(d.next(), 6);
        // the first packet retries it in the next slot, and is sent once it is logged
        REQUIRE(d.packet(first, &send));
        CHECK(send);
        CHECK_EQ(d.limit(), 6);
        CHECK(d.decode((uint8_t)p0, 6, &v) && v == 5);

        // a worn slot at a rollover: the block after the newest one is sent all the same (the
        // next boot skips it), and the next packets of that block retry it
        for (unsigned i = 7; i < 10; i++)
            emu_stuck1[p0 * EMU_PAGE_SIZE + i * 4 + 3] = 0x01;
        REQUIRE(d.packet(6u << 8, &send));
        CHECK(send);
        CHECK_EQ(d.limit(), 6);
        REQUIRE(d.packet((6u << 8) + 1, &send));
        CHECK(send);
        CHECK_EQ(d.limit(), 7);
        CHECK(d.decode((uint8_t)p0, 10, &v) && v == 6);

        // ctr_log() alone: 3 tries, then false
        for (unsigned i = 11; i < 14; i++)
            emu_stuck1[p0 * EMU_PAGE_SIZE + i * 4 + 3] = 0x01;
        REQUIRE(d.log(8, &ok));
        CHECK(!ok);
        CHECK_EQ(d.next(), 14);
        CHECK_EQ(d.limit(), 7);
        checkClean(d);
    }
}

/* The counter never wraps: nothing above CTR_BLOCK_MAX is logged (b2 never reaches 0xFF), the
 * block after the newest entry (CTR_BLOCK_MAX + 1) is the last one sent, and every packet after
 * it is held, also after a reset (a device that gets there needs a reflash). */
TEST(ctr_block_max_stops_the_counter) {
    for (const ctr_dev* dp : {&dev_door, &dev_cam}) {
        const ctr_dev& d = *dp;
        const unsigned p0 = d.page0;
        const uint32_t max = d.block_max;
        uint32_t first = 0, v = 0;
        bool ok, send;
        CHECK_EQ(max, 0xFEFFFF);
        blank(d);
        putEntry(p0, 0, max - 2);
        REQUIRE(d.boot(&first));
        CHECK_EQ(first, max << 8);
        CHECK(d.decode((uint8_t)p0, 1, &v));
        CHECK_EQ(v, max);
        CHECK_EQ(entryPtr(p0, 1)[3], 0xFE);
        CHECK_EQ(d.limit(), max + 1);
        unsigned ops = emu_stats.ops;
        REQUIRE(d.packet(first, &send));
        CHECK(send);
        REQUIRE(d.packet((max + 1) << 8, &send));   // 0xFF000000: not logged, but sent
        CHECK(send);
        REQUIRE(d.packet((max + 2) << 8, &send));   // 0xFF000100: held
        CHECK(!send);
        REQUIRE(d.log(max + 1, &ok));
        CHECK(!ok);
        REQUIRE(d.log(0xFFFFFF, &ok));
        CHECK(!ok);
        CHECK_EQ(emu_stats.ops, ops);                // nothing written above the maximum
        d.reset();
        REQUIRE(d.boot(&first));
        CHECK_EQ(first, (max + 2) << 8);
        CHECK_EQ(emu_stats.ops, ops);
        REQUIRE(d.packet(first, &send));
        CHECK(!send);
        checkClean(d);
    }
}

/* Was a known problem until 2026-11-19: ctr_boot() returned (best + 2) << 8 even above
 * CTR_BLOCK_MAX while ctr_log() logged the clamped CTR_BLOCK_MAX, so once the log held
 * CTR_BLOCK_MAX every boot started at the same counter 0xFF000100. Now those packets are held. */
TEST(no_counter_reuse_once_the_log_is_saturated) {
    for (const ctr_dev* dp : {&dev_door, &dev_cam}) {
        const ctr_dev& d = *dp;
        blank(d);
        putEntry(d.page0, 0, d.block_max - 1);
        Sim s(d);
        s.sentUpTo(((d.block_max - 1) << 8) | 0xFF);
        for (int r = 0; r < 3; r++)
            if (s.boot())
                s.packets(2);
        s.report();
        CHECK_EQ(s.violations, 0);
        CHECK_EQ(s.held, 6);
    }
}

/* A write cut in a flash word leaves bits at 1 that should be 0 (and a cut erase bits at 1 that
 * should stay 0). The check byte counts the 0 bits of the block, so the stored check can only go
 * up and the count of the block bytes only down: no cut word can leave a valid entry. Every
 * partial result of both words of one entry, for blocks with many and with few 0 bits. */
TEST(cut_write_never_leaves_a_valid_entry) {
    const ctr_dev& d = dev_door;
    static const uint32_t kBlocks[] = {0x0001FD, 0x000101, 0x7F7F7F, 0xFEFFFD, 0x123456};
    unsigned cases = 0, valid = 0;
    for (uint32_t prev : kBlocks) {
        uint8_t e[4];
        entryBytes(prev + 2, e);   // what the boot writes
        for (unsigned op = 0; op < 2; op++) {
            uint8_t wlo = op ? e[2] : e[0], whi = op ? e[3] : e[1];
            for (unsigned lo = 0; lo < 256; lo++) {
                for (unsigned hi = 0; hi < 256; hi++) {
                    uint32_t first = 0, v = 0;
                    // only masks that leave a bit at 1 that should be 0 are a cut
                    if (!((lo & ~wlo) | (hi & ~whi)))
                        continue;
                    blank(d);
                    putEntry(d.page0, 0, prev);
                    emu_cut_word_at(op, (uint8_t)lo, (uint8_t)hi);
                    CHECK(!d.boot(&first));
                    emu_cut_clear();
                    cases++;
                    if (d.decode((uint8_t)d.page0, 1, &v) && !valid++)
                        printf("    valid after a cut of word %u of %06X (%02X %02X): %06X\n", op,
                               prev + 2, lo, hi, v);
                }
            }
        }
    }
    printf("    %u cut words\n", cases);
    CHECK(cases > 100000);
    CHECK_EQ(valid, 0);
}

/* Was a known problem until 2026-11-19, with the XOR check byte: block 0x0001FF cut in its
 * second word with bits 7..1 of b1 and b2 still 1 read FF 5B FF FE, a valid CTR_BLOCK_MAX, which
 * saturated the log. With the 0-bit count it is skipped, and the next boot logs the block again. */
TEST(cut_second_word_does_not_saturate_the_log) {
    for (const ctr_dev* dp : {&dev_door, &dev_cam}) {
        const ctr_dev& d = *dp;
        uint32_t v = 0;
        blank(d);
        putEntry(d.page0, 0, 0x1FD);
        Sim s(d);
        s.sentUpTo((0x1FDu << 8) | 0xFF);
        // the boot logs 0x1FF; its second word (b1 = 0x01, b2 = 0x00) is cut with bits 7..1
        // of both bytes still 1
        emu_cut_word_at(1, 0xFE, 0xFE);
        CHECK(!s.boot());
        emu_cut_clear();
        CHECK(!d.decode((uint8_t)d.page0, 1, &v));
        for (int r = 0; r < 3; r++)
            if (s.boot())
                s.packets(2);
        CHECK(newestBlock(d, &v));
        CHECK_EQ(v, 0x1FFu + 4);   // 0x1FF, 0x201, 0x203
        s.report();
        CHECK_EQ(s.violations, 0);
        CHECK_EQ(s.held, 0);
    }
}

TEST(no_flash_operation_without_the_crystal) {
    for (const ctr_dev* dp : kDevices) {
        const ctr_dev& d = *dp;
        uint32_t first = 0, v = 0;
        bool ok = true;
        const uint8_t data[4] = {1, 2, 3, 4};
        bool send;
        blank(d);
        d.set_log_state(d.page0, 0, 0, true);
        CLKCON |= 0x40;   // OSC = 1: HS RCOSC (clock_xosc() timed out)
        CHECK(!d.clock_ok());
        REQUIRE(d.erase_page((uint8_t)d.page0));
        REQUIRE(d.write4((uint16_t)(d.page0 * EMU_PAGE_SIZE), data));
        REQUIRE(d.log(9, &ok));
        CHECK(!ok);
        CHECK_EQ(emu_stats.ops, 0);
        d.reset();
        CLKCON |= 0x40;
        REQUIRE(d.boot(&first));
        CHECK_EQ(emu_stats.ops, 0);
        CHECK_EQ(first, seedBlock(d) << 8);
        CHECK_EQ(d.limit(), seedBlock(d) - 1);
        // the boot's block is not logged: its packets are held
        REQUIRE(d.packet(first, &send));
        CHECK(!send);
        CHECK_EQ(emu_stats.ops, 0);
        // the crystal back: the next packet logs the block and is sent
        CLKCON = 0x80;
        CHECK(d.clock_ok());
        REQUIRE(d.packet(first, &send));
        CHECK(send);
        CHECK_EQ(d.limit(), seedBlock(d) + 1);
        CHECK(newestBlock(d, &v) && v == seedBlock(d));
        checkClean(d);
    }
}

/* Was a known problem until 2026-11-19: without the crystal (clock_xosc() timed out) at the
 * end of a page, ctr_log() switched to the other page although flash_erase_page() did nothing,
 * wrote over its old entries and, once it was "full", erased the page with the newest entry; a
 * power cut there lost it. Now ctr_log() does nothing without the crystal (the block after the
 * newest is sent unlogged as usual), and the next packet with the crystal erases and switches. */
TEST(crystal_failure_at_a_page_end_never_loses_the_newest_entry) {
    const ctr_dev& d = dev_door;
    for (int kind = 0; kind < 2; kind++) {
        // page 30 full (blocks 1..511), page 31 with 255 entries (513..1021)
        emu_init();
        uint32_t last = prefill(d, 2 * d.entries - 1);
        Sim s(d);
        s.sentUpTo((last << 8) | 0xFF);
        REQUIRE(s.boot());               // block 1023 in the last slot of page 31
        REQUIRE(s.packets(256));         // all of block 1023
        CLKCON |= 0x40;                  // the crystal did not start for this packet
        REQUIRE(s.packet(false));        // rollover: block 1024 cannot be logged, page 30 not erased
        CLKCON = 0x80;
        CHECK_EQ(d.limit(), 1024);
        CHECK_EQ(d.page(), d.page0 + 1);  // no page switch
        CHECK_EQ(d.next(), d.entries);
        // the next packets of block 1024 retry on the unerased page 30 until it is "full";
        // find the erase of page 31 (with the newest entry) that follows
        std::vector<uint8_t> snap(emu_state_size());
        emu_save(snap.data());
        unsigned page = d.page(), next = d.next();
        uint32_t lim = d.limit();
        bool safe = d.safe();
        Sim probe = s;
        unsigned erases = emu_stats.erases, k = 0;
        while (emu_stats.erases == erases && k++ < 256)
            REQUIRE(probe.packet());
        REQUIRE(emu_stats.erases == erases + 1);
        CHECK_EQ(k, 1);                  // the first packet with the crystal
        unsigned eraseOp = emu_stats.last_erase_op;
        CHECK_EQ(emu_stats.rewrites, 0);
        // again from the snapshot, with a power cut in that erase or right after it
        emu_restore(snap.data());
        d.set_log_state(page, next, lim, safe);
        emu_cut_at(kind ? eraseOp + 1 : eraseOp, kind ? EMU_CUT_BEFORE : EMU_CUT_PARTIAL, 5);
        for (k = 0; k < 256 && s.packet(); k++) {
        }
        CHECK(!emu_cut_armed());
        emu_cut_clear();
        s.boot();
        s.packets(1);
        s.report();
        CHECK_EQ(s.violations, 0);
        CHECK_EQ(emu_stats.rewrites, 0);
    }
}

/* Was a known problem until 2026-11-19: the boot's own block (newest + 2) was used even when it
 * could not be logged, so if the retry before the first packet also failed (6 worn slots in a
 * row, or a weak supply) and the device reset, the next boot started at the same counter. Now
 * such a packet is held (ctr_use) until its block is logged. */
TEST(unlogged_boot_block_is_not_sent) {
    for (const ctr_dev* dp : {&dev_door, &dev_cam}) {
        const ctr_dev& d = *dp;
        blank(d);
        Sim s(d);
        REQUIRE(s.boot());                       // block 1 in slot 0
        REQUIRE(s.packets(3));
        for (unsigned i = 1; i <= 6; i++)        // slots 1..6: bit 0 of b2 no longer programs
            emu_stuck1[d.page0 * EMU_PAGE_SIZE + i * 4 + 3] = 0x01;
        REQUIRE(s.boot());                       // block 3: slots 1..3 fail
        CHECK_EQ(d.limit(), 2);
        REQUIRE(s.packet());                     // retry in slots 4..6 fails: 0x300 is held
        CHECK_EQ(d.limit(), 2);
        CHECK_EQ(s.held, 1);
        REQUIRE(s.boot());                       // newest is still block 1: block 3 again
        CHECK_EQ(d.limit(), 4);                  // slot 7 works
        s.packets(1);
        s.report();
        CHECK_EQ(s.violations, 0);
        CHECK_EQ(s.held, 1);
    }
}

/* Erases that fail without a power cut (weak supply) at a page switch: the page without a
 * valid entry is erased again as soon as its next slot is not blank, so the device sends again
 * after at most one held packet per 3 failed erases (3 tries per ctr_log()), and the page with
 * the newest entry is never touched (a failed erase whose next slot is blank needs no retry). */
TEST(failed_erase_at_a_page_switch_is_retried) {
    for (const ctr_dev* dp : {&dev_door, &dev_door4, &dev_cam}) {
        const ctr_dev& d = *dp;
        for (unsigned k = 1; k <= 6; k++) {
            for (uint32_t seed = 1; seed <= 20; seed++) {
                emu_init();
                uint32_t last = prefill(d, 2 * d.entries);   // page 1 full, newest at its end
                emu_seed(seed * 977 + k);
                emu_erase_fail = k;
                Sim s(d);
                s.sentUpTo((last << 8) | 0xFF);
                s.note("[%s, %u failed erases, seed %u] ", d.name, k, seed);
                REQUIRE(s.boot());
                REQUIRE(s.packets(300));
                uint32_t v = 0;
                CHECK(newestBlock(d, &v) && v > last);
                // an erase that failed but left the next slot blank needs no second one
                CHECK(emu_stats.failed_erases >= 1 && emu_stats.failed_erases <= k);
                CHECK(s.held <= (emu_stats.failed_erases + 2) / 3);
                CHECK_EQ(emu_stats.rewrites, 0);
                s.report();
                CHECK_EQ(s.violations, 0);
            }
        }
    }
}

/* A page whose cells no longer erase (stuck at 0): nothing can be logged there, so the device
 * holds its packets (the newest entry is in the other page, which is never erased), and the
 * early erases are capped (CTR_RETRY_MAX per boot, then one per full walk of ~85 calls). */
TEST(dead_page_holds_packets_with_bounded_wear) {
    for (const ctr_dev* dp : {&dev_door, &dev_cam}) {
        const ctr_dev& d = *dp;
        emu_init();
        uint32_t last = prefill(d, d.entries);   // page 0 full, newest at its end
        memset(&emu_stuck0[(d.page0 + 1) * EMU_PAGE_SIZE], 0xFF, EMU_PAGE_SIZE);
        std::vector<uint8_t> page0(entryPtr(d.page0, 0), entryPtr(d.page0, 0) + EMU_PAGE_SIZE);
        Sim s(d);
        s.sentUpTo((last << 8) | 0xFF);
        const unsigned kAttempts = 1000;
        REQUIRE(s.boot());
        REQUIRE(s.packets(kAttempts));
        CHECK_EQ(s.held, kAttempts);
        CHECK(memcmp(page0.data(), entryPtr(d.page0, 0), EMU_PAGE_SIZE) == 0);
        unsigned erases = emu_stats.erases + emu_stats.failed_erases;
        printf("    %s: %u erases of the dead page in %u held packets\n", d.name, erases, kAttempts);
        CHECK(erases <= 1 + 8 + (kAttempts + 1) / 85 + 1);
        // the page works again (e.g. after the supply recovered): the next packet is sent
        memset(&emu_stuck0[(d.page0 + 1) * EMU_PAGE_SIZE], 0, EMU_PAGE_SIZE);
        unsigned held = s.held;
        REQUIRE(s.packets(90));
        CHECK(s.held - held <= d.entries / 3 + 2);
        CHECK(s.held - held < 90);
        s.report();
        CHECK_EQ(s.violations, 0);
        CHECK_EQ(emu_stats.rewrites, 0);
    }
}

/* If an erase pre-programmed cells to 0 before erasing (the CC1110 documents no such step), a
 * cut one could leave a 0-heavy pattern that passes the 0-bit count, e.g. FF 01 FF FE = block
 * 0xFEFFFF. Such an entry can only be in the page without the newest entry and can only raise
 * the counter: the device then holds every packet (CTR_BLOCK_MAX), it never reuses one. */
TEST(zero_heavy_cut_erase_can_only_raise_the_counter) {
    for (const ctr_dev* dp : {&dev_door, &dev_cam}) {
        const ctr_dev& d = *dp;
        uint32_t v = 0;
        blank(d);
        putEntry(d.page0, 0, 0x100);
        putRaw(d.page0 + 1, 7, 0xFF, 0x01, 0xFF, 0xFE);
        CHECK(d.decode((uint8_t)(d.page0 + 1), 7, &v) && v == d.block_max);   // documented
        Sim s(d);
        s.sentUpTo((0x100u << 8) | 0xFF);
        for (int r = 0; r < 2; r++)
            if (s.boot())
                s.packets(3);
        s.report();
        CHECK_EQ(s.violations, 0);
        CHECK_EQ(s.held, 6);
    }
}

/* Camera: a picture starts with a 24-byte packet (0xB2) whose counter tags all its chunks. If
 * that packet is held (its block cannot be logged), no picture may be taken or sent: its chunks
 * would carry a counter that was not sent and that the next packet uses again. */
TEST(cam_held_start_packet_sends_no_picture) {
    const ctr_dev& d = dev_cam;
    REQUIRE(d.picture && d.pic_ctr_ok);
    uint32_t first = 0;
    bool ok = true;
    struct pic_trace t;
    blank(d);
    REQUIRE(d.boot(&first));                     // block 1 in slot 0
    for (unsigned i = 1; i <= 9; i++)            // slots 1..9: bit 0 of b2 no longer programs
        emu_stuck1[d.page0 * EMU_PAGE_SIZE + i * 4 + 3] = 0x01;
    d.reset();
    REQUIRE(d.boot(&first));                     // block 3 cannot be logged (slots 1..3)
    CHECK_EQ(d.limit(), 2);
    uint32_t ctr = first;
    REQUIRE(d.pic_ctr_ok(&ctr, &ok));            // slots 4..6: still not logged
    CHECK(!ok);
    REQUIRE(d.picture(0xD3, &ctr, &t, &ok));     // slots 7..9
    CHECK(!ok);
    CHECK_EQ(t.npackets, 0);
    CHECK_EQ(t.nchunks, 0);
    CHECK_EQ(t.bf_cmds, 0);
    CHECK_EQ(ctr, first);
    // the next slot works: the picture goes out, tagged with its start packet's counter
    REQUIRE(d.picture(0xD3, &ctr, &t, &ok));
    CHECK(ok);
    CHECK_EQ(t.npackets, 1);
    CHECK_EQ(t.packet_ctr[0], first);
    CHECK_EQ(t.bf_cmds, 1);
    CHECK_EQ(t.nchunks, 3);                      // 100 bytes, 40 per chunk
    for (unsigned i = 0; i < t.nchunks && i < PIC_TRACE_MAX; i++)
        CHECK_EQ(t.chunk_tag[i], first);
    checkClean(d);
}

TEST(cam_each_picture_has_its_own_tag) {
    const ctr_dev& d = dev_cam;
    REQUIRE(d.picture && d.pic_ctr_ok);
    uint32_t first = 0, prevTag = 0;
    bool ok;
    struct pic_trace t;
    blank(d);
    REQUIRE(d.boot(&first));
    uint32_t ctr = first + 250;                  // pictures across a block rollover
    for (unsigned p = 0; p < 12; p++) {
        REQUIRE(d.picture(0xD3, &ctr, &t, &ok));
        CHECK(ok);
        REQUIRE(t.npackets == 1);
        CHECK_EQ(t.packet_marker[0], 0xB2);
        uint32_t tag = t.packet_ctr[0];
        if (p)
            CHECK(tag > prevTag);
        CHECK_EQ(t.nchunks, 3);
        for (unsigned i = 0; i < t.nchunks && i < PIC_TRACE_MAX; i++)
            CHECK_EQ(t.chunk_tag[i], tag);
        prevTag = tag;
    }
    uint32_t v = 0;
    CHECK(newestBlock(d, &v) && v == (first >> 8) + 1);   // the rollover block was logged first
    checkClean(d);
}
