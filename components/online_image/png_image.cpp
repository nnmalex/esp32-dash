#include "png_image.h"
#ifdef USE_ONLINE_IMAGE_PNG_SUPPORT

#include "esphome/components/display/display_buffer.h"
#include "esphome/core/application.h"
#include "esphome/core/helpers.h"
#include "esphome/core/log.h"

#include "online_image.h"

#include <algorithm>
#include <cstring>

static const char *const TAG = "online_image.png";

namespace esphome {
namespace online_image {

/**
 * @brief Callback method that will be called by the PNGLE engine when the basic
 * data of the image is received (i.e. width and height);
 *
 * @param pngle The PNGLE object, including the context data.
 * @param w The width of the image.
 * @param h The height of the image.
 */
static void init_callback(pngle_t *pngle, uint32_t w, uint32_t h) {
  PngDecoder *decoder = (PngDecoder *) pngle_get_user_data(pngle);
  decoder->on_header(w, h);
}

/**
 * @brief Callback method that will be called by the PNGLE engine when a chunk
 * of the image is decoded.
 *
 * @param pngle The PNGLE object, including the context data.
 * @param x The X coordinate to draw the rectangle on.
 * @param y The Y coordinate to draw the rectangle on.
 * @param w The width of the rectangle to draw.
 * @param h The height of the rectangle to draw.
 * @param rgba The color to paint the rectangle in.
 */
static void draw_callback(pngle_t *pngle, uint32_t x, uint32_t y, uint32_t w, uint32_t h, const uint8_t rgba[4]) {
  PngDecoder *decoder = (PngDecoder *) pngle_get_user_data(pngle);
  decoder->on_pixels(x, y, w, h, rgba);

  // Feed watchdog periodically to avoid triggering during long decode operations.
  // Feed every 1024 pixels to balance efficiency and responsiveness.
  uint32_t pixels = w * h;
  decoder->increment_pixels_decoded(pixels);
  if ((decoder->get_pixels_decoded() % 1024) < pixels) {
    App.feed_wdt();
  }
}

static void done_callback(pngle_t *pngle) {
  PngDecoder *decoder = (PngDecoder *) pngle_get_user_data(pngle);
  decoder->on_done();
}

// Largest source buffered for the smooth resample (RGB888: 7.7 MB of PSRAM).
// Bigger PNGs fall back to draw()'s nearest-neighbour scaling.
static constexpr uint64_t FRAME_MAX_PIXELS = 1600ULL * 1600ULL;

void PngDecoder::on_header(uint32_t w, uint32_t h) {
  this->free_frame_();
  if (!this->set_size(w, h))
    return;
  // Buffer the source only when it will actually be resampled, into an opaque
  // RGB565 image (alpha and other formats keep the per-pixel path).
  const auto *img = this->image_;
  bool resized = img->get_buffer_width() != static_cast<int>(w) ||
                 img->get_buffer_height() != static_cast<int>(h) || img->is_fit_cover();
  if (!resized || img->image_type() != image::ImageType::IMAGE_TYPE_RGB565 || img->has_transparency() ||
      static_cast<uint64_t>(w) * h > FRAME_MAX_PIXELS)
    return;
  this->frame_ = this->frame_allocator_.allocate(static_cast<size_t>(w) * h * 3);
  if (this->frame_ == nullptr) {
    ESP_LOGW(TAG, "No memory to buffer a %ux%u PNG; scaling without smoothing", (unsigned) w, (unsigned) h);
    return;
  }
  this->frame_w_ = static_cast<int>(w);
  this->frame_h_ = static_cast<int>(h);
}

void PngDecoder::on_pixels(uint32_t x, uint32_t y, uint32_t w, uint32_t h, const uint8_t rgba[4]) {
  if (this->frame_ == nullptr) {
    this->draw(x, y, w, h, Color(rgba[0], rgba[1], rgba[2], rgba[3]));
    return;
  }
  // Interlaced passes paint blocks larger than a pixel; clip to the frame.
  uint32_t x_end = std::min<uint32_t>(x + w, this->frame_w_);
  uint32_t y_end = std::min<uint32_t>(y + h, this->frame_h_);
  for (uint32_t yy = y; yy < y_end; yy++) {
    uint8_t *p = this->frame_ + (static_cast<size_t>(yy) * this->frame_w_ + x) * 3;
    for (uint32_t xx = x; xx < x_end; xx++, p += 3) {
      p[0] = rgba[0];
      p[1] = rgba[1];
      p[2] = rgba[2];
    }
  }
}

void PngDecoder::on_done() {
  if (this->frame_ == nullptr)
    return;
  uint8_t *scratch =
      static_cast<uint8_t *>(malloc(resample_scratch_size(this->frame_w_, this->image_->get_buffer_width())));
  if (scratch != nullptr) {
    struct Rows {
      const uint8_t *frame;
      size_t stride;
      int next;
    } rows{this->frame_, static_cast<size_t>(this->frame_w_) * 3, 0};
    this->resample(
        [](void *ctx, uint8_t *row) {
          auto *r = static_cast<Rows *>(ctx);
          memcpy(row, r->frame + static_cast<size_t>(r->next++) * r->stride, r->stride);
        },
        &rows, this->frame_w_, this->frame_h_, this->fit_window(this->frame_w_, this->frame_h_), scratch);
    free(scratch);
  } else {
    ESP_LOGW(TAG, "No memory to resample PNG");
  }
  this->free_frame_();
}

void PngDecoder::free_frame_() {
  if (this->frame_ != nullptr) {
    this->frame_allocator_.deallocate(this->frame_, static_cast<size_t>(this->frame_w_) * this->frame_h_ * 3);
    this->frame_ = nullptr;
  }
}

PngDecoder::PngDecoder(OnlineImage *image) : ImageDecoder(image) {
  {
    pngle_t *pngle = this->allocator_.allocate(1, PNGLE_T_SIZE);
    if (!pngle) {
      ESP_LOGE(TAG, "Failed to allocate memory for PNGLE engine!");
      return;
    }
    memset(pngle, 0, PNGLE_T_SIZE);
    pngle_reset(pngle);
    this->pngle_ = pngle;
  }
}

PngDecoder::~PngDecoder() {
  this->free_frame_();
  if (this->pngle_) {
    pngle_reset(this->pngle_);
    this->allocator_.deallocate(this->pngle_, PNGLE_T_SIZE);
  }
}

int PngDecoder::prepare(size_t download_size) {
  ImageDecoder::prepare(download_size);
  if (!this->pngle_) {
    ESP_LOGE(TAG, "PNG decoder engine not initialized!");
    return DECODE_ERROR_OUT_OF_MEMORY;
  }
  pngle_set_user_data(this->pngle_, this);
  pngle_set_init_callback(this->pngle_, init_callback);
  pngle_set_draw_callback(this->pngle_, draw_callback);
  pngle_set_done_callback(this->pngle_, done_callback);
  return 0;
}

int HOT PngDecoder::decode(uint8_t *buffer, size_t size) {
  if (!this->pngle_) {
    ESP_LOGE(TAG, "PNG decoder engine not initialized!");
    return DECODE_ERROR_OUT_OF_MEMORY;
  }
  if (size < 256 && size < this->download_size_ - this->decoded_bytes_) {
    ESP_LOGD(TAG, "Waiting for data");
    return 0;
  }
  auto fed = pngle_feed(this->pngle_, buffer, size);
  if (fed < 0) {
    ESP_LOGE(TAG, "Error decoding image: %s", pngle_error(this->pngle_));
  } else {
    this->decoded_bytes_ += fed;
  }
  return fed;
}

}  // namespace online_image
}  // namespace esphome

#endif  // USE_ONLINE_IMAGE_PNG_SUPPORT
