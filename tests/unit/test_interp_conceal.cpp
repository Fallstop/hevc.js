#include <gtest/gtest.h>

#include <cstdint>

#include "decoding/coding_tree.h"   // DecodingContext
#include "decoding/dpb.h"
#include "decoding/interpolation.h"
#include "syntax/sps.h"

using namespace hevc;

// Robustness guard for lost references (RTSP packet loss / mid-GOP join).
// A default-constructed DPB has empty ref pic lists, so ref_pic_list0/1(idx)
// resolves to nullptr for any idx. With both prediction lists flagged but no
// valid reference, perform_inter_prediction must NOT blend the uninitialized
// predLx_buf stack arrays (UB) — it must conceal the PU with neutral mid-gray.
//
// On valid streams a flagged list always resolves to a non-null refPic, so this
// path never fires there and pixel output stays byte-identical.

namespace {

// Build the minimal DecodingContext perform_inter_prediction touches before the
// concealment early-return: sps (bit depth + chroma sizing) and an empty dpb.
DecodingContext make_ctx(SPS& sps, DPB& dpb) {
    DecodingContext ctx;
    ctx.sps = &sps;
    ctx.dpb = &dpb;
    return ctx;
}

}  // namespace

// Both lists flagged but no valid reference -> luma filled with 1 << (BitDepthY-1).
TEST(InterpConceal, BothRefsMissingLumaNeutral8bit) {
    SPS sps;  // defaults: BitDepthY = 8, ChromaArrayType = 0
    DPB dpb;  // empty ref lists
    DecodingContext ctx = make_ctx(sps, dpb);

    const int W = 16, H = 16;
    int16_t pred[64 * 64];
    for (int i = 0; i < W * H; i++) pred[i] = -12345;  // poison

    // predFlagL0 = predFlagL1 = true, refIdx >= 0, but DPB is empty -> null refs.
    perform_inter_prediction(ctx, 0, 0, W, H, /*cIdx=*/0,
                             MV{0, 0}, MV{0, 0},
                             /*refIdxL0=*/0, /*refIdxL1=*/0,
                             /*predFlagL0=*/true, /*predFlagL1=*/true, pred);

    const int16_t neutral = 1 << (8 - 1);  // 128
    for (int i = 0; i < W * H; i++)
        EXPECT_EQ(pred[i], neutral) << "sample " << i;
}

// predFlag set but refIdx negative on both lists must also conceal (no UB).
TEST(InterpConceal, NegativeRefIdxBothListsNeutral) {
    SPS sps;
    DPB dpb;
    DecodingContext ctx = make_ctx(sps, dpb);

    const int W = 8, H = 8;
    int16_t pred[64 * 64];
    for (int i = 0; i < W * H; i++) pred[i] = 999;

    perform_inter_prediction(ctx, 0, 0, W, H, /*cIdx=*/0,
                             MV{4, 4}, MV{-4, -4},
                             /*refIdxL0=*/-1, /*refIdxL1=*/-1,
                             /*predFlagL0=*/true, /*predFlagL1=*/true, pred);

    const int16_t neutral = 1 << (8 - 1);
    for (int i = 0; i < W * H; i++)
        EXPECT_EQ(pred[i], neutral) << "sample " << i;
}

// 10-bit chroma: missing refs -> 1 << (BitDepthC-1) = 512.
TEST(InterpConceal, BothRefsMissingChromaNeutral10bit) {
    SPS sps;
    sps.BitDepthC = 10;
    sps.ChromaArrayType = 1;  // 4:2:0
    sps.SubWidthC = 2;
    sps.SubHeightC = 2;
    DPB dpb;
    DecodingContext ctx = make_ctx(sps, dpb);

    const int W = 16, H = 16;          // luma PU -> 8x8 chroma
    int16_t cpred[32 * 32];
    for (int i = 0; i < (W / 2) * (H / 2); i++) cpred[i] = -1;

    perform_inter_prediction(ctx, 0, 0, W, H, /*cIdx=*/1,
                             MV{0, 0}, MV{0, 0},
                             /*refIdxL0=*/0, /*refIdxL1=*/0,
                             /*predFlagL0=*/true, /*predFlagL1=*/true, cpred);

    const int16_t neutral = 1 << (10 - 1);  // 512
    for (int i = 0; i < (W / 2) * (H / 2); i++)
        EXPECT_EQ(cpred[i], neutral) << "sample " << i;
}
