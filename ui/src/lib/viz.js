// Shared visual vocabulary. One place, so panels cannot drift into private
// palettes and scales — the thing that makes eight panels read as one tool.

/** Sequential blue, 100 → 700, from the validated reference palette. */
const STEPS = [
  '#cde2fb', '#b7d3f6', '#9ec5f4', '#86b6ef', '#6da7ec', '#5598e7', '#3987e5',
  '#2a78d6', '#256abf', '#1c5cab', '#184f95', '#104281', '#0d366b',
];

/** True when the viewer is in dark mode, mirroring the CSS resolution order. */
export function isDark() {
  const t = document.documentElement.dataset.theme;
  if (t === 'dark') return true;
  if (t === 'light') return false;
  return matchMedia('(prefers-color-scheme: dark)').matches;
}

/**
 * A sequential ramp ordered so MORE is always further from the surface.
 *
 * Reversing for dark is not a preference. The first version of the matrix
 * panel mapped the largest values to the darkest step, so against a dark
 * background a 1.3 GB diagonal read *quieter* than the 158 KB cells beside it.
 * A sequential ramp is defined against its surface, not in the abstract.
 */
export const ramp = () => (isDark() ? [...STEPS].reverse() : STEPS);

/**
 * Log-scaled colour lookup over [min,max].
 *
 * Log because the corpus measured a real matrix spanning five orders of
 * magnitude — two cells near 200k sends and the rest around 14. Linear encodes
 * that as two lit cells on a dead grid.
 */
export function logScale(values) {
  const r = ramp();
  const pos = values.filter((v) => v > 0);
  if (!pos.length) return () => r[0];
  const lo = Math.log(Math.min(...pos));
  const hi = Math.log(Math.max(...pos));
  return (v) => {
    if (!(v > 0)) return r[0];
    const t = hi > lo ? (Math.log(v) - lo) / (hi - lo) : 1;
    return r[Math.max(0, Math.min(r.length - 1, Math.round(t * (r.length - 1))))];
  };
}

/**
 * Categorical hues, in fixed order, from the same validated palette as STEPS.
 *
 * Fixed order matters: colour follows the entity, not its rank, so a chart that
 * drops a series must not repaint the survivors. Both modes are *selected* —
 * the dark column is the same eight hues re-stepped for the dark surface, not
 * an automatic flip of the light one.
 *
 * Validated (OKLab ΔE ×100) against both surfaces: worst adjacent CVD 9.1 light
 * / 8.4 dark, worst adjacent normal-vision 19.6 / 19.3. Three light-mode slots
 * (aqua, yellow, magenta) sit under 3:1 against the light surface, so anything
 * using them keeps a legend and a readable table beside it — which the chat
 * pane does by construction, since every chart ships with its query.
 */
const SERIES_LIGHT = ['#2a78d6', '#eb6834', '#1baf7a', '#eda100', '#e87ba4', '#008300', '#4a3aa7', '#e34948'];
const SERIES_DARK = ['#3987e5', '#d95926', '#199e70', '#c98500', '#d55181', '#008300', '#9085e9', '#e66767'];

export const series = () => (isDark() ? SERIES_DARK : SERIES_LIGHT);

export function bytes(n) {
  if (n === null || Number.isNaN(n)) return '—';
  if (n < 1024) return `${n} B`;
  const u = ['KB', 'MB', 'GB', 'TB', 'PB'];
  let i = -1;
  while (n >= 1024 && i < u.length - 1) { n /= 1024; i++; }
  return `${n.toFixed(n < 10 ? 1 : 0)} ${u[i]}`;
}

export const count = (n) => (n === null ? '—' : n.toLocaleString());
