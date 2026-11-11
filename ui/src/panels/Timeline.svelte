<script>
  import { query, num } from '../lib/query.js';
  import { bytes, count, ramp, isDark } from '../lib/viz.js';
  import Card from '../lib/Card.svelte';

  // A timeline, finally — but a small one, on purpose.
  //
  // This panel used to say "deliberately not a timeline, handed to Perfetto".
  // That was right while the UI had no time axis at all, and it stops being
  // right the moment you need to know why rank 17 went quiet at t=2.4s without
  // exporting a .pftrace and uploading it somewhere. Perfetto is NOT replaced:
  // it stays the forensics tool and the hand-off line stays below. This is the
  // 5% of it that removes the export step from the common case.
  //
  // THE CONSTRAINT THAT DECIDES THE DESIGN, measured on the 64-rank corpus:
  // 2,987,732 spans, a median ib_post_send of ~122 ns, in a 7.0 s window. That
  // is 1:57,000,000. At 1400 px one pixel holds ~40,000 events on a busy lane,
  // so drawing spans at that zoom is not slow — it is MEANINGLESS, every lane
  // is a solid bar. Hence two modes over one query shape:
  //
  //   density  zoomed out. Per (rank, stream, pixel): count, max duration,
  //            bytes. Colour is the log ramp, same one the matrix uses.
  //   spans    zoomed in past ~2 events/px. The rows themselves, at their real
  //            width, hoverable.
  //
  // The mode is switched automatically AND named in the caption, because "each
  // mark is one message" and "each mark is 40,000 messages" look identical and
  // are not the same claim. Devices.svelte states its two actor sources for the
  // same reason.
  //
  // THE VIEWPORT DRIVES THE QUERY. No client-side index, no pre-bucketing, no
  // WASM. Measured: the full-window density query over all 2.99 M spans at
  // 1400 px x 64 ranks returns in 58 ms / 298 KB. That is inside an interactive
  // budget with room to spare, and it means zooming shows MORE detail rather
  // than the same rows re-scaled — which is the entire point of zooming.
  //
  // Alignment is not done here. timeline_span is already on the wall clock;
  // see _clock in link_sql.hpp for why per-rank TSC fitting is mandatory.
  //
  // EVERYTHING BELOW WORKS IN ns SINCE `origin`, NEVER IN ABSOLUTE ns, and that
  // is the same float64 trap link_sql.hpp documents, arriving a second time on
  // this side of the wire. An absolute wall-clock ns is ~1.79e18; a JS Number
  // is a float64 and its ulp up there is 256 ns. `t - view.lo` on two absolute
  // times would therefore quantise to 256 ns — and the deepest zoom this panel
  // allows is a 100 ns window, which would collapse to a single point.
  //
  // So `origin` is kept as the raw STRING the server sent and is NEVER passed
  // through Number(): it only ever gets interpolated back into SQL, where
  // BIGINT arithmetic is exact. Every number this component holds is an offset
  // (< 1e10 for a 7 s capture), which a float64 carries exactly.

  const PX = 1400;              // query resolution; canvas is scaled to fit
  const SPAN_MODE_MAX = 2.0;    // events per pixel below which we draw real spans
  // Density buckets an event by its START, so an event LONGER than one bucket
  // is not a bucket — it is a bar. Drawn as a bucket it lights the column it
  // began in and nothing after, and when it began before the window it computes
  // a negative px and is clipped away entirely: a 17.5 s barrier kernel covering
  // a 2 s view rendered as NOTHING, while the same record in spans mode drew as
  // a full-width line. Same data, opposite answers, chosen by a threshold.
  //
  // So long spans are fetched as spans in both modes and drawn at their true
  // extent. There are few of them by construction — a span is "long" relative
  // to the current zoom — and the cap below is the guard for the case where
  // that stops being true.
  const LONG_CAP = 20000;
  const ROW_H = 26, LANE_H = 5, LANE_GAP = 1, ROW_PAD = 4;
  const LEFT = 52, AXIS_H = 22;

  // Ordered so a rank's lanes read top-to-bottom in causal order: you post,
  // you ring, the device sends, you reap. An alphabetical sort would put
  // recv_cqe above post_send and make every lane stack read backwards.
  //
  // gpu_kernel leads, and that placement is the point of the lane. Comm rows
  // alone can show that a rank went quiet and can never say whether it was
  // computing or blocked; putting compute directly above the sends is what
  // makes overlap-vs-serialisation readable at a glance. The rows arrive on
  // their own clock domain and are already aligned by _clock — see
  // PLAN-20260813-0600.
  //
  // ib_wr_flight sits directly under ib_post_send because it STARTS at a send:
  // its left edge lines up with the mark above it, so "this send, that long"
  // reads without tracing across the stack. It is the one lane that is a
  // duration rather than an event — every other lane says something happened,
  // this one says how long it took — which is why it earns the position next to
  // the lane it explains rather than a slot at the bottom.
  const STREAMS = [
    'gpu_kernel',
    'ib_post_send', 'ib_wr_flight', 'mlx5_doorbell', 'mlx5_device_send',
    'ib_post_recv', 'ib_recv_cqe', 'ib_poll_cq', 'gpu_access',
  ];
  const SHORT = {
    gpu_kernel: 'kernel',
    ib_post_send: 'send', ib_wr_flight: 'flight', ib_poll_cq: 'poll',
    ib_post_recv: 'recv',
    ib_recv_cqe: 'cqe', mlx5_device_send: 'dsend', mlx5_doorbell: 'db',
    gpu_access: 'gpu',
  };

  let origin = $state(null);    // capture start, absolute ns, AS A STRING — never Number()
  let win = $state(null);       // {lo, hi} — the whole capture, in ns since origin
  let view = $state(null);      // {lo, hi} — what is on screen, in ns since origin
  let ranks = $state([]);
  let lanes = $state([]);       // streams actually present in this capture
  let rows = $state(null);
  // Spans at least one bucket long, carried beside the density grid and drawn
  // at their true extent. Empty in spans mode, where every row already is one.
  let longRows = $state([]);
  let mode = $state('density');
  // The viewport the rows in hand were QUERIED for — not the one on screen.
  //
  // These are never equal while a pan is in flight, and that gap is the whole
  // reason this exists. Density rows carry `px`, a bucket index computed in SQL
  // against the query's own window; drawing them at `px/PX * plotW` pins them
  // to a SCREEN position, so during a drag the bars sat frozen while the axis
  // moved under them and then jumped when the result landed. Spans mode never
  // had the bug because it draws through toX(t0_ns), which is absolute. Keeping
  // the query's window lets density do the same: bucket -> time -> toX.
  let qview = $state(null);
  let error = $state(null);
  let busy = $state(false);
  let hover = $state(null);
  // $state, not plain lets: the render and resize effects read these, so the
  // first paint depends on the bind:this assignment being reactive.
  let canvas = $state(null), wrap = $state(null);
  let width = $state(900);

  // The viewport lives in the fragment next to the view id (#time,lo,hi), so a
  // zoom is linkable exactly as a panel already is. It is also the hook the
  // future cross-panel time selection will read.
  function readHash() {
    const p = location.hash.slice(1).split(',');
    if (p.length === 3) {
      const lo = Number(p[1]), hi = Number(p[2]);
      if (Number.isFinite(lo) && Number.isFinite(hi) && hi > lo) return { lo, hi };
    }
    return null;
  }
  let hashTimer;
  function writeHash(v) {
    clearTimeout(hashTimer);
    hashTimer = setTimeout(() => {
      history.replaceState(null, '', `#time,${Math.round(v.lo)},${Math.round(v.hi)}`);
    }, 250);
  }

  // Bootstrap: the capture's own extent, and which lanes it actually has. Both
  // come from one scan so the panel does not pay for the same table twice.
  //
  // `origin` and the per-stream span are computed IN SQL against the capture
  // minimum, so the wide value never has to survive a float64. What crosses the
  // wire is one opaque string plus small offsets.
  $effect(() => {
    query(`WITH o AS (SELECT min(t0_ns) AS z FROM timeline_span)
           SELECT (SELECT z FROM o)::VARCHAR      AS origin,
                  stream,
                  count(*)::BIGINT                AS n,
                  (max(t1_ns) - (SELECT z FROM o))::BIGINT AS hi
           FROM timeline_span GROUP BY stream`)
      .then((r) => {
        const live = r.filter((x) => num(x.n) > 0);
        if (!live.length) { rows = []; return; }
        origin = live[0].origin;                 // string in, string out
        const hi = Math.max(...live.map((x) => num(x.hi)));
        win = { lo: 0, hi: Math.max(1, hi) };
        lanes = STREAMS.filter((s) => live.some((x) => x.stream === s));
        view = readHash() ?? { ...win };
        return query('SELECT DISTINCT rank FROM timeline_span ORDER BY rank')
          .then((rr) => (ranks = rr.map((x) => num(x.rank))));
      })
      .catch((e) => (error = e.message));
  });

  // One query per viewport. Debounced, and never overlapping: a drag fires
  // dozens of frames and a queue of stale 58 ms queries would arrive out of
  // order and paint the wrong window.
  let pending = null, inflight = false, timer;
  function refresh() {
    if (!view) return;
    pending = { ...view };
    clearTimeout(timer);
    timer = setTimeout(run, 90);
  }
  async function run() {
    if (inflight || !pending) return;
    const v = pending; pending = null; inflight = true; busy = true;
    const span = v.hi - v.lo;
    try {
      // Bounds are rebuilt in SQL as `origin + offset` so the comparison
      // happens in exact BIGINT, and every value returned is an offset.
      const lo = Math.floor(v.lo), hi = Math.ceil(v.hi);
      const w = `t1_ns >= ${origin} + ${lo} AND t0_ns <= ${origin} + ${hi}`;
      // Estimate first: how many events land in the window decides which shape
      // the real query takes. Cheap next to the alternative, which is fetching
      // 40,000 rows per pixel to discover you cannot draw them.
      const [{ n }] = await query(
        `SELECT count(*)::BIGINT AS n FROM timeline_span WHERE ${w}`);
      const perPx = num(n) / PX;
      const m = perPx <= SPAN_MODE_MAX ? 'spans' : 'density';
      // One bucket's worth of time, in ns and integral so SQL compares in
      // BIGINT. This is the line between "a mark" and "a bar".
      const bucketNs = Math.max(1, Math.round(span / PX));
      // Anything at least a bucket long, at its true extent. In spans mode
      // every row is already drawn this way, so this is density-only.
      const lp = m === 'density'
        ? query(`SELECT rank, stream,
                        (t0_ns - ${origin})::BIGINT AS t0_ns,
                        (t1_ns - ${origin})::BIGINT AS t1_ns, ep, bytes
                 FROM timeline_span
                 WHERE ${w} AND t1_ns - t0_ns >= ${bucketNs}
                 ORDER BY t1_ns - t0_ns DESC LIMIT ${LONG_CAP}`)
        : Promise.resolve([]);
      const rp = query(
        m === 'spans'
          // Real spans. ep and bytes come along because at this zoom the user
          // is reading individual messages and the QP is the thing they want.
          ? `SELECT rank, stream,
                    (t0_ns - ${origin})::BIGINT AS t0_ns,
                    (t1_ns - ${origin})::BIGINT AS t1_ns, ep, bytes
             FROM timeline_span WHERE ${w} ORDER BY t0_ns LIMIT 40000`
          // Density. max(duration) is carried, not just count: a pixel holding
          // one 84 us stall and a pixel holding one 122 ns send are the same
          // count and are not the same event, and the stall is the one being
          // looked for.
          // Density, for the short events only — the long ones are fetched
          // above and drawing them twice would double-count the colour.
          : `SELECT rank, stream,
                    ((t0_ns - ${origin} - ${lo}) * ${PX} / ${Math.max(1, Math.round(span))})::INTEGER AS px,
                    count(*)::BIGINT AS n,
                    max(t1_ns - t0_ns)::BIGINT AS dmax,
                    coalesce(sum(bytes), 0)::BIGINT AS bytes
             FROM timeline_span
             WHERE ${w} AND t1_ns - t0_ns < ${bucketNs} GROUP BY 1, 2, 3`);
      const [r, lng] = await Promise.all([rp, lp]);
      // Set together: a frame that read `rows` against the wrong `qview` would
      // reintroduce exactly the misplacement this pair exists to prevent.
      mode = m; rows = r; longRows = lng;
      qview = { lo, span: Math.max(1, Math.round(span)) }; error = null;
    } catch (e) {
      error = e.message;
    } finally {
      inflight = false; busy = false;
      if (pending) run();
    }
  }
  $effect(() => { if (view) { writeHash(view); refresh(); } });

  const rowY = (i) => AXIS_H + i * ROW_H;
  const height = $derived(AXIS_H + ranks.length * ROW_H + 8);
  const laneH = $derived(Math.max(3, Math.min(LANE_H,
    (ROW_H - 2 * ROW_PAD - (lanes.length - 1) * LANE_GAP) / Math.max(1, lanes.length))));
  const laneY = (li) => ROW_PAD + li * (laneH + LANE_GAP);

  // ns -> canvas x, and back. Every interaction is expressed through these two
  // so pan, zoom and hover cannot disagree about where a time is.
  const plotW = $derived(Math.max(80, width - LEFT - 8));
  const toX = (t) => LEFT + ((t - view.lo) / (view.hi - view.lo)) * plotW;
  const toT = (x) => view.lo + ((x - LEFT) / plotW) * (view.hi - view.lo);

  // Already ns-since-origin, so this formats its argument directly.
  function fmtT(d) {
    if (d < 1e3) return `${d.toFixed(0)} ns`;
    if (d < 1e6) return `${(d / 1e3).toFixed(2)} µs`;
    if (d < 1e9) return `${(d / 1e6).toFixed(3)} ms`;
    return `${(d / 1e9).toFixed(4)} s`;
  }
  const fmtDur = (ns) =>
    ns < 1e3 ? `${ns} ns` : ns < 1e6 ? `${(ns / 1e3).toFixed(2)} µs` : `${(ns / 1e6).toFixed(3)} ms`;

  // ---- interaction -------------------------------------------------------
  // Zoom is about the CURSOR, not the centre. Zooming to centre means the thing
  // you are pointing at slides away as you zoom in, and you chase it.
  function onWheel(e) {
    if (!view || !win) return;
    e.preventDefault();
    const span = view.hi - view.lo;
    if (e.shiftKey) {                       // shift-wheel pans
      const d = (e.deltaY || e.deltaX) * span * 0.0015;
      return (view = clamp({ lo: view.lo + d, hi: view.hi + d }, span));
    }
    const t = toT(e.offsetX);
    const k = Math.exp(e.deltaY * 0.0015);
    // A floor of 100 ns: below that the axis is finer than the clock fit is
    // honest, and there is nothing left to resolve.
    const next = Math.min(win.hi - win.lo, Math.max(100, span * k));
    const f = Math.max(0, Math.min(1, (t - view.lo) / span));
    view = clamp({ lo: t - f * next, hi: t + (1 - f) * next }, next);
  }
  function clamp(v, span) {
    if (v.lo < win.lo) v = { lo: win.lo, hi: win.lo + span };
    if (v.hi > win.hi) v = { lo: win.hi - span, hi: win.hi };
    return { lo: Math.max(win.lo, v.lo), hi: Math.min(win.hi, v.hi) };
  }
  let drag = $state(null);   // reactive: the canvas cursor reads it
  function onDown(e) { if (view) drag = { x: e.offsetX, lo: view.lo, hi: view.hi }; }
  function onUp() { drag = null; }
  function onMove(e) {
    if (!view) return;
    if (drag) {
      const d = ((drag.x - e.offsetX) / plotW) * (drag.hi - drag.lo);
      view = clamp({ lo: drag.lo + d, hi: drag.hi + d }, drag.hi - drag.lo);
      hover = null;
      return;
    }
    hover = pick(e.offsetX, e.offsetY);
  }
  const reset = () => win && (view = { ...win });

  // Nearest mark under the cursor, in the lane under the cursor. Hit-testing in
  // DATA space rather than reading pixels back, so it stays correct at any DPR.
  function pick(x, y) {
    if (!rows || !rows.length || x < LEFT) return null;
    const ri = Math.floor((y - AXIS_H) / ROW_H);
    if (ri < 0 || ri >= ranks.length) return null;
    const rank = ranks[ri];
    const li = Math.max(0, Math.min(lanes.length - 1,
      Math.floor((y - AXIS_H - ri * ROW_H - ROW_PAD) / (laneH + LANE_GAP))));
    const stream = lanes[li];
    const t = toT(x);
    let best = null, bestD = Infinity;
    // Density buckets are resolved against the window they were QUERIED for,
    // exactly as the draw does — hit-testing against the drawn viewport would
    // put the cursor on a different bucket than the one under it.
    const q = qview ?? { lo: view.lo, span: view.hi - view.lo };
    for (const r of rows) {
      if (num(r.rank) !== rank || r.stream !== stream) continue;
      const t0 = mode === 'spans' ? num(r.t0_ns) : q.lo + (num(r.px) / PX) * q.span;
      const t1 = mode === 'spans' ? num(r.t1_ns) : t0 + q.span / PX;
      const d = t < t0 ? t0 - t : t > t1 ? t - t1 : 0;
      if (d < bestD) { bestD = d; best = r; }
    }
    // Long spans are drawn over the grid, so they win the hover too — being on
    // top and not hoverable is the combination that reads as a broken tooltip.
    // They carry t0_ns/t1_ns, so the tooltip must render them as spans.
    let bestLong = null;
    for (const r of longRows) {
      if (num(r.rank) !== rank || r.stream !== stream) continue;
      const t0 = num(r.t0_ns), t1 = num(r.t1_ns);
      const d = t < t0 ? t0 - t : t > t1 ? t - t1 : 0;
      if (d <= bestD) { bestD = d; bestLong = r; }
    }
    if (bestLong) return { x, y, rank, stream, r: bestLong, asSpan: true };
    // 6 px of slop, so a 122 ns span is still catchable without pixel-perfect aim.
    if (!best || bestD > ((view.hi - view.lo) / plotW) * 6) return null;
    return { x, y, rank, stream, r: best };
  }

  // ---- render ------------------------------------------------------------
  $effect(() => {
    // read the reactive deps so the effect re-runs on any of them
    rows; view; width; height; lanes; ranks; mode; hover; qview; longRows;
    if (!canvas || !view) return;
    const dpr = devicePixelRatio || 1;
    canvas.width = width * dpr; canvas.height = height * dpr;
    canvas.style.width = `${width}px`; canvas.style.height = `${height}px`;
    const g = canvas.getContext('2d');
    g.setTransform(dpr, 0, 0, dpr, 0, 0);
    g.clearRect(0, 0, width, height);
    const css = getComputedStyle(document.documentElement);
    const tok = (n) => css.getPropertyValue(n).trim();
    const border = tok('--border'), t2 = tok('--text-2'), t3 = tok('--text-3');
    const r = ramp();

    // rank rows + labels
    g.font = '10px ui-monospace, monospace';
    g.textBaseline = 'middle';
    ranks.forEach((rk, i) => {
      const y = rowY(i);
      if (i % 2) { g.fillStyle = tok('--surface-0'); g.fillRect(LEFT, y, plotW, ROW_H); }
      g.fillStyle = t3; g.textAlign = 'right';
      g.fillText(`r${rk}`, LEFT - 6, y + ROW_H / 2);
    });

    // time axis: ticks at a 1/2/5 step, so labels land on numbers a human reads
    const span = view.hi - view.lo;
    const raw = span / 6;
    const p10 = 10 ** Math.floor(Math.log10(raw));
    const step = [1, 2, 5, 10].find((m) => raw <= m * p10) * p10;
    g.strokeStyle = border; g.lineWidth = 1; g.fillStyle = t2; g.textAlign = 'center';
    for (let t = Math.ceil(view.lo / step) * step; t <= view.hi; t += step) {
      const x = Math.round(toX(t)) + 0.5;
      g.beginPath(); g.moveTo(x, AXIS_H - 4); g.lineTo(x, height); g.stroke();
      g.fillText(fmtT(t), x, AXIS_H / 2);
    }

    const rowIx = new Map(ranks.map((v, i) => [v, i]));
    const laneIx = new Map(lanes.map((v, i) => [v, i]));

    // Clip to the plot area. The window selects any span that OVERLAPS it, so
    // a span starting before view.lo has a negative x and would otherwise be
    // painted straight over the rank labels in the gutter.
    g.save();
    g.beginPath();
    g.rect(LEFT, AXIS_H, plotW, height - AXIS_H);
    g.clip();

    if (mode === 'density') {
      // Log scale, for the reason logScale() carries: the corpus spans five
      // orders of magnitude and linear encodes that as two lit cells.
      const ns = rows.map((d) => num(d.n));
      const hi = Math.log(Math.max(1, Math.max(...ns)));
      // Bucket width in TIME, converted through the current view — so a stale
      // frame drawn at a new zoom scales instead of lying about its resolution.
      // Falls back to the drawn viewport if a frame somehow arrives without one.
      const q = qview ?? { lo: view.lo, span: view.hi - view.lo };
      const bucketNs = q.span / PX;
      const bw = Math.max(1, (bucketNs / (view.hi - view.lo)) * plotW);
      for (const d of rows) {
        const ri = rowIx.get(num(d.rank)), li = laneIx.get(d.stream);
        if (ri === undefined || li === undefined) continue;
        const t = Math.min(1, Math.log(Math.max(1, num(d.n))) / (hi || 1));
        g.fillStyle = r[Math.round(t * (r.length - 1))];
        g.fillRect(toX(q.lo + num(d.px) * bucketNs), rowY(ri) + laneY(li), bw, laneH);
      }
      // The long ones, at their true extent and on top — a kernel running
      // through the whole window is the most important thing in it, and it is
      // exactly what bucketing by start time cannot say.
      g.fillStyle = r[isDark() ? 2 : r.length - 4];
      for (const d of longRows) {
        const ri = rowIx.get(num(d.rank)), li = laneIx.get(d.stream);
        if (ri === undefined || li === undefined) continue;
        const x0 = toX(num(d.t0_ns));
        g.fillRect(x0, rowY(ri) + laneY(li), Math.max(1, toX(num(d.t1_ns)) - x0), laneH);
      }
    } else {
      // Spans at their true width, floored at 1 px so a 122 ns mark in a 3 ms
      // window is still visible rather than rounding away to nothing.
      g.fillStyle = r[isDark() ? 2 : r.length - 4];
      for (const d of rows) {
        const ri = rowIx.get(num(d.rank)), li = laneIx.get(d.stream);
        if (ri === undefined || li === undefined) continue;
        const x0 = toX(num(d.t0_ns));
        g.fillRect(x0, rowY(ri) + laneY(li), Math.max(1, toX(num(d.t1_ns)) - x0), laneH);
      }
    }
    g.restore();

    if (hover) {
      const x = Math.round(hover.x) + 0.5;
      g.strokeStyle = tok('--text-1'); g.globalAlpha = 0.4;
      g.beginPath(); g.moveTo(x, AXIS_H); g.lineTo(x, height); g.stroke();
      g.globalAlpha = 1;
    }
  });

  $effect(() => {
    if (!wrap) return;
    const ro = new ResizeObserver(([e]) => (width = e.contentRect.width));
    ro.observe(wrap);
    return () => ro.disconnect();
  });

  // A flight span's width IS the completion latency, and calling it "dur" makes
  // the one lane that answers "how long did this take" read like the eight that
  // answer "how long did this call take".
  const durLabel = (stream) => (stream === 'ib_wr_flight' ? 'latency' : 'dur');

  const tip = $derived.by(() => {
    if (!hover) return null;
    const { r: d, rank, stream, asSpan } = hover;
    const head = `rank ${rank} · ${SHORT[stream] ?? stream}`;
    // `asSpan` is set for a long span picked out of the density grid: it holds
    // t0/t1, not a bucket, and reading it as a bucket would print `t NaN`.
    if (mode === 'spans' || asSpan) {
      const dur = num(d.t1_ns) - num(d.t0_ns);
      // `ep` is whatever endpoint the stream is keyed on, and naming it "qp"
      // unconditionally mislabels the two lanes that are not queue pairs.
      const epLabel = stream === 'gpu_kernel' ? 'gpu'
                    : stream === 'gpu_access' ? 'dev'
                    : 'qp';
      return [head, `t ${fmtT(num(d.t0_ns))}`, `${durLabel(stream)} ${fmtDur(dur)}`,
              `${epLabel} ${d.ep}`,
              d.bytes === null ? null : bytes(num(d.bytes))].filter(Boolean);
    }
    return [head, `t ${fmtT(view.lo + (num(d.px) / PX) * (view.hi - view.lo))}`,
            `${count(num(d.n))} events`, `max ${durLabel(stream)} ${fmtDur(num(d.dmax))}`,
            num(d.bytes) ? bytes(num(d.bytes)) : null].filter(Boolean);
  });

  const zoomLabel = $derived(
    view && win ? `${fmtDur(view.hi - view.lo)} of ${fmtDur(win.hi - win.lo)}` : '');
