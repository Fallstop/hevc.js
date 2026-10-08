// Deterministic corruption corpus: every fixture below is mutated with fixed
// seeds (bit flips, truncations, NAL drops/duplicates/swaps, garbage NALs,
// parameter-set and slice-header corruption) and decoded per access unit with
// tags, under the default policy and with concealment. Run under ASan (Debug
// build) a memory error fails the test; beyond that each run must return OK
// from every feed(), keep the DPB bounded, output frames with sane geometry and
// tags that were actually fed, and flag every concealed picture. Across the
// corpus, concealment must drop strictly fewer pictures than skip-to-IRAP.

#include <gtest/gtest.h>

#include <set>

#include "../fuzz/mutate.h"
#include "test_util.h"

using namespace hevc;
using namespace hevc_test;
using hevc_fuzz::Mutation;

namespace {

struct RunResult {
    size_t frames = 0;
    size_t corrupt_frames = 0;
    size_t max_dpb = 0;
    std::vector<double> tags;  // output order
    std::vector<uint64_t> frame_hashes;  // POC, flags and samples, output order
    DecodeStats stats;
};

uint64_t frame_hash(const Picture* p) {
    uint64_t h = 1469598103934665603ull;
    auto mix = [&h](uint64_t v) { h = (h ^ v) * 1099511628211ull; };
    mix(static_cast<uint64_t>(static_cast<uint32_t>(p->poc)));
    mix(p->flags);
    for (const auto& plane : p->plane_bytes)
        for (uint8_t b : plane) mix(b);
    return h;
}

// drain: drain after every chunk (the live path); otherwise only flush at the
// end — the output must not depend on it.
RunResult run(const std::vector<uint8_t>& data, const ErrorPolicy& policy, bool drain = true) {
    RunResult r;
    auto chunks = hevc_fuzz::split_chunks(data);
    Decoder dec;
    dec.set_error_policy(policy);
    auto take = [&](const std::vector<Picture*>& pics, size_t fed) {
        for (const Picture* p : pics) {
            r.frames++;
            if (p->flags & PICTURE_CORRUPT) r.corrupt_frames++;
            EXPECT_GT(p->pic_width_in_luma, 0);
            EXPECT_GT(p->pic_height_in_luma, 0);
            EXPECT_GE(p->plane_bytes[0].size(),
                      static_cast<size_t>(p->stride[0]) * p->height[0] * p->bytes_per_sample);
            // The tag must be one that was fed (chunk index), never invented
            EXPECT_FALSE(std::isnan(p->tag));
            EXPECT_GE(p->tag, 0.0);
            EXPECT_LT(p->tag, static_cast<double>(fed));
            EXPECT_EQ(p->tag, static_cast<double>(static_cast<size_t>(p->tag)));
            r.tags.push_back(p->tag);
            r.frame_hashes.push_back(frame_hash(p));
        }
    };
    for (size_t i = 0; i < chunks.size(); i++) {
        EXPECT_EQ(dec.feed(chunks[i].data(), chunks[i].size(), static_cast<double>(i)), DecodeStatus::OK);
        if (drain) take(dec.drain(), i + 1);
        r.max_dpb = std::max(r.max_dpb, dec.dpb().pictures().size());
    }
    take(dec.flush(), chunks.size());
    r.stats = dec.stats();
    // Every picture handed out was released by the output process
    EXPECT_EQ(r.stats.pictures_output, r.frames);
    return r;
}

size_t count_vcl(const std::vector<uint8_t>& d) {
    size_t n = 0;
    for (const auto& s : hevc_fuzz::find_nals(d))
        if (s.payload_begin < d.size() && ((d[s.payload_begin] >> 1) & 0x3F) < 32) n++;
    return n;
}

struct Aggregate {
    size_t default_frames = 0, conceal_frames = 0;
    size_t default_dropped = 0, conceal_dropped = 0;
    size_t concealed = 0, corrupt_flagged = 0;
};

void check_run(const RunResult& r, const std::vector<uint8_t>& data, const char* what) {
    // A picture needs at least one VCL NAL of its own
    EXPECT_LE(r.frames, count_vcl(data)) << what;
    // MaxDpbSize, the picture being decoded and pictures bumped but not yet
    // handed over (drained after every chunk, so at most a chunk's worth)
    EXPECT_LE(r.max_dpb, static_cast<size_t>(DPB::kMaxDpbPictures + 2)) << what;
    EXPECT_LE(r.corrupt_frames, r.stats.concealed) << what;
}

class CorruptionCorpus : public ::testing::TestWithParam<const char*> {};

}  // namespace

