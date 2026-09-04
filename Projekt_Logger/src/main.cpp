#include <SPI.h>
#include <SD.h>
#include <ModbusMaster.h>
#include <ETH.h>
#include <WebServer.h>
#include <time.h> 

// --- Modbus Pins ---
#define SD_CS   4
#define SD_SCK  14
#define SD_MOSI 32
#define SD_MISO 33
// --- SD-Card Pins ---
#define RXD2 17
#define TXD2 5
// --- WT32-ETH01 Pins ---
#define ETH_PHY_ADDR    1
#define ETH_PHY_POWER   16
#define ETH_PHY_MDC     23
#define ETH_PHY_MDIO    18
#define ETH_PHY_TYPE    ETH_PHY_LAN8720
#define ETH_CLK_MODE    ETH_CLOCK_GPIO0_IN

// --- Netzwerkdaten ---
// Die Adressen dieses Netzes stehen in include/network_config.h. Diese Datei
// liegt bewusst nicht im Repository. Zum Bauen einmalig die Vorlage
// include/network_config.example.h nach include/network_config.h kopieren
// und die eigenen Werte eintragen.
#include "network_config.h"
uint8_t mac[] = {0xDE, 0xAD, 0xBE, 0xEE, 0xFE, 0xEE};

// --- Globale Status-Variablen (Fehlerüberwachung) ---
bool ethInitialized = false;
bool sdInitialized = false;
bool modbusReady = false;
unsigned long lastRetryMillis = 0;
unsigned long lastModbusRead = 0;

// --- Instanzen ---
ModbusMaster node;
WebServer server(80);
SPIClass sdSPI(VSPI);

