/* =====================================================================
   MERAZ BATTLESHIP - OFFICIAL PARTICIPANT STARTER CODE
   =====================================================================
   WHAT THIS FILE IS
     The finished firmware for one team's ESP32 + touch display. The
     SAME file goes on all 4 team boards; only the CONFIGURATION block
     just below changes (team ID, team name, ships, Central's MAC).
     (Participants who are building their own firmware use
     participant_starter.ino + PARTICIPANT_GUIDE.md instead.)

   WHERE IT FITS IN THE WHOLE SYSTEM
       this board  <--ESP-NOW radio-->  central_node.ino  <--USB-->  dashboard.py
     Central (central_node.ino) is the only referee. This board NEVER
     decides whether a shot is valid - it asks Central and shows the
     answer. All real game rules live in game_logic.h on the Central side.

   FILES THIS SKETCH NEEDS
     meraz_loadout.h the power ids, the loadout byte and its validity check
                     (keep it in the same folder as this sketch)
     VirtualTFT.h    only when USE_VIRTUAL_DISPLAY is switched on (below)
     NOTE: this file keeps its OWN copy of the constants and packet
     structs (it does not include game_logic.h). They must stay
     byte-for-byte identical to the ones in central_node.ino and
     game_logic.h, or Central will ignore this board's packets.

   WHAT THE BOARD DOES
     1. LOADOUT: the team picks 2 attack powers (Sonar, Salvo, Mine, Double
        Attack) and 1 defence power (Smoke Screen, Repair) and locks them in.
        Single Strike and Shield are always theirs. NOTHING is sent to
        Central before the loadout is locked.
     2. Registers with Central (retrying until accepted), loadout byte included.
     3. Listens for turn updates so it knows when it is this team's turn.
     4. On your turn: Single Strike (target team, tap a cell, ATTACK) or
        POWERS -> pick a power -> set it up -> press its button.
        Every power takes the turn. Shield: never on two turns in a row.
     5. Shows the result (MISS / HIT / ELIMINATED / MINE / BLOCKED / NO REPORT /
        INVALID, or the power's result) from Central.

   HOW THE FILE IS ORGANISED (top to bottom)
     CONFIGURATION            <- the only part you edit per board
     TFT DISPLAY CONFIGURATION
     PACKET STRUCTURES + CONSTANTS (must match Central)
     ESP-NOW communication
     SERIAL COMMANDS          <- virtual display input / screen touch feed
     LOADOUT AND POWER-UPS    <- what the powers are, local checks, sending
     UI LAYER                 <- everything that draws or reads touches

   NO DISPLAY YET?
     Uncomment  #define USE_VIRTUAL_DISPLAY  (near the includes). The
     same UI then draws on a virtual screen in your browser through
     virtual_display.py, and mouse clicks act as touches. Details are
     in VirtualTFT.h. Comment it out again for the real TFT.

   SERIAL COMMANDS:
     Manual gameplay commands via Serial are disabled (gameplay is controlled
     exclusively via the touch screen interface). If USE_VIRTUAL_DISPLAY is
     enabled, mouse clicks and demo UI commands are received via Serial.
   ===================================================================== */

#include <WiFi.h>
#include <esp_now.h>
#include "esp_wifi.h"
#include <esp_idf_version.h>
#include "soc/soc.h"
#include "soc/rtc_cntl_reg.h"
#include <string.h>
#include <stdio.h>
#include "meraz_loadout.h"   // power ids, loadout byte, loadoutIsValid() - must sit next to this sketch

// ---------------------------------------------------------------------
// NO DISPLAY YET? Uncomment the next line to run the exact same UI on a
// virtual screen in your browser (see VirtualTFT.h and
// virtual_display.py). Comment it out again for the real TFT.
// ---------------------------------------------------------------------
 #define USE_VIRTUAL_DISPLAY

#ifdef USE_VIRTUAL_DISPLAY
  #include "VirtualTFT.h"   // must be in the same folder as this sketch
#else
  #include <SPI.h>
  #include <TFT_eSPI.h>     // library: bollworm/Bodmer's TFT_eSPI - see the setup note below
#endif

// =====================================================================
// A couple of definitions the CONFIGURATION section below needs.
// You shouldn't need to touch these.
// =====================================================================

#define NUM_SHIPS         4
#define TOTAL_SHIP_CELLS  16

// Ship directions & shapes:
// Straight lines (used by Ship 1 [len 5], Ship 2 [len 4], and Ship 4 [len 3]):
#define ORIENT_HORIZONTAL 0   // rightward    (dx=+1, dy= 0)
#define ORIENT_VERTICAL   1   // downward     (dx= 0, dy=+1)
#define ORIENT_DIAG_DOWN  2   // down-right   (dx=+1, dy=+1)  e.g. (0,0) -> (4,4)
#define ORIENT_DIAG_UP    3   // up-right     (dx=+1, dy=-1)  e.g. (0,4) -> (4,0)

// T-Shapes (4 cells, for Ship 3):
#define SHAPE_T_DOWN      4   // bar top, stem bottom
#define SHAPE_T_UP        5   // bar bottom, stem top
#define SHAPE_T_RIGHT     6   // bar left, stem right
#define SHAPE_T_LEFT      7   // bar right, stem left

// L-Shapes (4 cells, for Ship 3: 3-cell bar + 1 foot):
#define SHAPE_L_0         8   // vert bar left, foot bottom-right
#define SHAPE_L_1         9   // horiz bar top, foot bottom-left
#define SHAPE_L_2         10  // vert bar right, foot top-left
#define SHAPE_L_3         11  // horiz bar bottom, foot top-right
#define SHAPE_J_0         12  // vert bar right, foot bottom-left
#define SHAPE_J_1         13  // horiz bar top, foot bottom-right
#define SHAPE_J_2         14  // vert bar left, foot top-right
#define SHAPE_J_3         15  // horiz bar bottom, foot top-left

// 3-Cell Corner L (for Ship 4):
#define SHAPE_CORNER_RD   16  // corner top-left, arms right & down
#define SHAPE_CORNER_LD   17  // corner top-right, arms left & down
#define SHAPE_CORNER_RU   18  // corner bottom-left, arms right & up
#define SHAPE_CORNER_LU   19  // corner bottom-right, arms left & up

// One ship: how long it is, where it starts, and which way it points.
typedef struct __attribute__((packed)) {
  uint8_t ship_len;     // 5, 4, 4, or 3
  uint8_t start_x;      // column, 0-6
  uint8_t start_y;      // row, 0-6
  uint8_t orientation;  // ORIENT_* or SHAPE_*
} ShipPlacement;

// =====================================================================
// CONFIGURATION - CHANGE THESE FOR EACH TEAM'S BOARD
// =====================================================================
// This is the ONLY section that should differ between the 4 boards.

// You no longer need to hardcode this before compiling!
// On startup, an on-screen Team Selection menu lets you tap your assigned
// squad number (1, 2, 3, or 4), or type TEAM <1-4> in Serial before registering.
uint8_t myTeamId = 1;         // Active team ID (selected at startup)
bool teamSelected = false;    // Becomes true once selected on screen or Serial
#define MY_TEAM_ID myTeamId
const char *MY_TEAM_NAME = "TRIcom7";

// Your 4 ships (total 16 HP):
//   Ship 1 (len 5): straight (H, V, DiagDown, DiagUp)
//   Ship 2 (len 4): straight (H, V, DiagDown, DiagUp)
//   Ship 3 (len 4): T-shape or L-shape
//   Ship 4 (len 3): straight or 3-cell corner L
const ShipPlacement DEFAULT_CONFIGS[10][NUM_SHIPS] = {
  // Config 1: Clean horizontal / vertical split
  { { 5, 0, 0, ORIENT_HORIZONTAL }, { 4, 0, 2, ORIENT_HORIZONTAL }, { 4, 0, 4, ORIENT_HORIZONTAL }, { 3, 5, 0, ORIENT_VERTICAL } },
  // Config 2: Vertical heavy
  { { 5, 0, 1, ORIENT_VERTICAL },   { 4, 2, 0, ORIENT_VERTICAL },   { 4, 4, 2, ORIENT_VERTICAL },   { 3, 6, 1, ORIENT_VERTICAL } },
  // Config 3: Diagonal main fleet
  { { 5, 0, 0, ORIENT_DIAG_DOWN },  { 4, 0, 6, ORIENT_HORIZONTAL }, { 4, 5, 0, ORIENT_VERTICAL },   { 3, 2, 0, ORIENT_HORIZONTAL } },
  // Config 4: Anti-diagonal strike fleet
  { { 5, 0, 4, ORIENT_DIAG_UP },    { 4, 0, 6, ORIENT_HORIZONTAL }, { 4, 6, 0, ORIENT_VERTICAL },   { 3, 0, 0, ORIENT_HORIZONTAL } },
  // Config 5: Perimeter & grid defense
  { { 5, 1, 0, ORIENT_HORIZONTAL }, { 4, 0, 2, ORIENT_VERTICAL },   { 4, 6, 2, ORIENT_VERTICAL },   { 3, 2, 6, ORIENT_HORIZONTAL } },
  // Config 6: Dual diagonal cross tactic
  { { 5, 5, 0, ORIENT_VERTICAL },   { 4, 0, 0, ORIENT_DIAG_DOWN },  { 4, 0, 6, ORIENT_HORIZONTAL }, { 3, 4, 4, ORIENT_VERTICAL } },
  // Config 7: Staggered combat lines
  { { 5, 0, 1, ORIENT_HORIZONTAL }, { 4, 1, 3, ORIENT_HORIZONTAL }, { 4, 0, 5, ORIENT_HORIZONTAL }, { 3, 6, 2, ORIENT_VERTICAL } },
  // Config 8: Mixed horizontal & vertical wall
  { { 5, 0, 6, ORIENT_HORIZONTAL }, { 4, 1, 0, ORIENT_VERTICAL },   { 4, 3, 1, ORIENT_VERTICAL },   { 3, 5, 0, ORIENT_VERTICAL } },
  // Config 9: Outer ring fortress
  { { 5, 0, 0, ORIENT_VERTICAL },   { 4, 1, 0, ORIENT_HORIZONTAL }, { 4, 2, 6, ORIENT_HORIZONTAL }, { 3, 6, 3, ORIENT_VERTICAL } },
  // Config 10: Tactical diagonal & horizontal spread
  { { 5, 1, 6, ORIENT_HORIZONTAL }, { 4, 0, 1, ORIENT_DIAG_DOWN },  { 4, 5, 0, ORIENT_VERTICAL },   { 3, 0, 4, ORIENT_HORIZONTAL } },
};

ShipPlacement myShips[NUM_SHIPS] = {
  { 5, 0, 0, ORIENT_HORIZONTAL },  // Ship 1: (0,0)..(4,0)
  { 4, 0, 2, ORIENT_HORIZONTAL },  // Ship 2: (0,2)..(3,2)
  { 4, 0, 4, ORIENT_HORIZONTAL },  // Ship 3: (0,4)..(3,4)
  { 3, 5, 0, ORIENT_VERTICAL   },  // Ship 4: (5,0),(5,1),(5,2)
};

ShipPlacement wizShips[NUM_SHIPS] = {
  { 5, 0, 0, ORIENT_HORIZONTAL },
  { 4, 0, 2, ORIENT_HORIZONTAL },
  { 4, 0, 4, ORIENT_HORIZONTAL },
  { 3, 5, 0, ORIENT_VERTICAL   },
};

// TODO: replace with the Central ESP32's real MAC address.
// The Central prints its own MAC on boot - copy it from there.
uint8_t centralMac[6] = {
  0x6c, 0xc8, 0x40, 0x86, 0xfb, 0x20
};

// All 5 boards (Central + 4 participants) must use the SAME channel.
#define ESPNOW_CHANNEL 1

// =====================================================================
// STATUS LEDS & AUDIO BUZZER CONFIGURATION
// =====================================================================
// Green LED: ON when it is YOUR TURN to play, OFF otherwise
// Red LED:   Flashes when your shot HITS an opponent's ship
// Buzzer:    Plays alert sound on YOUR TURN and confirmation beep on HIT
// (Set any pin to -1 if that peripheral is not wired to your board)
#define LED_GREEN_PIN   32   // Green LED (turn indicator)
#define LED_RED_PIN     33   // Red LED (hit indicator)
#define BUZZER_PIN      26   // Audio Piezo Buzzer

// =====================================================================
// TFT DISPLAY CONFIGURATION - 2.4" SPI touch TFT (ILI9341 + XPT2046)
// =====================================================================
// ONE-TIME LIBRARY SETUP (not per-sketch): TFT_eSPI needs its pin
// wiring set in the LIBRARY's own User_Setup.h, not here. Find
// TFT_eSPI/User_Setup.h in your Arduino libraries folder and paste
// this in (replacing whatever driver/pin lines are already there):
//
//   #define ILI9341_DRIVER
//   #define TFT_CS   5
//   #define TFT_DC   2
//   #define TFT_RST  4
//   #define TFT_MOSI 23
//   #define TFT_SCLK 18
//   #define TFT_MISO 19
//   #define TOUCH_CS 15
//   #define SPI_FREQUENCY        40000000
//   #define SPI_TOUCH_FREQUENCY  2500000
//
// This matches the wiring table already worked out for this project;
// rewire (and edit these lines to match) if you used different pins.
//
// PER-BOARD SETUP (do this once per physical board - touch panels are
// calibrated individually): run the TFT_eSPI example sketch at
// File > Examples > TFT_eSPI > Generic > Touch_calibrate on THIS
// board, then paste the 5 numbers it prints below.
#if !defined(USE_VIRTUAL_DISPLAY) && !defined(TOUCH_CS)
  #warning "TOUCH_CS is not defined in TFT_eSPI's User_Setup.h - touch will be disabled. Add: #define TOUCH_CS 15"
#endif
uint16_t touchCalData[5] = { 300, 3600, 300, 3600, 7 };  // PLACEHOLDER - replace per board

#ifdef USE_VIRTUAL_DISPLAY
VirtualTFT tft;
#else
TFT_eSPI tft = TFT_eSPI();
#endif
uint8_t numOpponents = 3;
uint8_t opponentIds[3] = {2, 3, 4};  // active opponent team IDs, dynamically updated from Central

// =====================================================================
// EVERYTHING BELOW THIS LINE IS THE SAME FOR ALL 4 TEAMS
// =====================================================================

#define MAX_TEAMS  4
#define GRID_SIZE  7

// FeedbackPacket.result_code (must match central_node.ino)
#define RESULT_MISS      0
#define RESULT_HIT       1
#define RESULT_INVALID   2
#define RESULT_SUNK      3
#define RESULT_MINE      4   // the cell held a mine: counts as a miss, you lose your next turn
#define RESULT_BLOCKED   5   // the cell is under a shield: nothing happened, turn used
#define RESULT_UNKNOWN   6   // the target is behind a smoke screen: you are not told
#define RESULT_SHIP_SUNK 7   // an individual ship was sunk (team still alive)
#define RESULT_REPAIRED  8   // an opponent repaired a cell: radar tracking is cleared

// PowerResultPacket.status (must match central_node.ino)
#define PWR_STATUS_OK        0
#define PWR_STATUS_REJECTED  1
#define PWR_STATUS_JAMMED    2   // sonar blocked by smoke: the use and the turn are still spent

#define POWER_USES  2            // uses of each optional power per match

// RegAckPacket.status (must match central_node.ino)
#define REG_OK                   0
#define REG_REJECTED             1
#define REG_RECONNECTED          2
#define REG_RECONNECT_REJECTED   3

// round_state (must match central_node.ino)
#define STATE_SETUP     0
#define STATE_READY     1
#define STATE_RUNNING   2
#define STATE_GAMEOVER  3

#define MAX_PACKET_SIZE     64
#define PENDING_QUEUE_SIZE  4
#define REG_RETRY_INTERVAL_MS 2000  // resend registration this often until acknowledged

// =====================================================================
// PACKET STRUCTURES (must match central_node.ino exactly, byte for byte)
// =====================================================================

typedef struct __attribute__((packed)) {
  uint8_t team_id;
  char team_name[20];
  ShipPlacement ships[NUM_SHIPS];
  uint8_t loadout;       // the locked-in loadout byte (see meraz_loadout.h)
} RegistrationPacket;   // 38 bytes - we send this

typedef struct __attribute__((packed)) {
  uint8_t status;        // REG_* code
} RegAckPacket;          // 1 byte - Central sends this back

typedef struct __attribute__((packed)) {
  uint8_t attacker_id;
  uint8_t target_id;
  uint8_t x;
  uint8_t y;
} AttackPacket;          // 4 bytes - we send this

typedef struct __attribute__((packed)) {
  uint8_t target_id;
  uint8_t x;
  uint8_t y;
  uint8_t result_code;   // RESULT_* code
} FeedbackPacket;        // 4 bytes - Central sends this back

typedef struct __attribute__((packed)) {
  uint8_t current_turn;  // 1-4, or 0 if no team currently has the turn
  uint8_t round_state;   // STATE_* code
  uint8_t active_mask;   // bit 0=T1, bit 1=T2, bit 2=T3, bit 3=T4 (registered & !eliminated)
} TurnUpdatePacket;      // 3 bytes - Central broadcasts this to everyone

typedef struct __attribute__((packed)) {
  uint8_t power;         // POWER_* id
  uint8_t team_id;       // our team id
  uint8_t target_id;     // the opponent for Sonar / Salvo / Double, else 0
  uint8_t x1, y1;        // meaning depends on the power (see PARTICIPANT_GUIDE.md)
  uint8_t x2, y2;
  uint8_t x3, y3;
} PowerUpPacket;         // 9 bytes - we send this

typedef struct __attribute__((packed)) {
  uint8_t power;
  uint8_t status;        // PWR_STATUS_*
  uint8_t target_id;
  uint8_t count;         // Sonar: ship cells found in the 3x3 area
  uint8_t result[3];     // Salvo / Double: result per cell (RESULT_*, 2 = not fired)
} PowerResultPacket;     // 7 bytes - Central sends this back

static_assert(sizeof(RegistrationPacket) == 38, "RegistrationPacket must be 38 bytes");
static_assert(sizeof(AttackPacket) == 4, "AttackPacket must be 4 bytes");
static_assert(sizeof(PowerUpPacket) == 9, "PowerUpPacket must be 9 bytes");
static_assert(sizeof(PowerResultPacket) == 7, "PowerResultPacket must be 7 bytes");
static_assert(sizeof(RegAckPacket) == 1 && sizeof(FeedbackPacket) == 4 && sizeof(TurnUpdatePacket) == 3,
              "reply packets must be 1, 4 and 3 bytes (Central tells packets apart by length)");

// =====================================================================
// GAME STATE (what THIS board currently believes about the match)
// =====================================================================

bool registrationConfirmed = false;   // true once Central sends REG_OK / REG_RECONNECTED
uint8_t currentTurnTeam = 0;          // 0 = unknown / no active turn
uint8_t roundState = STATE_SETUP;     // mirrors Central's round_state
unsigned long lastRegAttemptMillis = 0;
unsigned long lastRegAcceptedMillis = 0;

