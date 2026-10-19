#!/bin/bash
# Temporarily joins the Pi wlan0 to the bringup AP (SPECTRAL_BRINGUP) via a one-off /32-route profile, pings + curls it, then restores the home Wi-Fi profile ($HOME_CON, default: the one active on wlan0).
# Needs sudo and takes wlan0 off the home Wi-Fi for ~1 min; 192.168.69.1 is ALSO the home router IP, hence the /32-only route. Get user approval first.
# Usage: python -I serial_capture.py out.log 80 &   then   ./pi_wifi_csi_test.sh
HOME_CON=${HOME_CON:-$(nmcli -t -f NAME,DEVICE connection show --active | awk -F: '$2=="wlan0"{print $1; exit}')}
restore() {
  sudo nmcli connection down esp-bringup >/dev/null 2>&1
  sudo nmcli connection delete esp-bringup >/dev/null 2>&1
  sudo nmcli connection up "$HOME_CON" >/dev/null 2>&1
  echo "restored: $(sudo nmcli -t -f NAME,DEVICE connection show --active | grep wlan0)"
}
trap restore EXIT
found=0
for i in $(seq 1 12); do
  if sudo nmcli -t -f SSID dev wifi list ifname wlan0 --rescan yes | grep -qx SPECTRAL_BRINGUP; then found=1; break; fi
  sleep 3
done
echo "ssid visible: $found"; [ $found = 1 ] || exit 2
sudo nmcli connection add type wifi ifname wlan0 con-name esp-bringup ssid SPECTRAL_BRINGUP \
  connection.autoconnect no ipv4.method manual ipv4.addresses 192.168.69.250/32 \
  ipv4.routes "192.168.69.1/32" ipv4.never-default yes ipv6.method disabled >/dev/null || exit 3
timeout 40 sudo nmcli connection up esp-bringup ifname wlan0 | tail -n 1
ip -br addr show wlan0
ip route get 192.168.69.1
sleep 4
echo "--- ping (generates uplink frames the ESP can capture CSI from)"
ping -I wlan0 -c 40 -i 0.1 -W 1 192.168.69.1 | tail -n 3
echo "--- http"
for i in 1 2 3; do curl -s -m 5 --interface wlan0 http://192.168.69.1/; echo; done
sleep 3
