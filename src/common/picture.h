#pragma once

#include <cstdint>
#include <vector>

#include "common/types.h"

namespace hevc {

// Picture buffer — planar YUV layout (AD-002)
// Spec ref: §6.1 (source, coded, decoded picture formats)
struct Picture {
    // Plane data — raw byte storage so 8-bit content can be held 1 byte/sample
    // (uint8) and >8-bit as 2 bytes/sample (uint16). Always access through the
    // typed plane_ptr<S>() / sample<S>() accessors; bytes_per_sample selects S.
    std::vector<uint8_t> plane_bytes[3];  // 0=Y, 1=Cb, 2=Cr (raw bytes)
    int width[3]  = {};               // width per plane (in samples)
    int height[3] = {};               // height per plane (in samples)
    int stride[3] = {};               // stride per plane (in samples)
    int bytes_per_sample = 2;         // 1 = uint8 (8-bit native), 2 = uint16

    // Picture properties
    int pic_width_in_luma = 0;
    int pic_height_in_luma = 0;
    int bit_depth_luma = 8;
    int bit_depth_chroma = 8;
    ChromaFormat chroma_format = ChromaFormat::YUV420;

    // Conformance window (in luma samples, spec §7.4.3.2.1)
    int conf_win_left = 0;
    int conf_win_right = 0;
    int conf_win_top = 0;
    int conf_win_bottom = 0;

    // Picture Order Count
    int32_t poc = 0;
    // Coded Video Sequence ID (incremented at each IRAP with NoRaslOutputFlag)
    int32_t cvs_id = 0;

    // Reference status
    bool used_for_short_term_ref = false;
    bool used_for_long_term_ref = false;
    bool needed_for_output = false;
    // Bumped (§C.5.2.4) but not yet handed to the caller by drain()/flush();
    // keeps the storage alive until the hand-off.
    bool output_queued = false;
    // §C.5.2.3 PicLatencyCount
    uint32_t pic_latency_count = 0;

    // Inter: per-PU motion info for TMVP (stored after decoding)
    struct PUMotionInfoCompact {
        int16_t mv_x[2] = {};
        int16_t mv_y[2] = {};
        int8_t ref_idx[2] = {-1, -1};
        bool pred_flag[2] = {};
    };
    std::vector<PUMotionInfoCompact> motion_info_buf;  // owned by this Picture
    int motion_info_stride = 0;
    // Convenience accessors
    PUMotionInfoCompact* motion_info_data() { return motion_info_buf.data(); }
    const PUMotionInfoCompact* motion_info_data() const { return motion_info_buf.data(); }

    // Reference picture lists of one slice, snapshot at decode time: TMVP
    // (§8.5.3.2.9) scales by the POC of the collocated block's reference *in
    // the slice containing it*, and needs to know whether it was long-term.
    struct RefPocTable {
        int32_t poc[2][16] = {};
        bool is_long_term[2][16] = {};
        uint8_t count[2] = {};
    };
    std::vector<RefPocTable> ref_tables;   // indexed by slice number
    std::vector<uint16_t> ctb_slice;       // CtbAddrRs -> slice number (empty: one table)
    int ctb_log2_size = 0;
    int width_in_ctbs = 0;

    // Ref table of the slice containing luma sample (x, y); nullptr if none.
    const RefPocTable* ref_table_at(int x, int y) const {
        if (ref_tables.empty()) return nullptr;
        if (ctb_slice.empty()) return &ref_tables[0];
        const size_t addr = static_cast<size_t>(y >> ctb_log2_size) * width_in_ctbs +
                            static_cast<size_t>(x >> ctb_log2_size);
        if (addr >= ctb_slice.size()) return nullptr;
        const uint16_t s = ctb_slice[addr];
        return s < ref_tables.size() ? &ref_tables[s] : nullptr;
    }

    // Allocate planes based on dimensions and chroma format
    void allocate(int width, int height, ChromaFormat fmt, int bd_luma, int bd_chroma);

    // Typed plane pointer (reinterprets the byte storage as S samples).
    template<class S> S* plane_ptr(int c) {
        return reinterpret_cast<S*>(plane_bytes[c].data());
    }
    template<class S> const S* plane_ptr(int c) const {
        return reinterpret_cast<const S*>(plane_bytes[c].data());
    }
    // Number of samples in plane c.
    size_t plane_samples(int c) const {
        return bytes_per_sample ? plane_bytes[c].size() / bytes_per_sample : 0;
    }

    // Get sample at position (x, y) in plane c.
    template<class S> S& sample(int c, int x, int y) {
        return plane_ptr<S>(c)[y * stride[c] + x];
    }
    template<class S> S sample(int c, int x, int y) const {
        return plane_ptr<S>(c)[y * stride[c] + x];
    }

    // Width-dispatched single-sample read for cold/scalar paths that must work
    // for both uint8 and uint16 storage (e.g. intra neighbour fetch).
    int sample_i(int c, int x, int y) const {
        size_t off = static_cast<size_t>(y) * stride[c] + x;
        return bytes_per_sample == 1 ? plane_ptr<uint8_t>(c)[off]
                                     : plane_ptr<uint16_t>(c)[off];
    }

    // Write to raw YUV file (crops to conformance window if set)
    bool write_yuv(const char* path) const;

    // Is this picture a reference?
    bool is_reference() const {
        return used_for_short_term_ref || used_for_long_term_ref;
    }
};

} // namespace hevc
