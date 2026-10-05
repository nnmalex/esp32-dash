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
    time.yaml           # HA time sync
    timezone.yaml       # clock timezone select
  assets/
    fonts.yaml          # Roboto variants (19–100px)
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
    timer_overlay.yaml  # timer_bar, shown in the right 480px of the nav bar
  theme/
    button.yaml         # LVGL style definitions
components/
  online_image/         # custom C++ component: downloads & decodes album art / weather bg.
                        # JPEG: P4 hardware decoder, libjpeg-turbo fallback; one shared
                        # bilinear resampler (ImageDecoder::resample) for JPEG and PNG
  calendar_json/        # shared C++ for calendar + forecast: JSON parser, event-line and
                        # date helpers (calendar_json.h), weather_icons.h, and
                        # http_worker.* (background HTTP, see "Background HTTP")
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
does not exist before 2026.7 — older versions fail validation. CI builds a
matrix: the minimum (`>=2026.7.0,<2026.8`) and the latest release, unpinned.
Testing only one pinned version has missed breaks twice: `<2026.5` let a 2026.6+
break through, and `<2026.8` let 2026.9's removal of `set_timezone()` reach
users (who run whatever the HA add-on ships). To check locally against a newer
release, `pip install esphome==<version>` into a venv under `builds/.esphome/`
and compile the same file.

**`api: homeassistant_states: true` is load-bearing** (set in `device/device.yaml`).
It emits `USE_API_HOMEASSISTANT_STATES`, which is what compiles
`APIServer::subscribe_home_assistant_state` into the binary. The option defaults to
`false`; ESPHome only turns it on automatically for `platform: homeassistant`
entities, and it cannot see the subscriptions this project makes from raw lambdas
(`media_player_select.yaml`, `weather_sensors.yaml`, `calendar_sensors.yaml`,
`timer_overlay.yaml`). Removing it breaks the compile with "class
`esphome::api::APIServer` has no member named `subscribe_home_assistant_state`" in
every one of those files. It was previously satisfied by accident: the
speaker-grouping addon's `platform: homeassistant` text_sensor set the define, and
deleting that addon surfaced the latent dependency.

**Local build verification:** `esphome` is not on `PATH`, but the ESPHome Device
Builder app bundles a matching 2026.7.3 CLI:
`"/Applications/ESPHome Device Builder.app/Contents/Resources/python/bin/esphome" compile builds/guition-esp32-p4-jc8012p4a1.yaml`
— same command CI runs, and it needs no `secrets.yaml` (unlike `dev.yaml`, which
wants `wifi_ssid` / `wifi_password`).

**There is currently no OTA update-check feature.** `.github/workflows/firmware.yml` only compiles `builds/guition-esp32-p4-jc8012p4a1.yaml` and validates the factory build (`.factory.yaml`, which adds only `dashboard_import`; it has no ESP32-C6 coprocessor firmware update — the `esp32_hosted` update and its `network_adapter_esp32c6.bin` blob, never committed, were dropped) — it does not publish a manifest, upload an OTA binary, or inject a version. `project.version` is hardcoded to `dev` in both build files. Devices therefore expose no `update` entity for dashboard firmware, and users update by re-flashing or via the ESPHome dashboard. See "Not implemented" below.

## Architecture: LVGL state machine

Four LVGL pages defined across two files (`device/lvgl.yaml` + `device/navbar.yaml`):

- **`music_page`** (1280×800) — existing media player UI
  - Left 800px: album art panel (`album_art_background_widget`). `online_image`
    decodes art straight to the 740×740 box (`resize: 740x740`) and the widget
    draws it 1:1, centred. Baseline JPEGs decode on the P4's hardware JPEG codec
    (`decode_hw_`, RGB565 little-endian, MCU-padded stride); progressive or
    greyscale JPEGs, sources over 2048², or any driver error fall back to
    libjpeg-turbo. Both, and PNGs up to 1600² (buffered as RGB888, resampled in
    pngle's done callback), go through `ImageDecoder::resample` (bilinear,
    streamed, handles `fit: cover` crops). Do not reintroduce
    an LVGL zoom/scale on it: a transformed image is re-rendered on every redraw,
    and the progress bar over it redraws every second
  - Right 480px: track info (title, artist, time, play/pause button)
  - Bottom: 6px progress bar
  - Full-screen overlays: setup prompts, loading screen
  - No volume or speaker-grouping UI: the swipe-down settings panel, the volume
    arc, and `addon/speaker_group.yaml` were removed. Volume is controlled from
    HA, not the panel.
