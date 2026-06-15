#include <gtest/gtest.h>

#include <cstdint>
#include <vector>

#include "bitstream/bitstream_reader.h"
#include "syntax/sps.h"

using namespace hevc;

// Hardening: pic_width/height and the conf_win_* offsets arrive via ue(v) from
// untrusted media with no inherent bound, then drive Picture::allocate strides
// and SIMD loop extents. SPS::validate() must reject malformed dimensions so
// SPS::parse() fails instead of letting a hostile SPS over-allocate or run a
// SIMD kernel out of bounds. These tests cover the validate() predicate
// directly (every rejection reason + an accept case) and the full parse() path.

namespace {

// Minimal MSB-first bit writer (Exp-Golomb capable), matching the style used by
// the existing slice-header tests.
class BitWriter {
public:
    void put_bit(uint32_t b) {
        cur_ = static_cast<uint8_t>((cur_ << 1) | (b & 1u));
        if (++nbits_ == 8) {
            bytes_.push_back(cur_);
            cur_ = 0;
            nbits_ = 0;
        }
    }
    void put_bits(uint32_t v, int n) {
        for (int i = n - 1; i >= 0; --i) put_bit((v >> i) & 1u);
    }
    void put_ue(uint32_t v) {
        uint32_t code = v + 1;
        int len = 0;
        for (uint32_t t = code; t; t >>= 1) ++len;
        for (int i = 0; i < len - 1; ++i) put_bit(0);
        for (int i = len - 1; i >= 0; --i) put_bit((code >> i) & 1u);
    }
    std::vector<uint8_t> finish() {
        if (nbits_ > 0) {
            cur_ = static_cast<uint8_t>(cur_ << (8 - nbits_));
            bytes_.push_back(cur_);
            cur_ = 0;
            nbits_ = 0;
        }
        // Trailing padding so any residual reads after the fields we care about
        // stay in-buffer; leading 1 bits keep stray ue/se reads short.
        for (int i = 0; i < 8; ++i) bytes_.push_back(0xFF);
        return bytes_;
    }

private:
    std::vector<uint8_t> bytes_;
    uint8_t cur_ = 0;
    int nbits_ = 0;
};

// Build a Main-profile SPS RBSP (no extensions). Dimensions and conformance
// window are caller-supplied so we can craft both conformant and hostile
// variants. MinCbSizeY here derives to 16 (log2_min_luma_coding_block_size_minus3
// = 1 -> MinCbLog2SizeY = 4).
std::vector<uint8_t> make_sps_rbsp(uint32_t width, uint32_t height,
                                   bool conf_win, uint32_t l, uint32_t r,
                                   uint32_t t, uint32_t b) {
    BitWriter w;
    w.put_bits(0, 4);   // sps_video_parameter_set_id
    w.put_bits(0, 3);   // sps_max_sub_layers_minus1
    w.put_bit(1);       // sps_temporal_id_nesting_flag

    // profile_tier_level(1, 0): 88-bit general profile_tier block + 8-bit
    // general_level_idc. Sub-layer loops are empty (max_sub_layers_minus1 == 0).
    // Mirror the exact bit consumption of ProfileTierLevel::parse():
    w.put_bits(0, 2);   // general_profile_space
    w.put_bit(0);       // general_tier_flag
    w.put_bits(1, 5);   // general_profile_idc = 1 (Main) -> non-RExt "else" branch
    w.put_bits(0, 32);  // general_profile_compatibility_flag[0..31]
    w.put_bits(0, 4);   // progressive/interlaced/non_packed/frame_only flags
    w.put_bits(0, 32);  // general_reserved_zero_43bits (first 32)
    w.put_bits(0, 11);  // general_reserved_zero_43bits (last 11)
    w.put_bit(0);       // general_inbld_flag / reserved
    w.put_bits(30, 8);  // general_level_idc

    w.put_ue(0);        // sps_seq_parameter_set_id
    w.put_ue(1);        // chroma_format_idc = 1 (4:2:0)

    w.put_ue(width);    // pic_width_in_luma_samples
    w.put_ue(height);   // pic_height_in_luma_samples

    w.put_bit(conf_win ? 1 : 0);  // conformance_window_flag
    if (conf_win) {
        w.put_ue(l);
        w.put_ue(r);
        w.put_ue(t);
        w.put_ue(b);
    }

    w.put_ue(0);        // bit_depth_luma_minus8
    w.put_ue(0);        // bit_depth_chroma_minus8
    w.put_ue(0);        // log2_max_pic_order_cnt_lsb_minus4

    w.put_bit(0);       // sps_sub_layer_ordering_info_present_flag = 0
    // start == max_sub_layers_minus1 == 0 -> exactly one ordering entry.
    w.put_ue(0);        // max_dec_pic_buffering_minus1
    w.put_ue(0);        // max_num_reorder_pics
    w.put_ue(0);        // max_latency_increase_plus1

    w.put_ue(1);        // log2_min_luma_coding_block_size_minus3 -> MinCbSizeY = 16
    w.put_ue(1);        // log2_diff_max_min_luma_coding_block_size -> CtbSizeY = 32
    w.put_ue(0);        // log2_min_luma_transform_block_size_minus2
    w.put_ue(3);        // log2_diff_max_min_luma_transform_block_size
    w.put_ue(0);        // max_transform_hierarchy_depth_inter
    w.put_ue(0);        // max_transform_hierarchy_depth_intra

    w.put_bit(0);       // scaling_list_enabled_flag
    w.put_bit(0);       // amp_enabled_flag
    w.put_bit(0);       // sample_adaptive_offset_enabled_flag
    w.put_bit(0);       // pcm_enabled_flag

    w.put_ue(0);        // num_short_term_ref_pic_sets
    w.put_bit(0);       // long_term_ref_pics_present_flag
    w.put_bit(0);       // sps_temporal_mvp_enabled_flag
    w.put_bit(0);       // strong_intra_smoothing_enabled_flag
    w.put_bit(0);       // vui_parameters_present_flag
    w.put_bit(0);       // sps_extension_present_flag
    // rbsp_trailing_bits handled by trailing padding.
    return w.finish();
}

bool parse_sps(const std::vector<uint8_t>& rbsp, SPS& out) {
    BitstreamReader bs(rbsp.data(), rbsp.size());
    return out.parse(bs);
}

}  // namespace

