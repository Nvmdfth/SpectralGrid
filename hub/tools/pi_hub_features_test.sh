#!/bin/bash
# End-to-end test of the backlog features against the REAL hub, from the Pi (wlan0 joined to SPECTRAL_GRID).
# Same safety as pi_hub_web_test.sh: one-off profile, static /32 address + /32 route only (192.168.69.1 is also the home
# router's address), no default route, no autoconnect, home Wi-Fi restored on any exit. Needs sudo. Takes ~10 minutes
# (it waits for the 5-minute "uncalibrated" drift prompt). Usage: SHOTS_DIR=... FW_HUB=... FW_PROBE=... ./pi_hub_features_test.sh
HOME_CON=${HOME_CON:-$(nmcli -t -f NAME,DEVICE connection show --active | awk -F: '$2=="wlan0"{print $1; exit}')}; S=${TMPDIR:-/tmp}
want() { [[ ",${SECTIONS:-1,2,3,4,5,6}," == *",$1,"* ]]; }
D=$(dirname "$0"); API=http://192.168.69.1; SHOTS=${SHOTS_DIR:-/tmp}
FW_HUB=${FW_HUB:-$D/../.pio/build/hub/firmware.bin}; FW_PROBE=${FW_PROBE:-$D/../../probe/.pio/build/probe/firmware.bin}
PASS=0; FAILN=0
restore() {
  sudo nmcli connection down esp-hub >/dev/null 2>&1; sudo nmcli connection delete esp-hub >/dev/null 2>&1
  sudo nmcli connection up "$HOME_CON" >/dev/null 2>&1
  echo "restored: $(nmcli -t -f NAME,DEVICE connection show --active | grep wlan0)"; echo "RESULT pass=$PASS fail=$FAILN"
}
trap restore EXIT
get()  { curl -s -m 12 --interface wlan0 "$API$1"; }
post() { curl -s -m 12 --interface wlan0 -X POST -w '\n%{http_code}' "$API$1"; }
code() { echo "$1" | tail -n1; }
body() { echo "$1" | sed '$d'; }
chk() { # name raw-json python-expr(d)
  local out; out=$(python3 - "$1" "$2" "$3" <<'PY'
import sys,json
name,raw,expr=sys.argv[1:4]
try: d=json.loads(raw); ok=bool(eval(expr))
except Exception as e: ok=False
print(("PASS " if ok else "FAIL ")+name+("" if ok else "  -> "+raw[:240].replace("\n"," ")))
PY
); echo "$out"; case "$out" in PASS*) PASS=$((PASS+1));; *) FAILN=$((FAILN+1));; esac; }
eq() { if [ "$2" = "$3" ]; then echo "PASS $1"; PASS=$((PASS+1)); else echo "FAIL $1 -> got '$2' want '$3'"; FAILN=$((FAILN+1)); fi; }
join() {
  for i in $(seq 1 20); do sudo nmcli -t -f SSID dev wifi list ifname wlan0 --rescan yes | grep -qx SPECTRAL_GRID && break; sleep 3; done
  sudo nmcli connection delete esp-hub >/dev/null 2>&1
  sudo nmcli connection add type wifi ifname wlan0 con-name esp-hub ssid SPECTRAL_GRID connection.autoconnect no ipv4.method manual \
    ipv4.addresses 192.168.69.250/32 ipv4.routes "192.168.69.1/32" ipv4.never-default yes ipv6.method disabled >/dev/null || return 1
  timeout 40 sudo nmcli connection up esp-hub ifname wlan0 >/dev/null 2>&1
}
serial_cmd() { echo "$1" > "${SERIAL_CMD_FILE:-$S/serial_cmd}"; sleep 2; }   # the capture process owns the port (a second open would reset the board)
browser() { node --experimental-websocket "$D/browser_check.mjs" "$@" 2>&1 | grep -v ExperimentalWarning; }

join || { echo "could not join the hub AP"; exit 2; }
ip route get 192.168.69.1 | head -1
ping -I wlan0 -i 0.1 -c 5000 -W 1 192.168.69.1 >/dev/null 2>&1 & PING=$!            # keeps the hub's AP-client CSI link fed
sleep 3

