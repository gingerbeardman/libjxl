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

#ifndef LIB_JXL_FRAME_HEADER_H_
#define LIB_JXL_FRAME_HEADER_H_

// Frame header with backward and forward-compatible extension capability and
// compressed integer fields.

#include <stddef.h>
#include <stdint.h>

#include <string>

#include "lib/jxl/aux_out_fwd.h"
#include "lib/jxl/base/compiler_specific.h"
#include "lib/jxl/base/override.h"
#include "lib/jxl/base/padded_bytes.h"
#include "lib/jxl/base/status.h"
#include "lib/jxl/coeff_order_fwd.h"
#include "lib/jxl/common.h"
#include "lib/jxl/dec_bit_reader.h"
#include "lib/jxl/enc_bit_writer.h"
#include "lib/jxl/fields.h"
#include "lib/jxl/gaborish.h"
#include "lib/jxl/image_metadata.h"
#include "lib/jxl/loop_filter.h"

namespace jxl {

// Also used by extra channel names.
static inline Status VisitNameString(Visitor* JXL_RESTRICT visitor,
                                     std::string* name) {
  uint32_t name_length = static_cast<uint32_t>(name->length());
  // Allows layer name lengths up to 1071 bytes
  JXL_QUIET_RETURN_IF_ERROR(visitor->U32(Val(0), Bits(4), BitsOffset(5, 16),
                                         BitsOffset(10, 48), 0, &name_length));
  if (visitor->IsReading()) {
    name->resize(name_length);
  }
  for (size_t i = 0; i < name_length; i++) {
    uint32_t c = (*name)[i];
    JXL_QUIET_RETURN_IF_ERROR(visitor->Bits(8, 0, &c));
    (*name)[i] = static_cast<char>(c);
  }
  return true;
}

enum class FrameEncoding : uint32_t {
  kVarDCT,
  kModular,
};

enum class ColorTransform : uint32_t {
  kXYB,    // Values are encoded with XYB. May only be used if
           // ImageBundle::xyb_encoded.
  kNone,   // Values are encoded according to the attached color profile. May
           // only be used if !ImageBundle::xyb_encoded.
  kYCbCr,  // Values are encoded according to the attached color profile, but
           // transformed to YCbCr. May only be used if
           // !ImageBundle::xyb_encoded.
};

struct YCbCrChromaSubsampling : public Fields {
  YCbCrChromaSubsampling();
  const char* Name() const override { return "YCbCrChromaSubsampling"; }
  size_t HShift(size_t c) const { return maxhs_ - kHShift[channel_mode_[c]]; }
  size_t VShift(size_t c) const { return maxvs_ - kVShift[channel_mode_[c]]; }

  Status VisitFields(Visitor* JXL_RESTRICT visitor) override {
    // TODO(veluca): consider allowing 4x downsamples
    for (size_t i = 0; i < 3; i++) {
      JXL_QUIET_RETURN_IF_ERROR(visitor->Bits(2, 0, &channel_mode_[i]));
    }
    Recompute();
    return true;
  }

  uint8_t MaxHShift() const { return maxhs_; }
  uint8_t MaxVShift() const { return maxvs_; }

  // Uses JPEG channel order (Y, Cb, Cr).
  Status Set(const uint8_t* hsample, const uint8_t* vsample) {
    for (size_t c = 0; c < 3; c++) {
      size_t cjpeg = c < 2 ? c ^ 1 : c;
      size_t i = 0;
      for (; i < 4; i++) {
        if (1 << kHShift[i] == hsample[cjpeg] &&
            1 << kVShift[i] == vsample[cjpeg]) {
          channel_mode_[c] = i;
          break;
        }
      }
      if (i == 4) {
        return JXL_FAILURE("Invalid subsample mode");
      }
    }
    Recompute();
    return true;
  }

  bool Is444() const {
    for (size_t c : {0, 2}) {
      if (channel_mode_[c] != channel_mode_[1]) {
        return false;
      }
    }
    return true;
  }

  bool Is420() const {
    return channel_mode_[0] == 1 && channel_mode_[1] == 0 &&
           channel_mode_[2] == 1;
  }

