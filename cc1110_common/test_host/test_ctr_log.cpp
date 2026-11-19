/**
 * @file test_ctr_log.cpp
 * @brief Directed tests of the persistent packet counter log (ctr_log.c + each ctr_boot) on the
 *        emulated CC1110 flash: seed, boot steps, block rollover, page switch, a power cut at
 *        every flash operation, garbage and worn entries, CTR_BLOCK_MAX, no crystal.
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
        CHECK_EQ(d.pending(), 0);
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
            CHECK_EQ(d.pending(), 0);
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
                        // reuse only after a cut write saturated the log: the XFAIL tests below
                        saturated += s.saturated > 0;
                        if (s.violations > s.saturated && !reuse++)
                            s.report();
                        cases++;
                    }
                }
            }
        }
    }
    printf("    %u cut cases (in %u of them counters were reused only after a cut write "
           "saturated the log)\n", cases, saturated);
    CHECK(cases > 1000);
    CHECK_EQ(reuse, 0);
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
        putRaw(p0, 3, 0x01, (uint8_t)(0x01 ^ 0x02 ^ 0xFF ^ 0xA5), 0x02, 0xFF);
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

TEST(worn_slot_is_skipped_and_ctr_pending_retries_it) {
    for (const ctr_dev* dp : {&dev_door, &dev_cam}) {
        const ctr_dev& d = *dp;
        const unsigned p0 = d.page0;
        uint32_t first = 0, v = 0;
        bool ok;

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
        CHECK_EQ(d.pending(), 0);

        // three worn slots: the boot's entry fails, ctr_pending holds its block
        for (unsigned i = 3; i < 6; i++)
            emu_stuck1[p0 * EMU_PAGE_SIZE + i * 4 + 3] = 0x01;
        d.reset();
        REQUIRE(d.boot(&first));
        CHECK_EQ(first, 5u << 8);
        CHECK_EQ(d.pending(), 5);
        CHECK_EQ(d.next(), 6);
        // the first packet retries it in the next slot
        REQUIRE(d.packet(first, 0));
        CHECK_EQ(d.pending(), 0);
        CHECK(d.decode((uint8_t)p0, 6, &v) && v == 5);

        // a worn slot at a rollover: the next packets of that block retry it
        for (unsigned i = 7; i < 10; i++)
            emu_stuck1[p0 * EMU_PAGE_SIZE + i * 4 + 3] = 0x01;
        REQUIRE(d.packet(6u << 8, 1));
        CHECK_EQ(d.pending(), 6);
        REQUIRE(d.packet((6u << 8) + 1, 2));
        CHECK_EQ(d.pending(), 0);
        CHECK(d.decode((uint8_t)p0, 10, &v) && v == 6);

        // ctr_log() alone: 3 tries, then false
        for (unsigned i = 11; i < 14; i++)
            emu_stuck1[p0 * EMU_PAGE_SIZE + i * 4 + 3] = 0x01;
        REQUIRE(d.log(8, &ok));
        CHECK(!ok);
        CHECK_EQ(d.next(), 14);
        checkClean(d);
    }
}

TEST(ctr_block_max_clamps_the_log) {
    for (const ctr_dev* dp : {&dev_door, &dev_cam}) {
        const ctr_dev& d = *dp;
        const unsigned p0 = d.page0;
        uint32_t first = 0, v = 0;
        bool ok;
        CHECK_EQ(d.block_max, 0xFEFFFF);
        blank(d);
        putEntry(p0, 0, d.block_max - 1);
        REQUIRE(d.boot(&first));
        CHECK(d.decode((uint8_t)p0, 1, &v));
        CHECK_EQ(v, d.block_max);              // MAX - 1 + 2, clamped
        CHECK_EQ(entryPtr(p0, 1)[3], 0xFE);    // b2 never reaches 0xFF
        CHECK_EQ(d.pending(), 0);
        REQUIRE(d.log(d.block_max + 5, &ok));
        CHECK(ok);
        CHECK(d.decode((uint8_t)p0, 2, &v) && v == d.block_max);
        REQUIRE(d.log(0xFFFFFF, &ok));
        CHECK(ok);
        CHECK(d.decode((uint8_t)p0, 3, &v) && v == d.block_max);
        checkClean(d);
    }
}

/* KNOWN PROBLEM (needs a firmware decision and a bench test): ctr_boot() returns
 * (best + 2) << 8 even when best + 2 > CTR_BLOCK_MAX, while ctr_log() logs the clamped
 * CTR_BLOCK_MAX. Once the log holds CTR_BLOCK_MAX every boot starts at the same counter
 * 0xFF000100, so counter values are reused at every reset. A normal device never gets there
 * (2^32 packets), but a cut write or erase can leave an entry with a much larger block
 * (cut_write_can_only_raise_a_block), which brings it closer. */
XFAIL_TEST(counter_reused_once_the_log_is_saturated,
           "boots after CTR_BLOCK_MAX reuse 0xFF000100 (ctr_boot does not stop at the maximum)") {
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
    }
}

/* A write cut in the second flash word (b1, b2) leaves bits of b1/b2 at 1. If the check byte
 * still matches, the entry is valid with a LARGER block (bits only stay 1), never a smaller one,
 * so this cannot reuse a counter; it only uses up counter space (see the XFAIL above). */
