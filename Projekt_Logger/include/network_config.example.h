#pragma once
///////////////////////////////////////////////////////////////////////////////
// Vorlage für die Netzwerkkonfiguration.
//
// Diese Datei nach "network_config.h" im selben Verzeichnis kopieren und die
// Adressen des eigenen Netzes eintragen. Die tatsächliche Netzstruktur der Anlage
// steht nur in "network_config.h" und wird nicht veröffentlicht.
///////////////////////////////////////////////////////////////////////////////
#include <Arduino.h>

IPAddress local_IP(192, 168, 1, 50);      // Feste Adresse des Geräts
IPAddress gateway(192, 168, 1, 1);        // Gateway, muss NTP erreichen können
IPAddress subnet(255, 255, 255, 0);       // Subnetzmaske
IPAddress primaryDNS(8, 8, 8, 8);         // Primärer DNS
IPAddress secondaryDNS(8, 8, 4, 4);       // Sekundärer DNS
