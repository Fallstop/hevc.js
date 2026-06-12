// Sample Adaptive Offset — Spec §8.7.3
// Transcription directe de la spec ITU-T H.265 v8 (08/2021)

#include "filters/sao.h"
#include "common/types.h"

#include <cstring>
#include <vector>
#include <algorithm>

// Portable SSE2 (native x86-64 baseline; emscripten lowers to wasm128 with -msse2).
// Same single-source strategy as interpolation.cpp / transform.cpp: the oracle host
// compiles the exact intrinsics that ship to WASM.
#if defined(__SSE2__)
  #define HEVC_SIMD_SAO 1
  #include <emmintrin.h>
#endif

namespace hevc {

#ifdef HEVC_SIMD_SAO
// sign(c - n) per §8.7.3.2 as an int16 lane: +1 if c>n, -1 if c<n, else 0.
// Picture samples are < 32768 so the signed-int16 compare equals the scalar
// unsigned compare (bit-exact with `(c<n)?-1:(c>n)?1:0`).
static inline __m128i sao_sign16(__m128i c, __m128i n) {
    return _mm_sub_epi16(_mm_cmpgt_epi16(n, c), _mm_cmpgt_epi16(c, n));
}

// Edge-offset, one contiguous run [lo,hi) of a single row. c = cRow[x],
// a = aRow[x+dx0], b = bRow[x+dx1] (both neighbour rows already chosen by the
// caller). Bit-exact with the scalar inner loop: edgeIdx = 2 + sign(c-a) +
// sign(c-b), out = Clip3(0, maxVal, c + offTab[edgeIdx]).
static inline void sao_eo_row(const uint16_t* cRow, const uint16_t* aRow,
                              const uint16_t* bRow, uint16_t* dRow,
                              int lo, int hi, int dx0, int dx1,
                              const int16_t* offTab, int maxVal) {
    const __m128i two = _mm_set1_epi16(2);
    const __m128i vmax = _mm_set1_epi16(static_cast<int16_t>(maxVal));
    const __m128i zero = _mm_setzero_si128();
    // edgeIdx ∈ [0,4]; select the matching offset by masked-accumulate (no pshufb,
    // which is SSSE3 — keep to baseline SSE2 so emscripten lowers cleanly to wasm128).
    const __m128i o0 = _mm_set1_epi16(offTab[0]);
    const __m128i o1 = _mm_set1_epi16(offTab[1]);
    const __m128i o2 = _mm_set1_epi16(offTab[2]);
    const __m128i o3 = _mm_set1_epi16(offTab[3]);
    const __m128i o4 = _mm_set1_epi16(offTab[4]);
    int x = lo;
    for (; x + 8 <= hi; x += 8) {
        __m128i c = _mm_loadu_si128(reinterpret_cast<const __m128i*>(cRow + x));
        __m128i a = _mm_loadu_si128(reinterpret_cast<const __m128i*>(aRow + x + dx0));
        __m128i b = _mm_loadu_si128(reinterpret_cast<const __m128i*>(bRow + x + dx1));
        __m128i e = _mm_add_epi16(two, _mm_add_epi16(sao_sign16(c, a), sao_sign16(c, b)));
        __m128i off = _mm_or_si128(
            _mm_or_si128(_mm_and_si128(_mm_cmpeq_epi16(e, zero), o0),
                         _mm_and_si128(_mm_cmpeq_epi16(e, _mm_set1_epi16(1)), o1)),
            _mm_or_si128(_mm_and_si128(_mm_cmpeq_epi16(e, two), o2),
                _mm_or_si128(_mm_and_si128(_mm_cmpeq_epi16(e, _mm_set1_epi16(3)), o3),
                             _mm_and_si128(_mm_cmpeq_epi16(e, _mm_set1_epi16(4)), o4))));
        __m128i v = _mm_add_epi16(c, off);
        v = _mm_max_epi16(_mm_min_epi16(v, vmax), zero);
        _mm_storeu_si128(reinterpret_cast<__m128i*>(dRow + x), v);
    }
    for (; x < hi; x++) {
        int c = cRow[x], a = aRow[x + dx0], b = bRow[x + dx1];
        int e = 2 + ((c < a) ? -1 : (c > a) ? 1 : 0) + ((c < b) ? -1 : (c > b) ? 1 : 0);
        int v = c + offTab[e];
        dRow[x] = static_cast<uint16_t>(v < 0 ? 0 : v > maxVal ? maxVal : v);
    }
}

// Band-offset, one contiguous run [lo,hi). bandIdx = (sample>>bandShift) - bandPos;
// when bandIdx ∈ [0,4) add offTab[bandIdx], else leave the sample unchanged.
// Out-of-range lanes select offset 0 (no eq match) → Clip3 is a no-op self-write,
// bit-exact with the scalar `continue`.
static inline void sao_bo_row(const uint16_t* sRow, uint16_t* dRow,
                              int lo, int hi, int bandShift, int bandPos,
                              const int16_t* offTab, int maxVal) {
    const __m128i vmax = _mm_set1_epi16(static_cast<int16_t>(maxVal));
    const __m128i zero = _mm_setzero_si128();
    const __m128i vpos = _mm_set1_epi16(static_cast<int16_t>(bandPos));
    const __m128i o0 = _mm_set1_epi16(offTab[0]);
    const __m128i o1 = _mm_set1_epi16(offTab[1]);
    const __m128i o2 = _mm_set1_epi16(offTab[2]);
    const __m128i o3 = _mm_set1_epi16(offTab[3]);
    int x = lo;
    for (; x + 8 <= hi; x += 8) {
        __m128i s = _mm_loadu_si128(reinterpret_cast<const __m128i*>(sRow + x));
        __m128i bi = _mm_sub_epi16(_mm_srli_epi16(s, bandShift), vpos);
        __m128i off = _mm_or_si128(
            _mm_or_si128(_mm_and_si128(_mm_cmpeq_epi16(bi, zero), o0),
                         _mm_and_si128(_mm_cmpeq_epi16(bi, _mm_set1_epi16(1)), o1)),
            _mm_or_si128(_mm_and_si128(_mm_cmpeq_epi16(bi, _mm_set1_epi16(2)), o2),
                         _mm_and_si128(_mm_cmpeq_epi16(bi, _mm_set1_epi16(3)), o3)));
        __m128i v = _mm_add_epi16(s, off);
        v = _mm_max_epi16(_mm_min_epi16(v, vmax), zero);
        _mm_storeu_si128(reinterpret_cast<__m128i*>(dRow + x), v);
    }
    for (; x < hi; x++) {
        int sample = sRow[x];
        int bandIdx = (sample >> bandShift) - bandPos;
        if (bandIdx >= 0 && bandIdx < 4) {
            int v = sample + offTab[bandIdx];
            dRow[x] = static_cast<uint16_t>(v < 0 ? 0 : v > maxVal ? maxVal : v);
        }
    }
}
#endif  // HEVC_SIMD_SAO

// §8.7.3.2: EO class direction offsets
// Class 0 (H):    (-1, 0), (1, 0)
// Class 1 (V):    (0, -1), (0, 1)
// Class 2 (D135): (-1, -1), (1, 1)
// Class 3 (D45):  (1, -1), (-1, 1)
static const int eo_dx[4][2] = {{-1, 1}, {0, 0}, {-1, 1}, {1, -1}};
static const int eo_dy[4][2] = {{0, 0}, {-1, 1}, {-1, 1}, {-1, 1}};

void apply_sao(DecodingContext& ctx) {
    auto& sps = *ctx.sps;
    auto& pps = *ctx.pps;
    auto* pic = ctx.pic;

    if (!sps.sample_adaptive_offset_enabled_flag) return;

    int ctbSize = 1 << sps.CtbLog2SizeY;
    int subW = sps.SubWidthC;
    int subH = sps.SubHeightC;
    int numComp = (sps.ChromaArrayType != 0) ? 3 : 1;

    // Quick check: skip entirely if no CTU has SAO enabled. Track per component:
    // a component with no SAO anywhere is never read from the backup (its CTBs are
    // skipped below), so there is no reason to copy its plane — at 4K the chroma
    // planes are half the picture's bytes, and chroma SAO is frequently off.
    bool anySaoComp[3] = { false, false, false };
    for (int i = 0; i < sps.PicSizeInCtbsY; i++) {
        for (int c = 0; c < numComp; c++) {
            if (ctx.sao_params[i].sao_type_idx[c] != 0) anySaoComp[c] = true;
        }
        if (anySaoComp[0] && (numComp == 1 || (anySaoComp[1] && anySaoComp[2]))) break;
    }
    if (!anySaoComp[0] && !anySaoComp[1] && !anySaoComp[2]) return;

    // Picture-level boundary pre-check (§8.7.3.2). The per-pixel cross-slice/tile
    // edge test only ever changes a result when the picture actually has multiple
    // slices, or has tiles with cross-tile loop filtering disabled. For the common
    // single-slice / no-tile case it is pure overhead that always yields "don't
    // skip" — hoist it out so the SIMD fast path engages. slice_idx is always
    // allocated, so the old `slice_idx != nullptr` test was always true.
    bool multiSlice = false;
    if (ctx.slice_idx) {
        for (int i = 1; i < sps.PicSizeInCtbsY; i++)
            if (ctx.slice_idx[i] != ctx.slice_idx[0]) { multiSlice = true; break; }
    }
    bool tilesActive = !pps.loop_filter_across_tiles_enabled_flag && !pps.TileId.empty();
    bool saoBoundaryPossible = multiSlice || tilesActive;

    // §8.7.3.1: SAO operates on a copy of the deblocked picture
    // Use persistent backup buffers (avoids heap allocation per frame)
    auto* origPlane = ctx.sao_backup;
    for (int c = 0; c < numComp; c++) {
        if (!anySaoComp[c]) continue;  // never read → don't copy
        size_t nSamp = pic->plane_samples(c);
        auto& backup = origPlane[c];
        backup.resize(nSamp);
        std::memcpy(backup.data(), pic->plane_ptr<uint16_t>(c), nSamp * sizeof(uint16_t));
    }

    // Process each CTU
    for (int ry = 0; ry < sps.PicHeightInCtbsY; ry++) {
        for (int rx = 0; rx < sps.PicWidthInCtbsY; rx++) {
            auto& sao = ctx.sao_params[ry * ctx.sao_params_stride + rx];

            for (int cIdx = 0; cIdx < numComp; cIdx++) {
                if (sao.sao_type_idx[cIdx] == 0) continue;

                // Check slice SAO flags
                // Note: we apply SAO globally; per-slice flag check would need
                // slice index per CTU. For single-slice pictures this is correct.
                // Multi-slice: the SAO params are already set to type=0 during
                // parsing if the slice flag was off.

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
                if (pcmFilterDisabled || pps.transquant_bypass_enabled_flag) {
                    // Only scan when PCM or transquant_bypass are possible in this stream
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

                // Pre-check: do we need cross-slice/tile boundary checks?
                // Only needed if neighbors can be in a different slice/tile
                bool needBoundaryCheck = saoBoundaryPossible;

                const uint16_t* origData = origPlane[cIdx].data();
                uint16_t* destData = pic->plane_ptr<uint16_t>(cIdx);

                if (sao.sao_type_idx[cIdx] == 2) {
                    // Edge offset — §8.7.3.2
                    int eoClass = sao.sao_eo_class[cIdx];
                    int dx0 = eo_dx[eoClass][0], dy0 = eo_dy[eoClass][0];
                    int dx1 = eo_dx[eoClass][1], dy1 = eo_dy[eoClass][1];

#ifdef HEVC_SIMD_SAO
                    // SSE2/wasm128 fast path: no PCM/bypass and no cross-slice/tile
                    // boundary checks needed (the common case). Picture-edge columns
                    // and top/bottom rows fall out as no-ops, matching the scalar
                    // `continue`. Bit-exact with the scalar loop below.
                    const bool sao_simd = !ctbHasPcmOrBypass && !needBoundaryCheck;
                    const int16_t offTab[5] = {
                        static_cast<int16_t>(sao.sao_offset_val[cIdx][0]),
                        static_cast<int16_t>(sao.sao_offset_val[cIdx][1]),
                        static_cast<int16_t>(sao.sao_offset_val[cIdx][2]),
                        static_cast<int16_t>(sao.sao_offset_val[cIdx][3]),
                        static_cast<int16_t>(sao.sao_offset_val[cIdx][4]) };
#endif

                    for (int j = 0; j < nCtbSh; j++) {
                        int ySj = yCtb + j;
                        if (ySj >= compH) break;
#ifdef HEVC_SIMD_SAO
                        if (sao_simd) {
                            int yN1 = ySj + dy0, yN2 = ySj + dy1;
                            // Vertical neighbour out of picture → whole row no-op.
                            if (yN1 >= 0 && yN1 < compH && yN2 >= 0 && yN2 < compH) {
                                int iEnd = std::min(nCtbSw, compW - xCtb);
                                int gLo = std::max(xCtb, std::max(0, -std::min(dx0, dx1)));
                                int gHi = std::min(xCtb + iEnd, compW - std::max(0, std::max(dx0, dx1)));
                                if (gLo < gHi)
                                    sao_eo_row(origData + static_cast<size_t>(ySj) * stride,
                                               origData + static_cast<size_t>(yN1) * stride,
                                               origData + static_cast<size_t>(yN2) * stride,
                                               destData + static_cast<size_t>(ySj) * stride,
                                               gLo, gHi, dx0, dx1, offTab, maxVal);
                            }
                            continue;
                        }
#endif
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
                            if (needBoundaryCheck) {
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

                            // Store unconditionally (no per-pixel offset!=0 branch):
                            // c_val is already in [0,maxVal] and destData holds c_val from
                            // the pre-SAO backup, so offset==0 is a bit-exact self-write.
                            // This removes the decoder's single worst branch (~16% of all
                            // mispredicts; edgeIdx is high-entropy and unpredictable).
                            int offset = sao.sao_offset_val[cIdx][edgeIdx];
                            destData[ySj * stride + xSi] =
                                static_cast<uint16_t>(Clip3(0, maxVal, c_val + offset));
                        }
                    }
                } else {
                    // Band offset — §8.7.3.3
                    int bandShift = bitDepth - 5;
                    int bandPos = sao.sao_band_position[cIdx];

#ifdef HEVC_SIMD_SAO
                    const bool sao_simd = !ctbHasPcmOrBypass && !needBoundaryCheck;
                    const int16_t offTab[5] = {
                        static_cast<int16_t>(sao.sao_offset_val[cIdx][0]),
                        static_cast<int16_t>(sao.sao_offset_val[cIdx][1]),
                        static_cast<int16_t>(sao.sao_offset_val[cIdx][2]),
                        static_cast<int16_t>(sao.sao_offset_val[cIdx][3]),
                        static_cast<int16_t>(sao.sao_offset_val[cIdx][4]) };
#endif

                    for (int j = 0; j < nCtbSh; j++) {
                        int ySj = yCtb + j;
                        if (ySj >= compH) break;
#ifdef HEVC_SIMD_SAO
                        if (sao_simd) {
                            int iEnd = std::min(nCtbSw, compW - xCtb);
                            if (iEnd > 0)
                                sao_bo_row(origData + static_cast<size_t>(ySj) * stride,
                                           destData + static_cast<size_t>(ySj) * stride,
                                           xCtb, xCtb + iEnd, bandShift, bandPos, offTab, maxVal);
                            continue;
                        }
#endif
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

} // namespace hevc
