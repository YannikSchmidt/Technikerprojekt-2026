///////////////////////////////////////////////////////////////////////////////
//
//  TECHNIKERPROJEKT 2026 - DATENLOGGER (Modbus-Master)
//
//  AUFGABE
//  Dieses Geraet liest zyklisch die Stromwerte von acht Strangleitungen einer
//  Photovoltaik-Anlage aus, speichert sie mit Zeitstempel auf einer SD-Karte
//  und stellt sie im Netzwerk als Webseite sowie als CSV-Datei bereit.
//
//  HARDWARE
//  WT32-ETH01: ESP32 mit fest verbautem Ethernet-Chip LAN8720.
//  Dazu ein SD-Karten-Modul (SPI) und ein RS485-Transceiver (UART).
//
//  DATENFLUSS
//
//     [Projekt_Sensor]                [dieses Geraet]              [Anwender]
//      ESP32 + ACS758                  WT32-ETH01
//           |                               |                          |
//           |  1) Modbus RTU ueber RS485    |                          |
//           |------------------------------>|                          |
//           |     alle 5 s, 10 Register     |                          |
//           |                               |                          |
//           |                    2) + Zeitstempel von NTP              |
//           |                               |                          |
//           |                    3) binaer auf SD-Karte                |
//           |                       (24 Byte je Datensatz)             |
//           |                               |                          |
//           |                               |  4) HTTP: Live-Ansicht   |
//           |                               |<-------------------------|
//           |                               |     und CSV-Download     |
//
//  WARUM BINAER SPEICHERN UND ERST BEIM DOWNLOAD IN CSV WANDELN?
//  Ein Datensatz als Text ("2026-03-14 12:00:00;17;1;5.12;...") braucht rund
//  70 Zeichen, also 70 Byte. Als Binaerstruktur sind es nur 24 Byte. Das ist
//  nicht einmal ein Drittel. Die SD-Karte wird also seltener beschrieben und
//  die Datei bleibt klein. Lesbaren Text braucht nur der Mensch - und der
//  bekommt ihn beim Download erzeugt.
//
//  WARUM KEIN delay() IM PROGRAMM?
//  delay() haelt den gesamten Prozessor an. Waehrend eines delay(5000) koennte
//  der Webserver keine Anfrage beantworten. Stattdessen wird mit millis() (der
//  Betriebszeit in Millisekunden) verglichen, ob genug Zeit vergangen ist.
//  Dieses Muster heisst "nicht blockierend" und ist der Standardweg bei
//  Arduino, sobald mehrere Dinge gleichzeitig laufen sollen.
//
//  QUELLEN ZUM NACHLESEN (Stand 2026)
//  - Modbus-Spezifikation (offiziell, kostenlos):
//    https://modbus.org/docs/Modbus_Application_Protocol_V1_1b3.pdf
//  - Bibliothek ModbusMaster:      https://github.com/4-20ma/ModbusMaster
//  - Arduino-ESP32 Ethernet (ETH): https://docs.espressif.com/projects/arduino-esp32/en/latest/api/ethernet.html
//  - Arduino-ESP32 SD ueber SPI:   https://docs.espressif.com/projects/arduino-esp32/en/latest/api/sdmmc.html
//  - WT32-ETH01 Pinbelegung:       https://github.com/egnor/wt32-eth01
//  - millis() statt delay():       https://docs.arduino.cc/built-in-examples/digital/BlinkWithoutDelay
//  - UNIX-Zeitstempel erklaert:    https://de.wikipedia.org/wiki/Unixzeit
//
///////////////////////////////////////////////////////////////////////////////


///////////////////////////////////////////////////////////////////////////////
// 1. PINBELEGUNG
//
// WICHTIG: Die ETH_-Definitionen muessen VOR #include <ETH.h> stehen.
// Die Bibliothek legt darin naemlich eigene Standardwerte fest, allerdings nur
// dann, wenn der jeweilige Name noch nicht definiert ist (#ifndef). Stehen
// unsere Werte vorher, gewinnen unsere. Stehen sie danach, meldet der Compiler
// "macro redefined".
///////////////////////////////////////////////////////////////////////////////

