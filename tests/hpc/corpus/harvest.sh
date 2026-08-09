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

REMOTE="${PCOMM_REMOTE:-koc296797@alogin1.bsc.es}"
OUT_ROOT="${PCOMM_REMOTE_OUT:-pcomm-suite}"     # relative to the remote $HOME
CORPUS="${PCOMM_CORPUS:-$HOME/waste/pcomm/corpus}"
SSH="ssh -o BatchMode=yes -o ConnectTimeout=20"

# The eight cases that run on MN5. 09-12 are du04-only (IBGDA); see README.
CASES="ib_pingpong nccl_internode p2p_kernel nvshmem nccl_p2p nvshmem_coexist nvshmem_multinode ib_send_bw"
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
  echo "  if that is alogin1, try: PCOMM_REMOTE=${REMOTE%%@*}@alogin2.bsc.es $0" >&2
  exit 1
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
  # the remote clone, but a finished capture records nothing about which one.
  jobid="${d##*-}"
  ranks=$(find "$CORPUS/$d" -maxdepth 1 -type d -name 'rank-*' | wc -l)
  meta="$(dirname "$0")/../cases"/*_"${c}"/meta.json
  {
    printf 'field\tvalue\n'
    printf 'case\t%s\n'          "$c"
    printf 'jobid\t%s\n'         "$jobid"
    printf 'ranks\t%s\n'         "$ranks"
    printf 'source\t%s:%s/%s\n'  "$REMOTE" "$OUT_ROOT" "$d"
    printf 'harvested\t%s\n'     "$(date -u +%Y-%m-%dT%H:%M:%SZ)"
    printf 'tracer_commit\tunknown\n'
    # shellcheck disable=SC2086
    for m in $meta; do [ -f "$m" ] && printf 'meta_json\t%s\n' "$(tr -d '\n' < "$m" | tr -s ' ')"; done
  } > "$CORPUS/$d/provenance.tsv"
done

say "corpus at $CORPUS"
du -sh "$CORPUS"/*/ 2>/dev/null | sort -h
echo
say "now run: $(dirname "$0")/index.sh"
