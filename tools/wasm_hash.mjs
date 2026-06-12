// WASM decode correctness probe: decode a bitstream and print a SHA-256 over all
// decoded frame planes (cropped, in display order). Used to prove a build-flag
// change (e.g. ASSERTIONS=0) is output-neutral by comparing the hash across builds.
//
// Usage: node tools/wasm_hash.mjs <bitstream.265> <wasm_dir>
import { readFileSync } from 'fs';
import { fileURLToPath } from 'url';
import { dirname, resolve } from 'path';
import { createHash } from 'crypto';
import vm from 'vm';

const __dirname = dirname(fileURLToPath(import.meta.url));
const projectDir = resolve(__dirname, '..');
const file = process.argv[2];
const wasmDir = process.argv[3] || 'build-wasm';
const wasmPath = resolve(projectDir, wasmDir);

const jsCode = readFileSync(resolve(wasmPath, 'hevc-decode.js'), 'utf8');
const script = new vm.Script(jsCode + '\nHEVCDecoderModule;', { filename: resolve(wasmPath, 'hevc-decode.js') });
const context = vm.createContext({
    ...globalThis,
    __filename: resolve(wasmPath, 'hevc-decode.js'),
    __dirname: wasmPath,
    require: (await import('module')).createRequire(resolve(wasmPath, 'hevc-decode.js')),
    process, console, URL, Buffer, WebAssembly,
});
const Module = await script.runInContext(context)({ locateFile: (p) => resolve(wasmPath, p) });

const data = readFileSync(file);
const ptr = Module._malloc(data.length);
Module.HEAPU8.set(data, ptr);
const dec = Module._hevc_decoder_create();
Module._hevc_decoder_decode(dec, ptr, data.length);
const frames = Module._hevc_decoder_get_frame_count(dec);

// HEVCFrame: y,cb,cr (3 ptrs) then width,height,stride_y,stride_c,chroma_width,
// chroma_height,bit_depth,poc (8 ints). 44 bytes in wasm32.
const fp = Module._malloc(44);
const I32 = (off) => Module.HEAP32[(fp + off) >> 2];
const hash = createHash('sha256');
for (let i = 0; i < frames; i++) {
    Module._hevc_decoder_get_frame(dec, i, fp);
    const y = I32(0), cb = I32(4), cr = I32(8);
    const w = I32(12), h = I32(16), sy = I32(20), sc = I32(24);
    const cw = I32(28), ch = I32(32);
    // Hash each plane row-by-row (respecting stride, only valid columns).
    for (const [base, pw, ph, st] of [[y, w, h, sy], [cb, cw, ch, sc], [cr, cw, ch, sc]]) {
        for (let row = 0; row < ph; row++) {
            const start = (base >> 1) + row * st;
            hash.update(Buffer.from(Module.HEAPU16.buffer, start * 2, pw * 2));
        }
    }
}
console.log(`frames=${frames} sha256=${hash.digest('hex')}`);
Module._hevc_decoder_destroy(dec);
Module._free(fp);
Module._free(ptr);
