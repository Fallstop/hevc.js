#pragma once

// Shared helpers for the decoder unit tests: fixture I/O, Annex B NAL
// re-emission, an RBSP bit writer and emulation-prevention escaping.

#include <algorithm>
#include <cstdint>
#include <fstream>
#include <string>
#include <vector>

#include "bitstream/bitstream_reader.h"
#include "bitstream/nal_unit.h"
#include "decoding/decoder.h"

namespace hevc_test {

inline std::vector<uint8_t> read_file(const std::string& path) {
    std::ifstream f(path, std::ios::binary);
    if (!f) return {};
    return {std::istreambuf_iterator<char>(f), {}};
}

inline std::string fixture(const char* name) {
    return std::string(FIXTURES_DIR) + "/" + name;
}

// One NAL unit re-emitted as an Annex B chunk (4-byte start code + header +
// escaped payload exactly as it appears in `data`).
inline std::vector<uint8_t> nal_chunk(const std::vector<uint8_t>& data,
                                      const hevc::NalUnit& nal) {
    std::vector<uint8_t> chunk = {0, 0, 0, 1};
    chunk.insert(chunk.end(), data.begin() + static_cast<long>(nal.offset),
                 data.begin() + static_cast<long>(nal.offset + nal.size));
    return chunk;
}

// Splits a stream into access-unit chunks: each chunk holds the non-VCL NALs
// that precede a picture plus that picture's VCL NALs (§7.4.2.4.4, simplified
// to "a VCL NAL with first_slice_segment_in_pic_flag starts a new picture").
inline std::vector<std::vector<uint8_t>> split_access_units(const std::vector<uint8_t>& data) {
    hevc::NalParser parser;
    auto nals = parser.parse(data.data(), data.size());
    std::vector<std::vector<uint8_t>> aus;
    std::vector<uint8_t> cur;
    bool cur_has_vcl = false;
    for (const auto& nal : nals) {
        const bool vcl = hevc::is_vcl(nal.header.nal_unit_type);
        const bool first_slice = vcl && !nal.rbsp.empty() && (nal.rbsp[0] & 0x80);
        const bool starts_au = (vcl && first_slice) || (!vcl && cur_has_vcl &&
            nal.header.nal_unit_type != hevc::NalUnitType::SUFFIX_SEI);
        if (starts_au && cur_has_vcl) {
            aus.push_back(std::move(cur));
            cur.clear();
            cur_has_vcl = false;
        }
        auto chunk = nal_chunk(data, nal);
        cur.insert(cur.end(), chunk.begin(), chunk.end());
        if (vcl) cur_has_vcl = true;
    }
    if (!cur.empty()) aus.push_back(std::move(cur));
    return aus;
}

inline uint32_t get_bit(const std::vector<uint8_t>& d, size_t i) {
    return (d[i >> 3] >> (7 - (i & 7))) & 1u;
}

inline void set_bit(std::vector<uint8_t>& d, size_t i, uint32_t v) {
    const uint8_t mask = static_cast<uint8_t>(0x80u >> (i & 7));
    if (v) d[i >> 3] |= mask;
    else   d[i >> 3] = static_cast<uint8_t>(d[i >> 3] & ~mask);
}

// MSB-first bit writer; ue()/se() are Exp-Golomb (§9.2).
class BitWriter {
public:
    void put_bit(uint32_t b) {
        if (nbits_ % 8 == 0) bytes_.push_back(0);
        if (b & 1) bytes_.back() |= static_cast<uint8_t>(0x80u >> (nbits_ % 8));
        nbits_++;
    }
    void put_bits(uint64_t v, int n) {
        for (int i = n - 1; i >= 0; i--) put_bit(static_cast<uint32_t>(v >> i) & 1u);
    }
    void put_ue(uint32_t v) {
        uint64_t code = static_cast<uint64_t>(v) + 1;
        int n = 0;
        while (code >> (n + 1)) n++;
        put_bits(0, n);
        put_bits(code, n + 1);
    }
    void put_se(int32_t v) {
        put_ue(v > 0 ? static_cast<uint32_t>(2 * v - 1) : static_cast<uint32_t>(-2 * static_cast<int64_t>(v)));
    }
    void copy_bits(const std::vector<uint8_t>& src, size_t from, size_t to) {
        for (size_t i = from; i < to; i++) put_bit(get_bit(src, i));
    }
    // rbsp_trailing_bits()
    void trailing() {
        put_bit(1);
        while (nbits_ % 8) put_bit(0);
    }
    size_t bits() const { return nbits_; }
    std::vector<uint8_t> take() { nbits_ = 0; return std::move(bytes_); }

private:
    std::vector<uint8_t> bytes_;
    size_t nbits_ = 0;
};

// §7.3.1.1 — re-insert emulation prevention bytes.
inline std::vector<uint8_t> escape_rbsp(const std::vector<uint8_t>& rbsp) {
    std::vector<uint8_t> out;
    out.reserve(rbsp.size() + 8);
    int zeros = 0;
    for (uint8_t b : rbsp) {
        if (zeros >= 2 && b <= 3) {
            out.push_back(0x03);
            zeros = 0;
        }
        out.push_back(b);
        zeros = (b == 0) ? zeros + 1 : 0;
    }
    return out;
}

// Annex B NAL from a 2-byte header and an RBSP payload.
inline std::vector<uint8_t> make_nal(uint8_t h0, uint8_t h1, const std::vector<uint8_t>& rbsp) {
    std::vector<uint8_t> out = {0, 0, 0, 1, h0, h1};
    auto esc = escape_rbsp(rbsp);
    out.insert(out.end(), esc.begin(), esc.end());
    return out;
}

// Feed one access unit per call, draining after each — the live/transcoder
// path. Returns output pictures' POCs in output order (drained then flushed).
struct IncrementalOutcome {
    std::vector<int32_t> pocs;
    std::vector<int32_t> cvs;
    size_t max_dpb = 0;
};

inline IncrementalOutcome decode_per_au(hevc::Decoder& dec, const std::vector<uint8_t>& data) {
    IncrementalOutcome out;
    for (const auto& au : split_access_units(data)) {
        dec.feed(au.data(), au.size());
        for (const hevc::Picture* p : dec.drain()) {
            out.pocs.push_back(p->poc);
            out.cvs.push_back(p->cvs_id);
        }
        out.max_dpb = std::max(out.max_dpb, dec.dpb().pictures().size());
    }
    for (const hevc::Picture* p : dec.flush()) {
        out.pocs.push_back(p->poc);
        out.cvs.push_back(p->cvs_id);
    }
    return out;
}

}  // namespace hevc_test
