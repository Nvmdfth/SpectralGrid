#!/bin/bash
# Compiles and runs every host-side unit test (no board needed). Usage: hub/host_test/run_all.sh
set -u
cd "$(dirname "$0")/../.."
OUT=$(mktemp -d); FAIL=0
run() { # name, sources...
  local n=$1; shift
  if g++ -std=c++17 -Wall -Wextra -Wno-unused-parameter -Ishared -Ihub/src "$@" -o "$OUT/$n" 2>"$OUT/$n.err"; then
    r=$("$OUT/$n" 2>&1 | tail -n1); [ "$r" = ok ] && echo "PASS $n" || { echo "FAIL $n: $r"; FAIL=1; }
  else echo "FAIL $n (compile)"; head -5 "$OUT/$n.err"; FAIL=1; fi
}
H=hub/host_test; S=hub/src
run shared    shared/test/test_shared.cpp
run p2p       shared/test/test_p2p.cpp
run moved     shared/test/test_moved.cpp
run ringlog   $H/test_ringlog.cpp $S/ringlog.cpp
run session   $H/test_session.cpp $S/session.cpp $S/ringlog.cpp
run events    $H/test_events.cpp $S/events.cpp
run peers     $H/test_peers.cpp $S/peers.cpp
run presence  $H/test_presence.cpp
run fieldformat $H/test_fieldformat.cpp
run touch     $H/test_touch.cpp
run linkgate  $H/test_linkgate.cpp
run score     $H/test_score.cpp $S/score.cpp
run calib     $H/test_calib.cpp $S/calib.cpp $S/score.cpp
run tags_drift $H/test_tags_drift.cpp $S/tags.cpp $S/drift.cpp $S/calib.cpp $S/score.cpp
rm -rf "$OUT"; [ $FAIL = 0 ] && echo "all host tests passed" || { echo "SOME TESTS FAILED"; exit 1; }
