import type { ReactNode } from "react";

import type { EquityView } from "../hooks/useEquityEngine.ts";
import type { DealSpace } from "../lib/equity.ts";
import { bigf, fmtDur, nf, pctf } from "../lib/format.ts";
import type { Oriented, OrientedHigh, OrientedHilo, Verdict } from "../lib/tally.ts";
import type { PanelState } from "../lib/engine.ts";

// ---- pieces ----

type Seg = [cls: string, frac: number];

function Bar({ segs, thin }: { segs: Seg[]; thin?: boolean }) {
  return (
    <div className={thin ? "eqbar thin" : "eqbar"}>
      {segs.filter(([, f]) => f > 0).map(([c, f]) => (
        <div key={c} className={c} style={{ width: `${(100 * f).toFixed(3)}%` }} />
      ))}
    </div>
  );
}

function Cell(
  { dot, label, pct, n, right }:
  { dot: string; label: string; pct: string; n?: ReactNode; right?: boolean },
) {
  return (
    <div className={right ? "eqcell right" : "eqcell"}>
      <div className="lbl"><span className={`dot ${dot}`} />{label}</div>
      <div className="pct">{pct}</div>
      {/* wraps: the quarter-pot count is wider than its cell */}
      {n ? <div className="n">{n}</div> : null}
    </div>
  );
}

// While running, the cells are laid out before the first snapshot (~200 ms
// after Compute) so the panel height, and the transport buttons under it,
// do not move when it lands. Placeholders rather than zeros: "0.00%" would
// be a claim about the hand.
function CellsHigh({ d }: { d: OrientedHigh | null }) {
  const P = (x: number) => (d ? pctf(x) : "—");
  const N = (x: number) => (d ? nf(x) : "\u00a0"); // holds the line, shows nothing
  const b = d ? d.boards : 1;                      // divisions stay finite
  return (
    <div className="eqcells">
      <Cell dot="seg-win" label="Hero wins" pct={P((d?.win ?? 0) / b)} n={N(d?.win ?? 0)} />
      <Cell dot="seg-tie" label="Tie" pct={P((d?.tie ?? 0) / b)} n={N(d?.tie ?? 0)} />
      <Cell dot="seg-loss" label="Villain wins" pct={P((d?.loss ?? 0) / b)} n={N(d?.loss ?? 0)} />
    </div>
  );
}

function CellsHilo({ d }: { d: OrientedHilo | null }) {
  const P = (x: number) => (d ? pctf(x) : "—");
  const b = d ? d.boards : 1;
  const share = (d?.quarters ?? 0) / (4 * b);
  return (
    <div className="eqcells">
      <Cell dot="seg-win" label="Hero pot share" pct={P(share)}
            n={d ? `${nf(d.quarters)} of ${nf(4 * b)} quarter-pots` : "\u00a0"} />
      <Cell dot="seg-loss" label="Villain" pct={P(1 - share)} right />
    </div>
  );
}

// `hilo` picks the placeholder shape before the first snapshot; once there
// is a tally its own discriminant decides.
function Tally({ d, hilo }: { d: Oriented | null; hilo: boolean }) {
  if (d === null) {
    return hilo
      ? <><CellsHilo d={null} /><Halves d={null} /></>
      : <CellsHigh d={null} />;
  }
  return d.hilo
    ? <><CellsHilo d={d} /><Halves d={d} /></>
    : <CellsHigh d={d} />;
}

function Halves({ d }: { d: OrientedHilo | null }) {
  const P = (x: number) => (d ? pctf(x) : "—");
  const b = d ? d.boards : 1;
  const v = (k: keyof OrientedHilo) => (d ? (d[k] as number) : 0);
  return (
    <>
      <div className="eqhalf">
        <span className="tag">HIGH</span>
        <Bar thin segs={[["seg-win", v("winH") / b], ["seg-tie", v("tieH") / b],
                         ["seg-loss", v("lossH") / b]]} />
      </div>
      <div className="eqhalfnums">
        <span>win <b>{P(v("winH") / b)}</b></span>
        <span>tie <b>{P(v("tieH") / b)}</b></span>
        <span>lose <b>{P(v("lossH") / b)}</b></span>
      </div>
      <div className="eqhalf">
        <span className="tag">LOW</span>
        <Bar thin segs={[["seg-win", v("heroLow") / b], ["seg-tie", v("splitLow") / b],
                         ["seg-loss", v("villLow") / b], ["seg-nolow", v("noLow") / b]]} />
      </div>
      <div className="eqhalfnums">
        <span>hero best <b>{P(v("heroLow") / b)}</b></span>
        <span>split <b>{P(v("splitLow") / b)}</b></span>
        <span>villain best <b>{P(v("villLow") / b)}</b></span>
        <span><span className="hatch" />no low <b>{P(v("noLow") / b)}</b></span>
      </div>
    </>
  );
}

