#include <SPI.h>
#include <SD.h>
#include <ModbusMaster.h>
#include <ETH.h>
#include <WebServer.h>
#include <time.h>
 
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
#define ETH_CLK_MODE    ETH_CLOCK_GPIO0_IN  
 
// Deine Netzwerkdaten
IPAddress local_IP(192, 168,  1, 50);    
IPAddress gateway(192, 168,  1, 1);      
IPAddress subnet(255, 255, 255, 0);       
IPAddress primaryDNS(8, 8, 8, 8);         
IPAddress secondaryDNS(8, 8, 4, 4);       
uint8_t mac[] = {0xDE, 0xAD, 0xBE, 0xEE, 0xFE, 0xEE};
uint16_t g_paketNr = 0;
unsigned long lastModbusRead = 0;
 
// Modbus-Instanz initialisieren
ModbusMaster node;
WebServer server(80);
SPIClass sdSPI(VSPI);
 
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
// Funktion um die echte Uhrzeit abzufragen
String getTimestamp() {
  struct tm timeinfo;
  if (!getLocalTime(&timeinfo)) {
    return "Keine_Zeit_verfuegbar"; 
  }
  char timeStringBuff[25];
  strftime(timeStringBuff, sizeof(timeStringBuff), "%Y-%m-%d_%H:%M:%S", &timeinfo);
  return String(timeStringBuff);
}
 
