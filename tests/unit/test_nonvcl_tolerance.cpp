// P1 — non-VCL failures are contained: a corrupt VPS/SPS/PPS/SEI never throws
// out of feed() (the previous valid set stays in force), the VUI is parsed in
// full — hrd_parameters() included — and tolerated when it does not parse, an
// invalid conformance window is ignored, a PPS may precede its SPS, and a PPS
// is re-derived when the SPS it refers to is replaced.

#include <gtest/gtest.h>

#include "ps_builder.h"
#include "syntax/parameter_sets.h"

using namespace hevc;
using namespace hevc_test;

namespace {

// VUI with video signal type, timing info and NAL+VCL HRD parameters with
// sub-picture params and two CPBs, then bitstream restrictions.
void write_hrd_vui(BitWriter& w) {
    w.put_bit(0);  // aspect_ratio_info_present_flag
    w.put_bit(0);  // overscan_info_present_flag
    w.put_bit(1);  // video_signal_type_present_flag
    w.put_bits(5, 3);
    w.put_bit(0);
    w.put_bit(1);  // colour_description_present_flag
    w.put_bits(1, 8); w.put_bits(1, 8); w.put_bits(1, 8);
    w.put_bit(0);  // chroma_loc_info_present_flag
    w.put_bit(0); w.put_bit(0); w.put_bit(0);
    w.put_bit(0);  // default_display_window_flag
    w.put_bit(1);  // vui_timing_info_present_flag
    w.put_bits(1001, 32);
    w.put_bits(30000, 32);
    w.put_bit(0);  // vui_poc_proportional_to_timing_flag
    w.put_bit(1);  // vui_hrd_parameters_present_flag
    // hrd_parameters(1, 0)
    w.put_bit(1);  // nal_hrd_parameters_present_flag
    w.put_bit(1);  // vcl_hrd_parameters_present_flag
    w.put_bit(1);  // sub_pic_hrd_params_present_flag
    w.put_bits(23, 8); w.put_bits(7, 5); w.put_bit(1); w.put_bits(4, 5);
    w.put_bits(3, 4); w.put_bits(5, 4); w.put_bits(5, 4);
    w.put_bits(23, 5); w.put_bits(23, 5); w.put_bits(23, 5);
    w.put_bit(0);  // fixed_pic_rate_general_flag[0]
    w.put_bit(1);  // fixed_pic_rate_within_cvs_flag[0]
    w.put_ue(0);   // elemental_duration_in_tc_minus1
    w.put_ue(1);   // cpb_cnt_minus1 (low_delay_hrd_flag inferred 0)
    for (int nal_vcl = 0; nal_vcl < 2; nal_vcl++)
        for (int i = 0; i < 2; i++) {
            w.put_ue(5000); w.put_ue(9000); w.put_ue(9000); w.put_ue(5000); w.put_bit(1);
        }
    w.put_bit(1);  // bitstream_restriction_flag
    w.put_bit(0); w.put_bit(1); w.put_bit(1);
    w.put_ue(0); w.put_ue(2); w.put_ue(1); w.put_ue(15); w.put_ue(15);
}

std::vector<uint8_t> concat(std::initializer_list<std::vector<uint8_t>> parts) {
    std::vector<uint8_t> out;
    for (const auto& p : parts) out.insert(out.end(), p.begin(), p.end());
    return out;
}

// Split a fixture into (VPS, SPS, PPS) NAL chunks and the rest of the stream.
struct Split {
    std::vector<uint8_t> vps, sps, pps, rest;
};
Split split_parameter_sets(const std::vector<uint8_t>& data) {
    NalParser parser;
    Split s;
    for (const auto& nal : parser.parse(data.data(), data.size())) {
        auto chunk = nal_chunk(data, nal);
        auto t = nal.header.nal_unit_type;
        auto& dst = (t == NalUnitType::VPS_NUT && s.vps.empty()) ? s.vps
                  : (t == NalUnitType::SPS_NUT && s.sps.empty()) ? s.sps
                  : (t == NalUnitType::PPS_NUT && s.pps.empty()) ? s.pps
                  : s.rest;
        dst.insert(dst.end(), chunk.begin(), chunk.end());
    }
    return s;
}

std::vector<std::vector<uint8_t>> decode_planes(const std::vector<uint8_t>& data, DecodeStats* stats = nullptr) {
    Decoder dec;
    EXPECT_EQ(dec.decode(data.data(), data.size()), DecodeStatus::OK);
    std::vector<std::vector<uint8_t>> out;
    for (const Picture* p : dec.output_pictures()) out.push_back(p->plane_bytes[0]);
    if (stats) *stats = dec.stats();
    return out;
}

}  // namespace

