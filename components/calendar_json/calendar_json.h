#pragma once

#include <algorithm>
#include <cstdlib>
#include <cstdio>
#include <string>

namespace esp32_dash {
namespace calendar_json {

/** Longest event title kept in the pipe-delimited buffer. Titles longer than
 * this are truncated explicitly so the line separator can never be lost. */
static constexpr size_t MAX_TITLE_LEN = 96;

inline void skip_ws(const std::string &json, size_t &pos) {
  while (pos < json.size()) {
    char c = json[pos];
    if (c != ' ' && c != '\n' && c != '\r' && c != '\t') {
      break;
    }
    pos++;
  }
}

/** Append a Unicode code point to out as UTF-8. */
inline void append_utf8(std::string *out, uint32_t cp) {
  if (out == nullptr) {
    return;
  }
  if (cp < 0x80) {
    out->push_back(static_cast<char>(cp));
  } else if (cp < 0x800) {
    out->push_back(static_cast<char>(0xC0 | (cp >> 6)));
    out->push_back(static_cast<char>(0x80 | (cp & 0x3F)));
  } else if (cp < 0x10000) {
    out->push_back(static_cast<char>(0xE0 | (cp >> 12)));
    out->push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
    out->push_back(static_cast<char>(0x80 | (cp & 0x3F)));
  } else {
    out->push_back(static_cast<char>(0xF0 | (cp >> 18)));
    out->push_back(static_cast<char>(0x80 | ((cp >> 12) & 0x3F)));
    out->push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
    out->push_back(static_cast<char>(0x80 | (cp & 0x3F)));
  }
}

/** Read exactly 4 hex digits at pos; returns false if malformed. */
inline bool parse_hex4(const std::string &json, size_t pos, uint32_t &out) {
  if (pos + 3 >= json.size()) {
    return false;
  }
  uint32_t v = 0;
  for (int i = 0; i < 4; i++) {
    char c = json[pos + i];
    v <<= 4;
    if (c >= '0' && c <= '9') {
      v |= static_cast<uint32_t>(c - '0');
    } else if (c >= 'a' && c <= 'f') {
      v |= static_cast<uint32_t>(c - 'a' + 10);
    } else if (c >= 'A' && c <= 'F') {
      v |= static_cast<uint32_t>(c - 'A' + 10);
    } else {
      return false;
    }
  }
  out = v;
  return true;
}

/**
 * Parse a JSON string literal starting at pos (which must point at the opening
 * quote). On success pos is left on the closing quote and out holds the decoded
 * value with escape sequences resolved (including \uXXXX surrogate pairs).
 */
inline bool parse_json_string(const std::string &json, size_t &pos, std::string *out) {
  if (pos >= json.size() || json[pos] != '"') {
    return false;
  }

  pos++;
  if (out != nullptr) {
    out->clear();
  }

  for (; pos < json.size(); pos++) {
    char c = json[pos];

    if (c == '"') {
      return true;
    }

    if (c != '\\') {
      if (out != nullptr) {
        out->push_back(c);
      }
      continue;
    }

    // Escape sequence
    pos++;
    if (pos >= json.size()) {
      break;
    }
    char e = json[pos];
    switch (e) {
      case 'n': if (out) out->push_back('\n'); break;
      case 't': if (out) out->push_back('\t'); break;
      case 'r': if (out) out->push_back('\r'); break;
      case 'b': if (out) out->push_back('\b'); break;
      case 'f': if (out) out->push_back('\f'); break;
      case '"': if (out) out->push_back('"'); break;
      case '\\': if (out) out->push_back('\\'); break;
      case '/': if (out) out->push_back('/'); break;
      case 'u': {
        uint32_t cp = 0;
        if (!parse_hex4(json, pos + 1, cp)) {
          // Malformed — emit the raw character and carry on.
          if (out) out->push_back(e);
          break;
        }
        pos += 4;
        // High surrogate: try to pair it with the following low surrogate.
        if (cp >= 0xD800 && cp <= 0xDBFF && pos + 6 < json.size() && json[pos + 1] == '\\' &&
            json[pos + 2] == 'u') {
          uint32_t lo = 0;
          if (parse_hex4(json, pos + 3, lo) && lo >= 0xDC00 && lo <= 0xDFFF) {
            cp = 0x10000 + ((cp - 0xD800) << 10) + (lo - 0xDC00);
            pos += 6;
          }
        }
        // Unpaired surrogates are not valid scalars; substitute U+FFFD.
        if (cp >= 0xD800 && cp <= 0xDFFF) {
          cp = 0xFFFD;
        }
        append_utf8(out, cp);
        break;
      }
      default:
        if (out) out->push_back(e);
        break;
    }
  }

  return false;
}

inline bool extract_object_span(const std::string &json, size_t start, size_t &end) {
  if (start >= json.size() || json[start] != '{') {
    return false;
  }

  int depth = 0;
  bool in_string = false;
  bool escape = false;
  for (size_t i = start; i < json.size(); i++) {
    char c = json[i];
    if (in_string) {
      if (escape) {
        escape = false;
      } else if (c == '\\') {
        escape = true;
      } else if (c == '"') {
        in_string = false;
      }
      continue;
    }

    if (c == '"') {
      in_string = true;
    } else if (c == '{') {
      depth++;
    } else if (c == '}') {
      depth--;
      if (depth == 0) {
        end = i;
        return true;
      }
    }
  }

  return false;
}

inline bool extract_array_span(const std::string &json, size_t start, size_t &end) {
  if (start >= json.size() || json[start] != '[') {
    return false;
  }

  int depth = 0;
  bool in_string = false;
  bool escape = false;
  for (size_t i = start; i < json.size(); i++) {
    char c = json[i];
    if (in_string) {
      if (escape) {
        escape = false;
      } else if (c == '\\') {
        escape = true;
      } else if (c == '"') {
        in_string = false;
      }
      continue;
    }

    if (c == '"') {
      in_string = true;
    } else if (c == '[') {
      depth++;
    } else if (c == ']') {
      depth--;
      if (depth == 0) {
        end = i;
        return true;
      }
    }
  }

  return false;
}

/**
 * Locate the value of a top-level (depth 1) key inside a JSON object.
 *
 * Only strings in *key position* are considered, i.e. those followed by ':'.
 * A string value that happens to equal the key name is skipped rather than
 * aborting the search. On success value_pos points at the first character of
 * the value.
 */
inline bool find_member_value(const std::string &obj, const char *key, size_t &value_pos) {
  int depth = 0;
  for (size_t i = 0; i < obj.size(); i++) {
    char c = obj[i];

    if (c == '"') {
      std::string token;
      size_t str_pos = i;
      if (!parse_json_string(obj, str_pos, &token)) {
        return false;
      }
      // str_pos now sits on the closing quote.
      size_t after = str_pos + 1;
      skip_ws(obj, after);
      bool is_key = (after < obj.size() && obj[after] == ':');
      i = str_pos;

      if (is_key && depth == 1 && token == key) {
        size_t pos = after + 1;  // skip ':'
        skip_ws(obj, pos);
        if (pos >= obj.size()) {
          return false;
        }
        value_pos = pos;
        return true;
      }
      continue;
    }

    if (c == '{' || c == '[') {
      depth++;
    } else if (c == '}' || c == ']') {
      depth--;
    }
  }

  return false;
}

inline bool extract_string_field(const std::string &obj, const char *key, std::string &value) {
  size_t pos = 0;
  if (!find_member_value(obj, key, pos)) {
    return false;
  }
  if (obj[pos] != '"') {
    return false;
  }
  return parse_json_string(obj, pos, &value);
}

inline bool extract_object_field(const std::string &obj, const char *key, std::string &value) {
  size_t pos = 0;
  if (!find_member_value(obj, key, pos)) {
    return false;
  }
  if (obj[pos] != '{') {
    return false;
  }
  size_t end = 0;
  if (!extract_object_span(obj, pos, end)) {
    return false;
  }
  value.assign(obj, pos, end - pos + 1);
  return true;
}

inline bool extract_array_field(const std::string &obj, const char *key, std::string &value) {
  size_t pos = 0;
  if (!find_member_value(obj, key, pos)) {
    return false;
  }
  if (obj[pos] != '[') {
    return false;
  }
  size_t end = 0;
  if (!extract_array_span(obj, pos, end)) {
    return false;
  }
  value.assign(obj, pos, end - pos + 1);
  return true;
}

inline bool extract_number_field(const std::string &obj, const char *key, float &value) {
  size_t pos = 0;
  if (!find_member_value(obj, key, pos)) {
    return false;
  }
  if (obj.compare(pos, 4, "null") == 0) {
    return false;
  }
  const char *start = obj.c_str() + pos;
  char *end = nullptr;
  float parsed = std::strtof(start, &end);
  if (end == start) {
    return false;
  }
  value = parsed;
  return true;
}

/** Replace characters that would corrupt the pipe/newline delimited buffer. */
inline void sanitize_field(std::string &s) {
  for (char &c : s) {
    if (c == '|' || c == '\n' || c == '\r') {
      c = ' ';
    }
  }
}

/**
 * Parse a Home Assistant calendar API response and append one line per event to
 * out_buf in the form:
 *
 *   CAL_IDX|TITLE|START|END|ALLDAY\n
 *
 * Lines are assembled with std::string so a long title can never truncate away
 * the trailing newline and merge two events together.
 */
inline int append_calendar_events(const std::string &body, int cal_idx, std::string &out_buf) {
  int appended = 0;

  for (size_t pos = 0; pos < body.size(); pos++) {
    if (body[pos] != '{') {
      continue;
    }

    size_t end = 0;
    if (!extract_object_span(body, pos, end)) {
      break;
    }

    std::string event(body, pos, end - pos + 1);
    pos = end;

    std::string title;
    if (!extract_string_field(event, "summary", title) || title.empty()) {
      continue;
    }

    sanitize_field(title);
    if (title.size() > MAX_TITLE_LEN) {
      title.resize(MAX_TITLE_LEN);
    }

    bool allday = false;
    std::string start_obj;
    std::string start_val;
    if (extract_object_field(event, "start", start_obj)) {
      if (!extract_string_field(start_obj, "dateTime", start_val)) {
        if (extract_string_field(start_obj, "date", start_val)) {
          allday = true;
        }
      }
    }
    if (start_val.empty()) {
      continue;
    }

    std::string end_obj;
    std::string end_val;
    if (!allday && extract_object_field(event, "end", end_obj)) {
      extract_string_field(end_obj, "dateTime", end_val);
    }

    if (start_val.size() > (allday ? 10U : 19U)) {
      start_val.resize(allday ? 10U : 19U);
    }
    if (end_val.size() > 19U) {
      end_val.resize(19U);
    }
    sanitize_field(start_val);
    sanitize_field(end_val);

    out_buf += std::to_string(cal_idx);
    out_buf += '|';
    out_buf += title;
    out_buf += '|';
    out_buf += start_val;
    out_buf += '|';
    out_buf += end_val;
    out_buf += '|';
    out_buf += (allday ? '1' : '0');
    out_buf += '\n';
    appended++;
  }

  return appended;
}

}  // namespace calendar_json
}  // namespace esp32_dash
