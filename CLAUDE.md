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
    lvgl.yaml           # all LVGL widget definitions (music_page + overlays)
    sensors.yaml        # template sensors, 1s playback interpolation
    media_player_select.yaml  # dynamic HA entity subscription
    linked_sensors.yaml # TV/linked player sensors
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

`addon/music.yaml` pulls `online_image` as an `external_component` from this same repo (`components/online_image/`). The firmware update manifest lives at `nnmalex.github.io/esp32-dash/firmware/esp32-dash-jc8012p4a1.manifest.json` (not yet published — `update_interval: never` until CI is set up).

## Architecture: LVGL state machine

All UI lives in `device/lvgl.yaml`. Currently a single page (`music_page`, 1280×800):

- **Left 800px** — album art panel (`album_art_background_widget`)
- **Right 480px** — track info (title, artist, time, play/pause button)
- **Bottom** — 6px progress bar
- **Swipe-down overlay** — volume arc / speaker group panel
- **Full-screen overlays** — clock screensaver, setup prompts, loading screen

Global state flags in `device/device.yaml`:
- `is_screen_dimmed`, `is_clock_screensaver_showing` — backlight stages
- `is_tv_mode` — switches to linked media player
- `setup_done`, `is_wifi_setup_done` — boot flow

Media state driven by `sensors.yaml`: subscribes to HA entity attributes, interpolates playback position every 1 second between HA updates.

## Roadmap: multi-view architecture

The project is being extended to support four views. `music_page` is the renamed original `main_page` — this rename is the only structural change made in Phase 1.

### Planned LVGL pages

```yaml
lvgl:
  pages:
    - id: music_page     # done — existing media player
    - id: idle_page      # Phase 3 — weather + calendar preview + HA sensors
    - id: calendar_page  # Phase 4 — full agenda
    - id: climate_page   # Phase 5 — climate controls + room sensors
```

### Navigation bar (Phase 2)

New file `device/navbar.yaml`, added to `packages.yaml`. A 60px bar at y=740..800, promoted to `lv_layer_top()` via lambda so it floats above all pages. Four icon buttons: Home, Music, Calendar, Climate.

### New global state (Phase 2 — in `device/navbar.yaml`)

```yaml
globals:
  - id: current_view    # 0=idle 1=music 2=calendar 3=climate
    type: int
    initial_value: '0'
```

### Auto-switching (Phase 2 — `device/sensors.yaml`)

`media_player_state_sensor` on_value:
- `"playing"` + `current_view == 0` → show `music_page`
- `"idle"/"off"/"standby"` → `delayed_idle_cleanup` runs → `current_view == 1` → show `idle_page`

Note: `"paused"` state stops `delayed_idle_cleanup` (player is paused, not stopped — stay on music page).
When player eventually goes idle/off, the cleanup runs and switches back to idle.

### Per-view packages (Phases 3–5)

| Phase | New file | Contents |
|---|---|---|
| 3 | `device/idle_view.yaml` | LVGL `idle_page`: weather widget, calendar preview, sensor row |
| 3 | `device/weather_sensors.yaml` | `text` entity `weather_entity`, HA subscriptions |
| 4 | `device/calendar_view.yaml` | LVGL `calendar_page`: agenda list |
| 4 | `device/calendar_sensors.yaml` | `text` entity `calendar_entity`, HA subscriptions |
| 5 | `device/climate_view.yaml` | LVGL `climate_page`: climate cards + room sensors |
| 5 | `device/climate_sensors.yaml` | `text` entities for 4× climate entities, HA subscriptions |

Each new `*_sensors.yaml` follows the pattern in `device/media_player_select.yaml`: a `text` entity persisted in NVS, a generation-counter subscription script, and template sensors that drive LVGL widget updates via `on_value`.
