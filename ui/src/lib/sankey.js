// A minimal staged-flow (sankey) layout.
//
// Hand-written because there is no sankey transform in Vega — not in
// `vega-transforms`, not in `vega-hierarchy`. The plan predicted this would be
// one of the two places the grammar does not reach, and it is. The alternative
// was precomputing the layout in the reader, which would put a *rendering*
// concern into the data pass.
//
// Scope is deliberately small: ordered stages, links only between adjacent
// stages, no cycles. That is what a breakdown ("of these operations, these
// sizes") needs. It is not a general flow diagram and should not grow into one.

/**
 * @param {{from:string,to:string,value:number}[]} links  from/to are "stage:name"
 * @param {string[][]} stages  node names per stage, in draw order
 * @param {{width:number,height:number,nodeWidth:number,gap:number}} opts
 */
export function sankey(links, stages, { width, height, nodeWidth = 12, gap = 6 }) {
  const totals = new Map();
  const add = (k, v) => totals.set(k, (totals.get(k) ?? 0) + v);
  for (const l of links) { add(l.from, l.value); add(l.to, l.value); }

  // Each stage is scaled independently to fill the height, so a stage whose
  // total is smaller (because some flow ends early) still reads clearly. Every
  // stage here conserves flow, but scaling per stage keeps that assumption out
  // of the geometry.
  const nodes = new Map();
  const colX = stages.length > 1 ? (width - nodeWidth) / (stages.length - 1) : 0;

  stages.forEach((names, si) => {
    const present = names.filter((n) => totals.has(`${si}:${n}`));
    const sum = present.reduce((a, n) => a + totals.get(`${si}:${n}`), 0) || 1;
    const avail = height - gap * Math.max(0, present.length - 1);
    let y = 0;
    for (const n of present) {
      const key = `${si}:${n}`;
      const h = Math.max(1, (totals.get(key) / sum) * avail);
      nodes.set(key, { key, name: n, stage: si, x: si * colX, y, h, value: totals.get(key) });
      y += h + gap;
    }
  });

  // Ribbons stack by the position of the node at the OTHER end, not by the
  // order the query happened to return. Stacking in query order (which was
  // `sends DESC`) made a single-source stage cross its own ribbons for no
  // reason. Node order stays semantic — the size axis reads small-to-large —
  // so the crossings that remain are real, not artefacts of link order.
  const yOf = (k) => nodes.get(k)?.y ?? 0;
  const ordered = [...links].sort(
    (a, b) => yOf(a.from) - yOf(b.from) || yOf(a.to) - yOf(b.to),
  );

  const outAt = new Map(), inAt = new Map();
  const ribbons = [];
  for (const l of ordered) {
    const a = nodes.get(l.from), b = nodes.get(l.to);
    if (!a || !b) continue;
    const ay = (outAt.get(l.from) ?? 0), by = (inAt.get(l.to) ?? 0);
    const ah = (l.value / a.value) * a.h, bh = (l.value / b.value) * b.h;
    ribbons.push({
      ...l,
      x0: a.x + nodeWidth, y0: a.y + ay, h0: ah,
      x1: b.x,             y1: b.y + by, h1: bh,
    });
    outAt.set(l.from, ay + ah);
    inAt.set(l.to, by + bh);
  }
  return { nodes: [...nodes.values()], ribbons, nodeWidth };
}

/** A filled bezier ribbon between two stacked segments. */
export function ribbonPath(r) {
  const mx = (r.x0 + r.x1) / 2;
  return (
    `M${r.x0},${r.y0}` +
    `C${mx},${r.y0} ${mx},${r.y1} ${r.x1},${r.y1}` +
    `L${r.x1},${r.y1 + r.h1}` +
    `C${mx},${r.y1 + r.h1} ${mx},${r.y0 + r.h0} ${r.x0},${r.y0 + r.h0}Z`
  );
}
