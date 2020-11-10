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

#ifndef LIB_JXL_CODEC_IN_OUT_H_
#define LIB_JXL_CODEC_IN_OUT_H_

// Holds inputs/outputs for decoding/encoding images.

#include <stddef.h>

#include <utility>
#include <vector>

#include "lib/jxl/base/data_parallel.h"
#include "lib/jxl/common.h"
#include "lib/jxl/frame_header.h"
#include "lib/jxl/headers.h"
#include "lib/jxl/image.h"
#include "lib/jxl/image_bundle.h"
#include "lib/jxl/luminance.h"

namespace jxl {

// Per-channel interval, used to convert between (full-range) external and
// (bounded or unbounded) temp values. See external_image.cc for the definitions
// of temp/external.
struct CodecInterval {
  CodecInterval() = default;
  constexpr CodecInterval(float min, float max) : min(min), width(max - min) {}
  // Defaults for temp.
  float min = 0.0f;
  float width = 1.0f;
};

using CodecIntervals = std::array<CodecInterval, 4>;  // RGB[A] or Y[A]

// Allows passing arbitrary metadata to decoders (required for PNM).
class DecoderHints {
 public:
  // key=color_space, value=Description(c/pp): specify the ColorEncoding of
  //   the pixels for decoding. Otherwise, if the codec did not obtain an ICC
  //   profile from the image, assume sRGB.
  //
  // Strings are taken from the command line, so avoid spaces for convenience.
  void Add(const std::string& key, const std::string& value) {
    kv_.emplace_back(key, value);
  }

  // Calls `func(key, value)` for each key/value in the order they were added,
  // returning false immediately if `func` returns false.
  template <class Func>
  Status Foreach(const Func& func) const {
    for (const KeyValue& kv : kv_) {
      Status ok = func(kv.key, kv.value);
      if (!ok) {
        return JXL_FAILURE("DecoderHints::Foreach returned false");
      }
    }
    return true;
  }

 private:
  // Splitting into key/value avoids parsing in each codec.
  struct KeyValue {
    KeyValue(std::string key, std::string value)
        : key(std::move(key)), value(std::move(value)) {}

    std::string key;
    std::string value;
  };

  std::vector<KeyValue> kv_;
};

// Optional text/EXIF metadata.
struct Blobs {
  PaddedBytes exif;
  PaddedBytes iptc;
  PaddedBytes jumbf;
  PaddedBytes xmp;
};

// All metadata applicable to the entire codestream (dimensions, extra channels,
// ...)
struct CodecMetadata {
  // TODO(lode): use the other fields too, this is work in progress, currently
  // only "m" is used. SizeHeader should be used instead of m.nonserialized_size
  ImageMetadata m;
  SizeHeader size;
  PreviewHeader preview;
  AnimationHeader animation;
};

// For Codec::kJPG, convert between JPEG and pixels or between JPEG and
// quantized DCT coefficients
// For float data (pfm,exr): kPixels uses 0..maxnits, kLosslessFloat doesn't
// touch range
enum class DecodeTarget { kPixels, kQuantizedCoeffs, kLosslessFloat };

// Holds a preview, a main image or one or more frames, plus the inputs/outputs
// to/from decoding/encoding.
class CodecInOut {
 public:
  CodecInOut() : preview_frame(&metadata.m) {
    frames.reserve(1);
    frames.emplace_back(&metadata.m);
  }

  // Move-only.
  CodecInOut(CodecInOut&&) = default;
  CodecInOut& operator=(CodecInOut&&) = default;

  ImageBundle& Main() {
    JXL_DASSERT(frames.size() == 1);
    return frames[0];
  }
  const ImageBundle& Main() const {
    JXL_DASSERT(frames.size() == 1);
    return frames[0];
  }

  // If c_current.IsGray(), all planes must be identical.
  void SetFromImage(Image3F&& color, const ColorEncoding& c_current) {
    Main().SetFromImage(std::move(color), c_current);
    SetIntensityTarget(this);
    SetSize(color.xsize(), color.ysize());
  }

  void SetSize(size_t xsize, size_t ysize) {
    JXL_CHECK(metadata.m.nonserialized_size.Set(xsize, ysize));
  }

