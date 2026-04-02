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
    backlight.yaml      # 2-stage screensaver, day/night brightness
    network.yaml        # WiFi boot flow, diagnostics
    speaker_group.yaml  # multi-room speaker grouping
    time.yaml           # HA time sync
    timezone.yaml       # clock timezone select
    firmware_update.yaml# OTA http_request update check
  assets/
    fonts.yaml          # Roboto variants (20–300px)
    icons.yaml          # Material Design icons (28–64px)
    placeholder.png     # fallback album art
  device/
    device.yaml         # hardware config, globals, touch gestures, state machine
    ha_settings.yaml    # NVS-backed ha_host/ha_port/ha_protocol/ha_token UI components
    lvgl.yaml           # all LVGL widget definitions (music_page + overlays)
    sensors.yaml        # template sensors, 1s playback interpolation
    media_player_select.yaml  # dynamic HA entity subscription
  theme/
    button.yaml         # LVGL style definitions
components/
  online_image/         # custom C++ component: downloads & decodes album art
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

`addon/music.yaml` pulls `online_image` as an `external_component` from this same repo (`components/online_image/`). The firmware update manifest is published to `nnmalex.github.io/esp32-dash/firmware/esp32-dash-jc8012p4a1.manifest.json` by `.github/workflows/firmware.yml` on every push to `main`. The OTA binary is `esp32-dash-jc8012p4a1.ota.bin` alongside the manifest. `update_interval: never` in `firmware_update.yaml` is intentional — the `interval: 1h` block drives all checks (respects the user-configurable frequency select). `project.version` is injected via the `FIRMWARE_VERSION` env var at CI build time (defaults to `"dev"` locally).

## Architecture: LVGL state machine

Four LVGL pages defined across two files (`device/lvgl.yaml` + `device/navbar.yaml`):

- **`music_page`** (1280×800) — existing media player UI
  - Left 800px: album art panel (`album_art_background_widget`)
  - Right 480px: track info (title, artist, time, play/pause button)
  - Bottom: 6px progress bar
  - Swipe-down (from top 200px): volume arc / speaker group overlay
  - Swipe-up: go to idle screen
  - Full-screen overlays: clock screensaver, setup prompts, loading screen
- **`idle_page`** (Phase 4b complete) — two-pane layout
  - Left 800px: weather background image + dark overlay; clock+date top-left; condition+temp top-right; two columns of 5 sensor tiles each (left col x=0..389 = tiles 0-4, right col x=400..789 = tiles 5-9; hidden when entity not configured)
  - Right 480px: merged calendar agenda (today+tomorrow from all 3 calendars, sorted, past events greyed out) — 5 agenda slots (`idle_agenda_slot_0..4`)
- **`calendar_page`** (Phase 4c complete) — 5-day week grid view
  - Header (y=0..60): 5 day-column labels (day name + date), highlighted today, prev/next nav (offset -1..+2)
  - All-day strip (y=60..84): one chip per column for all-day events
  - Time grid (y=84..740, scrollable 656px): 44px/hour, default scroll shows 6 AM–9 PM; 30 pre-allocated event blocks (6 per column), current-time red indicator
  - Data: fetched via HA REST API (`GET /api/calendars/<entity>?start=...&end=...`) sequential chain for 3 calendars; requires `ha_token_text` to be set (configurable via HA device settings page)
  - Legacy 9 labels kept hidden for idle-agenda subscription compatibility
