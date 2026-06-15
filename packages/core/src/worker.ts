import type { WorkerRequest, WorkerResponse, HEVCFrame, HEVCStreamInfo } from "./types.js";
import { FRAME_STRUCT, copyPlane, readFrameStruct } from "./frame-layout.js";

/**
 * Emscripten module interface
 */
interface EmscriptenModule {
  cwrap(name: string, returnType: string | null, argTypes: string[]): Function;
  _malloc(size: number): number;
  _free(ptr: number): void;
  getValue(ptr: number, type: string): number;
  HEAPU8: Uint8Array;
  HEAPU16: Uint16Array;
}

interface DecoderAPI {
  create: () => number;
  destroy: (dec: number) => void;
  decode: (dec: number, ptr: number, size: number) => number;
  getFrameCount: (dec: number) => number;
  getFrame: (dec: number, index: number, framePtr: number) => number;
  getInfo: (dec: number, infoPtr: number) => number;
}

let decoder: { m: EmscriptenModule; api: DecoderAPI; dec: number } | null = null;

function post(msg: WorkerResponse, transfer?: Transferable[]): void {
  if (transfer) {
    self.postMessage(msg, transfer);
  } else {
    self.postMessage(msg);
  }
}

self.onmessage = async (e: MessageEvent<WorkerRequest>) => {
  const { type } = e.data;

  try {
    if (type === "init") {
      importScripts(e.data.wasmUrl);
      const ModuleFactory = (self as unknown as Record<string, unknown>).HEVCDecoderModule as (
        opts?: Record<string, unknown>,
      ) => Promise<EmscriptenModule>;
      const m = await ModuleFactory();

      const api: DecoderAPI = {
        create: m.cwrap("hevc_decoder_create", "number", []) as () => number,
        destroy: m.cwrap("hevc_decoder_destroy", null, ["number"]) as (dec: number) => void,
        decode: m.cwrap("hevc_decoder_decode", "number", ["number", "number", "number"]) as (dec: number, ptr: number, size: number) => number,
        getFrameCount: m.cwrap("hevc_decoder_get_frame_count", "number", ["number"]) as (dec: number) => number,
        getFrame: m.cwrap("hevc_decoder_get_frame", "number", ["number", "number", "number"]) as (dec: number, index: number, framePtr: number) => number,
        getInfo: m.cwrap("hevc_decoder_get_info", "number", ["number", "number"]) as (dec: number, infoPtr: number) => number,
      };

      const dec = api.create();
      if (!dec) throw new Error("Failed to create decoder");

      decoder = { m, api, dec };
      post({ type: "ready" });

    } else if (type === "decode") {
      if (!decoder) throw new Error("Decoder not initialized");
      const { m, api, dec } = decoder;
      const data = new Uint8Array(e.data.data);

      const ptr = m._malloc(data.length);
      m.HEAPU8.set(data, ptr);
      const ret = api.decode(dec, ptr, data.length);
      m._free(ptr);

      if (ret !== 0) throw new Error("Decode failed");

      // Stream info
      const infoPtr = m._malloc(24);
      if (api.getInfo(dec, infoPtr) === 0) {
        post({
          type: "info",
          info: {
            width:        m.getValue(infoPtr, "i32"),
            height:       m.getValue(infoPtr + 4, "i32"),
            bitDepth:     m.getValue(infoPtr + 8, "i32"),
            chromaFormat: m.getValue(infoPtr + 12, "i32"),
            profile:      m.getValue(infoPtr + 16, "i32"),
            level:        m.getValue(infoPtr + 20, "i32"),
          },
        });
      }
      m._free(infoPtr);

      // Frames
      const count = api.getFrameCount(dec);
      const framePtr = m._malloc(FRAME_STRUCT.SIZE);

      for (let i = 0; i < count; i++) {
        if (api.getFrame(dec, i, framePtr) !== 0) continue;

        const s = readFrameStruct(m, framePtr);
        const y  = copyPlane(m, s.yPtr,  s.width, s.height, s.strideY, s.bytesPerSample);
        const cb = copyPlane(m, s.cbPtr, s.chromaWidth, s.chromaHeight, s.strideC, s.bytesPerSample);
        const cr = copyPlane(m, s.crPtr, s.chromaWidth, s.chromaHeight, s.strideC, s.bytesPerSample);

        // `bytesPerSample` discriminates the plane element type; build the
        // matching union member so the transferred HEVCFrame stays well-typed.
        const meta = {
          width: s.width, height: s.height,
          chromaWidth: s.chromaWidth, chromaHeight: s.chromaHeight,
          bitDepth: s.bitDepth, poc: s.poc,
        };
        const frame: HEVCFrame = s.bytesPerSample === 1
          ? { ...meta, bytesPerSample: 1, y: y as Uint8Array, cb: cb as Uint8Array, cr: cr as Uint8Array }
          : { ...meta, bytesPerSample: 2, y: y as Uint16Array, cb: cb as Uint16Array, cr: cr as Uint16Array };

        post(
          { type: "frame", index: i, frame },
          [y.buffer, cb.buffer, cr.buffer],
        );
      }

      m._free(framePtr);
      post({ type: "done", frameCount: count });

    } else if (type === "destroy") {
      if (decoder) {
        decoder.api.destroy(decoder.dec);
        decoder = null;
      }
    }
  } catch (err) {
    post({ type: "error", message: err instanceof Error ? err.message : String(err) });
  }
};
