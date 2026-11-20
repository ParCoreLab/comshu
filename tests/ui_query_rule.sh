#!/bin/bash
# ui_query_rule.sh — `comshu ui`'s POST /query over real HTTP (the CR-75 audit:
# its guards were tested only as a header, never as the server answers).
#
#   ui_query_rule.sh <comshu> <csv_export_contract_writer>
#
#   CR-01  exactly one read-only SELECT reaches DuckDB: a second statement is
#          refused, a trailing closed comment is not
#   CR-27  each request has its own connection: concurrent queries all answer
#   CR-64  error and value text comes back as valid JSON, quotes and control
#          characters included
set -u
COMSHU=${1:?usage: ui_query_rule.sh <comshu> <writer>}
WRITER=${2:?usage: ui_query_rule.sh <comshu> <writer>}
WORK=$(mktemp -d)
UI_PID=
cleanup() { [ -n "$UI_PID" ] && kill "$UI_PID" 2>/dev/null; rm -rf "$WORK"; }
trap cleanup EXIT

"$WRITER" "$WORK/cap" >/dev/null 2>&1 || { echo "FAIL: writer"; exit 1; }
PORT=$(python3 -c 'import socket; s=socket.socket(); s.bind(("127.0.0.1",0)); print(s.getsockname()[1])')
"$COMSHU" ui "$WORK/cap" --port "$PORT" > "$WORK/ui.log" 2>&1 &
UI_PID=$!
URL="http://127.0.0.1:$PORT/query"
for _ in $(seq 100); do
  curl -s -o /dev/null "http://127.0.0.1:$PORT/" && break
  sleep 0.1
done

fail=0
q() { curl -s -X POST --data-binary "$1" "$URL"; }
# expect <name> <sql> <python predicate over the parsed JSON `j`>
expect() {
  local body
  body=$(q "$2")
  if python3 -c 'import json,sys; j=json.loads(sys.argv[1]); sys.exit(0 if eval(sys.argv[2]) else 1)' \
       "$body" "$3" 2>/dev/null; then
    echo "ok: $1"
  else
    echo "FAIL: $1 — got: $body"; fail=1
  fi
}

expect "a SELECT answers"                 'SELECT 1 AS a'                    'j["rows"] == [["1"]]'
expect "a closed trailing comment is fine" 'SELECT 1; /* done */'            '"rows" in j'
expect "a second statement is refused"    'SELECT 1; DROP TABLE x'           '"error" in j'
expect "a write is refused"               'CREATE TABLE x AS SELECT 1'       '"error" in j'
expect "an open comment is refused"       $'SELECT 1; -- ok\n/* open'        '"error" in j'
expect "quotes and control chars survive" $'SELECT \'a"b\\\\c\n\x01\' AS s'  $'j["rows"] == [["a\\"b\\\\\\\\c\\n\\x01"]]'
expect "a DuckDB error is valid JSON"     'SELECT "no such col"'             '"error" in j'

# Concurrent requests on per-request connections: every one answers.
pids=()
for i in $(seq 16); do
  q "SELECT $i AS n, count(*) FROM range(200000)" > "$WORK/c$i" &
  pids+=($!)
done
wait "${pids[@]}"  # not a bare wait: that would wait for the server too
for i in $(seq 16); do
  if ! python3 -c 'import json,sys; j=json.load(open(sys.argv[1])); sys.exit(0 if j["rows"][0][0]==sys.argv[2] else 1)' \
         "$WORK/c$i" "$i" 2>/dev/null; then
    echo "FAIL: concurrent query $i — got: $(cat "$WORK/c$i")"; fail=1
  fi
done
[ "$fail" -eq 0 ] && echo "ok: 16 concurrent queries each answered their own"

kill -0 "$UI_PID" 2>/dev/null || { echo "FAIL: the server died"; tail -5 "$WORK/ui.log"; fail=1; }
[ "$fail" -eq 0 ] && echo "PASS: /query"
exit "$fail"
