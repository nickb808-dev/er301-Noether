#!/bin/sh
# check-version.sh — the version must be the SAME in every place that states it.
#
# WHY: the device ran landau 0.1.5 for a week while the source tree was at
# 0.2.8. Nothing in the UI could say so, and every symptom was that old
# build's already-fixed bugs. The unit now PRINTS its version — which is only
# useful if the printed value cannot drift from the packaged one.
set -e
MK="$1"; TOC="$2"; shift 2

v_mk=`sed -n 's/^VERSION *:= *//p' "$MK" | head -1 | tr -d ' \r'`
v_toc=`sed -n 's/.*version *= *"\([^"]*\)".*/\1/p' "$TOC" | head -1`

fail=0
[ -n "$v_mk" ]  || { echo "check-version: no VERSION in $MK"; exit 2; }
[ -n "$v_toc" ] || { echo "check-version: no version in $TOC"; exit 2; }
if [ "$v_mk" != "$v_toc" ]; then
  echo "check-version: FAIL  Makefile=$v_mk  toc.lua=$v_toc"; fail=1
fi
for f in "$@"; do
  v=`sed -n 's/^local VERSION *= *"\([^"]*\)".*/\1/p' "$f" | head -1`
  [ -n "$v" ] || continue
  if [ "$v" != "$v_mk" ]; then
    echo "check-version: FAIL  $f=$v  Makefile=$v_mk"; fail=1
  fi
done

if [ "$fail" -ne 0 ]; then
  echo ""
  echo "The unit displays its version on screen. If these disagree it will"
  echo "display a lie, which is worse than displaying nothing."
  exit 1
fi
echo "check-version: OK — everything says $v_mk"