// §E.2.2: with HRD parameters the old VUI skipper returned early and read the
// SPS extension flags out of the HRD data. They must now come from where the
// stream put them.
TEST(NonVclTolerance, VuiWithHrdParametersIsParsedInFull) {
    SpsSpec s;
    s.vui = write_hrd_vui;
    s.extension_present = true;
    s.range_extension = true;
    s.range_flags = 1u << 3;  // intra_smoothing_disabled_flag (index 5 of 9)
    SPS sps;
    ASSERT_TRUE(parse_sps(build_sps_rbsp(s), sps));
    EXPECT_FALSE(sps.vui_tolerated);
    EXPECT_TRUE(sps.vui.vui_hrd_parameters_present_flag);
    EXPECT_EQ(sps.vui.vui_time_scale, 30000u);
    EXPECT_TRUE(sps.intra_smoothing_disabled_flag);
}

// A VUI cut short keeps the SPS (with its extension flags at 0).
TEST(NonVclTolerance, TruncatedVuiKeepsTheSps) {
    SpsSpec s;
    s.vui = write_hrd_vui;
    s.width = 176;
    s.height = 144;
    auto rbsp = build_sps_rbsp(s);
    rbsp.resize(rbsp.size() - 8);  // ends inside the HRD parameters
    SPS sps;
    ASSERT_TRUE(parse_sps(rbsp, sps));
    EXPECT_TRUE(sps.vui_tolerated);
    EXPECT_EQ(sps.pic_width_in_luma_samples, 176u);
    EXPECT_EQ(sps.pic_height_in_luma_samples, 144u);
    EXPECT_FALSE(sps.intra_smoothing_disabled_flag);
}

// Some encoders set default_display_window_flag but write the timing info
// where the window offsets belong. Either the alternative layout is
// recognised or the VUI is tolerated — the SPS must survive.
TEST(NonVclTolerance, AlternativeVuiLayoutKeepsTheSps) {
    SpsSpec s;
    s.vui = [](BitWriter& w) {
        w.put_bit(0); w.put_bit(0); w.put_bit(0); w.put_bit(0);
        w.put_bit(0); w.put_bit(0); w.put_bit(0);
        w.put_bit(1);  // default_display_window_flag ... but no offsets follow
        w.put_bit(1);  // vui_timing_info_present_flag
        w.put_bits(1, 32);
        w.put_bits(25, 32);
        w.put_bit(0);
        w.put_bit(0);
        w.put_bit(0);  // bitstream_restriction_flag
    };
    SPS sps;
    ASSERT_TRUE(parse_sps(build_sps_rbsp(s), sps));
    EXPECT_TRUE(sps.vui.used_alternative_syntax || sps.vui_tolerated);
    EXPECT_EQ(sps.pic_width_in_luma_samples, 64u);
}

// An SPS whose extension "flags" are really mis-read VUI bits (the RBSP does
// not end where the parse does) must not switch range-extension tools on.
TEST(NonVclTolerance, ExtensionFlagsAreIgnoredWhenTheSpsDoesNotEndThere) {
    SpsSpec s;
    s.extension_present = true;
    s.range_extension = true;
    s.range_flags = 1u << 3;
    s.tail = [](BitWriter& w) { w.put_bits(0x5A, 8); };  // junk before the trailing bits
    SPS sps;
    ASSERT_TRUE(parse_sps(build_sps_rbsp(s), sps));
    EXPECT_TRUE(sps.vui_tolerated);
    EXPECT_FALSE(sps.intra_smoothing_disabled_flag);
}

