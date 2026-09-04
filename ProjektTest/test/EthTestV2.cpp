#include <ETH.h>
IPAddress local_IP(192, 168,  1, 50);   // Static IP Address
IPAddress gateway(192, 168, 1, 1);       // Gateway
IPAddress subnet(255, 255, 255, 0);      // Subnet Mask
IPAddress primaryDNS(8, 8, 8, 8);        // Primary DNS
IPAddress secondaryDNS(8, 8, 4, 4);      // Secondary DNS
#define ETH_PHY_TYPE ETH_PHY_LAN8720
//#define ETH_PHY_ADDR 0      // commented out
//eth_phy_type_t ETH_PHY_LAN8720;
#define ETH_ADDR        1
#define ETH_POWER_PIN   16
// Pin# of the I²C clock signal for the Ethernet PHY
#define ETH_MDC_PIN     23
#define ETH_TYPE        ETH_PHY_LAN8720
// Pin# of the I²C IO signal for the Ethernet PHY
#define ETH_MDIO_PIN    18
//#define ETH_PHY_POWER -1    // commented out
#define ETH_CLK_MODE    ETH_CLOCK_GPIO17_OUT
uint8_t mac[] = {0xDE, 0xAD, 0xBE, 0xEE, 0xFE, 0xEE}; // use any MAC
void setup() {
  Serial.begin(115200);
  delay(1000);
  // pinMode(16, OUTPUT);     // set pin to output
  //digitalWrite(16, HIGH);  // turn on power
  ETH.begin(ETH_PHY_ADDR, ETH_PHY_POWER, ETH_PHY_MDC, ETH_PHY_MDIO, ETH_PHY_TYPE, ETH_CLK_MODE);
  ETH.macAddress(mac);
  delay(100);
  ETH.begin();  // Start Ethernet
  ETH.config(local_IP, gateway, subnet, primaryDNS);;
  Serial.print("IP Address: ");
  Serial.println(ETH.localIP());
}

void loop() {
delay(100);
}