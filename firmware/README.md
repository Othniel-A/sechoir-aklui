# 💻 Firmware — Séchoir hybride Aklui (ESP32)

Programme embarqué du séchoir solaire hybride : lecture des capteurs, régulation de la résistance chauffante et du ventilateur, gestion des phases de séchage, affichage LCD et envoi des mesures vers Firebase.

> Présentation générale du projet : voir le [README principal](../README.md).

---

## ✨ Ce que fait le programme

- Lit **2 capteurs DHT11** (température + humidité) et **2 sondes DS18B20** (température), avec détection des capteurs en défaut.
- Commande la **résistance chauffante** par relais, en cycles de 60 s, avec une coupure de sécurité immédiate.
- Commande la **vitesse du ventilateur** en PWM via un driver L293D, à partir de la sortie du PID.
- Gère les **phases de séchage** (60 °C, puis 55 °C, puis arrêt) à partir de l'écart d'humidité entre la sortie de la chambre et la sortie du capteur.
- Affiche les mesures, la phase et l'état du chauffage sur un **LCD 20×4 I2C**.
- Envoie les mesures à **Firebase Realtime Database** toutes les 60 s (Wi-Fi), avec la date et l'heure NTP.
- Fonctionne **en autonome** si le Wi-Fi est indisponible.

---

## 🔌 Câblage

| Périphérique | Broche ESP32 |
|---|---|
| DHT11 n°1 | GPIO 5 |
| DHT11 n°2 | GPIO 14 |
| DS18B20 n°1 (bus OneWire 1) | GPIO 4 |
| DS18B20 n°2 (bus OneWire 2) | GPIO 13 |
| Relais (résistance chauffante) | GPIO 18 |
| L293D — IN3 / IN4 | GPIO 32 / GPIO 33 |
| L293D — EN2 (PWM) | GPIO 27 |
| LCD I2C 20×4 (adresse `0x27`) | SDA GPIO 21 / SCL GPIO 22 |

> ⚠️ Le relais commute la résistance de 1000 W (secteur ou onduleur).

### Correspondance des capteurs

| Capteur | Variable | Clé JSON | Position dans le séchoir | Libellé LCD |
|---|---|---|---|---|
| DS18B20 n°1 | `tec` | `TEC` | Entrée du capteur solaire (air ambiant) | `AMB` |
| DHT11 n°1 | `tsc`, `hsc` | `TSC`, `HSC` | Sortie du capteur solaire | `ECH` |
| DS18B20 n°2 | `tich` | `TICH` | Intérieur de la chambre de séchage | `ICH` |
| DHT11 n°2 | `tsch`, `hsch` | `TSCH`, `HSCH` | Sortie de la chambre de séchage | `SCH` |

---

## 🗂️ Structure

```
firmware/
├── README.md
├── .gitignore                       # ignore secrets.h
└── Code_sechoir/
    ├── Code_sechoir.ino         # programme principal
    └── secrets.example.h            # modèle des identifiants (à copier en secrets.h)
```

Le dossier du sketch doit porter le même nom que le fichier `.ino`.

| Fonction | Rôle |
|---|---|
| `setup()` | Init LCD, relais, ventilateur, Wi-Fi (attente limitée), capteurs, NTP, PID |
| `loop()` | Cycle de 2 s : capteurs → phases → régulation → LCD ; envoi Firebase |
| `lireCapteurs()` | Lecture des 6 mesures avec validation (`majMesure`, `dsValide`) |
| `gererPhases()` | Passage 60 °C → 55 °C → fin selon l'écart d'humidité |
| `regulation()` | PID du ventilateur, résistance, repli sûr |
| `controlHeater()` | Résistance en tout ou rien (cycles de 60 s) et coupure de sécurité |
| `setFan()` / `setHeater()` | Pilotage du L293D et du relais |
| `displayData()` | Affichage des mesures, de la phase et du chauffage |
| `maintenirWiFi()` | Reconnexion automatique toutes les 30 s |
| `sendDataToFirebase()` | POST JSON toutes les 60 s |

---

## 🧠 Logique de régulation

### Phases de séchage

L'écart d'humidité est `e = HSCH − HSC` (en % HR).

| Condition | Phase | Consigne | Action |
|---|---|---|---|
| `e ≥ 3` | I et II | 60 °C | Régulation normale |
| `1 ≤ e < 3` | III | 55 °C | Régulation normale |
| `e < 1` | Fin | — | Résistance et ventilateur arrêtés |

- Aucun changement de phase pendant les **30 premières minutes**.
- Une condition doit rester vraie **10 minutes** avant que la phase change.
- Le passage en phase III est définitif ; la fin du séchage l'est aussi jusqu'au redémarrage.

