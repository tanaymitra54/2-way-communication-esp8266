# ANIL — wiring and physical setup

v1 is a link test. There is nothing to wire: two ESP8266 boards, one USB cable
each, and the onboard LED is the only "component". No sensor, no router, no
breadboard, no jumper wires.

## Bill of materials

| Item | Qty | Notes |
|---|---|---|
| ESP8266 board (NodeMCU v2/v3 or Wemos D1 mini) | 2 | Onboard USB-to-serial chip required |
| USB cable | 2 | **Data** cable, not charge-only — the most common "dead board" |
| USB power source | 2 | One 5 V port per board, or two USB chargers |

Common denominators: NodeMCU clones usually carry a **CH340** USB chip, D1
minis usually a **CP210x**. If no serial port appears when you plug in, that
is a driver problem, not a code problem.

## Pin map

| Signal | GPIO | Board pin | Used for |
|---|---|---|---|
| Onboard LED | GPIO2 | `D4` | Link activity indicator, **active LOW** |
| USB serial | — | — | 115200 baud, 8N1, power |

That is the complete list. No other pin is read or driven by either sketch.

### Pins to leave alone

| Pin | Why |
|---|---|
| GPIO0, GPIO2, GPIO15 | Boot strapping. Must be at their required levels during reset, so nothing external should pull them. GPIO2 is the LED; that is safe because the LED circuit holds the level at boot. |
| GPIO16 / `D0` | No interrupt support and no internal pull-up. Unused by design. |
| `A0` | 0–1.0 V on most boards (3.3 V behind a divider on NodeMCU only). Unused. |
| `TX`/`RX` (GPIO1/3) | Carries the serial console. Do not attach anything there while using the monitor. |

**Logic level is 3.3 V.** Never apply 5 V to a GPIO. USB VBUS (5 V) is for the
board's regulator only.

## Power

- One USB cable **per board**. Do not power one board from the other's `3V3`
  pin, and do not tie the two `3V3` rails together.
- Idle draw is roughly 80 mA; transmit peaks around 170–300 mA. A weak hub or a
  thin charge-only cable browns out and the board reboots at random — that
  looks exactly like a firmware crash, so rule power out before debugging code.
- For standalone use, any 5 V USB supply works. No common ground is needed
  between the two boards; the link is radio, not wires.

## Radio

| Setting | Value |
|---|---|
| Band | 2.4 GHz |
| Channel | **1**, hardcoded on both boards |
| Peer | broadcast `FF:FF:FF:FF:FF:FF` |
| Role | `ESP_NOW_ROLE_COMBO` on both |
| TX power | 20.5 dBm (maximum the ESP8266 core allows) |
| Rate | 1 DATA packet per second |

Both boards must be on the same channel. Nothing negotiates it — there is no
router involved — which is why both sketches print `wifi_get_channel()` at boot
and again in every 60 s report. If those disagree, the boards will never link
and no amount of reflashing will help.

Channel 1 is busy because it is the default for consumer routers. It is
hardcoded deliberately, for determinism. If you change it, change it in
**both** files.

Antenna orientation matters more than power: keep the two boards roughly
parallel and in line of sight. Same room is a certainty; through a couple of
walls expect tens of metres; outdoors, clear line of sight, 100 m+ is realistic.
The `RTT` figure is not a range or signal measurement — see below.

## Serial monitor

- **115200 baud, 8N1.** Two monitors, one per board.
- macOS: `screen /dev/cu.usbserial-* 115200` (CH340) or
  `/dev/cu.SLAB_USBtoUART 115200` (CP210x). List ports with
  `ls /dev/cu.*`.
- Exit `screen` with `Ctrl-A` then `K`.
- The boot banner names the role, so a board always identifies which firmware
  it is running. That is the check that prevents swapping the two sketches.

## Expected output

Node A, first report at t = 60 s:

```
[A] t=60s ch=1 TX=60 ACK=60 tot=60 loss=0% RTT=5ms bad=0 fail=0 dup=0
```

Node B, same moment:

```
[B] t=60s ch=1 RX=60 tot=60 ack=60 lastSeq=59 gaps=0 bad=0 fail=0
```

Reading the fields:

| Field | Node | Meaning | Scope |
|---|---|---|---|
| `TX` | A | DATA frames handed to ESP-NOW this window | per window |
| `ACK` | A | ACKs that closed a pending request this window | per window |
| `tot` | A | DATA frames sent since boot | cumulative |
| `loss` | A | `(TX - ACK) / TX` for the window | per window |
| `RTT` | A | mean send→ACK time this window | per window |
| `bad` | A/B | frames rejected (length, magic, version) | cumulative |
| `fail` | A/B | `esp_now_send` returned non-zero | cumulative |
| `dup` | A | ACKs not matching the pending seq | cumulative |
| `RX` | B | DATA packets accepted this window | per window |
| `tot` | B | DATA packets accepted since boot | cumulative |
| `ack` | B | ACK frames handed to ESP-NOW since boot | cumulative |
| `lastSeq` | B | last DATA seq accepted | cumulative |
| `gaps` | B | missing sequence numbers observed | cumulative |

### What RTT does and does not mean

`RTT` is measured on A, from handing the frame to ESP-NOW to the moment the ACK
callback fires. It includes **up to one full 10 ms loop period on B**, because B
queues the ACK and sends it from `loop()` instead of from inside the receive
callback (see the comment in `anil_node_b.ino`). Expect roughly 1–12 ms in the
same room, varying with where in B's loop the packet lands.

So RTT is a liveness and sanity indicator, not a signal-strength or range
measurement. A steady RTT with `loss=0` means the link is healthy. It is not
comparable between board pairs.

## LED behaviour

| Board | Pattern |
|---|---|
| A | Toggles on each matched ACK — so it blinks once per second while the link is up |
| B | Toggles on each accepted DATA packet — likewise once per second |

Both LEDs are dark at boot. Steady on or steady off with traffic flowing means
the link is down. The LED is wired active LOW, which the sketches handle
(`LOW` = on); nothing needs inverting externally.

## Flashing

Full procedure is in `README.md`. The wiring-relevant part:

1. Flash `anil_node_a/anil_node_a.ino` to one board and
   `anil_node_b/anil_node_b.ino` to the other. The folder name must match the
   `.ino` name, which is why each sketch lives in its own folder.
2. Watch each boot banner: it must say `node A` on one board and `node B` on
   the other.
3. If both say `node A`, you flashed the same file twice. Reflash the second.
   This is the failure the separate files and banners exist to catch.

## Troubleshooting

| Symptom | Likely cause |
|---|---|
| No serial port appears | Missing CH340/CP210x driver, or a charge-only USB cable |
| `FATAL: channel did not stick` | `wifi_set_channel` failed; a Wi-Fi reconnect or `WiFi.disconnect(true)` killed the radio |
| `add_peer(broadcast)=-1` or similar | ESP-NOW not initialised first, or the peer is already registered |
| A `TX` climbs, B `RX=0` | Channel mismatch, both boards on the same firmware, or one board browned out |
| B `RX` climbs, A `ACK=0` | B is receiving but its ACK send is failing — check `fail` on B |
| `bad` climbing | The two sketch copies have drifted apart; the structs no longer match |
| Random reboots mid-run | Power: weak hub, thin cable, or two boards on one port |
| `loss=1%` on the very first report only | Old firmware; the report/send ordering fix removed this |
| Both LEDs steady, no reports | Both boards are halted on a `FATAL` line — read the boot output |

## Known limitations (v1)

- **Broadcast only.** B's ACK is broadcast too, so with a third ESP8266 in
  range A cannot tell which node replied. Fine for two boards.
- **No encryption.** ESP-NOW is unencrypted here. Do not send anything private.
- **No retries.** Broadcast frames are never retransmitted by the radio;
  `loss` is measured, not corrected.
- **ACK queue is depth 1.** If two DATA packets arrive within one loop period,
  only one ACK is sent and A counts the other as lost. Impossible at 1 Hz;
  relevant only if `SEND_INTERVAL_MS` is reduced.
- **No RSSI.** The ESP8266 ESP-NOW receive callback
  (`esp_now_recv_cb_t`) carries no `rx_ctrl`, so RSSI would require
  promiscuous-mode sniffing, which conflicts with ESP-NOW.
- **Hardcoded channel.** Changing it means editing both sketches.
