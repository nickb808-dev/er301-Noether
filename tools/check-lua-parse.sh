#!/bin/sh
# check-lua-parse.sh — actually PARSE every Lua asset with a real Lua grammar.
#
# WHY THIS EXISTS
# ---------------
# v0.2.26's cleanup deleted every line matching ^\s*trace\( — but one trace
# call spanned TWO lines. The first line matched and was deleted; the orphaned
# continuation
#
#         tostring(sample.pSample ~= nil), tostring(sample.slices ~= nil))
#
# survived, which is a syntax error. On the device, require() fails and the
# Factory shows "Error in Landau (landau)." on adding the unit. Nothing else
# caught it: the host suite never loads the Lua, check-lua-scope only checks
# declaration order, and a hand-rolled block-balance counter matched fine
# because the orphan contains no block keywords. Two releases shipped broken
# (0.2.26, 0.2.27) and the working 0.2.25 had already been pruned.
#
# A real parser found it in one second. So a real parser is now a gate.
#
# Two backends, tried in order:
#   1. luac -p        — the real Lua compiler in parse-only mode. Preferred:
#                       it is the same grammar the device runs.
#   2. python3 + luaparser — fallback for boxes with python but no lua
#                       (the Cowork sandbox is one).
# What matters is a full grammar, not a heuristic.
#
# Usage: tools/check-lua-parse.sh <file.lua...>
set -e

# Backend 1: a real luac, if any is installed (brew install lua / apt lua5.4).
# luac -p is the actual Lua compiler in parse-only mode — the most faithful
# check possible, since it is the same grammar the device's interpreter uses.
LUAC=""
for c in luac luac5.4 luac5.3 luac5.2; do
    command -v "$c" >/dev/null 2>&1 && { LUAC="$c"; break; }
done

if [ -n "$LUAC" ]; then
    FAIL=0
    for f in "$@"; do
        if err=$("$LUAC" -p "$f" 2>&1); then
            echo "check-lua-parse: OK   $f  ($LUAC)"
        else
            echo "check-lua-parse: FAIL $f"
            echo "                 $err"
            echo "                 This file will fail require() on the device and the"
            echo "                 unit will show 'Error in <Unit> (<pkg>).' when added."
            FAIL=1
        fi
    done
    exit $FAIL
fi

# Backend 2: python3 + luaparser.
command -v python3 >/dev/null || { echo "check-lua-parse: need luac or python3" >&2; exit 2; }
python3 -c 'import luaparser' 2>/dev/null || {
    echo "check-lua-parse: no Lua parser available. Install ONE of:" >&2
    echo "                   brew install lua            (gives luac; preferred)" >&2
    echo "                   pip3 install luaparser      (or with --break-system-packages" >&2
    echo "                                                on PEP-668 pythons)" >&2
    exit 2
}

python3 - "$@" <<'PYEOF'
import sys
from luaparser import ast

fail = 0
for path in sys.argv[1:]:
    try:
        src = open(path, encoding="utf-8").read()
    except OSError as e:
        print(f"check-lua-parse: cannot read {path}: {e}")
        fail = 1
        continue
    try:
        ast.parse(src)
        print(f"check-lua-parse: OK   {path}")
    except Exception as e:
        print(f"check-lua-parse: FAIL {path}")
        print(f"                 {e}")
        print(f"                 This file will fail require() on the device and the")
        print(f"                 unit will show 'Error in <Unit> (<pkg>).' when added.")
        fail = 1
sys.exit(fail)
PYEOF
