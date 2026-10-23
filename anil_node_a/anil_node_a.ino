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
#define WIFI_TX_POWER     WIFI_OUTPUT_POWER // full power, v1 is a range test

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

// ---------------------------------------------------------------------------
// State
// ---------------------------------------------------------------------------
static volatile uint32_t sTxCount   = 0;   // DATA frames handed to ESP-NOW
static volatile uint32_t sAckCount  = 0;   // ACKs received
static volatile uint32_t sSendFail  = 0;   // esp_now_send refused (status != 0)
static volatile uint32_t sBadPacket = 0;   // wrong magic/version/length
static volatile uint32_t sRttSumMs  = 0;
static volatile uint32_t sRttCount  = 0;

static uint32_t sSeq        = 0;
static uint32_t sNextSendMs = 0;
static uint32_t sPendingSeq = 0;
static uint32_t sPendingMs  = 0;
static bool     sPending    = false;

static uint32_t sWindowStartMs = 0;
static uint32_t sWindowTx      = 0;
static uint32_t sWindowAck     = 0;
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
  // Broadcast frames have no per-peer link-layer ACK, so status is the only
  // signal we get here. Real delivery is proven by the ACK packet instead.
  if (status != 0) sSendFail++;
}

static void onDataRecv(u8 *mac, u8 *data, u8 len) {
  (void)mac;
  if (len != sizeof(anil_packet_t)) { sBadPacket++; return; }

  anil_packet_t pkt;
  memcpy(&pkt, data, sizeof(pkt));

  if (pkt.magic != PKT_MAGIC || pkt.version != PKT_VERSION) { sBadPacket++; return; }
  if (pkt.senderId != PEER_ID || pkt.type != TYPE_ACK) return;   // stale/other node

  sAckCount++;
  sWindowAck++;
  ledToggle();

  if (sPending && pkt.seq == sPendingSeq) {
    sRttSumMs += (millis() - sPendingMs);
    sRttCount++;
    sPending = false;
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
  // NULL = every registered peer (we registered the broadcast address).
  if (esp_now_send(NULL, (u8 *)&pkt, sizeof(pkt)) == 0) {
    sTxCount++;
    sWindowTx++;
  } else {
    sSendFail++;
    sPending = false;
  }
}

// ---------------------------------------------------------------------------
// Reporting
// ---------------------------------------------------------------------------
static void report() {
  uint32_t rtt = sRttCount ? (sRttSumMs / sRttCount) : 0;
  uint32_t loss = sWindowTx ? (100UL * (sWindowTx - sWindowAck) / sWindowTx) : 0;

  Serial.printf("[A] t=%lus ch=%u TX=%lu ACK=%lu loss=%lu%% RTT=%lums bad=%lu fail=%lu\n",
                millis() / 1000UL,
                (unsigned)wifi_get_channel(),
                (unsigned long)sTxCount,
                (unsigned long)sAckCount,
                (unsigned long)loss,
                (unsigned long)rtt,
                (unsigned long)sBadPacket,
                (unsigned long)sSendFail);

  sWindowStartMs = millis();
  sWindowTx      = 0;
  sWindowAck     = 0;
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
  WiFi.setOutputPower(WIFI_TX_POWER);

  Serial.printf("channel=%u (expected %u)\n",
                (unsigned)wifi_get_channel(), (unsigned)ESPNOW_CHANNEL);
  if (wifi_get_channel() != ESPNOW_CHANNEL) {
    Serial.println("FATAL: channel did not stick — ESP-NOW will not link");
  }

  if (esp_now_init() != 0) {
    Serial.println("FATAL: esp_now_init() failed");
    return;
  }

  esp_now_set_self_role(ESP_NOW_ROLE_COMBO);   // must precede add_peer
  esp_now_register_send_cb(onDataSent);
  esp_now_register_recv_cb(onDataRecv);

  u8 bcast[6] = {0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF};
  int err = esp_now_add_peer(bcast, ESP_NOW_ROLE_COMBO, ESPNOW_CHANNEL, NULL, 0);
  Serial.printf("add_peer(broadcast)=%d\n", err);
  if (err != 0) Serial.println("FATAL: esp_now_add_peer failed");

  Serial.printf("MAC=%s\n", WiFi.macAddress().c_str());

  sWindowStartMs = millis();
  sNextSendMs    = millis();
}

void loop() {
  uint32_t now = millis();

  if ((int32_t)(now - sNextSendMs) >= 0) {
    sNextSendMs += SEND_INTERVAL_MS;
    sendData();
  }

  if (now - sWindowStartMs >= REPORT_WINDOW_MS) {
    report();
  }

  delay(10);   // keeps the WiFi stack fed
}
