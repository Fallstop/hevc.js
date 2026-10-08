import type { HEVCFrame, HEVCFrameView } from "./types.js";

/**
 * Pack a decoded frame into a contiguous 8-bit I420 buffer (Y, then Cb, then
 * Cr), narrowing samples above 8 bits by `bitDepth - 8` and clamping to 255.
 * Accepts a packed {@link HEVCFrame} (rows contiguous) or a zero-copy
 * {@link HEVCFrameView} (strided rows straight from the WASM heap).
 *
 * 8-bit frames with `Uint8Array` planes — what every 8-bit stream decodes to —
 * are already in range, so whole rows are copied with `TypedArray.set`
 * instead of narrowing sample by sample; the result is identical.
 *
 * @param scratch Reused when large enough; otherwise a new buffer is allocated.
 * @returns The I420 bytes (a view of `scratch` or of the new buffer).
 */
export function packI420(frame: HEVCFrame | HEVCFrameView, scratch?: Uint8Array | null): Uint8Array {
  const w = frame.width;
  const h = frame.height;
  const cw = frame.chromaWidth;
  const ch = frame.chromaHeight;
  const shift = frame.bitDepth > 8 ? frame.bitDepth - 8 : 0;
  // Packed HEVCFrame rows are contiguous (stride == width); views carry the heap stride.
  const strideY = "strideY" in frame ? frame.strideY : w;
  const strideC = "strideC" in frame ? frame.strideC : cw;

  const ySize = w * h;
  const cSize = cw * ch;
  const need = ySize + cSize * 2;
  const out = scratch && scratch.length >= need ? scratch.subarray(0, need) : new Uint8Array(need);

  let di = 0;
  const plane = (src: Uint8Array | Uint16Array, pw: number, ph: number, stride: number) => {
    if (shift === 0 && src instanceof Uint8Array) {
      if (stride === pw) {
        out.set(src.subarray(0, pw * ph), di);
        di += pw * ph;
      } else {
        for (let r = 0; r < ph; r++, di += pw) out.set(src.subarray(r * stride, r * stride + pw), di);
      }
      return;
    }
    for (let r = 0; r < ph; r++) {
      const base = r * stride;
      for (let c = 0; c < pw; c++) {
        const v = src[base + c]! >> shift;
        out[di++] = v > 255 ? 255 : v;
      }
    }
  };
  plane(frame.y, w, h, strideY);
  plane(frame.cb, cw, ch, strideC);
  plane(frame.cr, cw, ch, strideC);
  return out;
}
