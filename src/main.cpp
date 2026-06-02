/*
 * SPECTER, Native MeshCore Flood Repeater
 * Hardware: STM32F103C8T6 + SX1262 (DX-LR30 module)
 * Version: 1.1.0
 *
 * Pin mapping (confirmed from BoardInfo + sx1262_driver.h):
 *   SPI1 : SCK=PA5  MISO=PA6  MOSI=PA7
 *   SX1262: NSS=PA4  BUSY=PA2  NRST=PA3
 *   RF switch: TXEN=PA0  RXEN=PA1
 *   LED  : PB11 (active HIGH)
 *   UART1: TX=PA9  RX=PA10  → CH340 USB (requires ENABLE_HWSERIAL1 flag)
 *
 * MeshCore radio settings (EU868 UK channel):
 *   Freq=869.618 MHz  SF=8  BW=62.5 kHz  CR=4/8  SyncWord=0x12
 */

#include <Arduino.h>
#include <SPI.h>
#include <RadioLib.h>
#include <SHA256.h>
#include <Ed25519.h>
#include <IWatchdog.h>

// ─── Version ─────────────────────────────────────────────────────────────────
#define FW_VERSION "1.1.0"

// ─── Pin definitions ─────────────────────────────────────────────────────────
#define PIN_NSS   PA4
#define PIN_DIO1  RADIOLIB_NC   // Not exposed on DX-LR30; use polling
#define PIN_NRST  PA3
#define PIN_BUSY  PA2
#define PIN_TXEN  PA0
#define PIN_RXEN  PA1
#define PIN_LED   PB11
#define LED_ON    HIGH   // DX-LR30: LED anode on PB11 (active HIGH)
#define LED_OFF   LOW

// ─── Radio settings ──────────────────────────────────────────────────────────
#define LORA_FREQ   869.618f
#define LORA_SF     8
#define LORA_BW     62.5f
#define LORA_CR     8            // 4/8  (EU/UK Narrow preset standard)
#define LORA_PREAMBLE 8
#define LORA_SYNC   0x12         // LoRa private network

// ─── MeshCore protocol constants ─────────────────────────────────────────────
#define ROUTE_TRANSPORT_FLOOD  0x00
#define ROUTE_FLOOD            0x01
#define ROUTE_DIRECT           0x02
#define ROUTE_TRANSPORT_DIRECT 0x03
#define PAYLOAD_ADVERT         0x04
#define MAX_HOPS               64
#define MAX_PKT                256

// ─── Timing ──────────────────────────────────────────────────────────────────
#define LOCAL_ADVERT_INTERVAL_MS (2UL * 60UL * 1000UL)   // 2 minutes (neighbor discovery)
#define FLOOD_ADVERT_INTERVAL_MS (12UL * 3600UL * 1000UL) // 12 hours (global presence)
#define FIRST_ADVERT_MS          5000UL                   // 5 s after boot
#define RX_TIMEOUT_MS            30000                    // poll cycle

// ─── Node identity ────────────────────────────────────────────────────────────
// Default: auto-generated name "SPECTER-XXXX" + key derived from STM32 UID.
//
// For a fixed identity (e.g. to control the path-hash byte), set in platformio.ini:
//   -D NODE_NAME_STR=\"SPECTER-1811\"
//   -D FIXED_PRIVKEY_HEX=\"<64 hex chars>\"   ← 32-byte Ed25519 seed
//
// The path hash is always pubkey[0], so pick a seed whose public key starts
// with the desired byte. Use tools/gen_identity.py to search for one.
#ifdef NODE_NAME_STR
static const char node_name[] = NODE_NAME_STR;
#else
static char node_name[14];   // "SPECTER-" + 4 hex digits + null
#endif
static uint8_t own_privkey[32];
static uint8_t own_pubkey[32];
static uint8_t own_hash;     // 1-byte path hash = own_pubkey[0]

