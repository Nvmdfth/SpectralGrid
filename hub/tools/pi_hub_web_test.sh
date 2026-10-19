#!/bin/bash
# Joins the Pi wlan0 to the hub AP (SPECTRAL_GRID) via a one-off /32-route profile and exercises the web stack.
# Needs sudo; takes wlan0 off the home Wi-Fi for ~1-2 min; 192.168.69.1 is ALSO the home router IP, hence the /32-only
# route. Get user approval first. Restores the home profile on any exit.
HOME_CON=${HOME_CON:-$(nmcli -t -f NAME,DEVICE connection show --active | awk -F: '$2=="wlan0"{print $1; exit}')}; S=${TMPDIR:-/tmp}
D=$(dirname "$0")
restore() {
  sudo nmcli connection down esp-hub >/dev/null 2>&1
  sudo nmcli connection delete esp-hub >/dev/null 2>&1
  sudo nmcli connection up "$HOME_CON" >/dev/null 2>&1
  echo "restored: $(nmcli -t -f NAME,DEVICE connection show --active | grep wlan0)"
}
trap restore EXIT
found=0
for i in $(seq 1 15); do
  if sudo nmcli -t -f SSID dev wifi list ifname wlan0 --rescan yes | grep -qx SPECTRAL_GRID; then found=1; break; fi
  sleep 3
done
echo "ssid visible: $found"; [ $found = 1 ] || exit 2
sudo nmcli connection add type wifi ifname wlan0 con-name esp-hub ssid SPECTRAL_GRID \
  connection.autoconnect no ipv4.method manual ipv4.addresses 192.168.69.250/32 \
  ipv4.routes "192.168.69.1/32" ipv4.never-default yes ipv6.method disabled >/dev/null || exit 3
timeout 40 sudo nmcli connection up esp-hub ifname wlan0 | tail -n 1
ip route get 192.168.69.1 | head -1
sleep 3
echo "--- ping"; ping -I wlan0 -c 20 -i 0.1 -W 1 192.168.69.1 | tail -n 2
echo "--- GET / (gzip)"; curl -s -m 8 --interface wlan0 -D - -o $S/hub_index.html --compressed http://192.168.69.1/ | grep -iE "^HTTP|content-(encoding|length|type)"
echo "bytes after decompress: $(wc -c < $S/hub_index.html)  has_title: $(grep -c SPECTRALGRID $S/hub_index.html)"
echo "--- captive redirect"; curl -s -m 5 --interface wlan0 -o /dev/null -w "generate_204 -> %{http_code} %{redirect_url}\n" http://192.168.69.1/generate_204
curl -s -m 5 --interface wlan0 -o /dev/null -w "random path   -> %{http_code} %{redirect_url}\n" http://192.168.69.1/no/such/page
echo "--- REST"
curl -s -m 5 --interface wlan0 http://192.168.69.1/api/status; echo
curl -s -m 5 --interface wlan0 http://192.168.69.1/api/nodes; echo
curl -s -m 5 --interface wlan0 -X POST -w " [%{http_code}]\n" http://192.168.69.1/api/calibrate
curl -s -m 5 --interface wlan0 -X POST -w " [%{http_code}]\n" "http://192.168.69.1/api/unpair"
curl -s -m 5 --interface wlan0 -X POST -w " [%{http_code}]\n" http://192.168.69.1/api/pair
curl -s -m 5 --interface wlan0 http://192.168.69.1/api/status; echo
echo "--- websocket 8 s"; python3 -I "$D/ws_probe.py" 192.168.69.250 8
echo "--- ping during ws (keeps CSI flowing)"; ping -I wlan0 -c 30 -i 0.1 -W 1 192.168.69.1 | tail -n 2

SHOTS=${SHOTS_DIR:-/tmp}
shot() { # name width height budget_ms
  timeout 60 chromium --headless=new --no-sandbox --disable-gpu --hide-scrollbars --window-size=$2,$3 \
    --virtual-time-budget=$4 --screenshot="$SHOTS/$1.png" http://192.168.69.1/ >/dev/null 2>&1
  echo "screenshot $1: $(ls -la "$SHOTS/$1.png" 2>/dev/null | awk '{print $5}') bytes"
}
echo "--- live screenshots (idle)"
shot live_desktop 1100 1500 9000
shot live_mobile 390 1500 9000

api() { curl -s -m 5 --interface wlan0 -X POST -w " [%{http_code}]" "http://192.168.69.1$1"; echo; }
cal() { curl -s -m 5 --interface wlan0 http://192.168.69.1/api/calibration; echo; }
echo "--- calibration: input validation and sequencing"
api "/api/calibrate?node=abc"
api "/api/calibrate/accept"
echo "--- calibration cycle 1 (capture, review, accept, reset)"
ping -I wlan0 -i 0.1 -c 600 -W 1 192.168.69.1 >/dev/null 2>&1 & PING=$!      # keeps the hub's AP-client CSI link fed
api "/api/calibrate?node=all&sens=1"
sleep 2; api "/api/calibrate?node=all"; cal
python3 -I "$D/ws_probe.py" 192.168.69.250 3 | tail -n 1 | cut -c1-260
echo "--- live screenshot while capturing"; shot live_calibrating 1100 1500 6000
sleep 4; echo "--- pending"; cal
shot live_calibration_review 1100 1500 7000
api "/api/calibrate/accept"; echo "applied:"; cal
api "/api/calibrate/reset?node=all"; echo "after reset:"; cal
echo "--- calibration cycle 2 (left applied for the reboot persistence check)"
api "/api/calibrate?node=all&sens=1"; sleep 17; cal | cut -c1-200
api "/api/calibrate/accept"; echo "applied:"; cal
kill $PING 2>/dev/null
sleep 1
