// Host-side work lists for the equity kernels. Every combination the GPU
// visits is enumerated here as packed plane-mask pairs the shaders read
// directly; the kernels contain no combinatorics.

// All k-card subsets of `planes` (an array of circuit plane indices), in
// colexicographic order, packed one subset per entry as (lo, hi) u32
// pairs: bit p of lo = plane p, bit p-32 of hi = plane 32+p. The layout
// matches the kernels' array<vec2<u32>> work-list bindings.
export function comboMasks(planes: number[], k: number): Uint32Array<ArrayBuffer> {
  const n = planes.length;
  let count = 1;
  for (let i = 0; i < k; i++) count = (count * (n - i)) / (i + 1);
  const out = new Uint32Array(count * 2);
  const c = Array.from({ length: k }, (_, i) => i); // current subset, ascending
  for (let r = 0; r < count; r++) {
    let lo = 0, hi = 0;
    for (let i = 0; i < k; i++) {
      const p = planes[c[i]];
      if (p < 32) lo |= 1 << p;
      else hi |= 1 << (p - 32);
    }
    out[2 * r] = lo >>> 0;
    out[2 * r + 1] = hi >>> 0;
    if (r + 1 < count) { // colex successor
      let i = 0;
      while (i + 1 < k && c[i] + 1 === c[i + 1]) { c[i] = i; i++; }
      c[i] += 1;
    }
  }
  return out;
}

// In-place Fisher-Yates shuffle of a packed mask list, swapping whole
// (lo, hi) pairs. The kernels walk the lists in storage order, so after a
// shuffle any prefix of the run covers a uniform random subset of the
// combinations and a truncated run is a fair sample (colex order is
// rank-sorted). xorshift32 from the caller's seed (nonzero) so a given spot
// enumerates in the same order every run.
export function shuffleMasks<T extends Uint32Array<ArrayBufferLike>>(masks: T, seed: number): T {
  let s = seed >>> 0;
  const rnd = () => {
    s ^= s << 13; s >>>= 0;
    s ^= s >>> 17;
    s ^= s << 5; s >>>= 0;
    return s / 0x100000000;
  };
  for (let i = masks.length / 2 - 1; i > 0; i--) {
    const j = Math.floor(rnd() * (i + 1));
    const lo = masks[2 * i], hi = masks[2 * i + 1];
    masks[2 * i] = masks[2 * j];
    masks[2 * i + 1] = masks[2 * j + 1];
    masks[2 * j] = lo;
    masks[2 * j + 1] = hi;
  }
  return masks;
}
