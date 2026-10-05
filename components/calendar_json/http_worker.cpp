#include "http_worker.h"

#include <esp_http_client.h>
#include <esp_timer.h>
#include <freertos/task.h>
#if CONFIG_MBEDTLS_CERTIFICATE_BUNDLE
#include <esp_crt_bundle.h>
#endif

#include "esphome/core/log.h"

namespace esp32_dash {

static const char *const TAG = "http_worker";

// TLS handshakes need a few KB of stack; bodies live on the heap.
static constexpr uint32_t WORKER_STACK = 12288;
static constexpr UBaseType_t QUEUE_LEN = 8;
static constexpr size_t READ_CHUNK = 4096;

void HttpWorker::setup() {
  this->todo_ = xQueueCreate(QUEUE_LEN, sizeof(Job *));
  this->done_ = xQueueCreate(QUEUE_LEN, sizeof(Job *));
  if (this->todo_ == nullptr || this->done_ == nullptr ||
      xTaskCreate(&HttpWorker::worker_task_, "http_worker", WORKER_STACK, this, 1, nullptr) != pdPASS) {
    ESP_LOGE(TAG, "Could not start the HTTP worker task");
    this->mark_failed();
  }
}

bool HttpWorker::request(std::string url, std::string body, HttpHeaders headers, HttpCallback done) {
  if (this->todo_ == nullptr)
    return false;
  auto *job = new Job{std::move(url), std::move(body), std::move(headers), std::move(done), {}};
  if (xQueueSend(this->todo_, &job, 0) != pdTRUE) {
    ESP_LOGW(TAG, "Queue full, dropping request to %s", job->url.c_str());
    delete job;
    return false;
  }
  this->pending_++;
  return true;
}

void HttpWorker::loop() {
  Job *job;
  while (this->done_ != nullptr && xQueueReceive(this->done_, &job, 0) == pdTRUE) {
    this->pending_--;
    const HttpResponse &r = job->response;
    if (!r.error.empty()) {
      ESP_LOGW(TAG, "%s failed after %u ms: %s", job->url.c_str(), (unsigned) r.duration_ms, r.error.c_str());
    } else {
      ESP_LOGD(TAG, "%s -> HTTP %d, %u bytes in %u ms (off the main loop)", job->url.c_str(), r.status,
               (unsigned) r.body.size(), (unsigned) r.duration_ms);
    }
    if (job->done)
      job->done(r);
    delete job;
  }
}

void HttpWorker::worker_task_(void *self) {
  auto *worker = static_cast<HttpWorker *>(self);
  Job *job;
  for (;;) {
    if (xQueueReceive(worker->todo_, &job, portMAX_DELAY) != pdTRUE)
      continue;
    worker->run_(job);
    // done_ has the same depth as todo_ and holds at most the jobs taken from
    // it, so this cannot block for long; loop() drains it every iteration.
    xQueueSend(worker->done_, &job, portMAX_DELAY);
  }
}

// Runs on the worker task: touches only `job` and the ESP-IDF client.
void HttpWorker::run_(Job *job) const {
  HttpResponse &r = job->response;
  const int64_t start_us = esp_timer_get_time();

  esp_http_client_config_t cfg = {};
  cfg.url = job->url.c_str();
  cfg.method = job->body.empty() ? HTTP_METHOD_GET : HTTP_METHOD_POST;
  cfg.timeout_ms = static_cast<int>(this->timeout_ms_);
  cfg.buffer_size = READ_CHUNK;
  cfg.buffer_size_tx = 1024;
  // Same TLS policy as ESPHome's http_request, which configures the matching
  // sdkconfig (certificate bundle, or insecure mode when verify_ssl is false).
#if CONFIG_MBEDTLS_CERTIFICATE_BUNDLE
  if (this->verify_ssl_ && job->url.rfind("https:", 0) == 0)
    cfg.crt_bundle_attach = esp_crt_bundle_attach;
#endif

  esp_http_client_handle_t client = esp_http_client_init(&cfg);
  if (client == nullptr) {
    r.error = "client init failed";
    r.duration_ms = (esp_timer_get_time() - start_us) / 1000;
    return;
  }
  for (const auto &h : job->headers)
    esp_http_client_set_header(client, h.first.c_str(), h.second.c_str());

  do {
    esp_err_t err = esp_http_client_open(client, static_cast<int>(job->body.size()));
    if (err != ESP_OK) {
      r.error = esp_err_to_name(err);
      break;
    }
    if (!job->body.empty() &&
        esp_http_client_write(client, job->body.data(), static_cast<int>(job->body.size())) < 0) {
      r.error = "write failed";
      break;
    }
    int64_t length = esp_http_client_fetch_headers(client);
    if (length < 0) {
      r.error = "no response headers";
      break;
    }
    r.status = esp_http_client_get_status_code(client);
    if (length > 0)
      r.body.reserve(static_cast<size_t>(length));

    std::vector<char> chunk(READ_CHUNK);
    for (;;) {
      int n = esp_http_client_read(client, chunk.data(), static_cast<int>(chunk.size()));
      if (n < 0) {
        r.error = "read failed";
        break;
      }
      if (n == 0)
        break;  // end of body (also for chunked transfer encoding)
      r.body.append(chunk.data(), n);
    }
  } while (false);

  esp_http_client_close(client);
  esp_http_client_cleanup(client);
  r.duration_ms = (esp_timer_get_time() - start_us) / 1000;
}

}  // namespace esp32_dash
