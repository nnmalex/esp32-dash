#pragma once

#include <string>

namespace esphome {
namespace online_image {

/// Rewrite known CDN artwork URLs to cap the requested resolution.
///
/// Apple Music embeds resolution in the filename (e.g. /3000x3000bb.jpg).
/// Requesting the full-res image can exceed ESP32 heap and cause OOM.
/// This rewrites the dimension component to at most max_dim (default 600).
///
/// URLs that don't match a known pattern are returned unchanged.
inline std::string cap_artwork_url(const std::string &url, int max_dim = 600) {
  // Apple Music CDN: ...mzstatic.com/.../3000x3000bb.jpg
  // Pattern: /{W}x{H}bb.{ext} at the end of the URL path
  auto bb_pos = url.rfind("bb.");
  if (bb_pos == std::string::npos)
    return url;

  // Walk backwards from bb_pos to find the 'x' separator and leading '/'
  auto x_pos = url.rfind('x', bb_pos);
  if (x_pos == std::string::npos || x_pos == 0)
    return url;

  auto slash_pos = url.rfind('/', x_pos);
  if (slash_pos == std::string::npos)
    return url;

  // Extract width and height strings between '/' and 'x', and 'x' and 'bb.'
  std::string w_str = url.substr(slash_pos + 1, x_pos - slash_pos - 1);
  std::string h_str = url.substr(x_pos + 1, bb_pos - x_pos - 1);

  // Validate that both are numeric
  if (w_str.empty() || h_str.empty())
    return url;
  for (char c : w_str) {
    if (c < '0' || c > '9') return url;
  }
  for (char c : h_str) {
    if (c < '0' || c > '9') return url;
  }

  int w = std::stoi(w_str);
  int h = std::stoi(h_str);

  if (w <= max_dim && h <= max_dim)
    return url;

  // Rebuild with capped dimensions
  std::string dim = std::to_string(max_dim) + "x" + std::to_string(max_dim);
  return url.substr(0, slash_pos + 1) + dim + url.substr(bb_pos);
}

}  // namespace online_image
}  // namespace esphome
