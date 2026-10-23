# ANIL — 2-way ESP8266 link over ESP-NOW

Two ESP8266 boards talking directly to each other with no router, no Wi-Fi
network and no broker. This is **v1: prove the link**. No sensors, no
encryption, no payload beyond a counter.

```
Node A  ──── DATA (seq, uptime) ────►  Node B
        ◄─── ACK  (same seq)     ────
```

Node A is the initiator: it sends one DATA packet per second and measures
round-trip time and packet loss. Node B replies with an ACK carrying the same
`seq`, so A can match the reply to the request. Both boards toggle their
onboard LED on every packet they accept.

| File | Board | Role |
|---|---|---|
| `anil_node_a/anil_node_a.ino` | ESP8266 | Sender, measures RTT + loss |
| `anil_node_b/anil_node_b.ino` | ESP8266 | Receiver, replies ACK |
| `WIRING.md` | — | Pin map and physical setup |

## Wire format

12 bytes, `__attribute__((packed))`, identical in both sketches:

| Offset | Field | Type | Notes |
|---|---|---|---|
| 0 | `magic` | `uint8_t` | `0xA1` |
| 1 | `version` | `uint8_t` | `0x01` |
| 2 | `senderId` | `uint8_t` | `1` = A, `2` = B |
| 3 | `type` | `uint8_t` | `0` = DATA, `1` = ACK |
| 4 | `seq` | `uint32_t` | monotonic per sender |
| 8 | `uptimeMs` | `uint32_t` | sender `millis()` at transmit |

Both files carry `static_assert(sizeof(anil_packet_t) == 12, ...)`. If you
change the layout you must change it in both files and bump `version`.

## Settings

| Setting | Value | Where |
|---|---|---|
| Channel | `1`, hardcoded | `ESPNOW_CHANNEL` |
| Peer | broadcast `FF:FF:FF:FF:FF:FF` | `esp_now_add_peer` |
| Role | `ESP_NOW_ROLE_COMBO` on both | `esp_now_set_self_role` |
| Interval | 1000 ms | `SEND_INTERVAL_MS` (node A) |
| Report | every 60 s | `REPORT_WINDOW_MS` |
| Baud | 115200 | `Serial.begin` |

Channel 1 is the ESP8266 default, which is why both sketches print the
readback from `wifi_get_channel()` at boot. If that line does not say `1` on
both boards, they will never link — and without the readback a stale channel
looks exactly like broken hardware.

## Flashing

No CLI toolchain is used — build and upload from the Arduino IDE.

1. **Install the ESP8266 core.** *Arduino IDE → Settings → Additional Board
   Manager URLs*, add:
   ```
   https://arduino.esp8266.com/stable/package_esp8266com_index.json
   ```
   Then *Tools → Board → Boards Manager* → install **esp8266** by ESP8266
   Community. Tested against **3.1.2**.

2. **Select the board.** *Tools → Board → ESP8266 → NodeMCU 1.0 (ESP-12E
   Module)* — or *LOLIN(WEMOS) D1 R2 & mini* if that is what you have. The
   sketches only use GPIO2, which is identical on both.

3. **Upload node A.** Open `anil_node_a/anil_node_a.ino`, pick the port, upload.
   The Arduino IDE requires the sketch folder name to match the `.ino` name —
   both already do.

4. **Upload node B.** Same procedure with `anil_node_b/anil_node_b.ino`.

5. **Open both serial monitors at 115200.** You need two instances: run the
   IDE once per board, or use any serial terminal (`screen`, `minicom`) for
   the second.

## Pass criteria

Run for **60 seconds** with both boards powered.

| Node | Expected |
|---|---|
| A | `[A] t=60s ch=1 TX=60 ACK=60 loss=0% RTT=<N>ms bad=0 fail=0` |
| B | `[B] t=60s ch=1 RX=60 lastSeq=59 gaps=0 bad=0 fail=0` |
| Both | onboard LED toggles once per packet |

**Pass:** `loss=0%`, `gaps=0`, `bad=0`, `fail=0`, and both LEDs visibly
toggling. Loss under a few percent with boards in the same room still counts
as a working link but is worth investigating.

**Fail signals and what they mean:**

| Symptom | Likely cause |
|---|---|
| `ch=0` or `ch` not `1` | `wifi_set_channel` did not stick — sketches print `FATAL` |
| A sends, B `RX=0` | Channel mismatch, or `WiFi.disconnect(true)` was added (kills the radio) |
| `add_peer` returns non-zero | Peer already registered, or `esp_now_init` failed first |
| `bad` climbing | Struct mismatch — the two `.ino` files have drifted |
| `fail` climbing | `esp_now_send` refused; check the logged return code |

## Known v1 limitations

- **Broadcast only.** B's ACKs are broadcast too, so on a bench with a third
  ESP8266 in range, A cannot tell which node replied. Fine for two boards.
- **No encryption.** ESP-NOW is unencrypted here. Do not send anything you
  care about.
- **No peer discovery.** The peer address is hardcoded to broadcast. Adding a
  second node means adding real MACs.
- **No delivery guarantee.** Broadcast frames are never retransmitted by the
  radio; `loss%` is the measurement, not a correction.
- **RSSI is not reported.** The ESP8266 ESP-NOW receive callback
  (`esp_now_recv_cb_t`) provides no `rx_ctrl`, so RSSI is unavailable without
  promiscuous-mode sniffing, which conflicts with ESP-NOW.