// --- Ethernet-Chip LAN8720, fest auf dem WT32-ETH01 verdrahtet ---
// Diese Werte sind durch die Platine vorgegeben und duerfen nicht veraendert
// werden. ETH_CLOCK_GPIO0_IN bedeutet: Der 50-MHz-Takt kommt von aussen an
// GPIO0 herein. Beim WT32-ETH01 ist das zwingend so.
#define ETH_PHY_ADDR    1
#define ETH_PHY_POWER   16
#define ETH_PHY_MDC     23
#define ETH_PHY_MDIO    18
#define ETH_PHY_TYPE    ETH_PHY_LAN8720
#define ETH_CLK_MODE    ETH_CLOCK_GPIO0_IN

#include <SPI.h>
#include <SD.h>
#include <ModbusMaster.h>
#include <ETH.h>
#include <WebServer.h>
#include <time.h>

// --- SD-Karte (SPI-Bus) ---
// CS  = Chip Select, waehlt den Baustein aus
// SCK = Takt, MOSI = Daten zum Modul, MISO = Daten vom Modul
#define SD_CS   4
#define SD_SCK  14
#define SD_MOSI 32
#define SD_MISO 33

// --- RS485-Transceiver (zweite serielle Schnittstelle Serial2) ---
// RXD2 empfaengt, TXD2 sendet. Der verwendete Transceiver schaltet die
// Senderichtung selbst um, deshalb wird kein zusaetzlicher DE/RE-Pin benoetigt.
#define RXD2 17
#define TXD2 5

// --- Netzwerkdaten ---
// Die Adressen dieses Netzes stehen in include/network_config.h. Diese Datei
// liegt bewusst nicht im Repository. Zum Bauen einmalig die Vorlage
// include/network_config.example.h nach include/network_config.h kopieren
// und die eigenen Werte eintragen.
#include "network_config.h"


///////////////////////////////////////////////////////////////////////////////
// 2. EINSTELLUNGEN
//
// Alle veraenderbaren Zahlen stehen hier gesammelt und haben einen Namen.
// Das vermeidet sogenannte "magic numbers" - also Zahlen, die irgendwo mitten
// im Programm stehen und bei denen spaeter niemand mehr weiss, was sie bedeuten.
///////////////////////////////////////////////////////////////////////////////

// --- Modbus ---
// Der Sensor meldet sich unter der Adresse 1. Seine Messwerte liegen ab
// Register 10 und belegen 10 aufeinanderfolgende Register.
// Die Registeraufteilung ist zwischen Sensor und Logger fest vereinbart:
//
//   Register 10 : Paketnummer (zaehlt bei jedem neuen Mittelwert hoch)
//   Register 11 : GAK-Nummer  (welcher Generatoranschlusskasten)
//   Register 12 : Strang 1 in Milliampere
//   ...
//   Register 19 : Strang 8 in Milliampere
//
const uint8_t  MODBUS_SLAVE_ID   = 1;
const uint16_t MODBUS_START_REG  = 10;
const uint16_t MODBUS_REG_ANZAHL = 10;
const uint32_t MODBUS_BAUDRATE   = 9600;

// --- Anzahl der Messkanaele (Straenge) ---
const uint8_t KANAELE = 8;

// --- Zeitintervalle in Millisekunden ---
const unsigned long INTERVALL_MESSUNG = 5000;   // Sensor abfragen und speichern
const unsigned long INTERVALL_RETRY   = 10000;  // Defekte Hardware neu versuchen

// --- Dateiname des Logbuchs auf der SD-Karte ---
const char* LOG_DATEI = "/pva_log.dat";

// --- Umrechnung ---
// Der Sensor liefert Milliampere als ganze Zahl, damit ueber Modbus keine
// Kommazahlen uebertragen werden muessen (Modbus kennt nur 16-Bit-Register).
// Fuer die Anzeige wird wieder in Ampere umgerechnet.
const float MA_JE_AMPERE = 1000.0f;

// --- Plausibilitaetsgrenze fuer die Uhrzeit ---
// Ohne Netzwerk startet die interne Uhr des ESP32 bei 0, das entspricht dem
// 01.01.1970. Alles vor dem 13.09.2020 (Zeitstempel 1600000000) kann also
// keine echte Uhrzeit sein und wird als "nicht synchronisiert" behandelt.
const uint32_t ZEIT_PLAUSIBEL_AB = 1600000000UL;


