// P0 spec-range validation: out-of-range parameter-set, RPS and slice-header
// values are refused at parse time (return false) instead of reaching a
// fixed-size array or an unbounded allocation; CABAC Exp-Golomb prefixes and
// CuQpDeltaVal are bounded; the DPB never stores more than 16 pictures.
// Run under ASan (Debug build): any overrun these values used to cause fails
// the test even where the parse result alone would not.

#include <gtest/gtest.h>

#include <stdexcept>

#include "decoding/cabac.h"
#include "decoding/dpb.h"
#include "decoding/syntax_elements.h"
#include "ps_builder.h"
#include "syntax/parameter_sets.h"
#include "syntax/pps.h"
#include "syntax/slice_header.h"
#include "syntax/sps.h"
#include "syntax/vps.h"

using namespace hevc;
using namespace hevc_test;

namespace {

SPS sps_from(const SpsSpec& spec) {
    SPS sps;
    EXPECT_TRUE(parse_sps(build_sps_rbsp(spec), sps));
    return sps;
}

bool parse_pps(const PpsSpec& spec, const SPS& sps, PPS& pps) {
    auto rbsp = build_pps_rbsp(spec);
    BitstreamReader bs(rbsp.data(), rbsp.size());
    try { return pps.parse(bs, sps); } catch (...) { return false; }
}

// Slice-header fields a test may want to break. Writes a TRAIL_R P slice (or
// an I slice) against the given SPS/PPS.
struct SliceSpec {
    bool first_slice = true;
    uint32_t address = 0;
    uint32_t slice_type = 1;  // P
    uint32_t poc_lsb = 1;
    bool inline_rps = false;
    uint32_t inline_neg = 1;
    uint32_t st_rps_idx = 0;
    bool override_num_ref = false;
    uint32_t num_ref_l0_minus1 = 0;
    uint32_t five_minus_max_merge = 0;
    int32_t slice_qp_delta = 0;
    uint32_t num_entry_points = 0;
    uint32_t offset_len_minus1 = 0;
};

std::vector<uint8_t> build_slice_rbsp(const SliceSpec& s, const SPS& sps, const PPS& pps) {
    BitWriter w;
    w.put_bit(s.first_slice ? 1 : 0);
    w.put_ue(0);  // slice_pic_parameter_set_id
    if (!s.first_slice) {
        int bits = 0;
        while ((1 << bits) < sps.PicSizeInCtbsY) bits++;
        w.put_bits(s.address, bits);
    }
    w.put_ue(s.slice_type);
    w.put_bits(s.poc_lsb, static_cast<int>(sps.log2_max_pic_order_cnt_lsb_minus4) + 4);
    w.put_bit(s.inline_rps ? 0 : 1);  // short_term_ref_pic_set_sps_flag
    if (s.inline_rps) {
        w.put_bit(0);  // inter_ref_pic_set_prediction_flag (num sets > 0)
        w.put_ue(s.inline_neg);
        w.put_ue(0);
        for (uint32_t i = 0; i < s.inline_neg && i < 40; i++) { w.put_ue(0); w.put_bit(1); }
    } else if (sps.num_short_term_ref_pic_sets > 1) {
        int bits = 0;
        while ((1u << bits) < sps.num_short_term_ref_pic_sets) bits++;
        w.put_bits(s.st_rps_idx, bits);
    }
    if (s.slice_type != 2) {
        w.put_bit(s.override_num_ref ? 1 : 0);
        if (s.override_num_ref) w.put_ue(s.num_ref_l0_minus1);
        w.put_ue(s.five_minus_max_merge);
    }
    w.put_se(s.slice_qp_delta);
    if (pps.tiles_enabled_flag || pps.entropy_coding_sync_enabled_flag) {
        w.put_ue(s.num_entry_points);
        if (s.num_entry_points > 0) {
            w.put_ue(s.offset_len_minus1);
            for (uint32_t i = 0; i < s.num_entry_points && i < 64; i++)
                w.put_bits(0, static_cast<int>(s.offset_len_minus1) + 1);
        }
    }
    w.trailing();
    auto out = w.take();
    out.resize(out.size() + 16, 0);  // slack so a rejection is not an over-read
    return out;
}

bool parse_slice(const SliceSpec& spec, const SPS& sps, const PPS& pps) {
    auto rbsp = build_slice_rbsp(spec, sps, pps);
    BitstreamReader bs(rbsp.data(), rbsp.size());
    SliceHeader sh;
    try {
        return sh.parse(bs, sps, pps, NalUnitType::TRAIL_R, 0);
    } catch (...) {
        return false;
    }
}

}  // namespace

