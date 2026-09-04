///////////////////////////////////////////////////////////////////////////
// Technikerprojekt 2026 - Sensorknoten (Modbus-Slave)
// Misst den Strom in acht Strängen und stellt die Werte als Modbus-Register
// bereit. Der Logger holt sie dort ab. Hardware: ESP32 DevKit v1.
///////////////////////////////////////////////////////////////////////////

#include <ModbusRTU.h>
#include <algorithm>   // für std::sort

const uint8_t KANAELE = 8;

// --- Modbus Konfiguration ---
// Registerblock: 10 = Paketnummer, 11 = gakID, 12..19 = Stränge 1..8 in mA.
// Startadresse = SLAVE_ID * 10, damit ein zweiter GAK ab Register 20 liegt.
const uint16_t SLAVE_ID          = 1;               // = gakID
const uint16_t MODBUS_START_REG  = SLAVE_ID * 10;
const uint16_t MODBUS_REG_ANZAHL = 2 + KANAELE;
const uint32_t MODBUS_BAUDRATE   = 9600;
const uint16_t REG_PAKET = 0;   // Offsets im Registerblock
const uint16_t REG_GAK   = 1;
const uint16_t REG_WERTE = 2;

// --- RS485 Pins ---
const int PIN_RS485_RX = 27;
const int PIN_RS485_TX = 14;
const int PIN_RS485_RE_DE = -1;   // -1 = Transceiver schaltet die Richtung selbst um

// --- Sensor Pins ---
// GPIO 4 und 25 liegen auf ADC2. Der funktioniert nur ohne WLAN, hier unkritisch
// weil der Knoten ausschließlich über RS485 kommuniziert. Rest ist ADC1.
const uint8_t SENSOR_PINS[KANAELE]  = { 4, 25, 36, 39, 34, 35, 32, 33 };
const char*   SENSOR_NAMEN[KANAELE] = { "U1", "U2", "U3", "U4", "U5", "U6", "U7", "U8" };


///////////////////////////////////////////////////////////////////////////
// Sensorplatine auswählen
// Für beide Sensortypen existiert eine eigene Platine, sie sind tauschbar.
// Nach einem Wechsel muss hier umgestellt UND neu kalibriert werden.
///////////////////////////////////////////////////////////////////////////
#define SENSOR_CYHCS_LSP25   1
#define SENSOR_ACS758_100B   2

#define SENSOR_TYP  SENSOR_CYHCS_LSP25    // <<< hier die verbaute Platine eintragen

#if SENSOR_TYP == SENSOR_CYHCS_LSP25
  const float SENSOR_SENSITIVITY = 0.080;   // 80 mV/A (CYHCS-LSP25)
  // Nullpunkte am Aufbau bei stromlosen Strängen gemessen
  const float V_OFFSET[KANAELE] = {1.561, 1.553, 1.581, 1.557, 1.573, 1.546, 1.563, 1.563};
#elif SENSOR_TYP == SENSOR_ACS758_100B
  const float SENSOR_SENSITIVITY = 0.020;   // 20 mV/A (ACS758-100B)
  // ACHTUNG: theoretischer Wert 2.5 V * 0.667, vor dem Einsatz nachmessen
  const float V_OFFSET[KANAELE] = {1.667, 1.667, 1.667, 1.667, 1.667, 1.667, 1.667, 1.667};
#else
  #error "SENSOR_TYP muss SENSOR_CYHCS_LSP25 oder SENSOR_ACS758_100B sein"
#endif

// --- Sensor Konstanten ---
const float V_REF   = 3.3;
const float ADC_MAX = 4095.0;     // 12 Bit Auflösung

// --- Spannungsteiler-Korrektur ---
// Nötig weil der Sensor mit 5 V arbeitet, die ADC-Eingänge aber nur 3,3 V vertragen.
const float R1 = 10000.0;
const float R2 = 20000.0;
const float DIVIDER_RATIO = R2 / (R1 + R2);   // 0.666 (2/3)
const float EFFECTIVE_SENSITIVITY = SENSOR_SENSITIVITY * DIVIDER_RATIO;   // am ESP32-Pin

// Unterdrückt Rauschen um den Nullpunkt. Echte Ströme darunter gehen dabei verloren.
const float TOTBAND_AMPERE = 0.5;

// --- Ausreißer-Filter (getrimmter Mittelwert) ---
// 128 Rohwerte sortieren, oben und unten je 32 verwerfen, aus dem Rest mitteln.
// Ein einzelner Störimpuls landet beim Sortieren am Rand und fällt so heraus.
const int NUM_SAMPLES     = 128;
const int IGNORE_SAMPLES  = 32;
const int VALID_SAMPLES   = NUM_SAMPLES - (2 * IGNORE_SAMPLES);

// --- Timer & Mittelwert ---
// 10 Punktmessungen à 3 s ergeben alle 30 s einen neuen Modbus-Wert.
const unsigned long MEASURE_INTERVAL   = 3000;
const int           SAMPLES_PER_AVERAGE = 10;

ModbusRTU mb;
unsigned long previousSampleMillis = 0;
int   currentSampleCount = 0;
float currentSum[KANAELE] = {0};    // aufaddierte Ampere-Werte
uint16_t paketNr = 0;


