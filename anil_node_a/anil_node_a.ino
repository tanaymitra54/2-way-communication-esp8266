/*
 * ANIL — Node A (sender / initiator)
 * Board: ESP8266 (NodeMCU v2 / Wemos D1 mini)
 *
 * v1 scope: prove the ESP-NOW link only. No sensors, no router, no encryption.
 *
 * Node A sends a DATA packet once per second to the broadcast address.
 * Node B replies with an ACK carrying the same seq. A measures RTT and loss.
 *
 * Companion sketch: anil_node_b/anil_node_b.ino
 * The packet struct below MUST stay byte-identical in both files (12 bytes).
 * 4 x uint8_t + 2 x uint32_t = 12. Do not reorder, do not add fields
 * without bumping VERSION and updating the other node.
 */

#include <ESP8266WiFi.h>
// Both SDK headers carry their own extern "C" guards — no wrapper needed.
#include <espnow.h>
#include <user_interface.h>

// ---------------------------------------------------------------------------
// Identity / configuration
// ---------------------------------------------------------------------------
#define NODE_ID           1        // 1 = A, 2 = B
#define PEER_ID           2
#define ESPNOW_CHANNEL    1
#define LED_PIN           2        // onboard LED = GPIO2, ACTIVE LOW
#define SEND_INTERVAL_MS  1000
#define REPORT_WINDOW_MS  60000    // 60 s summary window

// ESP8266 core exposes WiFi.setOutputPower(float dBm) only, clamped 0..20.5.
// There is no WIFI_OUTPUT_POWER constant in this core — that is ESP32 lore.
#define TX_POWER_DBM      20.5f

// ---------------------------------------------------------------------------
// Wire format — keep identical in anil_node_a.ino and anil_node_b.ino
// ---------------------------------------------------------------------------
static const uint8_t PKT_MAGIC   = 0xA1;
static const uint8_t PKT_VERSION = 0x01;
static const uint8_t TYPE_DATA   = 0;
static const uint8_t TYPE_ACK    = 1;

struct __attribute__((packed)) anil_packet_t {
  uint8_t  magic;      // 0xA1
  uint8_t  version;    // 0x01
  uint8_t  senderId;   // 1 = A, 2 = B
  uint8_t  type;       // 0 = DATA, 1 = ACK
  uint32_t seq;        // monotonic per sender
  uint32_t uptimeMs;   // sender millis() at transmit
};

static_assert(sizeof(anil_packet_t) == 12,
              "anil_packet_t must be exactly 12 bytes — both nodes must agree");

// Broadcast peer address, file scope so we pass an explicit pointer rather
// than relying on esp_now_send(NULL, ...) semantics hidden inside libespnow.a.
static u8 sBcast[6] = {0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF};

// ---------------------------------------------------------------------------
// State
// ---------------------------------------------------------------------------
static volatile uint32_t sTxCount    = 0;  // DATA frames handed to ESP-NOW
static volatile uint32_t sAckCount   = 0;  // ACKs received
static volatile uint32_t sSendFail   = 0;  // esp_now_send refused (status != 0)
static volatile uint32_t sBadPacket  = 0;  // wrong magic/version/length
static volatile uint32_t sDupAck     = 0;  // ACK for a seq we are not waiting on

// per-window accumulators, callback-written
static volatile uint32_t sWinTx      = 0;
static volatile uint32_t sWinAck     = 0;
static volatile uint32_t sWinRttSum  = 0;
static volatile uint32_t sWinRttCnt  = 0;

static uint32_t sSeq        = 0;
static uint32_t sNextSendMs = 0;
static uint32_t sPendingSeq = 0;
static uint32_t sPendingMs  = 0;
static bool     sPending    = false;

static uint32_t sWindowStartMs = 0;
static bool     sReady         = false;  // false = setup failed, stay quiet
static bool     sLedOn         = false;

// ---------------------------------------------------------------------------
// LED — GPIO2 is ACTIVE LOW on NodeMCU / D1 mini
// ---------------------------------------------------------------------------
static void ledWrite(bool on) {
  sLedOn = on;
  digitalWrite(LED_PIN, on ? LOW : HIGH);
}

static void ledToggle() {
  ledWrite(!sLedOn);
}

// ---------------------------------------------------------------------------
// ESP-NOW callbacks
// ---------------------------------------------------------------------------
static void onDataSent(u8 *mac, u8 status) {
  (void)mac;
  // Broadcast frames get no per-peer link-layer ACK, so status is the only
  // signal available here. Real delivery is proven by the ACK packet instead.
  if (status != 0) sSendFail++;
}

