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
  HEAPU8: Uint8Array;
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
  // Object.create() bypasses the constructor's field initializer, so seed the
  // view-generation counter the same way the real constructor does.
  d._gen = 0;
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
      2044: 2,     // bytesPerSample (uint16 planes)
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
    expect(v.bytesPerSample).toBe(2);

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

  it("returns native Uint8Array views aliasing HEAPU8 for 8-bit content (bytesPerSample=1)", () => {
    // The production default for 8-bit HEVC: planes live in HEAPU8 with byte
    // base == sample base (no >>1). A single 2x2 luma frame with stride 4 and
    // 1x1 chroma with stride 2, laid out as bytes in HEAPU8.
    const heap = new Uint8Array(256);
    heap[0] = 10; heap[1] = 11;   // luma row 0 (cols 0,1); cols 2,3 are padding
    heap[4] = 12; heap[5] = 13;   // luma row 1 (base = 1 * strideY(4))
    heap[100] = 20;               // cb plane (byte base == cbPtr, no >>1)
    heap[200] = 30;               // cr plane (byte base == crPtr, no >>1)

    let mallocCall = 0;
    const ptrs = [1000, 2000];
    const _malloc = vi.fn(() => ptrs[mallocCall++]!);
    const _free = vi.fn();

    const values: Record<number, number> = {
      1000: 1,     // drain count
      2000: 0,     // yPtr  (byte base 0)
      2004: 100,   // cbPtr (byte base 100)
      2008: 200,   // crPtr (byte base 200)
      2012: 2,     // width
      2016: 2,     // height
      2020: 4,     // strideY
      2024: 2,     // strideC
      2028: 1,     // chromaWidth
      2032: 1,     // chromaHeight
      2036: 8,     // bitDepth
      2040: 42,    // poc
      2044: 1,     // bytesPerSample (uint8 planes — native 8-bit path)
    };
    const getValue = vi.fn((ptr: number) => values[ptr]!);
    const drain = vi.fn().mockReturnValue(0);
    const getDrainedFrame = vi.fn().mockReturnValue(0);

    const d = makeDecoder({
      module: { _malloc, _free, getValue, HEAPU8: heap },
      api: { drain, getDrainedFrame },
    });

    const views = d.drainViews();
    expect(views).toHaveLength(1);
    const v = views[0];

    // Element type is the native uint8 path, discriminated by bytesPerSample.
    expect(v.bytesPerSample).toBe(1);
    expect(v.y).toBeInstanceOf(Uint8Array);
    expect(v.cb).toBeInstanceOf(Uint8Array);
    expect(v.cr).toBeInstanceOf(Uint8Array);

    // Metadata, incl. strides preserved verbatim from the struct.
    expect(v.width).toBe(2);
    expect(v.height).toBe(2);
    expect(v.chromaWidth).toBe(1);
    expect(v.chromaHeight).toBe(1);
    expect(v.strideY).toBe(4);
    expect(v.strideC).toBe(2);
    expect(v.bitDepth).toBe(8);
    expect(v.poc).toBe(42);

    // Zero-copy: planes alias HEAPU8 (byte base == sample base, no offset shift).
    expect(v.y.buffer).toBe(heap.buffer);
    expect(v.y.byteOffset).toBe(0);
    expect(v.cb.byteOffset).toBe(100);
    expect(v.cr.byteOffset).toBe(200);
    // Span covers every readable row: (h-1)*strideY + w = 6 samples.
    expect(v.y.length).toBe(6);

    // Strided indexing reaches the correct samples in the HEAPU8 plane.
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

describe("HEVCFrameView lifetime guards", () => {
  /** Build a decoder whose drainViews() yields exactly one 2x2 uint16 view. */
  function decoderWithOneView(heap = new Uint16Array(256)): any {
    heap[0] = 10; heap[1] = 11;
    // drainViews() allocates countPtr (1000) then framePtr (2000) each call;
    // cycle so repeated calls reuse the same deterministic scratch pointers.
    let mallocCall = 0;
    const ptrs = [1000, 2000];
    const _malloc = vi.fn(() => ptrs[mallocCall++ % ptrs.length]!);
    const _free = vi.fn();
    const values: Record<number, number> = {
      1000: 1, 2000: 0, 2004: 100, 2008: 200, 2012: 2, 2016: 2,
      2020: 4, 2024: 2, 2028: 1, 2032: 1, 2036: 8, 2040: 42, 2044: 2,
    };
    const getValue = vi.fn((ptr: number) => values[ptr]!);
    const drain = vi.fn().mockReturnValue(0);
    const getDrainedFrame = vi.fn().mockReturnValue(0);
    return makeDecoder({
      module: { _malloc, _free, getValue, HEAPU16: heap },
      api: { drain, getDrainedFrame, feed: vi.fn().mockReturnValue(0), reset: vi.fn().mockReturnValue(0), destroy: vi.fn() },
    });
  }

  it("a fresh view reports valid and assertValid() does not throw", () => {
    const d = decoderWithOneView();
    const v = d.drainViews()[0];
    expect(v.isValid()).toBe(true);
    expect(() => v.assertValid()).not.toThrow();
  });

  it("a later decoder call (generation bump) invalidates the view", () => {
    const d = decoderWithOneView();
    const v = d.drainViews()[0];
    // Any further heap-touching call advances the generation.
    d.reset();
    expect(v.isValid()).toBe(false);
    expect(() => v.assertValid()).toThrow(/stale/);
  });

  it("a subsequent drainViews() invalidates views minted by the prior call", () => {
    // The most common misuse: holding views across the next drain. Each call
    // bumps the generation, so the earlier batch must report stale and throw,
    // while the fresh batch is valid.
    const d = decoderWithOneView();
    const first = d.drainViews()[0];
    expect(first.isValid()).toBe(true);

    const second = d.drainViews()[0];
    expect(first.isValid()).toBe(false);
    expect(() => first.assertValid()).toThrow(/stale/);
    // Reading planes after the stale call must surface the guard, not zeros.
    expect(() => first.assertValid()).toThrow(/Consume views synchronously/);
    expect(second.isValid()).toBe(true);
    expect(() => second.assertValid()).not.toThrow();
  });

  it("destroy() invalidates outstanding views", () => {
    const d = decoderWithOneView();
    const v = d.drainViews()[0];
    d.destroy();
    expect(v.isValid()).toBe(false);
    expect(() => v.assertValid()).toThrow(/stale/);
  });

  it("a detached heap ArrayBuffer (memory growth) invalidates the view", () => {
    const d = decoderWithOneView();
    const v = d.drainViews()[0];
    // Simulate ALLOW_MEMORY_GROWTH swapping the heap for a new buffer without
    // advancing the generation (e.g. growth triggered by an unrelated path).
    d._m.HEAPU16 = new Uint16Array(512);
    expect(v.isValid()).toBe(false);
    expect(() => v.assertValid()).toThrow(/detached/);
  });
});
