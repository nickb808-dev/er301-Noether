#!/bin/sh
# check-swig-api.sh — every method Lua calls on the engine must be visible to SWIG.
#
# WHY THIS EXISTS
# ---------------
# The engine header wraps its od:: members in `#ifndef SWIGLUA`, because SWIG
# cannot parse them. SWIG generates bindings only for what it CAN see — so a
# method declared inside that guard compiles, links, loads, and is simply
# missing from Lua. The failure is a crash on the device, at the moment the
# user opens the menu, with nothing wrong in the C++:
#
#     Landau.lua:123: attempt to call a nil value (method 'getCutMode')
#
# Landau shipped that in v0.1.3, 0.1.4 and 0.1.5. Nothing in a build, a test
# run, or a code read catches it. This does.
#
# Usage: tools/check-swig-api.sh <header> <lua...>
set -e

HDR="$1"
shift
[ -f "$HDR" ] || { echo "check-swig-api: no such header: $HDR" >&2; exit 2; }

# Methods inherited from the SDK are wrapped by the SDK, not by us.
#
# VERIFIED, not assumed (this was a guess that happened to be right until it
# was checked): od/glue/mod.cpp.swig — which liblandau.swig %includes — does
#     %import(module="app") <od/objects/heads/SliceHead.h>
# and SliceHead.h declares BOTH setSample arities ABOVE its `#ifndef SWIGLUA`.
# So Landau, deriving from od::SliceHead, inherits real Lua bindings for them.
# If you ever change the base class, re-check that mod.cpp.swig imports it —
# an unimported base makes SWIG silently drop every inherited method.
INHERITED="setSample getOption attach release getName setName"

# What SWIG can see: the header with every `#ifndef SWIGLUA` region removed.
# A header normally has TWO of them — one around the includes and one around
# the class internals — so this has to track nesting, not just cut at the
# first match. (Getting that wrong is how the first version of this script
# reported false failures.)
VISIBLE=$(awk '
    /^[[:space:]]*#[[:space:]]*ifndef[[:space:]]+SWIGLUA/ { if (!skip) { skip = 1; depth = 1; next } }
    skip {
        if ($0 ~ /^[[:space:]]*#[[:space:]]*(if|ifdef|ifndef)/) depth++
        else if ($0 ~ /^[[:space:]]*#[[:space:]]*endif/) { depth--; if (depth == 0) skip = 0 }
        next
    }
    { print }
' "$HDR")

CALLS=$(grep -ohE '\bhead:[a-zA-Z_][a-zA-Z0-9_]*' "$@" 2>/dev/null \
        | sed 's/.*://' | sort -u)

[ -n "$CALLS" ] || { echo "check-swig-api: no head:method() calls found — check the Lua paths" >&2; exit 2; }

FAIL=0
for m in $CALLS; do
    case " $INHERITED " in *" $m "*) continue ;; esac
    if ! printf '%s\n' "$VISIBLE" | grep -qE "[^a-zA-Z0-9_]$m[[:space:]]*\("; then
        echo "check-swig-api: FAIL: Lua calls head:$m() but SWIG cannot see it in $HDR."
        echo "                      It is inside a '#ifndef SWIGLUA' region, so it will"
        echo "                      be nil at runtime and crash when that menu opens."
        FAIL=1
    fi
done

if [ "$FAIL" -ne 0 ]; then
    echo ""
    echo "Move the declaration above the guard (declaration only, plain types)"
    echo "and define it in the .cpp."
    exit 1
fi

echo "check-swig-api: OK — all $(echo "$CALLS" | wc -w | tr -d ' ') head:method() calls are visible to SWIG"
