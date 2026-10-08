#!/usr/bin/env node
// Streaming WASM decode benchmark: feed one access unit per call and drain
// after each, as a live player or transcoder does (the batch benchmark,
// bench_wasm_stats.mjs, decodes the whole stream in one call and never
// releases a picture). Frames are drained but their planes are not copied.
//
// Usage: node tools/bench_wasm_stream.mjs <bitstream.265> <wasm_dir> [--iters N] [--warmup W] [--csv]
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
const iters = parseInt(getOpt('--iters', '10'), 10);
const warmup = parseInt(getOpt('--warmup', '2'), 10);
const csv = args.includes('--csv');

const wasmPath = resolve(projectDir, wasmDir);
const jsCode = readFileSync(resolve(wasmPath, 'hevc-decode.js'), 'utf8');
const script = new vm.Script(jsCode + '\nHEVCDecoderModule;', { filename: resolve(wasmPath, 'hevc-decode.js') });
const context = vm.createContext({
    ...globalThis,
    __filename: resolve(wasmPath, 'hevc-decode.js'),
    __dirname: wasmPath,
    require: (await import('module')).createRequire(resolve(wasmPath, 'hevc-decode.js')),
    process, console, URL, Buffer, WebAssembly,
    globalThis: { ...globalThis, process, WebAssembly },
    performance, setTimeout,
});
const Module = await script.runInContext(context)({ locateFile: (p) => resolve(wasmPath, p) });

// Access units: a VCL NAL with first_slice_segment_in_pic_flag (or a non-VCL
// NAL after VCL data, other than a suffix SEI) starts a new one.
const data = readFileSync(resolve(projectDir, file));
const starts = [];
for (let i = 0; i + 3 <= data.length; i++) {
    if (data[i] === 0 && data[i + 1] === 0 && data[i + 2] === 1) { starts.push(i > 0 && data[i - 1] === 0 ? i - 1 : i); i += 2; }
}
const aus = [];
let cur = 0, curVcl = false;
for (const s of starts) {
    const h = data[s + 2] === 1 ? s + 3 : s + 4;
    const t = (data[h] >> 1) & 63, vcl = t < 32, first = vcl && (data[h + 2] & 0x80);
    if (((vcl && first) || (!vcl && curVcl && t !== 40)) && curVcl) { aus.push([cur, s]); cur = s; curVcl = false; }
    if (vcl) curVcl = true;
}
aus.push([cur, data.length]);

const ptr = Module._malloc(data.length);
Module.HEAPU8.set(data, ptr);
const countPtr = Module._malloc(4);
const tagged = typeof Module._hevc_decoder_feed_tagged === 'function';

function decodeOnce() {
    const dec = Module._hevc_decoder_create();
    let frames = 0;
    const t0 = performance.now();
    for (let i = 0; i < aus.length; i++) {
        const [a, b] = aus[i];
        if (tagged) Module._hevc_decoder_feed_tagged(dec, ptr + a, b - a, i);
        else Module._hevc_decoder_feed(dec, ptr + a, b - a);
        Module._hevc_decoder_drain(dec, countPtr);
        frames += Module.getValue(countPtr, 'i32');
    }
    Module._hevc_decoder_flush(dec);
    const t1 = performance.now();
    Module._hevc_decoder_destroy(dec);
    return { ms: t1 - t0, frames };
}

let frames = 0;
for (let i = 0; i < warmup; i++) frames = decodeOnce().frames;
const samples = [];
for (let i = 0; i < iters; i++) { const r = decodeOnce(); frames = r.frames; samples.push(r.ms); }
const sorted = [...samples].sort((a, b) => a - b);
const median = sorted[Math.floor(sorted.length / 2)];
const fps = (ms) => (frames / ms) * 1000;
if (csv) console.log(`${file},${aus.length},${iters},${sorted[0].toFixed(3)},${median.toFixed(3)},${fps(median).toFixed(3)}`);
else console.log(`stream ${file} [${wasmDir}] AUs=${aus.length} frames(drained)=${frames} min ${sorted[0].toFixed(1)} ms median ${median.toFixed(1)} ms (${fps(median).toFixed(1)} fps)`);