// ── Compile-time hex → bytes helper (constexpr-friendly for a macro) ─────────
static void hex2bin(const char* hex, uint8_t* out, int len) {
    for (int i = 0; i < len; i++) {
        uint8_t hi = hex[2*i];
        uint8_t lo = hex[2*i+1];
        hi = (hi >= 'a') ? hi-'a'+10 : (hi >= 'A') ? hi-'A'+10 : hi-'0';
        lo = (lo >= 'a') ? lo-'a'+10 : (lo >= 'A') ? lo-'A'+10 : lo-'0';
        out[i] = (hi << 4) | lo;
    }
}

static void identity_init(void) {
#ifdef FIXED_PRIVKEY_HEX
    // ── Fixed identity: use pre-generated keypair with known path hash ───────
    hex2bin(FIXED_PRIVKEY_HEX, own_privkey, 32);
    Ed25519::derivePublicKey(own_pubkey, own_privkey);
#else
    // ── Auto identity: derive deterministic key from STM32 UID ───────────────
    const uint32_t uid[3] = {
        *(volatile uint32_t*)0x1FFFF7E8U,
        *(volatile uint32_t*)0x1FFFF7ECU,
        *(volatile uint32_t*)0x1FFFF7F0U,
    };
#ifndef NODE_NAME_STR
    snprintf(node_name, sizeof(node_name), "SPECTER-%04X",
             (unsigned)(uid[2] & 0xFFFF));
#endif
    SHA256 sha;
    sha.reset();
    const uint8_t domain[] = "SPECTER-MESHCORE-KEY-V1";
    sha.update(domain, sizeof(domain) - 1);
    sha.update((const uint8_t*)uid, 12);
    sha.finalize(own_privkey, 32);
    Ed25519::derivePublicKey(own_pubkey, own_privkey);
#endif
    own_hash = own_pubkey[0];
}

// ─── Dedup cache (circular, 128 slots × 8-byte SHA256 fingerprint) ───────────
// Fingerprint = first 8 bytes of SHA256(ptype ‖ payload), matching the
// reference repeater.py key: packet_fingerprint(ptype_byte + payload).
#define DEDUP_SIZE 128
#define DEDUP_FP   8
static uint8_t  dedup_buf[DEDUP_SIZE][DEDUP_FP];
static uint8_t  dedup_head = 0;

static bool dedup_seen(const uint8_t* payload, uint8_t ptype, uint8_t plen) {
    // Compute SHA256(ptype || payload), keep first DEDUP_FP bytes
    uint8_t fp[DEDUP_FP];
    {
        SHA256 sha;
        sha.reset();
        sha.update(&ptype, 1);
        sha.update(payload, plen);
        uint8_t full[32];
        sha.finalize(full, 32);
        memcpy(fp, full, DEDUP_FP);
    }
    for (int i = 0; i < DEDUP_SIZE; i++) {
        if (memcmp(dedup_buf[i], fp, DEDUP_FP) == 0) return true;
    }
    memcpy(dedup_buf[dedup_head], fp, DEDUP_FP);
    dedup_head = (dedup_head + 1) % DEDUP_SIZE;
    return false;
}

// ─── RF switch helpers ───────────────────────────────────────────────────────
static void rf_rx(void)   { digitalWrite(PIN_TXEN, LOW);  digitalWrite(PIN_RXEN, HIGH); }
static void rf_tx(void)   { digitalWrite(PIN_TXEN, HIGH); digitalWrite(PIN_RXEN, LOW);  }
static void rf_idle(void) { digitalWrite(PIN_TXEN, LOW);  digitalWrite(PIN_RXEN, LOW);  }

// LED patterns (active LOW on PC13)
//   1 blink        = packet received
//   2 fast blinks  = relay transmitted (connectivity confirmed)
//   solid ON       = ADVERT transmitting
static void led_blink(uint8_t n, uint16_t ms) {
    for (uint8_t i = 0; i < n; i++) {
        digitalWrite(PIN_LED, LED_ON);
        delay(ms);
        digitalWrite(PIN_LED, LED_OFF);
        if (i < n - 1) delay(ms);
    }
}

