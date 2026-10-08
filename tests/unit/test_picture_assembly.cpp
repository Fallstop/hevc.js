// P5 — picture assembly. Slice segments are grouped into pictures by
// first_slice_segment_in_pic_flag, address order, PPS and POC — not by NAL
// adjacency — so suffix SEI / filler data between slices no longer split a
// picture, a picture may span feed() calls, a slice of the next picture (whose
// first slice was lost) is not merged into the current one, and NAL units of
// other layers, reserved VCL types and NALs with the forbidden bit are ignored.

#include <gtest/gtest.h>

#include <algorithm>
#include <random>

#include "ps_builder.h"

using namespace hevc;
using namespace hevc_test;

namespace {

const char* kMultiSlice[] = {"multislice_qcif_8f.265", "multislice_wpp_qcif_8f.265"};

struct Output {
    std::vector<int32_t> pocs;
    std::vector<std::vector<uint8_t>> luma;
    std::vector<std::vector<uint8_t>> cb;
};

Output batch(const std::vector<uint8_t>& data, DecodeStats* stats = nullptr) {
    Decoder dec;
    EXPECT_EQ(dec.decode(data.data(), data.size()), DecodeStatus::OK);
    Output o;
    for (const Picture* p : dec.output_pictures()) {
        o.pocs.push_back(p->poc);
        o.luma.push_back(p->plane_bytes[0]);
        o.cb.push_back(p->plane_bytes[1]);
    }
    if (stats) *stats = dec.stats();
    return o;
}

// Rebuild a stream NAL by NAL, letting `edit` insert, drop or replace units.
// edit(index, nal, out) appends whatever should take the NAL's place.
template <typename Edit>
std::vector<uint8_t> rewrite(const std::vector<uint8_t>& data, Edit edit) {
    NalParser parser;
    auto nals = parser.parse(data.data(), data.size());
    std::vector<uint8_t> out;
    for (size_t i = 0; i < nals.size(); i++) edit(i, nals[i], nal_chunk(data, nals[i]), out);
    return out;
}

void append(std::vector<uint8_t>& out, const std::vector<uint8_t>& v) {
    out.insert(out.end(), v.begin(), v.end());
}

bool first_slice(const NalUnit& nal) {
    return is_vcl(nal.header.nal_unit_type) && !nal.rbsp.empty() && (nal.rbsp[0] & 0x80);
}

// The live path: one access unit per feed(), tagged with its index, drained
// after each.
struct Fed {
    Output out;
    std::vector<double> tags;
    std::vector<uint32_t> flags;
    DecodeStats stats;
};

Fed feed_per_au(const std::vector<uint8_t>& data, const ErrorPolicy& policy = ErrorPolicy{}) {
    Decoder dec;
    dec.set_error_policy(policy);
    Fed f;
    auto take = [&](const std::vector<Picture*>& pics) {
        for (const Picture* p : pics) {
            f.out.pocs.push_back(p->poc);
            f.out.luma.push_back(p->plane_bytes[0]);
            f.out.cb.push_back(p->plane_bytes[1]);
            f.tags.push_back(p->tag);
            f.flags.push_back(p->flags);
        }
    };
    const auto aus = split_access_units(data);
    for (size_t i = 0; i < aus.size(); i++) {
        EXPECT_EQ(dec.feed(aus[i].data(), aus[i].size(), static_cast<double>(i)), DecodeStatus::OK);
        take(dec.drain());
    }
    take(dec.flush());
    f.stats = dec.stats();
    return f;
}

bool unique(std::vector<double> v) {
    std::sort(v.begin(), v.end());
    return std::adjacent_find(v.begin(), v.end()) == v.end();
}

size_t count_vcl(const std::vector<uint8_t>& data) {
    NalParser parser;
    size_t n = 0;
    for (const auto& nal : parser.parse(data.data(), data.size()))
        if (is_vcl(nal.header.nal_unit_type)) n++;
    return n;
}

}  // namespace

