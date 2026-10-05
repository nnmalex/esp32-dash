#pragma once

#include <functional>
#include <string>
#include <utility>
#include <vector>

#include <freertos/FreeRTOS.h>
#include <freertos/queue.h>

#include "esphome/core/component.h"

namespace esp32_dash {

/// Result of one HttpWorker request, delivered on the main loop.
struct HttpResponse {
  int status{-1};          ///< HTTP status code; -1 if the request never got a response
  std::string body;        ///< full response body
  std::string error;       ///< transport error (empty when a response arrived)
  uint32_t duration_ms{0}; ///< wall time spent in the worker for this request
};

using HttpHeaders = std::vector<std::pair<std::string, std::string>>;
using HttpCallback = std::function<void(const HttpResponse &)>;

/// Runs HTTP requests on a dedicated FreeRTOS task so they never block the
/// main loop (ESPHome's http_request is synchronous, and a calendar fetch can
/// take seconds while HA waits on the upstream provider).
///
/// Thread discipline: the worker task only touches the Job it was handed and
/// ESP-IDF's HTTP client. Finished jobs go back through a queue that loop()
/// drains, so every callback runs on the main loop and may freely use LVGL,
/// globals and other components.
class HttpWorker : public esphome::Component {
 public:
  void setup() override;
  void loop() override;
  float get_setup_priority() const override { return esphome::setup_priority::AFTER_WIFI; }

  void set_verify_ssl(bool verify) { this->verify_ssl_ = verify; }
  void set_timeout(uint32_t timeout_ms) { this->timeout_ms_ = timeout_ms; }

  /// Queue a GET (empty body) or POST (non-empty body). Returns false, without
  /// calling `done`, if the worker is not running or its queue is full.
  bool request(std::string url, std::string body, HttpHeaders headers, HttpCallback done);

  /// Requests queued or running whose callbacks have not yet been delivered.
  int pending() const { return this->pending_; }

 protected:
  struct Job {
    std::string url;
    std::string body;
    HttpHeaders headers;
    HttpCallback done;  // only ever invoked or destroyed on the main loop
    HttpResponse response;
  };

  static void worker_task_(void *self);
  void run_(Job *job) const;

  QueueHandle_t todo_{nullptr};
  QueueHandle_t done_{nullptr};
  bool verify_ssl_{true};
  uint32_t timeout_ms_{15000};
  int pending_{0};
};

}  // namespace esp32_dash
