///////////////////////////////////////////////////////////////////////////////
//
//  TECHNIKERPROJEKT 2026 - SENSORKNOTEN (Modbus-Slave)
//
//  AUFGABE
//  Dieses Geraet misst den Strom in acht Strangleitungen eines
//  Generatoranschlusskastens (GAK) einer Photovoltaik-Anlage und stellt die
//  Messwerte als Modbus-Register bereit. Der Datenlogger (Projekt_Logger)
//  holt sie sich von dort ab.
//
//  HARDWARE
//  ESP32 DevKit v1, acht Hall-Stromsensoren der Baureihe ACS758 und ein
//  RS485-Transceiver fuer die Verbindung zum Logger.
//
//  MESSPRINZIP
//  Der ACS758 ist ein Hall-Sensor. Er sitzt nicht in der Leitung, sondern misst
//  das Magnetfeld, das der Strom um den Leiter erzeugt. Dadurch gibt es keine
//  elektrische Verbindung zwischen Messleitung und Elektronik. Der Sensor gibt
//  eine Spannung aus, die sich proportional zum Strom aendert:
//
//      Strom = 0 A    ->  Ausgang = halbe Versorgungsspannung ("Nullpunkt")
//      Strom > 0 A    ->  Ausgang steigt
//      Strom < 0 A    ->  Ausgang faellt
//
//  SIGNALKETTE
//
//     Strang        ACS758          Spannungsteiler        ESP32
//   (bis 100 A) -> Hall-Sensor  ->  10k / 20k         ->  ADC-Eingang
//                  0 .. 5 V         mal 0,667             0 .. 3,3 V
//
//  Der Spannungsteiler ist notwendig, weil der ACS758 mit 5 V versorgt wird und
//  bis 5 V ausgeben kann, die ADC-Eingaenge des ESP32 aber hoechstens 3,3 V
//  vertragen. Eine hoehere Spannung wuerde den Eingang zerstoeren.
//
//  ABLAUF IM PROGRAMM
//
//    alle 3 s:  128 Einzelmessungen je Kanal
//                 -> sortieren, oberste und unterste 32 Werte verwerfen
//                 -> Mittelwert der verbleibenden 64 Werte
//                 -> in Ampere umrechnen
//                 -> aufaddieren
//
//    nach 10 solchen Messungen (also alle 30 s):
//                 -> Mittelwert bilden
//                 -> in Milliampere umrechnen
//                 -> in die Modbus-Register schreiben
//                 -> Paketnummer erhoehen
//
//  Zwei Mittelungsstufen deshalb, weil die Sonneneinstrahlung schwankt und ein
//  einzelner Messwert wenig aussagt. Die erste Stufe unterdrueckt kurze
//  Stoerimpulse, die zweite glaettet den Verlauf ueber eine halbe Minute.
//
//  QUELLEN ZUM NACHLESEN (Stand 2026)
//  - ACS758 Datenblatt (Allegro MicroSystems):
//    https://www.allegromicro.com/en/products/sense/current-sensor-ics/fifty-to-two-hundred-amp-integrated-conductor-sensor-ics/acs758
//  - Modbus-Spezifikation:        https://modbus.org/docs/Modbus_Application_Protocol_V1_1b3.pdf
//  - Bibliothek modbus-esp8266:   https://github.com/emelianov/modbus-esp8266
//  - ESP32 ADC (Analog-Digital-Wandler):
//    https://docs.espressif.com/projects/arduino-esp32/en/latest/api/adc.html
//  - Spannungsteiler erklaert:    https://de.wikipedia.org/wiki/Spannungsteiler
//  - Hall-Effekt erklaert:        https://de.wikipedia.org/wiki/Hall-Effekt
//  - Getrimmter Mittelwert:       https://de.wikipedia.org/wiki/Gestutztes_Mittel
//
///////////////////////////////////////////////////////////////////////////////

#include <ModbusRTU.h>
#include <algorithm>   // fuer std::sort


///////////////////////////////////////////////////////////////////////////////
// 1. ANZAHL DER KANAELE
///////////////////////////////////////////////////////////////////////////////

