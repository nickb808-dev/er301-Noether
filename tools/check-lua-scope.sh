#!/bin/sh
# check-lua-scope.sh — a file-local used ABOVE its own declaration is nil.
#
# WHY THIS EXISTS
# ---------------
# Lua resolves an undeclared name to a GLOBAL, and an unset global is nil. So
# calling a `local function` from a line above its declaration is not a syntax
# error, not a load error, and not a warning. It compiles, installs, and dies
# at the call site with:
#
#     attempt to call a nil value (global 'trace')
#
# v0.2.24 shipped exactly that: a `trace()` helper declared at line 427 and
# called from line 302. Every call site below the declaration worked, so the
# trace file contained one convincing line and then stopped — which read as
# evidence about the bug under investigation rather than about the tracer.
# A wrong diagnostic is worse than none; that is the second time on this unit.
#
# Usage: tools/check-lua-scope.sh <file.lua...>
set -e

FAIL=0

for f in "$@"; do
    [ -f "$f" ] || { echo "check-lua-scope: no such file: $f" >&2; exit 2; }

    # Every file-local function, with the line it becomes visible on.
    # (Only `local function NAME` and `local NAME = function` — plain locals
    # holding non-callables are not worth the false positives.)
    grep -nE '^[[:space:]]*local[[:space:]]+(function[[:space:]]+)?[A-Za-z_][A-Za-z0-9_]*[[:space:]]*(\(|=[[:space:]]*function)' "$f" \
    | sed -E 's/^([0-9]+):[[:space:]]*local[[:space:]]+function[[:space:]]+([A-Za-z_][A-Za-z0-9_]*).*/\1 \2/; s/^([0-9]+):[[:space:]]*local[[:space:]]+([A-Za-z_][A-Za-z0-9_]*)[[:space:]]*=[[:space:]]*function.*/\1 \2/' \
    | while read -r decl name; do
        [ -n "$name" ] || continue
        # A call to NAME( strictly above its declaration line.
        bad=$(grep -nE "(^|[^A-Za-z0-9_.:])$name[[:space:]]*\(" "$f" \
              | awk -F: -v d="$decl" '$1 < d { print $1 }' | head -3)
        if [ -n "$bad" ]; then
            for ln in $bad; do
                echo "check-lua-scope: FAIL: $f:$ln calls '$name(' but the local"
                echo "                       is not declared until line $decl."
                echo "                       Above line $decl that name is a nil GLOBAL."
            done
            echo "$name" >> "$f.scopefail.$$"
        fi
      done

    if [ -f "$f.scopefail.$$" ]; then
        rm -f "$f.scopefail.$$"
        FAIL=1
    fi
done

if [ "$FAIL" -ne 0 ]; then
    echo ""
    echo "Move the declaration above its first use (near the requires is safest)."
    exit 1
fi

echo "check-lua-scope: OK — no file-local is used above its declaration"
