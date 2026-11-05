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
//                 any failed allocation from this boot or the one before, and
//                 (pre-v3 ESP32-P4) the startup heap headroom below.
//
// Startup heap (pre-v3 ESP32-P4 only). Until ESP-IDF's `main` task runs, the
// only internal heap is the low region (below 0x4ff3afc0) minus static data
// (.data/.bss, which holds every ESPHome entity and global). IDF's startup
// allocations and then the 8 KB main task stack must fit in what is left; if
// they do not, the panel aborts at power-up in app_startup.c
// (`assert(res == pdTRUE)`), before ESPHome runs, and an OTA install is rolled
// back. That is what the 8-thermostat builds (#63, #73) did.
// The size of that startup heap is fixed at link time (LOW_DRAM_END minus
// _heap_start_low); log_report() logs it against the size known to boot, and
// CI (.github/scripts/check_startup_ram.py) fails a build that falls below it.
// See CLAUDE.md, Gotchas.

#include <cinttypes>
#include <cstdint>

#include "esp_attr.h"
#include "esp_heap_caps.h"
#include "esphome/core/log.h"
#include "sdkconfig.h"

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

#if CONFIG_IDF_TARGET_ESP32P4 && CONFIG_ESP32P4_SELECTS_REV_LESS_V3
#define ESP32_DASH_STARTUP_HEAP_REPORT 1
/// End of the low internal RAM region on pre-v3 P4 (ROM stack start - 8 KB,
/// APP_USABLE_DIRAM_END in esp-idf heap/port/esp32p4/memory_layout.c).
static constexpr intptr_t LOW_DRAM_END = 0x4ff3afc0;
/// Known boot / no-boot sizes of the startup heap (ESPHome 2026.10.2): the
/// 4-thermostat build (#72) booted with 111488 bytes, the 8-thermostat build
/// (#73) aborted at power-up with 107008.
static constexpr uint32_t STARTUP_HEAP_KNOWN_GOOD = 111488;
extern "C" int _heap_start_low;  // linker: end of static data in the low region
#endif

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
#ifdef ESP32_DASH_STARTUP_HEAP_REPORT
  // Fixed at link time: what static RAM leaves of the low region. IDF's startup
  // allocations and the 8 KB main task stack must fit in it.
  const uint32_t startup_heap = (uint32_t) (LOW_DRAM_END - (intptr_t) &_heap_start_low);
  if (startup_heap < STARTUP_HEAP_KNOWN_GOOD) {
    ESP_LOGW("alloc_diag", "Startup heap (low internal RAM): %u bytes, below the %u known to boot: "
             "static RAM growth can stop the panel booting",
             (unsigned) startup_heap, (unsigned) STARTUP_HEAP_KNOWN_GOOD);
  } else {
    ESP_LOGI("alloc_diag", "Startup heap (low internal RAM): %u bytes (%d vs the %u known to boot)",
             (unsigned) startup_heap, (int) (startup_heap - STARTUP_HEAP_KNOWN_GOOD),
             (unsigned) STARTUP_HEAP_KNOWN_GOOD);
  }
#endif
  log_one("Previous boot", s_previous);
  log_one("This boot", s_current);
}

}  // namespace alloc_diag
}  // namespace esp32_dash
