///////////////////////////////////////////////////////////////////////////
// Technikerprojekt 2026 - Datenlogger (Modbus-Master)
// Liest die Stromwerte des Sensorknotens, speichert sie auf SD und stellt
// sie als Webseite und CSV bereit. Hardware: WT32-ETH01 (ESP32 + LAN8720).
///////////////////////////////////////////////////////////////////////////

// --- WT32-ETH01 Pins ---
// Muss VOR #include <ETH.h> stehen, die Library setzt sonst eigene Defaults (#ifndef)
#define ETH_PHY_ADDR    1
#define ETH_PHY_POWER   16
#define ETH_PHY_MDC     23
#define ETH_PHY_MDIO    18
#define ETH_PHY_TYPE    ETH_PHY_LAN8720
#define ETH_CLK_MODE    ETH_CLOCK_GPIO0_IN   // 50-MHz-Takt kommt an GPIO0 herein

#include <SPI.h>          // Library for SPI communication zur SD-Karte
#include <SD.h>
#include <ModbusMaster.h>
#include <ETH.h>          // Library for Ethernet (WT32-ETH01)
#include <WebServer.h>
#include <time.h>         // Library for NTP time synchronization

// --- SD-Karte Pins ---
#define SD_CS   4
#define SD_SCK  14
#define SD_MOSI 32
#define SD_MISO 33
// --- Modbus Pins ---
#define RXD2 17
#define TXD2 5

// --- Netzwerkdaten ---
// Echte Adressen in include/network_config.h (nicht im Repo, Vorlage: network_config.example.h)
#include "network_config.h"

// --- Einstellungen ---
const uint8_t  MODBUS_SLAVE_ID   = 1;
const uint16_t MODBUS_START_REG  = 10;          // Registerblock des Sensors: 10..19
const uint16_t MODBUS_REG_ANZAHL = 10;          // Paket + GAK + 8 Messwerte
const uint8_t  KANAELE           = 8;
const unsigned long INTERVALL_MESSUNG = 5000;   // Sensor abfragen
const unsigned long INTERVALL_RETRY   = 10000;  // Ausgefallene Hardware neu starten
const char*    LOG_DATEI         = "/pva_log.dat";
const float    MA_JE_AMPERE      = 1000.0f;     // Modbus liefert mA, angezeigt wird A
const uint32_t ZEIT_PLAUSIBEL_AB = 1788213600;  // 2026-09-01 00:00:00, davor ist die Uhr ungestellt

// --- Datenstrukturen ---
// pack(push, 1) verhindert Padding (auffüllen von leeren Bytes)
#pragma pack(push, 1)
struct MessDaten {
  uint16_t gakID;
  int16_t  strom_mA[KANAELE];   // darf negativ sein (Rückspeisung)
};

struct StorageRecord {
  uint32_t  timestamp;          // UNIX-Zeitstempel
  uint16_t  paketNr;
  MessDaten data;
};
#pragma pack(pop) // beendet das pack(push, 1)

// Bricht den Build ab, falls das Dateiformat versehentlich geändert wird
static_assert(sizeof(StorageRecord) == 24, "StorageRecord muss 24 Byte bleiben, sonst sind alte Logs unlesbar");

// --- Instanzen ---
ModbusMaster node;
WebServer server(80);
SPIClass sdSPI(VSPI); // VSPI für SD-Karte weil nicht Standard SPI Pins genutzt werden (V = Virtual)

// --- Globale Status-Variablen (Fehlerüberwachung zur anzeige auf dem Webinterface) ---
bool ethInitialized = false;
bool sdInitialized = false;
bool modbusReady = false;
unsigned long lastRetryMillis = 0;
unsigned long lastModbusRead = 0;

// --- Globale Variablen für Live-Daten ---
StorageRecord currentLiveRecord;
uint16_t letztePaketNr = 0;       // zum Erkennen doppelter Messwerte
bool     ersteMessung  = true;


///////////////////////////////////////////////////////////////////////////
// Hilfsfunktionen
///////////////////////////////////////////////////////////////////////////
// --- Aktuelle Zeit als UNIX-Timestamp holen ---
uint32_t getUnixTimestamp() {
  time_t now;
  time(&now);
  return (uint32_t)now;
}