///////////////////////////////////////////////////////////////////////////////
// 3. DATENSTRUKTUREN
//
// #pragma pack(push, 1) schaltet das sogenannte "Padding" ab.
// Normalerweise fuegt der Compiler zwischen den Feldern einer Struktur
// unsichtbare Fuellbytes ein, damit jedes Feld an einer fuer den Prozessor
// guenstigen Adresse liegt. Auf der SD-Karte waere das verschwendeter Platz
// und die Dateigroesse haenge vom Compiler ab. Mit pack(1) liegt jedes Feld
// direkt hinter dem vorherigen - die Datei ist damit exakt vorhersagbar.
//
// Groesse eines Datensatzes:
//   zeitstempel   4 Byte
//   paketNr       2 Byte
//   gakID         2 Byte
//   strom_mA[8]  16 Byte  (8 Werte zu je 2 Byte)
//   --------------------
//   Summe        24 Byte
//
// Rechenbeispiel zur Dateigroesse:
//   24 Byte alle 5 s  =  17280 Datensaetze pro Tag  =  rund 415 kB pro Tag
//   Ein Jahr Dauerbetrieb ergibt somit etwa 150 MB.
///////////////////////////////////////////////////////////////////////////////
#pragma pack(push, 1)

struct MessDaten {
  uint16_t gakID;                // Nummer des Generatoranschlusskastens
  int16_t  strom_mA[KANAELE];    // Strom je Strang in Milliampere, darf negativ sein
};

struct Datensatz {
  uint32_t  zeitstempel;         // UNIX-Zeit: Sekunden seit dem 01.01.1970
  uint16_t  paketNr;             // Laufende Nummer des Sensors
  MessDaten daten;
};

#pragma pack(pop)

// Sicherheitsnetz: static_assert prueft die Groesse bereits beim Uebersetzen.
// Stimmt sie nicht mehr, bricht der Compiler mit einer klaren Meldung ab,
// statt dass spaeter unbemerkt unlesbare Logdateien entstehen. Wird ein Feld
// hinzugefuegt, muessen die Zahl hier und alte Logdateien angepasst werden.
static_assert(sizeof(MessDaten) == 18, "MessDaten muss genau 18 Byte gross sein");
static_assert(sizeof(Datensatz) == 24, "Datensatz muss genau 24 Byte gross sein - sonst sind alte Logdateien unlesbar");


///////////////////////////////////////////////////////////////////////////////
// 4. GLOBALE OBJEKTE UND ZUSTANDSVARIABLEN
///////////////////////////////////////////////////////////////////////////////

ModbusMaster node;        // Modbus-Master, fragt den Sensor ab
WebServer    server(80);  // Webserver auf dem Standard-HTTP-Port 80
SPIClass     sdSPI(VSPI); // Eigener SPI-Bus fuer die SD-Karte

// Statusmerker. Sie zeigen an, welche Baugruppe gerade funktioniert.
// Der Logger soll bewusst weiterlaufen, auch wenn eine davon ausfaellt.
bool ethBereit = false;
bool sdBereit  = false;
bool modbusOk  = false;

// Zeitpunkte der letzten Ausfuehrung, fuer die millis()-Vergleiche
unsigned long letzterRetry   = 0;
unsigned long letzteMessung  = 0;

// Der zuletzt gelesene Datensatz. Er wird auf der SD-Karte gespeichert und
// gleichzeitig auf der Webseite als Live-Wert angezeigt.
// Globale Variablen werden vom System automatisch mit 0 vorbelegt.
Datensatz aktuellerDatensatz;


///////////////////////////////////////////////////////////////////////////////
// 5. HILFSFUNKTIONEN ZUR ZEIT
//
// Der ESP32 hat keine batteriegepufferte Uhr. Nach jedem Start weiss er nicht,
// wie spaet es ist. Deshalb holt er sich die Uhrzeit ueber das Netzwerk von
// einem NTP-Server (Network Time Protocol). Bis das geklappt hat, liefert die
// interne Uhr Werte nahe 0.
///////////////////////////////////////////////////////////////////////////////

// Liefert die aktuelle Zeit als UNIX-Zeitstempel (Sekunden seit 01.01.1970).
uint32_t holeZeitstempel() {
  time_t jetzt;
  time(&jetzt);
  return (uint32_t)jetzt;
}

