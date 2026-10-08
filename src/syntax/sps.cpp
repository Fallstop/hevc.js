#include "syntax/sps.h"
#include "bitstream/bitstream_reader.h"
#include "decoding/cabac_tables.h"  // diag_scan_4x4 / diag_scan_8x8 (up-right diagonal scan)
#include "common/debug.h"

#include <algorithm>
#include <cstring>

namespace hevc {

// Reject a parameter set (or RPS) whose value is outside the range the spec
// allows. Out-of-range values are what turns a corrupt header into a stack or
// heap overrun downstream, so they are refused at parse time.
#define REJECT_IF(cond, ...)                                       \
    do {                                                            \
        if (cond) {                                                 \
            HEVC_LOG(PARSE, "SPS/RPS rejected: " __VA_ARGS__);      \
            return false;                                           \
        }                                                           \
    } while (0)

// §A.4.1 Table A.8, level 6.2 (the highest defined): MaxLumaPs. With the
// per-dimension cap below it bounds every per-picture allocation.
static constexpr uint64_t kMaxLumaPs = 35651584;
static constexpr uint32_t kMaxLumaDimension = 16384;

// ---- Default scaling list matrices (spec Tables 7-3 to 7-5) ----

static const uint8_t default_scaling_list_4x4[16] = {
    16, 16, 16, 16, 16, 16, 16, 16,
    16, 16, 16, 16, 16, 16, 16, 16,
};

static const uint8_t default_scaling_list_8x8_intra[64] = {
    16, 16, 16, 16, 17, 18, 21, 24,
    16, 16, 16, 16, 17, 19, 22, 25,
    16, 16, 17, 18, 20, 22, 25, 29,
    16, 16, 18, 21, 24, 27, 31, 36,
    17, 17, 20, 24, 30, 35, 41, 47,
    18, 19, 22, 27, 35, 44, 54, 65,
    21, 22, 25, 31, 41, 54, 70, 88,
    24, 25, 29, 36, 47, 65, 88, 115,
};

static const uint8_t default_scaling_list_8x8_inter[64] = {
    16, 16, 16, 16, 17, 18, 20, 24,
    16, 16, 16, 17, 18, 20, 24, 25,
    16, 16, 17, 18, 20, 24, 25, 28,
    16, 17, 18, 20, 24, 25, 28, 33,
    17, 18, 20, 24, 25, 28, 33, 41,
    18, 20, 24, 25, 28, 33, 41, 54,
    20, 24, 25, 28, 33, 41, 54, 71,
    24, 25, 28, 33, 41, 54, 71, 91,
};

// ---- ScalingListData ----

void ScalingListData::set_defaults() {
    // sizeId 0 (4x4): all flat 16
    for (int matrixId = 0; matrixId < 6; matrixId++) {
        std::memcpy(scaling_list[0][matrixId].data(), default_scaling_list_4x4, 16);
    }
    // sizeId 1 (8x8): intra for matrixId 0-2, inter for 3-5
    for (int matrixId = 0; matrixId < 6; matrixId++) {
        const uint8_t* src = (matrixId < 3) ? default_scaling_list_8x8_intra
                                             : default_scaling_list_8x8_inter;
        std::memcpy(scaling_list[1][matrixId].data(), src, 64);
    }
    // sizeId 2 (16x16): same as sizeId 1 (uses 8x8 coefficients)
    for (int matrixId = 0; matrixId < 6; matrixId++) {
        const uint8_t* src = (matrixId < 3) ? default_scaling_list_8x8_intra
                                             : default_scaling_list_8x8_inter;
        std::memcpy(scaling_list[2][matrixId].data(), src, 64);
    }
    // sizeId 3 (32x32): matrixId 0 = intra, matrixId 3 = inter
    std::memcpy(scaling_list[3][0].data(), default_scaling_list_8x8_intra, 64);
    std::memcpy(scaling_list[3][3].data(), default_scaling_list_8x8_inter, 64);

    // Default DC coefficients = 16
    for (int i = 0; i < 2; i++) {
        for (int j = 0; j < 6; j++) {
            scaling_list_dc[i][j] = 16;
        }
    }
}

// Spec §7.3.4 — scaling_list_data()
bool ScalingListData::parse(BitstreamReader& bs) {
    // Start with defaults, then override with parsed data
    set_defaults();

    for (int sizeId = 0; sizeId < 4; sizeId++) {
        int step = (sizeId == 3) ? 3 : 1;
        for (int matrixId = 0; matrixId < 6; matrixId += step) {
            bool scaling_list_pred_mode_flag = bs.read_flag();
            if (!scaling_list_pred_mode_flag) {
                // Copy from another matrix
                uint32_t delta = bs.read_ue();
                // §7.4.5: delta in 0..matrixId / (sizeId == 3 ? 3 : 1)
                REJECT_IF(delta > static_cast<uint32_t>(matrixId / step),
                          "scaling_list_pred_matrix_id_delta=%u", delta);
                if (delta == 0) {
                    // Use default values (already set)
                } else {
                    // Copy from matrixId - delta (same sizeId)
                    int refMatrixId = matrixId - static_cast<int>(delta) * step;
                    if (refMatrixId >= 0) {
                        scaling_list[sizeId][matrixId] = scaling_list[sizeId][refMatrixId];
                        if (sizeId > 1) {
                            scaling_list_dc[sizeId - 2][matrixId] =
                                scaling_list_dc[sizeId - 2][refMatrixId];
                        }
                    }
                }
            } else {
                // Parse explicit coefficients
                int nextCoef = 8;
                int coefNum = std::min(64, 1 << (4 + (sizeId << 1)));

                if (sizeId > 1) {
                    int32_t dc_minus8 = bs.read_se();
                    // §7.4.5: scaling_list_dc_coef_minus8 in -7..247
                    REJECT_IF(dc_minus8 < -7 || dc_minus8 > 247,
                              "scaling_list_dc_coef_minus8=%d", dc_minus8);
                    int dc_coef = dc_minus8 + 8;
                    scaling_list_dc[sizeId - 2][matrixId] = static_cast<uint8_t>(dc_coef);
                    nextCoef = dc_coef;
                }

                // Coefficients arrive in up-right diagonal scan order (spec
                // §7.3.4 indexes ScalingList[][][i] by scan position i). The
                // dequant read (transform.cpp) and set_defaults() use RASTER
                // layout, so map each scan position to its (xC,yC) raster cell
                // via the inverse diagonal scan and store there. sbStride is 4
                // for the 4x4 list (coefNum 16) and 8 for the 8x8 list (64).
                const int sbStride = (coefNum == 16) ? 4 : 8;
                for (int i = 0; i < coefNum; i++) {
                    int delta_coef = bs.read_se();
                    // §7.4.5: scaling_list_delta_coef in -128..127
                    REJECT_IF(delta_coef < -128 || delta_coef > 127,
                              "scaling_list_delta_coef=%d", delta_coef);
                    nextCoef = (nextCoef + delta_coef + 256) % 256;
                    int xC, yC;
                    if (sbStride == 4) {
                        xC = diag_scan_4x4[i][0];
                        yC = diag_scan_4x4[i][1];
                    } else {
                        xC = diag_scan_8x8[i][0];
                        yC = diag_scan_8x8[i][1];
                    }
                    scaling_list[sizeId][matrixId][yC * sbStride + xC] =
                        static_cast<uint8_t>(nextCoef);
                }
            }
        }
    }

    return true;
}

// ---- Short-term Reference Picture Set ----

// Spec §7.3.7 — st_ref_pic_set(stRpsIdx)
bool ShortTermRefPicSet::parse(BitstreamReader& bs, int stRpsIdx,
                                int num_short_term_ref_pic_sets,
                                const std::vector<ShortTermRefPicSet>& rps_array) {
    inter_ref_pic_set_prediction_flag = false;
    if (stRpsIdx != 0) {
        inter_ref_pic_set_prediction_flag = bs.read_flag();
    }

    if (inter_ref_pic_set_prediction_flag) {
        // Inter-prediction from a reference RPS
        uint32_t delta_idx_minus1 = 0;
        if (stRpsIdx == num_short_term_ref_pic_sets) {
            delta_idx_minus1 = bs.read_ue();
        }
        // §7.4.8: delta_idx_minus1 in 0..stRpsIdx-1, so RefRpsIdx >= 0 and
        // indexes an already-parsed set
        REJECT_IF(stRpsIdx <= 0 || delta_idx_minus1 >= static_cast<uint32_t>(stRpsIdx) ||
                  static_cast<size_t>(stRpsIdx) > rps_array.size(),
                  "delta_idx_minus1=%u stRpsIdx=%d", delta_idx_minus1, stRpsIdx);
        bool delta_rps_sign = bs.read_flag();
        uint32_t abs_delta_rps_minus1 = bs.read_ue();
        // §7.4.8: abs_delta_rps_minus1 in 0..2^15-1
        REJECT_IF(abs_delta_rps_minus1 > 32767, "abs_delta_rps_minus1=%u", abs_delta_rps_minus1);

        int RefRpsIdx = stRpsIdx - 1 - static_cast<int>(delta_idx_minus1);
        int deltaRps = (1 - 2 * delta_rps_sign) * (static_cast<int>(abs_delta_rps_minus1) + 1);

        const auto& ref = rps_array[RefRpsIdx];
        int NumDeltaPocsRef = static_cast<int>(ref.NumDeltaPocs);
        // DPB-bounded (<= 16): keeps the flag/dPoc arrays below in range
        REJECT_IF(NumDeltaPocsRef > 16 || ref.NumNegativePics > 16 || ref.NumPositivePics > 16,
                  "reference RPS has %d delta POCs", NumDeltaPocsRef);

        // Parse used_by_curr_pic_flag and use_delta_flag for each entry
        std::array<bool, 33> used_by_curr_pic_flag = {};
        std::array<bool, 33> use_delta_flag = {};
        for (int j = 0; j <= NumDeltaPocsRef; j++) {
            used_by_curr_pic_flag[j] = bs.read_flag();
            if (!used_by_curr_pic_flag[j]) {
                use_delta_flag[j] = bs.read_flag();
            } else {
                use_delta_flag[j] = true;  // inferred
            }
        }

        // Derive the new RPS from reference + deltaRps
        // Spec §7.4.8 — derivation of st_ref_pic_set variables
        int i = 0;
        // Negative entries (DeltaPocS0)
        NumNegativePics = 0;
        NumPositivePics = 0;

        // Build combined delta POC list from reference
        // Process: for each entry in ref RPS + deltaRps, classify into S0 (negative) or S1 (positive)
        std::array<int32_t, 32> dPoc = {};
        std::array<bool, 32> usedFlag = {};
        int numEntries = 0;

        // Process S0 entries from reference
        for (int j = static_cast<int>(ref.NumPositivePics) - 1; j >= 0; j--) {
            int d = ref.DeltaPocS1[j] + deltaRps;
            if (d < 0 && use_delta_flag[ref.NumNegativePics + j]) {
                dPoc[numEntries] = d;
                usedFlag[numEntries] = used_by_curr_pic_flag[ref.NumNegativePics + j];
                numEntries++;
            }
        }
        if (deltaRps < 0 && use_delta_flag[NumDeltaPocsRef]) {
            dPoc[numEntries] = deltaRps;
            usedFlag[numEntries] = used_by_curr_pic_flag[NumDeltaPocsRef];
            numEntries++;
        }
        for (int j = 0; j < static_cast<int>(ref.NumNegativePics); j++) {
            int d = ref.DeltaPocS0[j] + deltaRps;
            if (d < 0 && use_delta_flag[j]) {
                dPoc[numEntries] = d;
                usedFlag[numEntries] = used_by_curr_pic_flag[j];
                numEntries++;
            }
        }
        REJECT_IF(numEntries > 16, "%d derived negative pictures", numEntries);
        NumNegativePics = numEntries;
        for (int k = 0; k < numEntries; k++) {
            DeltaPocS0[k] = dPoc[k];
            UsedByCurrPicS0[k] = usedFlag[k];
        }

        // S1 entries (positive)
        numEntries = 0;
        for (int j = static_cast<int>(ref.NumNegativePics) - 1; j >= 0; j--) {
            int d = ref.DeltaPocS0[j] + deltaRps;
            if (d > 0 && use_delta_flag[j]) {
                dPoc[numEntries] = d;
                usedFlag[numEntries] = used_by_curr_pic_flag[j];
                numEntries++;
            }
        }
        if (deltaRps > 0 && use_delta_flag[NumDeltaPocsRef]) {
            dPoc[numEntries] = deltaRps;
            usedFlag[numEntries] = used_by_curr_pic_flag[NumDeltaPocsRef];
            numEntries++;
        }
        for (int j = 0; j < static_cast<int>(ref.NumPositivePics); j++) {
            int d = ref.DeltaPocS1[j] + deltaRps;
            if (d > 0 && use_delta_flag[ref.NumNegativePics + j]) {
                dPoc[numEntries] = d;
                usedFlag[numEntries] = used_by_curr_pic_flag[ref.NumNegativePics + j];
                numEntries++;
            }
        }
        REJECT_IF(numEntries > 16, "%d derived positive pictures", numEntries);
        NumPositivePics = numEntries;
        for (int k = 0; k < numEntries; k++) {
            DeltaPocS1[k] = dPoc[k];
            UsedByCurrPicS1[k] = usedFlag[k];
        }

        REJECT_IF(NumNegativePics + NumPositivePics > 16, "%u derived delta POCs",
                  NumNegativePics + NumPositivePics);
        NumDeltaPocs = NumNegativePics + NumPositivePics;
        (void)i;
    } else {
        // Direct specification
        num_negative_pics = bs.read_ue();
        num_positive_pics = bs.read_ue();
        // §7.4.8: both bounded by the DPB size; the delta_poc arrays hold 16
        REJECT_IF(num_negative_pics > 16 || num_positive_pics > 16 ||
                  num_negative_pics + num_positive_pics > 16,
                  "num_negative_pics=%u num_positive_pics=%u", num_negative_pics, num_positive_pics);

        NumNegativePics = num_negative_pics;
        NumPositivePics = num_positive_pics;
        NumDeltaPocs = NumNegativePics + NumPositivePics;

        // S0 (negative delta POCs)
        for (uint32_t i = 0; i < num_negative_pics; i++) {
            uint32_t delta_poc_s0_minus1 = bs.read_ue();
            // §7.4.8: 0..2^15-1 — keeps the running DeltaPocS0 sum in int range
            REJECT_IF(delta_poc_s0_minus1 > 32767, "delta_poc_s0_minus1=%u", delta_poc_s0_minus1);
            used_by_curr_pic_s0[i] = bs.read_flag();
            UsedByCurrPicS0[i] = used_by_curr_pic_s0[i];

            if (i == 0) {
                DeltaPocS0[i] = -(static_cast<int32_t>(delta_poc_s0_minus1) + 1);
            } else {
                DeltaPocS0[i] = DeltaPocS0[i - 1] - (static_cast<int32_t>(delta_poc_s0_minus1) + 1);
            }
        }

        // S1 (positive delta POCs)
        for (uint32_t i = 0; i < num_positive_pics; i++) {
            uint32_t delta_poc_s1_minus1 = bs.read_ue();
            REJECT_IF(delta_poc_s1_minus1 > 32767, "delta_poc_s1_minus1=%u", delta_poc_s1_minus1);
            used_by_curr_pic_s1[i] = bs.read_flag();
            UsedByCurrPicS1[i] = used_by_curr_pic_s1[i];

            if (i == 0) {
                DeltaPocS1[i] = static_cast<int32_t>(delta_poc_s1_minus1) + 1;
            } else {
                DeltaPocS1[i] = DeltaPocS1[i - 1] + (static_cast<int32_t>(delta_poc_s1_minus1) + 1);
            }
        }
    }

    HEVC_LOG(PARSE, "  st_rps[%d]: neg=%d pos=%d total=%d inter=%d",
             stRpsIdx, NumNegativePics, NumPositivePics, NumDeltaPocs,
             inter_ref_pic_set_prediction_flag ? 1 : 0);

    return true;
}

// ---- SPS Parsing ----

// Skip VUI parameters — complex, not needed for basic decoding
// We just skip to the end of the RBSP
static void skip_vui_parameters(BitstreamReader& bs) {
    // aspect_ratio_info_present_flag
    bool aspect_ratio_info_present = bs.read_flag();
    if (aspect_ratio_info_present) {
        uint8_t aspect_ratio_idc = bs.read_bits(8);
        if (aspect_ratio_idc == 255) {  // EXTENDED_SAR
            bs.read_bits(16);  // sar_width
            bs.read_bits(16);  // sar_height
        }
    }
    // overscan_info_present_flag
    bool overscan_info_present = bs.read_flag();
    if (overscan_info_present) {
        bs.read_flag();  // overscan_appropriate_flag
    }
    // video_signal_type_present_flag
    bool video_signal_type_present = bs.read_flag();
    if (video_signal_type_present) {
        bs.read_bits(3);  // video_format
        bs.read_flag();   // video_full_range_flag
        bool colour_description_present = bs.read_flag();
        if (colour_description_present) {
            bs.read_bits(8);   // colour_primaries
            bs.read_bits(8);   // transfer_characteristics
            bs.read_bits(8);   // matrix_coeffs
        }
    }
    // chroma_loc_info_present_flag
    bool chroma_loc_info_present = bs.read_flag();
    if (chroma_loc_info_present) {
        bs.read_ue();  // chroma_sample_loc_type_top_field
        bs.read_ue();  // chroma_sample_loc_type_bottom_field
    }
    // neutral_chroma_indication_flag, field_seq_flag, frame_field_info_present_flag
    bs.read_flag();
    bs.read_flag();
    bs.read_flag();
    // default_display_window_flag
    bool default_display_window = bs.read_flag();
    if (default_display_window) {
        bs.read_ue();  // def_disp_win_left_offset
        bs.read_ue();  // def_disp_win_right_offset
        bs.read_ue();  // def_disp_win_top_offset
        bs.read_ue();  // def_disp_win_bottom_offset
    }
    // vui_timing_info_present_flag
    bool timing_info_present = bs.read_flag();
    if (timing_info_present) {
        bs.read_bits(32);  // vui_num_units_in_tick
        bs.read_bits(32);  // vui_time_scale
        bool vui_poc_proportional = bs.read_flag();
        if (vui_poc_proportional) {
            bs.read_ue();  // vui_num_ticks_poc_diff_one_minus1
        }
        bool vui_hrd_parameters_present = bs.read_flag();
        if (vui_hrd_parameters_present) {
            // HRD parameters are very complex — skip the rest
            // This is safe because HRD is not needed for decoding
            HEVC_LOG(PARSE, "SPS VUI: HRD parameters present, skipping rest of VUI%s", "");
            return;
        }
    }
    // bitstream_restriction_flag
    bool bitstream_restriction = bs.read_flag();
    if (bitstream_restriction) {
        bs.read_flag();  // tiles_fixed_structure_flag
        bs.read_flag();  // motion_vectors_over_pic_boundaries_flag
        bs.read_flag();  // restricted_ref_pic_lists_flag
        bs.read_ue();    // min_spatial_segmentation_idc
        bs.read_ue();    // max_bytes_per_pic_denom
        bs.read_ue();    // max_bits_per_min_cu_denom
        bs.read_ue();    // log2_max_mv_length_horizontal
        bs.read_ue();    // log2_max_mv_length_vertical
    }
}

// Spec §7.3.2.2.1 — seq_parameter_set_rbsp()
bool SPS::parse(BitstreamReader& bs) {
    sps_video_parameter_set_id = bs.read_bits(4);
    sps_max_sub_layers_minus1 = bs.read_bits(3);
    // §7.4.3.2.1: 0..6 — indexes sub_layer_ordering[7] and the PTL sub-layer arrays
    REJECT_IF(sps_max_sub_layers_minus1 > 6, "sps_max_sub_layers_minus1=%u",
              sps_max_sub_layers_minus1);
    sps_temporal_id_nesting_flag = bs.read_flag();

    // profile_tier_level(1, sps_max_sub_layers_minus1)
    if (!profile_tier_level.parse(bs, true, sps_max_sub_layers_minus1)) return false;

    sps_seq_parameter_set_id = bs.read_ue();
    chroma_format_idc = bs.read_ue();
    // §7.4.3.2.1: sps id 0..15 (indexes the 16-entry store), chroma_format_idc 0..3
    REJECT_IF(sps_seq_parameter_set_id > 15 || chroma_format_idc > 3,
              "id=%u chroma_format_idc=%u", sps_seq_parameter_set_id, chroma_format_idc);

    if (chroma_format_idc == 3) {
        separate_colour_plane_flag = bs.read_flag();
    }

    pic_width_in_luma_samples = bs.read_ue();
    pic_height_in_luma_samples = bs.read_ue();

    conformance_window_flag = bs.read_flag();
    if (conformance_window_flag) {
        conf_win_left_offset = bs.read_ue();
        conf_win_right_offset = bs.read_ue();
        conf_win_top_offset = bs.read_ue();
        conf_win_bottom_offset = bs.read_ue();
    }

    bit_depth_luma_minus8 = bs.read_ue();
    bit_depth_chroma_minus8 = bs.read_ue();
    log2_max_pic_order_cnt_lsb_minus4 = bs.read_ue();
    // §7.4.3.2.1 allows bit depths up to 16, but past 12 bits the
    // interpolation/weighting shifts go negative without
    // extended_precision_processing, which this decoder does not implement.
    REJECT_IF(bit_depth_luma_minus8 > 4 || bit_depth_chroma_minus8 > 4,
              "bit depth %u/%u unsupported", bit_depth_luma_minus8 + 8, bit_depth_chroma_minus8 + 8);
    // §7.4.3.2.1: log2_max_pic_order_cnt_lsb_minus4 in 0..12
    REJECT_IF(log2_max_pic_order_cnt_lsb_minus4 > 12, "log2_max_poc_lsb_minus4=%u",
              log2_max_pic_order_cnt_lsb_minus4);

    // Sub-layer ordering
    sps_sub_layer_ordering_info_present_flag = bs.read_flag();
    int start = sps_sub_layer_ordering_info_present_flag ? 0 : sps_max_sub_layers_minus1;
    for (int i = start; i <= sps_max_sub_layers_minus1; i++) {
        auto& o = sub_layer_ordering[i];
        o.max_dec_pic_buffering_minus1 = bs.read_ue();
        o.max_num_reorder_pics = bs.read_ue();
        o.max_latency_increase_plus1 = bs.read_ue();
        // §7.4.3.2.1 bounds these by MaxDpbSize (16). They only steer output
        // timing, so a camera that overstates them is clamped, not rejected.
        if (o.max_dec_pic_buffering_minus1 > 15) o.max_dec_pic_buffering_minus1 = 15;
        if (o.max_num_reorder_pics > o.max_dec_pic_buffering_minus1)
            o.max_num_reorder_pics = o.max_dec_pic_buffering_minus1;
    }
    // §7.4.3.2.1: when not present, lower sub-layers take the highest one's values
    for (int i = 0; i < start; i++) sub_layer_ordering[i] = sub_layer_ordering[start];

    // Quad-tree config
    log2_min_luma_coding_block_size_minus3 = bs.read_ue();
    log2_diff_max_min_luma_coding_block_size = bs.read_ue();
    log2_min_luma_transform_block_size_minus2 = bs.read_ue();
    log2_diff_max_min_luma_transform_block_size = bs.read_ue();
    max_transform_hierarchy_depth_inter = bs.read_ue();
    max_transform_hierarchy_depth_intra = bs.read_ue();

    // Coarse bounds first, so the derived sums below cannot overflow
    REJECT_IF(log2_min_luma_coding_block_size_minus3 > 3 ||
              log2_diff_max_min_luma_coding_block_size > 3 ||
              log2_min_luma_transform_block_size_minus2 > 3 ||
              log2_diff_max_min_luma_transform_block_size > 3,
              "log2 block size field out of range%s", "");
    {
        // §7.4.3.2.1 + §A.4.1: CTB 16..64, TB smaller than the min CB, TB <= 32
        const int minCbLog2 = static_cast<int>(log2_min_luma_coding_block_size_minus3) + 3;
        const int ctbLog2 = minCbLog2 + static_cast<int>(log2_diff_max_min_luma_coding_block_size);
        const int minTbLog2 = static_cast<int>(log2_min_luma_transform_block_size_minus2) + 2;
        const int maxTbLog2 = minTbLog2 + static_cast<int>(log2_diff_max_min_luma_transform_block_size);
        REJECT_IF(ctbLog2 < 4 || ctbLog2 > 6 || minTbLog2 >= minCbLog2 ||
                  maxTbLog2 > std::min(ctbLog2, 5) ||
                  max_transform_hierarchy_depth_inter > static_cast<uint32_t>(ctbLog2 - minTbLog2) ||
                  max_transform_hierarchy_depth_intra > static_cast<uint32_t>(ctbLog2 - minTbLog2),
                  "block sizes ctb=%d mincb=%d tb=[%d,%d]", ctbLog2, minCbLog2, minTbLog2, maxTbLog2);
    }

    // Scaling lists
    scaling_list_enabled_flag = bs.read_flag();
    if (scaling_list_enabled_flag) {
        sps_scaling_list_data_present_flag = bs.read_flag();
        if (sps_scaling_list_data_present_flag) {
            if (!scaling_list_data.parse(bs)) return false;
        } else {
            // Use default (non-flat) scaling lists
            scaling_list_data.set_defaults();
        }
    }
    // If scaling_list_enabled_flag == 0, flat 16 everywhere (handled in dequant)

    amp_enabled_flag = bs.read_flag();
    sample_adaptive_offset_enabled_flag = bs.read_flag();

    pcm_enabled_flag = bs.read_flag();
    if (pcm_enabled_flag) {
        pcm_sample_bit_depth_luma_minus1 = bs.read_bits(4);
        pcm_sample_bit_depth_chroma_minus1 = bs.read_bits(4);
        log2_min_pcm_luma_coding_block_size_minus3 = bs.read_ue();
        log2_diff_max_min_pcm_luma_coding_block_size = bs.read_ue();
        pcm_loop_filter_disabled_flag = bs.read_flag();

        // §7.4.3.2.1: PCM sizes within [8, min(CtbSizeY, 32)], PCM depth <= bit depth
        REJECT_IF(log2_min_pcm_luma_coding_block_size_minus3 > 2 ||
                  log2_diff_max_min_pcm_luma_coding_block_size > 2,
                  "PCM log2 size out of range%s", "");
        const int ctbLog2 = static_cast<int>(log2_min_luma_coding_block_size_minus3) + 3 +
                            static_cast<int>(log2_diff_max_min_luma_coding_block_size);
        const int maxIpcmLog2 = static_cast<int>(log2_min_pcm_luma_coding_block_size_minus3) + 3 +
                                static_cast<int>(log2_diff_max_min_pcm_luma_coding_block_size);
        REJECT_IF(maxIpcmLog2 > std::min(ctbLog2, 5) ||
                  pcm_sample_bit_depth_luma_minus1 + 1u > 8u + bit_depth_luma_minus8 ||
                  pcm_sample_bit_depth_chroma_minus1 + 1u > 8u + bit_depth_chroma_minus8,
                  "PCM config out of range%s", "");
    }

    // Short-term reference picture sets
    num_short_term_ref_pic_sets = bs.read_ue();
    // §7.4.3.2.1: 0..64 — bounds the resize below
    REJECT_IF(num_short_term_ref_pic_sets > 64, "num_short_term_ref_pic_sets=%u",
              num_short_term_ref_pic_sets);
    st_ref_pic_sets.resize(num_short_term_ref_pic_sets);
    for (uint32_t i = 0; i < num_short_term_ref_pic_sets; i++) {
        if (!st_ref_pic_sets[i].parse(bs, static_cast<int>(i),
                                      static_cast<int>(num_short_term_ref_pic_sets),
                                      st_ref_pic_sets))
            return false;
    }

    // Long-term reference pictures
    long_term_ref_pics_present_flag = bs.read_flag();
    if (long_term_ref_pics_present_flag) {
        num_long_term_ref_pics_sps = bs.read_ue();
        // §7.4.3.2.1: 0..32 — bounds the lt_ref_pic_poc_lsb_sps[32] writes
        REJECT_IF(num_long_term_ref_pics_sps > 32, "num_long_term_ref_pics_sps=%u",
                  num_long_term_ref_pics_sps);
        for (uint32_t i = 0; i < num_long_term_ref_pics_sps; i++) {
            // lt_ref_pic_poc_lsb_sps[i] — u(v) where v = log2_max_pic_order_cnt_lsb_minus4 + 4
            int poc_lsb_bits = static_cast<int>(log2_max_pic_order_cnt_lsb_minus4) + 4;
            lt_ref_pic_poc_lsb_sps[i] = bs.read_bits(poc_lsb_bits);
            used_by_curr_pic_lt_sps_flag[i] = bs.read_flag();
        }
    }

    sps_temporal_mvp_enabled_flag = bs.read_flag();
    strong_intra_smoothing_enabled_flag = bs.read_flag();

    vui_parameters_present_flag = bs.read_flag();
    if (vui_parameters_present_flag) {
        skip_vui_parameters(bs);
    }

    bool sps_extension_present_flag = bs.read_flag();
    if (sps_extension_present_flag) {
        bool sps_range_extension_flag = bs.read_flag();
        /*bool sps_multilayer_extension_flag =*/ bs.read_flag();
        /*bool sps_3d_extension_flag =*/ bs.read_flag();
        /*bool sps_extension_5bits =*/ bs.read_bits(5);
        if (sps_range_extension_flag) {
            /*bool transform_skip_rotation_enabled_flag =*/ bs.read_flag();
            /*bool transform_skip_context_enabled_flag =*/ bs.read_flag();
            /*bool implicit_rdpcm_enabled_flag =*/ bs.read_flag();
            /*bool explicit_rdpcm_enabled_flag =*/ bs.read_flag();
            /*bool extended_precision_processing_flag =*/ bs.read_flag();
            intra_smoothing_disabled_flag = bs.read_flag();
            /*bool high_precision_offsets_enabled_flag =*/ bs.read_flag();
            /*bool persistent_rice_adaptation_enabled_flag =*/ bs.read_flag();
            cabac_bypass_alignment_enabled_flag = bs.read_flag();
        }
    }

    // Compute derived values
    derive();

    // Reject malformed/hostile dimensions before any allocation downstream.
    if (!validate()) {
        return false;
    }

    HEVC_LOG(PARSE, "SPS: id=%d vps=%d %dx%d chroma=%d bit_depth=%d/%d",
             sps_seq_parameter_set_id, sps_video_parameter_set_id,
             pic_width_in_luma_samples, pic_height_in_luma_samples,
             chroma_format_idc, BitDepthY, BitDepthC);
    HEVC_LOG(PARSE, "  CTB=%dx%d (%dx%d CTBs) MinCb=%d MinTb=%d MaxTb=%d",
             CtbSizeY, CtbSizeY, PicWidthInCtbsY, PicHeightInCtbsY,
             MinCbSizeY, MinTbSizeY, MaxTbSizeY);

    return true;
}

// Spec §7.4.3.2.1 — Derived values
void SPS::derive() {
    // Chroma
    ChromaArrayType = separate_colour_plane_flag ? 0 : static_cast<int>(chroma_format_idc);

    // Chroma subsampling
    if (chroma_format_idc == 1) {       // 4:2:0
        SubWidthC = 2;
        SubHeightC = 2;
    } else if (chroma_format_idc == 2) { // 4:2:2
        SubWidthC = 2;
        SubHeightC = 1;
    } else if (chroma_format_idc == 3 && !separate_colour_plane_flag) { // 4:4:4
        SubWidthC = 1;
        SubHeightC = 1;
    } else { // monochrome or separate planes
        SubWidthC = 1;
        SubHeightC = 1;
    }

    // Bit depth
    BitDepthY = 8 + static_cast<int>(bit_depth_luma_minus8);
    BitDepthC = 8 + static_cast<int>(bit_depth_chroma_minus8);
    QpBdOffsetY = 6 * static_cast<int>(bit_depth_luma_minus8);
    QpBdOffsetC = 6 * static_cast<int>(bit_depth_chroma_minus8);

    // POC
    MaxPicOrderCntLsb = 1 << (static_cast<int>(log2_max_pic_order_cnt_lsb_minus4) + 4);

    // Coding block sizes
    MinCbLog2SizeY = static_cast<int>(log2_min_luma_coding_block_size_minus3) + 3;
    CtbLog2SizeY = MinCbLog2SizeY + static_cast<int>(log2_diff_max_min_luma_coding_block_size);
    MinCbSizeY = 1 << MinCbLog2SizeY;
    CtbSizeY = 1 << CtbLog2SizeY;

    // Picture dimensions in min CB and CTB units
    PicWidthInMinCbsY = static_cast<int>(pic_width_in_luma_samples) / MinCbSizeY;
    PicHeightInMinCbsY = static_cast<int>(pic_height_in_luma_samples) / MinCbSizeY;
    PicWidthInCtbsY = (static_cast<int>(pic_width_in_luma_samples) + CtbSizeY - 1) / CtbSizeY;
    PicHeightInCtbsY = (static_cast<int>(pic_height_in_luma_samples) + CtbSizeY - 1) / CtbSizeY;
    PicSizeInCtbsY = PicWidthInCtbsY * PicHeightInCtbsY;

    // Transform block sizes
    MinTbLog2SizeY = static_cast<int>(log2_min_luma_transform_block_size_minus2) + 2;
    MaxTbLog2SizeY = MinTbLog2SizeY + static_cast<int>(log2_diff_max_min_luma_transform_block_size);
    MinTbSizeY = 1 << MinTbLog2SizeY;
    MaxTbSizeY = 1 << MaxTbLog2SizeY;

    // PCM block sizes (§7.4.3.2.1)
    if (pcm_enabled_flag) {
        Log2MinIpcmCbSizeY = static_cast<int>(log2_min_pcm_luma_coding_block_size_minus3) + 3;
        Log2MaxIpcmCbSizeY = Log2MinIpcmCbSizeY +
                              static_cast<int>(log2_diff_max_min_pcm_luma_coding_block_size);
    }
}

// Validate dimensions from untrusted media. pic_width/height and the
// conf_win_* offsets are read as ue(v) with no inherent bound and are used as
// strides/extents into Picture::allocate and SIMD loops, so an out-of-range or
// non-conformant value can drive an over-allocation or out-of-bounds access.
bool SPS::validate() const {
    // A valid MinCbSizeY is required to bound the picture (must be a power of
    // two in [8,64]); derive() should have set it, but guard anyway.
    if (MinCbSizeY <= 0) {
        return false;
    }

    // Sane upper bound on picture dimensions (well above any real profile/level
    // and the 16-bit allocator stride this decoder uses).
    constexpr uint32_t kMaxDim = kMaxLumaDimension;

    // Width/height must be non-zero, within the cap, and (spec §A.3.2 /
    // conformance) an integer multiple of MinCbSizeY.
    if (pic_width_in_luma_samples == 0 || pic_height_in_luma_samples == 0) {
        return false;
    }
    if (pic_width_in_luma_samples > kMaxDim || pic_height_in_luma_samples > kMaxDim) {
        return false;
    }
    if (pic_width_in_luma_samples % static_cast<uint32_t>(MinCbSizeY) != 0 ||
        pic_height_in_luma_samples % static_cast<uint32_t>(MinCbSizeY) != 0) {
        return false;
    }
    // §A.4.1 level 6.2 MaxLumaPs: bounds the per-picture allocation (16384x16384
    // would be ~400 MB per picture, times the DPB).
    if (static_cast<uint64_t>(pic_width_in_luma_samples) * pic_height_in_luma_samples > kMaxLumaPs) {
        return false;
    }

    // Conformance window must lie within the picture. Offsets are in chroma
    // sample units; the cropped region (SubWidthC*(left+right) etc.) must leave
    // at least one luma sample of picture, matching the spec §7.4.3.2.1
    // constraint on conf_win_*_offset.
    if (conformance_window_flag) {
        const uint64_t crop_w =
            static_cast<uint64_t>(SubWidthC) *
            (static_cast<uint64_t>(conf_win_left_offset) + conf_win_right_offset);
        const uint64_t crop_h =
            static_cast<uint64_t>(SubHeightC) *
            (static_cast<uint64_t>(conf_win_top_offset) + conf_win_bottom_offset);
        if (crop_w >= pic_width_in_luma_samples ||
            crop_h >= pic_height_in_luma_samples) {
            return false;
        }
    }

    return true;
}

} // namespace hevc
