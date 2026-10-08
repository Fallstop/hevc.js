// Fuzz smoke: decode thousands of randomly mutated fixtures under every error
// policy and feeding mode, in one process. Built with the Debug (ASan)
// configuration, any memory error aborts with a report; hangs abort through a
// watchdog; contract violations (feed() error, unbounded DPB, invented tags)
// abort with the reproducing seed. Exit 0 means every input was contained.
//
//   hevc_fuzz_smoke [--iterations N] [--seed S] [--fixtures DIR] [--quiet]
//
// Reproduce a reported case with --seed <case seed> --iterations 1.

#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <string>
#include <thread>
#include <vector>

#include "decoding/decoder.h"
#include "mutate.h"

using namespace hevc;

namespace {

const char* kFixtures[] = {
    "p_qcif_10f.265", "b_qcif_10f.265", "full_qcif_10f.265", "full_qcif_10f_10bit.265",
    "multislice_qcif_8f.265", "multislice_wpp_qcif_8f.265", "camera_qcif_12f.265",
    "opengop_qcif_12f.265", "opengop_qcif_cra_first.265", "opengop_cra_rasl.265",
    "sei_recovery_point.265", "i_p_qcif_422_4f.265", "i_p_qcif_444_4f.265",
    "i_p_qcif_444_10b_4f.265", "mono_qcif_4f.265", "i_64x64_scaling_list.265",
    "i_64x64_full.265", "toy_qp30.265", "i_p_qcif_tskip_4f.265",
};

std::vector<uint8_t> read_file(const std::string& path) {
    std::ifstream f(path, std::ios::binary);
    return {std::istreambuf_iterator<char>(f), {}};
}

[[noreturn]] void fail(const char* what, uint32_t seed, const std::string& desc) {
    std::fprintf(stderr, "FUZZ FAILURE: %s (seed %u: %s)\n", what, seed, desc.c_str());
    std::fflush(stderr);
    std::abort();
}

struct Totals {
    uint64_t frames = 0, concealed = 0, skipped_error = 0, nal_errors = 0, bytes = 0;
};

void run_case(const std::vector<uint8_t>& data, uint32_t seed, const std::string& desc,
              int policy_bits, int feed_mode, Totals& t) {
    Decoder dec;
    ErrorPolicy policy;
    policy.conceal_slice_errors = policy_bits & 1;
    policy.decode_before_irap = policy_bits & 2;
    dec.set_error_policy(policy);

    std::vector<std::vector<uint8_t>> chunks;
    if (feed_mode == 0) {
        chunks.push_back(data);
    } else if (feed_mode == 1) {
        chunks = hevc_fuzz::split_chunks(data);
    } else {
        for (const auto& n : hevc_fuzz::find_nals(data))
            chunks.emplace_back(data.begin() + static_cast<long>(n.start_code),
                                data.begin() + static_cast<long>(n.end));
    }

    auto check = [&](const std::vector<Picture*>& pics, size_t fed) {
        for (const Picture* p : pics) {
            t.frames++;
            if (p->pic_width_in_luma <= 0 || p->pic_height_in_luma <= 0) fail("bad geometry", seed, desc);
            if (feed_mode != 0) {
                if (std::isnan(p->tag) || p->tag < 0 || p->tag >= static_cast<double>(fed))
                    fail("tag not fed", seed, desc);
            }
        }
    };
    for (size_t i = 0; i < chunks.size(); i++) {
        if (chunks[i].empty()) continue;
        if (dec.feed(chunks[i].data(), chunks[i].size(), static_cast<double>(i)) != DecodeStatus::OK)
            fail("feed() returned an error", seed, desc);
        check(dec.drain(), i + 1);
        if (dec.dpb().pictures().size() > static_cast<size_t>(DPB::kMaxDpbPictures + 2) && feed_mode != 0)
            fail("DPB grew past its bound", seed, desc);
        if (i % 7 == 3 && (seed & 1)) dec.discontinuity();
    }
    check(dec.flush(), chunks.size());
    const DecodeStats s = dec.stats();
    t.concealed += s.concealed;
    t.skipped_error += s.skipped_error;
    t.nal_errors += s.nal_errors;
    t.bytes += data.size();
}

}  // namespace

int main(int argc, char** argv) {
    uint32_t iterations = 2000, seed0 = 1;
    std::string dir = FIXTURES_DIR;
    bool quiet = false;
    for (int i = 1; i < argc; i++) {
        if (!std::strcmp(argv[i], "--iterations") && i + 1 < argc) iterations = static_cast<uint32_t>(std::atoi(argv[++i]));
        else if (!std::strcmp(argv[i], "--seed") && i + 1 < argc) seed0 = static_cast<uint32_t>(std::atoll(argv[++i]));
        else if (!std::strcmp(argv[i], "--fixtures") && i + 1 < argc) dir = argv[++i];
        else if (!std::strcmp(argv[i], "--quiet")) quiet = true;
    }

    std::vector<std::vector<uint8_t>> fixtures;
    for (const char* name : kFixtures) {
        auto d = read_file(dir + "/" + name);
        if (d.empty()) { std::fprintf(stderr, "missing fixture %s\n", name); return 2; }
        fixtures.push_back(std::move(d));
    }

    // Watchdog: a single case running for over a minute is a hang.
    std::atomic<uint32_t> current{0};
    std::atomic<int64_t> started{0};
    std::atomic<bool> done{false};
    std::thread watchdog([&] {
        while (!done.load()) {
            std::this_thread::sleep_for(std::chrono::seconds(1));
            const int64_t now = std::chrono::steady_clock::now().time_since_epoch().count();
            const int64_t begin = started.load();
            if (begin && now - begin > std::chrono::nanoseconds(std::chrono::seconds(60)).count()) {
                std::fprintf(stderr, "FUZZ FAILURE: hang (seed %u)\n", current.load());
                std::abort();
            }
        }
    });

    Totals t;
    const auto t0 = std::chrono::steady_clock::now();
    for (uint32_t it = 0; it < iterations; it++) {
        const uint32_t seed = seed0 + it;
        std::mt19937 rng(seed);
        auto data = fixtures[seed % fixtures.size()];
        const std::string desc = std::string(kFixtures[seed % fixtures.size()]) + " " +
                                 hevc_fuzz::mutate(data, rng, 4);
        current = seed;
        started = std::chrono::steady_clock::now().time_since_epoch().count();
        run_case(data, seed, desc, static_cast<int>((seed / 3) % 4), static_cast<int>(seed % 3), t);
        started = 0;
        if (!quiet && (it + 1) % 250 == 0)
            std::fprintf(stderr, "  %u / %u cases\n", it + 1, iterations);
    }
    done = true;
    watchdog.join();
    const double secs = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
    std::printf("fuzz smoke: %u mutated inputs (seeds %u..%u), 0 failures; %llu frames out, "
                "%llu concealed, %llu pictures skipped for errors, %llu NAL errors contained, "
                "%.1f MB decoded in %.1fs\n",
                iterations, seed0, seed0 + iterations - 1,
                static_cast<unsigned long long>(t.frames), static_cast<unsigned long long>(t.concealed),
                static_cast<unsigned long long>(t.skipped_error), static_cast<unsigned long long>(t.nal_errors),
                static_cast<double>(t.bytes) / 1e6, secs);
    return 0;
}