// ---- loadout + power-ups (what this team chose, and local bookkeeping) ----
// Central is the referee for ALL of this. These copies only drive the screens,
// so a wrong guess here can never cheat: Central simply refuses the move.
uint8_t myLoadout = 0;               // the locked-in loadout byte (shield bit set)
bool loadoutLocked = false;          // nothing goes to Central before this is true
bool demoNoRegister = false;         // DEMO commands pretend a loadout without registering
uint8_t usesLeft[8] = {0, 0, 0, 0, 0, 0, 0, 0};   // uses left, indexed by power id (the shield is unlimited)
uint8_t shieldCooldownTurns = 0;   // hint for menu only: Central enforces 2-round cooldown
bool ownShip[GRID_SIZE][GRID_SIZE];
bool ownDamage[GRID_SIZE][GRID_SIZE] = { false };  // where my ships are (built from myShips[])
bool ownMisses[GRID_SIZE][GRID_SIZE] = { false };  // shots taken on our water cells / mines
bool ownBlocked[GRID_SIZE][GRID_SIZE] = { false }; // shots deflected by smoke/shield (turns normal next round)
int8_t myMineX = -1, myMineY = -1;   // my last mine (Central never tells us when it goes off)
#define TRACK_UNKNOWN 0
#define TRACK_MISS    1
#define TRACK_HIT     2
#define TRACK_MINE    3
#define TRACK_BLOCKED 4
uint8_t oppGrid[MAX_TEAMS + 1][GRID_SIZE][GRID_SIZE]; // opponent attack radar tracking
uint8_t pendingPower = 0, pendingTarget = 0, pendingCells = 0;   // the power-up now on its way to Central
uint8_t pendingX[3] = {0, 0, 0}, pendingY[3] = {0, 0, 0};
unsigned long uiSendLockUntil = 0;   // briefly ignore touches after sending a move
unsigned long lastLoadoutPromptMillis = 0;
bool shipsLocked = false;
int wizShipIdx = 0;                  // 0..3 (Ship 1: len 5, Ship 2: len 4, Ship 3: len 4 shape, Ship 4: len 3)
bool uiMyTurnShown = false;          // my ACTION/MENU screen is up (so a repeated turn update doesn't wipe it)
uint8_t ldAttack = 0;                // LOADOUT screen: bit (power id) set for each attack picked
uint8_t ldDefence = 0;               // ... the defence power picked (0 = none)

// =====================================================================
// UI SCREEN IDS & STATE VARIABLES
// =====================================================================
#define SCR_NONE     0
#define SCR_ACTION   1
#define SCR_WAITING  2
#define SCR_MESSAGE  3
#define SCR_LOADOUT  4
#define SCR_MENU     5
#define SCR_WIZARD      6
#define SCR_TEAM_SELECT 7

#define UIM_STRIKE   0     // the ACTION screen is set up for the Single Strike
#define UIM_POWER    1     // ... or for the power in uiPower

unsigned long uiResultUntil = 0;   // while millis() < this, a result banner is showing
bool uiBannerShowing = false;      // a banner was drawn and the screen after it has not been drawn yet
uint8_t uiPendingScreen = 0;       // 0 = none, else SCR_ACTION / SCR_WAITING / SCR_MESSAGE
uint8_t uiPendingWaitingTeam = 0;
char uiPendingMessage[40] = "";

uint8_t uiLastScreen = SCR_NONE;
uint8_t uiLastWaitingTeam = 0;
char uiLastMessage[40] = "";

uint8_t uiMode = UIM_STRIKE;
uint8_t uiPower = 0;               // the power being set up (UIM_POWER)
uint8_t uiSelTarget = 0;           // 0 = no target chosen yet
uint8_t uiSelCount = 0;            // cells chosen so far
int8_t uiSelX[3] = {0, 0, 0}, uiSelY[3] = {0, 0, 0};
bool uiViewingOwnFleet = false;    // true when participant views their own fleet on grid
bool wizReviewMode = false;        // true when reviewing a randomly selected default config
uint8_t wizDefaultChoice = 0;      // index of default config being previewed (0-9)


// How each power looks on screen and how many cells it needs.
typedef struct {
  uint8_t id;
  const char *name;      // short, fits the buttons (size-2 text)
  const char *hint;      // one line, size-1 text
  bool needTarget;       // pick an opponent first
  bool ownGrid;          // the cells are on MY board, not an opponent's
  uint8_t cells;         // how many cells to pick (0 = none)
  const char *action;    // label of the big red button
} PowerSpec;

static const PowerSpec POWER_SPECS[7] = {
  { POWER_SONAR,  "SONAR",  "scan a 3x3 area",   true,  false, 1, "PING"       },
  { POWER_SALVO,  "SALVO",  "3 in a line", true,  false, 3, "FIRE x3"     },
  { POWER_MINE,   "MINE",   "hidden trap",       false, true,  1, "PLACE MINE"  },
  { POWER_REPAIR, "REPAIR", "fix a hit cell",    false, true,  1, "REPAIR"      },
  { POWER_SMOKE,  "SMOKE",  "deflect all",  false, false, 0, "DEPLOY"    },
  { POWER_SHIELD, "SHIELD", "protect 3x3 area", false, true,  1, "RAISE"       },
  { POWER_DOUBLE, "DOUBLE", "2 shots at once",   true,  false, 2, "FIRE x2"     },
};

int specIndex(uint8_t id) {
  for (int i = 0; i < 7; i++) if (POWER_SPECS[i].id == id) return i;
  return -1;
}

// n cells forming one straight, consecutive run in a row, column, or diagonal (same rule as Central).
bool cellsFormLine(const uint8_t xs[], const uint8_t ys[], int n) {
  if (n <= 1) return true;
  if (n > 8) return false;
  int px[8], py[8];
  for (int i = 0; i < n; i++) {
    px[i] = xs[i];
    py[i] = ys[i];
  }
  for (int i = 0; i < n - 1; i++) {
    for (int j = i + 1; j < n; j++) {
      if (px[j] < px[i] || (px[j] == px[i] && py[j] < py[i])) {
        int tx = px[i]; px[i] = px[j]; px[j] = tx;
        int ty = py[i]; py[i] = py[j]; py[j] = ty;
      }
    }
  }
  int dx = px[1] - px[0];
  int dy = py[1] - py[0];
  bool validDir = (dx == 1 && dy == 0) ||
                  (dx == 0 && dy == 1) ||
                  (dx == 1 && dy == 1) ||
                  (dx == 1 && dy == -1);
  if (!validDir) return false;
  for (int i = 2; i < n; i++) {
    if (px[i] - px[i - 1] != dx || py[i] - py[i - 1] != dy) return false;
  }
  return true;
}

// Returns the cell count (3, 4, or 5) and fills outX/outY with coordinates.
inline int getShipCells(const ShipPlacement &sp, int outX[5], int outY[5]) {
  uint8_t len = sp.ship_len;
  uint8_t sx = sp.start_x;
  uint8_t sy = sp.start_y;
  uint8_t orient = sp.orientation;

  if (orient <= ORIENT_DIAG_UP) {
    int dx = 0, dy = 0;
    switch (orient) {
      case ORIENT_HORIZONTAL: dx = 1; dy = 0; break;
      case ORIENT_VERTICAL:   dx = 0; dy = 1; break;
      case ORIENT_DIAG_DOWN:  dx = 1; dy = 1; break;
      case ORIENT_DIAG_UP:    dx = 1; dy = -1; break;
    }
    for (int i = 0; i < len; i++) {
      outX[i] = sx + dx * i;
      outY[i] = sy + dy * i;
    }
    return len;
  }

  // T-shapes (4 cells)
  if (len == 4) {
    if (orient == SHAPE_T_DOWN) {
      outX[0] = sx;     outY[0] = sy;
      outX[1] = sx + 1; outY[1] = sy;
      outX[2] = sx + 2; outY[2] = sy;
      outX[3] = sx + 1; outY[3] = sy + 1;
      return 4;
    }
    if (orient == SHAPE_T_UP) {
      outX[0] = sx;     outY[0] = sy + 1;
      outX[1] = sx + 1; outY[1] = sy + 1;
      outX[2] = sx + 2; outY[2] = sy + 1;
      outX[3] = sx + 1; outY[3] = sy;
      return 4;
    }
    if (orient == SHAPE_T_RIGHT) {
      outX[0] = sx;     outY[0] = sy;
      outX[1] = sx;     outY[1] = sy + 1;
      outX[2] = sx;     outY[2] = sy + 2;
      outX[3] = sx + 1; outY[3] = sy + 1;
      return 4;
    }
    if (orient == SHAPE_T_LEFT) {
      outX[0] = sx + 1; outY[0] = sy;
      outX[1] = sx + 1; outY[1] = sy + 1;
      outX[2] = sx + 1; outY[2] = sy + 2;
      outX[3] = sx;     outY[3] = sy + 1;
      return 4;
    }

    // L-shapes (4 cells)
    if (orient == SHAPE_L_0) {
      outX[0] = sx;     outY[0] = sy;
      outX[1] = sx;     outY[1] = sy + 1;
      outX[2] = sx;     outY[2] = sy + 2;
      outX[3] = sx + 1; outY[3] = sy + 2;
      return 4;
    }
    if (orient == SHAPE_L_1) {
      outX[0] = sx;     outY[0] = sy;
      outX[1] = sx + 1; outY[1] = sy;
      outX[2] = sx + 2; outY[2] = sy;
      outX[3] = sx;     outY[3] = sy + 1;
      return 4;
    }
    if (orient == SHAPE_L_2) {
      outX[0] = sx + 1; outY[0] = sy;
      outX[1] = sx + 1; outY[1] = sy + 1;
      outX[2] = sx + 1; outY[2] = sy + 2;
      outX[3] = sx;     outY[3] = sy;
      return 4;
    }
    if (orient == SHAPE_L_3) {
      outX[0] = sx;     outY[0] = sy + 1;
      outX[1] = sx + 1; outY[1] = sy + 1;
      outX[2] = sx + 2; outY[2] = sy + 1;
      outX[3] = sx + 2; outY[3] = sy;
      return 4;
    }
    if (orient == SHAPE_J_0) {
      outX[0] = sx + 1; outY[0] = sy;
      outX[1] = sx + 1; outY[1] = sy + 1;
      outX[2] = sx + 1; outY[2] = sy + 2;
      outX[3] = sx;     outY[3] = sy + 2;
      return 4;
    }
    if (orient == SHAPE_J_1) {
      outX[0] = sx;     outY[0] = sy;
      outX[1] = sx + 1; outY[1] = sy;
      outX[2] = sx + 2; outY[2] = sy;
      outX[3] = sx + 2; outY[3] = sy + 1;
      return 4;
    }
    if (orient == SHAPE_J_2) {
      outX[0] = sx;     outY[0] = sy;
      outX[1] = sx;     outY[1] = sy + 1;
      outX[2] = sx;     outY[2] = sy + 2;
      outX[3] = sx + 1; outY[3] = sy;
      return 4;
    }
    if (orient == SHAPE_J_3) {
      outX[0] = sx;     outY[0] = sy + 1;
      outX[1] = sx + 1; outY[1] = sy + 1;
      outX[2] = sx + 2; outY[2] = sy + 1;
      outX[3] = sx;     outY[3] = sy;
      return 4;
    }
  }

  // 3-cell corner L
  if (len == 3) {
    if (orient == SHAPE_CORNER_RD) {
      outX[0] = sx;     outY[0] = sy;
      outX[1] = sx + 1; outY[1] = sy;
      outX[2] = sx;     outY[2] = sy + 1;
      return 3;
    }
    if (orient == SHAPE_CORNER_LD) {
      outX[0] = sx;     outY[0] = sy;
      outX[1] = sx + 1; outY[1] = sy;
      outX[2] = sx + 1; outY[2] = sy + 1;
      return 3;
    }
    if (orient == SHAPE_CORNER_RU) {
      outX[0] = sx;     outY[0] = sy;
      outX[1] = sx;     outY[1] = sy + 1;
      outX[2] = sx + 1; outY[2] = sy + 1;
      return 3;
    }
    if (orient == SHAPE_CORNER_LU) {
      outX[0] = sx + 1; outY[0] = sy;
      outX[1] = sx;     outY[1] = sy + 1;
      outX[2] = sx + 1; outY[2] = sy + 1;
      return 3;
    }
  }

  return 0; // invalid orientation
}

// Marks the cells of myShips[] on ownShip[][] (used to show my board and to keep mines off my ships).
void computeOwnShips() {
  for (int r = 0; r < GRID_SIZE; r++)
    for (int c = 0; c < GRID_SIZE; c++) {
      ownShip[r][c] = false;
      ownDamage[r][c] = false;
    }
  for (int s = 0; s < NUM_SHIPS; s++) {
    int xs[5], ys[5];
    int n = getShipCells(myShips[s], xs, ys);
    for (int i = 0; i < n; i++) {
      int x = xs[i], y = ys[i];
      if (x >= 0 && x < GRID_SIZE && y >= 0 && y < GRID_SIZE) ownShip[y][x] = true;
    }
  }
}

String serialBuffer = "";

// Small queue so the ESP-NOW callback stays short - it just copies
// bytes in here, and loop() does the actual work.
typedef struct {
  uint8_t data[MAX_PACKET_SIZE];
  int len;
} PendingPacket;

PendingPacket pendingQueue[PENDING_QUEUE_SIZE];
volatile int pendingHead = 0;
volatile int pendingTail = 0;

// =====================================================================
// FUNCTION DECLARATIONS
// =====================================================================

#if ESP_IDF_VERSION_MAJOR >= 5
void OnDataRecv(const esp_now_recv_info_t *recv_info, const uint8_t *incomingData, int len);
#else
void OnDataRecv(const uint8_t *senderMac, const uint8_t *incomingData, int len);
#endif
#if ESP_IDF_VERSION >= ESP_IDF_VERSION_VAL(5, 5, 0)
void OnDataSent(const wifi_tx_info_t *tx_info, esp_now_send_status_t status);
#else
void OnDataSent(const uint8_t *mac_addr, esp_now_send_status_t status);
#endif
void processPacket(const uint8_t *data, int len);

bool localValidateShips(ShipPlacement ships[NUM_SHIPS]);
void sendRegistration();
void sendAttack(uint8_t targetTeam, uint8_t x, uint8_t y);
bool attemptManualAttack(uint8_t targetTeam, uint8_t x, uint8_t y);

void handleRegAck(RegAckPacket *pkt);
void handleFeedback(FeedbackPacket *pkt);
void handleTurnUpdate(TurnUpdatePacket *pkt);

void handleSerialInput();
void processSerialCommand(String line);

void uiInit();
void uiShowMessage(const char *msg);
void uiShowYourTurn();
void uiShowWaiting(uint8_t currentTeam);
void uiShowAttackResult(uint8_t targetTeam, uint8_t x, uint8_t y, uint8_t result);
void uiShowPowerResult(uint8_t power, uint8_t status, uint8_t target, uint8_t count, const uint8_t res[3]);
void uiTick();
void uiRedraw();
void uiPollTouch();
void uiPollLoadoutTouch();
void uiDrawLoadout();
void uiDrawTeamSelect();
void uiPollTeamSelectTouch();
void selectTeam(uint8_t id);
void uiDrawShipWizard();
void uiPollShipWizardTouch();
void wizUseDefaultShips();
const char *wizGetOrientName(uint8_t shipIdx, uint8_t orient);
void uiDrawYourTurn();
void uiDrawActionScreen();
void uiDrawPowerMenu();
void uiDrawWaiting(uint8_t currentTeam);
void uiDrawMessage(const char *msg);
void uiDrawResultBanner(uint8_t targetTeam, uint8_t x, uint8_t y, uint8_t result);
void uiDrawPowerBanner(uint8_t power, uint8_t status, uint8_t target, uint8_t count, const uint8_t res[3]);
void drawTabButton(int tabIdx, bool selected);
void drawAllTabs();
void drawTargetButton(int i, bool selected);
void drawGrid();
void drawAltButton(const char *label);
void drawAttackButton(bool enabled, const char *label);
bool uiActionReady();
const char *uiActionLabel();
void computeOpponentIds();

void lockLoadout(uint8_t loadout);
bool attemptPower(uint8_t power, uint8_t target, const uint8_t xs[3], const uint8_t ys[3]);
void handlePowerResult(PowerResultPacket *pkt);

// =====================================================================
// HARDWARE PERIPHERALS: STATUS LEDS & AUDIO BUZZER HELPERS
// =====================================================================

void ledBuzzerInit() {
  if (LED_GREEN_PIN >= 0) {
    pinMode(LED_GREEN_PIN, OUTPUT);
    digitalWrite(LED_GREEN_PIN, LOW);
  }
  if (LED_RED_PIN >= 0) {
    pinMode(LED_RED_PIN, OUTPUT);
    digitalWrite(LED_RED_PIN, LOW);
  }
  if (BUZZER_PIN >= 0) {
    pinMode(BUZZER_PIN, OUTPUT);
    digitalWrite(BUZZER_PIN, LOW);
  }

  // Quick startup self-test flash
  if (LED_RED_PIN >= 0) {
    digitalWrite(LED_RED_PIN, HIGH);
    delay(100);
    digitalWrite(LED_RED_PIN, LOW);
  }
}

void setTurnLed(bool active) {
  if (LED_GREEN_PIN >= 0) {
    digitalWrite(LED_GREEN_PIN, active ? HIGH : LOW);
  }
}

void playTurnAlert() {
  if (BUZZER_PIN >= 0) {
    tone(BUZZER_PIN, 1500, 150);
  }
}

void flashHitFeedback() {
  if (BUZZER_PIN >= 0) {
    tone(BUZZER_PIN, 2400, 200);
  }
  if (LED_RED_PIN >= 0) {
    for (int i = 0; i < 3; i++) {
      digitalWrite(LED_RED_PIN, HIGH);
      delay(60);
      digitalWrite(LED_RED_PIN, LOW);
      delay(60);
    }
  }
}

// =====================================================================
// SETUP / LOOP
// =====================================================================

