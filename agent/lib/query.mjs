// The sidecar's half of the one data path.
//
// Every read the agent performs goes through `comshu ui`'s POST /query, the same
// endpoint the browser panels use. The sidecar has no DuckDB of its own and
// must not grow one: two engines over one capture is how two answers to "how
// many bytes" start existing, and the second one is always the wrong one.

const CELL_CHARS = 200; // a single cell wider than this is truncated in the model's view

/**
 * Lex SQL just far enough to know what is code: comments are dropped, string
 * literals ('…', '' escaped) and quoted identifiers ("…") are kept verbatim
 * and never looked inside. The regex version this replaces (CR-65) stripped a
 * `--` or `/*` that sat inside a string — `WHERE note = 'a--b'` lost its tail —
 * and counted a `;` inside a string as a second statement.
 *
 * Returns the code with comments removed, and the index (in that code) of
 * every `;` outside a literal.
 */
function lex(sql) {
  let out = '';
  const semis = [];
  for (let i = 0; i < sql.length; ) {
    const c = sql[i];
    if (c === "'" || c === '"') {
      let j = i + 1;
      for (;;) {
        if (j >= sql.length) break;
        if (sql[j] === c) {
          if (sql[j + 1] === c) { j += 2; continue; } // doubled quote: escaped
          j += 1;
          break;
        }
        j += 1;
      }
      out += sql.slice(i, j);
      i = j;
    } else if (c === '-' && sql[i + 1] === '-') {
      const nl = sql.indexOf('\n', i);
      i = nl === -1 ? sql.length : nl;
    } else if (c === '/' && sql[i + 1] === '*') {
      const end = sql.indexOf('*/', i + 2);
      out += ' ';
      i = end === -1 ? sql.length : end + 2;
    } else {
      if (c === ';') semis.push(out.length);
      out += c;
      i += 1;
    }
  }
  return { code: out, semis };
}

/** Strip SQL comments and a trailing `;`, never touching string literals. */
export function stripComments(sql) {
  const { code, semis } = lex(sql);
  let s = code;
  // Trailing semicolons only (outside literals): `SELECT 1;` is one statement.
  for (let k = semis.length - 1; k >= 0 && !s.slice(semis[k] + 1).trim(); k -= 1)
    s = s.slice(0, semis[k]) + s.slice(semis[k] + 1);
  return s.trim();
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
  if (lex(s).semis.length)
    return 'one statement at a time — remove the `;` and the statements after it';
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