// Hilfsfunktion für Web-UI: Wandelt Zeile in Tabellenzeile um
String parseLineToHTML(String line) {
  char timeStr[25];
  unsigned int rtc, gak;
  int v1, v2, v3, v4, v5, v6, v7, v8;
  if (sscanf(line.c_str(), "Zeit:%24[^;];Paket:%u;GAK:%u;1:%d;2:%d;3:%d;4:%d;5:%d;6:%d;7:%d;8:%d",
      timeStr, &rtc, &gak, &v1, &v2, &v3, &v4, &v5, &v6, &v7, &v8) == 11) {
      String prettyTime = String(timeStr);
      prettyTime.replace("_", " ");
      char buffer[512];
      snprintf(buffer, sizeof(buffer),
               "<tr><td>%s</td><td>%u</td><td>%u</td><td>%d</td><td>%d</td><td>%d</td><td>%d</td><td>%d</td><td>%d</td><td>%d</td><td>%d</td></tr>\n",
               prettyTime.c_str(), rtc, gak, v1, v2, v3, v4, v5, v6, v7, v8);
      return String(buffer);
  }
  else if (sscanf(line.c_str(), "(Paket)RTC:%u;GAK:%u;1:%d;2:%d;3:%d;4:%d;5:%d;6:%d;7:%d;8:%d",
&rtc, &gak, &v1, &v2, &v3, &v4, &v5, &v6, &v7, &v8) == 10) {
      char buffer[512];
      snprintf(buffer, sizeof(buffer),
               "<tr><td>---</td><td>%u</td><td>%u</td><td>%d</td><td>%d</td><td>%d</td><td>%d</td><td>%d</td><td>%d</td><td>%d</td><td>%d</td></tr>\n",
               rtc, gak, v1, v2, v3, v4, v5, v6, v7, v8);
      return String(buffer);
  }
  return ""; 
}
// Baut das Web-Interface auf
void handleRoot()
{
  server.setContentLength(CONTENT_LENGTH_UNKNOWN);
  server.send(200, "text/html", "");
  String html = "<!DOCTYPE html><html><head><meta charset='UTF-8'>";
  html += "<meta name='viewport' content='width=device-width, initial-scale=1.0'>";
  html += "<title>PVA Musterhausen</title>";
  html += "<style>";
  html += "body { font-family: sans-serif; margin: 15px; }";
  html += "table { border-collapse: collapse; width: 100%; font-size: 14px; }";
  html += "th, td { border: 1px solid #ccc; padding: 6px; text-align: center; }";
  html += "th { background-color: #e0e0e0; position: sticky; top: 0; z-index: 2; }"; /* Header bleibt beim Scrollen fixiert */
  html += ".btn { display: inline-block; padding: 8px 12px; margin-right: 10px; background: #ddd; color: #000; text-decoration: none; border: 1px solid #aaa; border-radius: 4px; }";
  // Neu: Style für die aktuellste Messung und den Tabellen-Kasten
  html += ".latest-box { background: #e8f5e9; border: 2px solid #4caf50; padding: 12px; margin: 15px 0; border-radius: 6px; font-size: 15px; }";
  html += ".table-container { max-height: 60vh; overflow-y: auto; border: 1px solid #ccc; margin-top: 15px; }"; 
  html += "</style></head><body>";
  html += "<h2 style='margin-bottom: 5px;'>PVA Musterhausen</h2>";
  String timeCheck = getTimestamp();
  html += "<p style='margin-top: 0; color: #555;'>IP: " + local_IP.toString() + " | Zeit: " + (timeCheck == "Keine_Zeit_verfuegbar" ? "NTP Fehler" : timeCheck) + " | Status: OK</p>";
  // Extra-Box für die aktuellsten Daten, die später per JavaScript gefüllt wird
  html += "<div id='latestData' class='latest-box'>Lade aktuellste Messwerte...</div>";
  html += "<div>";
  html += "<a href='/' class='btn'>Aktualisieren</a>";
  html += "<a href='/download' class='btn'>CSV Herunterladen</a>";
  html += "</div>";
  // Die Tabelle steckt jetzt im scrollbaren div
  html += "<div class='table-container'>";
  html += "<table><thead><tr>";
  html += "<th>Zeitstempel</th><th>Paket</th><th>GAK ID</th>";
  html += "<th>S1 (mA)</th><th>S2 (mA)</th><th>S3 (mA)</th><th>S4 (mA)</th>";
  html += "<th>S5 (mA)</th><th>S6 (mA)</th><th>S7 (mA)</th><th>S8 (mA)</th>";
  html += "</tr></thead><tbody id='dataTbody'>";
  server.sendContent(html);
  File file = SD.open("/test_log.txt", FILE_READ);
  if (file) {
    String currentLine = "";
    while (file.available()) {
      char c = (char)file.read();
      if (c == '\n') {
        if(currentLine.length() > 5) server.sendContent(parseLineToHTML(currentLine));
        currentLine = "";
      } else {
        currentLine += c;
      }
    }
    if(currentLine.length() > 5) server.sendContent(parseLineToHTML(currentLine));
    file.close();
  } else {
    server.sendContent("<tr><td colspan='11'>Fehler: Konnte Log-Datei nicht lesen!</td></tr>");
  }
  // Tabellenende
  html = "</tbody></table></div>";
  // JavaScript: Dreht die Tabelle um UND schreibt den neusten Wert in die Extra-Box oben!
  html += "<script>";
  html += "var tb = document.getElementById('dataTbody');";
  html += "if(tb.children.length > 0 && tb.children[0].children[0].innerText !== 'Fehler: Konnte Log-Datei nicht lesen!') {";
  html += "  for(var i = tb.children.length - 1; i >= 0; i--) { tb.appendChild(tb.children[i]); }"; // Tabelle umdrehen
  html += "  var first = tb.children[0];"; // Das ist jetzt der neuste Wert
  html += "  var out = '<strong>Letzte Messung (' + first.cells[0].innerText + '):</strong><br>';";
  html += "  out += 'Strang 1-4: ' + first.cells[3].innerText + ' mA, ' + first.cells[4].innerText + ' mA, ' + first.cells[5].innerText + ' mA, ' + first.cells[6].innerText + ' mA <br>';";
  html += "  out += 'Strang 5-8: ' + first.cells[7].innerText + ' mA, ' + first.cells[8].innerText + ' mA, ' + first.cells[9].innerText + ' mA, ' + first.cells[10].innerText + ' mA';";
  html += "  document.getElementById('latestData').innerHTML = out;";
  html += "} else {";
  html += "  document.getElementById('latestData').innerHTML = 'Keine Daten auf der SD-Karte gefunden.';";
  html += "}";
  html += "</script>";
  html += "</body></html>";
  server.sendContent(html);
  server.sendContent(""); 
}
// Handler für den CSV Download
void handleDownload() {
  File file = SD.open("/test_log.txt", FILE_READ);
  if (!file) {
    server.send(500, "text/plain", "Fehler: Log-Datei auf SD-Karte nicht gefunden!");
    return;
  }
  server.setContentLength(CONTENT_LENGTH_UNKNOWN);
  server.sendHeader("Content-Disposition", "attachment; filename=\"pva_logdaten.csv\"");
  server.send(200, "text/csv", "");
  server.sendContent("Zeitstempel;Paket;GAK_ID;Strang_1_mA;Strang_2_mA;Strang_3_mA;Strang_4_mA;Strang_5_mA;Strang_6_mA;Strang_7_mA;Strang_8_mA\n");
  String currentLine = "";
  char timeStr[25];
  unsigned int rtc, gak;
  int v1, v2, v3, v4, v5, v6, v7, v8;
  char buffer[256];
  while (file.available()) {
    char c = (char)file.read();
    if (c == '\n') {
      if(currentLine.length() > 5) {
         if (sscanf(currentLine.c_str(), "Zeit:%24[^;];Paket:%u;GAK:%u;1:%d;2:%d;3:%d;4:%d;5:%d;6:%d;7:%d;8:%d",
             timeStr, &rtc, &gak, &v1, &v2, &v3, &v4, &v5, &v6, &v7, &v8) == 11) {
             String cleanTime = String(timeStr);
             cleanTime.replace("_", " ");
             snprintf(buffer, sizeof(buffer), "%s;%u;%u;%d;%d;%d;%d;%d;%d;%d;%d\n",
                      cleanTime.c_str(), rtc, gak, v1, v2, v3, v4, v5, v6, v7, v8);
             server.sendContent(String(buffer));
         }
         else if (sscanf(currentLine.c_str(), "(Paket)RTC:%u;GAK:%u;1:%d;2:%d;3:%d;4:%d;5:%d;6:%d;7:%d;8:%d",
&rtc, &gak, &v1, &v2, &v3, &v4, &v5, &v6, &v7, &v8) == 10) {
             snprintf(buffer, sizeof(buffer), "---;%u;%u;%d;%d;%d;%d;%d;%d;%d;%d\n",
                      rtc, gak, v1, v2, v3, v4, v5, v6, v7, v8);
             server.sendContent(String(buffer));
         }
      }
      currentLine = "";
    } else {
      currentLine += c;
    }
  }
  file.close();
  server.sendContent(""); 
}
 