  bool Is422() const {
    for (size_t c : {0, 2}) {
      if (kHShift[channel_mode_[c]] == kHShift[channel_mode_[1]] + 1 &&
          kVShift[channel_mode_[c]] == kVShift[channel_mode_[1]]) {
        return false;
      }
    }
    return true;
  }

  bool Is440() const {
    for (size_t c : {0, 2}) {
      if (kHShift[channel_mode_[c]] == kHShift[channel_mode_[1]] &&
          kVShift[channel_mode_[c]] == kVShift[channel_mode_[1]] + 1) {
        return false;
      }
    }
    return true;
  }

 private:
  void Recompute() {
    maxhs_ = 0;
    maxvs_ = 0;
    for (size_t i = 0; i < 3; i++) {
      maxhs_ = std::max(maxhs_, kHShift[channel_mode_[i]]);
      maxvs_ = std::max(maxvs_, kVShift[channel_mode_[i]]);
    }
  }
  static constexpr uint8_t kHShift[4] = {0, 1, 1, 0};
  static constexpr uint8_t kVShift[4] = {0, 1, 0, 1};
  uint32_t channel_mode_[3];
  uint8_t maxhs_;
  uint8_t maxvs_;
};

// Indicates how to combine the current frame with a previously-saved one. Can
// be independently controlled for color and extra channels. Formulas are
// indicative and treat alpha as if it is in range 0.0-1.0. In descriptions
// below, alpha channel is the extra channel of type alpha used for blending
// according to the blend_channel, or fully opaque if there is no alpha channel.
// The blending specified here is used for performing blending *after* color
// transforms - in linear sRGB if blending a XYB-encoded frame on another
// XYB-encoded frame, in sRGB if blending a frame with kColorSpace == kSRGB, or
// in the original colorspace otherwise. Blending in XYB or YCbCr is done by
// using patches.
enum class BlendMode {
  // The new values (in the crop) replace the old ones: sample = new
  kReplace = 0,
  // The new values (in the crop) get added to the old ones: sample = old + new
  kAdd = 1,
  // The new values (in the crop) replace the old ones if alpha>0:
  // For the alpha channel that is used as source:
  // alpha = old + new * (1 - old)
  // For other channels if !alpha_associated:
  // sample = ((1 - new_alpha) * old * old_alpha + new_alpha * new) / alpha
  // For other channels if alpha_associated:
  // sample = (1 - new_alpha) * old + new
  // The alpha formula applies to the alpha used for the division in the other
  // channels formula, and applies to the alpha channel itself if its
  // blend_channel value matches itself.
  kBlend = 2,
  // The new values (in the crop) are added to the old ones if alpha>0:
  // For the alpha channel that is used as source:
  // sample = sample = old + new * (1 - old)
  // For other channels: sample = old + alpha * new
  kAlphaWeightedAdd = 3,
  // The new values (in the crop) get multiplied by the old ones:
  // sample = old * new
  // The range of the new value matters for multiplication purposes, and its
  // nominal range of 0..1 is computed the same way as this is done for the
  // alpha values in kBlend and kAlphaWeightedAdd.
  // If using kMul as a blend mode for color channels, no color transform is
  // performed on the current frame.
  kMul = 4,
};

struct BlendingInfo : public Fields {
  BlendingInfo();
  const char* Name() const override { return "BlendingInfo"; }
  Status VisitFields(Visitor* JXL_RESTRICT visitor) override;
  BlendMode mode;
  // Which extra channel to use as alpha channel for blending, only encoded
  // for blend modes that involve alpha and if there are more than 1 extra
  // channels.
  uint32_t alpha_channel;
  // Clamp alpha or channel values to 0-1 range.
  bool clamp;
  // Frame ID to copy from (0-3). Only encoded if blend_mode is not kReplace.
  uint32_t source;