  void CheckMetadata() const {
    JXL_CHECK(metadata.m.bit_depth.bits_per_sample != 0);
    JXL_CHECK(!metadata.m.color_encoding.ICC().empty());

    if (preview_frame.xsize() != 0) preview_frame.VerifyMetadata();
    JXL_CHECK(preview_frame.metadata() == &metadata.m);

    for (const ImageBundle& ib : frames) {
      ib.VerifyMetadata();
      JXL_CHECK(ib.metadata() == &metadata.m);
    }
  }

  size_t xsize() const { return metadata.m.xsize(); }
  size_t ysize() const { return metadata.m.ysize(); }
  void ShrinkTo(size_t xsize, size_t ysize) {
    // preview is unaffected.
    for (ImageBundle& ib : frames) {
      ib.ShrinkTo(xsize, ysize);
    }
    SetSize(xsize, ysize);
  }

  template <typename T>
  Status VerifyDimensions(T xs, T ys) const {
    if (xs == 0 || ys == 0) return JXL_FAILURE("Empty image.");
    if (xs > dec_max_xsize) return JXL_FAILURE("Image too wide.");
    if (ys > dec_max_ysize) return JXL_FAILURE("Image too tall.");

    const uint64_t num_pixels = uint64_t(xs) * ys;
    if (num_pixels > dec_max_pixels) return JXL_FAILURE("Image too big.");

    return true;
  }

  // Calls TransformTo for each ImageBundle (preview/frames).
  Status TransformTo(const ColorEncoding& c_desired,
                     ThreadPool* pool = nullptr) {
    if (metadata.m.m2.have_preview) {
      JXL_RETURN_IF_ERROR(preview_frame.TransformTo(c_desired, pool));
    }
    for (ImageBundle& ib : frames) {
      JXL_RETURN_IF_ERROR(ib.TransformTo(c_desired, pool));
    }
    return true;
  }

  // -- DECODER INPUT:

  // Upper limit on pixel dimensions/area, enforced by VerifyDimensions
  // (called from decoders). Fuzzers set smaller values to limit memory use.
  uint32_t dec_max_xsize = 0xFFFFFFFFu;
  uint32_t dec_max_ysize = 0xFFFFFFFFu;
  uint64_t dec_max_pixels = ~0ull;

  // Used to set c_current for codecs that lack color space metadata.
  DecoderHints dec_hints;
  // Decode to pixels or keep JPEG as quantized DCT coefficients
  DecodeTarget dec_target = DecodeTarget::kPixels;

  // Intended white luminance, in nits (cd/m^2).
  // It is used by codecs that do not know the absolute luminance of their
  // images. For those codecs, decoders map from white to this luminance. There
  // is no other way of knowing the target brightness for those codecs - depends
  // on source material. 709 typically targets 100 nits, BT.2100 PQ up to 10K,
  // but HDR content is more typically mastered to 4K nits. Codecs that do know
  // the absolute luminance of their images will typically ignore it as a
  // decoder input. The corresponding decoder output and encoder input is the
  // intensity target in the metadata. ALL decoders MUST set that metadata
  // appropriately, but it does not have to be identical to this hint. Encoders
  // for codecs that do not encode absolute luminance levels should use that
  // metadata to decide on what to map to white. Encoders for codecs that *do*
  // encode absolute luminance levels may use it to decide on encoding values,
  // but not in a way that would affect the range of interpreted luminance.
  //
  // 0 means that it is up to the codec (or the Map255ToTargetNits function from
  // luminance.h) to decide on a reasonable value to use.

  float target_nits = 0;

  // -- DECODER OUTPUT:

  // Total number of pixels decoded (may differ from #frames * xsize * ysize
  // if frames are cropped)
  uint64_t dec_pixels = 0;

  // -- DECODER OUTPUT, ENCODER INPUT:

  // Metadata stored into / retrieved from bitstreams.

  Blobs blobs;

  CodecMetadata metadata;  // applies to preview and all frames

  // If metadata.have_preview:
  ImageBundle preview_frame;

  std::vector<ImageBundle> frames;  // size=1 if !metadata.have_animation

  bool use_sjpeg = false;
  // If the image should be written to a JPEG, use this quality for encoding.
  size_t jpeg_quality;
};

}  // namespace jxl

#endif  // LIB_JXL_CODEC_IN_OUT_H_
