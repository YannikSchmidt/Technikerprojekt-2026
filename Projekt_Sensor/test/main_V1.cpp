#include <Arduino.h>

// --- Pin-Definitionen laut Schaltplan ---
// Analoge Pins für die ACS758 Stromsensoren
const int PIN_SENSOR_U6 = 34; // Sensor U6
const int PIN_SENSOR_U7 = 35; // Sensor U7
const int PIN_SENSOR_U8 = 32; // Sensor U8
const int PIN_SENSOR_U9 = 33; // Sensor U9

// Pins für das RS485 Modbus Modul (Serial2)
const int PIN_RS485_RX = 16; // RX2
const int PIN_RS485_TX = 17; // TX2

// --- Sensor Spezifikationen (ACS758xCB-100B) ---
// Bei 5V Versorgungsspannung:
const float SENSOR_OFFSET_V = 2.5;     // Nullpunkt liegt bei 2.5V (0 Ampere)
const float SENSOR_SENSITIVITY = 0.02; // Empfindlichkeit ist 20mV (0.02V) pro Ampere

void setup() {
  // 1. Standard-Serielle Schnittstelle für Ausgaben am PC starten
  Serial.begin(115200);
  Serial.println("System startet...");

  // 2. Hardware Serial 2 für das RS485-Modul starten
  // 9600 Baud ist ein Standardwert für Modbus, passe ihn ggf. an deinen Zähler/Wechselrichter an
  Serial2.begin(9600, SERIAL_8N1, PIN_RS485_RX, PIN_RS485_TX);
  
  // Analoge Pins explizit als Eingang setzen (optional, aber sauber)
  pinMode(PIN_SENSOR_U6, INPUT);
  pinMode(PIN_SENSOR_U7, INPUT);
  pinMode(PIN_SENSOR_U8, INPUT);
  pinMode(PIN_SENSOR_U9, INPUT);

  // Auflösung des ESP32 ADCs auf 12-Bit setzen (Werte von 0 bis 4095)
  analogReadResolution(12);
}

// Hilfsfunktion zum Auslesen und Berechnen des Stroms
float readCurrent(int pin) {
  // Analogwert einlesen (0 bis 4095)
  int adcValue = analogRead(pin);
  
  // ADC-Wert in Spannung (Volt) umrechnen
  // Achtung: Der ESP32 kann standardmäßig nur bis ca. 3.3V messen!
  float voltage = (adcValue / 4095.0) * 3.3; 
  
  // Spannung in Strom (Ampere) umrechnen
  float current = (voltage - SENSOR_OFFSET_V) / SENSOR_SENSITIVITY;
  
  return current;
}

void loop() {
  // Sensoren auslesen
  float stromU6 = readCurrent(PIN_SENSOR_U6);
  float stromU7 = readCurrent(PIN_SENSOR_U7);
  float stromU8 = readCurrent(PIN_SENSOR_U8);
  float stromU9 = readCurrent(PIN_SENSOR_U9);

  // Werte im Serial Monitor ausgeben
  Serial.print("U6 (Pin 34): "); Serial.print(stromU6, 2); Serial.print(" A  |  ");
  Serial.print("U7 (Pin 35): "); Serial.print(stromU7, 2); Serial.print(" A  |  ");
  Serial.print("U8 (Pin 32): "); Serial.print(stromU8, 2); Serial.print(" A  |  ");
  Serial.print("U9 (Pin 33): "); Serial.print(stromU9, 2); Serial.println(" A");

  // Hier kommt später die Modbus-Logik hin. 
  // Um z.B. Daten über RS485 zu senden:
  // Serial2.println("Daten an Modbus senden...");

  delay(1000); // 1 Sekunde warten bis zur nächsten Messung
}