if want 1; then
echo "=== 1. clock + events"
r=$(post "/api/time?ms=$(( $(date +%s) * 1000 ))"); eq "time set" "$(code "$r")" 200
r=$(post "/api/time?ms=5"); eq "time rejects junk" "$(code "$r")" 400
chk "events: boot + clock events with epoch" "$(get '/api/events?limit=50')" "d['epoch_known'] and any(e['text']=='hub boot' for e in d['events']) and any('clock' in e['text'] for e in d['events']) and d['last']>=2"
chk "events: since filter" "$(get '/api/events?since=999999')" "d['events']==[]"
fi

if want 2; then
echo "=== 2. tags + control probe"
r=$(post "/api/tag?kind=hvac&dur=120&note=furnace"); eq "tag hvac accepted" "$(code "$r")" 200
r=$(post "/api/tag?kind=ghost&dur=10"); eq "tag bad kind rejected" "$(code "$r")" 400
r=$(post "/api/tag?kind=note&note=%3Cscript%3Ealert(1)%3C%2Fscript%3E"); eq "tag note with markup accepted (rendered as text)" "$(code "$r")" 200
r=$(post "/api/tag/clear?kind=pet"); eq "clear unknown tag 404" "$(code "$r")" 404
python3 -I "$D/ws_probe.py" 192.168.69.250 3 > $S/ws_tag.txt; chk "websocket frame carries the active tag" "$(grep '^last frame' $S/ws_tag.txt | sed 's/^last frame: //')" "any(t['k']=='hvac' and t['left']>0 for t in d.get('tags',[]))"
r=$(post "/api/control?node=99"); eq "control out of range" "$(code "$r")" 400
r=$(post "/api/control?node=abc"); eq "control junk" "$(code "$r")" 400
r=$(post "/api/control?node=1"); eq "control set" "$(code "$r")" 200
chk "control event logged" "$(get '/api/events?limit=30')" "any(e['type']=='control' and 'control probe' in e['text'] for e in d['events'])"
r=$(post "/api/control?node=none"); eq "control cleared" "$(code "$r")" 200
r=$(post "/api/tag/clear?kind=hvac"); eq "clear hvac tag" "$(code "$r")" 200
fi

if want 3; then
echo "=== 3. session recording + exports (waits for >= 24 s of data)"
sleep 22
chk "session info" "$(get /api/session/info)" "d['count']>=20 and d['rate_hz']==2 and d['newest_ms']>d['oldest_ms'] and d['bytes_cap']>=1000000"
chk "session frames shape" "$(get '/api/session/frames?limit=5')" "d['n']==5 and d['frames'][0]['nodes'][0]['id']==0 and 'vector' in d['frames'][0] and 'vbat' in d['frames'][0]['nodes'][0]"
T0=$(get '/api/session/info' | python3 -c 'import sys,json; print(json.load(sys.stdin)["oldest_ms"])')
chk "session frames seek by time" "$(get "/api/session/frames?limit=3&from=$((T0+5000))")" "d['n']==3 and d['frames'][0]['t']>=$((T0+5000))"
curl -s -m 30 --interface wlan0 "$API/api/session.csv" -o $S/session.csv
eq "session csv header" "$(head -n1 $S/session.csv | cut -c1-30)" "t_ms,wall_utc,score,vector,vec"
ROWS=$(wc -l < $S/session.csv); [ "$ROWS" -ge 20 ] && { echo "PASS session csv rows ($ROWS)"; PASS=$((PASS+1)); } || { echo "FAIL session csv rows ($ROWS)"; FAILN=$((FAILN+1)); }
eq "session csv has wall clock" "$(sed -n 2p $S/session.csv | cut -d, -f2 | cut -c1-2)" "20"
curl -s -m 20 --interface wlan0 "$API/api/events.csv" -o $S/events.csv
eq "events csv header" "$(head -n1 $S/events.csv)" "seq,t_ms,wall_utc,type,node,score,text"
eq "events csv keeps note text as data" "$(grep -c 'script' $S/events.csv)" 1
fi

