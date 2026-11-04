[![Build firmware](https://github.com/nnmalex/esp32-dash/actions/workflows/firmware.yml/badge.svg)](https://github.com/nnmalex/esp32-dash/actions/workflows/firmware.yml)

# esp32-dash

Multi-view ESPHome kitchen dashboard for the **Guition ESP32-P4-JC8012P4A1** (10", 1280×800).

## Installation

In the ESPHome dashboard, create a new device and paste the following configuration:

```yaml
substitutions:
  name: "esp32-dash"
  friendly_name: "ESP32 Dash"

  # ── Network / HA connection ────────────────────────────────────────────────
  # Compile-time only. The token is needed for the calendar, the forecast and
  # the entity lists on the settings page.
  # ha_host: "homeassistant.local"  # change if HA is on a different host
  # ha_port: "8123"
  # ha_token: !secret ha_token      # long-lived access token

  # ── Settings page login (optional) ─────────────────────────────────────────
  # The settings page at http://<name>.local/ is open to your LAN unless set.
  # settings_username: "admin"
  # settings_password: !secret dash_settings_password

  # ── Display orientation ────────────────────────────────────────────────────
  # display_rotation: "270"         # 90 (default) or 270 (cable on left side)
  # Touch transform must match display_rotation:
  #   90:  touch_mirror_x "false", touch_mirror_y "false"  (defaults)
  #   270: touch_mirror_x "true",  touch_mirror_y "true"
  # touch_mirror_x: "true"
  # touch_mirror_y: "true"

  # ── Music screen ───────────────────────────────────────────────────────────
  # favourite_entity: "button.spotify_save_track"  # heart button (optional)

  # ── Initial values for the settings page (optional) ─────────────────────────
  # Everything below can be set on the settings page after flashing instead.
  # These only seed the first boot; values saved on the page take precedence.

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

  # ── Climate screen (up to 8 thermostats, 3 buttons each, + 6 at the bottom)
  # climate_entity_1: "climate.living_room"
  # climate_entity_1_button_1: "script.living_room_boost"
  # climate_entity_2: "climate.bedroom"
  # climate_button_1: "scene.movie_night"
  # climate_button_2: "switch.towel_rail"

  # ── NTP time server ────────────────────────────────────────────────────────
  # ntp_server: "time.cloudflare.com"   # default; change to your preferred NTP server

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

After flashing, add the device in Home Assistant (**Settings → Devices → ESPHome**), then open the device's settings page to pick your media player and everything else.

## Settings page

The panel serves its own settings page at `http://<name>.local/` (`http://esp32-dash.local/` for the snippet above; the device's IP address works too). The panel shows the address on its "Media Player" setup screen.

Settings are no longer exposed to Home Assistant as device configuration entities, so HA's device page only shows the panel's status sensors and the Accent Color light.

- **Entity fields** (media player, weather, sensor tiles, calendars, timers, thermostats, climate buttons) suggest the entities your Home Assistant has: click into one to list everything that fits the field, and type any part of the id or friendly name to narrow it. The panel fetches this list from HA when the page opens, using `ha_token`. Without a token you can still type entity ids in. If HA's list arrives incomplete, the page says so instead of showing a partial list; press **Refresh** to retry.
- **Other settings** (brightness, night hours, timezone, playback options, timeouts) are regular inputs.
- Changes save as you make them and apply immediately. They are stored on the panel and survive reboots and firmware updates.

Set `settings_username` / `settings_password` to make the browser ask for a login. This is HTTP basic authentication over plain HTTP: it keeps casual visitors on your network out, but it isn't encryption.

## Music screen setup

The music screen shows album art, track info, and playback controls (previous / play-pause / next).

### Favourite button

A heart button can optionally be shown on the music screen to trigger a HA action (e.g. save the current track to a playlist). It is **hidden by default** — uncomment `favourite_entity` in your config to enable it. When set, pressing the heart button calls `button.press` on that entity.

## Home screen setup

The home (idle) screen has a left panel with a weather background and a right panel with a calendar agenda.

### Weather & calendar entities

Set these on the device's [settings page](#settings-page) (or seed them with substitutions):

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

## Climate screen

The fifth tab (thermostat icon) shows a card for each configured thermostat, plus an optional row of buttons along the bottom. Set them on the [settings page](#settings-page): each thermostat has its own section (**Climate: thermostat 1** … **8**) with up to three buttons for its card, and **Climate: bottom buttons** holds the bottom row. They can also be seeded with the `climate_entity_N`, `climate_entity_N_button_M` and `climate_button_N` substitutions.

Only the thermostats and buttons you set are shown, and they share the space by how many there are:

- 1–4 thermostats sit side by side in one row of tall cards.
- 5–8 thermostats use two rows of smaller cards (5 → 3 + 2, 7 → 4 + 3, 8 → 4 + 4); a shorter second row stretches to the full width. The smaller cards show the current temperature next to the name and leave out the status line.
- The bottom row is hidden when no bottom button is set, and its buttons share the width.

Each thermostat card shows, all read from Home Assistant:

- the name, what it is doing now (Heating, Idle, Off…) and the active preset
- the current temperature
- the target temperature with **−** / **+** (in the thermostat's own step, within its min/max). Taps are collected and sent once you stop tapping; the value shows blue until HA confirms it. A thermostat in heat/cool mode shows its range instead.
- a **power** button (when the thermostat has an *off* mode; turning it back on restores the last mode) and a button for each HVAC mode it supports (heat, cool, auto…)
- a button for each preset it supports (Eco, Boost…). Tapping the active preset returns to *none*, so Boost works as an on/off toggle.
- up to three buttons of your own for that thermostat (e.g. a boost script, a towel-rail switch)

A card whose controls do not fit (many modes and presets on a small card) scrolls.

Buttons (on a card or in the bottom row) act by the entity's domain: `button` / `input_button` are pressed, `scene` / `script` run, `automation` is triggered, `lock` locks or unlocks, and anything else (light, switch, fan, cover…) toggles. Buttons for entities that are on are highlighted. The icon is the entity's own icon when it is one the panel knows, otherwise one for its domain.

Thermostat and button taps are Home Assistant actions, like the music controls, so **Allow the device to perform Home Assistant actions** must be enabled in the device's options in HA.

## Firmware updates

There is no automatic update check. Update by reinstalling from the ESPHome dashboard, which pulls the latest `main` (see `refresh: 1s` in the snippet above).

## Calendar week view

The calendar screen shows a 5-day grid with events from all three configured calendar entities. The view defaults to 6 AM–9 PM and can be scrolled up/down; nav arrows shift the window one day back or up to two days forward.

Events are fetched from the HA REST API, which requires a long-lived access token.

### Getting a token

1. In HA, go to your **Profile → Security → Long-lived access tokens** and create a token.

2. Set it with the `ha_token` substitution in your config (`ha_token: !secret ha_token`, with the token in `secrets.yaml`) and reinstall.

The same token lets the settings page list your Home Assistant entities.