// ─── Compile-time Unix timestamp ────────────────────────────────────────────
// Converts __DATE__ "Mmm DD YYYY" + __TIME__ "HH:MM:SS" → Unix epoch seconds.
// Used as timestamp base so ADVERTs carry valid Unix time (companions may
// reject packets where |now - ts| > 3600 s as replay-protection).
static uint32_t build_unix_ts(void) {
    const char* d = __DATE__;   // "May 31 2026"
    const char* t = __TIME__;   // "14:30:00"
    // Month
    static const char* mn = "JanFebMarAprMayJunJulAugSepOctNovDec";
    int month = 1;
    for (int m = 0; m < 12; m++) {
        if (d[0]==mn[m*3] && d[1]==mn[m*3+1] && d[2]==mn[m*3+2]) { month=m+1; break; }
    }
    int day  = (d[4]==' ' ? 0 : d[4]-'0')*10 + (d[5]-'0');
    int year = (d[7]-'0')*1000+(d[8]-'0')*100+(d[9]-'0')*10+(d[10]-'0');
    int hour = (t[0]-'0')*10+(t[1]-'0');
    int min  = (t[3]-'0')*10+(t[4]-'0');
    int sec  = (t[6]-'0')*10+(t[7]-'0');
    // Days from 1970 to Jan 1 of build year
    int y = year - 1970;
    uint32_t days = (uint32_t)y * 365UL + (uint32_t)((y + 1) / 4);
    // Days in each month up to build month
    const int md[] = {31,28,31,30,31,30,31,31,30,31,30,31};
    for (int m = 1; m < month; m++) {
        days += md[m-1];
        if (m == 2 && (year % 4 == 0)) days++;
    }
    days += (uint32_t)(day - 1);
    return days * 86400UL + (uint32_t)hour*3600UL + (uint32_t)min*60UL + (uint32_t)sec;
}
static uint32_t ts_base = 0;   // set in setup(); millis() added at runtime

// ─── RadioLib object ─────────────────────────────────────────────────────────
SX1262 radio = new Module(PIN_NSS, PIN_DIO1, PIN_NRST, PIN_BUSY, SPI);

static bool radio_init(void) {
    SPI.begin();
    int state = radio.begin(LORA_FREQ, LORA_BW, LORA_SF, LORA_CR,
                            LORA_SYNC, 14 /*dBm*/, LORA_PREAMBLE);
    if (state != RADIOLIB_ERR_NONE) {
        Serial1.print("FAILED: "); Serial1.println(state);
        return false;
    }
    radio.setRfSwitchPins(PIN_RXEN, PIN_TXEN);
    radio.setCRC(2);   // CRC-16 on
    return true;
}