  bool nonserialized_has_multiple_extra_channels = false;
  bool nonserialized_is_partial_frame = false;
};

// Origin of the current frame. Not present for frames of type
// kOnlyPatches.
struct FrameOrigin {
  int32_t x0, y0;  // can be negative.
};

// Size of the current frame.
struct FrameSize {
  uint32_t xsize, ysize;
};

// AnimationFrame defines duration of animation frames.
struct AnimationFrame : public Fields {
  explicit AnimationFrame(const ImageMetadata* metadata);
  const char* Name() const override { return "AnimationFrame"; }

  Status VisitFields(Visitor* JXL_RESTRICT visitor) override;

  // How long to wait [in ticks, see Animation{}] after rendering.
  // May be 0 if the current frame serves as a foundation for another frame.
  uint32_t duration;

  uint32_t timecode;  // 0xHHMMSSFF

  // Must be set to the one ImageMetadata acting as the full codestream header,
  // with correct xyb_encoded, list of extra channels, etc...
  const ImageMetadata* nonserialized_image_metadata = nullptr;
};

// For decoding to lower resolutions. Only used for kRegular frames.
struct Passes : public Fields {
  Passes();
  const char* Name() const override { return "Passes"; }

  Status VisitFields(Visitor* JXL_RESTRICT visitor) override;

  uint32_t num_passes;      // <= kMaxNumPasses
  uint32_t num_downsample;  // <= num_passes

  // Array of num_downsample pairs. downsample=1/last_pass=num_passes-1 and
  // downsample=8/last_pass=0 need not be specified; they are implicit.
  uint32_t downsample[kMaxNumPasses];
  uint32_t last_pass[kMaxNumPasses];
  // Array of shift values for each pass. It is implicitly assumed to be 0 for
  // the last pass.
  uint32_t shift[kMaxNumPasses];
};

enum FrameType {
  // A "regular" frame: might be a crop, and will be blended on a previous
  // frame, if any, and displayed or blended in future frames.
  kRegularFrame = 0,
  // A DC frame: this frame is downsampled and will be *only* used as the DC of
  // a future frame and, possibly, for previews. Cannot be cropped, blended, or
  // referenced by patches or blending modes. Frames that *use* a DC frame
  // cannot have non-default sizes either.
  kDCFrame = 1,
  // A PatchesSource frame: this frame will be only used as a source frame for
  // taking patches. Can be cropped, but cannot have non-(0, 0) x0 and y0.
  kReferenceOnly = 2,
};

// Image/frame := one of more of these, where the last has is_last = true.
// Starts at a byte-aligned address "a"; the next pass starts at "a + size".
struct FrameHeader : public Fields {
  // Optional postprocessing steps. These flags are the source of truth;
  // Override must set/clear them rather than change their meaning. Values
  // chosen such that typical flags == 0 (encoded in only two bits).
  enum Flags {
    // Often but not always off => low bit value:

    // Inject noise into decoded output.
    kNoise = 1,

    // Overlay patches.
    kPatches = 2,

    // 4, 8 = reserved for future sometimes-off

    // Overlay splines.
    kSplines = 16,

    kUseDcFrame = 32,  // Implies kSkipAdaptiveDCSmoothing.

    // 64 = reserved for future often-off

    // Almost always on => negated:

    kSkipAdaptiveDCSmoothing = 128,
  };

  explicit FrameHeader(const ImageMetadata* metadata);
  const char* Name() const override { return "FrameHeader"; }

  Status VisitFields(Visitor* JXL_RESTRICT visitor) override;

  // Sets/clears `flag` based upon `condition`.
  void UpdateFlag(const bool condition, const uint64_t flag) {
    if (condition) {
      flags |= flag;
    } else {
      flags &= ~flag;
    }
  }

  // Returns true if this frame is supposed to be saved for future usage by
  // other frames.
  bool CanBeReferenced() const {
    // DC frames cannot be referenced. The last frame cannot be referenced. A
    // duration 0 frame makes little sense if it is not referenced. A
    // non-duration 0 frame may or may not be referenced.
    return !is_last && frame_type != FrameType::kDCFrame &&
           (animation_frame.duration == 0 || save_as_reference != 0);
  }

  mutable bool all_default;

  // Always present
  FrameEncoding encoding;
  // Some versions of UBSAN complain in VisitFrameType if not initialized.
  FrameType frame_type = FrameType::kRegularFrame;