</script>

<Card
  title="Timeline"
  subtitle={rows && rows.length
    ? (mode === 'spans'
        ? `each mark is one event · ${zoomLabel}`
        : `each mark is a bin of events, colour is count (log) · ${zoomLabel}`)
    : ''}
>
  {#if error}
    <p class="text-[13px] m-0" style="color: var(--critical)">{error}</p>
    <p class="text-xs mt-1 m-0" style="color: var(--text-2)">
      Needs: any event stream, and a clock_anchor per rank.
    </p>
  {:else if rows === null}
    <p class="text-[13px] m-0" style="color: var(--text-2)">loading…</p>
  {:else if !rows.length && !win}
    <p class="text-[13px] m-0" style="color: var(--text-2)">
      Nothing to show — this capture has no such data.
    </p>
  {:else}
    <div class="flex items-center gap-3 mb-2 text-xs" style="color: var(--text-2)">
      <button
        class="rounded-md px-2 py-1 border"
        style="border-color: var(--border); background: var(--surface-1); color: var(--text-2)"
        onclick={reset}>reset zoom</button>
      <span>wheel = zoom · drag / shift-wheel = pan</span>
      {#if busy}<span style="color: var(--text-3)">…</span>{/if}
      <span class="ml-auto flex gap-2">
        {#each lanes as s}<span style="color: var(--text-3)">{SHORT[s] ?? s}</span>{/each}
      </span>
    </div>

    <div bind:this={wrap} class="relative w-full select-none">
      <canvas
        bind:this={canvas}
        class="block w-full"
        style="cursor: {drag ? 'grabbing' : 'crosshair'}"
        onwheel={onWheel}
        onpointerdown={onDown}
        onpointerup={onUp}
        onpointerleave={() => { drag = null; hover = null; }}
        onpointermove={onMove}
      ></canvas>

      {#if tip}
        <div
          class="absolute pointer-events-none rounded-md border px-2 py-1 text-[11px] leading-snug whitespace-nowrap"
          style="left: {Math.min(hover.x + 12, width - 150)}px; top: {hover.y + 12}px;
                 background: var(--surface-1); border-color: var(--border); color: var(--text-1)"
        >
          {#each tip as line, i}
            <div style={i ? 'color: var(--text-2)' : ''}>{line}</div>
          {/each}
        </div>
      {/if}
    </div>

    <!-- The hand-off is not deprecated by this panel. This one answers "is the
         shape right"; Perfetto answers "what exactly happened at t", and it has
         the filtering, search and flow arrows this one does not. -->
    <p class="text-[11px] mt-3 mb-0" style="color: var(--text-3)">
      Forensics, filtering and search still belong to Perfetto —
      <code>comshu-read &lt;trace-dir&gt; --pftrace out.pftrace</code>, then
      <a href="https://ui.perfetto.dev" target="_blank" rel="noreferrer"
         style="color: var(--text-2)">ui.perfetto.dev</a>.
    </p>
  {/if}
</Card>
