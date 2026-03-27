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