// Prueft, ob die Uhr bereits vom NTP-Server gestellt wurde.
bool zeitIstGueltig() {
  return holeZeitstempel() >= ZEIT_PLAUSIBEL_AB;
}

// Wandelt einen UNIX-Zeitstempel in lesbaren Text um, z. B. "2026-03-14 12:00:05".
// Bei noch nicht gestellter Uhr wird das im Text deutlich gemacht, damit im
// Nachhinein niemand einen falschen Zeitpunkt fuer echt haelt.
String formatiereZeit(uint32_t unixZeit) {
  if (unixZeit < ZEIT_PLAUSIBEL_AB) {
    return "Keine_Netzwerkzeit";
  }
  time_t t = unixZeit;
  struct tm *zeitfelder = localtime(&t);
  char text[32];
  strftime(text, sizeof(text), "%Y-%m-%d %H:%M:%S", zeitfelder);
  return String(text);
}


///////////////////////////////////////////////////////////////////////////////
// 6. INITIALISIERUNG DER BAUGRUPPEN
//
// Beide Funktionen sind absichtlich so gebaut, dass sie jederzeit erneut
// aufgerufen werden koennen. Schlaegt der Start fehl, versucht es der
// Fehler-Manager in loop() alle 10 Sekunden erneut. So bootet das Geraet
// zum Beispiel auch dann durch, wenn die SD-Karte beim Einschalten fehlt und
// erst spaeter gesteckt wird.
///////////////////////////////////////////////////////////////////////////////

void starteSD() {
  if (sdBereit) return;

  // Die vier SD-Pins liegen nicht auf den Standardpins des ESP32, deshalb
  // wird der SPI-Bus ausdruecklich mit unseren Pins gestartet.
  sdSPI.begin(SD_SCK, SD_MISO, SD_MOSI, SD_CS);

  // 4000000 = 4 MHz Taktrate. Bewusst niedrig gewaehlt: Bei langen Leitungen
  // oder einfachen Modulen ist ein hoeherer Takt fehleranfaellig.
  if (SD.begin(SD_CS, sdSPI, 4000000)) {
    sdBereit = true;
    Serial.println("[OK] SD-Karte verbunden.");
  } else {
    Serial.println("[FEHLER] SD-Karte nicht gefunden!");
  }
}

void starteEthernet() {
  if (ethBereit) return;

  // Hinweis: Das Geraet verwendet die MAC-Adresse, die ab Werk fest im ESP32
  // gespeichert ist. Eine eigene MAC-Adresse ist nicht noetig und war in einer
  // frueheren Fassung auch nur scheinbar gesetzt - ETH.macAddress() ist naemlich
  // eine Lesefunktion und keine Schreibfunktion.

  if (ETH.begin(ETH_PHY_ADDR, ETH_PHY_POWER, ETH_PHY_MDC, ETH_PHY_MDIO,
                ETH_PHY_TYPE, ETH_CLK_MODE)) {

    // Feste IP-Adresse vergeben, statt sie per DHCP zuteilen zu lassen.
    // Ein Logger soll immer unter derselben Adresse erreichbar sein.
    ETH.config(local_IP, gateway, subnet, primaryDNS, secondaryDNS);
    ethBereit = true;

    Serial.print("[OK] Ethernet gestartet. IP-Adresse: ");
    Serial.println(ETH.localIP());

    // Uhrzeit per NTP holen.
    // Die beiden 0 bedeuten: keine feste Zeitverschiebung, keine Sommerzeit-
    // Regel. Beides wird stattdessen ueber die Zeitzone TZ gesetzt.
    configTime(0, 0, "pool.ntp.org", "time.nist.gov");

    // "CET-1CEST,M3.5.0,M10.5.0/3" ist die Zeitzonenregel fuer Deutschland:
    // Normalzeit CET (UTC+1), Sommerzeit CEST ab dem letzten Sonntag im Maerz
    // bis zum letzten Sonntag im Oktober. Der ESP32 rechnet die Umstellung
    // damit selbst aus, ganz ohne Netzwerkabfrage.
    setenv("TZ", "CET-1CEST,M3.5.0,M10.5.0/3", 1);
    tzset();
  } else {
    Serial.println("[FEHLER] Ethernet Init fehlgeschlagen!");
  }
}


