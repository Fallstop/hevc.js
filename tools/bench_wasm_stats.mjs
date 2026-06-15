#!/usr/bin/env node
// Rigorous WASM decode benchmark — warmup + N timed iterations + percentiles.
// Mirrors tools/bench_decode.cpp (native) so native-vs-WASM is apples-to-apples.
//
// Usage: node tools/bench_wasm_stats.mjs <bitstream.265> <wasm_dir> [--iters N] [--warmup W] [--csv]
//   wasm_dir: dir containing hevc-decode.js + hevc-decode.wasm
//             (e.g. build-wasm, or packages/core/wasm for the shipped artifact)

import { readFileSync } from 'fs';
import { fileURLToPath } from 'url';
import { dirname, resolve } from 'path';
import vm from 'vm';

const __dirname = dirname(fileURLToPath(import.meta.url));
const projectDir = resolve(__dirname, '..');

const args = process.argv.slice(2);
const file = args[0];
const wasmDir = args[1] || 'build-wasm';
const getOpt = (name, def) => { const i = args.indexOf(name); return i >= 0 ? args[i + 1] : def; };
const iters = parseInt(getOpt('--iters', '15'), 10);
const warmup = parseInt(getOpt('--warmup', '3'), 10);
const csv = args.includes('--csv');

if (!file) {
    console.error('Usage: node tools/bench_wasm_stats.mjs <bitstream.265> <wasm_dir> [--iters N] [--warmup W] [--csv]');
    process.exit(1);
}

const wasmPath = resolve(projectDir, wasmDir);
const jsCode = readFileSync(resolve(wasmPath, 'hevc-decode.js'), 'utf8');
const script = new vm.Script(jsCode + '\nHEVCDecoderModule;', {
    filename: resolve(wasmPath, 'hevc-decode.js'),
});
const context = vm.createContext({
    ...globalThis,
    __filename: resolve(wasmPath, 'hevc-decode.js'),
    __dirname: wasmPath,
    require: (await import('module')).createRequire(resolve(wasmPath, 'hevc-decode.js')),
    process, console, URL, Buffer, WebAssembly,
    globalThis: { ...globalThis, process, WebAssembly },
    performance, setTimeout,
});
const HEVCDecoderModule = script.runInContext(context);
const Module = await HEVCDecoderModule({ locateFile: (p) => resolve(wasmPath, p) });

const data = readFileSync(resolve(projectDir, file));
const ptr = Module._malloc(data.length);
Module.HEAPU8.set(data, ptr);

function decodeOnce() {
    const dec = Module._hevc_decoder_create();
    const t0 = performance.now();
    Module._hevc_decoder_decode(dec, ptr, data.length);
    const t1 = performance.now();
    const frames = Module._hevc_decoder_get_frame_count(dec);
    Module._hevc_decoder_destroy(dec);
    return { ms: t1 - t0, frames };
}

let frames = 0;
for (let i = 0; i < warmup; i++) frames = decodeOnce().frames;

const samples = [];
for (let i = 0; i < iters; i++) { const r = decodeOnce(); frames = r.frames; samples.push(r.ms); }
Module._free(ptr);

const sorted = [...samples].sort((a, b) => a - b);
const mn = sorted[0], mx = sorted[sorted.length - 1];
const median = sorted[Math.floor(sorted.length / 2)];
const p95 = sorted[Math.min(sorted.length - 1, Math.round(0.95 * (sorted.length - 1)))];
const mean = samples.reduce((a, b) => a + b, 0) / samples.length;
const stddev = Math.sqrt(samples.reduce((a, b) => a + (b - mean) ** 2, 0) / samples.length);
const fps = (ms) => (frames / ms) * 1000;

if (csv) {
    console.log(`${file},${frames},${iters},${mn.toFixed(3)},${median.toFixed(3)},${mean.toFixed(3)},${p95.toFixed(3)},${mx.toFixed(3)},${stddev.toFixed(3)},${fps(median).toFixed(3)},${(median / frames).toFixed(4)}`);
} else {
    console.log(`\nbench_wasm — ${file}  [${wasmDir}]`);
    console.log(`  frames=${frames}  warmup=${warmup}  iters=${iters}  (single-threaded WASM)`);
    console.log(`  min    ${mn.toFixed(2).padStart(9)} ms   ${fps(mn).toFixed(2).padStart(7)} fps   ${(mn / frames).toFixed(3)} ms/frame`);
    console.log(`  median ${median.toFixed(2).padStart(9)} ms   ${fps(median).toFixed(2).padStart(7)} fps   ${(median / frames).toFixed(3)} ms/frame`);
    console.log(`  mean   ${mean.toFixed(2).padStart(9)} ms   ${fps(mean).toFixed(2).padStart(7)} fps   ${(mean / frames).toFixed(3)} ms/frame  (stddev ${stddev.toFixed(2)} ms, ${(100 * stddev / mean).toFixed(2)}%)`);
    console.log(`  p95    ${p95.toFixed(2).padStart(9)} ms`);
    console.log(`  max    ${mx.toFixed(2).padStart(9)} ms`);
}
