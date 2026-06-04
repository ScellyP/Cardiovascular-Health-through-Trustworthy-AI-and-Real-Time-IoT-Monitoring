// ================================================================
//  ECG MONITOR — Arduino Uno
//  Hardware: AD8232 ECG module, HC-05 Bluetooth, ESP8266 (Serial)
//
//  Wiring summary:
//    AD8232  → Arduino
//      OUTPUT → A0
//      LO+    → D10
//      LO−    → D11
//      3.3V   → 3.3V
//      GND    → GND
//
//    HC-05   → Arduino
//      TXD    → D4  (Arduino RX)
//      RXD    → D5  (Arduino TX)
//      VCC    → 5V
//      GND    → GND
//
//    ESP8266 → Arduino  (voltage divider on Arduino D2 → ESP RX!)
//      D2/GPIO4 (ESP RX) → Arduino D2  via 1kΩ + 2kΩ divider
//      D1/GPIO5 (ESP TX) → Arduino D3  direct
//      3.3V              → 3.3V
//      GND               → GND
//
//  NOTE: Arduino D2 → ESP RX needs a voltage divider:
//        Arduino D2 → 1kΩ → ESP D2(GPIO4)
//                                |
//                               2kΩ
//                                |
//                               GND
// ================================================================

#include <SoftwareSerial.h>

// ── Pin definitions ──────────────────────────────────────────────
#define ECG_PIN          A0
#define LO_PLUS_PIN      10
#define LO_MINUS_PIN     11

#define BT_RX_PIN        4     // HC-05 TXD  → Arduino D4
#define BT_TX_PIN        5     // HC-05 RXD  ← Arduino D5

#define ESP_RX_PIN       3     // ESP8266 TX → Arduino D3
#define ESP_TX_PIN       2     // Arduino D2 → ESP8266 RX (via divider!)

// ── Timing ───────────────────────────────────────────────────────
#define SAMPLE_INTERVAL_MS    20      // 50 Hz ECG sample rate
#define BT_REPORT_INTERVAL_MS 30000   // 30-second BT report
#define STATS_WINDOW_MS       5000    // Rolling stats window

// ── Serial ports ─────────────────────────────────────────────────
SoftwareSerial Bluetooth(BT_RX_PIN, BT_TX_PIN);
SoftwareSerial ESP_Serial(ESP_RX_PIN, ESP_TX_PIN);

// ── State ─────────────────────────────────────────────────────────
uint32_t tsLastSample  = 0;
uint32_t tsLastBTRpt   = 0;
uint32_t tsWindowStart = 0;

int   ecgMin         = 1023;
int   ecgMax         = 0;
long  ecgSum         = 0;
int   ecgCount       = 0;

// ── Helpers ──────────────────────────────────────────────────────
bool leadsOff() {
    return (digitalRead(LO_PLUS_PIN) == HIGH ||
            digitalRead(LO_MINUS_PIN) == HIGH);
}

void resetStats() {
    ecgMin  = 1023; ecgMax = 0;
    ecgSum  = 0;    ecgCount = 0;
    tsWindowStart = millis();
}

void btDivider() {
    Bluetooth.println(F("=========================="));
}

const char* rhythmHint(int amplitude, bool lo) {
    if (lo)              return "NO_SIGNAL";
    if (amplitude > 600) return "STRONG";
    if (amplitude > 300) return "NORMAL";
    if (amplitude > 100) return "WEAK";
    return "FLAT";
}

// ── Send one ECG line to ESP (switches listener back after) ──────
void sendToESP(int ecgVal, bool lo) {
    ESP_Serial.listen();                  // give bus to ESP
    ESP_Serial.print(F("ECG:"));
    ESP_Serial.print(ecgVal);
    ESP_Serial.print(F(",LO:"));
    ESP_Serial.println(lo ? 1 : 0);
    Bluetooth.listen();                   // hand bus back to BT
}

// ── Setup ────────────────────────────────────────────────────────
void setup() {
    Serial.begin(9600);
    Bluetooth.begin(9600);
    ESP_Serial.begin(9600);

    pinMode(LO_PLUS_PIN,  INPUT);
    pinMode(LO_MINUS_PIN, INPUT);

    resetStats();

    // Bluetooth starts listening by default (last begin() wins)
    Bluetooth.listen();

    Bluetooth.println(F("--- ECG Monitor Ready ---"));
    Bluetooth.println(F("Leads-Off: ENABLED | BT reports: every 30s"));
    Serial.println(F("ECG_START"));
}

// ── Loop ─────────────────────────────────────────────────────────
void loop() {
    uint32_t now = millis();

    // ── 1. Sample ECG at 50 Hz ───────────────────────────────────
    if (now - tsLastSample >= SAMPLE_INTERVAL_MS) {
        tsLastSample = now;

        bool lo    = leadsOff();
        int  ecgVal = lo ? 0 : analogRead(ECG_PIN);

        // Update rolling stats
        if (!lo && ecgCount < 32000) {
            ecgSum += ecgVal;
            ecgCount++;
            if (ecgVal < ecgMin) ecgMin = ecgVal;
            if (ecgVal > ecgMax) ecgMax = ecgVal;
        }

        // Reset stats window every STATS_WINDOW_MS
        if (now - tsWindowStart >= STATS_WINDOW_MS) resetStats();

        // Send to ESP via SoftwareSerial (switches listen internally)
        sendToESP(ecgVal, lo);

        // USB debug
        Serial.println(ecgVal);
    }

    // ── 2. Bluetooth report every 30 s ───────────────────────────
    if (now - tsLastBTRpt >= BT_REPORT_INTERVAL_MS) {
        tsLastBTRpt = now;

        bool lo        = leadsOff();
        int  ecgNow    = lo ? 0 : analogRead(ECG_PIN);
        int  ecgAvg    = (ecgCount > 0) ? (int)(ecgSum / ecgCount) : 0;
        int  amplitude = ecgMax - ecgMin;

        // Bluetooth.listen() already active — safe to send directly
        btDivider();
        Bluetooth.println(F("   30-SECOND ECG READOUT"));
        btDivider();

        if (lo) {
            Bluetooth.println(F("  !! LEADS OFF — CHECK PADS !!"));
        } else {
            Bluetooth.print(F("Instant ECG Value : ")); Bluetooth.println(ecgNow);
            Bluetooth.print(F("Average (last 5s)  : ")); Bluetooth.println(ecgAvg);
            Bluetooth.print(F("Min Value          : ")); Bluetooth.println(ecgMin);
            Bluetooth.print(F("Max Value          : ")); Bluetooth.println(ecgMax);
            Bluetooth.print(F("Peak-to-Peak       : ")); Bluetooth.println(amplitude);

            Bluetooth.print(F("Signal Quality     : "));
            if      (amplitude > 400) Bluetooth.println(F("GOOD"));
            else if (amplitude > 150) Bluetooth.println(F("FAIR"));
            else                      Bluetooth.println(F("POOR — recheck pads"));

            Bluetooth.print(F("Rhythm Hint        : "));
            Bluetooth.println(rhythmHint(amplitude, lo));
        }

        btDivider();
        Bluetooth.println();
    }
}