TEST(PictureAssembly, InterleavedNonVclDoesNotSplitPictures) {
    for (const char* name : kMultiSlice) {
        auto data = read_file(fixture(name));
        ASSERT_FALSE(data.empty()) << name;
        BitWriter sei;
        sei.put_bits(5, 8);   // user_data_unregistered
        sei.put_bits(17, 8);
        for (int i = 0; i < 17; i++) sei.put_bits(0x42, 8);
        sei.trailing();
        const auto suffix_sei = make_nal(nal_h0(NalUnitType::SUFFIX_SEI), kNalH1, sei.take());
        const auto filler = make_nal(nal_h0(NalUnitType::FD_NUT), kNalH1, {0xFF, 0xFF, 0x80});
        const auto unspecified = make_nal(static_cast<uint8_t>(50 << 1), kNalH1, {0x12, 0x80});

        auto mixed = rewrite(data, [&](size_t, const NalUnit& nal, const std::vector<uint8_t>& chunk,
                                       std::vector<uint8_t>& out) {
            append(out, chunk);
            if (is_vcl(nal.header.nal_unit_type)) {
                append(out, suffix_sei);
                append(out, filler);
                append(out, unspecified);
            }
        });
        Output clean = batch(data);
        ASSERT_EQ(clean.pocs.size(), 8u) << name;
        Output got = batch(mixed);
        EXPECT_EQ(got.pocs, clean.pocs) << name;
        EXPECT_EQ(got.luma, clean.luma) << name;
    }
}

// The slices of one picture may arrive in separate feed() calls (an RTP
// depacketizer hands over NAL units one at a time).
TEST(PictureAssembly, PicturesMaySpanFeedCalls) {
    for (const char* name : kMultiSlice) {
        auto data = read_file(fixture(name));
        Output clean = batch(data);

        NalParser parser;
        Decoder dec;
        Output got;
        for (const auto& nal : parser.parse(data.data(), data.size())) {
            auto chunk = nal_chunk(data, nal);
            ASSERT_EQ(dec.feed(chunk.data(), chunk.size()), DecodeStatus::OK);
            for (const Picture* p : dec.drain()) {
                got.pocs.push_back(p->poc);
                got.luma.push_back(p->plane_bytes[0]);
            }
        }
        for (const Picture* p : dec.flush()) {
            got.pocs.push_back(p->poc);
            got.luma.push_back(p->plane_bytes[0]);
        }
        EXPECT_EQ(got.pocs, clean.pocs) << name;
        EXPECT_EQ(got.luma, clean.luma) << name;
        EXPECT_EQ(dec.stats().skipped_error, 0u) << name;
    }
}

// Lose the middle slice of the 4th picture: under the default policy that
// picture is dropped and decoding resumes at the next IRAP — and the pictures
// before it are untouched.
TEST(PictureAssembly, LostMiddleSliceDropsThePictureByDefault) {
    auto data = read_file(fixture("multislice_qcif_8f.265"));
    int pic = -1, slice = 0;
    auto lossy = rewrite(data, [&](size_t, const NalUnit& nal, const std::vector<uint8_t>& chunk,
                                   std::vector<uint8_t>& out) {
        if (is_vcl(nal.header.nal_unit_type)) {
            if (first_slice(nal)) { pic++; slice = 0; } else { slice++; }
            if (pic == 3 && slice == 1) return;
        }
        append(out, chunk);
    });
    DecodeStats stats;
    Output clean = batch(data);
    Output got = batch(lossy, &stats);
    EXPECT_LT(got.pocs.size(), clean.pocs.size());
    EXPECT_GE(stats.skipped_error, 1u);
    for (size_t i = 0; i < got.pocs.size(); i++) {
        for (size_t j = 0; j < clean.pocs.size(); j++)
            if (clean.pocs[j] == got.pocs[i]) EXPECT_EQ(got.luma[i], clean.luma[j]) << "POC " << got.pocs[i];
    }
}

