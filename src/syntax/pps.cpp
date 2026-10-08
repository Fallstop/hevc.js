#include "syntax/pps.h"
#include "syntax/sps.h"
#include "bitstream/bitstream_reader.h"
#include "common/debug.h"

namespace hevc {

#define REJECT_IF(cond, ...)                                       \
    do {                                                            \
        if (cond) {                                                 \
            HEVC_LOG(PARSE, "PPS rejected: " __VA_ARGS__);          \
            return false;                                           \
        }                                                           \
    } while (0)

bool PPS::parse(BitstreamReader& bs, const SPS& sps) {
    return parse_rbsp(bs) && derive(sps);
}

// Spec §7.3.2.3.1 — pic_parameter_set_rbsp(). The syntax does not depend on
// the SPS; only derive() does.
bool PPS::parse_rbsp(BitstreamReader& bs) {
    pps_pic_parameter_set_id = bs.read_ue();
    pps_seq_parameter_set_id = bs.read_ue();
    // §7.4.3.3.1: ids index the 64-entry PPS / 16-entry SPS stores
    REJECT_IF(pps_pic_parameter_set_id > 63 || pps_seq_parameter_set_id > 15,
              "pps id=%u sps id=%u", pps_pic_parameter_set_id, pps_seq_parameter_set_id);
    dependent_slice_segments_enabled_flag = bs.read_flag();
    output_flag_present_flag = bs.read_flag();
    num_extra_slice_header_bits = bs.read_bits(3);
    sign_data_hiding_enabled_flag = bs.read_flag();
    cabac_init_present_flag = bs.read_flag();

    num_ref_idx_l0_default_active_minus1 = bs.read_ue();
    num_ref_idx_l1_default_active_minus1 = bs.read_ue();
    // §7.4.3.3.1: 0..14 — copied into slice headers whose ref arrays hold 16
    REJECT_IF(num_ref_idx_l0_default_active_minus1 > 14 ||
              num_ref_idx_l1_default_active_minus1 > 14,
              "num_ref_idx_default l0=%u l1=%u", num_ref_idx_l0_default_active_minus1,
              num_ref_idx_l1_default_active_minus1);

    init_qp_minus26 = bs.read_se();
    // §7.4.3.3.1: -(26 + QpBdOffsetY)..+25 (the SPS-dependent bound is checked
    // in derive(); this one holds for every bit depth)
    REJECT_IF(init_qp_minus26 < -(26 + 48) || init_qp_minus26 > 25,
              "init_qp_minus26=%d", init_qp_minus26);
    constrained_intra_pred_flag = bs.read_flag();
    transform_skip_enabled_flag = bs.read_flag();

    cu_qp_delta_enabled_flag = bs.read_flag();
    if (cu_qp_delta_enabled_flag) {
        diff_cu_qp_delta_depth = bs.read_ue();
        REJECT_IF(diff_cu_qp_delta_depth > 3, "diff_cu_qp_delta_depth=%u", diff_cu_qp_delta_depth);
    }

    pps_cb_qp_offset = bs.read_se();
    pps_cr_qp_offset = bs.read_se();
    // §7.4.3.3.1: -12..12
    REJECT_IF(pps_cb_qp_offset < -12 || pps_cb_qp_offset > 12 ||
              pps_cr_qp_offset < -12 || pps_cr_qp_offset > 12,
              "chroma qp offsets %d/%d", pps_cb_qp_offset, pps_cr_qp_offset);
    pps_slice_chroma_qp_offsets_present_flag = bs.read_flag();

    weighted_pred_flag = bs.read_flag();
    weighted_bipred_flag = bs.read_flag();
    transquant_bypass_enabled_flag = bs.read_flag();

    tiles_enabled_flag = bs.read_flag();
    entropy_coding_sync_enabled_flag = bs.read_flag();

    if (tiles_enabled_flag) {
        num_tile_columns_minus1 = bs.read_ue();
        num_tile_rows_minus1 = bs.read_ue();
        // §7.4.3.3.1 bounds these by the picture size in CTBs (checked in
        // derive_tile_scan); this bound keeps the resizes below small even
        // before an SPS is known: 16384 / 16 = 1024 CTBs per dimension.
        REJECT_IF(num_tile_columns_minus1 > 1023 || num_tile_rows_minus1 > 1023,
                  "tiles %ux%u", num_tile_columns_minus1 + 1, num_tile_rows_minus1 + 1);
        uniform_spacing_flag = bs.read_flag();

        column_width_minus1.clear();
        row_height_minus1.clear();
        if (!uniform_spacing_flag) {
            column_width_minus1.resize(num_tile_columns_minus1);
            for (uint32_t i = 0; i < num_tile_columns_minus1; i++) {
                column_width_minus1[i] = bs.read_ue();
                REJECT_IF(column_width_minus1[i] > 1023, "column_width_minus1=%u",
                          column_width_minus1[i]);
            }
            row_height_minus1.resize(num_tile_rows_minus1);
            for (uint32_t i = 0; i < num_tile_rows_minus1; i++) {
                row_height_minus1[i] = bs.read_ue();
                REJECT_IF(row_height_minus1[i] > 1023, "row_height_minus1=%u", row_height_minus1[i]);
            }
        }

        loop_filter_across_tiles_enabled_flag = bs.read_flag();
    }

    pps_loop_filter_across_slices_enabled_flag = bs.read_flag();

    deblocking_filter_control_present_flag = bs.read_flag();
    if (deblocking_filter_control_present_flag) {
        deblocking_filter_override_enabled_flag = bs.read_flag();
        pps_deblocking_filter_disabled_flag = bs.read_flag();
        if (!pps_deblocking_filter_disabled_flag) {
            pps_beta_offset_div2 = bs.read_se();
            pps_tc_offset_div2 = bs.read_se();
            // §7.4.3.3.1: -6..6
            REJECT_IF(pps_beta_offset_div2 < -6 || pps_beta_offset_div2 > 6 ||
                      pps_tc_offset_div2 < -6 || pps_tc_offset_div2 > 6,
                      "deblocking offsets %d/%d", pps_beta_offset_div2, pps_tc_offset_div2);
        }
    }

    pps_scaling_list_data_present_flag = bs.read_flag();
    if (pps_scaling_list_data_present_flag) {
        REJECT_IF(!scaling_list_data.parse(bs), "scaling_list_data%s", "");
    }

    lists_modification_present_flag = bs.read_flag();
    log2_parallel_merge_level_minus2 = bs.read_ue();
    // §7.4.3.3.1: 0..CtbLog2SizeY-2 (<= 4 for any CTB size)
    REJECT_IF(log2_parallel_merge_level_minus2 > 4, "log2_parallel_merge_level_minus2=%u",
              log2_parallel_merge_level_minus2);
    slice_segment_header_extension_present_flag = bs.read_flag();

    // pps_extension: the range extension changes slice-header / transform-unit
    // syntax this decoder does not implement. Parse its flags so a PPS that
    // actually uses those tools is refused instead of silently mis-decoded.
    pps_range_extension_flag = false;
    if (bs.more_rbsp_data()) {
        bool pps_extension_present_flag = bs.read_flag();
        if (pps_extension_present_flag) {
            pps_range_extension_flag = bs.read_flag();
            /*pps_multilayer_extension_flag =*/ bs.read_flag();
            /*pps_3d_extension_flag =*/ bs.read_flag();
            /*pps_scc_extension_flag =*/ bs.read_flag();
            /*pps_extension_4bits =*/ bs.read_bits(4);
            if (pps_range_extension_flag) {
                if (transform_skip_enabled_flag)
                    log2_max_transform_skip_block_size_minus2 = bs.read_ue();
                cross_component_prediction_enabled_flag = bs.read_flag();
                chroma_qp_offset_list_enabled_flag = bs.read_flag();
                REJECT_IF(log2_max_transform_skip_block_size_minus2 != 0 ||
                          cross_component_prediction_enabled_flag ||
                          chroma_qp_offset_list_enabled_flag,
                          "unsupported range-extension tools (tskip=%u ccp=%d cqp=%d)",
                          log2_max_transform_skip_block_size_minus2,
                          cross_component_prediction_enabled_flag ? 1 : 0,
                          chroma_qp_offset_list_enabled_flag ? 1 : 0);
                log2_sao_offset_scale_luma = bs.read_ue();
                log2_sao_offset_scale_chroma = bs.read_ue();
                REJECT_IF(log2_sao_offset_scale_luma != 0 || log2_sao_offset_scale_chroma != 0,
                          "unsupported log2_sao_offset_scale %u/%u",
                          log2_sao_offset_scale_luma, log2_sao_offset_scale_chroma);
            }
        }
    }

    derived_valid = false;
    return true;
}

// SPS-dependent derivation and range checks. Re-run whenever the SPS this PPS
// refers to is replaced: the tile tables are sized by the SPS picture size.
bool PPS::derive(const SPS& sps) {
    derived_valid = false;
    // §7.4.3.3.1: init_qp_minus26 >= -(26 + QpBdOffsetY)
    REJECT_IF(init_qp_minus26 < -(26 + sps.QpBdOffsetY), "init_qp_minus26=%d for QpBdOffsetY=%d",
              init_qp_minus26, sps.QpBdOffsetY);
    REJECT_IF(cu_qp_delta_enabled_flag &&
              diff_cu_qp_delta_depth > sps.log2_diff_max_min_luma_coding_block_size,
              "diff_cu_qp_delta_depth=%u", diff_cu_qp_delta_depth);
    REJECT_IF(static_cast<int>(log2_parallel_merge_level_minus2) + 2 > sps.CtbLog2SizeY,
              "log2_parallel_merge_level=%u", log2_parallel_merge_level_minus2 + 2);

    // Compute derived values
    Log2MinCuQpDeltaSize = sps.CtbLog2SizeY - static_cast<int>(diff_cu_qp_delta_depth);

    // Derive tile scan tables
    if (!derive_tile_scan(sps)) return false;

    HEVC_LOG(PARSE, "PPS: id=%d sps=%d tiles=%d(%dx%d) qp=%d deblock=%s",
             pps_pic_parameter_set_id, pps_seq_parameter_set_id,
             tiles_enabled_flag ? 1 : 0,
             num_tile_columns_minus1 + 1, num_tile_rows_minus1 + 1,
             26 + init_qp_minus26,
             pps_deblocking_filter_disabled_flag ? "off" : "on");

    derived_valid = true;
    return true;
}

// Spec §6.5.1 — CTB raster and tile scanning conversion
bool PPS::derive_tile_scan(const SPS& sps) {
    int numCtbsY = sps.PicSizeInCtbsY;
    int picWidthInCtbsY = sps.PicWidthInCtbsY;
    int picHeightInCtbsY = sps.PicHeightInCtbsY;
    int numTileCols = tiles_enabled_flag ? static_cast<int>(num_tile_columns_minus1) + 1 : 1;
    int numTileRows = tiles_enabled_flag ? static_cast<int>(num_tile_rows_minus1) + 1 : 1;

    // §7.4.3.3.1: at most one tile column/row per CTB column/row, and explicit
    // sizes must leave at least one CTB for the last column/row — otherwise the
    // scan below writes outside the picture.
    REJECT_IF(numTileCols > picWidthInCtbsY || numTileRows > picHeightInCtbsY,
              "%dx%d tiles for %dx%d CTBs", numTileCols, numTileRows, picWidthInCtbsY,
              picHeightInCtbsY);
    if (tiles_enabled_flag && !uniform_spacing_flag) {
        if (column_width_minus1.size() + 1 != static_cast<size_t>(numTileCols) ||
            row_height_minus1.size() + 1 != static_cast<size_t>(numTileRows))
            return false;
        int64_t sum = 0;
        for (uint32_t w : column_width_minus1) sum += static_cast<int64_t>(w) + 1;
        REJECT_IF(sum >= picWidthInCtbsY, "tile columns cover %lld of %d CTBs",
                  static_cast<long long>(sum), picWidthInCtbsY);
        sum = 0;
        for (uint32_t h : row_height_minus1) sum += static_cast<int64_t>(h) + 1;
        REJECT_IF(sum >= picHeightInCtbsY, "tile rows cover %lld of %d CTBs",
                  static_cast<long long>(sum), picHeightInCtbsY);
    }

    // Column widths in CTBs
    std::vector<int> colWidth(numTileCols);
    if (uniform_spacing_flag) {
        for (int i = 0; i < numTileCols; i++) {
            colWidth[i] = ((i + 1) * picWidthInCtbsY) / numTileCols -
                          (i * picWidthInCtbsY) / numTileCols;
        }
    } else {
        int sum = 0;
        for (int i = 0; i < numTileCols - 1; i++) {
            colWidth[i] = static_cast<int>(column_width_minus1[i]) + 1;
            sum += colWidth[i];
        }
        colWidth[numTileCols - 1] = picWidthInCtbsY - sum;
    }

    // Row heights in CTBs
    std::vector<int> rowHeight(numTileRows);
    if (uniform_spacing_flag) {
        for (int i = 0; i < numTileRows; i++) {
            rowHeight[i] = ((i + 1) * picHeightInCtbsY) / numTileRows -
                           (i * picHeightInCtbsY) / numTileRows;
        }
    } else {
        int sum = 0;
        for (int i = 0; i < numTileRows - 1; i++) {
            rowHeight[i] = static_cast<int>(row_height_minus1[i]) + 1;
            sum += rowHeight[i];
        }
        rowHeight[numTileRows - 1] = picHeightInCtbsY - sum;
    }

    // Column boundary positions (in CTBs)
    std::vector<int> colBd(numTileCols + 1);
    colBd[0] = 0;
    for (int i = 0; i < numTileCols; i++) {
        colBd[i + 1] = colBd[i] + colWidth[i];
    }

    // Row boundary positions (in CTBs)
    std::vector<int> rowBd(numTileRows + 1);
    rowBd[0] = 0;
    for (int i = 0; i < numTileRows; i++) {
        rowBd[i + 1] = rowBd[i] + rowHeight[i];
    }

    // CtbAddrRsToTs and CtbAddrTsToRs
    CtbAddrRsToTs.resize(numCtbsY);
    CtbAddrTsToRs.resize(numCtbsY);
    TileId.resize(numCtbsY);

    // Build the raster-to-tile-scan mapping
    // Spec §6.5.1 — Equations 6-1 to 6-3
    int tbIdx = 0;
    for (int tileRow = 0; tileRow < numTileRows; tileRow++) {
        for (int tileCol = 0; tileCol < numTileCols; tileCol++) {
            int tileId = tileRow * numTileCols + tileCol;
            for (int y = rowBd[tileRow]; y < rowBd[tileRow + 1]; y++) {
                for (int x = colBd[tileCol]; x < colBd[tileCol + 1]; x++) {
                    int rsAddr = y * picWidthInCtbsY + x;
                    CtbAddrRsToTs[rsAddr] = tbIdx;
                    CtbAddrTsToRs[tbIdx] = rsAddr;
                    TileId[tbIdx] = tileId;
                    tbIdx++;
                }
            }
        }
    }
    return tbIdx == numCtbsY;
}

} // namespace hevc
