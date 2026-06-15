#pragma once

// Transform inverse + Dequantization
// Spec §8.6 (transform), §8.6.3 (scaling/dequant)

#include <cstdint>

namespace hevc {

struct DecodingContext;

// Dequantization (§8.6.3)
// out_lastX/out_lastY (optional) report the inclusive non-zero coefficient
// bounding box, used to drive the inverse-transform non-zero-region skip.
void perform_dequant(DecodingContext& ctx, int x0, int y0,
                     int log2TrafoSize, int cIdx, int qp,
                     const int16_t* coefficients, int16_t* scaled,
                     int* out_lastX = nullptr, int* out_lastY = nullptr);

// Inverse transform (§8.6.4)
// DST 4x4 for luma intra, DCT for all others.
// lastX/lastY: inclusive non-zero coefficient bounding box (< 0 = unknown → full).
void perform_transform_inverse(int log2TrafoSize, int cIdx,
                                bool is_intra, bool transform_skip,
                                int bit_depth,
                                const int16_t* scaled, int16_t* residual,
                                int lastX = -1, int lastY = -1);

} // namespace hevc
