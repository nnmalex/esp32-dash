#include "image_decoder.h"
#include "online_image.h"

#include "esphome/core/application.h"
#include "esphome/core/log.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <utility>

namespace esphome {
namespace online_image {

static const char *const TAG = "online_image.decoder";

bool ImageDecoder::set_size(int width, int height) {
  bool success = this->image_->resize_(width, height) > 0;
  this->x_scale_ = static_cast<double>(this->image_->buffer_width_) / width;
  this->y_scale_ = static_cast<double>(this->image_->buffer_height_) / height;
  return success;
}

void ImageDecoder::draw(int x, int y, int w, int h, const Color &color) {
  auto width = std::min(this->image_->buffer_width_, static_cast<int>(std::ceil((x + w) * this->x_scale_)));
  auto height = std::min(this->image_->buffer_height_, static_cast<int>(std::ceil((y + h) * this->y_scale_)));
  for (int i = x * this->x_scale_; i < width; i++) {
    for (int j = y * this->y_scale_; j < height; j++) {
      this->image_->draw_pixel_(i, j, color);
    }
  }
}

void ImageDecoder::draw_rgb565_block(int x, int y, int w, int h, const uint8_t *data) {
  int bpp_bytes = this->image_->get_bpp() / 8;

  if (this->x_scale_ == 1.0 && this->y_scale_ == 1.0 && bpp_bytes == 2) {
    for (int row = 0; row < h; row++) {
      int dy = y + row;
      if (dy < 0 || dy >= this->image_->buffer_height_)
        continue;
      int start_x = std::max(0, x);
      int end_x = std::min(x + w, this->image_->buffer_width_);
      if (start_x >= end_x)
        continue;
      int copy_w = end_x - start_x;
      int src_offset = (row * w + (start_x - x)) * 2;
      int dst_pos = this->image_->get_position_(start_x, dy);
      memcpy(this->image_->buffer_ + dst_pos, data + src_offset, copy_w * 2);
    }
    return;
  }

  for (int row = 0; row < h; row++) {
    for (int col = 0; col < w; col++) {
      int src_x = x + col;
      int src_y = y + row;
      int src_offset = (row * w + col) * 2;

      auto target_w = std::min(this->image_->buffer_width_,
                               static_cast<int>(std::ceil((src_x + 1) * this->x_scale_)));
      auto target_h = std::min(this->image_->buffer_height_,
                               static_cast<int>(std::ceil((src_y + 1) * this->y_scale_)));
      for (int dy = static_cast<int>(src_y * this->y_scale_); dy < target_h; dy++) {
        for (int dx = static_cast<int>(src_x * this->x_scale_); dx < target_w; dx++) {
          int dst_pos = this->image_->get_position_(dx, dy);
          memcpy(this->image_->buffer_ + dst_pos, data + src_offset, 2);
          if (bpp_bytes > 2) {
            this->image_->buffer_[dst_pos + 2] = 0xFF;
          }
        }
      }
    }
  }
}

void ImageDecoder::write_rgb565_row(int y, const uint8_t *data) {
  if (y < 0 || y >= this->image_->buffer_height_ || this->image_->get_bpp() != 16)
    return;
  memcpy(this->image_->buffer_ + this->image_->get_position_(0, y), data, this->image_->buffer_width_ * 2);
}

size_t ImageDecoder::resample_scratch_size(int src_w, int dst_w) {
  // x-map (uint16 x0 + uint8 weight per output column), two source RGB888 rows,
  // one output RGB565 row.
  return static_cast<size_t>(dst_w) * 3 + static_cast<size_t>(src_w) * 6 + static_cast<size_t>(dst_w) * 2;
}

ImageDecoder::Window ImageDecoder::fit_window(int src_w, int src_h) const {
  Window win{0, 0, src_w, src_h};
  if (this->image_->is_fit_cover()) {
    double s = this->image_->fit_scale(src_w, src_h);
    win.w = std::min(src_w, static_cast<int>(std::lround(this->image_->get_buffer_width() / s)));
    win.h = std::min(src_h, static_cast<int>(std::lround(this->image_->get_buffer_height() / s)));
    win.x = (src_w - win.w) / 2;
    win.y = (src_h - win.h) / 2;
  }
  return win;
}

void HOT ImageDecoder::resample(RowSource next_row, void *ctx, int src_w, int src_h, Window win, uint8_t *scratch) {
  const int dst_w = this->image_->get_buffer_width();
  const int dst_h = this->image_->get_buffer_height();
  const bool big_endian = this->image_->is_big_endian();
  uint16_t *x0s = reinterpret_cast<uint16_t *>(scratch);
  uint8_t *fxs = scratch + static_cast<size_t>(dst_w) * 2;
  uint8_t *prev = fxs + dst_w;
  uint8_t *cur = prev + static_cast<size_t>(src_w) * 3;
  uint8_t *out = cur + static_cast<size_t>(src_w) * 3;

  // Source position of an output pixel centre, in 1/256 px, clamped to the image.
  // `w0`/`w` is the window along this axis; `src` the full source extent.
  auto src_pos = [](int d, int w0, int w, int dst, int src) -> int {
    int p = w0 * 256 + static_cast<int>(static_cast<int64_t>(2 * d + 1) * w * 256 / (2 * dst)) - 128;
    int max = (src - 1) * 256;
    return p < 0 ? 0 : (p > max ? max : p);
  };
  for (int dx = 0; dx < dst_w; dx++) {
    int p = src_pos(dx, win.x, win.w, dst_w, src_w);
    x0s[dx] = static_cast<uint16_t>(p >> 8);
    fxs[dx] = static_cast<uint8_t>(p & 0xFF);
  }

  int have = -1;  // source row held in `cur`; `prev` holds row have - 1
  for (int dy = 0; dy < dst_h; dy++) {
    int p = src_pos(dy, win.y, win.h, dst_h, src_h);
    int y0 = p >> 8;
    int fy = p & 0xFF;
    int y1 = std::min(y0 + 1, src_h - 1);
    while (have < y1) {
      std::swap(prev, cur);
      next_row(ctx, cur);
      have++;
    }
    const uint8_t *r0 = (y0 == have) ? cur : prev;
    const uint8_t *r1 = cur;

    uint8_t *o = out;
    for (int dx = 0; dx < dst_w; dx++) {
      int x0 = x0s[dx];
      int x1 = x0 + 1 < src_w ? x0 + 1 : x0;
      int fx = fxs[dx];
      const uint8_t *a = r0 + x0 * 3, *b = r0 + x1 * 3, *c = r1 + x0 * 3, *d = r1 + x1 * 3;
      int rgb[3];
      for (int ch = 0; ch < 3; ch++) {
        int top = a[ch] * (256 - fx) + b[ch] * fx;
        int bot = c[ch] * (256 - fx) + d[ch] * fx;
        rgb[ch] = (top * (256 - fy) + bot * fy) >> 16;
      }
      uint16_t rgb565 = ((rgb[0] & 0xF8) << 8) | ((rgb[1] & 0xFC) << 3) | (rgb[2] >> 3);
      if (big_endian) {
        o[0] = rgb565 >> 8;
        o[1] = rgb565 & 0xFF;
      } else {
        o[0] = rgb565 & 0xFF;
        o[1] = rgb565 >> 8;
      }
      o += 2;
    }
    this->write_rgb565_row(dy, out);

    if ((dy & 63) == 0) {
      App.feed_wdt();
    }
  }
}

DownloadBuffer::DownloadBuffer(size_t size) : size_(size) {
  this->buffer_ = this->allocator_.allocate(size);
  this->reset();
  if (!this->buffer_) {
    ESP_LOGE(TAG, "Initial allocation of download buffer failed!");
    this->size_ = 0;
  }
}

uint8_t *DownloadBuffer::data(size_t offset) {
  if (offset > this->size_) {
    ESP_LOGE(TAG, "Tried to access beyond download buffer bounds!!!");
    return this->buffer_;
  }
  return this->buffer_ + offset;
}

size_t DownloadBuffer::read(size_t len) {
  this->unread_ -= len;
  if (this->unread_ > 0) {
    memmove(this->data(), this->data(len), this->unread_);
  }
  return this->unread_;
}

size_t DownloadBuffer::resize(size_t size) {
  if (this->size_ >= size) {
    return this->size_;
  }
  uint8_t *new_buffer = this->allocator_.allocate(size);
  if (new_buffer) {
    if (this->buffer_ && this->unread_ > 0) {
      memcpy(new_buffer, this->buffer_, this->unread_);
    }
    this->allocator_.deallocate(this->buffer_, this->size_);
    this->buffer_ = new_buffer;
    this->size_ = size;
    return size;
  } else {
    ESP_LOGE(TAG, "allocation of %zu bytes failed. Biggest block in heap: %zu Bytes", size,
             this->allocator_.get_max_free_block_size());
    this->allocator_.deallocate(this->buffer_, this->size_);
    this->buffer_ = nullptr;
    this->size_ = 0;
    this->reset();
    return 0;
  }
}

}  // namespace online_image
}  // namespace esphome
