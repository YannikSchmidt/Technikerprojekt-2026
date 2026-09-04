#include <SPI.h>
#include <SD.h>
#include <ModbusMaster.h>

// Sichere Pinbelegung für WT32-ETH01 (keine Boot-Pins!)
#define SD_CS   4
#define SD_SCK  14
#define SD_MOSI 32
#define SD_MISO 33

#define RXD2 5
#define TXD2 17

// Modbus-Instanz initialisieren
ModbusMaster node;

SPIClass sdSPI(VSPI);

void setup() {
  Serial.begin(115200);
  delay(1000);

  Serial.println("\n==========================================");
  Serial.println("  WT32-ETH01: SD-Karten Schreib-/Lese-Test");
  Serial.println("==========================================");

  // Custom SPI auf unseren Pins starten
  sdSPI.begin(SD_SCK, SD_MISO, SD_MOSI, SD_CS);

  // Initialisierung mit 4 MHz für hohe Stabilität
  if (!SD.begin(SD_CS, sdSPI, 4000000)) {
    Serial.println("[FEHLER] SD-Karte konnte nicht initialisiert werden!");
    Serial.println("Bitte Verkabelung (besonders MISO an IO33) prüfen.");
    return;
  }
  Serial.println("[OK] SD-Karte erfolgreich initialisiert.");

  // ----------------------------------------------------
  // 1. DATEN SCHREIBEN (FILE_APPEND oder FILE_WRITE)
  // ----------------------------------------------------
  Serial.println("\n--> 1. Schreibe Testdaten auf die SD-Karte...");

  // FILE_APPEND fügt neue Zeilen ans Ende der Datei an
  File myFile = SD.open("/test_log.txt", FILE_APPEND);

  if (myFile) {
    // Beispielhafte Log-Daten schreiben
    unsigned long uptime = millis() / 1000;
    myFile.printf("LOG [Uptime: %lu s]: Test-Eintrag erfolgreich geschrieben.\n", uptime);
    myFile.println("LOG [Modbus-Sim]: Temp=22.5C | Volt=230.1V | Amp=5.2A");

    // Wichtig: Datei schließen, damit die Puffer auf die Karte geschrieben werden!
    myFile.close();
    Serial.println("[OK] Daten wurden erfolgreich in '/test_log.txt' geschrieben.");
  } else {
    Serial.println("[FEHLER] '/test_log.txt' konnte nicht zum Schreiben geöffnet werden!");
  }

  // ----------------------------------------------------
  // 2. DATEN LESEN & IM SERIAL MONITOR AUSGEBEN
  // ----------------------------------------------------
  Serial.println("\n--> 2. Lese Daten von der SD-Karte ab...");

  myFile = SD.open("/test_log.txt", FILE_READ);

  if (myFile) {
    Serial.println("--- INHALT VON /test_log.txt ---");
    
    // Solange noch Daten in der Datei vorhanden sind, Zeile für Zeile auslesen
    while (myFile.available()) {
      Serial.write(myFile.read());
    }

    myFile.close();
    Serial.println("--- ENDE DER DATEI ---");
  } else {
    Serial.println("[FEHLER] '/test_log.txt' konnte nicht zum Lesen geöffnet werden!");
  }
}

void loop() {
  // Für diesen Test reicht der Durchlauf in setup()
}