#!/usr/bin/env bash
# Fetch suite captures off MN5 into the local corpus, with provenance.
#
# A capture directory is `rank-N/<type_id>.pcs` and nothing else — no meta.json,
# no job record, no tracer commit. Every fact about it lives in the name of the
# directory it was written into, which is exactly what a copy destroys. So the
# provenance sidecar is written HERE, at fetch time, not derived later.
#
# See README.md in this directory for what the corpus is for.
set -euo pipefail

REMOTE="${COMSHU_REMOTE:-koc296797@alogin1.bsc.es}"
OUT_ROOT="${COMSHU_REMOTE_OUT:-comshu-suite}"     # relative to the remote $HOME
CORPUS="${COMSHU_CORPUS:-$HOME/waste/comshu/corpus}"
SSH="ssh -o BatchMode=yes -o ConnectTimeout=20"

# The fourteen cases that run on MN5. 09-12 are du04-only (IBGDA); see README.
#
# 13-18 (hub/pipe/skew, on NVLink and on IB) were missing from this list — and
# therefore from the corpus — for as long as they have existed, which is exactly
# backwards: they are the ONLY asymmetric workloads we have. Every other capture
# here is a ring or a pair, so every object heatmap built against this corpus was
# 512 accesses in each of 32 bins and a solid bar was indistinguishable from a
# broken query. A UI panel designed without these is designed against flat data.
CASES="ib_pingpong nccl_internode p2p_kernel nvshmem nccl_p2p nvshmem_coexist \
nvshmem_multinode ib_send_bw hubspoke pipeline skew hubspoke_ib pipeline_ib skew_ib"
JOB=""

while [ $# -gt 0 ]; do
  case "$1" in
    --job)    JOB="$2"; shift 2 ;;
    --cases)  CASES="$2"; shift 2 ;;
    --dest)   CORPUS="$2"; shift 2 ;;
    -h|--help) sed -n '2,9p' "$0"; exit 0 ;;
    *) echo "unknown argument: $1" >&2; exit 1 ;;
  esac
done

say() { printf '\033[36m==\033[0m %s\n' "$*"; }

$SSH "$REMOTE" true 2>/dev/null || {
  echo "cannot reach $REMOTE" >&2
  echo "  if that is alogin1, try: COMSHU_REMOTE=${REMOTE%%@*}@alogin2.bsc.es $0" >&2
  exit 1
}

# --- what commit built the tracer, when that is knowable -------------------
#
# The remote clone's HEAD is the commit the last run synced and built, so for a
# capture THAT run produced it is exactly the answer. For any other capture it is
# whatever has been synced since, which is not — and writing it down anyway would
# be a guess wearing a fact's clothes, the thing README.md says not to do.
#
# `.last-run.manifest` is what tells the two apart, and soundly rather than by
# heuristic: run_on_mn5.sh writes it in the same run that pushes HEAD and
# submits the jobs, one `<case> <jobid>` line per case. So if a capture's jobid
# is the one the manifest pins for its case, that capture came out of the run
# that left HEAD where it is, and HEAD is its tracer commit. Otherwise: unknown,
# as before.
#
# Both reads are best-effort — a missing manifest or an unreachable tree costs an
# `unknown`, never a failed harvest.
REMOTE_DIR="${COMSHU_REMOTE_DIR:-proj/comshu}"
remote_head=$($SSH "$REMOTE" "git -C '$REMOTE_DIR' rev-parse --short=7 HEAD 2>/dev/null" \
              2>/dev/null | tr -d '\r') || remote_head=""
manifest=$($SSH "$REMOTE" "cat '$OUT_ROOT/.last-run.manifest' 2>/dev/null" 2>/dev/null) || manifest=""

# The jobid the last run pinned for this case, or empty.
manifest_job() {  # <case>
  printf '%s\n' "$manifest" | awk -v c="$1" '$0 ~ ("_" c "$") || $1 ~ ("_" c "$") {print $2; exit}'
}

# Newest job per case unless one is named. Deliberately resolved per case rather
# than assuming one job produced all of them: --per-case runs exist, and a
# combined job that died partway leaves some cases on an older id.
mkdir -p "$CORPUS"
for c in $CASES; do
  if [ -n "$JOB" ]; then
    d="$c-$JOB"
    $SSH "$REMOTE" "test -d '$OUT_ROOT/$d'" 2>/dev/null || { say "skip $c: no $d"; continue; }
  else
    d=$($SSH "$REMOTE" "ls -d '$OUT_ROOT'/$c-* 2>/dev/null | grep -v '\.csv$' | sort -t- -k2 -n | tail -1 | xargs -r basename") || true
    [ -n "$d" ] || { say "skip $c: no captures"; continue; }
  fi

  say "fetch $d"
  rsync -az --info=stats0 "$REMOTE:$OUT_ROOT/$d" "$CORPUS"/

  # Provenance, written now because it is unrecoverable later. The tracer commit
  # is honestly `unknown` for a retroactive harvest: the suite syncs a commit to
  # the remote clone, but a finished capture records nothing about which one — so
  # it is recorded only when the manifest proves this capture came from the run
  # that left the remote tree where it is. See the block above.
  jobid="${d##*-}"
  commit="unknown"
  if [ -n "$remote_head" ] && [ "$(manifest_job "$c")" = "$jobid" ]; then
    commit="$remote_head"
  fi
  ranks=$(find "$CORPUS/$d" -maxdepth 1 -type d -name 'rank-*' | wc -l)
  meta="$(dirname "$0")/../cases"/*_"${c}"/meta.json
  {
    printf 'field\tvalue\n'
    printf 'case\t%s\n'          "$c"
    printf 'jobid\t%s\n'         "$jobid"
    printf 'ranks\t%s\n'         "$ranks"
    printf 'source\t%s:%s/%s\n'  "$REMOTE" "$OUT_ROOT" "$d"
    printf 'harvested\t%s\n'     "$(date -u +%Y-%m-%dT%H:%M:%SZ)"
    printf 'tracer_commit\t%s\n' "$commit"
    # shellcheck disable=SC2086
    for m in $meta; do [ -f "$m" ] && printf 'meta_json\t%s\n' "$(tr -d '\n' < "$m" | tr -s ' ')"; done
  } > "$CORPUS/$d/provenance.tsv"
done

say "corpus at $CORPUS"
du -sh "$CORPUS"/*/ 2>/dev/null | sort -h
echo
say "now run: $(dirname "$0")/index.sh"