// --- Prüfen ob die Uhr per NTP gestellt wurde ---
bool zeitIstGueltig() {
  return getUnixTimestamp() >= ZEIT_PLAUSIBEL_AB;
}

// --- UNIX-Zeitstempel für CSV formatieren ---
String formatTimestamp(uint32_t unixTime) {
  if (unixTime < ZEIT_PLAUSIBEL_AB) return "Keine_Netzwerkzeit";
  time_t t = unixTime;
  struct tm *ti = localtime(&t);                              // pointer auf struct tm mit lokalen Zeitdaten (notwendig für strftime)
  char buffer[30];                                            // Rückgabevariable
  strftime(buffer, sizeof(buffer), "%Y-%m-%d %H:%M:%S", ti);  // formatiert die Zeit in "YYYY-MM-DD HH:MM:SS"
  return String(buffer);
}

// --- Statusanzeige für das Webinterface einfärben ---
String statusText(bool ok, const char* textOk, const char* textFehler) {
  if (ok) return String("<b style='color:green'>") + textOk + "</b>";
  return String("<b style='color:red'>") + textFehler + "</b>";
}


///////////////////////////////////////////////////////////////////////////
// Fehler-Manager & Initialisierung
///////////////////////////////////////////////////////////////////////////
void tryInitSD() {
  if (sdInitialized) return;
  sdSPI.begin(SD_SCK, SD_MISO, SD_MOSI, SD_CS);
  if (SD.begin(SD_CS, sdSPI, 4000000)) {      // 4 MHz, bewusst niedrig gegen Übertragungsfehler
    sdInitialized = true;
    Serial.println("[OK] SD-Karte verbunden.");
  } else {
    Serial.println("[FEHLER] SD-Karte nicht gefunden!");
  }
}

void tryInitETH() {
  if (ethInitialized) return;
  // Keine eigene MAC-Adresse: ETH.macAddress() ist eine Lese- und keine Schreibfunktion,
  // die frühere Zuweisung war wirkungslos. Genutzt wird die ab Werk hinterlegte MAC.
  if (ETH.begin(ETH_PHY_ADDR, ETH_PHY_POWER, ETH_PHY_MDC, ETH_PHY_MDIO, ETH_PHY_TYPE, ETH_CLK_MODE)) {
    ETH.config(local_IP, gateway, subnet, primaryDNS, secondaryDNS);   // feste IP statt DHCP
    ethInitialized = true;
    Serial.print("[OK] Ethernet gestartet. IP: ");
    Serial.println(ETH.localIP());

    configTime(0, 0, "pool.ntp.org", "time.nist.gov");
    setenv("TZ", "CET-1CEST,M3.5.0,M10.5.0/3", 1);   // Zeitzonenregel DE, Sommerzeit rechnet der ESP32 selbst
    tzset();
  } else {
    Serial.println("[FEHLER] Ethernet Init fehlgeschlagen!");
  }
}


///////////////////////////////////////////////////////////////////////////
// WEBSERVER
///////////////////////////////////////////////////////////////////////////
void handleRoot() {
  String html;
  html.reserve(2500);   // Speicher einmal am Stück holen, sonst Fragmentierung durch jedes +=

  html += "<!DOCTYPE html><html lang='de'><head><meta charset='UTF-8'>";
  html += "<meta http-equiv='refresh' content='5'>";   // Seite lädt sich passend zum Messintervall neu
  html += "<title>PVA Musterhausen - Live</title>";
  html += "<style>body{font-family: Arial; padding: 20px;} table{border-collapse: collapse; width: 100%; max-width: 800px;} th, td{border: 1px solid #ccc; padding: 10px; text-align: center;} th{background: #eee;} .status{margin-bottom: 20px; padding: 10px; background: #f9f9f9; border: 1px solid #ddd;}</style>";
  html += "</head><body>";

  html += "<h1>PVA Musterhausen - Live-Werte</h1>";

  html += "<div class='status'><h3>System-Status:</h3>";
  html += "Netzwerk: "           + statusText(ETH.linkUp(),   "Verbunden", "Getrennt") + "<br>";
  html += "SD-Karte: "           + statusText(sdInitialized,  "Verbunden", "Fehler/Fehlt") + "<br>";
  html += "Modbus (Sensorik): "  + statusText(modbusReady,    "OK",        "Verbindungsfehler") + "<br>";
  html += "Uhrzeit (NTP): "      + statusText(zeitIstGueltig(), "Synchronisiert", "Nicht synchronisiert") + "<br>";
  html += "Letzte Messzeit: "    + formatTimestamp(currentLiveRecord.timestamp) + "<br>";
  html += "Paketnummer: "        + String(currentLiveRecord.paketNr);
  html += "</div>";

  html += "<h3>Aktuelle Messwerte:</h3><table><tr>";
  for (uint8_t i = 0; i < KANAELE; i++) html += "<th>Kanal " + String(i + 1) + "</th>";
  html += "</tr><tr>";
  for (uint8_t i = 0; i < KANAELE; i++) {
    html += "<td>" + String(currentLiveRecord.data.strom_mA[i] / MA_JE_AMPERE, 2) + " A</td>";
  }
  html += "</tr></table><br><br>";

  html += "<a href='/download'><button style='padding:10px 20px; font-size:16px;'>Logdaten als CSV Herunterladen</button></a>";
  html += "</body></html>";

  server.send(200, "text/html", html);
}

