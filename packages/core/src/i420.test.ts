import { describe, it, expect } from "vitest";
import { packI420 } from "./i420.js";

/* eslint-disable @typescript-eslint/no-explicit-any */

// The per-sample narrowing every path must agree with.
function reference(f: any): Uint8Array {
  const shift = f.bitDepth > 8 ? f.bitDepth - 8 : 0;
  const sy = f.strideY ?? f.width;
  const sc = f.strideC ?? f.chromaWidth;
  const out: number[] = [];
  const plane = (p: any, w: number, h: number, s: number) => {
    for (let r = 0; r < h; r++) for (let c = 0; c < w; c++) out.push(Math.min(255, p[r * s + c] >> shift));
  };
  plane(f.y, f.width, f.height, sy);
  plane(f.cb, f.chromaWidth, f.chromaHeight, sc);
  plane(f.cr, f.chromaWidth, f.chromaHeight, sc);
  return Uint8Array.from(out);
}

function frame(opts: { w: number; h: number; stride?: number; bitDepth: number; bps: 1 | 2; seed: number }): any {
  const { w, h, bitDepth, bps } = opts;
  const stride = opts.stride ?? w;
  const cw = w >> 1, ch = h >> 1, cstride = stride >> 1;
  const Arr = bps === 1 ? Uint8Array : Uint16Array;
  let s = opts.seed;
  const rnd = () => { s = (s * 1103515245 + 12345) & 0x7fffffff; return s % (1 << bitDepth); };
  const fill = (n: number) => { const a = new Arr(n); for (let i = 0; i < n; i++) a[i] = rnd(); return a; };
  const f: any = {
    bytesPerSample: bps, y: fill(stride * h), cb: fill(cstride * ch), cr: fill(cstride * ch),
    width: w, height: h, chromaWidth: cw, chromaHeight: ch, bitDepth, poc: 0, tag: NaN, flags: 0,
  };
  if (opts.stride !== undefined) { f.strideY = stride; f.strideC = cstride; }
  return f;
}

describe("packI420", () => {
  it.each([
    ["8-bit packed (bulk copy)", { w: 16, h: 8, bitDepth: 8, bps: 1 as const, seed: 1 }],
    ["8-bit strided view (row copies)", { w: 12, h: 6, stride: 16, bitDepth: 8, bps: 1 as const, seed: 2 }],
    ["10-bit packed (narrowed)", { w: 16, h: 8, bitDepth: 10, bps: 2 as const, seed: 3 }],
    ["10-bit strided view", { w: 10, h: 4, stride: 14, bitDepth: 10, bps: 2 as const, seed: 4 }],
    ["8-bit in uint16 storage", { w: 8, h: 4, bitDepth: 8, bps: 2 as const, seed: 5 }],
  ])("%s matches per-sample narrowing", (_name, opts) => {
    const f = frame(opts);
    expect(Array.from(packI420(f))).toEqual(Array.from(reference(f)));
  });

  it("reuses a large enough scratch buffer", () => {
    const f = frame({ w: 8, h: 4, bitDepth: 8, bps: 1, seed: 9 });
    const scratch = new Uint8Array(1024);
    const out = packI420(f, scratch);
    expect(out.buffer).toBe(scratch.buffer);
    expect(out.length).toBe(8 * 4 + 2 * 4 * 2);
    expect(packI420(f, new Uint8Array(4)).length).toBe(48);
  });
});
