#!/usr/bin/env python3
"""
detect.py -- Detection Tesla Model Y via PCAN-USB
0x3F5 : phares / veilleuses / eteint
0x3E2 : clignotant gauche / droit / warning / off
0x102 b0=0x21 : porte AV gauche
0x103 b0=0x21 : porte AV droite
0x102 b0=0x12 : porte AR gauche
0x103 b0=0x12 : porte AR droite
"""

import can
import time

BITRATE  = 500000
CHANNEL  = "PCAN_USBBUS1"

PHARES         = bytes.fromhex("0000C838840C0000")
VEILLEUSES     = bytes.fromhex("0000C838810C0000")
ETEINT_3F5     = bytes.fromhex("0000C838800C0000")

CLIGNO_DROITE  = bytes.fromhex("0000100000F000")
CLIGNO_GAUCHE  = bytes.fromhex("1000040000F000")
CLIGNO_WARNING = bytes.fromhex("1000140000F000")

CLIGNO_TIMEOUT = 0.9
PORTE_TIMEOUT  = 0.2


def main():
    bus = can.Bus(interface="pcan", channel=CHANNEL, bitrate=BITRATE)
    print("Detection Tesla -- en attente (Ctrl+C)\n")

    state_lights   = None
    state_cligno   = None
    last_cligno    = 0.0

    portes = {
        "AV GAUCHE": {"state": None, "last": 0.0},
        "AV DROITE": {"state": None, "last": 0.0},
        "AR GAUCHE": {"state": None, "last": 0.0},
        "AR DROITE": {"state": None, "last": 0.0},
    }

    try:
        while True:
            msg = bus.recv(timeout=0.1)
            now = time.monotonic()

            if state_cligno is not None and (now - last_cligno) > CLIGNO_TIMEOUT:
                state_cligno = None
                print("CLIGNO OFF")

            for nom, p in portes.items():
                if p["state"] == "OUVERTE" and (now - p["last"]) > PORTE_TIMEOUT:
                    p["state"] = "FERMEE"
                    print(f"PORTE {nom} FERMEE")

            if msg is None:
                continue

            data = bytes(msg.data)
            aid  = msg.arbitration_id

            if aid == 0x3F5:
                if data == PHARES and state_lights != "PHARES":
                    state_lights = "PHARES"
                    print("PHARES ON")
                elif data == VEILLEUSES and state_lights != "VEILLEUSES":
                    state_lights = "VEILLEUSES"
                    print("VEILLEUSES ON")
                elif data == ETEINT_3F5 and state_lights not in (None, "ETEINT"):
                    state_lights = "ETEINT"
                    print("PHARES OFF")

            elif aid == 0x3E2:
                if data == CLIGNO_WARNING:
                    last_cligno = now
                    if state_cligno != "WARNING":
                        state_cligno = "WARNING"
                        print("WARNING")
                elif data == CLIGNO_DROITE:
                    last_cligno = now
                    if state_cligno != "DROITE":
                        state_cligno = "DROITE"
                        print("CLIGNO DROITE")
                elif data == CLIGNO_GAUCHE:
                    last_cligno = now
                    if state_cligno != "GAUCHE":
                        state_cligno = "GAUCHE"
                        print("CLIGNO GAUCHE")

            elif aid == 0x102 and len(data) >= 1:
                if data[0] == 0x21:
                    p = portes["AV GAUCHE"]
                    p["last"] = now
                    if p["state"] != "OUVERTE":
                        p["state"] = "OUVERTE"
                        print("PORTE AV GAUCHE OUVERTE")
                elif data[0] == 0x12:
                    p = portes["AR GAUCHE"]
                    p["last"] = now
                    if p["state"] != "OUVERTE":
                        p["state"] = "OUVERTE"
                        print("PORTE AR GAUCHE OUVERTE")

            elif aid == 0x103 and len(data) >= 1:
                if data[0] == 0x21:
                    p = portes["AV DROITE"]
                    p["last"] = now
                    if p["state"] != "OUVERTE":
                        p["state"] = "OUVERTE"
                        print("PORTE AV DROITE OUVERTE")
                elif data[0] == 0x12:
                    p = portes["AR DROITE"]
                    p["last"] = now
                    if p["state"] != "OUVERTE":
                        p["state"] = "OUVERTE"
                        print("PORTE AR DROITE OUVERTE")

    except KeyboardInterrupt:
        print("\nArret.")
    finally:
        bus.shutdown()

if __name__ == "__main__":
    main()