function VerdictBody({ v }: { v: Verdict }) {
  return (
    <>
      <div className={`sdverdict ${v.cls}`}>{v.headline}</div>
      {v.rows.map(([tag, who, cls]) => (
        <div className="eqhalf" key={tag}>
          <span className="tag">{tag}</span>
          <span className={`sdwho ${cls}`}>{who}</span>
        </div>
      ))}
    </>
  );
}

const DealLead = ({ deals, note }: { deals: number; note?: string }) => (
  <div className="eqlead">
    <b>{bigf(deals)}</b>{" "}
    <span className="sz">deal{deals === 1 ? "" : "s"}{note ? ` (${note})` : ""}</span>
  </div>
);

// hands x hands x runouts, skipping any factor of 1. Computed from the
// displayed seats, so it reads hero-first whichever seat the kernel used.
function factorLine(size: DealSpace): string {
  const parts: string[] = [];
  if (size.heroHands > 1) parts.push(`${nf(size.heroHands)} hero hands`);
  if (size.villainHands > 1) parts.push(`${nf(size.villainHands)} villain hands`);
  if (size.runouts > 1) parts.push(`${nf(size.runouts)} runouts`);
  if (parts.length > 1) return `${parts.join(" × ")} = ${nf(size.deals)} deals`;
  return parts[0] ?? `${nf(size.runouts)} runout${size.runouts === 1 ? "" : "s"}`;
}

// ---- transport controls ----
//
// Glyphs so pause and resume occupy the same square and Stop never moves;
// the word is in the tooltip and the accessible name.
const ICON: Record<string, ReactNode> = {
  pause: <><rect x="6" y="5" width="4" height="14" rx="1" />
           <rect x="14" y="5" width="4" height="14" rx="1" /></>,
  play: <path d="M8 5.2 19 12 8 18.8Z" />,
  stop: <rect x="6" y="6" width="12" height="12" rx="1.5" />,
};

function IconButton(
  { icon, label, onClick }: { icon: keyof typeof ICON; label: string; onClick: () => void },
) {
  return (
    <button className="icon" onClick={onClick} title={label} aria-label={label}>
      <svg viewBox="0 0 24 24" fill="currentColor" aria-hidden="true">{ICON[icon]}</svg>
    </button>
  );
}

const BADGE: Record<PanelState["kind"], [cls: string, text: string]> = {
  empty: ["idle", "Not run"],
  refused: ["idle", "Not run"],
  pending: ["idle", "Not run"],
  running: ["run", "Enumerating"],
  stopped: ["warn", "Partial"],
  done: ["exact", "Exact"],
  showdown: ["exact", "Showdown"],
};

// ---- the panel ----

