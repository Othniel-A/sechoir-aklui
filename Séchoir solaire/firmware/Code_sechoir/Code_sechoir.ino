/*
 * Séchoir solaire hybride automatique — granulés de maïs « Aklui »
 * ESP32 : régulation température / ventilation, phases de séchage,
 * affichage LCD et envoi des mesures vers Firebase.
 *
 * Identifiants Wi-Fi et Firebase : voir secrets.example.h
 */

#include <Wire.h>
#include <LiquidCrystal_I2C.h>
#include <WiFi.h>
#include <HTTPClient.h>
#include <DHT.h>
#include <OneWire.h>
#include <DallasTemperature.h>
#include <PID_v1.h>
#include <NTPClient.h>
#include <WiFiUdp.h>
#include <time.h>
#include "secrets.h"

// ===================== Broches =====================
#define DHTPIN1 5          // DHT11 n°1 (TSC / HSC)
#define DHTPIN2 14         // DHT11 n°2 (TSCH / HSCH)
#define DHTTYPE DHT11

#define ONE_WIRE_BUS1 4    // DS18B20 n°1 (TEC)
#define ONE_WIRE_BUS2 13   // DS18B20 n°2 (TICH)

#define RELAY_PIN 18       // Relais de la résistance chauffante

#define IN3 32             // L293D : sens du ventilateur
#define IN4 33
#define EN2 27             // L293D : PWM du ventilateur

// ===================== Paramètres de séchage =====================
const double CONSIGNE_PHASE_1_2 = 60.0;   // °C (phases I et II)
const double CONSIGNE_PHASE_3   = 55.0;   // °C (phase III)
const double TEMP_SECURITE      = 65.0;   // °C : coupure immédiate de la résistance

// Phases selon l'écart d'humidité e = HSCH - HSC (en % HR)
const float E_SEUIL_PHASE_3 = 3.0;        // e < 3 : passage en phase III
const float E_SEUIL_FIN     = 1.0;        // e < 1 : fin du séchage
const unsigned long DUREE_MIN_AVANT_PHASES_MS = 30UL * 60UL * 1000UL;  // pas de changement de phase avant 30 min
const unsigned long DUREE_CONFIRMATION_MS     = 10UL * 60UL * 1000UL;  // le seuil doit tenir 10 min

// Ventilateur (PWM 8 bits)
const double FAN_PWM_MIN = 50;
const double FAN_PWM_MAX = 255;

// Cycles et intervalles
const unsigned long CYCLE_MS           = 2000;    // période de la boucle de régulation
const unsigned long heaterInterval     = 60000;   // durée minimale ON / OFF de la résistance (1 min)
const unsigned long dbInterval         = 60000;   // envoi Firebase toutes les minutes
const unsigned long WIFI_TIMEOUT_MS    = 15000;   // attente max de connexion au démarrage
const unsigned long WIFI_RETRY_MS      = 30000;   // nouvelle tentative si connexion perdue
const uint8_t       MAX_ECHECS_LECTURE = 5;       // lectures ratées consécutives avant de déclarer un capteur en défaut

// ===================== Capteurs =====================
DHT dht1(DHTPIN1, DHTTYPE);
DHT dht2(DHTPIN2, DHTTYPE);

OneWire oneWire1(ONE_WIRE_BUS1);
OneWire oneWire2(ONE_WIRE_BUS2);
DallasTemperature sensors1(&oneWire1);
DallasTemperature sensors2(&oneWire2);

LiquidCrystal_I2C lcd(0x27, 20, 4);

struct Mesure {
  float valeur = NAN;
  bool valide = false;
  uint8_t echecs = 0;
};

Mesure tsc;    // DHT11 n°1 : température (grandeur régulée)
Mesure hsc;    // DHT11 n°1 : humidité
Mesure tsch;   // DHT11 n°2 : température
Mesure hsch;   // DHT11 n°2 : humidité
Mesure tec;    // DS18B20 n°1 : température
Mesure tich;   // DS18B20 n°2 : température

// ===================== Phases =====================
enum Phase { PHASE_1_2, PHASE_3, FIN };
Phase phase = PHASE_1_2;
Phase cibleEnCours = PHASE_1_2;
unsigned long cibleDepuis = 0;
unsigned long debutSechage = 0;