TEST(NonVclTolerance, InvalidConformanceWindowIsIgnored) {
    SpsSpec s;
    s.conf_win = true;
    s.conf_l = 40;  // 2 * 40 = 80 luma samples of a 64-wide picture
    SPS sps;
    ASSERT_TRUE(parse_sps(build_sps_rbsp(s), sps));
    EXPECT_FALSE(sps.conformance_window_flag);
    EXPECT_EQ(sps.conf_win_left_offset, 0u);
}

// A truncated duplicate SPS and PPS, and a malformed SEI, are counted and
// contained: the rest of the buffer decodes exactly as without them.
TEST(NonVclTolerance, CorruptParameterSetsAndSeiAreContained) {
    auto data = read_file(fixture("full_qcif_10f.265"));
    ASSERT_FALSE(data.empty());
    auto aus = split_access_units(data);
    ASSERT_GE(aus.size(), 4u);
    auto ps = split_parameter_sets(aus[0]);

    std::vector<uint8_t> bad_sps(ps.sps.begin(), ps.sps.begin() + 10);
    std::vector<uint8_t> bad_pps(ps.pps.begin(), ps.pps.begin() + 7);
    BitWriter sei;
    sei.put_bits(6, 8);     // payloadType = recovery_point
    sei.put_bits(200, 8);   // payloadSize far past the end
    sei.put_bits(0x80, 8);
    auto bad_sei = make_nal(nal_h0(NalUnitType::PREFIX_SEI), kNalH1, sei.take());

    std::vector<uint8_t> corrupted;
    for (size_t i = 0; i < aus.size(); i++) {
        if (i == 2) corrupted = concat({corrupted, bad_sps, bad_pps, bad_sei});
        corrupted.insert(corrupted.end(), aus[i].begin(), aus[i].end());
    }

    DecodeStats stats;
    auto clean = decode_planes(data);
    auto got = decode_planes(corrupted, &stats);
    EXPECT_EQ(got, clean);
    EXPECT_GE(stats.nal_errors, 2u);
}

// §7.4.3.3 does not tie the PPS syntax to the SPS; a PPS that arrives first is
// kept and derived when its SPS shows up.
TEST(NonVclTolerance, PpsBeforeSpsIsKept) {
    auto data = read_file(fixture("full_qcif_10f.265"));
    auto ps = split_parameter_sets(data);
    auto reordered = concat({ps.vps, ps.pps, ps.sps, ps.rest});
    EXPECT_EQ(decode_planes(reordered), decode_planes(data));
}

// An SPS replaced by one of another size without the PPS being re-sent: the
// PPS tile tables (sized from the old SPS) are re-derived instead of indexed
// out of bounds. Pixel output is not the point (the withheld PPS differs), the
// absence of an ASan report and the re-sized tables are.
TEST(NonVclTolerance, PpsIsRederivedWhenItsSpsChanges) {
    auto a = read_file(fixture("i_64x64_qp22.265"));
    auto b = read_file(fixture("p_qcif_10f.265"));
    ASSERT_FALSE(a.empty());
    ASSERT_FALSE(b.empty());
    auto pa = split_parameter_sets(a);
    auto pb = split_parameter_sets(b);
    auto stream = concat({a, pb.vps, pb.sps, pb.rest});  // b's PPS withheld

    Decoder dec;
    ASSERT_EQ(dec.decode(stream.data(), stream.size()), DecodeStatus::OK);
    EXPECT_FALSE(dec.output_pictures().empty());
    const PPS* pps = dec.parameter_sets().get_pps(0);
    ASSERT_NE(pps, nullptr);
    EXPECT_EQ(pps->CtbAddrRsToTs.size(),
              static_cast<size_t>(dec.parameter_sets().get_sps(0)->PicSizeInCtbsY));
    EXPECT_EQ(dec.parameter_sets().get_sps(0)->pic_width_in_luma_samples, 176u);
}
