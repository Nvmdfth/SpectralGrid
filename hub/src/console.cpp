// Serial console (UART): lets the host that owns the board drive privileged actions without touching the open Wi-Fi.
#include <Arduino.h>
#include "hub.h"
#include "ota.h"
#include "logger.h"
#include "csicap.h"
#include "fieldlog.h"
#include "calibration.h"
#include "p2p.h"

void console_loop() {
    static String line;
    while (Serial.available()) {
        char c = (char)Serial.read();
        if (c != '\n' && c != '\r') { if (line.length() < 64) line += c; continue; }
        line.trim();
        if (!line.length()) continue;
        if (line == "help") Serial.println("console: help | status | pair | pcmd <node|all> <info|bcast|p2p|links|reboot|ident> [0|1] | pdiag | p2p [on|off] | ota | ota-close | time <epoch_ms> | csicap on|off | csilabel <name> | field on|off|status | presdbg <dB>");
        else if (line == "status") Serial.printf("console: paired=%d pairing=%d ota=%d heap=%u uptime=%us\n", mesh_paired_count(), (int)mesh_pairing_open(), (int)ota_window_open(), (unsigned)ESP.getFreeHeap(), (unsigned)(millis() / 1000));
        else if (line.startsWith("pcmd ")) {      // pcmd <node|all> <info|bcast|p2p|links|reboot|ident> [arg]
            char nb[8] = "", ob[12] = ""; int arg = 0;
            int got = sscanf(line.c_str() + 5, "%7s %11s %d", nb, ob, &arg);
            int op = proto_cmd_op(ob), node = !strcmp(nb, "all") ? 0 : atoi(nb);
            if (got < 2 || op < 0 || (node == 0 && strcmp(nb, "all"))) Serial.println("console: usage: pcmd <node|all> <info|bcast|p2p|links|reboot|ident> [0|1]");
            else Serial.println(mesh_send_cmd(node, (uint8_t)op, arg) ? "console: command sent" : "console: not sent (no such probe, or probe commands not provisioned)");
        }
        else if (line == "p2p" || line == "p2p on" || line == "p2p off") {      // probe-to-probe link presence: status, or switch the feature
            if (line != "p2p") p2p_set_enabled(line == "p2p on");
            Serial.printf("p2p: %s %s\n", p2p_enabled() ? "on" : "off", p2p_json().c_str());
        }
        else if (line == "pdiag") {
            bool any = false;
            for (int id = 1; id <= MESH_MAX_PROBES; id++) {
                PktDiag d; uint32_t age;
                if (!mesh_get_diag((uint8_t)id, &d, &age)) continue;
                any = true;
                Serial.printf("diag n%d age=%.1fs build=%08x part=%u flags=%02x up=%us heap=%uK ack=%u peers:", id, age / 1000.0f, (unsigned)d.build_id, d.part, d.flags, (unsigned)d.uptime_s, d.heap_kb * 4, d.ack_seq);
                for (int i = 0; i < d.npeers && i < DIAG_MAX_PEERS; i++) Serial.printf(" [n%u fps=%u rssi=%.1f tstd=%.4f win=%u]", d.peer[i].node, d.peer[i].fps, d.peer[i].rssi_x10 / 10.0f, d.peer[i].tstd_x10000 / 10000.0f, d.peer[i].win_seq);
                Serial.println();
            }
            if (!any) Serial.println("console: no diagnostics received yet (send: pcmd all info)");
        }
        else if (line == "pair") { mesh_open_pairing(60000); Serial.println("console: pairing window open for 60 s"); }
        else if (line == "ota") { ota_open_window(300000); Serial.println("console: OTA window open for 5 min"); }
        else if (line == "ota-close") { ota_close_window(); Serial.println("console: OTA window closed"); }
        else if (line.startsWith("presdbg ")) { calibration_debug_shift_baseline(line.substring(8).toFloat()); Serial.println("console: presence baseline shifted (test aid)"); }
        else if (line == "field on") Serial.println(fieldlog_set(true) ? "console: field log on" : "console: field log could not start");
        else if (line == "field off") { fieldlog_set(false); Serial.println("console: field log off"); }
        else if (line == "field status") Serial.printf("console: %s\n", fieldlog_status_json().c_str());
        else if (line == "csicap on") csicap_start();
        else if (line == "csicap off") csicap_stop();
        else if (line.startsWith("csilabel ")) { if (!csicap_label(line.c_str() + 9)) Serial.println("console: csilabel ignored (capture off or empty name)"); }
        else if (line.startsWith("time ")) { logger_set_epoch_ms(strtoll(line.c_str() + 5, nullptr, 10)); Serial.println("console: clock set"); }
        else Serial.printf("console: unknown command '%s' (try help)\n", line.c_str());
        line = "";
    }
}