///////////////////////////////////////////////////////////////////////////////
// 7. WEBSERVER: LIVE-ANSICHT
//
// Die Seite wird als reiner Text zusammengesetzt und an den Browser geschickt.
// Sie enthaelt keine externen Dateien, damit sie auch ohne Internetzugang
// vollstaendig funktioniert.
///////////////////////////////////////////////////////////////////////////////

// Erzeugt eine farbige Statusmeldung: gruen bei "in Ordnung", rot bei "Fehler".
String statusText(bool inOrdnung, const char* textOk, const char* textFehler) {
  if (inOrdnung) {
    return String("<b style='color:green'>") + textOk + "</b>";
  }
  return String("<b style='color:red'>") + textFehler + "</b>";
}

void seiteLive() {
  String html;

  // reserve() fordert den Speicher einmal in einem Stueck an. Ohne das muesste
  // der Speicher bei jedem += neu angefordert und umkopiert werden. Auf einem
  // Mikrocontroller mit wenig RAM fuehrt das mit der Zeit zu Luecken im
  // Speicher ("Fragmentierung") und irgendwann zu Abstuerzen.
  html.reserve(2500);

  html += "<!DOCTYPE html><html lang='de'><head><meta charset='UTF-8'>";

  // Laedt die Seite alle 5 Sekunden automatisch neu, passend zum Messintervall.
  // Ohne diese Zeile muesste man von Hand aktualisieren, obwohl "Live" drauf steht.
  html += "<meta http-equiv='refresh' content='5'>";
  html += "<title>PVA Musterhausen - Live</title>";
  html += "<style>body{font-family:Arial;padding:20px;}"
          "table{border-collapse:collapse;width:100%;max-width:800px;}"
          "th,td{border:1px solid #ccc;padding:10px;text-align:center;}"
          "th{background:#eee;}"
          ".status{margin-bottom:20px;padding:10px;background:#f9f9f9;border:1px solid #ddd;}"
          "</style></head><body>";

  html += "<h1>PVA Musterhausen - Live-Werte</h1>";

  // --- Systemstatus ---
  html += "<div class='status'><h3>System-Status:</h3>";
  html += "Netzwerk: "           + statusText(ETH.linkUp(), "Verbunden", "Getrennt") + "<br>";
  html += "SD-Karte: "           + statusText(sdBereit,     "Verbunden", "Fehler/Fehlt") + "<br>";
  html += "Modbus (Sensorik): "  + statusText(modbusOk,     "OK",        "Verbindungsfehler") + "<br>";
  html += "Uhrzeit (NTP): "      + statusText(zeitIstGueltig(), "Synchronisiert", "Nicht synchronisiert") + "<br>";
  html += "Letzte Messzeit: "    + formatiereZeit(aktuellerDatensatz.zeitstempel) + "<br>";
  html += "Paketnummer Sensor: " + String(aktuellerDatensatz.paketNr);
  html += "</div>";

  // --- Messwerte ---
  html += "<h3>Aktuelle Messwerte:</h3><table><tr>";
  for (uint8_t i = 0; i < KANAELE; i++) {
    html += "<th>Kanal " + String(i + 1) + "</th>";
  }
  html += "</tr><tr>";
  for (uint8_t i = 0; i < KANAELE; i++) {
    // Umrechnung von Milliampere in Ampere, auf zwei Nachkommastellen gerundet
    html += "<td>" + String(aktuellerDatensatz.daten.strom_mA[i] / MA_JE_AMPERE, 2) + " A</td>";
  }
  html += "</tr></table><br><br>";

  html += "<a href='/download'><button style='padding:10px 20px;font-size:16px;'>"
          "Logdaten als CSV herunterladen</button></a>";
  html += "</body></html>";

  server.send(200, "text/html", html);
}

// Antwort auf unbekannte Adressen, zum Beispiel die vom Browser automatisch
// angefragte /favicon.ico. Ohne diese Funktion laufen solche Anfragen ins Leere.
void seiteUnbekannt() {
  server.send(404, "text/plain", "Seite nicht gefunden. Startseite: /");
}


