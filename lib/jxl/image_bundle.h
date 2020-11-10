// Copyright (c) the JPEG XL Project
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//      http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

#ifndef LIB_JXL_IMAGE_BUNDLE_H_
#define LIB_JXL_IMAGE_BUNDLE_H_

// The main image or frame consists of a bundle of associated images.

#include <stddef.h>
#include <stdint.h>

// Brunsli headers
#include <brunsli/jpeg_data.h>

#include <vector>

#include "lib/jxl/aux_out_fwd.h"
#include "lib/jxl/base/compiler_specific.h"
#include "lib/jxl/base/data_parallel.h"
#include "lib/jxl/base/status.h"
#include "lib/jxl/color_encoding_internal.h"
#include "lib/jxl/common.h"
#include "lib/jxl/dec_bit_reader.h"
#include "lib/jxl/dec_xyb.h"
#include "lib/jxl/enc_bit_writer.h"
#include "lib/jxl/field_encodings.h"
#include "lib/jxl/frame_header.h"
#include "lib/jxl/headers.h"
#include "lib/jxl/image.h"
#include "lib/jxl/image_metadata.h"
#include "lib/jxl/opsin_params.h"
#include "lib/jxl/quantizer.h"

namespace jxl {

// A bundle of color/alpha/depth/plane images.
class ImageBundle {
 public:
  // Uninitialized state for use as output parameter.
  ImageBundle() : metadata_(nullptr) {}
  // Caller is responsible for setting metadata before calling Set*.
  explicit ImageBundle(const ImageMetadata* metadata) : metadata_(metadata) {}

  // Move-only (allows storing in std::vector).
  ImageBundle(ImageBundle&&) = default;
  ImageBundle& operator=(ImageBundle&&) = default;

  ImageBundle Copy() const {
    ImageBundle copy(metadata_);
    copy.color_ = CopyImage(color_);
    copy.c_current_ = c_current_;
    copy.extra_channels_.reserve(extra_channels_.size());
    for (const ImageU& plane : extra_channels_) {
      copy.extra_channels_.emplace_back(CopyImage(plane));
    }

    copy.jpeg_data =
        jpeg_data ? make_unique<brunsli::JPEGData>(*jpeg_data) : nullptr;
    copy.color_transform = color_transform;
    copy.chroma_subsampling = chroma_subsampling;

    return copy;
  }

  // -- SIZE

  size_t xsize() const {
    if (IsJPEG()) return jpeg_data->width;
    if (color_.xsize() != 0) return color_.xsize();
    return extra_channels_.empty() ? 0 : extra_channels_[0].xsize();
  }
  size_t ysize() const {
    if (IsJPEG()) return jpeg_data->height;
    if (color_.ysize() != 0) return color_.ysize();
    return extra_channels_.empty() ? 0 : extra_channels_[0].ysize();
  }
  void ShrinkTo(size_t xsize, size_t ysize);

  // -- COLOR

  // Whether color() is valid/usable. Returns true in most cases. Even images
  // with spot colors (one example of when !planes().empty()) typically have a
  // part that can be converted to RGB.
  bool HasColor() const { return color_.xsize() != 0; }

  // For resetting the size when switching from a reference to main frame.
  void RemoveColor() { color_ = Image3F(); }

  // Do not use if !HasColor().
  const Image3F& color() const {
    // If this fails, Set* was not called - perhaps because decoding failed?
    JXL_DASSERT(HasColor());
    return color_;
  }

  // Do not use if !HasColor().
  Image3F* color() {
    JXL_DASSERT(HasColor());
    return &color_;
  }

  // If c_current.IsGray(), all planes must be identical. NOTE: c_current is
  // independent of metadata()->color_encoding, which is the original, whereas
  // a decoder might return pixels in a different c_current.
  void SetFromImage(Image3F&& color, const ColorEncoding& c_current);

  // -- COLOR ENCODING

  const ColorEncoding& c_current() const { return c_current_; }

  // Returns whether the color image has identical planes. Once established by
  // Set*, remains unchanged until a subsequent Set* or TransformTo.
  bool IsGray() const { return c_current_.IsGray(); }

  bool IsSRGB() const { return c_current_.IsSRGB(); }
  bool IsLinearSRGB() const {
    return c_current_.white_point == WhitePoint::kD65 &&
           c_current_.primaries == Primaries::kSRGB && c_current_.tf.IsLinear();
  }

  // Transforms color to c_desired and sets c_current to c_desired. Alpha and
  // metadata remains unchanged.
  Status TransformTo(const ColorEncoding& c_desired,
                     ThreadPool* pool = nullptr);

