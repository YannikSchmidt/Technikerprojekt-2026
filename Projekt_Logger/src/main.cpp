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

#include <SPI.h>          // Library für SPI Kommunikation zur SD-Karte
#include <SD.h>
#include <ModbusMaster.h>
#include <ETH.h>          // Library für Ethernet (WT32-ETH01)
#include <WebServer.h>
#include <time.h>         // Library für die NTP-Zeitsynchronisation

// --- SD-Karte Pins ---
#define SD_CS   4
#define SD_SCK  14
#define SD_MOSI 32
#define SD_MISO 33
// --- Modbus Pins ---
#define RXD2 17
#define TXD2 5

// --- Netzwerkdaten ---
// Echte Adressen in include/network_config.h
#include "network_config.h"

// --- Einstellungen ---
const uint8_t  MODBUS_SLAVE_ID  = 1;
const uint16_t MODBUS_START_REG = 10;           // Registerblock des Sensors: 10..19
const uint16_t MODBUS_REG_COUNT = 10;           // Paket + GAK + 8 Messwerte
const uint8_t  CHANNELS         = 8;
const unsigned long INTERVAL_MEASURE = 5000;    // Sensor abfragen
const unsigned long INTERVAL_RETRY   = 10000;   // Ausgefallene Hardware neu starten
const char*    LOG_FILE         = "/pva_log.dat";
const float    MA_PER_AMPERE    = 1000.0f;      // Modbus liefert mA, angezeigt wird A
const uint32_t TIME_VALID_FROM  = 1788213600;   // 2026-09-01 00:00:00, davor ist die Uhr ungestellt

// --- Datenstrukturen ---
// pack(push, 1) verhindert Padding (auffüllen von leeren Bytes)
#pragma pack(push, 1)
struct MeasureData {
  uint16_t gakID;
  int16_t  current_mA[CHANNELS];   // darf negativ sein (Rückspeisung)
};

struct StorageRecord {
  uint32_t    timestamp;           // UNIX-Zeitstempel
  uint16_t    packetNr;
  MeasureData data;
};
#pragma pack(pop) // beendet das pack(push, 1)

// Bricht den Build ab, falls das Dateiformat versehentlich geändert wird
static_assert(sizeof(StorageRecord) == 24, "StorageRecord muss 24 Byte bleiben, sonst sind alte Logs unlesbar");
// StorageRecord: timestamp (4 Byte) + packetNr (2 Byte) + gakID (2 Byte) + array mit 8 * current_mA á 2 Byte (16 Byte) = 24 Byte

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
uint16_t lastPacketNr = 0;        // zum Erkennen doppelter Messwerte
bool     firstReading = true;


///////////////////////////////////////////////////////////////////////////
// Hilfsfunktionen
///////////////////////////////////////////////////////////////////////////

// --- getUnixTimestamp() --------------------------------------------------
// Liest die aktuelle Zeit aus der internen Uhr des ESP32.
// Rückgabe: UnixTime - Sekunden seit dem 01.01.1970.
uint32_t getUnixTimestamp() {
  time_t now;
  time(&now);
  return (uint32_t)now;
}

// --- isTimeValid() -------------------------------------------------------
// Prüft, ob die Uhr bereits per NTP gestellt wurde.
// Rückgabe: true wenn der Zeitstempel plausibel ist.
bool isTimeValid() {
  return getUnixTimestamp() >= TIME_VALID_FROM;
}

// --- formatTimestamp() ---------------------------------------------------
// Wandelt einen UNIX-Zeitstempel in lesbaren Text für die CSV-Datei um.
// Parameter: unixTime - Sekunden seit dem 01.01.1970.
// Rückgabe:  "YYYY-MM-DD HH:MM:SS" oder ein Hinweis bei ungestellter Uhr.
String formatTimestamp(uint32_t unixTime) {
  if (unixTime < TIME_VALID_FROM) return "Keine_Netzwerkzeit";
  time_t t = unixTime;
  struct tm *ti = localtime(&t);                              // pointer auf struct tm mit lokalen Zeitdaten (notwendig für strftime)
  char buffer[30];                                            // Rückgabevariable
  strftime(buffer, sizeof(buffer), "%Y-%m-%d %H:%M:%S", ti);  // formatiert die Zeit in "YYYY-MM-DD HH:MM:SS"
  return String(buffer);
}

// --- statusHtml() --------------------------------------------------------
// Baut eine eingefärbte Statusmeldung für das Webinterface.
// Parameter: ok - Zustand, textOk/textError - anzuzeigender Text.
// Rückgabe:  HTML-Fragment, grün bei ok, sonst rot.
String statusHtml(bool ok, const char* textOk, const char* textError) {
  if (ok) return String("<b style='color:green'>") + textOk + "</b>";
  return String("<b style='color:red'>") + textError + "</b>";
}


