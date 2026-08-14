# esp32-dash

ESPHome-based multi-view kitchen dashboard for the **Guition ESP32-P4-JC8012P4A1** (10", 1280×800 landscape). Derived from [esphome-media-player](https://github.com/jtenniswood/esphome-media-player) — the music view and all media player infrastructure is preserved intact.

## Device

- **Hardware:** Guition ESP32-P4 + ESP32-C6 coprocessor (WiFi/BT via SDIO)
- **Display:** MIPI DSI JC8012P4A1, 1280×800, landscape (90° or 270°)
- **Touch:** GSL3680
- **Flash:** 16MB, PSRAM hex mode 200MHz
- **GitHub repo:** `nnmalex/esp32-dash`

## Project structure

```
guition-esp32-p4-jc8012p4a1/
  esphome.yaml          # user-facing GitHub import entry point
  dev.yaml              # local dev (uses local components path)
  packages.yaml         # top-level manifest — includes all sub-packages
  addon/                # feature modules
    music.yaml          # online_image external_component + album art scripts
    accent_color.yaml   # extract dominant colour from album art
    backlight.yaml      # day/night brightness (no dimming/screensaver stage)
    network.yaml        # WiFi boot flow, diagnostics
    speaker_group.yaml  # multi-room speaker grouping
    time.yaml           # HA time sync
    timezone.yaml       # clock timezone select
  assets/
    fonts.yaml          # Roboto variants (20–300px)
    icons.yaml          # Material Design icons (28–64px)
    placeholder.png     # fallback album art
  device/
    device.yaml         # hardware config, globals, touch gestures, state machine
    ha_settings.yaml    # ha_base_url / ha_token_global from compile-time substitutions
    lvgl.yaml           # music_page widget definitions + overlays
    sensors.yaml        # template sensors, 1s playback interpolation
    media_player_select.yaml  # dynamic HA entity subscription
    navbar.yaml         # nav_bar widget, current_view global, show_*_view scripts
    idle_view.yaml      # idle_page widgets, update_idle_clock
    weather_sensors.yaml      # weather + 10 sensor tile subscriptions, idle_sensor_reformat
    calendar_view.yaml  # calendar_page 5-day grid, render_calendar_grid
    calendar_sensors.yaml     # calendar subscriptions, fetch_calendar_data, render_idle_agenda
    forecast_view.yaml  # forecast_page widgets, render_forecast
    forecast_sensors.yaml     # fetch_forecast, wf_data_buf
    timer_overlay.yaml  # floating timer_bar above the nav bar
  theme/
    button.yaml         # LVGL style definitions
components/
  online_image/         # custom C++ component: downloads & decodes album art
  calendar_json/        # header-only JSON parser shared by calendar + forecast
  gsl3680/              # vendored touch driver (see its README.md)
  libjpeg-turbo-esp32/  # JPEG decode library (CMake IDF component)
builds/
  guition-esp32-p4-jc8012p4a1.yaml          # base build config
  guition-esp32-p4-jc8012p4a1.factory.yaml  # factory/web-installer build
```

## Local development

```bash
# Compile using local components (bypasses GitHub URL for online_image)
cd guition-esp32-p4-jc8012p4a1
esphome compile dev.yaml

# Flash
esphome run dev.yaml
```

`dev.yaml` overrides `external_components` to use `../components` (local path) and sets `display_rotation: "270"` for a flipped-landscape bench setup.

## Package deployment

End users import from GitHub:

```yaml
packages:
  esp32_dash:
    url: https://github.com/nnmalex/esp32-dash
    files: [guition-esp32-p4-jc8012p4a1/packages.yaml]
    ref: main
    refresh: 1s
```

`addon/music.yaml` pulls `online_image` and `calendar_json` as `external_components`
from this same repo; `device/device.yaml` pulls `gsl3680` the same way. `dev.yaml` and
`builds/guition-esp32-p4-jc8012p4a1.yaml` override all three with a local path so local
builds use the working tree instead of GitHub.

**Minimum ESPHome version is 2026.7.0.** `image:` entries use `platform: file`, which
does not exist before 2026.7 — older versions fail validation. CI pins
`>=2026.7.0,<2026.8` so it tests what devices actually build with; that pin was
previously `<2026.5`, which is why a build break on 2026.6+ passed CI unnoticed.

**There is currently no OTA update-check feature.** `.github/workflows/firmware.yml` only compiles `builds/guition-esp32-p4-jc8012p4a1.yaml` and validates the factory build — it does not publish a manifest, upload an OTA binary, or inject a version. `project.version` is hardcoded to `dev` in both build files. Devices therefore expose no `update` entity for dashboard firmware, and users update by re-flashing or via the ESPHome dashboard. See "Not implemented" below.

## Architecture: LVGL state machine

Four LVGL pages defined across two files (`device/lvgl.yaml` + `device/navbar.yaml`):

- **`music_page`** (1280×800) — existing media player UI
  - Left 800px: album art panel (`album_art_background_widget`)
  - Right 480px: track info (title, artist, time, play/pause button)
  - Bottom: 6px progress bar
  - Swipe-down (from top 200px): volume arc / speaker group overlay
  - Full-screen overlays: setup prompts, loading screen
- **`idle_page`** (Phase 4b complete) — two-pane layout
  - Left 800px: weather background image + dark overlay; clock+date top-left; condition+temp top-right; two columns of 5 sensor tiles each (left col x=0..389 = tiles 0-4, right col x=400..789 = tiles 5-9; hidden when entity not configured)
  - Right 480px: merged calendar agenda (today+tomorrow from all 3 calendars, sorted, past events greyed out) — 5 agenda slots (`idle_agenda_slot_0..4`)
- **`calendar_page`** (Phase 4c complete) — 5-day week grid view
  - Header (y=0..60): 5 day-column labels (day name + date), highlighted today, prev/next nav (offset -1..+2)
  - All-day strip (y=60..84): one chip per column for all-day events
  - Time grid (y=84..740, scrollable 656px): 44px/hour, default scroll shows 6 AM–9 PM; 30 pre-allocated event blocks (6 per column), current-time red indicator
  - Data: fetched via HA REST API (`GET /api/calendars/<entity>?start=...&end=...`) by `fetch_calendar_data`, which loops over the 3 calendar slots in one script and fills the shared `cal_events_buf`; requires the `ha_token` substitution to be set
  - Legacy 9 labels kept hidden for idle-agenda subscription compatibility
- **`forecast_page`** — (Phase 5 complete) 7-day weather forecast
  - 7 columns (183 px wide each); column centres at 91+i×183
  - Header (y=0..88): day name + date number per column; today highlighted in accent blue
  - Temperature chart (y=90..450): two `lv_line` polylines (`wf_line_high` 0x6CB4F0, `wf_line_low` 0x4B78B0) with points computed in `render_forecast` at x = 91 + 1098·i/6; floating temp labels above/below each point. Days whose forecast omits `temperature`/`templow` are left out of the polyline rather than plotted at zero
  - Precipitation (y=452..608): bars scaled to daily max, opacity ∝ probability; mm + % labels
  - Condition icons (y=614): MDI weather icons per day (14 condition glyphs added to `icon_font`)
  - Data: `POST /api/services/weather/get_forecasts?return_response=true`; reuses `weather_entity_select` + the `ha_token` substitution
  - Key IDs: `wf_line_high`, `wf_line_low`, `wf_col_bg/day/date/high_lbl/low_lbl/precip_bar/mm_lbl/pct_lbl/icon_0..6`
  - Key scripts: `fetch_forecast`, `render_forecast`
  - Globals: `wf_data_buf` (pipe-delimited lines), `wf_today_col`, `wf_last_fetch_ms`

Navigation bar (`nav_bar`) defined in `device/navbar.yaml`, reparented to `lv_layer_top()` on boot so it floats above all pages. 60px bar at y=740, visible on all four views (every `show_*_view` script reveals it; it starts hidden only so it does not flash during boot/setup), four icon buttons: Home, Music, Calendar, Forecast.

Timer overlay (`timer_bar`) defined in `device/timer_overlay.yaml`, also reparented to `lv_layer_top()` on boot. 42px bar at y=698 (immediately above nav bar), hidden when no timers active. Shows soonest-expiring active timer name + MM:SS countdown + "+N more" badge. Tapping dismisses until next HA `remaining` update. Subscribes to up to 3 `timer.*` entities (compile-time substitutions `timer_entity_1..3` or runtime via HA device settings). Local 1s countdown between HA updates (same pattern as playback interpolation). Time label turns red when < 60 s remaining.

Global state flags in `device/device.yaml`:
- `actions_prompt_acked` — user dismissed the "enable actions" prompt (NVS-backed)
- `device_has_been_setup` — first successful setup completed; suppresses setup prompts on restart (NVS-backed)
- `is_panel_open`, `was_panel_open` — speaker/settings panel state
- `touch_x_start/y_start/x_end/y_end` — swipe gesture tracking

Global state in `device/navbar.yaml`:
- `current_view` (int) — 0=idle, 1=music, 2=calendar, 3=forecast

View-switching scripts in `device/navbar.yaml`: `show_idle_view`, `show_music_view`, `show_calendar_view`, `show_forecast_view`.

Auto-switching driven by `sensors.yaml`:
- `"playing"` + `current_view == 0` → `show_music_view`
- `"idle"/"off"/"standby"` → `delayed_idle_cleanup` → `current_view == 1` → `show_idle_view`
- `"paused"` stops `delayed_idle_cleanup` (stay on music page until actually idle)

Media state driven by `sensors.yaml`: subscribes to HA entity attributes, interpolates playback position every 1 second between HA updates.

## Roadmap: multi-view architecture

Phase 1 (complete): scaffolding — 10" device, URLs updated, `main_page` → `music_page`.
Phase 2 (complete): navbar, `current_view` global, auto-switching, swipe gestures scoped per view (swipe-down opens the settings panel, horizontal swipes skip tracks — both music-page only).
Phase 3 (complete): `device/idle_view.yaml` + `device/weather_sensors.yaml` — real idle page with clock, weather card, calendar preview placeholders, 4-tile sensor row; weather entity + 4 sensor row entities (NVS-persisted, gen-counter subscriptions).
Phase 4 (complete): `device/calendar_view.yaml` + `device/calendar_sensors.yaml` — real calendar page; 3 calendar entity slots.
Phase 4c (complete): Calendar view redesigned as 5-day week grid. Key IDs: `cal_grid_scroll` (scrollable time grid), `cal_col_hdr_0..4`, `cal_ev_00..29` (event blocks), `cal_ad_0..4` (all-day chips), `cal_now_line` (current time). New globals: `cal_view_offset`, `cal_events_buf`, `cal_fetch_start/end`, `cal_last_fetch_ms`. New scripts: `fetch_calendar_data`, `render_calendar_grid`, `position_cal_now_line`. Token set via the `ha_token` substitution.
Phase 4b (complete): Idle view redesign — two-pane layout (800px left + 480px right). Left pane: weather background image (online_image, loaded from HA `/local/` path by condition name), dark overlay, clock+date top-left, weather condition+temperature top-right, two columns of 5 sensor tiles each (left col x=0..389 = tiles 0-4, right col x=400..789 = tiles 5-9; hidden when entity not configured). Right pane: merged calendar agenda (today+tomorrow from all 3 calendars, sorted, past events greyed out). New substitutions: `weather_bg_path`, `local_temp_entity`, `idle_sensor_1..10`. Key new IDs: `idle_weather_bg_image` (online_image in weather_sensors.yaml), `idle_sensor_tile_0..9`, `idle_agenda_slot_0..4`, `render_idle_agenda` script.

### Phase 5 (complete): forecast view

| Phase | New file | Contents |
|---|---|---|
| 5 | `device/forecast_view.yaml` | LVGL `forecast_page`: 7-day chart + precipitation + condition icons |
| 5 | `device/forecast_sensors.yaml` | `wf_data_buf` global, `fetch_forecast` (HTTP POST), 30-min interval |
| 6 | `device/timer_overlay.yaml` | floating `timer_bar` above nav bar; subscribes to up to 3 `timer.*` entities |

## Idle page: weather background images

The left pane of the idle page displays a full-panel weather background image loaded from the HA server. Set up:

1. **Substitution** — add to your `packages.yaml` substitutions:
   ```yaml
   weather_bg_path: "/local/weather-backgrounds"
   ```
   Leave empty (default) to disable — the panel shows a plain dark background.

2. **Images** — place JPEG files at `<HA config>/www/weather-backgrounds/` named after the OpenWeatherMap condition strings HA reports:
   ```
   clear-night.jpg   cloudy.jpg        exceptional.jpg   fog.jpg
   hail.jpg          lightning.jpg     lightning-rainy.jpg
   partlycloudy.jpg  pouring.jpg       rainy.jpg
   snowy.jpg         snowy-rainy.jpg   sunny.jpg
   windy.jpg         windy-variant.jpg
   ```
   Recommended size: 800×740 px JPEG. The `online_image` component decodes and scales the image on-device.

3. **Local temperature sensor** — optionally add to substitutions:
   ```yaml
   local_temp_entity: "sensor.indoor_temperature"
   ```
   When set, this sensor's value replaces the weather entity's temperature on the idle page. Can also be configured at runtime in HA device settings.

## Creating GitHub releases

When the user asks to create a release, follow this process:

1. **Find the previous release tag** — `gh release list --limit 1` to get the last tag.
2. **Collect commits since last release** — `git log <last-tag>..HEAD --oneline`. If no prior release, use the last 20 commits.
3. **Write user-friendly release notes** — rewrite the raw commit messages as plain-English bullet points grouped by theme (e.g. "New features", "Improvements", "Bug fixes"). Rules:
   - Drop internal/tooling commits (CI tweaks, CLAUDE.md updates, doc-only fixes) unless they affect users.
   - Translate technical shorthand into what the user actually sees change (e.g. "Fix #18: move ha_host/port to substitutions" → "HA connection settings (host, port, token) are now configured in your `packages.yaml` substitutions instead of the HA device settings UI").
   - Keep each bullet to one sentence. No jargon, no commit hashes.
4. **Create the release** — pick a version tag (`YYYY.MM.DD` or semantic if appropriate):
   ```bash
   gh release create <tag> --title "<tag>: <one-line summary>" --notes "<notes>"
   ```
5. **Note** — the release does not rebuild or publish firmware. CI compiles on push to `main` but publishes nothing, so release notes are not surfaced on-device anywhere; the release is documentation only.

## Not implemented

Features that earlier revisions of this document described as existing, but which
are not in the tree. Listed so they are not mistaken for regressions:

- **OTA update check / firmware manifest.** There is no `addon/firmware_update.yaml`,
  no `update` entity for dashboard firmware, and no manifest or OTA binary published
  anywhere. `.github/workflows/firmware.yml` compiles and validates only.
- **Screensaver / screen dimming.** `addon/backlight.yaml` provides day/night
  brightness only. `backlight_wake_timeout` contains no delay despite its name, so
  the panel never dims or blanks — it stays at the configured brightness
  indefinitely. There are no `is_screen_dimmed` / `is_clock_screensaver_showing`
  globals and no clock screensaver overlay.
- **Swipe-up to idle.** The touch handler in `device/device.yaml` implements
  swipe-down (settings panel) and horizontal swipes (track skip) only.

## Broken: factory build is missing its C6 blob

`builds/guition-esp32-p4-jc8012p4a1.factory.yaml` declares:

```yaml
update:
  - platform: esp32_hosted
    path: network_adapter_esp32c6.bin
```

`builds/network_adapter_esp32c6.bin` is **not in the repo and never has been**, so
`esphome config builds/guition-esp32-p4-jc8012p4a1.factory.yaml` fails with
"Could not find file". The factory/web-installer build therefore cannot be built
from a clean checkout. CI skips its validation step while the file is absent (with
a warning) and starts enforcing it once the blob is committed. Either commit the
blob or drop the `update:` block.

## Known constraint: synchronous HTTP

ESPHome's `http_request` is blocking — `->get()` / `->post()` and the response
drain all run inline on the main loop, stalling LVGL for the duration. This
affects `fetch_calendar_data` (1 request) and `fetch_forecast` (1). It cannot be
made async without a custom component, so the code minimises how often it
happens, and how long each one takes:

- **One request per calendar cycle.** `fetch_calendar_data` POSTs to
  `/api/services/calendar/get_events?return_response=true` with every configured
  entity in one `entity_id` list, so HA resolves all three server-side. Requires
  HA ≥ 2024.2. HA rejects the whole call if any one entity id is unknown, so a
  failed batch falls back to the old per-entity `GET /api/calendars/<entity>`
  loop, where a bad entity only costs its own slot.
- One fetch feeds both the week grid and the idle agenda, on a single 15-minute
  interval, **skipped while `current_view == 1`** (music) — neither consumer is
  on screen there. `show_idle_view` calls `maybe_fetch_calendar`, which catches
  up whatever went stale.
- The fetch window is fixed at day −1..+7, so calendar prev/next navigation
  re-renders from cache and never refetches.
- **Interactive callers go through `maybe_fetch_calendar` /
  `maybe_fetch_forecast`** (`calendar_sensors.yaml`, `forecast_sensors.yaml`),
  never `fetch_*` directly. Those wrappers `delay: 300ms` so the stall lands
  after the page transition has drawn, check staleness (5 min calendar, 15 min
  forecast) before fetching at all, and are `mode: restart` so a flurry of
  navbar taps coalesces into one fetch.
- `buffer_size_rx: 4096` on `http_request` (default is 512), with matching 4 KB
  read chunks off the heap. Every `read()` constructs a `WatchdogManager`, i.e. a
  task-WDT reconfigure, so this cuts the per-response overhead ~8×. Response
  strings are reserved from `content_length` where available.
- `timeout` is deliberately left at the 4.5 s default: it applies per socket
  operation, and most of a calendar fetch is HA waiting on the upstream provider
  before sending headers, so lowering it would fail slow-but-valid fetches. Both
  fetch scripts log `container->duration_ms` ("blocked the loop for N ms") so
  this can be revisited with real numbers.

Keep new callers on the same principle: render from cache first, fetch only when
the data is actually stale, and never fetch synchronously from a touch handler.

### Future enhancement: replace REST polling with HA push

The blocking fetches disappear entirely if HA pushes the data instead of the
device pulling it. Sketch, not implemented:

- HA-side template sensor holds the payload in an **attribute** (not the state —
  state values are capped at 255 chars, attributes are not), rendered in the same
  pipe-delimited format `cal_events_buf` / `wf_data_buf` already use.
- Device subscribes with `subscribe_home_assistant_state` over the already-open,
  non-blocking native API socket — the same mechanism `calendar_sensors.yaml`
  and `weather_sensors.yaml` use for entity attributes today.
- That removes `http_request`, the `ha_token` substitution, the JSON parsing, and
  every main-loop stall; the device only renders. `calendar_json.h` stays for
  the fallback path.
- Cost is a config burden: users must add the template sensors to their HA
  config. So it belongs as a *preferred* path with the REST fetch kept as the
  fallback when the template sensor is absent, not as a straight replacement.

The other option considered was a custom component with a FreeRTOS worker task
doing the HTTP off the loop task and handing results back via `defer()`. It keeps
the zero-config story but costs ~250 lines of C++ and careful thread discipline
(no LVGL or global access from the worker). Push is the cheaper win if the HA-side
setup is acceptable.

## Idle page: key substitutions summary

| Substitution | Default | Purpose |
|---|---|---|
| `weather_entity` | `""` | `weather.*` entity for condition + temperature |
| `local_temp_entity` | `""` | Optional `sensor.*` to override displayed temperature |
| `weather_bg_path` | `""` | HA `/local/` path for condition background images |
| `idle_sensor_1..10` | `""` | Up to 10 arbitrary HA entities for sensor tiles (5 left + 5 right column) |
| `calendar_entity_1..3` | `""` | Up to 3 `calendar.*` entities for the agenda panel |
