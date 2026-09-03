#!/usr/bin/env bash
# link_health's capture-completeness rows: does the rubric notice a capture that
# is not all there?
#
# WHAT THIS GUARDS. Until 2026-09-03 it did not. One real du04 capture, copied
# four times and damaged the three ways a capture can actually be damaged, put
# through `pcomm export --parquet` and `pcomm link`:
#
#     variant      mlx5_umem rows   ranks   link_health non-zero
#     pristine          10            2          0 of 25
#     deleted            5            1          0 of 25
#     zeroed             5            1          0 of 25
#     truncated          9            2          0 of 25
#
# Half the table gone, one of two ranks gone entirely, and the health rubric
# returned a clean bill of health four times out of four. Both halves of the tool
# HAD noticed — the tracer printed CAPTURE INCOMPLETE, the reader printed "not a
# whole number of 88-byte records" — onto the stderr of processes that had since
# exited. What survives to be analysed is the Parquet.
#
# TWO WITNESSES, AND THEY SEE DIFFERENT THINGS. That is the design, and it is
# what this fixture is built to check separately:
#
#   capture_status     what the WRITER attested to. Knows what it failed to
#                      write; cannot know what happened to the files afterwards.
#                      Written only by a normal finalize(), so its ABSENCE for a
#                      rank means that rank never finished.
#   capture_integrity  what the READER found on disk. Knows a file is gone,
#                      unframed or cut short; cannot know about a record that
#                      never reached a file at all.
#
# A rubric that fired on healthy captures would be turned off within a week, so
# the no-false-positive case is checked first and is not a formality.
set -uo pipefail

PCOMM=${1:?usage: capture_integrity_rule.sh <pcomm-binary>}
if [ ! -x "$PCOMM" ]; then echo "SKIP: no pcomm binary at $PCOMM"; exit 0; fi

ROOT=$(mktemp -d "${TMPDIR:-/tmp}/pcomm-capint-XXXXXX")
trap 'rm -rf "$ROOT"' EXIT

fail=0

# Raw column names, not the --csv export's decoded ones: `pcomm link` mounts
# these through mount_raw_tables, which expects the on-disk record layout.
CS_COLS='rank,temporal_kind,temporal_a_domain,temporal_a_ticks,temporal_b_domain,temporal_b_ticks,temporal_value,records_lost,bytes_lost,streams_open,streams_lost,dir_failed,header_lost,never_opened,pad,pad2'
CI_COLS='rank,streams_found,streams_unreadable,streams_truncated,streams_foreign_schema'
CA_COLS='rank,temporal_kind,temporal_a_domain,temporal_a_ticks,temporal_b_domain,temporal_b_ticks,temporal_value,domain,local_ticks,wall_ns,mono_ns'

link_or_die() {  # <dir>
    "$PCOMM" link "$1" > "$1/link.out" 2>&1
    local rc=$?
    if grep -q "no DuckDB" "$1/link.out"; then echo "SKIP: build without DuckDB"; exit 0; fi
    if [ "$rc" -ne 0 ] || grep -q "SOME FAILED" "$1/link.out"; then
        cat "$1/link.out"; echo "FAIL: link did not complete over $1"; exit 1
    fi
}

health() {  # <dir> <finding> -> n
    "$PCOMM" sql "$1" \
        "SELECT coalesce(sum(n), 0) FROM link_health WHERE finding = '$2'" 2>/dev/null |
        tail -1 | tr -d '"'
}

expect() {  # <label> <got> <want>
    if [ "$2" != "$3" ]; then
        echo "FAIL: $1"; echo "  expected: $3"; echo "  got:      ${2:-<no result>}"; fail=1
    fi
}

# ===========================================================================
# A healthy capture. Every row must be zero, and `streams_found` equals
# `streams_open` exactly — the writer counts its own capture_status stream,
# which does not exist yet when the count is taken, so the two are directly
# comparable. Measured on a real du04 capture: 14 and 14.
# ===========================================================================
OK=$ROOT/healthy; mkdir -p "$OK"
{ echo "$CS_COLS"
  echo '0,0,0,0,0,0,0,0,0,14,0,0,0,0,0,0'
  echo '1,0,0,0,0,0,0,0,0,14,0,0,0,0,0,0'; } > "$OK/capture_status.csv"
{ echo "$CI_COLS"; echo '0,14,0,0,0'; echo '1,14,0,0,0'; } > "$OK/capture_integrity.csv"
{ echo "$CA_COLS"
  echo '0,1,0,0,0,0,0,0,0,1000000000,0'
  echo '1,1,0,0,0,0,0,0,0,1000000000,0'; } > "$OK/clock_anchor.csv"

