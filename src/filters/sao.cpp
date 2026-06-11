// Sample Adaptive Offset — Spec §8.7.3
// Transcription directe de la spec ITU-T H.265 v8 (08/2021)
//
// Two code paths per CTU:
//  - Fast path: no PCM/transquant-bypass CUs and no slice/tile boundary
//    restrictions apply → tight row-pointer loops, no per-pixel checks.
//  - Slow path: literal spec transcription with per-pixel checks (rare:
//    multi-slice/tile pictures with loop-filter crossing disabled, or PCM).

#include "filters/sao.h"
#include "common/types.h"

#include <algorithm>
#include <cstring>
#include <vector>

namespace hevc {

// §8.7.3.2: EO class direction offsets
// Class 0 (H):    (-1, 0), (1, 0)
// Class 1 (V):    (0, -1), (0, 1)
// Class 2 (D135): (-1, -1), (1, 1)
// Class 3 (D45):  (1, -1), (-1, 1)
static const int eo_dx[4][2] = {{-1, 1}, {0, 0}, {-1, 1}, {1, -1}};
static const int eo_dy[4][2] = {{0, 0}, {-1, 1}, {-1, 1}, {-1, 1}};

// Fast edge-offset: all neighbors in-picture and same slice/tile.
// Loop bounds are pre-shrunk so neighbor accesses never leave the picture
// (out-of-picture neighbors → no modification per §8.7.3.2, i.e. skipped).
static void sao_eo_fast(const uint16_t* orig, uint16_t* dest, int stride,
                        int xCtb, int yCtb, int nCtbSw, int nCtbSh,
                        int compW, int compH, int eoClass,
                        const int* offsets, int maxVal) {
    int dx0 = eo_dx[eoClass][0], dy0 = eo_dy[eoClass][0];
    int dx1 = eo_dx[eoClass][1], dy1 = eo_dy[eoClass][1];

    int yA = yCtb, yB = std::min(yCtb + nCtbSh, compH);
    int xA = xCtb, xB = std::min(xCtb + nCtbSw, compW);
    if (dy0 < 0 || dy1 < 0) yA = std::max(yA, 1);
    if (dy0 > 0 || dy1 > 0) yB = std::min(yB, compH - 1);
    if (dx0 < 0 || dx1 < 0) xA = std::max(xA, 1);
    if (dx0 > 0 || dx1 > 0) xB = std::min(xB, compW - 1);

    for (int y = yA; y < yB; y++) {
        const uint16_t* oc = orig + y * stride;
        const uint16_t* oa = orig + (y + dy0) * stride + dx0;
        const uint16_t* ob = orig + (y + dy1) * stride + dx1;
        uint16_t* d = dest + y * stride;
        for (int x = xA; x < xB; x++) {
            int c = oc[x];
            // edgeIdx = 2 + sign(c-a) + sign(c-b), computed branchless
            int edgeIdx = 2 + ((c > oa[x]) - (c < oa[x])) + ((c > ob[x]) - (c < ob[x]));
            d[x] = static_cast<uint16_t>(Clip3(0, maxVal, c + offsets[edgeIdx]));
        }
    }
}

// Fast band-offset: applies a per-sample LUT covering the full value range.
static void sao_bo_fast(const uint16_t* orig, uint16_t* dest, int stride,
                        int xCtb, int yCtb, int nCtbSw, int nCtbSh,
                        int compW, int compH, const uint16_t* lut) {
    int yB = std::min(yCtb + nCtbSh, compH);
    int xB = std::min(xCtb + nCtbSw, compW);
    for (int y = yCtb; y < yB; y++) {
        const uint16_t* oc = orig + y * stride;
        uint16_t* d = dest + y * stride;
        for (int x = xCtb; x < xB; x++)
            d[x] = lut[oc[x]];
    }
}

void apply_sao(DecodingContext& ctx) {
    auto& sps = *ctx.sps;
    auto& pps = *ctx.pps;
    auto* pic = ctx.pic;

    if (!sps.sample_adaptive_offset_enabled_flag) return;

    int ctbSize = 1 << sps.CtbLog2SizeY;
    int subW = sps.SubWidthC;
    int subH = sps.SubHeightC;
    int numComp = (sps.ChromaArrayType != 0) ? 3 : 1;

    // Quick check: skip entirely if no CTU has SAO enabled
    bool anySao = false;
    for (int i = 0; i < sps.PicSizeInCtbsY && !anySao; i++) {
        for (int c = 0; c < numComp; c++) {
            if (ctx.sao_params[i].sao_type_idx[c] != 0) { anySao = true; break; }
        }
    }
    if (!anySao) return;

    // Per-picture: can slice/tile boundaries restrict filtering anywhere?
    // Single slice + single tile (the common case) → never.
    bool multiSlice = false;
    if (ctx.slice_idx) {
        for (int i = 1; i < sps.PicSizeInCtbsY; i++) {
            if (ctx.slice_idx[i] != ctx.slice_idx[0]) { multiSlice = true; break; }
        }
    }
    bool multiTile = !pps.loop_filter_across_tiles_enabled_flag && !pps.TileId.empty() &&
                     (pps.num_tile_columns_minus1 > 0 || pps.num_tile_rows_minus1 > 0);
    bool pcmOrBypassPossible =
        sps.pcm_loop_filter_disabled_flag || pps.transquant_bypass_enabled_flag;

    // §8.7.3.1: SAO operates on a copy of the deblocked picture
    // Use persistent backup buffers (avoids heap allocation per frame)
    auto* origPlane = ctx.sao_backup;
    for (int c = 0; c < numComp; c++) {
        auto& plane = pic->planes[c];
        auto& backup = origPlane[c];
        backup.resize(plane.size());
        std::memcpy(backup.data(), plane.data(), plane.size() * sizeof(uint16_t));
    }

    // Band-offset LUT, rebuilt only when the CTU's BO params change
    std::vector<uint16_t> boLut;
    int lutBandPos = -1, lutOff[4] = {0, 0, 0, 0}, lutCIdx = -1;

    // Process each CTU
    for (int ry = 0; ry < sps.PicHeightInCtbsY; ry++) {
        for (int rx = 0; rx < sps.PicWidthInCtbsY; rx++) {
            auto& sao = ctx.sao_params[ry * ctx.sao_params_stride + rx];

            for (int cIdx = 0; cIdx < numComp; cIdx++) {
                if (sao.sao_type_idx[cIdx] == 0) continue;

                int bitDepth = (cIdx == 0) ? sps.BitDepthY : sps.BitDepthC;
                int maxVal = (1 << bitDepth) - 1;
                bool pcmFilterDisabled = sps.pcm_loop_filter_disabled_flag;

                // CTB dimensions in this component
                int nCtbSw, nCtbSh;
                if (cIdx == 0) {
                    nCtbSw = ctbSize;
                    nCtbSh = ctbSize;
                } else {
                    nCtbSw = ctbSize / subW;
                    nCtbSh = ctbSize / subH;
                }

                int xCtb = rx * nCtbSw;
                int yCtb = ry * nCtbSh;
                int compW = pic->width[cIdx];
                int compH = pic->height[cIdx];
                int stride = pic->stride[cIdx];

                // Pre-check: does this CTU have any PCM or transquant_bypass CUs?
                // If not, skip per-pixel cu_at() checks (common case).
                bool ctbHasPcmOrBypass = false;
                if (pcmOrBypassPossible) {
                    int xYctb = rx * ctbSize;
                    int yYctb = ry * ctbSize;
                    int minCb = sps.MinCbSizeY;
                    int cbEnd_x = std::min(xYctb + ctbSize, (int)sps.pic_width_in_luma_samples);
                    int cbEnd_y = std::min(yYctb + ctbSize, (int)sps.pic_height_in_luma_samples);
                    for (int cy = yYctb; cy < cbEnd_y && !ctbHasPcmOrBypass; cy += minCb)
                        for (int cx = xYctb; cx < cbEnd_x && !ctbHasPcmOrBypass; cx += minCb) {
                            auto& cu = ctx.cu_at(cx, cy);
                            if ((pcmFilterDisabled && cu.is_pcm) || cu.cu_transquant_bypass)
                                ctbHasPcmOrBypass = true;
                        }
                }

                bool needBoundaryCheck = multiSlice || multiTile;

                const uint16_t* origData = origPlane[cIdx].data();
                uint16_t* destData = pic->planes[cIdx].data();

                int offsets[5];
                for (int k = 0; k < 5; k++) offsets[k] = sao.sao_offset_val[cIdx][k];

                if (sao.sao_type_idx[cIdx] == 2 && !ctbHasPcmOrBypass && !needBoundaryCheck) {
                    sao_eo_fast(origData, destData, stride, xCtb, yCtb, nCtbSw, nCtbSh,
                                compW, compH, sao.sao_eo_class[cIdx], offsets, maxVal);
                    continue;
                }
                if (sao.sao_type_idx[cIdx] == 1 && !ctbHasPcmOrBypass) {
                    // Build/rebuild the LUT only when the BO params change
                    int bandPos = sao.sao_band_position[cIdx];
                    bool dirty = (int)boLut.size() != maxVal + 1 || lutCIdx != cIdx ||
                                 lutBandPos != bandPos;
                    for (int k = 0; k < 4 && !dirty; k++)
                        if (lutOff[k] != offsets[k]) dirty = true;
                    if (dirty) {
                        int bandShift = bitDepth - 5;
                        boLut.resize(maxVal + 1);
                        for (int s = 0; s <= maxVal; s++) {
                            int bandIdx = (s >> bandShift) - bandPos;
                            int off = (bandIdx >= 0 && bandIdx < 4) ? offsets[bandIdx] : 0;
                            boLut[s] = static_cast<uint16_t>(Clip3(0, maxVal, s + off));
                        }
                        lutBandPos = bandPos;
                        lutCIdx = cIdx;
                        for (int k = 0; k < 4; k++) lutOff[k] = offsets[k];
                    }
                    sao_bo_fast(origData, destData, stride, xCtb, yCtb, nCtbSw, nCtbSh,
                                compW, compH, boLut.data());
                    continue;
                }

                // ---- Slow path: literal spec transcription ----
                if (sao.sao_type_idx[cIdx] == 2) {
                    // Edge offset — §8.7.3.2
                    int eoClass = sao.sao_eo_class[cIdx];
                    int dx0 = eo_dx[eoClass][0], dy0 = eo_dy[eoClass][0];
                    int dx1 = eo_dx[eoClass][1], dy1 = eo_dy[eoClass][1];

                    for (int j = 0; j < nCtbSh; j++) {
                        int ySj = yCtb + j;
                        if (ySj >= compH) break;
                        for (int i = 0; i < nCtbSw; i++) {
                            int xSi = xCtb + i;
                            if (xSi >= compW) break;

                            // §8.7.3.2: skip PCM and transquant_bypass
                            int xY = (cIdx == 0) ? xSi : xSi * subW;
                            int yY = (cIdx == 0) ? ySj : ySj * subH;
                            if (ctbHasPcmOrBypass) {
                                auto& cu = ctx.cu_at(xY, yY);
                                if ((pcmFilterDisabled && cu.is_pcm) || cu.cu_transquant_bypass)
                                    continue;
                            }

                            // Neighbor positions
                            int xN1 = xSi + dx0;
                            int yN1 = ySj + dy0;
                            int xN2 = xSi + dx1;
                            int yN2 = ySj + dy1;

                            // §8.7.3.2: out-of-picture neighbors → no modification
                            if (xN1 < 0 || xN1 >= compW || yN1 < 0 || yN1 >= compH) continue;
                            if (xN2 < 0 || xN2 >= compW || yN2 < 0 || yN2 >= compH) continue;

                            // §8.7.3.2: cross-slice boundary check
                            // edgeIdx = 0 when neighbor is in a different slice and the
                            // relevant slice_loop_filter_across_slices_enabled_flag == 0
                            bool skipEdge = false;
                            if (needBoundaryCheck && ctx.slice_idx) {
                                int curAddr = (yY / ctbSize) * sps.PicWidthInCtbsY + (xY / ctbSize);
                                int si_cur = ctx.slice_idx[curAddr];
                                for (int nk = 0; nk < 2 && !skipEdge; nk++) {
                                    int xNk = (nk == 0) ? xN1 : xN2;
                                    int yNk = (nk == 0) ? yN1 : yN2;
                                    int xYn = (cIdx == 0) ? xNk : xNk * subW;
                                    int yYn = (cIdx == 0) ? yNk : yNk * subH;
                                    int nbrAddr = (yYn / ctbSize) * sps.PicWidthInCtbsY + (xYn / ctbSize);
                                    int si_nbr = ctx.slice_idx[nbrAddr];
                                    if (si_cur != si_nbr) {
                                        // §8.7.3.2: check the flag of the slice whose entry
                                        // boundary is being crossed
                                        if (si_nbr < si_cur) {
                                            // neighbor in earlier slice → check current's flag
                                            if (!ctx.sh_at_ctb(curAddr).slice_loop_filter_across_slices_enabled_flag)
                                                skipEdge = true;
                                        } else {
                                            // neighbor in later slice → check neighbor's flag
                                            if (!ctx.sh_at_ctb(nbrAddr).slice_loop_filter_across_slices_enabled_flag)
                                                skipEdge = true;
                                        }
                                    }
                                }
                                // §8.7.3.2: cross-tile boundary check
                                if (!skipEdge && !pps.loop_filter_across_tiles_enabled_flag
                                    && !pps.TileId.empty()) {
                                    int ts_cur = pps.CtbAddrRsToTs[curAddr];
                                    for (int nk = 0; nk < 2 && !skipEdge; nk++) {
                                        int xNk = (nk == 0) ? xN1 : xN2;
                                        int yNk = (nk == 0) ? yN1 : yN2;
                                        int xYn = (cIdx == 0) ? xNk : xNk * subW;
                                        int yYn = (cIdx == 0) ? yNk : yNk * subH;
                                        int nbrAddr = (yYn / ctbSize) * sps.PicWidthInCtbsY + (xYn / ctbSize);
                                        int ts_nbr = pps.CtbAddrRsToTs[nbrAddr];
                                        if (pps.TileId[ts_cur] != pps.TileId[ts_nbr])
                                            skipEdge = true;
                                    }
                                }
                            }
                            if (skipEdge) continue;

                            int c_val = origData[ySj * stride + xSi];
                            int a = origData[yN1 * stride + xN1];
                            int b = origData[yN2 * stride + xN2];

                            // §8.7.3.2: edge index categorization
                            int edgeIdx;
                            int signC_A = (c_val < a) ? -1 : (c_val > a) ? 1 : 0;
                            int signC_B = (c_val < b) ? -1 : (c_val > b) ? 1 : 0;
                            // edgeIdx = 2 + sign(c-a) + sign(c-b)
                            edgeIdx = 2 + signC_A + signC_B;
                            // Map: 0=valley(both<), 1=concave(one<), 2=flat, 3=convex(one>), 4=peak(both>)

                            int offset = sao.sao_offset_val[cIdx][edgeIdx];
                            if (offset != 0) {
                                destData[ySj * stride + xSi] =
                                    static_cast<uint16_t>(Clip3(0, maxVal, c_val + offset));
                            }
                        }
                    }
                } else {
                    // Band offset — §8.7.3.3
                    int bandShift = bitDepth - 5;
                    int bandPos = sao.sao_band_position[cIdx];

                    for (int j = 0; j < nCtbSh; j++) {
                        int ySj = yCtb + j;
                        if (ySj >= compH) break;
                        for (int i = 0; i < nCtbSw; i++) {
                            int xSi = xCtb + i;
                            if (xSi >= compW) break;

                            if (ctbHasPcmOrBypass) {
                                int xY = (cIdx == 0) ? xSi : xSi * subW;
                                int yY = (cIdx == 0) ? ySj : ySj * subH;
                                auto& cu = ctx.cu_at(xY, yY);
                                if ((pcmFilterDisabled && cu.is_pcm) || cu.cu_transquant_bypass)
                                    continue;
                            }

                            int sample = origData[ySj * stride + xSi];
                            int band = sample >> bandShift;
                            int bandIdx = band - bandPos;
                            if (bandIdx >= 0 && bandIdx < 4) {
                                int offset = sao.sao_offset_val[cIdx][bandIdx];
                                if (offset != 0) {
                                    destData[ySj * stride + xSi] =
                                        static_cast<uint16_t>(Clip3(0, maxVal, sample + offset));
                                }
                            }
                        }
                    }
                }
            }
        }
    }
}

} // namespace hevc