// ===================== PID (ventilateur) =====================
// Direction REVERSE : plus la température dépasse la consigne, plus le ventilateur accélère.
double Setpoint = CONSIGNE_PHASE_1_2, Input = 0, Output = FAN_PWM_MIN;
double Kp = 2.0, Ki = 0.5, Kd = 1.0;
PID myPID(&Input, &Output, &Setpoint, Kp, Ki, Kd, REVERSE);

// ===================== Résistance chauffante =====================
unsigned long previousHeaterMillis = 0;
bool heaterOn = false;

// ===================== Réseau =====================
unsigned long previousDBMillis = 0;
unsigned long derniereTentativeWiFi = 0;
unsigned long dernierCycle = 0;

WiFiUDP ntpUDP;
NTPClient timeClient(ntpUDP, "pool.ntp.org", 3600, 60000);   // UTC+1 (Bénin)

// ===================== Setup =====================
void setup() {
  Serial.begin(115200);

  lcd.init();
  lcd.backlight();
  lcd.clear();
  lcd.setCursor(0, 0);
  lcd.print("Sechoir AKLUI");

  // Relais et ventilateur à l'arrêt
  pinMode(RELAY_PIN, OUTPUT);
  digitalWrite(RELAY_PIN, LOW);
  pinMode(IN3, OUTPUT);
  pinMode(IN4, OUTPUT);
  pinMode(EN2, OUTPUT);
  setFan(0);

  // Wi-Fi : attente limitée, le séchoir démarre même sans réseau
  lcd.setCursor(0, 1);
  lcd.print("Connexion Wi-Fi...");
  WiFi.mode(WIFI_STA);
  WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
  unsigned long t0 = millis();
  while (WiFi.status() != WL_CONNECTED && millis() - t0 < WIFI_TIMEOUT_MS) {
    delay(500);
    Serial.println("Connexion en cours...");
  }
  derniereTentativeWiFi = millis();
  lcd.setCursor(0, 1);
  if (WiFi.status() == WL_CONNECTED) {
    Serial.println("Connecte au Wi-Fi");
    lcd.print("Wi-Fi OK            ");
  } else {
    Serial.println("Wi-Fi indisponible : mode autonome");
    lcd.print("Mode autonome       ");
  }

  // Capteurs
  dht1.begin();
  dht2.begin();
  sensors1.begin();
  sensors2.begin();
  sensors1.setWaitForConversion(false);   // lecture non bloquante
  sensors2.setWaitForConversion(false);
  sensors1.requestTemperatures();
  sensors2.requestTemperatures();

  // Heure NTP
  timeClient.begin();
  if (WiFi.status() == WL_CONNECTED) timeClient.update();

  // PID
  myPID.SetOutputLimits(FAN_PWM_MIN, FAN_PWM_MAX);
  myPID.SetSampleTime(CYCLE_MS - 200);    // légèrement inférieur au cycle pour que Compute() s'exécute à chaque cycle
  myPID.SetMode(AUTOMATIC);

  delay(1500);
  lcd.clear();
  debutSechage = millis();
}

// ===================== Boucle principale =====================
void loop() {
  unsigned long now = millis();

  maintenirWiFi(now);

  if (now - dernierCycle >= CYCLE_MS) {
    dernierCycle = now;
    lireCapteurs();
    gererPhases(now);
    regulation();
    displayData();
    debugSerie();
  }

  sendDataToFirebase();
}

// ===================== Lecture des capteurs =====================
void majMesure(Mesure &m, float brut, bool ok) {
  if (ok) {
    m.valeur = brut;
    m.valide = true;
    m.echecs = 0;
  } else {
    if (m.echecs < 255) m.echecs++;
    if (m.echecs >= MAX_ECHECS_LECTURE) m.valide = false;   // en dessous, on garde la dernière valeur valide
  }
}

// DS18B20 : -127 = capteur débranché, 85 °C = valeur par défaut avant la première conversion
bool dsValide(float t) {
  return !isnan(t) && t != DEVICE_DISCONNECTED_C && fabs(t - 85.0f) > 0.01f;
}

