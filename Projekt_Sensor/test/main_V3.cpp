#include <ModbusRTU.h>
#include <algorithm> // WICHTIG: Wird für std::sort (den Ausreißer-Filter) benötigt
 
// --- Modbus Konfiguration ---
ModbusRTU mb;
const int SLAVE_ID = 1;       // =gakID
const int MODBUS_START_REG = SLAVE_ID * 10;
 
// *** BITTE HIER DEINE ECHTEN PINS EINTRAGEN! ***
const int PIN_RS485_RX = 27;
const int PIN_RS485_TX = 14;
const int PIN_RS485_RE_DE = -1;
 
// *** SENSOR PINS ANPASSEN ***
const int PIN_SENSOR_U2 = 4;
const int PIN_SENSOR_U3 = 25;
const int PIN_SENSOR_U4 = 36;
const int PIN_SENSOR_U5 = 39;
const int PIN_SENSOR_U6 = 34;
const int PIN_SENSOR_U7 = 35;
const int PIN_SENSOR_U8 = 32;
const int PIN_SENSOR_U9 = 33;
 
const uint8_t SENSOR_PINS[8] = {
    PIN_SENSOR_U2, PIN_SENSOR_U3, PIN_SENSOR_U4, PIN_SENSOR_U5,
    PIN_SENSOR_U6, PIN_SENSOR_U7, PIN_SENSOR_U8, PIN_SENSOR_U9
};
 
const char* SENSOR_NAMES[8] = {"U2", "U3", "U4", "U5", "U6", "U7", "U8", "U9"};
 
// --- Sensor Konstanten ---
const float V_REF = 3.3;          
const float ADC_MAX = 4095.0;    
const float SENSOR_SENSITIVITY = 0.020;  // 20 mV/A (ACS758-100B)
 
// *** SPANNUNGSTEILER-KORREKTUR ***
const float R1 = 10000.0; 
const float R2 = 20000.0; 
const float DIVIDER_RATIO = R2 / (R1 + R2); 
const float EFFECTIVE_SENSITIVITY = SENSOR_SENSITIVITY * DIVIDER_RATIO;
float V_OFFSET[8] = {0};
 
// --- Dein Struct ---
struct logData {
  uint16_t dataPaket;
  uint16_t gakID;
  int16_t value1; 
  int16_t value2; 
  int16_t value3;
  int16_t value4;
  int16_t value5;
  int16_t value6;
  int16_t value7;
  int16_t value8;
};
 
logData myData;
 
// --- Timer & Mittelwert Variablen ---
unsigned long previousSampleMillis = 0;
const unsigned long MEASURE_INTERVAL = 5000; // Alle 5 Sekunden wird gemessen
 
// --- Ausreißer-Filter Konstanten (NEU) ---
const int NUM_SAMPLES = 128;                              // Gesamtzahl der Messungen pro Durchlauf
const int IGNORE_SAMPLES = 32;                            // Schneidet die tiefsten und höchsten 32 Werte (je 25%) ab
const int VALID_SAMPLES = NUM_SAMPLES - (2 * IGNORE_SAMPLES); // Es bleiben 64 saubere Werte für den Mittelwert übrig
 
long sum_raw_U[8] = {0};
 
// --- Funktionen ---
void printSensorDebug(uint16_t gakID, uint16_t dataPaket, const float avgRawValues[8], const float currentValues[8], int16_t sensorValues[8]) {
  Serial.printf("\n---[ GAK Nr:%u | Paket: %u ]---\n", gakID, dataPaket);
  for (int i = 0; i < 8; i++) {
    Serial.printf("%s (Pin %2d): Ø Raw=%6.1f | Ø I=%6.2f A | ModbusVal=%6d\n",
      SENSOR_NAMES[i],
      SENSOR_PINS[i],
      avgRawValues[i],
      currentValues[i],
      sensorValues[i]);
  }
}
 
// Führt die Kalibrierung beim Start durch
void calibrateSensors() {
  Serial.println("\n=========================================");
  Serial.println("Warte auf Stabilisierung der Spannungen...");
  Serial.println("ACHTUNG: Es darf aktuell KEIN Strom fliessen!");
  Serial.println("=========================================\n");
 
  delay(5000);
 
  // Kalibrierungs-Parameter mit Ausreißer-Filter
  const int CAL_SAMPLES = 200;
  const int CAL_IGNORE = 50; // Ignoriere tiefste und höchste 50 Werte (je 25%)
  uint16_t cal_data[8][CAL_SAMPLES];
 
  // 1. Messwerte sammeln
  for (int s = 0; s < CAL_SAMPLES; s++) {
    for (int i = 0; i < 8; i++) {
      cal_data[i][s] = analogRead(SENSOR_PINS[i]);
    }
    delay(2); // minimales Delay für den ADC
  }
 
  // 2. Filtern und Auswerten
  for (int i = 0; i < 8; i++) {
    // Array für diesen Sensor sortieren
    std::sort(cal_data[i], cal_data[i] + CAL_SAMPLES);
    long cal_sum = 0;
    // Nur die mittleren Werte aufsummieren
    for (int s = CAL_IGNORE; s < CAL_SAMPLES - CAL_IGNORE; s++) {
      cal_sum += cal_data[i][s];
    }
    // Mittelwert aus den sauberen Werten berechnen
    float avg_raw = (float)cal_sum / (CAL_SAMPLES - (2 * CAL_IGNORE));
    V_OFFSET[i] = (avg_raw / ADC_MAX) * V_REF;
 
    Serial.printf("Sensor %s kalibriert: Nullpunkt bei %.3f V\n", SENSOR_NAMES[i], V_OFFSET[i]);
  }
 
  Serial.printf("\nInfo Spannungsteiler: Teilerfaktor = %.4f\n", DIVIDER_RATIO);
  Serial.printf("Info Empfindlichkeit: %.4f V/A (am ESP32 Pin)\n", EFFECTIVE_SENSITIVITY);
  Serial.println("Kalibrierung abgeschlossen!\n");
}
 