// Acht Straenge, also acht Sensoren.
const uint8_t KANAELE = 8;


///////////////////////////////////////////////////////////////////////////////
// 2. MODBUS-EINSTELLUNGEN
//
// Modbus RTU kennt einen Master (hier: der Logger) und einen oder mehrere
// Slaves (hier: dieses Geraet). Nur der Master fragt, der Slave antwortet.
// Jeder Slave hat eine eigene Adresse, damit an einer Leitung mehrere Geraete
// haengen koennen.
//
// REGISTERAUFTEILUNG - fest vereinbart mit dem Logger:
//
//   Register 10 : Paketnummer (erhoeht sich mit jedem neuen Mittelwert)
//   Register 11 : GAK-Nummer  (entspricht der Slave-Adresse)
//   Register 12 : Strang 1 in Milliampere
//   Register 13 : Strang 2 in Milliampere
//   ...
//   Register 19 : Strang 8 in Milliampere
//
// Die Startadresse ergibt sich aus der Slave-Adresse mal 10. Bei einem zweiten
// GAK mit der Adresse 2 laegen die Register also ab 20 - so kollidieren
// mehrere Sensorknoten an derselben Leitung nicht.
//
// Warum Milliampere statt Ampere?
// Ein Modbus-Register fasst genau 16 Bit, also eine ganze Zahl. Kommazahlen
// gibt es nicht. Statt 5,12 A wird deshalb 5120 mA uebertragen. Der Logger
// teilt beim Anzeigen wieder durch 1000.
///////////////////////////////////////////////////////////////////////////////

const uint16_t SLAVE_ID          = 1;               // zugleich die GAK-Nummer
const uint16_t MODBUS_START_REG  = SLAVE_ID * 10;   // erstes eigenes Register
const uint16_t MODBUS_REG_ANZAHL = 2 + KANAELE;     // Paket + GAK + 8 Messwerte
const uint32_t MODBUS_BAUDRATE   = 9600;

// Positionen innerhalb des Registerblocks, damit unten keine nackten Zahlen stehen
const uint16_t REG_OFFSET_PAKET  = 0;
const uint16_t REG_OFFSET_GAK    = 1;
const uint16_t REG_OFFSET_WERTE  = 2;


///////////////////////////////////////////////////////////////////////////////
// 3. PINBELEGUNG
///////////////////////////////////////////////////////////////////////////////

// --- RS485-Transceiver an der zweiten seriellen Schnittstelle ---
const int PIN_RS485_RX = 27;
const int PIN_RS485_TX = 14;

// Manche RS485-Bausteine brauchen einen Steuerpin, der zwischen Senden und
// Empfangen umschaltet (DE/RE). Der hier verwendete Transceiver erledigt das
// selbst, deshalb steht der Wert auf -1 und die Bibliothek laesst den Pin weg.
const int PIN_RS485_RE_DE = -1;

// --- Analoge Sensoreingaenge ---
//
// WICHTIGER HINWEIS ZU DEN ADC-EINGAENGEN DES ESP32:
// Der ESP32 hat zwei getrennte Analog-Digital-Wandler, ADC1 und ADC2.
// ADC2 wird vom Funkteil mitbenutzt und liefert keine Werte mehr, sobald WLAN
// eingeschaltet ist. In diesem Projekt ist das unkritisch, weil der Sensorknoten
// ausschliesslich ueber RS485 kommuniziert und kein WLAN startet.
//
//   GPIO 4  -> ADC2   GPIO 25 -> ADC2
//   GPIO 36 -> ADC1   GPIO 39 -> ADC1   GPIO 34 -> ADC1   GPIO 35 -> ADC1
//   GPIO 32 -> ADC1   GPIO 33 -> ADC1
//
// Sollte das Geraet spaeter einmal WLAN nutzen, muessen U1 und U2 auf
// ADC1-Pins umgezogen werden.
const uint8_t SENSOR_PINS[KANAELE] = { 4, 25, 36, 39, 34, 35, 32, 33 };

