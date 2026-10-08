#pragma once

// Deterministic bitstream mutations for the corruption corpus test and the
// fuzz smoke tool. Every mutation is a pure function of the input and a
// seeded std::mt19937, so a failing case is reproduced from (fixture, seed).

#include <cstdint>
#include <random>
#include <string>
#include <utility>
#include <vector>

namespace hevc_fuzz {

// Byte ranges of the NAL units of an Annex B stream: [payload_begin, end),
// payload_begin pointing at the 2-byte NAL header (after the start code).
struct NalSpan {
    size_t start_code;
    size_t payload_begin;
    size_t end;
};

inline std::vector<NalSpan> find_nals(const std::vector<uint8_t>& d) {
    std::vector<NalSpan> out;
    size_t i = 0;
    while (i + 3 <= d.size()) {
        if (d[i] == 0 && d[i + 1] == 0 && d[i + 2] == 1) {
            size_t sc = (i > 0 && d[i - 1] == 0) ? i - 1 : i;
            if (!out.empty()) out.back().end = sc;
            out.push_back({sc, i + 3, d.size()});
            i += 3;
        } else {
            i++;
        }
    }
    return out;
}

enum class Mutation {
    FlipBits,          // 1..16 random bit flips anywhere
    FlipHeaderBits,    // bit flips in the first bytes of one NAL (slice/PS headers)
    FlipParamSet,      // bit flips inside a VPS/SPS/PPS payload
    TruncateStream,    // cut the stream at a random byte
    TruncateNal,       // cut one NAL short
    DropNal,           // remove one NAL
    DuplicateNal,      // repeat one NAL
    SwapNals,          // exchange two NALs
    InsertGarbageNal,  // add a NAL of random bytes
    OverwriteRange,    // overwrite a byte range with 0x00 / 0xFF / random
    MangleNalHeader,   // random nal_unit_type / layer / tid / forbidden bit
    kCount
};

inline const char* mutation_name(Mutation m) {
    static const char* names[] = {"flip_bits", "flip_header_bits", "flip_param_set",
                                  "truncate_stream", "truncate_nal", "drop_nal",
                                  "duplicate_nal", "swap_nals", "insert_garbage_nal",
                                  "overwrite_range", "mangle_nal_header"};
    return names[static_cast<int>(m)];
}

inline void apply(Mutation m, std::vector<uint8_t>& d, std::mt19937& rng) {
    if (d.size() < 8) return;
    auto nals = find_nals(d);
    auto pick = [&](size_t n) { return std::uniform_int_distribution<size_t>(0, n - 1)(rng); };
    auto flip = [&](size_t lo, size_t hi, int n) {
        if (hi <= lo) return;
        for (int i = 0; i < n; i++) {
            size_t pos = lo + pick(hi - lo);
            d[pos] ^= static_cast<uint8_t>(1u << pick(8));
        }
    };
    switch (m) {
    case Mutation::FlipBits:
        flip(0, d.size(), 1 + static_cast<int>(pick(16)));
        break;
    case Mutation::FlipHeaderBits: {
        if (nals.empty()) break;
        const auto& n = nals[pick(nals.size())];
        flip(n.payload_begin + 2, std::min(n.end, n.payload_begin + 24), 1 + static_cast<int>(pick(4)));
        break;
    }
    case Mutation::FlipParamSet: {
        std::vector<NalSpan> ps;
        for (const auto& n : nals) {
            if (n.payload_begin >= d.size()) continue;
            int t = (d[n.payload_begin] >> 1) & 0x3F;
            if (t >= 32 && t <= 34) ps.push_back(n);
        }
        if (ps.empty()) break;
        const auto& n = ps[pick(ps.size())];
        flip(n.payload_begin + 2, n.end, 1 + static_cast<int>(pick(3)));
        break;
    }
    case Mutation::TruncateStream:
        d.resize(pick(d.size()) + 1);
        break;
    case Mutation::TruncateNal: {
        if (nals.empty()) break;
        const auto& n = nals[pick(nals.size())];
        if (n.end <= n.payload_begin + 3) break;
        size_t cut = n.payload_begin + 2 + pick(n.end - n.payload_begin - 2);
        d.erase(d.begin() + static_cast<long>(cut), d.begin() + static_cast<long>(n.end));
        break;
    }
    case Mutation::DropNal: {
        if (nals.empty()) break;
        const auto& n = nals[pick(nals.size())];
        d.erase(d.begin() + static_cast<long>(n.start_code), d.begin() + static_cast<long>(n.end));
        break;
    }
    case Mutation::DuplicateNal: {
        if (nals.empty()) break;
        const auto& n = nals[pick(nals.size())];
        std::vector<uint8_t> copy(d.begin() + static_cast<long>(n.start_code), d.begin() + static_cast<long>(n.end));
        d.insert(d.begin() + static_cast<long>(n.end), copy.begin(), copy.end());
        break;
    }
    case Mutation::SwapNals: {
        if (nals.size() < 2) break;
        size_t a = pick(nals.size()), b = pick(nals.size());
        if (a == b) break;
        if (a > b) std::swap(a, b);
        std::vector<uint8_t> out(d.begin(), d.begin() + static_cast<long>(nals[a].start_code));
        out.insert(out.end(), d.begin() + static_cast<long>(nals[b].start_code), d.begin() + static_cast<long>(nals[b].end));
        out.insert(out.end(), d.begin() + static_cast<long>(nals[a].end), d.begin() + static_cast<long>(nals[b].start_code));
        out.insert(out.end(), d.begin() + static_cast<long>(nals[a].start_code), d.begin() + static_cast<long>(nals[a].end));
        out.insert(out.end(), d.begin() + static_cast<long>(nals[b].end), d.end());
        d.swap(out);
        break;
    }
    case Mutation::InsertGarbageNal: {
        std::vector<uint8_t> g = {0, 0, 0, 1};
        size_t len = 2 + pick(64);
        for (size_t i = 0; i < len; i++) g.push_back(static_cast<uint8_t>(pick(256)));
        size_t at = nals.empty() ? 0 : nals[pick(nals.size())].start_code;
        d.insert(d.begin() + static_cast<long>(at), g.begin(), g.end());
        break;
    }
    case Mutation::OverwriteRange: {
        size_t at = pick(d.size());
        size_t len = std::min(d.size() - at, 1 + pick(48));
        int mode = static_cast<int>(pick(3));
        for (size_t i = 0; i < len; i++)
            d[at + i] = mode == 0 ? 0x00 : mode == 1 ? 0xFF : static_cast<uint8_t>(pick(256));
        break;
    }
    case Mutation::MangleNalHeader: {
        if (nals.empty()) break;
        const auto& n = nals[pick(nals.size())];
        if (n.payload_begin + 1 >= d.size()) break;
        d[n.payload_begin] = static_cast<uint8_t>(pick(256));
        if (pick(2)) d[n.payload_begin + 1] = static_cast<uint8_t>(pick(256));
        break;
    }
    default:
        break;
    }
}

// 1..max_ops random mutations
inline std::string mutate(std::vector<uint8_t>& d, std::mt19937& rng, int max_ops = 3) {
    std::string desc;
    int ops = 1 + std::uniform_int_distribution<int>(0, max_ops - 1)(rng);
    for (int i = 0; i < ops; i++) {
        auto m = static_cast<Mutation>(std::uniform_int_distribution<int>(
            0, static_cast<int>(Mutation::kCount) - 1)(rng));
        apply(m, d, rng);
        if (!desc.empty()) desc += "+";
        desc += mutation_name(m);
    }
    return desc;
}

// Split into chunks the way a live client feeds: one chunk per access unit
// (a VCL NAL with first_slice_segment_in_pic_flag, or a non-VCL NAL after a
// VCL NAL other than suffix SEI, starts a new chunk).
inline std::vector<std::vector<uint8_t>> split_chunks(const std::vector<uint8_t>& d) {
    auto nals = find_nals(d);
    std::vector<std::vector<uint8_t>> out;
    std::vector<uint8_t> cur;
    bool cur_vcl = false;
    if (!nals.empty() && nals[0].start_code > 0)
        cur.insert(cur.end(), d.begin(), d.begin() + static_cast<long>(nals[0].start_code));
    for (const auto& n : nals) {
        int t = n.payload_begin < d.size() ? (d[n.payload_begin] >> 1) & 0x3F : 63;
        bool vcl = t < 32;
        bool first = vcl && n.payload_begin + 2 < d.size() && (d[n.payload_begin + 2] & 0x80);
        bool starts = (vcl && first) || (!vcl && cur_vcl && t != 40);
        if (starts && cur_vcl) {
            out.push_back(std::move(cur));
            cur.clear();
            cur_vcl = false;
        }
        cur.insert(cur.end(), d.begin() + static_cast<long>(n.start_code), d.begin() + static_cast<long>(n.end));
        if (vcl) cur_vcl = true;
    }
    if (!cur.empty()) out.push_back(std::move(cur));
    return out;
}

}  // namespace hevc_fuzz
