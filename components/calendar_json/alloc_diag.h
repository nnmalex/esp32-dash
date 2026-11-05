#pragma once

// Memory diagnostics: report how much internal RAM is left, and record any
// failed heap allocation so it can be read after the reset that follows.
//
// Internal RAM is the scarce heap on this board (see CLAUDE.md, Gotchas): with
// exceptions disabled a failed new / malloc / std::string growth aborts, and
// the 8-thermostat climate build (#63) aborted during start-up for reasons the
// backtrace alone could not show. This is deliberately small and passive, so it
// can sit in a build that boots without changing how it behaves:
//
//   install()     emitted into setup() by calendar_json's to_code. Registers a
//                 failed-allocation callback, which keeps the last failure
//                 (size, caps, free and largest block then) in .noinit RAM.
//   log_report()  runs from api: on_client_connected (device/device.yaml), so
//                 the dashboard log viewer shows it on connect: the internal
//                 heap now / largest block / minimum since boot, PSRAM free,
//                 and any failed allocation from this boot or the one before.

#include <cinttypes>
#include <cstdint>

#include "esp_attr.h"
#include "esp_heap_caps.h"
#include "esphome/core/log.h"

namespace esp32_dash {
namespace alloc_diag {

struct Record {
  uint32_t magic;
  uint32_t fails;    ///< failed allocations during that boot
  uint32_t size;     ///< last failed request, bytes
  uint32_t caps;     ///< its MALLOC_CAP_* flags
  uint32_t free;     ///< free bytes for those caps at the time
  uint32_t largest;  ///< largest free block for those caps at the time
};

static constexpr uint32_t MAGIC = 0xA110CD1A;

// Not zeroed at startup, so a soft reset (abort, panic, esp_restart) keeps them.
static __NOINIT_ATTR Record s_current;   ///< this boot, written by the callback
static __NOINIT_ATTR Record s_previous;  ///< what the previous boot recorded

static void on_failed_alloc(size_t size, uint32_t caps, const char *function_name) {
  (void) function_name;
  // Runs inside the allocator, possibly on any task: no logging, no allocation.
  s_current.fails++;
  s_current.size = size;
  s_current.caps = caps;
  s_current.free = heap_caps_get_free_size(caps);
  s_current.largest = heap_caps_get_largest_free_block(caps);
}

/// Moves the previous boot's record aside and starts recording this boot.
inline void install() {
  if (s_current.magic == MAGIC && s_current.fails > 0) {
    s_previous = s_current;
  } else {
    s_previous.magic = 0;
  }
  s_current = Record{MAGIC, 0, 0, 0, 0, 0};
  heap_caps_register_failed_alloc_callback(on_failed_alloc);
}

inline void log_one(const char *when, const Record &r) {
  if (r.magic != MAGIC || r.fails == 0)
    return;
  const char *kind = (r.caps & MALLOC_CAP_SPIRAM) ? "PSRAM" : (r.caps & MALLOC_CAP_INTERNAL) ? "internal" : "default";
  ESP_LOGW("alloc_diag",
           "%s: %" PRIu32 " failed allocation(s); last: %" PRIu32 " bytes (%s, caps 0x%" PRIX32 "), "
           "%" PRIu32 " free, largest block %" PRIu32,
           when, r.fails, r.size, kind, r.caps, r.free, r.largest);
}

inline void log_report() {
  ESP_LOGI("alloc_diag", "Internal heap: %u free, %u largest block, %u minimum since boot",
           (unsigned) heap_caps_get_free_size(MALLOC_CAP_INTERNAL),
           (unsigned) heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL),
           (unsigned) heap_caps_get_minimum_free_size(MALLOC_CAP_INTERNAL));
  ESP_LOGI("alloc_diag", "PSRAM: %u free, %u largest block",
           (unsigned) heap_caps_get_free_size(MALLOC_CAP_SPIRAM),
           (unsigned) heap_caps_get_largest_free_block(MALLOC_CAP_SPIRAM));
  log_one("Previous boot", s_previous);
  log_one("This boot", s_current);
}

}  // namespace alloc_diag
}  // namespace esp32_dash
