#pragma once

// Records failed heap allocations so an out-of-memory abort can be diagnosed
// from the next boot's log.
//
// With exceptions disabled, a failed `new` / std::string growth aborts the
// firmware. ESPHome's crash handler then reports the backtrace, but not what
// was being allocated or how much memory was left. This keeps the last failed
// allocation (size, capabilities, free and largest block for those
// capabilities at that moment) in .noinit RAM, which survives the software
// reset after the abort, and logs it on every API log connection after that.
//
// Allocations are expected to fail now and then without harm (LVGL tries PSRAM
// first, then internal RAM, for example), so a record on its own is not an
// error: it matters when it is followed by a crash.
//
// install() is called from an early on_boot (device/device.yaml); log_report()
// from api: on_client_connected.

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

/// Moves the record left by the previous boot aside and starts recording.
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
  ESP_LOGE("alloc_diag",
           "%s: %" PRIu32 " failed allocation(s); last: %" PRIu32 " bytes (%s, caps 0x%" PRIX32 "), "
           "%" PRIu32 " free, largest block %" PRIu32,
           when, r.fails, r.size, kind, r.caps, r.free, r.largest);
}

/// Logs what was recorded. Runs on API connect, so the dashboard log viewer
/// shows it next to ESPHome's own "crash detected on previous boot" report.
/// If install() did not run this boot (safe mode), s_current still holds the
/// record of the boot that crashed.
inline void log_report() {
  log_one("Previous boot", s_previous);
  log_one("This boot (or the crashed boot, in safe mode)", s_current);
  ESP_LOGI("alloc_diag", "Internal heap now: %u free, %u largest block, %u minimum since boot",
           (unsigned) heap_caps_get_free_size(MALLOC_CAP_INTERNAL),
           (unsigned) heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL),
           (unsigned) heap_caps_get_minimum_free_size(MALLOC_CAP_INTERNAL));
}

}  // namespace alloc_diag
}  // namespace esp32_dash