void lireCapteurs() {
  float t1 = dht1.readTemperature();
  float h1 = dht1.readHumidity();
  float t2 = dht2.readTemperature();
  float h2 = dht2.readHumidity();
  majMesure(tsc,  t1, !isnan(t1));
  majMesure(hsc,  h1, !isnan(h1));
  majMesure(tsch, t2, !isnan(t2));
  majMesure(hsch, h2, !isnan(h2));

  // Résultat de la conversion lancée au cycle précédent, puis nouvelle demande
  float d1 = sensors1.getTempCByIndex(0);
  float d2 = sensors2.getTempCByIndex(0);
  majMesure(tec,  d1, dsValide(d1));
  majMesure(tich, d2, dsValide(d2));
  sensors1.requestTemperatures();
  sensors2.requestTemperatures();
}

// ===================== Phases de séchage =====================
void gererPhases(unsigned long now) {
  if (phase == FIN) return;
  if (!hsc.valide || !hsch.valide) return;                          // humidités nécessaires
  if (now - debutSechage < DUREE_MIN_AVANT_PHASES_MS) return;       // temps de montée en régime

  float e = hsch.valeur - hsc.valeur;

  Phase cible = phase;
  if (e < E_SEUIL_FIN) cible = FIN;
  else if (e < E_SEUIL_PHASE_3 && phase == PHASE_1_2) cible = PHASE_3;

  if (cible == phase) {
    cibleEnCours = phase;                 // condition non remplie : on réinitialise la confirmation
    return;
  }
  if (cible != cibleEnCours) {
    cibleEnCours = cible;                 // nouvelle cible : démarrage de la confirmation
    cibleDepuis = now;
  } else if (now - cibleDepuis >= DUREE_CONFIRMATION_MS) {
    changerPhase(cible);
  }
}

void changerPhase(Phase p) {
  phase = p;
  cibleEnCours = p;
  if (p == PHASE_3) {
    Setpoint = CONSIGNE_PHASE_3;
    Serial.println("Phase III : consigne 55 C");
  } else if (p == FIN) {
    setHeater(false);
    setFan(0);
    Serial.println("Fin du sechage");
  }
}

// ===================== Régulation =====================
void regulation() {
  if (phase == FIN) {
    setHeater(false);
    setFan(0);
    return;
  }

  // Capteur de régulation en défaut : repli sûr (pas de chauffage, ventilation maximale)
  if (!tsc.valide) {
    setHeater(false);
    setFan((uint8_t)FAN_PWM_MAX);
    Serial.println("Defaut capteur de regulation : repli sur");
    return;
  }

  Input = tsc.valeur;
  myPID.Compute();
  setFan((uint8_t)Output);   // température > consigne : ventilateur accéléré, sinon ralenti
  controlHeater();
}

void setHeater(bool on) {
  if (on == heaterOn) return;
  digitalWrite(RELAY_PIN, on ? HIGH : LOW);
  heaterOn = on;
  previousHeaterMillis = millis();
}

// Résistance en tout ou rien : durée minimale de 1 minute à chaque état
void controlHeater() {
  // Coupure de sécurité immédiate
  if (Input >= TEMP_SECURITE) {
    setHeater(false);
    return;
  }

  unsigned long currentMillis = millis();
  if (heaterOn) {
    if (currentMillis - previousHeaterMillis >= heaterInterval) setHeater(false);
  } else {
    if (currentMillis - previousHeaterMillis >= heaterInterval && Input < Setpoint) setHeater(true);
  }
}

void setFan(uint8_t pwm) {
  if (pwm == 0) {
    digitalWrite(IN3, LOW);
    digitalWrite(IN4, LOW);
    analogWrite(EN2, 0);
  } else {
    digitalWrite(IN3, HIGH);
    digitalWrite(IN4, LOW);
    analogWrite(EN2, pwm);
  }
}

// ===================== Affichage LCD =====================
void fmtMesure(char *out, size_t n, const Mesure &m) {
  if (m.valide) snprintf(out, n, "%.2f", m.valeur);
  else snprintf(out, n, "ERR");
}

void lcdLigne(uint8_t ligne, const char *texte) {
  char buf[21];
  snprintf(buf, sizeof(buf), "%-20s", texte);   // complète avec des espaces : pas besoin de lcd.clear()
  lcd.setCursor(0, ligne);
  lcd.print(buf);
}

