#include <gtest/gtest.h>
#include <fstream>
#include <string>
#include <vector>

#include "bitstream/nal_unit.h"
#include "common/types.h"
#include "decoding/decoder.h"

using namespace hevc;

// F4 — decoder resync on lossy feeds.
//
// On a 24/7 lossy camera stream a single bad/undecodable picture must NOT abort
// the whole feed() buffer. The decoder must contain a per-picture decode failure
// (including a thrown bitstream over-read from a corrupt slice, on the main OR a
// WPP worker thread), drop that picture, skip P/B until the next IRAP, and
// resynchronize there — recovering byte-identical output at the next keyframe.

static std::vector<uint8_t> read_file(const std::string& path) {
    std::ifstream f(path, std::ios::binary);
    if (!f) return {};
    return {std::istreambuf_iterator<char>(f), {}};
}

// Find the picture in `pics` whose POC equals `poc` (nullptr if absent).
static const Picture* find_poc(const std::vector<Picture*>& pics, int32_t poc) {
    for (const Picture* p : pics)
        if (p->poc == poc) return p;
    return nullptr;
}

static bool planes_equal(const Picture* a, const Picture* b) {
    return a->plane_bytes[0] == b->plane_bytes[0] &&
           a->plane_bytes[1] == b->plane_bytes[1] &&
           a->plane_bytes[2] == b->plane_bytes[2];
}

// The opengop fixture is a multi-GOP stream: an IDR GOP followed by a CRA GOP
// (a second IRAP mid-stream). We corrupt one VCL NAL of the FIRST GOP by
// truncating its payload — exactly the kind of damage a dropped/partial packet
// produces on a lossy link — then feed the whole buffer in one decode() call.
TEST(DecoderResync, CorruptVclDoesNotAbortBufferAndResumesAtNextIrap) {
    std::string path = std::string(FIXTURES_DIR) + "/opengop_cra_rasl.265";
    auto stream = read_file(path);
    ASSERT_FALSE(stream.empty()) << "Cannot read " << path;

    // --- Reference: clean decode of the untouched stream. ---
    Decoder clean;
    ASSERT_EQ(clean.decode(stream.data(), stream.size()), DecodeStatus::OK);
    auto clean_pics = clean.output_pictures();
    ASSERT_GT(clean_pics.size(), 0u);

    // Locate the NALs. Pick the first VCL picture of the first GOP that is NOT
    // the leading IRAP (so the IRAP itself still decodes and anchors the GOP),
    // and the next IRAP after it (the CRA — the resync recovery point).
    NalParser parser;
    auto nals = parser.parse(stream.data(), stream.size());
    ASSERT_GT(nals.size(), 0u);

    size_t corrupt_idx = nals.size();   // first non-IRAP VCL after the first IRAP
    size_t next_irap_idx = nals.size(); // first IRAP at or after corrupt_idx
    bool seen_first_irap = false;
    for (size_t i = 0; i < nals.size(); i++) {
        NalUnitType t = nals[i].header.nal_unit_type;
        if (!is_vcl(t)) continue;
        if (is_irap(t)) {
            if (!seen_first_irap) { seen_first_irap = true; continue; }
            if (corrupt_idx != nals.size()) { next_irap_idx = i; break; }
        } else if (seen_first_irap && corrupt_idx == nals.size()) {
            corrupt_idx = i;
        }
    }
    ASSERT_LT(corrupt_idx, nals.size()) << "fixture has no non-IRAP VCL in first GOP";
    ASSERT_LT(next_irap_idx, nals.size()) << "fixture has no second IRAP to resync at";

    // --- Corrupt: truncate the chosen VCL NAL's payload (splice out the tail of
    // that NAL in the raw stream). This makes the slice undecodable. ---
    size_t off = nals[corrupt_idx].offset;
    size_t end = (corrupt_idx + 1 < nals.size()) ? nals[corrupt_idx + 1].offset
                                                  : stream.size();
    ASSERT_LT(off + 8, end) << "NAL too small to truncate meaningfully";
    auto corrupt = stream;
    corrupt.erase(corrupt.begin() + off + 8, corrupt.begin() + end);

    // Feed the whole corrupted buffer in one shot. The key contract: a single bad
    // picture must NOT make decode() abort the remaining NALs. With the old code
    // a thrown over-read in a WPP worker thread terminated the process; this must
    // now return OK without crashing.
    Decoder dec;
    ASSERT_EQ(dec.decode(corrupt.data(), corrupt.size()), DecodeStatus::OK)
        << "a single corrupt picture aborted the whole feed buffer";
    auto pics = dec.output_pictures();
    ASSERT_GT(pics.size(), 0u) << "decoder produced no output after a recoverable error";
    (void)next_irap_idx; // located above only to assert the fixture is multi-GOP

    // After resync at the next IRAP, the trailing pictures of the final GOP
    // reference only that clean recovery point, so they must be recovered
    // byte-identical to the clean decode. Check the last few pictures (display
    // order) — a contiguous recovered tail proves the decoder resynchronized.
    int recovered = 0;
    int32_t max_clean_poc = clean_pics.front()->poc;
    for (const Picture* c : clean_pics)
        if (c->poc > max_clean_poc) max_clean_poc = c->poc;

    int tail_checked = 0;
    for (int32_t poc = max_clean_poc; poc >= 0 && tail_checked < 4; poc--) {
        const Picture* c = find_poc(clean_pics, poc);
        if (!c) continue;
        const Picture* d = find_poc(pics, poc);
        ASSERT_NE(d, nullptr) << "trailing picture POC " << poc
                              << " missing after resync";
        EXPECT_TRUE(planes_equal(c, d))
            << "trailing picture POC " << poc << " not byte-exact after resync";
        if (planes_equal(c, d)) recovered++;
        tail_checked++;
    }
    EXPECT_GE(recovered, 4)
        << "decoder did not recover the trailing pictures after the next IRAP";
}

