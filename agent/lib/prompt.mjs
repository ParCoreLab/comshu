// The system prompt: the catalog, what is in this capture, and how to behave.
//
// pi's own coding-agent prompt is replaced wholesale (`--system-prompt`), not
// appended to: this agent has no filesystem, no shell and no repository, and a
// prompt describing tools it does not have is how a model ends up trying to
// `cat` a capture.

import { readFile } from 'node:fs/promises';
import { dirname, join } from 'node:path';
import { fileURLToPath } from 'node:url';
import { describe, renderPreamble } from './preamble.mjs';

const here = dirname(fileURLToPath(import.meta.url));

const CONDUCT = `
## Your tools

- \`schema\` — the tables, their columns, and what they mean. Call it with no
  arguments to see what exists; with a table name for its columns. Cheap. Use it
  rather than guessing a column name.
- \`sql\` — run one SELECT against the capture. Results come back capped at 50
  rows; the cap is reported when it bites. If DuckDB rejects the query you get
  its error verbatim — read it and fix the query, do not restate it to the user.
- \`chart\` — draw a result. Takes \`{ title, sql, spec }\`: the query, and a
  Vega-Lite spec **with no data in it**. The page runs your SQL itself and binds
  the rows into the spec under the name \`rows\`, so every field in the spec must
  be a column name that query returns. One chart per answer, and only when a
  picture answers the question better than a sentence.

## How to work

1. Read the question. If it is quantitative, it is answered by a query, not by
   recall — run one, even when you are fairly sure of the answer.
2. Check \`schema\` when you are not certain a column exists.
3. Prefer one query that answers the question over three that circle it.
4. Then answer in a few sentences: the number, what it means, and anything in
   the capture that qualifies it (unresolved peers, a non-clean \`link_health\`,
   an empty table). Do not narrate the steps you took or list the tools you
   called — show the finding.
5. Never invent a number. If a query failed three times, say what you could not
   compute and why.
`.trim();

export async function buildSystemPrompt(capture) {
  const catalog = await readFile(join(here, '..', 'catalog.md'), 'utf8');
  const described = await describe(capture);
  return `${catalog.trim()}\n\n${renderPreamble(described)}\n\n${CONDUCT}\n`;
}