- **`idle_page`** (Phase 4b complete) — two-pane layout
  - Left 800px: weather background image (`fit: cover`, darkened in its pixels on download — there is no overlay object); clock+date top-left (refreshed by `ha_time` `on_time: seconds: 0`, not a free-running interval); condition+temp top-right (condition ids mapped to display names); two columns of 5 sensor tiles each (left col x=0..389 = tiles 0-4, right col x=400..789 = tiles 5-9; hidden when entity not configured, visible ones restacked from the top by `pack_idle_tiles`). Tile icons come from the entity's `icon` attribute, else its `device_class`; `unavailable`/`unknown` show a dimmed "—"; the k-prefix applies only to SI units (W, Wh, V, A, Hz, g, m…)
  - Right 480px: merged calendar agenda (today+tomorrow from all 3 calendars, sorted) — 5 agenda slots (`idle_agenda_slot_0..4`) plus an `idle_agenda_empty` state. Includes events still running from earlier days and multi-day all-day events. Ended timed events stay greyed out for the "Agenda: Past Event Lookback" number (`agenda_lookback`, default 120 min), then drop off
- **`calendar_page`** (Phase 4c complete) — 5-day week grid view
  - Header (y=0..60): 5 day-column labels (day name + date, month on the first column and on the 1st), highlighted today, chevron prev/next nav (offset -1..+2). Entering the view resets to today and scrolls the current time ~1/3 down (`scroll_calendar_to_now`); paging keeps the scroll position
  - All-day strip (y=60..84): one chip per column; multi-day all-day events appear in every column they cover; "+N" when a column has more than fits (extra all-day events or more than 6 timed)
  - Time grid (y=84..740, scrollable 656px): 44px/hour; 30 pre-allocated event blocks (6 per column) labelled with their start time, dimmed once ended, split at midnight when they span days; current-time red indicator. Hour lines/labels and column separators are painted by an `LV_EVENT_DRAW_MAIN_END` callback on `cal_grid_scroll` (registered in `lvgl: on_boot`), with one invisible spacer holding the 24 h scroll extent
  - Data: `fetch_calendar_data` (see "Background HTTP") fills the shared `cal_events_buf`, one line per event: `CAL_IDX|TITLE|START|END|ALLDAY|LOCATION|DESCRIPTION` (all-day END is the exclusive date; location/description capped at 120/320 bytes, UTF-8-safe). Parse lines with `calendar_json::parse_event_line`, not ad-hoc splitting — fields were appended after ALLDAY. Requires the `ha_token` substitution
  - Event details: tapping a grid block, an all-day chip or an idle agenda item runs `show_event_details` with that widget's buffer line (`cal_ev_line` / `cal_ad_line` / `agenda_slot_line`, recorded at render time; taps hooked up in `calendar_view.yaml`'s `lvgl: on_boot`). The card (`ev_detail_overlay`, on `lv_layer_top()`) shows title, `format_event_when()` ("Mon 6 – Wed 8 Oct · All day"), calendar, location, description; tap or 60 s to close
  - Day boundaries use `esp32_dash::calendar_json::local_day/local_date` (mktime-based), never `timestamp + n*86400`, which is an hour off across DST
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

Navigation bar (`nav_bar`) defined in `device/navbar.yaml`, reparented to `lv_layer_top()` on boot so it floats above all pages. 60px bar at y=740, visible on all four views (every `show_*_view` script reveals it; it starts hidden only so it does not flash during boot/setup), four icon buttons: Home, Music, Calendar, Forecast. `update_nav_highlight` (run by every `show_*_view`) brightens the current view's icon and shows its accent mark (`nav_*_mark`); the others are dimmed. `layout_nav_bar` narrows the buttons to 4×200px in the left 800px while `timer_bar` is visible, and restores 4×320px otherwise.

Timer overlay (`timer_bar`) defined in `device/timer_overlay.yaml`, reparented into `nav_bar` on boot and occupying its right 480px (x=800). It sits inside the nav bar rather than floating above it so it never covers page content; it is hidden when no timers are active. Shows soonest-expiring active timer name + MM:SS countdown + "+N" badge. Tapping dismisses until next HA `remaining` update. Subscribes to up to 3 `timer.*` entities (compile-time substitutions `timer_entity_1..3` or runtime via HA device settings). Remaining time is computed from the `finishes_at` attribute against the clock (`tmr_finishes_0..2`), so a stalled main loop or a reconnect does not make it drift; a local 1 s decrement is only the fallback before time sync. Time label turns red when < 60 s remaining. A paused timer is shown (greyed, "· Paused") when none is running. When a timer goes active → idle within a few seconds of its `finishes_at`, `show_timer_done` raises `tmr_done_overlay` (full-screen card on `lv_layer_top()`, wakes the backlight, tap or 10 min to dismiss); a cancel, or a reconnect long after it ended, does not.