// ─── Build ADVERT packet (MeshCore v1 protocol with Ed25519 signature) ───────
// ⚠️  CRITICAL: MeshCore companions validate Ed25519 signatures on ADVERTs.
//     Unsigned or incorrectly signed packets are silently discarded.
//     This was the root cause of "0 repeaters" during early development.
//     See docs/development-notes.md#ed25519-signature-requirement for details.
static int build_advert(uint8_t* buf) {
    int i = 0;
    // Header: ROUTE_FLOOD | (PAYLOAD_ADVERT << 2) = 0x11
    buf[i++] = (uint8_t)(ROUTE_FLOOD | (PAYLOAD_ADVERT << 2));
    // Path length: 0 hops, 1-byte hashes
    buf[i++] = 0x00;

    // --- Payload ---
    // pubkey (32 bytes)
    memcpy(buf + i, own_pubkey, 32); i += 32;

    // timestamp (4 bytes LE): compile-time Unix base + uptime seconds
    uint32_t ts = ts_base + millis() / 1000UL;
    memcpy(buf + i, &ts, 4); i += 4;

    // signature placeholder (filled in after appdata is known)
    int sig_offset = i;
    memset(buf + i, 0, 64); i += 64;

    // appdata: flags [+ lat/lon] [+ name]
    int appdata_offset = i;
#if defined(NODE_LAT_I) && defined(NODE_LON_I)
    buf[i++] = 0x92;  // FLAG_IS_REPEATER(0x02) | FLAG_HAS_LOCATION(0x10) | FLAG_HAS_NAME(0x80)
    int32_t _lat = (int32_t)(NODE_LAT_I);
    int32_t _lon = (int32_t)(NODE_LON_I);
    memcpy(buf + i, &_lat, 4); i += 4;
    memcpy(buf + i, &_lon, 4); i += 4;
#else
    buf[i++] = 0x82;  // FLAG_IS_REPEATER(0x02) | FLAG_HAS_NAME(0x80)
#endif
    uint8_t nlen = (uint8_t)strlen(node_name);
    memcpy(buf + i, node_name, nlen); i += nlen;
    int appdata_len = i - appdata_offset;

    // Sign: message = pubkey(32) + timestamp(4) + appdata
    uint8_t msg[32 + 4 + 80];  // appdata: 1 flags + 8 GPS + 15 name = 24 bytes max
    int mlen = 0;
    memcpy(msg + mlen, own_pubkey, 32);           mlen += 32;
    memcpy(msg + mlen, buf + 2 + 32, 4);          mlen += 4;   // timestamp from buf
    memcpy(msg + mlen, buf + appdata_offset, appdata_len); mlen += appdata_len;
    Ed25519::sign(buf + sig_offset, own_privkey, own_pubkey, msg, mlen);

    return i;
}

// ─── Send a packet (with RF switch) ─────────────────────────────────────────
static bool send_pkt(const uint8_t* buf, int len) {
    rf_idle();
    // Random CSMA backoff 300–800 ms
    delay(300 + (millis() % 500));
    rf_tx();
    int state = radio.transmit(const_cast<uint8_t*>(buf), len);
    rf_idle();
    return (state == RADIOLIB_ERR_NONE);
}

// ─── Process received flood packet ───────────────────────────────────────────
// Returns true if a relay packet should be sent (writes it to relay_buf/relay_len)
static uint8_t relay_buf[MAX_PKT];
static int     relay_len = 0;

