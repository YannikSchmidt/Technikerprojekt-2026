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
unsigned long lastModbusRead = 0; // Für das non-blocking delay
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
 
// Hilfsfunktion: Wandelt eine Zeile aus der Textdatei direkt in eine HTML-Tabellenzeile um
String parseLineToHTML(String line) {
  unsigned int rtc, gak;
  int v1, v2, v3, v4, v5, v6, v7, v8;
  // Versucht die Zeile anhand eures Musters zu zerlegen
  if (sscanf(line.c_str(), "(Paket)RTC:%u;GAK:%u;1:%d;2:%d;3:%d;4:%d;5:%d;6:%d;7:%d;8:%d",
&rtc, &gak, &v1, &v2, &v3, &v4, &v5, &v6, &v7, &v8) == 10) {
      char buffer[512];
      snprintf(buffer, sizeof(buffer),
               "<tr><td>%u</td><td>%u</td><td>%d mA</td><td>%d mA</td><td>%d mA</td><td>%d mA</td><td>%d mA</td><td>%d mA</td><td>%d mA</td><td>%d mA</td></tr>\n",
               rtc, gak, v1, v2, v3, v4, v5, v6, v7, v8);
      return String(buffer);
  }
  // Falls die Zeile mal nicht dem Muster entspricht (z.B. Leerzeile), einfach als ganzen Text ausgeben
  return ""; 
}
 
