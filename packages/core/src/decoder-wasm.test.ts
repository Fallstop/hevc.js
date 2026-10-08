import { describe, it, expect, beforeAll } from "vitest";
import { HEVCDecoder } from "./decoder.js";
import { HEVC_FRAME_CORRUPT, HEVC_FRAME_PRE_RECOVERY } from "./frame-flags.js";
import type { HEVCFrame } from "./types.js";

/**
 * End-to-end tests of the wrapper against the real WASM decoder (the built
 * artifact in packages/core/wasm, or $HEVC_WASM_DIR). Skipped when the
 * artifact predates the tagged-output API.
 */
/* eslint-disable @typescript-eslint/no-explicit-any */

const g = globalThis as any;
let fs: any = null;
let wasmDir = "";
let fixturesDir = "";
let available = false;

beforeAll(async () => {
  if (!g.process?.versions?.node) return;
  const mod = await import(/* @vite-ignore */ "node:module" as string);
  const path = await import(/* @vite-ignore */ "node:path" as string);
  const url = await import(/* @vite-ignore */ "node:url" as string);
  fs = await import(/* @vite-ignore */ "node:fs" as string);
  const here = path.dirname(url.fileURLToPath(import.meta.url));
  wasmDir = g.process.env.HEVC_WASM_DIR ?? path.resolve(here, "../wasm");
  fixturesDir = path.resolve(here, "../../../tests/conformance/fixtures");
  const js = path.join(wasmDir, "hevc-decode.js");
  if (!fs.existsSync(js)) return;
  try {
    // Evaluate the Emscripten glue as a classic script: inside this package
    // ("type": "module") require() would load it as an ES module, where its
    // CommonJS export never runs.
    const vm = await import(/* @vite-ignore */ "node:vm" as string);
    const code = fs.readFileSync(js, "utf8") as string;
    const context = vm.createContext({
      ...globalThis,
      __filename: js,
      __dirname: wasmDir,
      require: mod.createRequire(js),
      process: g.process,
      console,
      URL,
      Buffer: g.Buffer,
      WebAssembly,
    });
    const factory = new vm.Script(`${code}\nHEVCDecoderModule;`, { filename: js }).runInContext(context);
    if (typeof factory !== "function") return;
    const locateFile = (p: string) => path.join(wasmDir, p);
    // The artifact must export the tagged-output API this wrapper needs
    const probe = await factory({ locateFile });
    if (typeof probe._hevc_decoder_feed_tagged !== "function") return;
    g.HEVCDecoderModule = (opts: Record<string, unknown> = {}) => factory({ locateFile, ...opts });
    available = true;
  } catch {
    // An artifact that cannot load here (e.g. a browser-only build) is skipped
  }
});

function fixture(name: string): Uint8Array {
  return new Uint8Array(fs.readFileSync(`${fixturesDir}/${name}`));
}

/** Split an Annex B stream into access units (first_slice_segment_in_pic_flag). */
function accessUnits(d: Uint8Array): Uint8Array[] {
  const starts: number[] = [];
  for (let i = 0; i + 3 <= d.length; i++) {
    if (d[i] === 0 && d[i + 1] === 0 && d[i + 2] === 1) {
      starts.push(i > 0 && d[i - 1] === 0 ? i - 1 : i);
      i += 2;
    }
  }
  const aus: Uint8Array[] = [];
  let cur = 0;
  let curHasVcl = false;
  for (let k = 0; k < starts.length; k++) {
    const s = starts[k]!;
    const hdr = d[s] === 0 && d[s + 1] === 0 && d[s + 2] === 0 ? s + 4 : s + 3;
    const type = (d[hdr]! >> 1) & 0x3f;
    const vcl = type < 32;
    const first = vcl && (d[hdr + 2]! & 0x80) !== 0;
    if (((vcl && first) || (!vcl && curHasVcl && type !== 40)) && curHasVcl) {
      aus.push(d.subarray(cur, s));
      cur = s;
      curHasVcl = false;
    }
    if (vcl) curHasVcl = true;
  }
  aus.push(d.subarray(cur));
  return aus;
}

async function decodeTagged(aus: Uint8Array[], setup?: (d: HEVCDecoder) => void) {
  const dec = await HEVCDecoder.create();
  setup?.(dec);
  const frames: HEVCFrame[] = [];
  aus.forEach((au, i) => {
    dec.feed(au, i * 1000 + 0.5);
    frames.push(...dec.drain());
  });
  frames.push(...dec.flush());
  const stats = dec.stats;
  dec.destroy();
  return { frames, stats };
}

