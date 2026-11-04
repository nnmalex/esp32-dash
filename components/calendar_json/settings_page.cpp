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

template<typename S> static void json_escape(S &out, const char *s, size_t len) {
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
template<typename S> static void json_escape(S &out, const std::string &s) { json_escape(out, s.data(), s.size()); }
template<typename S> static void json_escape(S &out, const char *s) { json_escape(out, s ? s : "", s ? strlen(s) : 0); }

template<typename S> static void json_number(S &out, float v) {
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
    auto json = this->build_state_json_();
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
  std::shared_ptr<const PsramString> json;
  {
    std::lock_guard<std::mutex> guard(this->lock_);
    json = this->state_json_;  // immutable once published; sent below without the lock
  }
  // Chunked straight from PSRAM, like the entity list: copying 20 KB+ into an
  // AsyncWebServerResponse would put it in internal RAM.
  httpd_req_t *r = *request;
  httpd_resp_set_type(r, "application/json");
  httpd_resp_set_hdr(r, "Cache-Control", "no-store");
  if (json && !json->empty()) {
    if (httpd_resp_send_chunk(r, json->data(), json->size()) != ESP_OK)
      return;
  } else if (httpd_resp_send_chunk(r, "{}", 2) != ESP_OK) {
    return;
  }
  httpd_resp_send_chunk(r, nullptr, 0);
}

void SettingsPage::send_entities_(AsyncWebServerRequest *request) {
  const bool force = request->hasArg("refresh");
  std::string head = "{\"status\":";
  std::shared_ptr<const PsramString> list;
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
    json_escape(head, this->entities_status_);
    head += ",\"error\":";
    json_escape(head, this->entities_error_);
    head += ",\"entities\":";
    list = this->entities_;  // immutable once published; sent below without the lock
  }

  // Chunked straight from the PSRAM buffer: the list can be hundreds of KB, more
  // than is safe to copy into internal RAM for an AsyncWebServerResponse.
  httpd_req_t *r = *request;
  httpd_resp_set_type(r, "application/json");
  httpd_resp_set_hdr(r, "Cache-Control", "no-store");
  if (httpd_resp_send_chunk(r, head.data(), head.size()) != ESP_OK)
    return;
  if (list && !list->empty()) {
    if (httpd_resp_send_chunk(r, list->data(), list->size()) != ESP_OK)
      return;
  } else if (httpd_resp_send_chunk(r, "[]", 2) != ESP_OK) {
    return;
  }
  if (httpd_resp_send_chunk(r, "}", 1) != ESP_OK)
    return;
  httpd_resp_send_chunk(r, nullptr, 0);
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

std::shared_ptr<const PsramString> SettingsPage::build_state_json_() const {
  auto json = std::make_shared<PsramString>();
  PsramString &out = *json;
  out.reserve(32 * 1024);
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
  return json;
}

void SettingsPage::set_entities_error_(const std::string &msg) {
  ESP_LOGW(TAG, "Entity list: %s", msg.c_str());
  std::lock_guard<std::mutex> guard(this->lock_);
  this->entities_status_ = "error";
  this->entities_error_ = msg;
}

void SettingsPage::start_entity_fetch_() {
  const std::string token = this->token_ ? this->token_() : std::string();
  const std::string base = this->base_url_ ? this->base_url_() : std::string();
  if (this->fetcher_ == nullptr) {
    this->set_entities_error_("No HTTP fetcher configured");
    return;
  }
  if (token.empty()) {
    this->set_entities_error_("No ha_token set: entity lists need a Home Assistant long-lived access token");
    return;
  }
  if (base.empty()) {
    this->set_entities_error_("Home Assistant address is not known yet");
    return;
  }

  // /api/states is the whole HA state machine, often megabytes: it is scanned
  // as it streams in (on the worker task) and never buffered.
  auto scanner = std::make_shared<EntityListScanner>();
  const uint32_t start = esphome::millis();
  this->fetch_in_flight_ = true;
  bool queued = this->fetcher_->request(
      base + "/api/states", "", {{"Authorization", "Bearer " + token}},
      [this, scanner, start](const HttpResponse &r) {
        this->fetch_in_flight_ = false;
        if (!r.error.empty()) {
          this->set_entities_error_("Could not reach Home Assistant: " + r.error);
          return;
        }
        if (r.status == 401) {
          this->set_entities_error_("Home Assistant rejected the ha_token (HTTP 401)");
          return;
        }
        if (r.status != 200) {
          this->set_entities_error_("Home Assistant answered HTTP " + std::to_string(r.status));
          return;
        }
        // A list that stops part-way would look fine on the page but silently
        // lack every entity HA sends after the cut. Keep the previous list.
        if (!scanner->complete()) {
          this->set_entities_error_("Home Assistant's entity list arrived incomplete (" +
                                    std::to_string(scanner->count()) + " entities read), try Refresh");
          return;
        }
        auto list = std::make_shared<PsramString>();
        PsramString &out = scanner->output();
        list->reserve(out.size() + 2);
        list->push_back('[');
        list->append(out);
        list->push_back(']');
        out = PsramString();  // free the scanner's copy now
        ESP_LOGI(TAG, "Entity list: %u entities, %u bytes, %u ms%s", (unsigned) scanner->count(),
                 (unsigned) list->size(), (unsigned) (esphome::millis() - start),
                 scanner->truncated() ? " (truncated)" : "");
        std::lock_guard<std::mutex> guard(this->lock_);
        this->entities_ = std::move(list);
        this->entities_status_ = "ok";
        this->entities_error_.clear();
        this->entities_fetched_ms_ = esphome::millis();
      },
      [scanner](const char *data, size_t len) { return scanner->feed(data, len); });
  if (!queued) {
    this->fetch_in_flight_ = false;
    this->set_entities_error_("HTTP worker busy, try again");
  }
}

// ---------------------------------------------------------------------------
// EntityListScanner (HttpWorker task)
// ---------------------------------------------------------------------------

bool EntityListScanner::feed(const char *data, size_t len) {
  for (size_t i = 0; i < len; i++)
    this->on_char_(data[i]);
  return true;  // keep reading even when truncated, so the connection ends cleanly
}

static void append_utf8_capped(std::string &out, uint32_t cp, size_t cap) {
  char buf[4];
  size_t n;
  if (cp < 0x80) {
    buf[0] = static_cast<char>(cp);
    n = 1;
  } else if (cp < 0x800) {
    buf[0] = static_cast<char>(0xC0 | (cp >> 6));
    buf[1] = static_cast<char>(0x80 | (cp & 0x3F));
    n = 2;
  } else if (cp < 0x10000) {
    buf[0] = static_cast<char>(0xE0 | (cp >> 12));
    buf[1] = static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
    buf[2] = static_cast<char>(0x80 | (cp & 0x3F));
    n = 3;
  } else {
    buf[0] = static_cast<char>(0xF0 | (cp >> 18));
    buf[1] = static_cast<char>(0x80 | ((cp >> 12) & 0x3F));
    buf[2] = static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
    buf[3] = static_cast<char>(0x80 | (cp & 0x3F));
    n = 4;
  }
  if (out.size() + n <= cap)
    out.append(buf, n);
}

void EntityListScanner::on_char_(char c) {
  if (this->in_string_) {
    if (this->escape_ == 1) {
      this->escape_ = 0;
      char d = c;
      switch (c) {
        case 'n':
        case 'r':
        case 't':
        case 'b':
        case 'f':
          d = ' ';
          break;
        case 'u':
          this->escape_ = 2;
          this->hex_ = 0;
          return;
        default:
          break;  // \" \\ \/ map to themselves
      }
      if (this->token_.size() < MAX_TOKEN)
        this->token_.push_back(d);
      return;
    }
    if (this->escape_ >= 2) {
      uint32_t v;
      if (c >= '0' && c <= '9') {
        v = c - '0';
      } else if (c >= 'a' && c <= 'f') {
        v = c - 'a' + 10;
      } else if (c >= 'A' && c <= 'F') {
        v = c - 'A' + 10;
      } else {
        v = 0;
      }
      this->hex_ = (this->hex_ << 4) | v;
      if (++this->escape_ < 6)
        return;
      this->escape_ = 0;
      uint32_t cp = this->hex_;
      if (cp >= 0xD800 && cp < 0xDC00) {
        this->high_surrogate_ = cp;
        return;
      }
      if (cp >= 0xDC00 && cp < 0xE000 && this->high_surrogate_ != 0) {
        cp = 0x10000 + ((this->high_surrogate_ - 0xD800) << 10) + (cp - 0xDC00);
      } else if (cp < 0x20) {
        cp = ' ';
      }
      this->high_surrogate_ = 0;
      append_utf8_capped(this->token_, cp, MAX_TOKEN);
      return;
    }
    if (c == '\\') {
      this->escape_ = 1;
    } else if (c == '"') {
      this->in_string_ = false;
      this->on_string_end_();
    } else if (this->token_.size() < MAX_TOKEN) {
      this->token_.push_back(c);
    }
    return;
  }

  switch (c) {
    case '"':
      this->in_string_ = true;
      this->string_is_key_ = this->depth_ > 0 && this->depth_ <= MAX_DEPTH &&
                             this->stack_[this->depth_ - 1] == '{' && this->expect_key_;
      this->token_.clear();
      this->high_surrogate_ = 0;
      break;
    case '{':
    case '[':
      if (this->depth_ < MAX_DEPTH) {
        this->stack_[this->depth_] = c;
      }
      this->depth_++;
      this->expect_key_ = c == '{';
      if (c == '{' && this->depth_ == 2) {
        // A new entity object in the top-level array.
        this->key2_.clear();
        this->id_.clear();
        this->name_.clear();
        this->unit_.clear();
      } else if (c == '{' && this->depth_ == 3) {
        this->key3_.clear();
      }
      break;
    case '}':
    case ']':
      if (this->depth_ == 0)
        break;
      if (c == '}' && this->depth_ == 2)
        this->emit_();
      this->depth_--;
      if (this->depth_ == 0)
        this->complete_ = true;
      this->expect_key_ = false;  // a value just ended; ',' re-arms the key
      break;
    case ':':
      this->expect_key_ = false;
      break;
    case ',':
      this->expect_key_ = this->depth_ > 0 && this->depth_ <= MAX_DEPTH && this->stack_[this->depth_ - 1] == '{';
      break;
    default:
      break;  // whitespace, numbers, true/false/null
  }
}

void EntityListScanner::on_string_end_() {
  if (this->string_is_key_) {
    if (this->depth_ == 2) {
      this->key2_ = this->token_;
    } else if (this->depth_ == 3) {
      this->key3_ = this->token_;
    }
    return;
  }
  if (this->depth_ == 2 && this->key2_ == "entity_id") {
    this->id_ = this->token_;
  } else if (this->depth_ == 3 && this->key2_ == "attributes") {
    if (this->key3_ == "friendly_name") {
      this->name_ = this->token_;
    } else if (this->key3_ == "unit_of_measurement") {
      this->unit_ = this->token_;
    }
  }
}

void EntityListScanner::emit_() {
  if (this->id_.empty() || this->truncated_)
    return;
  if (this->out_.size() + this->id_.size() + this->name_.size() + this->unit_.size() + 64 > MAX_OUTPUT) {
    this->truncated_ = true;
    return;
  }
  if (this->count_ > 0)
    this->out_.push_back(',');
  this->out_.push_back('[');
  json_escape(this->out_, this->id_);
  this->out_.push_back(',');
  json_escape(this->out_, this->name_);
  this->out_.push_back(',');
  json_escape(this->out_, this->unit_);
  this->out_.push_back(']');
  this->count_++;
}

}  // namespace esp32_dash

#endif  // USE_ESP32_DASH_SETTINGS
