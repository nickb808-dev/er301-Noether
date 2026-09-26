#!/bin/sh
# check-ports.sh — every engine port Lua connects to must exist on the device.
#
# WHY THIS EXISTS
# ---------------
# An od::Inlet member is only a port once the constructor calls addInput()
# on it. Noether 0.8.0 declared `od::Inlet mBarsIn {"Bars"}`, read it in
# process(), and Noether.lua connected a GainBias to head "Bars" — but the
# constructor never called addInput(mBarsIn). The host suite was green (its
# stub addInput() does nothing and the tests write the member directly), so
# only the device would have found it: a connect() to a port that is not
# there, while the unit loads.
#
# Two checks:
#   1. every od::Inlet / od::Outlet member declared in the header is
#      registered with addInput / addOutput in the .cpp (and the right one);
#   2. every port name Lua passes to connect() on `head` is the name of a
#      registered port.
#
# Usage: tools/check-ports.sh <header> <cpp> <lua...>
set -e

HDR="$1"; CPP="$2"; shift 2
[ -f "$HDR" ] || { echo "check-ports: no such header: $HDR" >&2; exit 2; }
[ -f "$CPP" ] || { echo "check-ports: no such source: $CPP" >&2; exit 2; }

fail=0
TAB=$(printf '\t')

# "Kind<TAB>member<TAB>port name" for each declared port
DECL=$(sed -nE 's/^[[:space:]]*od::(Inlet|Outlet)[[:space:]]+(m[A-Za-z0-9_]+)[[:space:]]*\{"([^"]+)"\}.*/\1	\2	\3/p' "$HDR")
[ -n "$DECL" ] || { echo "check-ports: found no od::Inlet / od::Outlet members in $HDR" >&2; exit 2; }

echo "$DECL" | while IFS="$TAB" read -r kind member name; do
  if [ "$kind" = "Inlet" ]; then call=addInput; else call=addOutput; fi
  if ! grep -Eq "\\b${call}\\(${member}\\)" "$CPP"; then
    echo "check-ports: $member (\"$name\") is declared in $HDR but never ${call}()'d in $CPP" >&2
    exit 1
  fi
done || fail=1

# port names Lua uses on the engine: connect(x, "A", head, "P"),
# connect(head, "P", ...), and the param(self, head, "name", "P", bias)
# helper, whose connect() takes the port as a VARIABLE — a scan of literal
# connect() calls alone reported ok with "Bars" misspelt (verified).
USED=$( { cat "$@" | grep -oE 'connect\([^)]*\)' | \
          sed -nE 's/.*,[[:space:]]*head,[[:space:]]*"([^"]+)"[[:space:]]*\)$/\1/p; s/^connect\(head,[[:space:]]*"([^"]+)".*/\1/p'
        cat "$@" | sed -nE 's/.*param\(self,[[:space:]]*head,[[:space:]]*"[^"]+",[[:space:]]*"([^"]+)".*/\1/p'
      } | sort -u)
echo "$USED" | while read -r name; do
  [ -n "$name" ] || continue
  member=$(echo "$DECL" | awk -F"$TAB" -v n="$name" '$3 == n { print $2 }')
  if [ -z "$member" ]; then
    echo "check-ports: Lua connects to head \"$name\", which no od::Inlet / od::Outlet in $HDR is named" >&2
    exit 1
  fi
  if ! grep -Eq "\\badd(Input|Output)\\(${member}\\)" "$CPP"; then
    echo "check-ports: Lua connects to head \"$name\" ($member), which $CPP never registers" >&2
    exit 1
  fi
done || fail=1

[ $fail -eq 0 ] || exit 1
n=$(echo "$USED" | grep -c .)
echo "check-ports: ok ($n engine ports used from Lua, all registered)"
