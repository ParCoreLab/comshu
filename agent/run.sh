#!/usr/bin/env bash
# Start a capture and its agent together, and print the one URL to open.
#
#   agent/run.sh <capture-or-export-dir> [ui-port]
#
# Two processes to start is the kind of friction that makes a feature go unused,
# so this exists purely to remove it. Ctrl-C stops both.
set -euo pipefail

here="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
root="$(dirname "$here")"

if [ $# -lt 1 ]; then
    echo "usage: agent/run.sh <capture-or-export-dir> [ui-port]" >&2
    exit 1
fi
capture="$1"
port="${2:-8777}"
agent_port=$((port + 1))

pcomm="${PCOMM:-}"
if [ -z "$pcomm" ]; then
    for candidate in "$root"/build*/pcomm/pcomm; do
        [ -x "$candidate" ] || continue
        # `pcomm ui` only exists in a build configured with DuckDB. Note the
        # capture into a variable: `pcomm` with no args exits 1, and under
        # `pipefail` a `| grep -q` pipeline inherits that and reads as "no".
        verbs="$("$candidate" 2>&1 || true)"
        case "$verbs" in
            *"  ui "*)
                pcomm="$candidate"
                break
                ;;
        esac
    done
fi
if [ -z "$pcomm" ]; then
    echo "no pcomm binary with a \`ui\` verb found under $root/build*; set PCOMM=<path>" >&2
    exit 1
fi

command -v pi >/dev/null || {
    echo "pi is not on PATH — npm i -g @earendil-works/pi-coding-agent" >&2
    exit 1
}

cleanup() { kill 0 2>/dev/null || true; }
trap cleanup EXIT INT TERM

"$pcomm" ui "$capture" --port "$port" &

# The bridge reads the capture through the server, so it has to be up first.
for _ in $(seq 1 60); do
    if curl -fsS -m 2 -X POST "http://127.0.0.1:$port/query" -d 'SELECT 1' >/dev/null 2>&1; then break; fi
    sleep 1
done

node "$here/bridge.mjs" --ui "http://127.0.0.1:$port" --port "$agent_port" &

echo
echo "  open  http://127.0.0.1:$port/?agent=http://127.0.0.1:$agent_port"
echo
wait