///////////////////////////////////////////////////////////////////////////
// Punktmessung: alle Kanäle einlesen, filtern, in Ampere umrechnen
///////////////////////////////////////////////////////////////////////////
void punktmessung() {
  uint16_t raw_data[KANAELE][NUM_SAMPLES];    // 2 kB, liegt auf dem Stack

  // Kanäle verschachtelt einlesen, damit alle Stränge nahezu gleichzeitig erfasst werden
  for (int s = 0; s < NUM_SAMPLES; s++) {
    for (int i = 0; i < KANAELE; i++) {
      raw_data[i][s] = analogRead(SENSOR_PINS[i]);
    }
  }

  for (int i = 0; i < KANAELE; i++) {
    std::sort(raw_data[i], raw_data[i] + NUM_SAMPLES);

    long sum_raw = 0;
    for (int s = IGNORE_SAMPLES; s < NUM_SAMPLES - IGNORE_SAMPLES; s++) {
      sum_raw += raw_data[i][s];
    }

    float avg_raw = (float)sum_raw / VALID_SAMPLES;
    float voltage = (avg_raw / ADC_MAX) * V_REF;
    float amps    = (voltage - V_OFFSET[i]) / EFFECTIVE_SENSITIVITY;

    if (amps > -TOTBAND_AMPERE && amps < TOTBAND_AMPERE) amps = 0.0f;

    currentSum[i] += amps;
  }
}


///////////////////////////////////////////////////////////////////////////
// Mittelwert bilden und in die Modbus-Register schreiben
///////////////////////////////////////////////////////////////////////////
void calculateAndPublishAverage() {
  Serial.printf("\n---[ GAK Nr:%u | Paket: %u ] MITTELWERT ---\n", SLAVE_ID, paketNr);

  mb.Hreg(MODBUS_START_REG + REG_PAKET, paketNr);
  mb.Hreg(MODBUS_START_REG + REG_GAK,   SLAVE_ID);

  for (int i = 0; i < KANAELE; i++) {
    float avg_amps = currentSum[i] / SAMPLES_PER_AVERAGE;
    int16_t milliampere = (int16_t)(avg_amps * 1000.0f);   // mA, weil Modbus nur ganze Zahlen kennt

    // Cast nach uint16_t nur fürs Bitmuster, der Logger wandelt es zurück nach int16_t
    mb.Hreg(MODBUS_START_REG + REG_WERTE + i, (uint16_t)milliampere);

    Serial.printf("Strang %d (%s): %.2f A (Modbus: %d mA)\n", i + 1, SENSOR_NAMEN[i], avg_amps, milliampere);
  }

  paketNr++;   // uint16_t springt nach 65535 von selbst auf 0
}


///////////////////////////////////////////////////////////////////////////
// Setup & Loop
///////////////////////////////////////////////////////////////////////////
void setup() {
  Serial.begin(115200);
  delay(500);
  Serial.println("\n\n--- ESP32 Modbus Sensor Knoten startet ---");

#if SENSOR_TYP == SENSOR_CYHCS_LSP25
  Serial.println("Sensorplatine: CYHCS-LSP25 (80 mV/A)");
#else
  Serial.println("Sensorplatine: ACS758-100B (20 mV/A)");
#endif
  Serial.printf("Teilerfaktor %.4f, Empfindlichkeit am ESP32-Pin %.4f V/A\n", DIVIDER_RATIO, EFFECTIVE_SENSITIVITY);
  Serial.printf("Neuer Mittelwert alle %lu s\n", (MEASURE_INTERVAL * SAMPLES_PER_AVERAGE) / 1000UL);

  Serial2.begin(MODBUS_BAUDRATE, SERIAL_8N1, PIN_RS485_RX, PIN_RS485_TX);   // 8N1 muss beim Logger identisch sein

  if (PIN_RS485_RE_DE >= 0) {
    mb.begin(&Serial2, PIN_RS485_RE_DE);
  } else {
    mb.begin(&Serial2);
  }
  mb.slave(SLAVE_ID);

  // addHreg legt die Register an, ohne das antwortet der Slave mit "Illegal Data Address"
  for (uint16_t i = 0; i < MODBUS_REG_ANZAHL; i++) mb.addHreg(MODBUS_START_REG + i, 0);
  mb.Hreg(MODBUS_START_REG + REG_GAK, SLAVE_ID);

  Serial.printf("Modbus Slave ID %u, Register %u..%u\n", SLAVE_ID, MODBUS_START_REG, MODBUS_START_REG + MODBUS_REG_ANZAHL - 1);
  Serial.println("Setup abgeschlossen. Starte Punktmessung...\n");
}

void loop() {
  mb.task();   // muss so oft wie möglich laufen, nur hier werden Anfragen des Loggers beantwortet

  unsigned long currentMillis = millis();
  if (currentMillis - previousSampleMillis < MEASURE_INTERVAL) return;
  previousSampleMillis = currentMillis;

  punktmessung();
  currentSampleCount++;
  Serial.printf("Punktmessung %d / %d ausgefuehrt.\n", currentSampleCount, SAMPLES_PER_AVERAGE);

  if (currentSampleCount >= SAMPLES_PER_AVERAGE) {
    calculateAndPublishAverage();
    currentSampleCount = 0;
    for (int i = 0; i < KANAELE; i++) currentSum[i] = 0;
  }
}
