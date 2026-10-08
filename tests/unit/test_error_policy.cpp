// P3 — the forgiving error policy. With conceal_slice_errors a picture that
// loses slices is repaired (undecoded CTUs copied from the nearest picture,
// kept out of the in-loop filters), output with PICTURE_CORRUPT and kept as a
// reference — no freeze until the next IRAP. Missing references are
// substituted by the nearest picture. With decode_before_irap a stream joined
// mid-GOP decodes from its first picture, flagged PICTURE_PRE_RECOVERY until
// an IRAP or its recovery point.

#include <gtest/gtest.h>

#include <cstdlib>

#include "ps_builder.h"

using namespace hevc;
using namespace hevc_test;

namespace {

struct Frame {
    int32_t poc;
    uint32_t flags;
    std::vector<uint8_t> luma;
};

std::vector<Frame> decode_aus(const std::vector<std::vector<uint8_t>>& aus, ErrorPolicy policy,
                              DecodeStats* stats = nullptr) {
    Decoder dec;
    dec.set_error_policy(policy);
    std::vector<Frame> out;
    for (const auto& au : aus) {
        dec.feed(au.data(), au.size());
        for (const Picture* p : dec.drain()) out.push_back({p->poc, p->flags, p->plane_bytes[0]});
    }
    for (const Picture* p : dec.flush()) out.push_back({p->poc, p->flags, p->plane_bytes[0]});
    if (stats) *stats = dec.stats();
    return out;
}

ErrorPolicy conceal() { ErrorPolicy p; p.conceal_slice_errors = true; return p; }
ErrorPolicy before_irap() { ErrorPolicy p; p.decode_before_irap = true; return p; }
ErrorPolicy both() { ErrorPolicy p; p.conceal_slice_errors = true; p.decode_before_irap = true; return p; }

// Drop VCL NAL `slice` (0-based within the picture) of picture `pic`
// (decode order) from an access-unit list.
std::vector<std::vector<uint8_t>> drop_slice(const std::vector<std::vector<uint8_t>>& aus,
                                             size_t pic, int slice) {
    auto out = aus;
    NalParser parser;
    std::vector<uint8_t> au;
    int s = 0;
    for (const auto& nal : parser.parse(aus[pic].data(), aus[pic].size())) {
        if (is_vcl(nal.header.nal_unit_type) && s++ == slice) continue;
        auto c = nal_chunk(aus[pic], nal);
        au.insert(au.end(), c.begin(), c.end());
    }
    out[pic] = au;
    return out;
}

const Frame* find(const std::vector<Frame>& frames, int32_t poc) {
    for (const auto& f : frames) if (f.poc == poc) return &f;
    return nullptr;
}

uint64_t sad(const std::vector<uint8_t>& a, const std::vector<uint8_t>& b) {
    uint64_t s = 0;
    for (size_t i = 0; i < a.size() && i < b.size(); i++) s += static_cast<uint64_t>(std::abs(a[i] - b[i]));
    return s;
}

std::vector<int32_t> decode_order_pocs(const std::vector<std::vector<uint8_t>>& aus) {
    Decoder dec;
    std::vector<int32_t> pocs;
    for (const auto& au : aus) {
        const Picture* before = dec.dpb().current_pic();
        dec.feed(au.data(), au.size());
        const Picture* now = dec.dpb().current_pic();
        pocs.push_back(now && now != before ? now->poc : -1);
        dec.drain();
    }
    return pocs;
}

}  // namespace

// A lost middle slice: the default drops the picture and everything after it
// until the next IRAP; concealment outputs every picture, flags exactly the
// damaged one, and keeps decoding.
TEST(ErrorPolicy, ConcealedSliceLossDoesNotFreeze) {
    auto aus = split_access_units(read_file(fixture("multislice_qcif_8f.265")));
    ASSERT_EQ(aus.size(), 8u);
    auto pocs = decode_order_pocs(aus);
    auto lossy = drop_slice(aus, 3, 1);

    DecodeStats def_stats, con_stats;
    auto def = decode_aus(lossy, ErrorPolicy{}, &def_stats);
    auto con = decode_aus(lossy, conceal(), &con_stats);
    auto clean = decode_aus(aus, ErrorPolicy{});

    EXPECT_LT(def.size(), 8u);
    EXPECT_GE(def_stats.skipped_error, 1u);
    ASSERT_EQ(con.size(), 8u);
    EXPECT_EQ(con_stats.skipped_error, 0u);
    EXPECT_EQ(con_stats.concealed, 1u);
    EXPECT_GT(con.size(), def.size());
    for (const auto& f : con) {
        if (f.poc == pocs[3]) EXPECT_TRUE(f.flags & PICTURE_CORRUPT);
        else EXPECT_FALSE(f.flags & PICTURE_CORRUPT) << "POC " << f.poc;
    }
    // Pictures decoded before the loss are untouched
    for (size_t i = 0; i < 3; i++) {
        const Frame* a = find(con, pocs[i]);
        const Frame* b = find(clean, pocs[i]);
        ASSERT_TRUE(a && b);
        EXPECT_EQ(a->luma, b->luma);
    }
}