void displayData() {
  char a[10], b[10], c[10], d[10], e[10], f[10];
  fmtMesure(a, sizeof(a), tec);    // AMB : entrée du capteur solaire
  fmtMesure(b, sizeof(b), tsc);    // ECH : température
  fmtMesure(c, sizeof(c), hsc);    // ECH : humidité
  fmtMesure(d, sizeof(d), tich);   // ICH : intérieur de la chambre
  fmtMesure(e, sizeof(e), tsch);   // SCH : température
  fmtMesure(f, sizeof(f), hsch);   // SCH : humidité

  const char *phaseTxt = (phase == PHASE_1_2) ? "PH1-2" : (phase == PHASE_3) ? "PH3" : "FIN";
  char l[24];

  snprintf(l, sizeof(l), "AMB: %s%cC  %-5s", a, 223, phaseTxt);
  lcdLigne(0, l);
  snprintf(l, sizeof(l), "ECH: %s%cC %s%%", b, 223, c);
  lcdLigne(1, l);
  snprintf(l, sizeof(l), "ICH: %s%cC  %s", d, 223, heaterOn ? "CHAUF" : "");
  lcdLigne(2, l);
  snprintf(l, sizeof(l), "SCH: %s%cC %s%%", e, 223, f);
  lcdLigne(3, l);
}

void debugSerie() {
  Serial.printf("Phase:%d Consigne:%.1f TSC:%.2f Fan:%d Chauf:%d WiFi:%d\n",
                (int)phase, Setpoint, tsc.valeur, (int)Output, heaterOn, WiFi.status() == WL_CONNECTED);
}

// ===================== Réseau =====================
void maintenirWiFi(unsigned long now) {
  if (WiFi.status() == WL_CONNECTED) return;
  if (now - derniereTentativeWiFi >= WIFI_RETRY_MS) {
    derniereTentativeWiFi = now;
    Serial.println("Reconnexion Wi-Fi...");
    WiFi.disconnect();
    WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
  }
}

String dateHeure() {
  if (!timeClient.isTimeSet()) return "inconnue";
  time_t brut = timeClient.getEpochTime();   // décalage UTC+1 déjà appliqué
  struct tm t;
  gmtime_r(&brut, &t);
  char buf[24];
  snprintf(buf, sizeof(buf), "%04d-%02d-%02d %02d:%02d:%02d",
           t.tm_year + 1900, t.tm_mon + 1, t.tm_mday, t.tm_hour, t.tm_min, t.tm_sec);
  return String(buf);
}

// Valeur JSON : "null" si le capteur est en défaut (évite un JSON invalide avec NaN)
String jsonVal(const Mesure &m) {
  return m.valide ? String(m.valeur, 2) : String("null");
}

void sendDataToFirebase() {
  unsigned long currentMillis = millis();
  if (currentMillis - previousDBMillis < dbInterval) return;
  previousDBMillis = currentMillis;

  if (WiFi.status() != WL_CONNECTED) {
    Serial.println("Non connecte au WiFi : envoi ignore");
    return;
  }

  timeClient.update();

  String url = String(FIREBASE_HOST) + "/datas.json";
  if (strlen(FIREBASE_AUTH) > 0) url += "?auth=" + String(FIREBASE_AUTH);

  String jsonData = "{";
  jsonData += "\"TSC\":" + jsonVal(tsc) + ",";
  jsonData += "\"HSC\":" + jsonVal(hsc) + ",";
  jsonData += "\"TSCH\":" + jsonVal(tsch) + ",";
  jsonData += "\"HSCH\":" + jsonVal(hsch) + ",";
  jsonData += "\"TEC\":" + jsonVal(tec) + ",";
  jsonData += "\"TICH\":" + jsonVal(tich) + ",";
  jsonData += "\"DateTime\":\"" + dateHeure() + "\"";
  jsonData += "}";

  HTTPClient http;
  http.setTimeout(5000);
  http.begin(url);
  http.addHeader("Content-Type", "application/json");

  int httpResponseCode = http.POST(jsonData);
  if (httpResponseCode > 0) {
    Serial.println("Reponse: " + http.getString());
  } else {
    Serial.println("Erreur dans la requete : " + http.errorToString(httpResponseCode));
  }
  http.end();
}