///////////////////////////////////////////////////////////////////////////////
// 8. WEBSERVER: CSV-DOWNLOAD
//
// Hier werden die 24-Byte-Datensaetze von der SD-Karte gelesen und in lesbare
// Textzeilen umgewandelt.
//
// Als Trennzeichen wird das Semikolon verwendet, weil die deutsche Version von
// Excel Kommas als Dezimaltrenner erwartet und CSV-Dateien deshalb ueblicherweise
// mit Semikolon aufbaut.
//
// WICHTIG FUER DIE GESCHWINDIGKEIT:
// Die Datei wird in Bloecken gelesen und die fertigen Textzeilen werden in einem
// Puffer gesammelt. Erst wenn der Puffer voll ist, geht ein Paket ans Netzwerk.
// Wuerde man jede einzelne Zeile sofort verschicken, entstuende pro Datensatz
// ein eigenes HTTP-Paket mit eigenem Kopf - bei 17280 Datensaetzen pro Tag waere
// der Download dadurch quaelend langsam.
///////////////////////////////////////////////////////////////////////////////

const size_t SENDE_PUFFER_GROESSE = 2048;  // Sammelpuffer fuer ausgehenden Text
const size_t ZEILEN_PUFFER_GROESSE = 256;  // Platz fuer eine einzelne CSV-Zeile

void seiteDownload() {
  if (!sdBereit) {
    server.send(500, "text/plain", "Fehler: Keine SD-Karte gefunden!");
    return;
  }

  File datei = SD.open(LOG_DATEI, FILE_READ);
  if (!datei) {
    server.send(500, "text/plain", "Fehler: Log-Datei nicht gefunden!");
    return;
  }

  // CONTENT_LENGTH_UNKNOWN: Die Groesse der fertigen CSV-Datei ist vorher nicht
  // bekannt, weil erst beim Umwandeln feststeht, wie lang der Text wird.
  // Der Server nutzt dann "chunked transfer encoding" und schickt die Daten
  // stueckweise. Siehe https://developer.mozilla.org/de/docs/Web/HTTP/Headers/Transfer-Encoding
  server.setContentLength(CONTENT_LENGTH_UNKNOWN);

  // Content-Disposition sorgt dafuer, dass der Browser die Datei speichert,
  // statt sie im Fenster anzuzeigen.
  server.sendHeader("Content-Disposition", "attachment; filename=\"pva_logdaten.csv\"");
  server.send(200, "text/csv", "");

  // Kopfzeile mit den Spaltennamen
  String kopfzeile = "Zeitstempel;Paket;GAK_ID";
  for (uint8_t i = 0; i < KANAELE; i++) {
    kopfzeile += ";Kanal_" + String(i + 1) + "_A";
  }
  kopfzeile += "\n";
  server.sendContent(kopfzeile);

  Datensatz datensatz;
  char zeile[ZEILEN_PUFFER_GROESSE];
  char puffer[SENDE_PUFFER_GROESSE];
  size_t pufferFuellstand = 0;
  uint32_t anzahlDatensaetze = 0;

  while (true) {
    // Rueckgabewert pruefen: Wenn die Datei zum Beispiel durch einen
    // Stromausfall mitten im Schreiben abgeschnitten wurde, liefert read()
    // weniger Bytes als angefordert. Ein solcher halber Datensatz waere
    // Datenmuell und wird deshalb verworfen.
    int gelesen = datei.read((uint8_t*)&datensatz, sizeof(Datensatz));
    if (gelesen != (int)sizeof(Datensatz)) {
      break;
    }

    // Eine Textzeile zusammensetzen.
    // snprintf schreibt hoechstens so viele Zeichen, wie in den Puffer passen,
    // und verhindert damit ein Ueberschreiben des Speichers.
    int laenge = snprintf(zeile, sizeof(zeile), "%s;%u;%u",
                          formatiereZeit(datensatz.zeitstempel).c_str(),
                          datensatz.paketNr,
                          datensatz.daten.gakID);

    for (uint8_t i = 0; i < KANAELE && laenge > 0 && laenge < (int)sizeof(zeile); i++) {
      laenge += snprintf(zeile + laenge, sizeof(zeile) - laenge, ";%.2f",
                         datensatz.daten.strom_mA[i] / MA_JE_AMPERE);
    }

    if (laenge < 0 || laenge >= (int)sizeof(zeile) - 1) {
      continue;  // Zeile passte nicht in den Puffer, sollte nie vorkommen
    }
    zeile[laenge++] = '\n';

    // Passt die Zeile nicht mehr in den Sammelpuffer, wird dieser zuerst
    // abgeschickt und danach von vorn gefuellt.
    if (pufferFuellstand + laenge > SENDE_PUFFER_GROESSE) {
      server.sendContent(puffer, pufferFuellstand);
      pufferFuellstand = 0;
    }
    memcpy(puffer + pufferFuellstand, zeile, laenge);
    pufferFuellstand += laenge;
    anzahlDatensaetze++;
  }

  // Rest aus dem Sammelpuffer nachschicken
  if (pufferFuellstand > 0) {
    server.sendContent(puffer, pufferFuellstand);
  }

  datei.close();

  // Ein leeres Stueck signalisiert dem Browser das Ende der Uebertragung.
  server.sendContent("");

  Serial.printf("[INFO] CSV-Download beendet: %u Datensaetze uebertragen.\n",
                anzahlDatensaetze);
}


