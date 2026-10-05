#include "jpeg_image.h"
#ifdef USE_ONLINE_IMAGE_JPEG_SUPPORT

#include "esphome/components/display/display_buffer.h"
#include "esphome/core/application.h"
#include "esphome/core/helpers.h"
#include "esphome/core/log.h"

#include "online_image.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <utility>
#include <vector>

#include <esp_timer.h>
#include "soc/soc_caps.h"
#if SOC_JPEG_DECODE_SUPPORTED && __has_include(<driver/jpeg_decode.h>)
#define ONLINE_IMAGE_HW_JPEG 1
#include <driver/jpeg_decode.h>
#include <esp_heap_caps.h>
#endif

static const char *const TAG = "online_image.jpeg";

namespace esphome {
namespace online_image {

/// Custom error manager that longjmps instead of calling exit()
struct JpegErrorMgr {
  jpeg_error_mgr pub;
  jmp_buf setjmp_buffer;
  char message[JMSG_LENGTH_MAX];
};

static void jpeg_error_exit(j_common_ptr cinfo) {
  auto *err = reinterpret_cast<JpegErrorMgr *>(cinfo->err);
  (*(cinfo->err->format_message))(cinfo, err->message);
  longjmp(err->setjmp_buffer, 1);
}

static constexpr size_t MAX_JPEG_DOWNLOAD_SIZE = 4 * 1024 * 1024;  // 4 MB

int JpegDecoder::prepare(size_t download_size) {
  if (download_size > MAX_JPEG_DOWNLOAD_SIZE) {
    ESP_LOGE(TAG, "JPEG too large to decode: %zu bytes (max %zu). Consider using a smaller image URL.",
             download_size, MAX_JPEG_DOWNLOAD_SIZE);
    return DECODE_ERROR_OUT_OF_MEMORY;
  }
  ImageDecoder::prepare(download_size);
  auto size = this->image_->resize_download_buffer(download_size);
  if (size < download_size) {
    ESP_LOGE(TAG, "Download buffer resize failed!");
    return DECODE_ERROR_OUT_OF_MEMORY;
  }
  return 0;
}

#ifdef ONLINE_IMAGE_HW_JPEG
// Largest source the hardware path takes. Bigger images go to libjpeg, whose
// IDCT scaling avoids materialising them at full size (an RGB565 frame this
// big is already 8 MB of PSRAM).
static constexpr uint64_t HW_MAX_PIXELS = 2048ULL * 2048ULL;
// Created on first use and kept: it owns DMA descriptors and an interrupt.
static jpeg_decoder_handle_t hw_engine = nullptr;

bool JpegDecoder::decode_hw_(const uint8_t *buffer, size_t size) {
  if (this->image_->image_type() != image::ImageType::IMAGE_TYPE_RGB565)
    return false;
  jpeg_decode_picture_info_t info;
  if (jpeg_decoder_get_info(buffer, size, &info) != ESP_OK)
    return false;
  const int w = static_cast<int>(info.width), h = static_cast<int>(info.height);
  if (w <= 0 || h <= 0 || static_cast<uint64_t>(w) * h > HW_MAX_PIXELS)
    return false;

  // The decoder writes whole MCUs, so its frame is padded: 16 px along a
  // chroma-subsampled axis, 8 otherwise. The padded width is the row stride.
  const bool sub_x = info.sample_method == JPEG_DOWN_SAMPLING_YUV420 ||
                     info.sample_method == JPEG_DOWN_SAMPLING_YUV422;
  const bool sub_y = info.sample_method == JPEG_DOWN_SAMPLING_YUV420;
  const int mcu_x = sub_x ? 16 : 8, mcu_y = sub_y ? 16 : 8;
  const int pad_w = (w + mcu_x - 1) / mcu_x * mcu_x;
  const int pad_h = (h + mcu_y - 1) / mcu_y * mcu_y;
  const size_t frame_bytes = static_cast<size_t>(pad_w) * pad_h * 2;

  if (hw_engine == nullptr) {
    jpeg_decode_engine_cfg_t engine_cfg = {};
    engine_cfg.timeout_ms = 1000;
    if (jpeg_new_decoder_engine(&engine_cfg, &hw_engine) != ESP_OK) {
      ESP_LOGW(TAG, "Hardware JPEG decoder unavailable, using libjpeg");
      hw_engine = nullptr;
      return false;
    }
  }

  // DMA-capable, cache-aligned buffers in PSRAM, as the driver requires.
  jpeg_decode_memory_alloc_cfg_t in_cfg = {};
  in_cfg.buffer_direction = JPEG_DEC_ALLOC_INPUT_BUFFER;
  jpeg_decode_memory_alloc_cfg_t out_cfg = {};
  out_cfg.buffer_direction = JPEG_DEC_ALLOC_OUTPUT_BUFFER;
  size_t in_cap = 0, out_cap = 0;
  auto *in = static_cast<uint8_t *>(jpeg_alloc_decoder_mem(size, &in_cfg, &in_cap));
  auto *frame = static_cast<uint8_t *>(jpeg_alloc_decoder_mem(frame_bytes, &out_cfg, &out_cap));
  bool ok = false;

  if (in != nullptr && frame != nullptr) {
    memcpy(in, buffer, size);
    jpeg_decode_cfg_t cfg = {};
    cfg.output_format = JPEG_DECODE_OUT_FORMAT_RGB565;
    cfg.rgb_order = JPEG_DEC_RGB_ELEMENT_ORDER_BGR;  // "small endian": little-endian RGB565
    cfg.conv_std = JPEG_YUV_RGB_CONV_STD_BT601;
    uint32_t out_len = 0;
    const int64_t t0 = esp_timer_get_time();
    esp_err_t err = jpeg_decoder_process(hw_engine, &cfg, in, size, frame, out_cap, &out_len);
    const int decode_ms = static_cast<int>((esp_timer_get_time() - t0) / 1000);

    // Unsupported streams (progressive, greyscale, ...) fail here; a length
    // that is not the padded frame would mean the stride assumption is wrong.
    if (err != ESP_OK || out_len != frame_bytes) {
      ESP_LOGD(TAG, "Hardware JPEG decode declined (%s, %u bytes), using libjpeg", esp_err_to_name(err),
               (unsigned) out_len);
    } else if (this->set_size(w, h)) {
      const int dst_w = this->image_->get_buffer_width();
      const int dst_h = this->image_->get_buffer_height();
      const Window win = this->fit_window(w, h);
      const bool big_endian = this->image_->is_big_endian();

      if (dst_w == w && dst_h == h && win.w == w && win.h == h) {
        // Already the stored size: copy rows across the padded stride.
        std::vector<uint8_t> row(static_cast<size_t>(w) * 2);
        for (int y = 0; y < h; y++) {
          const uint8_t *src = frame + static_cast<size_t>(y) * pad_w * 2;
          if (big_endian) {
            for (int x = 0; x < w * 2; x += 2) {
              row[x] = src[x + 1];
              row[x + 1] = src[x];
            }
            this->write_rgb565_row(y, row.data());
          } else {
            this->write_rgb565_row(y, src);
          }
        }
        ok = true;
      } else {
        auto *scratch = static_cast<uint8_t *>(malloc(resample_scratch_size(w, dst_w)));
        if (scratch != nullptr) {
          struct Rows {
            const uint8_t *frame;
            size_t stride;  // bytes per padded row
            int width;      // visible pixels per row (the resampler's row size)
            int next;
          } rows{frame, static_cast<size_t>(pad_w) * 2, w, 0};
          // Expand the visible part of one little-endian RGB565 row to RGB888.
          auto next_row = [](void *ctx, uint8_t *rgb) {
            auto *r = static_cast<Rows *>(ctx);
            const uint8_t *src = r->frame + static_cast<size_t>(r->next++) * r->stride;
            for (int i = 0; i < r->width; i++) {
              uint16_t c = src[2 * i] | (src[2 * i + 1] << 8);
              uint8_t r5 = c >> 11, g6 = (c >> 5) & 0x3F, b5 = c & 0x1F;
              rgb[3 * i + 0] = (r5 << 3) | (r5 >> 2);
              rgb[3 * i + 1] = (g6 << 2) | (g6 >> 4);
              rgb[3 * i + 2] = (b5 << 3) | (b5 >> 2);
            }
          };
          this->resample(next_row, &rows, w, h, win, scratch);
          free(scratch);
          ok = true;
        }
      }
      if (ok) {
        ESP_LOGD(TAG, "Hardware JPEG decode %dx%d in %d ms", w, h, decode_ms);
      }
    }
  }

  heap_caps_free(in);
  heap_caps_free(frame);
  return ok;
}
#else
bool JpegDecoder::decode_hw_(const uint8_t *, size_t) { return false; }
#endif

int HOT JpegDecoder::decode(uint8_t *buffer, size_t size) {
  if (size < this->download_size_) {
    ESP_LOGV(TAG, "Download not complete. Size: %zu/%zu", size, this->download_size_);
    return 0;
  }
  if (this->decode_hw_(buffer, size)) {
    this->decoded_bytes_ = size;
    return size;
  }
  return this->decode_sw_(buffer, size);
}

int HOT JpegDecoder::decode_sw_(uint8_t *buffer, size_t size) {
  jpeg_decompress_struct cinfo;
  JpegErrorMgr jerr{};

  cinfo.err = jpeg_std_error(&jerr.pub);
  jerr.pub.error_exit = jpeg_error_exit;

  // Raw pointer for longjmp safety — unique_ptr destructors are skipped by longjmp.
  // volatile: it is assigned after setjmp() and read in the handler.
  uint8_t *volatile row_buffer = nullptr;

  if (setjmp(jerr.setjmp_buffer)) {
    ESP_LOGE(TAG, "JPEG decode error: %s", jerr.message);
    free(row_buffer);
    jpeg_destroy_decompress(&cinfo);
    return DECODE_ERROR_UNSUPPORTED_FORMAT;
  }

  jpeg_create_decompress(&cinfo);
  jpeg_mem_src(&cinfo, buffer, size);

  if (jpeg_read_header(&cinfo, TRUE) != JPEG_HEADER_OK) {
    ESP_LOGE(TAG, "Could not read JPEG header");
    jpeg_destroy_decompress(&cinfo);
    return DECODE_ERROR_INVALID_TYPE;
  }

  int src_w = cinfo.image_width;
  int src_h = cinfo.image_height;
  ESP_LOGD(TAG, "JPEG header: %dx%d, components=%d, progressive=%s",
           src_w, src_h, cinfo.num_components,
           cinfo.progressive_mode ? "yes" : "no");

  // Request RGB output regardless of input colorspace
  cinfo.out_color_space = JCS_RGB;
  // Use fast integer IDCT — slightly lower quality but faster on ESP32
  // and avoids pulling in the float IDCT code path.
  cinfo.dct_method = JDCT_IFAST;

  // The whole image scaled as it will be stored: inside the box (contain), or
  // filling it before the crop (cover).
  double scale = this->image_->fit_scale(src_w, src_h);
  int fit_w = static_cast<int>(std::ceil(src_w * scale));
  int fit_h = static_cast<int>(std::ceil(src_h * scale));

  // libjpeg can downscale for free inside the IDCT (1/1, 1/2, 1/4, 1/8). Take the
  // smallest step that still covers the fitted size, so the resample below only
  // ever shrinks by less than 2x and never upscales a large source.
  constexpr unsigned int denoms[] = {8, 4, 2, 1};
  for (unsigned int denom : denoms) {
    cinfo.scale_num = 1;
    cinfo.scale_denom = denom;
    jpeg_calc_output_dimensions(&cinfo);
    if (static_cast<int>(cinfo.output_width) >= fit_w && static_cast<int>(cinfo.output_height) >= fit_h)
      break;
  }

  int out_w = cinfo.output_width;
  int out_h = cinfo.output_height;
  if (out_w != src_w || out_h != src_h) {
    ESP_LOGD(TAG, "Using IDCT downscale: %dx%d -> %dx%d", src_w, src_h, out_w, out_h);
  }

  if (!this->set_size(out_w, out_h)) {
    jpeg_destroy_decompress(&cinfo);
    return DECODE_ERROR_OUT_OF_MEMORY;
  }

  bool use_rgb565 = (this->image_->image_type() == image::ImageType::IMAGE_TYPE_RGB565);
  bool big_endian = this->image_->is_big_endian();
  int dst_w = this->image_->get_buffer_width();
  int dst_h = this->image_->get_buffer_height();

  jpeg_start_decompress(&cinfo);

  // Cover crops the overflow evenly from both sides; contain keeps it all.
  Window win = this->fit_window(out_w, out_h);

  if (use_rgb565 && (dst_w != out_w || dst_h != out_h || win.w != out_w || win.h != out_h)) {
    // Scratch for the resample, held in row_buffer so the setjmp handler above
    // frees it if libjpeg bails out part-way.
    row_buffer = static_cast<uint8_t *>(malloc(resample_scratch_size(out_w, dst_w)));
    if (row_buffer == nullptr) {
      jpeg_destroy_decompress(&cinfo);
      return DECODE_ERROR_OUT_OF_MEMORY;
    }
    ESP_LOGD(TAG, "Resampling %dx%d -> %dx%d", out_w, out_h, dst_w, dst_h);
    this->resample(
        [](void *ctx, uint8_t *row) {
          JSAMPROW r = row;
          jpeg_read_scanlines(static_cast<j_decompress_ptr>(ctx), &r, 1);
        },
        &cinfo, out_w, out_h, win, row_buffer);
    // jpeg_finish_decompress() rejects a stream with unread scanlines.
    while (cinfo.output_scanline < cinfo.output_height) {
      JSAMPROW r = row_buffer;
      jpeg_read_scanlines(&cinfo, &r, 1);
    }
    jpeg_finish_decompress(&cinfo);
    jpeg_destroy_decompress(&cinfo);
    free(row_buffer);
    this->decoded_bytes_ = size;
    return size;
  }

  // Allocate row buffers (raw pointers — safe across longjmp)
  size_t row_stride = static_cast<size_t>(out_w) * 3;
  row_buffer = static_cast<uint8_t *>(malloc(row_stride));
  if (row_buffer == nullptr) {
    jpeg_destroy_decompress(&cinfo);
    return DECODE_ERROR_OUT_OF_MEMORY;
  }

  int y = 0;
  while (cinfo.output_scanline < cinfo.output_height) {
    uint8_t *row_ptr = row_buffer;
    jpeg_read_scanlines(&cinfo, &row_ptr, 1);

    if ((y & 63) == 0) {
      App.feed_wdt();
    }

    if (use_rgb565) {
      // Convert RGB888 -> RGB565 in-place (2 bpp fits within the 3 bpp
      // source buffer, so no separate allocation needed).  We read forward
      // and write forward; the write pointer never overtakes the read
      // pointer because 2 < 3.
      uint8_t *dst = row_buffer;
      for (int x = 0; x < out_w; x++) {
        uint8_t r = row_buffer[x * 3 + 0];
        uint8_t g = row_buffer[x * 3 + 1];
        uint8_t b = row_buffer[x * 3 + 2];
        uint16_t rgb565 = ((r & 0xF8) << 8) | ((g & 0xFC) << 3) | (b >> 3);
        if (big_endian) {
          dst[0] = rgb565 >> 8;
          dst[1] = rgb565 & 0xFF;
        } else {
          dst[0] = rgb565 & 0xFF;
          dst[1] = rgb565 >> 8;
        }
        dst += 2;
      }
      this->draw_rgb565_block(0, y, out_w, 1, row_buffer);
    } else {
      // Per-pixel draw for other image types
      for (int x = 0; x < out_w; x++) {
        Color color(row_buffer[x * 3 + 0], row_buffer[x * 3 + 1], row_buffer[x * 3 + 2]);
        this->draw(x, y, 1, 1, color);
      }
    }
    y++;
  }

  jpeg_finish_decompress(&cinfo);
  jpeg_destroy_decompress(&cinfo);
  free(row_buffer);

  this->decoded_bytes_ = size;
  return size;
}

}  // namespace online_image
}  // namespace esphome

#endif  // USE_ONLINE_IMAGE_JPEG_SUPPORT
