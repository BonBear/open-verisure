/**
 * @file esp_heap_caps.h (host test stub)
 * @brief ESP-IDF capability-based allocation for the host unit tests: there is no PSRAM on the
 *        host, so heap_caps_malloc() fails and the code under test falls back to malloc().
 */

#pragma once

#include <stddef.h>
#include <stdint.h>

#define MALLOC_CAP_8BIT   (1 << 2)
#define MALLOC_CAP_SPIRAM (1 << 10)

inline void* heap_caps_malloc(size_t, uint32_t) { return nullptr; }
