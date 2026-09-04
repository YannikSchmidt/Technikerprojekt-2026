#include <ModbusRTU.h>
 
// --- Modbus Konfiguration ---
ModbusRTU mb;
const int SLAVE_ID = 1;       // =gakID
const int MODBUS_START_REG = SLAVE_ID * 10;

// *** BITTE HIER DEINE ECHTEN PINS EINTRAGEN! ***
const int PIN_RS485_RX = 27;
const int PIN_RS485_TX = 14;
const int PIN_RS485_RE_DE = -1; // Auf -1 setzen, falls automatisches Modul
 
// *** SENSOR PINS ANPASSEN ***
const int PIN_SENSOR_U2 = 4;
const int PIN_SENSOR_U3 = 25; //14;
const int PIN_SENSOR_U4 = 36; //19;
const int PIN_SENSOR_U5 = 39; //23;
const int PIN_SENSOR_U6 = 34;
const int PIN_SENSOR_U7 = 35;
const int PIN_SENSOR_U8 = 32; //21;
const int PIN_SENSOR_U9 = 33; //22;

const uint8_t SENSOR_PINS[8] = {
    PIN_SENSOR_U2, PIN_SENSOR_U3, PIN_SENSOR_U4, PIN_SENSOR_U5,
    PIN_SENSOR_U6, PIN_SENSOR_U7, PIN_SENSOR_U8, PIN_SENSOR_U9
};

const char* SENSOR_NAMES[8] = {"U2", "U3", "U4", "U5", "U6", "U7", "U8", "U9"};
 
// --- Sensor Konstanten ---
const float V_REF = 3.3;          
const float ADC_MAX = 4095.0;    
const float V_OFFSET = 1.59;      // Dein Multimeter-Nullpunkt //alter Wert ohne Teiler 2.306 1.375;
const float SENSITIVITY = 0.020;  // 20 mV/A (ACS758-100B)
 
// --- Dein Struct aus "1000058771.jpg" ---
struct logData {
  uint16_t dataPaket;
  uint16_t gakID;
  int16_t value1; // Strom U6 * 100
  int16_t value2; // Strom U7 * 100
  int16_t value3;
  int16_t value4;
  int16_t value5;
  int16_t value6;
  int16_t value7;
  int16_t value8;
};
 
logData myData;
 
// Timer Variablen für periodische Messung/Ausgabe
unsigned long previousMillis = 0;
const long interval = 5000; // Intervall: 1000ms = 1 Sekunde
 
void printSensorDebug(uint16_t gakID, uint16_t dataPaket, const int rawValues[8], const float currentValues[8], int16_t sensorValues[8]) {
  Serial.printf("\n---[ GAK Nr:%u | Paket: %u ]---\n", gakID, dataPaket);
  for (int i = 0; i < 8; i++) {
    Serial.printf("%s (Pin %2d): Raw=%4d | I=%6.2f A | ModbusVal=%6d\n",
      SENSOR_NAMES[i],
      SENSOR_PINS[i],
      rawValues[i],
      currentValues[i],
      sensorValues[i]);
  }
}

void readSensors() {
  int raw_U[8];
  float current_U[8];
  int16_t modbus_U[8];

  // 1. Sensoren auslesen & berechnen
  for (int i = 0; i < 8; i++) {
      raw_U[i] = analogRead(SENSOR_PINS[i]);
      
      // (float) Fix: Verhindert die Integer-Division zu 0
      current_U[i] = ((((float)raw_U[i] / ADC_MAX) * V_REF) - V_OFFSET) / SENSITIVITY;

      // Deadband für alle Kanäle anwenden
      if (current_U[i] > -0.3f && current_U[i] < 0.3f) {
          current_U[i] = 0.0f;
      }

      // Modbus-Skalierung (x100)
      modbus_U[i] = (int16_t)(current_U[i] * 100.0f);
  }

  // 2. Struct für Modbus befüllen
  myData.value1 = modbus_U[0]; // U2
  myData.value2 = modbus_U[1]; // U3
  myData.value3 = modbus_U[2]; // U4
  myData.value4 = modbus_U[3]; // U5
  myData.value5 = modbus_U[4]; // U6
  myData.value6 = modbus_U[5]; // U7
  myData.value7 = modbus_U[6]; // U8
  myData.value8 = modbus_U[7]; // U9

  // 3. Serielle Kontroll-Ausgabe aufrufen
  printSensorDebug(myData.gakID, myData.dataPaket, raw_U, current_U, modbus_U);
}



void setup() {
  // 1. Serielle Schnittstelle für PC-Kontrolle starten
  Serial.begin(115200);
  while(!Serial); // Warten bis Serial bereit ist (nur für manche Boards nötig)
  Serial.println("\n\n--- ESP32 Modbus Sensor Knoten startet ---");
 
  // 2. Hardware Serial 2 für RS485 starten
  Serial2.begin(9600, SERIAL_8N1, PIN_RS485_RX, PIN_RS485_TX);
  Serial.println("Serial2 (RS485) gestartet (9600 Baud).");
 
  // 3. Modbus-Bibliothek starten und verknüpfen
  if (PIN_RS485_RE_DE >= 0) {
    mb.begin(&Serial2, PIN_RS485_RE_DE);
  } else {
    mb.begin(&Serial2); // Automatisches Modul
  }
 
  mb.slave(SLAVE_ID);
  Serial.print("Modbus Slave initialisiert. ID: "); Serial.println(SLAVE_ID);
 
  // 11 Modbus-Register anlegen (ab 100 bis 110)
  for (uint16_t i = 0; i < 11; i++) {
    mb.addHreg(MODBUS_START_REG + i);
  }
  Serial.printf("Modbus Holding Register %i-%i angelegt.", MODBUS_START_REG, MODBUS_START_REG+10);
 
  // Statische Grundeinstellungen setzen
  myData.gakID = SLAVE_ID;        
  // Ungenutzte Werte nullen
  myData.value3 = myData.value4 = myData.value5 = myData.value6 = myData.value7 = myData.value8 = 0;
 
  Serial.println("Setup abgeschlossen. Starte Messschleife.\n");
}
 
void loop() {
  // WICHTIG: Modbus-Anfragen müssen IMMER und SOFORT bedient werden!
  mb.task();
 
  // Periodische Aufgabe: Messen und Ausgeben
  unsigned long currentMillis = millis();
  if (currentMillis - previousMillis >= interval) {
    previousMillis = currentMillis;
    
    readSensors();
    
    mb.Hreg(MODBUS_START_REG + 0, myData.dataPaket);
    mb.Hreg(MODBUS_START_REG + 1, myData.gakID);
    mb.Hreg(MODBUS_START_REG + 2, (uint16_t)myData.value1); // Cast für Bitmuster-Erhalt
    mb.Hreg(MODBUS_START_REG + 3, (uint16_t)myData.value2);
    mb.Hreg(MODBUS_START_REG + 4, (uint16_t)myData.value3);
    mb.Hreg(MODBUS_START_REG + 5, (uint16_t)myData.value4);
    mb.Hreg(MODBUS_START_REG + 6, (uint16_t)myData.value5);
    mb.Hreg(MODBUS_START_REG + 7, (uint16_t)myData.value6);
    mb.Hreg(MODBUS_START_REG + 8, (uint16_t)myData.value7);
    mb.Hreg(MODBUS_START_REG + 9, (uint16_t)myData.value8);
    if(myData.dataPaket > 65.500) myData.dataPaket = 0;                          // 65.500 max wert von 16bit minus 36 zur sicherheit.
    myData.dataPaket += 1;
  }
}
