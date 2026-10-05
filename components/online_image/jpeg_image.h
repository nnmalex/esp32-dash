#pragma once

#include "image_decoder.h"
#include "esphome/core/defines.h"
#ifdef USE_ONLINE_IMAGE_JPEG_SUPPORT
#include <jpeglib.h>
#include <csetjmp>

namespace esphome {
namespace online_image {

/**
 * @brief Image decoder specialization for JPEG images.
 *
 * On chips with a JPEG codec (ESP32-P4) baseline JPEGs are decoded in
 * hardware; anything it cannot handle (progressive, oversized, any driver
 * error) falls back to libjpeg-turbo.
 */
class JpegDecoder : public ImageDecoder {
 public:
  /**
   * @brief Construct a new JPEG Decoder object.
   *
   * @param display The image to decode the stream into.
   */
  JpegDecoder(OnlineImage *image) : ImageDecoder(image) {}
  ~JpegDecoder() override {}

  int prepare(size_t download_size) override;
  int HOT decode(uint8_t *buffer, size_t size) override;

 protected:
  /// Hardware decode into the image buffer. False (with nothing to clean up)
  /// when the hardware cannot take this image, so the caller falls back.
  bool decode_hw_(const uint8_t *buffer, size_t size);
  int decode_sw_(uint8_t *buffer, size_t size);
};

}  // namespace online_image
}  // namespace esphome

#endif  // USE_ONLINE_IMAGE_JPEG_SUPPORT
