// Rendering a model-authored Vega-Lite spec, with our colours and our rows.
//
// The spec arrives from the agent with no data in it and the query that fills
// it alongside. This module runs neither: the panel fetches rows through
// `query()` — the one data path — and hands them here. So a chart is exactly as
// trustworthy as the SQL printed above it, and it renders in the WASM build
// unchanged.
//
// Colour is ours, not the model's. The spec's own `config` is refused by the
// bridge's guard, and the theme config below is merged in here, so a model
// asking for "viridis" cannot reintroduce the dark-mode inversion bug that the
// matrix panel already survived once.

import { isDark, ramp, series } from './viz.js';

// Vega is ~800 KB of the bundle and only the chat pane's charts need it, so it
// loads on first chart rather than on page load. The panels that render their
// own SVG stay as cheap as they were.
let vegaLibs = null;
const vega = () => (vegaLibs ??= Promise.all([import('vega-lite'), import('vega')]));

const css = (name) => getComputedStyle(document.documentElement).getPropertyValue(name).trim();

export function themeConfig() {
  const text1 = css('--text-1');
  const text2 = css('--text-2');
  const border = css('--border');
  return {
    background: null, // the Card supplies the surface
    font: 'ui-sans-serif, system-ui, sans-serif',
    axis: {
      labelColor: text2,
      titleColor: text2,
      labelFontSize: 11,
      titleFontSize: 11,
      titleFontWeight: 500,
      domainColor: border,
      tickColor: border,
      gridColor: border,
      gridOpacity: isDark() ? 0.35 : 0.6,
    },
    legend: {
      labelColor: text2,
      titleColor: text2,
      labelFontSize: 11,
      titleFontSize: 11,
      symbolStrokeWidth: 0,
    },
    title: { color: text1, fontSize: 13, fontWeight: 600, anchor: 'start' },
    view: { stroke: null, continuousWidth: 320, continuousHeight: 200 },
    range: { category: series(), ramp: ramp(), heatmap: ramp(), ordinal: ramp() },
    // Thin marks, rounded data ends, a surface gap between adjacent fills.
    bar: { color: series()[0], cornerRadiusEnd: 4, binSpacing: 2 },
    line: { color: series()[0], strokeWidth: 2 },
    point: { color: series()[0], filled: true, size: 64 },
    rect: { color: series()[0] },
    boxplot: { median: { color: css('--surface-1') } },
  };
}

/**
 * Which fields a spec reads as numbers.
 *
 * Rows arrive from `query()` as strings — deliberately, because a JSON number
 * cannot hold a UBIGINT address without dropping the low bits. Vega will happily
 * plot '81920' as a category, so quantitative fields are coerced here and
 * nowhere else. Absolute wall-ns is still a float64 once coerced: a chart of
 * `t0_ns` is a chart of 256 ns buckets, which is why the catalog tells the model
 * to subtract the capture start in SQL before plotting time.
 */
function quantitativeFields(spec) {
  const fields = new Set();
  const walk = (n) => {
    if (Array.isArray(n)) return n.forEach(walk);
    if (!n || typeof n !== 'object') return;
    if (typeof n.field === 'string' && (n.type === 'quantitative' || n.aggregate || n.bin)) fields.add(n.field);
    Object.values(n).forEach(walk);
  };
  walk(spec.encoding);
  if (Array.isArray(spec.layer)) spec.layer.forEach((l) => quantitativeFields(l).forEach((f) => fields.add(f)));
  return fields;
}

export function coerce(spec, rows) {
  const numeric = quantitativeFields(spec);
  if (!numeric.size) return rows;
  return rows.map((r) => {
    const out = { ...r };
    for (const f of numeric) if (out[f] !== null && out[f] !== undefined) out[f] = Number(out[f]);
    return out;
  });
}

/**
 * Render into `el`. Returns the view, which the caller must `finalize()` —
 * a Vega view left alive keeps timers and listeners after the node is gone.
 */
export async function render(el, spec, rows) {
  const [{ compile }, { parse, View }] = await vega();
  // Vega-Lite `structuredClone`s the spec, which throws on a Svelte $state
  // proxy — and the spec always arrives inside one, because it came from a
  // reactive chat turn. Flattening here rather than at the call site keeps the
  // panel from having to know what vega-lite does internally.
  spec = JSON.parse(JSON.stringify(spec));
  const vl = {
    width: 'container',
    height: 240,
    ...spec,
    autosize: { type: 'fit', contains: 'padding' },
    config: themeConfig(),
  };
  const compiled = compile(vl).spec;
  const view = new View(parse(compiled), { renderer: 'svg', container: el, hover: true });
  view.data('rows', coerce(spec, rows));
  await view.runAsync();
  return view;
}