static void onDataRecv(u8 *mac, u8 *data, u8 len) {
  (void)mac;
  if (len != sizeof(anil_packet_t)) { sBadPacket++; return; }

  anil_packet_t pkt;
  memcpy(&pkt, data, sizeof(pkt));

  if (pkt.magic != PKT_MAGIC || pkt.version != PKT_VERSION) { sBadPacket++; return; }
  if (pkt.senderId != PEER_ID || pkt.type != TYPE_ACK) return;   // stale/other node

  // Only an ACK that actually closes the outstanding request may count toward
  // this window. Counting every ACK would let a duplicate or stale reply push
  // ACK above TX and underflow the unsigned loss calculation.
  if (sPending && pkt.seq == sPendingSeq) {
    sWinRttSum += (millis() - sPendingMs);
    sWinRttCnt++;
    sWinAck++;
    sAckCount++;
    sPending = false;
    ledToggle();
  } else {
    sDupAck++;
  }
}

// ---------------------------------------------------------------------------
// Transmit
// ---------------------------------------------------------------------------
static void sendData() {
  anil_packet_t pkt;
  pkt.magic    = PKT_MAGIC;
  pkt.version  = PKT_VERSION;
  pkt.senderId = NODE_ID;
  pkt.type     = TYPE_DATA;
  pkt.seq      = sSeq++;
  pkt.uptimeMs = millis();

  sPendingSeq = pkt.seq;
  sPendingMs  = millis();
  sPending    = true;

  // esp_now_send takes a non-const u8*, hence the cast.
  if (esp_now_send(sBcast, (u8 *)&pkt, sizeof(pkt)) == 0) {
    sTxCount++;
    sWinTx++;
  } else {
    sSendFail++;
    sPending = false;
  }
}

// ---------------------------------------------------------------------------
// Reporting
// ---------------------------------------------------------------------------
static void report() {
  // Snapshot then reset, so a callback mid-report cannot skew the numbers.
  uint32_t tx  = sWinTx;
  uint32_t ack = sWinAck;
  uint32_t rtt = sWinRttCnt ? (sWinRttSum / sWinRttCnt) : 0;

  sWinTx     = 0;
  sWinAck    = 0;
  sWinRttSum = 0;
  sWinRttCnt = 0;
  sWindowStartMs = millis();

  uint32_t loss = 0;
  if (tx > 0 && ack < tx) loss = 100UL * (tx - ack) / tx;   // guard the underflow

  Serial.printf("[A] t=%lus ch=%u TX=%lu ACK=%lu loss=%lu%% RTT=%lums bad=%lu fail=%lu dup=%lu\n",
                millis() / 1000UL,
                (unsigned)wifi_get_channel(),
                (unsigned long)tx,
                (unsigned long)ack,
                (unsigned long)loss,
                (unsigned long)rtt,
                (unsigned long)sBadPacket,
                (unsigned long)sSendFail,
                (unsigned long)sDupAck);
}

// ---------------------------------------------------------------------------
// Setup / loop
// ---------------------------------------------------------------------------
void setup() {
  pinMode(LED_PIN, OUTPUT);
  ledWrite(false);

  Serial.begin(115200);
  delay(100);
  Serial.println();
  Serial.println("ANIL node A — sender");
  Serial.printf("sizeof(anil_packet_t)=%u\n", (unsigned)sizeof(anil_packet_t));

  WiFi.persistent(false);
  WiFi.mode(WIFI_STA);
  WiFi.disconnect();          // BARE on purpose: disconnect(true) turns the
                              // radio off and ESP-NOW dies silently.
  wifi_set_channel(ESPNOW_CHANNEL);
  WiFi.setOutputPower(TX_POWER_DBM);

  Serial.printf("channel=%u (expected %u)\n",
                (unsigned)wifi_get_channel(), (unsigned)ESPNOW_CHANNEL);
  if (wifi_get_channel() != ESPNOW_CHANNEL) {
    Serial.println("FATAL: channel did not stick — ESP-NOW will not link");
    return;
  }

  if (esp_now_init() != 0) {
    Serial.println("FATAL: esp_now_init() failed");
    return;
  }

  esp_now_set_self_role(ESP_NOW_ROLE_COMBO);   // must precede add_peer
  esp_now_register_send_cb(onDataSent);
  esp_now_register_recv_cb(onDataRecv);

  int err = esp_now_add_peer(sBcast, ESP_NOW_ROLE_COMBO, ESPNOW_CHANNEL, NULL, 0);
  Serial.printf("add_peer(broadcast)=%d\n", err);
  if (err != 0) {
    Serial.println("FATAL: esp_now_add_peer failed");
    return;
  }

  Serial.printf("MAC=%s\n", WiFi.macAddress().c_str());

  sWindowStartMs = millis();
  sNextSendMs    = millis();
  sReady         = true;
}

void loop() {
  if (!sReady) { delay(1000); return; }   // setup failed: do not print lies

  uint32_t now = millis();

  // Report BEFORE sending. Both anchors are started from the same millis() in
  // setup(), so reporting first keeps the first window at exactly 60 sends;
  // sending first would put a 61st frame in window one and report a phantom
  // 1% loss on a perfectly healthy link.
  if (now - sWindowStartMs >= REPORT_WINDOW_MS) {
    report();
  }

  if ((int32_t)(now - sNextSendMs) >= 0) {
    sNextSendMs += SEND_INTERVAL_MS;
    sendData();
  }

  delay(10);   // yields to the WiFi stack, which is what lets callbacks run
}