Global state flags in `device/device.yaml`:
- `actions_prompt_acked` — user dismissed the "enable actions" prompt (NVS-backed)
- `device_has_been_setup` — first successful setup completed; suppresses setup prompts on restart (NVS-backed)
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
Phase 2 (complete): navbar, `current_view` global, auto-switching, swipe gestures scoped per view (horizontal swipes skip tracks, music-page only). The swipe-down settings panel added in this phase has since been removed.
Phase 3 (complete): `device/idle_view.yaml` + `device/weather_sensors.yaml` — real idle page with clock, weather card, calendar preview placeholders, 4-tile sensor row; weather entity + 4 sensor row entities (NVS-persisted, gen-counter subscriptions).
Phase 4 (complete): `device/calendar_view.yaml` + `device/calendar_sensors.yaml` — real calendar page; 3 calendar entity slots.
Phase 4c (complete): Calendar view redesigned as 5-day week grid. Key IDs: `cal_grid_scroll` (scrollable time grid), `cal_col_hdr_0..4`, `cal_ev_00..29` (event blocks), `cal_ad_0..4` (all-day chips), `cal_now_line` (current time). New globals: `cal_view_offset`, `cal_events_buf`, `cal_fetch_start/end`, `cal_last_fetch_ms`. New scripts: `fetch_calendar_data`, `render_calendar_grid`, `position_cal_now_line`. Token set via the `ha_token` substitution.
Phase 4b (complete): Idle view redesign — two-pane layout (800px left + 480px right). Left pane: weather background image (online_image, loaded from HA `/local/` path by condition name), darkened, clock+date top-left, weather condition+temperature top-right, two columns of 5 sensor tiles each (left col x=0..389 = tiles 0-4, right col x=400..789 = tiles 5-9; hidden when entity not configured). Right pane: merged calendar agenda (today+tomorrow from all 3 calendars, sorted, ended events greyed out within a look-back window). New substitutions: `weather_bg_path`, `local_temp_entity`, `idle_sensor_1..10`. Key new IDs: `idle_weather_bg_image` (online_image in weather_sensors.yaml), `idle_sensor_tile_0..9`, `idle_agenda_slot_0..4`, `render_idle_agenda` script.

### Phase 5 (complete): forecast view

| Phase | New file | Contents |
|---|---|---|
| 5 | `device/forecast_view.yaml` | LVGL `forecast_page`: 7-day chart + precipitation + condition icons |
| 5 | `device/forecast_sensors.yaml` | `wf_data_buf` global, `fetch_forecast` (HTTP POST), 30-min interval (not on music view) |
| 6 | `device/timer_overlay.yaml` | `timer_bar` in the right of the nav bar; subscribes to up to 3 `timer.*` entities |

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
   Recommended size: 800×740 px JPEG. The `online_image` component decodes and scales the image on-device, cropping other aspect ratios to fill the pane (`fit: cover`).

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
  horizontal swipes (track skip) only. Note `touch.x/y` in the `touchscreen`
  callbacks are panel-native portrait; the handler maps them through
  `id(lvgl_main)->rotate_coordinates()` before comparing directions. (Before
  that fix, the "horizontal" track-skip swipe fired on vertical swipes.)
- **On-device volume control and speaker grouping.** The swipe-down settings
  panel, the volume arc, and `addon/speaker_group.yaml` (which needed a
  `sensor.speaker_group` template sensor in HA) were removed. Nothing subscribes
  to `volume_level` or `group_members` any more, and the "Speakers: Auto-Close
  Timeout" number entity is gone.

## Background HTTP: calendar and forecast fetches

ESPHome's `http_request` is synchronous — `->get()` / `->post()` and the body
drain run inline on the main loop and stall LVGL and touch for the whole
request, which for a calendar fetch can be seconds while HA waits on the
upstream provider. So the calendar and forecast fetches do **not** use it.
They go through `esp32_dash::HttpWorker` (`components/calendar_json/http_worker.*`,
enabled by `calendar_json: fetcher: id: http_worker` in `device/device.yaml`):

- `id(http_worker)->request(url, body, headers, callback)` queues a GET (empty
  body) or POST. A dedicated FreeRTOS task runs it with `esp_http_client`; the
  finished job comes back through a queue that `HttpWorker::loop()` drains, so
  **callbacks run on the main loop** and may touch LVGL, globals and scripts.
  The worker task touches nothing but its job and the IDF client — keep it that
  way (no `id()`, no LVGL, no logging from `run_()`).