void calculateAndPublish() {
  float avg_raw_U[8];
  float current_U[8];
  int16_t modbus_U[8];
 
  for (int i = 0; i < 8; i++) {
      // HIER GEÄNDERT: Durch VALID_SAMPLES teilen, da wir die extremen Werte verworfen haben
      avg_raw_U[i] = (float)sum_raw_U[i] / VALID_SAMPLES;
 
      float voltage = (avg_raw_U[i] / ADC_MAX) * V_REF;
      current_U[i] = (voltage - V_OFFSET[i]) / EFFECTIVE_SENSITIVITY;
 
      if (current_U[i] > -1.5f && current_U[i] < -1.5f) { // (Tipp: In deiner Vorversion stand < 1.5f, evtl checken ob Absolutwert gemeint ist)
      } // Korrektur der Deadband-Logik für saubere Nullstellung:
      if (current_U[i] > -1.5f && current_U[i] < 1.5f) {
          current_U[i] = 0.0f;
      }
 
      modbus_U[i] = (int16_t)(current_U[i] * 100.0f);
  }
 
  myData.value1 = modbus_U[0];
  myData.value2 = modbus_U[1];
  myData.value3 = modbus_U[2];
  myData.value4 = modbus_U[3];
  myData.value5 = modbus_U[4];
  myData.value6 = modbus_U[5];
  myData.value7 = modbus_U[6];
  myData.value8 = modbus_U[7];
 
  mb.Hreg(MODBUS_START_REG + 0, myData.dataPaket);
  mb.Hreg(MODBUS_START_REG + 1, myData.gakID);
  mb.Hreg(MODBUS_START_REG + 2, (uint16_t)myData.value1); 
  mb.Hreg(MODBUS_START_REG + 3, (uint16_t)myData.value2);
  mb.Hreg(MODBUS_START_REG + 4, (uint16_t)myData.value3);
  mb.Hreg(MODBUS_START_REG + 5, (uint16_t)myData.value4);
  mb.Hreg(MODBUS_START_REG + 6, (uint16_t)myData.value5);
  mb.Hreg(MODBUS_START_REG + 7, (uint16_t)myData.value6);
  mb.Hreg(MODBUS_START_REG + 8, (uint16_t)myData.value7);
  mb.Hreg(MODBUS_START_REG + 9, (uint16_t)myData.value8);
 
  if(myData.dataPaket > 65500) myData.dataPaket = 0;                                 
  myData.dataPaket += 1;
 
  printSensorDebug(myData.gakID, myData.dataPaket, avg_raw_U, current_U, modbus_U);
}
 
void setup() {
  Serial.begin(115200);
  while(!Serial); 
  Serial.println("\n\n--- ESP32 Modbus Sensor Knoten startet ---");
 
  calibrateSensors();
 
  Serial2.begin(9600, SERIAL_8N1, PIN_RS485_RX, PIN_RS485_TX);
  Serial.println("Serial2 (RS485) gestartet (9600 Baud).");
 
  if (PIN_RS485_RE_DE >= 0) {
    mb.begin(&Serial2, PIN_RS485_RE_DE);
  } else {
    mb.begin(&Serial2); 
  }
 
  mb.slave(SLAVE_ID);
  Serial.print("Modbus Slave initialisiert. ID: "); Serial.println(SLAVE_ID);
 
  for (uint16_t i = 0; i < 11; i++) {
    mb.addHreg(MODBUS_START_REG + i);
  }
  Serial.printf("Modbus Holding Register %i-%i angelegt.\n", MODBUS_START_REG, MODBUS_START_REG+10);
 
  myData.gakID = SLAVE_ID;        
  myData.dataPaket = 0;
 
  Serial.println("Setup abgeschlossen. Starte Intervall-Punktmessung (alle 5 Sekunden).\n");
}
 
void loop() {
  mb.task(); // Modbus-Anfragen beantworten
 
  unsigned long currentMillis = millis();
 
  // Alle 5 Sekunden auslösen
  if (currentMillis - previousSampleMillis >= MEASURE_INTERVAL) {
    previousSampleMillis += MEASURE_INTERVAL;
 
    // HIER NEU: 2D-Array zum Speichern der 128 Messwerte für 8 Sensoren
    uint16_t raw_data[8][NUM_SAMPLES];
 
    // 1. PUNKTMESSUNG: 128 Messungen extrem schnell hintereinander sammeln
    for (int s = 0; s < NUM_SAMPLES; s++) {
      for (int i = 0; i < 8; i++) {
        raw_data[i][s] = analogRead(SENSOR_PINS[i]);
      }
    }
 
    // 2. AUSREISSER FILTERN (Trimmed Mean)
    for (int i = 0; i < 8; i++) {
      // Sortiert das Array für den aktuellen Sensor vom kleinsten zum größten Wert
      std::sort(raw_data[i], raw_data[i] + NUM_SAMPLES);
 
      sum_raw_U[i] = 0;
      // Wir überspringen die ersten 32 Werte (zu niedrig) und die letzten 32 (zu hoch)
      for (int s = IGNORE_SAMPLES; s < NUM_SAMPLES - IGNORE_SAMPLES; s++) {
        sum_raw_U[i] += raw_data[i][s];
      }
    }
 
    // 3. Sofort danach auswerten und ins Modbus-Register schreiben
    calculateAndPublish();
  }
}