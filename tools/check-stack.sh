#!/bin/sh
# check-stack.sh — no engine function may use more than a sliver of the
# ER-301's audio-thread stack.
#
# WHY THIS EXISTS
# ---------------
# The audio thread on the ER-301 runs on a 2 KB stack. Noether's seam search
# kept a 1 KB array of coarse errors as a LOCAL: half the stack in one frame,
# plus process(), the firmware's own frames and saved registers. For eleven
# versions it fit by luck; in 0.8.10 an unrelated change moved the compiler's
# inlining a few bytes and the first recording overflowed it. The stolmine
# crash diagnostics (firmware 9.6.0) showed it plainly:
#
#     Kind: data-abort   *** STACK  BLOWN ***
#     base=80535610 size=2048 used=2048 (100%) canary=BLOWN
#
# Without the diagnostics it showed as ALL audio harshly distorted until a
# reboot — the overflow had written over the memory beside that stack. Host
# tests cannot see this: the host's stacks are megabytes.
#
# The compiler reports every function's frame with -fstack-usage (a .su
# file next to each object). This fails the build if any frame in the given
# .su files exceeds the limit, or is dynamic (alloca / a variable-length
# array: unbounded).
#
# Usage: tools/check-stack.sh <limit-bytes> <file.su>...
set -e
LIMIT="$1"; shift
[ -n "$LIMIT" ] || { echo "check-stack: no limit given" >&2; exit 2; }
fail=0
found=0
for f in "$@"; do
  [ -f "$f" ] || { echo "check-stack: no such file: $f (built with -fstack-usage?)" >&2; exit 2; }
  found=1
  # each line: <file>:<line>:<col>:<function>\t<bytes>\t<static|dynamic|dynamic,bounded>
  while IFS="$(printf '\t')" read -r where bytes kind; do
    [ -n "$bytes" ] || continue
    fn=${where#*:*:*:}
    if [ "$bytes" -gt "$LIMIT" ]; then
      echo "check-stack: FAIL  $fn uses $bytes bytes of stack (limit $LIMIT; the audio thread has 2048)" >&2
      fail=1
    fi
    case "$kind" in
      dynamic|dynamic,*) echo "check-stack: FAIL  $fn has a $kind frame (alloca / VLA): unbounded on a 2 KB stack" >&2; fail=1 ;;
    esac
  done < "$f"
done
[ $found -eq 1 ] || { echo "check-stack: no .su files given" >&2; exit 2; }
[ $fail -eq 0 ] || exit 1
largest=$(cat "$@" | awk -F'\t' '$2 > m { m = $2; n = $1 } END { sub(/^[^:]*:[^:]*:[^:]*:/, "", n); print m " bytes (" n ")" }')
echo "check-stack: ok — every engine frame within $LIMIT bytes; largest $largest"
