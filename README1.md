  # DYÁL3 — Tesla CAN Controller via M5Stack DIAL

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
