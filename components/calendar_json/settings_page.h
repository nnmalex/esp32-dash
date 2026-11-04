#pragma once

#include "esphome/core/defines.h"

#ifdef USE_ESP32_DASH_SETTINGS

#include <atomic>
#include <functional>
#include <mutex>
#include <string>
#include <utility>
#include <vector>

#include <memory>

#include <esp_heap_caps.h>
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>

#include "esphome/components/web_server_base/web_server_base.h"
#include "esphome/core/component.h"

#ifdef USE_NUMBER
#include "esphome/components/number/number.h"
#endif
#ifdef USE_SELECT
#include "esphome/components/select/select.h"
#endif
#ifdef USE_SWITCH
#include "esphome/components/switch/switch.h"
#endif
#ifdef USE_TEXT
#include "esphome/components/text/text.h"
#endif

#include "http_worker.h"

namespace esp32_dash {

/// std allocator that places memory in PSRAM. This build uses
/// CONFIG_SPIRAM_USE_CAPS_ALLOC, so plain new/malloc only ever use internal RAM,
/// and a failed allocation aborts (no exceptions). Large buffers go here.
template<typename T> struct PsramAllocator {
  using value_type = T;
  PsramAllocator() = default;
  template<typename U> PsramAllocator(const PsramAllocator<U> &) {}
  T *allocate(size_t n) {
    void *p = heap_caps_malloc(n * sizeof(T), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (p == nullptr)
      p = heap_caps_malloc(n * sizeof(T), MALLOC_CAP_8BIT);
    if (p == nullptr)
      abort();  // what std::allocator would do here without exceptions, made explicit
    return static_cast<T *>(p);
  }
  void deallocate(T *p, size_t) { heap_caps_free(p); }
  template<typename U> bool operator==(const PsramAllocator<U> &) const { return true; }
  template<typename U> bool operator!=(const PsramAllocator<U> &) const { return false; }
};
using PsramString = std::basic_string<char, std::char_traits<char>, PsramAllocator<char>>;

/// Streaming scanner for HA's GET /api/states. Picks entity_id and the
/// friendly_name / unit_of_measurement attributes out of each top-level object
/// as the bytes arrive, so the multi-megabyte body is never held in memory, and
/// appends `["id","name","unit"],...` (JSON, unsorted) to a PSRAM string.
/// Runs on the HttpWorker task; owned by a shared_ptr the done callback reads.
class EntityListScanner {
 public:
  /// Stop collecting past this many bytes of output (a sane upper bound).
  static constexpr size_t MAX_OUTPUT = 4 * 1024 * 1024;

  bool feed(const char *data, size_t len);
  size_t count() const { return this->count_; }
  bool truncated() const { return this->truncated_; }
  /// The collected entries without the enclosing brackets.
  PsramString &output() { return this->out_; }

 protected:
  void on_string_end_();
  void on_char_(char c);
  void emit_();

  static constexpr size_t MAX_DEPTH = 32;
  static constexpr size_t MAX_TOKEN = 255;

  char stack_[MAX_DEPTH];  // '{' or '['
  size_t depth_{0};
  bool expect_key_{false};  // inside an object, before ':'

  bool in_string_{false};
  bool string_is_key_{false};
  uint8_t escape_{0};  // 0 none, 1 after a backslash, 2..5 reading the 4 hex digits of a u-escape
  uint32_t hex_{0};
  uint32_t high_surrogate_{0};
  std::string token_;  // capped at MAX_TOKEN bytes

  std::string key2_, key3_;  // current key in the entity object / one level below
  std::string id_, name_, unit_;
  PsramString out_;
  size_t count_{0};
  bool truncated_{false};
};

/// Device-hosted settings page (http://<device>/).
///
/// The settings are ordinary template text/number/select/switch entities,
/// declared `internal: true` so they are not exposed to Home Assistant. They keep
/// their NVS restore and their on_value / set_action triggers; this page reads
/// and writes them in their place. Entity pickers are filled from HA's
/// /api/states, fetched through the HttpWorker with the ha_token.
///
/// Threading: handleRequest() runs on the ESP-IDF httpd task. It only reads the
/// immutable field table, and hands everything else to the main loop: state
/// snapshots are built in loop() on request, writes are queued for loop() to
/// apply (performing the entity call there, so triggers run on the main loop),
/// and the entity list is fetched and parsed on the main loop.
class SettingsPage : public esphome::Component, public AsyncWebHandler {
 public:
  enum class Kind : uint8_t { TEXT, NUMBER, SELECT, SWITCH };

  struct Field {
    const char *key;      ///< stable identifier used by the page (the entity's YAML id)
    const char *group;    ///< section heading
    const char *label;
    const char *help;     ///< optional hint under the control
    const char *domains;  ///< entity pickers: comma-separated HA domains; "" = any domain
    const char *unit;     ///< numbers: unit shown next to the value
    Kind kind;
    void *entity;
  };

  explicit SettingsPage(esphome::web_server_base::WebServerBase *base) : base_(base) {}

  void setup() override;
  void loop() override;
  void dump_config() override;
  float get_setup_priority() const override { return esphome::setup_priority::WIFI - 1.0f; }

  void set_fetcher(HttpWorker *fetcher) { this->fetcher_ = fetcher; }
  void set_title(const char *title) { this->title_ = title; }
  void set_credentials(std::string username, std::string password);
  /// Read on the main loop each fetch: ha_base_url changes at runtime.
  void set_base_url(std::function<std::string()> f) { this->base_url_ = std::move(f); }
  void set_token(std::function<std::string()> f) { this->token_ = std::move(f); }

  void add_field(const char *key, const char *group, const char *label, const char *help, const char *domains,
                 const char *unit, Kind kind, void *entity) {
    this->fields_.push_back(Field{key, group, label, help, domains, unit, kind, entity});
  }

  // AsyncWebHandler (httpd task)
  bool canHandle(AsyncWebServerRequest *request) const override;
  void handleRequest(AsyncWebServerRequest *request) override;

 protected:
  enum class Route : uint8_t { NONE, PAGE, STATE, ENTITIES, SET };
  Route route_(AsyncWebServerRequest *request) const;
  bool check_auth_(AsyncWebServerRequest *request) const;

  // httpd task
  void send_state_(AsyncWebServerRequest *request);
  void send_entities_(AsyncWebServerRequest *request);
  void handle_set_(AsyncWebServerRequest *request);
  /// Validates `value` for `field` without touching the entity's state. Empty
  /// string on success, otherwise the reason it was rejected.
  std::string validate_(const Field &field, std::string &value) const;

  // main loop
  std::string build_state_json_() const;
  void apply_(const Field &field, const std::string &value);
  void start_entity_fetch_();
  void set_entities_error_(const std::string &msg);

  esphome::web_server_base::WebServerBase *base_;
  HttpWorker *fetcher_{nullptr};
  const char *title_{"ESP32 Dash"};
  std::string auth_header_;  ///< expected "Basic ..." value; empty = no authentication
  std::function<std::string()> base_url_;
  std::function<std::string()> token_;
  std::vector<Field> fields_;  ///< filled before setup(), read-only afterwards

  std::mutex lock_;  ///< guards everything below that both tasks touch

  // State snapshot: the httpd task asks, loop() builds it and signals.
  std::atomic<bool> state_wanted_{false};
  SemaphoreHandle_t state_ready_{nullptr};
  std::string state_json_;

  // Writes waiting for the main loop: (field index, validated value).
  std::vector<std::pair<size_t, std::string>> pending_;

  // HA entity list. The array lives in PSRAM and is replaced, never modified,
  // so the httpd task can send it after dropping the lock.
  std::atomic<bool> fetch_wanted_{false};
  bool fetch_in_flight_{false};  ///< main loop only
  std::shared_ptr<const PsramString> entities_;  ///< JSON array; null until the first fetch
  std::string entities_status_{"idle"};          ///< idle | loading | ok | error
  std::string entities_error_;
  uint32_t entities_fetched_ms_{0};
};

}  // namespace esp32_dash

#endif  // USE_ESP32_DASH_SETTINGS