static bool process_pkt(const uint8_t* data, int len) {
    if (len < 3) { Serial1.println("Drop: malformed"); return false; }

    uint8_t header = data[0];
    uint8_t route  = header & 0x03;

    int idx = 1;

    // Path header byte: [hop_count(6) | hash_size_code(2)]
    uint8_t path_hdr  = data[idx++];
    uint8_t hop_count = path_hdr & 0x3F;
    uint8_t hs_code   = (path_hdr >> 6) & 0x03;
    uint8_t hash_size = hs_code + 1;   // 1–4 bytes per hop

    int path_bytes = hop_count * hash_size;
    if (idx + path_bytes > len) { Serial1.println("Drop: malformed"); return false; }

    const uint8_t* path_ptr    = data + idx;
    const uint8_t* payload_ptr = data + idx + path_bytes;
    int            payload_len = len - idx - path_bytes;

    // ── DIRECT routing ────────────────────────────────────────────────────────
    // Re-transmit packet UNCHANGED if our hash appears anywhere in the path.
    // (MeshCore reference: repeater.py _handle_direct)
    if (route == ROUTE_DIRECT || route == ROUTE_TRANSPORT_DIRECT) {
        for (int j = 0; j < hop_count; j++) {
            if (path_ptr[j * hash_size] == own_hash) {
                if (len > MAX_PKT) return false;
                relay_len = len;
                memcpy(relay_buf, data, len);
                Serial1.println("Relay direct");
                return true;
            }
        }
        return false;   // Our hash not in path — not our route
    }

    // ── FLOOD routing (ROUTE_FLOOD 0x01 + ROUTE_TRANSPORT_FLOOD 0x00) ────────

    // Guard: max hops
    if (hop_count >= MAX_HOPS) { Serial1.println("Drop: max hops"); return false; }

    // Guard: payload sanity
    if (payload_len < 1) { Serial1.println("Drop: malformed"); return false; }

    uint8_t ptype = (header >> 2) & 0x0F;

    // Dedup check
    if (dedup_seen(payload_ptr, ptype, (uint8_t)payload_len)) {
        Serial1.println("Drop: dedup"); return false;
    }

    // Guard: our hash already in path (loop prevention)
    for (int j = 0; j < hop_count; j++) {
        if (path_ptr[j * hash_size] == own_hash) {
            Serial1.println("Drop: our hash in path"); return false;
        }
    }

    // Build relay packet
    relay_len = 0;
    relay_buf[relay_len++] = header;

    // New path header: hop_count+1 with same hash_size_code
    uint8_t new_hop = hop_count + 1;
    relay_buf[relay_len++] = (uint8_t)((new_hop & 0x3F) | (hs_code << 6));

    // Old path hashes
    if (relay_len + path_bytes + hash_size + payload_len >= MAX_PKT) {
        Serial1.println("Drop: relay too large"); return false;
    }
    memcpy(relay_buf + relay_len, path_ptr, path_bytes);
    relay_len += path_bytes;

    // Append own hash (1..4 bytes depending on hash_size_code)
    relay_buf[relay_len++] = own_hash;
    if (hash_size >= 2) relay_buf[relay_len++] = own_pubkey[1];
    if (hash_size >= 3) relay_buf[relay_len++] = own_pubkey[2];
    if (hash_size >= 4) relay_buf[relay_len++] = own_pubkey[3];

    // Payload
    memcpy(relay_buf + relay_len, payload_ptr, payload_len);
    relay_len += payload_len;

    Serial1.print("Relay hop="); Serial1.println(new_hop);
    return true;
}

// ─── ARDUINO ENTRY POINTS ───────────────────────────────────────────────────

void setup(void) {
    // LED
    pinMode(PIN_LED,  OUTPUT);
    digitalWrite(PIN_LED, LED_ON);   // ON during init

    // RF switch pins
    pinMode(PIN_TXEN, OUTPUT); digitalWrite(PIN_TXEN, LOW);
    pinMode(PIN_RXEN, OUTPUT); digitalWrite(PIN_RXEN, LOW);

    // Serial1 = USART1 = PA9(TX)/PA10(RX) = CH340 USB
    Serial1.begin(115200);
    delay(200);
    IWatchdog.begin(8000000);  // 8 s IWDG — auto-resets if radio HALT or loop freeze

    ts_base = build_unix_ts();
    Serial1.println();
    identity_init();
    Serial1.print("=== SPECTER MeshCore Repeater v");
    Serial1.print(FW_VERSION);
    Serial1.print(" [");
    Serial1.print(node_name);
    Serial1.println("] ===");
    Serial1.print("Hash: 0x"); Serial1.println(own_hash, HEX);

    Serial1.print("Radio init...");
    if (!radio_init()) {
        Serial1.println(" FAILED, halting");
        while (true) {
            digitalWrite(PIN_LED, LED_ON);  delay(100);
            digitalWrite(PIN_LED, LED_OFF); delay(100);
        }
    }
    Serial1.println(" OK");

    digitalWrite(PIN_LED, LED_OFF);   // OFF, ready
}

static uint32_t last_local_advert = 0;
static uint32_t last_flood_advert = 0;
static bool     first_advert      = false;
static bool     rx_started        = false;
static uint32_t last_beat_ms      = 0;

// ─── Packet statistics (printed in ALIVE every 10 s) ─────────────────────────
static uint32_t stat_rx    = 0;   // valid packets received from radio
static uint32_t stat_relay = 0;   // packets successfully relayed
static uint32_t stat_drop  = 0;   // packets rejected (dedup/hops/malformed)

