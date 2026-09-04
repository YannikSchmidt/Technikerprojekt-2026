#include <SPI.h>
#include <SD.h>
#include <ModbusMaster.h>
#include <ETH.h>
#include <WebServer.h>

// Pinbelegung für WT32-ETH01
#define SD_CS   4
#define SD_SCK  14
#define SD_MOSI 32
#define SD_MISO 33

#define RXD2 17
#define TXD2 5
// WT32-ETH01 spezifische Pin-Konfiguration
#define ETH_PHY_ADDR    1
#define ETH_PHY_POWER   16
#define ETH_PHY_MDC     23
#define ETH_PHY_MDIO    18
#define ETH_PHY_TYPE    ETH_PHY_LAN8720
#define ETH_CLK_MODE    ETH_CLOCK_GPIO0_IN  // WICHTIG: Muss GPIO0_IN sein beim WT32-ETH01!

// Deine Netzwerkdaten
IPAddress local_IP(192, 168,  1, 50);    // Static IP Address
IPAddress gateway(192, 168,  1, 1);      // Gateway
IPAddress subnet(255, 255, 255, 0);     // Subnet Mask
IPAddress primaryDNS(8, 8, 8, 8);       // Primary DNS
IPAddress secondaryDNS(8, 8, 4, 4);     // Secondary DNS

uint8_t mac[] = {0xDE, 0xAD, 0xBE, 0xEE, 0xFE, 0xEE};

uint32_t g_Time = 0;
uint16_t g_paketNr = 0;

// Modbus-Instanz initialisieren
ModbusMaster node;
WebServer server(80);
SPIClass sdSPI(VSPI);

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

String sdRead(const char* filepath) {
  Serial.println("\n--> Lese Daten von der SD-Karte...");
  File file = SD.open(filepath, FILE_READ);
  String content = "";
  if (file) {
    Serial.println("--- INHALT VON /test_log.txt ---");
    while (file.available()) {
      char c = (char)file.read();
      Serial.print(c);
      if (c == '\n') {
        content += "<br>";
      } else {
        content += c;
      }
    }
    Serial.print("\n");

    file.close();
    Serial.println("--- ENDE DER DATEI ---");
  } else {
    Serial.println("[FEHLER] Datei konnte nicht gelesen werden!");
  }
  return content;
}

void handleRoot()
{
  String html = sdRead("/test_log.txt");
  html += "<h1>Strangstroeme PVA Musterhausen</h1>";
  html += "<p>Technikerprojekt 2026 von Danny, Jakob, Florian und Yannik</p>";
  html += "<p>IP: " + local_IP.toString() + "</p>";
  html += "</body></html>";
  server.send(200, "text/html", html);
}

// Öffnet die Datei direkt beim Schreiben im APPEND-Modus
void sdWrite(const char* filepath, const struct logData &data) {
  Serial.println("\n--> Schreibe Daten auf die SD-Karte...");
  
  File file = SD.open(filepath, FILE_APPEND);
  
  if (file) {
    file.printf("(Paket)RTC:%u;", g_paketNr);
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
    Serial.printf("(Paket)RTC:%u;", g_paketNr);
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

bool readModBus(int address, struct logData &data) {
  Serial.println("Lese Holding Register...");
  uint8_t result = node.readHoldingRegisters(address, 9);

  if (result == node.ku8MBSuccess) {
    g_paketNr = node.getResponseBuffer(0);
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

  // 1. Eigene MAC-Adresse setzen (optional, aber wenn gewünscht, dann vor begin)
  ETH.macAddress(mac);
  // 2. Ethernet mit den exakten Pins starten (nur EINMAL aufrufen!)
  if (!ETH.begin(ETH_PHY_ADDR, ETH_PHY_POWER, ETH_PHY_MDC, ETH_PHY_MDIO, ETH_PHY_TYPE, ETH_CLK_MODE)) {
    Serial.println("Fehler beim Starten des Ethernet-Moduls!");
  }
  // 3. Statische IP konfigurieren
  ETH.config(local_IP, gateway, subnet, primaryDNS, secondaryDNS);

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
  
  server.on("/", HTTP_GET, handleRoot);
  server.begin();

  Serial.println("WebServer started on port 80");
  Serial.print("on IP Address: ");
  Serial.println(ETH.localIP());
}

void loop() {
  server.handleClient();

  logData curData = {0};

  getTime();                        // Korrekte Funktions-Syntax ()
  curData.realTimeStamp = g_Time;   // Timestamp zuweisen

  if(readModBus(10, curData)){
    sdWrite("/test_log.txt", curData);
  }

  delay(5000); // Pause im Haupt-Loop für Stabilität
}