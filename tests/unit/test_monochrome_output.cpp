// Monochrome (4:0:0) output-path tests — feature F3 (IR/thermal cameras).
//
// Two regression guards:
//  1. The SubWidthC/SubHeightC helpers must report 1/1 for MONOCHROME (spec
//     Table 6-1), matching SPS::derive() for chroma_format_idc == 0. The old
//     values (2/1) produced bogus non-zero chroma dimensions.
//  2. The C output API (hevc_decoder_get_frame) must report 0-width/0-height
//     chroma and NULL cb/cr pointers for monochrome — the chroma planes are
//     zero-length, so any non-null pointer / non-zero dim is an out-of-bounds
//     handle for the consumer.

#include <gtest/gtest.h>
#include <cstdint>
#include <fstream>
#include <string>
#include <vector>

#include "common/types.h"
#include "wasm/hevc_api.h"

using namespace hevc;

// SubWidthC / SubHeightC must match the spec (Table 6-1) for every format.
TEST(MonochromeOutput, ChromaSubsampleHelpers) {
    // Monochrome: no chroma grid -> 1/1 (the fix).
    EXPECT_EQ(SubWidthC(ChromaFormat::MONOCHROME), 1);
    EXPECT_EQ(SubHeightC(ChromaFormat::MONOCHROME), 1);

    // Regression anchors for the other formats.
    EXPECT_EQ(SubWidthC(ChromaFormat::YUV420), 2);
    EXPECT_EQ(SubHeightC(ChromaFormat::YUV420), 2);
    EXPECT_EQ(SubWidthC(ChromaFormat::YUV422), 2);
    EXPECT_EQ(SubHeightC(ChromaFormat::YUV422), 1);
    EXPECT_EQ(SubWidthC(ChromaFormat::YUV444), 1);
    EXPECT_EQ(SubHeightC(ChromaFormat::YUV444), 1);
}

// Decoding an i400 clip through the C API must yield luma-only frames: real
// luma dimensions/pointer, but zeroed chroma dims and NULL chroma pointers.
TEST(MonochromeOutput, GetFrameHasNoChroma) {
    std::string path = std::string(FIXTURES_DIR) + "/mono_qcif_4f.265";
    std::ifstream f(path, std::ios::binary);
    ASSERT_TRUE(f.good()) << "missing fixture: " << path;
    std::vector<uint8_t> data{std::istreambuf_iterator<char>(f), {}};
    ASSERT_FALSE(data.empty());

    HEVCDecoder* dec = hevc_decoder_create();
    ASSERT_NE(dec, nullptr);

    ASSERT_EQ(hevc_decoder_decode(dec, data.data(), data.size()), HEVC_OK);
    int n = hevc_decoder_get_frame_count(dec);
    ASSERT_GT(n, 0);

    for (int i = 0; i < n; i++) {
        HEVCFrame frame{};
        ASSERT_EQ(hevc_decoder_get_frame(dec, i, &frame), HEVC_OK);

        // Luma is present and correctly sized (176x144 QCIF).
        EXPECT_NE(frame.y, nullptr);
        EXPECT_EQ(frame.width, 176);
        EXPECT_EQ(frame.height, 144);
        EXPECT_GT(frame.stride_y, 0);

        // Monochrome: no chroma planes.
        EXPECT_EQ(frame.cb, nullptr);
        EXPECT_EQ(frame.cr, nullptr);
        EXPECT_EQ(frame.chroma_width, 0);
        EXPECT_EQ(frame.chroma_height, 0);
        EXPECT_EQ(frame.stride_c, 0);
    }

    // Stream info reports monochrome (chroma_format == 0).
    HEVCStreamInfo info{};
    ASSERT_EQ(hevc_decoder_get_info(dec, &info), HEVC_OK);
    EXPECT_EQ(info.chroma_format, 0);

    hevc_decoder_destroy(dec);
}
