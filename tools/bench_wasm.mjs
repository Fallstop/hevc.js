#!/usr/bin/env node
// Benchmark WASM decoder performance via Node.js
//
// Usage:
//   node tools/bench_wasm.mjs <bitstream.265> [wasm_dir] [--runs=N] [--json]
//   node tools/bench_wasm.mjs <bitstream.265> --compare=<dirA>,<dirB> [--runs=N]
// Default wasm_dir: build-wasm (falls back to packages/core/wasm)
//
// Methodology: 1 warmup run (JIT/tier-up), then N timed runs (default 7).
// Reports median (the stable headline number), best, and spread.
//
// --compare runs A and B interleaved (A,B,A,B,...) so machine load and
// thermal drift hit both builds equally — like hyperfine, the per-pair
// ratio stays meaningful even when absolute times swing.

import { existsSync, readFileSync } from 'fs';
import { fileURLToPath } from 'url';
import { dirname, resolve } from 'path';
import { createRequire } from 'module';

const __dirname = dirname(fileURLToPath(import.meta.url));
const projectDir = resolve(__dirname, '..');

const args = process.argv.slice(2);
const flags = args.filter((a) => a.startsWith('--'));
const positional = args.filter((a) => !a.startsWith('--'));

const file = positional[0];
if (!file) {
    console.error('Usage: node tools/bench_wasm.mjs <bitstream.265> [wasm_dir] [--runs=N] [--json]');
    process.exit(1);
}
const runs = parseInt(flags.find((f) => f.startsWith('--runs='))?.split('=')[1] ?? '7', 10);
const asJson = flags.includes('--json');
const compare = flags.find((f) => f.startsWith('--compare='))?.split('=')[1]?.split(',');

const data = readFileSync(file);

// The Emscripten glue is UMD-style (module.exports glue) but the repo is
// "type": "module", so plain require/import won't load it. Evaluate it in the
// real global scope with CJS-shaped locals — unlike a vm context, this keeps
// typed arrays and WebAssembly identical to the host realm.
async function loadBench(wasmDir) {
    const wasmPath = resolve(projectDir, wasmDir);
    const glueFile = resolve(wasmPath, 'hevc-decode.js');
    const jsCode = readFileSync(glueFile, 'utf8');
    const cjsModule = { exports: {} };
    new Function('module', 'exports', 'require', '__dirname', '__filename', jsCode)(
        cjsModule,
        cjsModule.exports,
        createRequire(glueFile),
        wasmPath,
        glueFile,
    );
    const Module = await cjsModule.exports({
        locateFile: (path) => resolve(wasmPath, path),
    });
    const ptr = Module._malloc(data.length);
    Module.HEAPU8.set(data, ptr);
    return function timedDecode() {
        const dec = Module._hevc_decoder_create();
        const c0 = process.cpuUsage();
        const t0 = performance.now();
        Module._hevc_decoder_decode(dec, ptr, data.length);
        const t1 = performance.now();
        const c1 = process.cpuUsage(c0);
        const frames = Module._hevc_decoder_get_frame_count(dec);
        Module._hevc_decoder_destroy(dec);
        // cpuMs (user CPU) is robust to preemption by other processes —
        // prefer it for A/B ratios on a loaded machine
        return { frames, ms: t1 - t0, cpuMs: c1.user / 1000 };
    };
}

const stats = (msList) => {
    const t = [...msList].sort((a, b) => a - b);
    return { median: t[Math.floor(t.length / 2)], best: t[0], worst: t[t.length - 1] };
};

