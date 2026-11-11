#!/usr/bin/env node
// One question, one answer, no UI.
//
//   node agent/ask.mjs "which rank pair moved the most bytes?"
//   node agent/ask.mjs --ui http://127.0.0.1:8777 --json "how many QPs did rank 0 open?"
//
// This is the half of the chat panel that can be tested without a browser, and
// it exists first for that reason: prompt work and model choice are the risky
// part, and neither needs a Svelte component to evaluate.

import { Capture } from './lib/query.mjs';
import { buildSystemPrompt } from './lib/prompt.mjs';
import { PiSession } from './lib/session.mjs';

function parseArgs(argv) {
  const opts = { ui: process.env.COMSHU_UI_URL ?? 'http://127.0.0.1:8777', json: false, question: '' };
  const rest = [];
  for (let i = 0; i < argv.length; i++) {
    const a = argv[i];
    if (a === '--ui') opts.ui = argv[++i];
    else if (a === '--model') opts.model = argv[++i];
    else if (a === '--provider') opts.provider = argv[++i];
    else if (a === '--json') opts.json = true;
    else if (a === '--print-prompt') opts.printPrompt = true;
    else if (a === '-h' || a === '--help') opts.help = true;
    else rest.push(a);
  }
  opts.question = rest.join(' ');
  return opts;
}

const usage = `usage: node agent/ask.mjs [options] "<question>"

  --ui <url>        a running \`comshu ui\` (default $COMSHU_UI_URL or http://127.0.0.1:8777)
  --model <name>    override pi's configured model
  --provider <name> override pi's configured provider
  --json            print one JSON object instead of a transcript
  --print-prompt    print the system prompt for this capture and exit
`;

const opts = parseArgs(process.argv.slice(2));
if (opts.help || (!opts.question && !opts.printPrompt)) {
  process.stdout.write(usage);
  process.exit(opts.help ? 0 : 1);
}

const capture = new Capture(opts.ui);
let systemPrompt;
try {
  systemPrompt = await buildSystemPrompt(capture);
} catch (e) {
  process.stderr.write(`cannot read the capture at ${opts.ui}: ${e.message}\n` + 'is `comshu ui <capture>` running?\n');
  process.exit(2);
}

if (opts.printPrompt) {
  process.stdout.write(systemPrompt);
  process.exit(0);
}

const session = new PiSession({
  uiUrl: opts.ui,
  systemPrompt,
  provider: opts.provider,
  model: opts.model,
}).start();

const dim = (s) => (process.stdout.isTTY ? `\x1b[2m${s}\x1b[0m` : s);

try {
  const result = await session.ask(opts.question, (ev) => {
    if (opts.json) return;
    if (ev.kind === 'text') process.stdout.write(ev.delta);
    else if (ev.kind === 'tool' && ev.name === 'sql') process.stdout.write(dim(`\n[sql] ${ev.args.query}\n`));
    else if (ev.kind === 'tool' && ev.name === 'schema') process.stdout.write(dim(`\n[schema] ${ev.args.table ?? '(all tables)'}\n`));
    else if (ev.kind === 'chart') process.stdout.write(dim(`\n[chart] ${ev.chart.title} — ${ev.chart.rows} rows\n`));
    else if (ev.kind === 'tool_end' && !ev.ok) process.stdout.write(dim(`\n[${ev.name} failed] ${ev.summary}\n`));
    else if (ev.kind === 'error') process.stderr.write(`\n${ev.message}\n`);
  });

  if (opts.json) process.stdout.write(`${JSON.stringify(result, null, 2)}\n`);
  else process.stdout.write('\n');
  session.stop();
  process.exit(result.error ? 1 : 0);
} catch (e) {
  process.stderr.write(`\n${e.message}\n`);
  session.stop();
  process.exit(1);
}