- **`forecast_page`** — (Phase 5 complete) 7-day weather forecast
  - 7 columns (183 px wide each); column centres at 91+i×183
  - Header (y=0..88): day name + date number per column; today highlighted in accent blue
  - Temperature chart (y=90..450): `lv_chart` line type, 2 series (high 0x6CB4F0, low 0x4B78B0); `pad_left/right=91` aligns points to column centres; floating temp labels above/below each dot
  - Precipitation (y=452..608): bars scaled to daily max, opacity ∝ probability; mm + % labels
  - Condition icons (y=614): MDI weather icons per day (14 condition glyphs added to `icon_font`)
  - Data: `POST /api/services/weather/get_forecasts?return_response=true`; reuses `weather_entity_select` + `ha_token_text`
  - Key IDs: `wf_chart`, `wf_col_bg/day/date/high_lbl/low_lbl/precip_bar/mm_lbl/pct_lbl/icon_0..6`
  - Key scripts: `init_forecast_chart` (lvgl.on_boot), `fetch_forecast`, `render_forecast`
  - Globals: `wf_data_buf` (pipe-delimited lines), `wf_chart_high/low` (lv_chart_series_t*), `wf_today_col`, `wf_fetch_gen`

Navigation bar (`nav_bar`) defined in `device/navbar.yaml`, reparented to `lv_layer_top()` on boot so it floats above all pages. 60px bar at y=740, hidden on music page, four icon buttons: Home, Music, Calendar, Forecast.

Timer overlay (`timer_bar`) defined in `device/timer_overlay.yaml`, also reparented to `lv_layer_top()` on boot. 42px bar at y=698 (immediately above nav bar), hidden when no timers active. Shows soonest-expiring active timer name + MM:SS countdown + "+N more" badge. Tapping dismisses until next HA `remaining` update. Subscribes to up to 3 `timer.*` entities (compile-time substitutions `timer_entity_1..3` or runtime via HA device settings). Local 1s countdown between HA updates (same pattern as playback interpolation). Time label turns red when < 60 s remaining.

Global state flags in `device/device.yaml`:
- `is_screen_dimmed`, `is_clock_screensaver_showing` — backlight stages
- `setup_done`, `is_wifi_setup_done` — boot flow

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
Phase 2 (complete): navbar, `current_view` global, auto-switching, swipe gestures scoped per view, swipe-up to idle.
Phase 3 (complete): `device/idle_view.yaml` + `device/weather_sensors.yaml` — real idle page with clock, weather card, calendar preview placeholders, 4-tile sensor row; weather entity + 4 sensor row entities (NVS-persisted, gen-counter subscriptions).
Phase 4 (complete): `device/calendar_view.yaml` + `device/calendar_sensors.yaml` — real calendar page; 3 calendar entity slots.
Phase 4c (complete): Calendar view redesigned as 5-day week grid. Key IDs: `cal_grid_scroll` (scrollable time grid), `cal_col_hdr_0..4`, `cal_ev_00..29` (event blocks), `cal_ad_0..4` (all-day chips), `cal_now_line` (current time). New globals: `cal_view_offset`, `cal_events_buf`, `cal_fetch_start/end`. New scripts: `fetch_calendar_week`, `fetch_cal_http_0/1/2`, `render_calendar_grid`. Token set via `ha_token_text` (HA device settings page) or `ha_token` substitution for initial value.
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
5. **Trigger a build** — the release itself does not rebuild firmware; that only happens on push to `main`. If the release corresponds to the current `HEAD`, the CI run already in flight (or the next push) will pick up the new release notes via the `gh api releases/latest` step and embed them in the manifest.

The `summary` and `release_url` fields in the published manifest are populated from the **latest GitHub release** at CI build time — so release notes appear to users in the HA firmware update entity the next time firmware is built and deployed.

## Idle page: key substitutions summary

| Substitution | Default | Purpose |
|---|---|---|
| `weather_entity` | `""` | `weather.*` entity for condition + temperature |
| `local_temp_entity` | `""` | Optional `sensor.*` to override displayed temperature |
| `weather_bg_path` | `""` | HA `/local/` path for condition background images |
| `idle_sensor_1..10` | `""` | Up to 10 arbitrary HA entities for sensor tiles (5 left + 5 right column) |
| `calendar_entity_1..3` | `""` | Up to 3 `calendar.*` entities for the agenda panel |