// Lose the FIRST slice of a picture: its remaining slices start a new
// picture (POC / address order tell them apart) instead of being decoded into
// the previous picture, which must come out unchanged.
TEST(PictureAssembly, SlicesOfAPictureMissingItsFirstSliceAreNotMergedBackwards) {
    auto data = read_file(fixture("multislice_qcif_8f.265"));
    int pic = -1;
    auto lossy = rewrite(data, [&](size_t, const NalUnit& nal, const std::vector<uint8_t>& chunk,
                                   std::vector<uint8_t>& out) {
        if (is_vcl(nal.header.nal_unit_type) && first_slice(nal)) {
            pic++;
            if (pic == 3) return;
        }
        append(out, chunk);
    });
    Output clean = batch(data);
    Output got = batch(lossy);
    // Every picture that comes out matches the clean decode.
    for (size_t i = 0; i < got.pocs.size(); i++)
        for (size_t j = 0; j < clean.pocs.size(); j++)
            if (clean.pocs[j] == got.pocs[i]) EXPECT_EQ(got.luma[i], clean.luma[j]) << "POC " << got.pocs[i];
}

// NAL units of another layer, reserved VCL types and NAL units with the
// forbidden bit set never reach the decoding process.
TEST(PictureAssembly, ForeignLayerReservedAndForbiddenNalsAreIgnored) {
    auto data = read_file(fixture("multislice_qcif_8f.265"));
    auto noisy = rewrite(data, [&](size_t, const NalUnit& nal, const std::vector<uint8_t>& chunk,
                                   std::vector<uint8_t>& out) {
        append(out, chunk);
        if (is_vcl(nal.header.nal_unit_type) || nal.header.nal_unit_type == NalUnitType::SPS_NUT) {
            // Same NAL in layer 1 (nuh_layer_id in the low bit of byte 0 and
            // the top 5 bits of byte 1)
            auto layer1 = chunk;
            layer1[5] = static_cast<uint8_t>(layer1[5] | 0x08);
            append(out, layer1);
            // Same NAL with the forbidden_zero_bit set
            auto forbidden = chunk;
            forbidden[4] = static_cast<uint8_t>(forbidden[4] | 0x80);
            append(out, forbidden);
            // A reserved VCL type carrying the slice payload
            auto reserved = chunk;
            reserved[4] = static_cast<uint8_t>((reserved[4] & 0x81) | (12 << 1));
            append(out, reserved);
        }
    });
    DecodeStats stats;
    Output clean = batch(data);
    Output got = batch(noisy, &stats);
    EXPECT_EQ(got.pocs, clean.pocs);
    EXPECT_EQ(got.luma, clean.luma);
    EXPECT_GT(stats.nal_errors, 0u);  // the forbidden-bit copies
}

// Each slice segment NAL repeated once in turn (a retransmission, a
// duplicating relay): the repeat is dropped — the output is the clean decode,
// under either policy, and nothing is skipped.
TEST(PictureAssembly, RepeatedSliceSegmentsAreDropped) {
    ErrorPolicy conceal;
    conceal.conceal_slice_errors = true;
    for (const char* name : kMultiSlice) {
        const auto data = read_file(fixture(name));
        const Output clean = batch(data);
        const size_t nvcl = count_vcl(data);
        ASSERT_GT(nvcl, clean.pocs.size()) << name;
        for (size_t k = 0; k < nvcl; k++) {
            size_t v = 0;
            const auto dup = rewrite(data, [&](size_t, const NalUnit& nal, const std::vector<uint8_t>& chunk,
                                               std::vector<uint8_t>& out) {
                append(out, chunk);
                if (is_vcl(nal.header.nal_unit_type) && v++ == k) append(out, chunk);
            });
            for (const ErrorPolicy& policy : {ErrorPolicy{}, conceal}) {
                SCOPED_TRACE(std::string(name) + " repeat of slice NAL " + std::to_string(k) +
                             (policy.conceal_slice_errors ? " conceal" : " default"));
                const Fed got = feed_per_au(dup, policy);
                EXPECT_EQ(got.out.pocs, clean.pocs);
                EXPECT_EQ(got.out.luma, clean.luma);
                EXPECT_EQ(got.stats.skipped_error, 0u);
                EXPECT_EQ(got.stats.nal_errors, 1u);
                EXPECT_TRUE(unique(got.tags));
            }
            Output b = batch(dup);
            EXPECT_EQ(b.pocs, clean.pocs) << name << " slice NAL " << k;
        }
    }
}