// Fängt unbekannte Adressen ab, z.B. die automatische Anfrage nach /favicon.ico
void handleNotFound() {
  server.send(404, "text/plain", "Seite nicht gefunden. Startseite: /");
}


///////////////////////////////////////////////////////////////////////////
// CSV Download (Konvertiert Binärdaten zurück zu Text)
///////////////////////////////////////////////////////////////////////////
const size_t SENDE_PUFFER  = 2048;   // Sammelpuffer, damit nicht jede Zeile ein eigenes HTTP-Paket wird
const size_t ZEILEN_PUFFER = 256;

void handleDownload() {
  if (!sdInitialized) {
    server.send(500, "text/plain", "Fehler: Keine SD-Karte gefunden!");
    return;
  }

  File file = SD.open(LOG_DATEI, FILE_READ);
  if (!file) {
    server.send(500, "text/plain", "Fehler: Log-Datei nicht gefunden!");
    return;
  }

  server.setContentLength(CONTENT_LENGTH_UNKNOWN);      // Unbekannte Länge, da die Dateigröße dynamisch ist
  server.sendHeader("Content-Disposition", "attachment; filename=\"pva_logdaten.csv\"");
  server.send(200, "text/csv", "");

  // CSV Header (Überschriften)
  server.sendContent("Zeitstempel;Paket;GAK_ID;Kanal_1_A;Kanal_2_A;Kanal_3_A;Kanal_4_A;Kanal_5_A;Kanal_6_A;Kanal_7_A;Kanal_8_A\n");

  StorageRecord record;
  char zeile[ZEILEN_PUFFER];
  char puffer[SENDE_PUFFER];
  size_t fuellstand = 0;

  while (true) {
    // Rückgabewert prüfen: bei abgeschnittener Datei (Stromausfall) kommt ein halber Datensatz
    if (file.read((uint8_t*)&record, sizeof(StorageRecord)) != (int)sizeof(StorageRecord)) break;

    // snprintf formatiert in CSV-Format (Semikolon getrennt), s = String, u = unsigned int, f = float
    int len = snprintf(zeile, sizeof(zeile), "%s;%u;%u",
                       formatTimestamp(record.timestamp).c_str(), record.paketNr, record.data.gakID);

    for (uint8_t i = 0; i < KANAELE && len > 0 && len < (int)sizeof(zeile); i++) {
      len += snprintf(zeile + len, sizeof(zeile) - len, ";%.2f", record.data.strom_mA[i] / MA_JE_AMPERE);
    }
    if (len < 0 || len >= (int)sizeof(zeile) - 1) continue;   // Zeile zu lang, überspringen
    zeile[len++] = '\n';

    if (fuellstand + len > SENDE_PUFFER) {    // Puffer voll -> abschicken und neu füllen
      server.sendContent(puffer, fuellstand);
      fuellstand = 0;
    }
    memcpy(puffer + fuellstand, zeile, len);
    fuellstand += len;
  }

  if (fuellstand > 0) server.sendContent(puffer, fuellstand);
  file.close();
  server.sendContent(""); // sendContent("") beendet den Transfer
}