// The concealed CTUs are a co-located copy of another decoded picture — not
// grey, not garbage — and the decoded part of the picture is unchanged except
// for the filtering across the seam it no longer has.
TEST(ErrorPolicy, ConcealedAreaIsCopiedFromAnotherPicture) {
    auto aus = split_access_units(read_file(fixture("multislice_qcif_8f.265")));
    auto pocs = decode_order_pocs(aus);
    auto con = decode_aus(drop_slice(aus, 3, 1), conceal());
    const Frame* hurt = find(con, pocs[3]);
    ASSERT_TRUE(hurt);
    // 176x144 with 32x32 CTBs: 6 CTB columns, 5 CTB rows; x265 --slices 3
    // starts the slices at CTB 0, 6 and 18, so slice 1 covers CTB rows 1..2
    const int w = 176, y0 = 32, y1 = 96;
    bool matched = false;
    for (const auto& other : con) {
        if (other.poc == hurt->poc) continue;
        bool same = true;
        for (int y = y0; y < y1 && same; y++)
            for (int x = 0; x < w; x++)
                if (other.luma[y * w + x] != hurt->luma[y * w + x]) { same = false; break; }
        if (same) { matched = true; break; }
    }
    EXPECT_TRUE(matched) << "concealed rows are not a copy of any other picture";
}

// The first slice of a picture lost: the picture is rebuilt from its later
// slices (their headers carry the POC and RPS) instead of being dropped.
TEST(ErrorPolicy, LostFirstSliceIsConcealed) {
    auto aus = split_access_units(read_file(fixture("multislice_qcif_8f.265")));
    auto pocs = decode_order_pocs(aus);
    DecodeStats stats;
    auto con = decode_aus(drop_slice(aus, 4, 0), conceal(), &stats);
    ASSERT_EQ(con.size(), 8u);
    const Frame* f = find(con, pocs[4]);
    ASSERT_TRUE(f);
    EXPECT_TRUE(f->flags & PICTURE_CORRUPT);
    EXPECT_EQ(stats.skipped_error, 0u);
}

// A single-slice picture whose data is cut short (the slice throws mid-way):
// concealed and output, and the pictures after it still decode.
TEST(ErrorPolicy, TruncatedSliceIsConcealed) {
    auto aus = split_access_units(read_file(fixture("p_qcif_10f.265")));
    ASSERT_EQ(aus.size(), 10u);
    auto lossy = aus;
    lossy[3].resize(lossy[3].size() / 2);
    DecodeStats def_stats, con_stats;
    auto def = decode_aus(lossy, ErrorPolicy{}, &def_stats);
    auto con = decode_aus(lossy, conceal(), &con_stats);
    EXPECT_LT(def.size(), 10u);
    EXPECT_EQ(con.size(), 10u);
    EXPECT_EQ(con_stats.concealed, 1u);
    EXPECT_EQ(con_stats.skipped_error, 0u);
}

// A whole access unit lost: the next picture's missing reference is replaced
// by the nearest picture, which on camera-like content (static background,
// moving object) tracks the clean decode far better than the mid-grey
// prediction the default falls back to.
TEST(ErrorPolicy, MissingReferenceIsSubstitutedByNearestPicture) {
    auto aus = split_access_units(read_file(fixture("camera_qcif_12f.265")));
    auto pocs = decode_order_pocs(aus);
    auto lossy = aus;
    lossy.erase(lossy.begin() + 5);
    auto clean = decode_aus(aus, ErrorPolicy{});
    auto def = decode_aus(lossy, ErrorPolicy{});
    auto con = decode_aus(lossy, conceal());

    const int32_t next = pocs[6];
    const Frame* c = find(clean, next);
    const Frame* d = find(def, next);
    const Frame* s = find(con, next);
    ASSERT_TRUE(c && d && s);
    EXPECT_TRUE(d->flags & PICTURE_MISSING_REF);
    EXPECT_TRUE(s->flags & PICTURE_MISSING_REF);
    EXPECT_LT(sad(s->luma, c->luma), sad(d->luma, c->luma) / 4);
}