///////////////////////////////////////////////////////////////////////////////
// 9. DATENERFASSUNG UND SPEICHERUNG
///////////////////////////////////////////////////////////////////////////////

// Fragt den Sensor ueber Modbus ab und traegt die Antwort in den Datensatz ein.
// Rueckgabe: true, wenn die Abfrage erfolgreich war.
bool leseSensor(Datensatz &ziel) {
  // readHoldingRegisters entspricht dem Modbus-Funktionscode 3.
  // Der Master schickt eine Anfrage und wartet auf die Antwort des Slaves.
  uint8_t ergebnis = node.readHoldingRegisters(MODBUS_START_REG, MODBUS_REG_ANZAHL);

  if (ergebnis != node.ku8MBSuccess) {
    // Haeufigste Ursachen: Kabel ab, Sensor stromlos, falsche Slave-Adresse,
    // A und B der RS485-Leitung vertauscht, oder abweichende Baudrate.
    modbusOk = false;
    return false;
  }

  // Die Antwort liegt in einem Zwischenspeicher der Bibliothek.
  // Index 0 ist das erste angeforderte Register, hier also Register 10.
  ziel.paketNr    = node.getResponseBuffer(0);
  ziel.daten.gakID = node.getResponseBuffer(1);

  for (uint8_t i = 0; i < KANAELE; i++) {
    // Modbus uebertraegt 16 Bit ohne Vorzeichen. Der Strom kann aber negativ
    // sein (Rueckspeisung). Die Umwandlung nach int16_t stellt das Vorzeichen
    // wieder her: aus 65535 wird dadurch wieder -1.
    ziel.daten.strom_mA[i] = (int16_t)node.getResponseBuffer(2 + i);
  }

  modbusOk = true;
  return true;
}

// Haengt einen Datensatz hinten an die Logdatei an.
void speichereAufSD(const Datensatz &datensatz) {
  if (!sdBereit) return;

  // FILE_APPEND oeffnet die Datei und schreibt ans Ende, ohne den bisherigen
  // Inhalt zu laden. Die Datei wird nach jedem Schreiben wieder geschlossen.
  // Das kostet etwas Zeit, sorgt aber dafuer, dass bei einem Stromausfall
  // hoechstens der letzte Datensatz fehlt und nicht die ganze Datei beschaedigt ist.
  File datei = SD.open(LOG_DATEI, FILE_APPEND);
  if (!datei) {
    Serial.println("[FEHLER] Konnte nicht auf SD schreiben! SD wird deaktiviert.");
    sdBereit = false;  // erzwingt einen neuen Versuch im Fehler-Manager
    return;
  }

  datei.write((const uint8_t*)&datensatz, sizeof(Datensatz));
  datei.close();
}


///////////////////////////////////////////////////////////////////////////////
// 10. RUECKRUF WAEHREND DER MODBUS-WARTEZEIT
//
// Antwortet der Sensor nicht, wartet die Bibliothek bis zu 2 Sekunden auf eine
// Antwort. Ohne Gegenmassnahme stuende in dieser Zeit das ganze Programm still
// und die Webseite waere genau dann nicht erreichbar, wenn man wegen der
// Stoerung nachschauen will.
//
// ModbusMaster bietet dafuer die Funktion idle(): Der uebergebene Ruf wird
// waehrend des Wartens wiederholt aufgerufen. Wir nutzen ihn, um weiterhin
// Browser-Anfragen zu beantworten.
///////////////////////////////////////////////////////////////////////////////
void waehrendModbusWartezeit() {
  if (ethBereit) {
    server.handleClient();
  }
}


