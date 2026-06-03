#!/usr/bin/env bash
# Guards tools/discard_spike.sh's SAFETY property (TW-02): it must refuse any
# non-spike branch with exit 2 BEFORE touching git — so it can never delete a
# real branch. The happy path (deleting a spike/*) mutates git state and is
# smoke-tested by hand, not here.
set -u
S=tools/discard_spike.sh
m="$("$S" main        >/dev/null 2>&1; echo $?)"
i="$("$S" impl/raster >/dev/null 2>&1; echo $?)"
n="$("$S" ""          >/dev/null 2>&1; echo $?)"
[ "$m" = "2" ] && [ "$i" = "2" ] && [ "$n" = "2" ] && echo PASS \
  || echo "FAIL (main=$m impl=$i empty=$n)"
