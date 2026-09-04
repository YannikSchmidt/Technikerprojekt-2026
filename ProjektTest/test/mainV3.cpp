#include <ETH.h>
 
// Deine Netzwerkdaten (Bitte die echten Zahlen eintragen)
IPAddress local_IP(192, 168,  1, 50);    // Static IP Address
IPAddress gateway(192, 168,  1, 1);      // Gateway
IPAddress subnet(255, 255, 255, 0);     // Subnet Mask
IPAddress primaryDNS(8, 8, 8, 8);       // Primary DNS
IPAddress secondaryDNS(8, 8, 4, 4);     // Secondary DNS
 
// WT32-ETH01 spezifische Pin-Konfiguration (Namen exakt passend zum Befehl unten)
#define ETH_PHY_ADDR    1
#define ETH_PHY_POWER   16
#define ETH_PHY_MDC     23
#define ETH_PHY_MDIO    18
#define ETH_PHY_TYPE    ETH_PHY_LAN8720
#define ETH_CLK_MODE    ETH_CLOCK_GPIO0_IN  // WICHTIG: Muss GPIO0_IN sein beim WT32-ETH01!
 
uint8_t mac[] = {0xDE, 0xAD, 0xBE, 0xEE, 0xFE, 0xEE};
 
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
}
 
void loop() {
  Serial.print("IP Address: ");
  Serial.println(ETH.localIP());
  delay(5000);
}