// Joined mid-GOP with no IRAP to come: the default outputs nothing; with
// decode_before_irap every picture comes out, flagged pre-recovery.
TEST(ErrorPolicy, DecodeBeforeIrapOutputsFlaggedPictures) {
    auto aus = split_access_units(read_file(fixture("p_qcif_10f.265")));
    NalParser parser;
    std::vector<uint8_t> params;
    for (const auto& nal : parser.parse(aus[0].data(), aus[0].size()))
        if (!is_vcl(nal.header.nal_unit_type)) { auto c = nal_chunk(aus[0], nal); params.insert(params.end(), c.begin(), c.end()); }
    std::vector<std::vector<uint8_t>> joined(aus.begin() + 3, aus.end());
    joined[0].insert(joined[0].begin(), params.begin(), params.end());

    DecodeStats def_stats;
    auto def = decode_aus(joined, ErrorPolicy{}, &def_stats);
    EXPECT_TRUE(def.empty());
    EXPECT_EQ(def_stats.skipped_wait_irap, 7u);

    auto early = decode_aus(joined, before_irap());
    ASSERT_EQ(early.size(), 7u);
    for (const auto& f : early) EXPECT_TRUE(f.flags & PICTURE_PRE_RECOVERY);
    EXPECT_TRUE(early.front().flags & PICTURE_MISSING_REF);

    auto both_out = decode_aus(joined, both());
    EXPECT_EQ(both_out.size(), 7u);
}

// A recovery point SEI ahead of the first picture ends the pre-recovery span
// at POC + recovery_poc_cnt (§D.3.8) — a gradual-decoding-refresh tune-in.
TEST(ErrorPolicy, RecoveryPointEndsPreRecovery) {
    auto aus = split_access_units(read_file(fixture("p_qcif_10f.265")));
    auto pocs = decode_order_pocs(aus);
    NalParser parser;
    std::vector<uint8_t> params;
    for (const auto& nal : parser.parse(aus[0].data(), aus[0].size()))
        if (!is_vcl(nal.header.nal_unit_type)) { auto c = nal_chunk(aus[0], nal); params.insert(params.end(), c.begin(), c.end()); }
    BitWriter sei;
    sei.put_bits(6, 8);  // recovery_point
    sei.put_bits(1, 8);  // payloadSize
    sei.put_se(3);       // recovery_poc_cnt = 3 -> se code 5 = 00110, then 2 flag bits
    sei.put_bit(0);
    sei.put_bit(0);
    while (sei.bits() % 8) sei.put_bit(0);
    sei.trailing();
    auto sei_nal = make_nal(nal_h0(NalUnitType::PREFIX_SEI), kNalH1, sei.take());

    std::vector<std::vector<uint8_t>> joined(aus.begin() + 3, aus.end());
    joined[0].insert(joined[0].begin(), sei_nal.begin(), sei_nal.end());
    joined[0].insert(joined[0].begin(), params.begin(), params.end());

    auto out = decode_aus(joined, before_irap());
    ASSERT_EQ(out.size(), 7u);
    const int32_t clean_from = pocs[3] + 3;
    for (const auto& f : out) {
        if (f.poc < clean_from) EXPECT_TRUE(f.flags & PICTURE_PRE_RECOVERY) << "POC " << f.poc;
        else EXPECT_FALSE(f.flags & PICTURE_PRE_RECOVERY) << "POC " << f.poc;
    }
}

// The policy is a no-op on clean streams: identical output, no flags.
TEST(ErrorPolicy, CleanStreamsAreUnaffected) {
    for (const char* name : {"full_qcif_10f.265", "multislice_wpp_qcif_8f.265", "opengop_qcif_12f.265"}) {
        auto aus = split_access_units(read_file(fixture(name)));
        auto a = decode_aus(aus, ErrorPolicy{});
        auto b = decode_aus(aus, both());
        ASSERT_EQ(a.size(), b.size()) << name;
        for (size_t i = 0; i < a.size(); i++) {
            EXPECT_EQ(a[i].poc, b[i].poc);
            EXPECT_EQ(a[i].luma, b[i].luma);
            EXPECT_EQ(b[i].flags, 0u);
        }
    }
}

