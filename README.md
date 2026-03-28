# esp32-dash

Multi-view ESPHome kitchen dashboard for the **Guition ESP32-P4-JC8012P4A1** (10", 1280×800).

## Installation

In the ESPHome dashboard, create a new device and paste the following configuration:

```yaml
substitutions:
  name: "esp32-dash"
  friendly_name: "ESP32 Dash"
  # ha_host: "homeassistant.local"  # change if HA is on a different host
  # ha_port: "8123"
  # Display rotation: 90 (default landscape) or 270 (flipped landscape)
  # display_rotation: "270"
  # Touch transform (must match display_rotation):
  #   90:  touch_mirror_x "false", touch_mirror_y "false"
  #   270: touch_mirror_x "true",  touch_mirror_y "true"
  # touch_mirror_x: "true"
  # touch_mirror_y: "true"

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

## Home screen setup

The home (idle) screen has a left panel with a weather background and a right panel with a calendar agenda.

### Weather & calendar entities

Set these from the device's settings page in HA (or via substitutions):

| Setting | Description |
|---|---|
| **Weather Entity** | A `weather.*` entity (e.g. `weather.openweathermap`) |
| **Local Temperature Sensor** | Optional `sensor.*` to override the displayed temperature |
| **Calendar 1/2/3 Entity** | Up to three `calendar.*` entities — events are merged and sorted on the home screen |
| **Sensor Tile Slot 1–4** | Any HA entity; its state is shown as a tile on the home screen. Tiles are hidden when no entity is set. |

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

3. Add the path to your ESPHome configuration:
   ```yaml
   substitutions:
     weather_bg_path: "/local/weather-backgrounds"
   ```

Leave `weather_bg_path` unset (default) to skip background images and show a plain dark panel instead.
