#pragma once
///////////////////////////////////////////////////////////////////////////////
// Vorlage für die Netzwerkkonfiguration.
//
// Diese Datei nach "network_config.h" im selben Verzeichnis kopieren und die
// Adressen des eigenen Netzes eintragen. In "network_config.h" ist die tatsaechliche
// Netzstruktur der Anlage und wird dadurch nicht veroeffentlicht.
///////////////////////////////////////////////////////////////////////////////
#include <Arduino.h>

IPAddress local_IP(192, 168, 1, 50);      // Feste Adresse des Geraets
IPAddress gateway(192, 168, 1, 1);        // Gateway, muss NTP erreichen koennen
IPAddress subnet(255, 255, 255, 0);       // Subnetzmaske
IPAddress primaryDNS(8, 8, 8, 8);         // Primaerer DNS
IPAddress secondaryDNS(8, 8, 4, 4);       // Sekundaerer DNS