// ---------------------------------------------------------------- SPS

TEST(Validation, DefaultSpsAndPpsParse) {
    SPS sps = sps_from({});
    EXPECT_EQ(sps.CtbSizeY, 64);
    PPS pps;
    EXPECT_TRUE(parse_pps({}, sps, pps));
    EXPECT_TRUE(parse_slice({}, sps, pps));
}

TEST(Validation, SpsRejectsSevenSubLayers) {
    SpsSpec s;
    s.max_sub_layers_minus1 = 7;  // indexes sub_layer_ordering[7] (array of 7)
    SPS sps;
    EXPECT_FALSE(parse_sps(build_sps_rbsp(s), sps));
}

TEST(Validation, SpsRejectsOversizedShortTermRps) {
    SpsSpec s;
    s.st_rps = {{17, 0}};  // DeltaPocS0 holds 16
    SPS sps;
    EXPECT_FALSE(parse_sps(build_sps_rbsp(s), sps));

    SpsSpec t;
    t.st_rps = {{10, 7}};  // each fits, the sum does not
    EXPECT_FALSE(parse_sps(build_sps_rbsp(t), sps));

    SpsSpec ok;
    ok.st_rps = {{16, 0}};
    EXPECT_TRUE(parse_sps(build_sps_rbsp(ok), sps));
}

TEST(Validation, SpsRejectsTooManyRpsAndLongTermEntries) {
    SpsSpec s;
    s.st_rps.assign(65, {1, 0});
    SPS sps;
    EXPECT_FALSE(parse_sps(build_sps_rbsp(s), sps));

    SpsSpec lt;
    lt.long_term_present = true;
    lt.num_long_term_sps = 33;  // lt_ref_pic_poc_lsb_sps[32]
    EXPECT_FALSE(parse_sps(build_sps_rbsp(lt), sps));
    lt.num_long_term_sps = 32;
    EXPECT_TRUE(parse_sps(build_sps_rbsp(lt), sps));
}

TEST(Validation, SpsRejectsOutOfRangeSizesAndDepths) {
    SPS sps;
    SpsSpec poc;
    poc.log2_max_poc_lsb_minus4 = 13;
    EXPECT_FALSE(parse_sps(build_sps_rbsp(poc), sps));

    SpsSpec ctb;
    ctb.log2_diff_max_min_cb = 4;  // 128x128 CTB
    EXPECT_FALSE(parse_sps(build_sps_rbsp(ctb), sps));

    SpsSpec tb;
    tb.log2_min_tb_minus2 = 1;  // MinTb 8 == MinCb 8
    EXPECT_FALSE(parse_sps(build_sps_rbsp(tb), sps));

    SpsSpec depth;
    depth.depth_intra = 5;  // > CtbLog2 - MinTbLog2 = 4
    EXPECT_FALSE(parse_sps(build_sps_rbsp(depth), sps));

    SpsSpec bd;
    bd.bit_depth_luma_minus8 = 6;  // 14-bit needs extended precision
    EXPECT_FALSE(parse_sps(build_sps_rbsp(bd), sps));

    SpsSpec id;
    id.sps_id = 16;
    EXPECT_FALSE(parse_sps(build_sps_rbsp(id), sps));

    SpsSpec area;  // 16384 x 4096 > level 6.2 MaxLumaPs
    area.width = 16384;
    area.height = 4096;
    EXPECT_FALSE(parse_sps(build_sps_rbsp(area), sps));
}

TEST(Validation, SpsClampsOutputTimingFields) {
    SpsSpec s;
    s.max_dec_pic_buffering_minus1 = 40;
    s.max_num_reorder = 50;
    SPS sps;
    ASSERT_TRUE(parse_sps(build_sps_rbsp(s), sps));
    EXPECT_EQ(sps.sub_layer_ordering[0].max_dec_pic_buffering_minus1, 15u);
    EXPECT_EQ(sps.sub_layer_ordering[0].max_num_reorder_pics, 15u);
}