if (compare) {
    if (compare.length !== 2) {
        console.error('--compare needs exactly two comma-separated wasm dirs');
        process.exit(1);
    }
    const [dirA, dirB] = compare;
    const benchA = await loadBench(dirA);
    const benchB = await loadBench(dirB);

    // Warmup both, then interleave: every run pair sees the same machine state.
    const wa = benchA(), wb = benchB();
    if (wa.frames === 0 || wb.frames === 0) {
        console.error('Decoder produced 0 frames — aborting.');
        process.exit(1);
    }
    const a = [], b = [], ratios = [], cpuRatios = [];
    for (let run = 0; run < runs; run++) {
        const ra = benchA(), rb = benchB();
        a.push(ra.cpuMs); b.push(rb.cpuMs);
        ratios.push(ra.ms / rb.ms);
        cpuRatios.push(ra.cpuMs / rb.cpuMs);
        console.log(
            `  Pair ${run + 1}: A ${ra.cpuMs.toFixed(0)}ms cpu (${ra.ms.toFixed(0)} wall)  ` +
            `B ${rb.cpuMs.toFixed(0)}ms cpu (${rb.ms.toFixed(0)} wall)  ` +
            `(cpu A/B ${(ra.cpuMs / rb.cpuMs).toFixed(3)})`,
        );
    }
    const sa = stats(a), sb = stats(b), sr = stats(cpuRatios), srw = stats(ratios);
    const frames = wa.frames;
    console.log(`\n  Input: ${file} (${frames} frames, ${runs} interleaved pairs)`);
    console.log(`  A: ${dirA}  median ${sa.median.toFixed(1)}ms cpu (${(frames / sa.median * 1000).toFixed(1)} fps)`);
    console.log(`  B: ${dirB}  median ${sb.median.toFixed(1)}ms cpu (${(frames / sb.median * 1000).toFixed(1)} fps)`);
    console.log(`  Median per-pair CPU ratio A/B: ${sr.median.toFixed(3)} ` +
        `(B is ${((sr.median - 1) * 100).toFixed(1)}% ${sr.median >= 1 ? 'faster' : 'slower'})`);
    console.log(`  Median per-pair wall ratio A/B: ${srw.median.toFixed(3)}`);
} else {
    let wasmDir = positional[1];
    if (!wasmDir) {
        wasmDir = existsSync(resolve(projectDir, 'build-wasm/hevc-decode.js'))
            ? 'build-wasm'
            : 'packages/core/wasm';
    }
    const bench = await loadBench(wasmDir);

    // Warmup (not counted): lets the engine tier-up the wasm code.
    const warmup = bench();
    if (warmup.frames === 0) {
        console.error('Decoder produced 0 frames — aborting.');
        process.exit(1);
    }

    const results = [];
    for (let run = 0; run < runs; run++) {
        const r = bench();
        results.push(r);
        if (!asJson) {
            const fps = (r.frames / r.ms) * 1000;
            console.log(
                `  Run ${run + 1}: ${r.frames} frames in ${r.ms.toFixed(1)}ms ` +
                `(${fps.toFixed(1)} fps, ${(r.ms / r.frames).toFixed(2)} ms/frame)`,
            );
        }
    }

    const { median, best, worst } = stats(results.map((r) => r.ms));
    const frames = results[0].frames;
    const fps = (ms) => (frames / ms) * 1000;
    const mbps = (ms) => data.length / 1e6 / (ms / 1000);

    if (asJson) {
        console.log(JSON.stringify({
            file, wasmDir, bytes: data.length, frames, runs,
            medianMs: median, bestMs: best, worstMs: worst,
            medianFps: fps(median), bestFps: fps(best), mbPerSec: mbps(median),
        }));
    } else {
        console.log(`\n  Input:  ${file} (${(data.length / 1e6).toFixed(2)} MB, ${frames} frames)`);
        console.log(`  WASM:   ${wasmDir}`);
        console.log(`  Median: ${fps(median).toFixed(1)} fps (${median.toFixed(1)}ms, ${mbps(median).toFixed(1)} MB/s)`);
        console.log(`  Best:   ${fps(best).toFixed(1)} fps (${best.toFixed(1)}ms)`);
        console.log(`  Spread: ${(((worst - best) / median) * 100).toFixed(1)}% (best→worst over ${runs} runs)`);
    }
}
