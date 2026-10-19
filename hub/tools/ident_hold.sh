#!/bin/bash
# Keep the "which board is this" colours lit (the firmware holds them for 2 min) until the stop file appears, then turn them off.
# Colour = node id: 1 red, 2 green, 3 blue, 4 yellow, 5 magenta, 6 cyan, 7 white.   Usage: hub/tools/ident_hold.sh "1 3 4" [STOPFILE]
NODES=${1:-"1 3 4"}; STOP=${2:-/tmp/ident_stop}; D=$(dirname "$0"); rm -f "$STOP"
echo "ident hold: nodes $NODES; stop with: touch $STOP"
while [ ! -e "$STOP" ]; do
    for n in $NODES; do "$D/hubctl.sh" pcmd $n ident $n >/dev/null 2>&1; done
    for i in $(seq 1 18); do [ -e "$STOP" ] && break; sleep 5; done
done
for n in $NODES; do "$D/hubctl.sh" pcmd $n ident 0 >/dev/null 2>&1; done
echo "ident hold: colours off"