TEST(Validation, VpsRejectsSevenSubLayers) {
    BitWriter w;
    w.put_bits(0, 4);
    w.put_bit(1);
    w.put_bit(1);
    w.put_bits(0, 6);
    w.put_bits(7, 3);  // vps_max_sub_layers_minus1 = 7
    w.put_bit(1);
    w.put_bits(0xffff, 16);
    write_ptl(w, 6);
    w.trailing();
    auto rbsp = w.take();
    rbsp.resize(rbsp.size() + 64, 0);
    BitstreamReader bs(rbsp.data(), rbsp.size());
    VPS vps;
    bool ok = true;
    try { ok = vps.parse(bs); } catch (...) { ok = false; }
    EXPECT_FALSE(ok);
}

// ---------------------------------------------------------------- PPS

TEST(Validation, PpsRejectsOutOfRangeFields) {
    SPS sps = sps_from({});
    PPS pps;
    PpsSpec refs;
    refs.num_ref_idx_l0_default_minus1 = 15;
    EXPECT_FALSE(parse_pps(refs, sps, pps));

    PpsSpec cb;
    cb.cb_qp_offset = 13;
    EXPECT_FALSE(parse_pps(cb, sps, pps));

    PpsSpec qp;
    qp.init_qp_minus26 = -27;  // 8-bit: QpBdOffsetY == 0
    EXPECT_FALSE(parse_pps(qp, sps, pps));

    PpsSpec id;
    id.pps_id = 64;
    EXPECT_FALSE(parse_pps(id, sps, pps));

    PpsSpec merge;
    merge.log2_parallel_merge_level_minus2 = 5;
    EXPECT_FALSE(parse_pps(merge, sps, pps));
}

TEST(Validation, PpsRejectsTileLayoutsThatDoNotFitThePicture) {
    SpsSpec wide;
    wide.width = 256;   // 4x1 CTBs
    SPS sps = sps_from(wide);
    PPS pps;

    PpsSpec too_many;
    too_many.tiles = true;
    too_many.tile_cols_minus1 = 4;  // 5 columns over 4 CTBs
    EXPECT_FALSE(parse_pps(too_many, sps, pps));

    PpsSpec overflow;
    overflow.tiles = true;
    overflow.tile_cols_minus1 = 1;
    overflow.uniform_spacing = false;
    overflow.col_widths_minus1 = {9};  // first column 10 CTBs wide, picture is 4
    overflow.row_heights_minus1 = {};
    EXPECT_FALSE(parse_pps(overflow, sps, pps));

    PpsSpec ok;
    ok.tiles = true;
    ok.tile_cols_minus1 = 1;
    ok.uniform_spacing = false;
    ok.col_widths_minus1 = {0};
    EXPECT_TRUE(parse_pps(ok, sps, pps));
    EXPECT_EQ(pps.CtbAddrRsToTs.size(), 4u);
}

// ---------------------------------------------------------------- slice header

TEST(Validation, SliceRejectsAddressPastThePicture) {
    SpsSpec wide;
    wide.width = 192;  // 3 CTBs -> 2-bit address can code 3
    SPS sps = sps_from(wide);
    PPS pps;
    ASSERT_TRUE(parse_pps({}, sps, pps));
    SliceSpec ok;
    ok.first_slice = false;
    ok.address = 2;
    EXPECT_TRUE(parse_slice(ok, sps, pps));
    SliceSpec bad = ok;
    bad.address = 3;
    EXPECT_FALSE(parse_slice(bad, sps, pps));
}

TEST(Validation, SliceRejectsOutOfRangeFields) {
    SpsSpec spec;
    spec.st_rps = {{1, 0}, {2, 0}, {3, 0}};  // 2-bit index can code 3
    SPS sps = sps_from(spec);
    PPS pps;
    ASSERT_TRUE(parse_pps({}, sps, pps));

    SliceSpec type;
    type.slice_type = 3;
    EXPECT_FALSE(parse_slice(type, sps, pps));

    SliceSpec idx;
    idx.st_rps_idx = 3;
    EXPECT_FALSE(parse_slice(idx, sps, pps));
    idx.st_rps_idx = 2;
    EXPECT_TRUE(parse_slice(idx, sps, pps));

    SliceSpec rps;
    rps.inline_rps = true;
    rps.inline_neg = 17;  // inline RPS overrunning DeltaPocS0[16]
    EXPECT_FALSE(parse_slice(rps, sps, pps));

    SliceSpec refs;
    refs.override_num_ref = true;
    refs.num_ref_l0_minus1 = 15;
    EXPECT_FALSE(parse_slice(refs, sps, pps));

    SliceSpec merge;
    merge.five_minus_max_merge = 5;  // MaxNumMergeCand 0
    EXPECT_FALSE(parse_slice(merge, sps, pps));

    SliceSpec qp;
    qp.slice_qp_delta = 26;  // SliceQpY 52
    EXPECT_FALSE(parse_slice(qp, sps, pps));
    qp.slice_qp_delta = -27;
    EXPECT_FALSE(parse_slice(qp, sps, pps));
}

