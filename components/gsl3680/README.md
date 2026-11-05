# gsl3680 (vendored)

GSL3680 touchscreen driver for the Guition ESP32-P4-JC8012P4A1.

## Provenance

Vendored from [kvj/esphome](https://github.com/kvj/esphome), branch
`jd9365_gsl3680`, commit `dca6f3e` (2025-10-16, "gsl3680: Migrate to new i2c
driver"). Licensed under the ESPHome license (see the upstream `LICENSE`);
copyright remains with the original authors.

This component is not part of upstream ESPHome.

## Why it is vendored

It used to be pulled directly:

```yaml
external_components:
  - source: github://kvj/esphome@jd9365_gsl3680
    components: [gsl3680]
```

That broke on ESPHome 2026.6.0. `Component::mark_failed(const char *)` was
deprecated in 2025.12 ("Remove before 2026.6.0") and duly removed, leaving only
`mark_failed()` and `mark_failed(const LogString *)`. `gsl3680.cpp` still called
the `const char *` form, so the build failed with:

```
error: no matching function for call to 'GSL3680::mark_failed(const char [15])'
```

The upstream branch has had no commits since 2025-10-16, so waiting on a fix
there was not viable. Vendoring also removes a second problem: the old reference
tracked a *branch*, so upstream could change the driver under us at any time
with no pin.

## Local changes vs upstream

Two changes. First, one line in `gsl3680.cpp`:

```diff
-        this->mark_failed("I2C init error");
+        this->mark_failed(LOG_STR("I2C init error"));
```

`mark_failed(const LogString *)` exists in 2026.4 and later, so this form works
across every ESPHome version this project supports.

Second, `gsl_config_data_id` (`gsl3680_firmware.h`) is `const`, and
`gsl_DataInit` / `ConfigCoorMulti` / `ConfigFilter` / `ConfigKeyMap`
(`gsl_point_id.*`) take `const unsigned int *`. They only read the table; as a
non-const array it sat in internal RAM (2 KB of `.data`) in the region ESP-IDF
needs free at power-up on pre-v3 P4 silicon (see CLAUDE.md, Gotchas).

## Re-syncing from upstream

If kvj's branch gains fixes worth taking, diff against it and re-apply the change
above: `gsl3680.cpp` carries a comment marking the first, and the second is the
`const` on the table and those four signatures. Everything else is untouched.
