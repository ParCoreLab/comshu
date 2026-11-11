// The sidecar's half of the one data path.
//
// Every read the agent performs goes through `comshu ui`'s POST /query, the same
// endpoint the browser panels use. The sidecar has no DuckDB of its own and
// must not grow one: two engines over one capture is how two answers to "how
// many bytes" start existing, and the second one is always the wrong one.

const CELL_CHARS = 200; // a single cell wider than this is truncated in the model's view

/** Strip SQL comments that would confuse the server's first-token check. */
export function stripComments(sql) {
  return sql
    .replace(/\/\*[\s\S]*?\*\//g, ' ')
    .split('\n')
    .map((l) => l.replace(/--.*$/, ''))
    .join('\n')
    .trim()
    .replace(/;+\s*$/, '');
}

/**
 * Reject anything that is not a single read.
 *
 * `comshu ui` does its own first-token check, so this is not the only guard —
 * but its error text is written for a human pasting into the Ask panel, and a
 * model that gets "use `comshu export` to write anything" back from a stray
 * semicolon will not work out what to do. Refusing here says why.
 */
export function checkReadOnly(sql) {
  const s = stripComments(sql);
  if (!s) return 'empty query';
  if (!/^(select|with)\b/i.test(s)) return 'only SELECT and WITH queries can be run against a capture';
  if (s.includes(';')) return 'one statement at a time — remove the `;` and the statements after it';
  return null;
}

export class Capture {
  /** @param {string} url base URL of a running `comshu ui` */
  constructor(url) {
    this.url = url.replace(/\/+$/, '');
  }

  /** Raw query. Returns { columns, rows } with every value a string or null. */
  async raw(sql, { timeoutMs = 60000 } = {}) {
    const res = await fetch(`${this.url}/query`, {
      method: 'POST',
      body: sql,
      signal: AbortSignal.timeout(timeoutMs),
    });
    let payload;
    try {
      payload = await res.json();
    } catch {
      throw new Error(`comshu ui returned ${res.status} with no JSON body`);
    }
    if (payload.error) throw new Error(payload.error);
    return payload;
  }

  /** One scalar, or null. */
  async scalar(sql) {
    const { rows } = await this.raw(sql);
    return rows.length ? rows[0][0] : null;
  }

  /** Rows as objects. For our own queries, not the model's. */
  async rows(sql) {
    const { columns, rows } = await this.raw(sql);
    return rows.map((r) => Object.fromEntries(columns.map((c, i) => [c, r[i]])));
  }

  /**
   * Run the model's query under a row cap.
   *
   * The cap is the "never sees raw rows" constraint made operational: the agent
   * sees aggregates it asked for, never a capture dump. It is also the context
   * budget — one unguarded scan of `gpu_access` is millions of rows.
   */
  async modelQuery(sql, { limit = 50 } = {}) {
    const bad = checkReadOnly(sql);
    if (bad) throw new Error(bad);
    const inner = stripComments(sql);
    const { columns, rows } = await this.raw(`SELECT * FROM (\n${inner}\n) LIMIT ${limit + 1}`);
    const truncated = rows.length > limit;
    const shown = truncated ? rows.slice(0, limit) : rows;
    // Only pay for the count when the cap actually bit.
    const total = truncated ? Number(await this.scalar(`SELECT count(*) FROM (\n${inner}\n)`)) : rows.length;
    return { columns, rows: shown, total, truncated };
  }
}

/** Render a result the way the model reads best: a fixed-width table. */
export function renderTable({ columns, rows, total, truncated }) {
  if (!rows.length) return 'no rows';
  const cells = rows.map((r) => r.map((v) => (v === null ? 'NULL' : String(v)).slice(0, CELL_CHARS)));
  const width = columns.map((c, i) => Math.max(c.length, ...cells.map((r) => r[i].length)));
  const line = (vals) => vals.map((v, i) => v.padEnd(width[i])).join('  ').trimEnd();
  const out = [line(columns), width.map((w) => '-'.repeat(w)).join('  ')];
  for (const r of cells) out.push(line(r));
  out.push(truncated ? `(${rows.length} of ${total} rows — the query returned more than the cap)` : `(${total} rows)`);
  return out.join('\n');
}
