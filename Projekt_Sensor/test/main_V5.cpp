#include <ModbusRTU.h>
#include <algorithm> 
// --- Modbus Konfiguration ---
ModbusRTU mb;
const int SLAVE_ID = 1;       // =gakID
const int MODBUS_START_REG = SLAVE_ID * 10;
// *** BITTE HIER DEINE ECHTEN PINS EINTRAGEN! ***
const int PIN_RS485_RX = 27;
const int PIN_RS485_TX = 14;
const int PIN_RS485_RE_DE = -1;
// *** SENSOR PINS ANPASSEN ***
const int PIN_SENSOR_U1 = 4;
const int PIN_SENSOR_U2 = 25;
const int PIN_SENSOR_U3 = 36;
const int PIN_SENSOR_U4 = 39;
const int PIN_SENSOR_U5 = 34;
const int PIN_SENSOR_U6 = 35;
const int PIN_SENSOR_U7 = 32;
const int PIN_SENSOR_U8 = 33;
const uint8_t SENSOR_PINS[8] = {
    PIN_SENSOR_U1, PIN_SENSOR_U2, PIN_SENSOR_U3, PIN_SENSOR_U4,
    PIN_SENSOR_U5, PIN_SENSOR_U6, PIN_SENSOR_U7, PIN_SENSOR_U8
};
const char* SENSOR_NAMES[8] = {"U1", "U2", "U3", "U4", "U5", "U6", "U7", "U8"};
// --- Sensor Konstanten ---
const float V_REF = 3.3;          
const float ADC_MAX = 4095.0;    
// HIER KORRIGIERT: 20 mV/A für den ACS758-100B
const float SENSOR_SENSITIVITY = 0.020;  
// *** SPANNUNGSTEILER-KORREKTUR ***
const float R1 = 10000.0; 
const float R2 = 20000.0; 
const float DIVIDER_RATIO = R2 / (R1 + R2); // 0.666 (2/3)
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
const unsigned long MEASURE_INTERVAL = 3000; // Punktmessung alle 5 Sekunden
 
// NEU: 5 Minuten = 300 Sekunden = 60 Messungen à 5 Sekunden
const int SAMPLES_PER_AVERAGE = 10;
int currentSampleCount = 0;
float currentSum[8] = {0}; // Speichert die aufaddierten Ampere-Werte
// --- Ausreißer-Filter Konstanten (für die Punktmessung) ---
const int NUM_SAMPLES = 128;                              
const int IGNORE_SAMPLES = 32;                            
const int VALID_SAMPLES = NUM_SAMPLES - (2 * IGNORE_SAMPLES); 
void calibrateSensors() {
  Serial.println("\n=========================================");
  Serial.println("Warte auf Stabilisierung der Spannungen...");
  Serial.println("ACHTUNG: Es darf aktuell KEIN Strom fliessen!");
  Serial.println("=========================================\n");
  delay(5000);
  const int CAL_SAMPLES = 200;
  const int CAL_IGNORE = 50; 
  uint16_t cal_data[8][CAL_SAMPLES];
  for (int s = 0; s < CAL_SAMPLES; s++) {
    for (int i = 0; i < 8; i++) {
      cal_data[i][s] = analogRead(SENSOR_PINS[i]);
    }
    delay(2); 
  }
  for (int i = 0; i < 8; i++) {
    std::sort(cal_data[i], cal_data[i] + CAL_SAMPLES);
    long cal_sum = 0;
    for (int s = CAL_IGNORE; s < CAL_SAMPLES - CAL_IGNORE; s++) {
      cal_sum += cal_data[i][s];
    }
    float avg_raw = (float)cal_sum / (CAL_SAMPLES - (2 * CAL_IGNORE));
    V_OFFSET[i] = (avg_raw / ADC_MAX) * V_REF;
    Serial.printf("Sensor %s kalibriert: Nullpunkt bei %.3f V\n", SENSOR_NAMES[i], V_OFFSET[i]);
  }
  Serial.printf("\nInfo Spannungsteiler: Teilerfaktor = %.4f\n", DIVIDER_RATIO);
  Serial.printf("Info Empfindlichkeit: %.4f V/A (am ESP32 Pin)\n", EFFECTIVE_SENSITIVITY);
  Serial.println("Kalibrierung abgeschlossen!\n");
}
 
// NEU: Diese Funktion wird nur alle 5 Minuten aufgerufen
void calculateAndPublish5MinAverage() {
  int16_t modbus_U[8];
  Serial.printf("\n---[ GAK Nr:%u | Paket: %u ] 5-MINUTEN-MITTELWERT ---\n", myData.gakID, myData.dataPaket);
  for (int i = 0; i < 8; i++) {
      // Mittelwert der letzten 5 Minuten (60 Messungen) berechnen
      float final_avg_amps = currentSum[i] / SAMPLES_PER_AVERAGE;
      // Umrechnung in Milliampere (mA) für den Logger
      modbus_U[i] = (int16_t)(final_avg_amps * 1000.0f);
      Serial.printf("Strang %d: Ø %.2f A (Modbus: %d mA)\n", i+1, final_avg_amps, modbus_U[i]);
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
  // Register beim Start einmalig mit 0 befüllen
  for (uint16_t i = 2; i < 10; i++) mb.Hreg(MODBUS_START_REG + i, 0);
 
  myData.gakID = SLAVE_ID;        
  myData.dataPaket = 0;
  Serial.println("Setup abgeschlossen. Starte 5s-Punktmessung (Modbus-Update alle 5 Minuten).\n");
}
void loop() {
  mb.task(); // Hält die Modbus-Verbindung zum Logger jederzeit aufrecht
  unsigned long currentMillis = millis();
  if (currentMillis - previousSampleMillis >= MEASURE_INTERVAL) {
    previousSampleMillis += MEASURE_INTERVAL;
    uint16_t raw_data[8][NUM_SAMPLES];
    // 1. Schnelle Punktmessung ausführen
    for (int s = 0; s < NUM_SAMPLES; s++) {
      for (int i = 0; i < 8; i++) {
        raw_data[i][s] = analogRead(SENSOR_PINS[i]);
      }
    }
    // 2. Punktmessung auswerten
    for (int i = 0; i < 8; i++) {
      std::sort(raw_data[i], raw_data[i] + NUM_SAMPLES);
      long sum_raw = 0;
      for (int s = IGNORE_SAMPLES; s < NUM_SAMPLES - IGNORE_SAMPLES; s++) {
        sum_raw += raw_data[i][s];
      }
      float avg_raw = (float)sum_raw / VALID_SAMPLES;
      float voltage = (avg_raw / ADC_MAX) * V_REF;
      float amps = (voltage - V_OFFSET[i]) / EFFECTIVE_SENSITIVITY;
      // Deadband: Ignoriert reines Rauschen unter 0.5 Ampere
      if (amps > -0.5f && amps < 0.5f) {
          amps = 0.0f;
      }
      // Wert zum 5-Minuten-Speicher hinzufügen
      currentSum[i] += amps;
    }
    currentSampleCount++;
    Serial.printf("Punktmessung %d / %d ausgefuehrt.\n", currentSampleCount, SAMPLES_PER_AVERAGE);
 
    // 3. Wenn 5 Minuten um sind (60 Messungen) -> Ab zu Modbus!
    if (currentSampleCount >= SAMPLES_PER_AVERAGE) {
        calculateAndPublish5MinAverage();
        // Zähler und Speicher für die nächsten 5 Minuten zurücksetzen
        currentSampleCount = 0;
        for (int i = 0; i < 8; i++) {
            currentSum[i] = 0;
        }
    }
  }
}