link_or_die "$OK"
for f in capture_records_lost capture_unattested_ranks capture_streams_unreadable \
         capture_streams_truncated capture_streams_missing; do
    expect "a healthy capture does not trip $f" "$(health "$OK" "$f")" "0"
done

# ===========================================================================
# The damaged capture. One rank per failure mode, so a row that fires for the
# wrong reason is visible as the wrong COUNT rather than merely as non-zero.
#
#   rank 0  healthy                       contributes to nothing
#   rank 1  writer lost 7 records         capture_records_lost
#   rank 2  no capture_status row at all  capture_unattested_ranks
#   rank 3  files damaged after the run   unreadable + truncated + missing (6-4)
#   rank 4  MORE files than attested      must contribute NOTHING — see below
#
# RANK 4 IS THE MUTATION GUARD for `WHERE i.streams_found < s.streams_open`. A
# replayed capture nests several rank-* directories and legitimately holds more
# files than one finalize() framed. Written with `<>` instead of `<`, or with no
# WHERE at all, rank 4 contributes 3 - 5 = -2 and cancels rank 3's +2 exactly,
# so capture_streams_missing reads 0 and the row silently stops working. That is
# not hypothetical arithmetic: it is why the numbers below are 6/4 and 3/5.
# ===========================================================================
BAD=$ROOT/damaged; mkdir -p "$BAD"
{ echo "$CS_COLS"
  echo '0,0,0,0,0,0,0,0,0,5,0,0,0,0,0,0'
  echo '1,0,0,0,0,0,0,7,560,5,1,0,0,1,0,0'
  echo '3,0,0,0,0,0,0,0,0,6,0,0,0,0,0,0'
  echo '4,0,0,0,0,0,0,0,0,3,0,0,0,0,0,0'; } > "$BAD/capture_status.csv"
{ echo "$CI_COLS"
  echo '0,5,0,0,0'
  echo '1,5,0,0,0'
  echo '2,4,0,0,0'
  echo '3,4,1,1,0'
  echo '4,5,0,0,0'; } > "$BAD/capture_integrity.csv"
{ echo "$CA_COLS"
  for r in 0 1 2 3 4; do echo "$r,1,0,0,0,0,0,0,0,1000000000,0"; done; } > "$BAD/clock_anchor.csv"

link_or_die "$BAD"

expect "the writer's own loss count is reported" \
    "$(health "$BAD" capture_records_lost)" "7"

# rank 2 is in clock_anchor and not in capture_status. This is the state a rank
# killed mid-run leaves behind, and no loss counter can express it — the process
# that would have written the count is the one that died.
expect "a rank that started and never finished is counted" \
    "$(health "$BAD" capture_unattested_ranks)" "1"

expect "an unframed stream file is counted" \
    "$(health "$BAD" capture_streams_unreadable)" "1"

expect "a stream cut mid-record is counted" \
    "$(health "$BAD" capture_streams_truncated)" "1"

# rank 3 alone: 6 attested, 4 present. rank 4 has MORE than attested and must
# not subtract. See the mutation-guard note above.
expect "files gone since the run are counted, and a replay's extra files are not" \
    "$(health "$BAD" capture_streams_missing)" "2"

# Every one of the five is a 'defect', not a 'note': each is something that
# makes the tables beside it untrustworthy, which is the severity rule
# link_health's own header sets out.
expect "all five are defects, so the clean predicate sees them" \
    "$("$PCOMM" sql "$BAD" \
        "SELECT count(*) FROM link_health
         WHERE n > 0 AND severity = 'defect' AND finding LIKE 'capture_%'" 2>/dev/null |
        tail -1 | tr -d '"')" "5"

if [ "$fail" -eq 0 ]; then
    echo "PASS — link_health reports a capture that is not all there"
else
    echo "FAIL"
fi
exit $fail
