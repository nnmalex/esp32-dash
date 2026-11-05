#pragma once

// State of the climate page (device/climate_sensors.yaml), kept in PSRAM.
//
// Why not ESPHome `globals:`: every global, like every entity, is stored
// statically in internal RAM, in the low region below 0x4ff3afc0 on pre-v3
// ESP32-P4 silicon. Until ESP-IDF's `main` task exists, that region minus the
// static data is the only internal heap, and the 8 KB main task stack is
// allocated from it after IDF's own startup allocations. The 8-thermostat
// builds (#63, #73) grew static RAM by ~4.5 KB with per-slot globals and
// entities, the main task could no longer be created, and the panel aborted at
// power-up in app_startup.c (`assert(res == pdTRUE)`) before any ESPHome code
// ran. So the climate state lives here, allocated once from PSRAM, and only a
// pointer (`clim`) is a global. See CLAUDE.md, Gotchas ("Static internal RAM").
//
// Widget pointers stay in globals (climate_view.yaml): they are small.

#include <array>
#include <cmath>
#include <cstdint>
#include <new>
#include <string>

#include "esp_heap_caps.h"

namespace esp32_dash {

struct ClimateState {
  static constexpr int CARDS = 8;
  static constexpr int SWITCHES = CARDS * 3;  ///< slot = card * 3 + n
  static constexpr int BUTTONS = 6;

  // ---- Thermostats ------------------------------------------------------------
  /// Validated entity per slot ("" = slot unused).
  std::array<std::string, CARDS> entity;
  /// Entity registered with the API's subscription list (no unsubscribe exists).
  std::array<std::string, CARDS> subbed;
  /// Generation counter: invalidates callbacks of an entity that was replaced.
  std::array<int, CARDS> gen{};
  std::array<std::string, CARDS> name;
  std::array<std::string, CARDS> state;   ///< hvac mode ("off", "heat", ...) or "unavailable"
  std::array<std::string, CARDS> action;  ///< hvac_action
  std::array<std::string, CARDS> preset;  ///< preset_mode
  // Numeric attributes; NAN = not reported (or "None").
  std::array<float, CARDS> cur, target, low, high, step, min, max;
  /// Target set with +/- but not yet confirmed by HA (NAN = none).
  std::array<float, CARDS> pending;
  std::array<uint32_t, CARDS> pending_ms{};
  std::array<bool, CARDS> pending_sent{};

  // ---- Per-card switch rows ---------------------------------------------------
  std::array<std::string, SWITCHES> sw_entity, sw_subbed, sw_name, sw_state;
  std::array<int, SWITCHES> sw_gen{};

  // ---- Button row -------------------------------------------------------------
  std::array<std::string, BUTTONS> btn_entity, btn_subbed, btn_name, btn_state;
  std::array<int, BUTTONS> btn_gen{};
  /// Glyph mapped from the entity's `icon` attribute (nullptr = none / unmapped).
  std::array<const char *, BUTTONS> btn_icon_attr{};

  ClimateState() {
    for (auto *a : {&cur, &target, &low, &high, &step, &min, &max, &pending})
      a->fill(NAN);
  }
};

/// Allocates the climate state in PSRAM (internal RAM only if PSRAM is missing).
/// Never freed: it lives as long as the firmware.
inline ClimateState *new_climate_state() {
  void *p = heap_caps_malloc(sizeof(ClimateState), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
  if (p == nullptr)
    p = heap_caps_malloc(sizeof(ClimateState), MALLOC_CAP_8BIT);
  return new (p) ClimateState();
}

}  // namespace esp32_dash