// Klartextnamen, nur fuer die Ausgabe auf der seriellen Schnittstelle
const char* SENSOR_NAMEN[KANAELE] = { "U1", "U2", "U3", "U4", "U5", "U6", "U7", "U8" };


///////////////////////////////////////////////////////////////////////////////
// 4. UMRECHNUNG VON ADC-WERT NACH AMPERE
//
// Der Weg vom Rohwert zum Strom in drei Schritten:
//
//   1) Rohwert -> Spannung am ESP32-Pin
//        Spannung = (Rohwert / 4095) * 3,3 V
//
//   2) Spannung am Pin -> Abweichung vom Nullpunkt
//        Der Sensor gibt bei 0 A die halbe Versorgungsspannung aus. Dieser
//        Ruhewert (V_OFFSET) wird abgezogen. Was uebrig bleibt, ist das
//        eigentliche Messsignal.
//
//   3) Abweichung -> Strom
//        Strom = Abweichung / Empfindlichkeit
//
// Die Empfindlichkeit muss um den Spannungsteiler korrigiert werden: Am
// ESP32-Pin kommen nur zwei Drittel der Sensorspannung an, also ist dort auch
// die Aenderung je Ampere nur zwei Drittel so gross.
///////////////////////////////////////////////////////////////////////////////

const float V_REF   = 3.3f;      // Referenzspannung des ADC in Volt
const float ADC_MAX = 4095.0f;   // Groesster Rohwert bei 12 Bit Aufloesung (2^12 - 1)

// Empfindlichkeit des Stromsensors in Volt je Ampere.
//
// ACHTUNG FUER DIE PROJEKTDOKUMENTATION:
// Dieser Wert wurde am Aufbau empirisch ermittelt und weicht vom Katalogwert
// der gaengigen ACS758-Varianten ab (ACS758-100B: 20 mV/A, ACS758-050B: 40 mV/A).
// Vor der Abgabe sollte die genaue Sensorvariante am Bauteil abgelesen und hier
// entweder der Datenblattwert eingetragen oder die Abweichung im Messprotokoll
// begruendet werden. Der Wert bestimmt unmittelbar die Genauigkeit der Anlage.
const float SENSOR_EMPFINDLICHKEIT = 0.080f;   // 80 mV je Ampere

// --- Spannungsteiler ---
// Aufbau: Sensorausgang --[ R1 ]--+--[ R2 ]-- Masse
//                                 |
//                            ESP32-Pin
//
// Am Pin liegt der Anteil R2 / (R1 + R2) der Sensorspannung an:
//     20000 / (10000 + 20000) = 0,6667
// Aus 5,0 V am Sensor werden so 3,33 V am ESP32 - gerade noch zulaessig.
const float R1 = 10000.0f;
const float R2 = 20000.0f;
const float TEILERFAKTOR = R2 / (R1 + R2);

// Wirksame Empfindlichkeit, gemessen am ESP32-Pin
const float EFFEKTIVE_EMPFINDLICHKEIT = SENSOR_EMPFINDLICHKEIT * TEILERFAKTOR;

// --- Nullpunkte der acht Sensoren (manuelle Kalibrierung) ---
//
// Jeder Sensor hat fertigungsbedingt einen leicht anderen Ruhewert. Diese Werte
// wurden am Aufbau bei stromlosen Straengen gemessen und hier fest eingetragen.
// Rechnerisch waeren es 2,5 V * 0,667 = 1,667 V; die Abweichung nach unten
// stammt aus den Toleranzen der Sensoren und der Widerstaende.
//
// Wird ein Sensor getauscht, muss der zugehoerige Wert neu bestimmt werden:
// Strang stromlos schalten, Spannung am ESP32-Pin messen und hier eintragen.
const float V_OFFSET[KANAELE] = {
  1.561f,  // U1
  1.553f,  // U2
  1.581f,  // U3
  1.557f,  // U4
  1.573f,  // U5
  1.546f,  // U6
  1.563f,  // U7
  1.563f   // U8
};