  uint64_t flags;

  ColorTransform color_transform;
  YCbCrChromaSubsampling chroma_subsampling;

  uint32_t group_size_shift;  // only if encoding == kModular;

  uint32_t x_qm_scale;  // only if IsLossy() and color_transform == kXYB

  std::string name;

  // Skipped for kReferenceOnly.
  Passes passes;

  // Skipped for kDCFrame
  bool custom_size_or_origin;
  FrameSize frame_size;

  // upsampling factors for color and extra channels.
  // For color channels, upsampling is performed in the same color space that
  // the frame is blended or saved in. Skipped (1) if kUseDCFrame
  uint32_t upsampling;
  std::vector<uint32_t> extra_channel_upsampling;

  // Only for kRegular frames.
  FrameOrigin frame_origin;

  BlendingInfo blending_info;
  std::vector<BlendingInfo> extra_channel_blending_info;

  // Animation info for this frame.
  AnimationFrame animation_frame;

  // This is the last frame.
  bool is_last;

  // ID to refer to this frame with. 0-3, not present if kDCFrame.
  // 0 has a special meaning for kRegular frames of nonzero duration: it defines
  // a frame that will not be referenced in the future.
  uint32_t save_as_reference;

  // Whether to save this frame before or after the color transform. A frame
  // that is saved before the color tansform can only be used for blending
  // through patches. On the contrary, a frame that is saved after the color
  // transform can only be used for blending through blending modes.
  // Irrelevant for extra channel blending. Can only be true if
  // blending_info.mode == kReplace and this is not a partial kRegularFrame; if
  // this is a DC frame, it is always true.
  bool save_before_color_transform;

  uint32_t dc_level;  // 1-4 if kDCFrame (0 otherwise).

  // Must be set to the one ImageMetadata acting as the full codestream header,
  // with correct xyb_encoded, list of extra channels, etc...
  const ImageMetadata* nonserialized_image_metadata = nullptr;

  // This is the only LoopFilter instance for this frame; it's not serialized by
  // VisitFields, but is kept here for convenience.
  LoopFilter nonserialized_loop_filter;

  bool nonserialized_is_preview = false;

  size_t default_xsize() const {
    if (!nonserialized_image_metadata) return 0;
    if (nonserialized_is_preview) {
      return nonserialized_image_metadata->nonserialized_preview.xsize();
    }
    return nonserialized_image_metadata->xsize();
  }

  size_t default_ysize() const {
    if (!nonserialized_image_metadata) return 0;
    if (nonserialized_is_preview) {
      return nonserialized_image_metadata->nonserialized_preview.ysize();
    }
    return nonserialized_image_metadata->ysize();
  }

  FrameDimensions ToFrameDimensions() const {
    size_t xsize = default_xsize();
    size_t ysize = default_ysize();

    xsize = frame_size.xsize ? frame_size.xsize : xsize;
    ysize = frame_size.ysize ? frame_size.ysize : ysize;

    if (dc_level != 0) {
      xsize = DivCeil(xsize, 1 << (3 * dc_level));
      ysize = DivCeil(ysize, 1 << (3 * dc_level));
    }

    FrameDimensions frame_dim;
    frame_dim.Set(xsize, ysize, group_size_shift,
                  chroma_subsampling.MaxHShift(),
                  chroma_subsampling.MaxVShift(), upsampling);
    return frame_dim;
  }

  uint64_t extensions;
};

Status ReadFrameHeader(BitReader* JXL_RESTRICT reader,
                       FrameHeader* JXL_RESTRICT frame);

Status WriteFrameHeader(const FrameHeader& frame,
                        BitWriter* JXL_RESTRICT writer, AuxOut* aux_out);

// Shared by enc/dec. 5F and 13 are by far the most common for d1/2/4/8, 0
// ensures low overhead for small images.
static constexpr U32Enc kOrderEnc =
    U32Enc(Val(0x5F), Val(0x13), Val(0), Bits(kNumOrders));

}  // namespace jxl

#endif  // LIB_JXL_FRAME_HEADER_H_
