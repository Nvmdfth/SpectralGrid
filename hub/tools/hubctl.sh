#!/usr/bin/env bash
# Helper for the hub from the Pi. Serial commands never toggle DTR/RTS (that resets the board).
#   hubctl.sh ports                list USB serial devices (serial numbers; no reset)
#   hubctl.sh macs                 read each port's MAC with esptool (RESETS every board it touches)
#   hubctl.sh status               one clean snapshot of the hub's node table
#   hubctl.sh cmd "<console cmd>"  send a console command, print the reply (help | status | pair | ota | field status | ...)
#   hubctl.sh pair                 open the 60 s pairing window
#   hubctl.sh pcmd <node|all> <info|bcast|p2p|reboot> [0|1]   runtime command to probe(s) (v4); then hubctl.sh pdiag shows their replies
#   hubctl.sh watch [secs]         node table every second (default 20 s)
#   HUB_URL=http://<hub-ip> hubctl.sh cal <all|N> [sens 0|1|2]   start calibration (30 s capture, room empty)
#   HUB_URL=... hubctl.sh cal-accept | cal-discard | cal-reset <all|N> | cal-show
# The hub port is found by USB serial HUB_SERIAL (default 5C63053377, hub as of 2026-10-14); override with HUB_PORT=/dev/ttyACMn.
set -u
HUB_SERIAL=${HUB_SERIAL:-5C63053377}
TMP=${TMPDIR:-/tmp}/hubctl.$$
trap 'rm -f "$TMP"' EXIT

usb_serial() { udevadm info -q property -n "$1" 2>/dev/null | sed -n 's/^ID_SERIAL_SHORT=//p'; }

hub_port() {
    [ -n "${HUB_PORT:-}" ] && { echo "$HUB_PORT"; return; }
    for d in /dev/ttyACM* /dev/ttyUSB*; do
        [ -e "$d" ] && [ "$(usb_serial "$d")" = "$HUB_SERIAL" ] && { echo "$d"; return; }
    done
    echo "hubctl: hub (USB serial $HUB_SERIAL) not found; run 'hubctl.sh ports'" >&2; return 1
}

# send <line> <secs>: capture the port for <secs>, sending "\n<line>\n" after 1 s (the leading newline flushes boot-time junk)
send() {
    local p; p=$(hub_port) || return 1
    stty -F "$p" 115200 raw -echo -hupcl
    timeout "$2" cat "$p" > "$TMP" &
    local c=$!
    sleep 1
    if [ -n "$1" ]; then printf '\n' > "$p"; sleep 0.5; printf '%s\n' "$1" > "$p"; fi
    wait $c 2>/dev/null
}

nodes() { grep -E '^\[hub\] paired=|^      node [0-9]+ ' "$TMP" | tail -"${1:-8}" | cut -c1-200; }

case "${1:-}" in
ports)
    for d in /dev/ttyACM* /dev/ttyUSB*; do [ -e "$d" ] && echo "$d  usb-serial=$(usb_serial "$d")$([ "$(usb_serial "$d")" = "$HUB_SERIAL" ] && echo '  <- hub')"; done ;;
macs)
    PY=~/.platformio/penv/bin/python; ET=~/.platformio/packages/tool-esptoolpy/esptool.py
    for d in /dev/ttyACM* /dev/ttyUSB*; do [ -e "$d" ] && echo "$d  usb-serial=$(usb_serial "$d")  $(timeout 40 $PY $ET --port "$d" read_mac 2>&1 | grep -m1 '^MAC')"; done ;;
status)  send "" 9; nodes 6 ;;
cmd)     send "${2:?usage: hubctl.sh cmd \"<console command>\"}" 5; grep -E '^console:|^event|^mesh:|^p2p:|^\{' "$TMP" | tail -10 ;;
pcmd)    send "pcmd ${2:?node|all} ${3:?info|bcast|p2p|reboot} ${4:-0}" 5; grep -E '^console:' "$TMP" | tail -2 ;;
pdiag)   send "pdiag" 5; grep -E '^diag |no diagnostics' "$TMP" | cut -c1-260 ;;
pair)    send "pair" 8; grep -E 'pairing window|mesh: paired|refused' "$TMP" ;;
watch)   send "" "${2:-20}"; grep -E '^      node [0-9]+ ' "$TMP" | awk '{print}' | cut -c1-170 ;;
cal|cal-accept|cal-discard|cal-reset|cal-show)
    : "${HUB_URL:?set HUB_URL=http://<hub-ip> (the Pi must be able to reach the hub AP; this script changes no network config)}"
    case "$1" in
    cal)         curl -fsS -m 10 -X POST "$HUB_URL/api/calibrate?node=${2:?all|N}&sens=${3:-1}"; echo; echo "wait ~30 s with the room empty, then: hubctl.sh cal-show / cal-accept" ;;
    cal-accept)  curl -fsS -m 10 -X POST "$HUB_URL/api/calibrate/accept"; echo ;;
    cal-discard) curl -fsS -m 10 -X POST "$HUB_URL/api/calibrate/discard"; echo ;;
    cal-reset)   curl -fsS -m 10 -X POST "$HUB_URL/api/calibrate/reset?node=${2:?all|N}"; echo ;;
    cal-show)    curl -fsS -m 10 "$HUB_URL/api/calibration"; echo ;;
    esac ;;
*) sed -n '2,13p' "$0" ;;
esac
