#!/usr/bin/env python3
"""
tesla_can.py -- Commandes CAN Tesla Model Y via PCAN-USB (PEAK System)
Bus : Vehicle CAN | 500 kbps

Usage : python3 tesla_can.py <action>
        python3 tesla_can.py listen [duree_secondes] [0xID_filtre]
        python3 tesla_can.py -h

Actions confirmees (0x273) :
  trunk, frunk, horn, lock, unlock
  mirror_retract, mirror_present, mirror_heat
  lights_on, lights_off, lights_parking, lights_auto
  fog, fog_rear, highbeam, hazard
  wiper_off, wiper_auto, wiper_slow, wiper_fast
  interior_light_on (reactif -- Ctrl+C pour eteindre)
  heat_fl_1/2/3/off, heat_rl_1/2/off, heat_rr_1/2/off, heat_rc_1/off
  child_lock, alarm, summon, accessory, remote_start
  power_off   !! DANGER !!

Actions ID secondaires :
  flash, flash5            (0x3F5 -- appel de phares)
  charge_port_open/close   (0x333 -- trappe de charge)
  charge_enable            (0x333)
  precondition_on/off      (0x2F3 -- HVAC cabine 22C blower AUTO)
  defrost_front            (0x2F3 -- degivrage pare-brise)
  hvac_on                  (0x2E5 -- HVAC_Command mikegapinski)
  hvac_off                 (0x2E5)
  battery_heat             (0x082 -- prechauffage batterie, periodique 500ms, Ctrl+C pour eteindre)
"""

import can
import sys
import time

BITRATE     = 500000
CAN_ID      = 0x273
CHANNEL     = "PCAN_USBBUS1"
PULSE_MS    = 100
BASE_FRAME  = bytes.fromhex("C1A00000C8023002")  # 0x273 repos
LIGHT_ON    = bytes.fromhex("C1A00000C802300A")  # 0x273 lumiere bit59=1

REACT_DELAY = 0.003   # 3ms apres trame voiture (UI injecte a 2ms)

# ── IDs secondaires ──────────────────────────────────────────
FLASH_ID    = 0x3F5
FLASH_REPOS = bytes.fromhex("0000C81890040000")
FLASH_ACTIF = bytes.fromhex("0000C838800C0000")

HVAC_ID     = 0x2F3   # UI_hvacRequest  (5 octets)
HVAC2_ID    = 0x2E5   # HVAC_Command    (mikegapinski / Commander doc)
CHARGE_ID   = 0x333   # UI_chargeRequest
TRIP_ID     = 0x082   # UI_tripPlanning

# Trames HVAC 0x2F3 (5 octets) -- UI_hvacRequest
# UI_hvacReqUserPowerState bits 26|3 : 0=OFF 1=ON 2=PRECONDITION
# UI_hvacReqTempSetpoint   bits  0|5 : (degC-15)/0.5  => 22C = 14 = 0x0E
# UI_hvacReqBlowerSegment  bits 16|4 : 11=AUTO 0=OFF
HVAC_PRECOND = bytes.fromhex("0E0E0B0800")  # PRECONDITION 22C blower AUTO
HVAC_OFF     = bytes.fromhex("0E0E000000")  # OFF
HVAC_DEFROST = bytes.fromhex("0E0E0B0900")  # PRECONDITION + DEFOG

# Trames HVAC 0x2E5 (8 octets) -- HVAC_Command (source : mikegapinski)
# byte0 : power state  0x00=off  0x01=on
# bytes 1-7 : 0x00 (minimal -- a affiner par capture sur bus)
HVAC2_ON    = bytes.fromhex("0100000000000000")
HVAC2_OFF   = bytes.fromhex("0000000000000000")

