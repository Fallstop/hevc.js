// Conformance guard for 4:4:4 (ChromaArrayType == 3) 32x32 chroma dequant
// scaling-list selection (H.265 Range Extensions, §7.4.5 / §8.6.3).
//
// The 32x32 scaling-list matrices are luma-only (sizeId 3 has matrices for
// matrixId 0/3 only). A 32x32 *chroma* transform block can only occur in 4:4:4,
// and the spec gives it no dedicated matrix — it reuses the 16x16 CHROMA matrix
// (sizeId 2) and its DC, NOT the 32x32 luma matrix. perform_dequant must
// therefore select scaling_list[2][chromaMatrixId] for a 32x32 chroma block.
//
// We can't reach this via the ffmpeg/libx265 oracle (its param interface can't
// set custom scaling lists), so this test drives perform_dequant directly with a
// scaling list whose 16x16 chroma matrix differs sharply from the 32x32 luma
// matrix and asserts the chroma block uses the 16x16-chroma value.

#include <gtest/gtest.h>
#include <cstdint>
#include <cstring>

#include "decoding/coding_tree.h"   // DecodingContext
#include "decoding/transform.h"
#include "syntax/sps.h"
#include "syntax/pps.h"

using namespace hevc;

namespace {

// levelScale[0] = 40 (see cabac_tables.h). Mirror the §8.6.3 dequant math so the
// expectation is independent of the implementation under test.
int expected_scaled(int coeff, int m, int qp, int bitDepth, int log2TrafoSize) {
    int qpPer = qp / 6;
    int qpRem = qp % 6;
    static const int levelScale[6] = { 40, 45, 51, 57, 64, 72 };
    int scale = levelScale[qpRem];
    int bdShift = bitDepth + log2TrafoSize + 10 - 15;
    int add = (bdShift > 0) ? (1 << (bdShift - 1)) : 0;
    long long v = static_cast<long long>(coeff) * m * scale;
    v = (v << qpPer) + add;
    v >>= bdShift;
    if (v > 32767) v = 32767;
    if (v < -32768) v = -32768;
    return static_cast<int>(v);
}

struct Fixture {
    SPS sps;
    PPS pps;
    CUInfo cu;            // defaults to MODE_INTRA
    DecodingContext ctx;

    Fixture() {
        sps.ChromaArrayType = 3;          // 4:4:4
        sps.BitDepthY = 8;
        sps.BitDepthC = 8;
        sps.MinCbSizeY = 64;              // any non-zero; cu_at(0,0) -> index 0
        sps.scaling_list_enabled_flag = true;
        sps.scaling_list_data.set_defaults();
        // Make the 16x16 INTRA chroma (Cb=matrixId 1, Cr=matrixId 2) matrices and
        // their DC sharply distinct from the 32x32 INTRA luma matrix (matrixId 0).
        for (int i = 0; i < 64; i++) {
            sps.scaling_list_data.scaling_list[2][1][i] = 60;  // 16x16 intra Cb
            sps.scaling_list_data.scaling_list[2][2][i] = 60;  // 16x16 intra Cr
            sps.scaling_list_data.scaling_list[3][0][i] = 16;  // 32x32 intra luma
        }
        sps.scaling_list_data.scaling_list_dc[0][1] = 60;       // 16x16 Cb DC
        sps.scaling_list_data.scaling_list_dc[0][2] = 60;       // 16x16 Cr DC
        sps.scaling_list_data.scaling_list_dc[1][0] = 16;       // 32x32 luma DC

        pps.pps_scaling_list_data_present_flag = false;         // use SPS lists

        ctx.sps = &sps;
        ctx.pps = &pps;
        ctx.cu_info = &cu;
        ctx.cu_info_stride = 1;
    }
};

}  // namespace

// A 32x32 chroma transform block must dequantize using the 16x16 chroma scaling
// matrix value (60), not the 32x32 luma value (16).
TEST(Dequant444ScalingList, Chroma32x32UsesChromaMatrix) {
    Fixture f;
    const int log2 = 5, trSize = 32, qp = 0;

    int16_t coeff[32 * 32] = {};
    coeff[0] = 1;                 // DC
    coeff[9 * trSize + 5] = 1;    // an interior coefficient (non-DC)
    int16_t scaled[32 * 32] = {};
    int lastX = 0, lastY = 0;

    perform_dequant(f.ctx, 0, 0, log2, /*cIdx=*/1, qp, coeff, scaled, &lastX, &lastY);

    // Both the DC (scaling_list_dc[0][1]) and the interior coefficient
    // (scaling_list[2][1][...]) are 60 in this fixture.
    EXPECT_EQ(scaled[0], expected_scaled(1, 60, qp, 8, log2)) << "DC must use 16x16 chroma DC";
    EXPECT_EQ(scaled[9 * trSize + 5], expected_scaled(1, 60, qp, 8, log2))
        << "interior coeff must use 16x16 chroma matrix";
    // Guard against the pre-fix behaviour (32x32 luma matrix value 16).
    EXPECT_NE(scaled[0], expected_scaled(1, 16, qp, 8, log2));
}

// Control: a 32x32 LUMA block still uses the 32x32 luma matrix (value 16) —
// the fix must not perturb luma.
TEST(Dequant444ScalingList, Luma32x32Unchanged) {
    Fixture f;
    const int log2 = 5, trSize = 32, qp = 0;

    int16_t coeff[32 * 32] = {};
    coeff[0] = 1;
    coeff[9 * trSize + 5] = 1;
    int16_t scaled[32 * 32] = {};
    int lastX = 0, lastY = 0;

    perform_dequant(f.ctx, 0, 0, log2, /*cIdx=*/0, qp, coeff, scaled, &lastX, &lastY);

    EXPECT_EQ(scaled[0], expected_scaled(1, 16, qp, 8, log2)) << "luma DC uses 32x32 luma DC";
    EXPECT_EQ(scaled[9 * trSize + 5], expected_scaled(1, 16, qp, 8, log2))
        << "luma interior uses 32x32 luma matrix";
}

// Control: a 16x16 chroma block already used the chroma matrix and must be
// unchanged (value 60) — confirms the fix only redirects the 32x32 chroma read.
TEST(Dequant444ScalingList, Chroma16x16Unchanged) {
    Fixture f;
    const int log2 = 4, qp = 0;

    int16_t coeff[16 * 16] = {};
    coeff[0] = 1;
    int16_t scaled[16 * 16] = {};
    int lastX = 0, lastY = 0;

    perform_dequant(f.ctx, 0, 0, log2, /*cIdx=*/1, qp, coeff, scaled, &lastX, &lastY);

    EXPECT_EQ(scaled[0], expected_scaled(1, 60, qp, 8, log2));
}
