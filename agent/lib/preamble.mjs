// What is actually in THIS capture, computed once at session start.
//
// The catalog describes captures in general; this describes the one in front of
// the model. It is the difference between a model that knows `source_line` is
// empty here and one that writes a beautiful query against nothing, and it
// costs five round trips at startup.

const PROVIDER_EVIDENCE = [
  ['ibverbs (host-posted InfiniBand)', 'ib_post_send'],
  ['mlx5 DEVX (IBGDA device-issued, or ibdevx through the send-queue watcher)', 'mlx5_device_send'],
  ['gpumem (GPU memory access, NVLink peer traffic)', 'gpu_access'],
  ['gpumem source attribution (PC -> line)', 'source_line'],
];

/** Every table's name and row count, in one round trip. */
export async function tableCounts(capture) {
  const names = (
    await capture.rows(
      `SELECT view_name AS t FROM duckdb_views() WHERE NOT internal
       UNION ALL SELECT table_name FROM duckdb_tables() ORDER BY 1`,
    )
  )
    .map((r) => r.t)
    .filter((t) => !t.startsWith('_'));

  const union = names.map((t) => `SELECT '${t}' AS t, count(*) AS n FROM "${t}"`).join(' UNION ALL ');
  const rows = await capture.rows(`SELECT t, n FROM (${union}) ORDER BY n DESC, t`);
  return rows.map((r) => ({ table: r.t, rows: Number(r.n) }));
}

export async function describe(capture) {
  const counts = await tableCounts(capture);
  const byName = Object.fromEntries(counts.map((c) => [c.table, c.rows]));
  const nonEmpty = counts.filter((c) => c.rows > 0);
  const empty = counts.filter((c) => c.rows === 0).map((c) => c.table);

  // Ranks that appear as either end of an edge; -1 is "unresolved peer", not a rank.
  const ranks = Number(
    (await capture.scalar(
      `SELECT count(DISTINCT rank) FROM (
         SELECT src_rank AS rank FROM comm_edges UNION SELECT dst_rank FROM comm_edges
       ) WHERE rank >= 0`,
    )) ?? 0,
  );
  const nodes = byName.node_identity
    ? Number(await capture.scalar('SELECT count(DISTINCT node_hash) FROM node_identity'))
    : null;

  let span = null;
  if (byName.timeline_span) {
    const r = (
      await capture.rows(
        'SELECT max(t1_ns) - min(t0_ns) AS ns, count(DISTINCT stream) AS streams, count(*) AS n FROM timeline_span',
      )
    )[0];
    span = { ns: Number(r.ns), streams: Number(r.streams), spans: Number(r.n) };
  }

  // Both severities are fetched and they are rendered separately below. A model
  // told "wr_flight_unretired = 69,470" with no counterweight will build an
  // answer around a broken capture; the same model shown the 'note' row beside
  // it — the queue pairs declared they do not signal every work request — has
  // the explanation in the same paragraph as the alarming number.
  const health = byName.link_health
    ? await capture.rows(
        'SELECT finding, severity, n, note FROM link_health WHERE n > 0 ORDER BY n DESC',
      )
    : [];

  const providers = PROVIDER_EVIDENCE.filter(([, t]) => (byName[t] ?? 0) > 0).map(([label]) => label);

  return { counts, nonEmpty, empty, ranks, nodes, span, health, providers };
}

export function renderPreamble(d) {
  const out = ['## This capture', ''];

  out.push(
    `- ranks: ${d.ranks || 'unknown'}${d.nodes !== null ? `, physical nodes: ${d.nodes}` : ''}`,
  );
  out.push(`- providers that produced data: ${d.providers.length ? d.providers.join('; ') : 'none detected'}`);
  if (d.span) {
    const ms = d.span.ns / 1e6;
    out.push(
      `- timeline: ${d.span.spans} spans across ${d.span.streams} stream kind(s), ` +
        `wall duration ${ms >= 1 ? `${ms.toFixed(1)} ms` : `${d.span.ns} ns`}`,
    );
  }

  out.push('', '**Tables with rows** (everything else is empty in this capture):', '');
  for (const c of d.nonEmpty) out.push(`- \`${c.table}\` — ${c.rows.toLocaleString('en-US')} rows`);

  if (d.empty.length) {
    out.push('', `**Empty here:** ${d.empty.map((t) => `\`${t}\``).join(', ')}.`);
    out.push('Querying one of these is not an error, but it answers nothing — say the capture lacks that data.');
  }

  const defects = d.health.filter((h) => h.severity === 'defect');
  const notes = d.health.filter((h) => h.severity !== 'defect');

  if (defects.length) {
    out.push('', '**`link_health` is not clean.** These findings are non-zero and may explain odd numbers:', '');
    for (const h of defects) out.push(`- \`${h.finding}\` = ${h.n} — ${h.note}`);
  } else {
    out.push('', '`link_health` reports no defect: attribution is clean in this capture.');
  }

  // Said explicitly, because the failure this guards against is a model
  // reporting a healthy capture as broken. These counts are the capture
  // accounting for itself — a queue pair that declared it does not signal every
  // work request, a kernel timed at submission, a forced version guard.
  if (notes.length) {
    out.push(
      '',
      '**`link_health` notes** — non-zero and NOT defects. They explain the numbers above, ' +
        'or bound what a table can contain. Do not report these as problems:',
      '',
    );
    for (const h of notes) out.push(`- \`${h.finding}\` = ${h.n} — ${h.note}`);
  }

  return out.join('\n');
}