ACTIONS = {

    # ── Impulsions simples 0x273 ──────────────────────────────
    "frunk":         {"bits": [5],         "desc": "Ouverture frunk (bit5)"},
    "horn":          {"bits": [61],        "desc": "Klaxon (bit61)"},
    "fog":           {"bits": [3],         "desc": "Feux brouillard avant (bit3)"},
    "fog_rear":      {"bits": [23],        "desc": "Feux brouillard arriere (bit23)"},
    "accessory":     {"bits": [0],         "desc": "Alimentation accessoires (bit0)"},
    "alarm":         {"bits": [20],        "desc": "Alarme ON (bit20)"},
    "ambient":       {"bits": [40],        "desc": "Eclairage ambiant (bit40)"},
    "highbeam":      {"bits": [41],        "desc": "Feux de route auto (bit41)"},
    "child_lock":    {"bits": [16],        "desc": "Verrouillage enfant portes (bit16)"},
    "summon":        {"bits": [4],         "desc": "Summon actif (bit4)"},
    "mirror_heat":   {"bits": [26],        "desc": "Chauffage retroviseurs (bit26)"},
    "hazard":        {"bits": [7],         "desc": "Feux de detresse via 0x273 bit7 (hypothese -- preferer body_hazard_on)"},
    "power_off":     {"bits": [31],        "desc": "!! DANGER !! Extinction vehicule (bit31)"},

    # ── DAS_bodyControls 0x3E9 (1001, 8 octets) -- opendbc confirme ──────
    # DAS_headlightRequest   bits 0|2  : 0=OFF 1=ON 3=INVALID
    # DAS_hazardLightRequest bits 2|2  : 0=OFF 1=ON 2=UNUSED 3=SNA
    # DAS_wiperSpeed         bits 4|4  : 0=OFF 1..14=vitesses 15=INVALID
    # DAS_turnIndicatorRequest bits 8|2: 0=NONE 1=LEFT 2=RIGHT 3=CANCEL
    # Counter bits 52|4 + Checksum bits 56|8 laisses a 0 pour test
    "body_hazard_on":   {"can_id": 0x3E9, "raw": bytes.fromhex("0400000000000000"),
                         "desc": "Feux de detresse ON  (0x3E9 hazard=1) -- DBC confirme"},
    "body_hazard_off":  {"can_id": 0x3E9, "raw": bytes.fromhex("0000000000000000"),
                         "desc": "Feux de detresse OFF (0x3E9 hazard=0)"},
    "body_lights_on":   {"can_id": 0x3E9, "raw": bytes.fromhex("0100000000000000"),
                         "desc": "Phares ON  (0x3E9 headlight=1)"},
    "body_lights_off":  {"can_id": 0x3E9, "raw": bytes.fromhex("0000000000000000"),
                         "desc": "Phares OFF (0x3E9 headlight=0)"},
    "body_wiper_auto":  {"can_id": 0x3E9, "raw": bytes.fromhex("1000000000000000"),
                         "desc": "Essuie-glaces AUTO speed=1  (0x3E9)"},
    "body_wiper_fast":  {"can_id": 0x3E9, "raw": bytes.fromhex("6000000000000000"),
                         "desc": "Essuie-glaces RAPIDE speed=6 (0x3E9)"},
    "body_wiper_off":   {"can_id": 0x3E9, "raw": bytes.fromhex("0000000000000000"),
                         "desc": "Essuie-glaces OFF (0x3E9 wiper=0)"},
    "body_turn_left":   {"can_id": 0x3E9, "raw": bytes.fromhex("0001000000000000"),
                         "desc": "Clignotant GAUCHE (0x3E9 turn=LEFT)"},
    "body_turn_right":  {"can_id": 0x3E9, "raw": bytes.fromhex("0002000000000000"),
                         "desc": "Clignotant DROIT  (0x3E9 turn=RIGHT)"},
    "body_turn_off":    {"can_id": 0x3E9, "raw": bytes.fromhex("0003000000000000"),
                         "desc": "Clignotant OFF    (0x3E9 turn=CANCEL)"},

    # ── Champs multi-bits 0x273 ───────────────────────────────
    "trunk":         {"field": (54, 2, 1), "desc": "Coffre arriere (UI_remoteClosureRequest=1)"},
    "frunk_v2":      {"field": (54, 2, 2), "desc": "Frunk via remoteClosureRequest=2"},
    "lock":          {"field": (17, 3, 1), "desc": "Verrouillage (UI_lockRequest=LOCK)"},
    "unlock":        {"field": (17, 3, 2), "desc": "Deverrouillage (UI_lockRequest=UNLOCK)"},
    "remote_lock":   {"field": (17, 3, 4), "desc": "Verrouillage telecommande"},
    "remote_unlock": {"field": (17, 3, 3), "desc": "Deverrouillage telecommande"},
    "remote_start":  {"field": (27, 3, 1), "desc": "Demarrage a distance"},

    "mirror_retract":{"field": (24, 2, 1), "desc": "Replier retroviseurs (UI_mirrorFoldRequest=1)"},
    "mirror_present":{"field": (24, 2, 2), "desc": "Deployer retroviseurs (UI_mirrorFoldRequest=2)"},

    "lights_on":     {"field": (1, 2, 1),  "desc": "Phares ON (UI_lightSwitch=1)"},
    "lights_parking":{"field": (1, 2, 2),  "desc": "Feux de position (UI_lightSwitch=2)"},
    "lights_off":    {"field": (1, 2, 3),  "desc": "Phares OFF (UI_lightSwitch=3)"},
    "lights_auto":   {"field": (1, 2, 0),  "desc": "Phares AUTO (UI_lightSwitch=0)"},

    "wiper_off":     {"field": (56, 3, 1), "desc": "Essuie-glaces OFF"},
    "wiper_auto":    {"field": (56, 3, 2), "desc": "Essuie-glaces AUTO"},
    "wiper_slow":    {"field": (56, 3, 5), "desc": "Essuie-glaces lent continu"},
    "wiper_fast":    {"field": (56, 3, 6), "desc": "Essuie-glaces rapide continu"},

    # ── Sieges chauffants 0x273 ───────────────────────────────
    "heat_fl_1":     {"field": (42, 2, 1), "desc": "Siege AV gauche niveau 1"},
    "heat_fl_2":     {"field": (42, 2, 2), "desc": "Siege AV gauche niveau 2"},
    "heat_fl_3":     {"field": (42, 2, 3), "desc": "Siege AV gauche niveau 3"},
    "heat_fl_off":   {"field": (42, 2, 0), "desc": "Siege AV gauche OFF"},
    "heat_rl_1":     {"field": (46, 2, 1), "desc": "Siege AR gauche niveau 1"},
    "heat_rl_2":     {"field": (46, 2, 2), "desc": "Siege AR gauche niveau 2"},
    "heat_rl_off":   {"field": (46, 2, 0), "desc": "Siege AR gauche OFF"},
    "heat_rr_1":     {"field": (50, 2, 1), "desc": "Siege AR droit niveau 1"},
    "heat_rr_2":     {"field": (50, 2, 2), "desc": "Siege AR droit niveau 2"},
    "heat_rr_off":   {"field": (50, 2, 0), "desc": "Siege AR droit OFF"},
    "heat_rc_1":     {"field": (48, 2, 1), "desc": "Siege AR centre niveau 1"},
    "heat_rc_off":   {"field": (48, 2, 0), "desc": "Siege AR centre OFF"},

    # ── Reactif 0x273 ─────────────────────────────────────────
    "interior_light_on":  {"reactive": LIGHT_ON,  "desc": "Plafonnier ON (reactif -- Ctrl+C)"},
    "interior_light_off": {"reactive": BASE_FRAME, "desc": "Plafonnier OFF (restaure base)"},

    # ── Appel de phares 0x3F5 ─────────────────────────────────
    "flash":   {"can_id": FLASH_ID, "raw": FLASH_ACTIF, "raw_reset": FLASH_REPOS,
                "pulse_ms": 100, "desc": "Appel de phares 1x"},
    "flash5":  {"can_id": FLASH_ID, "raw": FLASH_ACTIF, "raw_reset": FLASH_REPOS,
                "pulse_ms": 100, "repeat": 5, "gap_ms": 500, "desc": "5x appels de phares"},

    # ── Port de charge 0x333 ──────────────────────────────────
    "charge_port_open":  {"can_id": CHARGE_ID, "raw": bytes.fromhex("01000000"),
                          "desc": "Ouvrir trappe de charge (bit0)"},
    "charge_port_close": {"can_id": CHARGE_ID, "raw": bytes.fromhex("02000000"),
                          "desc": "Fermer trappe de charge (bit1)"},
    "charge_enable":     {"can_id": CHARGE_ID, "raw": bytes.fromhex("04000000"),
                          "desc": "Activer charge (UI_chargeEnableRequest bit2)"},

    # ── HVAC cabine 0x2F3 (5 octets) ─────────────────────────
    "precondition_on":  {"can_id": HVAC_ID, "raw": HVAC_PRECOND,
                         "desc": "HVAC PRECONDITION ON 22C blower AUTO (0x2F3)"},
    "precondition_off": {"can_id": HVAC_ID, "raw": HVAC_OFF,
                         "desc": "HVAC OFF (0x2F3)"},
    "defrost_front":    {"can_id": HVAC_ID, "raw": HVAC_DEFROST,
                         "desc": "Degivrage pare-brise DEFOG+PRECONDITION (0x2F3)"},

    # ── HVAC_Command 0x2E5 (8 octets) -- mikegapinski/Commander ─
    # A TESTER -- byte0=0x01=ON  byte0=0x00=OFF
    # Capturer d'abord avec : python3 tesla_can.py listen 10 0x2E5
    "hvac_on":  {"can_id": HVAC2_ID, "raw": HVAC2_ON,
                 "desc": "HVAC ON via 0x2E5 HVAC_Command (a valider par capture)"},
    "hvac_off": {"can_id": HVAC2_ID, "raw": HVAC2_OFF,
                 "desc": "HVAC OFF via 0x2E5 HVAC_Command (a valider par capture)"},

    # ── Prechauffage batterie 0x082 (periodique) ─────────────
    # bit0=UI_tripPlanningActive  bit2=UI_requestActiveBatteryHeating
    # Source : tuncasoftbildik / Commander v2.3.0
    # DOIT etre envoye toutes les 500ms -- mode periodique ci-dessous
    "battery_heat": {"periodic": bytes.fromhex("0500000000000000"),
                     "can_id": TRIP_ID, "interval_ms": 500,
                     "desc": "Prechauffage batterie 0x082 (periodique 500ms -- Ctrl+C)"},
}


