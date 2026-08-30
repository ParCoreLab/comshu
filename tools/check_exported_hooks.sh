#!/bin/sh
# check_exported_hooks.sh — every extern "C" interposer a provider defines must
# be named in that provider's linker version script.
#
# WHY THIS EXISTS. A provider .so is an LD_PRELOAD interposer: it works by
# exporting a symbol that shadows the real one. Each provider links with
# `-Wl,--version-script=<name>.map`, whose `global:` list is an allow-list and
# whose `local: *` hides everything else. So an interposer that is defined in
# the .cpp and absent from the .map compiles, links, and EXPORTS NOTHING. The
# hook is dead and there is no diagnostic anywhere: no warning, no link error,
# no missing symbol at load, no crash. The capture is simply missing rows that
# nobody asked for by name.
#
# Found on 2026-08-30 adding ucp_ep_rkey_unpack_internal, the rendezvous rkey
# door. The build was clean and the other three rkey doors exported normally;
# only `nm -D` on the built .so showed the new one was not there. Had that check
# been skipped, case 21 would have stayed red and the obvious next suspect would
# have been the ABI trick in the hook — the interesting, subtle, wrong place to
# look — rather than a one-line list.
#
# This is the fourth list in this repo that must be edited in the same change as
# the thing it describes; the other three are the two tag lists in each
# provider's event source and the `opens` CTE in
# tests/hpc/suite/asserts/entity_close_accounted.sql, whose own comment records
# being forgotten four separate times. The pattern is that a list maintained by
# memory gets forgotten, so this one is maintained by the machine instead.
#
# Exit 0 and print one ok line when every interposer is exported; exit 1 and
# print the offenders on stderr otherwise.
set -u

root=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
rc=0
checked=0

for map in "$root"/providers/*/*.map; do
    [ -e "$map" ] || continue
    dir=$(dirname "$map")
    provider=$(basename "$dir")

    # The allow-list, one symbol per line: strip comments, take the `global:`
    # side, drop the trailing semicolon. `local:` ends it.
    globals=$(awk '
        /local:/  { in_g = 0 }
        in_g      { gsub(/[ \t;]/, ""); if ($0 != "") print }
        /global:/ { in_g = 1 }
    ' "$map")

    # Every extern "C" function DEFINED in this provider's sources. A definition
    # ends in `{` or has its brace on a later line; both spellings appear in the
    # tree, so match the name and let the export check do the judging. Prototypes
    # are not an issue here: providers declare their interposers nowhere else.
    defined=$(grep -ho '^extern "C"[^(]*[ *]\([A-Za-z_][A-Za-z0-9_]*\)[ ]*(' \
                   "$dir"/*.cpp 2>/dev/null |
              sed 's/.*[ *]\([A-Za-z_][A-Za-z0-9_]*\)[ ]*($/\1/')

    for sym in $defined; do
        checked=$((checked + 1))
        if ! printf '%s\n' "$globals" | grep -qx "$sym"; then
            echo "$provider: $sym is defined but not in $(basename "$map")" >&2
            echo "  it will not be exported, so the hook can never fire" >&2
            rc=1
        fi
    done
done

if [ "$rc" -eq 0 ]; then
    echo "check_exported_hooks: ok — $checked interposers, all exported"
fi
exit $rc