### Résistance chauffante

- Grandeur régulée : `tsc` (DHT11 n°1).
- Un état (allumée ou éteinte) dure au minimum `heaterInterval` = 60 s.
- Allumée seulement si la température est inférieure à la consigne au moment de la décision.
- **Coupure de sécurité** : arrêt immédiat dès que la température atteint 65 °C.

### Ventilateur

- Le **PID** (Kp = 2, Ki = 0,5, Kd = 1, direction inverse) calcule le PWM entre 50 et 255.
- Température au-dessus de la consigne : ventilateur accéléré. En dessous : ventilateur ralenti.
- En fin de séchage : ventilateur arrêté.

### Gestion des défauts capteurs

- **DHT11** : lecture `NaN` ignorée ; après 5 échecs consécutifs, le capteur est déclaré en défaut.
- **DS18B20** : les valeurs `-127 °C` (débranché) et `85 °C` (valeur par défaut avant conversion) sont rejetées.
- Une mesure en défaut s'affiche `ERR` sur le LCD et vaut `null` dans le JSON.
- Si le capteur de régulation (`tsc`) est en défaut : **repli sûr**, résistance coupée et ventilateur au maximum.

---

## 🖥️ Affichage LCD

```
AMB: 34.88°C  PH1-2
ECH: 60.10°C 5.00%
ICH: 48.75°C  CHAUF
SCH: 45.80°C 31.00%
```

La phase affichée est `PH1-2`, `PH3` ou `FIN`. `CHAUF` apparaît quand la résistance est allumée.

---

## 📦 Installation

### Prérequis

- Arduino IDE ≥ 2.x avec le support **ESP32** (core ≥ 3.0, nécessaire pour `analogWrite`)
- Bibliothèques :
  - `LiquidCrystal_I2C`
  - `DHT sensor library` (Adafruit)
  - `OneWire`
  - `DallasTemperature`
  - `PID_v1`
  - `NTPClient`
  - `WiFi`, `HTTPClient`, `Wire` (incluses avec le core ESP32)

### Configuration

1. Copier `secrets.example.h` en `secrets.h` dans le dossier du sketch.
2. Renseigner vos valeurs :

```cpp
#define WIFI_SSID      "<NOM_WIFI>"
#define WIFI_PASSWORD  "<MOT_DE_PASSE_WIFI>"
#define FIREBASE_HOST  "https://<votre-projet>-default-rtdb.firebaseio.com"
#define FIREBASE_AUTH  ""    // jeton Firebase, vide si la base est ouverte
```

Paramètres de séchage modifiables en tête de `Codefinalsechoir.ino` :

| Constante | Valeur | Rôle |
|---|---|---|
| `CONSIGNE_PHASE_1_2` | 60 °C | Consigne des phases I et II |
| `CONSIGNE_PHASE_3` | 55 °C | Consigne de la phase III |
| `TEMP_SECURITE` | 65 °C | Coupure immédiate de la résistance |
| `E_SEUIL_PHASE_3` / `E_SEUIL_FIN` | 3 / 1 | Seuils d'écart d'humidité |
| `DUREE_MIN_AVANT_PHASES_MS` | 30 min | Délai avant tout changement de phase |
| `DUREE_CONFIRMATION_MS` | 10 min | Durée de confirmation d'un seuil |
| `FAN_PWM_MIN` / `FAN_PWM_MAX` | 50 / 255 | Plage du PWM du ventilateur |
| `heaterInterval` | 60 s | Durée minimale ON/OFF de la résistance |
| `dbInterval` | 60 s | Intervalle d'envoi Firebase |

### Téléversement

1. Carte : **ESP32 Dev Module**.
2. Compiler et téléverser `Codefinalsechoir.ino`.
3. Moniteur série à **115200 bauds** : une ligne d'état s'affiche toutes les 2 s (phase, consigne, température, PWM, chauffage, Wi-Fi).

---

## ☁️ Données envoyées

POST sur `<FIREBASE_HOST>/datas.json`, une entrée par minute :

```json
{
  "TSC": 52.40,
  "HSC": 35.00,
  "TSCH": 41.20,
  "HSCH": 58.00,
  "TEC": 31.50,
  "TICH": 48.75,
  "DateTime": "2025-05-22 14:32:05"
}
```

Un capteur en défaut est envoyé avec la valeur `null`.

---

## 🔐 Sécurité

- `secrets.h` contient le Wi-Fi et le jeton Firebase : il est listé dans `.gitignore` et ne doit jamais être publié.
- Seul `secrets.example.h` (sans valeurs réelles) est versionné.