// --- Datenstrukturen ---
// WICHTIG: pack(push, 1) verhindert Padding, damit der Struct genau passend
// im Binärformat auf die SD-Karte geschrieben wird (spart maximal Speicher).
#pragma pack(push, 1)
struct logData {
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

struct StorageRecord {
  uint32_t timestamp; // UNIX-Zeitstempel (nur 4 Bytes!)
  uint16_t paketNr;
  logData data;
};
#pragma pack(pop)

// Globale Variablen für Live-Daten
StorageRecord currentLiveRecord; 
uint16_t g_paketNr = 0;


// --- Hilfsfunktion: Aktuelle Zeit als UNIX-Timestamp holen ---
uint32_t getUnixTimestamp() {
  time_t now;
  time(&now);
  return (uint32_t)now;
}

// --- Hilfsfunktion: UNIX-Zeitstempel für CSV formatieren ---
String formatTimestamp(uint32_t unixTime) {
  // Wenn Zeit noch nicht synchronisiert ist (Wert nahe 1970)
  if (unixTime < 1600000000) return "Keine_Netzwerkzeit"; 
  
  time_t t = unixTime;
  struct tm *ti = localtime(&t);
  char buffer[30];
  strftime(buffer, sizeof(buffer), "%Y-%m-%d %H:%M:%S", ti);
  return String(buffer);
}


///////////////////////////////////////////////////////////////////////////
// FEHLER-MANAGER & INITIALISIERUNG
///////////////////////////////////////////////////////////////////////////
void tryInitSD() {
  if (sdInitialized) return;
  sdSPI.begin(SD_SCK, SD_MISO, SD_MOSI, SD_CS);
  if (SD.begin(SD_CS, sdSPI, 4000000)) {
    sdInitialized = true;
    Serial.println("[OK] SD-Karte verbunden.");
  } else {
    Serial.println("[FEHLER] SD-Karte nicht gefunden!");
  }
}

void tryInitETH() {
  if (ethInitialized) return;
  ETH.macAddress(mac);
  if (ETH.begin(ETH_PHY_ADDR, ETH_PHY_POWER, ETH_PHY_MDC, ETH_PHY_MDIO, ETH_PHY_TYPE, ETH_CLK_MODE)) {
    ETH.config(local_IP, gateway, subnet, primaryDNS, secondaryDNS);
    ethInitialized = true;
    Serial.println("[OK] Ethernet gestartet.");
    
    // NTP Server starten sobald Netzwerk da ist
    configTime(0, 0, "pool.ntp.org", "time.nist.gov");
    setenv("TZ", "CET-1CEST,M3.5.0,M10.5.0/3", 1); 
    tzset();
  } else {
    Serial.println("[FEHLER] Ethernet Init fehlgeschlagen!");
  }
}


///////////////////////////////////////////////////////////////////////////
// WEBSERVER (Einfaches Design & Live Werte)
///////////////////////////////////////////////////////////////////////////
void handleRoot() {
  // Sehr einfaches, leicht verständliches HTML
  String html = "<!DOCTYPE html><html lang='de'><head><meta charset='UTF-8'>";
  html += "<title>PVA Musterhausen - Live</title>";
  html += "<style>body{font-family: Arial; padding: 20px;} table{border-collapse: collapse; width: 100%; max-width: 800px;} th, td{border: 1px solid #ccc; padding: 10px; text-align: center;} th{background: #eee;} .status{margin-bottom: 20px; padding: 10px; background: #f9f9f9; border: 1px solid #ddd;}</style>";
  html += "</head><body>";
  
  html += "<h1> PVA Musterhausen - Live-Werte</h1>";
  
  // Systemstatus-Anzeige
  html += "<div class='status'><h3>System-Status:</h3>";
  html += "Netzwerk: " + String(ETH.linkUp() ? "<b style='color:green'>Verbunden</b>" : "<b style='color:red'>Getrennt</b>") + "<br>";
  html += "SD-Karte: " + String(sdInitialized ? "<b style='color:green'>Verbunden</b>" : "<b style='color:red'>Fehler/Fehlt</b>") + "<br>";
  html += "Modbus (Sensorik): " + String(modbusReady ? "<b style='color:green'>OK</b>" : "<b style='color:red'>Verbindungsfehler</b>") + "<br>";
  html += "Letzte Messzeit: " + formatTimestamp(currentLiveRecord.timestamp);
  html += "</div>";

  // Live-Werte Tabelle (mit Umrechnung / 1000.0)
  html += "<h3>Aktuelle Messwerte:</h3>";
  html += "<table><tr><th>Kanal 1</th><th>Kanal 2</th><th>Kanal 3</th><th>Kanal 4</th>";
  html += "<th>Kanal 5</th><th>Kanal 6</th><th>Kanal 7</th><th>Kanal 8</th></tr><tr>";
  
  html += "<td>" + String(currentLiveRecord.data.value1 / 1000.0, 2) + " A</td>";
  html += "<td>" + String(currentLiveRecord.data.value2 / 1000.0, 2) + " A</td>";
  html += "<td>" + String(currentLiveRecord.data.value3 / 1000.0, 2) + " A</td>";
  html += "<td>" + String(currentLiveRecord.data.value4 / 1000.0, 2) + " A</td>";
  html += "<td>" + String(currentLiveRecord.data.value5 / 1000.0, 2) + " A</td>";
  html += "<td>" + String(currentLiveRecord.data.value6 / 1000.0, 2) + " A</td>";
  html += "<td>" + String(currentLiveRecord.data.value7 / 1000.0, 2) + " A</td>";
  html += "<td>" + String(currentLiveRecord.data.value8 / 1000.0, 2) + " A</td>";
  html += "</tr></table><br><br>";

  html += "<a href='/download'><button style='padding:10px 20px; font-size:16px;'> Logdaten als CSV Herunterladen</button></a>";
  html += "</body></html>";
  
  server.send(200, "text/html", html);
}


///////////////////////////////////////////////////////////////////////////
// CSV DOWNLOAD (Konvertiert Binärdaten zurück zu Text)
///////////////////////////////////////////////////////////////////////////
void handleDownload() {
  if (!sdInitialized) {
    server.send(500, "text/plain", "Fehler: Keine SD-Karte gefunden!");
    return;
  }

  File file = SD.open("/pva_log.dat", FILE_READ);
  if (!file) {
    server.send(500, "text/plain", "Fehler: Log-Datei nicht gefunden!");
    return;
  }

  server.setContentLength(CONTENT_LENGTH_UNKNOWN);
  server.sendHeader("Content-Disposition", "attachment; filename=\"pva_logdaten.csv\"");
  server.send(200, "text/csv", "");

  // CSV Header
  server.sendContent("Zeitstempel;Paket;GAK_ID;Kanal_1_A;Kanal_2_A;Kanal_3_A;Kanal_4_A;Kanal_5_A;Kanal_6_A;Kanal_7_A;Kanal_8_A\n");

  StorageRecord record;
  char buffer[256];

  // Lese Datei strukturweise aus
  while (file.available() >= sizeof(StorageRecord)) {
    file.read((uint8_t*)&record, sizeof(StorageRecord));

    // Zeit formatieren und Werte durch 1000.0 teilen
    String timeStr = formatTimestamp(record.timestamp);
    
    snprintf(buffer, sizeof(buffer), "%s;%u;%u;%.2f;%.2f;%.2f;%.2f;%.2f;%.2f;%.2f;%.2f\n",
             timeStr.c_str(), record.paketNr, record.data.gakID,
             record.data.value1 / 1000.0, record.data.value2 / 1000.0, 
             record.data.value3 / 1000.0, record.data.value4 / 1000.0, 
             record.data.value5 / 1000.0, record.data.value6 / 1000.0, 
             record.data.value7 / 1000.0, record.data.value8 / 1000.0);
             
    server.sendContent(String(buffer));
  }
  
  file.close();
  server.sendContent(""); 
}


///////////////////////////////////////////////////////////////////////////
// DATENERFASSUNG & SPEICHERUNG
///////////////////////////////////////////////////////////////////////////
void sdWriteBinary(const StorageRecord &record) {
  if (!sdInitialized) return; // Wenn keine SD da ist, abbrechen

  // FILE_APPEND öffnet die Datei und hängt hinten an (Wear Leveling freundlich)
  File file = SD.open("/pva_log.dat", FILE_APPEND);
  if (file) {
    file.write((const uint8_t*)&record, sizeof(StorageRecord));
    file.close(); 
  } else {
    Serial.println("[FEHLER] Konnte nicht auf SD schreiben! SD wird deaktiviert.");
    sdInitialized = false; // Fehler erzwingt Neuinitialisierung im nächsten Loop
  }
}

bool readModBus(int address, logData &data) {
  uint8_t result = node.readHoldingRegisters(address, 10);
  if (result == node.ku8MBSuccess) {
    g_paketNr   = node.getResponseBuffer(0);
    data.gakID  = node.getResponseBuffer(1);
    data.value1 = node.getResponseBuffer(2);
    data.value2 = node.getResponseBuffer(3);
    data.value3 = node.getResponseBuffer(4);
    data.value4 = node.getResponseBuffer(5);
    data.value5 = node.getResponseBuffer(6);
    data.value6 = node.getResponseBuffer(7);
    data.value7 = node.getResponseBuffer(8);
    data.value8 = node.getResponseBuffer(9);
    modbusReady = true;
    return true;
  } else {
    modbusReady = false;
    return false;
  }
}


///////////////////////////////////////////////////////////////////////////
// SETUP & LOOP
///////////////////////////////////////////////////////////////////////////
void setup() {
  Serial.begin(115200);
  delay(1000);

  // 1. Initialisierungs-Versuche (ohne Blockierung!)
  tryInitETH();
  tryInitSD();

  // Modbus Vorbereitung
  Serial2.begin(9600, SERIAL_8N1, RXD2, TXD2);
  node.begin(1, Serial2);

  // Webserver vorbereiten
  server.on("/", HTTP_GET, handleRoot);
  server.on("/download", HTTP_GET, handleDownload); 
  server.begin();
}

void loop() {
  // Webserver Clients bedienen (läuft auch, wenn ETH temporär getrennt wurde)
  if (ethInitialized) {
    server.handleClient();
  }

  unsigned long currentMillis = millis();

  // --- Fehler-Manager / Retry (Alle 10 Sekunden) ---
  if (currentMillis - lastRetryMillis >= 10000) {
    lastRetryMillis = currentMillis;
    if (!sdInitialized) tryInitSD();
    if (!ethInitialized) tryInitETH();
  }

  // --- Modbus Daten zyklisch einlesen & Speichern (Alle 5 Sekunden) ---
  if (currentMillis - lastModbusRead >= 5000) {
    lastModbusRead = currentMillis; 
    
    logData curData = {0};
    
    // Daten von Modbus holen (funktioniert auch, wenn Netzwerk oder SD ausfällt!)
    if (readModBus(10, curData)) {
      
      // Live-Werte für die Webseite speichern
      currentLiveRecord.timestamp = getUnixTimestamp();
      currentLiveRecord.paketNr = g_paketNr;
      currentLiveRecord.data = curData;

      // Als binären Struct auf die SD speichern
      sdWriteBinary(currentLiveRecord);
    }
  }
}