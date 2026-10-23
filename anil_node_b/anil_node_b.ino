/*
 * ANIL — Node B (receiver / responder)
 * Board: ESP8266 (NodeMCU v2 / Wemos D1 mini)
 *
 * v1 scope: prove the ESP-NOW link only. No sensors, no router, no encryption.
 *
 * Node B listens for DATA packets from Node A and replies with an ACK that
 * echoes the same seq, which lets A measure RTT and loss. The onboard LED
 * toggles on every accepted packet.
 *
 * Companion sketch: anil_node_a/anil_node_a.ino
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
#define NODE_ID           2        // 1 = A, 2 = B
#define PEER_ID           1
#define ESPNOW_CHANNEL    1
#define LED_PIN           2        // onboard LED = GPIO2, ACTIVE LOW
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
static volatile uint32_t sRxCount   = 0;   // valid DATA packets accepted
static volatile uint32_t sBadPacket = 0;   // wrong magic/version/length
static volatile uint32_t sAckSent   = 0;   // ACK frames handed to ESP-NOW
static volatile uint32_t sSendFail  = 0;   // esp_now_send refused
static volatile uint32_t sGaps      = 0;   // missing seq numbers seen
static volatile uint32_t sWinRx     = 0;   // DATA accepted in this window
static volatile uint32_t sLastSeq   = 0;
static volatile bool     sReplyNow  = false;
static volatile uint32_t sReplySeq  = 0;

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
  if (status != 0) sSendFail++;
}

static void onDataRecv(u8 *mac, u8 *data, u8 len) {
  (void)mac;
  if (len != sizeof(anil_packet_t)) { sBadPacket++; return; }

  anil_packet_t pkt;
  memcpy(&pkt, data, sizeof(pkt));

  if (pkt.magic != PKT_MAGIC || pkt.version != PKT_VERSION) { sBadPacket++; return; }
  if (pkt.senderId != PEER_ID || pkt.type != TYPE_DATA) return;   // ignore ACKs/other nodes

  // seq gap detection (first packet establishes the baseline)
  if (sRxCount > 0 && pkt.seq > sLastSeq + 1) {
    sGaps += (pkt.seq - sLastSeq - 1);
  }
  sLastSeq = pkt.seq;
  sRxCount++;
  sWinRx++;
  ledToggle();

  // Hand the reply to loop(): the core's own ESP8266WiFiMesh library asserts
  // that calling transmit from inside a receive callback can corrupt program
  // state, so sending is deferred rather than done here.
  sReplySeq = pkt.seq;
  sReplyNow = true;
}

// ---------------------------------------------------------------------------
// Transmit
// ---------------------------------------------------------------------------
static void sendAck(uint32_t seq) {
  anil_packet_t pkt;
  pkt.magic    = PKT_MAGIC;
  pkt.version  = PKT_VERSION;
  pkt.senderId = NODE_ID;
  pkt.type     = TYPE_ACK;
  pkt.seq      = seq;         // echo the DATA seq so A can match the RTT
  pkt.uptimeMs = millis();

  // esp_now_send takes a non-const u8*, hence the cast.
  if (esp_now_send(sBcast, (u8 *)&pkt, sizeof(pkt)) == 0) {
    sAckSent++;
  } else {
    sSendFail++;
  }
}

// ---------------------------------------------------------------------------
// Reporting
// ---------------------------------------------------------------------------
static void report() {
  uint32_t rx = sWinRx;
  sWinRx = 0;
  sWindowStartMs = millis();

  Serial.printf("[B] t=%lus ch=%u RX=%lu tot=%lu ack=%lu lastSeq=%lu gaps=%lu bad=%lu fail=%lu\n",
                millis() / 1000UL,
                (unsigned)wifi_get_channel(),
                (unsigned long)rx,
                (unsigned long)sRxCount,
                (unsigned long)sAckSent,
                (unsigned long)sLastSeq,
                (unsigned long)sGaps,
                (unsigned long)sBadPacket,
                (unsigned long)sSendFail);
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
  Serial.println("ANIL node B — receiver");
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
  sReady         = true;
}

void loop() {
  if (!sReady) { delay(1000); return; }   // setup failed: do not print lies

  if (sReplyNow) {
    sReplyNow = false;
    sendAck(sReplySeq);
  }

  if (millis() - sWindowStartMs >= REPORT_WINDOW_MS) {
    report();
  }

  // The deferred ACK is emitted at the top of the next iteration, so this
  // delay is the responder's ACK latency floor: node A's measured RTT includes
  // up to one loop period of it. Shrinking this tightens RTT but raises CPU use.
  delay(10);
}
