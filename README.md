# DYÁL3 — Tesla CAN Controller via M5Stack DIAL

🇫🇷 Français — [🇬🇧 English version below](#english-version)

Contrôleur de bus CAN pour Tesla Model Y, basé sur un **M5Stack DIAL** (molette rotative avec écran rond) connecté sans fil à un relais **ESP32-C3** branché directement sur le bus CAN du véhicule.

---

## Vue d'ensemble

DYÁL3 permet de déclencher des actions sur une Tesla Model Y (coffre, frunk, klaxon, verrouillage, HVAC, chauffage batterie, etc.) depuis une petite molette rotative posée dans l'habitacle, sans aucun câble entre le contrôleur et le véhicule.

```
Tesla CAN bus
      │
  ESP32-C3 "MAJOR"          WiFi caché (Dyal3-CAN)
  SN65HVD230 × 2    ◄────────────────────────────►   M5Stack DIAL
  Bus A (Vehicle)                                     Molette + écran
  Bus B (second)             HTTP / UDP push          Interface utilisateur
```

---

## Matériel requis

| Composant | Rôle |
|-----------|------|
| **M5Stack DIAL** | Interface utilisateur (écran GC9A01 240×240, molette encodeur, touch FT3267, buzzer) |
| **ESP32-C3** | Relais CAN — connecté physiquement au bus CAN Tesla |
| **SN65HVD230 × 2** | Transceivers CAN 3.3V (Bus A et Bus B) |
| Câblage OBD / harness | Accès au bus CAN du véhicule |

---

## Fonctionnalités

### Contrôle véhicule
- **Coffre arrière** (ouverture / fermeture)
- **Frunk** (ouverture)
- **Klaxon**
- **Verrouillage / déverrouillage** des portes
- **Rétroviseurs** (replier / déployer)

### Interface M5Stack DIAL
- Navigation par **molette rotative** entre les actions configurées
- **Écran tactile** configurable : simple tap, double tap, ou désactivé
- **Cercle de statut** vert/rouge autour de chaque action (connexion Major)
- **Appui long 2s** : accès au menu secondaire (BRIGHT, CAN-A, CAN-B, REBOOT, EXIT)
- **Réglage luminosité** en temps réel depuis la molette
- **Moniteur CAN** temps réel Bus A et Bus B

### Connexion et réseau
- Le M5Stack se connecte automatiquement au Major au démarrage
- Reconnexion automatique après 2 pings consécutifs sans réponse (max 3s)
- AP interne `DYAL3-M5` pour accès à la page de configuration
- Mode SETUP accessible via menu long : affiche l'IP locale ou active un AP de secours

### Configuration web
Accessible sur `http://192.168.10.1/config` (ou l'IP locale si connecté au réseau maison) :

- **Page principale** : statut Major, luminosité, mode tactile, actions rapides, système
- **Page `/slots`** : configuration des 10 slots de la molette par drag & drop, choix de couleur, action secondaire (double-clic)
- **OTA** : mise à jour firmware sans fil

---

## Architecture logicielle

### Fichiers

| Fichier | Description |
|---------|-------------|
| `dyal3.ino` | Firmware M5Stack DIAL — interface, affichage, réseau, HTTP |
| `dyal3_network.h` | Couche réseau non-bloquante (FreeRTOS queue, HTTP async, UDP push) |
| `dyal3_esp32c3.ino` | Firmware ESP32-C3 — relais CAN, AP WiFi caché, HTTP/UDP |
| `secrets.h` | Identifiants WiFi et clés WPA2 (ne pas committer) |
| `secrets.h.example` | Modèle à copier pour créer `secrets.h` |

### Protocole de communication

**Envoi d'une trame CAN** (M5 → Major) :
```
GET http://192.168.20.1/?bus=A&id=0x273&data=C1A00000C8023002
```

**Réception de trames CAN** (Major → M5) via UDP push :
```
BUS_A:0x273:C1A00000C8023002\n
```

---

## Installation

### Prérequis
- Arduino IDE 1.8.x ou 2.x
- Board M5Stack ESP32 (via gestionnaire de cartes)
- Board ESP32C3 Dev Module
- Bibliothèque M5GFX

### Étapes

1. Cloner le dépôt
2. Copier `secrets.h.example` en `secrets.h` et renseigner les valeurs
3. Flasher `dyal3_esp32c3.ino` sur l'ESP32-C3 (USB CDC On Boot : Enabled)
4. Flasher `dyal3.ino` sur le M5Stack DIAL
5. Alimenter l'ESP32-C3 et le connecter au bus CAN
6. Allumer le M5Stack — il se connecte automatiquement au Major

### Paramètres réseau par défaut

| Paramètre | Valeur |
|-----------|--------|
| SSID Major (caché) | `Dyal3-CAN` |
| IP Major | `192.168.20.1` |
| AP config M5 | `DYAL3-M5` |
| IP config M5 | `192.168.10.1` |
| Page config | `http://192.168.10.1/config` |

---

## Sécurité

- L'AP du Major est en mode caché (SSID non diffusé) + WPA2
- L'accès à la page de configuration est restreint au mode AP actif

---

## Compatibilité

Testé sur **Tesla Model Y** (bus Vehicle/ETH, 500 kbps). Les identifiants CAN et les trames sont issus du projet **opendbc** et de captures réelles. Certaines actions peuvent varier selon l'année de fabrication et la version du firmware Tesla.

---

## Licence

Projet personnel, usage privé. Inspiré de la communauté Tesla hacking (opendbc, Commander).

> ⚠️ L'utilisation de ce système sur le bus CAN d'un véhicule en circulation est de votre responsabilité. Certaines commandes peuvent affecter la conduite ou la sécurité.

---

---

# English version

🇬🇧 English — [🇫🇷 Version française en haut](#dyál3--tesla-can-controller-via-m5stack-dial)

CAN bus controller for Tesla Model Y, based on an **M5Stack DIAL** (rotary knob with round screen) connected wirelessly to an **ESP32-C3** relay plugged directly into the vehicle's CAN bus.

---

## Overview

DYÁL3 allows triggering actions on a Tesla Model Y (trunk, frunk, horn, lock/unlock, HVAC, battery heating, etc.) from a small rotary knob placed inside the cabin, with no cable between the controller and the vehicle.

```
Tesla CAN bus
      │
  ESP32-C3 "MAJOR"          Hidden WiFi (Dyal3-CAN)
  SN65HVD230 × 2    ◄────────────────────────────►   M5Stack DIAL
  Bus A (Vehicle)                                     Rotary knob + screen
  Bus B (second)             HTTP / UDP push          User interface
```

---

## Required Hardware

| Component | Role |
|-----------|------|
| **M5Stack DIAL** | User interface (GC9A01 240×240 screen, encoder knob, FT3267 touch, buzzer) |
| **ESP32-C3** | CAN relay — physically connected to the Tesla CAN bus |
| **SN65HVD230 × 2** | 3.3V CAN transceivers (Bus A and Bus B) |
| OBD cable / harness | Access to the vehicle's CAN bus |

---

## Features

### Vehicle Control
- **Rear trunk** (open / close)
- **Frunk** (open)
- **Horn**
- **Lock / unlock** doors
- **Mirrors** (fold / unfold)

### M5Stack DIAL Interface
- Navigation via **rotary knob** between configured actions
- **Touchscreen** configurable: single tap, double tap, or disabled
- **Status ring** green/red around each action (Major connection)
- **2s long press**: access to secondary menu (BRIGHT, CAN-A, CAN-B, REBOOT, EXIT)
- **Brightness adjustment** in real time from the knob
- **CAN monitor** live Bus A and Bus B

### Connectivity and Network
- M5Stack connects automatically to Major at startup
- Automatic reconnection after 2 consecutive failed pings (max 3s)
- Internal AP `DYAL3-M5` for access to the configuration page
- SETUP mode accessible via long press menu: displays local IP or activates a fallback AP

### Web Configuration
Accessible at `http://192.168.10.1/config` (or local IP if connected to home network):

- **Main page**: Major status, brightness, touch mode, quick actions, system
- **`/slots` page**: configure the 10 knob slots by drag & drop, color picker, secondary action (double-click)
- **OTA**: wireless firmware update

---

## Software Architecture

### Files

| File | Description |
|------|-------------|
| `dyal3.ino` | M5Stack DIAL firmware — UI, display, network, HTTP |
| `dyal3_network.h` | Non-blocking network layer (FreeRTOS queue, async HTTP, UDP push) |
| `dyal3_esp32c3.ino` | ESP32-C3 firmware — CAN relay, hidden WiFi AP, HTTP/UDP |
| `secrets.h` | WiFi credentials and WPA2 keys (do not commit) |
| `secrets.h.example` | Template to copy for creating `secrets.h` |

### Communication Protocol

**Send a CAN frame** (M5 → Major):
```
GET http://192.168.20.1/?bus=A&id=0x273&data=C1A00000C8023002
```

**Receive CAN frames** (Major → M5) via UDP push:
```
BUS_A:0x273:C1A00000C8023002\n
```

---

## Installation

### Prerequisites
- Arduino IDE 1.8.x or 2.x
- M5Stack ESP32 board package (via board manager)
- ESP32C3 Dev Module board package
- M5GFX library

### Steps

1. Clone the repository
2. Copy `secrets.h.example` to `secrets.h` and fill in your values
3. Flash `dyal3_esp32c3.ino` to the ESP32-C3 (USB CDC On Boot: Enabled)
4. Flash `dyal3.ino` to the M5Stack DIAL
5. Power the ESP32-C3 and connect it to the CAN bus
6. Turn on the M5Stack — it connects automatically to Major

### Default Network Settings

| Parameter | Value |
|-----------|-------|
| Major SSID (hidden) | `Dyal3-CAN` |
| Major IP | `192.168.20.1` |
| M5 config AP | `DYAL3-M5` |
| M5 config IP | `192.168.10.1` |
| Config page | `http://192.168.10.1/config` |

---

## Security

- Major AP is in hidden mode (SSID not broadcast) + WPA2
- Access to the configuration page is restricted to active AP mode

---

## Compatibility

Tested on **Tesla Model Y** (Vehicle/ETH bus, 500 kbps). CAN IDs and frames are sourced from the **opendbc** project and real captures. Some actions may vary depending on the manufacturing year and Tesla firmware version.

---

## License

Personal project, private use. Inspired by the Tesla hacking community (opendbc, Commander).

> ⚠️ Using this system on a vehicle's CAN bus while driving is your own responsibility. Some commands may affect driving or safety.
