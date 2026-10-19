#!/bin/bash
# OTA one probe by node number while the hub is on the Pi (probes need no USB). Same recipe as probe_ota.sh, generalised.
# Usage: hub/tools/probe_ota_node.sh <node> [firmware.bin]      env: HUB_IP (default 192.168.69.1), HUB_PORT (default: found by USB serial)
# Needs sudo; takes wlan0 off the home Wi-Fi for ~2 min and restores it on exit. Judge the result afterwards with hubctl.sh status
# (the node goes off for ~10-20 s, then back ON). There is no OTA rollback (#22): a bad image strands the probe until a USB reflash.
NODE=${1:?usage: probe_ota_node.sh <node> [firmware.bin]}
FW=${2:-/home/pi/Projects/SpectralNode/probe/.pio/build/probe/firmware.bin}
HUB_IP=${HUB_IP:-192.168.69.1}; ADDR=${HUB_IP%.*}.250; API=http://$HUB_IP
HOME_CON=${HOME_CON:-$(nmcli -t -f NAME,DEVICE connection show --active | awk -F: '$2=="wlan0"{print $1; exit}')}; HUB_SERIAL=${HUB_SERIAL:-5C63053377}
if [ -z "${HUB_PORT:-}" ]; then
    for d in /dev/ttyACM* /dev/ttyUSB*; do [ -e "$d" ] && [ "$(udevadm info -q property -n "$d" | sed -n 's/^ID_SERIAL_SHORT=//p')" = "$HUB_SERIAL" ] && HUB_PORT=$d; done
fi
[ -n "${HUB_PORT:-}" ] || { echo "hub port not found"; exit 1; }
[ -f "$FW" ] || { echo "no firmware at $FW"; exit 1; }
restore(){ sudo nmcli connection down esp-hub >/dev/null 2>&1; sudo nmcli connection delete esp-hub >/dev/null 2>&1; sudo nmcli connection up "$HOME_CON" >/dev/null 2>&1; echo "restored: $(nmcli -t -f NAME,DEVICE connection show --active | grep wlan0)"; }
trap restore EXIT
get(){ curl -s -m 12 --interface wlan0 "$API$1"; }
stty -F "$HUB_PORT" 115200 raw -echo -hupcl
found=0; for i in $(seq 1 15); do sudo nmcli -t -f SSID dev wifi list ifname wlan0 --rescan yes | grep -qx SPECTRAL_GRID && { found=1; break; }; sleep 3; done
echo "ssid visible: $found"; [ $found = 1 ] || exit 2
sudo nmcli connection add type wifi ifname wlan0 con-name esp-hub ssid SPECTRAL_GRID connection.autoconnect no ipv4.method manual ipv4.addresses $ADDR/32 ipv4.routes "$HUB_IP/32" ipv4.never-default yes ipv6.method disabled >/dev/null || exit 3
timeout 40 sudo nmcli connection up esp-hub ifname wlan0 | tail -n 1; sleep 3
printf '\n' > "$HUB_PORT"; sleep 0.5; echo ota > "$HUB_PORT"; sleep 2
echo "window: $(get /api/ota/status | cut -c1-120)"
echo "store fw: $(curl -s -m 90 --interface wlan0 -F "file=@$FW" -w ' [%{http_code}]' $API/api/fw/probe)"
echo "send ota node $NODE: $(curl -s -m 12 --interface wlan0 -X POST -w ' [%{http_code}]' "$API/api/ota/probe?node=$NODE")"
sleep 60
