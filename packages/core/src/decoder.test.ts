import { describe, it, expect, vi } from "vitest";
import { HEVCDecoder } from "./decoder.js";

/**
 * Unit tests for the JS wrapper's reset() forwarding and the zero-copy
 * drainViews() path. WASM is not available under vitest, so we build the
 * decoder via Object.create() and inject a fake Emscripten module + API,
 * mirroring the "poke privates" pattern used elsewhere in the suite.
 */
/* eslint-disable @typescript-eslint/no-explicit-any */

interface FakeModule {
  _malloc: (size: number) => number;
  _free: (ptr: number) => void;
  getValue: (ptr: number, type: string) => number;
  HEAPU16: Uint16Array;
}

function makeDecoder(opts: {
  module: Partial<FakeModule>;
  api: Record<string, any>;
  dec?: number;
}): any {
  const d = Object.create(HEVCDecoder.prototype);
  d._m = opts.module;
  d._api = opts.api;
  d._dec = opts.dec ?? 7;
  return d;
}

describe("HEVCDecoder.reset", () => {
  it("forwards to hevc_decoder_reset with clearParameterSets=1 by default", () => {
    const reset = vi.fn().mockReturnValue(0);
    const d = makeDecoder({ module: {}, api: { reset }, dec: 7 });
    d.reset();
    expect(reset).toHaveBeenCalledWith(7, 1);
  });

  it("passes clearParameterSets=0 when called with false", () => {
    const reset = vi.fn().mockReturnValue(0);
    const d = makeDecoder({ module: {}, api: { reset }, dec: 7 });
    d.reset(false);
    expect(reset).toHaveBeenCalledWith(7, 0);
  });

  it("throws when the native reset returns an error code", () => {
    const reset = vi.fn().mockReturnValue(-1);
    const d = makeDecoder({ module: {}, api: { reset }, dec: 7 });
    expect(() => d.reset()).toThrow(/Reset failed/);
  });

  it("is a no-op once the decoder has been destroyed (dec == 0)", () => {
    const reset = vi.fn().mockReturnValue(0);
    const d = makeDecoder({ module: {}, api: { reset }, dec: 0 });
    expect(() => d.reset()).not.toThrow();
    expect(reset).not.toHaveBeenCalled();
  });
});

describe("HEVCDecoder.drainViews", () => {
  it("returns strided zero-copy sub-array views aliasing the WASM heap", () => {
    // A single 2x2 luma frame with a padded stride of 4, and 1x1 chroma
    // with a padded stride of 2. Lay the visible samples out in the heap.
    const heap = new Uint16Array(256);
    heap[0] = 10; heap[1] = 11;   // luma row 0 (cols 0,1); cols 2,3 are padding
    heap[4] = 12; heap[5] = 13;   // luma row 1 (base = 1 * strideY(4))
    heap[50] = 20;                // cb plane (base = cbPtr 100 >> 1)
    heap[100] = 30;               // cr plane (base = crPtr 200 >> 1)

    // Deterministic malloc: countPtr=1000, framePtr=2000.
    let mallocCall = 0;
    const ptrs = [1000, 2000];
    const _malloc = vi.fn(() => ptrs[mallocCall++]!);
    const _free = vi.fn();

    // Struct field reads, keyed by absolute pointer.
    const values: Record<number, number> = {
      1000: 1,     // drain count
      2000: 0,     // yPtr  (>>1 -> sample 0)
      2004: 100,   // cbPtr (>>1 -> sample 50)
      2008: 200,   // crPtr (>>1 -> sample 100)
      2012: 2,     // width
      2016: 2,     // height
      2020: 4,     // strideY
      2024: 2,     // strideC
      2028: 1,     // chromaWidth
      2032: 1,     // chromaHeight
      2036: 8,     // bitDepth
      2040: 42,    // poc
    };
    const getValue = vi.fn((ptr: number) => values[ptr]!);

    const drain = vi.fn().mockReturnValue(0);
    const getDrainedFrame = vi.fn().mockReturnValue(0);

    const d = makeDecoder({
      module: { _malloc, _free, getValue, HEAPU16: heap },
      api: { drain, getDrainedFrame },
    });

    const views = d.drainViews();
    expect(views).toHaveLength(1);
    const v = views[0];

    // Metadata
    expect(v.width).toBe(2);
    expect(v.height).toBe(2);
    expect(v.chromaWidth).toBe(1);
    expect(v.chromaHeight).toBe(1);
    expect(v.strideY).toBe(4);
    expect(v.strideC).toBe(2);
    expect(v.bitDepth).toBe(8);
    expect(v.poc).toBe(42);

    // Zero-copy: the plane aliases the heap buffer (no copy was made).
    expect(v.y.buffer).toBe(heap.buffer);
    // Span covers every readable row: (h-1)*strideY + w = 6 samples.
    expect(v.y.length).toBe(6);

    // Strided indexing reaches the correct samples.
    expect(v.y[0 * v.strideY + 0]).toBe(10);
    expect(v.y[0 * v.strideY + 1]).toBe(11);
    expect(v.y[1 * v.strideY + 0]).toBe(12);
    expect(v.y[1 * v.strideY + 1]).toBe(13);
    expect(v.cb[0]).toBe(20);
    expect(v.cr[0]).toBe(30);

    // Mutating the heap is visible through the view — proves it is not a copy.
    heap[0] = 99;
    expect(v.y[0]).toBe(99);

    // The 48-byte struct scratch and the count scratch are both freed.
    expect(_free).toHaveBeenCalledWith(2000);
    expect(_free).toHaveBeenCalledWith(1000);
  });

  it("returns an empty array when drain reports an error", () => {
    const _malloc = vi.fn(() => 1000);
    const _free = vi.fn();
    const getValue = vi.fn(() => 0);
    const drain = vi.fn().mockReturnValue(-1);
    const d = makeDecoder({
      module: { _malloc, _free, getValue, HEAPU16: new Uint16Array(8) },
      api: { drain },
    });
    expect(d.drainViews()).toEqual([]);
  });
});
