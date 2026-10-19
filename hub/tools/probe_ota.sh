#!/bin/bash
# Update the probe over the air, with both boards on the Pi (hub /dev/ttyACM0, probe /dev/ttyACM1, UART ports). Verified 2026-10-08:
# probe logs "ota: success, rebooting" then "running partition app1/app0", back online in ~10 s. Needs sudo; takes wlan0 off the home Wi-Fi for ~2.5 min.
# Usage: hub/tools/probe_ota.sh [firmware.bin]   (default: probe/.pio/build/probe/firmware.bin, the app image, not firmware.factory.bin)
S=${TMPDIR:-/tmp}; LOGS=${LOGS:-$S}; HOME_CON=${HOME_CON:-$(nmcli -t -f NAME,DEVICE connection show --active | awk -F: '$2=="wlan0"{print $1; exit}')}; API=http://192.168.69.1
FW=${1:-/home/pi/Projects/SpectralNode/probe/.pio/build/probe/firmware.bin}
restore(){ kill $C0 $C1 2>/dev/null; sudo nmcli connection down esp-hub >/dev/null 2>&1; sudo nmcli connection delete esp-hub >/dev/null 2>&1; sudo nmcli connection up "$HOME_CON" >/dev/null 2>&1; echo "restored: $(nmcli -t -f NAME,DEVICE connection show --active | grep wlan0)"; }
trap restore EXIT
get(){ curl -s -m 12 --interface wlan0 "$API$1"; }
stty -F /dev/ttyACM0 115200 raw -echo -hupcl; stty -F /dev/ttyACM1 115200 raw -echo -hupcl
cat /dev/ttyACM0 > $LOGS/ota_hub.log & C0=$!; cat /dev/ttyACM1 > $LOGS/ota_probe.log & C1=$!
found=0; for i in $(seq 1 15); do sudo nmcli -t -f SSID dev wifi list ifname wlan0 --rescan yes | grep -qx SPECTRAL_GRID && { found=1; break; }; sleep 3; done
echo "ssid visible: $found"; [ $found = 1 ] || exit 2
sudo nmcli connection add type wifi ifname wlan0 con-name esp-hub ssid SPECTRAL_GRID connection.autoconnect no ipv4.method manual ipv4.addresses 192.168.69.250/32 ipv4.routes "192.168.69.1/32" ipv4.never-default yes ipv6.method disabled >/dev/null || exit 3
timeout 40 sudo nmcli connection up esp-hub ifname wlan0 | tail -n 1; sleep 3
echo ota > /dev/ttyACM0; sleep 2
echo "window: $(get /api/ota/status | cut -c1-120)"
echo "store fw: $(curl -s -m 90 --interface wlan0 -F "file=@$FW" -w ' [%{http_code}]' $API/api/fw/probe)"
echo "send ota: $(curl -s -m 12 --interface wlan0 -X POST -w ' [%{http_code}]' "$API/api/ota/probe?node=1")"
for i in $(seq 1 24); do sleep 5; echo "t+$((i*5))s $(get /api/nodes | cut -c1-90)"; done
echo "probe log: $LOGS/ota_probe.log"; grep -a "ota:\|running partition\|boot state" $LOGS/ota_probe.log | head -6