void sdWrite(const char* filepath, const struct logData &data) {
  File file = SD.open(filepath, FILE_APPEND);
  if (file) {
    file.printf("Zeit:%s;", getTimestamp().c_str());
    file.printf("Paket:%u;", g_paketNr);
    file.printf("GAK:%u;", data.gakID);
    file.printf("1:%d;", data.value1);
    file.printf("2:%d;", data.value2);
    file.printf("3:%d;", data.value3);
    file.printf("4:%d;", data.value4);
    file.printf("5:%d;", data.value5);
    file.printf("6:%d;", data.value6);
    file.printf("7:%d;", data.value7);
    file.printf("8:%d\n", data.value8);
    file.close(); 
    Serial.println("[OK] Daten erfolgreich geschrieben.");
  } else {
    Serial.println("[FEHLER] Dateizugriff fehlgeschlagen!");
  }
}
 
bool readModBus(int address, struct logData &data) {
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
    return 1;
  } else {
    Serial.print("Fehler beim Lesen! Fehlercode: 0x");
    Serial.println(result, HEX);
    return 0;
  }
}
 
void setup() {
  Serial.begin(115200);
  delay(1000);
  ETH.macAddress(mac);
  if (!ETH.begin(ETH_PHY_ADDR, ETH_PHY_POWER, ETH_PHY_MDC, ETH_PHY_MDIO, ETH_PHY_TYPE, ETH_CLK_MODE)) {
    Serial.println("Fehler beim Starten des Ethernet-Moduls!");
  }
  ETH.config(local_IP, gateway, subnet, primaryDNS, secondaryDNS);
 
  Serial.println("Synchronisiere Uhrzeit über NTP...");
  configTime(0, 0, "pool.ntp.org", "time.nist.gov");
  setenv("TZ", "CET-1CEST,M3.5.0,M10.5.0/3", 1); 
  tzset();
  sdSPI.begin(SD_SCK, SD_MISO, SD_MOSI, SD_CS);
  if (!SD.begin(SD_CS, sdSPI, 4000000)) {
    Serial.println("[FEHLER] SD-Karte konnte nicht initialisiert werden!");
    return;
  }
  Serial.println("[OK] SD-Karte erfolgreich initialisiert.");
  Serial2.begin(9600, SERIAL_8N1, RXD2, TXD2);
  node.begin(1, Serial2);
  server.on("/", HTTP_GET, handleRoot);
  server.on("/download", HTTP_GET, handleDownload); 
  server.begin();
  Serial.println("WebServer started on port 80");
  Serial.print("on IP Address: ");
  Serial.println(ETH.localIP());
}
 
void loop() {
  server.handleClient();
  if (millis() - lastModbusRead >= 5000) {
    lastModbusRead = millis(); 
    logData curData = {0};
    if(readModBus(10, curData)){
      sdWrite("/test_log.txt", curData);
    }
  }
}