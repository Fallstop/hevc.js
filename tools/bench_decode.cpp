// bench_decode.cpp — decode-only microbenchmark for the native HEVC decoder.
//
// Unlike the CLI (src/main.cpp), this harness:
//   * never writes YUV (the CLI's per-pixel fwrite path dominates wall-clock),
//   * reads the bitstream once into RAM (no per-iteration file I/O),
//   * times ONLY hevc::Decoder::decode() — decoder construction (thread-pool
//     spin-up) is excluded from the timed region,
//   * runs warmup + N timed iterations and reports min/median/mean/p95/stddev.
//
// It doubles as a clean hyperfine target: with `--once` it does a single
// read+decode and prints one line, so `hyperfine './hevc-bench file --once'`
// measures decode wall-time without I/O noise.
//
// Usage:
//   hevc-bench <input.265> [--iters N] [--warmup W] [--once] [--csv]
//
// Build (against the already-built static lib):
//   g++ -O3 -DNDEBUG -std=c++17 -I src tools/bench_decode.cpp \
//       build-rel/libhevc-decode-lib.a -o build-rel/hevc-bench -pthread

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <chrono>
#include <fstream>
#include <vector>
#include <algorithm>
#include <cmath>

#include "decoding/decoder.h"

static std::vector<uint8_t> read_file(const char* path) {
    std::ifstream file(path, std::ios::binary | std::ios::ate);
    if (!file.is_open()) {
        fprintf(stderr, "Error: cannot open '%s'\n", path);
        exit(1);
    }
    auto size = file.tellg();
    file.seekg(0, std::ios::beg);
    std::vector<uint8_t> data(static_cast<size_t>(size));
    file.read(reinterpret_cast<char*>(data.data()), size);
    return data;
}

static double decode_once(const std::vector<uint8_t>& data, size_t* frames_out) {
    hevc::Decoder decoder;  // not timed: excludes thread-pool spin-up
    auto t0 = std::chrono::high_resolution_clock::now();
    auto status = decoder.decode(data.data(), data.size());
    auto t1 = std::chrono::high_resolution_clock::now();
    if (status != hevc::DecodeStatus::OK) {
        fprintf(stderr, "Error: decode failed\n");
        exit(1);
    }
    if (frames_out) *frames_out = decoder.output_pictures().size();
    return std::chrono::duration<double, std::milli>(t1 - t0).count();
}

int main(int argc, char* argv[]) {
    if (argc < 2) {
        fprintf(stderr, "Usage: %s <input.265> [--iters N] [--warmup W] [--once] [--csv]\n", argv[0]);
        return 1;
    }
    const char* input_path = nullptr;
    int iters = 15;
    int warmup = 3;
    bool once = false;
    bool csv = false;

    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--iters") == 0 && i + 1 < argc) iters = atoi(argv[++i]);
        else if (strcmp(argv[i], "--warmup") == 0 && i + 1 < argc) warmup = atoi(argv[++i]);
        else if (strcmp(argv[i], "--once") == 0) once = true;
        else if (strcmp(argv[i], "--csv") == 0) csv = true;
        else input_path = argv[i];
    }
    if (!input_path) { fprintf(stderr, "Error: no input file\n"); return 1; }

    auto data = read_file(input_path);

    if (once) {
        size_t frames = 0;
        double ms = decode_once(data, &frames);
        printf("%zu frames in %.2f ms (%.2f fps, %.3f ms/frame)\n",
               frames, ms, frames * 1000.0 / ms, ms / frames);
        return 0;
    }

    size_t frames = 0;
    for (int i = 0; i < warmup; i++) decode_once(data, &frames);

    std::vector<double> samples;
    samples.reserve(iters);
    for (int i = 0; i < iters; i++) samples.push_back(decode_once(data, &frames));

    std::vector<double> sorted = samples;
    std::sort(sorted.begin(), sorted.end());
    double mn = sorted.front();
    double mx = sorted.back();
    double median = sorted[sorted.size() / 2];
    double p95 = sorted[std::min(sorted.size() - 1, (size_t)std::llround(0.95 * (sorted.size() - 1)))];
    double sum = 0; for (double s : samples) sum += s;
    double mean = sum / samples.size();
    double var = 0; for (double s : samples) var += (s - mean) * (s - mean);
    double stddev = std::sqrt(var / samples.size());

    if (csv) {
        // file,frames,iters,min_ms,median_ms,mean_ms,p95_ms,max_ms,stddev_ms,median_fps,median_ms_per_frame
        printf("%s,%zu,%d,%.3f,%.3f,%.3f,%.3f,%.3f,%.3f,%.3f,%.4f\n",
               input_path, frames, iters, mn, median, mean, p95, mx, stddev,
               frames * 1000.0 / median, median / frames);
        return 0;
    }

    printf("\nbench_decode — %s\n", input_path);
    printf("  frames=%zu  warmup=%d  iters=%d  (decode-only, single fresh Decoder per iter)\n",
           frames, warmup, iters);
    printf("  min    %8.2f ms   %7.2f fps   %6.3f ms/frame\n", mn, frames * 1000.0 / mn, mn / frames);
    printf("  median %8.2f ms   %7.2f fps   %6.3f ms/frame\n", median, frames * 1000.0 / median, median / frames);
    printf("  mean   %8.2f ms   %7.2f fps   %6.3f ms/frame  (stddev %.2f ms, %.2f%%)\n",
           mean, frames * 1000.0 / mean, mean / frames, stddev, 100.0 * stddev / mean);
    printf("  p95    %8.2f ms\n", p95);
    printf("  max    %8.2f ms\n", mx);
    return 0;
}