if want 4; then
echo "=== 4. dashboard in a real browser (live hub)"
cat > $S/steps_live.json <<JSON
[{"waitfor":"document.querySelector('#c-link').textContent==='link live'","timeout":15000},
 {"waitfor":"document.querySelectorAll('#log div').length>=3","timeout":8000},
 {"eval":"({link:document.querySelector('#c-link').textContent,demo:!document.querySelector('#c-demo').classList.contains('hide'),log:[...document.querySelectorAll('#log div')].slice(0,4).map(x=>x.textContent),xssNodes:document.querySelectorAll('#log script').length})","print":true},
 {"click":"[data-tag='phone']"},{"wait":1500},
 {"eval":"({chips:[...document.querySelectorAll('#tagchips .tagchip')].map(x=>x.textContent)})","print":true},
 {"click":"#b-repload"},{"waitfor":"document.querySelector('#repinfo').textContent.includes('frames')","timeout":8000},
 {"click":"#b-repplay"},{"wait":4000},
 {"eval":"({replayChip:!document.querySelector('#c-replay').classList.contains('hide'),info:document.querySelector('#repinfo').textContent,t:document.querySelector('#reptime').textContent,slider:document.querySelector('#rep-slider').value})","print":true},
 {"shot":"$SHOTS/live_replay.jpg"},{"click":"#b-replive"},{"wait":800},
 {"eval":"({replayChip:!document.querySelector('#c-replay').classList.contains('hide')})","print":true},
 {"shot":"$SHOTS/live_dashboard.jpg"}]
JSON
browser "$API/" $S/steps_live.json 1100 1700
cat > $S/steps_mob.json <<JSON
[{"waitfor":"document.querySelector('#c-link').textContent==='link live'","timeout":15000},{"wait":2000},{"shot":"$SHOTS/live_mobile_full.jpg"}]
JSON
browser "$API/" $S/steps_mob.json 390 2600
post "/api/tag/clear?kind=phone" >/dev/null
fi

if want 5; then
echo "=== 5. OTA gating, safe-fail and a real update"
chk "ota status (closed)" "$(get /api/ota/status)" "d['window']==False and d['partition'] in ('app0','app1') and len(d['sketch_md5'])==32 and d['probe_ota_enabled']==False"
P0=$(get /api/ota/status | python3 -c 'import sys,json; print(json.load(sys.stdin)["partition"])')
r=$(curl -s -m 60 --interface wlan0 -F "file=@$FW_HUB" -w '\n%{http_code}' "$API/api/ota"); eq "ota refused while the window is closed" "$(code "$r")" 403
r=$(post "/api/ota/window"); eq "window cannot be opened over Wi-Fi" "$(code "$r")" 403
r=$(post "/api/ota/probe?node=1"); eq "probe OTA not provisioned -> 501" "$(code "$r")" 501
serial_cmd ota; sleep 1
chk "ota window opened from the serial console" "$(get /api/ota/status)" "d['window']==True and d['left_s']>200"
head -c 200000 "$FW_HUB" > $S/fw_trunc.bin
r=$(curl -s -m 60 --interface wlan0 -F "file=@$S/fw_trunc.bin" -w '\n%{http_code}' "$API/api/ota"); eq "truncated image rejected" "$(code "$r")" 500
chk "hub still running its old firmware after the failed update" "$(get /api/ota/status)" "d['partition']=='$P0'"
r=$(curl -s -m 60 --interface wlan0 -F "file=@$FW_PROBE" -w '\n%{http_code}' "$API/api/fw/probe"); eq "probe firmware stored on the hub" "$(code "$r")" 200
chk "probe fw reported by status" "$(get /api/ota/status)" "d['probe_fw']['size']==$(stat -c %s "$FW_PROBE")"
curl -s -m 60 --interface wlan0 "$API/fw/probe.bin" -o $S/probe_dl.bin
eq "probe firmware downloads intact (md5)" "$(md5sum < $S/probe_dl.bin | cut -d' ' -f1)" "$(md5sum < "$FW_PROBE" | cut -d' ' -f1)"
UP0=$(get /api/status | python3 -c 'import sys,json; print(json.load(sys.stdin)["uptime_ms"])')
r=$(curl -s -m 120 --interface wlan0 -F "file=@$FW_HUB" -w '\n%{http_code}' "$API/api/ota"); eq "hub firmware update accepted" "$(code "$r")" 200
echo "waiting for the hub to reboot and its AP to return..."
sleep 12; UP1=-1
for i in $(seq 1 30); do
  sudo nmcli connection down esp-hub >/dev/null 2>&1; sleep 2; timeout 25 sudo nmcli connection up esp-hub ifname wlan0 >/dev/null 2>&1
  UP1=$(get /api/status 2>/dev/null | python3 -c 'import sys,json; print(json.load(sys.stdin)["uptime_ms"])' 2>/dev/null) && [ -n "$UP1" ] && break; UP1=-1; sleep 3
