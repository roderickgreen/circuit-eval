// The grid walks: how one solve's deal space is cut into dispatches. Each
// walk submits through a Dispatch (dispatch.ts) and writes the kernel's
// uniform block (uniform.ts); the work lists are already bound.
//
// The hero side is uniform across a dispatch (every kernel evaluates hero
// once and compares many villains against it), so hero draws are enumerated
// here: one pass of the walk per draw, rewriting the uniform's hero mask
// and hdraw words. Hero draws are the INNER loop of every walk: every
// chunk runs once per draw before the cursor advances, so a truncated run
// has covered every hero draw to the same depth and partial tallies stay a
// fair sample of the whole space. The cross-compare kernel carries the
// hero list as a storage binding instead, and needs no such loop.
//
// The work lists are uploaded shuffled (worklist.ts), so every chunk covers
// a random subset of the deal space and partial tallies converge to the
// exact result rather than drifting through rank order.

import { Dispatch, PACE_MS } from "./dispatch.ts";
import { CELLBASE_OFFSET, CURSOR_OFFSET, crossCompareUniform, fusedUniform,
         type KnownMasks } from "./uniform.ts";
import type { Mask } from "./types.ts";

export interface Grid {
  known: KnownMasks;
  /** work list lengths: boards, villain combos, hero combos */
  nb: number;
  nv: number;
  nh: number;
  /** one (lo, hi) mask per hero draw; a single empty mask when the hero is
   *  fully known */
  heroDraws: Mask[];
  /** deals the walk will count, collision-dropped tuples excluded */
  deals: number;
}

// Deal-major and board-major kernels over the plain invocation grid: one
// stride of the grid per chunk. Deal-major invocations are shallow (<= 32
// deals each), so hooked chunks are cadence-sized strides of ~2^29 deals,
// floored at 2048 groups so a dispatch still fills the GPU. Without hooks
// the whole grid goes up front with a single readback at the end.
export async function walkGrid(
  run: Dispatch, { known, nb, nv, heroDraws }: Grid, boardMajor: boolean,
): Promise<void> {
  const totalInv = boardMajor ? nb : nv * Math.ceil(nb / 32);
  const dealsPerInv = boardMajor ? nv : Math.min(nb, 32);
  const chunkGroups = run.hooked
    ? Math.min(run.maxGroups, Math.max(2048, Math.ceil(2 ** 29 / dealsPerInv / 64)))
    : run.maxGroups;
  let base = 0;
  while (base < totalInv && !run.aborted) {
    const nGroups = Math.min(chunkGroups, Math.ceil((totalInv - base) / 64));
    const lastStride = base + nGroups * 64 >= totalInv;
    for (let i = 0; i < heroDraws.length; i++) {
      if (await run.checkpoint()) break;
      run.writeUniform(0, fusedUniform(known, nb, nv, heroDraws[i], boardMajor));
      run.writeUniform(CURSOR_OFFSET, [base]);
      const last = lastStride && i === heroDraws.length - 1;
      run.submit(nGroups, run.hooked || last);
      await run.throttle();
    }
    base += nGroups * 64;
  }
}

// Board-major kernel with hooks: chunked along the villain axis. A
// board-major invocation carries the whole villain inner loop, so chunking
// the invocation grid alone leaves chunks indivisibly huge on big-inner
// spots. Instead every chunk stays wide -- all boards, full occupancy --
// and shallow: the uniform's ngrps/gbase words bound a run of villain
// lane-groups (32 list entries each). Spans are paced to ~PACE_MS of GPU
// work from the measured rate; until a rate is known the span is one lane
// group, whose cost scales with the board count.
export async function walkBoardMajorChunked(
  run: Dispatch, { known, nb, nv, heroDraws }: Grid,
): Promise<void> {
  const groupsAll = Math.ceil(nb / 64);
  const laneGroups = Math.ceil(nv / 32);
  let gbase = 0;
  while (gbase < laneGroups && !run.aborted) {
    const target = run.pacedDeals(PACE_MS);
    const span = Math.min(laneGroups - gbase,
                          target ? Math.max(1, Math.round(target / nb / 32)) : 1);
    for (const drawn of heroDraws) {
      if (await run.checkpoint()) break;
      run.writeUniform(0, fusedUniform(known, nb, nv, drawn, true));
      for (let gb = 0; gb < groupsAll; gb += run.maxGroups) {
        const nGroups = Math.min(run.maxGroups, groupsAll - gb);
        run.writeUniform(CURSOR_OFFSET, [gb * 64, span, gbase]);
        run.submit(nGroups, gb + nGroups >= groupsAll); // snapshot per slice
      }
      await run.throttle();
    }
    gbase += span;
  }
}

// Cross-compare kernel: the grid is a row-major walk of (board, hero-tile)
// cells, one workgroup per cell, with cellbase as the only cursor. A chunk
// is a contiguous cell range, so a truncated run covers complete (shuffled)
// boards plus one partially covered board -- still a fair sample. Hooked
// chunks are paced to ~PACE_MS of GPU work from the measured rate; the
// pacing quantum is one cell (one board x up to ht heroes x the whole
// villain list).
export async function walkCrossCompare(
  run: Dispatch, { known, nb, nv, nh, deals }: Grid, ht: number,
): Promise<void> {
  // until a rate is measured: tens of ms of work on a real GPU
  const INITIAL_TARGET = 2 ** 25;
  const htiles = Math.ceil(nh / ht);
  const cells = nb * htiles;
  run.writeUniform(0, crossCompareUniform(known, nb, nv, nh, htiles));
  // exact average deals per cell (collision-dropped pairs included):
  // small-hand spots have cells far below ht * nv, and sizing chunks from
  // an overestimate starves the GPU behind snapshot roundtrips
  const dealsPerCell = deals / cells;
  let base = 0;
  while (base < cells) {
    if (await run.checkpoint()) break;
    let n = Math.min(cells - base, run.maxGroups);
    if (run.hooked) {
      const target = run.pacedDeals(PACE_MS) ?? INITIAL_TARGET;
      n = Math.min(n, Math.max(1, Math.round(target / dealsPerCell)));
    }
    run.writeUniform(CELLBASE_OFFSET, [base]);
    run.submit(n, run.hooked || base + n >= cells);
    base += n;
    await run.throttle();
  }
}
