#pragma once

#include <algorithm>
#include <cstdint>
#include <cstdlib>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <string>

namespace esp32_dash {
namespace calendar_json {

/** Local calendar day `day_offset` days from `now`, as a struct tm at noon.
 *
 * Steps the day through mktime() rather than adding 86400 s per day: across a
 * DST change that addition lands an hour off, which near midnight is the wrong
 * date (a repeated or skipped column in the week grid). */
inline struct tm local_day(time_t now, int day_offset) {
  struct tm tm_d;
  localtime_r(&now, &tm_d);
  tm_d.tm_mday += day_offset;
  tm_d.tm_hour = 12;
  tm_d.tm_min = 0;
  tm_d.tm_sec = 0;
  tm_d.tm_isdst = -1;
  mktime(&tm_d);  // normalises the date and fills tm_wday
  return tm_d;
}

/** "YYYY-MM-DD" (11 bytes incl. NUL) for local_day(now, day_offset). */
inline void local_date(time_t now, int day_offset, char *out) {
  struct tm tm_d = local_day(now, day_offset);
  // Fields bounded with % so the compiler can prove the output fits.
  snprintf(out, 11, "%04u-%02u-%02u", (unsigned) (tm_d.tm_year + 1900) % 10000u,
           (unsigned) (tm_d.tm_mon + 1) % 100u, (unsigned) tm_d.tm_mday % 100u);
}

/** "YYYY-MM-DD HH:MM:SS" (20 bytes incl. NUL) local time for `t`. Comparable
 * as a string with the event times in the calendar buffer once their 'T'
 * separator is normalised to a space. */
inline void local_datetime(time_t t, char *out) {
  struct tm tm_d;
  localtime_r(&t, &tm_d);
  snprintf(out, 20, "%04u-%02u-%02u %02u:%02u:%02u", (unsigned) (tm_d.tm_year + 1900) % 10000u,
           (unsigned) (tm_d.tm_mon + 1) % 100u, (unsigned) tm_d.tm_mday % 100u, (unsigned) tm_d.tm_hour % 100u,
           (unsigned) tm_d.tm_min % 100u, (unsigned) tm_d.tm_sec % 100u);
}

/** Longest event title kept in the pipe-delimited buffer. Titles longer than
 * this are truncated explicitly so the line separator can never be lost. */
static constexpr size_t MAX_TITLE_LEN = 96;
/** Location / description kept for the event details card (bytes). Meeting
 * invites can carry kilobytes of boilerplate, so these are capped. */
static constexpr size_t MAX_LOCATION_LEN = 120;
static constexpr size_t MAX_DESCRIPTION_LEN = 320;

/** Shorten s to at most max_len bytes without splitting a UTF-8 sequence. */
inline void truncate_utf8(std::string &s, size_t max_len) {
  if (s.size() <= max_len)
    return;
  size_t n = max_len;
  while (n > 0 && (static_cast<unsigned char>(s[n]) & 0xC0) == 0x80)
    n--;  // s[n] is a continuation byte: back up to the start of its character
  s.resize(n);
}

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
 * Parse a Home Assistant calendar event list and append one line per event to
 * out_buf in the form:
 *
 *   CAL_IDX|TITLE|START|END|ALLDAY|LOCATION|DESCRIPTION\n
 *
 * Handles both shapes HA emits for start/end:
 *   REST  /api/calendars/<entity>          — {"start": {"dateTime": "..."}}
 *                                            {"start": {"date": "..."}} for all-day
 *   service calendar.get_events            — {"start": "2026-08-14T09:00:00+01:00"}
 *                                            {"start": "2026-08-14"} for all-day
 * The flat form is detected by the absence of a 'T' separator in a 10-char value.
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
    truncate_utf8(title, MAX_TITLE_LEN);

    std::string location, description;
    extract_string_field(event, "location", location);
    extract_string_field(event, "description", description);
    sanitize_field(location);
    sanitize_field(description);
    truncate_utf8(location, MAX_LOCATION_LEN);
    truncate_utf8(description, MAX_DESCRIPTION_LEN);

    bool allday = false;
    std::string start_obj;
    std::string start_val;
    if (extract_object_field(event, "start", start_obj)) {
      if (!extract_string_field(start_obj, "dateTime", start_val)) {
        if (extract_string_field(start_obj, "date", start_val)) {
          allday = true;
        }
      }
    } else if (extract_string_field(event, "start", start_val)) {
      // Flat form: a date-only value ("YYYY-MM-DD") means an all-day event.
      allday = (start_val.find('T') == std::string::npos);
    }
    if (start_val.empty()) {
      continue;
    }

    // All-day events carry their end *date*, which is exclusive (a one-day
    // event on the 4th ends on the 5th); renderers use it to span multi-day
    // events across every day they cover.
    std::string end_obj;
    std::string end_val;
    if (extract_object_field(event, "end", end_obj)) {
      if (!extract_string_field(end_obj, allday ? "date" : "dateTime", end_val)) {
        extract_string_field(end_obj, allday ? "dateTime" : "date", end_val);
      }
    } else {
      extract_string_field(event, "end", end_val);
    }

    const size_t keep = allday ? 10U : 19U;
    if (start_val.size() > keep) {
      start_val.resize(keep);
    }
    if (end_val.size() > keep) {
      end_val.resize(keep);
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
    out_buf += '|';
    out_buf += location;
    out_buf += '|';
    out_buf += description;
    out_buf += '\n';
    appended++;
  }

