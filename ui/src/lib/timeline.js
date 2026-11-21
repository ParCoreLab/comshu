// Pure helpers for the Timeline panel, here so they can be unit-tested
// (ui/test/timeline.test.mjs).

// The shortest run of overview bins holding BUSY_SHARE of all events, padded.
//
// Why the timeline opens here and not on the whole capture: a run's extent is
// set by its setup and teardown, not its work. On MN5's nccl_internode the
// capture is 5.07 s, and 82% of its events — every send, completion and poll
// — sit in one 0.1 s stretch at 5.0 s; the other 4.9 s hold 16k receives
// pre-posted in batches during setup. Opening on the whole run put all of
// the communication in the last 2% of the width. The overview strip still
// shows the whole run, and "full run" is one click.
export const BUSY_SHARE = 0.8;
export function busiest(bins, w) {
  const total = bins.reduce((a, b) => a + b, 0);
  if (!total) return { ...w };
  const need = total * BUSY_SHARE;
  let best = [0, bins.length - 1], sum = 0, lo = 0;
  for (let hi = 0; hi < bins.length; hi++) {
    sum += bins[hi];
    while (sum - bins[lo] >= need) sum -= bins[lo++];
    if (sum >= need && hi - lo < best[1] - best[0]) best = [lo, hi];
  }
  const binNs = (w.hi - w.lo) / bins.length;
  const a = w.lo + best[0] * binNs, b = w.lo + (best[1] + 1) * binNs;
  // Most of the run is busy: the run IS the view.
  if (b - a >= 0.6 * (w.hi - w.lo)) return { ...w };
  const pad = (b - a) * 0.04;
  return { lo: Math.max(w.lo, a - pad), hi: Math.min(w.hi, b + pad) };
}

// An axis label with exactly the precision its tick step needs. fmtT's fixed
// 4 decimals of a second labelled a 50 µs step as 5.0400 s, 5.0400 s,
// 5.0401 s: ticks that differ, labels that do not.
export function fmtTick(t, step) {
  const [unit, scale] = t >= 1e9 ? ['s', 1e9] : t >= 1e6 ? ['ms', 1e6] : t >= 1e3 ? ['µs', 1e3] : ['ns', 1];
  const dec = Math.max(0, Math.ceil(-Math.log10(step / scale) - 1e-9));
  return `${(t / scale).toFixed(Math.min(dec, 9))} ${unit}`;
}