// Baut das Web-Interface zusammen und sendet es Stück für Stück (Chunked)
void handleRoot()
{
  // Wichtig für große Dateien: Dem Browser sagen, dass die Daten gestückelt kommen
  server.setContentLength(CONTENT_LENGTH_UNKNOWN);
  server.send(200, "text/html", "");
 
  // --- HTML HEADER & CSS STYLING ---
  String html = "<!DOCTYPE html><html lang='de'><head><meta charset='UTF-8'>";
  html += "<meta name='viewport' content='width=device-width, initial-scale=1.0'>";
  html += "<title>PVA Musterhausen - Dashboard</title>";
  html += "<style>";
  // Modernes CSS Design
  html += ":root { --primary: #005288; --secondary: #009FE3; --bg: #f4f7f6; --text: #333; }";
  html += "body { font-family: 'Segoe UI', Roboto, Helvetica, Arial, sans-serif; background-color: var(--bg); color: var(--text); margin: 0; padding: 20px; }";
  html += ".container { max-width: 1200px; margin: 0 auto; background: #fff; padding: 25px; border-radius: 12px; box-shadow: 0 10px 20px rgba(0,0,0,0.08); }";
  html += ".header { background: linear-gradient(135deg, var(--primary), var(--secondary)); color: white; padding: 25px; border-radius: 8px; text-align: center; margin-bottom: 25px; box-shadow: 0 4px 6px rgba(0,0,0,0.1); }";
  html += ".header h1 { margin: 0; font-size: 32px; letter-spacing: 1px; }";
  html += ".header p { margin: 10px 0 0 0; font-size: 16px; opacity: 0.9; }";
  html += ".info-bar { display: flex; justify-content: space-between; background: #e9ecef; padding: 15px 20px; border-radius: 8px; margin-bottom: 20px; font-weight: 600; border-left: 5px solid var(--primary); }";
  html += ".btn-refresh { display: inline-block; padding: 10px 20px; background-color: var(--primary); color: white; text-decoration: none; border-radius: 6px; font-weight: bold; transition: 0.3s; margin-bottom: 15px; }";
  html += ".btn-refresh:hover { background-color: var(--secondary); transform: translateY(-2px); }";
  // Tabellen-Design
  html += ".table-container { overflow-x: auto; max-height: 600px; overflow-y: auto; border: 1px solid #dee2e6; border-radius: 8px; }";
  html += "table { width: 100%; border-collapse: collapse; text-align: center; white-space: nowrap; }";
  html += "th { background-color: var(--primary); color: white; position: sticky; top: 0; padding: 16px; font-size: 15px; }";
  html += "td { padding: 14px; border-bottom: 1px solid #e9ecef; font-size: 14px; }";
  html += "tr:nth-child(even) { background-color: #f8f9fa; }";
  html += "tr:hover { background-color: #e2e6ea; }";
  // Responsive Anpassung für Handys
  html += "@media (max-width: 768px) { .info-bar { flex-direction: column; gap: 10px; text-align: center; } }";
  html += "</style></head><body>";
  // --- HTML BODY ---
  html += "<div class='container'>";
  html += "<div class='header'>";
  html += "<h1>⚡ Strangströme PVA Musterhausen</h1>";
  html += "<p>Technikerprojekt 2026 von Danny, Jakob, Florian und Yannik</p>";
  html += "</div>";
  html += "<div class='info-bar'>";
  html += "<span>IP-Adresse: " + local_IP.toString() + "</span>";
  html += "<span>Status: Verbunden & Aufzeichnend 🟢</span>";
  html += "</div>";
  html += "<a href='/' class='btn-refresh'>🔄 Daten aktualisieren</a>";
  html += "<div class='table-container'>";
  html += "<table>";
  html += "<thead><tr>";
  html += "<th>Paket / RTC</th><th>GAK ID</th>";
  html += "<th>Strang 1</th><th>Strang 2</th><th>Strang 3</th><th>Strang 4</th>";
  html += "<th>Strang 5</th><th>Strang 6</th><th>Strang 7</th><th>Strang 8</th>";
  html += "</tr></thead><tbody>";
  // Sende den ersten HTML-Block an den Browser
  server.sendContent(html);
 
  // --- DATEI LESEN UND PARSEN (Speicherschonend!) ---
  File file = SD.open("/test_log.txt", FILE_READ);
  if (file) {
    String currentLine = "";
    while (file.available()) {
      char c = (char)file.read();
      if (c == '\n') {
        // Zeile ist komplett -> Als Tabellenzeile parsen und sofort senden
        if(currentLine.length() > 5) {
           server.sendContent(parseLineToHTML(currentLine));
        }
        currentLine = ""; // String leeren für nächste Zeile
      } else {
        currentLine += c;
      }
    }
    // Falls die letzte Zeile keinen Zeilenumbruch hat
    if(currentLine.length() > 5) {
        server.sendContent(parseLineToHTML(currentLine));
    }
    file.close();
  } else {
    server.sendContent("<tr><td colspan='10' style='color:red; font-weight:bold;'>FEHLER: Konnte Log-Datei auf SD-Karte nicht lesen!</td></tr>");
  }
 
  // --- HTML FOOTER ---
  html = "</tbody></table></div>";
  html += "<p style='text-align:center; color:#777; margin-top:20px; font-size:12px;'>Generiert vom WT32-ETH01 Modbus-Master</p>";
  html += "</div></body></html>";
  server.sendContent(html);
  // Dem Browser mitteilen, dass die Übertragung beendet ist
  server.sendContent(""); 
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
    Serial.println("[OK] Daten erfolgreich geschrieben.");
  } else {
    Serial.println("[FEHLER] Dateizugriff fehlgeschlagen!");
  }
}
bool readModBus(int address, struct logData &data) {
  Serial.println("Lese Holding Register...");
  // WICHTIGE KORREKTUR: 10 Register lesen, da wir Index 0 bis 9 abrufen!
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
  // 1. Eigene MAC-Adresse setzen
  ETH.macAddress(mac);
  // 2. Ethernet mit den exakten Pins starten
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
  // 1. Webserver regelmäßig abfragen (Sorgt für schnelle Ladezeiten!)
  server.handleClient();
  // 2. "Non-Blocking Delay": Führt den Modbus-Code nur alle 5000ms aus, 
  // blockiert den ESP32 aber in der Zwischenzeit nicht!
  if (millis() - lastModbusRead >= 5000) {
    lastModbusRead = millis(); // Timer zurücksetzen
    logData curData = {0};
    getTime();
    curData.realTimeStamp = g_Time;
    if(readModBus(10, curData)){
      sdWrite("/test_log.txt", curData);
    }
  }
}