describe("HEVCDecoder (real WASM)", () => {
  it("tags every frame with the access unit it was coded in, through reordering", async (ctx) => {
    if (!available) ctx.skip();
    const aus = accessUnits(fixture("b_qcif_10f.265"));
    const { frames, stats } = await decodeTagged(aus);
    expect(frames).toHaveLength(10);
    const seen = new Set<number>();
    let reordered = false;
    for (let i = 0; i < frames.length; i++) {
      const f = frames[i]!;
      expect(Number.isNaN(f.tag)).toBe(false);
      expect(f.flags).toBe(0);
      seen.add(f.tag);
      if (i > 0 && f.tag < frames[i - 1]!.tag) reordered = true;
    }
    expect(seen.size).toBe(10);           // one frame per access unit
    expect(reordered).toBe(true);         // B-frames: output order != feed order
    // POCs come out in display order
    for (let i = 1; i < frames.length; i++) expect(frames[i]!.poc).toBeGreaterThan(frames[i - 1]!.poc);
    expect(stats.picturesDecoded).toBe(10);
    expect(stats.picturesOutput).toBe(10);
    expect(stats.skippedError + stats.skippedRasl + stats.skippedWaitIrap + stats.nalErrors).toBe(0);
  });

  it("untagged feed() yields NaN tags", async (ctx) => {
    if (!available) ctx.skip();
    const dec = await HEVCDecoder.create();
    dec.feed(fixture("p_qcif_10f.265"));
    const frames = [...dec.drain(), ...dec.flush()];
    expect(frames).toHaveLength(10);
    for (const f of frames) expect(Number.isNaN(f.tag)).toBe(true);
    dec.destroy();
  });

  it("drainViews() carries tags and flags too", async (ctx) => {
    if (!available) ctx.skip();
    const dec = await HEVCDecoder.create();
    const tags: number[] = [];
    accessUnits(fixture("p_qcif_10f.265")).forEach((au, i) => {
      dec.feed(au, i);
      for (const v of dec.drainViews()) {
        expect(v.flags).toBe(0);
        tags.push(v.tag);
      }
    });
    expect(tags).toEqual([0, 1, 2, 3, 4, 5, 6, 7, 8, 9]);
    dec.destroy();
  });

  it("concealSliceErrors keeps a damaged stream moving and flags the repaired frame", async (ctx) => {
    if (!available) ctx.skip();
    const aus = accessUnits(fixture("p_qcif_10f.265"));
    const damaged = aus.map((au, i) => (i === 3 ? au.subarray(0, au.length >> 1) : au));
    const strict = await decodeTagged(damaged);
    const forgiving = await decodeTagged(damaged, (d) => d.setErrorPolicy({ concealSliceErrors: true }));
    expect(strict.frames.length).toBeLessThan(10);
    expect(strict.stats.skippedError).toBeGreaterThan(0);
    expect(forgiving.frames).toHaveLength(10);
    expect(forgiving.stats.concealed).toBe(1);
    const corrupt = forgiving.frames.filter((f) => f.flags & HEVC_FRAME_CORRUPT);
    expect(corrupt.map((f) => f.tag)).toEqual([3000.5]);
  });

  it("decodeBeforeIrap decodes a stream joined mid-GOP, flagged pre-recovery", async (ctx) => {
    if (!available) ctx.skip();
    const aus = accessUnits(fixture("p_qcif_10f.265"));
    // Parameter sets from the first access unit, then pictures 3..9 only
    const first = aus[0]!;
    let psEnd = 0;
    for (let i = 0; i + 3 <= first.length; i++) {
      if (first[i] === 0 && first[i + 1] === 0 && first[i + 2] === 1) {
        const hdr = i + 3;
        if (((first[hdr]! >> 1) & 0x3f) < 32) { psEnd = i > 0 && first[i - 1] === 0 ? i - 1 : i; break; }
      }
    }
    const joined = aus.slice(3);
    const head = new Uint8Array(psEnd + joined[0]!.length);
    head.set(first.subarray(0, psEnd));
    head.set(joined[0]!, psEnd);
    joined[0] = head;

    const strict = await decodeTagged(joined);
    expect(strict.frames).toHaveLength(0);
    expect(strict.stats.skippedWaitIrap).toBe(7);
    const early = await decodeTagged(joined, (d) => d.setErrorPolicy({ decodeBeforeIrap: true }));
    expect(early.frames).toHaveLength(7);
    for (const f of early.frames) expect(f.flags & HEVC_FRAME_PRE_RECOVERY).toBeTruthy();
  });

  it("discontinuity() releases pending frames and restarts at the next IRAP", async (ctx) => {
    if (!available) ctx.skip();
    const aus = accessUnits(fixture("b_qcif_10f.265"));
    const dec = await HEVCDecoder.create();
    const out: HEVCFrame[] = [];
    aus.slice(0, 6).forEach((au, i) => { dec.feed(au, i); out.push(...dec.drain()); });
    const before = out.length;
    dec.discontinuity();
    out.push(...dec.drain());
    expect(out.length).toBeGreaterThan(before);
    expect(out.length).toBe(dec.stats.picturesDecoded);
    // Non-IRAP pictures after the cut are skipped (default policy)
    aus.slice(6).forEach((au, i) => { dec.feed(au, 100 + i); out.push(...dec.drain()); });
    out.push(...dec.flush());
    expect(out.every((f) => f.tag < 100)).toBe(true);
    expect(dec.stats.skippedWaitIrap).toBe(4);
    dec.destroy();
  });

  it("flushViews() at a segment end hands over the frames held for reordering", async (ctx) => {
    if (!available) ctx.skip();
    const aus = accessUnits(fixture("opengop_qcif_12f.265"));
    const { frames: whole } = await decodeTagged(aus);
    const cut = 5; // the second CRA opens the next segment
    const dec = await HEVCDecoder.create();
    const tags: number[] = [];
    const segment = (from: number, to: number): number => {
      for (let i = from; i < to; i++) {
        dec.feed(aus[i]!, i * 1000 + 0.5);
        tags.push(...dec.drain().map((f) => f.tag));
      }
      const views = dec.flushViews();
      for (const v of views) {
        expect(v.isValid()).toBe(true);
        tags.push(v.tag);
      }
      return views.length;
    };
    expect(segment(0, cut)).toBeGreaterThan(0);
    // Every access unit of the first segment is out before the next is fed
    expect([...tags].sort((a, b) => a - b)).toEqual(
      aus.slice(0, cut).map((_, i) => i * 1000 + 0.5),
    );
    segment(cut, aus.length);
    // References survive the flush: the second segment (RASL included)
    // decodes exactly as in one pass
    expect(tags).toEqual(whole.map((f) => f.tag));
    dec.destroy();
  });
});
