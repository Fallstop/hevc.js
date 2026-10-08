#pragma once

// Hand-built SPS / PPS RBSPs for the parameter-set validation and tolerance
// tests. Every field the tests vary is exposed; the defaults describe a small
// valid Main-profile 4:2:0 stream (64x64, 8x8 min CB, 64x64 CTB).

#include <cstdint>
#include <functional>
#include <utility>
#include <vector>

#include "test_util.h"

namespace hevc_test {

struct SpsSpec {
    uint32_t max_sub_layers_minus1 = 0;
    uint32_t sps_id = 0;
    uint32_t chroma_format_idc = 1;
    uint32_t width = 64, height = 64;
    bool conf_win = false;
    uint32_t conf_l = 0, conf_r = 0, conf_t = 0, conf_b = 0;
    uint32_t bit_depth_luma_minus8 = 0, bit_depth_chroma_minus8 = 0;
    uint32_t log2_max_poc_lsb_minus4 = 4;
    uint32_t max_dec_pic_buffering_minus1 = 4, max_num_reorder = 0, max_latency_plus1 = 0;
    uint32_t log2_min_cb_minus3 = 0, log2_diff_max_min_cb = 3;
    uint32_t log2_min_tb_minus2 = 0, log2_diff_max_min_tb = 3;
    uint32_t depth_inter = 1, depth_intra = 1;
    bool amp = false, sao = false;
    // Explicit short-term RPSs: (num_negative_pics, num_positive_pics), deltas 1 apart
    std::vector<std::pair<uint32_t, uint32_t>> st_rps = {{1, 0}};
    bool long_term_present = false;
    uint32_t num_long_term_sps = 0;
    bool temporal_mvp = false, strong_intra = false;
    // When set, vui_parameters_present_flag = 1 and this writes vui_parameters()
    std::function<void(BitWriter&)> vui;
    bool extension_present = false;
    bool range_extension = false;
    // The 9 sps_range_extension() flags, MSB first (bit 3 = intra_smoothing_disabled_flag)
    uint32_t range_flags = 0;
    // Bits written after the extension flags, before rbsp_trailing_bits
    std::function<void(BitWriter&)> tail;
    bool trailing = true;
};

inline void write_ptl(BitWriter& w, uint32_t max_sub_layers_minus1) {
    w.put_bits(0, 2);   // general_profile_space
    w.put_bit(0);       // general_tier_flag
    w.put_bits(1, 5);   // general_profile_idc = Main
    w.put_bits(0x40000000u, 32);  // compatibility: Main
    w.put_bits(0x9, 4); // progressive, !interlaced, !non_packed, frame_only
    w.put_bits(0, 43);
    w.put_bit(0);
    w.put_bits(93, 8);  // general_level_idc (3.1)
    for (uint32_t i = 0; i < max_sub_layers_minus1; i++) { w.put_bit(0); w.put_bit(0); }
    if (max_sub_layers_minus1 > 0)
        for (uint32_t i = max_sub_layers_minus1; i < 8; i++) w.put_bits(0, 2);
}

inline std::vector<uint8_t> build_sps_rbsp(const SpsSpec& s) {
    BitWriter w;
    w.put_bits(0, 4);
    w.put_bits(s.max_sub_layers_minus1, 3);
    w.put_bit(1);
    write_ptl(w, s.max_sub_layers_minus1 > 6 ? 6 : s.max_sub_layers_minus1);
    w.put_ue(s.sps_id);
    w.put_ue(s.chroma_format_idc);
    if (s.chroma_format_idc == 3) w.put_bit(0);
    w.put_ue(s.width);
    w.put_ue(s.height);
    w.put_bit(s.conf_win ? 1 : 0);
    if (s.conf_win) { w.put_ue(s.conf_l); w.put_ue(s.conf_r); w.put_ue(s.conf_t); w.put_ue(s.conf_b); }
    w.put_ue(s.bit_depth_luma_minus8);
    w.put_ue(s.bit_depth_chroma_minus8);
    w.put_ue(s.log2_max_poc_lsb_minus4);
    w.put_bit(1);  // sps_sub_layer_ordering_info_present_flag
    for (uint32_t i = 0; i <= s.max_sub_layers_minus1 && i < 8; i++) {
        w.put_ue(s.max_dec_pic_buffering_minus1);
        w.put_ue(s.max_num_reorder);
        w.put_ue(s.max_latency_plus1);
    }
    w.put_ue(s.log2_min_cb_minus3);
    w.put_ue(s.log2_diff_max_min_cb);
    w.put_ue(s.log2_min_tb_minus2);
    w.put_ue(s.log2_diff_max_min_tb);
    w.put_ue(s.depth_inter);
    w.put_ue(s.depth_intra);
    w.put_bit(0);  // scaling_list_enabled_flag
    w.put_bit(s.amp ? 1 : 0);
    w.put_bit(s.sao ? 1 : 0);
    w.put_bit(0);  // pcm_enabled_flag
    w.put_ue(static_cast<uint32_t>(s.st_rps.size()));
    for (size_t i = 0; i < s.st_rps.size(); i++) {
        if (i != 0) w.put_bit(0);  // inter_ref_pic_set_prediction_flag
        w.put_ue(s.st_rps[i].first);
        w.put_ue(s.st_rps[i].second);
        for (uint32_t k = 0; k < s.st_rps[i].first; k++) { w.put_ue(0); w.put_bit(1); }
        for (uint32_t k = 0; k < s.st_rps[i].second; k++) { w.put_ue(0); w.put_bit(1); }
    }
    w.put_bit(s.long_term_present ? 1 : 0);
    if (s.long_term_present) {
        w.put_ue(s.num_long_term_sps);
        for (uint32_t i = 0; i < s.num_long_term_sps && i < 64; i++) {
            w.put_bits(i, static_cast<int>(s.log2_max_poc_lsb_minus4) + 4);
            w.put_bit(1);
        }
    }
    w.put_bit(s.temporal_mvp ? 1 : 0);
    w.put_bit(s.strong_intra ? 1 : 0);
    w.put_bit(s.vui ? 1 : 0);
    if (s.vui) s.vui(w);
    w.put_bit(s.extension_present ? 1 : 0);
    if (s.extension_present) {
        w.put_bit(s.range_extension ? 1 : 0);
        w.put_bit(0);
        w.put_bit(0);
        w.put_bits(0, 5);
        if (s.range_extension) w.put_bits(s.range_flags, 9);
    }
    if (s.tail) s.tail(w);
    if (s.trailing) w.trailing();
    return w.take();
}

struct PpsSpec {
    uint32_t pps_id = 0, sps_id = 0;
    bool dependent_slices = false, output_flag_present = false;
    uint32_t num_extra_slice_header_bits = 0;
    bool sign_data_hiding = false, cabac_init_present = false;
    uint32_t num_ref_idx_l0_default_minus1 = 0, num_ref_idx_l1_default_minus1 = 0;
    int32_t init_qp_minus26 = 0;
    bool constrained_intra = false, transform_skip = false;
    bool cu_qp_delta = false;
    uint32_t diff_cu_qp_delta_depth = 0;
    int32_t cb_qp_offset = 0, cr_qp_offset = 0;
    bool slice_chroma_qp_offsets_present = false;
    bool weighted_pred = false, weighted_bipred = false, transquant_bypass = false;
    bool tiles = false, entropy_sync = false;
    uint32_t tile_cols_minus1 = 0, tile_rows_minus1 = 0;
    bool uniform_spacing = true;
    std::vector<uint32_t> col_widths_minus1, row_heights_minus1;
    bool loop_filter_across_slices = false;
    bool lists_modification = false;
    uint32_t log2_parallel_merge_level_minus2 = 0;
    bool slice_header_extension = false;
    bool trailing = true;
};

inline std::vector<uint8_t> build_pps_rbsp(const PpsSpec& p) {
    BitWriter w;
    w.put_ue(p.pps_id);
    w.put_ue(p.sps_id);
    w.put_bit(p.dependent_slices ? 1 : 0);
    w.put_bit(p.output_flag_present ? 1 : 0);
    w.put_bits(p.num_extra_slice_header_bits, 3);
    w.put_bit(p.sign_data_hiding ? 1 : 0);
    w.put_bit(p.cabac_init_present ? 1 : 0);
    w.put_ue(p.num_ref_idx_l0_default_minus1);
    w.put_ue(p.num_ref_idx_l1_default_minus1);
    w.put_se(p.init_qp_minus26);
    w.put_bit(p.constrained_intra ? 1 : 0);
    w.put_bit(p.transform_skip ? 1 : 0);
    w.put_bit(p.cu_qp_delta ? 1 : 0);
    if (p.cu_qp_delta) w.put_ue(p.diff_cu_qp_delta_depth);
    w.put_se(p.cb_qp_offset);
    w.put_se(p.cr_qp_offset);
    w.put_bit(p.slice_chroma_qp_offsets_present ? 1 : 0);
    w.put_bit(p.weighted_pred ? 1 : 0);
    w.put_bit(p.weighted_bipred ? 1 : 0);
    w.put_bit(p.transquant_bypass ? 1 : 0);
    w.put_bit(p.tiles ? 1 : 0);
    w.put_bit(p.entropy_sync ? 1 : 0);
    if (p.tiles) {
        w.put_ue(p.tile_cols_minus1);
        w.put_ue(p.tile_rows_minus1);
        w.put_bit(p.uniform_spacing ? 1 : 0);
        if (!p.uniform_spacing) {
            for (uint32_t v : p.col_widths_minus1) w.put_ue(v);
            for (uint32_t v : p.row_heights_minus1) w.put_ue(v);
        }
        w.put_bit(1);  // loop_filter_across_tiles_enabled_flag
    }
    w.put_bit(p.loop_filter_across_slices ? 1 : 0);
    w.put_bit(0);  // deblocking_filter_control_present_flag
    w.put_bit(0);  // pps_scaling_list_data_present_flag
    w.put_bit(p.lists_modification ? 1 : 0);
    w.put_ue(p.log2_parallel_merge_level_minus2);
    w.put_bit(p.slice_header_extension ? 1 : 0);
    w.put_bit(0);  // pps_extension_present_flag
    if (p.trailing) w.trailing();
    return w.take();
}

// NAL header bytes (nuh_layer_id 0, TemporalId 0)
inline uint8_t nal_h0(hevc::NalUnitType t) { return static_cast<uint8_t>(static_cast<uint8_t>(t) << 1); }
constexpr uint8_t kNalH1 = 0x01;

inline std::vector<uint8_t> sps_nal(const SpsSpec& s) {
    return make_nal(nal_h0(hevc::NalUnitType::SPS_NUT), kNalH1, build_sps_rbsp(s));
}
inline std::vector<uint8_t> pps_nal(const PpsSpec& p) {
    return make_nal(nal_h0(hevc::NalUnitType::PPS_NUT), kNalH1, build_pps_rbsp(p));
}

// Parse one hand-built RBSP straight into an SPS / PPS.
inline bool parse_sps(const std::vector<uint8_t>& rbsp, hevc::SPS& sps) {
    hevc::BitstreamReader bs(rbsp.data(), rbsp.size());
    try { return sps.parse(bs); } catch (...) { return false; }
}

}  // namespace hevc_test
