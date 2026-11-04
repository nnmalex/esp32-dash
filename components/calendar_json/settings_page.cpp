#include "settings_page.h"

#ifdef USE_ESP32_DASH_SETTINGS

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>

#include "esphome/core/hal.h"
#include "esphome/core/log.h"

#ifdef USE_CAPTIVE_PORTAL
#include "esphome/components/captive_portal/captive_portal.h"
#endif

#include "calendar_json.h"
#include "settings_page_html.h"

namespace esp32_dash {

static const char *const TAG = "settings_page";

/// How long a fetched entity list is served before the page triggers a refetch.
static constexpr uint32_t ENTITY_LIST_MAX_AGE_MS = 5 * 60 * 1000;
/// How long the httpd task waits for the main loop to build a state snapshot.
static constexpr TickType_t STATE_WAIT = pdMS_TO_TICKS(3000);

// ---------------------------------------------------------------------------
// helpers
// ---------------------------------------------------------------------------

static void json_escape(std::string &out, const char *s, size_t len) {
  out.push_back('"');
  for (size_t i = 0; i < len; i++) {
    unsigned char c = static_cast<unsigned char>(s[i]);
    switch (c) {
      case '"':
        out += "\\\"";
        break;
      case '\\':
        out += "\\\\";
        break;
      case '\n':
        out += "\\n";
        break;
      case '\r':
        out += "\\r";
        break;
      case '\t':
        out += "\\t";
        break;
      default:
        if (c < 0x20) {
          char buf[8];
          snprintf(buf, sizeof(buf), "\\u%04x", c);
          out += buf;
        } else {
          out.push_back(static_cast<char>(c));
        }
    }
  }
  out.push_back('"');
}
static void json_escape(std::string &out, const std::string &s) { json_escape(out, s.data(), s.size()); }
static void json_escape(std::string &out, const char *s) { json_escape(out, s ? s : "", s ? strlen(s) : 0); }

static void json_number(std::string &out, float v) {
  if (!std::isfinite(v)) {
    out += "null";
    return;
  }
  char buf[24];
  snprintf(buf, sizeof(buf), "%.6g", v);
  out += buf;
}

/// StringRef-like (c_str() + size(), c_str() may be null when empty) to std::string.
template<typename R> static std::string ref_to_string(const R &ref) {
  return ref.size() == 0 ? std::string() : std::string(ref.c_str(), ref.size());
}

static std::string trim(const std::string &s) {
  size_t b = s.find_first_not_of(" \t\r\n");
  if (b == std::string::npos)
    return "";
  size_t e = s.find_last_not_of(" \t\r\n");
  return s.substr(b, e - b + 1);
}

static std::string base64(const std::string &in) {
  static const char *const tbl = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
  std::string out;
  size_t i = 0;
  for (; i + 2 < in.size(); i += 3) {
    uint32_t n = (uint8_t) in[i] << 16 | (uint8_t) in[i + 1] << 8 | (uint8_t) in[i + 2];
    out += tbl[n >> 18 & 63];
    out += tbl[n >> 12 & 63];
    out += tbl[n >> 6 & 63];
    out += tbl[n & 63];
  }
  if (i + 1 == in.size()) {
    uint32_t n = (uint8_t) in[i] << 16;
    out += tbl[n >> 18 & 63];
    out += tbl[n >> 12 & 63];
    out += "==";
  } else if (i + 2 == in.size()) {
    uint32_t n = (uint8_t) in[i] << 16 | (uint8_t) in[i + 1] << 8;
    out += tbl[n >> 18 & 63];
    out += tbl[n >> 12 & 63];
    out += tbl[n >> 6 & 63];
    out += '=';
  }
  return out;
}

static const char *kind_name(SettingsPage::Kind kind) {
  switch (kind) {
    case SettingsPage::Kind::TEXT:
      return "entity";
    case SettingsPage::Kind::NUMBER:
      return "number";
    case SettingsPage::Kind::SELECT:
      return "select";
    case SettingsPage::Kind::SWITCH:
      return "switch";
  }
  return "";
}

// ---------------------------------------------------------------------------
// component
// ---------------------------------------------------------------------------

void SettingsPage::set_credentials(std::string username, std::string password) {
  if (username.empty()) {
    this->auth_header_.clear();
  } else {
    this->auth_header_ = "Basic " + base64(username + ":" + password);
  }
}

void SettingsPage::setup() {
  this->state_ready_ = xSemaphoreCreateBinary();
  if (this->state_ready_ == nullptr) {
    this->mark_failed();
    return;
  }
  this->base_->init();
  // Authentication, when configured, is done here rather than through
  // web_server's `auth:` (which this project does not use).
  this->base_->add_handler_without_auth(this);
}

void SettingsPage::dump_config() {
  ESP_LOGCONFIG(TAG,
                "Settings page:\n"
                "  Port: %u\n"
                "  Fields: %u\n"
                "  Authentication: %s",
                (unsigned) this->base_->get_port(), (unsigned) this->fields_.size(),
                this->auth_header_.empty() ? "none" : "basic");
}

void SettingsPage::loop() {
  // Writes first, so a snapshot requested right after a save already shows it.
  std::vector<std::pair<size_t, std::string>> pending;
  {
    std::lock_guard<std::mutex> guard(this->lock_);
    pending.swap(this->pending_);
  }
  for (auto &p : pending)
    this->apply_(this->fields_[p.first], p.second);

  if (this->state_wanted_.exchange(false)) {
    std::string json = this->build_state_json_();
    {
      std::lock_guard<std::mutex> guard(this->lock_);
      this->state_json_ = std::move(json);
    }
    xSemaphoreGive(this->state_ready_);
  }

  if (this->fetch_wanted_.exchange(false) && !this->fetch_in_flight_)
    this->start_entity_fetch_();
}

// ---------------------------------------------------------------------------
// routing (httpd task)
// ---------------------------------------------------------------------------

SettingsPage::Route SettingsPage::route_(AsyncWebServerRequest *request) const {
  char buf[AsyncWebServerRequest::URL_BUF_SIZE];
  auto url = request->url_to(buf);
  const auto method = request->method();
  if (method == HTTP_GET) {
    if (url == "/" || url == "/settings")
      return Route::PAGE;
    if (url == "/settings/state")
      return Route::STATE;
    if (url == "/settings/entities")
      return Route::ENTITIES;
  } else if (method == HTTP_POST) {
    if (url == "/settings/set")
      return Route::SET;
  }
  return Route::NONE;
}

bool SettingsPage::canHandle(AsyncWebServerRequest *request) const {
#ifdef USE_CAPTIVE_PORTAL
  // In AP mode the captive portal owns every GET (it answers OS probe URLs with
  // its WiFi page); stay out of its way.
  if (esphome::captive_portal::global_captive_portal != nullptr &&
      esphome::captive_portal::global_captive_portal->is_active())
    return false;
#endif
  return this->route_(request) != Route::NONE;
}

bool SettingsPage::check_auth_(AsyncWebServerRequest *request) const {
  if (this->auth_header_.empty())
    return true;
  auto header = request->get_header("Authorization");
  if (header.has_value() && header.value() == this->auth_header_)
    return true;
  auto *response = request->beginResponse(401, "text/plain", std::string("Authentication required"));
  response->addHeader("WWW-Authenticate", "Basic realm=\"esp32-dash settings\"");
  request->send(response);
  return false;
}

void SettingsPage::handleRequest(AsyncWebServerRequest *request) {
  if (!this->check_auth_(request))
    return;
  switch (this->route_(request)) {
    case Route::PAGE: {
      auto *response = request->beginResponse(200, "text/html", reinterpret_cast<const uint8_t *>(SETTINGS_PAGE_HTML),
                                              sizeof(SETTINGS_PAGE_HTML) - 1);
      response->addHeader("Cache-Control", "no-cache");
      request->send(response);
      return;
    }
    case Route::STATE:
      this->send_state_(request);
      return;
    case Route::ENTITIES:
      this->send_entities_(request);
      return;
    case Route::SET:
      this->handle_set_(request);
      return;
    case Route::NONE:
      break;
  }
  request->send(404, "text/plain", "Not found");
}

// ---------------------------------------------------------------------------
// endpoints (httpd task)
// ---------------------------------------------------------------------------

void SettingsPage::send_state_(AsyncWebServerRequest *request) {
  xSemaphoreTake(this->state_ready_, 0);  // drop a stale signal
  this->state_wanted_ = true;
  if (xSemaphoreTake(this->state_ready_, STATE_WAIT) != pdTRUE) {
    request->send(503, "application/json", "{\"error\":\"device busy, try again\"}");
    return;
  }
  std::string json;
  {
    std::lock_guard<std::mutex> guard(this->lock_);
    json = this->state_json_;
  }
  auto *response = request->beginResponse(200, "application/json", json);
  response->addHeader("Cache-Control", "no-store");
  request->send(response);
}

void SettingsPage::send_entities_(AsyncWebServerRequest *request) {
  const bool force = request->hasArg("refresh");
  std::string json = "{\"status\":";
  {
    std::lock_guard<std::mutex> guard(this->lock_);
    // Fetch on first use and when the list has aged out. After a failure only an
    // explicit refresh retries, so a poll cannot turn an error back into
    // "loading" and hammer an unreachable HA.
    const bool never = this->entities_status_ == "idle";
    const bool stale = this->entities_status_ == "ok" &&
                       esphome::millis() - this->entities_fetched_ms_ > ENTITY_LIST_MAX_AGE_MS;
    if ((force || never || stale) && this->entities_status_ != "loading") {
      // Report "loading" straight away so the page keeps polling; loop() starts it.
      this->entities_status_ = "loading";
      this->fetch_wanted_ = true;
    }
    json_escape(json, this->entities_status_);
    json += ",\"error\":";
    json_escape(json, this->entities_error_);
    json += ",\"age\":";
    json += this->entities_have_data_ ? std::to_string((esphome::millis() - this->entities_fetched_ms_) / 1000) : "null";
    json += ",\"entities\":";
    json += this->entities_json_;
  }
  json += "}";
  auto *response = request->beginResponse(200, "application/json", json);
  response->addHeader("Cache-Control", "no-store");
  request->send(response);
}

void SettingsPage::handle_set_(AsyncWebServerRequest *request) {
  const std::string key = request->arg("key");
  std::string value = request->arg("value");
  size_t index = 0;
  for (; index < this->fields_.size(); index++) {
    if (key == this->fields_[index].key)
      break;
  }
  std::string err;
  if (index == this->fields_.size()) {
    err = "unknown setting";
  } else {
    err = this->validate_(this->fields_[index], value);
  }
  if (!err.empty()) {
    std::string json = "{\"ok\":false,\"error\":";
    json_escape(json, err);
    json += "}";
    request->send(400, "application/json", json.c_str());
    return;
  }
  {
    std::lock_guard<std::mutex> guard(this->lock_);
    this->pending_.emplace_back(index, std::move(value));
  }
  request->send(200, "application/json", "{\"ok\":true}");
}

// Reads only immutable configuration (traits are set during setup and never
// change), never the entity's state, so it is safe on the httpd task.
std::string SettingsPage::validate_(const Field &field, std::string &value) const {
  switch (field.kind) {
#ifdef USE_TEXT
    case Kind::TEXT: {
      value = trim(value);
      auto *t = static_cast<esphome::text::Text *>(field.entity);
      if (value.size() > (size_t) t->traits.get_max_length())
        return "too long";
      for (char c : value) {
        if (static_cast<unsigned char>(c) < 0x20)
          return "invalid character";
      }
      return "";
    }
#endif
#ifdef USE_NUMBER
    case Kind::NUMBER: {
      auto *n = static_cast<esphome::number::Number *>(field.entity);
      char *end = nullptr;
      value = trim(value);
      float v = strtof(value.c_str(), &end);
      if (value.empty() || end == nullptr || *end != '\0' || !std::isfinite(v))
        return "not a number";
      if (v < n->traits.get_min_value() || v > n->traits.get_max_value())
        return "out of range";
      return "";
    }
#endif
#ifdef USE_SELECT
    case Kind::SELECT: {
      auto *s = static_cast<esphome::select::Select *>(field.entity);
      if (!s->has_option(value.c_str()))
        return "unknown option";
      return "";
    }
#endif
#ifdef USE_SWITCH
    case Kind::SWITCH:
      if (value == "true" || value == "1" || value == "on") {
        value = "1";
      } else if (value == "false" || value == "0" || value == "off") {
        value = "0";
      } else {
        return "expected true or false";
      }
      return "";
#endif
    default:
      return "unsupported setting";
  }
}

// ---------------------------------------------------------------------------
// main loop
// ---------------------------------------------------------------------------

void SettingsPage::apply_(const Field &field, const std::string &value) {
  ESP_LOGI(TAG, "%s = '%s' (from settings page)", field.key, value.c_str());
  // Same calls Home Assistant used to make, so on_value / set_action triggers
  // and NVS restore behave exactly as before.
  switch (field.kind) {
#ifdef USE_TEXT
    case Kind::TEXT:
      static_cast<esphome::text::Text *>(field.entity)->make_call().set_value(value).perform();
      break;
#endif
#ifdef USE_NUMBER
    case Kind::NUMBER:
      static_cast<esphome::number::Number *>(field.entity)->make_call().set_value(strtof(value.c_str(), nullptr)).perform();
      break;
#endif
#ifdef USE_SELECT
    case Kind::SELECT:
      static_cast<esphome::select::Select *>(field.entity)->make_call().set_option(value).perform();
      break;
#endif
#ifdef USE_SWITCH
    case Kind::SWITCH: {
      auto *sw = static_cast<esphome::switch_::Switch *>(field.entity);
      if (value == "1") {
        sw->turn_on();
      } else {
        sw->turn_off();
      }
      break;
    }
#endif
    default:
      break;
  }
}

std::string SettingsPage::build_state_json_() const {
  std::string out;
  out.reserve(4096);
  out += "{\"title\":";
  json_escape(out, this->title_);
  out += ",\"token\":";
  out += (this->token_ && !this->token_().empty()) ? "true" : "false";
  out += ",\"fields\":[";
  bool first = true;
  for (const auto &f : this->fields_) {
    if (!first)
      out += ',';
    first = false;
    out += "{\"key\":";
    json_escape(out, f.key);
    out += ",\"group\":";
    json_escape(out, f.group);
    out += ",\"label\":";
    json_escape(out, f.label);
    out += ",\"help\":";
    json_escape(out, f.help);
    out += ",\"kind\":";
    json_escape(out, kind_name(f.kind));
    out += ",\"value\":";
    switch (f.kind) {
#ifdef USE_TEXT
      case Kind::TEXT: {
        auto *t = static_cast<esphome::text::Text *>(f.entity);
        json_escape(out, t->state);
        out += ",\"domains\":";
        json_escape(out, f.domains);
        break;
      }
#endif
#ifdef USE_NUMBER
      case Kind::NUMBER: {
        auto *n = static_cast<esphome::number::Number *>(f.entity);
        json_number(out, n->state);
        out += ",\"min\":";
        json_number(out, n->traits.get_min_value());
        out += ",\"max\":";
        json_number(out, n->traits.get_max_value());
        out += ",\"step\":";
        json_number(out, n->traits.get_step());
        out += ",\"unit\":";
        json_escape(out, f.unit);
        break;
      }
#endif
#ifdef USE_SELECT
      case Kind::SELECT: {
        auto *s = static_cast<esphome::select::Select *>(f.entity);
        json_escape(out, ref_to_string(s->current_option()));
        out += ",\"options\":[";
        bool first_opt = true;
        for (const char *opt : s->traits.get_options()) {
          if (!first_opt)
            out += ',';
          first_opt = false;
          json_escape(out, opt);
        }
        out += ']';
        break;
      }
#endif
#ifdef USE_SWITCH
      case Kind::SWITCH:
        out += static_cast<esphome::switch_::Switch *>(f.entity)->state ? "true" : "false";
        break;
#endif
      default:
        out += "null";
        break;
    }
    out += '}';
  }
  out += "]}";
  return out;
}

void SettingsPage::start_entity_fetch_() {
  const std::string token = this->token_ ? this->token_() : std::string();
  const std::string base = this->base_url_ ? this->base_url_() : std::string();
  const char *err = nullptr;
  if (this->fetcher_ == nullptr) {
    err = "No HTTP fetcher configured";
  } else if (token.empty()) {
    err = "No ha_token set: entity lists need a Home Assistant long-lived access token";
  } else if (base.empty()) {
    err = "Home Assistant address is not known yet";
  }
  if (err != nullptr) {
    std::lock_guard<std::mutex> guard(this->lock_);
    this->entities_status_ = "error";
    this->entities_error_ = err;
    return;
  }
  this->fetch_in_flight_ = true;
  bool queued = this->fetcher_->request(base + "/api/states", "", {{"Authorization", "Bearer " + token}},
                                        [this](const HttpResponse &r) {
                                          this->fetch_in_flight_ = false;
                                          this->parse_entities_(r);
                                        });
  if (!queued) {
    this->fetch_in_flight_ = false;
    std::lock_guard<std::mutex> guard(this->lock_);
    this->entities_status_ = "error";
    this->entities_error_ = "HTTP worker busy, try again";
  }
}

void SettingsPage::parse_entities_(const HttpResponse &r) {
  namespace cj = calendar_json;
  auto fail = [this](const std::string &msg) {
    ESP_LOGW(TAG, "Entity list: %s", msg.c_str());
    std::lock_guard<std::mutex> guard(this->lock_);
    this->entities_status_ = "error";
    this->entities_error_ = msg;
  };
  if (!r.error.empty()) {
    fail("Could not reach Home Assistant: " + r.error);
    return;
  }
  if (r.status == 401) {
    fail("Home Assistant rejected the ha_token (HTTP 401)");
    return;
  }
  if (r.status != 200) {
    fail("Home Assistant answered HTTP " + std::to_string(r.status));
    return;
  }

  const uint32_t start = esphome::millis();
  struct Entry {
    std::string id, name, unit;
  };
  std::vector<Entry> entries;
  const std::string &body = r.body;
  size_t pos = 0;
  cj::skip_ws(body, pos);
  if (pos >= body.size() || body[pos] != '[') {
    fail("Unexpected response from /api/states");
    return;
  }
  pos++;
  std::string obj, attrs;
  while (pos < body.size()) {
    cj::skip_ws(body, pos);
    if (pos < body.size() && body[pos] == ',') {
      pos++;
      continue;
    }
    if (pos >= body.size() || body[pos] != '{')
      break;  // ']' or garbage
    size_t end = 0;
    if (!cj::extract_object_span(body, pos, end))
      break;
    obj.assign(body, pos, end - pos + 1);
    pos = end + 1;

    Entry e;
    if (!cj::extract_string_field(obj, "entity_id", e.id) || e.id.empty())
      continue;
    if (cj::extract_object_field(obj, "attributes", attrs)) {
      cj::extract_string_field(attrs, "friendly_name", e.name);
      cj::extract_string_field(attrs, "unit_of_measurement", e.unit);
    }
    entries.push_back(std::move(e));
  }
  std::sort(entries.begin(), entries.end(), [](const Entry &a, const Entry &b) { return a.id < b.id; });

  std::string json;
  json.reserve(entries.size() * 64 + 2);
  json += '[';
  for (size_t i = 0; i < entries.size(); i++) {
    if (i)
      json += ',';
    json += '[';
    json_escape(json, entries[i].id);
    json += ',';
    json_escape(json, entries[i].name);
    json += ',';
    json_escape(json, entries[i].unit);
    json += ']';
  }
  json += ']';

  ESP_LOGI(TAG, "Entity list: %u entities from %u bytes, parsed in %u ms", (unsigned) entries.size(),
           (unsigned) body.size(), (unsigned) (esphome::millis() - start));
  std::lock_guard<std::mutex> guard(this->lock_);
  this->entities_json_ = std::move(json);
  this->entities_status_ = "ok";
  this->entities_error_.clear();
  this->entities_fetched_ms_ = esphome::millis();
  this->entities_have_data_ = true;
}

}  // namespace esp32_dash

#endif  // USE_ESP32_DASH_SETTINGS