///////////////////////////////////////////////////////////////////////////
// Datenerfassung & Speicherung
///////////////////////////////////////////////////////////////////////////
void sdWriteBinary(const StorageRecord &record) {
  if (!sdInitialized) return; // Wenn keine SD da ist, abbrechen

  File file = SD.open(LOG_DATEI, FILE_APPEND);   // FILE_APPEND = anhängen (andere Modi sind FILE_WRITE = überschreiben, FILE_READ = nur lesen)
  if (file) {
    file.write((const uint8_t*)&record, sizeof(StorageRecord));
    file.close();
  } else {
    Serial.println("[FEHLER] Konnte nicht auf SD schreiben! SD wird deaktiviert.");
    sdInitialized = false; // Fehler erzwingt Neuinitialisierung im nächsten Loop
  }
}

bool readModBus(StorageRecord &record) {
  uint8_t result = node.readHoldingRegisters(MODBUS_START_REG, MODBUS_REG_ANZAHL);  // entspricht Modbus-Funktionscode 3
  if (result != node.ku8MBSuccess) {                        // ku8MBSuccess = 0, d.h. wenn die Modbus-Abfrage erfolgreich war
    modbusReady = false;
    return false;
  }
  record.paketNr    = node.getResponseBuffer(0);
  record.data.gakID = node.getResponseBuffer(1);
  for (uint8_t i = 0; i < KANAELE; i++) {
    // Cast nach int16_t stellt das Vorzeichen wieder her, Modbus überträgt vorzeichenlos
    record.data.strom_mA[i] = (int16_t)node.getResponseBuffer(2 + i);
  }
  modbusReady = true;
  return true;
}

// Wird von ModbusMaster während der Wartezeit auf die Antwort aufgerufen (bis 2 s).
// Ohne das wäre die Webseite bei einem Sensorausfall nicht erreichbar.
void modbusIdle() {
  if (ethInitialized) server.handleClient();
}


///////////////////////////////////////////////////////////////////////////
// Setup & Loop
///////////////////////////////////////////////////////////////////////////
void setup() {
  Serial.begin(115200);
  delay(1000);

  tryInitETH();
  tryInitSD();

  // --- Modbus Initialisierung ---
  Serial2.begin(9600, SERIAL_8N1, RXD2, TXD2);   // 8N1 = 8 Datenbits, keine Parität, 1 Stoppbit
  node.begin(MODBUS_SLAVE_ID, Serial2);
  node.idle(modbusIdle);

  // --- Webserver Initialisierung ---
  server.on("/", HTTP_GET, handleRoot);
  server.on("/download", HTTP_GET, handleDownload);
  server.onNotFound(handleNotFound);
  server.begin();
}

void loop() {
  if (ethInitialized) {
    server.handleClient();
  }

  unsigned long currentMillis = millis();

  // --- Fehler-Manager / Retry (Alle 10 Sekunden) ---
  if (currentMillis - lastRetryMillis >= INTERVALL_RETRY) {
    lastRetryMillis = currentMillis;
    if (!sdInitialized) tryInitSD();
    if (!ethInitialized) tryInitETH();
  }

  // --- Modbus Daten zyklisch einlesen & Speichern (Alle 5 Sekunden) ---
  if (currentMillis - lastModbusRead >= INTERVALL_MESSUNG) {
    lastModbusRead = currentMillis;

    StorageRecord neuerRecord = {};

    if (readModBus(neuerRecord)) {
      neuerRecord.timestamp = getUnixTimestamp();
      currentLiveRecord = neuerRecord;    // Live-Anzeige immer aktualisieren

      // Der Sensor liefert nur alle 30 s einen neuen Mittelwert, abgefragt wird alle 5 s.
      // Ohne diese Prüfung stünde jeder Messwert sechsmal in der Datei.
      // Hängt der Sensor, entsteht bewusst eine Lücke im Log statt Scheindaten.
      if (ersteMessung || neuerRecord.paketNr != letztePaketNr) {
        letztePaketNr = neuerRecord.paketNr;
        ersteMessung = false;
        sdWriteBinary(neuerRecord);
      }
    }
  }
}
