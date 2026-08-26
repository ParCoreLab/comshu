#!/usr/bin/env bash
# Refuse a script that calls `pcomm` the old way — flag first, no verb.
#
# This exists because it happened. The one-binary refactor rewrote every
# `$PCOMM_READ` call site and missed every `"$PCOMM" -p ...` one, so 24 job
# scripts still invoked the launcher with a flag where the verb goes. The
# dispatcher did exactly the right thing — printed "`-p` is a flag of `pcomm
# run`" and exited 2 — but a job.sbatch does not read stderr, so the whole thing
# surfaced as `rc=2` on a remote cluster, one queue slot later. A grep that takes
# 30 ms locally is a better place to find that than du04 is.
#
#   tools/check_cli_callsites.sh        # every tracked .sh/.sbatch
#
# Deliberately NOT a general command-line validator. It knows one shape: the
# token right after the pcomm binary must be a verb. That is the rule the
# refactor broke, and the only one cheap enough to check with a regex.
set -uo pipefail
cd "$(git rev-parse --show-toplevel)" || exit 1

# Flags that used to sit where the verb now goes. Everything else after the
# binary is either a verb or a variable, and this check does not guess.
# Comment lines are dropped — a shell comment cannot invoke anything, and the
# first version of this check failed on its own explanation of what it forbids.
# The filter keys on `<file>:<line>:` followed by whitespace and `#`, so a real
# call with a trailing comment still matches.
#
# Lines that only PRINT the command are dropped for the same reason and were
# added for the same reason: a job script that echoes the invocation it is about
# to make is doing something useful, and `echo "=== pcomm -p ib,ucx ... ==="` can
# no more invoke the binary than a comment can. Found by
# tests/hpc/apps/03_quantumespresso/job.sbatch, which labels each of its four
# runs that way. Keyed on a line that STARTS with echo/printf and carries no
# command separator after it, so `echo hi && "$PCOMM" -p ib -- ./app` is still
# caught — the first version of this exclusion let that through, which the
# mutation test found.
bad=$(git ls-files '*.sh' '*.sbatch' |
      xargs grep -nE '(\$\{?PCOMM\}?"?|\bpcomm)[[:space:]]+(-p|--providers|-o|--output|--list|--dry-run|--enable-doorbell-hijack)\b' \
        2>/dev/null | grep -vE ':[[:space:]]*#' | grep -vE ':[[:space:]]*(echo|printf)[[:space:]][^;|&]*$')

if [ -n "$bad" ]; then
  echo "pcomm CLI: these call the binary with a flag where the verb goes:" >&2
  printf '%s\n' "$bad" | sed 's/^/  /' >&2
  echo >&2
  echo "  pcomm takes a verb first:  pcomm run -p ib -- ./app" >&2
  echo "  (verbs: run list read dump export link sql types ui)" >&2
  exit 1
fi
echo "pcomm CLI: call sites ok"
