#pragma once

#include <cstring>

namespace esp32_dash {
namespace weather {

/** MDI glyph (UTF-8, from icon_font) for a Home Assistant weather condition id.
 * Shared by the forecast page and the idle weather card. Every glyph returned
 * here must be in icon_font's `glyphs:` list (assets/icons.yaml). */
inline const char *condition_icon(const char *c) {
  static const char *const MAP[][2] = {
      {"sunny", "\U000F0599"},         {"clear-day", "\U000F0599"},
      {"clear-night", "\U000F0594"},   {"partlycloudy", "\U000F0595"},
      {"cloudy", "\U000F0590"},        {"fog", "\U000F0591"},
      {"hail", "\U000F0592"},          {"lightning", "\U000F0593"},
      {"lightning-rainy", "\U000F067E"}, {"rainy", "\U000F0597"},
      {"drizzle", "\U000F0597"},       {"pouring", "\U000F0596"},
      {"snowy", "\U000F0598"},         {"snowy-rainy", "\U000F067F"},
      {"windy", "\U000F059D"},         {"windy-variant", "\U000F059E"},
  };
  for (auto &m : MAP)
    if (strcmp(c, m[0]) == 0)
      return m[1];
  return "\U000F0590";  // cloudy fallback (covers "exceptional")
}

}  // namespace weather
}  // namespace esp32_dash
