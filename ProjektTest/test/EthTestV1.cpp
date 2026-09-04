#include <Arduino.h>
#include <ETH.h>
#include <WebServer.h>

#define ETH_PHY_ADDR 1
#define ETH_PHY_POWER 16
#define ETH_PHY_MDC 23
#define ETH_PHY_MDIO 18
#define ETH_CLK_MODE ETH_CLOCK_GPIO0_IN
#define ETH_PHY_TYPE ETH_PHY_LAN8720

static const uint8_t macAddr[] = {0xDE, 0xAD, 0xBE, 0xEF, 0xFE, 0xED};

IPAddress localIP(192, 168,  1, 5);
IPAddress gateway(192, 168,  1, 1);
IPAddress subnet(255, 255, 255, 0);
IPAddress dnsServer(1, 1, 1, 1);

WebServer server(80);

void handleRoot()
{
  String html = "<html><head><meta charset='utf-8'><title>WT32-ETH01</title></head><body>";
  html += "<h1>WT32-ETH01 WebServer</h1>";
  html += "<p>Ethernet connected successfully.</p>";
  html += "<p>IP: " + localIP.toString() + "</p>";
  html += "</body></html>";
  server.send(200, "text/html", html);
}

void setup()
{
  Serial.begin(115200);
  delay(1000);

  Serial.println("Starting WT32-ETH01 Ethernet WebServer...");

  ETH.config(localIP, gateway, subnet, dnsServer);

  if (!ETH.begin(ETH_PHY_ADDR, ETH_PHY_POWER, ETH_PHY_MDC, ETH_PHY_MDIO, ETH_PHY_TYPE, ETH_CLK_MODE))
  {
    Serial.println("Failed to start Ethernet module!");
    while (true)
    {
      delay(1000);
    }
  }

  server.on("/", HTTP_GET, handleRoot);
  server.begin();

  Serial.println("WebServer started on port 80");
  Serial.println("Static IP: " + localIP.toString());
}

void loop()
{
  server.handleClient();

  if (ETH.linkUp())
  {
    static bool printedLink = false;
    if (!printedLink)
    {
      Serial.println("Ethernet link is up.");
      printedLink = true;
    }
  }
  else
  {
    static bool printedLink = false;
    if (!printedLink)
    {
      Serial.println("Ethernet link is down.");
      printedLink = true;
    }
  }

  delay(1000);
}