void setup() {
  WRITE_PERI_REG(RTC_CNTL_BROWN_OUT_REG, 0); // Disable brownout detector
  Serial.begin(115200);
  delay(200);

  Serial.print("=== MERAZ BATTLESHIP - ");
  Serial.print(MY_TEAM_NAME);
  Serial.println(" ===");

  // Initialize Status LEDs & Audio Buzzer
  ledBuzzerInit();
  randomSeed(esp_random());

  // Catch obviously-wrong ship configs locally before even trying to
  // register - saves a round trip and gives a clearer error message.
  if (!localValidateShips(myShips)) {
    Serial.println("WARNING: your ship configuration looks INVALID");
    Serial.println("(overlap, out of bounds, or wrong sizes). Fix myShips[] above.");
  } else {
    Serial.println("Local ship configuration check: OK");
  }

  WiFi.mode(WIFI_STA);
#if ESP_IDF_VERSION_MAJOR >= 5
  int timeout = 0;
  while (!WiFi.STA.started() && timeout < 20) {
    delay(50);
    timeout++;
  }
#else
  WiFi.disconnect();
#endif
  esp_wifi_set_channel(ESPNOW_CHANNEL, WIFI_SECOND_CHAN_NONE);

  String macStr = WiFi.macAddress();
  if (macStr == "00:00:00:00:00:00") {
    uint8_t mac[6];
    if (esp_wifi_get_mac(WIFI_IF_STA, mac) == ESP_OK) {
      char buf[20];
      snprintf(buf, sizeof(buf), "%02X:%02X:%02X:%02X:%02X:%02X",
               mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
      macStr = String(buf);
    }
  }
  Serial.print("My MAC address: ");
  Serial.println(macStr);
  Serial.println("(Central needs this MAC in its participantMacs[] config)");

  if (esp_now_init() != ESP_OK) {
    Serial.println("FATAL: esp_now_init() failed. Check the board and reset.");
    while (true) { delay(1000); }
  }

  esp_now_register_recv_cb(OnDataRecv);
  esp_now_register_send_cb(OnDataSent);

  esp_now_peer_info_t peerInfo = {};
  memcpy(peerInfo.peer_addr, centralMac, 6);
  peerInfo.channel = 0;  // 0 = use current device channel (ESPNOW_CHANNEL)
  peerInfo.encrypt = false;
  if (esp_now_add_peer(&peerInfo) != ESP_OK) {
    Serial.println("WARNING: could not add Central as an ESP-NOW peer.");
  }

  uiInit();
  computeOwnShips();

  // Show Team Selection screen first so participants pick their assigned Team ID (1-4)
  uiDrawTeamSelect();
  Serial.println("[TEAM] >>> SELECT YOUR TEAM ID (1-4) ON SCREEN OR TYPE: TEAM 1..4 <<<");
  lastLoadoutPromptMillis = millis();
}

void loop() {
  // Drain packets that arrived via ESP-NOW since the last loop().
  while (pendingHead != pendingTail) {
    PendingPacket p = pendingQueue[pendingHead];
    pendingHead = (pendingHead + 1) % PENDING_QUEUE_SIZE;
    processPacket(p.data, p.len);
  }

  // Keep re-sending registration (non-blocking) until Central confirms it.
  // Handles the case where our very first registration packet gets lost.
  if (teamSelected && shipsLocked && loadoutLocked && !demoNoRegister && !registrationConfirmed && millis() - lastRegAttemptMillis > REG_RETRY_INTERVAL_MS) {
    sendRegistration();
    lastRegAttemptMillis = millis();
  }

  // Remind (on Serial) while team, wizard or loadout is still open
  if (!teamSelected && millis() - lastLoadoutPromptMillis > 4000) {
    Serial.println("[TEAM] waiting for team selection (tap screen or type: TEAM 1..4)");
    lastLoadoutPromptMillis = millis();
  } else if (!shipsLocked && millis() - lastLoadoutPromptMillis > 4000) {
    Serial.println("[WIZARD] waiting to place ships on screen");
    lastLoadoutPromptMillis = millis();
  } else if (!loadoutLocked && millis() - lastLoadoutPromptMillis > 4000) {
    Serial.println("[LOADOUT] waiting for the team to choose powers on screen");
    lastLoadoutPromptMillis = millis();
  }

  // Auto-transition from "Registered!" to "Waiting for start..." after 1.5s in setup
  if (registrationConfirmed && roundState == STATE_SETUP && uiLastScreen == SCR_MESSAGE && strcmp(uiLastMessage, "Registered!") == 0) {
    if (millis() - lastRegAcceptedMillis >= 1500) {
      uiShowMessage("Waiting for start...");
    }
  }

  handleSerialInput();
  uiTick();
}

// =====================================================================
// ESP-NOW CALLBACKS (keep these SHORT - just copy and queue)
// =====================================================================

#if ESP_IDF_VERSION_MAJOR >= 5
void OnDataRecv(const esp_now_recv_info_t *recv_info, const uint8_t *incomingData, int len) {
  const uint8_t *senderMac = recv_info->src_addr;
#else
void OnDataRecv(const uint8_t *senderMac, const uint8_t *incomingData, int len) {
#endif
  // Only trust packets that actually came from the Central.
  if (memcmp(senderMac, centralMac, 6) != 0) return;
  if (len <= 0 || len > MAX_PACKET_SIZE) return;

  int next = (pendingTail + 1) % PENDING_QUEUE_SIZE;
  if (next == pendingHead) return;  // queue full (shouldn't happen) - drop

  memcpy(pendingQueue[pendingTail].data, incomingData, len);
  pendingQueue[pendingTail].len = len;
  pendingTail = next;
}

#if ESP_IDF_VERSION >= ESP_IDF_VERSION_VAL(5, 5, 0)
void OnDataSent(const wifi_tx_info_t *tx_info, esp_now_send_status_t status) {
#else
void OnDataSent(const uint8_t *mac_addr, esp_now_send_status_t status) {
#endif
  if (status != ESP_NOW_SEND_SUCCESS) {
    Serial.println("WARNING: an ESP-NOW send to Central failed.");
  }
}

// =====================================================================
// PACKET ROUTING - Central only ever sends us RegAck (1 byte),
// TurnUpdate (2 bytes), Feedback (4 bytes) or PowerResult (7 bytes), and
// those four sizes are all different, so length alone tells us what we received.
// (See the matching note in central_node.ino for the full reasoning.)
// =====================================================================

void processPacket(const uint8_t *data, int len) {
  if (len == sizeof(RegAckPacket)) {
    RegAckPacket pkt;
    memcpy(&pkt, data, sizeof(pkt));
    handleRegAck(&pkt);
  } else if (len == sizeof(FeedbackPacket)) {
    FeedbackPacket pkt;
    memcpy(&pkt, data, sizeof(pkt));
    handleFeedback(&pkt);
  } else if (len == sizeof(TurnUpdatePacket)) {
    TurnUpdatePacket pkt;
    memcpy(&pkt, data, sizeof(pkt));
    handleTurnUpdate(&pkt);
  } else if (len == sizeof(PowerResultPacket)) {
    PowerResultPacket pkt;
    memcpy(&pkt, data, sizeof(pkt));
    handlePowerResult(&pkt);
  } else {
    Serial.print("WARNING: ignored a packet of unexpected size (");
    Serial.print(len);
    Serial.println(" bytes)");
  }
}

// =====================================================================
// LOCAL SHIP VALIDATION (a quick sanity check, mirrors the Central's
// real check so config mistakes show up immediately on this board
// instead of only after Central rejects the registration)
// =====================================================================

bool localValidateShips(ShipPlacement ships[NUM_SHIPS]) {
  uint8_t tempGrid[GRID_SIZE][GRID_SIZE];
  for (int r = 0; r < GRID_SIZE; r++)
    for (int c = 0; c < GRID_SIZE; c++)
      tempGrid[r][c] = 0;

  bool sawLen5 = false;
  int sawLen4 = 0;
  bool sawLen3 = false;
  int totalCells = 0;

  for (int s = 0; s < NUM_SHIPS; s++) {
    uint8_t len = ships[s].ship_len;
    uint8_t orient = ships[s].orientation;

    if (orient > ORIENT_DIAG_UP) return false; // all 4 ships must be straight (H, V, or Diag)

    if (len == 5) {
      if (sawLen5) return false;
      sawLen5 = true;
    } else if (len == 4) {
      sawLen4++;
    } else if (len == 3) {
      if (sawLen3) return false;
      sawLen3 = true;
    } else {
      return false;
    }

    int xs[5], ys[5];
    int n = getShipCells(ships[s], xs, ys);
    if (n != len) return false;

    for (int i = 0; i < n; i++) {
      int x = xs[i], y = ys[i];
      if (x < 0 || x >= GRID_SIZE || y < 0 || y >= GRID_SIZE) return false;
      if (tempGrid[y][x] != 0) return false;
      tempGrid[y][x] = 1;
      totalCells++;
    }
  }

  return sawLen5 && (sawLen4 == 2) && sawLen3 && (totalCells == TOTAL_SHIP_CELLS);
}

// =====================================================================
// REGISTRATION
// =====================================================================

void sendRegistration() {
  RegistrationPacket pkt;
  pkt.team_id = MY_TEAM_ID;
  strncpy(pkt.team_name, MY_TEAM_NAME, 20);
  memcpy(pkt.ships, myShips, sizeof(myShips));
  pkt.loadout = myLoadout;

  esp_now_send(centralMac, (uint8_t *)&pkt, sizeof(pkt));
  Serial.print("Sent registration to Central (loadout 0x"); Serial.print(myLoadout, HEX); Serial.println(")...");
}

void handleRegAck(RegAckPacket *pkt) {
  switch (pkt->status) {
    case REG_OK:
      registrationConfirmed = true;
      shipsLocked = true;
      loadoutLocked = true;
      wizReviewMode = false;
      lastRegAcceptedMillis = millis();
      Serial.println("Registration ACCEPTED by Central.");
      uiShowMessage("Waiting for start...");
      break;
    case REG_RECONNECTED:
      registrationConfirmed = true;
      shipsLocked = true;
      loadoutLocked = true;
      wizReviewMode = false;
      Serial.println("Reconnected - Central restored our existing game state.");
      uiShowMessage("Reconnected!");
      break;
    case REG_REJECTED:
      registrationConfirmed = false;
      Serial.println("Registration REJECTED. Central already has a match in progress or MAC locked. Click RESET on Central for a new game.");
      uiShowMessage("REJECTED: Reset Central");
      break;
    case REG_RECONNECT_REJECTED:
      registrationConfirmed = false;
      Serial.println("Reconnect REJECTED - fleet or loadout does not match what Central has locked. Click RESET on Central for a new game.");
      uiShowMessage("REJECTED: Config mismatch");
      break;
    default:
      Serial.println("Got an unknown registration status from Central.");
      break;
  }
}

// =====================================================================
// ATTACKING
// =====================================================================

void sendAttack(uint8_t targetTeam, uint8_t x, uint8_t y) {
  AttackPacket pkt;
  pkt.attacker_id = MY_TEAM_ID;
  pkt.target_id = targetTeam;
  pkt.x = x;
  pkt.y = y;
  esp_now_send(centralMac, (uint8_t *)&pkt, sizeof(pkt));
}

static bool pendingAttackActive = false;
static uint8_t pendingAttackTarget = 0, pendingAttackX = 0, pendingAttackY = 0;

// Client-side sanity checks only, to avoid pointless sends and give
// fast feedback. Central re-checks everything anyway - it never
// trusts this board's judgement about what's a legal attack.
bool attemptManualAttack(uint8_t targetTeam, uint8_t x, uint8_t y) {
  if (millis() < uiSendLockUntil) {
    return false;
  }
  if (!registrationConfirmed) {
    Serial.println("Cannot attack - not registered with Central yet.");
    return false;
  }
  if (roundState != STATE_RUNNING) {
    Serial.println("Cannot attack - the game is not running.");
    return false;
  }
  if (currentTurnTeam != MY_TEAM_ID) {
    Serial.println("Cannot attack - it is not your turn.");
    return false;
  }
  if (targetTeam < 1 || targetTeam > MAX_TEAMS || targetTeam == MY_TEAM_ID) {
    Serial.println("Invalid target team.");
    return false;
  }
  if (x >= GRID_SIZE || y >= GRID_SIZE) {
    Serial.println("Invalid coordinates - must be 0-6.");
    return false;
  }

  uiSendLockUntil = millis() + 2000;
  pendingAttackActive = true;
  pendingAttackTarget = targetTeam;
  pendingAttackX = x;
  pendingAttackY = y;

  sendAttack(targetTeam, x, y);
  Serial.print("Attack sent -> TEAM "); Serial.print(targetTeam);
  Serial.print(" ("); Serial.print(x); Serial.print(","); Serial.print(y); Serial.println(")");
  return true;
}

void handleFeedback(FeedbackPacket *pkt) {
  // 1. If target_id == MY_TEAM_ID, this is an incoming attack ON MY BOARD, or our own repair
  if (pkt->target_id == MY_TEAM_ID) {
    if (pkt->x < GRID_SIZE && pkt->y < GRID_SIZE) {
      if (pkt->result_code == RESULT_HIT || pkt->result_code == RESULT_SHIP_SUNK || pkt->result_code == RESULT_SUNK) {
        ownDamage[pkt->y][pkt->x] = true;
        Serial.printf("[DEFENSE] Hit taken on own ship at (%d,%d)!\n", pkt->x, pkt->y);
        flashHitFeedback();
        if ((uiLastScreen == SCR_ACTION || uiLastScreen == SCR_WAITING) && uiViewingOwnFleet) {
          drawGrid();
        }
      } else if (pkt->result_code == RESULT_REPAIRED) {
        ownDamage[pkt->y][pkt->x] = false;
        Serial.printf("[DEFENSE] Repaired (%d,%d) - ship cell is intact!\n", pkt->x, pkt->y);
        if ((uiLastScreen == SCR_ACTION || uiLastScreen == SCR_WAITING) && uiViewingOwnFleet) {
          drawGrid();
        }
      } else if (pkt->result_code == RESULT_MINE) {
        myMineX = -1;
        myMineY = -1;
        ownMisses[pkt->y][pkt->x] = true;
        Serial.printf("[DEFENSE] Our mine at (%d,%d) was triggered!\n", pkt->x, pkt->y);
        flashHitFeedback();
        if ((uiLastScreen == SCR_ACTION || uiLastScreen == SCR_WAITING) && uiViewingOwnFleet) {
          drawGrid();
        }
      } else if (pkt->result_code == RESULT_MISS) {
        ownMisses[pkt->y][pkt->x] = true;
        if ((uiLastScreen == SCR_ACTION || uiLastScreen == SCR_WAITING) && uiViewingOwnFleet) {
          drawGrid();
        }
      } else if (pkt->result_code == RESULT_BLOCKED) {
        ownBlocked[pkt->y][pkt->x] = true;
        if ((uiLastScreen == SCR_ACTION || uiLastScreen == SCR_WAITING) && uiViewingOwnFleet) {
          drawGrid();
        }
      }
    }
    return;
  }

  // 2. If target_id != MY_TEAM_ID, this is an opponent!
  if (pkt->result_code == RESULT_REPAIRED) {
    if (pkt->target_id >= 1 && pkt->target_id <= MAX_TEAMS && pkt->x < GRID_SIZE && pkt->y < GRID_SIZE) {
      oppGrid[pkt->target_id][pkt->y][pkt->x] = TRACK_UNKNOWN;
      Serial.printf("RESULT: TEAM %d (%d,%d) -> REPAIRED\n", pkt->target_id, pkt->x, pkt->y);
      Serial.printf("[RADAR] TEAM %d repaired (%d,%d) - cleared from radar!\n", pkt->target_id, pkt->x, pkt->y);
      if ((uiLastScreen == SCR_ACTION || uiLastScreen == SCR_WAITING) && !uiViewingOwnFleet && uiSelTarget == pkt->target_id) {
        drawGrid();
        if (uiLastScreen == SCR_ACTION) drawAttackButton(uiActionReady(), uiActionLabel());
      }
    }
    return;
  }

  const char *resName = "UNKNOWN";
  switch (pkt->result_code) {
    case RESULT_MISS: resName = "MISS"; break;
    case RESULT_HIT: resName = "HIT"; break;
    case RESULT_SHIP_SUNK: resName = "HIT - ENEMY SHIP SUNK!"; break;
    case RESULT_SUNK: resName = "HIT - TEAM ELIMINATED!"; break;
    case RESULT_MINE: resName = "MINE - it was a mine! You lose your next turn."; break;
    case RESULT_BLOCKED: resName = "BLOCKED by a shield (turn used, nothing marked)."; break;
    case RESULT_UNKNOWN: resName = "UNKNOWN - the target is behind a smoke screen"; break;
    case RESULT_INVALID: resName = "INVALID (rejected by Central)"; break;
    default: resName = "OTHER"; break;
  }
  Serial.printf("RESULT: TEAM %d (%d,%d) -> %s\n", pkt->target_id, pkt->x, pkt->y, resName);

  if (pkt->target_id >= 1 && pkt->target_id <= MAX_TEAMS && pkt->x < GRID_SIZE && pkt->y < GRID_SIZE) {
    if (pkt->result_code == RESULT_HIT || pkt->result_code == RESULT_SHIP_SUNK || pkt->result_code == RESULT_SUNK) {
      oppGrid[pkt->target_id][pkt->y][pkt->x] = TRACK_HIT;
    } else if (pkt->result_code == RESULT_MISS || pkt->result_code == RESULT_MINE) {
      oppGrid[pkt->target_id][pkt->y][pkt->x] = TRACK_MISS;
    } else if (pkt->result_code == RESULT_BLOCKED) {
      oppGrid[pkt->target_id][pkt->y][pkt->x] = TRACK_BLOCKED;
    }
  }

  // Live refresh of opponent radar if this board is currently inspecting it
  if ((uiLastScreen == SCR_ACTION || uiLastScreen == SCR_WAITING) && !uiViewingOwnFleet && uiSelTarget == pkt->target_id) {
    drawGrid();
    if (uiLastScreen == SCR_ACTION) drawAttackButton(uiActionReady(), uiActionLabel());
  }

  // ONLY show full-screen attack banner if THIS team fired the attack!
  // Spectator nodes keep radars updated silently without popup interruption.
  bool wasMyAttack = (currentTurnTeam == MY_TEAM_ID) || 
                     (pendingAttackActive && pendingAttackTarget == pkt->target_id && pendingAttackX == pkt->x && pendingAttackY == pkt->y);
  if (wasMyAttack) {
    pendingAttackActive = false;
    Serial.printf("[MY_ATTACK] target=%d x=%d y=%d\n", pkt->target_id, pkt->x, pkt->y);
    switch (pkt->result_code) {
      case RESULT_MISS:
        uiShowAttackResult(pkt->target_id, pkt->x, pkt->y, RESULT_MISS);
        break;
      case RESULT_HIT:
        flashHitFeedback();
        uiShowAttackResult(pkt->target_id, pkt->x, pkt->y, RESULT_HIT);
        break;
      case RESULT_SHIP_SUNK:
        flashHitFeedback();
        uiShowAttackResult(pkt->target_id, pkt->x, pkt->y, RESULT_SHIP_SUNK);
        break;
      case RESULT_SUNK:
        flashHitFeedback();
        uiShowAttackResult(pkt->target_id, pkt->x, pkt->y, RESULT_SUNK);
        break;
      case RESULT_MINE:
        uiShowAttackResult(pkt->target_id, pkt->x, pkt->y, RESULT_MINE);
        break;
      case RESULT_BLOCKED:
        uiShowAttackResult(pkt->target_id, pkt->x, pkt->y, RESULT_BLOCKED);
        break;
      case RESULT_UNKNOWN:
        uiShowAttackResult(pkt->target_id, pkt->x, pkt->y, RESULT_UNKNOWN);
        break;
      case RESULT_INVALID:
        uiShowAttackResult(pkt->target_id, pkt->x, pkt->y, RESULT_INVALID);
        break;
      default:
        return;
    }
  }
  if (pkt->result_code != RESULT_INVALID && shieldCooldownTurns > 0) shieldCooldownTurns--;   // shot counts toward cooldown
}

// =====================================================================
// LOADOUT AND POWER-UPS
// =====================================================================
// The loadout is picked on the touch screen BEFORE anything is sent to
// Central (see uiDrawLoadout()). Locking it in starts the registration, with
// the loadout byte as the last byte of the 34-byte registration packet.
// Every power-up uses the turn. Central checks everything again, and it is the
// only one that counts uses - the copies here only decide what the screens show.

void lockLoadout(uint8_t loadout) {
  myLoadout = loadout;
  loadoutLocked = true;
  for (int p = 1; p <= 7; p++) usesLeft[p] = (p != POWER_SHIELD && (loadout & LOADOUT_BIT(p))) ? POWER_USES : 0;
  Serial.print("Loadout locked: "); printLoadout(loadout);
  Serial.print("   (byte 0x"); Serial.print(loadout, HEX); Serial.println(")");
  uiShowMessage("Connecting...");
  if (!demoNoRegister) {
    sendRegistration();
    lastRegAttemptMillis = millis();
  }
}

// "SONAR", "sonar" or "1" -> 1, ... ; 0 if not a power.
uint8_t parsePowerToken(const char *tok) {
  if (tok[0] >= '1' && tok[0] <= '7' && tok[1] == '\0') return (uint8_t)(tok[0] - '0');
  static const char *names[7] = { "SONAR", "SALVO", "MINE", "REPAIR", "SMOKE", "SHIELD", "DOUBLE" };
  for (int i = 0; i < 7; i++) {
    const char *a = names[i], *b = tok;
    while (*a && *b && (*b | 0x20) == (*a | 0x20)) { a++; b++; }
    if (!*a && !*b) return (uint8_t)(i + 1);
  }
  return 0;
}

static bool sendPower(uint8_t power, uint8_t target, const uint8_t xs[3], const uint8_t ys[3]) {
  PowerUpPacket pkt;
  pkt.power = power;
  pkt.team_id = MY_TEAM_ID;
  pkt.target_id = target;
  pkt.x1 = xs[0]; pkt.y1 = ys[0];
  pkt.x2 = xs[1]; pkt.y2 = ys[1];
  pkt.x3 = xs[2]; pkt.y3 = ys[2];
  return esp_now_send(centralMac, (uint8_t *)&pkt, sizeof(pkt)) == ESP_OK;
}

// Local sanity checks, then send. Returns true if the request was sent.
bool attemptPower(uint8_t power, uint8_t target, const uint8_t xs[3], const uint8_t ys[3]) {
  if (millis() < uiSendLockUntil) return false;
  const int si = specIndex(power);
  if (si < 0) { Serial.println("Unknown power."); return false; }
  const PowerSpec &sp = POWER_SPECS[si];

  if (!registrationConfirmed) { Serial.println("Cannot use a power - not registered with Central yet."); return false; }
  if (roundState != STATE_RUNNING) { Serial.println("Cannot use a power - the game is not running."); return false; }
  if (currentTurnTeam != MY_TEAM_ID) { Serial.println("Cannot use a power - it is not your turn."); return false; }
  if (power != POWER_SHIELD && !(myLoadout & LOADOUT_BIT(power))) {
    Serial.print(sp.name); Serial.println(" is not in your loadout."); return false;
  }
  if (power != POWER_SHIELD && usesLeft[power] == 0) {
    Serial.print("No uses of "); Serial.print(sp.name); Serial.println(" left."); return false;
  }
  if (sp.needTarget && (target < 1 || target > MAX_TEAMS || target == MY_TEAM_ID)) {
    Serial.println("Invalid target team."); return false;
  }
  for (int i = 0; i < sp.cells; i++) {
    if (xs[i] >= GRID_SIZE || ys[i] >= GRID_SIZE) { Serial.println("Invalid coordinates - must be 0-6."); return false; }
  }
  if (power == POWER_SALVO && !cellsFormLine(xs, ys, 3)) {
    Serial.println("A Salvo needs 3 consecutive cells in one row or one column."); return false;
  }
  if (power == POWER_DOUBLE && xs[0] == xs[1] && ys[0] == ys[1]) {
    Serial.println("A Double Attack needs 2 different cells."); return false;
  }
  if (power == POWER_MINE && ownShip[ys[0]][xs[0]]) {
    Serial.println("A mine can't go on one of your ships."); return false;
  }
  if (power == POWER_SHIELD && (xs[0] < 1 || xs[0] > GRID_SIZE - 2 || ys[0] < 1 || ys[0] > GRID_SIZE - 2)) {
    Serial.println("Shield centre must be 1-5 in both x and y."); return false;
  }

  uint8_t sx[3] = {0, 0, 0}, sy[3] = {0, 0, 0};
  for (int i = 0; i < sp.cells; i++) { sx[i] = xs[i]; sy[i] = ys[i]; }
  if (!sendPower(power, sp.needTarget ? target : 0, sx, sy)) { Serial.println("Could not send the power-up."); return false; }

  pendingPower = power; pendingTarget = target; pendingCells = sp.cells;
  for (int i = 0; i < 3; i++) { pendingX[i] = sx[i]; pendingY[i] = sy[i]; }
  uiSendLockUntil = millis() + 2000;

  Serial.print("Power sent -> "); Serial.print(sp.name);
  if (sp.needTarget) { Serial.print(" TEAM "); Serial.print(target); }
  for (int i = 0; i < sp.cells; i++) { Serial.print(" ("); Serial.print(sx[i]); Serial.print(","); Serial.print(sy[i]); Serial.print(")"); }
  Serial.println();
  return true;
}

void handlePowerResult(PowerResultPacket *pkt) {
  // One easy-to-parse line with shot coordinates (the screen mirror reads it), then the screen.
  Serial.printf("POWER_RESULT: power=%d status=%d target=%d count=%d r=%d,%d,%d pts=(%d,%d),(%d,%d),(%d,%d)\n",
                pkt->power, pkt->status, pkt->target_id, pkt->count,
                pkt->result[0], pkt->result[1], pkt->result[2],
                pendingX[0], pendingY[0], pendingX[1], pendingY[1], pendingX[2], pendingY[2]);

  const int si = specIndex(pkt->power);
  if (pkt->status == PWR_STATUS_REJECTED) {
    Serial.print("REJECTED by Central: "); Serial.println(si >= 0 ? POWER_SPECS[si].name : "power");
    Serial.println("(your turn and your uses are NOT spent - fix the move and try again)");
  } else {
    if (pkt->power != POWER_SHIELD && pkt->power >= 1 && pkt->power <= 7 && usesLeft[pkt->power] > 0) usesLeft[pkt->power]--;
    if (pkt->power == POWER_SHIELD) {
      shieldCooldownTurns = 2;
    } else {
      if (shieldCooldownTurns > 0) shieldCooldownTurns--;
    }
    if (pkt->power == POWER_MINE) { myMineX = (int8_t)pendingX[0]; myMineY = (int8_t)pendingY[0]; }
    if (pkt->power == POWER_REPAIR && pkt->status != PWR_STATUS_REJECTED) {
      int rx = pendingX[0];
      int ry = pendingY[0];
      if (rx >= 0 && rx < GRID_SIZE && ry >= 0 && ry < GRID_SIZE) {
        ownDamage[ry][rx] = false;
        Serial.printf("[REPAIR] Repaired (%d,%d) - ship cell is intact!\n", rx, ry);
      }
    }
    if (pkt->power == POWER_SONAR) {
      if (pkt->status == PWR_STATUS_JAMMED) Serial.println("SONAR JAMMED by a smoke screen (use and turn spent).");
      else { Serial.print("SONAR: "); Serial.print(pkt->count); Serial.println(" ship cell(s) in the area."); }
    }
    if (pkt->power == POWER_SALVO || pkt->power == POWER_DOUBLE) {
      bool anyHit = false;
      int nCells = (pkt->power == POWER_SALVO) ? 3 : 2;
      for (int i = 0; i < nCells; i++) {
        if (pkt->target_id >= 1 && pkt->target_id <= MAX_TEAMS && pendingX[i] < GRID_SIZE && pendingY[i] < GRID_SIZE) {
          uint8_t res = pkt->result[i];
          const char *resName = "MISS";
          if (res == RESULT_HIT || res == RESULT_SHIP_SUNK || res == RESULT_SUNK) {
            oppGrid[pkt->target_id][pendingY[i]][pendingX[i]] = TRACK_HIT;
            resName = (res == RESULT_SUNK) ? "ELIMINATED!" : "HIT";
            anyHit = true;
          } else if (res == RESULT_MISS || res == RESULT_MINE) {
            oppGrid[pkt->target_id][pendingY[i]][pendingX[i]] = TRACK_MISS;
            resName = (res == RESULT_MINE) ? "MINE" : "MISS";
          } else if (res == RESULT_BLOCKED) {
            oppGrid[pkt->target_id][pendingY[i]][pendingX[i]] = TRACK_BLOCKED;
            resName = "BLOCKED";
          }
          Serial.printf("[POWER_SHOT] TEAM %d (%d,%d) -> %s\n", pkt->target_id, pendingX[i], pendingY[i], resName);
        }
      }
      if (anyHit) flashHitFeedback();
      if (uiLastScreen == SCR_ACTION) {
        drawGrid();
        drawAttackButton(uiActionReady(), uiActionLabel());
      }
    }
  }
  uiShowPowerResult(pkt->power, pkt->status, pkt->target_id, pkt->count, pkt->result);
}

// =====================================================================
// TURN TRACKING
// =====================================================================

void handleTurnUpdate(TurnUpdatePacket *pkt) {
  currentTurnTeam = pkt->current_turn;
  roundState = pkt->round_state;
  uint8_t mask = pkt->active_mask;

  if (mask != 0) {
    uint8_t newCount = 0;
    uint8_t newOpponents[3];
    for (uint8_t t = 1; t <= MAX_TEAMS; t++) {
      if (t != myTeamId && (mask & (1 << (t - 1)))) {
        if (newCount < 3) newOpponents[newCount++] = t;
      }
    }
    if (newCount > 0) {
      numOpponents = newCount;
      for (int i = 0; i < numOpponents; i++) opponentIds[i] = newOpponents[i];
    }
  }

  // If only 1 opponent in match, auto-select them immediately
  if (numOpponents == 1) {
    uiSelTarget = opponentIds[0];
  } else {
    bool valid = false;
    for (int i = 0; i < numOpponents; i++) {
      if (opponentIds[i] == uiSelTarget) { valid = true; break; }
    }
    if (!valid) uiSelTarget = 0;
  }

  Serial.printf("[OPPONENTS] %d", numOpponents);
  for (int i = 0; i < numOpponents; i++) Serial.printf(" %d", opponentIds[i]);
  Serial.println();

  // If Central has reset to setup, unregister locally so participant can set up fresh
  if (roundState == STATE_SETUP && registrationConfirmed && !(mask & (1 << (myTeamId - 1)))) {
    Serial.println("[RESET] Match reset by Central organizer. Resetting fleet & powers...");
    registrationConfirmed = false;
    shipsLocked = false;
    loadoutLocked = false;
    wizShipIdx = 0;
    ldAttack = 0;
    ldDefence = 0;
    myMineX = -1; myMineY = -1;
    memset(oppGrid, TRACK_UNKNOWN, sizeof(oppGrid));
    memset(ownDamage, 0, sizeof(ownDamage));
    memset(ownMisses, 0, sizeof(ownMisses));
    memset(ownBlocked, 0, sizeof(ownBlocked));
    setTurnLed(false);
    uiDrawShipWizard();
    return;
  }

  if (!teamSelected || !shipsLocked || !loadoutLocked || !registrationConfirmed) {
    // Participant is still selecting team, placing ships in Wizard, or locking in powers!
    // Do NOT interrupt their setup screens with "Waiting for start...".
    return;
  }

  if (roundState == STATE_RUNNING) {
    // When currentTurnTeam's turn begins, its 1-round smoke screen / shield has expired!
    // Clear any deflected cells (TRACK_BLOCKED) so they turn back to normal and can be targeted again!
    if (currentTurnTeam >= 1 && currentTurnTeam <= MAX_TEAMS) {
      for (int r = 0; r < GRID_SIZE; r++) {
        for (int c = 0; c < GRID_SIZE; c++) {
          if (oppGrid[currentTurnTeam][r][c] == TRACK_BLOCKED) {
            oppGrid[currentTurnTeam][r][c] = TRACK_UNKNOWN;
          }
        }
      }
    }
    if (currentTurnTeam == MY_TEAM_ID) {
      memset(ownBlocked, 0, sizeof(ownBlocked));
      Serial.println(">>> YOUR TURN <<<");
      setTurnLed(true);
      playTurnAlert();
      uiShowYourTurn();
    } else {
      Serial.print("Waiting - it is TEAM "); Serial.print(currentTurnTeam); Serial.println("'s turn.");
      setTurnLed(false);
      uiShowWaiting(currentTurnTeam);
    }
  } else if (roundState == STATE_GAMEOVER) {
    Serial.println("GAME OVER.");
    setTurnLed(false);
    uiShowMessage("GAME OVER");
  } else {
    // SETUP or READY
    Serial.println("Waiting for the organizer to start the game...");
    memset(oppGrid, TRACK_UNKNOWN, sizeof(oppGrid));
    myMineX = -1; myMineY = -1;
    setTurnLed(false);
    uiShowMessage("Waiting for start...");
  }
}

// =====================================================================
// SERIAL COMMANDS (typed test commands; they work alongside the touch UI)
// =====================================================================

void handleSerialInput() {
  while (Serial.available() > 0) {
    char c = Serial.read();
    if (c == '\n' || c == '\r') {
      if (serialBuffer.length() > 0) {
        processSerialCommand(serialBuffer);
        serialBuffer = "";
      }
    } else {
      serialBuffer += c;
      if (serialBuffer.length() > 100) serialBuffer = "";  // safety against garbage input
    }
  }
}

static uint16_t simTouchX = 0, simTouchY = 0;
static bool simTouchPending = false;

void triggerSimTouch(uint16_t x, uint16_t y) {
  simTouchX = x;
  simTouchY = y;
  simTouchPending = true;
}

// Splits `line` into words (spaces / commas) into toks[]; returns how many.
static int splitWords(const char *line, char toks[][12], int maxToks) {
  int n = 0;
  const char *p = line;
  while (*p && n < maxToks) {
    while (*p == ' ' || *p == ',' || *p == '\t') p++;
    if (!*p) break;
    int k = 0;
    while (*p && *p != ' ' && *p != ',' && *p != '\t') { if (k < 11) toks[n][k++] = *p; p++; }
    toks[n][k] = '\0';
    n++;
  }
  return n;
}

// LOADOUT 1 7 5   (power ids or names: two attacks + one defence)
void handleLoadoutCommand(const char *line) {
  if (loadoutLocked) {
    Serial.print("Loadout already locked: "); printLoadout(myLoadout); Serial.println();
    return;
  }
  if (!shipsLocked) {
    memcpy(myShips, wizShips, sizeof(myShips));
    computeOwnShips();
    shipsLocked = true;
    wizReviewMode = false;
  }
  char toks[8][12];
  int n = splitWords(line, toks, 8);
  uint8_t loadout = LOADOUT_SHIELD_BIT;
  for (int i = 1; i < n; i++) {                        // toks[0] is the word LOADOUT
    uint8_t id = parsePowerToken(toks[i]);
    if (id == 0) { Serial.print("Unknown power: "); Serial.println(toks[i]); return; }
    if (id != POWER_SHIELD) loadout |= LOADOUT_BIT(id);
  }
  if (!loadoutIsValid(loadout)) {
    Serial.println("A loadout is exactly 2 attack powers (Sonar 1, Salvo 2, Mine 3, Double 7) and 1 defence power (Repair 4, Smoke 5).");
    Serial.println("Example:  LOADOUT 1 7 5");
    return;
  }
  lockLoadout(loadout);
}

// POWER SONAR 2 3 3   POWER SALVO 2 1 1 2 1 3 1   POWER DOUBLE 2 0 3 6 6   POWER MINE 5 6   POWER SMOKE
void handlePowerCommand(const char *line) {
  char toks[10][12];
  int n = splitWords(line, toks, 10);
  uint8_t power = (n >= 2) ? parsePowerToken(toks[1]) : 0;
  const int si = specIndex(power);
  if (si < 0) {
    Serial.println("Usage: POWER <SONAR|SALVO|MINE|REPAIR|SMOKE|SHIELD|DOUBLE> [team] [x y] ...  e.g.  POWER SONAR 2 3 3");
    return;
  }
  const PowerSpec &sp = POWER_SPECS[si];
  int vals[7], nv = 0;
  for (int i = 2; i < n && nv < 7; i++) vals[nv++] = atoi(toks[i]);
  const int need = (sp.needTarget ? 1 : 0) + 2 * sp.cells;
  if (nv != need) {
    Serial.print(sp.name); Serial.print(" needs "); Serial.print(need); Serial.println(" number(s): [team] then x y for each cell.");
    return;
  }
  uint8_t target = 0, xs[3] = {0, 0, 0}, ys[3] = {0, 0, 0};
  int k = 0;
  if (sp.needTarget) target = (uint8_t)vals[k++];
  for (int i = 0; i < sp.cells; i++) { xs[i] = (uint8_t)vals[k++]; ys[i] = (uint8_t)vals[k++]; }
  attemptPower(power, target, xs, ys);
}

void processSerialCommand(String line) {
  line.trim();
  if (line.length() == 0) return;

#ifdef USE_VIRTUAL_DISPLAY
  // Messages from virtual_display.py (not typed by a person)
  if (line.startsWith("@TOUCH ")) {
    int tx, ty;
    if (sscanf(line.c_str() + 7, "%d %d", &tx, &ty) == 2 && tx >= 0 && ty >= 0) {
      tft.feedTouch((uint16_t)tx, (uint16_t)ty);
    }
    return;
  }
  if (line == "@REDRAW") { uiRedraw(); return; }

  // DEMO commands: show UI screens without needing Central. These only
  // exist in virtual-display builds, so the real firmware never has them.
  String demo = line;
  demo.toUpperCase();
  if (demo.startsWith("DEMO")) {
    if (demo == "DEMO WIZARD") {
      shipsLocked = false;
      wizShipIdx = 0;
      uiDrawShipWizard();
      return;
    }
    if (demo == "DEMO LOADOUT") {
      loadoutLocked = false; demoNoRegister = true; ldAttack = 0; ldDefence = 0;
      uiDrawLoadout();
      return;
    }
    // every other demo needs a locked loadout; pretend one (Sonar + Double + Smoke) without registering
    if (!loadoutLocked) {
      demoNoRegister = true;
      myLoadout = (uint8_t)(LOADOUT_SHIELD_BIT | LOADOUT_BIT(POWER_SONAR) | LOADOUT_BIT(POWER_DOUBLE) | LOADOUT_BIT(POWER_SMOKE));
      loadoutLocked = true;
      for (int p = 1; p <= 7; p++) usesLeft[p] = (p != POWER_SHIELD && (myLoadout & LOADOUT_BIT(p))) ? POWER_USES : 0;
    }
    if (demo == "DEMO MENU") {
      roundState = STATE_RUNNING;
      currentTurnTeam = MY_TEAM_ID;
      uiMyTurnShown = true;
      uiDrawPowerMenu();
    } else if (demo == "DEMO SONAR") {
      pendingX[0] = 3; pendingY[0] = 4;
      uint8_t r[3] = {2, 2, 2};
      uiShowPowerResult(POWER_SONAR, PWR_STATUS_OK, opponentIds[0], 2, r);
    } else if (demo == "DEMO SALVO") {
      pendingCells = 3; pendingX[0] = 1; pendingX[1] = 2; pendingX[2] = 3; pendingY[0] = pendingY[1] = pendingY[2] = 4;
      uint8_t r[3] = {RESULT_MISS, RESULT_HIT, RESULT_UNKNOWN};
      uiShowPowerResult(POWER_SALVO, PWR_STATUS_OK, opponentIds[0], 0, r);
    } else if (demo == "DEMO MINE")    { uiShowAttackResult(opponentIds[0], 2, 3, RESULT_MINE); }
    else if (demo == "DEMO BLOCKED")   { uiShowAttackResult(opponentIds[0], 2, 3, RESULT_BLOCKED); }
    else if (demo == "DEMO UNKNOWN")   { uiShowAttackResult(opponentIds[0], 2, 3, RESULT_UNKNOWN); }
    else if (demo == "DEMO HIT") {
      bool found = false;
      for (int r = 0; r < GRID_SIZE && !found; r++) {
        for (int c = 0; c < GRID_SIZE && !found; c++) {
          if (ownShip[r][c] && !ownDamage[r][c]) {
            ownDamage[r][c] = true;
            Serial.printf("[DEFENSE] Hit taken on own ship at (%d,%d)!\n", c, r);
            flashHitFeedback();
            found = true;
          }
        }
      }
      if (uiLastScreen == SCR_ACTION) { drawGrid(); drawAttackButton(uiActionReady(), uiActionLabel()); }
    }
    else if (demo == "DEMO TURN") {
      roundState = STATE_RUNNING;
      currentTurnTeam = MY_TEAM_ID;          // makes the touch handling active too
      uiShowYourTurn();
    } else if (demo == "DEMO WAIT") {
      roundState = STATE_RUNNING;
      currentTurnTeam = (MY_TEAM_ID % MAX_TEAMS) + 1;   // some other team
      uiShowWaiting(currentTurnTeam);
    } else if (demo == "DEMO HIT")     { uiShowAttackResult(opponentIds[0], 2, 3, RESULT_HIT); }
    else if (demo == "DEMO MISS")      { uiShowAttackResult(opponentIds[0], 2, 3, RESULT_MISS); }
    else if (demo == "DEMO SUNK")      { uiShowAttackResult(opponentIds[0], 2, 3, RESULT_SUNK); }
    else if (demo == "DEMO INVALID")   { uiShowAttackResult(opponentIds[0], 2, 3, RESULT_INVALID); }
    else if (demo == "DEMO MSG")       { uiShowMessage("Demo message"); }
    else Serial.println("DEMO options: WIZARD, LOADOUT, TURN, MENU, WAIT, HIT, MISS, SUNK, INVALID, MINE, BLOCKED, UNKNOWN, SONAR, SALVO, MSG");
    return;
  }
#endif

  String upper = line;
  if (upper.startsWith("@TOUCH")) {
    int tx, ty;
    if (sscanf(line.c_str(), "@TOUCH %d %d", &tx, &ty) == 2) {
      triggerSimTouch((uint16_t)tx, (uint16_t)ty);
      return;
    }
  }
  if (upper == "SHIPS DEFAULT") { wizUseDefaultShips(); return; }
  if (wizReviewMode && (upper == "CONFIRM" || upper == "LOCK" || upper == "SHIPS LOCK")) {
    memcpy(myShips, wizShips, sizeof(myShips));
    computeOwnShips();
    shipsLocked = true;
    wizReviewMode = false;
    Serial.printf("[WIZARD] Default 4-ship configuration (#%d) locked in!\n", wizDefaultChoice + 1);
    uiDrawLoadout();
    return;
  }
  if (upper.startsWith("SHIPS SET")) {
    int x0, y0, o0, x1, y1, o1, x2, y2, o2, x3, y3, o3;
    if (sscanf(line.c_str(), "SHIPS SET %d %d %d %d %d %d %d %d %d %d %d %d",
               &x0, &y0, &o0, &x1, &y1, &o1, &x2, &y2, &o2, &x3, &y3, &o3) == 12) {
      wizShips[0] = { 5, (uint8_t)x0, (uint8_t)y0, (uint8_t)o0 };
      wizShips[1] = { 4, (uint8_t)x1, (uint8_t)y1, (uint8_t)o1 };
      wizShips[2] = { 4, (uint8_t)x2, (uint8_t)y2, (uint8_t)o2 };
      wizShips[3] = { 3, (uint8_t)x3, (uint8_t)y3, (uint8_t)o3 };
      if (localValidateShips(wizShips)) {
        memcpy(myShips, wizShips, sizeof(myShips));
        computeOwnShips();
        shipsLocked = true;
        Serial.println("[WIZARD] All 4 ships configured & locked in!");
        uiDrawLoadout();
        return;
      } else {
        Serial.println("[WIZARD] Custom ships invalid, using defaults");
        wizUseDefaultShips();
        return;
      }
    }
  }
  if (upper == "RESET" || upper == "RELOAD" || upper == "RESTART") {
    registrationConfirmed = false;
    teamSelected = false;
    shipsLocked = false;
    loadoutLocked = false;
    wizShipIdx = 0;
    ldAttack = 0;
    ldDefence = 0;
    myMineX = -1; myMineY = -1;
    memset(oppGrid, TRACK_UNKNOWN, sizeof(oppGrid));
    memset(ownDamage, 0, sizeof(ownDamage));
    memset(ownMisses, 0, sizeof(ownMisses));
    setTurnLed(false);
    Serial.println("[RESET] Board reset. Returning to Team Selection.");
    uiDrawTeamSelect();
    return;
  }
  if (upper == "CONFIG") {
    Serial.printf("MY_TEAM_ID: %d\n", myTeamId);
    Serial.print("Central MAC: ");
    for (int i = 0; i < 6; i++) {
      if (centralMac[i] < 16) Serial.print("0");
      Serial.print(centralMac[i], HEX);
      if (i < 5) Serial.print(":");
    }
    Serial.println();
    Serial.print("My MAC: "); Serial.println(WiFi.macAddress());
    for (int s = 0; s < NUM_SHIPS; s++) {
      Serial.printf("  ship %d: len=%d start=(%d,%d) %s\n",
                    s + 1, myShips[s].ship_len, myShips[s].start_x, myShips[s].start_y,
                    wizGetOrientName(s, myShips[s].orientation));
    }
    Serial.printf("Loadout: (byte 0x%02X)\n", myLoadout);
    return;
  }
  if (upper == "STATUS") {
    Serial.printf("=== STATUS ===\nTeam: %d (%s)\nRegistered: %s\nRound State: %d\nShips Locked: %s\nLoadout Locked: %s\n",
                  myTeamId, MY_TEAM_NAME,
                  registrationConfirmed ? "YES" : "NO",
                  roundState,
                  shipsLocked ? "YES" : "NO",
                  loadoutLocked ? "YES" : "NO");
    return;
  }
  if (upper == "TEAM" || upper == "TEAMS") { uiDrawTeamSelect(); return; }
  if (upper.startsWith("TEAM ") || upper.startsWith("T ")) {
    int spaceIdx = upper.indexOf(' ');
    int tid = upper.substring(spaceIdx + 1).toInt();
    if (tid >= 1 && tid <= MAX_TEAMS) {
      selectTeam(tid);
      return;
    } else {
      Serial.println("Invalid team ID. Please choose 1, 2, 3, or 4.");
      return;
    }
  }
  if (upper == "SHIPS") {
    shipsLocked = false;
    wizShipIdx = 0;
    Serial.println("[WIZARD] Fleet wizard reopened.");
    uiDrawShipWizard();
    return;
  }
  if (upper == "LOADOUT") {
    loadoutLocked = false;
    ldAttack = 0;
    ldDefence = 0;
    Serial.println("[LOADOUT] Loadout selection reopened.");
    uiDrawLoadout();
    return;
  }
  if (upper.startsWith("LOADOUT ")) {
    if (!shipsLocked) {
      wizUseDefaultShips();
    }
    handleLoadoutCommand(line.c_str());
    return;
  }
  if (upper.startsWith("POWER"))   { handlePowerCommand(line.c_str());   return; }

  // Otherwise, try to parse it as a manual attack: "target x y", e.g. "2 3 4"
  int target, x, y;
  if (sscanf(line.c_str(), "%d %d %d", &target, &x, &y) == 3) {
    attemptManualAttack((uint8_t)target, (uint8_t)x, (uint8_t)y);
    return;
  }

  Serial.println("Commands: SHIPS DEFAULT, LOADOUT <ids>, POWER <name> ..., or 'target x y'");
}

// =====================================================================
// UI LAYER - real implementation for a 2.4" SPI touch TFT
// (ILI9341 + XPT2046 via the TFT_eSPI library)
// =====================================================================
// Screens, in the order a team meets them:
//   LOADOUT   before registering: tap 2 attack powers + 1 defence power, LOCK IN
//   MESSAGE / WAITING   while connecting, waiting for the start, or for another team
//   ACTION    on your turn: the Single Strike (target + cell + ATTACK), or - after
//             POWERS -> pick one - the same screen set up for that power
//   POWER MENU   the powers you can use right now, with the uses left
//   BANNER    the result of a shot or a power, shown for a moment
//
// Only game logic talks to this layer, and only through uiInit() / uiDrawLoadout() /
// uiShowMessage() / uiShowYourTurn() / uiShowWaiting() / uiShowAttackResult() /
// uiShowPowerResult(). Screen is assumed 240x320 portrait. If your display is
// mounted upside down, change tft.setRotation(0) in uiInit() to 2.
//
// A result banner stays on screen for a moment before the next screen is drawn.
// Central sends the result and the next turn update in quick succession, so
// without this the result would flash by too fast to read.

#define UI_RESULT_DISPLAY_MS 3500   // hit / miss / mine result banner duration
#define UI_POWER_RESULT_MS   4000   // salvo / double results need longer to read
#define UI_SONAR_RESULT_MS   8000   // sonar ping result stays for 8s so participants can note it

// --- ACTION screen layout (strike and every power that picks cells) ---
#define UI_TARGET_BTN_Y   36
#define UI_TARGET_BTN_H   30
#define UI_TARGET_BTN_W   70
#define UI_TARGET_GAP     10

#define UI_GRID_X         22
#define UI_GRID_Y         74
#define UI_CELL_SIZE      28

// bottom row:  [ POWERS / BACK ]  [ ATTACK / the power's action ]
#define UI_ALT_BTN_X      10
#define UI_ALT_BTN_W      84
#define UI_ATTACK_BTN_X   104
#define UI_ATTACK_BTN_Y   280
#define UI_ATTACK_BTN_W   126
#define UI_ATTACK_BTN_H   28

// --- POWER MENU layout ---
#define UI_MENU_X         10
#define UI_MENU_W         220
#define UI_MENU_Y0        36
#define UI_MENU_H         50
#define UI_MENU_GAP       6

// --- LOADOUT screen layout ---
#define LD_BTN_W          108
#define LD_BTN_H          40
#define LD_COL1_X         8
#define LD_COL2_X         124
#define LD_ATK_Y1         56
#define LD_ATK_Y2         100
#define LD_DEF_Y          158
#define LD_OK_X           40
#define LD_OK_Y           264
#define LD_OK_W           160
#define LD_OK_H           40

// The 7x7 grid must fit between the target buttons and the bottom buttons on the 240x320 screen.
static_assert(UI_GRID_X + GRID_SIZE * UI_CELL_SIZE <= 240, "grid is too wide for the screen");
static_assert(UI_GRID_Y + GRID_SIZE * UI_CELL_SIZE < UI_ATTACK_BTN_Y, "grid overlaps the bottom buttons");
static_assert(UI_ATTACK_BTN_Y + UI_ATTACK_BTN_H <= 320, "bottom buttons are off the screen");
static_assert(UI_ALT_BTN_X + UI_ALT_BTN_W < UI_ATTACK_BTN_X, "bottom buttons overlap");
static_assert(UI_ATTACK_BTN_X + UI_ATTACK_BTN_W <= 240, "action button is off the screen");
static_assert(UI_MENU_Y0 + 3 * (UI_MENU_H + UI_MENU_GAP) + UI_MENU_H < UI_ATTACK_BTN_Y, "power menu overlaps the BACK button");
static_assert(LD_COL2_X + LD_BTN_W <= 240, "loadout buttons are too wide");
static_assert(LD_OK_Y + LD_OK_H <= 320, "LOCK IN button is off the screen");

// (Screen IDs SCR_* and UI state variables uiLastScreen, uiSelTarget, etc. are declared in the GAME & UI STATE section above)

bool ldArmed = false;              // LOCK IN was tapped once; tap again to confirm
unsigned long ldArmedUntil = 0;

static const uint8_t LD_ATTACK_IDS[4] = { POWER_SONAR, POWER_SALVO, POWER_MINE, POWER_DOUBLE };
static const uint8_t LD_DEFENCE_IDS[2] = { POWER_SMOKE, POWER_REPAIR };

// --- small helpers ---

static bool inBox(int tx, int ty, int x, int y, int w, int h) {
  return tx >= x && tx < x + w && ty >= y && ty < y + h;
}

static void uiCenterText(const char *s, int y, uint8_t size, uint16_t fg, uint16_t bg) {
  tft.setTextSize(size);
  tft.setTextColor(fg, bg);
  int16_t tw = tft.textWidth(s);
  tft.setCursor((240 - tw) / 2, y);
  tft.print(s);
}

// One touch per tap (the panel reports a held finger over and over).
static bool uiReadTouch(uint16_t *tx, uint16_t *ty) {
  if (simTouchPending) {
    *tx = simTouchX;
    *ty = simTouchY;
    simTouchPending = false;
    return true;
  }
#if defined(USE_VIRTUAL_DISPLAY) || defined(TOUCH_CS)
  if (!tft.getTouch(tx, ty)) return false;
#else
  return false;  // touch not compiled into TFT_eSPI - see the TOUCH_CS note above
#endif
  static unsigned long lastTouchMillis = 0;
  if (millis() - lastTouchMillis < 250) return false;  // simple debounce
  lastTouchMillis = millis();
  return true;
}

void computeOpponentIds() {
  int n = 0;
  for (uint8_t t = 1; t <= MAX_TEAMS; t++) {
    if (t != MY_TEAM_ID) opponentIds[n++] = t;
  }
  numOpponents = n;
  if (numOpponents == 1) uiSelTarget = opponentIds[0];
}

void uiInit() {
  tft.init();
  tft.setRotation(0);  // portrait - change to 2 if mounted upside down
#ifndef USE_VIRTUAL_DISPLAY
  #ifdef TOUCH_CS
  tft.setTouch(touchCalData);
  #else
  Serial.println("WARNING: TOUCH_CS is not defined in TFT_eSPI's User_Setup.h - touch is DISABLED.");
  #endif
#else
  tft.setTouch(touchCalData);
#endif
  tft.fillScreen(TFT_BLACK);
  computeOpponentIds();
  memset(oppGrid, TRACK_UNKNOWN, sizeof(oppGrid));
  myMineX = -1; myMineY = -1;
}

// --- public interface (called from game logic / packet handlers) ---

void uiShowMessage(const char *msg) {
  Serial.print("[UI] "); Serial.println(msg);
  if (millis() < uiResultUntil) {
    uiPendingScreen = SCR_MESSAGE;
    strncpy(uiPendingMessage, msg, sizeof(uiPendingMessage) - 1);
    uiPendingMessage[sizeof(uiPendingMessage) - 1] = '\0';
    return;
  }
  uiDrawMessage(msg);
}

void uiShowYourTurn() {
  if (millis() < uiResultUntil) {
    uiPendingScreen = SCR_ACTION;
    return;
  }
  if (uiMyTurnShown) return;   // a repeated turn update must not wipe a half-made selection
  uiDrawYourTurn();
}

void uiShowWaiting(uint8_t currentTeam) {
  if (millis() < uiResultUntil) {
    uiPendingScreen = SCR_WAITING;
    uiPendingWaitingTeam = currentTeam;
    return;
  }
  uiDrawWaiting(currentTeam);
}

void uiShowAttackResult(uint8_t targetTeam, uint8_t x, uint8_t y, uint8_t result) {
  uiDrawResultBanner(targetTeam, x, y, result);
  uiResultUntil = millis() + UI_RESULT_DISPLAY_MS;
  uiBannerShowing = true;
  uiSendLockUntil = 0;
}

void uiShowPowerResult(uint8_t power, uint8_t status, uint8_t target, uint8_t count, const uint8_t res[3]) {
  uiDrawPowerBanner(power, status, target, count, res);
  unsigned long duration = UI_RESULT_DISPLAY_MS + 300;
  if (status != PWR_STATUS_REJECTED) {
    if (power == POWER_SONAR) duration = UI_SONAR_RESULT_MS;
    else if (power == POWER_SALVO || power == POWER_DOUBLE) duration = UI_POWER_RESULT_MS;
  }
  uiResultUntil = millis() + duration;
  uiBannerShowing = true;
  uiSendLockUntil = 0;
}

// Called every loop() iteration: finishes a result banner, and reads touches.
void uiTick() {
  if (!teamSelected) {             // still selecting team at startup
    uiPollTeamSelectTouch();
    return;
  }
  if (!shipsLocked) {              // still placing ships in wizard
    uiPollShipWizardTouch();
    return;
  }
  if (!loadoutLocked) {            // still choosing powers
    uiPollLoadoutTouch();
    return;
  }

  // Early tap-to-dismiss result banner or natural expiry
  if (uiBannerShowing) {
    uint16_t tx, ty;
    bool tapped = uiReadTouch(&tx, &ty);
    if (tapped || millis() >= uiResultUntil) {
      uiBannerShowing = false;
      uiResultUntil = 0;
      uint8_t screen = uiPendingScreen;
      uiPendingScreen = 0;
      if (screen == SCR_ACTION) uiDrawYourTurn();
      else if (screen == SCR_WAITING) uiDrawWaiting(uiPendingWaitingTeam);
      else if (screen == SCR_MESSAGE) uiDrawMessage(uiPendingMessage);
      else if (roundState == STATE_RUNNING && currentTurnTeam == MY_TEAM_ID) uiDrawYourTurn();   // rejected move: still my turn
      else if (roundState == STATE_RUNNING) uiDrawWaiting(currentTurnTeam);
      else uiRedraw();
    }
    return;
  }

  // Interactive controls: active on my turn (ACTION, MENU) and allows tab switching while WAITING
  bool interactive = (roundState == STATE_RUNNING && !uiBannerShowing && uiPendingScreen == 0
                      && (uiLastScreen == SCR_ACTION || uiLastScreen == SCR_MENU || uiLastScreen == SCR_WAITING));
  if (interactive) uiPollTouch();
}

// Repaint whatever full screen was last showing (no-op if none yet).
void uiRedraw() {
  if (!teamSelected && uiLastScreen == SCR_TEAM_SELECT) uiDrawTeamSelect();
  else if (uiLastScreen == SCR_WIZARD) uiDrawShipWizard();
  else if (uiLastScreen == SCR_LOADOUT) uiDrawLoadout();
  else if (uiLastScreen == SCR_ACTION) uiDrawActionScreen();
  else if (uiLastScreen == SCR_MENU) uiDrawPowerMenu();
  else if (uiLastScreen == SCR_WAITING) uiDrawWaiting(uiLastWaitingTeam);
  else if (uiLastScreen == SCR_MESSAGE) {
    char msg[40];
    strncpy(msg, uiLastMessage, sizeof(msg));
    msg[sizeof(msg) - 1] = '\0';
    uiDrawMessage(msg);
  }
}

// --- simple full screens ---

void uiDrawMessage(const char *msg) {
  uiLastScreen = SCR_MESSAGE;
  uiMyTurnShown = false;
  strncpy(uiLastMessage, msg, sizeof(uiLastMessage) - 1);
  uiLastMessage[sizeof(uiLastMessage) - 1] = '\0';
  tft.fillScreen(TFT_BLACK);
  tft.setTextColor(TFT_WHITE, TFT_BLACK);
  tft.setTextSize(2);
  tft.setCursor(10, 140);
  tft.print(msg);
}

void uiDrawWaiting(uint8_t currentTeam) {
  uiLastScreen = SCR_WAITING;
  uiMyTurnShown = false;
  uiLastWaitingTeam = currentTeam;
  tft.fillScreen(TFT_BLACK);

  // Header Title
  tft.setTextColor(TFT_YELLOW, TFT_BLACK);
  tft.setTextSize(2);
  tft.setCursor(10, 8);
  char buf[36];
  snprintf(buf, sizeof(buf), "WAITING (T%d)", currentTeam);
  tft.print(buf);

  // Subtitle / context
  tft.setTextColor(TFT_LIGHTGREY, TFT_BLACK);
  tft.setTextSize(1);
  tft.setCursor(10, 26);
  if (uiViewingOwnFleet) {
    int total = 0, dmg = 0;
    for (int r = 0; r < GRID_SIZE; r++) {
      for (int c = 0; c < GRID_SIZE; c++) {
        if (ownShip[r][c]) { total++; if (ownDamage[r][c]) dmg++; }
      }
    }
    snprintf(buf, sizeof(buf), "Viewing My Fleet - HP: %d/%d", total - dmg, total);
  } else {
    snprintf(buf, sizeof(buf), "Viewing Radar: TEAM %d", uiSelTarget);
  }
  tft.print(buf);

  drawAllTabs();
  drawGrid();

  // Bottom Waiting indicator
  tft.fillRoundRect(10, UI_ATTACK_BTN_Y, 220, UI_ATTACK_BTN_H, 6, TFT_NAVY);
  tft.drawRoundRect(10, UI_ATTACK_BTN_Y, 220, UI_ATTACK_BTN_H, 6, TFT_DARKGREY);
  snprintf(buf, sizeof(buf), "WAITING FOR TEAM %d...", currentTeam);
  tft.setTextColor(TFT_WHITE, TFT_NAVY);
  tft.setTextSize(1);
  int16_t tw = tft.textWidth(buf);
  tft.setCursor(10 + (220 - tw) / 2, UI_ATTACK_BTN_Y + 10);
  tft.print(buf);
}

// --- result banners ---

void uiDrawResultBanner(uint8_t targetTeam, uint8_t x, uint8_t y, uint8_t result) {
  uiMyTurnShown = false;
  uint16_t bg;
  const char *label;
  const char *sub = "";
  const char *extra = "";
  switch (result) {
    case RESULT_MISS:
      bg = TFT_NAVY;
      label = "MISS";
      sub = "SHOT IN THE OCEAN";
      break;
    case RESULT_HIT:
      bg = TFT_ORANGE;
      label = "DIRECT HIT!";
      sub = "+10 POINTS";
      break;
    case RESULT_SHIP_SUNK:
      bg = TFT_RED;
      label = "SHIP SUNK!";
      sub = "+15 POINTS (HIT + SINK)";
      break;
    case RESULT_SUNK:
      bg = TFT_MAROON;
      label = "TEAM ELIMINATED!";
      sub = "+20 POINTS";
      break;
    case RESULT_MINE:
      bg = TFT_MAROON;
      label = "MINE TRIGGERED!";
      sub = "1 CHANCE LOST!";
      extra = "You lose your next turn.";
      break;
    case RESULT_BLOCKED:
      bg = TFT_PURPLE;
      label = "BLOCKED";
      sub = "DEFLECTED BY SHIELD";
      break;
    case RESULT_UNKNOWN:
      bg = TFT_DARKGREY;
      label = "SMOKE SCREEN";
      sub = "RESULT HIDDEN";
      break;
    default:
      bg = TFT_DARKGREY;
      label = "INVALID MOVE";
      sub = "REJECTED BY REFEREE";
      break;
  }

  tft.fillScreen(bg);
  tft.drawRect(5, 5, 230, 310, TFT_WHITE);
  tft.drawRect(7, 7, 226, 306, TFT_WHITE);

  uiCenterText(label, 70, 3, TFT_WHITE, bg);
  if (sub[0]) uiCenterText(sub, 120, 2, TFT_YELLOW, bg);

  char buf[36];
  snprintf(buf, sizeof(buf), "TARGET: TEAM %d (%d,%d)", targetTeam, x, y);
  uiCenterText(buf, 170, 2, TFT_WHITE, bg);
  if (extra[0]) uiCenterText(extra, 210, 1, TFT_WHITE, bg);

  uiCenterText("[Tap anywhere to dismiss]", 285, 1, TFT_LIGHTGREY, bg);
}

static const char *uiResultName(uint8_t r) {
  switch (r) {
    case RESULT_MISS:      return "MISS";
    case RESULT_HIT:       return "HIT";
    case RESULT_SHIP_SUNK: return "SHIP SUNK";
    case RESULT_SUNK:      return "SUNK";
    case RESULT_MINE:      return "MINE";
    case RESULT_BLOCKED:   return "BLOCKED";
    case RESULT_UNKNOWN:   return "UNKNOWN";
  }
  return "NOT FIRED";
}

static const char *uiRejectHint(uint8_t power) {
  switch (power) {
    case POWER_SONAR:
    case POWER_SALVO:
    case POWER_DOUBLE: return "Cell already shot? Target out?";
    case POWER_MINE:   return "Mine still active? Cell shot?";
    case POWER_REPAIR: return "Cell not damaged or ship sunk";
    case POWER_SHIELD: return "No Shield two turns in a row";
    case POWER_SMOKE:  return "Smoke can't be used now";
  }
  return "Central refused it";
}

// The result of a power-up. `res` holds Salvo / Double results in the order they were sent.
void uiDrawPowerBanner(uint8_t power, uint8_t status, uint8_t target, uint8_t count, const uint8_t res[3]) {
  uiMyTurnShown = false;
  const int si = specIndex(power);
  const char *name = (si >= 0) ? POWER_SPECS[si].name : "POWER";
  char buf[32];
  uint16_t bg = TFT_DARKGREEN;

  if (status == PWR_STATUS_REJECTED) {
    bg = TFT_DARKGREY;
    tft.fillScreen(bg);
    uiCenterText("REJECTED", 70, 3, TFT_WHITE, bg);
    uiCenterText(name, 110, 2, TFT_WHITE, bg);
    uiCenterText("Turn NOT used.", 150, 2, TFT_WHITE, bg);
    uiCenterText(uiRejectHint(power), 186, 1, TFT_WHITE, bg);
    return;
  }

  switch (power) {
    case POWER_SONAR:
      if (status == PWR_STATUS_JAMMED) {
        bg = TFT_DARKGREY;
        tft.fillScreen(bg);
        uiCenterText("SONAR", 70, 3, TFT_WHITE, bg);
        uiCenterText("JAMMED", 120, 3, TFT_WHITE, bg);
        uiCenterText("Smoke blocked the ping", 170, 1, TFT_WHITE, bg);
      } else {
        bg = TFT_DARKCYAN;
        tft.fillScreen(bg);
        uiCenterText("SONAR", 70, 3, TFT_WHITE, bg);
        snprintf(buf, sizeof(buf), "TEAM %d around (%d,%d)", target, pendingX[0], pendingY[0]);
        uiCenterText(buf, 114, 1, TFT_WHITE, bg);
        snprintf(buf, sizeof(buf), count == 1 ? "%d SHIP CELL" : "%d SHIP CELLS", count);
        uiCenterText(buf, 140, 3, TFT_YELLOW, bg);
      }
      uiCenterText("[Tap screen to dismiss]", 285, 1, TFT_LIGHTGREY, bg);
      break;

    case POWER_SALVO:
    case POWER_DOUBLE: {
      int totalFired = (power == POWER_SALVO) ? 3 : 2;
      int hitCount = 0, missCount = 0, mineCount = 0;
      for (int i = 0; i < totalFired && i < 3; i++) {
        if (res[i] == RESULT_HIT || res[i] == RESULT_SHIP_SUNK || res[i] == RESULT_SUNK) hitCount++;
        else if (res[i] == RESULT_MINE) mineCount++;
        else if (res[i] == RESULT_MISS) missCount++;
      }

      bg = TFT_NAVY;
      if (hitCount > 0) bg = TFT_ORANGE;
      if (mineCount > 0) bg = TFT_MAROON;

      tft.fillScreen(bg);
      tft.drawRect(5, 5, 230, 310, TFT_WHITE);
      tft.drawRect(7, 7, 226, 306, TFT_WHITE);

      uiCenterText(name, 35, 2, TFT_WHITE, bg);

      char summary[36];
      if (mineCount > 0) {
        snprintf(summary, sizeof(summary), "MINE DETONATED!");
      } else if (hitCount == totalFired) {
        snprintf(summary, sizeof(summary), "%d HITS! PERFECT!", hitCount);
      } else if (hitCount > 0 && missCount > 0) {
        snprintf(summary, sizeof(summary), "%d HITS, %d MISS", hitCount, missCount);
      } else if (hitCount > 0) {
        snprintf(summary, sizeof(summary), "%d HIT%s!", hitCount, hitCount > 1 ? "S" : "");
      } else {
        snprintf(summary, sizeof(summary), "ALL %d MISSES", missCount);
      }
      uiCenterText(summary, 70, 3, (hitCount > 0) ? TFT_YELLOW : TFT_WHITE, bg);

      if (mineCount > 0) {
        uiCenterText("1 CHANCE LOST!", 110, 2, TFT_YELLOW, bg);
      } else {
        snprintf(buf, sizeof(buf), "TARGET: TEAM %d", target);
        uiCenterText(buf, 110, 2, TFT_WHITE, bg);
      }

      for (int i = 0; i < totalFired && i < 3; i++) {
        snprintf(buf, sizeof(buf), "Shot %d: (%d,%d) -> %s", i + 1, pendingX[i], pendingY[i], uiResultName(res[i]));
        uiCenterText(buf, 150 + i * 32, 2, TFT_WHITE, bg);
      }

      uiCenterText("[Tap screen to dismiss]", 285, 1, TFT_LIGHTGREY, bg);
      break;
    }

    case POWER_MINE:
      tft.fillScreen(bg);
      uiCenterText("MINE SET", 80, 3, TFT_WHITE, bg);
      snprintf(buf, sizeof(buf), "at (%d,%d)", pendingX[0], pendingY[0]);
      uiCenterText(buf, 130, 2, TFT_YELLOW, bg);
      uiCenterText("Only you know where it is.", 170, 1, TFT_WHITE, bg);
      break;

    case POWER_REPAIR:
      tft.fillScreen(bg);
      uiCenterText("REPAIRED", 80, 3, TFT_WHITE, bg);
      snprintf(buf, sizeof(buf), "(%d,%d) is intact", pendingX[0], pendingY[0]);
      uiCenterText(buf, 130, 2, TFT_YELLOW, bg);
      break;

    case POWER_SMOKE:
      tft.fillScreen(bg);
      uiCenterText("SMOKE UP", 80, 3, TFT_WHITE, bg);
      uiCenterText("Attacks on you are hidden", 130, 1, TFT_WHITE, bg);
      uiCenterText("until your next turn.", 144, 1, TFT_WHITE, bg);
      break;

    case POWER_SHIELD:
      tft.fillScreen(bg);
      uiCenterText("SHIELD UP", 80, 3, TFT_WHITE, bg);
      snprintf(buf, sizeof(buf), "3x3 around (%d,%d)", pendingX[0], pendingY[0]);
      uiCenterText(buf, 130, 2, TFT_YELLOW, bg);
      uiCenterText("until your next turn.", 170, 1, TFT_WHITE, bg);
      break;

    default:
      tft.fillScreen(bg);
      uiCenterText("DONE", 120, 3, TFT_WHITE, bg);
      break;
  }
}

// ---------------------------------------------------------------------
// ACTION screen: the Single Strike, or a power being set up
// ---------------------------------------------------------------------

static int uiActionCells() {
  if (uiMode == UIM_STRIKE) return 1;
  int si = specIndex(uiPower);
  return (si >= 0) ? POWER_SPECS[si].cells : 0;
}

static bool uiActionNeedsTarget() {
  if (uiMode == UIM_STRIKE) return true;
  int si = specIndex(uiPower);
  return si >= 0 && POWER_SPECS[si].needTarget;
}

static bool uiActionOwnGrid() {
  if (uiViewingOwnFleet) return true;
  if (uiMode == UIM_STRIKE) return false;
  int si = specIndex(uiPower);
  return si >= 0 && POWER_SPECS[si].ownGrid;
}

const char *uiActionLabel() {
  if (uiViewingOwnFleet && uiMode == UIM_STRIKE) return "PICK ENEMY";
  if (uiMode == UIM_STRIKE) return "ATTACK";
  int si = specIndex(uiPower);
  return (si >= 0) ? POWER_SPECS[si].action : "GO";
}

static bool isCellOfSunkShip(int c, int r) {
  for (int s = 0; s < NUM_SHIPS; s++) {
    int xs[5], ys[5];
    int n = getShipCells(myShips[s], xs, ys);
    bool inShip = false;
    for (int i = 0; i < n; i++) {
      if (xs[i] == c && ys[i] == r) {
        inShip = true;
        break;
      }
    }
    if (!inShip) continue;
    bool allHit = true;
    for (int i = 0; i < n; i++) {
      if (!ownDamage[ys[i]][xs[i]]) {
        allHit = false;
        break;
      }
    }
    return allHit;
  }
  return false;
}

bool uiActionReady() {
  if (uiMode == UIM_STRIKE && uiViewingOwnFleet) return false;  // Must select an opponent tab to attack!
  if (uiMode == UIM_POWER && uiPower == POWER_MINE && myMineX >= 0) return false;
  if (uiMode == UIM_POWER && uiPower == POWER_REPAIR) {
    if (uiSelCount != 1) return false;
    return ownShip[uiSelY[0]][uiSelX[0]] && ownDamage[uiSelY[0]][uiSelX[0]] && !isCellOfSunkShip(uiSelX[0], uiSelY[0]);
  }
  if (uiActionNeedsTarget() && uiSelTarget == 0) return false;
  if (uiSelCount != uiActionCells()) return false;
  if (uiMode == UIM_POWER && uiPower == POWER_SALVO) {
    uint8_t xs[3], ys[3];
    for (int i = 0; i < 3; i++) { xs[i] = (uint8_t)uiSelX[i]; ys[i] = (uint8_t)uiSelY[i]; }
    if (!cellsFormLine(xs, ys, 3)) return false;
  }
  return true;
}

// May this cell be picked in the current mode?
static bool uiCellAllowed(int c, int r) {
  if (uiMode == UIM_STRIKE && uiViewingOwnFleet) return false;  // Can't shoot own fleet
  if (uiMode == UIM_POWER) {
    switch (uiPower) {
      case POWER_MINE:
        if (myMineX >= 0) return false;
        return !ownShip[r][c];      // an empty cell of mine
      case POWER_REPAIR: return ownShip[r][c] && ownDamage[r][c] && !isCellOfSunkShip(c, r); // only damaged ship cells of non-sunk ships can be picked
      case POWER_SHIELD: return c >= 1 && c <= GRID_SIZE - 2 && r >= 1 && r <= GRID_SIZE - 2;   // centre of a full 3x3
      case POWER_SONAR:  return true;
      default: break;
    }
  }
  if (!uiActionOwnGrid() && uiSelTarget >= 1 && uiSelTarget <= MAX_TEAMS) {
    if (oppGrid[uiSelTarget][r][c] != TRACK_UNKNOWN) return false;
  }
  return true;
}

static void uiToggleCell(int c, int r) {
  const int maxCells = uiActionCells();
  if (maxCells == 1) {                  // single pick: tapping another cell replaces it
    uiSelX[0] = c; uiSelY[0] = r; uiSelCount = 1;
    return;
  }
  for (int i = 0; i < uiSelCount; i++) {
    if (uiSelX[i] == c && uiSelY[i] == r) {          // tapping a picked cell un-picks it
      for (int j = i; j < uiSelCount - 1; j++) { uiSelX[j] = uiSelX[j + 1]; uiSelY[j] = uiSelY[j + 1]; }
      uiSelCount--;
      return;
    }
  }
  if (uiSelCount == maxCells) {                      // full: drop the oldest pick
    for (int j = 0; j < uiSelCount - 1; j++) { uiSelX[j] = uiSelX[j + 1]; uiSelY[j] = uiSelY[j + 1]; }
    uiSelCount--;
  }
  uiSelX[uiSelCount] = c; uiSelY[uiSelCount] = r; uiSelCount++;
}

void uiDrawYourTurn() {
  uiMode = UIM_STRIKE;
  uiPower = 0;
  uiViewingOwnFleet = false;
  if (uiSelTarget == 0 && numOpponents > 0) {
    uiSelTarget = opponentIds[0];
  }
  uiSelCount = 0;
  uiMyTurnShown = true;
  uiDrawActionScreen();
}

static void uiStartPower(uint8_t power) {
  uiMode = UIM_POWER;
  uiPower = power;
  int si = specIndex(power);
  if (si >= 0 && POWER_SPECS[si].ownGrid) {
    uiViewingOwnFleet = true;
    uiSelTarget = 0;
  } else {
    uiViewingOwnFleet = false;
    if (uiSelTarget == 0 && numOpponents > 0) uiSelTarget = opponentIds[0];
  }
  uiSelCount = 0;
  uiDrawActionScreen();
}

static void getTabBtnRect(int tabIdx, int *outX, int *outY, int *outW, int *outH) {
  *outY = UI_TARGET_BTN_Y;
  *outH = UI_TARGET_BTN_H;
  int totalTabs = 1 + numOpponents;
  if (totalTabs >= 4) {
    *outW = 54;
    *outX = 5 + tabIdx * (54 + 4);
  } else if (totalTabs == 3) {
    *outW = 70;
    *outX = 8 + tabIdx * (70 + 6);
  } else {
    *outW = 100;
    *outX = 15 + tabIdx * (100 + 10);
  }
}

void drawTabButton(int tabIdx, bool selected) {
  int x, y, w, h;
  getTabBtnRect(tabIdx, &x, &y, &w, &h);
  int totalTabs = 1 + numOpponents;
  char label[16];
  uint16_t fillColor, textColor;

  if (tabIdx == 0) {
    fillColor = selected ? TFT_CYAN : TFT_NAVY;
    textColor = selected ? TFT_BLACK : TFT_WHITE;
    if (totalTabs >= 4) snprintf(label, sizeof(label), "FLEET");
    else snprintf(label, sizeof(label), "MY FLEET");
  } else {
    fillColor = selected ? TFT_YELLOW : TFT_NAVY;
    textColor = selected ? TFT_BLACK : TFT_WHITE;
    uint8_t oppId = opponentIds[tabIdx - 1];
    if (totalTabs == 2) snprintf(label, sizeof(label), "TGT T%d", oppId);
    else snprintf(label, sizeof(label), "T%d", oppId);
  }

  tft.fillRoundRect(x, y, w, h, 6, fillColor);
  tft.drawRoundRect(x, y, w, h, 6, TFT_WHITE);
  tft.setTextColor(textColor, fillColor);
  uint8_t sz = (totalTabs >= 4 && tabIdx == 0) ? 1 : 2;
  tft.setTextSize(sz);
  int16_t tw = tft.textWidth(label);
  if (tw > w - 4 && sz > 1) {
    sz = 1;
    tft.setTextSize(1);
    tw = tft.textWidth(label);
  }
  int textY = y + (h - (sz == 1 ? 8 : 16)) / 2;
  tft.setCursor(x + (w - tw) / 2, textY);
  tft.print(label);
}

void drawAllTabs() {
  int totalTabs = 1 + numOpponents;
  for (int i = 0; i < totalTabs; i++) {
    bool isSel = (i == 0) ? uiViewingOwnFleet : (!uiViewingOwnFleet && uiSelTarget == opponentIds[i - 1]);
    drawTabButton(i, isSel);
  }
}

// Backward-compatibility wrapper
void drawTargetButton(int i, bool selected) {
  drawTabButton(i + 1, selected);
}

static void getTargetBtnRect(int i, int *outX, int *outY, int *outW, int *outH) {
  getTabBtnRect(i + 1, outX, outY, outW, outH);
}

static uint16_t uiCellColor(int c, int r) {
  bool sel = false;
  if (uiMode == UIM_POWER && (uiPower == POWER_SHIELD || uiPower == POWER_SONAR) && uiSelCount == 1) {
    int dx = c - uiSelX[0], dy = r - uiSelY[0];
    sel = (dx >= -1 && dx <= 1 && dy >= -1 && dy <= 1);      // show the whole 3x3 area
  } else {
    for (int i = 0; i < uiSelCount; i++) if (uiSelX[i] == c && uiSelY[i] == r) sel = true;
  }
  if (sel) return TFT_YELLOW;

  if (uiViewingOwnFleet || uiActionOwnGrid()) {
    if (c == myMineX && r == myMineY) return TFT_MAROON;
    if (ownBlocked[r][c]) return TFT_PURPLE;
    if (ownShip[r][c]) {
      if (ownDamage[r][c]) return TFT_RED;
      return TFT_BLUE;
    }
    if (ownMisses[r][c]) return TFT_NAVY;
    return TFT_DARKGREY;
  } else if (uiSelTarget >= 1 && uiSelTarget <= MAX_TEAMS) {
    uint8_t st = oppGrid[uiSelTarget][r][c];
    if (st == TRACK_HIT) return TFT_RED;
    if (st == TRACK_MISS || st == TRACK_MINE) return TFT_NAVY;
    if (st == TRACK_BLOCKED) return TFT_PURPLE;
  }
  return TFT_DARKGREY;
}

void drawGrid() {
  const bool own = (uiViewingOwnFleet || uiActionOwnGrid());
  for (int r = 0; r < GRID_SIZE; r++) {
    for (int c = 0; c < GRID_SIZE; c++) {
      int x = UI_GRID_X + c * UI_CELL_SIZE;
      int y = UI_GRID_Y + r * UI_CELL_SIZE;
      uint16_t color = uiCellColor(c, r);
      tft.fillRect(x + 1, y + 1, UI_CELL_SIZE - 2, UI_CELL_SIZE - 2, color);
      tft.drawRect(x, y, UI_CELL_SIZE, UI_CELL_SIZE, TFT_WHITE);
      if (own && color == TFT_MAROON) {                      // my active mine
        tft.setTextColor(TFT_WHITE, TFT_MAROON);
        tft.setTextSize(2);
        tft.setCursor(x + 8, y + 6);
        tft.print("M");
      } else if (own && ownBlocked[r][c]) {
        tft.setTextColor(TFT_WHITE, TFT_PURPLE);
        tft.setTextSize(2);
        tft.setCursor(x + 8, y + 6);
        tft.print("S");
      } else if (own && ownDamage[r][c]) {
        bool isSel = (uiSelCount > 0 && uiSelX[0] == c && uiSelY[0] == r);
        tft.setTextColor(isSel ? TFT_RED : TFT_WHITE, color);
        tft.setTextSize(2);
        tft.setCursor(x + 8, y + 6);
        tft.print("X");
      } else if (own && ownMisses[r][c]) {
        tft.fillCircle(x + UI_CELL_SIZE / 2, y + UI_CELL_SIZE / 2, 4, TFT_CYAN);
      } else if (!own && uiSelTarget >= 1 && uiSelTarget <= MAX_TEAMS) {
        uint8_t st = oppGrid[uiSelTarget][r][c];
        if (st == TRACK_HIT) {
          tft.setTextColor(TFT_WHITE, TFT_RED);
          tft.setTextSize(2);
          tft.setCursor(x + 8, y + 6);
          tft.print("X");
        } else if (st == TRACK_MISS || st == TRACK_MINE) {
          tft.fillCircle(x + UI_CELL_SIZE / 2, y + UI_CELL_SIZE / 2, 4, TFT_CYAN);
        } else if (st == TRACK_BLOCKED) {
          tft.setTextColor(TFT_WHITE, TFT_PURPLE);
          tft.setTextSize(2);
          tft.setCursor(x + 8, y + 6);
          tft.print("S");
        }
      }
    }
  }
}

void uiDrawActionScreen() {
  uiLastScreen = SCR_ACTION;
  const bool strike = (uiMode == UIM_STRIKE);
  const int si = strike ? -1 : specIndex(uiPower);
  const bool noGrid = (uiActionCells() == 0);

  tft.fillScreen(TFT_BLACK);
  tft.setTextColor(strike ? TFT_GREEN : TFT_YELLOW, TFT_BLACK);
  tft.setTextSize(2);
  tft.setCursor(10, 8);
  tft.print(strike ? "YOUR TURN" : (si >= 0 ? POWER_SPECS[si].name : "POWER"));

  tft.setTextColor(TFT_WHITE, TFT_BLACK);
  tft.setTextSize(1);
  if (strike) {
    tft.setCursor(10, 26);
    if (uiViewingOwnFleet) {
      int total = 0, dmg = 0;
      for (int r = 0; r < GRID_SIZE; r++) {
        for (int c = 0; c < GRID_SIZE; c++) {
          if (ownShip[r][c]) { total++; if (ownDamage[r][c]) dmg++; }
        }
      }
      char hBuf[36];
      snprintf(hBuf, sizeof(hBuf), "Viewing Fleet - HP: %d/%d (Intact)", total - dmg, total);
      tft.print(hBuf);
    } else {
      char rBuf[36];
      snprintf(rBuf, sizeof(rBuf), "Radar: TEAM %d | Pick target cell", uiSelTarget);
      tft.print(rBuf);
    }
  } else {
    const char *l1 = "", *l2 = "";
    static char mineBuf[36];
    uint16_t cHint = TFT_WHITE;
    switch (uiPower) {
      case POWER_MINE:
        if (myMineX >= 0) {
          snprintf(mineBuf, sizeof(mineBuf), "MINE ACTIVE at (%d,%d).", myMineX, myMineY);
          l1 = mineBuf;
          l2 = "Max 1 active mine until detonated.";
          cHint = TFT_YELLOW;
        } else {
          l1 = "Tap an EMPTY cell of YOUR board.";
          l2 = "Hidden until someone shoots it.";
        }
        break;
      case POWER_REPAIR: {
        int dmg = 0;
        for (int r = 0; r < GRID_SIZE; r++)
          for (int c = 0; c < GRID_SIZE; c++)
            if (ownShip[r][c] && ownDamage[r][c]) dmg++;
        if (dmg == 0) {
          l1 = "ALL SHIPS INTACT!";
          l2 = "No damaged cells to repair.";
          cHint = TFT_YELLOW;
        } else {
          l1 = "Tap a RED damaged cell (X)";
          l2 = "to repair it. (teal=intact)";
          cHint = TFT_WHITE;
        }
        break;
      }
      case POWER_SHIELD: l1 = "Tap the CENTRE of a 3x3 block on";   l2 = "YOUR board (inner cells only)."; break;
      case POWER_SMOKE:  l1 = "No cells to pick."; break;
      default: l1 = "Pick target cell(s)"; break;
    }
    tft.setTextColor(cHint, TFT_BLACK);
    tft.setCursor(10, 24); tft.print(l1);
    tft.setTextColor(TFT_WHITE, TFT_BLACK);
  }

  drawAllTabs();

  if (noGrid) {                                    // smoke screen: just an explanation
    tft.setTextSize(2);
    tft.setCursor(10, 96);  tft.print("SMOKE SCREEN");
    tft.setTextSize(1);
    tft.setCursor(10, 130); tft.print("Until your next turn, every attack");
    tft.setCursor(10, 144); tft.print("on you is hidden from the attacker");
    tft.setCursor(10, 158); tft.print("AND from the projector.");
    tft.setCursor(10, 180); tft.print("Sonar pings at you are jammed.");
    tft.setCursor(10, 194); tft.print("Using it takes your turn.");
  } else {
    drawGrid();
  }

  drawAltButton(strike ? "POWERS" : "BACK");
  const char *actLabel = strike ? (uiViewingOwnFleet ? "PICK ENEMY" : "ATTACK") : ((uiPower == POWER_MINE && myMineX >= 0) ? "MINE ARMED" : uiActionLabel());
  drawAttackButton(uiActionReady(), actLabel);
}

void drawAltButton(const char *label) {
  tft.fillRoundRect(UI_ALT_BTN_X, UI_ATTACK_BTN_Y, UI_ALT_BTN_W, UI_ATTACK_BTN_H, 6, TFT_NAVY);
  tft.drawRoundRect(UI_ALT_BTN_X, UI_ATTACK_BTN_Y, UI_ALT_BTN_W, UI_ATTACK_BTN_H, 6, TFT_WHITE);
  tft.setTextColor(TFT_WHITE, TFT_NAVY);
  tft.setTextSize(2);
  int16_t tw = tft.textWidth(label);
  tft.setCursor(UI_ALT_BTN_X + (UI_ALT_BTN_W - tw) / 2, UI_ATTACK_BTN_Y + 6);
  tft.print(label);
}

void drawAttackButton(bool enabled, const char *label) {
  uint16_t fillColor = enabled ? TFT_RED : TFT_DARKGREY;
  tft.fillRoundRect(UI_ATTACK_BTN_X, UI_ATTACK_BTN_Y, UI_ATTACK_BTN_W, UI_ATTACK_BTN_H, 6, fillColor);
  tft.drawRoundRect(UI_ATTACK_BTN_X, UI_ATTACK_BTN_Y, UI_ATTACK_BTN_W, UI_ATTACK_BTN_H, 6, TFT_WHITE);
  tft.setTextColor(TFT_WHITE, fillColor);
  tft.setTextSize(2);
  int16_t tw = tft.textWidth(label);
  tft.setCursor(UI_ATTACK_BTN_X + (UI_ATTACK_BTN_W - tw) / 2, UI_ATTACK_BTN_Y + 6);
  tft.print(label);
}

static void uiNote(const char *text) {
  tft.fillRect(125, 8, 110, 12, TFT_BLACK);
  tft.setTextColor(TFT_WHITE, TFT_BLACK);
  tft.setTextSize(1);
  tft.setCursor(125, 12);
  tft.print(text);
}
static void uiRefreshAction() {
  drawGrid();
  drawAttackButton(uiActionReady(), uiActionLabel());
}

// ---------------------------------------------------------------------
// POWER MENU
// ---------------------------------------------------------------------

// The powers this team can pick from: its 3 chosen ones, then the Shield. Returns how many.
static int uiMenuItems(uint8_t items[4]) {
  static const uint8_t order[6] = { POWER_SONAR, POWER_SALVO, POWER_MINE, POWER_DOUBLE, POWER_SMOKE, POWER_REPAIR };
  int n = 0;
  for (int i = 0; i < 6; i++) if ((myLoadout & LOADOUT_BIT(order[i])) && n < 3) items[n++] = order[i];
  items[n++] = POWER_SHIELD;
  return n;
}

static bool uiPowerUsable(uint8_t id) {
  return id == POWER_SHIELD || usesLeft[id] > 0;     // the shield is unlimited
}

void uiDrawPowerMenu() {
  uiLastScreen = SCR_MENU;
  tft.fillScreen(TFT_BLACK);
  tft.setTextColor(TFT_GREEN, TFT_BLACK);
  tft.setTextSize(2);
  tft.setCursor(10, 8);
  tft.print("POWERS");
  tft.setTextColor(TFT_WHITE, TFT_BLACK);
  tft.setTextSize(1);
  tft.setCursor(10, 26);
  tft.print("Using a power takes your turn.");

  uint8_t items[4];
  const int n = uiMenuItems(items);
  for (int i = 0; i < n; i++) {
    const int si = specIndex(items[i]);
    const bool usable = uiPowerUsable(items[i]);
    const int y = UI_MENU_Y0 + i * (UI_MENU_H + UI_MENU_GAP);
    const uint16_t fill = usable ? TFT_NAVY : TFT_DARKGREY;
    tft.fillRoundRect(UI_MENU_X, y, UI_MENU_W, UI_MENU_H, 8, fill);
    tft.drawRoundRect(UI_MENU_X, y, UI_MENU_W, UI_MENU_H, 8, TFT_WHITE);
    tft.setTextColor(TFT_WHITE, fill);
    tft.setTextSize(2);
    tft.setCursor(UI_MENU_X + 10, y + 8);
    tft.print(POWER_SPECS[si].name);
    tft.setTextSize(1);
    tft.setCursor(UI_MENU_X + 10, y + 32);
    tft.print(POWER_SPECS[si].hint);

    char buf[12];
    if (items[i] == POWER_SHIELD) {
      tft.setCursor(UI_MENU_X + 150, y + 20);
      tft.print((shieldCooldownTurns > 0) ? "COOLDOWN" : "ALWAYS");
    } else {
      tft.setTextSize(3);
      snprintf(buf, sizeof(buf), "%d", usesLeft[items[i]]);
      tft.setCursor(UI_MENU_X + 192, y + 8);
      tft.print(buf);
      tft.setTextSize(1);
      tft.setCursor(UI_MENU_X + 184, y + 36);
      tft.print("left");
    }
  }
  drawAltButton("BACK");
}

static void uiTouchMenu(int tx, int ty) {
  if (inBox(tx, ty, UI_ALT_BTN_X, UI_ATTACK_BTN_Y, UI_ALT_BTN_W, UI_ATTACK_BTN_H)) { uiDrawYourTurn(); return; }
  uint8_t items[4];
  const int n = uiMenuItems(items);
  for (int i = 0; i < n; i++) {
    const int y = UI_MENU_Y0 + i * (UI_MENU_H + UI_MENU_GAP);
    if (inBox(tx, ty, UI_MENU_X, y, UI_MENU_W, UI_MENU_H)) {
      if (uiPowerUsable(items[i])) uiStartPower(items[i]);
      return;
    }
  }
}

// ---------------------------------------------------------------------
// touch handling on my turn
// ---------------------------------------------------------------------

static void uiSendSelection() {
  bool sent;
  if (uiMode == UIM_STRIKE) {
    sent = attemptManualAttack(uiSelTarget, (uint8_t)uiSelX[0], (uint8_t)uiSelY[0]);
  } else {
    uint8_t xs[3] = {0, 0, 0}, ys[3] = {0, 0, 0};
    for (int i = 0; i < uiSelCount && i < 3; i++) { xs[i] = (uint8_t)uiSelX[i]; ys[i] = (uint8_t)uiSelY[i]; }
    sent = attemptPower(uiPower, uiActionNeedsTarget() ? uiSelTarget : 0, xs, ys);
  }
  uiNote(sent ? "Sent - waiting..." : "Not allowed");
}

void uiPollTouch() {
  uint16_t tx, ty;
  if (!uiReadTouch(&tx, &ty)) return;

  if (millis() < uiSendLockUntil) return;          // a move is already on its way to Central

  if (uiLastScreen == SCR_MENU) { uiTouchMenu(tx, ty); return; }

  // 1. Tab Bar: FLEET (tab 0) + Opponents (tabs 1..numOpponents)
  int totalTabs = 1 + numOpponents;
  for (int i = 0; i < totalTabs; i++) {
    int x, y, w, h;
    getTabBtnRect(i, &x, &y, &w, &h);
    if (inBox(tx, ty, x, y, w, h)) {
      if (i == 0) {
        if (!uiViewingOwnFleet) {
          uiViewingOwnFleet = true;
          drawAllTabs();
          drawGrid();
          if (uiLastScreen == SCR_ACTION) drawAttackButton(uiActionReady(), uiActionLabel());
          else if (uiLastScreen == SCR_WAITING) uiDrawWaiting(uiLastWaitingTeam);
        }
      } else {
        uint8_t target = opponentIds[i - 1];
        if (uiViewingOwnFleet || uiSelTarget != target) {
          uiViewingOwnFleet = false;
          uiSelTarget = target;
          uiSelCount = 0;
          drawAllTabs();
          drawGrid();
          if (uiLastScreen == SCR_ACTION) drawAttackButton(uiActionReady(), uiActionLabel());
          else if (uiLastScreen == SCR_WAITING) uiDrawWaiting(uiLastWaitingTeam);
        }
      }
      return;
    }
  }

  // Active play controls below only respond during this team's turn on SCR_ACTION!
  if (roundState != STATE_RUNNING || currentTurnTeam != MY_TEAM_ID || uiLastScreen != SCR_ACTION) {
    return;
  }

  // 2. Grid cells
  if (uiActionCells() > 0 && inBox(tx, ty, UI_GRID_X, UI_GRID_Y, GRID_SIZE * UI_CELL_SIZE, GRID_SIZE * UI_CELL_SIZE)) {
    int c = (tx - UI_GRID_X) / UI_CELL_SIZE;
    int r = (ty - UI_GRID_Y) / UI_CELL_SIZE;
    if (uiCellAllowed(c, r)) {
      uiToggleCell(c, r);
      uiRefreshAction();
    }
    return;
  }

  // 3. POWERS / BACK
  if (inBox(tx, ty, UI_ALT_BTN_X, UI_ATTACK_BTN_Y, UI_ALT_BTN_W, UI_ATTACK_BTN_H)) {
    uiDrawPowerMenu();
    return;
  }

  // 4. ATTACK / the power's action
  if (inBox(tx, ty, UI_ATTACK_BTN_X, UI_ATTACK_BTN_Y, UI_ATTACK_BTN_W, UI_ATTACK_BTN_H)) {
    if (uiActionReady()) uiSendSelection();
  }
}

// ---------------------------------------------------------------------
// GUIDED SHIP PLACEMENT WIZARD (before loadout selection)
// ---------------------------------------------------------------------

#define WIZ_GRID_X       22
#define WIZ_GRID_Y       56
#define WIZ_CELL_SIZE    28

static bool wizCurrentShipValid(uint8_t shipIdx, int outX[5], int outY[5]) {
  int xs[5], ys[5];
  int n = getShipCells(wizShips[shipIdx], xs, ys);
  if (n != wizShips[shipIdx].ship_len) return false;

  for (int i = 0; i < n; i++) {
    if (xs[i] < 0 || xs[i] >= GRID_SIZE || ys[i] < 0 || ys[i] >= GRID_SIZE) return false;
  }

  for (uint8_t s = 0; s < shipIdx; s++) {
    int prevX[5], prevY[5];
    int pn = getShipCells(wizShips[s], prevX, prevY);
    for (int i = 0; i < n; i++) {
      for (int j = 0; j < pn; j++) {
        if (xs[i] == prevX[j] && ys[i] == prevY[j]) return false;
      }
    }
  }

  if (outX && outY) {
    for (int i = 0; i < n; i++) {
      outX[i] = xs[i];
      outY[i] = ys[i];
    }
  }
  return true;
}

const char *wizGetOrientName(uint8_t shipIdx, uint8_t orient) {
  switch (orient) {
    case ORIENT_HORIZONTAL: return "HORIZONTAL";
    case ORIENT_VERTICAL:   return "VERTICAL";
    case ORIENT_DIAG_DOWN:  return "DIAG DOWN";
    case ORIENT_DIAG_UP:    return "DIAG UP";
    default:                return "UNKNOWN";
  }
}

static void wizRotateCurrentShip() {
  uint8_t &o = wizShips[wizShipIdx].orientation;
  o = (o + 1) % 4;
}

void wizUseDefaultShips() {
  wizDefaultChoice = random(10);
  for (int s = 0; s < NUM_SHIPS; s++) {
    wizShips[s] = DEFAULT_CONFIGS[wizDefaultChoice][s];
  }
  wizReviewMode = true;
  uiDrawShipWizard();
  Serial.printf("[WIZARD] Loaded Default Config #%d for preview. Tap CONFIRM & LOCK to save.\n", wizDefaultChoice + 1);
}

// =====================================================================
// TEAM SELECTION SCREEN (RUNS AT STARTUP BEFORE REGISTRATION)
// =====================================================================

void selectTeam(uint8_t id) {
  if (id < 1 || id > MAX_TEAMS) return;
  myTeamId = id;
  teamSelected = true;
  shipsLocked = false;
  loadoutLocked = false;
  registrationConfirmed = false;
  wizShipIdx = 0;
  ldAttack = 0;
  ldDefence = 0;
  computeOpponentIds();

  // Print both formats so serial monitors and virtual_display.py catch the selection
  Serial.printf("MY_TEAM_ID: %d\n", myTeamId);
  Serial.printf("[TEAM] >>> TEAM %d SELECTED <<<\n", myTeamId);

  // Play a quick buzzer confirmation tone if wired
  if (BUZZER_PIN >= 0) {
    tone(BUZZER_PIN, 1800, 120);
  }

  // Draw brief confirmation banner
  tft.fillScreen(TFT_BLACK);
  tft.drawRect(5, 5, 230, 310, TFT_GREEN);
  tft.drawRect(7, 7, 226, 306, TFT_DARKGREEN);
  char buf[32];
  snprintf(buf, sizeof(buf), "TEAM %d CONFIRMED!", myTeamId);
  uiCenterText(buf, 125, 2, TFT_GREEN, TFT_BLACK);
  uiCenterText("Loading Fleet Wizard...", 160, 1, TFT_WHITE, TFT_BLACK);
  delay(500);

  // Proceed directly to the Ship Placement Wizard
  uiDrawShipWizard();
  Serial.println("[WIZARD] Place your 4 ships on screen (or tap USE DEFAULT, or type SHIPS DEFAULT).");
  lastLoadoutPromptMillis = millis();
}

void uiDrawTeamSelect() {
  uiLastScreen = SCR_TEAM_SELECT;
  uiMyTurnShown = false;
  teamSelected = false;
  shipsLocked = false;
  loadoutLocked = false;
  registrationConfirmed = false;
  wizShipIdx = 0;
  ldAttack = 0;
  ldDefence = 0;
  tft.fillScreen(TFT_BLACK);

  // Decorative border
  tft.drawRect(5, 5, 230, 310, TFT_DARKCYAN);
  tft.drawRect(7, 7, 226, 306, TFT_DARKGREY);

  // Header Title
  uiCenterText("MERAZ BATTLESHIP", 18, 2, TFT_CYAN, TFT_BLACK);
  uiCenterText("SELECT YOUR SQUAD", 42, 2, TFT_YELLOW, TFT_BLACK);
  uiCenterText("Tap your assigned team number:", 68, 1, TFT_WHITE, TFT_BLACK);

  // 4 Team Selection Buttons (Y: 88, 138, 188, 238; H: 42; W: 200)
  // Team 1: Blue
  tft.fillRoundRect(20, 88, 200, 42, 6, TFT_NAVY);
  tft.drawRoundRect(20, 88, 200, 42, 6, TFT_CYAN);
  uiCenterText("TEAM 1", 100, 2, TFT_WHITE, TFT_NAVY);

  // Team 2: Red
  tft.fillRoundRect(20, 138, 200, 42, 6, TFT_MAROON);
  tft.drawRoundRect(20, 138, 200, 42, 6, TFT_RED);
  uiCenterText("TEAM 2", 150, 2, TFT_WHITE, TFT_MAROON);

  // Team 3: Green
  tft.fillRoundRect(20, 188, 200, 42, 6, TFT_DARKGREEN);
  tft.drawRoundRect(20, 188, 200, 42, 6, TFT_GREEN);
  uiCenterText("TEAM 3", 200, 2, TFT_WHITE, TFT_DARKGREEN);

  // Team 4: Orange / Gold
  tft.fillRoundRect(20, 238, 200, 42, 6, TFT_BROWN);
  tft.drawRoundRect(20, 238, 200, 42, 6, TFT_YELLOW);
  uiCenterText("TEAM 4", 250, 2, TFT_WHITE, TFT_BROWN);

  // Footer instruction
  uiCenterText("Or type in Serial: TEAM 1..4", 294, 1, TFT_DARKGREY, TFT_BLACK);
}

void uiPollTeamSelectTouch() {
  uint16_t tx, ty;
  if (!uiReadTouch(&tx, &ty)) return;

  // Check buttons (X: 15 to 225)
  if (inBox(tx, ty, 15, 85, 210, 48)) {
    selectTeam(1);
  } else if (inBox(tx, ty, 15, 135, 210, 48)) {
    selectTeam(2);
  } else if (inBox(tx, ty, 15, 185, 210, 48)) {
    selectTeam(3);
  } else if (inBox(tx, ty, 15, 235, 210, 48)) {
    selectTeam(4);
  }
}

void uiDrawShipWizard() {
  uiLastScreen = SCR_WIZARD;
  uiMyTurnShown = false;
  tft.fillScreen(TFT_BLACK);

  if (wizReviewMode) {
    tft.setTextColor(TFT_YELLOW, TFT_BLACK);
    tft.setTextSize(2);
    tft.setCursor(10, 8);
    char titleBuf[32];
    snprintf(titleBuf, sizeof(titleBuf), "CONFIG #%d / 10", wizDefaultChoice + 1);
    tft.print(titleBuf);

    tft.setTextColor(TFT_WHITE, TFT_BLACK);
    tft.setTextSize(1);
    tft.setCursor(10, 28);
    tft.print("Preview fleet (Lengths 5, 4, 4, 3)");

    tft.setTextColor(TFT_CYAN, TFT_BLACK);
    tft.setCursor(10, 42);
    tft.print("Tap RANDOM to cycle, or LOCK to save");

    for (int r = 0; r < GRID_SIZE; r++) {
      for (int c = 0; c < GRID_SIZE; c++) {
        int px = WIZ_GRID_X + c * WIZ_CELL_SIZE;
        int py = WIZ_GRID_Y + r * WIZ_CELL_SIZE;

        int shipNum = -1;
        for (int s = 0; s < NUM_SHIPS; s++) {
          int xs[5], ys[5];
          int n = getShipCells(wizShips[s], xs, ys);
          for (int i = 0; i < n; i++) {
            if (xs[i] == c && ys[i] == r) { shipNum = s + 1; break; }
          }
          if (shipNum >= 0) break;
        }

        uint16_t fill = (shipNum >= 0) ? TFT_BLUE : TFT_DARKGREY;
        uint16_t border = (shipNum >= 0) ? TFT_CYAN : TFT_DARKGREY;
        tft.fillRect(px, py, WIZ_CELL_SIZE, WIZ_CELL_SIZE, fill);
        tft.drawRect(px, py, WIZ_CELL_SIZE, WIZ_CELL_SIZE, border);

        if (shipNum >= 0) {
          tft.setTextColor(TFT_YELLOW, fill);
          tft.setTextSize(1);
          tft.setCursor(px + 10, py + 10);
          tft.print(shipNum);
        }
      }
    }

    // Row 1 buttons: [RANDOM / NEXT] and [MANUAL PLACE]
    tft.fillRoundRect(10, 258, 105, 26, 6, TFT_DARKCYAN);
    tft.drawRoundRect(10, 258, 105, 26, 6, TFT_WHITE);
    tft.setTextColor(TFT_WHITE, TFT_DARKCYAN);
    tft.setTextSize(1);
    tft.setCursor(18, 267);
    tft.print("RANDOM / NEXT");

    tft.fillRoundRect(125, 258, 105, 26, 6, TFT_DARKGREY);
    tft.drawRoundRect(125, 258, 105, 26, 6, TFT_WHITE);
    tft.setTextColor(TFT_WHITE, TFT_DARKGREY);
    tft.setTextSize(1);
    tft.setCursor(136, 267);
    tft.print("MANUAL PLACE");

    // Row 2 button: [CONFIRM & LOCK]
    tft.fillRoundRect(10, 288, 220, 28, 6, TFT_DARKGREEN);
    tft.drawRoundRect(10, 288, 220, 28, 6, TFT_WHITE);
    tft.setTextColor(TFT_WHITE, TFT_DARKGREEN);
    tft.setTextSize(2);
    int16_t tw = tft.textWidth("CONFIRM & LOCK");
    tft.setCursor(10 + (220 - tw) / 2, 294);
    tft.print("CONFIRM & LOCK");
    return;
  }

  // Header Title
  tft.setTextColor(TFT_YELLOW, TFT_BLACK);
  tft.setTextSize(2);
  tft.setCursor(10, 8);
  char titleBuf[32];
  snprintf(titleBuf, sizeof(titleBuf), "SHIP %d/4 (LEN %d)", wizShipIdx + 1, wizShips[wizShipIdx].ship_len);
  tft.print(titleBuf);

  // Subtitle / Instruction
  tft.setTextColor(TFT_WHITE, TFT_BLACK);
  tft.setTextSize(1);
  tft.setCursor(10, 28);
  tft.print("Tap cell to set start position");

  // Shape Name
  tft.setTextColor(TFT_CYAN, TFT_BLACK);
  tft.setCursor(10, 42);
  tft.print("Orient: ");
  tft.setTextColor(TFT_WHITE, TFT_BLACK);
  tft.print(wizGetOrientName(wizShipIdx, wizShips[wizShipIdx].orientation));

  // Determine which cells are occupied
  int curXs[5], curYs[5];
  bool curValid = wizCurrentShipValid(wizShipIdx, curXs, curYs);
  int curCount = wizShips[wizShipIdx].ship_len;

  // Draw 7x7 grid
  for (int r = 0; r < GRID_SIZE; r++) {
    for (int c = 0; c < GRID_SIZE; c++) {
      int px = WIZ_GRID_X + c * WIZ_CELL_SIZE;
      int py = WIZ_GRID_Y + r * WIZ_CELL_SIZE;

      int lockedShip = -1;
      for (uint8_t s = 0; s < wizShipIdx; s++) {
        int xs[5], ys[5];
        int n = getShipCells(wizShips[s], xs, ys);
        for (int i = 0; i < n; i++) {
          if (xs[i] == c && ys[i] == r) { lockedShip = s + 1; break; }
        }
        if (lockedShip >= 0) break;
      }

      bool isCur = false;
      bool isStart = (c == wizShips[wizShipIdx].start_x && r == wizShips[wizShipIdx].start_y);
      if (curValid) {
        for (int i = 0; i < curCount; i++) {
          if (curXs[i] == c && curYs[i] == r) { isCur = true; break; }
        }
      } else {
        int rawXs[5], rawYs[5];
        int rn = getShipCells(wizShips[wizShipIdx], rawXs, rawYs);
        for (int i = 0; i < rn; i++) {
          if (rawXs[i] == c && rawYs[i] == r) { isCur = true; break; }
        }
      }

      uint16_t fill = TFT_DARKGREY;
      uint16_t border = TFT_DARKGREY;

      if (lockedShip >= 0) {
        fill = TFT_BLUE;
        border = TFT_CYAN;
      } else if (isCur) {
        if (curValid) {
          fill = TFT_DARKGREEN;
          border = TFT_GREEN;
        } else {
          fill = TFT_MAROON;
          border = TFT_RED;
        }
      }

      tft.fillRect(px, py, WIZ_CELL_SIZE, WIZ_CELL_SIZE, fill);
      tft.drawRect(px, py, WIZ_CELL_SIZE, WIZ_CELL_SIZE, border);

      if (lockedShip >= 0) {
        tft.setTextColor(TFT_WHITE, fill);
        tft.setTextSize(1);
        tft.setCursor(px + 10, py + 10);
        tft.print(lockedShip);
      } else if (isStart) {
        tft.setTextColor(TFT_YELLOW, fill);
        tft.setTextSize(1);
        tft.setCursor(px + 10, py + 10);
        tft.print("*");
      }
    }
  }

  // Draw Bottom Action Buttons
  // Row 1: [ROTATE] and [DEFAULT]
  tft.fillRoundRect(10, 258, 105, 26, 6, TFT_NAVY);
  tft.drawRoundRect(10, 258, 105, 26, 6, TFT_WHITE);
  tft.setTextColor(TFT_WHITE, TFT_NAVY);
  tft.setTextSize(1);
  tft.setCursor(20, 267);
  tft.print("ROTATE SHAPE");

  tft.fillRoundRect(125, 258, 105, 26, 6, TFT_DARKCYAN);
  tft.drawRoundRect(125, 258, 105, 26, 6, TFT_WHITE);
  tft.setTextColor(TFT_WHITE, TFT_DARKCYAN);
  tft.setTextSize(1);
  tft.setCursor(136, 267);
  tft.print("USE DEFAULT");

  // Row 2: [PREV] and [CONFIRM / LOCK]
  if (wizShipIdx > 0) {
    tft.fillRoundRect(10, 288, 65, 28, 6, TFT_DARKGREY);
    tft.drawRoundRect(10, 288, 65, 28, 6, TFT_WHITE);
    tft.setTextColor(TFT_WHITE, TFT_DARKGREY);
    tft.setTextSize(1);
    tft.setCursor(24, 298);
    tft.print("PREV");
  }

  const char *confirmLabel = (wizShipIdx == NUM_SHIPS - 1) ? "LOCK ALL SHIPS" : "CONFIRM SHIP";
  uint16_t confirmFill = curValid ? TFT_DARKGREEN : TFT_DARKGREY;
  tft.fillRoundRect(82, 288, 148, 28, 6, confirmFill);
  tft.drawRoundRect(82, 288, 148, 28, 6, TFT_WHITE);
  tft.setTextColor(TFT_WHITE, confirmFill);
  tft.setTextSize(1);
  int16_t tw = tft.textWidth(confirmLabel);
  tft.setCursor(82 + (148 - tw) / 2, 298);
  tft.print(confirmLabel);
}

void uiPollShipWizardTouch() {
  uint16_t tx, ty;
  if (!uiReadTouch(&tx, &ty)) return;

  if (wizReviewMode) {
    // 1. RANDOM / NEXT button (10, 258, 105, 26)
    if (inBox(tx, ty, 10, 258, 105, 26)) {
      wizDefaultChoice = (wizDefaultChoice + 1) % 10;
      for (int s = 0; s < NUM_SHIPS; s++) {
        wizShips[s] = DEFAULT_CONFIGS[wizDefaultChoice][s];
      }
      uiDrawShipWizard();
      return;
    }

    // 2. MANUAL PLACE button (125, 258, 105, 26)
    if (inBox(tx, ty, 125, 258, 105, 26)) {
      wizReviewMode = false;
      wizShipIdx = 0;
      uiDrawShipWizard();
      return;
    }

    // 3. CONFIRM & LOCK FLEET (10, 288, 220, 28)
    if (inBox(tx, ty, 10, 288, 220, 28)) {
      memcpy(myShips, wizShips, sizeof(myShips));
      computeOwnShips();
      shipsLocked = true;
      wizReviewMode = false;
      Serial.printf("[WIZARD] Default 4-ship configuration (#%d) locked in!\n", wizDefaultChoice + 1);
      uiDrawLoadout();
      return;
    }
    return;
  }

  // 1. Grid Tap: select start position
  if (inBox(tx, ty, WIZ_GRID_X, WIZ_GRID_Y, GRID_SIZE * WIZ_CELL_SIZE, GRID_SIZE * WIZ_CELL_SIZE)) {
    uint8_t c = (tx - WIZ_GRID_X) / WIZ_CELL_SIZE;
    uint8_t r = (ty - WIZ_GRID_Y) / WIZ_CELL_SIZE;
    if (c < GRID_SIZE && r < GRID_SIZE) {
      wizShips[wizShipIdx].start_x = c;
      wizShips[wizShipIdx].start_y = r;
      uiDrawShipWizard();
      return;
    }
  }

  // 2. ROTATE button (10, 258, 105, 26)
  if (inBox(tx, ty, 10, 258, 105, 26)) {
    wizRotateCurrentShip();
    uiDrawShipWizard();
    return;
  }

  // 3. USE DEFAULT button (125, 258, 105, 26)
  if (inBox(tx, ty, 125, 258, 105, 26)) {
    wizUseDefaultShips();
    return;
  }

  // 4. PREV button (10, 288, 65, 28)
  if (wizShipIdx > 0 && inBox(tx, ty, 10, 288, 65, 28)) {
    wizShipIdx--;
    uiDrawShipWizard();
    return;
  }

  // 5. CONFIRM button (82, 288, 148, 28)
  if (inBox(tx, ty, 82, 288, 148, 28)) {
    if (wizCurrentShipValid(wizShipIdx, NULL, NULL)) {
      if (wizShipIdx < NUM_SHIPS - 1) {
        wizShipIdx++;
        uiDrawShipWizard();
      } else {
        if (localValidateShips(wizShips)) {
          memcpy(myShips, wizShips, sizeof(myShips));
          computeOwnShips();
          shipsLocked = true;
          Serial.println("[WIZARD] All 4 ships configured & locked in!");
          uiDrawLoadout();
        } else {
          Serial.println("[WIZARD] Ship validation failed. Check placement.");
        }
      }
    }
    return;
  }
}

// ---------------------------------------------------------------------
// LOADOUT screen (before registering)
// ---------------------------------------------------------------------

static bool ldValid() {
  return loadoutBitCount(ldAttack) == 2 && ldDefence != 0;
}

static void drawLoadoutButton(int x, int y, uint8_t id, bool selected) {
  const int si = specIndex(id);
  const uint16_t fill = selected ? TFT_YELLOW : TFT_NAVY;
  const uint16_t text = selected ? TFT_BLACK : TFT_WHITE;
  tft.fillRoundRect(x, y, LD_BTN_W, LD_BTN_H, 6, fill);
  tft.drawRoundRect(x, y, LD_BTN_W, LD_BTN_H, 6, TFT_WHITE);
  tft.setTextColor(text, fill);
  tft.setTextSize(2);
  tft.setCursor(x + 8, y + 6);
  tft.print(POWER_SPECS[si].name);
  tft.setTextSize(1);
  tft.setCursor(x + 8, y + 26);
  tft.print(POWER_SPECS[si].hint);
}

static void drawLoadoutConfirm() {
  uint16_t fill;
  const char *label;
  if (!ldValid())    { fill = TFT_DARKGREY;  label = "PICK 2 + 1"; }
  else if (ldArmed)  { fill = TFT_RED;       label = "TAP AGAIN"; }
  else               { fill = TFT_DARKGREEN; label = "LOCK IN"; }
  tft.fillRoundRect(LD_OK_X, LD_OK_Y, LD_OK_W, LD_OK_H, 8, fill);
  tft.drawRoundRect(LD_OK_X, LD_OK_Y, LD_OK_W, LD_OK_H, 8, TFT_WHITE);
  tft.setTextColor(TFT_WHITE, fill);
  tft.setTextSize(2);
  int16_t tw = tft.textWidth(label);
  tft.setCursor(LD_OK_X + (LD_OK_W - tw) / 2, LD_OK_Y + 12);
  tft.print(label);
}

static void drawLoadoutStatus() {
  char buf[40];
  snprintf(buf, sizeof(buf), "Picked: %d/2 attack, %d/1 defence   ", loadoutBitCount(ldAttack), ldDefence ? 1 : 0);
  tft.setTextColor(TFT_WHITE, TFT_BLACK);
  tft.setTextSize(1);
  tft.setCursor(8, 214);
  tft.print(buf);
}

void uiDrawLoadout() {
  uiLastScreen = SCR_LOADOUT;
  uiMyTurnShown = false;
  tft.fillScreen(TFT_BLACK);
  tft.setTextColor(TFT_GREEN, TFT_BLACK);
  tft.setTextSize(2);
  tft.setCursor(10, 8);
  tft.print("CHOOSE POWERS");
  tft.setTextColor(TFT_WHITE, TFT_BLACK);
  tft.setTextSize(1);
  tft.setCursor(8, 30);
  tft.print("Always yours: STRIKE + SHIELD");
  tft.setTextColor(TFT_CYAN, TFT_BLACK);
  tft.setCursor(8, 44);
  tft.print("ATTACK - pick 2 (2 uses each)");
  for (int i = 0; i < 4; i++) {
    int x = (i % 2) ? LD_COL2_X : LD_COL1_X;
    int y = (i / 2) ? LD_ATK_Y2 : LD_ATK_Y1;
    drawLoadoutButton(x, y, LD_ATTACK_IDS[i], (ldAttack & (1u << LD_ATTACK_IDS[i])) != 0);
  }
  tft.setTextColor(TFT_CYAN, TFT_BLACK);
  tft.setTextSize(1);
  tft.setCursor(8, 146);
  tft.print("DEFENCE - pick 1 (2 uses each)");
  for (int j = 0; j < 2; j++) {
    drawLoadoutButton(j ? LD_COL2_X : LD_COL1_X, LD_DEF_Y, LD_DEFENCE_IDS[j], ldDefence == LD_DEFENCE_IDS[j]);
  }
  drawLoadoutStatus();
  tft.setTextColor(TFT_WHITE, TFT_BLACK);
  tft.setTextSize(1);
  tft.setCursor(8, 232);
  tft.print("Locked in = no changes this match.");
  drawLoadoutConfirm();
}

void uiPollLoadoutTouch() {
  if (ldArmed && millis() > ldArmedUntil) { ldArmed = false; drawLoadoutConfirm(); }

  uint16_t tx, ty;
  if (!uiReadTouch(&tx, &ty)) return;

  for (int i = 0; i < 4; i++) {
    int x = (i % 2) ? LD_COL2_X : LD_COL1_X;
    int y = (i / 2) ? LD_ATK_Y2 : LD_ATK_Y1;
    if (!inBox(tx, ty, x, y, LD_BTN_W, LD_BTN_H)) continue;
    const uint8_t bit = (uint8_t)(1u << LD_ATTACK_IDS[i]);
    if (ldAttack & bit) ldAttack &= (uint8_t)~bit;                       // un-pick
    else if (loadoutBitCount(ldAttack) < 2) ldAttack |= bit;             // pick (max 2)
    ldArmed = false;
    uiDrawLoadout();
    return;
  }
  for (int j = 0; j < 2; j++) {
    int x = j ? LD_COL2_X : LD_COL1_X;
    if (!inBox(tx, ty, x, LD_DEF_Y, LD_BTN_W, LD_BTN_H)) continue;
    ldDefence = (ldDefence == LD_DEFENCE_IDS[j]) ? 0 : LD_DEFENCE_IDS[j];   // pick one, tap again to clear
    ldArmed = false;
    uiDrawLoadout();
    return;
  }
  if (inBox(tx, ty, LD_OK_X, LD_OK_Y, LD_OK_W, LD_OK_H) && ldValid()) {
    if (!ldArmed) {
      ldArmed = true;
      ldArmedUntil = millis() + 3000;
      drawLoadoutConfirm();
    } else {
      uint8_t loadout = LOADOUT_SHIELD_BIT;
      for (int i = 0; i < 4; i++) if (ldAttack & (1u << LD_ATTACK_IDS[i])) loadout |= LOADOUT_BIT(LD_ATTACK_IDS[i]);
      loadout |= LOADOUT_BIT(ldDefence);
      ldArmed = false;
      lockLoadout(loadout);
    }
  }
}
