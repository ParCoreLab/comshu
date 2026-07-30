#!/usr/bin/env bash
# Point git at the hooks committed in tools/hooks.
#
# `core.hooksPath` is per-clone local config, so it does not travel with the
# files — run this once per machine. (Both simurgh-base and university-base need
# it: syncthing mirrors the working tree, not .git/config.)
set -euo pipefail

root="$(git rev-parse --show-toplevel)"
chmod +x "$root"/tools/hooks/*
git -C "$root" config core.hooksPath tools/hooks

echo "hooks installed: $(git -C "$root" config core.hooksPath)"
for h in "$root"/tools/hooks/*; do echo "  - $(basename "$h")"; done
echo
echo "pre-commit runs tools/build_all.sh --quick (core config + tests, incremental)."
echo "Skip one commit with --no-verify; undo with: git config --unset core.hooksPath"