done
[ "$UP1" != "-1" ] && [ "$UP1" -lt "$UP0" ] && { echo "PASS hub rebooted (uptime $UP0 -> $UP1 ms)"; PASS=$((PASS+1)); } || { echo "FAIL hub did not come back (uptime $UP0 -> $UP1)"; FAILN=$((FAILN+1)); }
P1=$(get /api/ota/status | python3 -c 'import sys,json; print(json.load(sys.stdin)["partition"])')
[ "$P1" != "$P0" ] && { echo "PASS running partition flipped $P0 -> $P1"; PASS=$((PASS+1)); } || { echo "FAIL partition unchanged ($P0)"; FAILN=$((FAILN+1)); }
chk "events survived the reboot (flash persistence)" "$(get '/api/events?limit=300')" "any(e['text']=='hub boot' for e in d['events']) and sum(1 for e in d['events'] if e['text']=='hub boot')>=2 and any(e['type']=='tag' for e in d['events'])"
chk "stored probe firmware survived the reboot" "$(get /api/ota/status)" "d['probe_fw']['size']==$(stat -c %s "$FW_PROBE")"
kill $PING 2>/dev/null; ping -I wlan0 -i 0.1 -c 5000 -W 1 192.168.69.1 >/dev/null 2>&1 & PING=$!
post "/api/time?ms=$(( $(date +%s) * 1000 ))" >/dev/null
fi

if want 6; then
echo "=== 6. drift prompt (an uncalibrated node online for 5 minutes) + calibration clears it"
for i in $(seq 1 120); do U=$(get /api/status | python3 -c 'import sys,json; print(json.load(sys.stdin)["uptime_ms"])' 2>/dev/null || echo 0); [ "${U:-0}" -ge 312000 ] && break; sleep 5; done
chk "drift event for the uncalibrated hub node" "$(get '/api/events?limit=100')" "any(e['type']=='drift' and 'not calibrated' in e['text'] for e in d['events'])"
python3 -I "$D/ws_probe.py" 192.168.69.250 2 > $S/ws_drift.txt
chk "websocket frame carries the drift prompt" "$(grep '^last frame' $S/ws_drift.txt | sed 's/^last frame: //')" "any(x['id']==0 and x['r']=='uncalibrated' for x in d.get('drift',[]))"
cat > $S/steps_drift.json <<JSON
[{"waitfor":"document.querySelector('#c-link').textContent==='link live'","timeout":15000},
 {"waitfor":"document.querySelector('#driftbar .dr')","timeout":8000},
 {"eval":"document.querySelector('#driftbar').textContent","print":true},{"shot":"$SHOTS/live_drift.jpg"}]
JSON
browser "$API/" $S/steps_drift.json 1100 1700
r=$(post "/api/calibrate?node=all&sens=1"); eq "calibration started" "$(code "$r")" 202; sleep 18
r=$(post "/api/calibrate/accept"); eq "calibration accepted" "$(code "$r")" 200
echo "waiting 38 s for the drift prompt to clear..."; sleep 38
python3 -I "$D/ws_probe.py" 192.168.69.250 2 > $S/ws_drift2.txt
chk "drift prompt cleared after calibration" "$(grep '^last frame' $S/ws_drift2.txt | sed 's/^last frame: //')" "not d.get('drift')"
r=$(post "/api/calibrate/reset?node=all"); eq "calibration reset (leave the board clean)" "$(code "$r")" 200
kill $PING 2>/dev/null
fi