///////////////////////////////////////////////////////////////////////////
// Fehler-Manager & Initialisierung
///////////////////////////////////////////////////////////////////////////

// --- tryInitSD() ---------------------------------------------------------
// Startet den SPI-Bus und die SD-Karte. Kann gefahrlos wiederholt werden,
// der Fehler-Manager ruft die Funktion nach einem Ausfall erneut auf.
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

// --- tryInitETH() --------------------------------------------------------
// Startet Ethernet mit fester IP und stellt anschließend die Uhr per NTP.
// Kann wie tryInitSD() jederzeit erneut aufgerufen werden.
void tryInitETH() {
  if (ethInitialized) return;
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

// --- handleRoot() --------------------------------------------------------
// Beantwortet "/" mit der Live-Ansicht: Systemstatus und aktuelle Messwerte.
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
  html += "Netzwerk: "           + statusHtml(ETH.linkUp(),   "Verbunden", "Getrennt") + "<br>";
  html += "SD-Karte: "           + statusHtml(sdInitialized,  "Verbunden", "Fehler/Fehlt") + "<br>";
  html += "Modbus (Sensorik): "  + statusHtml(modbusReady,    "OK",        "Verbindungsfehler") + "<br>";
  html += "Uhrzeit (NTP): "      + statusHtml(isTimeValid(),  "Synchronisiert", "Nicht synchronisiert") + "<br>";
  html += "Letzte Messzeit: "    + formatTimestamp(currentLiveRecord.timestamp) + "<br>";
  html += "Paketnummer: "        + String(currentLiveRecord.packetNr);
  html += "</div>";

  html += "<h3>Aktuelle Messwerte:</h3><table><tr>";
  for (uint8_t i = 0; i < CHANNELS; i++) html += "<th>Kanal " + String(i + 1) + "</th>";
  html += "</tr><tr>";
  for (uint8_t i = 0; i < CHANNELS; i++) {
    html += "<td>" + String(currentLiveRecord.data.current_mA[i] / MA_PER_AMPERE, 2) + " A</td>";
  }
  html += "</tr></table><br><br>";

  html += "<a href='/download'><button style='padding:10px 20px; font-size:16px;'>Logdaten als CSV Herunterladen</button></a>";
  html += "</body></html>";

  server.send(200, "text/html", html);
}

// --- handleNotFound() ----------------------------------------------------
// Fängt unbekannte Adressen ab, z.B. die automatische Anfrage nach /favicon.ico
void handleNotFound() {
  server.send(404, "text/plain", "Seite nicht gefunden. Startseite: /");
}


///////////////////////////////////////////////////////////////////////////
// CSV Download (Konvertiert Binärdaten zurück zu Text)
///////////////////////////////////////////////////////////////////////////

const size_t SEND_BUFFER_SIZE = 2048;   // Sammelpuffer, damit nicht jede Zeile ein eigenes HTTP-Paket wird
const size_t LINE_BUFFER_SIZE = 256;

// --- handleDownload() ----------------------------------------------------
// Beantwortet "/download": liest die Logdatei satzweise und schickt sie als
// CSV an den Browser. Die Umwandlung nach Text passiert erst hier.
void handleDownload() {
  if (!sdInitialized) {
    server.send(500, "text/plain", "Fehler: Keine SD-Karte gefunden!");
    return;
  }

  File file = SD.open(LOG_FILE, FILE_READ);
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
  char line[LINE_BUFFER_SIZE];
  char buffer[SEND_BUFFER_SIZE];
  size_t bufferFill = 0;

  while (true) {
    // Rückgabewert prüfen: bei abgeschnittener Datei (Stromausfall) kommt ein halber Datensatz
    if (file.read((uint8_t*)&record, sizeof(StorageRecord)) != (int)sizeof(StorageRecord)) break;

    // snprintf formatiert in CSV-Format (Semikolon getrennt), s = String, u = unsigned int, f = float
    int len = snprintf(line, sizeof(line), "%s;%u;%u",
                       formatTimestamp(record.timestamp).c_str(), record.packetNr, record.data.gakID);

    for (uint8_t i = 0; i < CHANNELS && len > 0 && len < (int)sizeof(line); i++) {
      len += snprintf(line + len, sizeof(line) - len, ";%.2f", record.data.current_mA[i] / MA_PER_AMPERE);
    }
    if (len < 0 || len >= (int)sizeof(line) - 1) continue;   // Zeile zu lang, überspringen
    line[len++] = '\n';

    if (bufferFill + len > SEND_BUFFER_SIZE) {    // Puffer voll -> abschicken und neu füllen
      server.sendContent(buffer, bufferFill);
      bufferFill = 0;
    }
    memcpy(buffer + bufferFill, line, len);
    bufferFill += len;
  }

  if (bufferFill > 0) server.sendContent(buffer, bufferFill);
  file.close();
  server.sendContent(""); // sendContent("") beendet den Transfer
}


