#pragma once

#include <algorithm>
#include <cstdlib>
#include <cstdio>
#include <string>

namespace esp32_dash {
namespace calendar_json {

inline void skip_ws(const std::string &json, size_t &pos) {
  while (pos < json.size()) {
    char c = json[pos];
    if (c != ' ' && c != '\n' && c != '\r' && c != '\t') {
      break;
    }
    pos++;
  }
}

inline bool parse_json_string(const std::string &json, size_t &pos, std::string *out) {
  if (pos >= json.size() || json[pos] != '"') {
    return false;
  }

  pos++;
  if (out != nullptr) {
    out->clear();
  }

  bool escape = false;
  for (; pos < json.size(); pos++) {
    char c = json[pos];
    if (escape) {
      if (out != nullptr) {
        out->push_back(c);
      }
      escape = false;
      continue;
    }
    if (c == '\\') {
      escape = true;
      continue;
    }
    if (c == '"') {
      return true;
    }
    if (out != nullptr) {
      out->push_back(c);
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

inline bool extract_string_field(const std::string &obj, const char *key, std::string &value) {
  int depth = 0;
  for (size_t i = 0; i < obj.size(); i++) {
    char c = obj[i];
    if (c == '"') {
      std::string token;
      if (!parse_json_string(obj, i, &token)) {
        return false;
      }
      if (depth == 1 && token == key) {
        size_t pos = i + 1;
        skip_ws(obj, pos);
        if (pos >= obj.size() || obj[pos] != ':') {
          return false;
        }
        pos++;
        skip_ws(obj, pos);
        if (pos >= obj.size() || obj[pos] != '"') {
          return false;
        }
        return parse_json_string(obj, pos, &value);
      }
    } else if (c == '{') {
      depth++;
    } else if (c == '}') {
      depth--;
    }
  }

  return false;
}

inline bool extract_object_field(const std::string &obj, const char *key, std::string &value) {
  int depth = 0;
  for (size_t i = 0; i < obj.size(); i++) {
    char c = obj[i];
    if (c == '"') {
      std::string token;
      if (!parse_json_string(obj, i, &token)) {
        return false;
      }
      if (depth == 1 && token == key) {
        size_t pos = i + 1;
        skip_ws(obj, pos);
        if (pos >= obj.size() || obj[pos] != ':') {
          return false;
        }
        pos++;
        skip_ws(obj, pos);
        if (pos >= obj.size() || obj[pos] != '{') {
          return false;
        }
        size_t end = 0;
        if (!extract_object_span(obj, pos, end)) {
          return false;
        }
        value.assign(obj, pos, end - pos + 1);
        return true;
      }
    } else if (c == '{') {
      depth++;
    } else if (c == '}') {
      depth--;
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

inline bool extract_array_field(const std::string &obj, const char *key, std::string &value) {
  int depth = 0;
  for (size_t i = 0; i < obj.size(); i++) {
    char c = obj[i];
    if (c == '"') {
      std::string token;
      if (!parse_json_string(obj, i, &token)) {
        return false;
      }
      if (depth == 1 && token == key) {
        size_t pos = i + 1;
        skip_ws(obj, pos);
        if (pos >= obj.size() || obj[pos] != ':') {
          return false;
        }
        pos++;
        skip_ws(obj, pos);
        if (pos >= obj.size() || obj[pos] != '[') {
          return false;
        }
        size_t end = 0;
        if (!extract_array_span(obj, pos, end)) {
          return false;
        }
        value.assign(obj, pos, end - pos + 1);
        return true;
      }
    } else if (c == '{') {
      depth++;
    } else if (c == '}') {
      depth--;
    }
  }

  return false;
}

inline bool extract_number_field(const std::string &obj, const char *key, float &value) {
  int depth = 0;
  for (size_t i = 0; i < obj.size(); i++) {
    char c = obj[i];
    if (c == '"') {
      std::string token;
      if (!parse_json_string(obj, i, &token)) {
        return false;
      }
      if (depth == 1 && token == key) {
        size_t pos = i + 1;
        skip_ws(obj, pos);
        if (pos >= obj.size() || obj[pos] != ':') {
          return false;
        }
        pos++;
        skip_ws(obj, pos);
        if (pos + 3 < obj.size() && obj.compare(pos, 4, "null") == 0) {
          return false;
        }
        const char *start = obj.c_str() + pos;
        char *end = nullptr;
        value = std::strtof(start, &end);
        return end != start;
      }
    } else if (c == '{') {
      depth++;
    } else if (c == '}') {
      depth--;
    }
  }

  return false;
}

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

    std::replace(title.begin(), title.end(), '|', ' ');
    std::replace(title.begin(), title.end(), '\n', ' ');
    std::replace(title.begin(), title.end(), '\r', ' ');

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

    char line[300];
    std::snprintf(line, sizeof(line), "%d|%s|%s|%s|%d\n", cal_idx, title.c_str(),
                  start_val.c_str(), end_val.c_str(), allday ? 1 : 0);
    out_buf += line;
    appended++;
  }

  return appended;
}

}  // namespace calendar_json
}  // namespace esp32_dash
