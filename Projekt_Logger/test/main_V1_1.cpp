#include <SPI.h>
#include <SD.h>
#include <ModbusMaster.h>

// Pinbelegung für WT32-ETH01
#define SD_CS   4
#define SD_SCK  14
#define SD_MOSI 32
#define SD_MISO 33

#define RXD2 5
#define TXD2 17

uint32_t g_Time = 0;

// Modbus-Instanz initialisieren
ModbusMaster node;

// FILE_APPEND fügt neue Zeilen ans Ende der Datei an
File logFile = SD.open("/test_log.txt", FILE_APPEND);

struct logData
{
  uint32_t realTimeStamp;
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

SPIClass sdSPI(VSPI);

void sdWrite(File file, struct logData data){
  Serial.println("\n--> 1. Schreibe Testdaten auf die SD-Karte...");
  if (file) {
    // Beispielhafte Log-Daten schreiben
    file.printf("(nochnicht)RTC:%lu;", data.realTimeStamp);
    file.printf("GAK:%lu;", data.gakID);
    file.printf("1:%lu;", data.value1);
    file.printf("2:%lu;", data.value2);
    file.printf("3:%lu;", data.value3);
    file.printf("4:%lu;", data.value4);
    file.printf("5:%lu;", data.value5);
    file.printf("6:%lu;", data.value6);
    file.printf("7:%lu;", data.value7);
    file.printf("8:%lu;", data.value8);

    // Wichtig: Datei schließen, damit die Puffer auf die Karte geschrieben werden!
    file.close();
    Serial.println("[OK] Daten wurden erfolgreich in '/test_log.txt' geschrieben.");
  } else {
    Serial.println("[FEHLER] '/test_log.txt' konnte nicht zum Schreiben geöffnet werden!");
  }
}

void sdRead(File file){
  // ----------------------------------------------------
  // 2. DATEN LESEN & IM SERIAL MONITOR AUSGEBEN
  // ----------------------------------------------------
  Serial.println("\n--> 2. Lese Daten von der SD-Karte ab...");

  file = SD.open("/test_log.txt", FILE_READ);

  if (file) {
    Serial.println("--- INHALT VON /test_log.txt ---");
    
    // Solange noch Daten in der Datei vorhanden sind, Zeile für Zeile auslesen
    while (file.available()) {
      Serial.write(file.read());
    }

    file.close();
    Serial.println("--- ENDE DER DATEI ---");
  } else {
    Serial.println("[FEHLER] '/test_log.txt' konnte nicht zum Lesen geöffnet werden!");
  }
}

void readModBus(int adress, struct logData &data){
  uint8_t result;
  
  // Beispiel: Lesen von 2 Holding Registern ab Adresse 0x0000
  // (Passe die Adresse 0x0000 und die Anzahl an dein Slave-Gerät an!)
  Serial.println("Lese Holding Register...");
  result = node.readHoldingRegisters(adress, 9);

  // Überprüfen, ob das Lesen erfolgreich war
  if (result == node.ku8MBSuccess) {
    // Daten erfolgreich empfangen
    data.gakID = node.getResponseBuffer(0);
    data.value1 = node.getResponseBuffer(1);
    data.value2 = node.getResponseBuffer(2);
    data.value3 = node.getResponseBuffer(3);
    data.value4 = node.getResponseBuffer(4);
    data.value5 = node.getResponseBuffer(5);
    data.value6 = node.getResponseBuffer(6);
    data.value7 = node.getResponseBuffer(7);
    data.value8 = node.getResponseBuffer(8);

  } else {
    // Fehler bei der Kommunikation (z.B. Timeout 0xE2)
    Serial.print("Fehler beim Lesen! Fehlercode: 0x");
    Serial.println(result, HEX);
  }

  Serial.println("-----------------------------");
  delay(2000); // 2 Sekunden warten bis zur nächsten Abfrage
}

void getTime(){
  g_Time = millis();
}

void setup() {
  Serial.begin(115200);
  delay(1000);

  // Custom SPI auf unseren Pins starten
  sdSPI.begin(SD_SCK, SD_MISO, SD_MOSI, SD_CS);

  // Initialisierung mit 4 MHz für hohe Stabilität
  if (!SD.begin(SD_CS, sdSPI, 4000000)) {
    Serial.println("[FEHLER] SD-Karte konnte nicht initialisiert werden!");
    return;
  }
  Serial.println("[OK] SD-Karte erfolgreich initialisiert.");

  // HardwareSerial 2 für Modbus starten (Baudrate standardmäßig oft 9600)
  // Konfiguration SERIAL_8N1 ist Standard für Modbus-RTU
  Serial2.begin(9600, SERIAL_8N1, RXD2, TXD2);
  // Modbus-Master initialisieren: Kommunikation mit Slave-ID 1
  node.begin(1, Serial2);
  // HINWEIS: Keine pre-/postTransmission Callbacks mehr nötig!
  Serial.println("ESP32 Modbus-Master (Auto-Flow) gestartet.");


}

void loop() {
  logData curData;
  getTime;
  readModBus(101, curData);
  sdWrite(logFile, curData);
}

