#include <gtest/gtest.h>

#include <array>
#include <cstdint>
#include <vector>

#include "decoding/dpb.h"

using namespace hevc;

// DPB::build_ref_pic_list only stores/copies Picture* values; it never
// dereferences them. Use distinct non-null sentinel pointers so we can assert
// on identity without allocating real pictures.
static Picture* SENT(uintptr_t n) { return reinterpret_cast<Picture*>(n); }

// ============================================================
// Defect (a): all-empty RPS must NOT hang.
// NumRpsCurrTempList = max(num_ref_idx+1, 0) >= 1, but the three inner loops
// never advance rIdx when every RPS set is empty. The guard must break out and
// produce a concealed (all-nullptr) list instead of spinning forever.
// ============================================================
TEST(RefLists, AllEmptyRpsDoesNotHang) {
    std::vector<Picture*> empty;
    std::array<uint32_t, 16> list_entry = {};

    // num_ref_idx_active_minus1 = 3 -> 4 requested entries, no RPS pictures.
    auto l = DPB::build_ref_pic_list(empty, empty, empty,
                                     /*num_ref_idx_active_minus1=*/3,
                                     /*modification_flag=*/false,
                                     list_entry.data(), list_entry.size());

    ASSERT_EQ(l.size(), 4u);  // exactly num_ref_idx+1 entries
    for (Picture* p : l) EXPECT_EQ(p, nullptr);  // all concealed
}

// Same hang scenario via the L1 ordering path (rps_first/second swapped) and
// with a single requested entry — still must terminate.
TEST(RefLists, AllEmptyRpsSingleEntryTerminates) {
    std::vector<Picture*> empty;
    std::array<uint32_t, 16> list_entry = {};

    auto l = DPB::build_ref_pic_list(empty, empty, empty,
                                     /*num_ref_idx_active_minus1=*/0,
                                     /*modification_flag=*/false,
                                     list_entry.data(), list_entry.size());

    ASSERT_EQ(l.size(), 1u);
    EXPECT_EQ(l[0], nullptr);
}

// ============================================================
// Defect (b): out-of-range list_entry must map to nullptr, not OOB read.
// ============================================================
TEST(RefLists, OutOfRangeListEntryConcealedToNull) {
    std::vector<Picture*> before = {SENT(0x10), SENT(0x20)};  // 2 short-term refs
    std::vector<Picture*> empty;
    std::array<uint32_t, 16> list_entry = {};
    list_entry[0] = 1;    // valid -> temp[1]
    list_entry[1] = 99;   // out of range -> nullptr (concealed)

    auto l = DPB::build_ref_pic_list(before, empty, empty,
                                     /*num_ref_idx_active_minus1=*/1,
                                     /*modification_flag=*/true,
                                     list_entry.data(), list_entry.size());

    ASSERT_EQ(l.size(), 2u);
    EXPECT_EQ(l[0], SENT(0x20));   // temp = {0x10, 0x20}; entry 1 -> 0x20
    EXPECT_EQ(l[1], nullptr);      // entry 99 out of range -> concealed
}

// list_entry index beyond the provided list_entry buffer also conceals.
TEST(RefLists, ListEntryBeyondBufferConcealedToNull) {
    std::vector<Picture*> before = {SENT(0x10), SENT(0x20)};
    std::vector<Picture*> empty;
    std::array<uint32_t, 16> list_entry = {};
    list_entry[0] = 0;

    // Request 3 entries but only 1 list_entry slot is meaningful; passing a
    // truncated count exercises the buffer-bounds branch.
    auto l = DPB::build_ref_pic_list(before, empty, empty,
                                     /*num_ref_idx_active_minus1=*/2,
                                     /*modification_flag=*/true,
                                     list_entry.data(), /*list_entry_count=*/1);

    ASSERT_EQ(l.size(), 3u);
    EXPECT_EQ(l[0], SENT(0x10));   // entry 0 -> temp[0]
    EXPECT_EQ(l[1], nullptr);      // index 1 >= count -> concealed
    EXPECT_EQ(l[2], nullptr);      // index 2 >= count -> concealed
}

// ============================================================
// Sanity: the spec-normal path (no modification, enough refs) is unchanged,
// including the eq 8-8 wrap-around when num_ref_idx exceeds NumPicTotalCurr.
// ============================================================
TEST(RefLists, NormalPathAndWrapAround) {
    std::vector<Picture*> before = {SENT(0xA)};
    std::vector<Picture*> after  = {SENT(0xB)};
    std::vector<Picture*> lt     = {SENT(0xC)};
    std::array<uint32_t, 16> list_entry = {};

    // 5 requested, 3 RPS pictures -> temp wraps: A,B,C,A,B
    auto l = DPB::build_ref_pic_list(before, after, lt,
                                     /*num_ref_idx_active_minus1=*/4,
                                     /*modification_flag=*/false,
                                     list_entry.data(), list_entry.size());

    ASSERT_EQ(l.size(), 5u);
    EXPECT_EQ(l[0], SENT(0xA));
    EXPECT_EQ(l[1], SENT(0xB));
    EXPECT_EQ(l[2], SENT(0xC));
    EXPECT_EQ(l[3], SENT(0xA));  // wrap-around
    EXPECT_EQ(l[4], SENT(0xB));
}
