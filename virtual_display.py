"""
MERAZ BATTLESHIP - VIRTUAL TFT DISPLAY & HARDWARE BRIDGE
=========================================================
Run this on your laptop to simulate the 2.4" ILI9341 touch display:
    python virtual_display.py
Or with a specific port:
    python virtual_display.py --port COM4 --team 1

What this script does:
  1. Mirrors the 240x320 touch UI of participant_node.ino (same pixel
     coordinates, buttons, colours and timing) in a desktop window.
  2. Connects over USB Serial to an ESP32 flashed with participant_node.ino.
     ZERO code changes are needed on participant_node.ino!
  3. BEFORE the board talks to Central, you choose your LOADOUT here
     (2 attack powers + 1 defence power) and press LOCK IN. The window then
     sends "LOADOUT ..." to the board, which registers with Central.
  4. On your turn you Single-Strike (pick a team, a cell, ATTACK) or press
     POWERS and pick a power: Sonar, Salvo, Mine, Double Attack, Smoke
     Screen, Repair - or the Shield, which every team has. The window sends the
     matching "t x y" or "POWER ..." command to the board.
  5. The ESP32 sends it over ESP-NOW to Central, receives the reply and
     prints it on Serial. This script parses the reply and shows the same
     result banner the real display would (HIT, MISS, ELIMINATED, MINE,
     BLOCKED, NO REPORT, or the power's result).
  6. Without any hardware the quick-test buttons still let you look at
     every screen and click through the flows ("Simulation Mode").

Serial lines this window understands (all printed by participant_node.ino):
  [LOADOUT] ...            the board is waiting for a loadout
  Loadout locked: ...      (byte 0xNN) the loadout is set
  >>> YOUR TURN <<<       Waiting - it is TEAM n's turn.       GAME OVER
  RESULT: TEAM t (x,y) -> HIT | MISS | MINE | BLOCKED | UNKNOWN | INVALID ...
  POWER_RESULT: power=P status=S target=T count=N r=a,b,c
  [UI] message            ship n: len=L start=(x,y) HORIZONTAL   (from CONFIG)
"""

import sys
import time
import threading
import argparse
import re
import tkinter as tk
from tkinter import ttk, messagebox

try:
    import serial
    import serial.tools.list_ports
    SERIAL_AVAILABLE = True
except ImportError:
    SERIAL_AVAILABLE = False


# =====================================================================
# CONSTANTS - the same numbers as participant_node.ino
# =====================================================================
BASE_WIDTH = 240
BASE_HEIGHT = 320
SCALE = 1.5  # 1.5x zoom -> 360x480 window for comfortable desktop viewing

UI_RESULT_DISPLAY_MS = 3500
UI_POWER_RESULT_MS = 4000
UI_SONAR_RESULT_MS = 8000

UI_TARGET_BTN_Y = 36
UI_TARGET_BTN_H = 30
UI_TARGET_BTN_W = 70
UI_TARGET_GAP   = 10

UI_GRID_X       = 22
UI_GRID_Y       = 74
UI_CELL_SIZE    = 28
GRID_SIZE       = 7

UI_ALT_BTN_X    = 10
UI_ALT_BTN_W    = 84
UI_ATTACK_BTN_X = 104
UI_ATTACK_BTN_Y = 280
UI_ATTACK_BTN_W = 126
UI_ATTACK_BTN_H = 28

UI_MENU_X, UI_MENU_W, UI_MENU_Y0, UI_MENU_H, UI_MENU_GAP = 10, 220, 36, 50, 6

LD_BTN_W, LD_BTN_H = 108, 40
LD_COL1_X, LD_COL2_X = 8, 124
LD_ATK_Y1, LD_ATK_Y2, LD_DEF_Y = 56, 100, 158
LD_OK_X, LD_OK_Y, LD_OK_W, LD_OK_H = 40, 264, 160, 40

# Guided ship placement wizard coordinates
WIZ_GRID_X, WIZ_GRID_Y, WIZ_CELL_SIZE = 22, 56, 28

WIZ_ORIENTS = {
    0: [0, 1, 2, 3],
    1: [0, 1, 2, 3],
    2: [0, 1, 2, 3],
    3: [0, 1, 2, 3]
}

ORIENT_NAMES = {
    0: "HORIZONTAL", 1: "VERTICAL", 2: "DIAG DOWN", 3: "DIAG UP",
    4: "T-DOWN", 5: "T-UP", 6: "T-RIGHT", 7: "T-LEFT",
    8: "L-0 (VERT-R)", 9: "L-1 (HORIZ-D)", 10: "L-2 (VERT-L)", 11: "L-3 (HORIZ-U)",
    12: "J-0 (VERT-L)", 13: "J-1 (HORIZ-D)", 14: "J-2 (VERT-R)", 15: "J-3 (HORIZ-U)",
    16: "CORNER RD", 17: "CORNER LD", 18: "CORNER RU", 19: "CORNER LU"
}

# power ids (same as meraz_loadout.h)
SONAR, SALVO, MINE, REPAIR, SMOKE, SHIELD, DOUBLE = 1, 2, 3, 4, 5, 6, 7
POWER_USES = 2
LD_ATTACK_IDS = [SONAR, SALVO, MINE, DOUBLE]
LD_DEFENCE_IDS = [SMOKE, REPAIR]

# id -> (name, hint, needs a target team, cells are on MY board, how many cells, action button label)
SPECS = {
    SONAR:  ("SONAR",  "scan a 3x3 area",  True,  False, 1, "PING"),
    SALVO:  ("SALVO",  "3 in a line",      True,  False, 3, "FIRE x3"),
    MINE:   ("MINE",   "hidden trap",      False, True,  1, "PLACE MINE"),
    REPAIR: ("REPAIR", "fix a hit cell",   False, True,  1, "REPAIR"),
    SMOKE:  ("SMOKE",  "deflect all attacks", False, False, 0, "DEPLOY"),
    SHIELD: ("SHIELD", "block a 3x3 area", False, True,  1, "RAISE"),
    DOUBLE: ("DOUBLE", "2 shots at once",  True,  False, 2, "FIRE x2"),
}
MENU_ORDER = [SONAR, SALVO, MINE, DOUBLE, SMOKE, REPAIR]

DEFAULT_CONFIGS = [
    [(5, 0, 0, 0), (4, 0, 2, 0), (4, 0, 4, 0), (3, 5, 0, 1)],
    [(5, 0, 1, 1), (4, 2, 0, 1), (4, 4, 2, 1), (3, 6, 1, 1)],
    [(5, 0, 0, 2), (4, 0, 6, 0), (4, 5, 0, 1), (3, 2, 0, 0)],
    [(5, 0, 4, 3), (4, 0, 6, 0), (4, 6, 0, 1), (3, 0, 0, 0)],
    [(5, 1, 0, 0), (4, 0, 2, 1), (4, 6, 2, 1), (3, 2, 6, 0)],
    [(5, 5, 0, 1), (4, 0, 0, 2), (4, 0, 6, 0), (3, 4, 4, 1)],
    [(5, 0, 1, 0), (4, 1, 3, 0), (4, 0, 5, 0), (3, 6, 2, 1)],
    [(5, 0, 6, 0), (4, 1, 0, 1), (4, 3, 1, 1), (3, 5, 0, 1)],
    [(5, 0, 0, 1), (4, 1, 0, 0), (4, 2, 6, 0), (3, 6, 3, 1)],
    [(5, 1, 6, 0), (4, 0, 1, 2), (4, 5, 0, 1), (3, 0, 4, 0)],
]


# Colours (TFT_eSPI names, desktop-friendly shades)
BLACK, WHITE, NAVY, YELLOW, GREEN = "#000000", "#FFFFFF", "#000080", "#FFFF00", "#00FF00"
BLUE, ORANGE, RED, CYAN = "#0066FF", "#FFA500", "#FF0000", "#00FFFF"
DARKGREY, MIDGREY, LIGHTGREY, MAROON, PURPLE = "#404040", "#7B7D7B", "#C0C0C0", "#7B0000", "#7B007B"
DARKCYAN, DARKGREEN, BROWN = "#007B7B", "#007B00", "#804000"


def sx(v):
    return int(v * SCALE)


def sy(v):
    return int(v * SCALE)


def cells_form_line(cells):
    """Same rule as Central: n consecutive cells in one row, column, or diagonal."""
    n = len(cells)
    if n <= 1:
        return True
    s = sorted(cells, key=lambda p: (p[0], p[1]))
    dx = s[1][0] - s[0][0]
    dy = s[1][1] - s[0][1]
    if (dx, dy) not in ((1, 0), (0, 1), (1, 1), (1, -1)):
        return False
    for i in range(2, n):
        if s[i][0] - s[i - 1][0] != dx or s[i][1] - s[i - 1][1] != dy:
            return False
    return True

def get_ship_cells(ln, sx, sy, o):
    if o <= 3:
        dx, dy = {0: (1, 0), 1: (0, 1), 2: (1, 1), 3: (1, -1)}.get(o, (1, 0))
        return [(sx + dx * i, sy + dy * i) for i in range(ln)]
    if ln == 4:
        if o == 4: return [(sx, sy), (sx + 1, sy), (sx + 2, sy), (sx + 1, sy + 1)]
        if o == 5: return [(sx, sy + 1), (sx + 1, sy + 1), (sx + 2, sy + 1), (sx + 1, sy)]
        if o == 6: return [(sx, sy), (sx, sy + 1), (sx, sy + 2), (sx + 1, sy + 1)]
        if o == 7: return [(sx + 1, sy), (sx + 1, sy + 1), (sx + 1, sy + 2), (sx, sy + 1)]
        if o == 8: return [(sx, sy), (sx, sy + 1), (sx, sy + 2), (sx + 1, sy + 2)]
        if o == 9: return [(sx, sy), (sx + 1, sy), (sx + 2, sy), (sx, sy + 1)]
        if o == 10: return [(sx + 1, sy), (sx + 1, sy + 1), (sx + 1, sy + 2), (sx, sy)]
        if o == 11: return [(sx, sy + 1), (sx + 1, sy + 1), (sx + 2, sy + 1), (sx + 2, sy)]
        if o == 12: return [(sx + 1, sy), (sx + 1, sy + 1), (sx + 1, sy + 2), (sx, sy + 2)]
        if o == 13: return [(sx, sy), (sx + 1, sy), (sx + 2, sy), (sx + 2, sy + 1)]
        if o == 14: return [(sx, sy), (sx, sy + 1), (sx, sy + 2), (sx + 1, sy)]
        if o == 15: return [(sx, sy + 1), (sx + 1, sy + 1), (sx + 2, sy + 1), (sx, sy)]
    if ln == 3:
        if o == 16: return [(sx, sy), (sx + 1, sy), (sx, sy + 1)]
        if o == 17: return [(sx, sy), (sx + 1, sy), (sx + 1, sy + 1)]
        if o == 18: return [(sx, sy), (sx, sy + 1), (sx + 1, sy + 1)]
        if o == 19: return [(sx + 1, sy), (sx, sy + 1), (sx + 1, sy + 1)]
    return []

