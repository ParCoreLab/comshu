#!/usr/bin/env bash
# Start a capture and its agent together, and print the one URL to open.
#
#   agent/run.sh <capture-or-export-dir> [ui-port] [--bind ADDR]
#
# Two processes to start is the kind of friction that makes a feature go unused,
# so this exists purely to remove it. Ctrl-C stops both.
#
# --bind serves both halves on an address other than loopback, for reaching the
# page from another machine on a private network. It is one flag here because it
# is three things underneath — the server's bind, the bridge's bind, and the
# bridge's --ui — and getting any one of them wrong fails in a way that looks
# like something else (see the checks in bridge.mjs). There is no authentication
# on either half; anyone who can route to ADDR can read the capture and drive
# the model.
set -euo pipefail

here="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
root="$(dirname "$here")"

capture=""
port=8777
bind=127.0.0.1
while [ $# -gt 0 ]; do
    case "$1" in
        --bind)
            bind="$2"
            shift 2
            ;;
        -h | --help)
            echo "usage: agent/run.sh <capture-or-export-dir> [ui-port] [--bind ADDR]" >&2
            exit 0
            ;;
        *)
            if [ -z "$capture" ]; then capture="$1"; else port="$1"; fi
            shift
            ;;
    esac
done
if [ -z "$capture" ]; then
    echo "usage: agent/run.sh <capture-or-export-dir> [ui-port] [--bind ADDR]" >&2
    exit 1
fi
agent_port=$((port + 1))

# Where the BROWSER will reach each half. Not the same as the bind address only
# in that a bound 0.0.0.0 has to be spoken to by a routable name; every other
# case they coincide. The bridge is told this too — the page refuses a bridge
# that names an origin other than its own.
host="$bind"
[ "$host" = "0.0.0.0" ] && host=127.0.0.1
ui_url="http://$host:$port"

comshu="${COMSHU:-}"
if [ -z "$comshu" ]; then
    for candidate in "$root"/build*/comshu/comshu; do
        [ -x "$candidate" ] || continue
        # `comshu ui` only exists in a build configured with DuckDB. Note the
        # capture into a variable: `comshu` with no args exits 1, and under
        # `pipefail` a `| grep -q` pipeline inherits that and reads as "no".
        verbs="$("$candidate" 2>&1 || true)"
        case "$verbs" in
            *"  ui "*)
                comshu="$candidate"
                break
                ;;
        esac
    done
fi
if [ -z "$comshu" ]; then
    echo "no comshu binary with a \`ui\` verb found under $root/build*; set COMSHU=<path>" >&2
    exit 1
fi

command -v pi >/dev/null || {
    echo "pi is not on PATH — npm i -g @earendil-works/pi-coding-agent" >&2
    exit 1
}

cleanup() { kill 0 2>/dev/null || true; }
trap cleanup EXIT INT TERM

remote_args=()
if [ "$bind" != "127.0.0.1" ] && [ "$bind" != "localhost" ] && [ "$bind" != "::1" ]; then
    remote_args=(--allow-remote)
    echo "!! serving on $bind — no authentication on either half." >&2
fi

"$comshu" ui "$capture" --port "$port" --bind "$bind" &

# The bridge reads the capture through the server, so it has to be up first.
for _ in $(seq 1 60); do
    if curl -fsS -m 2 -X POST "$ui_url/query" -d 'SELECT 1' >/dev/null 2>&1; then break; fi
    sleep 1
done

node "$here/bridge.mjs" --ui "$ui_url" --port "$agent_port" --bind "$bind" "${remote_args[@]}" &

echo
echo "  open  $ui_url/?agent=http://$host:$agent_port"
echo
wait
