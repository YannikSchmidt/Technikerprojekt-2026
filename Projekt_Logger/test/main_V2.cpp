#include <SPI.h>
#include <SD.h>
#include <ModbusMaster.h>

// Pinbelegung für WT32-ETH01
#define SD_CS   4
#define SD_SCK  14
#define SD_MOSI 32
#define SD_MISO 33

#define RXD2 17
#define TXD2 5

uint32_t g_Time = 0;
uint16_t paketNr =0;

// Modbus-Instanz initialisieren
ModbusMaster node;

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

// Öffnet die Datei direkt beim Schreiben im APPEND-Modus
void sdWrite(const char* filepath, const struct logData &data) {
  Serial.println("\n--> Schreibe Daten auf die SD-Karte...");
  
  File file = SD.open(filepath, FILE_APPEND);
  
  if (file) {
    file.printf("(Paket)RTC:%u;", paketNr);
    file.printf("GAK:%u;", data.gakID);             // %u für uint16_t
    file.printf("1:%d;", data.value1);              // %d für int16_t
    file.printf("2:%d;", data.value2);
    file.printf("3:%d;", data.value3);
    file.printf("4:%d;", data.value4);
    file.printf("5:%d;", data.value5);
    file.printf("6:%d;", data.value6);
    file.printf("7:%d;", data.value7);
    file.printf("8:%d\n", data.value8);             // \n für neue Zeile

    file.close(); // Puffer leeren & Datei sichern
    Serial.printf("(Paket)RTC:%u;", paketNr);
    Serial.printf("GAK:%u;", data.gakID);             // %u für uint16_t
    Serial.printf("1:%d;", data.value1);              // %d für int16_t
    Serial.printf("2:%d;", data.value2);
    Serial.printf("3:%d;", data.value3);
    Serial.printf("4:%d;", data.value4);
    Serial.printf("5:%d;", data.value5);
    Serial.printf("6:%d;", data.value6);
    Serial.printf("7:%d;", data.value7);
    Serial.printf("8:%d\n", data.value8);             // \n für neue Zeile
    Serial.println("[OK] Daten erfolgreich geschrieben.");
  } else {
    Serial.println("[FEHLER] Dateizugriff fehlgeschlagen!");
  }
}

void sdRead(const char* filepath) {
  Serial.println("\n--> Lese Daten von der SD-Karte...");
  File file = SD.open(filepath, FILE_READ);

  if (file) {
    Serial.println("--- INHALT VON /test_log.txt ---");
    while (file.available()) {
      Serial.write(file.read());
    }
    file.close();
    Serial.println("--- ENDE DER DATEI ---");
  } else {
    Serial.println("[FEHLER] Datei konnte nicht gelesen werden!");
  }
}

bool readModBus(int address, struct logData &data) {
  Serial.println("Lese Holding Register...");
  uint8_t result = node.readHoldingRegisters(address, 9);

  if (result == node.ku8MBSuccess) {
    paketNr = node.getResponseBuffer(0);
    data.gakID  = node.getResponseBuffer(1);
    data.value1 = node.getResponseBuffer(2);
    data.value2 = node.getResponseBuffer(3);
    data.value3 = node.getResponseBuffer(4);
    data.value4 = node.getResponseBuffer(5);
    data.value5 = node.getResponseBuffer(6);
    data.value6 = node.getResponseBuffer(7);
    data.value7 = node.getResponseBuffer(8);
    data.value8 = node.getResponseBuffer(9);
  } else {
    Serial.print("Fehler beim Lesen! Fehlercode: 0x");
    Serial.println(result, HEX);
    return 0;
  }

  Serial.println("-----------------------------");
  return 1;
}

void getTime() {
  g_Time = millis();
}

void setup() {
  Serial.begin(115200);
  delay(1000);

  // Custom SPI starten
  sdSPI.begin(SD_SCK, SD_MISO, SD_MOSI, SD_CS);

  if (!SD.begin(SD_CS, sdSPI, 4000000)) {
    Serial.println("[FEHLER] SD-Karte konnte nicht initialisiert werden!");
    return;
  }
  Serial.println("[OK] SD-Karte erfolgreich initialisiert.");

  // Modbus Serial2
  Serial2.begin(9600, SERIAL_8N1, RXD2, TXD2);
  node.begin(1, Serial2);
  Serial.println("ESP32 Modbus-Master gestartet.");
}

void loop() {
  logData curData = {0};

  getTime();                        // Korrekte Funktions-Syntax ()
  curData.realTimeStamp = g_Time;   // Timestamp zuweisen

  if(readModBus(10, curData)){
    sdWrite("/test_log.txt", curData);
  }

  delay(5000); // Pause im Haupt-Loop für Stabilität
}