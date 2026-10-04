#include "jpeg_image.h"
#ifdef USE_ONLINE_IMAGE_JPEG_SUPPORT

#include "esphome/components/display/display_buffer.h"
#include "esphome/core/application.h"
#include "esphome/core/helpers.h"
#include "esphome/core/log.h"

#include "online_image.h"

#include <algorithm>
#include <cmath>
#include <utility>

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

size_t JpegDecoder::resample_scratch_size(int src_w, int dst_w) {
  // x-map (uint16 x0 + uint8 weight per output column), two source RGB888 rows,
  // one output RGB565 row.
  return static_cast<size_t>(dst_w) * 3 + static_cast<size_t>(src_w) * 6 + static_cast<size_t>(dst_w) * 2;
}

// Bilinear resample of `win` (the whole image, or the centred crop for
// fit: cover) from the decoder's RGB888 scanlines straight into the RGB565
// image buffer. It is streamed: output rows walk the source top to bottom, so
// only the two source rows bracketing the current output row are held, never
// the whole frame. The IDCT step keeps any downscale under 2x, which is the
// range a 2x2 bilinear tap covers without dropping pixels.
void HOT JpegDecoder::decode_resampled_(jpeg_decompress_struct *cinfo, uint8_t *scratch, int dst_w, int dst_h,
                                        Window win, bool big_endian) {
  const int src_w = static_cast<int>(cinfo->output_width);
  const int src_h = static_cast<int>(cinfo->output_height);
  uint16_t *x0s = reinterpret_cast<uint16_t *>(scratch);
  uint8_t *fxs = scratch + static_cast<size_t>(dst_w) * 2;
  uint8_t *prev = fxs + dst_w;
  uint8_t *cur = prev + static_cast<size_t>(src_w) * 3;
  uint8_t *out = cur + static_cast<size_t>(src_w) * 3;

  // Source position of an output pixel centre, in 1/256 px, clamped to the image.
  // `w0`/`w` is the window along this axis; `src` the full decoded extent.
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
      JSAMPROW row = cur;
      jpeg_read_scanlines(cinfo, &row, 1);
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

  // jpeg_finish_decompress() rejects a stream with unread scanlines.
  while (cinfo->output_scanline < cinfo->output_height) {
    JSAMPROW row = cur;
    jpeg_read_scanlines(cinfo, &row, 1);
  }
}

int HOT JpegDecoder::decode(uint8_t *buffer, size_t size) {
  if (size < this->download_size_) {
    ESP_LOGV(TAG, "Download not complete. Size: %zu/%zu", size, this->download_size_);
    return 0;
  }

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
  Window win{0, 0, out_w, out_h};
  if (this->image_->is_fit_cover()) {
    double s = this->image_->fit_scale(out_w, out_h);
    win.w = std::min(out_w, static_cast<int>(std::lround(dst_w / s)));
    win.h = std::min(out_h, static_cast<int>(std::lround(dst_h / s)));
    win.x = (out_w - win.w) / 2;
    win.y = (out_h - win.h) / 2;
  }

  if (use_rgb565 && (dst_w != out_w || dst_h != out_h || win.w != out_w || win.h != out_h)) {
    // Scratch for the resample, held in row_buffer so the setjmp handler above
    // frees it if libjpeg bails out part-way.
    row_buffer = static_cast<uint8_t *>(malloc(resample_scratch_size(out_w, dst_w)));
    if (row_buffer == nullptr) {
      jpeg_destroy_decompress(&cinfo);
      return DECODE_ERROR_OUT_OF_MEMORY;
    }
    ESP_LOGD(TAG, "Resampling %dx%d -> %dx%d", out_w, out_h, dst_w, dst_h);
    this->decode_resampled_(&cinfo, row_buffer, dst_w, dst_h, win, big_endian);
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