TEST_P(CorruptionCorpus, RandomMutationsAreContained) {
    const auto clean = read_file(fixture(GetParam()));
    ASSERT_FALSE(clean.empty()) << GetParam();
    ErrorPolicy conceal;
    conceal.conceal_slice_errors = true;

    Aggregate agg;
    const uint32_t base = static_cast<uint32_t>(std::hash<std::string>{}(GetParam()) & 0xFFFF) * 1000;
    for (uint32_t k = 0; k < 24; k++) {
        std::mt19937 rng(base + k);
        auto data = clean;
        const std::string desc = hevc_fuzz::mutate(data, rng);
        SCOPED_TRACE(std::string(GetParam()) + " seed " + std::to_string(base + k) + " " + desc);

        RunResult d = run(data, ErrorPolicy{});
        RunResult c = run(data, conceal);
        check_run(d, data, "default");
        check_run(c, data, "conceal");
        // Concealed samples never depend on when the caller drains
        EXPECT_EQ(run(data, conceal, /*drain=*/false).frame_hashes, c.frame_hashes);
        EXPECT_EQ(d.corrupt_frames, 0u);
        agg.default_frames += d.frames;
        agg.conceal_frames += c.frames;
        agg.default_dropped += d.stats.skipped_error;
        agg.conceal_dropped += c.stats.skipped_error;
        agg.concealed += c.stats.concealed;
        agg.corrupt_flagged += c.corrupt_frames;
    }
    // Concealment never loses more frames over the corpus, and flags what it repairs
    EXPECT_GE(agg.conceal_frames, agg.default_frames) << GetParam();
    EXPECT_LE(agg.conceal_dropped, agg.default_dropped) << GetParam();
    EXPECT_EQ(agg.corrupt_flagged, agg.concealed) << GetParam();
    RecordProperty("default_frames", static_cast<int>(agg.default_frames));
    RecordProperty("conceal_frames", static_cast<int>(agg.conceal_frames));
}

// Losses in a low-delay stream: dropped/truncated NAL units must not reorder
// output (tags stay in feed order), and with concealment strictly fewer
// pictures are dropped than with skip-to-IRAP.
TEST(CorruptionCorpusLowDelay, LossKeepsTagsMonotonicAndConcealmentDropsLess) {
    ErrorPolicy conceal;
    conceal.conceal_slice_errors = true;
    size_t default_frames = 0, conceal_frames = 0;
    size_t default_dropped = 0, conceal_dropped = 0;
    const Mutation losses[] = {Mutation::DropNal, Mutation::TruncateNal, Mutation::TruncateStream,
                               Mutation::DuplicateNal};
    for (const char* name : {"p_qcif_10f.265", "camera_qcif_12f.265"}) {
        const auto clean = read_file(fixture(name));
        for (uint32_t k = 0; k < 24; k++) {
            std::mt19937 rng(7000 + k);
            auto data = clean;
            const Mutation m = losses[k % 4];
            hevc_fuzz::apply(m, data, rng);
            SCOPED_TRACE(std::string(name) + " seed " + std::to_string(7000 + k) + " " + hevc_fuzz::mutation_name(m));
            RunResult d = run(data, ErrorPolicy{});
            RunResult c = run(data, conceal);
            for (const RunResult* r : {&d, &c})
                for (size_t i = 1; i < r->tags.size(); i++) EXPECT_LE(r->tags[i - 1], r->tags[i]);
            EXPECT_GE(c.frames, d.frames);
            default_frames += d.frames;
            conceal_frames += c.frames;
            default_dropped += d.stats.skipped_error;
            conceal_dropped += c.stats.skipped_error;
        }
    }
    EXPECT_GT(conceal_frames, default_frames);
    EXPECT_LT(conceal_dropped, default_dropped);
}

INSTANTIATE_TEST_SUITE_P(Fixtures, CorruptionCorpus,
                         ::testing::Values("p_qcif_10f.265", "b_qcif_10f.265", "full_qcif_10f.265",
                                           "multislice_qcif_8f.265", "multislice_wpp_qcif_8f.265",
                                           "camera_qcif_12f.265", "opengop_qcif_12f.265",
                                           "i_p_qcif_422_4f.265", "i_p_qcif_444_4f.265",
                                           "mono_qcif_4f.265", "full_qcif_10f_10bit.265",
                                           "sei_recovery_point.265", "i_64x64_scaling_list.265"),
                         [](const ::testing::TestParamInfo<const char*>& info) {
                             std::string s = info.param;
                             s = s.substr(0, s.find('.'));
                             return s;
                         });
