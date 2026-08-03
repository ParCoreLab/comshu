#!/usr/bin/env bash
# format.sh — apply .clang-format to first-party C++.
#
#   tools/format.sh              format the staged files (what the hook runs)
#   tools/format.sh --all        format every tracked first-party source
#   tools/format.sh --check      report what is unformatted; exit 1 if any
#   tools/format.sh --check --all
#
# Vendored and generated trees are excluded twice over, belt and braces:
# by the path filter here, and by a `DisableFormat: true` .clang-format in
# third_party/, gpu-backends/nvbit/nvbit-1.7.7.1/ and libpcomm/schema/generated/
# (which also stops an editor formatting them on save).
#
# .cu is deliberately included: clang-format handles CUDA as C++ well enough for
# the two device files we own, and leaving them out would let the one part of
# the tree with the least review drift the furthest.
set -uo pipefail

root="$(git rev-parse --show-toplevel)"
cd "$root" || exit 1

check=0
all=0
for a in "$@"; do
  case "$a" in
    --check) check=1 ;;
    --all)   all=1 ;;
    -h|--help) sed -n '2,16p' "$0"; exit 0 ;;
    *) echo "format.sh: unknown argument: $a" >&2; exit 2 ;;
  esac
done

command -v clang-format >/dev/null || {
  echo "format.sh: clang-format not found on PATH" >&2; exit 127; }

# Tracked, first-party, formattable.
sources() {
  if [ "$all" -eq 1 ]; then
    git ls-files -- '*.cpp' '*.hpp' '*.h' '*.cu'
  else
    git diff --cached --name-only --diff-filter=ACMR -- '*.cpp' '*.hpp' '*.h' '*.cu'
  fi | grep -vE '^(third_party/|gpu-backends/nvbit/nvbit-1\.7\.7\.1/|libpcomm/schema/generated/)'
}

mapfile -t files < <(sources)
[ "${#files[@]}" -eq 0 ] && { [ "$check" -eq 1 ] && echo "format: nothing to check"; exit 0; }

if [ "$check" -eq 1 ]; then
  bad=()
  for f in "${files[@]}"; do
    [ -f "$f" ] || continue
    clang-format "$f" | diff -q - "$f" >/dev/null 2>&1 || bad+=("$f")
  done
  if [ "${#bad[@]}" -eq 0 ]; then
    echo "format: ${#files[@]} file(s) already formatted"
    exit 0
  fi
  echo "format: ${#bad[@]} of ${#files[@]} file(s) need formatting:" >&2
  printf '  %s\n' "${bad[@]}" >&2
  echo "  fix with: tools/format.sh${all:+ --all}" >&2
  exit 1
fi

n=0
for f in "${files[@]}"; do
  [ -f "$f" ] || continue
  before=$(cksum < "$f")
  clang-format -i "$f"
  [ "$(cksum < "$f")" != "$before" ] && { n=$((n+1)); echo "  formatted $f"; }
done
echo "format: ${#files[@]} file(s) processed, $n changed"

# Re-stage what we rewrote, so the commit contains the formatted text rather
# than leaving the working tree ahead of the index.
if [ "$all" -eq 0 ] && [ "$n" -gt 0 ]; then
  git add -- "${files[@]}"
  echo "format: re-staged"
fi
exit 0
