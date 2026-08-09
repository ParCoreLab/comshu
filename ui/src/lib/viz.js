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

export function bytes(n) {
  if (n === null || Number.isNaN(n)) return '—';
  if (n < 1024) return `${n} B`;
  const u = ['KB', 'MB', 'GB', 'TB', 'PB'];
  let i = -1;
  while (n >= 1024 && i < u.length - 1) { n /= 1024; i++; }
  return `${n.toFixed(n < 10 ? 1 : 0)} ${u[i]}`;
}

export const count = (n) => (n === null ? '—' : n.toLocaleString());