void loop(void) {
    IWatchdog.reload();
    uint32_t now = millis();

    // ── Heartbeat every 10 s ─────────────────────────────────────────────────
    if (now - last_beat_ms >= 10000UL) {
        last_beat_ms = now;
        Serial1.print("ALIVE uptime=");  Serial1.print(now / 1000);
        Serial1.print("s rx=");          Serial1.print(stat_rx);
        Serial1.print(" relay=");        Serial1.print(stat_relay);
        Serial1.print(" drop=");         Serial1.println(stat_drop);
        led_blink(1, 80);   // heartbeat blink every 10 s
    }

    // ── ADVERT timers (local 2min + flood 12h, MeshCore standard) ───────────
    bool do_advert = false;
    const char* advert_type = "";

    // First ADVERT at boot (flood)
    if (!first_advert && now >= FIRST_ADVERT_MS) {
        do_advert    = true;
        first_advert = true;
        last_flood_advert = now;
        last_local_advert = now;
        advert_type = "initial";
    }
    // Flood ADVERT every 12 hours (global presence)
    else if (first_advert && (now - last_flood_advert) >= FLOOD_ADVERT_INTERVAL_MS) {
        do_advert = true;
        last_flood_advert = now;
        last_local_advert = now;  // sync both timers
        advert_type = "flood";
    }
    // Local ADVERT every 2 minutes (neighbor discovery)
    else if (first_advert && (now - last_local_advert) >= LOCAL_ADVERT_INTERVAL_MS) {
        do_advert = true;
        last_local_advert = now;
        advert_type = "local";
    }

    if (do_advert) {
        rx_started = false;  // restart RX after TX
        rf_idle();
        uint8_t abuf[MAX_PKT];
        int     alen = build_advert(abuf);
        Serial1.print("ADVERT "); Serial1.println(advert_type);
        digitalWrite(PIN_LED, LED_ON);
        send_pkt(abuf, alen);
        digitalWrite(PIN_LED, LED_OFF);
    }

    // ── Non-blocking receive (startReceive + IRQ polling) ────────────────────
    // ⚠️  DIO1 is NOT connected on DX-LR30, so we CANNOT use radio.receive()
    //     (it blocks forever). Instead, use startReceive() + poll getIrqFlags().
    //     See docs/development-notes.md#radiolib-polling-mode-dio1-not-connected
    if (!rx_started) {
        rf_rx();
        int err = radio.startReceive();
        if (err != RADIOLIB_ERR_NONE) {
            Serial1.print("startReceive err: "); Serial1.println(err);
        }
        rx_started = true;
    }

    // Poll IRQ flags, bit 1 = RxDone, bit 5 = Timeout (SX1262 IRQ mask)
    uint16_t irq = radio.getIrqFlags();
    if (irq & 0x0002) {                        // RxDone
        uint8_t rxbuf[MAX_PKT];
        int state = radio.readData(rxbuf, 0);
        rx_started = false;

        if (state == RADIOLIB_ERR_NONE) {
            int   rxlen = (int)radio.getPacketLength();
            float rssi  = radio.getRSSI();
            int   snr   = (int)radio.getSNR();
            Serial1.print("RX RSSI="); Serial1.print((int)rssi);
            Serial1.print(" SNR=");    Serial1.print(snr);
            Serial1.print(" len=");    Serial1.println(rxlen);
            if (rxlen > 0 && rxlen < MAX_PKT) {
                stat_rx++;
                led_blink(1, 40);         // 1 blink = pkt received
                rf_idle();
                if (process_pkt(rxbuf, rxlen)) {
                    stat_relay++;
                    led_blink(2, 30);     // 2 fast blinks = relay OK
                    send_pkt(relay_buf, relay_len);
                } else {
                    stat_drop++;
                }
            }
        } else {
            Serial1.print("readData err: "); Serial1.println(state);
        }
    } else if (irq & 0x0200) {                 // RxTimeout
        rx_started = false;
    }
}
