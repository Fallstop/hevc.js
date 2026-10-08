#include "syntax/vui.h"
#include "bitstream/bitstream_reader.h"
#include "common/debug.h"

namespace hevc {

namespace {

// §E.2.3 — sub_layer_hrd_parameters(subLayerId)
void parse_sub_layer_hrd_parameters(BitstreamReader& bs, uint32_t cpb_cnt_minus1,
                                    bool sub_pic_hrd_params_present_flag) {
    for (uint32_t i = 0; i <= cpb_cnt_minus1; i++) {
        bs.read_ue();  // bit_rate_value_minus1
        bs.read_ue();  // cpb_size_value_minus1
        if (sub_pic_hrd_params_present_flag) {
            bs.read_ue();  // cpb_size_du_value_minus1
            bs.read_ue();  // bit_rate_du_value_minus1
        }
        bs.read_flag();  // cbr_flag
    }
}

}  // namespace

bool parse_hrd_parameters(BitstreamReader& bs, bool commonInfPresentFlag,
                          int maxNumSubLayersMinus1) {
    bool nal_hrd_parameters_present_flag = false;
    bool vcl_hrd_parameters_present_flag = false;
    bool sub_pic_hrd_params_present_flag = false;
    if (commonInfPresentFlag) {
        nal_hrd_parameters_present_flag = bs.read_flag();
        vcl_hrd_parameters_present_flag = bs.read_flag();
        if (nal_hrd_parameters_present_flag || vcl_hrd_parameters_present_flag) {
            sub_pic_hrd_params_present_flag = bs.read_flag();
            if (sub_pic_hrd_params_present_flag) {
                bs.read_bits(8);  // tick_divisor_minus2
                bs.read_bits(5);  // du_cpb_removal_delay_increment_length_minus1
                bs.read_flag();   // sub_pic_cpb_params_in_pic_timing_sei_flag
                bs.read_bits(5);  // dpb_output_delay_du_length_minus1
            }
            bs.read_bits(4);  // bit_rate_scale
            bs.read_bits(4);  // cpb_size_scale
            if (sub_pic_hrd_params_present_flag)
                bs.read_bits(4);  // cpb_size_du_scale
            bs.read_bits(5);  // initial_cpb_removal_delay_length_minus1
            bs.read_bits(5);  // au_cpb_removal_delay_length_minus1
            bs.read_bits(5);  // dpb_output_delay_length_minus1
        }
    }
    for (int i = 0; i <= maxNumSubLayersMinus1; i++) {
        bool fixed_pic_rate_general_flag = bs.read_flag();
        // §E.3.2: inferred 1 when fixed_pic_rate_general_flag is 1
        bool fixed_pic_rate_within_cvs_flag = true;
        if (!fixed_pic_rate_general_flag)
            fixed_pic_rate_within_cvs_flag = bs.read_flag();
        bool low_delay_hrd_flag = false;
        if (fixed_pic_rate_within_cvs_flag) {
            uint32_t elemental_duration_in_tc_minus1 = bs.read_ue();
            if (elemental_duration_in_tc_minus1 > 2047) return false;  // §E.3.2: 0..2047
        } else {
            low_delay_hrd_flag = bs.read_flag();
        }
        uint32_t cpb_cnt_minus1 = 0;
        if (!low_delay_hrd_flag) {
            cpb_cnt_minus1 = bs.read_ue();
            if (cpb_cnt_minus1 > 31) return false;  // §E.3.2: 0..31
        }
        if (nal_hrd_parameters_present_flag)
            parse_sub_layer_hrd_parameters(bs, cpb_cnt_minus1, sub_pic_hrd_params_present_flag);
        if (vcl_hrd_parameters_present_flag)
            parse_sub_layer_hrd_parameters(bs, cpb_cnt_minus1, sub_pic_hrd_params_present_flag);
    }
    return true;
}

namespace {

// Everything from vui_timing_info_present_flag to the end of the VUI.
bool parse_vui_tail(BitstreamReader& bs, int sps_max_sub_layers_minus1, VuiParameters& vui) {
    vui.vui_timing_info_present_flag = bs.read_flag();
    if (vui.vui_timing_info_present_flag) {
        vui.vui_num_units_in_tick = bs.read_bits(32);
        vui.vui_time_scale = bs.read_bits(32);
        bool vui_poc_proportional_to_timing_flag = bs.read_flag();
        if (vui_poc_proportional_to_timing_flag)
            bs.read_ue();  // vui_num_ticks_poc_diff_one_minus1
        vui.vui_hrd_parameters_present_flag = bs.read_flag();
        if (vui.vui_hrd_parameters_present_flag &&
            !parse_hrd_parameters(bs, true, sps_max_sub_layers_minus1))
            return false;
    }
    vui.bitstream_restriction_flag = bs.read_flag();
    if (vui.bitstream_restriction_flag) {
        bs.read_flag();  // tiles_fixed_structure_flag
        bs.read_flag();  // motion_vectors_over_pic_boundaries_flag
        bs.read_flag();  // restricted_ref_pic_lists_flag
        uint32_t min_spatial_segmentation_idc = bs.read_ue();
        uint32_t max_bytes_per_pic_denom = bs.read_ue();
        uint32_t max_bits_per_min_cu_denom = bs.read_ue();
        uint32_t log2_max_mv_length_horizontal = bs.read_ue();
        uint32_t log2_max_mv_length_vertical = bs.read_ue();
        // §E.3.1 ranges: a mis-parsed VUI usually trips one of these
        if (min_spatial_segmentation_idc > 4095 || max_bytes_per_pic_denom > 16 ||
            max_bits_per_min_cu_denom > 16 || log2_max_mv_length_horizontal > 15 ||
            log2_max_mv_length_vertical > 15)
            return false;
    }
    return true;
}

}  // namespace

bool parse_vui_parameters(BitstreamReader& bs, int sps_max_sub_layers_minus1,
                          VuiParameters& vui) {
    vui = VuiParameters{};
    vui.aspect_ratio_info_present_flag = bs.read_flag();
    if (vui.aspect_ratio_info_present_flag) {
        vui.aspect_ratio_idc = static_cast<uint8_t>(bs.read_bits(8));
        if (vui.aspect_ratio_idc == 255) {  // EXTENDED_SAR
            vui.sar_width = static_cast<uint16_t>(bs.read_bits(16));
            vui.sar_height = static_cast<uint16_t>(bs.read_bits(16));
        }
    }
    if (bs.read_flag())  // overscan_info_present_flag
        bs.read_flag();  // overscan_appropriate_flag
    if (bs.read_flag()) {  // video_signal_type_present_flag
        bs.read_bits(3);   // video_format
        vui.video_full_range_flag = bs.read_flag();
        if (bs.read_flag()) {  // colour_description_present_flag
            vui.colour_primaries = static_cast<uint8_t>(bs.read_bits(8));
            vui.transfer_characteristics = static_cast<uint8_t>(bs.read_bits(8));
            vui.matrix_coeffs = static_cast<uint8_t>(bs.read_bits(8));
        }
    }
    if (bs.read_flag()) {  // chroma_loc_info_present_flag
        if (bs.read_ue() > 5) return false;  // chroma_sample_loc_type_top_field
        if (bs.read_ue() > 5) return false;  // chroma_sample_loc_type_bottom_field
    }
    bs.read_flag();  // neutral_chroma_indication_flag
    vui.field_seq_flag = bs.read_flag();
    bs.read_flag();  // frame_field_info_present_flag

    // Position of default_display_window_flag, for the alternative-syntax retry
    BitstreamReader at_window = bs;
    vui.default_display_window_flag = bs.read_flag();
    if (vui.default_display_window_flag) {
        vui.def_disp_win_left_offset = bs.read_ue();
        vui.def_disp_win_right_offset = bs.read_ue();
        vui.def_disp_win_top_offset = bs.read_ue();
        vui.def_disp_win_bottom_offset = bs.read_ue();
    }

    bool ok = false;
    BitstreamReader standard = bs;
    VuiParameters standard_vui = vui;
    try {
        ok = parse_vui_tail(standard, sps_max_sub_layers_minus1, standard_vui);
    } catch (...) {
        ok = false;
    }
    if (ok) {
        bs = standard;
        vui = standard_vui;
        return true;
    }
    if (!vui.default_display_window_flag) return false;

    // Alternative layout: the default display window is absent and the bit
    // read as its flag is vui_timing_info_present_flag.
    HEVC_LOG(PARSE, "VUI: standard parse failed, retrying without default display window%s", "");
    VuiParameters alt = vui;
    alt.default_display_window_flag = false;
    alt.def_disp_win_left_offset = alt.def_disp_win_right_offset = 0;
    alt.def_disp_win_top_offset = alt.def_disp_win_bottom_offset = 0;
    alt.used_alternative_syntax = true;
    BitstreamReader alt_bs = at_window;
    if (!parse_vui_tail(alt_bs, sps_max_sub_layers_minus1, alt)) return false;
    bs = alt_bs;
    vui = alt;
    return true;
}

} // namespace hevc
