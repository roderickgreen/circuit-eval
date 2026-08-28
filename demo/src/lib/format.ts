// Number and duration formatting for the results panel.

export const nf = (x: number) => x.toLocaleString("en-US");
export const pctf = (x: number) => (100 * x).toFixed(2) + "%";

// Readable form for the running and pending lines (the factor line keeps
// exact digits). Names stop at quadrillion, past the 2^53 exactness bound,
// so only refused spots reach the exponent form.
const BIG_SCALE: Array<[number, string]> = [
  [1e15, "quadrillion"], [1e12, "trillion"], [1e9, "billion"], [1e6, "million"],
];

export function bigf(n: number): string {
  if (n >= 1e18) return n.toExponential(2).replace("e+", " × 10^");
  for (const [v, name] of BIG_SCALE) if (n >= v) return `${(n / v).toFixed(2)} ${name}`;
  return nf(n);
}

// Each unit hands off before its count reaches three digits, and precision
// drops with the scale (hours carry minutes, not seconds).
export function fmtDur(ms: number): string {
  if (!Number.isFinite(ms)) return "";
  if (ms < 1000) return `${Math.round(ms)} ms`;
  if (ms < 60000) return `${(ms / 1000).toFixed(1)} s`;
  const pad = (n: number) => String(n).padStart(2, "0");
  const sec = Math.round(ms / 1000);
  if (sec < 3600) return `${Math.floor(sec / 60)} m ${pad(sec % 60)} s`;
  const min = Math.round(ms / 60000);
  if (min < 1440) return `${Math.floor(min / 60)} h ${pad(min % 60)} m`;
  const hr = Math.round(ms / 3600000);
  return `${Math.floor(hr / 24)} d ${hr % 24} h`;
}