TEST(cut_write_can_only_raise_a_block) {
    for (const ctr_dev* dp : kDevices) {
        const ctr_dev& d = *dp;
        unsigned valid = 0, raised = 0;
        uint32_t maxSeen = 0;
        for (uint32_t seed = 1; seed <= 3000; seed++) {
            uint32_t first = 0, v = 0;
            blank(d);
            putEntry(d.page0, 0, 0x0101);   // the boot logs 0x0103: b0 0x03, b1 0x01, b2 0x00
            emu_cut_at(1, EMU_CUT_PARTIAL, seed);
            CHECK(!d.boot(&first));
            if (d.decode((uint8_t)d.page0, 1, &v)) {
                valid++;
                CHECK(v >= 0x0103);
                if (v > 0x0103) {
                    raised++;
                    if (v > maxSeen)
                        maxSeen = v;
                }
            }
            emu_cut_clear();
        }
        printf("    %s: 3000 cut second words: %u valid, %u with a raised block (max %06X)\n",
               d.name, valid, raised, maxSeen);
    }
}

/* KNOWN PROBLEM (needs a firmware decision and a bench test): the check byte is an XOR of b0,
 * b1 and b2, so a write cut in its second word (b1, b2) that leaves the SAME bits still 1 in b1
 * and b2 passes the check. Block 0x0001FF (bytes FF 5B 01 00) cut when only bit 0 of b2 is
 * programmed reads FF 5B FF FE = block 0xFEFFFF = CTR_BLOCK_MAX: one power cut saturates the
 * log, and from then on every boot starts at 0xFF000100 (counter_reused_once_the_log_is_
 * saturated). The random power cut test finds the same thing with random partial writes. */
XFAIL_TEST(cut_second_word_can_saturate_the_log_and_reuse_counters,
           "a cut write can read as a valid CTR_BLOCK_MAX entry (XOR check), then boots reuse") {
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
        CHECK(d.decode((uint8_t)d.page0, 1, &v));
        CHECK_EQ(v, d.block_max);
        for (int r = 0; r < 3; r++)
            if (s.boot())
                s.packets(2);
        s.report();
        CHECK_EQ(s.violations, 0);
    }
}

TEST(no_flash_operation_without_the_crystal) {
    for (const ctr_dev* dp : kDevices) {
        const ctr_dev& d = *dp;
        uint32_t first = 0, v = 0;
        bool ok = true;
        const uint8_t data[4] = {1, 2, 3, 4};
        blank(d);
        d.set_log_state(d.page0, 0, 0);
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
        CHECK_EQ(d.pending(), seedBlock(d));
        // the crystal back: the first packet logs the pending block
        CLKCON = 0x80;
        CHECK(d.clock_ok());
        REQUIRE(d.packet(first, 0));
        CHECK_EQ(d.pending(), 0);
        CHECK(newestBlock(d, &v) && v == seedBlock(d));
        checkClean(d);
    }
}

/* KNOWN PROBLEM (needs a bench test): if the crystal is not running (clock_xosc() timed out)
 * when ctr_log() reaches the end of a page, ctr_log() switches to the other page although
 * flash_erase_page() did nothing. With the crystal back, the next tries write over the old
 * entries of that unerased page (words programmed twice, garbage, nothing reads back), and when
 * that page is "full" ctr_log() erases the page that holds the newest entry. A power cut in
 * that erase, or before the next entry is written, loses the newest entry: the next boot starts
 * hundreds of blocks lower and reuses counter values. */
XFAIL_TEST(crystal_failure_at_a_page_end_can_lose_the_newest_entry,
           "a skipped erase (no crystal) is not noticed; the page with the newest entry is erased later") {
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
        CHECK_EQ(d.pending(), 1024);
        // the next packets of block 1024 retry on the unerased page 30 until it is "full";
        // find the erase of page 31 (with the newest entry) that follows
        std::vector<uint8_t> snap(emu_state_size());
        emu_save(snap.data());
        unsigned page = d.page(), next = d.next();
        uint32_t pend = d.pending();
        Sim probe = s;
        unsigned erases = emu_stats.erases, k = 0;
        while (emu_stats.erases == erases && k++ < 256)
            REQUIRE(probe.packet());
        REQUIRE(emu_stats.erases == erases + 1);
        unsigned eraseOp = emu_stats.last_erase_op;
        printf("    rewrites of unerased words before the erase: %u\n", emu_stats.rewrites);
        // again from the snapshot, with a power cut in that erase or right after it
        emu_restore(snap.data());
        d.set_log_state(page, next, pend);
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

/* KNOWN PROBLEM (needs a bench test): the boot's own block is used even when it could not be
 * logged (ctr_pending). The skipped block (best + 2) only covers an unlogged block after the
 * logged one, not the boot's block itself: if the boot's entry and the retry before the first
 * packet both fail (6 worn slots in a row, or a weak supply), and the device resets, the next
 * boot finds the same newest entry and starts at the same counter again. */
XFAIL_TEST(unlogged_boot_block_is_reused_after_a_reset,
           "the boot's block is used without being logged; the next boot picks the same block") {
    for (const ctr_dev* dp : {&dev_door, &dev_cam}) {
        const ctr_dev& d = *dp;
        blank(d);
        Sim s(d);
        REQUIRE(s.boot());                       // block 1 in slot 0
        REQUIRE(s.packets(3));
        for (unsigned i = 1; i <= 6; i++)        // slots 1..6: bit 0 of b2 no longer programs
            emu_stuck1[d.page0 * EMU_PAGE_SIZE + i * 4 + 3] = 0x01;
        REQUIRE(s.boot());                       // block 3: slots 1..3 fail, ctr_pending = 3
        CHECK_EQ(d.pending(), 3);
        REQUIRE(s.packet());                     // retry in slots 4..6 fails; 0x300 is sent
        CHECK_EQ(d.pending(), 3);
        REQUIRE(s.boot());                       // newest is still block 1: block 3 again
        s.packets(1);
        s.report();
        CHECK_EQ(s.violations, 0);
    }
}