TEST(Validation, SliceBoundsEntryPointCountBeforeAllocating) {
    SpsSpec spec;
    spec.height = 128;  // 2 CTB rows -> at most 1 WPP entry point
    SPS sps = sps_from(spec);
    PPS pps;
    PpsSpec wpp;
    wpp.entropy_sync = true;
    ASSERT_TRUE(parse_pps(wpp, sps, pps));

    SliceSpec ok;
    ok.num_entry_points = 1;
    EXPECT_TRUE(parse_slice(ok, sps, pps));

    SliceSpec bad;
    bad.num_entry_points = 0x7fffffff;  // used to drive resize() directly
    EXPECT_FALSE(parse_slice(bad, sps, pps));

    SliceSpec len;
    len.num_entry_points = 1;
    len.offset_len_minus1 = 32;
    EXPECT_FALSE(parse_slice(len, sps, pps));
}

// ---------------------------------------------------------------- bitstream / CABAC

TEST(Validation, SeekPastEndClampsInsteadOfAsserting) {
    const uint8_t data[4] = {0xAB, 0xCD, 0xEF, 0x01};
    BitstreamReader bs(data, sizeof(data));
    bs.seek_to_byte(1000);
    EXPECT_TRUE(bs.eof());
    EXPECT_THROW(bs.read_bits(1), std::runtime_error);
    bs.seek_to_byte(2);
    EXPECT_EQ(bs.read_bits(8), 0xEFu);
}

TEST(Validation, CabacRejectsInvalidInitialOffset) {
    std::vector<uint8_t> ones(64, 0xFF);
    BitstreamReader bs(ones.data(), ones.size());
    CabacEngine cabac;
    EXPECT_THROW(cabac.init_decoder(bs), std::runtime_error);
}

// A run of 1-valued bypass bins longer than any binarization needs must fail
// the slice rather than shift past 31 bits.
TEST(Validation, ExpGolombPrefixIsCapped) {
    // ivlOffset 509 (just valid), then all ones: with ivlCurrRange 510 every
    // bypass bin decodes as 1 and the offset stays at 509.
    std::vector<uint8_t> data(64, 0xFF);
    data[0] = 0xFE;  // first 9 bits = 0b111111101 = 509
    {
        BitstreamReader bs(data.data(), data.size());
        CabacEngine cabac;
        cabac.init_decoder(bs);
        EXPECT_THROW(decode_coeff_abs_level_remaining(cabac, 0), std::runtime_error);
    }
    {
        BitstreamReader bs(data.data(), data.size());
        CabacEngine cabac;
        cabac.init_decoder(bs);
        EXPECT_THROW(decode_mvd(cabac), std::runtime_error);
    }
}

// ---------------------------------------------------------------- DPB

// A corrupt RPS can keep every picture marked as reference; storage is capped
// at 16 pictures anyway, outputting first and then dropping the oldest refs.
TEST(Validation, DpbStorageIsCappedAtSixteenPictures) {
    DPB dpb;
    DPB::OutputLimits lim;
    lim.max_num_reorder = 16;
    lim.max_dec_pic_buffering = 64;  // what a corrupt SPS would ask for
    size_t peak = 0;
    for (int i = 0; i < 40; i++) {
        dpb.clear_current();
        dpb.bump_before_decode(lim, false);
        Picture* pic = dpb.alloc_picture(16, 16, ChromaFormat::YUV420, 8, 8);
        pic->poc = i;
        pic->used_for_short_term_ref = true;
        pic->needed_for_output = true;
        dpb.bump_after_decode(lim);
        dpb.take_output();
        peak = std::max(peak, dpb.pictures().size());
    }
    EXPECT_LE(peak, static_cast<size_t>(DPB::kMaxDpbPictures + 1));
}
