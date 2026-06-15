#include <gtest/gtest.h>

#include <cstdint>
#include <vector>

#include "bitstream/bitstream_reader.h"
#include "common/types.h"
#include "syntax/pps.h"
#include "syntax/slice_header.h"
#include "syntax/sps.h"

using namespace hevc;

// Minimal MSB-first bit writer to craft a slice_segment_header() bitstream up
// to and including the long-term reference picture loop (§7.3.6.1).
namespace {

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
    // ue(v) Exp-Golomb.
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
        // Padding so trailing slice-header reads after the LT loop (slice_qp_delta
        // se(v), byte_alignment) stay in-buffer. Leading 1 bits keep any residual
        // ue/se read short rather than scanning an infinite zero prefix.
        for (int i = 0; i < 4; ++i) bytes_.push_back(0xFF);
        return bytes_;
    }

private:
    std::vector<uint8_t> bytes_;
    uint8_t cur_ = 0;
    int nbits_ = 0;
};

// Build an SPS/PPS pair configured so SliceHeader::parse() reaches the
// long-term reference loop with a single short-term RPS already present.
SPS make_sps(uint32_t num_long_term_ref_pics_sps) {
    SPS sps;
    sps.separate_colour_plane_flag = false;
    sps.log2_max_pic_order_cnt_lsb_minus4 = 0;  // 4-bit POC LSB
    sps.sample_adaptive_offset_enabled_flag = false;
    sps.sps_temporal_mvp_enabled_flag = false;
    sps.ChromaArrayType = 1;
    sps.num_short_term_ref_pic_sets = 1;
    sps.st_ref_pic_sets.resize(1);  // active_rps = &st_ref_pic_sets[0]
    sps.long_term_ref_pics_present_flag = true;
    sps.num_long_term_ref_pics_sps = num_long_term_ref_pics_sps;
    return sps;
}

}  // namespace

// num_long_term_ref_pics_sps > 1: lt_idx_sps is coded and selects the SPS
// entry. Pre-fix, poc_lsb_lt/used_by_curr_pic_lt_flag stayed at 0/false,
// silently dropping the SPS-declared LTRP.
TEST(SliceLtrpSps, MultiSpsEntryResolvedFromIndex) {
    SPS sps = make_sps(/*num_long_term_ref_pics_sps=*/2);
    sps.lt_ref_pic_poc_lsb_sps[0] = 3;
    sps.used_by_curr_pic_lt_sps_flag[0] = false;
    sps.lt_ref_pic_poc_lsb_sps[1] = 5;
    sps.used_by_curr_pic_lt_sps_flag[1] = true;
    PPS pps;  // defaults: no extra header bits, no output flag, etc.

    BitWriter w;
    w.put_bit(1);          // first_slice_segment_in_pic_flag
    w.put_ue(0);           // slice_pic_parameter_set_id
    w.put_ue(2);           // slice_type = I
    w.put_bits(0, 4);      // slice_pic_order_cnt_lsb (4 bits)
    w.put_bit(1);          // short_term_ref_pic_set_sps_flag (num sets == 1)
    w.put_ue(1);           // num_long_term_sps = 1
    w.put_ue(0);           // num_long_term_pics = 0
    w.put_bits(1, 1);      // lt_idx_sps[0] = 1 (ceil_log2(2) == 1 bit)
    w.put_bit(0);          // delta_poc_msb_present_flag[0] = 0
    auto data = w.finish();

    BitstreamReader bs(data.data(), data.size());
    SliceHeader sh;
    ASSERT_TRUE(sh.parse(bs, sps, pps, NalUnitType::TRAIL_R, /*temporal_id=*/0));

    ASSERT_EQ(sh.num_long_term_sps, 1u);
    EXPECT_EQ(sh.lt_idx_sps[0], 1u);
    // Resolved from sps.lt_ref_pic_poc_lsb_sps[1] / used_by_curr_pic_lt_sps_flag[1].
    EXPECT_EQ(sh.poc_lsb_lt[0], 5u);
    EXPECT_TRUE(sh.used_by_curr_pic_lt_flag[0]);
}

// num_long_term_ref_pics_sps == 1: lt_idx_sps[i] is inferred 0 (not coded),
// so the single SPS entry must still be resolved.
TEST(SliceLtrpSps, SingleSpsEntryInferredIndexZero) {
    SPS sps = make_sps(/*num_long_term_ref_pics_sps=*/1);
    sps.lt_ref_pic_poc_lsb_sps[0] = 7;
    sps.used_by_curr_pic_lt_sps_flag[0] = true;
    PPS pps;

    BitWriter w;
    w.put_bit(1);          // first_slice_segment_in_pic_flag
    w.put_ue(0);           // slice_pic_parameter_set_id
    w.put_ue(2);           // slice_type = I
    w.put_bits(0, 4);      // slice_pic_order_cnt_lsb
    w.put_bit(1);          // short_term_ref_pic_set_sps_flag
    w.put_ue(1);           // num_long_term_sps = 1
    w.put_ue(0);           // num_long_term_pics = 0
    // num_long_term_ref_pics_sps == 1 -> lt_idx_sps NOT coded (inferred 0).
    w.put_bit(0);          // delta_poc_msb_present_flag[0] = 0
    auto data = w.finish();

    BitstreamReader bs(data.data(), data.size());
    SliceHeader sh;
    ASSERT_TRUE(sh.parse(bs, sps, pps, NalUnitType::TRAIL_R, /*temporal_id=*/0));

    ASSERT_EQ(sh.num_long_term_sps, 1u);
    EXPECT_EQ(sh.lt_idx_sps[0], 0u);  // inferred
    EXPECT_EQ(sh.poc_lsb_lt[0], 7u);
    EXPECT_TRUE(sh.used_by_curr_pic_lt_flag[0]);
}
