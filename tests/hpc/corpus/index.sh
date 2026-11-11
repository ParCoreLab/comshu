#!/usr/bin/env bash
# Build index.tsv — the sizing table every UI design decision reads.
#
# One row per (capture, record type): how many records of that type the whole
# capture holds, across every rank. This is the input to "does the raw event
# table need partitioning by rank", "is a per-address table ever a table", and
# "what does a time bucket cost" — none of which can be answered from pingpong.
#
# Built from `comshu read <dir>` (the stream inventory), which reads each file's
# own header rather than inferring anything from its name, and from
# `comshu types` for the type_id -> name map. Both come out of the one
# Table<> list, so this index cannot drift from what the reader actually decodes.
#
# Deliberately NOT built from `export --csv`: exporting a 168 MB capture to CSV to count
# its rows costs gigabytes and minutes, and the header already knows the count.
set -euo pipefail

CORPUS="${COMSHU_CORPUS:-$HOME/waste/comshu/corpus}"
HERE="$(cd "$(dirname "$0")" && pwd)"
REPO="$HERE/../../.."

COMSHU_BIN="${COMSHU_BIN:-}"
for c in "$REPO/build-corpus/comshu/comshu" "$REPO/build/comshu/comshu"; do
  [ -n "$COMSHU_BIN" ] && break
  [ -x "$c" ] && COMSHU_BIN="$c"
done
[ -n "$COMSHU_BIN" ] || { echo "no comshu binary found; build one or set COMSHU_BIN" >&2; exit 1; }
[ -d "$CORPUS" ] || { echo "no corpus at $CORPUS; run harvest.sh" >&2; exit 1; }

# The type vocabulary of THIS build, committed beside the index so a future
# reader can tell whether a type id meant the same thing when the index was made.
"$COMSHU_BIN" types > "$HERE/types.tsv"

TYPES="$HERE/types.tsv"
OUT="$HERE/index.tsv"

{
  printf 'capture\tcase\tjobid\tranks\tschema\ttype_id\tname\tkind\trecords\tbytes\n'

  for d in "$CORPUS"/*/; do
    cap=$(basename "$d")
    [ -d "$d" ] || continue
    case_name="${cap%-*}"; jobid="${cap##*-}"
    ranks=$(find "$d" -maxdepth 1 -type d -name 'rank-*' | wc -l)

    # Inventory columns: file type kind stride records schema [(other schema)].
    # Summed across ranks per type; bytes recomputed as records*stride so the
    # 32-byte header of every stream file is excluded and the number is payload.
    "$COMSHU_BIN" read "$d" 2>/dev/null | awk -v cap="$cap" -v cs="$case_name" -v job="$jobid" \
        -v ranks="$ranks" -v types="$TYPES" '
      BEGIN {
        while ((getline line < types) > 0) {
          split(line, tf, "\t")
          if (tf[1] == "type_id") continue
          nm[tf[1]] = tf[4]; kd[tf[1]] = tf[2]
        }
      }
      # Data lines only: field 2 must be a bare type id and field 5 a count.
      $2 ~ /^[0-9]+$/ && $5 ~ /^[0-9]+$/ {
        rec[$2] += $5; byt[$2] += $5 * $4; sch[$2] = $6
      }
      END {
        for (t in rec)
          printf "%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%d\t%d\n",
                 cap, cs, job, ranks, sch[t], t,
                 (t in nm ? nm[t] : "type" t), (t in kd ? kd[t] : "?"),
                 rec[t], byt[t]
      }'
  done
} | { IFS= read -r hdr; printf '%s\n' "$hdr"; sort -t$'\t' -k1,1 -k6,6n; } > "$OUT"

echo "wrote $OUT ($(( $(wc -l < "$OUT") - 1 )) rows)"
echo

# The summary is what a human actually reads: totals per capture, and the one
# stream that dominates it. A capture's size is almost never spread evenly —
# it is one firehose and a long tail, and which firehose it is decides how the
# UI has to treat that capture.
awk -F'\t' 'NR>1 {
  r[$1] += $9; b[$1] += $10; rk[$1] = $4
  if ($9 > top[$1]) { top[$1] = $9; topn[$1] = $7 }
}
END {
  printf "%-30s %6s %12s %10s   %s\n", "capture", "ranks", "records", "MB", "dominant stream"
  for (c in r)
    printf "%-30s %6d %12d %10.1f   %s (%d)\n", c, rk[c], r[c], b[c]/1048576, topn[c], top[c]
}' "$OUT" | { IFS= read -r hdr; printf '%s\n' "$hdr"; sort -k3 -rn; }
