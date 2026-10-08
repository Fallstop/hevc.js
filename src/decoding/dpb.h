#pragma once

// Decoded Picture Buffer — spec §8.3
// POC derivation (§8.3.1), RPS derivation (§8.3.2),
// Ref picture list construction (§8.3.4), ColPic derivation (§8.3.5)

#include <cstdint>
#include <vector>
#include <memory>

#include "common/picture.h"
#include "syntax/sps.h"
#include "syntax/slice_header.h"

namespace hevc {

struct PPS;

// Reference Picture List entry — pointer to a decoded picture in the DPB
struct RefPicListEntry {
    Picture* pic = nullptr;  // nullptr = "no reference picture"
};

// Decoded Picture Buffer
class DPB {
public:
    DPB() = default;

    // ============================================================
    // §8.3.1 — POC derivation
    // ============================================================
    // Returns PicOrderCntVal for the current picture.
    // Must be called once per picture, after slice header parsing.
    // no_rasl_output_flag is the NoRaslOutputFlag (§8.1) for this IRAP, computed
    // at decoder level (it depends on bitstream/EOS/reset position, not just the
    // NAL type — a mid-stream CRA has NoRaslOutputFlag == 0). Ignored for
    // non-IRAP pictures.
    int32_t derive_poc(const SliceHeader& sh, const SPS& sps,
                       NalUnitType nal_type, uint8_t nuh_temporal_id,
                       bool no_rasl_output_flag);

    // ============================================================
    // §8.3.2 — RPS derivation + picture marking
    // ============================================================
    // Derives the 5 RPS lists, marks pictures in the DPB.
    // Must be called once per picture, after POC derivation.
    // no_rasl_output_flag — see derive_poc(); drives the "mark all references
    // unused" reset that an IRAP with NoRaslOutputFlag == 1 performs.
    void derive_rps(const SliceHeader& sh, const SPS& sps,
                    NalUnitType nal_type, int32_t picOrderCntVal,
                    bool no_rasl_output_flag);

    // ============================================================
    // §8.3.4 — Reference picture list construction
    // ============================================================
    // Builds RefPicList0 and (for B slices) RefPicList1.
    // Must be called at the beginning of each P or B slice.
    void construct_ref_pic_lists(const SliceHeader& sh, const SPS& sps,
                                 const PPS& pps);

    // §8.3.4 — pure list builder (eq 8-8..8-11) shared by L0/L1, exposed for
    // unit testing the robustness guards (all-empty-RPS hang, out-of-range
    // list_entry clamp). rps_first/rps_second are the two short-term RPS sets
    // in the list's iteration order; rps_lt is the long-term set.
    static std::vector<Picture*> build_ref_pic_list(
        const std::vector<Picture*>& rps_first,
        const std::vector<Picture*>& rps_second,
        const std::vector<Picture*>& rps_lt,
        int num_ref_idx_active_minus1,
        bool modification_flag,
        const uint32_t* list_entry, size_t list_entry_count);

    // ============================================================
    // §8.3.5 — Collocated picture + NoBackwardPredFlag
    // ============================================================
    void derive_colpic(const SliceHeader& sh);

    // ============================================================
    // DPB management
    // ============================================================

    // Allocate a new picture in the DPB for decoding
    Picture* alloc_picture(int width, int height, ChromaFormat fmt,
                           int bd_luma, int bd_chroma);

    // Mark current picture as short-term reference after decoding (§8.1 step 4)
    void mark_current_as_short_term_ref();

    // Drop a partially-decoded current picture (error recovery). If a slice fails
    // to decode mid-picture, the half-filled Picture must not stay in the DPB:
    // it would be output as garbage and could be picked as a reference. Removes
    // current_pic_ from storage (freeing its pixel buffer) and clears the pointer.
    // Safe to call when there is no current picture (no-op).
    void drop_current();

    // Output and bumping process (§C.5 simplified — legacy batch mode)
    // Returns pictures that should be output (in POC order)
    std::vector<Picture*> get_output_pictures();

    // ============================================================
    // §C.5.2 — Output and bumping process
    // ============================================================
    //
    // Pictures are bumped (output) at the two points the spec defines — before
    // the current picture is stored (§C.5.2.2) and after it is decoded
    // (§C.5.2.3) — into an output queue. drain()/flush() hand the queue to the
    // caller. A bumped picture stays in storage (output_queued) until it has
    // been handed off, so the raw pointers returned by take_output() stay valid
    // until the next picture is allocated (the next feed).

    // §C.5.2.4 — "Bumping" process: output the picture with smallest POC
    // that is marked as "needed for output". Returns nullptr if no picture to bump.
    // The picture is appended to the output queue.
    Picture* bump();

    // Bumping limits of the active SPS (HighestTid = sps_max_sub_layers_minus1).
    struct OutputLimits {
        int max_num_reorder = 0;
        int max_dec_pic_buffering = 1;   // sps_max_dec_pic_buffering_minus1 + 1
        int max_latency_pictures = 0;    // SpsMaxLatencyPictures, 0 = no limit
    };
    static OutputLimits output_limits(const SPS& sps);

