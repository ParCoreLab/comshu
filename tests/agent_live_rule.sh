#!/bin/bash
# agent_live_rule.sh — the agent's tests against a LIVE `comshu ui`.
#
#   agent_live_rule.sh <comshu> <capture-dir>
#
# agent/test/lib.test.mjs skips its server half when nothing listens on
# COMSHU_UI_URL, so a plain `node --test` run went green with three tests
# never run. This starts the server on a real capture (the checked-in
# ib_pingpong one, which has the derived tables those tests query), points the
# tests at it, and fails if any test skipped.
set -u
COMSHU=${1:?usage: agent_live_rule.sh <comshu> <capture-dir>}
CAP=${2:?usage: agent_live_rule.sh <comshu> <capture-dir>}
ROOT=$(cd "$(dirname "$0")/.." && pwd)
command -v node >/dev/null || { echo "SKIP: no node on PATH"; exit 0; }

PORT=$(python3 -c 'import socket; s=socket.socket(); s.bind(("127.0.0.1",0)); print(s.getsockname()[1])')
LOG=$(mktemp)
"$COMSHU" ui "$CAP" --port "$PORT" > "$LOG" 2>&1 &
UI_PID=$!
trap 'kill $UI_PID 2>/dev/null; rm -f "$LOG"' EXIT
for _ in $(seq 100); do
  curl -s -o /dev/null "http://127.0.0.1:$PORT/" && break
  sleep 0.1
done
kill -0 "$UI_PID" 2>/dev/null || { echo "FAIL: comshu ui did not start"; cat "$LOG"; exit 1; }

out=$(cd "$ROOT/agent" && COMSHU_UI_URL="http://127.0.0.1:$PORT" node --test test/*.test.mjs 2>&1)
rc=$?
echo "$out" | grep -E '^(not )?ok|^# (tests|pass|fail|skipped)'
skipped=$(echo "$out" | sed -n 's/^# skipped //p')
[ "$rc" -eq 0 ] || { echo "FAIL: agent tests failed"; exit 1; }
[ "${skipped:-0}" -eq 0 ] || { echo "FAIL: $skipped test(s) skipped with a live server up"; exit 1; }
echo "PASS: agent tests, none skipped, against a live comshu ui"
