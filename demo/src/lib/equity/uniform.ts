// The kernels' 16-word uniform block. Two layouts share the first eight
// words (the known-card masks and the list lengths); what follows depends
// on the kernel family.
//
// Fused kernels (deal-major and board-major):
//   0-1 hero mask (known | drawn)   2-3 villain mask   4-5 board mask
//   6 nb   7 nv
//   8 base       grid cursor: first invocation of this chunk
//   deal-major:  9-10 hdraw (the drawn part of the hero mask alone)
//   board-major: 9 ngrps  10 gbase  11-12 hdraw
//                (ngrps villain lane-groups of 32 list entries, from gbase)
//
// Cross-compare kernels:
//   0-5 masks   6 nb   7 nv   8 nh
//   9 cellbase   grid cursor: first (board, hero-tile) cell of this chunk
//   10 htiles    hero tiles per board
//
// The kernels drop list entries that claim a card in hdraw, which is how a
// hero draw enumerated on the CPU keeps the counted deals collision-free.

import type { Mask } from "./types.ts";

export const UNIFORM_BYTES = 64;
/** byte offset of the fused kernels' cursor words (8 onward) */
export const CURSOR_OFFSET = 32;
/** byte offset of the cross-compare cellbase word (9) */
export const CELLBASE_OFFSET = 36;

export interface KnownMasks { hero: Mask; villain: Mask; board: Mask }

export function fusedUniform(
  known: KnownMasks, nb: number, nv: number, drawn: Mask, boardMajor: boolean,
): Uint32Array {
  const u = new Uint32Array(16);
  u.set([known.hero[0] | drawn[0], known.hero[1] | drawn[1],
         ...known.villain, ...known.board, nb, nv,
         0, // base
         // ngrps: all lane-groups, for the unchunked path; the chunked
         // board-major loop rewrites it per chunk
         boardMajor ? Math.ceil(nv / 32) : 0,
         0]); // gbase
  u.set(drawn, boardMajor ? 11 : 9);
  return u;
}

export function crossCompareUniform(
  known: KnownMasks, nb: number, nv: number, nh: number, htiles: number,
): Uint32Array {
  const u = new Uint32Array(16);
  u.set([...known.hero, ...known.villain, ...known.board, nb, nv, nh, 0, htiles]);
  return u;
}

// image for the no-op warmup dispatch: empty lists, and for cross-compare
// htiles=1 so the cell decode never divides by zero
export function warmupUniform(xc: boolean): Uint32Array {
  const u = new Uint32Array(16);
  if (xc) u[10] = 1;
  return u;
}