// --- Rauschunterdrueckung ---
// Unterhalb dieser Grenze wird der Messwert auf glatt 0 gesetzt. Grund: Der
// Nullpunkt schwankt um einige Millivolt, was rechnerisch schon einige
// Zehntelampere ergibt. Ohne diese Schwelle wuerde die Anlage nachts Stroeme
// anzeigen, obwohl gar keine fliessen.
//
// Achtung, das ist ein bewusster Kompromiss: Echte Stroeme unter 0,5 A werden
// dadurch ebenfalls als 0 gemeldet.
const float TOTBAND_AMPERE = 0.5f;


///////////////////////////////////////////////////////////////////////////////
// 5. EINSTELLUNGEN ZUR MITTELWERTBILDUNG
//
// ERSTE STUFE - getrimmter Mittelwert (auch "gestutztes Mittel"):
// Es werden 128 Rohwerte hintereinander gelesen und der Groesse nach sortiert.
// Danach werden die 32 kleinsten und die 32 groessten Werte weggeworfen und nur
// aus den mittleren 64 der Mittelwert gebildet.
//
//   sortierte Werte:  [ 32 verworfen ][ 64 verwendet ][ 32 verworfen ]
//
// Vorteil gegenueber dem einfachen Mittelwert: Ein einzelner Stoerimpuls, etwa
// durch einen schaltenden Wechselrichter, faellt beim Sortieren automatisch an
// den Rand und wird verworfen. Beim einfachen Mittelwert wuerde er das Ergebnis
// verfaelschen.
//
// ZWEITE STUFE - gleitende Aufsummierung:
// Zehn dieser Punktmessungen im Abstand von drei Sekunden werden aufaddiert und
// am Ende durch zehn geteilt. Ein neuer Modbus-Wert entsteht damit alle 30 s.
///////////////////////////////////////////////////////////////////////////////

const int ANZAHL_ROHWERTE   = 128;  // Einzelmessungen je Kanal und Punktmessung
const int VERWORFEN_JE_SEITE = 32;  // davon oben und unten je verworfen
const int VERWENDETE_WERTE  = ANZAHL_ROHWERTE - (2 * VERWORFEN_JE_SEITE);

const unsigned long INTERVALL_PUNKTMESSUNG = 3000;  // Millisekunden
const int MESSUNGEN_JE_MITTELWERT = 10;


///////////////////////////////////////////////////////////////////////////////
// 6. GLOBALE VARIABLEN
///////////////////////////////////////////////////////////////////////////////

ModbusRTU mb;  // Modbus-Slave

unsigned long letztePunktmessung = 0;  // Zeitpunkt der letzten Messung
int   anzahlPunktmessungen = 0;        // wie viele seit dem letzten Mittelwert
float summeAmpere[KANAELE] = {0};      // aufaddierte Stroeme je Kanal

uint16_t paketNummer = 0;              // laufende Nummer der Mittelwerte


///////////////////////////////////////////////////////////////////////////////
// 7. EINE PUNKTMESSUNG DURCHFUEHREN
//
// Liest alle acht Kanaele ein, filtert die Ausreisser heraus und rechnet in
// Ampere um. Das Ergebnis wird auf summeAmpere aufaddiert.
///////////////////////////////////////////////////////////////////////////////
void fuehrePunktmessungAus() {
  // Zwischenspeicher fuer die Rohwerte: 8 Kanaele * 128 Werte * 2 Byte = 2 kB.
  // Die Variable liegt bewusst innerhalb der Funktion, damit dieser Speicher
  // nach dem Verlassen sofort wieder frei ist.
  uint16_t rohwerte[KANAELE][ANZAHL_ROHWERTE];

  // --- Schritt 1: Rohwerte einlesen ---
  // Bewusst kanalweise verschachtelt, damit alle acht Straenge praktisch
  // gleichzeitig erfasst werden und nicht nacheinander.
  for (int messung = 0; messung < ANZAHL_ROHWERTE; messung++) {
    for (int kanal = 0; kanal < KANAELE; kanal++) {
      rohwerte[kanal][messung] = analogRead(SENSOR_PINS[kanal]);
    }
  }

  // --- Schritt 2: Auswerten ---
  for (int kanal = 0; kanal < KANAELE; kanal++) {
    // Sortieren, damit die Ausreisser an den Raendern liegen
    std::sort(rohwerte[kanal], rohwerte[kanal] + ANZAHL_ROHWERTE);

    // Nur den mittleren Bereich aufaddieren
    long summeRoh = 0;
    for (int i = VERWORFEN_JE_SEITE; i < ANZAHL_ROHWERTE - VERWORFEN_JE_SEITE; i++) {
      summeRoh += rohwerte[kanal][i];
    }

    // Rohwert -> Spannung -> Strom
    float mittlererRohwert = (float)summeRoh / VERWENDETE_WERTE;
    float spannung = (mittlererRohwert / ADC_MAX) * V_REF;
    float ampere   = (spannung - V_OFFSET[kanal]) / EFFEKTIVE_EMPFINDLICHKEIT;

    // Rauschen um den Nullpunkt unterdruecken
    if (ampere > -TOTBAND_AMPERE && ampere < TOTBAND_AMPERE) {
      ampere = 0.0f;
    }

    summeAmpere[kanal] += ampere;
  }
}