// One bit error in a slice that is not the last of its picture: the slice
// decodes on past its end into the CTBs of the next slice. That slice still
// belongs to the same picture: with concealment every picture comes out
// exactly once (never two frames with one POC and tag) and nothing freezes.
TEST(PictureAssembly, ASliceThatRunsOnDoesNotSplitItsPicture) {
    ErrorPolicy conceal;
    conceal.conceal_slice_errors = true;
    NalParser parser;
    for (const char* name : kMultiSlice) {
        const auto data = read_file(fixture(name));
        const Output clean = batch(data);
        const auto aus = split_access_units(data);
        size_t corrupted = 0;
        for (uint32_t seed = 1; seed <= 40; seed++) {
            std::mt19937 rng(seed);
            const size_t a = 1 + rng() % (aus.size() - 1);
            auto au = aus[a];
            std::vector<NalUnit> vcl;
            for (auto& n : parser.parse(au.data(), au.size()))
                if (is_vcl(n.header.nal_unit_type)) vcl.push_back(n);
            if (vcl.size() < 2) continue;
            const NalUnit& n = vcl[rng() % (vcl.size() - 1)];
            const size_t lo = n.offset + 12, hi = n.offset + n.size;
            if (hi <= lo) continue;
            au[lo + rng() % (hi - lo)] ^= static_cast<uint8_t>(1u << (rng() % 8));
            std::vector<uint8_t> stream;
            for (size_t i = 0; i < aus.size(); i++) append(stream, i == a ? au : aus[i]);

            SCOPED_TRACE(std::string(name) + " seed " + std::to_string(seed));
            const Fed got = feed_per_au(stream, conceal);
            EXPECT_EQ(got.out.pocs, clean.pocs);
            EXPECT_TRUE(unique(got.tags));
            EXPECT_EQ(got.stats.skipped_error, 0u);
            if (got.stats.concealed) corrupted++;
        }
        EXPECT_GT(corrupted, 0u) << name;
    }
}

// WPP with dependent slice segments (JCT-VC DSLICE_A_HHI_5, first picture):
// the first slice segment fails half-way, and the dependent segments after it
// can no longer be decoded. With concealment the picture is concealed — it is
// not dropped with a freeze until the next IRAP.
TEST(PictureAssembly, LostDependentSegmentsAreConcealedNotFrozen) {
    const auto data = read_file(fixture("jctvc_DSLICE_A_HHI_5_1f.265"));
    ASSERT_FALSE(data.empty());
    ErrorPolicy conceal;
    conceal.conceal_slice_errors = true;
    NalParser parser;
    // Seeds where the first segment decodes past the dependent segment's
    // address before it fails (the dependent segment then used to be taken
    // for an orphan of a lost picture, arming skip-to-IRAP)
    for (uint32_t seed : {7u, 9u, 10u, 11u}) {
        std::mt19937 rng(seed);
        auto d = data;
        for (const auto& n : parser.parse(data.data(), data.size())) {
            if (!is_vcl(n.header.nal_unit_type)) continue;
            // Randomize the second half of the first slice segment, keeping
            // start-code emulation out of it
            const size_t start = n.offset + n.size / 2, end = n.offset + n.size;
            for (size_t b = start; b < end; b++)
                if (rng() % 4 == 0) d[b] = static_cast<uint8_t>(rng() & 0xFF);
            for (size_t b = start; b + 2 < end; b++)
                if (d[b] == 0 && d[b + 1] == 0 && d[b + 2] <= 3) d[b + 2] = 0x55;
            break;
        }
        SCOPED_TRACE("seed " + std::to_string(seed));
        const Fed got = feed_per_au(d, conceal);
        ASSERT_EQ(got.out.pocs.size(), 1u);
        EXPECT_EQ(got.flags[0], static_cast<uint32_t>(PICTURE_CORRUPT));
        EXPECT_EQ(got.stats.skipped_error, 0u);
        EXPECT_EQ(got.stats.concealed, 1u);
    }
}

