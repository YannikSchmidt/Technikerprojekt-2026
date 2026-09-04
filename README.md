# Technikerprojekt 2026

Backup der aktiven PlatformIO-Projekte rund um die Strommessung einer
Photovoltaik-Anlage. Die drei Projekte gehoeren fachlich zusammen und
kommunizieren untereinander per Modbus RTU ueber RS485.

## Projekte

| Ordner | Board | Rolle |
| --- | --- | --- |
| `Projekt_Logger` | WT32-ETH01 (ESP32 + LAN8720) | Modbus-**Master** und Datenlogger. Liest zyklisch die Messwerte, schreibt sie binaer auf SD-Karte und stellt sie ueber einen Webserver als Live-Ansicht sowie als CSV-Download bereit. |
| `Projekt_Sensor` | ESP32 DevKit v1 | Modbus-**Slave**. Erfasst acht Sensorkanaele und stellt sie als Holding-Register bereit. |
| `ProjektTest` | WT32-ETH01 | Testprojekt zur Inbetriebnahme der Ethernet-Anbindung und des Webservers. |

## Aufbau

Der Logger fragt die Register des Sensors ueber RS485 ab, versieht die Werte
mit einem per NTP synchronisierten UNIX-Zeitstempel und legt sie als
gepackten Binaerdatensatz auf der SD-Karte ab. Erst beim Download werden die
Rohwerte in eine CSV-Datei mit Semikolon als Trennzeichen umgewandelt. Das
spart Schreibvolumen auf der Karte und haelt das Dateiformat kompakt.

## Netzwerkkonfiguration einrichten

`Projekt_Logger` und `ProjektTest` beziehen ihre IP-Adressen aus der Datei
`include/network_config.h`. Diese Datei ist **nicht** Teil des Repositorys, da
die Netzstruktur der Anlage nicht oeffentlich sein soll. Vor dem ersten Bauen
einmalig die Vorlage kopieren und anpassen:

```
cd Projekt_Logger/include
cp network_config.example.h network_config.h
```

Anschliessend in `network_config.h` die Adressen des eigenen Netzes eintragen.
Ohne diesen Schritt bricht der Build mit `network_config.h: No such file or
directory` ab. Fuer `ProjektTest` gilt dasselbe.

## Bauen und Flashen

Die Projekte werden einzeln uebersetzt, jedes hat eine eigene
`platformio.ini`:

```
cd Projekt_Logger
pio run              # uebersetzen
pio run -t upload    # flashen
pio device monitor   # serielle Ausgabe, 115200 Baud
```

## Hinweise zum Repository

Dieses Repository liegt im gemeinsamen `Projects`-Ordner, der noch viele
aeltere Arduino-Projekte enthaelt. Die `.gitignore` im Wurzelverzeichnis
arbeitet deshalb als Whitelist: Sie ignoriert zunaechst alles und schliesst
danach gezielt nur die drei oben genannten Projekte wieder ein. **Ein neues
Projekt taucht erst dann in Git auf, wenn es dort ergaenzt wird.**

Build-Artefakte (`.pio/`) und maschinenlokale VSCode-Dateien
(`c_cpp_properties.json`, `launch.json`) sind bewusst ausgeschlossen, da sie
absolute Pfade dieses Rechners enthalten. Ebenso ausgeschlossen ist
`network_config.h` mit den echten Netzwerkadressen.

Die Beispieldateien im jeweiligen `test/`-Ordner sind eingefrorene
Entwicklungsstaende und werden nicht gebaut. Die dort enthaltenen IP-Adressen
sind Platzhalter aus dem Bereich `192.168.1.x`.
