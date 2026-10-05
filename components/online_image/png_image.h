#pragma once

#include "esphome/core/defines.h"
#include "esphome/core/helpers.h"
#include "image_decoder.h"
#ifdef USE_ONLINE_IMAGE_PNG_SUPPORT
#include <pngle.h>

namespace esphome {
namespace online_image {

/**
 * @brief Image decoder specialization for PNG images.
 */
class PngDecoder : public ImageDecoder {
 public:
  /**
   * @brief Construct a new PNG Decoder object.
   *
   * @param display The image to decode the stream into.
   */
  PngDecoder(OnlineImage *image);
  ~PngDecoder() override;

  int prepare(size_t download_size) override;
  int HOT decode(uint8_t *buffer, size_t size) override;

  void increment_pixels_decoded(uint32_t count) { this->pixels_decoded_ += count; }
  uint32_t get_pixels_decoded() const { return this->pixels_decoded_; }

  /// pngle callbacks (see png_image.cpp).
  void on_header(uint32_t w, uint32_t h);
  void on_pixels(uint32_t x, uint32_t y, uint32_t w, uint32_t h, const uint8_t rgba[4]);
  void on_done();

 protected:
  void free_frame_();

  RAMAllocator<pngle_t> allocator_;
  pngle_t *pngle_;
  uint32_t pixels_decoded_{0};

  /// Full-size RGB888 copy of the source, kept when the image is stored at a
  /// different size: pixels arrive in pngle's order (interlaced passes out of
  /// order), so the bilinear resample runs once the whole image is in. Null
  /// when the image is stored as-is or too big, which uses draw() instead.
  RAMAllocator<uint8_t> frame_allocator_{};
  uint8_t *frame_{nullptr};
  int frame_w_{0};
  int frame_h_{0};
};

}  // namespace online_image
}  // namespace esphome

#endif  // USE_ONLINE_IMAGE_PNG_SUPPORT