///////////////////////////////////////////////////////////////////////////////
// 11. SETUP - wird einmal beim Einschalten ausgefuehrt
///////////////////////////////////////////////////////////////////////////////
void setup() {
  Serial.begin(115200);
  delay(1000);  // kurze Pause, damit die serielle Ausgabe von Anfang an mitliest

  Serial.println("\n\n--- Technikerprojekt 2026: Datenlogger startet ---");
  Serial.printf("Groesse eines Datensatzes: %u Byte\n", (unsigned)sizeof(Datensatz));

  // Netzwerk und SD-Karte starten. Schlaegt etwas fehl, laeuft das Programm
  // trotzdem weiter - der Fehler-Manager in loop() versucht es erneut.
  starteEthernet();
  starteSD();

  // --- Modbus vorbereiten ---
  // SERIAL_8N1 bedeutet: 8 Datenbits, keine Paritaet (None), 1 Stoppbit.
  // Das ist die uebliche Einstellung bei Modbus RTU und muss auf beiden
  // Seiten identisch sein, sonst kommt nur Zeichensalat an.
  Serial2.begin(MODBUS_BAUDRATE, SERIAL_8N1, RXD2, TXD2);
  node.begin(MODBUS_SLAVE_ID, Serial2);
  node.idle(waehrendModbusWartezeit);
  Serial.println("[OK] Modbus-Master bereit (9600 Baud, 8N1).");

  // --- Webserver vorbereiten ---
  // server.on() verknuepft eine Adresse mit der Funktion, die sie beantwortet.
  server.on("/", HTTP_GET, seiteLive);
  server.on("/download", HTTP_GET, seiteDownload);
  server.onNotFound(seiteUnbekannt);
  server.begin();
  Serial.println("[OK] Webserver gestartet auf Port 80.");
}


///////////////////////////////////////////////////////////////////////////////
// 12. LOOP - wird danach endlos wiederholt
//
// Die Schleife darf nie stehen bleiben. Jede Aufgabe prueft selbst, ob sie an
// der Reihe ist, und kehrt sonst sofort zurueck.
///////////////////////////////////////////////////////////////////////////////
void loop() {
  // --- Aufgabe 1: Browser-Anfragen beantworten ---
  if (ethBereit) {
    server.handleClient();
  }

  unsigned long jetzt = millis();

  // --- Aufgabe 2: Fehler-Manager ---
  // Alle 10 Sekunden wird versucht, ausgefallene Baugruppen neu zu starten.
  // So erholt sich das Geraet selbststaendig, etwa wenn die SD-Karte im Betrieb
  // gezogen und wieder gesteckt wurde.
  if (jetzt - letzterRetry >= INTERVALL_RETRY) {
    letzterRetry = jetzt;
    if (!sdBereit)  starteSD();
    if (!ethBereit) starteEthernet();
  }

  // --- Aufgabe 3: Messen und speichern ---
  // Der Vergleich "jetzt - letzteMessung" statt "jetzt >= naechsteMessung"
  // funktioniert auch dann korrekt, wenn millis() nach rund 49 Tagen
  // Dauerbetrieb wieder bei 0 beginnt (sogenannter Ueberlauf).
  if (jetzt - letzteMessung >= INTERVALL_MESSUNG) {
    letzteMessung = jetzt;

    Datensatz neuerDatensatz = {};  // alle Felder mit 0 vorbelegen

    if (leseSensor(neuerDatensatz)) {
      neuerDatensatz.zeitstempel = holeZeitstempel();

      // Fuer die Live-Anzeige merken ...
      aktuellerDatensatz = neuerDatensatz;

      // ... und dauerhaft auf der SD-Karte ablegen.
      // Hinweis: Es wird auch dann gespeichert, wenn die Uhr noch nicht per NTP
      // gestellt ist. Solche Datensaetze erscheinen in der CSV-Datei mit dem
      // Vermerk "Keine_Netzwerkzeit". Das ist bewusst so: Ein Messwert ohne
      // Uhrzeit ist immer noch wertvoller als gar kein Messwert.
      speichereAufSD(neuerDatensatz);
    } else {
      Serial.println("[FEHLER] Keine Antwort vom Sensor (Modbus).");
    }
  }
}