- TLS follows `http_request`'s `verify_ssl`, which also sets the shared sdkconfig
  (certificate bundle / insecure mode), so keep the two `verify_ssl` values in
  step (both use `${ha_verify_ssl}`). `online_image` still uses `http_request`;
  it downloads in chunks across loop iterations and was never the problem.
- It lives in `calendar_json` rather than its own component because that
  component already ships from `main`: a brand-new component would fail CI and
  end-user builds until merged (see `addon/music.yaml`'s note on refs).
- `fetch_calendar_data`: one batched
  `POST /api/services/calendar/get_events?return_response=true` for every
  configured calendar (HA ≥ 2024.2). If HA rejects the batch (one unknown entity
  fails the whole call) it falls back to one `GET /api/calendars/<entity>` per
  calendar, all in flight at once, merged when the last answers. The cache is
  only replaced when something succeeded. `cal_fetch_in_flight` drops overlapping
  calls. Window is day −1..+7, so calendar paging never refetches.
- `fetch_forecast`: one `POST /api/services/weather/get_forecasts`; parsing and
  rendering run in the callback. `wf_fetch_in_flight` guards overlap.
- Both refresh on every view (15 min calendar, 30 min forecast) and on API
  connect; interactive callers go through `maybe_fetch_calendar` /
  `maybe_fetch_forecast`, which skip the request while the cache is fresh
  (5 / 15 min).

### Alternative not taken: HA push

HA could push the same payloads via template-sensor attributes over the native
API instead of the device polling. It was rejected in favour of the worker
because it needs extra HA configuration from every user; the worker keeps the
zero-config `ha_token` setup.

## Gotchas

- **Template `number` `set_action` runs before the value is stored.**
  `TemplateNumber::control()` fires the trigger and only then `publish_state()`s,
  so a script called from `set_action` that reads `id(x).state` sees the
  *previous* value. Every such `set_action` here starts with
  `- lambda: 'id(x)->publish_state(x);'`. Do not switch to `on_value` instead:
  template numbers set up (`HARDWARE` priority) before LVGL, so the restore
  publish at boot would run LVGL code against uncreated widgets.
- **Bundled IDF libraries must be declared, not copied.** `online_image` adds
  `components/libjpeg-turbo-esp32` with `esp32.add_idf_component(path=...)`, so
  it lands in `src/idf_component.yml` and in `src`'s REQUIRES. It used to be
  copied into `<build>/components` and left to ESPHome's component discovery,
  whose list comes from the *previous* configure — a build dir configured
  before the copy existed then failed with "jpeglib.h: No such file or
  directory" (seen after PR #50 forced a reconfigure). Reproduce by deleting
  the entry from `build/project_description.json` and compiling.
- **Timezone API differs across ESPHome versions.** 2026.9 removed
  `RealTimeClock::set_timezone(string)`: the zone is a pre-parsed
  `time::ParsedTimezone` set with `time::set_global_tz()`, and ESPHome replaces
  libc `localtime()`/`localtime_r()` to read it, so `setenv("TZ")` alone no
  longer changes local time (libc `mktime()` still reads `TZ`). `apply_timezone`
  in `addon/timezone.yaml` branches on `ESPHOME_VERSION_CODE`; its table carries
  each zone pre-parsed by `aioesphomeapi.posix_tz.parse_posix_tz` — regenerate
  those columns with it when adding a zone. `ha_time` keeps `timezone: UTC`
  explicitly: without it 2026.9 defines `USE_HOMEASSISTANT_TIMEZONE` and HA's
  zone overwrites the selector after every time sync.
- **Check MDI codepoints against the font, not memory.** Several glyphs were
  wrong for a long time (pressure showed `format-wrap-tight`, "windy-variant" a
  globe). Verify with the 7.4.47 CSS
  (`cdn.jsdelivr.net/npm/@mdi/font@7.4.47/css/materialdesignicons.css`), and add
  any glyph used from a lambda to the font's `glyphs:` list.

## Idle page: key substitutions summary

| Substitution | Default | Purpose |
|---|---|---|
| `weather_entity` | `""` | `weather.*` entity for condition + temperature |
| `local_temp_entity` | `""` | Optional `sensor.*` to override displayed temperature |
| `weather_bg_path` | `""` | HA `/local/` path for condition background images |
| `idle_sensor_1..10` | `""` | Up to 10 arbitrary HA entities for sensor tiles (5 left + 5 right column) |
| `calendar_entity_1..3` | `""` | Up to 3 `calendar.*` entities for the agenda panel |