class VirtualTFTDisplay(tk.Tk):
    def __init__(self, default_port=None, default_team=None):
        super().__init__()

        if default_team is not None and 1 <= default_team <= 4:
            self.my_team_id = default_team
            self.team_selected = True
            self.screen = "WIZARD"
            self.title(f"Meraz Battleship - Virtual TFT (Team {default_team})")
        else:
            self.my_team_id = 1
            self.team_selected = False
            self.screen = "TEAM_SELECT"
            self.title("Meraz Battleship - Virtual TFT (Select Team)")

        self.resizable(False, False)
        self.configure(bg="#121620")

        self.opponent_ids = [t for t in range(1, 5) if t != self.my_team_id]
        self.sel_target = self.opponent_ids[0] if self.opponent_ids else 0
        self.own_ship = set()

        # Ship wizard state
        self.wiz_ship_idx = 0
        self.wiz_ships = [(5, 0, 0, 0), (4, 0, 2, 0), (4, 0, 4, 0), (3, 5, 0, 1)]
        self.ships_locked = False
        self.wiz_review_mode = False
        self.wiz_default_choice = 0
        self.viewing_own_fleet = False
        self.set_ships(self.wiz_ships)

        # which screen is showing: TEAM_SELECT, WIZARD, LOADOUT, MESSAGE, WAITING, ACTION, MENU, BANNER
        self.message_text = "Waiting for start..."
        self.waiting_team = 0
        self.banner = None                 # dict(bg, lines=[(text,size,color,y)])
        self.banner_expiry = 0
        self.banner_active = False
        self.banner_is_power = False
        self.pending_screen = None
        self.running = False               # a round is in progress
        self.my_turn = False

        # loadout
        self.ld_attack = set()
        self.ld_defence = None
        self.ld_armed_until = 0
        self.loadout_locked = False
        self.loadout = set()               # chosen optional powers
        self.uses_left = {p: 0 for p in SPECS}
        self.last_move_was_shield = False
        self.my_mine = None                # my last mine cell (x, y)
        self.opp_grid = {t: {} for t in range(1, 5)} # t -> {(c, r): "HIT"|"MISS"|"MINE"|"BLOCKED"}
        self.own_damage = set()            # {(c, r)} of damaged cells on own fleet
        self.own_misses = set()            # {(c, r)} of missed shots / triggered mines on own grid
        self.own_blocked = set()           # {(c, r)} deflected by smoke/shield (turns normal next round)

        # action screen state
        self.mode_power = 0                # 0 = Single Strike
        self.sel_target = self.opponent_ids[0] if self.opponent_ids else 0
        self.sel_cells = []
        self.send_lock_until = 0
        self.last_send = None              # (power, target, cells) for the result banner

        # Serial connection
        self.serial_conn = None
        self.serial_thread = None
        self.running_app = True
        self.default_port = default_port

        self._create_widgets()
        self._refresh_ports()
        self.redraw()

        if default_port and SERIAL_AVAILABLE:
            self.port_combo.set(default_port)
            self.connect_serial()

        self.after(50, self.update_timer)

    # ------------------------------------------------------------------
    # window furniture
    # ------------------------------------------------------------------
    def _create_widgets(self):
        top_bar = tk.Frame(self, bg="#1a202c", pady=6, padx=8)
        top_bar.pack(fill=tk.X)

        tk.Label(top_bar, text="COM Port:", bg="#1a202c", fg="#e2e8f0", font=("Arial", 9, "bold")).pack(side=tk.LEFT, padx=3)
        self.port_combo = ttk.Combobox(top_bar, width=9, values=[])
        self.port_combo.pack(side=tk.LEFT, padx=3)

        tk.Button(top_bar, text="⟳", bg="#2d3748", fg="#e2e8f0", relief=tk.FLAT,
                  command=self._refresh_ports, font=("Arial", 9, "bold"), padx=4).pack(side=tk.LEFT, padx=2)

        self.btn_connect = tk.Button(top_bar, text="Connect", bg="#3182ce", fg="#ffffff", relief=tk.FLAT,
                                     command=self.toggle_connection, font=("Arial", 9, "bold"), padx=6)
        self.btn_connect.pack(side=tk.LEFT, padx=4)

        self.lbl_status = tk.Label(top_bar, text="Disconnected", bg="#1a202c", fg="#e53e3e", font=("Arial", 9))
        self.lbl_status.pack(side=tk.LEFT, padx=6)

        tk.Label(top_bar, text="Team:", bg="#1a202c", fg="#e2e8f0", font=("Arial", 9, "bold")).pack(side=tk.RIGHT, padx=2)
        self.team_spin = ttk.Spinbox(top_bar, from_=1, to=4, width=3, command=self._on_team_change)
        self.team_spin.set(self.my_team_id)
        self.team_spin.pack(side=tk.RIGHT, padx=4)

        bezel = tk.Frame(self, bg="#2a2e39", padx=10, pady=10)
        bezel.pack(padx=12, pady=8)
        tk.Label(bezel, text="2.4\" TFT TOUCH (ILI9341 240x320)", bg="#2a2e39", fg="#718096",
                 font=("Consolas", 8)).pack(anchor=tk.W, pady=(0, 4))

        self.canvas = tk.Canvas(bezel, width=sx(BASE_WIDTH), height=sy(BASE_HEIGHT),
                                bg=BLACK, highlightthickness=1, highlightbackground="#4a5568")
        self.canvas.pack()
        self.canvas.bind("<Button-1>", self.on_canvas_click)

        ctrl_bar = tk.Frame(self, bg="#1a202c", pady=6, padx=8)
        ctrl_bar.pack(fill=tk.X)
        tk.Label(ctrl_bar, text="Quick Test:", bg="#1a202c", fg="#a0aec0", font=("Arial", 8, "bold")).pack(side=tk.LEFT, padx=4)

        def qbtn(text, bg, cmd):
            tk.Button(ctrl_bar, text=text, bg=bg, fg="#fff", relief=tk.FLAT, font=("Arial", 8),
                      command=cmd).pack(side=tk.LEFT, padx=2)

        qbtn("Ships", "#2b6cb0", self.demo_wizard)
        qbtn("Loadout", "#805ad5", self.demo_loadout)
        qbtn("Reset", "#c53030", self.reset_all)
        qbtn("Your Turn", "#2b6cb0", lambda: self.handle_line(">>> YOUR TURN <<<"))
        qbtn("Waiting", "#4a5568", lambda: self.handle_line("Waiting - it is TEAM 2's turn."))
        qbtn("Hit", "#dd6b20", lambda: self.handle_line("RESULT: TEAM 2 (1,2) -> HIT"))
        qbtn("Miss", "#3182ce", lambda: self.handle_line("RESULT: TEAM 2 (0,0) -> MISS"))
        qbtn("Mine", "#9b2c2c", lambda: self.handle_line("RESULT: TEAM 2 (4,4) -> MINE - it was a mine!"))
        def demo_hit_on_us():
            # Find an undamaged ship cell and mark it hit
            for c, r in sorted(list(self.own_ship)):
                if (c, r) not in self.own_damage:
                    self.own_damage.add((c, r))
                    self.redraw()
                    break
        qbtn("Hit Us", "#c53030", demo_hit_on_us)
        def demo_opp_repaired():
            # Clear first tracked hit on current target
            if self.sel_target and self.sel_target in self.opp_grid:
                hits = [pt for pt, st in self.opp_grid[self.sel_target].items() if st == "HIT"]
                if hits:
                    del self.opp_grid[self.sel_target][hits[0]]
                    self.redraw()
        qbtn("Opp Repaired", "#319795", demo_opp_repaired)

        self.log_visible = tk.BooleanVar(value=False)
        tk.Checkbutton(ctrl_bar, text="Console", variable=self.log_visible, bg="#1a202c", fg="#a0aec0",
                       selectcolor="#2d3748", command=self._toggle_log, font=("Arial", 8)).pack(side=tk.RIGHT, padx=4)

        self.log_frame = tk.Frame(self, bg="#0d1117", padx=4, pady=4)
        self.log_text = tk.Text(self.log_frame, height=5, width=45, bg="#0d1117", fg="#58a6ff",
                                font=("Consolas", 8), insertbackground="white")
        self.log_text.pack(fill=tk.BOTH, expand=True)

    def _toggle_log(self):
        if self.log_visible.get():
            self.log_frame.pack(fill=tk.BOTH, padx=8, pady=(0, 6), expand=True)
        else:
            self.log_frame.pack_forget()

    def reset_all(self):
        if self.serial_conn and self.serial_conn.is_open:
            self.send_serial_line("RESET")
        self.team_selected = False
        self.ships_locked = False
        self.loadout_locked = False
        self.running = False
        self.my_turn = False
        self.opp_grid = {t: {} for t in range(1, 5)}
        self.own_damage.clear()
        self.own_misses.clear()
        self.own_blocked.clear()
        self.my_mine = None
        self.show_team_select()

    def select_team(self, tid, from_serial=False):
        if not (1 <= tid <= 4):
            return
        self.my_team_id = tid
        self.team_selected = True
        self.opponent_ids = [t for t in range(1, 5) if t != self.my_team_id]
        if not self.sel_target or self.sel_target not in self.opponent_ids:
            self.sel_target = self.opponent_ids[0] if self.opponent_ids else 0
        self.team_spin.set(self.my_team_id)
        self.title(f"Meraz Battleship - Virtual TFT (Team {self.my_team_id})")
        if not from_serial and self.serial_conn and self.serial_conn.is_open:
            self.send_serial_line(f"TEAM {tid}")

        # If already running or ships are locked or past team select, never wipe locks or jump to wizard
        if self.running or self.ships_locked or self.screen in ("ACTION", "WAITING", "MENU", "LOADOUT"):
            self.redraw()
            return

        self.ships_locked = False
        self.loadout_locked = False
        self.show_wizard()

    def show_team_select(self):
        if self.running or self.screen in ("ACTION", "WAITING", "MENU"):
            return
        self.screen = "TEAM_SELECT"
        self.team_selected = False
        self.ships_locked = False
        self.loadout_locked = False
        self.title("Meraz Battleship - Virtual TFT (Select Team)")
        self.redraw()

    def _on_team_change(self):
        try:
            tid = int(self.team_spin.get())
        except ValueError:
            return
        if 1 <= tid <= 4:
            self.select_team(tid, from_serial=False)

    # ------------------------------------------------------------------
    # serial
    # ------------------------------------------------------------------
    def _refresh_ports(self):
        if not SERIAL_AVAILABLE:
            self.port_combo['values'] = ["No pyserial"]
            return
        ports = [p.device for p in serial.tools.list_ports.comports()]
        self.port_combo['values'] = ports
        if ports and not self.port_combo.get():
            self.port_combo.set(ports[0])

    def toggle_connection(self):
        if self.serial_conn and self.serial_conn.is_open:
            self.disconnect_serial()
        else:
            self.connect_serial()

    def connect_serial(self):
        if not SERIAL_AVAILABLE:
            messagebox.showerror("Error", "pyserial is not installed. Install with: pip install pyserial")
            return
        port = self.port_combo.get()
        if not port:
            messagebox.showwarning("Warning", "Select a valid COM port first.")
            return
        try:
            self.serial_conn = serial.Serial(port, 115200, timeout=0.1)
            self.lbl_status.config(text=f"Connected ({port})", fg="#38a169")
            self.btn_connect.config(text="Disconnect", bg="#e53e3e")
            self.append_log(f"--- Connected to {port} at 115200 baud ---")
            self.serial_thread = threading.Thread(target=self._read_serial_loop, daemon=True)
            self.serial_thread.start()
            self.after(500, lambda: self.send_serial_line("CONFIG"))
            self.after(900, lambda: self.send_serial_line("STATUS"))
        except Exception as e:
            messagebox.showerror("Serial Connection Failed", str(e))
            self.lbl_status.config(text="Error", fg="#e53e3e")

    def disconnect_serial(self):
        if self.serial_conn:
            try:
                self.serial_conn.close()
            except Exception:
                pass
            self.serial_conn = None
        self.lbl_status.config(text="Disconnected", fg="#e53e3e")
        self.btn_connect.config(text="Connect", bg="#3182ce")
        self.append_log("--- Disconnected ---")

    def send_serial_line(self, line):
        self.append_log(f">> {line}")
        if self.serial_conn and self.serial_conn.is_open:
            try:
                self.serial_conn.write((line.strip() + "\n").encode("utf-8"))
            except Exception as e:
                self.append_log(f"[Write Error] {e}")

    def append_log(self, text):
        def _add():
            self.log_text.insert(tk.END, text + "\n")
            self.log_text.see(tk.END)
        self.after(0, _add)

    def _read_serial_loop(self):
        buf = ""
        while self.running_app and self.serial_conn and self.serial_conn.is_open:
            try:
                data = self.serial_conn.read(256).decode("utf-8", errors="ignore")
                if data:
                    buf += data
                    while "\n" in buf:
                        line, buf = buf.split("\n", 1)
                        line = line.strip()
                        if line and not line.startswith("@TFT"):
                            self.append_log(line)
                            self.after(0, self.handle_line, line)
            except Exception as e:
                self.append_log(f"[Serial Read Error] {e}")
                break
            time.sleep(0.01)


    # ------------------------------------------------------------------
    # PARSE SERIAL MESSAGES FROM participant_node.ino
    # ------------------------------------------------------------------
    def set_ships(self, ships):
        """ships = [(len, x, y, orientation)] -> the set of my ship cells."""
        self.wiz_ships = list(ships)
        self.own_ship = set()
        for ln, x, y, o in ships:
            for cx, cy in get_ship_cells(ln, x, y, o):
                if 0 <= cx < GRID_SIZE and 0 <= cy < GRID_SIZE:
                    self.own_ship.add((cx, cy))
        self._ships_read = []

    def handle_line(self, line):
        line = line.strip()

        # --- team select ---
        if "[TEAM]" in line:
            if "SELECTED" in line:
                m = re.search(r"TEAM\s+(\d+)\s+SELECTED", line)
                if m:
                    self.select_team(int(m.group(1)), from_serial=True)
            elif not self.team_selected and not self.running and self.screen not in ("ACTION", "WAITING", "MENU", "LOADOUT", "WIZARD"):
                self.show_team_select()
            return

        # --- wizard ---
        if "[WIZARD]" in line:
            if "locked in!" in line:
                self.ships_locked = True
                self.wiz_review_mode = False
                if self.screen == "WIZARD":
                    self.show_loadout()
            elif "Loaded Default Config" in line or "for preview" in line:
                m_cfg = re.search(r"Config\s+#(\d+)", line, re.IGNORECASE)
                if m_cfg:
                    cfg_idx = int(m_cfg.group(1)) - 1
                    if 0 <= cfg_idx < len(DEFAULT_CONFIGS):
                        self.wiz_default_choice = cfg_idx
                        self.wiz_ships = list(DEFAULT_CONFIGS[cfg_idx])
                self.wiz_review_mode = True
                if self.screen == "WIZARD":
                    self.redraw()
            elif not self.ships_locked and not self.running and self.screen not in ("ACTION", "WAITING", "MENU", "LOADOUT"):
                if self.screen != "WIZARD":
                    self.show_wizard()
            return

        # --- loadout ---
        if line.startswith("[LOADOUT]"):
            self.ships_locked = True
            if not self.loadout_locked and not self.running and self.screen not in ("ACTION", "WAITING", "MENU"):
                if self.screen != "LOADOUT":
                    self.show_loadout()
            return
        m = re.search(r"Loadout locked:.*\(byte 0x([0-9A-Fa-f]+)\)", line)
        if m:
            self.apply_loadout_byte(int(m.group(1), 16))
            return

        # --- registration & reconnect ---
        if "Registration ACCEPTED" in line or "Reconnected" in line:
            self.team_selected = True
            self.ships_locked = True
            self.loadout_locked = True
            if "Reconnected" in line:
                self.running = True
                self.show_message("Reconnected!")
            else:
                self.show_message("Registered!")
                self.after(1500, self._after_registered_waiting)
            return

        # --- attack tracker marker ---
        m = re.search(r"\[MY_ATTACK\]\s*target=(\d+)\s+x=(\d+)\s+y=(\d+)", line)
        if m:
            self.last_attack_sent = (int(m.group(1)), int(m.group(2)), int(m.group(3)))
            return

        # --- config (ships, team id) ---
        m = re.search(r"MY_TEAM_ID:\s*(\d+)", line)
        if m:
            self.select_team(int(m.group(1)), from_serial=True)
            self._ships_read = []
            return
        m = re.search(r"ship\s+\d+:\s*len=(\d+)\s+start=\((\d+),(\d+)\)\s+(\w+)(?:\s+\((\w+)-?(\w*)\))?", line)
        if m:
            ln, x, y = int(m.group(1)), int(m.group(2)), int(m.group(3))
            word = m.group(4)
            o = {"HORIZONTAL": 0, "VERTICAL": 1}.get(word, 2)
            if word == "DIAGONAL":
                o = 3 if "up" in line else 2
            self._ships_read.append((ln, x, y, o))
            if len(self._ships_read) == 4:
                ships = self._ships_read
                self.set_ships(ships)
                self.redraw()
            return
        m = re.search(r"Loadout:.*\(byte 0x([0-9A-Fa-f]+)\)", line)
        if m and not self.loadout_locked:
            self.apply_loadout_byte(int(m.group(1), 16))
            return

        # --- opponent repaired a cell: clear from radar tracking ---
        m = re.search(r"\[RADAR\]\s*TEAM\s*(\d+)\s*repaired\s*\((\d+),(\d+)\)", line)
        if m:
            tid, rx, ry = int(m.group(1)), int(m.group(2)), int(m.group(3))
            if tid in self.opp_grid and (rx, ry) in self.opp_grid[tid]:
                del self.opp_grid[tid][(rx, ry)]
                self.redraw()
            return

        m = re.search(r"TEAM\s+(\d+)\s+REPAIRED\s+cell\s+\((\d+),(\d+)\)", line, re.IGNORECASE)
        if m:
            tid, rx, ry = int(m.group(1)), int(m.group(2)), int(m.group(3))
            if tid == self.my_team_id:
                self.own_damage.discard((rx, ry))
            elif tid in self.opp_grid and (rx, ry) in self.opp_grid[tid]:
                del self.opp_grid[tid][(rx, ry)]
            self.redraw()
            return

        # --- incoming hit on our fleet ---
        m = re.search(r"\[DEFENSE\]\s*Hit taken on own ship at \((\d+),(\d+)\)!", line)
        if m:
            hx, hy = int(m.group(1)), int(m.group(2))
            self.own_damage.add((hx, hy))
            self.redraw()
            return

        m = re.search(r"\[DEFENSE\]\s*Our mine at \((\d+),(\d+)\) was triggered!", line)
        if m:
            mx, my = int(m.group(1)), int(m.group(2))
            self.my_mine = None
            self.own_misses.add((mx, my))
            self.redraw()
            return

        m = re.search(r"\[REPAIR\]\s*Repaired \((\d+),(\d+)\)", line)
        if m:
            rx, ry = int(m.group(1)), int(m.group(2))
            self.own_damage.discard((rx, ry))
            self.sel_cells = [pt for pt in self.sel_cells if pt != (rx, ry)]
            self.redraw()
            return

        # --- active opponents update ---
        m = re.search(r"\[OPPONENTS\]\s*(\d+)(.*)", line)
        if m:
            n = int(m.group(1))
            rest = m.group(2).strip().split()
            if rest:
                self.opponent_ids = [int(x) for x in rest]
            else:
                self.opponent_ids = [t for t in range(1, 5) if t != self.my_team_id][:n]
            if not self.sel_target or self.sel_target not in self.opponent_ids:
                self.sel_target = self.opponent_ids[0] if self.opponent_ids else 0
            self.redraw()
            return

        # --- turn updates ---
        if ">>> YOUR TURN <<<" in line or ">>> IT IS YOUR TURN <<<" in line:
            self.team_selected = True
            self.ships_locked = True
            self.loadout_locked = True
            self.running, self.my_turn = True, True
            self.own_blocked.clear()
            if self.my_team_id in self.opp_grid:
                self.opp_grid[self.my_team_id] = {pt: st for pt, st in self.opp_grid[self.my_team_id].items() if st != "BLOCKED"}
            self.show_your_turn()
            return
        m = re.search(r"Waiting - it is TEAM (\d+)'s turn", line, re.IGNORECASE)
        if m:
            tid = int(m.group(1))
            self.team_selected = True
            self.ships_locked = True
            self.loadout_locked = True
            self.running, self.my_turn = True, False
            if tid in self.opp_grid:
                self.opp_grid[tid] = {pt: st for pt, st in self.opp_grid[tid].items() if st != "BLOCKED"}
            if tid == self.my_team_id:
                self.own_blocked.clear()
            self.show_waiting(tid)
            return
        if "Waiting for the organizer to start" in line or ("GAME OVER" in line and "RESULT" not in line) or "Game reset" in line or "[RESET]" in line:
            self.running, self.my_turn = False, False
            if "GAME OVER" in line:
                self.show_message("GAME OVER")
            elif "Game reset" in line or "[RESET]" in line:
                self.ships_locked = False
                self.loadout_locked = False
                self.opp_grid = {t: {} for t in range(1, 5)}
                self.own_damage.clear()
                self.own_misses.clear()
                self.my_mine = None
                self.show_wizard()
                return
            elif "Waiting" in line:
                if not self.team_selected or not self.ships_locked or not self.loadout_locked:
                    return  # Still in Team Select, Wizard or Loadout! Do not overwrite screen!
                self.show_message("Waiting for start...")
                self.opp_grid = {t: {} for t in range(1, 5)}
                self.own_misses.clear()
                self.my_mine = None
            return

        # --- Single Strike result ---
        m = re.search(r"RESULT:\s*TEAM\s*(\d+)\s*\((\d+),(\d+)\)\s*->\s*(.+)", line, re.IGNORECASE)
        if m:
            target, x, y = int(m.group(1)), int(m.group(2)), int(m.group(3))
            raw = m.group(4).strip().upper()
            if not raw:
                return
            if "REPAIR" in raw:
                if target in self.opp_grid and (x, y) in self.opp_grid[target]:
                    del self.opp_grid[target][(x, y)]
                self.redraw()
                return
            if "ELIMINATED" in raw or "SUNK" in raw:
                kind = "SHIP SUNK" if "SHIP" in raw else "ELIMINATED!"
            elif "MINE" in raw:
                kind = "MINE"
            elif "BLOCK" in raw:
                kind = "BLOCKED"
            elif "UNKNOWN" in raw:
                kind = "UNKNOWN"
            elif "HIT" in raw:
                kind = "HIT"
            elif "MISS" in raw:
                kind = "MISS"
            elif "INVALID" in raw:
                kind = "INVALID"
            else:
                return

            if target == self.my_team_id:
                if kind in ("MINE", "MISS") or (self.my_mine and (x, y) == self.my_mine):
                    self.my_mine = None
                    self.own_misses.add((x, y))
                elif kind in ("HIT", "SHIP SUNK", "ELIMINATED!"):
                    self.own_damage.add((x, y))
                elif kind == "BLOCKED":
                    self.own_blocked.add((x, y))

            if kind != "INVALID":
                self.last_move_was_shield = False
                if target in self.opp_grid:
                    if kind in ("HIT", "SHIP SUNK", "ELIMINATED!"):
                        self.opp_grid[target][(x, y)] = "HIT"
                    elif kind in ("MISS", "MINE"):
                        self.opp_grid[target][(x, y)] = "MISS"
                    elif kind == "BLOCKED":
                        self.opp_grid[target][(x, y)] = "BLOCKED"
                if (not self.viewing_own_fleet and self.sel_target == target) or (self.viewing_own_fleet and target == self.my_team_id):
                    self.redraw()
            is_my_attack = (getattr(self, 'last_attack_sent', None) == (target, x, y))
            if is_my_attack:
                self.last_attack_sent = None
            if kind == "INVALID" and self.opp_grid.get(target, {}).get((x, y)) in ("HIT", "MISS"):
                return
            if self.banner_active and getattr(self, 'banner_is_power', False):
                return
            if is_my_attack or self.my_turn or not self.running:
                self.show_result_banner(target, x, y, kind)
            return

        # --- power shot update ---
        m = re.search(r"\[POWER_SHOT\]\s*TEAM\s*(\d+)\s*\((\d+),(\d+)\)\s*->\s*(.+)", line)
        if m:
            tid, px, py = int(m.group(1)), int(m.group(2)), int(m.group(3))
            raw = m.group(4).strip().upper()
            if "HIT" in raw or "SUNK" in raw or "ELIMINATED" in raw:
                self.opp_grid.setdefault(tid, {})[(px, py)] = "HIT"
            elif "MISS" in raw or "MINE" in raw:
                self.opp_grid.setdefault(tid, {})[(px, py)] = "MISS"
            elif "BLOCK" in raw:
                self.opp_grid.setdefault(tid, {})[(px, py)] = "BLOCKED"
            if not self.viewing_own_fleet and self.sel_target == tid:
                self.redraw()
            return

        # --- power-up result ---
        m = re.search(r"POWER_RESULT:\s*power=(\d+)\s+status=(\d+)\s+target=(\d+)\s+count=(\d+)\s+r=(\d+),(\d+),(\d+)(?:\s+pts=\((\d+),(\d+)\),\((\d+),(\d+)\),\((\d+),(\d+)\))?", line)
        if m:
            power, status, target, count = (int(m.group(i)) for i in range(1, 5))
            res = [int(m.group(5)), int(m.group(6)), int(m.group(7))]
            pts = None
            if m.group(8) is not None:
                pts = [
                    (int(m.group(8)), int(m.group(9))),
                    (int(m.group(10)), int(m.group(11))),
                    (int(m.group(12)), int(m.group(13)))
                ]
            if pts:
                if not getattr(self, 'last_send', None) or not self.last_send[2] or self.last_send[0] != power:
                    self.last_send = (power, target, pts)
            if status != 1 and power in (2, 7) and hasattr(self, 'last_send') and self.last_send:
                p_id, p_target, p_cells = self.last_send
                if p_id == power and p_target == target:
                    for idx, (cx, cy) in enumerate(p_cells):
                        if idx < len(res):
                            code = res[idx]
                            if code in (1, 3, 7): # RESULT_HIT, RESULT_SUNK, RESULT_SHIP_SUNK
                                self.opp_grid.setdefault(target, {})[(cx, cy)] = "HIT"
                            elif code in (0, 4): # RESULT_MISS, RESULT_MINE
                                self.opp_grid.setdefault(target, {})[(cx, cy)] = "MISS"
                            elif code == 5: # RESULT_BLOCKED
                                self.opp_grid.setdefault(target, {})[(cx, cy)] = "BLOCKED"
            self.show_power_banner(power, status, target, count, res, pts)
            return

        if line.startswith("[UI]"):
            msg = line[4:].strip()
            if "Waiting for start" in msg and (not self.team_selected or not self.ships_locked or not self.loadout_locked):
                return
            self.show_message(msg)
            if msg == "Registered!":
                self.after(1500, self._after_registered_waiting)
            return

    def _after_registered_waiting(self):
        if not self.running and self.screen == "MESSAGE" and self.message_text == "Registered!":
            self.show_message("Waiting for start...")

    def apply_loadout_byte(self, byte):
        self.loadout = {p for p in LD_ATTACK_IDS + LD_DEFENCE_IDS if byte & (1 << (p - 1))}
        self.uses_left = {p: (POWER_USES if p in self.loadout else 0) for p in SPECS}
        self.loadout_locked = True
        if self.screen == "LOADOUT":
            self.show_message("Connecting...")

    # ------------------------------------------------------------------
    # SCREEN STATE LOGIC
    # ------------------------------------------------------------------
    def _banner_busy(self):
        return time.time() * 1000 < self.banner_expiry

    def show_wizard(self):
        if self.running or self.screen in ("ACTION", "WAITING", "MENU"):
            return
        self.ships_locked = False
        self.screen = "WIZARD"
        self.redraw()

    def demo_wizard(self):
        self.running = False
        self.ships_locked = False
        self.wiz_ship_idx = 0
        if self.serial_conn and self.serial_conn.is_open:
            self.send_serial_line("DEMO WIZARD")
        self.show_wizard()

    def show_loadout(self):
        if self.running or self.screen in ("ACTION", "WAITING", "MENU"):
            return
        self.loadout_locked = False
        self.screen = "LOADOUT"
        self.redraw()

    def demo_loadout(self):
        self.running = False
        self.loadout_locked = False
        self.ld_attack, self.ld_defence = set(), None
        if self.serial_conn and self.serial_conn.is_open:
            self.send_serial_line("DEMO LOADOUT")
        self.show_loadout()

    def show_message(self, msg):
        if "Waiting for start" in msg and (not self.team_selected or not self.ships_locked or not self.loadout_locked):
            return
        if self._banner_busy():
            self.pending_screen = ("MESSAGE", msg)
            return
        self.screen, self.message_text = "MESSAGE", msg
        self.redraw()

    def show_waiting(self, team):
        self.team_selected = True
        self.ships_locked = True
        self.loadout_locked = True
        self.running = True
        self.my_turn = False
        if not self.sel_target or self.sel_target not in self.opponent_ids:
            self.sel_target = self.opponent_ids[0] if self.opponent_ids else 0
        if self._banner_busy():
            self.pending_screen = ("WAITING", team)
            return
        self.screen, self.waiting_team = "WAITING", team
        self.redraw()

    def show_your_turn(self):
        self.team_selected = True
        self.ships_locked = True
        self.loadout_locked = True
        self.running = True
        self.my_turn = True
        if self._banner_busy():
            self.pending_screen = ("ACTION", None)
            return
        if self.screen in ("ACTION", "MENU") and self.my_turn and not self.banner_active:
            return                       # a repeated turn update must not wipe a half-made selection
        self.start_strike()

    def start_strike(self):
        self.mode_power = 0
        self.sel_cells = []
        if not self.sel_target or self.sel_target not in self.opponent_ids:
            self.sel_target = self.opponent_ids[0] if self.opponent_ids else 0
        self.viewing_own_fleet = False
        self.screen = "ACTION"
        self.redraw()

    def start_power(self, power):
        self.mode_power = power
        self.sel_cells = []
        if self._needs_target():
            if not self.sel_target or self.sel_target not in self.opponent_ids:
                self.sel_target = self.opponent_ids[0] if self.opponent_ids else 0
            self.viewing_own_fleet = False
        elif self._own_grid():
            self.viewing_own_fleet = True
        self.screen = "ACTION"
        self.redraw()

    def _start_banner(self, banner, ms):
        self.banner = banner
        self.banner_expiry = time.time() * 1000 + ms
        self.banner_active = True
        self.banner_is_power = banner.get("is_power", False)
        self.screen = "BANNER"
        self.send_lock_until = 0
        self.redraw()

    def show_result_banner(self, target, x, y, kind):
        look = {
            "MISS": (NAVY, "MISS", "SHOT IN THE OCEAN", ""),
            "HIT": (ORANGE, "DIRECT HIT!", "+10 POINTS", ""),
            "SHIP SUNK": (RED, "SHIP SUNK!", "+15 POINTS (HIT + SINK)", ""),
            "ELIMINATED!": (MAROON, "TEAM ELIMINATED!", "+20 POINTS", ""),
            "MINE": (MAROON, "MINE TRIGGERED!", "1 CHANCE LOST!", "You lose your next turn."),
            "BLOCKED": (PURPLE, "BLOCKED", "DEFLECTED BY SHIELD", ""),
            "UNKNOWN": (MIDGREY, "SMOKE SCREEN", "RESULT HIDDEN", "")
        }
        bg, label, sub, extra = look.get(kind, (MIDGREY, "INVALID MOVE", "REJECTED BY REFEREE", ""))
        lines = [(label, 3, WHITE, 70)]
        if sub:
            lines.append((sub, 2, YELLOW, 120))
        lines.append((f"TARGET: TEAM {target} ({x},{y})", 2, WHITE, 170))
        if extra:
            lines.append((extra, 1, WHITE, 210))
        lines.append(("[Click window to dismiss]", 1, MIDGREY, 285))
        self._start_banner({"bg": bg, "lines": lines, "is_power": False}, UI_RESULT_DISPLAY_MS)

    def show_power_banner(self, power, status, target, count, res, pts=None):
        name = SPECS[power][0] if power in SPECS else "POWER"
        info = self.last_send if self.last_send and self.last_send[0] == power else (power, target, [])
        cells = list(info[2]) if info[2] else (list(pts) if pts else [])
        lines, bg = [], "#007B00"
        if status == 1:                                     # rejected
            bg = MIDGREY
            hint = {SONAR: "Cell already shot? Target out?", SALVO: "Cell already shot? Target out?",
                    DOUBLE: "Cell already shot? Target out?", MINE: "Mine still active? Cell shot?",
                    REPAIR: "Cell not damaged or ship sunk", SHIELD: "No Shield two turns in a row",
                    SMOKE: "Smoke can't be used now"}.get(power, "Central refused it")
            lines = [("REJECTED", 3, WHITE, 70), (name, 2, WHITE, 110), ("Turn NOT used.", 2, WHITE, 150), (hint, 1, WHITE, 186)]
            self._start_banner({"bg": bg, "lines": lines, "is_power": True}, UI_RESULT_DISPLAY_MS + 300)
            return
        # the move worked: update what the screens show
        if power != SHIELD and power in self.uses_left and self.uses_left[power] > 0:
            self.uses_left[power] -= 1
        self.last_move_was_shield = (power == SHIELD)
        if power == MINE and cells:
            self.my_mine = cells[0]
        c0 = cells[0] if cells else (0, 0)
        long_read = power in (SONAR, SALVO, DOUBLE)
        if power == SONAR:
            if status == 2:
                bg = MIDGREY
                lines = [("SONAR", 3, WHITE, 70), ("JAMMED", 3, WHITE, 120), ("Smoke blocked the ping", 1, WHITE, 170)]
            else:
                bg = DARKCYAN
                lines = [("SONAR", 3, WHITE, 70), (f"TEAM {target} around ({c0[0]},{c0[1]})", 1, WHITE, 114),
                         (f"{count} SHIP CELL" + ("" if count == 1 else "S"), 3, YELLOW, 140),
                         ("[Click window to dismiss]", 1, MIDGREY, 280)]
        elif power in (SALVO, DOUBLE):
            names = {0: "MISS", 1: "HIT", 3: "SUNK", 4: "MINE", 5: "BLOCKED", 6: "UNKNOWN", 7: "SHIP SUNK"}
            n = 3 if power == SALVO else 2
            while len(cells) < n:
                if pts and len(pts) > len(cells):
                    cells.append(pts[len(cells)])
                else:
                    cells.append((0, 0))
            total_fired = n
            hit_cnt = sum(1 for r in res[:total_fired] if r in (1, 3, 7))
            mine_cnt = sum(1 for r in res[:total_fired] if r == 4)
            miss_cnt = sum(1 for r in res[:total_fired] if r == 0)

            bg = NAVY
            if hit_cnt > 0: bg = ORANGE
            if mine_cnt > 0: bg = MAROON

            if mine_cnt > 0:
                summary = "MINE DETONATED!"
            elif hit_cnt == total_fired:
                summary = f"{hit_cnt} HITS! PERFECT!"
            elif hit_cnt > 0 and miss_cnt > 0:
                summary = f"{hit_cnt} HITS, {miss_cnt} MISS"
            elif hit_cnt > 0:
                summary = f"{hit_cnt} HIT{'S' if hit_cnt > 1 else ''}!"
            else:
                summary = f"ALL {miss_cnt} MISSES"

            lines = [
                (name, 2, WHITE, 35),
                (summary, 3, YELLOW if hit_cnt > 0 else WHITE, 70),
            ]
            if mine_cnt > 0:
                lines.append(("1 CHANCE LOST!", 2, YELLOW, 108))
            else:
                lines.append((f"TARGET: TEAM {target}", 2, WHITE, 108))

            for i in range(total_fired):
                pt = cells[i]
                r_code = res[i]
                r_name = names.get(r_code, "NOT FIRED")
                shot_color = YELLOW if r_code in (1, 3, 7) else (RED if r_code == 4 else WHITE)
                lines.append((f"Shot {i+1}: ({pt[0]},{pt[1]}) -> {r_name}", 2, shot_color, 146 + i * 36))
                if target in self.opp_grid:
                    if r_code in (1, 3, 7):
                        self.opp_grid[target][pt] = "HIT"
                    elif r_code in (0, 4):
                        self.opp_grid[target][pt] = "MISS"
                    elif r_code == 5:
                        self.opp_grid[target][pt] = "BLOCKED"
            lines.append(("[Click window to dismiss]", 1, MIDGREY, 285))
        elif power == MINE:
            lines = [("MINE SET", 3, WHITE, 80), (f"at ({c0[0]},{c0[1]})", 2, YELLOW, 130), ("Only you know where it is.", 1, WHITE, 170)]
        elif power == REPAIR:
            if c0 in self.own_damage:
                self.own_damage.discard(c0)
            lines = [("REPAIRED", 3, WHITE, 80), (f"({c0[0]},{c0[1]}) is intact", 2, YELLOW, 130)]
        elif power == SMOKE:
            lines = [("SMOKE UP", 3, WHITE, 80), ("Attacks on you are hidden", 1, WHITE, 130), ("until your next turn.", 1, WHITE, 144)]
        elif power == SHIELD:
            lines = [("SHIELD UP", 3, WHITE, 80), (f"3x3 around ({c0[0]},{c0[1]})", 2, YELLOW, 130), ("until your next turn.", 1, WHITE, 170)]
        else:
            lines = [("DONE", 3, WHITE, 120)]
        duration = UI_RESULT_DISPLAY_MS + 300
        if status != 1:
            if power == SONAR:
                duration = UI_SONAR_RESULT_MS
            elif long_read:
                duration = UI_POWER_RESULT_MS
        self._start_banner({"bg": bg, "lines": lines, "is_power": True}, duration)

    def dismiss_banner(self):
        if not self.banner_active and not self.banner:
            return
        self.banner = None
        self.banner_active = False
        self.banner_is_power = False
        self.banner_expiry = 0
        pend, self.pending_screen = self.pending_screen, None
        if pend:
            kind, data = pend
            if kind == "ACTION":
                self.start_strike()
            elif kind == "WAITING":
                self.show_waiting(data)
            else:
                self.show_message(data)
        elif self.running and self.my_turn:
            self.start_strike()                       # a refused move: it is still my turn
        elif self.running:
            self.show_waiting(self.waiting_team)
        elif self.screen in ("ACTION", "WAITING", "MENU"):
            self.redraw()
        elif not self.team_selected:
            self.show_team_select()
        elif not self.ships_locked:
            self.show_wizard()
        elif not self.loadout_locked:
            self.show_loadout()
        else:
            self.show_message(self.message_text)
        self.redraw()

    def update_timer(self):
        now = time.time() * 1000
        if self.loadout_locked is False and self.ld_armed_until and now > self.ld_armed_until:
            self.ld_armed_until = 0
            if self.screen == "LOADOUT":
                self.redraw()
        if self.banner_active and now >= self.banner_expiry:
            self.dismiss_banner()
        self.after(50, self.update_timer)

    # ------------------------------------------------------------------
    # DRAWING (a mirror of participant_node.ino's UI layer)
    # ------------------------------------------------------------------
    def _font(self, size=1):
        # Calibrated font scaling for ILI9341 240x320 virtual display mirror.
        # Consolas provides clean monospaced proportions without clipping or overflowing button boxes.
        return ("Consolas", -int((4 + 5 * size) * SCALE), "bold")

    def text(self, x, y, s, size=1, color=WHITE, anchor="nw"):
        """x, y = top-left of the text in 240x320 screen pixels; size = TFT text size (6x8 px per char)."""
        self.canvas.create_text(sx(x), sy(y), text=s, fill=color, anchor=anchor,
                                font=self._font(size))

    def ctext(self, s, y, size, color):
        self.canvas.create_text(sx(120), sy(y + 4 * size), text=s, fill=color, anchor="center",
                                font=self._font(size))

    def rect(self, x, y, w, h, fill=None, outline=WHITE):
        self.canvas.create_rectangle(sx(x), sy(y), sx(x + w), sy(y + h), fill=fill or "", outline=outline or "", width=1)

    def circle(self, cx, cy, r, fill=None, outline=None):
        self.canvas.create_oval(sx(cx - r), sy(cy - r), sx(cx + r), sy(cy + r), fill=fill or "", outline=outline or "", width=1)

    def button(self, x, y, w, h, fill, label, size=2, color=WHITE, **kwargs):
        c = kwargs.get("fg", color)
        self.rect(x, y, w, h, fill=fill)
        self.canvas.create_text(sx(x + w / 2), sy(y + h / 2), text=label, fill=c, anchor="center",
                                font=self._font(size))

    def redraw(self):
        self.canvas.delete("all")
        self.canvas.create_rectangle(0, 0, sx(BASE_WIDTH), sy(BASE_HEIGHT), fill=BLACK, outline="")
        if self.screen == "BANNER" and self.banner:
            self.canvas.create_rectangle(0, 0, sx(BASE_WIDTH), sy(BASE_HEIGHT), fill=self.banner["bg"], outline="")
            for text, size, color, y in self.banner["lines"]:
                self.ctext(text, y, size, color)
        elif self.screen == "TEAM_SELECT":
            self._draw_team_select()
        elif self.screen == "WIZARD":
            self._draw_wizard()
        elif self.screen == "LOADOUT":
            self._draw_loadout()
        elif self.screen == "ACTION":
            self._draw_action()
        elif self.screen == "MENU":
            self._draw_menu()
        elif self.screen == "WAITING":
            self._draw_waiting()
        else:
            self._draw_message()

    def _draw_message(self):
        if self.message_text == "Waiting for start...":
            self.rect(5, 5, 230, 310, fill=None, outline=DARKCYAN)
            self.rect(7, 7, 226, 306, fill=None, outline=DARKGREY)
            self.ctext("FLEET REGISTERED", 70, 2, GREEN)
            self.ctext("READY FOR BATTLE", 100, 2, YELLOW)
            self.ctext("Waiting for start...", 150, 2, WHITE)
            self.ctext("Organizer will start match", 190, 1, MIDGREY)
            self.ctext(f"Squad: TEAM {self.my_team_id}", 240, 1, CYAN)
        elif self.message_text == "Registered!":
            self.rect(5, 5, 230, 310, fill=None, outline=GREEN)
            self.rect(7, 7, 226, 306, fill=None, outline=DARKGREEN)
            self.ctext("SUCCESS!", 95, 2, YELLOW)
            self.ctext("Registered!", 130, 3, GREEN)
            self.ctext("Connecting to Central...", 180, 1, WHITE)
        elif self.message_text == "Connecting...":
            self.rect(5, 5, 230, 310, fill=None, outline=DARKCYAN)
            self.ctext("Connecting...", 140, 2, CYAN)
            self.ctext("Sending fleet & powers", 175, 1, MIDGREY)
        else:
            self.ctext(self.message_text, 140, 2, WHITE)

    def _draw_team_select(self):
        # Decorative border
        self.rect(5, 5, 230, 310, fill=None, outline=DARKCYAN)
        self.rect(7, 7, 226, 306, fill=None, outline=DARKGREY)

        # Header Title
        self.ctext("MERAZ BATTLESHIP", 18, 2, CYAN)
        self.ctext("SELECT YOUR SQUAD", 42, 2, YELLOW)
        self.ctext("Tap your assigned team number:", 68, 1, WHITE)

        # 4 Team Buttons (Y: 88, 138, 188, 238; H: 42; W: 200)
        # Team 1: Blue
        self.rect(20, 88, 200, 42, fill=NAVY, outline=CYAN)
        self.ctext("TEAM 1", 100, 2, WHITE)

        # Team 2: Red
        self.rect(20, 138, 200, 42, fill=MAROON, outline=RED)
        self.ctext("TEAM 2", 150, 2, WHITE)

        # Team 3: Green
        self.rect(20, 188, 200, 42, fill=DARKGREEN, outline=GREEN)
        self.ctext("TEAM 3", 200, 2, WHITE)

        # Team 4: Orange / Brown
        self.rect(20, 238, 200, 42, fill=BROWN, outline=YELLOW)
        self.ctext("TEAM 4", 250, 2, WHITE)

        # Footer
        self.ctext("Or type in Serial: TEAM 1..4", 294, 1, DARKGREY)

    # ---- ship placement wizard ----
    def _wiz_ship_valid(self, ship_idx=None):
        if ship_idx is None:
            ship_idx = self.wiz_ship_idx
        ln, sx_pos, sy_pos, o = self.wiz_ships[ship_idx]
        cells = get_ship_cells(ln, sx_pos, sy_pos, o)
        if len(cells) != ln:
            return False, cells
        for cx, cy in cells:
            if not (0 <= cx < GRID_SIZE and 0 <= cy < GRID_SIZE):
                return False, cells
        if len(set(cells)) != ln:
            return False, cells
        # check overlap with locked ships (0 .. ship_idx - 1)
        for prev_idx in range(ship_idx):
            p_ln, p_sx, p_sy, p_o = self.wiz_ships[prev_idx]
            p_cells = get_ship_cells(p_ln, p_sx, p_sy, p_o)
            if not set(cells).isdisjoint(set(p_cells)):
                return False, cells
        return True, cells

    def rotate_current_ship(self):
        allowed = WIZ_ORIENTS.get(self.wiz_ship_idx, [0])
        ln, sx_pos, sy_pos, o = self.wiz_ships[self.wiz_ship_idx]
        if o in allowed:
            cur_idx = allowed.index(o)
            next_o = allowed[(cur_idx + 1) % len(allowed)]
        else:
            next_o = allowed[0]
        self.wiz_ships[self.wiz_ship_idx] = (ln, sx_pos, sy_pos, next_o)

    def use_default_ships(self):
        import random
        self.wiz_default_choice = random.randrange(len(DEFAULT_CONFIGS))
        self.wiz_ships = list(DEFAULT_CONFIGS[self.wiz_default_choice])
        self.wiz_review_mode = True
        self.send_serial_line("SHIPS DEFAULT")
        self.redraw()

    def confirm_locked_default_ships(self):
        self.ships_locked = True
        self.wiz_review_mode = False
        self.set_ships(self.wiz_ships)
        self.send_serial_line("CONFIRM")
        self.show_loadout()

    def lock_wizard_ships(self):
        self.ships_locked = True
        self.set_ships(self.wiz_ships)
        default_ships = [(5, 0, 0, 0), (4, 0, 2, 0), (4, 0, 4, 0), (3, 5, 0, 1)]
        if self.wiz_ships == default_ships:
            self.send_serial_line("SHIPS DEFAULT")
        else:
            args = []
            for ln, x, y, o in self.wiz_ships:
                args.extend([str(x), str(y), str(o)])
            self.send_serial_line("SHIPS SET " + " ".join(args))
        self.show_loadout()

    def _draw_wizard(self):
        if self.wiz_review_mode:
            self.text(10, 8, f"CONFIG #{self.wiz_default_choice + 1} / 10", 2, YELLOW)
            self.text(10, 28, "Preview fleet (Lengths 5, 4, 4, 3)", 1, WHITE)
            self.text(10, 42, "Tap RANDOM to cycle, or LOCK to save", 1, CYAN)

            for r in range(GRID_SIZE):
                for c in range(GRID_SIZE):
                    px = WIZ_GRID_X + c * WIZ_CELL_SIZE
                    py = WIZ_GRID_Y + r * WIZ_CELL_SIZE

                    ship_num = -1
                    for s, ship in enumerate(self.wiz_ships):
                        p_ln, p_sx, p_sy, p_o = ship
                        if (c, r) in get_ship_cells(p_ln, p_sx, p_sy, p_o):
                            ship_num = s + 1
                            break

                    fill = BLUE if ship_num >= 0 else DARKGREY
                    outline = CYAN if ship_num >= 0 else MIDGREY
                    self.rect(px, py, WIZ_CELL_SIZE, WIZ_CELL_SIZE, fill=fill, outline=outline)
                    if ship_num >= 0:
                        self.text(px + 10, py + 8, str(ship_num), 1, YELLOW)

            # Row 1 buttons: [RANDOM / NEXT] and [MANUAL PLACE]
            self.button(10, 258, 105, 26, DARKCYAN, "RANDOM / NEXT", size=1)
            self.button(125, 258, 105, 26, DARKGREY, "MANUAL PLACE", size=1)

            # Row 2 button: [CONFIRM & LOCK]
            self.button(10, 288, 220, 28, DARKGREEN, "CONFIRM & LOCK", size=2)
            return

        cur_ship = self.wiz_ships[self.wiz_ship_idx]
        ln, sx_pos, sy_pos, o = cur_ship
        orient_name = ORIENT_NAMES.get(o, f"TYPE {o}")
        valid, cur_cells = self._wiz_ship_valid()

        # Header Title
        self.text(10, 8, f"SHIP {self.wiz_ship_idx + 1}/4 (LEN {ln})", 2, YELLOW)
        # Subtitle
        self.text(10, 28, "Tap cell to set start position", 1, WHITE)
        # Orientation name
        self.text(10, 42, "Orient: ", 1, CYAN)
        self.text(60, 42, orient_name, 1, WHITE)

        cur_cells_set = set(cur_cells)

        # 7x7 grid
        for r in range(GRID_SIZE):
            for c in range(GRID_SIZE):
                px = WIZ_GRID_X + c * WIZ_CELL_SIZE
                py = WIZ_GRID_Y + r * WIZ_CELL_SIZE

                # Check locked ships
                locked_ship = -1
                for s in range(self.wiz_ship_idx):
                    p_ln, p_sx, p_sy, p_o = self.wiz_ships[s]
                    if (c, r) in get_ship_cells(p_ln, p_sx, p_sy, p_o):
                        locked_ship = s + 1
                        break

                is_cur = (c, r) in cur_cells_set
                is_start = (c == sx_pos and r == sy_pos)

                fill = DARKGREY
                outline = MIDGREY

                if locked_ship >= 0:
                    fill = BLUE
                    outline = CYAN
                elif is_cur:
                    fill = DARKGREEN if valid else MAROON
                    outline = GREEN if valid else RED

                self.rect(px, py, WIZ_CELL_SIZE, WIZ_CELL_SIZE, fill=fill, outline=outline)

                if locked_ship >= 0:
                    self.text(px + 10, py + 8, str(locked_ship), 1, WHITE)
                elif is_start:
                    self.text(px + 10, py + 8, "*", 1, YELLOW)

        # Row 1 buttons: [ROTATE SHAPE] and [USE DEFAULT]
        self.button(10, 258, 105, 26, NAVY, "ROTATE SHAPE", size=1)
        self.button(125, 258, 105, 26, DARKCYAN, "USE DEFAULT", size=1)

        # Row 2 buttons: [PREV] and [CONFIRM SHIP / LOCK ALL SHIPS]
        if self.wiz_ship_idx > 0:
            self.button(10, 288, 65, 28, DARKGREY, "PREV", size=1)

        confirm_label = "LOCK ALL SHIPS" if self.wiz_ship_idx == 3 else "CONFIRM SHIP"
        confirm_fill = DARKGREEN if valid else DARKGREY
        self.button(82, 288, 148, 28, confirm_fill, confirm_label, size=1)

    # ---- loadout ----
    def _ld_valid(self):
        return len(self.ld_attack) == 2 and self.ld_defence is not None

    def _draw_loadout(self):
        self.text(10, 8, "CHOOSE POWERS", 2, GREEN)
        self.text(8, 30, "Always yours: STRIKE + SHIELD", 1)
        self.text(8, 44, "ATTACK - pick 2 (2 uses each)", 1, CYAN)
        for i, pid in enumerate(LD_ATTACK_IDS):
            x = LD_COL2_X if i % 2 else LD_COL1_X
            y = LD_ATK_Y2 if i // 2 else LD_ATK_Y1
            self._ld_button(x, y, pid, pid in self.ld_attack)
        self.text(8, 146, "DEFENCE - pick 1 (2 uses each)", 1, CYAN)
        for j, pid in enumerate(LD_DEFENCE_IDS):
            self._ld_button(LD_COL2_X if j else LD_COL1_X, LD_DEF_Y, pid, self.ld_defence == pid)
        self.text(8, 214, f"Picked: {len(self.ld_attack)}/2 attack, {1 if self.ld_defence else 0}/1 defence", 1)
        self.text(8, 232, "Locked in = no changes this match.", 1)
        armed = self.ld_armed_until and time.time() * 1000 < self.ld_armed_until
        if not self._ld_valid():
            fill, label = DARKGREY, "PICK 2 + 1"
        elif armed:
            fill, label = RED, "TAP AGAIN"
        else:
            fill, label = DARKGREEN, "LOCK IN"
        self.button(LD_OK_X, LD_OK_Y, LD_OK_W, LD_OK_H, fill, label)

    def _ld_button(self, x, y, pid, selected):
        name, hint = SPECS[pid][0], SPECS[pid][1]
        fill, fg = (YELLOW, BLACK) if selected else (NAVY, WHITE)
        self.rect(x, y, LD_BTN_W, LD_BTN_H, fill=fill)
        self.text(x + 8, y + 6, name, 2, fg)
        self.text(x + 8, y + 26, hint, 1, fg)

    # ---- action screen (strike / power) ----
    def _spec(self):
        return SPECS.get(self.mode_power)

    def _needs_target(self):
        return True if self.mode_power == 0 else self._spec()[2]

    def _own_grid(self):
        return False if self.mode_power == 0 else self._spec()[3]

    def _n_cells(self):
        return 1 if self.mode_power == 0 else self._spec()[4]

    def _is_cell_of_sunk_ship(self, c, r):
        for ln, sx, sy, o in self.wiz_ships:
            cells = get_ship_cells(ln, sx, sy, o)
            if (c, r) in cells:
                return all(cell in self.own_damage for cell in cells)
        return False

    def _action_ready(self):
        if self.mode_power == 0 and self.viewing_own_fleet:
            return False
        if self.mode_power == MINE and self.my_mine is not None:
            return False
        if self._needs_target() and not self.sel_target:
            return False
        if len(self.sel_cells) != self._n_cells():
            return False
        if self.mode_power == REPAIR:
            return len(self.sel_cells) == 1 and self.sel_cells[0] in self.own_damage and not self._is_cell_of_sunk_ship(self.sel_cells[0][0], self.sel_cells[0][1])
        if self.mode_power == SALVO and not cells_form_line(self.sel_cells):
            return False
        return True

    def _cell_allowed(self, c, r):
        if self.mode_power == MINE:
            if self.my_mine is not None:
                return False
            return (c, r) not in self.own_ship
        if self.mode_power == REPAIR:
            return (c, r) in self.own_ship and (c, r) in self.own_damage and not self._is_cell_of_sunk_ship(c, r)
        if self.mode_power == SHIELD:
            return 1 <= c <= GRID_SIZE - 2 and 1 <= r <= GRID_SIZE - 2
        if self.mode_power == SONAR:
            return True
        if not self._own_grid() and self.sel_target:
            if (c, r) in self.opp_grid.get(self.sel_target, {}):
                return False
        return True

    def _toggle_cell(self, c, r):
        n = self._n_cells()
        if n == 1:
            self.sel_cells = [(c, r)]
        elif (c, r) in self.sel_cells:
            self.sel_cells.remove((c, r))
        else:
            if len(self.sel_cells) == n:
                self.sel_cells.pop(0)
            self.sel_cells.append((c, r))

    def _cell_color(self, c, r):
        sel = False
        if self.mode_power in (SHIELD, SONAR) and len(self.sel_cells) == 1:
            sel = abs(c - self.sel_cells[0][0]) <= 1 and abs(r - self.sel_cells[0][1]) <= 1
        else:
            sel = (c, r) in self.sel_cells
        if sel:
            return YELLOW
        if self.viewing_own_fleet or self._own_grid():
            if self.my_mine == (c, r):
                return MAROON
            if (c, r) in self.own_blocked:
                return PURPLE
            if (c, r) in self.own_ship:
                if (c, r) in self.own_damage:
                    return RED
                return BLUE
            if (c, r) in self.own_misses:
                return NAVY
            return DARKGREY
        elif self.sel_target:
            st = self.opp_grid.get(self.sel_target, {}).get((c, r))
            if st == "HIT":
                return RED
            elif st in ("MISS", "MINE"):
                return NAVY
            elif st == "BLOCKED":
                return PURPLE
        return DARKGREY

    def _get_tab_rect(self, tab_idx):
        total_tabs = 1 + len(self.opponent_ids)
        if total_tabs >= 4:
            w = 54
            x = 5 + tab_idx * (54 + 4)
        elif total_tabs == 3:
            w = 70
            x = 8 + tab_idx * (70 + 6)
        else:
            w = 100
            x = 15 + tab_idx * (100 + 10)
        return x, UI_TARGET_BTN_Y, w, UI_TARGET_BTN_H

    def _draw_all_tabs(self):
        total_tabs = 1 + len(self.opponent_ids)
        for i in range(total_tabs):
            x, y, w, h = self._get_tab_rect(i)
            if i == 0:
                sel = self.viewing_own_fleet
                fill = CYAN if sel else NAVY
                fg = BLACK if sel else WHITE
                label = "FLEET" if total_tabs >= 4 else "MY FLEET"
                sz = 1 if total_tabs >= 4 else 2
            else:
                opp_id = self.opponent_ids[i - 1]
                sel = (not self.viewing_own_fleet and self.sel_target == opp_id)
                fill = YELLOW if sel else NAVY
                fg = BLACK if sel else WHITE
                label = f"TARGET T{opp_id}" if total_tabs == 2 else f"T{opp_id}"
                sz = 2
            self.button(x, y, w, h, fill, label, size=sz, color=fg)

    def _draw_grid_cells(self):
        own = self.viewing_own_fleet or self._own_grid()
        for r in range(GRID_SIZE):
            for c in range(GRID_SIZE):
                x = UI_GRID_X + c * UI_CELL_SIZE
                y = UI_GRID_Y + r * UI_CELL_SIZE
                color = self._cell_color(c, r)
                self.rect(x + 1, y + 1, UI_CELL_SIZE - 2, UI_CELL_SIZE - 2, fill=color, outline=WHITE)
                if own:
                    if color == MAROON:
                        self.text(x + 8, y + 6, "M", 2, YELLOW)
                    elif (c, r) in self.own_blocked:
                        self.text(x + 8, y + 6, "S", 2, WHITE)
                    elif (c, r) in self.own_damage:
                        self.text(x + 8, y + 6, "X", 2, WHITE)
                    elif (c, r) in self.own_misses:
                        self.circle(x + UI_CELL_SIZE // 2, y + UI_CELL_SIZE // 2, 4, fill=CYAN, outline=None)
                elif not own and self.sel_target:
                    st = self.opp_grid.get(self.sel_target, {}).get((c, r))
                    if st == "HIT":
                        self.text(x + 8, y + 6, "X", 2, WHITE)
                    elif st in ("MISS", "MINE"):
                        self.circle(x + UI_CELL_SIZE // 2, y + UI_CELL_SIZE // 2, 4, fill=CYAN, outline=None)
                    elif st == "BLOCKED":
                        self.text(x + 8, y + 6, "S", 2, WHITE)

    def _draw_waiting(self):
        self.text(10, 8, f"WAITING (T{self.waiting_team})", 2, YELLOW)
        if self.viewing_own_fleet:
            total = len(self.own_ship)
            dmg = len(self.own_damage)
            self.text(10, 26, f"Viewing My Fleet - HP: {total - dmg}/{total}", 1, LIGHTGREY)
        else:
            self.text(10, 26, f"Viewing Radar: TEAM {self.sel_target}", 1, LIGHTGREY)

        self._draw_all_tabs()
        self._draw_grid_cells()

        self.button(10, UI_ATTACK_BTN_Y, 220, UI_ATTACK_BTN_H, NAVY, f"WAITING FOR TEAM {self.waiting_team}...", size=1)

    def _draw_action(self):
        strike = self.mode_power == 0
        spec = self._spec()
        self.text(10, 8, "YOUR TURN" if strike else spec[0], 2, GREEN if strike else YELLOW)

        if strike:
            if self.viewing_own_fleet:
                total = len(self.own_ship)
                dmg = len(self.own_damage)
                self.text(10, 26, f"Viewing Fleet - HP: {total - dmg}/{total} (Intact)", 1, WHITE)
            else:
                self.text(10, 26, f"Radar: TEAM {self.sel_target} | Pick target cell", 1, WHITE)
        else:
            if self.mode_power == MINE:
                l1 = f"MINE ACTIVE at {self.my_mine}" if self.my_mine else "Tap EMPTY cell of YOUR board"
            elif self.mode_power == REPAIR:
                dmg = len(self.own_damage)
                l1 = "ALL SHIPS INTACT!" if dmg == 0 else "Tap RED damaged cell (X)"
            elif self.mode_power == SHIELD:
                l1 = "Tap CENTRE of 3x3 block on board"
            elif self.mode_power == SMOKE:
                l1 = "No cells to pick"
            else:
                l1 = "Pick target cell(s)"
            self.text(10, 24, l1, 1, WHITE)

        self._draw_all_tabs()

        if not strike and spec[4] == 0:
            self.text(10, 96, "SMOKE SCREEN", 2)
            self.text(10, 130, "Until your next turn, attacks on you", 1)
            self.text(10, 144, "are hidden from the attacker.", 1)
            self.text(10, 168, "Sonar pings at you are jammed.", 1)
        else:
            self._draw_grid_cells()

        self.button(UI_ALT_BTN_X, UI_ATTACK_BTN_Y, UI_ALT_BTN_W, UI_ATTACK_BTN_H, NAVY, "POWERS" if strike else "BACK")
        if strike:
            label = "PICK ENEMY" if self.viewing_own_fleet else "ATTACK"
        elif self.mode_power == MINE and self.my_mine is not None:
            label = "MINE ARMED"
        else:
            label = spec[5]
        self.button(UI_ATTACK_BTN_X, UI_ATTACK_BTN_Y, UI_ATTACK_BTN_W, UI_ATTACK_BTN_H,
                    RED if self._action_ready() else DARKGREY, label)
        if time.time() * 1000 < self.send_lock_until:
            self.text(125, 12, "Sent - waiting...", 1)

    # ---- power menu ----
    def _menu_items(self):
        items = [p for p in MENU_ORDER if p in self.loadout][:3]
        return items + [SHIELD]

    def _usable(self, pid):
        return pid == SHIELD or self.uses_left.get(pid, 0) > 0

    def _draw_menu(self):
        self.text(10, 8, "POWERS", 2, GREEN)
        self.text(10, 26, "Using a power takes your turn.", 1)
        for i, pid in enumerate(self._menu_items()):
            y = UI_MENU_Y0 + i * (UI_MENU_H + UI_MENU_GAP)
            usable = self._usable(pid)
            self.rect(UI_MENU_X, y, UI_MENU_W, UI_MENU_H, fill=NAVY if usable else DARKGREY)
            self.text(UI_MENU_X + 10, y + 8, SPECS[pid][0], 2)
            self.text(UI_MENU_X + 10, y + 32, SPECS[pid][1], 1)
            if pid == SHIELD:
                self.text(UI_MENU_X + 150, y + 20, "COOLDOWN" if self.last_move_was_shield else "ALWAYS", 1)
            else:
                self.text(UI_MENU_X + 192, y + 8, str(self.uses_left.get(pid, 0)), 3)
                self.text(UI_MENU_X + 184, y + 36, "left", 1)
        self.button(UI_ALT_BTN_X, UI_ATTACK_BTN_Y, UI_ALT_BTN_W, UI_ATTACK_BTN_H, NAVY, "BACK")

    # ------------------------------------------------------------------
    # TOUCH / CLICK HANDLING
    # ------------------------------------------------------------------
    @staticmethod
    def _in(px, py, x, y, w, h):
        return x <= px < x + w and y <= py < y + h

    def _click_team_select(self, px, py):
        if self._in(px, py, 15, 85, 210, 48):
            self.select_team(1)
        elif self._in(px, py, 15, 135, 210, 48):
            self.select_team(2)
        elif self._in(px, py, 15, 185, 210, 48):
            self.select_team(3)
        elif self._in(px, py, 15, 235, 210, 48):
            self.select_team(4)

    def on_canvas_click(self, event):
        px, py = event.x / SCALE, event.y / SCALE
        is_attack_btn = (self.screen == "ACTION" and self._in(px, py, UI_ATTACK_BTN_X, UI_ATTACK_BTN_Y, UI_ATTACK_BTN_W, UI_ATTACK_BTN_H))
        if not is_attack_btn:
            self.send_serial_line(f"@TOUCH {int(px)} {int(py)}")

        if self.banner or self.banner_active:
            self.dismiss_banner()
            if self.screen in ("ACTION", "WAITING"):
                total_tabs = 1 + len(self.opponent_ids)
                for i in range(total_tabs):
                    tx, ty, tw, th = self._get_tab_rect(i)
                    if self._in(px, py, tx, ty, tw, th):
                        if i == 0:
                            self.viewing_own_fleet = True
                        else:
                            self.viewing_own_fleet = False
                            self.sel_target = self.opponent_ids[i - 1]
                            self.sel_cells = []
                        self.redraw()
                        return
            return
        if self.screen == "TEAM_SELECT":
            return self._click_team_select(px, py)
        if self.screen == "WIZARD":
            return self._click_wizard(px, py)
        if self.screen == "LOADOUT":
            return self._click_loadout(px, py)
        if self.screen == "MENU":
            return self._click_menu(px, py)

        # Check tab clicks (FLEET + Opponent tabs) on both ACTION and WAITING screens
        if self.screen in ("ACTION", "WAITING"):
            total_tabs = 1 + len(self.opponent_ids)
            for i in range(total_tabs):
                tx, ty, tw, th = self._get_tab_rect(i)
                if self._in(px, py, tx, ty, tw, th):
                    if i == 0:
                        if not self.viewing_own_fleet:
                            self.viewing_own_fleet = True
                            self.redraw()
                    else:
                        opp_id = self.opponent_ids[i - 1]
                        if self.viewing_own_fleet or self.sel_target != opp_id:
                            self.viewing_own_fleet = False
                            self.sel_target = opp_id
                            self.sel_cells = []
                            self.redraw()
                    return

        if self.screen != "ACTION" or not (self.running and self.my_turn):
            return
        if time.time() * 1000 < self.send_lock_until:
            return
        if self._n_cells() > 0 and self._in(px, py, UI_GRID_X, UI_GRID_Y, GRID_SIZE * UI_CELL_SIZE, GRID_SIZE * UI_CELL_SIZE):
            if self.mode_power == 0 and self.viewing_own_fleet:
                return
            c, r = int((px - UI_GRID_X) // UI_CELL_SIZE), int((py - UI_GRID_Y) // UI_CELL_SIZE)
            if self._cell_allowed(c, r):
                self._toggle_cell(c, r)
                self.redraw()
            return
        if self._in(px, py, UI_ALT_BTN_X, UI_ATTACK_BTN_Y, UI_ALT_BTN_W, UI_ATTACK_BTN_H):
            self.screen = "MENU"          # POWERS opens the menu; BACK on a power screen returns to it
            return self.redraw()
        if is_attack_btn and self._action_ready():
            self._send_selection()

    def _click_wizard(self, px, py):
        if self.wiz_review_mode:
            # 1. RANDOM / NEXT button (10, 258, 105, 26)
            if self._in(px, py, 10, 258, 105, 26):
                self.wiz_default_choice = (self.wiz_default_choice + 1) % len(DEFAULT_CONFIGS)
                self.wiz_ships = list(DEFAULT_CONFIGS[self.wiz_default_choice])
                return self.redraw()

            # 2. MANUAL PLACE button (125, 258, 105, 26)
            if self._in(px, py, 125, 258, 105, 26):
                self.wiz_review_mode = False
                self.wiz_ship_idx = 0
                return self.redraw()

            # 3. CONFIRM & LOCK FLEET (10, 288, 220, 28)
            if self._in(px, py, 10, 288, 220, 28):
                return self.confirm_locked_default_ships()
            return

        # 1. Grid Tap
        if self._in(px, py, WIZ_GRID_X, WIZ_GRID_Y, GRID_SIZE * WIZ_CELL_SIZE, GRID_SIZE * WIZ_CELL_SIZE):
            c = int((px - WIZ_GRID_X) // WIZ_CELL_SIZE)
            r = int((py - WIZ_GRID_Y) // WIZ_CELL_SIZE)
            if 0 <= c < GRID_SIZE and 0 <= r < GRID_SIZE:
                ln, _, _, o = self.wiz_ships[self.wiz_ship_idx]
                self.wiz_ships[self.wiz_ship_idx] = (ln, c, r, o)
                return self.redraw()

        # 2. ROTATE button (10, 258, 105, 26)
        if self._in(px, py, 10, 258, 105, 26):
            self.rotate_current_ship()
            return self.redraw()

        # 3. USE DEFAULT button (125, 258, 105, 26)
        if self._in(px, py, 125, 258, 105, 26):
            return self.use_default_ships()

        # 4. PREV button (10, 288, 65, 28)
        if self.wiz_ship_idx > 0 and self._in(px, py, 10, 288, 65, 28):
            self.wiz_ship_idx -= 1
            return self.redraw()

        # 5. CONFIRM button (82, 288, 148, 28)
        if self._in(px, py, 82, 288, 148, 28):
            valid, _ = self._wiz_ship_valid()
            if valid:
                if self.wiz_ship_idx < 3:
                    self.wiz_ship_idx += 1
                    return self.redraw()
                else:
                    return self.lock_wizard_ships()

    def _send_selection(self):
        if time.time() * 1000 < self.send_lock_until:
            return
        self.send_lock_until = time.time() * 1000 + 2000
        if self.mode_power == 0:
            x, y = self.sel_cells[0]
            self.last_attack_sent = (self.sel_target, x, y)
            self.send_serial_line(f"{self.sel_target} {x} {y}")
        else:
            name = SPECS[self.mode_power][0]
            parts = ["POWER", name]
            if SPECS[self.mode_power][2]:
                parts.append(str(self.sel_target))
            for x, y in self.sel_cells:
                parts += [str(x), str(y)]
            self.last_send = (self.mode_power, self.sel_target, list(self.sel_cells))
            self.send_serial_line(" ".join(parts))
        self.redraw()

    def _click_menu(self, px, py):
        if self._in(px, py, UI_ALT_BTN_X, UI_ATTACK_BTN_Y, UI_ALT_BTN_W, UI_ATTACK_BTN_H):
            return self.start_strike()
        for i, pid in enumerate(self._menu_items()):
            y = UI_MENU_Y0 + i * (UI_MENU_H + UI_MENU_GAP)
            if self._in(px, py, UI_MENU_X, y, UI_MENU_W, UI_MENU_H):
                if self._usable(pid):
                    self.start_power(pid)
                return

    def _click_loadout(self, px, py):
        if self.loadout_locked:
            return
        for i, pid in enumerate(LD_ATTACK_IDS):
            x = LD_COL2_X if i % 2 else LD_COL1_X
            y = LD_ATK_Y2 if i // 2 else LD_ATK_Y1
            if self._in(px, py, x, y, LD_BTN_W, LD_BTN_H):
                if pid in self.ld_attack:
                    self.ld_attack.discard(pid)
                elif len(self.ld_attack) < 2:
                    self.ld_attack.add(pid)
                self.ld_armed_until = 0
                return self.redraw()
        for j, pid in enumerate(LD_DEFENCE_IDS):
            if self._in(px, py, LD_COL2_X if j else LD_COL1_X, LD_DEF_Y, LD_BTN_W, LD_BTN_H):
                self.ld_defence = None if self.ld_defence == pid else pid
                self.ld_armed_until = 0
                return self.redraw()
        if self._in(px, py, LD_OK_X, LD_OK_Y, LD_OK_W, LD_OK_H) and self._ld_valid():
            now = time.time() * 1000
            if not (self.ld_armed_until and now < self.ld_armed_until):
                self.ld_armed_until = now + 3000          # first tap arms it, the second one locks in
                return self.redraw()
            ids = sorted(self.ld_attack) + [self.ld_defence]
            self.ld_armed_until = 0
            self.send_serial_line("LOADOUT " + " ".join(str(i) for i in ids))
            # the board answers "Loadout locked ... (byte 0x..)", which switches the window over;
            # with no board connected, lock it here so the rest of the screens can be tried
            if not (self.serial_conn and self.serial_conn.is_open):
                byte = 0x20
                for i in ids:
                    byte |= 1 << (i - 1)
                self.apply_loadout_byte(byte)
                self.handle_line("Waiting for the organizer to start the game...")


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description="Meraz Battleship - Virtual TFT Display")
    parser.add_argument("--port", type=str, default=None, help="Serial port of participant ESP32 (e.g. COM4)")
    parser.add_argument("--team", type=int, default=None, help="Team ID (1-4, optional - defaults to interactive selection)")
    args = parser.parse_args()

    app = VirtualTFTDisplay(default_port=args.port, default_team=args.team)
    try:
        app.mainloop()
    finally:
        app.running_app = False
        if app.serial_conn:
            try:
                app.serial_conn.close()
            except Exception:
                pass

