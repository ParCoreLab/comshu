// THE data path. Every panel reaches its data through here and nowhere else.
//
// Two reasons, and the second is the one that costs money later:
//
//  1. A panel that computes something server-side through its own endpoint
//     forks the derived layer, and two panels start disagreeing about total
//     bytes with no single place to fix it.
//  2. The static / DuckDB-WASM build swaps the transport in this file and
//     nowhere else. Hunting `fetch` calls through a grown UI is a different and
//     much worse job than editing one function.
//
// So: no component imports `fetch`. If a panel needs data, it needs SQL.

/** Run a read-only SQL query. Returns an array of plain row objects. */
export async function query(sql) {
  const res = await fetch('/query', { method: 'POST', body: sql });
  let payload;
  try {
    payload = await res.json();
  } catch {
    throw new Error(`server returned ${res.status} with no JSON body`);
  }
  if (payload.error) throw new Error(payload.error);
  const { columns, rows } = payload;
  return rows.map((r) => Object.fromEntries(columns.map((c, i) => [c, r[i]])));
}

// Values arrive as strings, deliberately: a JSON number cannot hold a UBIGINT
// address without dropping the low bits, and those addresses are join keys.
// Panels cast what they actually need, which is almost never the wide columns.
export const num = (v) => (v === null || v === undefined ? null : Number(v));