export function EquityPanel({ view }: { view: EquityView }) {
  const { panel, status, compute, pause, resume, stop } = view;
  let body: ReactNode = null;
  let items: ReactNode[] = [];
  let ctl: ReactNode = null;
  let ctlTitle = "";
  let note = "";

  switch (panel.kind) {
    case "empty":
      return (
        <section id="resultCol">
          <div id="eqPanel" className="empty">{panel.message}</div>
          <div id="eqStatus">{status}</div>
        </section>
      );

    case "refused":
      ctlTitle = panel.why === "gpu" ? "needs WebGPU" : "too large";
      body = <DealLead deals={panel.deals} note={ctlTitle} />;
      if (panel.size) note = factorLine(panel.size);
      ctl = <button className="go" disabled title={ctlTitle}>Compute</button>;
      break;

    case "pending":
      body = <DealLead deals={panel.deals} />;
      items = [panel.estimate];
      note = factorLine(panel.size);
      ctl = <button className="go" onClick={compute}>Compute</button>;
      break;

    case "running": {
      const w = Math.min(100, 100 * panel.frac).toFixed(2);
      body = (
        <>
          <div className={panel.paused ? "eqbar paused" : "eqbar"}>
            <div className="fill" style={{ width: `${w}%` }} />
          </div>
          <Tally d={panel.d} hilo={panel.hilo} />
        </>
      );
      // no percentage: the bar above is the percentage, and the width matters
      items = [`${bigf(panel.done)} of ${bigf(panel.total)} deals`,
               fmtDur(panel.elapsed), panel.eta];
      ctl = (
        <>
          {panel.paused
            ? <IconButton key="transport" icon="play" label="Resume" onClick={resume} />
            : <IconButton key="transport" icon="pause" label="Pause" onClick={pause} />}
          <IconButton key="stop" icon="stop" label="Stop" onClick={stop} />
        </>
      );
      break;
    }

    case "stopped":
      body = panel.d ? <Outcome d={panel.d} /> : null;
      items = [`stopped after ${bigf(panel.done)} of ${bigf(panel.total)} deals`,
               fmtDur(panel.elapsed)];
      ctl = <button className="go" onClick={compute}>Start over</button>;
      break;

    case "done":
      body = <Outcome d={panel.d} />;
      items = [
        fmtDur(panel.ms),
        panel.hilo && panel.d.hilo
          ? <span title="takes both the high and the low half">
              {panel.d.scoopBy} scoops {pctf(panel.d.scoop / panel.d.boards)}
            </span>
          : null,
      ];
      note = factorLine(panel.size);
      break;

    case "showdown":
      // one board: nothing to factorise, no tally
      body = <VerdictBody v={panel.verdict} />;
      items = [
        fmtDur(panel.ms),
        panel.hilo && panel.d.hilo ? `${panel.d.quarters} of 4 quarter-pots to hero` : null,
      ];
      break;
  }

  // paused is a phase of `running`; only the badge distinguishes it
  const [badgeCls, badgeText] = panel.kind === "running" && panel.paused
    ? (["run", "Paused"] as const)
    : BADGE[panel.kind];
  const shown = items.filter(Boolean);

  // The panel re-renders ~13x a second while a solve runs. If a transport
  // button is replaced between a press's pointerdown and pointerup, the
  // click lands on the panel and does nothing (reliably so on iOS). So
  // #eqBody / #eqMeta / #eqNote are always rendered and the transport
  // buttons keep stable keys, which keeps their DOM nodes across renders.
  return (
    <section id="resultCol">
      <div id="eqPanel">
        <div id="eqBody">{body}</div>
        <div id="eqMeta" className="eqmeta">
          <span id="eqBadge" className={`eqbadge ${badgeCls}`}>{badgeText}</span>
          <span id="eqFacts">
            {shown.map((s, i) => <span className="mi" key={i}>{s}</span>)}
          </span>
          {/* a disabled button swallows its tooltip in some browsers, so the
              reason also rides on the container */}
          <span id="eqCtl" className="eqctl" title={ctlTitle || undefined}>{ctl}</span>
        </div>
        <div id="eqNote" className="eqnote" hidden={!note}>{note}</div>
      </div>
      <div id="eqStatus">{status}</div>
    </section>
  );
}

/** a settled distribution: outcome bar plus the cells (and hi-lo halves) */
function Outcome({ d }: { d: Oriented }) {
  const b = d.boards;
  let bar: ReactNode;
  if (d.hilo) {
    const share = d.quarters / (4 * b);
    bar = <Bar segs={[["seg-win", share], ["seg-loss", 1 - share]]} />;
  } else {
    bar = <Bar segs={[["seg-win", d.win / b], ["seg-tie", d.tie / b],
                      ["seg-loss", d.loss / b]]} />;
  }
  return (
    <>
      {bar}
      <Tally d={d} hilo={d.hilo} />
    </>
  );
}
