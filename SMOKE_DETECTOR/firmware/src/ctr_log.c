#include "ctr_log.h"

#include "em_device.h"
#include "em_msc.h"

#define CTR_PAGE_SIZE   FLASH_PAGE_SIZE
#define CTR_PAGE_A      (FLASH_BASE + FLASH_SIZE - 2u * CTR_PAGE_SIZE)
#define CTR_PAGE_B      (FLASH_BASE + FLASH_SIZE - 1u * CTR_PAGE_SIZE)
#define CTR_WORDS       (CTR_PAGE_SIZE / 4u)
#define CTR_EMPTY       0xFFFFFFFFu
#define CTR_BLOCK_MAX   0x00FFFFFFu

/* Block confirmed in flash during this boot; counters of any other block are never sent. */
static uint32_t s_logged_block;

/* Highest block in a page and the index of the first free word (CTR_WORDS when full). */
static uint32_t page_scan(uint32_t base, uint32_t *free_idx)
{
  const volatile uint32_t *w = (const volatile uint32_t *)base;
  uint32_t max = 0u;
  uint32_t i;

  for (i = 0; i < CTR_WORDS && w[i] != CTR_EMPTY; i++) {
    if (w[i] <= CTR_BLOCK_MAX && w[i] > max) {
      max = w[i];
    }
  }
  *free_idx = i;
  return max;
}

static bool flash_write(uint32_t addr, uint32_t value)
{
  return MSC_WriteWord((uint32_t *)addr, &value, sizeof(value)) == mscReturnOk &&
         *(const volatile uint32_t *)addr == value;   /* read back: a failed write never counts */
}

static bool flash_erase(uint32_t addr)
{
  return MSC_ErasePage((uint32_t *)addr) == mscReturnOk;
}

/* Logs block in the page that holds the newest entry; when that page is full, writes the other
 * page first and only then erases the full one, so a power cut never loses the highest block. */
static bool log_block(uint32_t block)
{
  uint32_t free_a, free_b;
  const uint32_t max_a = page_scan(CTR_PAGE_A, &free_a);
  const uint32_t max_b = page_scan(CTR_PAGE_B, &free_b);
  const bool use_a = (max_a >= max_b);
  const uint32_t cur = use_a ? CTR_PAGE_A : CTR_PAGE_B;
  const uint32_t oth = use_a ? CTR_PAGE_B : CTR_PAGE_A;
  const uint32_t cur_free = use_a ? free_a : free_b;
  const uint32_t oth_free = use_a ? free_b : free_a;
  bool ok;

  if (block == 0u || block > CTR_BLOCK_MAX) {
    return false;   /* counter exhausted: send nothing rather than reuse a value */
  }
  MSC_Init();
  if (cur_free < CTR_WORDS) {
    ok = flash_write(cur + 4u * cur_free, block);
  } else {
    ok = (oth_free == 0u || flash_erase(oth)) && flash_write(oth, block);
    if (ok) {
      ok = flash_erase(cur);
    }
  }
  MSC_Deinit();
  return ok;
}

bool ctr_log_init(uint32_t *counter)
{
  uint32_t free_a, free_b;
  const uint32_t max_a = page_scan(CTR_PAGE_A, &free_a);
  const uint32_t max_b = page_scan(CTR_PAGE_B, &free_b);
  const uint32_t block = ((max_a > max_b) ? max_a : max_b) + 1u;   /* block 0 is never used */

  *counter = block << 8;
  if (!log_block(block)) {
    return false;
  }
  s_logged_block = block;
  return true;
}

bool ctr_log_reserve(uint32_t counter)
{
  const uint32_t block = counter >> 8;

  if (block == s_logged_block && block != 0u) {
    return true;
  }
  /* New block (or a block whose logging failed before): log it first. Never go back: after the
   * 32-bit counter wrapped (block 0, then 1, ...) nothing is sent again. log_block also refuses
   * blocks above CTR_BLOCK_MAX. */
  if (block == 0u || block < s_logged_block || !log_block(block)) {
    return false;
  }
  s_logged_block = block;
  return true;
}
