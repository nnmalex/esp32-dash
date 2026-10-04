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
  static size_t resample_scratch_size(int src_w, int dst_w);
  /// Window of the decoded image (in IDCT-output pixels) that gets resampled.
  struct Window {
    int x, y, w, h;
  };
  void decode_resampled_(jpeg_decompress_struct *cinfo, uint8_t *scratch, int dst_w, int dst_h, Window win,
                         bool big_endian);
};

}  // namespace online_image
}  // namespace esphome

#endif  // USE_ONLINE_IMAGE_JPEG_SUPPORT