// Parameter sets between the slices of a picture (§7.4.2.4.4; §7.4.3.3: a
// repeated PPS) neither split the picture nor change it: they apply from the
// next picture on.
TEST(PictureAssembly, ParameterSetsBetweenSlicesDoNotSplitThePicture) {
    for (const char* name : kMultiSlice) {
        const auto data = read_file(fixture(name));
        const Output clean = batch(data);
        NalParser parser;
        std::vector<uint8_t> vps, sps, pps;
        for (const auto& nal : parser.parse(data.data(), data.size())) {
            if (nal.header.nal_unit_type == NalUnitType::VPS_NUT) vps = nal_chunk(data, nal);
            if (nal.header.nal_unit_type == NalUnitType::SPS_NUT) sps = nal_chunk(data, nal);
            if (nal.header.nal_unit_type == NalUnitType::PPS_NUT) pps = nal_chunk(data, nal);
        }
        for (int variant = 0; variant < 2; variant++) {
            const auto mixed = rewrite(data, [&](size_t, const NalUnit& nal, const std::vector<uint8_t>& chunk,
                                                 std::vector<uint8_t>& out) {
                append(out, chunk);
                if (first_slice(nal)) {
                    if (variant == 1) { append(out, vps); append(out, sps); }
                    append(out, pps);
                }
            });
            SCOPED_TRACE(std::string(name) + (variant ? " VPS+SPS+PPS" : " PPS"));
            DecodeStats stats;
            const Output b = batch(mixed, &stats);
            EXPECT_EQ(b.pocs, clean.pocs);
            EXPECT_EQ(b.luma, clean.luma);
            EXPECT_EQ(stats.skipped_error, 0u);
            const Fed f = feed_per_au(mixed);
            EXPECT_EQ(f.out.pocs, clean.pocs);
            EXPECT_EQ(f.out.luma, clean.luma);
            EXPECT_TRUE(unique(f.tags));
        }
    }
}