///////////////////////////////////////////////////////////////////////////////
// 8. MITTELWERT BILDEN UND IN DIE MODBUS-REGISTER SCHREIBEN
//
// Wird alle 10 Punktmessungen aufgerufen, also etwa alle 30 Sekunden.
///////////////////////////////////////////////////////////////////////////////
void veroeffentlicheMittelwert() {
  Serial.printf("\n---[ GAK Nr:%u | Paket: %u ] MITTELWERT ---\n", SLAVE_ID, paketNummer);

  // Kopf des Registerblocks: Paketnummer und GAK-Nummer
  mb.Hreg(MODBUS_START_REG + REG_OFFSET_PAKET, paketNummer);
  mb.Hreg(MODBUS_START_REG + REG_OFFSET_GAK,   SLAVE_ID);

  for (int kanal = 0; kanal < KANAELE; kanal++) {
    float mittelwertAmpere = summeAmpere[kanal] / MESSUNGEN_JE_MITTELWERT;

    // In Milliampere umrechnen und auf eine ganze Zahl runden, weil ein
    // Modbus-Register nur ganze Zahlen aufnehmen kann.
    int16_t milliampere = (int16_t)(mittelwertAmpere * 1000.0f);

    // Die Umwandlung nach uint16_t ist noetig, weil die Bibliothek nur
    // vorzeichenlose Register kennt. Das Bitmuster bleibt dabei unveraendert;
    // der Logger wandelt es beim Lesen wieder in eine vorzeichenbehaftete
    // Zahl zurueck. Aus -1 wird also 65535 und beim Logger wieder -1.
    mb.Hreg(MODBUS_START_REG + REG_OFFSET_WERTE + kanal, (uint16_t)milliampere);

    Serial.printf("Strang %d (%s): %.2f A (Modbus: %d mA)\n",
                  kanal + 1, SENSOR_NAMEN[kanal], mittelwertAmpere, milliampere);
  }

  // Paketnummer erhoehen. Der Logger erkennt daran, ob er einen neuen Wert
  // bekommen hat oder denselben noch einmal liest.
  // Bei uint16_t springt der Zaehler nach 65535 von selbst wieder auf 0.
  paketNummer++;
}


