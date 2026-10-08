#pragma once

// VUI and HRD parameters — spec Annex E (§E.2.1 vui_parameters, §E.2.2
// hrd_parameters, §E.2.3 sub_layer_hrd_parameters).
//
// Nothing here steers decoding; the syntax is parsed so the SPS fields that
// follow the VUI (the extension flags) are read from the right position, and
// so an SPS carrying HRD parameters is not mis-parsed.

#include <cstdint>

namespace hevc {

class BitstreamReader;

struct VuiParameters {
    bool aspect_ratio_info_present_flag = false;
    uint8_t aspect_ratio_idc = 0;
    uint16_t sar_width = 0;
    uint16_t sar_height = 0;
    bool video_full_range_flag = false;
    uint8_t colour_primaries = 2;
    uint8_t transfer_characteristics = 2;
    uint8_t matrix_coeffs = 2;
    bool field_seq_flag = false;
    bool default_display_window_flag = false;
    uint32_t def_disp_win_left_offset = 0;
    uint32_t def_disp_win_right_offset = 0;
    uint32_t def_disp_win_top_offset = 0;
    uint32_t def_disp_win_bottom_offset = 0;
    bool vui_timing_info_present_flag = false;
    uint32_t vui_num_units_in_tick = 0;
    uint32_t vui_time_scale = 0;
    bool vui_hrd_parameters_present_flag = false;
    bool bitstream_restriction_flag = false;
    // True when the stream needed the alternative layout some encoders write,
    // which omits the default display window fields (see parse_vui_parameters).
    bool used_alternative_syntax = false;
};

// §E.2.2 — hrd_parameters(commonInfPresentFlag, maxNumSubLayersMinus1).
// Returns false on a value outside its §E.3.2 range; throws on over-read.
bool parse_hrd_parameters(BitstreamReader& bs, bool commonInfPresentFlag,
                          int maxNumSubLayersMinus1);

// §E.2.1 — vui_parameters(). Returns false when the VUI cannot be parsed
// consistently (out-of-range value or over-read). On failure `bs` is left at
// an unspecified position; the caller decides how to continue.
//
// Some encoders emit a VUI without the default display window fields even
// though default_display_window_flag reads 1 — the timing info sits where the
// window would be. When the standard parse over-reads and that flag was set,
// the parse is retried with that alternative layout (as libavcodec does).
bool parse_vui_parameters(BitstreamReader& bs, int sps_max_sub_layers_minus1,
                          VuiParameters& vui);

} // namespace hevc