    // §C.5.2.2 — invoked once per picture after RPS marking and before the
    // current picture is allocated. irap_no_rasl: the current picture is an
    // IRAP with NoRaslOutputFlag = 1, which outputs every prior picture (prior
    // pictures are never discarded, whatever no_output_of_prior_pics_flag says:
    // a frame the caller fed must come back out). Also enforces the hard
    // kMaxDpbPictures storage bound against a corrupt SPS/RPS.
    void bump_before_decode(const OutputLimits& lim, bool irap_no_rasl);

    // §C.5.2.3 — invoked after the current picture is decoded and marked.
    void bump_after_decode(const OutputLimits& lim);

    // Bump every picture still marked "needed for output" (end of stream).
    void bump_all();

    // Hand off the output queue (display order). Clears the queue; the
    // pictures stay allocated until the next alloc_picture().
    std::vector<Picture*> take_output();

    // Pictures released by the output process since construction / reset().
    uint64_t pictures_bumped() const { return pictures_bumped_; }

    // Pictures bumped but not yet handed off, in output order.
    const std::vector<Picture*>& output_queue() const { return output_queue_; }

    // Remove pictures from DPB that are neither reference nor needed for output.
    // §C.5.2.4 step 3 + §C.5.2.2
    void evict_unused();

    // Hard bound on stored pictures (MaxDpbSize is 16, §A.4.2). A corrupt RPS
    // can keep more than that marked as reference; storage is capped anyway.
    static constexpr int kMaxDpbPictures = 16;

    // Reset all decode state to a freshly-constructed condition: drops every
    // stored picture (freeing pixel memory), clears the POC carry-over used by
    // §8.3.1 (prevTid0Pic) and the first-picture flag, and clears the cached
    // RPS / reference / collocated state. Reuse the same DPB to decode a new,
    // independent stream without re-allocating the object.
    void reset();

    // Get all stored pictures (for external access)
    const std::vector<std::shared_ptr<Picture>>& pictures() const { return pictures_; }

    // Access ref pic lists (valid after construct_ref_pic_lists). refIdx
    // defaults to -1 in several callers, so both bounds are checked.
    Picture* ref_pic_list0(int idx) const {
        return (idx >= 0 && idx < static_cast<int>(ref_pic_list0_.size()))
               ? ref_pic_list0_[idx].pic : nullptr;
    }
    Picture* ref_pic_list1(int idx) const {
        return (idx >= 0 && idx < static_cast<int>(ref_pic_list1_.size()))
               ? ref_pic_list1_[idx].pic : nullptr;
    }
    int num_ref_list0() const { return static_cast<int>(ref_pic_list0_.size()); }
    int num_ref_list1() const { return static_cast<int>(ref_pic_list1_.size()); }

    // Collocated picture (valid after derive_colpic)
    Picture* col_pic() const { return col_pic_; }
    bool no_backward_pred_flag() const { return no_backward_pred_flag_; }

    // Current picture being decoded
    Picture* current_pic() const { return current_pic_; }

    // Forget the current-picture pointer before a new picture's RPS is
    // derived: the RPS marking loops skip current_pic_, and the previous
    // picture is an ordinary DPB entry by then.
    void clear_current() { current_pic_ = nullptr; }

private:
    // DPB storage
    std::vector<std::shared_ptr<Picture>> pictures_;
    Picture* current_pic_ = nullptr;

    // Bumped pictures awaiting hand-off (§C.5.2), in output order
    std::vector<Picture*> output_queue_;
    uint64_t pictures_bumped_ = 0;

    // Pictures occupying DPB storage (§C.5.2.2): reference or awaiting output,
    // excluding the current picture and pictures already bumped.
    int count_occupied() const;
    int count_needed_for_output() const;

    // POC state (§8.3.1) — "prevTid0Pic" values
    int32_t prev_poc_lsb_ = 0;
    int32_t prev_poc_msb_ = 0;

    // RPS lists (§8.3.2) — pictures from the DPB
    // "StCurrBefore" = short-term, used by current, POC < current
    std::vector<Picture*> ref_pic_set_st_curr_before_;
    std::vector<Picture*> ref_pic_set_st_curr_after_;
    std::vector<Picture*> ref_pic_set_st_foll_;
    std::vector<Picture*> ref_pic_set_lt_curr_;
    std::vector<Picture*> ref_pic_set_lt_foll_;

    // Ref pic lists (§8.3.4)
    std::vector<RefPicListEntry> ref_pic_list0_;
    std::vector<RefPicListEntry> ref_pic_list1_;

    // Collocated picture (§8.3.5)
    Picture* col_pic_ = nullptr;
    bool no_backward_pred_flag_ = false;

    // Helper: find picture in DPB by POC
    Picture* find_short_term_by_poc(int32_t poc) const;
    Picture* find_by_poc(int32_t poc) const;
    Picture* find_by_poc_lsb(int32_t poc_lsb, int32_t max_poc_lsb) const;
};

} // namespace hevc