  return appended;
}

/** One line of the event buffer, split into its fields. */
struct EventLine {
  int cal_idx{-1};
  std::string title, start, end;  // start/end: "YYYY-MM-DD" or "YYYY-MM-DD HH:MM:SS"
  bool allday{false};
  std::string location, description;
};

/** Split "CAL_IDX|TITLE|START|END|ALLDAY[|LOCATION|DESCRIPTION]" (the last two
 * are optional, for lines built without them). 'T' separators are normalised
 * to a space. False if the mandatory fields are missing. */
inline bool parse_event_line(const std::string &line, EventLine &out) {
  std::string f[7];
  size_t n = 0, pos = 0;
  while (n < 7) {
    size_t bar = n < 6 ? line.find('|', pos) : std::string::npos;  // last field keeps any '|'
    f[n++] = line.substr(pos, bar == std::string::npos ? std::string::npos : bar - pos);
    if (bar == std::string::npos)
      break;
    pos = bar + 1;
  }
  if (n < 5)
    return false;
  out.cal_idx = atoi(f[0].c_str());
  out.title = f[1];
  out.start = f[2];
  out.end = f[3];
  out.allday = f[4] == "1";
  out.location = n > 5 ? f[5] : std::string();
  out.description = n > 6 ? f[6] : std::string();
  for (std::string *s : {&out.start, &out.end})
    if (s->size() > 10 && (*s)[10] == 'T')
      (*s)[10] = ' ';
  return !out.start.empty();
}

/** Days since 1970-01-01 for a civil date (proleptic Gregorian). */
inline int64_t days_from_civil(int y, int m, int d) {
  y -= m <= 2;
  const int64_t era = (y >= 0 ? y : y - 399) / 400;
  const int yoe = static_cast<int>(y - era * 400);
  const int doy = (153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + d - 1;
  const int doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
  return era * 146097 + doe - 719468;
}

/** Inverse of days_from_civil. */
inline void civil_from_days(int64_t z, int &y, int &m, int &d) {
  z += 719468;
  const int64_t era = (z >= 0 ? z : z - 146096) / 146097;
  const int doe = static_cast<int>(z - era * 146097);
  const int yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
  const int doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
  const int mp = (5 * doy + 2) / 153;
  d = doy - (153 * mp + 2) / 5 + 1;
  m = mp < 10 ? mp + 3 : mp - 9;
  y = static_cast<int>(yoe + era * 400) + (m <= 2);
}

/** Human-readable "when" for the event details card, e.g.
 *   "Mon 6 Oct · 09:00–10:00"     "Mon 6 Oct 22:00 – Tue 7 Oct 06:00"
 *   "Mon 6 Oct · All day"         "Mon 6 – Wed 8 Oct · All day"
 * Pure date arithmetic on the event's own local fields (no timezone lookup). */
inline std::string format_event_when(const EventLine &e) {
  static const char *const DAYS[] = {"Sun", "Mon", "Tue", "Wed", "Thu", "Fri", "Sat"};
  static const char *const MONTHS[] = {"Jan", "Feb", "Mar", "Apr", "May", "Jun",
                                       "Jul", "Aug", "Sep", "Oct", "Nov", "Dec"};
  auto day_of = [](const std::string &s, int64_t &days) {
    int y, m, d;
    if (s.size() < 10 || sscanf(s.c_str(), "%d-%d-%d", &y, &m, &d) != 3 || m < 1 || m > 12)
      return false;
    days = days_from_civil(y, m, d);
    return true;
  };
  auto fmt_day = [](int64_t days, bool with_month) {
    int y, m, d;
    civil_from_days(days, y, m, d);
    int dow = static_cast<int>(((days % 7) + 11) % 7);  // 1970-01-01 was a Thursday
    char b[24];
    if (with_month)
      snprintf(b, sizeof(b), "%s %d %s", DAYS[dow], d, MONTHS[m - 1]);
    else
      snprintf(b, sizeof(b), "%s %d", DAYS[dow], d);
    return std::string(b);
  };
  auto month_of = [](int64_t days) {
    int y, m, d;
    civil_from_days(days, y, m, d);
    return y * 12 + m;
  };

  int64_t s_day, e_day;
  if (!day_of(e.start, s_day))
    return std::string();
  const char *const DOT = " \xc2\xb7 ";

  if (e.allday) {
    // The end date is exclusive: an event on the 6th ends on the 7th.
    int64_t last = day_of(e.end, e_day) && e_day > s_day ? e_day - 1 : s_day;
    if (last == s_day)
      return fmt_day(s_day, true) + DOT + "All day";
    bool same_month = month_of(s_day) == month_of(last);
    return fmt_day(s_day, !same_month) + " \xe2\x80\x93 " + fmt_day(last, true) + DOT + "All day";
  }

  std::string s_time = e.start.size() >= 16 ? e.start.substr(11, 5) : std::string();
  if (!day_of(e.end, e_day) || e.end.size() < 16)
    return fmt_day(s_day, true) + (s_time.empty() ? "" : DOT + s_time);
  std::string e_time = e.end.substr(11, 5);
  if (e_day == s_day)
    return fmt_day(s_day, true) + DOT + s_time + "\xe2\x80\x93" + e_time;
  return fmt_day(s_day, true) + " " + s_time + " \xe2\x80\x93 " + fmt_day(e_day, true) + " " + e_time;
}

}  // namespace calendar_json
}  // namespace esp32_dash