// A PPS with new content arriving between the slices of picture k (a stream
// spliced mid-picture) applies from picture k+1: picture k is decoded whole
// with the PPS it started with.
TEST(PictureAssembly, AChangedPpsMidPictureAppliesFromTheNextPicture) {
    const auto data = read_file(fixture("multislice_qcif_8f.265"));
    const Output clean = batch(data);
    NalParser parser;
    const auto nals = parser.parse(data.data(), data.size());

    // The stream's PPS with pps_cb_qp_offset moved: the Cb samples change,
    // the parse does not
    std::vector<uint8_t> pps2;
    for (const auto& nal : nals) {
        if (nal.header.nal_unit_type != NalUnitType::PPS_NUT) continue;
        BitstreamReader bs(nal.rbsp.data(), nal.rbsp.size());
        bs.read_ue();       // pps_pic_parameter_set_id
        bs.read_ue();       // pps_seq_parameter_set_id
        bs.read_bits(7);    // dependent slices .. cabac_init_present_flag
        bs.read_ue();       // num_ref_idx_l0_default_active_minus1
        bs.read_ue();       // num_ref_idx_l1_default_active_minus1
        bs.read_se();       // init_qp_minus26
        bs.read_bits(2);    // constrained_intra_pred_flag, transform_skip_enabled_flag
        if (bs.read_flag()) bs.read_ue();  // cu_qp_delta_enabled_flag, diff_cu_qp_delta_depth
        const size_t at = bs.bits_read();
        const int32_t cb = bs.read_se();
        const size_t after = bs.bits_read();
        size_t stop = nal.rbsp.size() * 8 - 1;
        while (!get_bit(nal.rbsp, stop)) stop--;
        BitWriter w;
        w.copy_bits(nal.rbsp, 0, at);
        w.put_se(cb > 0 ? cb - 6 : cb + 6);
        w.copy_bits(nal.rbsp, after, stop);
        w.trailing();
        pps2 = make_nal(data[nal.offset], data[nal.offset + 1], w.take());
    }
    ASSERT_FALSE(pps2.empty());

    // POC of each picture in decoding order
    std::vector<int32_t> decode_order_poc;
    {
        Decoder dec;
        for (const auto& au : split_access_units(data)) {
            dec.feed(au.data(), au.size());
            const Picture* last = nullptr;
            for (const auto& p : dec.dpb().pictures())
                if (!last || p->decode_order > last->decode_order) last = p.get();
            decode_order_poc.push_back(last->poc);
            dec.drain();
        }
    }
    const size_t k = 5;
    int pic = -1;
    const auto spliced = rewrite(data, [&](size_t, const NalUnit& nal, const std::vector<uint8_t>& chunk,
                                           std::vector<uint8_t>& out) {
        append(out, chunk);
        if (first_slice(nal) && ++pic == static_cast<int>(k)) append(out, pps2);
    });
    for (bool per_au : {false, true}) {
        SCOPED_TRACE(per_au ? "per access unit" : "batch");
        const Output got = per_au ? feed_per_au(spliced).out : batch(spliced);
        ASSERT_EQ(got.pocs, clean.pocs);
        for (size_t i = 0; i < got.pocs.size(); i++) {
            const size_t order = static_cast<size_t>(
                std::find(decode_order_poc.begin(), decode_order_poc.end(), got.pocs[i]) -
                decode_order_poc.begin());
            EXPECT_EQ(got.luma[i], clean.luma[i]) << "picture " << order;
            if (order <= k)
                EXPECT_EQ(got.cb[i], clean.cb[i]) << "picture " << order << " saw the new PPS";
            else
                EXPECT_NE(got.cb[i], clean.cb[i]) << "picture " << order << " missed the new PPS";
        }
    }
}

// A non-first slice arriving two pictures late (reordered in transit): its
// picture was finished — and may already be output and released — without
// it. It is dropped, not decoded as a second picture with the same POC.
TEST(PictureAssembly, ALateSegmentOfAFinishedPictureIsDropped) {
    ErrorPolicy conceal;
    conceal.conceal_slice_errors = true;
    for (const char* name : kMultiSlice) {
        const auto data = read_file(fixture(name));
        const Output clean = batch(data);
        const auto aus = split_access_units(data);
        NalParser parser;
        for (size_t k = 1; k + 2 < aus.size(); k++) {
            // Move the second slice of picture k behind picture k + 2
            std::vector<std::vector<uint8_t>> moved = aus;
            std::vector<uint8_t> late, rest;
            size_t v = 0;
            for (const auto& nal : parser.parse(aus[k].data(), aus[k].size())) {
                auto c = nal_chunk(aus[k], nal);
                append(is_vcl(nal.header.nal_unit_type) && v++ == 1 ? late : rest, c);
            }
            ASSERT_FALSE(late.empty());
            moved[k] = rest;
            append(moved[k + 2], late);
            std::vector<uint8_t> stream;
            for (const auto& au : moved) append(stream, au);
            for (const ErrorPolicy& policy : {ErrorPolicy{}, conceal}) {
                SCOPED_TRACE(std::string(name) + " picture " + std::to_string(k) +
                             (policy.conceal_slice_errors ? " conceal" : " default"));
                const Fed got = feed_per_au(stream, policy);
                std::vector<double> pocs(got.out.pocs.begin(), got.out.pocs.end());
                EXPECT_TRUE(unique(pocs));
                EXPECT_TRUE(unique(got.tags));
                if (policy.conceal_slice_errors) {
                    EXPECT_EQ(got.out.pocs, clean.pocs);
                    EXPECT_EQ(got.stats.skipped_error, 0u);
                    EXPECT_EQ(got.stats.concealed, 1u);
                }
            }
        }
    }
}
