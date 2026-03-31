# esp32-dash

Multi-view ESPHome kitchen dashboard for the **Guition ESP32-P4-JC8012P4A1** (10", 1280×800).

## Installation

In the ESPHome dashboard, create a new device and paste the following configuration:

```yaml
substitutions:
  name: "esp32-dash"
  friendly_name: "ESP32 Dash"

  # ── Network / HA connection ────────────────────────────────────────────────
  # ha_host, ha_port, ha_protocol, and ha_token are configurable at runtime
  # via the HA device settings page — no substitutions or recompile needed.
  # Uncomment below only if you want to set initial values at compile time:
  # ha_host: "homeassistant.local"  # change if HA is on a different host
  # ha_port: "8123"
  # ha_token: !secret ha_token      # long-lived token — or set via HA device settings

  # ── Display orientation ────────────────────────────────────────────────────
  # display_rotation: "270"         # 90 (default) or 270 (cable on left side)
  # Touch transform must match display_rotation:
  #   90:  touch_mirror_x "false", touch_mirror_y "false"  (defaults)
  #   270: touch_mirror_x "true",  touch_mirror_y "true"
  # touch_mirror_x: "true"
  # touch_mirror_y: "true"

  # ── Music screen ───────────────────────────────────────────────────────────
  # favourite_entity: "button.spotify_save_track"  # heart button (optional)

  # ── Home screen: weather ───────────────────────────────────────────────────
  # weather_entity: "weather.openweathermap"
  # local_temp_entity: "sensor.indoor_temperature"  # override displayed temp
  # weather_bg_path: "/local/weather-backgrounds"   # condition background JPEGs

  # ── Home screen: sensor tiles (up to 10) ──────────────────────────────────
  # idle_sensor_1: "sensor.living_room_temperature"
  # idle_sensor_2: "sensor.living_room_humidity"
  # idle_sensor_3: "sensor.power_usage"
  # idle_sensor_4: "binary_sensor.front_door"
  # idle_sensor_5: ""
  # idle_sensor_6: ""
  # idle_sensor_7: ""
  # idle_sensor_8: ""
  # idle_sensor_9: ""
  # idle_sensor_10: ""

  # ── Calendar (home screen agenda + week view) ──────────────────────────────
  # calendar_entity_1: "calendar.my_calendar"
  # calendar_entity_2: "calendar.family"
  # calendar_entity_3: "calendar.work"

wifi:
  ssid: !secret wifi_ssid
  password: !secret wifi_password

packages:
  esp32_dash:
    url: https://github.com/nnmalex/esp32-dash
    files: [guition-esp32-p4-jc8012p4a1/packages.yaml]
    ref: main
    refresh: 1s
```

After flashing, add the device in Home Assistant and set the **Media Player** entity from the device settings page.

## Music screen setup

The music screen shows album art, track info, and playback controls (previous / play-pause / next).

### Favourite button

A heart button can optionally be shown on the music screen to trigger a HA action (e.g. save the current track to a playlist). It is **hidden by default** — uncomment `favourite_entity` in your config to enable it. When set, pressing the heart button calls `button.press` on that entity.

## Home screen setup

The home (idle) screen has a left panel with a weather background and a right panel with a calendar agenda.

### Weather & calendar entities

Set these from the device's settings page in HA (or via substitutions):

| Setting | Description |
|---|---|
| **Weather Entity** | A `weather.*` entity (e.g. `weather.openweathermap`) |
| **Local Temperature Sensor** | Optional `sensor.*` to override the displayed temperature |
| **Calendar 1/2/3 Entity** | Up to three `calendar.*` entities — events are merged and sorted on the home screen |
| **Sensor Tile Slots 1–10** | Any HA entity; its state is shown as a tile on the home screen. Tiles are hidden when no entity is set. |

### Weather background images

The left panel displays a JPEG image that matches the current weather condition. To enable:

1. Create a folder on your HA server: `<config>/www/weather-backgrounds/`

2. Add JPEG images named after OpenWeatherMap condition strings:
   ```
   clear-night.jpg   cloudy.jpg     exceptional.jpg   fog.jpg
   hail.jpg          lightning.jpg  lightning-rainy.jpg
   partlycloudy.jpg  pouring.jpg    rainy.jpg
   snowy.jpg         snowy-rainy.jpg  sunny.jpg
   windy.jpg         windy-variant.jpg
   ```
   Recommended resolution: **800 × 740 px**.

3. Uncomment `weather_bg_path` in your ESPHome config (see the installation snippet above).

Leave `weather_bg_path` commented out (default) to skip background images and show a plain dark panel instead.

## Firmware updates

The dashboard checks for updates automatically. Controls are on the HA device settings page:

| Setting | Default | Description |
|---|---|---|
| **Auto Update** | On | Automatically install when a new version is found |
| **Update Frequency** | Daily | How often to check — Hourly, Daily, or Weekly |
| **Install Latest Firmware** | — | Button to trigger an immediate check and install |

Updates are fetched from GitHub Pages (`nnmalex.github.io/esp32-dash/firmware/`) and built from the `main` branch on every push. A new version is published automatically — no action needed on your part.

## Calendar week view

The calendar screen shows a 5-day grid with events from all three configured calendar entities. The view defaults to 6 AM–9 PM and can be scrolled up/down; nav arrows shift the window one day back or up to two days forward.

Events are fetched from the HA REST API, which requires a long-lived access token.

### Getting a token

1. In HA, go to your **Profile → Security → Long-lived access tokens** and create a token.

2. On the HA device settings page for ESP32 Dash, paste the token into the **HA Token** field.

The token is stored in device flash (NVS) and takes effect immediately on the next fetch — no recompile needed. Alternatively, set it at compile time via the `ha_token` substitution in your config (`ha_token: !secret ha_token` in `secrets.yaml`).
