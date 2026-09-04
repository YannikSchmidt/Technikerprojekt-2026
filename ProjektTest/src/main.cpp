#include <ETH.h>
#include <WebServer.h>

// --- Netzwerkdaten ---
// Die Adressen dieses Netzes stehen in include/network_config.h. Diese Datei
// liegt bewusst nicht im Repository. Zum Bauen einmalig die Vorlage
// include/network_config.example.h nach include/network_config.h kopieren
// und die eigenen Werte eintragen.
#include "network_config.h"
 
// WT32-ETH01 spezifische Pin-Konfiguration (Namen exakt passend zum Befehl unten)
#define ETH_PHY_ADDR    1
#define ETH_PHY_POWER   16
#define ETH_PHY_MDC     23
#define ETH_PHY_MDIO    18
#define ETH_PHY_TYPE    ETH_PHY_LAN8720
#define ETH_CLK_MODE    ETH_CLOCK_GPIO0_IN  // WICHTIG: Muss GPIO0_IN sein beim WT32-ETH01!
 
uint8_t mac[] = {0xDE, 0xAD, 0xBE, 0xEE, 0xFE, 0xEE};
 
WebServer server(80);

void handleRoot()
{
  String html = "<html><head><meta charset='utf-8'><title>WT32-ETH01</title></head><body>";
  html += "<h1>WT32-ETH01 WebServer</h1>";
  html += "<p>Ethernet connected successfully.</p>";
  html += "<p>IP: " + local_IP.toString() + "</p>";
  html += "</body></html>";
  server.send(200, "text/html", html);
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

  server.on("/", HTTP_GET, handleRoot);
  server.begin();

  Serial.println("WebServer started on port 80");
  Serial.print("on IP Address: ");
  Serial.println(ETH.localIP());
}
 
void loop() {
  server.handleClient();
  delay(100);
}