///////////////////////////////////////////////////////////////////////////
// Datenerfassung & Speicherung
///////////////////////////////////////////////////////////////////////////

// --- sdWriteBinary() -----------------------------------------------------
// Hängt einen Datensatz unverändert an die Logdatei an.
// Parameter: record - der zu speichernde Datensatz.
void sdWriteBinary(const StorageRecord &record) {
  if (!sdInitialized) return; // Wenn keine SD da ist, abbrechen

  File file = SD.open(LOG_FILE, FILE_APPEND);   // FILE_APPEND = anhängen (andere Modi sind FILE_WRITE = überschreiben, FILE_READ = nur lesen)
  if (file) {
    file.write((const uint8_t*)&record, sizeof(StorageRecord));
    file.close();
  } else {
    Serial.println("[FEHLER] Konnte nicht auf SD schreiben! SD wird deaktiviert.");
    sdInitialized = false; // Fehler erzwingt Neuinitialisierung im nächsten Loop
  }
}

// --- readModbus() --------------------------------------------------------
// Fragt die 10 Register des Sensors ab und trägt sie in den Datensatz ein.
// Parameter: record - wird mit Paketnummer, GAK-Nummer und Strömen gefüllt.
// Rückgabe:  true bei erfolgreicher Abfrage, sonst false.
bool readModbus(StorageRecord &record) {
  uint8_t result = node.readHoldingRegisters(MODBUS_START_REG, MODBUS_REG_COUNT);  // entspricht Modbus-Funktionscode 3
  if (result != node.ku8MBSuccess) {                        // ku8MBSuccess = 0, d.h. wenn die Modbus-Abfrage erfolgreich war
    modbusReady = false;
    return false;
  }
  record.packetNr   = node.getResponseBuffer(0);
  record.data.gakID = node.getResponseBuffer(1);
  for (uint8_t i = 0; i < CHANNELS; i++) {
    // Cast nach int16_t stellt das Vorzeichen wieder her, Modbus überträgt vorzeichenlos
    record.data.current_mA[i] = (int16_t)node.getResponseBuffer(2 + i);
  }
  modbusReady = true;
  return true;
}

// --- modbusIdle() --------------------------------------------------------
// Wird von ModbusMaster während der Wartezeit auf die Antwort aufgerufen
// (bis 2 s). Ohne das wäre die Webseite bei einem Sensorausfall nicht
// erreichbar.
void modbusIdle() {
  if (ethInitialized) server.handleClient();
}


///////////////////////////////////////////////////////////////////////////
// Setup & Loop
///////////////////////////////////////////////////////////////////////////

// --- setup() -------------------------------------------------------------
// Wird einmal beim Einschalten ausgeführt und startet alle Baugruppen.
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

// --- loop() --------------------------------------------------------------
// Läuft endlos. Jede Aufgabe prüft selbst, ob sie an der Reihe ist, damit
// keine Aufgabe die anderen blockiert.
void loop() {
  if (ethInitialized) {
    server.handleClient();
  }

  unsigned long currentMillis = millis();

  // --- Fehler-Manager / Retry (Alle 10 Sekunden) ---
  if (currentMillis - lastRetryMillis >= INTERVAL_RETRY) {
    lastRetryMillis = currentMillis;
    if (!sdInitialized) tryInitSD();
    if (!ethInitialized) tryInitETH();
  }

  // --- Modbus Daten zyklisch einlesen & Speichern (Alle 5 Sekunden) ---
  if (currentMillis - lastModbusRead >= INTERVAL_MEASURE) {
    lastModbusRead = currentMillis;

    StorageRecord newRecord = {};

    if (readModbus(newRecord)) {
      newRecord.timestamp = getUnixTimestamp();
      currentLiveRecord = newRecord;    // Live-Anzeige immer aktualisieren

      // Der Sensor liefert nur alle 30 s einen neuen Mittelwert, abgefragt wird alle 5 s.
      // Ohne diese Prüfung stünde jeder Messwert sechsmal in der Datei.
      // Hängt der Sensor, entsteht bewusst eine Lücke im Log statt Scheindaten.
      if (firstReading || newRecord.packetNr != lastPacketNr) {
        lastPacketNr = newRecord.packetNr;
        firstReading = false;
        sdWriteBinary(newRecord);
      }
    }
  }
}