// Sanity: a well-formed SPS RBSP must parse and validate, otherwise the
// negative cases below would be meaningless.
TEST(SpsValidateDims, ConformantSpsAccepted) {
    SPS sps;
    ASSERT_TRUE(parse_sps(make_sps_rbsp(/*w=*/64, /*h=*/64, false, 0, 0, 0, 0), sps));
    EXPECT_EQ(sps.pic_width_in_luma_samples, 64u);
    EXPECT_EQ(sps.pic_height_in_luma_samples, 64u);
    EXPECT_EQ(sps.MinCbSizeY, 16);
    EXPECT_TRUE(sps.validate());
}

// Zero width/height: SPS::parse() must reject (no zero-extent allocation).
TEST(SpsValidateDims, ParseRejectsZeroDimensions) {
    SPS sps_w, sps_h;
    EXPECT_FALSE(parse_sps(make_sps_rbsp(0, 64, false, 0, 0, 0, 0), sps_w));
    EXPECT_FALSE(parse_sps(make_sps_rbsp(64, 0, false, 0, 0, 0, 0), sps_h));
}

// Dimensions above the sane cap (16384) must be rejected by parse().
TEST(SpsValidateDims, ParseRejectsOversizedDimensions) {
    SPS sps_w, sps_h;
    // 16400 is a multiple of MinCbSizeY (16) but exceeds the cap, isolating the
    // cap check from the multiple-of-MinCb check.
    EXPECT_FALSE(parse_sps(make_sps_rbsp(16400, 64, false, 0, 0, 0, 0), sps_w));
    EXPECT_FALSE(parse_sps(make_sps_rbsp(64, 16400, false, 0, 0, 0, 0), sps_h));
}

// Dimensions not a multiple of MinCbSizeY (16 here) must be rejected.
TEST(SpsValidateDims, ParseRejectsNonMultipleOfMinCb) {
    SPS sps_w, sps_h;
    EXPECT_FALSE(parse_sps(make_sps_rbsp(70, 64, false, 0, 0, 0, 0), sps_w));
    EXPECT_FALSE(parse_sps(make_sps_rbsp(64, 70, false, 0, 0, 0, 0), sps_h));
}

// Conformance window that crops away the whole picture (or more) must be
// rejected. Offsets are in chroma units; SubWidthC == SubHeightC == 2 for 4:2:0,
// so left+right == 32 crops 64 luma samples == full width.
TEST(SpsValidateDims, ParseRejectsConfWindowExceedingPicture) {
    SPS sps_w, sps_h;
    EXPECT_FALSE(parse_sps(make_sps_rbsp(64, 64, true, 16, 16, 0, 0), sps_w));
    EXPECT_FALSE(parse_sps(make_sps_rbsp(64, 64, true, 0, 0, 16, 16), sps_h));
}

// A small, valid conformance window (crop < picture) must be accepted.
TEST(SpsValidateDims, ParseAcceptsValidConfWindow) {
    SPS sps;
    EXPECT_TRUE(parse_sps(make_sps_rbsp(64, 64, true, 0, 1, 0, 1), sps));
    EXPECT_TRUE(sps.validate());
    EXPECT_EQ(sps.conf_win_right_offset, 1u);
    EXPECT_EQ(sps.conf_win_bottom_offset, 1u);
}

// validate() predicate exercised directly with derived values, independent of
// the bitstream layout.
TEST(SpsValidateDims, ValidatePredicateDirect) {
    SPS sps;
    sps.chroma_format_idc = 1;
    sps.log2_min_luma_coding_block_size_minus3 = 1;          // MinCbSizeY = 16
    sps.log2_diff_max_min_luma_coding_block_size = 1;

    sps.pic_width_in_luma_samples = 1920;
    sps.pic_height_in_luma_samples = 1088;
    sps.derive();
    EXPECT_TRUE(sps.validate());

    // Width not a multiple of MinCbSizeY.
    sps.pic_width_in_luma_samples = 1921;
    sps.derive();
    EXPECT_FALSE(sps.validate());

    // Over the cap.
    sps.pic_width_in_luma_samples = 20000;  // multiple of 16, > 16384
    sps.derive();
    EXPECT_FALSE(sps.validate());

    // Conf window larger than the picture.
    sps.pic_width_in_luma_samples = 1920;
    sps.conformance_window_flag = true;
    sps.conf_win_left_offset = 1000;
    sps.conf_win_right_offset = 1000;  // 2*(1000+1000) = 4000 > 1920
    sps.derive();
    EXPECT_FALSE(sps.validate());
}