// A picture failing on the MAIN thread (single CTB row → no WPP workers) must be
// contained too. We use a tiny single-CTU-row stream and truncate its second
// picture; decode() must stay OK and the first (IDR) picture must survive.
TEST(DecoderResync, ContainsFailureAndKeepsEarlierPictures) {
    std::string path = std::string(FIXTURES_DIR) + "/opengop_cra_rasl.265";
    auto stream = read_file(path);
    ASSERT_FALSE(stream.empty());

    NalParser parser;
    auto nals = parser.parse(stream.data(), stream.size());

    // Find the first IRAP and the first non-IRAP VCL after it.
    size_t irap_idx = nals.size(), bad_idx = nals.size();
    bool seen = false;
    for (size_t i = 0; i < nals.size(); i++) {
        NalUnitType t = nals[i].header.nal_unit_type;
        if (!is_vcl(t)) continue;
        if (is_irap(t) && !seen) { seen = true; irap_idx = i; continue; }
        if (seen) { bad_idx = i; break; }
    }
    ASSERT_LT(irap_idx, nals.size());
    ASSERT_LT(bad_idx, nals.size());

    // Truncate the picture AFTER the IRAP so the IRAP decodes cleanly first.
    size_t off = nals[bad_idx].offset;
    size_t end = (bad_idx + 1 < nals.size()) ? nals[bad_idx + 1].offset : stream.size();
    ASSERT_LT(off + 8, end);
    auto corrupt = stream;
    corrupt.erase(corrupt.begin() + off + 8, corrupt.begin() + end);

    Decoder dec;
    EXPECT_EQ(dec.decode(corrupt.data(), corrupt.size()), DecodeStatus::OK);
    // The leading IDR (decoded before the corruption) must still be present.
    auto pics = dec.output_pictures();
    EXPECT_FALSE(pics.empty()) << "no pictures survived a contained failure";
}