///////////////////////////////////////////////////////////////////////////////
// 9. SETUP - wird einmal beim Einschalten ausgefuehrt
///////////////////////////////////////////////////////////////////////////////
void setup() {
  Serial.begin(115200);
  delay(500);  // kurze Pause, damit die serielle Ausgabe von Anfang an mitliest

  Serial.println("\n\n--- Technikerprojekt 2026: Sensorknoten startet ---");
  Serial.printf("Spannungsteiler:  Faktor %.4f\n", TEILERFAKTOR);
  Serial.printf("Empfindlichkeit:  %.4f V/A am Sensor, %.4f V/A am ESP32-Pin\n",
                SENSOR_EMPFINDLICHKEIT, EFFEKTIVE_EMPFINDLICHKEIT);
  Serial.printf("Totband:          +/- %.2f A\n", TOTBAND_AMPERE);
  Serial.printf("Mittelwert alle:  %lu s\n",
                (INTERVALL_PUNKTMESSUNG * MESSUNGEN_JE_MITTELWERT) / 1000UL);
  Serial.println("Nullpunkte (V_OFFSET) fest einprogrammiert.\n");

  // --- RS485 starten ---
  // SERIAL_8N1: 8 Datenbits, keine Paritaet, 1 Stoppbit. Diese Einstellung muss
  // beim Logger identisch sein, sonst kommt nur Zeichensalat an.
  Serial2.begin(MODBUS_BAUDRATE, SERIAL_8N1, PIN_RS485_RX, PIN_RS485_TX);
  Serial.printf("Serial2 (RS485) gestartet: %lu Baud, 8N1.\n", MODBUS_BAUDRATE);

  // --- Modbus starten ---
  if (PIN_RS485_RE_DE >= 0) {
    mb.begin(&Serial2, PIN_RS485_RE_DE);  // Transceiver mit Umschaltpin
  } else {
    mb.begin(&Serial2);                   // Transceiver schaltet selbst um
  }
  mb.slave(SLAVE_ID);
  Serial.printf("Modbus-Slave bereit. Adresse: %u\n", SLAVE_ID);

  // --- Register anlegen und mit 0 vorbelegen ---
  // addHreg() legt ein Holding-Register an. Ohne diesen Schritt antwortet der
  // Slave auf eine Anfrage mit dem Fehler "Illegal Data Address".
  for (uint16_t i = 0; i < MODBUS_REG_ANZAHL; i++) {
    mb.addHreg(MODBUS_START_REG + i, 0);
  }
  // Die GAK-Nummer aendert sich nie, sie kann sofort gesetzt werden.
  mb.Hreg(MODBUS_START_REG + REG_OFFSET_GAK, SLAVE_ID);

  Serial.printf("Register %u bis %u angelegt.\n",
                MODBUS_START_REG, MODBUS_START_REG + MODBUS_REG_ANZAHL - 1);
  Serial.println("Setup abgeschlossen. Starte Punktmessung...\n");
}


///////////////////////////////////////////////////////////////////////////////
// 10. LOOP - wird danach endlos wiederholt
///////////////////////////////////////////////////////////////////////////////
void loop() {
  // mb.task() muss so oft wie moeglich aufgerufen werden. Nur dort prueft die
  // Bibliothek, ob eine Anfrage des Loggers eingetroffen ist, und schickt die
  // Antwort ab. Ein delay() an dieser Stelle wuerde dazu fuehren, dass der
  // Logger keine Antwort bekommt und einen Verbindungsfehler meldet.
  mb.task();

  unsigned long jetzt = millis();

  // Ist es Zeit fuer die naechste Punktmessung?
  // Der Vergleich "jetzt - letztePunktmessung" funktioniert auch dann korrekt,
  // wenn millis() nach rund 49 Tagen Dauerbetrieb wieder bei 0 beginnt.
  if (jetzt - letztePunktmessung < INTERVALL_PUNKTMESSUNG) {
    return;
  }
  letztePunktmessung = jetzt;

  fuehrePunktmessungAus();
  anzahlPunktmessungen++;
  Serial.printf("Punktmessung %d / %d ausgefuehrt.\n",
                anzahlPunktmessungen, MESSUNGEN_JE_MITTELWERT);

  // Genug Einzelmessungen gesammelt? Dann Mittelwert bilden und bereitstellen.
  if (anzahlPunktmessungen >= MESSUNGEN_JE_MITTELWERT) {
    veroeffentlicheMittelwert();

    // Zaehler und Summen fuer den naechsten Durchlauf zuruecksetzen
    anzahlPunktmessungen = 0;
    for (int kanal = 0; kanal < KANAELE; kanal++) {
      summeAmpere[kanal] = 0.0f;
    }
  }
}
