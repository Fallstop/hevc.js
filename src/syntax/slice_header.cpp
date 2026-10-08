#include "syntax/slice_header.h"
#include "syntax/pps.h"
#include "bitstream/bitstream_reader.h"
#include "common/types.h"
#include "common/debug.h"

namespace hevc {

#define REJECT_IF(cond, ...)                                       \
    do {                                                            \
        if (cond) {                                                 \
            HEVC_LOG(PARSE, "Slice rejected: " __VA_ARGS__);        \
            return false;                                           \
        }                                                           \
    } while (0)

// Spec §7.3.6.3 — pred_weight_table()
static bool parse_pred_weight_table(BitstreamReader& bs, PredWeightTable& pwt,
                                     const SPS& sps, SliceType slice_type,
                                     uint32_t num_ref_idx_l0, uint32_t num_ref_idx_l1) {
    pwt.luma_log2_weight_denom = bs.read_ue();
    // §7.4.7.3: 0..7
    REJECT_IF(pwt.luma_log2_weight_denom > 7, "luma_log2_weight_denom=%u",
              pwt.luma_log2_weight_denom);

    if (sps.ChromaArrayType != 0) {
        pwt.delta_chroma_log2_weight_denom = bs.read_se();
        // §7.4.7.3: ChromaLog2WeightDenom in 0..7
        const int32_t c = static_cast<int32_t>(pwt.luma_log2_weight_denom) +
                          pwt.delta_chroma_log2_weight_denom;
        REJECT_IF(c < 0 || c > 7, "ChromaLog2WeightDenom=%d", c);
    }

    // L0 luma weight flags
    for (uint32_t i = 0; i <= num_ref_idx_l0; i++) {
        pwt.l0[i].luma_weight_flag = bs.read_flag();
    }

    // L0 chroma weight flags
    if (sps.ChromaArrayType != 0) {
        for (uint32_t i = 0; i <= num_ref_idx_l0; i++) {
            pwt.l0[i].chroma_weight_flag = bs.read_flag();
        }
    }

    // Derived chroma log2 weight denom (spec §7.4.7.3)
    int chromaLog2WeightDenom = static_cast<int>(pwt.luma_log2_weight_denom) +
                                pwt.delta_chroma_log2_weight_denom;

    // L0 weight/offset values — derive final values per spec §7.4.7.3
    for (uint32_t i = 0; i <= num_ref_idx_l0; i++) {
        if (pwt.l0[i].luma_weight_flag) {
            int32_t delta_raw = bs.read_se();
            int32_t offset_raw = 0;
            // §7.4.7.3: delta_luma_weight -128..127, luma_offset -128..127
            REJECT_IF(delta_raw < -128 || delta_raw > 127, "delta_luma_weight=%d", delta_raw);
            int16_t delta = static_cast<int16_t>(delta_raw);
            pwt.l0[i].luma_weight = static_cast<int16_t>((1 << pwt.luma_log2_weight_denom) + delta);
            offset_raw = bs.read_se();
            REJECT_IF(offset_raw < -128 || offset_raw > 127, "luma_offset=%d", offset_raw);
            pwt.l0[i].luma_offset = static_cast<int16_t>(offset_raw);
        } else {
            pwt.l0[i].luma_weight = static_cast<int16_t>(1 << pwt.luma_log2_weight_denom);
            pwt.l0[i].luma_offset = 0;
        }
        if (pwt.l0[i].chroma_weight_flag) {
            for (int j = 0; j < 2; j++) {
                int32_t dw = bs.read_se();
                int32_t dof = bs.read_se();
                // §7.4.7.3: delta_chroma_weight -128..127, delta_chroma_offset -512..508
                REJECT_IF(dw < -128 || dw > 127 || dof < -512 || dof > 508,
                          "chroma weight/offset %d/%d", dw, dof);
                int16_t delta_weight = static_cast<int16_t>(dw);
                int16_t delta_offset = static_cast<int16_t>(dof);
                pwt.l0[i].chroma_weight[j] = static_cast<int16_t>((1 << chromaLog2WeightDenom) + delta_weight);
                pwt.l0[i].chroma_offset[j] = static_cast<int16_t>(
                    Clip3(-128, 127,
                          static_cast<int>(delta_offset) -
                          ((128 * pwt.l0[i].chroma_weight[j]) >> chromaLog2WeightDenom) + 128));
            }
        } else {
            for (int j = 0; j < 2; j++) {
                pwt.l0[i].chroma_weight[j] = static_cast<int16_t>(1 << chromaLog2WeightDenom);
                pwt.l0[i].chroma_offset[j] = 0;
            }
        }
    }

    // L1 (B-slices only)
    if (slice_type == SliceType::B) {
        // L1 luma weight flags
        for (uint32_t i = 0; i <= num_ref_idx_l1; i++) {
            pwt.l1[i].luma_weight_flag = bs.read_flag();
        }
        if (sps.ChromaArrayType != 0) {
            for (uint32_t i = 0; i <= num_ref_idx_l1; i++) {
                pwt.l1[i].chroma_weight_flag = bs.read_flag();
            }
        }
        for (uint32_t i = 0; i <= num_ref_idx_l1; i++) {
            if (pwt.l1[i].luma_weight_flag) {
                int32_t delta_raw = bs.read_se();
                int32_t offset_raw = 0;
                REJECT_IF(delta_raw < -128 || delta_raw > 127, "delta_luma_weight=%d", delta_raw);
                int16_t delta = static_cast<int16_t>(delta_raw);
                pwt.l1[i].luma_weight = static_cast<int16_t>((1 << pwt.luma_log2_weight_denom) + delta);
                offset_raw = bs.read_se();
                REJECT_IF(offset_raw < -128 || offset_raw > 127, "luma_offset=%d", offset_raw);
                pwt.l1[i].luma_offset = static_cast<int16_t>(offset_raw);
            } else {
                pwt.l1[i].luma_weight = static_cast<int16_t>(1 << pwt.luma_log2_weight_denom);
                pwt.l1[i].luma_offset = 0;
            }
            if (pwt.l1[i].chroma_weight_flag) {
                for (int j = 0; j < 2; j++) {
                    int32_t dw = bs.read_se();
                    int32_t dof = bs.read_se();
                    REJECT_IF(dw < -128 || dw > 127 || dof < -512 || dof > 508,
                              "chroma weight/offset %d/%d", dw, dof);
                    int16_t delta_weight = static_cast<int16_t>(dw);
                    int16_t delta_offset = static_cast<int16_t>(dof);
                    pwt.l1[i].chroma_weight[j] = static_cast<int16_t>((1 << chromaLog2WeightDenom) + delta_weight);
                    pwt.l1[i].chroma_offset[j] = static_cast<int16_t>(
                        Clip3(-128, 127,
                              static_cast<int>(delta_offset) -
                              ((128 * pwt.l1[i].chroma_weight[j]) >> chromaLog2WeightDenom) + 128));
                }
            } else {
                for (int j = 0; j < 2; j++) {
                    pwt.l1[i].chroma_weight[j] = static_cast<int16_t>(1 << chromaLog2WeightDenom);
                    pwt.l1[i].chroma_offset[j] = 0;
                }
            }
        }
    }

    return true;
}

// Calculate ceil(log2(x)) for the number of bits needed to represent values 0..x-1
static int ceil_log2(int x) {
    if (x <= 1) return 0;
    int bits = 0;
    x--;
    while (x > 0) {
        bits++;
        x >>= 1;
    }
    return bits;
}

// Spec §7.3.6.1 — slice_segment_header()
bool SliceHeader::parse(BitstreamReader& bs, const SPS& sps, const PPS& pps,
                         NalUnitType nal_type, uint8_t nuh_temporal_id) {
    first_slice_segment_in_pic_flag = bs.read_flag();

    // no_output_of_prior_pics_flag — present only in IRAP pictures
    if (is_irap(nal_type)) {
        no_output_of_prior_pics_flag = bs.read_flag();
    }

    slice_pic_parameter_set_id = bs.read_ue();

    // Dependent slice segment
    dependent_slice_segment_flag = false;
    if (!first_slice_segment_in_pic_flag) {
        if (pps.dependent_slice_segments_enabled_flag) {
            dependent_slice_segment_flag = bs.read_flag();
        }
        // slice_segment_address — u(v) where v = Ceil(Log2(PicSizeInCtbsY))
        int addr_bits = ceil_log2(sps.PicSizeInCtbsY);
        slice_segment_address = bs.read_bits(addr_bits);
        // §7.4.7.1: < PicSizeInCtbsY (u(v) can code more for non-power-of-2
        // CTB counts); it indexes CtbAddrRsToTs downstream
        REJECT_IF(slice_segment_address >= static_cast<uint32_t>(sps.PicSizeInCtbsY),
                  "slice_segment_address=%u PicSizeInCtbsY=%d", slice_segment_address,
                  sps.PicSizeInCtbsY);
    }

    // The rest is only present for independent slice segments
    if (dependent_slice_segment_flag) {
        // Dependent slices inherit most fields from the independent slice
        return true;
    }

    // Extra slice header bits (reserved)
    for (int i = 0; i < pps.num_extra_slice_header_bits; i++) {
        bs.read_flag();  // slice_reserved_flag[i]
    }

    const uint32_t slice_type_raw = bs.read_ue();
    // §7.4.7.1: 0..2, and an IRAP picture contains I slices only
    REJECT_IF(slice_type_raw > 2, "slice_type=%u", slice_type_raw);
    slice_type = static_cast<SliceType>(slice_type_raw);
    REJECT_IF(is_irap(nal_type) && slice_type != SliceType::I,
              "slice_type=%u in an IRAP picture", slice_type_raw);

    if (pps.output_flag_present_flag) {
        pic_output_flag = bs.read_flag();
    }

    if (sps.separate_colour_plane_flag) {
        colour_plane_id = bs.read_bits(2);
        REJECT_IF(colour_plane_id > 2, "colour_plane_id=%u", colour_plane_id);
    }

    // POC and reference picture sets — not present for IDR
    if (!is_idr(nal_type)) {
        // slice_pic_order_cnt_lsb — u(v), v = log2_max_pic_order_cnt_lsb_minus4 + 4
        int poc_bits = static_cast<int>(sps.log2_max_pic_order_cnt_lsb_minus4) + 4;
        slice_pic_order_cnt_lsb = bs.read_bits(poc_bits);

        // Short-term reference picture set
        short_term_ref_pic_set_sps_flag = bs.read_flag();
        if (!short_term_ref_pic_set_sps_flag) {
            // Parse inline short-term RPS
            REJECT_IF(!st_rps.parse(bs, static_cast<int>(sps.num_short_term_ref_pic_sets),
                                    static_cast<int>(sps.num_short_term_ref_pic_sets),
                                    sps.st_ref_pic_sets),
                      "inline st_ref_pic_set%s", "");
            active_rps = &st_rps;
        } else {
            short_term_ref_pic_set_idx = 0;
            if (sps.num_short_term_ref_pic_sets > 1) {
                int idx_bits = ceil_log2(static_cast<int>(sps.num_short_term_ref_pic_sets));
                short_term_ref_pic_set_idx = bs.read_bits(idx_bits);
            }
            // §7.4.7.1: idx < num_short_term_ref_pic_sets (and an SPS with no
            // sets cannot be referenced at all)
            REJECT_IF(short_term_ref_pic_set_idx >= sps.num_short_term_ref_pic_sets ||
                      short_term_ref_pic_set_idx >= sps.st_ref_pic_sets.size(),
                      "st_rps_idx=%u num_sets=%u", short_term_ref_pic_set_idx,
                      sps.num_short_term_ref_pic_sets);
            active_rps = &sps.st_ref_pic_sets[short_term_ref_pic_set_idx];
        }

        // Long-term reference pictures
        if (sps.long_term_ref_pics_present_flag) {
            num_long_term_sps = 0;
            if (sps.num_long_term_ref_pics_sps > 0) {
                num_long_term_sps = bs.read_ue();
            }
            num_long_term_pics = bs.read_ue();
            // §7.4.7.1: the SPS subset is bounded by the SPS count; the arrays
            // hold 32 entries in total
            REJECT_IF(num_long_term_sps > sps.num_long_term_ref_pics_sps ||
                      num_long_term_pics > 32 || num_long_term_sps + num_long_term_pics > 32,
                      "num_long_term sps=%u pics=%u", num_long_term_sps, num_long_term_pics);

            int poc_lsb_bits = static_cast<int>(sps.log2_max_pic_order_cnt_lsb_minus4) + 4;
            int lt_sps_bits = (sps.num_long_term_ref_pics_sps > 1)
                              ? ceil_log2(static_cast<int>(sps.num_long_term_ref_pics_sps))
                              : 0;

            for (uint32_t i = 0; i < num_long_term_sps + num_long_term_pics; i++) {
                if (i < num_long_term_sps) {
                    lt_idx_sps[i] = 0;  // §7.4.7.1: inferred 0 when not coded
                    if (sps.num_long_term_ref_pics_sps > 1) {
                        lt_idx_sps[i] = bs.read_bits(lt_sps_bits);
                        // §7.4.7.1: indexes the SPS long-term tables
                        REJECT_IF(lt_idx_sps[i] >= sps.num_long_term_ref_pics_sps,
                                  "lt_idx_sps=%u", lt_idx_sps[i]);
                    }
                    // §7.4.7.1: resolve SPS-indexed LTRP entries. When
                    // num_long_term_ref_pics_sps == 1, lt_idx_sps[i] is inferred 0
                    // (not coded). derive_rps reads poc_lsb_lt/used_by_curr_pic_lt_flag
                    // for every i, so these must be populated from the SPS here.
                    poc_lsb_lt[i] = sps.lt_ref_pic_poc_lsb_sps[lt_idx_sps[i]];
                    used_by_curr_pic_lt_flag[i] =
                        sps.used_by_curr_pic_lt_sps_flag[lt_idx_sps[i]];
                } else {
                    poc_lsb_lt[i] = bs.read_bits(poc_lsb_bits);
                    used_by_curr_pic_lt_flag[i] = bs.read_flag();
                }
                delta_poc_msb_present_flag[i] = bs.read_flag();
                if (delta_poc_msb_present_flag[i]) {
                    delta_poc_msb_cycle_lt[i] = bs.read_ue();
                } else {
                    delta_poc_msb_cycle_lt[i] = 0;  // §7.4.7.1: inferred 0
                }
                // §7.4.7.1 eq 7-52
                const uint64_t cycle = (i == 0 || i == num_long_term_sps)
                    ? delta_poc_msb_cycle_lt[i]
                    : static_cast<uint64_t>(delta_poc_msb_cycle_lt[i]) + DeltaPocMsbCycleLt[i - 1];
                // The POC it derives (DeltaPocMsbCycleLt * MaxPicOrderCntLsb) has
                // to fit the 32-bit POC range
                REJECT_IF(cycle > (1ull << (31 - poc_lsb_bits)), "DeltaPocMsbCycleLt=%llu",
                          static_cast<unsigned long long>(cycle));
                DeltaPocMsbCycleLt[i] = static_cast<uint32_t>(cycle);
            }
        }

        // Temporal MVP
        if (sps.sps_temporal_mvp_enabled_flag) {
            slice_temporal_mvp_enabled_flag = bs.read_flag();
        }
    } else {
        // IDR: POC = 0, no RPS
        slice_pic_order_cnt_lsb = 0;
    }

    // SAO
    if (sps.sample_adaptive_offset_enabled_flag) {
        slice_sao_luma_flag = bs.read_flag();
        if (sps.ChromaArrayType != 0) {
            slice_sao_chroma_flag = bs.read_flag();
        }
    }

    // P/B slice specific fields
    if (slice_type == SliceType::P || slice_type == SliceType::B) {
        num_ref_idx_active_override_flag = bs.read_flag();
        if (num_ref_idx_active_override_flag) {
            num_ref_idx_l0_active_minus1 = bs.read_ue();
            if (slice_type == SliceType::B) {
                num_ref_idx_l1_active_minus1 = bs.read_ue();
            }
        } else {
            num_ref_idx_l0_active_minus1 = pps.num_ref_idx_l0_default_active_minus1;
            num_ref_idx_l1_active_minus1 = pps.num_ref_idx_l1_default_active_minus1;
        }

        // §7.4.7.1: 0..14 — list_entry_lX and the weight tables hold 16 entries
        REJECT_IF(num_ref_idx_l0_active_minus1 > 14 || num_ref_idx_l1_active_minus1 > 14,
                  "num_ref_idx l0=%u l1=%u", num_ref_idx_l0_active_minus1,
                  num_ref_idx_l1_active_minus1);

        // NumPicTotalCurr — derived from active RPS
        // For ref_pic_lists_modification: need NumPicTotalCurr > 1
        int NumPicTotalCurr = 0;
        if (active_rps) {
            for (uint32_t i = 0; i < active_rps->NumNegativePics; i++) {
                if (active_rps->UsedByCurrPicS0[i]) NumPicTotalCurr++;
            }
            for (uint32_t i = 0; i < active_rps->NumPositivePics; i++) {
                if (active_rps->UsedByCurrPicS1[i]) NumPicTotalCurr++;
            }
        }
        for (uint32_t i = 0; i < num_long_term_sps + num_long_term_pics; i++) {
            bool used = false;
            if (i < num_long_term_sps) {
                used = sps.used_by_curr_pic_lt_sps_flag[lt_idx_sps[i]];
            } else {
                used = used_by_curr_pic_lt_flag[i];
            }
            if (used) NumPicTotalCurr++;
        }

        // ref_pic_lists_modification()
        if (pps.lists_modification_present_flag && NumPicTotalCurr > 1) {
            int listEntryBits = ceil_log2(NumPicTotalCurr);

            ref_pic_list_modification_flag_l0 = bs.read_flag();
            if (ref_pic_list_modification_flag_l0) {
                for (uint32_t i = 0; i <= num_ref_idx_l0_active_minus1; i++) {
                    list_entry_l0[i] = bs.read_bits(listEntryBits);
                    // §7.4.7.2: 0..NumPicTotalCurr-1
                    REJECT_IF(list_entry_l0[i] >= static_cast<uint32_t>(NumPicTotalCurr),
                              "list_entry_l0[%u]=%u", i, list_entry_l0[i]);
                }
            }

            if (slice_type == SliceType::B) {
                ref_pic_list_modification_flag_l1 = bs.read_flag();
                if (ref_pic_list_modification_flag_l1) {
                    for (uint32_t i = 0; i <= num_ref_idx_l1_active_minus1; i++) {
                        list_entry_l1[i] = bs.read_bits(listEntryBits);
                        REJECT_IF(list_entry_l1[i] >= static_cast<uint32_t>(NumPicTotalCurr),
                                  "list_entry_l1[%u]=%u", i, list_entry_l1[i]);
                    }
                }
            }
        }

        if (slice_type == SliceType::B) {
            mvd_l1_zero_flag = bs.read_flag();
        }

        if (pps.cabac_init_present_flag) {
            cabac_init_flag = bs.read_flag();
        }

        if (slice_temporal_mvp_enabled_flag) {
            if (slice_type == SliceType::B) {
                collocated_from_l0_flag = bs.read_flag();
            }
            if ((collocated_from_l0_flag && num_ref_idx_l0_active_minus1 > 0) ||
                (!collocated_from_l0_flag && num_ref_idx_l1_active_minus1 > 0)) {
                collocated_ref_idx = bs.read_ue();
            }
            // §7.4.7.1: indexes the list ColPic is taken from
            REJECT_IF(collocated_ref_idx > (collocated_from_l0_flag ? num_ref_idx_l0_active_minus1
                                                                    : num_ref_idx_l1_active_minus1),
                      "collocated_ref_idx=%u", collocated_ref_idx);
        }

        // Prediction weight table
        if ((pps.weighted_pred_flag && slice_type == SliceType::P) ||
            (pps.weighted_bipred_flag && slice_type == SliceType::B)) {
            if (!parse_pred_weight_table(bs, pred_weight_table, sps, slice_type,
                                         num_ref_idx_l0_active_minus1,
                                         num_ref_idx_l1_active_minus1))
                return false;
        }

        five_minus_max_num_merge_cand = bs.read_ue();
        // §7.4.7.1: MaxNumMergeCand in 1..5
        REJECT_IF(five_minus_max_num_merge_cand > 4, "five_minus_max_num_merge_cand=%u",
                  five_minus_max_num_merge_cand);
        MaxNumMergeCand = 5 - static_cast<int>(five_minus_max_num_merge_cand);

        // motion_vector_resolution_control_idc == 2: use_integer_mv_flag
        // Not applicable for Main profile, skip
    }

    // QP
    slice_qp_delta = bs.read_se();
    // §7.4.7.1: SliceQpY in -QpBdOffsetY..51 (computed wide: slice_qp_delta is
    // an se(v) and could overflow the int sum)
    {
        const int64_t qp = 26 + static_cast<int64_t>(pps.init_qp_minus26) + slice_qp_delta;
        REJECT_IF(qp < -sps.QpBdOffsetY || qp > 51, "SliceQpY=%lld", static_cast<long long>(qp));
        SliceQpY = static_cast<int>(qp);
    }

    if (pps.pps_slice_chroma_qp_offsets_present_flag) {
        slice_cb_qp_offset = bs.read_se();
        slice_cr_qp_offset = bs.read_se();
        // §7.4.7.1: -12..12, and so is the sum with the PPS offset
        REJECT_IF(slice_cb_qp_offset < -12 || slice_cb_qp_offset > 12 ||
                  slice_cr_qp_offset < -12 || slice_cr_qp_offset > 12 ||
                  pps.pps_cb_qp_offset + slice_cb_qp_offset < -12 ||
                  pps.pps_cb_qp_offset + slice_cb_qp_offset > 12 ||
                  pps.pps_cr_qp_offset + slice_cr_qp_offset < -12 ||
                  pps.pps_cr_qp_offset + slice_cr_qp_offset > 12,
                  "slice chroma qp offsets %d/%d", slice_cb_qp_offset, slice_cr_qp_offset);
    }

    // cu_chroma_qp_offset_enabled_flag — only if chroma_qp_offset_list_enabled_flag
    // (PPS range extension, not Main profile) — skip

    // Deblocking
    slice_deblocking_filter_disabled_flag = pps.pps_deblocking_filter_disabled_flag;
    slice_beta_offset_div2 = pps.pps_beta_offset_div2;
    slice_tc_offset_div2 = pps.pps_tc_offset_div2;

    if (pps.deblocking_filter_override_enabled_flag) {
        deblocking_filter_override_flag = bs.read_flag();
    }
    if (deblocking_filter_override_flag) {
        slice_deblocking_filter_disabled_flag = bs.read_flag();
        if (!slice_deblocking_filter_disabled_flag) {
            slice_beta_offset_div2 = bs.read_se();
            slice_tc_offset_div2 = bs.read_se();
            // §7.4.7.1: -6..6
            REJECT_IF(slice_beta_offset_div2 < -6 || slice_beta_offset_div2 > 6 ||
                      slice_tc_offset_div2 < -6 || slice_tc_offset_div2 > 6,
                      "deblocking offsets %d/%d", slice_beta_offset_div2, slice_tc_offset_div2);
        }
    }

    // Loop filter across slices — infer from PPS when not present (spec §7.4.7.1)
    slice_loop_filter_across_slices_enabled_flag = pps.pps_loop_filter_across_slices_enabled_flag;
    if (pps.pps_loop_filter_across_slices_enabled_flag &&
        (slice_sao_luma_flag || slice_sao_chroma_flag ||
         !slice_deblocking_filter_disabled_flag)) {
        slice_loop_filter_across_slices_enabled_flag = bs.read_flag();
    }

    // Entry point offsets
    if (pps.tiles_enabled_flag || pps.entropy_coding_sync_enabled_flag) {
        num_entry_point_offsets = bs.read_ue();
        // §7.4.7.1: bounded by the substreams the picture can have — CTB rows
        // (WPP), tiles, or tile columns x CTB rows (both). Caps the resize below.
        {
            const uint32_t tiles = (pps.num_tile_columns_minus1 + 1) * (pps.num_tile_rows_minus1 + 1);
            uint32_t max_entry = 0;
            if (!pps.tiles_enabled_flag && pps.entropy_coding_sync_enabled_flag)
                max_entry = static_cast<uint32_t>(sps.PicHeightInCtbsY) - 1;
            else if (pps.tiles_enabled_flag && !pps.entropy_coding_sync_enabled_flag)
                max_entry = tiles - 1;
            else
                max_entry = (pps.num_tile_columns_minus1 + 1) * static_cast<uint32_t>(sps.PicHeightInCtbsY) - 1;
            REJECT_IF(num_entry_point_offsets > max_entry, "num_entry_point_offsets=%u (max %u)",
                      num_entry_point_offsets, max_entry);
        }
        if (num_entry_point_offsets > 0) {
            uint32_t offset_len_minus1 = bs.read_ue();
            // §7.4.7.1: 0..31
            REJECT_IF(offset_len_minus1 > 31, "offset_len_minus1=%u", offset_len_minus1);
            entry_point_offset_minus1.resize(num_entry_point_offsets);
            for (uint32_t i = 0; i < num_entry_point_offsets; i++) {
                entry_point_offset_minus1[i] = bs.read_bits(static_cast<int>(offset_len_minus1) + 1);
            }
        }
    }

    // Slice segment header extension
    if (pps.slice_segment_header_extension_present_flag) {
        uint32_t ext_length = bs.read_ue();
        // §7.4.7.1: 0..256
        REJECT_IF(ext_length > 256, "slice_segment_header_extension_length=%u", ext_length);
        for (uint32_t i = 0; i < ext_length; i++) {
            bs.read_bits(8);  // slice_segment_header_extension_data_byte
        }
    }

    // byte_alignment()
    bs.byte_alignment();

    HEVC_LOG(PARSE, "Slice: type=%d poc_lsb=%d qp=%d addr=%d first=%d dep=%d",
             static_cast<int>(slice_type), slice_pic_order_cnt_lsb, SliceQpY,
             slice_segment_address, first_slice_segment_in_pic_flag ? 1 : 0,
             dependent_slice_segment_flag ? 1 : 0);

    (void)nuh_temporal_id;
    return true;
}

} // namespace hevc
