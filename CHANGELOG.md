# Changelog

## [Unreleased]

### Perf: increase LVGL render buffer 12% → 25%

`device/lvgl.yaml` — `buffer_size: 12%` → `25%` (~240 KB → ~500 KB).
Fewer render passes per frame; view transitions and animations are visibly faster.
Extra cost: ~260 KB PSRAM — well within the ~5.6 MB headroom remaining after image buffers.

### Refactor: collapse 10 subscribe_idle_sensor scripts into one (`weather_sensors.yaml`)

The 10 identical `subscribe_idle_sensor_0..9` scripts (~1,450 lines) are replaced by a
single parameterized script `subscribe_idle_sensor(slot: int)`.

- LVGL widget pointers (`tile`, `name`, `value`, `icon`, `unit`) resolved via local C-style
  arrays indexed by `slot`, then captured by value in each subscription callback.
- Entity selectors and precision components (`idle_sensor_entity_N`, `idle_sensor_prec_N`)
  dispatched via `switch(slot)` — they remain individual ESPHome components with NVS persistence.
- State publish and icon lookup (`mdi:*` → compiled glyph) appear once instead of ten times.
- Left-column unit repositioning (`slot < 5`) and right-column fixed-position handling unified
  by a single `if (slot < 5)` guard in the `unit_of_measurement` callback.
- `idle_sensor_gen_0..9` (10 × `int`) and `idle_sensor_raw_0..9` (10 × `std::string`) replaced
  by two `std::array<T, 10>` globals; callbacks index with `id(idle_sensor_gen)[slot]`.

**Result:** `weather_sensors.yaml` 2304 → 1070 lines (−1,234 lines, −54%).

### Refactor: collapse 9 calendar scripts into 3 (`calendar_sensors.yaml`)

Three families of near-identical scripts collapsed into parameterized equivalents:

| Before | After | Lines saved |
|---|---|---|
| `fetch_idle_http_0/1/2` (3 × 107 lines) | `fetch_idle_http(slot: int)` | ~190 |
| `subscribe_calendar_0/1/2` (3 × 91 lines) | `subscribe_calendar(slot: int)` | ~170 |
| `fetch_cal_http_0/1/2` (3 × 111 lines) | `fetch_cal_http(slot: int)` | ~200 |

Each `fetch_*` script self-chains: at the end of its action list it calls
`id(fetch_*).execute(slot + 1)` if `slot < 2`, otherwise triggers the render script.
This preserves the original sequential-fetch behaviour (calendar 0 → 1 → 2 → render)
while using a single script definition.

`subscribe_calendar` resolves per-slot globals and label pointers via `switch(slot)` at the
top of the lambda, then captures them by pointer/value in the five subscription callbacks.

**Result:** `calendar_sensors.yaml` 1385 → 843 lines (−542 lines, −39%).

### What did NOT change

- All HA entity registrations, names, and NVS storage keys — no device re-pair needed.
- Subscription semantics and generation-counter invalidation logic — identical behaviour.
- LVGL widget pre-allocation in `idle_view.yaml`, `lvgl.yaml`, `forecast_view.yaml` —
  ESPHome's LVGL integration requires statically declared widget IDs; these cannot be
  loop-generated and were left as-is.
- All render lambdas (`render_forecast`, `render_calendar_grid`, `render_idle_agenda`).
