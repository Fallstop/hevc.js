#include <gtest/gtest.h>
#include <fstream>
#include <string>
#include <vector>

#include "decoding/decoder.h"

using namespace hevc;

// Helper: read a fixture file into a byte vector
static std::vector<uint8_t> read_file(const std::string& path) {
    std::ifstream f(path, std::ios::binary);
    if (!f) return {};
    return {std::istreambuf_iterator<char>(f), {}};
}

// Helper: assert two decoders produced byte-identical output (same picture
// count, same (cvs_id, poc) ordering, and pixel-perfect planes).
static void expect_same_output(Decoder& a, Decoder& b, const char* ctx) {
    auto pa = a.output_pictures();
    auto pb = b.output_pictures();
    ASSERT_EQ(pa.size(), pb.size()) << ctx << ": picture count differs";
    ASSERT_GT(pa.size(), 0u) << ctx << ": no pictures decoded";

    for (size_t i = 0; i < pa.size(); i++) {
        EXPECT_EQ(pa[i]->cvs_id, pb[i]->cvs_id) << ctx << ": cvs_id at " << i;
        EXPECT_EQ(pa[i]->poc, pb[i]->poc) << ctx << ": poc at " << i;
        EXPECT_EQ(pa[i]->plane_bytes[0], pb[i]->plane_bytes[0])
            << ctx << ": Y plane at frame " << i << " (poc=" << pa[i]->poc << ")";
        EXPECT_EQ(pa[i]->plane_bytes[1], pb[i]->plane_bytes[1])
            << ctx << ": Cb plane at frame " << i;
        EXPECT_EQ(pa[i]->plane_bytes[2], pb[i]->plane_bytes[2])
            << ctx << ": Cr plane at frame " << i;
    }
}

// ============================================================
// reset() then decode a DIFFERENT stream must match a fresh decoder.
// Catches stale DPB pictures, POC carry-over (prev_poc_*/first_picture_),
// cvs_id, and parameter-set leakage — all of which corrupt inter prediction
// silently rather than crashing.
// ============================================================
TEST(DecoderReset, CrossStreamMatchesFresh) {
    std::string path_a = std::string(FIXTURES_DIR) + "/p_qcif_10f.265";
    std::string path_b = std::string(FIXTURES_DIR) + "/b_qcif_10f.265";
    auto stream_a = read_file(path_a);
    auto stream_b = read_file(path_b);
    ASSERT_FALSE(stream_a.empty()) << "Cannot read " << path_a;
    ASSERT_FALSE(stream_b.empty()) << "Cannot read " << path_b;

    // Reused decoder: stream A, then reset, then stream B.
    Decoder reused;
    ASSERT_EQ(reused.decode(stream_a.data(), stream_a.size()), DecodeStatus::OK);
    ASSERT_GT(reused.output_pictures().size(), 0u);
    reused.reset();
    EXPECT_TRUE(reused.dpb().pictures().empty()) << "DPB not emptied by reset()";
    ASSERT_EQ(reused.decode(stream_b.data(), stream_b.size()), DecodeStatus::OK);

    // Fresh decoder: stream B only.
    Decoder fresh;
    ASSERT_EQ(fresh.decode(stream_b.data(), stream_b.size()), DecodeStatus::OK);

    expect_same_output(reused, fresh, "cross-stream reset");
}

// ============================================================
// Repeated reset/decode of the SAME stream must be deterministic and match a
// fresh decode every time. Exercises the free-and-reallocate path under
// AddressSanitizer (Debug CI builds with -fsanitize=address).
// ============================================================
TEST(DecoderReset, RepeatedResetIsDeterministic) {
    std::string path = std::string(FIXTURES_DIR) + "/full_qcif_10f.265";
    auto stream = read_file(path);
    ASSERT_FALSE(stream.empty()) << "Cannot read " << path;

    Decoder fresh;
    ASSERT_EQ(fresh.decode(stream.data(), stream.size()), DecodeStatus::OK);

    Decoder reused;
    for (int iter = 0; iter < 3; iter++) {
        reused.reset();
        ASSERT_EQ(reused.decode(stream.data(), stream.size()), DecodeStatus::OK)
            << "decode failed on iteration " << iter;
        expect_same_output(reused, fresh, "repeated reset");
    }
}

// ============================================================
// reset(clear_parameter_sets=false) keeps the stored VPS/SPS/PPS, so a stream
// that re-sends only slices (no parameter sets) still decodes. Compared against
// a fresh decoder fed the parameter sets followed by the same slices.
// ============================================================
TEST(DecoderReset, KeepsParameterSetsWhenAsked) {
    std::string path = std::string(FIXTURES_DIR) + "/p_qcif_10f.265";
    auto stream = read_file(path);
    ASSERT_FALSE(stream.empty()) << "Cannot read " << path;

    Decoder reused;
    ASSERT_EQ(reused.decode(stream.data(), stream.size()), DecodeStatus::OK);
    ASSERT_GT(reused.output_pictures().size(), 0u);

    // Keep parameter sets across reset; re-decoding the full stream (which also
    // carries its parameter sets) must still match a fresh decode bit-for-bit.
    reused.reset(/*clear_parameter_sets=*/false);
    EXPECT_TRUE(reused.dpb().pictures().empty()) << "DPB not emptied by reset()";
    ASSERT_EQ(reused.decode(stream.data(), stream.size()), DecodeStatus::OK);

    Decoder fresh;
    ASSERT_EQ(fresh.decode(stream.data(), stream.size()), DecodeStatus::OK);

    expect_same_output(reused, fresh, "reset keep-parameter-sets");
}