# ─────────────────────────────────────────────────────────────
def write_field(data, start, length, value):
    """Ecrit un champ Little Endian dans bytearray."""
    for i in range(length):
        bit = start + i
        if 0 <= bit <= 63:
            data[bit // 8] &= ~(1 << (bit % 8))
    for i in range(length):
        if (value >> i) & 1:
            bit = start + i
            if 0 <= bit <= 63:
                data[bit // 8] |= (1 << (bit % 8))


def build_frame(action):
    if "raw" in action:
        return action["raw"]
    can_id = action.get("can_id", CAN_ID)
    data = bytearray(BASE_FRAME) if can_id == CAN_ID else bytearray(8)
    if "bits" in action:
        for bit in action["bits"]:
            if 0 <= bit <= 63:
                data[bit // 8] |= (1 << (bit % 8))
    elif "field" in action:
        write_field(data, *action["field"])
    return bytes(data)


def open_bus():
    try:
        return can.Bus(interface="pcan", channel=CHANNEL, bitrate=BITRATE)
    except can.CanError as e:
        print("ERREUR : Impossible d'ouvrir le PCAN-USB ({})".format(e))
        sys.exit(1)


def send_reactive(target_frame):
    """Mode reactif : injecte apres chaque trame 0x273 voiture."""
    print("Mode REACTIF 0x273 -- injection apres chaque trame voiture")
    print("Cible  : {}".format(target_frame.hex().upper()))
    print("Ctrl+C pour arreter\n")
    bus = open_bus()
    inject_msg  = can.Message(arbitration_id=CAN_ID, data=target_frame, is_extended_id=False)
    restore_msg = can.Message(arbitration_id=CAN_ID, data=BASE_FRAME,   is_extended_id=False)
    count = 0
    try:
        while True:
            msg = bus.recv(timeout=1.0)
            if msg is None or msg.arbitration_id != CAN_ID:
                continue
            if bytes(msg.data) == target_frame:
                continue
            time.sleep(REACT_DELAY)
            bus.send(inject_msg)
            count += 1
            print("  [{}] inject apres {}  (x{})".format(
                time.strftime("%H:%M:%S"), bytes(msg.data).hex().upper(), count), end="\r")
    except KeyboardInterrupt:
        print("\n\nArret -- restauration BASE_FRAME")
        for _ in range(5):
            try:
                bus.send(restore_msg)
                time.sleep(0.05)
            except can.CanError:
                pass
        print("OK")
    except can.CanError as e:
        print("ERREUR CAN : " + str(e))
    finally:
        bus.shutdown()


def send_periodic(action):
    """Mode periodique : envoie une trame toutes les N ms jusqu'a Ctrl+C."""
    arb_id      = action.get("can_id", CAN_ID)
    frame       = action["periodic"]
    interval_s  = action.get("interval_ms", 500) / 1000.0
    reset_frame = bytes(len(frame))

    print("Mode PERIODIQUE 0x{:03X} -- envoi toutes les {}ms".format(arb_id, int(interval_s*1000)))
    print("Trame  : {}".format(frame.hex().upper()))
    print("Ctrl+C pour arreter\n")

    bus = open_bus()
    msg       = can.Message(arbitration_id=arb_id, data=frame,       is_extended_id=False)
    reset_msg = can.Message(arbitration_id=arb_id, data=reset_frame, is_extended_id=False)
    count = 0
    try:
        while True:
            bus.send(msg)
            count += 1
            print("  [{}] envoi #{} -- {}".format(
                time.strftime("%H:%M:%S"), count, frame.hex().upper()), end="\r")
            time.sleep(interval_s)
    except KeyboardInterrupt:
        print("\n\nArret -- envoi trame zero")
        for _ in range(3):
            try:
                bus.send(reset_msg)
                time.sleep(0.05)
            except can.CanError:
                pass
        print("OK")
    except can.CanError as e:
        print("ERREUR CAN : " + str(e))
    finally:
        bus.shutdown()


def send_action(action_name):
    if action_name not in ACTIONS:
        print("ERREUR : Action inconnue : " + action_name)
        print("  Disponibles : " + ", ".join(sorted(ACTIONS)))
        sys.exit(1)

    action = ACTIONS[action_name]
    arb_id = action.get("can_id", CAN_ID)
    print("Action : {} -- {}".format(action_name, action["desc"]))

    if "reactive" in action:
        send_reactive(action["reactive"])
        return

    if "periodic" in action:
        send_periodic(action)
        return

    bus    = open_bus()
    frame  = build_frame(action)
    print("Trame  : 0x{:03X}#{}".format(arb_id, frame.hex().upper()))

    if "raw_reset" in action:
        reset_frame = action["raw_reset"]
    elif arb_id == CAN_ID:
        reset_frame = BASE_FRAME
    else:
        reset_frame = bytes(len(frame))

    repeat = action.get("repeat", 1)
    pulse  = action.get("pulse_ms", PULSE_MS)
    gap_ms = action.get("gap_ms", 500)

    try:
        for i in range(repeat):
            bus.send(can.Message(arbitration_id=arb_id, data=frame,       is_extended_id=False))
            time.sleep(pulse / 1000)
            bus.send(can.Message(arbitration_id=arb_id, data=reset_frame, is_extended_id=False))
            if i < repeat - 1:
                time.sleep(gap_ms / 1000)
        print("OK ({} x {}ms)".format(repeat, pulse))
    except KeyboardInterrupt:
        print("\nInterrompu.")
    except can.CanError as e:
        print("ERREUR CAN : " + str(e))
    finally:
        bus.shutdown()


def listen_bus(duration=30, filter_id=None):
    filter_str = " (filtre: 0x{:03X})".format(filter_id) if filter_id else ""
    print("Ecoute {}s @ {}kbps{}... (Ctrl+C pour arreter)\n".format(
        duration, BITRATE // 1000, filter_str))
    bus = open_bus()
    end = time.time() + duration
    try:
        while time.time() < end:
            msg = bus.recv(timeout=1.0)
            if msg:
                if filter_id and msg.arbitration_id != filter_id:
                    continue
                flag = " <-- UI_vehicleControl" if msg.arbitration_id == CAN_ID else ""
                print("[{:.3f}] 0x{:03X}  {}{}".format(
                    msg.timestamp, msg.arbitration_id,
                    msg.data.hex().upper(), flag))
    except KeyboardInterrupt:
        print("\nArret.")
    finally:
        bus.shutdown()


def print_help():
    print(__doc__)
    print("Actions disponibles ({}) :".format(len(ACTIONS)))
    for k, v in sorted(ACTIONS.items()):
        if "reactive"  in v: detail = "reactif"
        elif "periodic" in v: detail = "periodique {}ms".format(v.get("interval_ms",500))
        elif "bits"    in v: detail = "bits " + ",".join(str(b) for b in v["bits"])
        elif "field"   in v: detail = "field start={} len={} val={}".format(*v["field"])
        else:                detail = "raw 0x{:03X}".format(v.get("can_id", CAN_ID))
        print("  {:<22} {}  [{}]".format(k, v["desc"], detail))


def monitor_bus():
    """
    Decodage en temps reel des trames de monitoring Tesla.
    Sources : opendbc Model3CAN.dbc + GTW_carState 0x318
    Ctrl+C pour arreter.
    """
    print("Monitor CAN Tesla -- decodage en temps reel (Ctrl+C pour arreter)\n")
    bus = open_bus()

    doors_prev = {}

    try:
        while True:
            msg = bus.recv(timeout=1.0)
            if msg is None:
                continue

            d   = msg.data
            aid = msg.arbitration_id

            # ── 0x318 (792) GTW_carState -- portes + OTA ─────────
            if aid == 0x318 and len(d) >= 7:
                fl = (d[1] >> 4) & 0x03
                fr = (d[1] >> 6) & 0x03
                rl = (d[2] >> 6) & 0x03
                rr = (d[3] >> 5) & 0x03
                fk = (d[6] >> 2) & 0x03
                ota = (d[6] >> 0) & 0x03
                state = {"FL": fl, "FR": fr, "RL": rl, "RR": rr, "FRUNK": fk}
                if state != doors_prev:
                    doors_prev = state
                    s = "  ".join(f"{k}={'OPEN' if v==1 else 'CLS'}" for k, v in state.items())
                    ota_s = " *** OTA EN COURS ***" if ota == 1 else ""
                    print(f"[0x318] Portes: {s}{ota_s}")

            # ── 0x283 (643) BODY_R1 -- temperatures ──────────────
            elif aid == 0x283 and len(d) >= 8:
                temp_out  = d[0] * 0.5 - 40
                temp_in   = d[1] * 0.25
                print(f"[0x283] Temp ext={temp_out:.1f}C  int={temp_in:.1f}C")

            # ── 0x132 (306) BMS_hvBusStatus -- tension/courant ───
            elif aid == 0x132 and len(d) >= 4:
                voltage = ((d[1] << 8) | d[0]) * 0.01
                current = int.from_bytes(d[2:4], 'little', signed=True) * 0.1
                print(f"[0x132] BMS: {voltage:.1f}V  {current:.1f}A")

            # ── 0x292 (658) BMS_socStatus -- SOC ─────────────────
            elif aid == 0x292 and len(d) >= 2:
                soc = (((d[1] & 0x03) << 8) | d[0]) * 0.1
                print(f"[0x292] SOC: {soc:.1f}%")

            # ── 0x118 (280) DI_torque2 -- vitesse + gear ─────────
            elif aid == 0x118 and len(d) >= 6:
                speed_raw = ((d[3] & 0x0F) << 8) | d[2]
                if speed_raw != 0xFFF:
                    speed = speed_raw * 0.05 - 25
                    gear_raw = (d[1] >> 4) & 0x07
                    gears = {0: "?", 1: "P", 2: "R", 3: "N", 4: "D", 7: "SNA"}
                    gear = gears.get(gear_raw, str(gear_raw))
                    print(f"[0x118] Vitesse: {speed:.1f}mph  Gear: {gear}")

    except KeyboardInterrupt:
        print("\nArret monitor.")
    finally:
        bus.shutdown()


if __name__ == "__main__":
    if len(sys.argv) < 2 or sys.argv[1] in ("-h", "--help"):
        print_help()
        sys.exit(0)

    cmd = sys.argv[1].lower()

    if cmd == "listen":
        duration  = 30
        filter_id = None
        for a in sys.argv[2:]:
            if a.lower().startswith("0x"):
                filter_id = int(a, 16)
            else:
                try: duration = int(a)
                except ValueError: pass
        listen_bus(duration, filter_id)
    elif cmd == "monitor":
        monitor_bus()
    else:
        send_action(cmd)