  // Copies this:rect, converts to c_desired, and allocates+fills out.
  Status CopyTo(const Rect& rect, const ColorEncoding& c_desired, Image3B* out,
                ThreadPool* pool = nullptr) const;
  Status CopyTo(const Rect& rect, const ColorEncoding& c_desired, Image3U* out,
                ThreadPool* pool = nullptr) const;
  Status CopyTo(const Rect& rect, const ColorEncoding& c_desired, Image3F* out,
                ThreadPool* pool = nullptr) const;
  Status CopyToSRGB(const Rect& rect, Image3B* out,
                    ThreadPool* pool = nullptr) const;

  // Detect 'real' bit depth, which can be lower than nominal bit depth
  // (this is common in PNG), returns 'real' bit depth
  size_t DetectRealBitdepth() const;

  // -- ALPHA

  void SetAlpha(ImageU&& alpha, bool alpha_is_premultiplied);
  bool HasAlpha() const {
    return metadata_->m2.Find(ExtraChannel::kAlpha) != nullptr;
  }
  bool AlphaIsPremultiplied() const {
    const ExtraChannelInfo* eci = metadata_->m2.Find(ExtraChannel::kAlpha);
    return (eci == nullptr) ? false : eci->alpha_associated;
  }
  const ImageU& alpha() const;
  ImageU* alpha();

  // -- DEPTH
  void SetDepth(ImageU&& depth);
  bool HasDepth() const {
    return metadata_->m2.Find(ExtraChannel::kDepth) != nullptr;
  }
  const ImageU& depth() const;
  // Returns the dimensions of the depth image. Do not call if !HasDepth.
  size_t DepthSize(size_t size) const {
    return metadata_->m2.Find(ExtraChannel::kDepth)->Size(size);
  }

  // -- EXTRA CHANNELS

  // Extra channels of unknown interpretation (e.g. spot colors).
  void SetExtraChannels(std::vector<ImageU>&& extra_channels);
  bool HasExtraChannels() const { return !extra_channels_.empty(); }
  const std::vector<ImageU>& extra_channels() const {
    JXL_ASSERT(HasExtraChannels());
    return extra_channels_;
  }
  std::vector<ImageU>& extra_channels() {
    JXL_ASSERT(HasExtraChannels());
    return extra_channels_;
  }

  const ImageMetadata* metadata() const { return metadata_; }

  void VerifyMetadata() const;

  void SetDecodedBytes(size_t decoded_bytes) { decoded_bytes_ = decoded_bytes; }
  size_t decoded_bytes() const { return decoded_bytes_; }

  // -- JPEG transcoding:

  // Returns true if image does or will represent quantized DCT-8 coefficients,
  // stored in 8x8 pixel regions.
  bool IsJPEG() const { return jpeg_data != nullptr; }

  std::unique_ptr<brunsli::JPEGData> jpeg_data;
  // these fields are used to signal the input JPEG color space
  // NOTE: JPEG doesn't actually provide a way to determine whether YCbCr was
  // applied or not.
  ColorTransform color_transform = ColorTransform::kNone;
  YCbCrChromaSubsampling chroma_subsampling;

  FrameOrigin origin{0, 0};
  // Animation-related information. This assumes GIF- and APNG- like animation.
  uint32_t duration = 0;
  bool use_for_next_frame = false;
  bool blend = false;

 private:
  // Called after any Set* to ensure their sizes are compatible.
  void VerifySizes() const;

  // Required for TransformTo so that an ImageBundle is self-sufficient. Always
  // points to the same thing, but cannot be const-pointer because that prevents
  // the compiler from generating a move ctor.
  const ImageMetadata* metadata_;

  // Initialized by Set*:
  Image3F color_;  // If empty, planes_ is not; all planes equal if IsGray().
  ColorEncoding c_current_;  // of color_

  // Initialized by SetPlanes; size = ImageMetadata.num_extra_channels
  // TODO(janwas): change to pixel_type
  std::vector<ImageU> extra_channels_;

  // How many bytes of the input were actually read.
  size_t decoded_bytes_ = 0;
};

// Does color transformation from in.c_current() to c_desired if the color
// encodings are different, or nothing if they are already the same.
// If color transformation is done, stores the transformed values into store and
// sets the out pointer to store, else leaves store untouched and sets the out
// pointer to &in.
// Returns false if color transform fails.
Status TransformIfNeeded(const ImageBundle& in, const ColorEncoding& c_desired,
                         ThreadPool* pool, ImageBundle* store,
                         const ImageBundle** out);

}  // namespace jxl

#endif  // LIB_JXL_IMAGE_BUNDLE_H_