namespace {

int bits_for(int x) {
    int b = 0;
    while ((1 << b) < x) b++;
    return b;
}

// The stream with the short-term RPS of every non-IDR first slice segment of
// picture `from` on (decoding order) replaced by an empty inline one: a P/B
// slice with NumPicTotalCurr == 0, which §7.4.7.1 forbids.
std::vector<uint8_t> empty_rps_from(const std::vector<uint8_t>& data, size_t from) {
    NalParser parser;
    ParameterSetManager ps;
    std::vector<uint8_t> out;
    size_t pic = 0;
    for (const auto& nal : parser.parse(data.data(), data.size())) {
        const auto chunk = nal_chunk(data, nal);
        const NalUnitType t = nal.header.nal_unit_type;
        if (!is_vcl(t)) {
            ps.process_nal(nal);
            out.insert(out.end(), chunk.begin(), chunk.end());
            continue;
        }
        const bool first = nal.rbsp[0] & 0x80;
        SliceHeader sh;
        const bool keep = !first || pic++ < from || is_idr(t) || !ps.parse_slice_header(sh, nal);
        if (keep) {
            out.insert(out.end(), chunk.begin(), chunk.end());
            continue;
        }
        const SPS& sps = *ps.active_sps();
        const PPS& pps = *ps.active_pps();
        const int num_sets = static_cast<int>(sps.num_short_term_ref_pic_sets);
        BitstreamReader bs(nal.rbsp.data(), nal.rbsp.size());
        bs.read_flag();                      // first_slice_segment_in_pic_flag
        if (is_irap(t)) bs.read_flag();      // no_output_of_prior_pics_flag
        bs.read_ue();                        // slice_pic_parameter_set_id
        for (int i = 0; i < pps.num_extra_slice_header_bits; i++) bs.read_flag();
        bs.read_ue();                        // slice_type
        if (pps.output_flag_present_flag) bs.read_flag();
        if (sps.separate_colour_plane_flag) bs.read_bits(2);
        bs.read_bits(static_cast<int>(sps.log2_max_pic_order_cnt_lsb_minus4) + 4);
        const size_t rps_start = bs.bits_read();
        if (bs.read_flag()) {
            if (num_sets > 1) bs.read_bits(bits_for(num_sets));
        } else {
            ShortTermRefPicSet st;
            EXPECT_TRUE(st.parse(bs, num_sets, num_sets, sps.st_ref_pic_sets));
        }
        const size_t rps_end = bs.bits_read();
        size_t stop = sh.slice_data_byte_offset * 8 - 1;
        while (!get_bit(nal.rbsp, stop)) stop--;  // byte_alignment()'s one bit

        BitWriter w;
        w.copy_bits(nal.rbsp, 0, rps_start);
        w.put_bit(0);                 // short_term_ref_pic_set_sps_flag
        if (num_sets > 0) w.put_bit(0);  // inter_ref_pic_set_prediction_flag
        w.put_ue(0);                  // num_negative_pics
        w.put_ue(0);                  // num_positive_pics
        w.copy_bits(nal.rbsp, rps_end, stop);
        w.trailing();
        auto rbsp = w.take();
        rbsp.insert(rbsp.end(), nal.rbsp.begin() + static_cast<long>(sh.slice_data_byte_offset), nal.rbsp.end());
        const auto rewritten = make_nal(data[nal.offset], data[nal.offset + 1], rbsp);
        out.insert(out.end(), rewritten.begin(), rewritten.end());
    }
    return out;
}

}  // namespace

// P slices whose RPS names nothing (NumPicTotalCurr == 0): every list entry
// is "no reference picture". Such pictures are flagged MISSING_REF under
// either policy; with concealment the previous picture stands in for the
// missing references (entry i taken to name POC - 1 - i, which is what this
// low-delay stream's RPS named) instead of mid-grey prediction.
TEST(ErrorPolicy, InterSliceWithNoReferencesIsFlaggedAndSubstituted) {
    const auto data = read_file(fixture("p_qcif_10f.265"));
    const auto broken = split_access_units(empty_rps_from(data, 5));
    const auto clean = decode_aus(split_access_units(data), ErrorPolicy{});
    const auto def = decode_aus(broken, ErrorPolicy{});
    const auto con = decode_aus(broken, conceal());
    ASSERT_EQ(clean.size(), 10u);
    ASSERT_EQ(def.size(), 10u);
    ASSERT_EQ(con.size(), 10u);
    for (size_t i = 0; i < 10; i++) {
        SCOPED_TRACE("POC " + std::to_string(clean[i].poc));
        const uint32_t want = i >= 5 ? PICTURE_MISSING_REF : 0u;
        EXPECT_EQ(def[i].flags, want);
        EXPECT_EQ(con[i].flags, want);
        if (i < 5) {
            EXPECT_EQ(def[i].luma, clean[i].luma);
            EXPECT_EQ(con[i].luma, clean[i].luma);
        }
    }
    // Predicted from the picture before it rather than from mid-grey
    EXPECT_LT(sad(con[5].luma, clean[5].luma), sad(def[5].luma, clean[5].luma));
}
