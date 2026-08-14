# Changelog

## [Unreleased]

### Fix: stop leaking Home Assistant state subscriptions on every reconnect

`subscribe_weather`, `subscribe_local_temp`, `subscribe_feels_like`,
`subscribe_idle_sensor`, `subscribe_calendar`, `subscribe_timer` and
`subscribe_media_player` all re-registered their callbacks from
`api.on_client_connected`. ESPHome has no unsubscribe — `subscribe_home_assistant_state()`
only ever `push_back()`s onto `APIServer::state_subs_`, and nothing in the codebase
erases from that vector — so each HA reconnect added ~77 closures (plus two heap
strings each) that were never freed, and that vector is walked for *every* inbound
state message.

Each script now records the entity it registered (`weather_subbed`, `cal_subbed`,
`tmr_subbed`, `idle_sensor_subbed`, `media_player_subbed`) and skips re-subscription
when it is unchanged. This is safe because `APIConnection::process_state_subscriptions_()`
re-sends the whole existing list to each newly connected client. The generation
counter is likewise only bumped when actually re-registering — bumping it while
reusing subscriptions would have neutered the live callbacks.

`subscribe_media_player` additionally no longer blanks the now-playing UI on
reconnect, which used to make the display flicker on every HA restart.

### Fix: calendar JSON parser (`components/calendar_json/calendar_json.h`)

- **Long titles merged two events into one.** Lines were built with
  `snprintf` into a `char[300]`; a title ≥ 255 chars truncated away the trailing
  `\n`, so the next event was appended onto the same line and parsed as garbage
  (and lost). Lines are now assembled with `std::string` and titles are explicitly
  capped at `MAX_TITLE_LEN`.
- **Escape sequences were emitted literally.** `\n` became the letter `n` and
  `é` became `u00e9`. Now decoded properly, including `\uXXXX` with surrogate
  pairs → UTF-8.
- **A string *value* equal to the key name aborted the search.** `extract_*_field`
  matched any depth-1 string, then bailed out when the next char was `,` instead of
  `:` — so an event with `"description": "summary"` dropped the whole event. Key
  position is now checked properly via a shared `find_member_value`, and depth
  tracking counts `[`/`]` as well as `{`/`}`.

### Fix: per-tile precision control was ignored below 1000

`weather_sensors.yaml` formatted values under 1000 with a hardcoded `%.0f`, so
"Sensor Tile N: Precision" did nothing for the common case (precision 2 on a
21.7 °C sensor rendered `22`). The k-scaling and precision logic existed in three
places and had drifted; it now lives only in `idle_sensor_reformat(slot)`, which
the state callback, the unit callback and the precision `set_action` all call.

### Fix: missing forecast temperatures no longer distort the chart

`forecast_sensors.yaml` defaulted absent `temperature` / `templow` to `0.0`, which
dragged the chart's y-range down to zero and printed a bogus `0°`. Presence is now
tracked per field, absent values are left out of the polyline, and the label is
blanked. Also: `day_names[tm_d.tm_wday & 7]` could index one past the 7-element
array — replaced with a real bounds check — and `mktime` now gets `tm_isdst = -1`
and midday instead of asserting "not DST" at 00:00.

### Perf: halve calendar HTTP traffic and stop blocking the UI on navbar taps

`http_request` is synchronous, so every fetch stalls the main loop and LVGL.

- `fetch_idle_agenda` + `fetch_calendar_week` and their six near-identical
  per-slot HTTP scripts are replaced by a single `fetch_calendar_data` that loops
  over the three slots. One fetch now feeds both the week grid and the idle
  agenda: 3 requests per cycle instead of 6, and they no longer both fire on the
  same 15-minute tick.
- The fetch window is fixed at day −1..+7, covering every reachable
  `cal_view_offset`, so calendar prev/next re-renders from cache instead of
  refetching.
- Navbar taps only refetch when the cache is stale (5 min calendar / 15 min
  forecast), via new `cal_last_fetch_ms` / `wf_last_fetch_ms` globals.
- The idle page's redundant hourly `minute == 0` fetch is gone — it duplicated the
  15-minute interval and could miss or double-fire as the 60 s tick drifted.
- Calendar subscription callbacks no longer trigger `render_idle_agenda` once a
  fetch has populated the buffer (they were 9 wasted full re-renders per reconnect).
- A failed fetch keeps the previous cache instead of blanking both views.

### Other fixes

- `online_image.cpp`: `dsc_.data = buffer_ + 1` removed — `get_lv_image_dsc()` on
  the next line rebuilds the descriptor from `data_start_`, so it was dead code
  that read as a deliberate one-byte offset (`accent_color.yaml` indexes
  `dsc->data` directly as pixels). Also guarded the `downloader_` dereference in
  the completion path.
- `sensors.yaml`: `ha_position_epoch` / `last_position_timestamp` widened to
  `int64_t` — `long` is 32-bit on ESP32, so the epoch arithmetic overflowed in 2038.
- `timer_overlay.yaml`: the bar showed a blank name when the optional
  `input_text.timer_name_*` helper did not exist; falls back to "Timer". The 1 s
  tick no longer does LVGL work while the bar is dismissed.
- `calendar_view.yaml`: the current-time indicator's geometry was duplicated with
  hardcoded `44`/`244` in the per-minute interval; extracted to
  `position_cal_now_line`. `render_calendar_grid` takes the event buffer by
  reference instead of copying it.
- `device.yaml`: logger `DEBUG` → `INFO` (drops per-touch/per-frame log traffic;
  every `ESP_LOGI` this project emits is still shown).
- `builds/*.factory.yaml`: the ESP32-C6 update check ran a 10 s interval forever to
  do nothing after the first pass; now a one-shot `on_boot`.
- `.github/workflows/firmware.yml`: also validates the factory build, which was
  never checked by CI.

Flash: 3,504,148 → 3,486,074 bytes (−18 KB).

### Docs: CLAUDE.md corrected to match the tree

Removed or rewrote descriptions of things that do not exist: `addon/firmware_update.yaml`
and the entire OTA manifest/`FIRMWARE_VERSION`/`gh api releases/latest` pipeline,
the 2-stage screensaver and its `is_screen_dimmed` / `is_clock_screensaver_showing`
/ `setup_done` / `is_wifi_setup_done` globals, swipe-up-to-idle, `ha_token_text`,
`wf_chart`/`wf_chart_high`/`init_forecast_chart` (the forecast uses `lv_line`, not
`lv_chart`), and the claim that the navbar is hidden on the music page. Added
"Not implemented" and "Known constraint: synchronous HTTP" sections.

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
