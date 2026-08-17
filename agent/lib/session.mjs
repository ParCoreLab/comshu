// One conversation with one `pi --mode rpc` child.
//
// The child is long-lived: a chat pane is a conversation, and restarting the
// process per question would throw away both the history and the ~15 KB system
// prompt the model has already read.
//
// Framing note, from pi's own docs and worth repeating because getting it wrong
// is silent: RPC is strict JSONL delimited by LF. Node's `readline` also splits
// on U+2028/U+2029, which are legal inside JSON strings — a node name in a
// capture containing one would desynchronise the stream. So we split on \n
// ourselves.

import { spawn } from 'node:child_process';
import { dirname, join } from 'node:path';
import { fileURLToPath } from 'node:url';

const here = dirname(fileURLToPath(import.meta.url));
const EXTENSION = join(here, '..', 'pcomm-agent.ts');

/**
 * The model this agent is built and measured against.
 *
 * Pinned rather than inherited from pi's own default, because the catalog is
 * tuned against a specific model and the eval numbers in the README mean
 * nothing if the model can change underneath them without anyone noticing.
 * Chosen 2026-08-17 over `gemma4-31b-qat` — the two tie on the suite (22/23
 * each) and gemma is 2–3× faster, but glimmer was the more reliable of the two
 * on questions whose trap is a wrong *measure* with right arithmetic, which is
 * this agent's characteristic failure. The comparison, and how thin its
 * evidence is, is in the plan.
 *
 * Override per-run with `--model`, or globally with $PCOMM_AGENT_MODEL.
 */
export const DEFAULT_MODEL = process.env.PCOMM_AGENT_MODEL ?? 'muse-glimmer-30b';

export class PiSession {
  /**
   * @param {object} opts
   * @param {string} opts.uiUrl        base URL of the running `pcomm ui`
   * @param {string} opts.systemPrompt catalog + this capture + conduct
   * @param {string} [opts.provider]   defaults to pi's configured provider
   * @param {string} [opts.model]
   */
  constructor({ uiUrl, systemPrompt, provider, model }) {
    this.uiUrl = uiUrl;
    this.systemPrompt = systemPrompt;
    this.provider = provider;
    this.model = model ?? DEFAULT_MODEL;
    this.child = null;
    this.buf = '';
    this.listeners = new Set();
    this.exited = null;
  }

  start() {
    const args = [
      '--mode', 'rpc',
      '--no-session',
      '--no-builtin-tools',
      '--no-extensions', // discovery off: this agent runs exactly the tools below
      '--extension', EXTENSION,
      '--system-prompt', this.systemPrompt,
    ];
    if (this.provider) args.push('--provider', this.provider);
    if (this.model) args.push('--model', this.model);

    this.child = spawn('pi', args, {
      stdio: ['pipe', 'pipe', 'pipe'],
      env: { ...process.env, PCOMM_UI_URL: this.uiUrl },
    });
    this.stderr = '';
    this.child.stderr.on('data', (d) => {
      this.stderr = (this.stderr + d.toString()).slice(-4000);
    });
    this.child.stdout.on('data', (d) => this.#onData(d));
    this.exited = new Promise((resolve) => {
      this.child.on('exit', (code) => {
        this.child = null;
        resolve(code);
      });
    });
    return this;
  }

  #onData(chunk) {
    this.buf += chunk.toString();
    let i;
    while ((i = this.buf.indexOf('\n')) !== -1) {
      const line = this.buf.slice(0, i).replace(/\r$/, '');
      this.buf = this.buf.slice(i + 1);
      if (!line.trim()) continue;
      let msg;
      try {
        msg = JSON.parse(line);
      } catch {
        continue; // not protocol traffic; pi prints nothing else, but be safe
      }
      for (const fn of this.listeners) fn(msg);
    }
  }

  send(cmd) {
    if (!this.child) throw new Error('the agent process is not running');
    this.child.stdin.write(`${JSON.stringify(cmd)}\n`);
  }

  /**
   * Ask one question. `onEvent` receives normalised UI events as they happen:
   *
   *   { kind: 'text',  delta }          assistant prose, streaming
   *   { kind: 'tool',  name, args }     a tool started
   *   { kind: 'tool_end', name, ok, summary }
   *   { kind: 'chart', chart }          { title, sql, spec, rows }
   *   { kind: 'error', message }
   *
   * Resolves when the turn settles, with everything collected.
   */
  ask(message, onEvent = () => {}) {
    const state = { text: '', charts: [], queries: [], error: null };

    return new Promise((resolve, reject) => {
      const done = (fn, arg) => {
        this.listeners.delete(listener);
        clearTimeout(idle);
        fn(arg);
      };

      // A local 30B can think for a while; it should not think forever.
      let idle = setTimeout(() => done(reject, new Error('the model did not respond within 10 minutes')), 600000);
      const touch = () => {
        clearTimeout(idle);
        idle = setTimeout(() => done(reject, new Error('the model stopped responding')), 600000);
      };

      const listener = (msg) => {
        touch();
        switch (msg.type) {
          case 'message_update': {
            const ev = msg.assistantMessageEvent;
            if (ev?.type === 'text_delta' && ev.delta) {
              state.text += ev.delta;
              onEvent({ kind: 'text', delta: ev.delta });
            }
            break;
          }
          case 'tool_execution_start':
            if (msg.toolName === 'sql' && msg.args?.query) state.queries.push(msg.args.query);
            onEvent({ kind: 'tool', name: msg.toolName, args: msg.args ?? {} });
            break;
          case 'tool_execution_end': {
            const chart = msg.result?.details?.chart;
            if (chart) {
              state.charts.push(chart);
              onEvent({ kind: 'chart', chart });
            }
            onEvent({
              kind: 'tool_end',
              name: msg.toolName,
              ok: !msg.isError,
              summary: msg.result?.content?.map((c) => c.text).filter(Boolean).join('\n') ?? '',
            });
            break;
          }
          case 'auto_retry_end':
            if (msg.success === false) {
              state.error = msg.finalError ?? 'the model call failed';
              onEvent({ kind: 'error', message: state.error });
            }
            break;
          case 'extension_error':
            state.error = msg.error ?? 'extension error';
            onEvent({ kind: 'error', message: state.error });
            break;
          case 'agent_settled':
            if (!state.text && !state.charts.length && !state.error) {
              state.error = this.stderr?.trim()
                ? `the model produced nothing. pi said:\n${this.stderr.trim()}`
                : 'the model produced nothing';
            }
            done(resolve, state);
            break;
          default:
            break;
        }
      };

      this.listeners.add(listener);
      try {
        this.send({ type: 'prompt', message });
      } catch (e) {
        done(reject, e);
      }
    });
  }

  abort() {
    try {
      this.send({ type: 'abort' });
    } catch {
      /* not running */
    }
  }

  stop() {
    this.child?.stdin.end();
    this.child?.kill();
  }
}
