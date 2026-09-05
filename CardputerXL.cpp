// ============================================================================
// CARDPUTER XL - a single-file "OS"/app launcher for the M5Stack Cardputer-ADV
// ============================================================================
//
// This one .cpp is the entire firmware: ~30 built-in apps (system info,
// Wi-Fi tools, notes, a calculator, a tiny scripting environment called
// "C LAB", games, a music/drum lab, QR tools, a lock screen, and more) all
// living in one Arduino sketch instead of separate files, so the whole OS
// can be read, searched, and edited as a single document.
//
// HARDWARE
//   Everything this file draws - every app, the launcher, the lock screen -
//   renders on a single EXTERNAL Adafruit_ILI9341 320x240 panel (the global
//   `tft` object below), wired to the Cardputer-ADV's EXT header on its own
//   SPI bus. The Cardputer's own built-in screen is only used for backlight
//   sleep/wake, never for app content - see the TFT_* pin #defines just
//   below for the wiring. A GPIO-driven haptic motor (HAPTIC_IN_PIN) and the
//   ADV's addressable RGB LED (RGB_LED_PIN) round out the extra hardware.
//
// RENDERING MODEL - read this before touching any draw code
//   There is no framebuffer/double-buffer for the main UI (one exception:
//   the Kart Racer game's live 3D view uses an off-screen `kartCanvas`,
//   since blitting one finished frame is far less flickery than painting
//   dozens of primitives straight to the panel over SPI - see the comment
//   above its declaration). Everywhere else, drawing happens immediate-mode
//   directly onto `tft`, gated by a simple dirty-flag + dispatch pattern:
//     - `Page page` selects the current app (see `enum Page` below); each
//       page has a `drawX()` function that repaints its whole screen
//       (header + content + footer) and is called from `draw()`.
//     - Setting `redrawNeeded = true` asks loop() to repaint. If `page`
//       just changed (or `forceFullRedraw` is set - used by the quick-menu
//       overlay below), it calls the full `draw()`; otherwise it calls
//       `refreshLocalPage()`, which repaints only the part of the current
//       page that actually changed (e.g. one settings row, one grid cell) -
//       this "local refresh" is what keeps interactive pages from visibly
//       flashing on every keypress. Search for `lastDrawnPage` to see the
//       page-change detection, and GAMEHUB's `lastDrawnGameMode` for the
//       same idea applied one level down, to its own SNAKE/GRID HUNT/KART
//       RACER sub-modes.
//     - A few continuously-animating things (Snake's movement tick, the Kart
//       Racer's live view, the drum machine's playhead) draw themselves
//       directly instead of going through redrawNeeded at all, since they
//       need to update faster than "once per user action" - see the
//       comments on `stepSnakeGame()` and `stepKartRace()`.
//   The ILI9341 is write-only (no readback), so nothing can be "erased" in
//   place - every overlay (toasts, the quick-launch menu below) either
//   stays inside one small fixed rectangle it always fully repaints, or,
//   like the quick menu, forces a full page repaint once it closes.
//
// INPUT
//   `keyboard()` is the single input dispatch, called once per loop(). It
//   reads M5Cardputer.Keyboard.keysState() into a `k` KeyEvent-like struct
//   and fans out on `page` (and, for GAMEHUB, `gameMode`) to decide what a
//   keystroke means. A few modifier keys are handled globally, before any
//   page gets a look at the input: Fn is universal "back/cancel", Ctrl+F
//   toggles the current app as a favourite, and Opt toggles the floating
//   quick-launch overlay (see "---- Floating quick-launch overlay ----"
//   below) on top of whatever page is currently showing.
//
// PERSISTENCE
//   Settings and small bits of app state (Home tile assignments, theme,
//   etc.) are saved to flash via the ESP32 `Preferences` (NVS) API, not a
//   filesystem - `markStateDirty()` + the debounced `savePersistentState()`
//   call in loop() avoid wearing the flash out on every keystroke. Kart
//   Racer's best-lap times use the same mechanism, in their own namespace.
//
// BUILDING
//   Board: FQBN esp32:esp32:m5stack_cardputer. This sketch does not fit in
//   the board's default 1.2MB app partition - build with the "Huge APP (3MB
//   No OTA/1MB SPIFFS)" partition scheme (Arduino IDE: Tools > Partition
//   Scheme; arduino-cli: --fqbn esp32:esp32:m5stack_cardputer:PartitionScheme=huge_app).
//   Besides the usual M5Stack/Adafruit libraries, this needs the "QRCode"
//   library (its qrcode.c/.h are vendored directly next to this file - the
//   ESP32 core ships its own unrelated internal qrcode.h under the same
//   name, which shadows the real library on the default include path) and
//   the HijelHID_BLEKeyboard + NimBLE-Arduino libraries for the Bluetooth
//   keyboard app.
// ============================================================================

#include <Arduino.h>
#include <M5Cardputer.h>
#include <SPI.h>
#include <WiFi.h>
#include <ESPmDNS.h>
#include <WebServer.h>
#include <HTTPClient.h>
#include <WiFiClientSecure.h>
#include <Preferences.h>
#include <Adafruit_GFX.h>
#include <Adafruit_ILI9341.h>
#include "qrcode.h"
#include <mbedtls/md.h>
#include <mbedtls/aes.h>
#include <esp_system.h>
#include <time.h>
#include <HijelHID_BLEKeyboard.h>
#include "cardc_documentation.h"
#include "lock_wallpaper.h"

#define TFT_CS 5
// A dedicated reset wire makes the external ILI9341 initialise reliably.
#define TFT_RST 15
#define TFT_DC 13
#define TFT_MOSI 3
#define TFT_SCK 6
#define TFT_BL 39
// Ready-made motor driver: GPIO1 controls only its IN input. The motor is
// switched by the module itself, so this is digital ON/OFF, never PWM.
#define HAPTIC_IN_PIN 1
// Cardputer ADV built-in RGB LED: GPIO38 supplies LED power and GPIO21 is
// the addressable RGB data line. GPIO38 must be high before any LED update.
#define RGB_LED_PIN 21
#define RGB_LED_POWER_PIN 38

// External ILI9341 backlight uses the user-wired TFT_BL line.  Keep it on a
// separate PWM channel so its brightness follows the built-in Cardputer panel.

struct Theme { uint16_t bg, panel, accent, text, dim, selected; };

enum Page { LAUNCHER, SYSTEM, WIFI, NOTES, CLOCK, CALC, CLAB, CARDCREPL, QRTEXT, SETTINGS, TEXTTOOLS, FAVOURITES, WIFIMONITOR, FILEBROWSER, WEBCOMPANION, HOMEEDITOR, CLABEXAMPLES, DASHBOARD, DICERANDOM, DEVICECHECK, QRTOOLSPLUS, MINIPAINT, LAUNCHERSEARCH, TEXTBROWSER, INPOSTTRACK, ZABKATOTP, MUSICLAB, MIC, BLEKEYBOARD, GAMEHUB, WIFISETUP, LOCKSCREEN, SCREENSAVER };

struct CVar { String name; long value; };

struct CStringVar { String name; String value; };

struct CFunction { String name; String body; };


// Forward declarations

// Forward declarations

// Forward declarations

// Forward declarations

// Forward declarations

// Forward declarations

// Forward declarations

// Forward declarations

// Forward declarations

// Forward declarations

// Forward declarations

// Forward declarations

// Forward declarations

// Forward declarations

// Forward declarations
bool chargingInferredFromBatteryRise(int battery);

void startSnakeGame();
void stepSnakeGame();

void captureCLabUserApp();

void paintToast();

void serviceLowBatteryAlert();
void queueToast(const String& text);
void serviceToasts();
void serviceConnectionToasts();

void drawScreensaverIcosahedron();

void drawBleKeyboard();

String lockDateText();

uint16_t cCanvasColor(uint8_t color);

void cBeep(long frequencyHz, long durationMs);
void drawCardCCanvas();
void cCanvasBegin();
void cCanvasClear(uint8_t color);
void cCanvasRect(long x, long y, long w, long h, uint8_t color, bool filled);
void cCanvasCircle(long x, long y, long radius, uint8_t color, bool filled);
void cCanvasLine(long x1, long y1, long x2, long y2, uint8_t color);
void cCanvasPixel(long x, long y, uint8_t color);
void cCanvasPause(long durationMs);

void triggerDrum(uint8_t track);
void playDrumStep(uint8_t step);
void drawMusicCell(uint8_t track, uint8_t step);

String hexEncode(const uint8_t* bytes, size_t length);
bool hexDecode(const String& text, uint8_t* bytes, size_t length);
void vaultKey(const String& passphrase, const uint8_t salt[16], uint8_t key[32]);
bool storeZabkaVault(const String& secret, const String& passphrase);
bool unlockZabkaVault(const String& passphrase);
void lockZabkaVault();

bool searchMatches(int app);
int searchResultCount();
int searchResultApp(int result);
String browserPlainText(String source);

void irMark(uint16_t usec);
void irSpace(uint16_t usec);
void sendNec(uint8_t address, uint8_t command);

// Ready-made driver on G1: digital ON/OFF only, never PWM.
void vibrate(uint16_t durationMs);
void playBootSound();
void playWifiConnectedSound();
void playBluetoothConnectedSound();
void playLowBatterySound();
void playChargingStartedSound();
void serviceStatusSounds();

constexpr uint8_t TFT_BL_PWM_CHANNEL = 7;
constexpr uint16_t TFT_BL_PWM_HZ = 5000;
constexpr uint8_t TFT_BL_PWM_BITS = 8;








// Forward declarations

// Forward declarations

// Forward declarations

// Forward declarations

// Forward declarations

// Forward declarations
String base64Encode(const String& input);
void drawFavourites();
void drawWifiMonitor();
void drawFileBrowser();
void drawWebCompanion();
void drawHomeEditor();
void drawCLabExamples();
void loadCLabExample(int example);
void drawDashboard();
void drawDiceRandom();
void drawGameHub();
void drawGameMenuList();
void drawGridHuntGrid();
void drawKartHub();
void startKartRace(int level);
void stepKartRace();
void drawDeviceCheck();
void drawQRToolsPlus();
void drawMiniPaint();
void drawLauncherSearch();
void drawTextBrowser();
void drawInPostTrack();
void drawZabkaTotp();
void drawMusicLab();
void drawMic();
void updateMicMonitor();
void playMusicNote(int semitone);
String zabkaTotpCode(const String& hexSecret, uint64_t counter);
void rollDiceRandom();
void fetchTextBrowserPage();
void fetchInPostTracking();
const char* homeTileLabel(int tile);
const char* homeTileIcon(int tile);

void loadCLabDemo();

bool cBuiltinNumber(const String& name, long& value);
bool cBuiltinText(const String& name, String& value);
bool isCBuiltinName(const String& name);
void cWifiScan();

int findCStringVar(const String& name);

void applyVolume();

void markStateDirty();
void loadPersistentState();
void savePersistentState();
void applyTheme();
void playMenuSound();
void playTypingSound();
void playBackspaceSound();
void playExitSound();
void playCursorSound();
void playFunctionSound();
void playEnterSound();
void playTabSound();
void setBacklight(bool on);
void applyBacklight();
void updateStatusLed();
void setCardCLed(uint8_t red, uint8_t green, uint8_t blue);
void playStartupRainbow();
void lockDevice();
bool checkLockPin(const String& pin);
String lockPinHashFor(const String& pin);
bool syncNetworkTime();
String lockClockText();
void drawLockScreen();
void drawScreensaver();
void startScreensaver();
void updateScreensaver();
void drawBootScreen();
void drawWelcomeScreen();
void drawLinuxBoot();
void header(const char* title);
void drawHeaderStatus();
void footer(const char* text);
void updateLauncherIndicators();
void drawHomeTile(int tile, bool selected);
void startHomeFocusAnimation(int oldTile, int newTile);
void drawQuickMenuTile(int tile, bool selected);
void drawQuickMenuCaption();
void drawQuickMenu();
void openQuickMenuAnimated();
void closeQuickMenuAnimated();
void updateHomeFocusAnimation();
void drawLauncherFocusRail(int y);
void startLauncherFocusAnimation(int oldSelected, int oldScroll);
void updateLauncherFocusAnimation();
void updateLauncherSelection(int oldSelected, int oldScroll);
void updateSystemValues();
void drawSystem();
const char* enc(wifi_auth_mode_t e);
void drawWifi();
void drawWifiSetup();
void drawNotes();
void updateNotesLine();
void updateClockValue();
void drawClock();
void updateCalcPanel();
void drawQRTextField();
void drawQRModules();
void drawQRText();
long cAtom(String token, bool& ok);
long cExpr(String expression, bool& ok);
bool cCondition(String condition, bool& ok);
String cTextExpr(String expression, bool& ok);
void cLog(const String& line);
void updateBuiltinDisplay(bool force = false);
int noteCharacterCount();

void drawLauncherRow(int row); void updateLauncherIndicators(); void updateLauncherSelection(int oldSelected, int oldScroll);
void drawSystemValue(int rowIndex, const String& value, uint16_t color); void updateSystemValues();
void handleCLabCursorKeys(); void loadPersistentState(); void savePersistentState(); void markStateDirty();
void syncLauncherScroll(); void moveCLabCursor(int direction);
int findCVar(const String& name); long cAtom(String token, bool& ok); long cExpr(String expression, bool& ok); void cLog(const String& line);
String uptime(); void applyTheme(); void setBacklight(bool on); void header(const char* title); void footer(const char* text);
void drawLauncher(); void drawSystem(); const char* enc(wifi_auth_mode_t e); void drawWifi(); void drawNotes(); void drawClock();
void drawTextTools(); void drawFavourites(); void drawWifiMonitor(); void drawFileBrowser(); void drawWebCompanion(); void drawHomeEditor(); void startWebCompanion(); void applyWebInput(); void applyWebAction(const String& action);
void drawCalc(); void updateClockValue(); void updateCalcPanel(); void updateNotesLine(); void updateSettingsRow(int index);
void drawCLab(); void drawCLabQR(); void drawCardCRepl(); void drawQRText(); void drawQRTextField(); void drawQRModules(); void drawCLabEditor(); void drawCLabCodeLine(int lineIndex); void drawCLabExamples(); void loadCLabExample(int example); void drawCLabExplorer(); void drawCLabSaveDialog(); void drawCLabSlotDialog(); void drawCLabNameDialog(); void openNewCLabFile(); void saveCLabFile(); void openCLabUserApp(int slot, bool runNow); void captureCLabUserApp();
void runCLab(); void runCardCRepl(); void loadCLabDemo(); String settingValue(int i); void drawSettings(); void draw(); void startScan(); void checkScan(); void calcResult();
void changeSetting(int d); void keyboard(); void clabBackspace(); void playMenuSound(); void playTypingSound(); void playBackspaceSound();

Adafruit_ILI9341 tft(&SPI, TFT_DC, TFT_CS, TFT_RST);
// BLE HID is advertised after boot. It only sends keys while the dedicated
// BLE KEYBOARD app is open, so launcher and application controls stay local.
HijelHID_BLEKeyboard bleKeyboard;
Preferences preferences;
bool stateDirty = false;
unsigned long stateChangedAt = 0;
constexpr int W = 320, H = 240, HEADER_H = 22, FOOTER_H = 16, CONTENT_Y = 27;

constexpr int THEME_COUNT = 7;
const Theme themes[THEME_COUNT] = {
  // background, panel, accent, text, dim, selected
  {ILI9341_BLACK, ILI9341_DARKCYAN, ILI9341_CYAN, ILI9341_WHITE, ILI9341_LIGHTGREY, ILI9341_BLUE},
  {ILI9341_BLACK, 0x4200, ILI9341_YELLOW, 0xFFE0, 0xBDF7, 0x7BE0},
  {0x0010, 0x001F, 0x07FF, ILI9341_WHITE, 0xBDF7, 0x401F},
  {0x1008, 0x400F, 0xF81F, 0xFFE0, 0xC618, 0x801F},
  {0x0200, 0x03E0, 0x07E0, ILI9341_WHITE, 0xBDF7, 0x05A0},
  {0x2104, 0x4208, 0xFD20, 0xFFFF, 0xC618, 0xA145},
  // Maximum legibility: black background, white text and yellow controls.
  {ILI9341_BLACK, ILI9341_BLACK, ILI9341_YELLOW, ILI9341_WHITE, ILI9341_LIGHTGREY, ILI9341_NAVY}
};
const char* themeNames[THEME_COUNT] = {"CYAN", "AMBER", "BLUE", "MAGENTA", "MATRIX", "SUNSET", "HIGH CONTRAST"};
int themeIndex = 0; Theme ui = themes[0];
uint8_t displayRotation = 3;
bool backlightOn = true, sleeping = false, redrawNeeded = true, fnLast = false;
// Floating quick-launch overlay (see drawQuickMenu()): Opt toggles it open
// on top of whatever page is currently showing, from anywhere in the OS.
// optLast mirrors fnLast's edge-detection trick above. forceFullRedraw asks
// the loop() dispatch for a full draw() even though `page` itself didn't
// change - needed because the overlay can only be removed by fully
// repainting whatever was underneath it (the panel is write-only, so there
// is no way to "erase" just the pixels the overlay covered).
bool quickMenuOpen = false, optLast = false, forceFullRedraw = false;
int quickMenuSelected = 0;
// A CardC led(r,g,b) command owns the normal unlocked colour until a lock
// transition occurs. Physical safety/status states always take precedence.
bool cardcLedOverride = false;
uint8_t cardcLedRed = 0, cardcLedGreen = 255, cardcLedBlue = 0;
// Saved Settings switch for the built-in RGB LED. When disabled it overrides
// every normal status colour, CardC led() command and startup rainbow effect.
bool statusLedEnabled = true;
// Critical battery warning always overrides the user LED switch and every
// normal status/CardC colour. Its dark phase makes the red warning blink.
bool lowBatteryAlertOn = false;
bool lowBatteryAlertWasActive = false;
int lastLowBatteryLevel = -999;
unsigned long lastLowBatteryAlertToggleAt = 0;
constexpr unsigned long LOW_BATTERY_ALERT_INTERVAL_MS = 350UL;
// Screensaver dimming is a temporary fixed output level. It never overwrites
// the user's saved Brightness setting and BtnA remains the on/off control.
bool screensaverDimmed = false;
// The wallpaper is transferred once whenever the lock scene opens. Subsequent
// PIN, clock and battery changes repaint only their opaque panel rectangles.
bool lockScreenBaseDrawn = false;
uint8_t volumeLevel = 10;  // 0..10, persisted; 0 is silent.
uint8_t brightnessLevel = 10; // 1..10, persisted; PWM duty for the ILI backlight.
// The PIN itself is never retained. Only its SHA-256 verification hash is
// stored in Preferences. Default PIN for a fresh device: 1234.
String lockPinHash = "03ac674216f3e15c761ee1a5e255f067953623c8b388b4459e13f978d7c846f4";
String lockPinInput = "";
bool pinChangeActive = false;
bool pinChangeConfirm = false;
String pinChangeFirst = "";
String pinChangeInput = "";
String pinChangeStatus = "Choose a new 4-digit PIN";
uint8_t lockFailures = 0;
unsigned long lockRetryAt = 0;
// BtnA is the physical boot button on the Cardputer ADV enclosure.
// It is active-low on GPIO0.
constexpr uint8_t BTNA_PIN = 0;
bool btnAWasDown = false;
bool clabDeleteHeld = false;
unsigned long clabDeleteRepeatAt = 0;
const uint16_t sleepValues[] = {0, 15, 30, 60, 120};
// Time spent on the dim screensaver before the PIN gate appears. Zero means
// the screensaver remains visible until the user returns manually.
const uint16_t lockAfterScreensaverValues[] = {0, 15, 30, 60, 120};
int sleepIndex = 0, lockAfterScreensaverIndex = 3;
unsigned long lastActivity = 0;
// The right side of the external-display header is refreshed independently.
// Its signature contains only values that are actually visible there, so a
// clock tick, battery change or radio-state transition never needs a full-page redraw.
String lastHeaderStatusSignature = "";
unsigned long lastHeaderStatusPollAt = 0;

// The ILI9341 driver is write-only: it cannot read back pixels from the panel.
// Toasts are therefore drawn as a small overlay and, when they finish, the
// active page redraws through its normal local-refresh path.
constexpr int TOAST_H = 42;
constexpr int TOAST_Y = H - FOOTER_H - TOAST_H;
constexpr unsigned long TOAST_SLIDE_MS = 180UL;
constexpr unsigned long TOAST_HOLD_MS = 2300UL;
bool toastActive = false, toastNeedsPaint = false;
String toastText = "", queuedToastText = "";
unsigned long toastStartedAt = 0;
bool toastWifiKnown = false, toastWifiWasConnected = false;
bool toastBtKnown = false, toastBtWasPaired = false;
bool toastBatteryKnown = false, toastBatteryWasLow = false;
// Each status tone is edge-triggered, so it plays once when the physical
// state begins rather than repeating while that state remains active.
bool soundWifiKnown = false, soundWifiWasConnected = false;
bool soundBtKnown = false, soundBtWasPaired = false;
bool soundBatteryKnown = false, soundBatteryWasLow = false;
bool soundChargeKnown = false, soundWasCharging = false;
// Some ADV battery reports jump upward when USB power is attached before the
// charging-state flag settles. Keep one short baseline so that jump can act as
// a fallback charging edge, without treating ordinary slow percentage drift as
// a charger event.
int chargeBaselineBattery = -1;
unsigned long chargeBaselineAt = 0;
unsigned long inferredChargingUntil = 0;
// Set for one service cycle when the live battery report makes the characteristic
// USB-insertion jump. It is an event, not a persistent charging state.
bool chargeRiseJustDetected = false;
// USB insertion on this Cardputer can make the reported level jump by about
// 10–20 percentage points. Require a substantial jump in a short window so
// normal one-percent measurement wobble is never treated as charging.
constexpr int CHARGE_RISE_TRIGGER_PERCENT = 8;
constexpr unsigned long CHARGE_RISE_WINDOW_MS = 90000UL;
constexpr unsigned long INFERRED_CHARGING_HOLD_MS = 120000UL;

Page page = LAUNCHER;
// Tracks the fully painted scene.  Input inside the same scene is refreshed
// locally; a real page change is still allowed one complete scene paint.
Page lastDrawnPage = LAUNCHER;
// "The app you were in before this one" - updated in draw() whenever a real
// navigation happens, skipping system transitions (lock/screensaver/the
// Wi-Fi setup sub-flow) that aren't something worth "resuming". Feeds the
// quick-launch overlay's 7th tile (see "---- Floating quick-launch overlay").
Page previousPage = LAUNCHER;
Page screensaverReturnPage = LAUNCHER;
unsigned long screensaverStartedAt = 0;
unsigned long screensaverLastFrameAt = 0;
const char* appNames[] = {"SYSTEM", "WI-FI SCAN", "NOTES", "CLOCK", "CALCULATOR", "C LAB", "CARDC REPL", "QR TEXT", "SETTINGS", "TEXT TOOLS", "FAVOURITES", "WI-FI MONITOR", "FILE BROWSER", "WEB COMPANION", "HOME MENU", "C LAB EXAMPLES", "DASHBOARD", "DICE & RANDOM", "DEVICE CHECK", "QR TOOLS +", "MINI PAINT", "LAUNCHER SEARCH", "TEXT BROWSER", "INPOST TRACK", "ZABKA TOTP", "MUSIC LAB", "MIC", "BLE KEYBOARD", "GAMES"};
const char* appInfo[] = {"battery, memory, uptime", "nearby networks", "quick text scratchpad", "local uptime clock", "basic arithmetic", "tiny C-style interpreter", "one-line CardC console", "encode text as a QR", "theme and display options", "text counters and transforms", "pinned launcher apps", "signal and channel summary", "saved local note documents", "phone control and C LAB input", "add, move or remove home tiles", "load ready-to-run CardC projects", "live device overview", "dice, coin and number picker", "screen, speaker and key checks", "QR presets and local link", "16 by 12 pixel sketchpad", "find an app by name", "simple HTTP text reader", "track a parcel by number", "SRLN loyalty QR with 6-digit code", "16-step drum sequencer", "live microphone level and waveform", "pair and type to a Bluetooth host", "Snake and Grid Hunt"};
constexpr int APP_COUNT = 29;
constexpr int APP_VISIBLE = 5;
// appNames[] is indexed by (Page - 1) and only covers pages up to GAMEHUB -
// WIFISETUP/LOCKSCREEN/SCREENSAVER aren't "apps" and have no entry, so a raw
// appNames[(int)p - 1] lookup on an arbitrary Page is not always safe. This
// wraps that lookup for previousPage/quick-menu display purposes.
const char* pageDisplayName(Page p) {
  if (p == LAUNCHER) return "HOME";
  int index = (int)p - 1;
  return (index >= 0 && index < APP_COUNT) ? appNames[index] : "APPS";
}
// The first launcher page is a compact 2x3 dashboard. APPS opens the
// secondary list, which deliberately excludes the five pinned home apps.
constexpr int HOME_TILE_COUNT = 6;
// Five configurable Home slots hold an app index or -1 for an empty slot.
// Tile 6 is deliberately permanent: it is the recovery path to the full APPS list.
int homeAppIndices[5] = {5, 2, 6, 4, 8}; // C LAB, Notes, REPL, Calc, Settings
const char* homeTileIcons[HOME_TILE_COUNT] = {"{}", "[]", ">_", "+-", "*", "::"};
// Entry 0 is a navigation action; the remaining entries open secondary apps.
const int secondaryAppIndices[24] = {0, 1, 3, 7, 9, 10, 11, 12, 13, 14, 15, 16, 17, 18, 19, 20, 21, 22, 23, 24, 25, 26, 27, 28};
constexpr int SECONDARY_APP_COUNT = 25;
bool launcherHome = true;
int homeSelected = 0;
int appSelected = 0, appScroll = 0;
int homeEditorSlot = 0;
bool homeEditorPicking = false;
int cExampleSelected = 0;
constexpr int C_EXAMPLE_COUNT = 7;
int homeEditorApp = 0;
// Launcher movement is a small non-blocking visual cue. Only the narrow rail
// beside the cards is repainted, at roughly 20 FPS, so normal navigation does
// not trigger a whole-screen refresh.
constexpr uint16_t LAUNCHER_ANIMATION_MS = 180;
constexpr uint16_t LAUNCHER_ANIMATION_FRAME_MS = 50;
// Home tiles fit completely above the footer: 3 rows of 54 px plus 6 px gaps.
constexpr int HOME_TILE_W = 142, HOME_TILE_H = 54, HOME_TILE_GAP_X = 12, HOME_TILE_GAP_Y = 6;
constexpr int HOME_TILE_LEFT = 12, HOME_TILE_TOP = 30;
bool homeFocusAnimating = false;
int homeFocusFromTile = 0, homeFocusToTile = 0;
unsigned long homeFocusStartedAt = 0;
unsigned long homeFocusLastFrameAt = 0;
bool launcherFocusAnimating = false;
int launcherFocusFromY = CONTENT_Y;
int launcherFocusToY = CONTENT_Y;
unsigned long launcherFocusStartedAt = 0;
unsigned long launcherFocusLastFrameAt = 0;

String notes[15]; int noteLine = 0, noteCount = 1;
bool scanRunning = false, scanDone = false; int networkCount = 0; unsigned long scanStarted = 0;
String toolText = "";
int toolMode = 0;
const char* toolModeNames[] = {"COUNT", "UPPER", "LOWER", "BASE64"};
bool appFavourite[APP_COUNT] = {false}; int favouriteSelected = 0;
String localFiles[3] = {"", "", ""};
const char* localFileNames[3] = {"NOTE A", "NOTE B", "NOTE C"};
int fileSelected = 0;
WebServer webServer(80);
bool webRunning = false;
bool mdnsRunning = false;
const char* WEB_MDNS_HOST = "cardputer-xl";
// Initial STA credentials are imported into Preferences once, then Web
// Companion reconnects using the device-stored values after later boots.
const char* INITIAL_WIFI_SSID = "California house";
const char* INITIAL_WIFI_PASSWORD = "2k20vVTS";
String webStatus = "Press ENTER to connect to Wi-Fi";
// Wi-Fi setup uses a scanned SSID and a RAM-only password entry field. The
// chosen credentials are persisted only after the user presses ENTER to connect.
int wifiSetupSelected = 0;
bool wifiSetupEditingPassword = false;
String wifiSetupSsid = "";
String wifiSetupPassword = "";
bool bleHidEnabled = true;
// Background STA reconnect state. It is independent of Web Companion: Wi-Fi
// can reconnect after boot even when the local web server remains stopped.
bool autoWifiConnecting = false;
// Edge detector for a connection established by the background auto-connect.
// It triggers NTP once per successful association, including after a retry.
bool autoWifiWasConnected = false;
unsigned long autoWifiStartedAt = 0;
unsigned long autoWifiNextAttemptAt = 0;
String pendingWebCLab = "";
bool pendingWebCLabRun = false;
String pendingWebNote = "";
// A Base32 TOTP secret pasted through the local Web Companion is transferred
// once into RAM. It is never stored in Preferences or sent anywhere else.
String pendingZabkaSecret = "";
String pendingZabkaPloyId = "";
// Vault requests arrive only through the local Web Companion page. The
// passphrase is used transiently and is never saved or echoed back.
String pendingZabkaVaultSecret = "";
String pendingZabkaVaultPloyId = "";
String pendingZabkaVaultPassphrase = "";
String pendingZabkaUnlockPassphrase = "";
bool zabkaVaultStored = false;
String pendingWebAction = "";
String calcInput = "0", calcStatus = "Enter numbers, then + - * /"; float calcTotal = 0; char calcOp = 0; bool calcNew = true;
int diceValue = 1, randomValue = 0;
bool coinHeads = true;
// GAMES contains three small, self-contained offline games. Snake/Grid Hunt
// state stays in RAM, so a reboot always starts a fresh game; Kart Racer's
// best-lap times persist to flash (see kartLoadBestLap/kartSaveBestLap below).
int gameMenuSelected = 0; // 0 Snake, 1 Grid Hunt, 2 Kart Racer
int gameMode = 0;         // 0 menu, 1 Snake, 2 Grid Hunt, 3 Kart Racer
// Mirrors lastDrawnPage's role, one level down: refreshLocalPage() only calls
// the full drawGameHub() (fillScreen + header/footer) when the submode itself
// just changed; staying within a submode uses the bounded per-submode redraw
// below instead, so continuous updates (Snake's tick, Kart's level select)
// don't flash the whole panel every time.
int lastDrawnGameMode = -1;
constexpr int SNAKE_COLS = 18, SNAKE_ROWS = 10, SNAKE_MAX = 60;
constexpr int SNAKE_CELL = 14, SNAKE_LEFT = 34, SNAKE_TOP = CONTENT_Y + 20;
int snakeX[SNAKE_MAX] = {}, snakeY[SNAKE_MAX] = {};
int snakeLength = 3, snakeDx = 1, snakeDy = 0, snakeFoodX = 12, snakeFoodY = 4;
int snakeScore = 0;
bool snakeRunning = false;
unsigned long snakeNextMoveAt = 0;
int huntCursor = 4, huntTarget = 0, huntScore = 0;
String gameStatus = "Choose a game";
// MUSIC LAB is a compact 4-track / 16-step drum sequencer. Its pattern is
// RAM-only so experimentation never writes flash on every beat.
constexpr uint8_t DRUM_TRACKS = 4, DRUM_STEPS = 16;
const char* drumNames[DRUM_TRACKS] = {"KICK", "SNARE", "HAT", "CLAP"};
bool drumPattern[DRUM_TRACKS][DRUM_STEPS] = {};
uint8_t drumTrack = 0, drumCursor = 0, drumPlayStep = 0;
uint16_t drumTempo = 120;
bool drumPlaying = false;
unsigned long drumNextStepAt = 0;
String musicLastNote = "ENTER toggles a step";
// MIC is an in-memory visual monitor only. No audio is stored or sent.
constexpr size_t MIC_SAMPLE_COUNT = 96;
int16_t micSamples[MIC_SAMPLE_COUNT] = {};
int micPeak = 0;
bool micAvailable = false;
unsigned long micLastSampleAt = 0;
int deviceCheckSelected = 0;
String deviceCheckStatus = "Select a test, then press ENTER";
int qrPlusSelected = 0;
const char* qrPlusLabels[] = {"WEB COMPANION URL", "CURRENT WI-FI NAME", "NOTES (FIRST LINE)", "CUSTOM QR TEXT"};
bool paintPixels[12][16] = {};
int paintX = 0, paintY = 0;
String launcherSearchText = "";
int launcherSearchSelected = 0;
String browserUrl = "http://example.com";
String browserText = "Open Web Companion first, then ENTER to fetch this HTTP page.";
int browserScroll = 0;
String inpostNumber = "";
// This code is deliberately RAM-only: it is never written to Preferences.
// It is displayed for manual entry at a Paczkomat, not converted into an
// authorization QR because only InPost can issue a valid locker-opening QR.
String inpostPickupCode = "";
bool inpostEditingPickupCode = false;
String inpostStatus = "Enter the parcel number, then press ENTER.";
int inpostScroll = 0;
// The TOTP secret stays in RAM only and is cleared on reboot. It is never
// sent over Wi-Fi, displayed in full, or stored in Preferences.
String zabkaSecret = ""; // raw secret bytes encoded as hexadecimal text
String zabkaPloyId = "993179205607";
String zabkaStatus = "Paste the HEX secret and ploy ID, then press ENTER.";
// Password entry for the local vault is RAM-only and is cleared immediately
// after each unlock attempt. It is never persisted or sent to the web page.
bool zabkaUnlocking = false;
String zabkaUnlockBuffer = "";

// Password-protected TOTP vault. The flash record contains a random salt,
// IV, AES-256-CBC ciphertext and an HMAC-SHA-256 authenticator. A wrong
// password therefore never releases a plausible secret. This is protection
// at rest; it does not turn the local HTTP control page into HTTPS.
String hexEncode(const uint8_t* bytes, size_t length) {
  const char* digits = "0123456789ABCDEF"; String out; out.reserve(length * 2);
  for (size_t i = 0; i < length; ++i) { out += digits[bytes[i] >> 4]; out += digits[bytes[i] & 15]; }
  return out;
}
bool hexDecode(const String& text, uint8_t* bytes, size_t length) {
  if (text.length() != (int)(length * 2)) return false;
  for (size_t i = 0; i < length; ++i) {
    char hi = text[i * 2], lo = text[i * 2 + 1];
    auto nibble = [](char c) -> int { if (c >= '0' && c <= '9') return c - '0'; if (c >= 'A' && c <= 'F') return c - 'A' + 10; if (c >= 'a' && c <= 'f') return c - 'a' + 10; return -1; };
    int a = nibble(hi), b = nibble(lo); if (a < 0 || b < 0) return false; bytes[i] = uint8_t((a << 4) | b);
  }
  return true;
}
void vaultKey(const String& passphrase, const uint8_t salt[16], uint8_t key[32]) {
  // Deliberately repeated HMAC derivation slows offline password guessing
  // without storing the passphrase anywhere on the device.
  const mbedtls_md_info_t* sha256 = mbedtls_md_info_from_type(MBEDTLS_MD_SHA256);
  mbedtls_md_hmac(sha256, reinterpret_cast<const uint8_t*>(passphrase.c_str()), passphrase.length(), salt, 16, key);
  for (int round = 0; round < 12000; ++round) {
    uint8_t block[36]; memcpy(block, key, 32); block[32] = uint8_t(round >> 24); block[33] = uint8_t(round >> 16); block[34] = uint8_t(round >> 8); block[35] = uint8_t(round);
    mbedtls_md_hmac(sha256, reinterpret_cast<const uint8_t*>(passphrase.c_str()), passphrase.length(), block, sizeof(block), key);
  }
}
bool storeZabkaVault(const String& secret, const String& passphrase) {
  if (secret.isEmpty() || passphrase.length() < 8) return false;
  uint8_t salt[16], iv[16], ivWork[16], key[32], plain[64] = {}, cipher[64], mac[32];
  for (int i = 0; i < 16; ++i) { salt[i] = uint8_t(esp_random()); iv[i] = uint8_t(esp_random()); }
  memcpy(ivWork, iv, sizeof(iv)); memcpy(plain, secret.c_str(), min((int)secret.length(), 63)); vaultKey(passphrase, salt, key);
  mbedtls_aes_context aes; mbedtls_aes_init(&aes);
  if (mbedtls_aes_setkey_enc(&aes, key, 256) != 0 || mbedtls_aes_crypt_cbc(&aes, MBEDTLS_AES_ENCRYPT, sizeof(plain), ivWork, plain, cipher) != 0) { mbedtls_aes_free(&aes); return false; }
  mbedtls_aes_free(&aes);
  uint8_t authenticated[96]; memcpy(authenticated, salt, 16); memcpy(authenticated + 16, cipher, 64); memcpy(authenticated + 80, iv, 16);
  const mbedtls_md_info_t* sha256 = mbedtls_md_info_from_type(MBEDTLS_MD_SHA256);
  mbedtls_md_hmac(sha256, key, sizeof(key), authenticated, sizeof(authenticated), mac);
  preferences.begin("zabka_vault", false); preferences.putString("salt", hexEncode(salt, 16)); preferences.putString("iv", hexEncode(iv, 16)); preferences.putString("data", hexEncode(cipher, 64)); preferences.putString("mac", hexEncode(mac, 32)); preferences.putString("ploy", zabkaPloyId); preferences.end();
  zabkaVaultStored = true; return true;
}
bool unlockZabkaVault(const String& passphrase) {
  if (passphrase.length() < 8) return false;
  uint8_t salt[16], iv[16], cipher[64], mac[32], key[32], calculated[32];
  preferences.begin("zabka_vault", true); String saltHex = preferences.getString("salt", ""), ivHex = preferences.getString("iv", ""), dataHex = preferences.getString("data", ""), macHex = preferences.getString("mac", ""), savedPloy = preferences.getString("ploy", ""); preferences.end();
  if (!savedPloy.isEmpty()) zabkaPloyId = savedPloy;
  if (!hexDecode(saltHex, salt, 16) || !hexDecode(ivHex, iv, 16) || !hexDecode(dataHex, cipher, 64) || !hexDecode(macHex, mac, 32)) return false;
  vaultKey(passphrase, salt, key); uint8_t authenticated[96]; memcpy(authenticated, salt, 16); memcpy(authenticated + 16, cipher, 64); memcpy(authenticated + 80, iv, 16);
  const mbedtls_md_info_t* sha256 = mbedtls_md_info_from_type(MBEDTLS_MD_SHA256); mbedtls_md_hmac(sha256, key, sizeof(key), authenticated, sizeof(authenticated), calculated);
  if (memcmp(mac, calculated, sizeof(mac)) != 0) return false;
  uint8_t plain[64], ivWork[16]; memcpy(ivWork, iv, sizeof(iv)); mbedtls_aes_context aes; mbedtls_aes_init(&aes); bool ok = mbedtls_aes_setkey_dec(&aes, key, 256) == 0 && mbedtls_aes_crypt_cbc(&aes, MBEDTLS_AES_DECRYPT, sizeof(plain), ivWork, cipher, plain) == 0; mbedtls_aes_free(&aes);
  if (!ok || plain[63] != 0) return false; zabkaSecret = String(reinterpret_cast<char*>(plain)); return !zabkaSecret.isEmpty();
}
void lockZabkaVault() { zabkaSecret = ""; zabkaStatus = zabkaVaultStored ? "Vault locked; unlock it in Web Companion." : "No vault secret stored."; }
// Version 5 at low error correction fits a typical short https:// URL.
constexpr int QR_MAX_TEXT = 106;
String qrText = "https://m5stack.com";

// ============================================================================
// C LAB - a tiny on-device IDE for "CardC", a deliberately small, safe,
// local-only C-style scripting language this firmware interprets itself
// (there is no compiler involved - cExpr()/runCLab() etc. down near line
// 3200+ walk the source text line-by-line at run time). It gives a user a
// way to write small programs (read sensors, print text, beep, draw on a
// mini canvas, scan Wi-Fi, send one IR command) without touching this
// firmware's own source. State here (cLines[], cCursorLine, ...) is edited
// by the on-device text editor (drawCLabEditor() etc., later in the file);
// CARDC REPL is a separate single-line "type one statement, see the result"
// console sharing the same interpreter. cLabExplorerVisible's little file
// browser (ten user slots + the read-only EXAMPLES set + New File) is what
// you land on when opening this app.
// ============================================================================
constexpr int C_MAX_LINES = 32, C_MAX_LINE_CHARS = 48, C_CODE_VISIBLE_LINES = 8;
String cLines[C_MAX_LINES] = {
  "void setup() {", "  // put your setup code here, to run once:", "", "}", "",
  "void loop() {", "  // put your main code here, to run repeatedly:", "", "}"
};
int cLineCount = 9, cCursorLine = 0, cCursorColumn = 0, cScrollLine = 0;
// The code panel has room for 45 six-pixel characters after the line-number gutter.
// Keep one character of right-side breathing room so long lines do not look clipped.
constexpr int C_CODE_COLUMNS_VISIBLE = 45;
int cHorizontalScroll = 0;
bool cNavigationMode = false;
// C LAB opens in its own small file explorer.  The user document is stored
// separately from the virtual EXAMPLES folder; editing never silently saves.
bool cLabExplorerVisible = true;
constexpr int C_USER_APP_COUNT = 10;
constexpr int C_LAB_EXPLORER_ITEMS = C_USER_APP_COUNT + 2; // ten user apps, Examples, New File
int cLabExplorerSelected = 0;
int cLabExplorerScroll = 0;
int cLabActiveUserApp = 0;
String cUserApps[C_USER_APP_COUNT];
bool cLabDirty = false;
bool cLabSaveDialogVisible = false;
int cLabSaveDialogSelected = 0; // Cancel, Save, Discard
// SAVE first asks which of the ten real user-app slots receives this source.
bool cLabSlotDialogVisible = false;
int cLabSlotDialogSelected = 0;
bool cLabNameDialogVisible = false;
String cLabFileName = "untitled.clab";
String cLabNameBuffer = "";
bool cLabGuideVisible = false;
int cGuideScroll = 0;
String cOutput[5]; int cOutputCount = 0;
CVar cVars[12]; int cVarCount = 0;
CStringVar cStringVars[8]; int cStringVarCount = 0;
String cInputValues[4]; int cInputValueCount = 0, cInputReadIndex = 0;
bool cInputActive = false;
// inputint() validates a whole signed integer; readkey() waits for one
// printable key and returns it as a String.
bool cInputNumeric = false, cInputReadKey = false;
String cInputPrompt = "", cInputBuffer = "";
bool cLabQrActive = false;
String cLabQrPayload = "";
// CardC Canvas is a deliberately small immediate-mode drawing surface.
// Its calls draw only after canvas_begin(), and Fn returns to the C LAB editor.
bool cCanvasActive = false;
// CardC program-control requests are limited to the current interpreter run.
// prgrestart() is capped to prevent an accidental endless restart loop.
bool cProgramHalted = false;
bool cProgramRestartRequested = false;
bool cProgramRestarting = false;
uint8_t cProgramRestartCount = 0;
long cProgramExitCode = 0;
CFunction cFunctions[8]; int cFunctionCount = 0;

// CardC REPL is a separate one-line console. It reuses the same deliberately
// limited interpreter as C LAB, but never overwrites the saved C LAB document.
String replInput = "";
String replHistory[5];
int replHistoryCount = 0;
bool cardcReplRunning = false;

int settingSelected = 0;
const char* settingNames[] = {"Theme", "Rotation", "Backlight", "Brightness", "Screensaver", "Lock after saver", "Volume", "Lock PIN", "Wi-Fi setup", "BLE HID", "LED"};
constexpr int SETTINGS_COUNT = 11;
// Eleven compact rows fit above the fixed footer.
constexpr int SETTINGS_ROW_Y = CONTENT_Y + 6;
constexpr int SETTINGS_ROW_STEP = 17;

String uptime() { unsigned long s = millis() / 1000UL; char x[18]; snprintf(x, sizeof(x), "%02lu:%02lu:%02lu", s / 3600UL, (s % 3600UL) / 60UL, s % 60UL); return String(x); }
int noteCharacterCount() { int total = 0; for (int i = 0; i < noteCount; ++i) total += notes[i].length(); return total; }

// Built-in Cardputer display: a small, independent companion panel.
// It is redrawn only when its displayed state changes.
void updateBuiltinDisplay(bool force) {
  static String previous = "";
  String title, value, detail;
  bool show = true;
  if (page == LAUNCHER) {
    title = "BATTERY";
    int battery = M5Cardputer.Power.getBatteryLevel();
    value = battery < 0 ? "--" : String(battery) + "%";
    detail = launcherHome ? homeTileLabel(homeSelected) : (appSelected == 0 ? "BACK TO HOME" : appNames[secondaryAppIndices[appSelected - 1]]);
  } else if (page == WIFI) {
    title = "WI-FI";
    value = scanRunning ? "..." : String(networkCount);
    detail = scanRunning ? "SCANNING" : (scanDone ? "NETWORKS FOUND" : "PRESS ENTER TO SCAN");
  } else if (page == NOTES) {
    title = "NOTES";
    value = String(noteCharacterCount());
    detail = "CHARACTERS";
  } else if (page == CALC) {
    title = "CALCULATOR";
    value = calcInput;
    detail = calcStatus;
  } else if (page == CLAB || page == CARDCREPL) {
    title = page == CARDCREPL ? "CARDC REPL" : "C LAB OUTPUT";
    value = "";
    for (int i = 0; i < cOutputCount; ++i) { if (i) value += "\n"; value += cOutput[i]; }
    if (value.isEmpty()) value = "(no output)";
    detail = "";
  } else if (page == TEXTTOOLS) {
    title = "TEXT TOOLS";
    value = toolModeNames[toolMode];
    detail = String(toolText.length()) + " CHARS  ;/. MODE  ENTER NOTES";
  } else if (page == FAVOURITES) {
    int favouriteCount = 0;
    for (int i = 0; i < APP_COUNT; ++i) if (i != 10 && appFavourite[i]) favouriteCount++;
    title = "FAVOURITES";
    value = String(favouriteCount);
    detail = ";/. SELECT  ENTER OPEN  DEL UNPIN";
  } else if (page == WIFIMONITOR) {
    title = "WI-FI MONITOR";
    value = scanRunning ? "..." : String(networkCount);
    detail = scanRunning ? "SCANNING" : (scanDone ? "NETWORKS  ENTER RESCAN" : "ENTER SCAN");
  } else if (page == FILEBROWSER) {
    title = "FILE BROWSER";
    value = localFileNames[fileSelected];
    detail = localFiles[fileSelected].isEmpty() ? "EMPTY  ;/. SELECT" : "ENTER LOAD  DEL CLEAR";
  } else if (page == WEBCOMPANION) {
    title = "WEB COMPANION";
    value = webRunning ? "ONLINE" : "OFFLINE";
    detail = webRunning ? "cardputer-xl.local  ENTER STOP" : "ENTER CONNECT WI-FI";
  } else {
    // System, Clock, QR Text, Settings and Home Menu intentionally leave this screen blank.
    show = false;
  }
  String signature = String((int)page) + "|" + title + "|" + value + "|" + detail + "|" + (show ? "1" : "0");
  if (!force && signature == previous) return;
  previous = signature;
  auto& screen = M5Cardputer.Display;
  screen.fillScreen(ui.bg);
  if (!show) return;
  screen.setTextWrap(false);
  screen.setTextSize(1);
  screen.setTextColor(ui.accent, ui.bg);
  screen.setCursor(8, 7);
  screen.print(title);
  screen.drawFastHLine(8, 18, 224, ui.dim);
  if (page == CLAB || page == CARDCREPL) {
    // C LAB and REPL output use compact, clipped lines so they always remain
    // inside the Cardputer companion display instead of using the large-value layout.
    const String* output = page == CARDCREPL ? replHistory : cOutput;
    int outputCount = page == CARDCREPL ? replHistoryCount : cOutputCount;
    screen.setTextColor(ui.accent, ui.bg);
    int y = 30, start = max(0, outputCount - 7);
    for (int i = start; i < outputCount; ++i) {
      String line = output[i];
      if (line.length() > 37) line = line.substring(0, 37);
      screen.setCursor(8, y);
      screen.print(line);
      y += 14;
    }
    if (outputCount == 0) {
      screen.setTextColor(ui.dim, ui.bg);
      screen.setCursor(8, 32);
      screen.print("(no output)");
    }
  } else {
    String shown = value;
    if (shown.length() > 10) shown = shown.substring(shown.length() - 10);
    screen.setTextSize(page == CALC ? 3 : 5);
    screen.setTextColor(ui.text, ui.bg);
    screen.setCursor(8, 33);
    screen.print(shown);
    screen.setTextSize(1);
    screen.setTextColor(ui.dim, ui.bg);
    screen.setCursor(8, 108);
    screen.print(detail);
  }
}
void markStateDirty() { stateDirty = true; stateChangedAt = millis(); }

void loadPersistentState() {
  preferences.begin("cyberdeck", true);
  themeIndex = constrain(preferences.getInt("theme", themeIndex), 0, THEME_COUNT - 1);
  displayRotation = preferences.getUChar("rotation", displayRotation);
  if (displayRotation != 1 && displayRotation != 3) displayRotation = 3;
  backlightOn = preferences.getBool("backlight", backlightOn);
  statusLedEnabled = preferences.getBool("status_led", statusLedEnabled);
  brightnessLevel = constrain(preferences.getUChar("brightness", brightnessLevel), 1, 10);
  sleepIndex = constrain(preferences.getInt("sleep", sleepIndex), 0, 4);
  lockAfterScreensaverIndex = constrain(preferences.getInt("lock_after_ss", lockAfterScreensaverIndex), 0, 4);
  // Migrate the old Sound ON/OFF preference to a full-volume/default setting.
  if (preferences.isKey("volume")) volumeLevel = constrain(preferences.getUChar("volume", volumeLevel), 0, 10);
  else volumeLevel = preferences.getBool("sound", true) ? 10 : 0;
  String storedNotes = preferences.getString("notes", "");
  String storedCode = preferences.getString("code", "");
  cLabFileName = preferences.getString("clab_name", cLabFileName);
  // Ten independent C LAB user-app slots. Migrate the older single document
  // into USER APP 1 the first time this firmware loads it.
  bool haveUserApps = false;
  for (int i = 0; i < C_USER_APP_COUNT; ++i) {
    cUserApps[i] = preferences.getString((String("capp") + i).c_str(), "");
    if (!cUserApps[i].isEmpty()) haveUserApps = true;
  }
  if (!haveUserApps && !storedCode.isEmpty()) cUserApps[0] = storedCode;
  cLabActiveUserApp = constrain(preferences.getInt("capp_active", 0), 0, C_USER_APP_COUNT - 1);
  String storedFavs = preferences.getString("favs", "");
  for (int i = 0; i < APP_COUNT && i < (int)storedFavs.length(); ++i) appFavourite[i] = storedFavs[i] == '1';
  String storedHome = preferences.getString("home", "");
  if (!storedHome.isEmpty()) {
    int start = 0;
    for (int slot = 0; slot < 5; ++slot) {
      int comma = storedHome.indexOf(',', start);
      String value = comma < 0 ? storedHome.substring(start) : storedHome.substring(start, comma);
      int app = value.toInt();
      homeAppIndices[slot] = (app >= 0 && app < APP_COUNT && app != 14) ? app : -1;
      if (comma < 0) break;
      start = comma + 1;
    }
  }
  for (int i = 0; i < 3; ++i) localFiles[i] = preferences.getString((String("file") + i).c_str(), "");
  qrText = preferences.getString("qrtext", qrText);
  String savedLockHash = preferences.getString("lockhash", "");
  if (savedLockHash.length() == 64) { savedLockHash.toLowerCase(); lockPinHash = savedLockHash; }
  preferences.end();
  preferences.begin("zabka_vault", true);
  zabkaVaultStored = preferences.isKey("salt") && preferences.isKey("iv") && preferences.isKey("data") && preferences.isKey("mac");
  preferences.end();
  if (zabkaVaultStored) zabkaStatus = "Vault stored and locked; unlock in Web Companion.";
  if (!storedNotes.isEmpty()) {
    noteCount = 0; int start = 0;
    while (noteCount < 15) { int end = storedNotes.indexOf('\n', start); if (end < 0) { notes[noteCount++] = storedNotes.substring(start); break; } notes[noteCount++] = storedNotes.substring(start, end); start = end + 1; }
    if (noteCount == 0) noteCount = 1;
  }
  String activeCode = cUserApps[cLabActiveUserApp];
  if (!activeCode.isEmpty()) {
    cLineCount = 0; int start = 0;
    while (cLineCount < C_MAX_LINES) { int end = activeCode.indexOf('\n', start); if (end < 0) { cLines[cLineCount++] = activeCode.substring(start); break; } cLines[cLineCount++] = activeCode.substring(start, end); start = end + 1; }
    if (cLineCount == 0) cLineCount = 1;
  }
  cLabFileName = String("user-app-") + String(cLabActiveUserApp + 1) + ".clab";
}
void captureCLabUserApp() {
  if (cLabActiveUserApp < 0 || cLabActiveUserApp >= C_USER_APP_COUNT) return;
  String stored;
  for (int i = 0; i < cLineCount; ++i) { if (i) stored += '\n'; stored += cLines[i]; }
  cUserApps[cLabActiveUserApp] = stored;
}
void savePersistentState() {
  String storedNotes, storedCode;
  for (int i = 0; i < noteCount; ++i) { if (i) storedNotes += '\n'; storedNotes += notes[i]; }
  captureCLabUserApp();
  storedCode = cUserApps[cLabActiveUserApp];
  preferences.begin("cyberdeck", false);
  preferences.putInt("theme", themeIndex); preferences.putUChar("rotation", displayRotation); preferences.putBool("backlight", backlightOn); preferences.putBool("status_led", statusLedEnabled); preferences.putUChar("brightness", brightnessLevel);
  String storedFavs; for (int i = 0; i < APP_COUNT; ++i) storedFavs += appFavourite[i] ? '1' : '0';
  String storedHome; for (int slot = 0; slot < 5; ++slot) { if (slot) storedHome += ','; storedHome += String(homeAppIndices[slot]); }
  preferences.putInt("sleep", sleepIndex); preferences.putInt("lock_after_ss", lockAfterScreensaverIndex); preferences.putUChar("volume", volumeLevel); preferences.putString("lockhash", lockPinHash); preferences.putString("notes", storedNotes); preferences.putString("code", storedCode); preferences.putString("clab_name", cLabFileName); preferences.putString("qrtext", qrText); preferences.putString("favs", storedFavs); preferences.putString("home", storedHome);
  preferences.putInt("capp_active", cLabActiveUserApp);
  for (int i = 0; i < C_USER_APP_COUNT; ++i) preferences.putString((String("capp") + i).c_str(), cUserApps[i]);
  for (int i = 0; i < 3; ++i) preferences.putString((String("file") + i).c_str(), localFiles[i]);
  preferences.end(); stateDirty = false;
}

void applyTheme() { ui = themes[themeIndex]; }
constexpr uint16_t MENU_TONE_HZ = 1050, TYPING_TONE_HZ = 1560, BACKSPACE_TONE_HZ = 1380;
constexpr uint16_t EXIT_TONE_HZ = 720, CURSOR_TONE_HZ = 1220, FUNCTION_TONE_HZ = 940, ENTER_TONE_HZ = 1740, TAB_TONE_HZ = 1480;
constexpr uint16_t MENU_TONE_MS = 24, TYPING_TONE_MS = 10, BACKSPACE_TONE_MS = 12;
constexpr uint16_t EXIT_TONE_MS = 32, CURSOR_TONE_MS = 9, FUNCTION_TONE_MS = 14, ENTER_TONE_MS = 20, TAB_TONE_MS = 16;
void applyVolume() { M5Cardputer.Speaker.setVolume((volumeLevel * 255U) / 10U); }
// The motor-driver IN pin is binary: HIGH runs the motor and LOW stops it.
// No LEDC/PWM channel is used for haptics.
void vibrate(uint16_t durationMs) {
  if (durationMs == 0) return;
  digitalWrite(HAPTIC_IN_PIN, HIGH);
  delay(durationMs);
  digitalWrite(HAPTIC_IN_PIN, LOW);
}
void playMenuSound() { vibrate(38); if (volumeLevel) M5Cardputer.Speaker.tone(MENU_TONE_HZ, MENU_TONE_MS); }
void playTypingSound() { if (volumeLevel) M5Cardputer.Speaker.tone(TYPING_TONE_HZ, TYPING_TONE_MS); }
void playBackspaceSound() { if (volumeLevel) M5Cardputer.Speaker.tone(BACKSPACE_TONE_HZ, BACKSPACE_TONE_MS); }
// Fn/back gets one short exit pulse; it never changes motor speed.
void playExitSound() { vibrate(55); if (volumeLevel) M5Cardputer.Speaker.tone(EXIT_TONE_HZ, EXIT_TONE_MS); }
void playCursorSound() { if (volumeLevel) M5Cardputer.Speaker.tone(CURSOR_TONE_HZ, CURSOR_TONE_MS); }
void playFunctionSound() { vibrate(45); if (volumeLevel) M5Cardputer.Speaker.tone(FUNCTION_TONE_HZ, FUNCTION_TONE_MS); }
// Enter confirms an action, so it has a slightly firmer ON/OFF pulse.
void playEnterSound() { vibrate(70); if (volumeLevel) M5Cardputer.Speaker.tone(ENTER_TONE_HZ, ENTER_TONE_MS); }
void playTabSound() { if (volumeLevel) M5Cardputer.Speaker.tone(TAB_TONE_HZ, TAB_TONE_MS); }
void playBootSound() {
  if (!volumeLevel) return;
  M5Cardputer.Speaker.tone(880, 70);
  delay(95);
  M5Cardputer.Speaker.tone(1320, 110);
}
// Short status motifs. They follow Settings > Volume and are only called
// when a state first appears.
void playWifiConnectedSound() {
  vibrate(150);
  if (!volumeLevel) return;
  M5Cardputer.Speaker.tone(880, 55);
  delay(70);
  M5Cardputer.Speaker.tone(1320, 85);
}
void playBluetoothConnectedSound() {
  vibrate(180);
  if (!volumeLevel) return;
  M5Cardputer.Speaker.tone(988, 55);
  delay(70);
  M5Cardputer.Speaker.tone(1480, 85);
}
void playLowBatterySound() {
  // One stronger warning when the existing <=15% low-battery sound edge occurs.
  vibrate(480);
  if (!volumeLevel) return;
  M5Cardputer.Speaker.tone(440, 110);
  delay(145);
  M5Cardputer.Speaker.tone(330, 170);
}
void playChargingStartedSound() {
  // A distinct ON/OFF pulse makes the charger event noticeable even when
  // speaker volume is set to zero.
  vibrate(220);
  if (!volumeLevel) return;
  M5Cardputer.Speaker.tone(660, 55);
  delay(70);
  M5Cardputer.Speaker.tone(880, 55);
  delay(70);
  M5Cardputer.Speaker.tone(1320, 90);
}
// Returns true briefly after the characteristic fast battery-level increase
// caused by plugging in USB power. This is a fallback only: the Power API
// remains the primary charging indication.
bool chargingInferredFromBatteryRise(int battery) {
  const unsigned long now = millis();
  chargeRiseJustDetected = false;
  if (battery < 0) return now < inferredChargingUntil;

  // Compare each fresh reading with the previous one. The old baseline was
  // discarded after 90 seconds, which could discard the pre-plug value before
  // the Cardputer reported its delayed 10–20% USB jump.
  if (chargeBaselineBattery >= 0 &&
      battery >= chargeBaselineBattery + CHARGE_RISE_TRIGGER_PERCENT &&
      now - chargeBaselineAt <= CHARGE_RISE_WINDOW_MS) {
    inferredChargingUntil = now + INFERRED_CHARGING_HOLD_MS;
    chargeRiseJustDetected = true;
  }
  chargeBaselineBattery = battery;
  chargeBaselineAt = now;
  return now < inferredChargingUntil;
}
void serviceStatusSounds() {
  const bool wifiConnected = WiFi.status() == WL_CONNECTED;
  const bool btPaired = bleHidEnabled && bleKeyboard.isPaired();
  const int battery = M5Cardputer.Power.getBatteryLevel();
  // Battery readings can wobble near a threshold. The visual warning still
  // begins at 20%, but this sound uses hysteresis: it starts at 15% and is not
  // armed again until the measured level recovers to 25% or higher.
  const bool batteryLow = battery >= 0 && (soundBatteryKnown && soundBatteryWasLow ? battery < 25 : battery <= 15);
  const bool chargerReported = M5Cardputer.Power.isCharging() == m5::Power_Class::is_charging;
  const bool charging = chargerReported || chargingInferredFromBatteryRise(battery);

  if (soundWifiKnown && !soundWifiWasConnected && wifiConnected) playWifiConnectedSound();
  if (soundBtKnown && !soundBtWasPaired && btPaired) playBluetoothConnectedSound();
  if (soundBatteryKnown && !soundBatteryWasLow && batteryLow) playLowBatterySound();
  // Announce the start once. The fallback catches the characteristic fast
  // 5%+ level jump that can occur when a USB charger is plugged in.
  // A physical charging edge is announced once. A fresh 8%+ battery jump is
  // also an independent event: some Cardputer ADV units keep isCharging()
  // latched, so relying only on its false-to-true transition misses USB plug-in.
  if (chargeRiseJustDetected || (!soundChargeKnown && charging) ||
      (soundChargeKnown && !soundWasCharging && charging)) {
    playChargingStartedSound();
    queueToast(chargeRiseJustDetected ? "Charging detected from battery rise" :
               (chargerReported ? "Charging started" : "Charging detected"));
  }

  soundWifiKnown = soundBtKnown = soundBatteryKnown = soundChargeKnown = true;
  soundWifiWasConnected = wifiConnected;
  soundBtWasPaired = btPaired;
  soundBatteryWasLow = batteryLow;
  soundWasCharging = charging;
}
void applyBacklight() {
  // The built-in Cardputer panel needs an explicit sleep/wake command as well
  // as a brightness change. On the ADV, brightness 0 alone can leave the
  // lower panel visibly lit, which made BtnA appear to control only the TFT.
  // The external ILI9341 uses its own PWM-controlled backlight line.
  uint8_t level = 0;
  if (backlightOn) {
    level = screensaverDimmed ? 38 : (uint16_t(brightnessLevel) * 255U) / 10U;
    M5Cardputer.Display.wakeup();
    M5Cardputer.Display.setBrightness(level);
  } else {
    M5Cardputer.Display.setBrightness(0);
    M5Cardputer.Display.sleep();
  }
  ledcWrite(TFT_BL, level);
}
// Physical LED status has priority over user/CardC colour: red means either
// panel backlight is deliberately off, yellow means the PIN gate is active,
// and the default unlocked state is green.
void updateStatusLed() {
  // Low battery is a safety/status alert, so it intentionally ignores LED
  // ON/OFF, CardC led(), the lock colour and the backlight state.
  int battery = M5Cardputer.Power.getBatteryLevel();
  if (battery >= 0 && battery <= 20) {
    neopixelWrite(RGB_LED_PIN, lowBatteryAlertOn ? 255 : 0, 0, 0);
  } else if (!statusLedEnabled) {
    neopixelWrite(RGB_LED_PIN, 0, 0, 0);
  } else if (!backlightOn) {
    neopixelWrite(RGB_LED_PIN, 255, 0, 0);
  } else if (page == LOCKSCREEN) {
    neopixelWrite(RGB_LED_PIN, 255, 180, 0);
  } else if (cardcLedOverride) {
    neopixelWrite(RGB_LED_PIN, cardcLedRed, cardcLedGreen, cardcLedBlue);
  } else {
    neopixelWrite(RGB_LED_PIN, 0, 255, 0);
  }
}
void queueToast(const String& text) {
  String clean = text;
  clean.replace('\n', ' ');
  clean.trim();
  if (clean.isEmpty()) return;
  if (clean.length() > 42) clean = clean.substring(0, 42);
  // A new event replaces an older toast that has not begun drawing yet.
  queuedToastText = clean;
}
void paintToast() {
  if (!toastActive) return;
  // The ILI9341 is write-only: a moving overlay cannot restore the pixels it
  // crossed. Keep this one compact, fixed rectangle so it never leaves a trail.
  const int y = TOAST_Y;
  tft.fillRoundRect(10, y + 3, W - 20, TOAST_H - 7, 6, ui.panel);
  tft.drawRoundRect(10, y + 3, W - 20, TOAST_H - 7, 6, ui.accent);
  tft.fillCircle(25, y + 20, 4, ui.accent);
  tft.setTextSize(1); tft.setTextColor(ui.text, ui.panel);
  tft.setCursor(37, y + 12); tft.print("CARDPUTER XL");
  tft.setTextColor(ui.dim, ui.panel); tft.setCursor(37, y + 24); tft.print(toastText);
}
void serviceToasts() {
  // A toast would land inside the quick-launch overlay's covered area and
  // punch a visible hole in it; hold it off until the overlay closes.
  if (sleeping || quickMenuOpen) return;
  if (!toastActive && !queuedToastText.isEmpty()) {
    toastText = queuedToastText;
    queuedToastText = "";
    toastActive = true;
    toastStartedAt = millis();
    toastNeedsPaint = true;
  }
  if (!toastActive) return;
  if (millis() - toastStartedAt >= TOAST_HOLD_MS) {
    toastActive = false;
    // Repaint once only after the toast disappears, restoring the covered area.
    redrawNeeded = true;
    return;
  }
  if (toastNeedsPaint) {
    paintToast();
    toastNeedsPaint = false;
  }
}
void serviceConnectionToasts() {
  bool wifiConnected = WiFi.status() == WL_CONNECTED;
  bool btPaired = bleHidEnabled && bleKeyboard.isPaired();
  int battery = M5Cardputer.Power.getBatteryLevel();
  bool batteryLow = battery >= 0 && battery <= 20;
  if (toastWifiKnown && !toastWifiWasConnected && wifiConnected) queueToast("Wi-Fi connected: " + WiFi.SSID());
  if (toastBtKnown && !toastBtWasPaired && btPaired) queueToast("Bluetooth device connected");
  // Do not announce the initial battery reading; announce only recovery after
  // the existing <=20% warning state has actually been observed.
  if (toastBatteryKnown && toastBatteryWasLow && !batteryLow && battery >= 0) queueToast("Battery is back above 20%");
  toastWifiKnown = toastBtKnown = toastBatteryKnown = true;
  toastWifiWasConnected = wifiConnected;
  toastBtWasPaired = btPaired;
  toastBatteryWasLow = batteryLow;
}
void serviceLowBatteryAlert() {
  const int battery = M5Cardputer.Power.getBatteryLevel();
  const bool lowBattery = battery >= 0 && battery <= 20;
  const unsigned long now = millis();
  if (lowBattery) {
    if (!lowBatteryAlertWasActive || battery != lastLowBatteryLevel ||
        now - lastLowBatteryAlertToggleAt >= LOW_BATTERY_ALERT_INTERVAL_MS) {
      lowBatteryAlertOn = !lowBatteryAlertOn;
      lastLowBatteryAlertToggleAt = now;
      updateStatusLed();
    }
  } else if (lowBatteryAlertWasActive) {
    lowBatteryAlertOn = false;
    updateStatusLed();
  }
  lowBatteryAlertWasActive = lowBattery;
  lastLowBatteryLevel = battery;
}
void setCardCLed(uint8_t red, uint8_t green, uint8_t blue) {
  cardcLedRed = red;
  cardcLedGreen = green;
  cardcLedBlue = blue;
  cardcLedOverride = true;
  updateStatusLed();
}
// A roughly one-second power-on colour sweep confirms that the built-in
// addressable LED is alive before normal status indication takes over.
void playStartupRainbow() {
  if (!statusLedEnabled) {
    neopixelWrite(RGB_LED_PIN, 0, 0, 0);
    return;
  }
  static const uint8_t colors[][3] = {
    {255, 0, 0}, {255, 80, 0}, {255, 180, 0}, {120, 255, 0},
    {0, 255, 0}, {0, 180, 120}, {0, 80, 255}, {70, 0, 255},
    {180, 0, 255}, {255, 0, 120}
  };
  // Two quick passes make the rainbow clearly visible without making boot
  // feel slow (about 1.1 seconds in total).
  for (uint8_t pass = 0; pass < 2; ++pass) {
    for (const auto& color : colors) {
      neopixelWrite(RGB_LED_PIN, color[0], color[1], color[2]);
      delay(55);
    }
  }
  neopixelWrite(RGB_LED_PIN, 0, 0, 0);
}
void setBacklight(bool on) { backlightOn = on; applyBacklight(); updateStatusLed(); }

String lockClockText() {
  time_t now = time(nullptr);
  if (now > 1700000000) {
    struct tm localTime;
    localtime_r(&now, &localTime);
    char value[6];
    strftime(value, sizeof(value), "%H:%M", &localTime);
    return String(value);
  }
  return "--:--";
}
String lockDateText() {
  time_t now = time(nullptr);
  if (now > 1700000000) {
    struct tm localTime;
    localtime_r(&now, &localTime);
    char value[15];
    strftime(value, sizeof(value), "%a, %d %b", &localTime);
    return String(value);
  }
  return "TIME NOT SYNCED";
}
String lockPinHashFor(const String& pin) {
  if (pin.length() != 4) return "";
  uint8_t digest[32];
  const mbedtls_md_info_t* sha256 = mbedtls_md_info_from_type(MBEDTLS_MD_SHA256);
  if (!sha256 || mbedtls_md(sha256, reinterpret_cast<const uint8_t*>(pin.c_str()), pin.length(), digest) != 0) return "";
  String hash = hexEncode(digest, sizeof(digest));
  hash.toLowerCase();
  return hash;
}
bool checkLockPin(const String& pin) { return lockPinHashFor(pin) == lockPinHash; }
bool syncNetworkTime() {
  if (WiFi.status() != WL_CONNECTED) return false;
  // Poland: CET in winter and CEST in summer. POSIX TZ is interpreted by newlib.
  configTzTime("CET-1CEST,M3.5.0/2,M10.5.0/3", "162.159.200.1", "129.6.15.28", "pool.ntp.org");
  unsigned long deadline = millis() + 9000UL;
  while (millis() < deadline) {
    if (time(nullptr) > 1700000000) return true;
    delay(100);
    M5Cardputer.update();
  }
  return false;
}
void lockDevice() {
  lockPinInput = "";
  // A fresh unlock always returns to the required green status indication.
  cardcLedOverride = false;
  sleeping = true;
  page = LOCKSCREEN;
  // The next wake enters a new scene, so transfer the wallpaper exactly once.
  lockScreenBaseDrawn = false;
  setBacklight(false);
  redrawNeeded = true;
}
void startScreensaver() {
  if (page == LOCKSCREEN || page == SCREENSAVER) return;
  screensaverReturnPage = page;
  page = SCREENSAVER;
  // This is temporary display output only: Settings > Brightness is untouched.
  screensaverDimmed = true;
  applyBacklight();
  screensaverStartedAt = millis();
  screensaverLastFrameAt = 0;
  redrawNeeded = true;
}
/* Removed: the rotating icosahedron caused an unattractive visible loop. */
#if 0
void drawScreensaverIcosahedron() {
  constexpr int BOX_X = 234, BOX_Y = 150, BOX_W = 76, BOX_H = 76;
  constexpr float PHI = 1.61803399f;
  constexpr float SCALE = 20.0f;
  constexpr int CX = 272, CY = 188;
  static const float vertices[12][3] = {
    {-1, PHI, 0}, {1, PHI, 0}, {-1, -PHI, 0}, {1, -PHI, 0},
    {0, -1, PHI}, {0, 1, PHI}, {0, -1, -PHI}, {0, 1, -PHI},
    {PHI, 0, -1}, {PHI, 0, 1}, {-PHI, 0, -1}, {-PHI, 0, 1}
  };
  static const uint8_t edges[30][2] = {
    {0,1},{0,5},{0,7},{0,10},{0,11},{1,5},{1,7},{1,8},{1,9},{2,3},
    {2,4},{2,6},{2,10},{2,11},{3,4},{3,6},{3,8},{3,9},{4,5},{4,9},
    {4,11},{5,9},{5,11},{6,7},{6,8},{6,10},{7,8},{7,10},{8,9},{10,11}
  };
  int px[12], py[12];
  // Three different rates make the motion read as a continuous tumble rather
  // than a single spin that returns to an obvious starting pose.
  float ay = screensaverIcoAngle;
  float ax = screensaverIcoAngle * 0.61803399f;
  float az = screensaverIcoAngle * 0.37139067f;
  float cy = cosf(ay), sy = sinf(ay), cx = cosf(ax), sx = sinf(ax);
  float cz = cosf(az), sz = sinf(az);
  for (int i = 0; i < 12; ++i) {
    float x = vertices[i][0], y = vertices[i][1], z = vertices[i][2];
    float rx = x * cy + z * sy;
    float rz = -x * sy + z * cy;
    float ry = y * cx - rz * sx;
    rz = y * sx + rz * cx;
    float finalX = rx * cz - ry * sz;
    float finalY = rx * sz + ry * cz;
    float perspective = 1.0f / (4.8f - rz * 0.30f);
    px[i] = CX + int(finalX * SCALE * perspective * 4.1f);
    py[i] = CY + int(finalY * SCALE * perspective * 4.1f);
  }
  for (int edge = 0; edge < 30; ++edge) {
    uint16_t color = (edge % 5 == 0) ? ui.text : ui.accent;
    tft.drawLine(px[edges[edge][0]], py[edges[edge][0]], px[edges[edge][1]], py[edges[edge][1]], color);
  }
  tft.drawRect(BOX_X, BOX_Y, BOX_W, BOX_H, ILI9341_DARKGREY);
}
#endif
// ---- SCREENSAVER: a static dim clock shown after the idle timeout --------
// Not a normal Page the user can navigate to - startScreensaver() switches
// into it from loop()'s inactivity check, and any key returns to
// screensaverReturnPage (or, if a lock delay is set, escalates to LOCKSCREEN).
void drawScreensaver() {
  tft.fillScreen(ILI9341_BLACK); tft.setTextWrap(false); tft.setTextSize(1);
  tft.setTextColor(ui.dim, ILI9341_BLACK); tft.setCursor(10, 10); tft.print("CARDPUTER XL  /  SCREENSAVER");
  tft.drawFastHLine(10, 22, 300, ui.dim);
  tft.setTextSize(5); tft.setTextColor(ui.accent, ILI9341_BLACK); tft.setCursor(92, 93); tft.print(lockClockText());
  // Keep the saver intentionally minimal: only the dim clock is visible.
  // The configured lock delay continues to run in loop(), without a label.
}
void updateScreensaver() {
  // Static clock-only screensaver: no animated geometry and no periodic SPI redraw.
}
// ---- LOCKSCREEN: PIN gate over the wallpaper, checked in keyboard() ------
void drawLockScreen() {
  // Lock styling deliberately does not use ui/theme colours. Crucially, the
  // RGB565 wallpaper is NOT redrawn for a minute tick or a typed PIN digit.
  constexpr uint16_t LOCK_INK = 0xFFFF, LOCK_MUTED = 0xCE59, LOCK_PANEL = 0x10A2;
  constexpr uint16_t LOCK_ACCENT = 0xFEA0, LOCK_LOW_BATTERY = 0xF986;
  static String shownTime = "";
  static String shownDate = "";
  static int shownBattery = -999;
  static String shownPin = "<unset>";
  static bool shownWaiting = false;

  if (!lockScreenBaseDrawn) {
    // Send the 320x240 RGB565 wallpaper as one contiguous SPI burst. This is
    // substantially faster than the per-pixel drawRGBBitmap() path; it runs
    // only once when the lock scene is entered, never for local clock/PIN updates.
    tft.startWrite();
    tft.setAddrWindow(0, 0, LOCK_WALLPAPER_WIDTH, LOCK_WALLPAPER_HEIGHT);
    tft.writePixels(const_cast<uint16_t*>(LOCK_WALLPAPER_DATA), uint32_t(LOCK_WALLPAPER_WIDTH) * LOCK_WALLPAPER_HEIGHT, true, false);
    tft.endWrite();
    tft.fillRect(0, 0, W, 28, LOCK_PANEL);
    tft.fillRect(0, H - 39, W, 39, LOCK_PANEL);
    tft.drawFastHLine(0, 28, W, LOCK_ACCENT);
    tft.setTextSize(1); tft.setTextColor(LOCK_ACCENT, LOCK_PANEL); tft.setCursor(11, 9); tft.print("CARDPUTER XL  /  LOCKED");
    tft.fillRoundRect(65, 63, 190, 67, 8, LOCK_PANEL);
    tft.drawRoundRect(65, 63, 190, 67, 8, LOCK_ACCENT);
    tft.setTextColor(LOCK_ACCENT, LOCK_PANEL); tft.setCursor(122, 148); tft.print("ENTER PIN");
    tft.fillRoundRect(91, 159, 138, 26, 4, LOCK_PANEL);
    tft.drawRoundRect(91, 159, 138, 26, 4, LOCK_MUTED);
    // Invalidate local caches after the one full lock-scene paint.
    shownTime = ""; shownDate = ""; shownBattery = -999; shownPin = "<unset>"; shownWaiting = !shownWaiting;
    lockScreenBaseDrawn = true;
  }

  String timeText = lockClockText();
  if (timeText != shownTime) {
    // Opaque clock panel: this rectangle contains no wallpaper, so clearing it
    // is safe and prevents every-minute full-screen SPI traffic.
    tft.fillRect(78, 78, 166, 45, LOCK_PANEL);
    tft.setTextSize(5); tft.setTextColor(LOCK_INK, LOCK_PANEL); tft.setCursor(93, 80); tft.print(timeText);
    shownTime = timeText;
  }

  String dateText = lockDateText();
  if (dateText != shownDate) {
    // The date occupies its own opaque strip above the bottom status line.
    // It changes at most once per day, or immediately after NTP sync.
    tft.fillRect(99, 201, 213, 10, LOCK_PANEL);
    tft.setTextSize(1); tft.setTextColor(LOCK_MUTED, LOCK_PANEL);
    tft.setCursor(312 - dateText.length() * 6, 203); tft.print(dateText);
    shownDate = dateText;
  }

  int battery = M5Cardputer.Power.getBatteryLevel();
  if (battery != shownBattery) {
    tft.fillRect(8, 211, 90, 16, LOCK_PANEL);
    tft.setTextSize(1); tft.setTextColor(battery <= 20 ? LOCK_LOW_BATTERY : LOCK_MUTED, LOCK_PANEL); tft.setCursor(11, 218);
    tft.print(battery < 0 ? "BATTERY --" : "BATTERY " + String(battery) + "%");
    shownBattery = battery;
  }

  String masked; for (int i = 0; i < (int)lockPinInput.length(); ++i) masked += "*";
  if (masked != shownPin) {
    tft.fillRect(94, 162, 132, 20, LOCK_PANEL);
    tft.setTextSize(2); tft.setTextColor(LOCK_INK, LOCK_PANEL); tft.setCursor(151 - masked.length() * 6, 165); tft.print(masked); tft.print("_");
    shownPin = masked;
  }

  bool waiting = lockRetryAt > millis();
  if (waiting != shownWaiting) {
    tft.fillRect(99, 211, 213, 16, LOCK_PANEL);
    tft.setTextSize(1); tft.setTextColor(waiting ? LOCK_LOW_BATTERY : LOCK_MUTED, LOCK_PANEL); tft.setCursor(100, 218);
    tft.print(waiting ? "WAIT 10 SEC" : "ENTER UNLOCK  DEL ERASE");
    shownWaiting = waiting;
  }
}

// Decorative startup log only: it does not boot Linux or start any services.
void drawLinuxBoot() {
  static const char* lines[] = {
    "[    0.000000] CARDPUTER XL kernel booting...",
    "[    0.071000] cpu0: ESP32-S3 cyberdeck mode",
    "[    0.143000] memory: heap allocator online",
    "[    0.216000] mount: /system read-only interface",
    "[    0.298000] storage: preferences vault ready",
    "[    0.374000] spi0: external display bus detected",
    "[    0.451000] display: ILI9341 framebuffer online",
    "[    0.536000] panel: companion display ready",
    "[    0.614000] input: Cardputer keyboard ready",
    "[    0.691000] audio: speaker service ready",
    "[    0.768000] network: local tools available",
    "[    0.846000] loading theme and user settings",
    "[    0.924000] starting cyberdeck-ui.service",
    "[  OK  ] Reached target CARDPUTER XL."
  };
  constexpr int lineCount = sizeof(lines) / sizeof(lines[0]);
  constexpr int firstY = 7;
  constexpr int lineHeight = 16;
  tft.fillScreen(ILI9341_BLACK);
  tft.setTextWrap(false);
  tft.setTextSize(1);
  for (int i = 0; i < lineCount; ++i) {
    // Scroll only the log area once it fills, preserving a terminal-like flow.
    if (i >= 14) tft.fillRect(0, 0, W, H, ILI9341_BLACK);
    uint16_t color = i == lineCount - 1 ? ILI9341_GREEN : (i == lineCount - 2 ? ui.accent : ILI9341_LIGHTGREY);
    tft.setTextColor(color, ILI9341_BLACK);
    tft.setCursor(8, firstY + (i % 14) * lineHeight);
    tft.print(lines[i]);
    delay(i == lineCount - 1 ? 260 : 115);
  }
  delay(180);
}

void drawBootScreen() {
  // Full-screen splash: each text line has its own vertical area, so text cannot overlap.
  // The startup cue follows the saved Volume setting; 0% remains silent.
  playBootSound();
  tft.fillScreen(ui.bg);
  tft.fillRect(0, 0, W, 5, ui.accent);
  tft.fillRect(0, H - 5, W, 5, ui.accent);
  tft.drawRoundRect(18, 28, 284, 184, 10, ui.panel);
  tft.drawRoundRect(23, 33, 274, 174, 8, ui.accent);

  tft.drawFastHLine(56, 68, 208, ui.dim);

  tft.setTextSize(3);
  tft.setTextColor(ui.accent, ui.bg);
  tft.setCursor(70, 82);
  tft.print("CARDPUTER");

  tft.setTextSize(5);
  tft.setTextColor(ui.text, ui.bg);
  tft.setCursor(128, 120);
  tft.print("XL");

  tft.drawFastHLine(56, 169, 208, ui.dim);
  tft.setTextSize(1);
  tft.setTextColor(ui.dim, ui.bg);
  tft.setCursor(94, 184);
  tft.print("CYBERDECK INTERFACE");

  // First half stays still; the longer second half shows a compact activity
  // throbber. Only its small 22x22 area is repainted for each frame.
  delay(600);
  constexpr int throbberX = W - 30;
  constexpr int throbberY = H - 22;
  constexpr int throbberRadius = 7;
  constexpr int throbberFrames = 14;
  for (int frame = 0; frame < throbberFrames; ++frame) {
    tft.fillRect(throbberX - 11, throbberY - 11, 22, 22, ui.bg);
    for (int dot = 0; dot < 8; ++dot) {
      int phase = (dot + frame) % 8;
      uint16_t color = phase == 0 ? ui.accent : (phase == 1 ? ui.text : ui.dim);
      const int dx[] = {0, 5, 7, 5, 0, -5, -7, -5};
      const int dy[] = {-7, -5, 0, 5, 7, 5, 0, -5};
      tft.fillCircle(throbberX + dx[dot], throbberY + dy[dot], phase == 0 ? 2 : 1, color);
    }
    delay(80);
  }
}
// Short post-unlock transition. The full scene is painted once; each animation
// frame redraws only the 24x24 throbber region at about 20 FPS.
void drawWelcomeScreen() {
  // Reuse the embedded lock wallpaper for a distinct post-unlock scene. It is
  // transferred once; only the spinner area is redrawn during the animation.
  tft.startWrite();
  tft.setAddrWindow(0, 0, LOCK_WALLPAPER_WIDTH, LOCK_WALLPAPER_HEIGHT);
  tft.writePixels(const_cast<uint16_t*>(LOCK_WALLPAPER_DATA), uint32_t(LOCK_WALLPAPER_WIDTH) * LOCK_WALLPAPER_HEIGHT, true, false);
  tft.endWrite();
  constexpr uint16_t WELCOME_PANEL = 0x10A2;
  tft.fillRoundRect(34, 48, 252, 144, 10, WELCOME_PANEL);
  tft.drawRoundRect(34, 48, 252, 144, 10, ui.panel);
  tft.drawRoundRect(39, 53, 242, 134, 8, ui.accent);
  tft.setTextSize(3);
  tft.setTextColor(ui.accent, WELCOME_PANEL);
  tft.setCursor(96, 78);
  tft.print("WELCOME");
  // A short ascending confirmation cue plays once when the welcome scene opens.
  if (volumeLevel) {
    M5Cardputer.Speaker.tone(1047, 70);
    delay(80);
    M5Cardputer.Speaker.tone(1568, 110);
  }
  // Larger 48x48 welcome throbber; only this local rectangle is refreshed.
  constexpr int spinnerX = 160, spinnerY = 137;
  const int dx[] = {0, 10, 14, 10, 0, -10, -14, -10};
  const int dy[] = {-14, -10, 0, 10, 14, 10, 0, -10};
  unsigned long startedAt = millis();
  int frame = 0;
  while (millis() - startedAt < 3000UL) {
    tft.fillRect(spinnerX - 24, spinnerY - 24, 48, 48, WELCOME_PANEL);
    for (int dot = 0; dot < 8; ++dot) {
      int phase = (dot + frame) % 8;
      uint16_t color = phase == 0 ? ui.accent : (phase == 1 ? ui.text : ui.dim);
      tft.fillCircle(spinnerX + dx[dot], spinnerY + dy[dot], phase == 0 ? 4 : 2, color);
    }
    M5Cardputer.update();
    delay(50);
    ++frame;
  }
}

void drawHeaderStatus() {
  // Clear only the fixed status zones; the page title at x=6..193 is untouched.
  tft.fillRect(194, 0, W - 194, HEADER_H, ui.panel);
  const bool wifiConnected = WiFi.status() == WL_CONNECTED;
  const bool btPaired = bleHidEnabled && bleKeyboard.isPaired();
  const uint16_t btColor = !bleHidEnabled ? ui.dim : (btPaired ? ILI9341_GREEN : ui.accent);

  // Fixed zones: BT x=194..206, Wi-Fi x=211..225, clock x=232..261,
  // battery x=269..319. No status item can overwrite another one.
  const int btX = 194, iconY = 3;
  tft.drawLine(btX + 6, iconY, btX + 6, iconY + 16, btColor);
  tft.drawLine(btX + 6, iconY, btX + 12, iconY + 5, btColor);
  tft.drawLine(btX + 12, iconY + 5, btX + 2, iconY + 13, btColor);
  tft.drawLine(btX + 2, iconY + 3, btX + 12, iconY + 11, btColor);
  tft.drawLine(btX + 12, iconY + 11, btX + 6, iconY + 16, btColor);

  const int wfX = 211, baseY = 18;
  int wifiBars = 0;
  if (wifiConnected) {
    int rssi = WiFi.RSSI();
    wifiBars = rssi >= -55 ? 4 : (rssi >= -67 ? 3 : (rssi >= -78 ? 2 : 1));
  }
  for (int bar = 0; bar < 4; ++bar) {
    int height = 4 + bar * 3, x = wfX + bar * 4;
    uint16_t color = bar < wifiBars ? ILI9341_GREEN : ui.dim;
    tft.drawRect(x, baseY - height, 2, height, color);
    if (bar < wifiBars) tft.fillRect(x, baseY - height, 2, height, color);
  }

  String clock = lockClockText();
  int battery = M5Cardputer.Power.getBatteryLevel();
  tft.setTextSize(1); tft.setTextColor(ui.text, ui.panel); tft.setCursor(232, 7); tft.print(clock);
  const int batteryX = 269, batteryY = 5, batteryW = 46, batteryH = 13;
  uint16_t batteryColor = battery >= 0 && battery <= 20 ? ILI9341_RED : ui.text;
  tft.drawRoundRect(batteryX, batteryY, batteryW, batteryH, 3, batteryColor);
  tft.fillRect(316, batteryY + 4, 3, 5, batteryColor);
  tft.setTextColor(batteryColor, ui.panel); tft.setCursor(batteryX + 3, batteryY + 3);
  tft.print(battery < 0 ? "--" : String(constrain(battery, 0, 100)));
  int batteryBars = battery < 0 ? 0 : (battery >= 67 ? 3 : (battery >= 34 ? 2 : 1));
  for (int bar = 0; bar < 3; ++bar) tft.fillRect(296 + bar * 5, 7, 3, 9, bar < batteryBars ? batteryColor : ILI9341_BLACK);
}
void header(const char* title) {
  tft.fillRect(0, 0, W, HEADER_H, ui.panel);
  tft.setTextSize(1); tft.setTextColor(ui.text, ui.panel); tft.setCursor(6, 7); tft.print("CARDPUTER XL");
  tft.setTextColor(ui.accent, ui.panel); tft.print(" / "); tft.print(title);
  drawHeaderStatus();
}
void footer(const char* text) { tft.fillRect(0, H - FOOTER_H, W, FOOTER_H, ILI9341_DARKGREY); tft.setTextSize(1); tft.setTextColor(ILI9341_WHITE, ILI9341_DARKGREY); tft.setCursor(5, H - 12); tft.print(text); }

void drawLauncherRow(int row) {
  int listIndex = appScroll + row, y = CONTENT_Y + row * 33;
  tft.fillRect(2, y - 2, 290, 33, ui.bg); if (listIndex >= SECONDARY_APP_COUNT) return;
  bool sel = listIndex == appSelected; uint16_t fill = sel ? ui.selected : ILI9341_DARKGREY;
  tft.fillRoundRect(12, y, 276, 29, 4, fill); tft.drawRoundRect(12, y, 276, 29, 4, sel ? ui.accent : ui.dim);
  tft.setTextSize(1); tft.setTextColor(ui.text, fill); tft.setCursor(20, y + 5);
  if (listIndex == 0) {
    tft.print("BACK TO HOME");
    tft.setTextColor(sel ? ui.text : ui.dim, fill); tft.setCursor(20, y + 16); tft.print("return to tile launcher");
  } else {
    int i = secondaryAppIndices[listIndex - 1];
    tft.print(appNames[i]);
    tft.setTextColor(sel ? ui.text : ui.dim, fill); tft.setCursor(20, y + 16); tft.print(appInfo[i]);
  }
}
void updateLauncherIndicators() {
  tft.fillRect(294, CONTENT_Y - 1, 22, APP_VISIBLE * 33, ui.bg);
  if (appScroll > 0) { tft.setTextColor(ui.accent, ui.bg); tft.setCursor(302, CONTENT_Y); tft.print("^"); }
  if (appScroll + APP_VISIBLE < SECONDARY_APP_COUNT) { tft.setTextColor(ui.accent, ui.bg); tft.setCursor(302, CONTENT_Y + APP_VISIBLE * 33 - 10); tft.print("v"); }
}
void drawLauncherFocusRail(int y) { (void)y; }
void startLauncherFocusAnimation(int oldSelected, int oldScroll) { (void)oldSelected; (void)oldScroll; }
void updateLauncherFocusAnimation() {}
void updateLauncherSelection(int oldSelected, int oldScroll) {
  if (oldScroll == appScroll) { drawLauncherRow(oldSelected - appScroll); drawLauncherRow(appSelected - appScroll); }
  else for (int row = 0; row < APP_VISIBLE; ++row) drawLauncherRow(row);
  updateLauncherIndicators();
}

const char* homeTileLabel(int tile) {
  if (tile == 5) return "APPS";
  if (tile < 0 || tile >= 5 || homeAppIndices[tile] < 0) return "EMPTY";
  return appNames[homeAppIndices[tile]];
}
const char* homeTileIcon(int tile) {
  if (tile == 5) return "::";
  if (tile < 0 || tile >= 5 || homeAppIndices[tile] < 0) return "--";
  return homeTileIcons[tile];
}
void drawHomeTile(int tile, bool selected) {
  int col = tile % 2, row = tile / 2;
  int x = HOME_TILE_LEFT + col * (HOME_TILE_W + HOME_TILE_GAP_X);
  int y = HOME_TILE_TOP + row * (HOME_TILE_H + HOME_TILE_GAP_Y);
  uint16_t fill = selected ? ui.selected : ILI9341_DARKGREY;
  tft.fillRoundRect(x, y, HOME_TILE_W, HOME_TILE_H, 7, fill);
  tft.drawRoundRect(x, y, HOME_TILE_W, HOME_TILE_H, 7, selected ? ui.accent : ui.dim);
  // Icons stay neutral. The moving focus marker is the only accent, so it
  // cannot look as if an icon itself is being redrawn or animated.
  tft.setTextSize(2); tft.setTextColor(ui.text, fill);
  tft.setCursor(x + 12, y + 8); tft.print(homeTileIcon(tile));
  tft.setTextSize(1); tft.setTextColor(ui.text, fill);
  tft.setCursor(x + 12, y + 36); tft.print(homeTileLabel(tile));
}
void startHomeFocusAnimation(int oldTile, int newTile) {
  // The external ILI9341 leaves visible trails when an extra focus frame is
  // drawn over locally refreshed tiles. Keep navigation clean: redraw only
  // the deselected and selected cards, with no travelling or pulsing marker.
  homeFocusAnimating = false;
  drawHomeTile(oldTile, false);
  drawHomeTile(newTile, true);
}
void updateHomeFocusAnimation() {
  // Intentionally empty: tile selection has no animated overlay because it
  // produced display trails on the real panel.
}

// ---- Floating quick-launch overlay (Opt key, any page) --------------------
// A macOS-dock-style strip: 7 icons in one row, anchored just above the
// bottom edge of the panel, that slides up into view and back down again.
// The first 6 icons are the exact same tiles as the Home screen above
// (homeTileIcon()/homeTileLabel()/homeAppIndices - pin an app to Home and it
// is automatically on this menu too, no separate setup); the 7th is a "last
// app" shortcut to previousPage (tracked in draw()), with no equivalent on
// the physical Home screen. Unlike every other screen in this file it does
// not own a Page value and is not drawn through draw()/refreshLocalPage():
// it is painted directly by keyboard() when Opt opens it or the selection
// moves, and removed by asking loop() for one full repaint of whatever page
// is underneath once it closes (see forceFullRedraw). This keeps it a true
// overlay - reachable without disturbing whatever app/state the user was in.
constexpr int QUICKMENU_TILE_COUNT = HOME_TILE_COUNT + 1;
constexpr int QUICKMENU_W = 302, QUICKMENU_H = 62, QUICKMENU_MARGIN_BOTTOM = 6;
constexpr int QUICKMENU_X = (W - QUICKMENU_W) / 2;
// Resting position, docked near the bottom edge; the slide animation moves
// the panel between this Y and H (fully off-screen, below the last pixel row).
constexpr int QUICKMENU_Y = H - QUICKMENU_H - QUICKMENU_MARGIN_BOTTOM;
constexpr int QUICKMENU_TILE_W = 38, QUICKMENU_TILE_H = 36, QUICKMENU_GAP_X = 4;
constexpr int QUICKMENU_TILE_LEFT = QUICKMENU_X + 6;
constexpr int QUICKMENU_TILE_TOP = QUICKMENU_Y + 20;
// Icon/label for the 7 quick-menu tiles: 0-5 are identical to Home's own
// tiles; tile 6 has no Home equivalent, so it's handled separately here.
const char* quickMenuIcon(int tile) { return tile == HOME_TILE_COUNT ? "<<" : homeTileIcon(tile); }
const char* quickMenuLabel(int tile) { return tile == HOME_TILE_COUNT ? pageDisplayName(previousPage) : homeTileLabel(tile); }
void drawQuickMenuTile(int tile, bool selected) {
  int x = QUICKMENU_TILE_LEFT + tile * (QUICKMENU_TILE_W + QUICKMENU_GAP_X);
  uint16_t fill = selected ? ui.selected : ILI9341_DARKGREY;
  tft.fillRoundRect(x, QUICKMENU_TILE_TOP, QUICKMENU_TILE_W, QUICKMENU_TILE_H, 6, fill);
  tft.drawRoundRect(x, QUICKMENU_TILE_TOP, QUICKMENU_TILE_W, QUICKMENU_TILE_H, 6, selected ? ui.accent : ui.dim);
  tft.setTextSize(2); tft.setTextColor(ui.text, fill);
  tft.setCursor(x + 7, QUICKMENU_TILE_TOP + 10); tft.print(quickMenuIcon(tile));
}
// Redraws just the caption naming the current selection - called on open and
// every time the selection moves (icons alone are too narrow for a label).
void drawQuickMenuCaption() {
  tft.fillRect(QUICKMENU_X + 4, QUICKMENU_Y + 4, QUICKMENU_W - 8, 12, ui.panel);
  tft.setTextSize(1); tft.setTextColor(ui.accent, ui.panel);
  String label = quickMenuLabel(quickMenuSelected);
  // Default GFX font advances 6px/char at text size 1 - the same trick the
  // lock screen already uses to center its date string.
  tft.setCursor(QUICKMENU_X + (QUICKMENU_W - (int)label.length() * 6) / 2, QUICKMENU_Y + 6);
  tft.print(label);
}
// Full, final paint of the dock at rest (all 7 icons + caption). Used to
// finish the opening animation below, and reusable on its own if the panel
// ever needs a plain, non-animated repaint.
void drawQuickMenu() {
  tft.fillRoundRect(QUICKMENU_X, QUICKMENU_Y, QUICKMENU_W, QUICKMENU_H, 10, ui.panel);
  tft.drawRoundRect(QUICKMENU_X, QUICKMENU_Y, QUICKMENU_W, QUICKMENU_H, 10, ui.accent);
  drawQuickMenuCaption();
  for (int tile = 0; tile < QUICKMENU_TILE_COUNT; ++tile) drawQuickMenuTile(tile, tile == quickMenuSelected);
}
// The panel only ever moves within Y in [QUICKMENU_Y, H] - from resting
// position down to fully off-screen. Both animations below clear this exact
// zone every frame before drawing the current frame's panel on top of it,
// which is what makes a MOVING (not just growing/shrinking) shape safe on a
// write-only display: the previous frame's pixels are never left behind,
// because the whole zone they could possibly be in is always reset first.
constexpr int QUICKMENU_SLIDE_ZONE_H = H - QUICKMENU_Y;
// Opening sweeps the panel up from H (invisible, its top edge sitting on the
// very last screen row) to QUICKMENU_Y - a dock "rising" into view. The
// icons themselves only appear on the final, full-size frame (drawQuickMenu())
// once the panel is at rest, rather than being clipped mid-slide.
void openQuickMenuAnimated() {
  constexpr int FRAMES = 8;
  for (int f = 1; f <= FRAMES; ++f) {
    int y = H - (H - QUICKMENU_Y) * f / FRAMES;
    tft.fillRect(QUICKMENU_X, QUICKMENU_Y, QUICKMENU_W, QUICKMENU_SLIDE_ZONE_H, ui.bg);
    tft.fillRoundRect(QUICKMENU_X, y, QUICKMENU_W, QUICKMENU_H, 10, ui.panel);
    tft.drawRoundRect(QUICKMENU_X, y, QUICKMENU_W, QUICKMENU_H, 10, ui.accent);
    M5Cardputer.update();
    delay(16);
  }
  drawQuickMenu();
}
// Closing is the reverse: the dock sinks back down out of view. The last
// frame (f=0, fully off-screen) is just the zone-clearing fill with nothing
// drawn on top, and loop()'s forceFullRedraw path (set by the caller right
// after this returns) replaces that flat colour with whatever the real page
// underneath actually looks like.
void closeQuickMenuAnimated() {
  constexpr int FRAMES = 8;
  for (int f = FRAMES - 1; f >= 0; --f) {
    int y = H - (H - QUICKMENU_Y) * f / FRAMES;
    tft.fillRect(QUICKMENU_X, QUICKMENU_Y, QUICKMENU_W, QUICKMENU_SLIDE_ZONE_H, ui.bg);
    if (f > 0) {
      tft.fillRoundRect(QUICKMENU_X, y, QUICKMENU_W, QUICKMENU_H, 10, ui.panel);
      tft.drawRoundRect(QUICKMENU_X, y, QUICKMENU_W, QUICKMENU_H, 10, ui.accent);
    }
    M5Cardputer.update();
    delay(16);
  }
}

void drawLauncher() {
  if (launcherHome) {
    // Home deliberately reuses the lock-screen wallpaper. It is transferred
    // once on entering/redrawing the Home scene; individual tile navigation
    // still repaints only the two changed cards.
    tft.startWrite();
    tft.setAddrWindow(0, 0, LOCK_WALLPAPER_WIDTH, LOCK_WALLPAPER_HEIGHT);
    tft.writePixels(const_cast<uint16_t*>(LOCK_WALLPAPER_DATA), uint32_t(LOCK_WALLPAPER_WIDTH) * LOCK_WALLPAPER_HEIGHT, true, false);
    tft.endWrite();
  } else {
    tft.fillScreen(ui.bg);
  }
  header(launcherHome ? "HOME" : "APPS");
  if (!launcherHome) {
    for (int row = 0; row < APP_VISIBLE; ++row) drawLauncherRow(row);
    updateLauncherIndicators();
    footer(";/. SELECT  ENTER OPEN  FN HOME");
    return;
  }
  homeFocusAnimating = false;
  for (int tile = 0; tile < HOME_TILE_COUNT; ++tile) drawHomeTile(tile, tile == homeSelected);
  footer("; UP  , LEFT  / RIGHT  . DOWN");
}

// ---- SYSTEM: a read-only device info panel (battery/uptime/heap/flash) ----
// updateSystemValues() is the "local refresh" for this page (see the
// rendering-model note at the top of the file) - it repaints only the three
// rows that can change live, called from refreshLocalPage() on ENTER.
void drawSystemValue(int rowIndex, const String& value, uint16_t color) { int y = CONTENT_Y + 8 + rowIndex * 18; tft.fillRect(133, y - 1, 178, 10, ui.bg); tft.setTextSize(1); tft.setTextColor(color, ui.bg); tft.setCursor(133, y); tft.print(value); }
void updateSystemValues() { int b = M5Cardputer.Power.getBatteryLevel(); drawSystemValue(2, b < 0 ? "Unknown" : String(b) + "%", b <= 20 ? ILI9341_RED : ILI9341_GREEN); drawSystemValue(3, uptime(), ui.accent); drawSystemValue(4, String(ESP.getFreeHeap() / 1024) + " KB", ui.text); }
void drawSystem() {
  tft.fillScreen(ui.bg); header("SYSTEM"); int y = CONTENT_Y + 8, b = M5Cardputer.Power.getBatteryLevel();
  auto row = [&](const char* a, String v, uint16_t c) { tft.setTextSize(1); tft.setTextColor(ui.dim, ui.bg); tft.setCursor(16, y); tft.print(a); tft.setTextColor(c, ui.bg); tft.setCursor(133, y); tft.print(v); y += 18; };
  row("DEVICE", "M5Stack Cardputer XL", ui.text); row("DISPLAY", "ILI9341 320x240", ILI9341_GREEN); row("BATTERY", b < 0 ? "Unknown" : String(b) + "%", b <= 20 ? ILI9341_RED : ILI9341_GREEN); row("UPTIME", uptime(), ui.accent); row("FREE HEAP", String(ESP.getFreeHeap() / 1024) + " KB", ui.text); row("HEAP TOTAL", String(ESP.getHeapSize() / 1024) + " KB", ui.text); row("FLASH", String(ESP.getFlashChipSize() / 1048576UL) + " MB", ui.text); row("THEME", themeNames[themeIndex], ui.accent); footer("FN BACK     ENTER REFRESH");
}
// ---- WI-FI SCAN: a plain SSID/RSSI/encryption list, ENTER (re)scans -------
// Shares its scan (startScan()/checkScan(), WiFi.scanNetworks(true)) with
// WI-FI MONITOR below and the QR TOOLS+/DASHBOARD pages - only one scan runs
// at a time, and every page that shows results just reads the same
// networkCount/scanDone globals rather than keeping its own copy.
const char* enc(wifi_auth_mode_t e) { if (e == WIFI_AUTH_OPEN) return "OPEN"; if (e == WIFI_AUTH_WPA2_PSK || e == WIFI_AUTH_WPA_WPA2_PSK) return "WPA2"; if (e == WIFI_AUTH_WPA3_PSK) return "WPA3"; return "LOCK"; }
void drawWifi() {
  tft.fillScreen(ui.bg); header("WI-FI SCAN"); tft.setTextSize(1);
  if (scanRunning) { tft.setTextColor(ui.accent, ui.bg); tft.setCursor(16, CONTENT_Y + 12); tft.print("Scanning nearby networks..."); tft.setTextColor(ui.dim, ui.bg); tft.setCursor(16, CONTENT_Y + 28); tft.print("This can take a few seconds."); footer("FN BACK"); return; }
  if (!scanDone) { tft.setTextColor(ui.dim, ui.bg); tft.setCursor(16, CONTENT_Y + 12); tft.print("No scan results yet."); tft.setTextColor(ui.accent, ui.bg); tft.setCursor(16, CONTENT_Y + 30); tft.print("Press ENTER to scan."); footer("FN BACK     ENTER SCAN"); return; }
  if (networkCount <= 0) { tft.setTextColor(ILI9341_YELLOW, ui.bg); tft.setCursor(16, CONTENT_Y + 12); tft.print("No networks found."); footer("FN BACK     ENTER RESCAN"); return; }
  tft.setTextColor(ILI9341_GREEN, ui.bg); tft.setCursor(12, CONTENT_Y); tft.printf("Found %d network(s)", networkCount);
  for (int i = 0; i < min(networkCount, 15); i++) { int y = CONTENT_Y + 16 + i * 13; String name = WiFi.SSID(i); if (name.isEmpty()) name = "<hidden>"; if (name.length() > 24) name = name.substring(0, 24); tft.setTextColor(ui.text, ui.bg); tft.setCursor(8, y); tft.printf("%02d %s", i + 1, name.c_str()); tft.setTextColor(ui.dim, ui.bg); tft.setCursor(194, y); tft.printf("%ddB", WiFi.RSSI(i)); tft.setCursor(255, y); tft.print(enc(WiFi.encryptionType(i))); } footer("FN BACK     ENTER RESCAN");
}
String base64Encode(const String& input) {
  const char* alphabet = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
  String output; int value = 0, bits = -6;
  for (uint8_t c : input) { value = (value << 8) + c; bits += 8; while (bits >= 0) { output += alphabet[(value >> bits) & 0x3F]; bits -= 6; } }
  if (bits > -6) output += alphabet[((value << 8) >> (bits + 8)) & 0x3F];
  while (output.length() % 4) output += '=';
  return output;
}
// ---- TEXT TOOLS: character count / upper / lower / base64 on typed text ---
void drawTextTools() {
  tft.fillScreen(ui.bg); header("TEXT TOOLS");
  String shown = toolText; if (shown.length() > 44) shown = shown.substring(shown.length() - 44);
  tft.setTextColor(ui.dim, ui.bg); tft.setTextSize(1); tft.setCursor(10, CONTENT_Y + 5); tft.print(toolModeNames[toolMode]);
  tft.fillRoundRect(8, CONTENT_Y + 14, 304, 24, 4, ILI9341_DARKGREY); tft.setTextColor(ui.text, ILI9341_DARKGREY); tft.setCursor(13, CONTENT_Y + 22); tft.print(shown); tft.print("_");
  String result;
  if (toolMode == 0) result = String(toolText.length()) + " chars";
  else if (toolMode == 1) { result = toolText; result.toUpperCase(); }
  else if (toolMode == 2) { result = toolText; result.toLowerCase(); }
  else result = base64Encode(toolText);
  if (result.length() > 45) result = result.substring(0, 45);
  tft.setTextColor(ui.accent, ui.bg); tft.setCursor(10, CONTENT_Y + 57); tft.print("RESULT");
  tft.setTextColor(ui.text, ui.bg); tft.setCursor(10, CONTENT_Y + 72); tft.print(result);
  tft.setTextColor(ui.dim, ui.bg); tft.setCursor(10, CONTENT_Y + 100); tft.print(";/. mode   ENTER copy result to Notes");
  footer("FN BACK     DEL ERASE");
}
// ---- FAVOURITES: apps pinned with Ctrl+F (see keyboard()'s ctrlF check) ---
// appFavourite[] is indexed by (Page - 1); app index 10 (this very page) is
// excluded so Favourites can never favourite itself.
void drawFavourites() {
  tft.fillScreen(ui.bg); header("FAVOURITES"); int y = CONTENT_Y + 8, listed = 0;
  for (int i = 0; i < APP_COUNT; ++i) if (i != 10 && appFavourite[i]) { bool sel = listed == favouriteSelected; uint16_t bg = sel ? ui.selected : ui.bg; if (sel) tft.fillRoundRect(8, y - 4, 304, 20, 4, bg); tft.setTextColor(sel ? ui.text : ui.dim, bg); tft.setCursor(15, y); tft.print(sel ? "> " : "  "); tft.print(appNames[i]); y += 23; listed++; }
  if (!listed) { tft.setTextColor(ui.dim, ui.bg); tft.setCursor(12, y); tft.print("No favourites. Open any app and"); tft.setCursor(12, y + 13); tft.print("press Ctrl+F to pin or unpin it."); }
  footer(";/. SELECT  ENTER OPEN  DEL UNPIN");
}
// ---- WI-FI MONITOR: strongest signal + a 13-channel activity bar chart ----
// Read-only analysis of the same scan WI-FI SCAN uses; press ENTER to
// trigger a fresh scan if scanDone is false.
void drawWifiMonitor() {
  tft.fillScreen(ui.bg); header("WI-FI MONITOR");
  if (!scanDone) { tft.setTextColor(ui.dim, ui.bg); tft.setCursor(12, CONTENT_Y + 12); tft.print("No scan data. Press ENTER to scan."); footer("FN BACK     ENTER SCAN"); return; }
  int strongest = -127, strongestIndex = -1; int channels[14] = {0};
  for (int i = 0; i < networkCount; ++i) { if (WiFi.RSSI(i) > strongest) { strongest = WiFi.RSSI(i); strongestIndex = i; } int ch = WiFi.channel(i); if (ch >= 1 && ch <= 13) channels[ch]++; }
  tft.setTextColor(ui.accent, ui.bg); tft.setCursor(12, CONTENT_Y + 8); tft.printf("NETWORKS: %d", networkCount);
  if (strongestIndex >= 0) { String ssid = WiFi.SSID(strongestIndex); if (ssid.isEmpty()) ssid = "<hidden>"; if (ssid.length() > 25) ssid = ssid.substring(0, 25); tft.setTextColor(ui.text, ui.bg); tft.setCursor(12, CONTENT_Y + 25); tft.print("STRONGEST: " + ssid); tft.setCursor(12, CONTENT_Y + 38); tft.printf("RSSI: %d dBm   CH: %d", strongest, WiFi.channel(strongestIndex)); }
  tft.setTextColor(ui.dim, ui.bg); tft.setCursor(12, CONTENT_Y + 61); tft.print("CHANNEL ACTIVITY");
  for (int ch = 1; ch <= 13; ++ch) { int x = 12 + (ch - 1) * 23; int bar = min(45, channels[ch] * 8); tft.drawRect(x, CONTENT_Y + 116 - bar, 12, bar, ui.dim); tft.setCursor(x + 2, CONTENT_Y + 122); tft.print(ch % 10); }
  footer("FN BACK     ENTER RESCAN");
}
// ---- FILE BROWSER: 3 flash-backed text slots (localFiles[]), no SD card ---
void drawFileBrowser() {
  tft.fillScreen(ui.bg); header("FILE BROWSER"); tft.setTextColor(ui.dim, ui.bg); tft.setCursor(10, CONTENT_Y + 5); tft.print("LOCAL FLASH DOCUMENTS (no SD card)");
  for (int i = 0; i < 3; ++i) { int y = CONTENT_Y + 24 + i * 35; bool sel = i == fileSelected; uint16_t bg = sel ? ui.selected : ui.bg; if (sel) tft.fillRoundRect(8, y - 5, 304, 28, 4, bg); String preview = localFiles[i]; preview.replace('\n', ' '); if (preview.length() > 38) preview = preview.substring(0, 38); tft.setTextColor(ui.text, bg); tft.setCursor(15, y); tft.print(sel ? "> " : "  "); tft.print(localFileNames[i]); tft.setTextColor(sel ? ui.text : ui.dim, bg); tft.setCursor(25, y + 12); tft.print(preview.isEmpty() ? "(empty)" : preview); }
  footer(";/. SELECT  ENTER LOAD TO NOTES  DEL CLEAR");
}
// ---- HOME MENU: assigns the 5 configurable Home/quick-launch tile slots ---
// Edits homeAppIndices[] directly - both the Home screen (drawHomeTile()
// above) and the floating quick-launch overlay read it live, so a change
// here shows up in both immediately, with no separate sync step.
void drawHomeEditor() {
  tft.fillScreen(ui.bg); header(homeEditorPicking ? "HOME MENU / PICK APP" : "HOME MENU");
  tft.setTextSize(1);
  if (homeEditorPicking) {
    tft.setTextColor(ui.dim, ui.bg); tft.setCursor(10, CONTENT_Y + 4); tft.print("SLOT " + String(homeEditorSlot + 1) + "  ENTER SET  FN CANCEL");
    int first = max(0, homeEditorApp - 6), last = min(APP_COUNT, first + 11);
    for (int i = first; i < last; ++i) {
      int y = CONTENT_Y + 22 + (i - first) * 15; bool sel = i == homeEditorApp; uint16_t bg = sel ? ui.selected : ui.bg;
      if (sel) tft.fillRoundRect(8, y - 3, 304, 13, 3, bg);
      tft.setTextColor(sel ? ui.text : ui.dim, bg); tft.setCursor(14, y); tft.print(sel ? "> " : "  "); tft.print(appNames[i]);
    }
    footer(";/. SELECT     ENTER ADD");
    return;
  }
  tft.setTextColor(ui.dim, ui.bg); tft.setCursor(10, CONTENT_Y + 4); tft.print("Five editable slots; APPS always stays.");
  for (int slot = 0; slot < 5; ++slot) {
    int y = CONTENT_Y + 24 + slot * 25; bool sel = slot == homeEditorSlot; uint16_t bg = sel ? ui.selected : ui.bg;
    if (sel) tft.fillRoundRect(8, y - 4, 304, 19, 4, bg);
    tft.setTextColor(sel ? ui.text : ui.dim, bg); tft.setCursor(14, y); tft.printf("%d  ", slot + 1);
    tft.setTextColor(ui.text, bg); tft.print(homeAppIndices[slot] < 0 ? "(EMPTY)" : appNames[homeAppIndices[slot]]);
  }
  tft.setTextColor(ui.accent, ui.bg); tft.setCursor(10, CONTENT_Y + 154); tft.print("ENTER ADD/CHANGE   ,/ MOVE   DEL REMOVE");
  footer(";/. SLOT     FN BACK");
}
// ---- C LAB EXAMPLES: a picker that loads canned source into C LAB --------
// (C LAB itself - the sketch's tiny scripting language/IDE - starts much
// further down, see "---- C LAB " below.)
void drawCLabExamples() {
  static const char* names[C_EXAMPLE_COUNT] = {"HELLO SYSTEM", "NUMBER GUESS", "BATTERY REPORT", "WI-FI SURVEY", "QR GREETING", "IR TEST", "CANVAS UI + KEYS"};
  static const char* details[C_EXAMPLE_COUNT] = {"text, variables and output", "inputint plus simple condition", "read-only device values", "nearby networks only", "make a QR from typed URL", "send one NEC IR command", "Canvas panel and readkey input"};
  tft.fillScreen(ui.bg); header("C LAB EXAMPLES");
  tft.setTextSize(1); tft.setTextColor(ui.dim, ui.bg); tft.setCursor(10, CONTENT_Y + 4); tft.print("ENTER LOADS CODE INTO C LAB");
  for (int i = 0; i < C_EXAMPLE_COUNT; ++i) {
    int y = CONTENT_Y + 24 + i * 23; bool sel = i == cExampleSelected; uint16_t bg = sel ? ui.selected : ui.bg;
    if (sel) tft.fillRoundRect(8, y - 4, 304, 19, 4, bg);
    tft.setTextColor(sel ? ui.text : ui.dim, bg); tft.setCursor(14, y); tft.print(sel ? "> " : "  "); tft.print(names[i]);
    tft.setTextColor(sel ? ui.text : ui.dim, bg); tft.setCursor(27, y + 10); tft.print(details[i]);
  }
  footer(";/. SELECT     ENTER LOAD     FN BACK");
}

void loadCLabExample(int example) {
  static const char* hello[] = {"// HELLO SYSTEM", "println(\"Hello from C LAB!\");", "println(device_name);", "println(\"Theme: \" + theme_name);", "println(\"Uptime: \" + uptime_text);"};
  static const char* guess[] = {"// NUMBER GUESS", "int secret = 7;", "int guess = inputint(\"Guess 1 to 10:\");", "println(\"You typed: \" + guess);", "if (guess == secret) println(\"Correct!\");", "if (guess != secret) println(\"Try again\");"};
  static const char* battery[] = {"// BATTERY REPORT", "println(\"Battery: \" + battery + \"%\");", "println(\"Free heap: \" + free_heap_kb);", "println(\"Flash MB: \" + flash_mb);", "if (battery < 20) println(\"Charge soon\");"};
  static const char* wifi[] = {"// WI-FI SURVEY", "wifi_scan();", "println(\"Scan finished\");", "println(\"Networks: \" + wifi_networks);"};
  static const char* qr[] = {"// QR URL", "// Type a full address, for example https://m5stack.com", "qrcode(input(\"URL (https://):\"));"};
  static const char* ir[] = {"// IR TEST", "// Change values for your remote.", "ir_nec(0, 16);", "println(\"NEC command sent\");"};
  static const char* toastDemo[] = {"// CANVAS UI + KEYS", "canvas_begin();", "canvas_clear(0);", "fill_rect(18,42,284,145,1);", "fill_rect(22,46,276,137,2);", "fill_rect(30,61,260,28,0);", "println(\"Canvas UI is open\");", "String key = readkey(\"Press any key\");", "println(\"Key: \" + key);"};
  const char* const* selected = hello; int count = 5;
  if (example == 1) { selected = guess; count = 7; }
  else if (example == 2) { selected = battery; count = 5; }
  else if (example == 3) { selected = wifi; count = 4; }
  else if (example == 4) { selected = qr; count = 3; }
  else if (example == 5) { selected = ir; count = 4; }
  else if (example == 6) { selected = toastDemo; count = 9; }
  cLineCount = count;
  for (int i = 0; i < count; ++i) cLines[i] = selected[i];
  for (int i = count; i < C_MAX_LINES; ++i) cLines[i] = "";
  cCursorLine = cCursorColumn = cScrollLine = cHorizontalScroll = 0;
  cOutputCount = 0; cInputActive = false; cInputValueCount = cInputReadIndex = 0;
  cLabQrActive = false; cLabQrPayload = "";
  cLog("[example loaded: Ctrl+Enter]");
  cLabFileName = "example.clab";
  cLabExplorerVisible = false;
  cLabDirty = true;
  markStateDirty(); playEnterSound(); page = CLAB; redrawNeeded = true;
}

void rollDiceRandom() {
  diceValue = random(1, 7);
  randomValue = random(0, 1000);
  coinHeads = random(0, 2) == 0;
}
// ---- DASHBOARD: one-glance battery/uptime/heap/theme/Wi-Fi summary --------
void drawDashboard() {
  tft.fillScreen(ui.bg); header("DASHBOARD");
  int battery = M5Cardputer.Power.getBatteryLevel();
  tft.setTextSize(1); tft.setTextColor(ui.dim, ui.bg); tft.setCursor(12, CONTENT_Y + 8); tft.print("LIVE DEVICE OVERVIEW");
  tft.setTextSize(3); tft.setTextColor(ui.accent, ui.bg); tft.setCursor(14, CONTENT_Y + 27); tft.print(battery < 0 ? "--%" : String(battery) + "%");
  tft.setTextSize(1); tft.setTextColor(ui.text, ui.bg); tft.setCursor(15, CONTENT_Y + 61); tft.print("BATTERY");
  tft.setCursor(142, CONTENT_Y + 36); tft.print("UPTIME  " + uptime());
  tft.setCursor(142, CONTENT_Y + 55); tft.print("HEAP    " + String(ESP.getFreeHeap() / 1024) + " KB");
  tft.setCursor(142, CONTENT_Y + 74); tft.print("THEME   " + String(themeNames[themeIndex]));
  tft.drawFastHLine(12, CONTENT_Y + 91, 296, ui.dim);
  tft.setTextColor(scanRunning ? ui.accent : ui.text, ui.bg); tft.setCursor(14, CONTENT_Y + 108); tft.print(scanRunning ? "WI-FI: SCANNING..." : "WI-FI: " + String(networkCount) + (scanDone ? " NETWORKS" : " (NO SCAN YET)"));
  tft.setTextColor(ui.dim, ui.bg); tft.setCursor(14, CONTENT_Y + 130); tft.print("ENTER refresh Wi-Fi / dashboard");
  footer("FN BACK     ENTER REFRESH");
}
void triggerDrum(uint8_t track) {
  // M5Unified may keep the previous oscillator active when a very short tone
  // is replaced. Explicitly stop before and after each transient so one step
  // is one hit, never a held synth note.
  if (!volumeLevel) return;
  M5Cardputer.Speaker.stop();
  if (track == 0) {                         // KICK: fast pitch drop + short tail
    const uint16_t sweep[] = {150, 126, 108, 94, 82, 72};
    for (uint8_t i = 0; i < 6; ++i) { M5Cardputer.Speaker.tone(sweep[i], 7); delay(7); }
    M5Cardputer.Speaker.tone(62, 18);
    delay(18);
  } else if (track == 1) {                  // SNARE: noisy body and low thump
    M5Cardputer.Speaker.tone(180, 10);
    delay(10);
    for (uint8_t i = 0; i < 14; ++i) {
      M5Cardputer.Speaker.tone(random(420, 2200), 3);
      delay(3);
    }
  } else if (track == 2) {                  // HAT: very short bright noise
    for (uint8_t i = 0; i < 7; ++i) {
      M5Cardputer.Speaker.tone(random(2800, 6200), 2);
      delay(2);
    }
  } else {                                  // CLAP: three separated noise bursts
    for (uint8_t burst = 0; burst < 3; ++burst) {
      for (uint8_t i = 0; i < 5; ++i) {
        M5Cardputer.Speaker.tone(random(700, 2600), 2);
        delay(2);
      }
      delay(7);
    }
  }
  M5Cardputer.Speaker.stop();
}
void playDrumStep(uint8_t step) {
  for (uint8_t track = 0; track < DRUM_TRACKS; ++track) if (drumPattern[track][step]) triggerDrum(track);
}
void drawMusicCell(uint8_t track, uint8_t step) {
  constexpr int left = 66, top = CONTENT_Y + 29, cellW = 15, rowH = 25;
  int x = left + step * cellW, y = top + track * rowH;
  bool selected = track == drumTrack && step == drumCursor;
  bool playing = drumPlaying && step == drumPlayStep;
  uint16_t fill = drumPattern[track][step] ? ui.accent : ILI9341_DARKGREY;
  if (playing && !drumPattern[track][step]) fill = ui.selected;
  tft.fillRoundRect(x, y, 12, 18, 2, fill);
  tft.drawRoundRect(x, y, 12, 18, 2, selected ? ILI9341_WHITE : (playing ? ui.text : ui.dim));
}
// ---- MUSIC LAB: a 4-track/16-step drum sequencer on M5Cardputer.Speaker ---
// The playhead advances in loop() (see the drumPlaying block there), not
// through the redrawNeeded machinery, so it can tick on a musical clock
// rather than "once per redraw" - drawMusicCell() repaints just the two
// steps that changed (old playhead position, new one) each tick.
void drawMusicLab() {
  tft.fillScreen(ui.bg); header("MUSIC LAB / DRUMS");
  tft.setTextSize(1); tft.setTextColor(ui.dim, ui.bg); tft.setCursor(10, CONTENT_Y + 4); tft.print("4 TRACK / 16 STEP DRUM SEQUENCER");
  for (uint8_t step = 0; step < DRUM_STEPS; ++step) { tft.setTextColor(step % 4 == 0 ? ui.accent : ui.dim, ui.bg); tft.setCursor(68 + step * 15, CONTENT_Y + 18); tft.print((step + 1) % 10); }
  for (uint8_t track = 0; track < DRUM_TRACKS; ++track) {
    tft.setTextColor(track == drumTrack ? ui.text : ui.dim, ui.bg); tft.setCursor(10, CONTENT_Y + 35 + track * 25); tft.print(drumNames[track]);
    for (uint8_t step = 0; step < DRUM_STEPS; ++step) drawMusicCell(track, step);
  }
  tft.setTextColor(drumPlaying ? ILI9341_GREEN : ui.accent, ui.bg); tft.setCursor(10, CONTENT_Y + 142); tft.print(drumPlaying ? "PLAYING" : "STOPPED");
  tft.setTextColor(ui.text, ui.bg); tft.setCursor(112, CONTENT_Y + 142); tft.print("TEMPO " + String(drumTempo) + " BPM");
  tft.setTextColor(ui.dim, ui.bg); tft.setCursor(10, CONTENT_Y + 162); tft.print(";/.: MOVE   ENTER: STEP   SPACE: PLAY");
  tft.setCursor(10, CONTENT_Y + 175); tft.print("[ / ]: TEMPO     DEL: CLEAR TRACK");
  footer("; UP  , LEFT  / RIGHT  . DOWN  FN BACK");
}
void updateMicMonitor() {
  if (page != MIC || millis() - micLastSampleAt < 50) return;
  micLastSampleAt = millis();
  // M5Cardputer/M5Unified owns the ADV ES8311 microphone configuration.
  // Read a short PCM block for display only; no recording is retained.
  if (!M5.Mic.isEnabled()) M5.Mic.begin();
  micAvailable = M5.Mic.isEnabled();
  if (!micAvailable || !M5.Mic.record(micSamples, MIC_SAMPLE_COUNT, 16000)) return;
  int peak = 0;
  for (size_t i = 0; i < MIC_SAMPLE_COUNT; ++i) peak = max(peak, abs((int)micSamples[i]));
  micPeak = peak;
  // The waveform rectangle is the only region refreshed on every sample.
  const int left = 10, top = CONTENT_Y + 47, width = 300, height = 92, mid = top + height / 2;
  tft.fillRect(left, top, width, height, ui.bg);
  tft.drawRect(left, top, width, height, ui.dim);
  tft.drawFastHLine(left + 1, mid, width - 2, ILI9341_DARKGREY);
  for (int x = 0; x < width - 2; ++x) {
    size_t index = (size_t)x * MIC_SAMPLE_COUNT / (width - 2);
    int y = mid - (int)((long)micSamples[index] * (height / 2 - 3) / 32768L);
    tft.drawPixel(left + 1 + x, constrain(y, top + 1, top + height - 2), ui.accent);
  }
  int meter = constrain(map(micPeak, 0, 24000, 0, 296), 0, 296);
  tft.fillRect(12, CONTENT_Y + 29, 296, 10, ILI9341_DARKGREY);
  tft.fillRect(12, CONTENT_Y + 29, meter, 10, meter > 245 ? ILI9341_RED : ui.accent);
  tft.fillRect(12, CONTENT_Y + 13, 296, 11, ui.bg);
  tft.setTextSize(1); tft.setTextColor(ui.text, ui.bg); tft.setCursor(12, CONTENT_Y + 13);
  tft.printf("LEVEL %d / 32767", micPeak);
}
// ---- BLE KEYBOARD: turns the Cardputer into a Bluetooth HID keyboard -----
// Forwards physical keystrokes to a paired host via the HijelHID_BLEKeyboard
// library (bleKeyboard global) instead of driving any app on this device.
void drawBleKeyboard() {
  tft.fillScreen(ui.bg); header("BLE KEYBOARD");
  bool paired = bleKeyboard.isPaired();
  tft.setTextSize(1);
  tft.setTextColor(paired ? ILI9341_GREEN : ui.accent, ui.bg);
  tft.setCursor(12, CONTENT_Y + 10);
  tft.print(paired ? "HOST CONNECTED / READY TO TYPE" : "ADVERTISING: CARDPUTER XL KEYBOARD");
  tft.setTextColor(ui.text, ui.bg);
  tft.setCursor(12, CONTENT_Y + 35);
  tft.print(paired ? "Keyboard input is sent to the paired host." : "Pair this device in your phone or PC Bluetooth menu.");
  tft.setTextColor(ui.dim, ui.bg);
  tft.setCursor(12, CONTENT_Y + 63);
  tft.print("Type normally after pairing. ENTER and DEL work too.");
  tft.setCursor(12, CONTENT_Y + 79);
  tft.print("FN returns to Home and stops key forwarding.");
  tft.setCursor(12, CONTENT_Y + 111);
  tft.print("BLE stays local; no text is stored by this app.");
  footer(paired ? "TYPE TO HOST     FN BACK" : "PAIR IN BLUETOOTH SETTINGS     FN BACK");
}
// ---- MIC: live waveform + level meter from the built-in ES8311 mic -------
// Nothing is recorded or stored; updateMicMonitor() (called from loop())
// just samples and redraws the waveform rectangle a few times a second.
void drawMic() {
  tft.fillScreen(ui.bg); header("MIC"); tft.setTextSize(1);
  tft.setTextColor(micAvailable ? ILI9341_GREEN : ui.dim, ui.bg); tft.setCursor(10, CONTENT_Y + 3);
  tft.print(micAvailable ? "BUILT-IN MIC LIVE / NOTHING IS SAVED" : "STARTING BUILT-IN MICROPHONE...");
  tft.setTextColor(ui.dim, ui.bg); tft.setCursor(10, CONTENT_Y + 155);
  tft.print("Live level and waveform only; no recording.");
  footer("FN BACK");
  micLastSampleAt = 0;
}
// ---- DICE & RANDOM: a d6, a coin flip, and a 0-999 random number ---------
void drawDiceRandom() {
  tft.fillScreen(ui.bg); header("DICE & RANDOM");
  tft.setTextSize(1); tft.setTextColor(ui.dim, ui.bg); tft.setCursor(12, CONTENT_Y + 7); tft.print("ENTER ROLLS ALL VALUES");
  tft.setTextSize(5); tft.setTextColor(ui.accent, ui.bg); tft.setCursor(22, CONTENT_Y + 31); tft.print(diceValue);
  tft.setTextSize(1); tft.setTextColor(ui.text, ui.bg); tft.setCursor(20, CONTENT_Y + 79); tft.print("D6 DICE");
  tft.setTextSize(3); tft.setTextColor(ui.text, ui.bg); tft.setCursor(143, CONTENT_Y + 40); tft.print(coinHeads ? "HEADS" : "TAILS");
  tft.setTextSize(1); tft.setTextColor(ui.dim, ui.bg); tft.setCursor(145, CONTENT_Y + 79); tft.print("COIN");
  tft.drawFastHLine(12, CONTENT_Y + 94, 296, ui.dim);
  tft.setTextSize(3); tft.setTextColor(ui.accent, ui.bg); tft.setCursor(92, CONTENT_Y + 112); tft.print(randomValue);
  tft.setTextSize(1); tft.setTextColor(ui.dim, ui.bg); tft.setCursor(92, CONTENT_Y + 145); tft.print("RANDOM 0-999");
  footer("FN BACK     ENTER ROLL");
}
// ============================================================================
// KART RACER (GAMEHUB gameMode 3) - solo time-trial 3D go-kart racer, ported
// from the standalone CardputerKart sketch (C:\Users\retro\OneDrive\Documents\
// claw'd\CardputerKart\). All symbols below are "kart"-prefixed to keep them
// clear of the rest of this file's globals.
//
// Display: this launcher draws its whole UI straight onto the single external
// `tft` panel with no off-screen buffer anywhere, so the racer does the same -
// no second SPI bus / second display object like the standalone sketch had.
// The level-select and results screens use the normal header()/footer() chrome
// (drawKartHub(), wired into drawGameHub() below); the live countdown/race view
// takes over the full 320x240 panel every frame (bypassing the header/footer),
// exactly like the original sketch's single external viewport did.
//
// Not ported: the standalone game's continuous PWM "engine hum" haptic and its
// own Fn+0 external-rotation cycling. The hum needs ledcAttach() on
// HAPTIC_IN_PIN, which would fight this launcher's own blocking
// digitalWrite()-based vibrate() on that same pin (see vibrate() above) - so
// discrete event pulses (lap/wall/drift/countdown) reuse vibrate() instead,
// same as every other sound/haptic feedback in this file. Panel rotation is
// already a global launcher setting and applies to the racer automatically.
// Best-lap times use this file's existing Preferences object (namespace
// "kartlap") rather than LittleFS, since LittleFS isn't used anywhere else
// here and needs a partition scheme that includes a spiffs/littlefs region.

static const float KART_PI = 3.14159265f;

// This file must stay a .cpp, not a .ino: Arduino's .ino build step
// auto-generates forward declarations for every function signature it finds
// and inserts them all above the sketch's own code, before types like
// KVec3/KartCam/KartTrack/KartCar below are even defined - functions taking
// them by value/reference then fail to compile ("does not name a type").
// A .cpp is just an ordinary top-to-bottom translation unit, so this file's
// own top-of-file forward-declaration block (see the very first functions
// declared near the top of the file) is enough on its own, in the order
// they're written. See CardputerXL.ino for the thin wrapper this pairs with.
struct KVec3 { float x = 0, y = 0, z = 0; };
inline KVec3 operator+(const KVec3& a, const KVec3& b) { return {a.x + b.x, a.y + b.y, a.z + b.z}; }
inline KVec3 operator-(const KVec3& a, const KVec3& b) { return {a.x - b.x, a.y - b.y, a.z - b.z}; }
inline KVec3 operator*(const KVec3& a, float s) { return {a.x * s, a.y * s, a.z * s}; }
inline float kdot(const KVec3& a, const KVec3& b) { return a.x * b.x + a.y * b.y + a.z * b.z; }
inline KVec3 kcross(const KVec3& a, const KVec3& b) { return {a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x}; }
inline KVec3 knormalized(const KVec3& v) { float len = sqrtf(kdot(v, v)); if (len < 1e-6f) return {0, 0, 0}; return v * (1.0f / len); }

// ---- Car model (kart.h/.cpp) ----
struct KartCar {
  static constexpr float MAX_SPEED = 20.0f;
  KVec3 pos{0, 0, 0};
  float heading = 0, moveHeading = 0, speed = 0;
  bool drifting = false;
  int driftDir = 0;
  float driftCharge = 0;
  void update(float dt, int throttle, int steer);
};
void KartCar::update(float dt, int throttle, int steer) {
  static const float ACCEL = 6.0f, BRAKE = 16.0f, FRICTION = 6.0f, MIN_SPEED = -4.0f;
  static const float TURN_RATE = 1.7f, TURN_SPEED_REF = 4.0f;
  static const float DRIFT_MIN_SPEED = 3.0f, DRIFT_TURN_RATE = 3.6f, DRIFT_EASE = 3.0f, DRIFT_BOOST_RATE = 6.0f, DRIFT_MAX_BOOST = 6.0f;
  if (throttle > 0) speed += ACCEL * dt;
  else if (throttle < 0) speed -= BRAKE * dt;
  else if (speed > 0) speed = max(0.0f, speed - FRICTION * dt);
  else if (speed < 0) speed = min(0.0f, speed + FRICTION * dt);
  speed = constrain(speed, MIN_SPEED, MAX_SPEED);

  bool wantDrift = (throttle < 0) && steer != 0 && fabsf(speed) > DRIFT_MIN_SPEED;
  if (!drifting && wantDrift) { drifting = true; driftDir = steer; driftCharge = 0; }
  if (drifting && (steer == 0 || throttle >= 0 || fabsf(speed) < DRIFT_MIN_SPEED * 0.5f)) {
    speed += min(driftCharge * DRIFT_BOOST_RATE, DRIFT_MAX_BOOST);
    drifting = false;
  }
  if (drifting) {
    driftCharge += dt;
    heading -= driftDir * DRIFT_TURN_RATE * dt;
    float diff = heading - moveHeading;
    while (diff > KART_PI) diff -= 2 * KART_PI;
    while (diff < -KART_PI) diff += 2 * KART_PI;
    moveHeading += diff * min(1.0f, DRIFT_EASE * dt);
  } else {
    float speedFactor = constrain(fabsf(speed) / TURN_SPEED_REF, 0.0f, 1.0f);
    heading -= steer * TURN_RATE * speedFactor * dt;
    moveHeading = heading;
  }
  KVec3 fwd{sinf(moveHeading), 0, cosf(moveHeading)};
  pos = pos + fwd * (speed * dt);
}

// ---- Track (track.h/.cpp) ----
struct KartTrack {
  static const int N = 128;
  static const int NUM_LEVELS = 5;
  static constexpr float WIDTH = 5.0f;
  KVec3 center[N], left[N], right[N];
  void generate(int level);
  static const char* levelName(int level);
  KVec3 tangentAt(int i) const;
  int nearestIndexLocal(const KVec3& p, int hint, int window) const;
  int nearestIndexFull(const KVec3& p) const;
  float lateralOffset(const KVec3& p, int idx) const;
private:
  void generateOval();
  void generateRectangle();
  void generatePolygonTrack(const KVec3* verts, int n, float cornerRadius);
};
void KartTrack::generate(int level) {
  switch (level) {
    case 1: generateRectangle(); return;
    case 2: {
      const int n = 6; KVec3 verts[n];
      for (int i = 0; i < n; ++i) { float ang = (2 * KART_PI * i) / n; float r = 38.0f * (1.0f + 0.13f * cosf(i * 2.4f)); verts[i] = {r * cosf(ang), 0, r * sinf(ang)}; }
      generatePolygonTrack(verts, n, 5.0f); return;
    }
    case 3: {
      const int n = 8; KVec3 verts[n];
      for (int i = 0; i < n; ++i) { float ang = (2 * KART_PI * i) / n; float r = 48.0f * (1.0f + 0.12f * cosf(i * 2.4f)); verts[i] = {r * cosf(ang), 0, r * sinf(ang)}; }
      generatePolygonTrack(verts, n, 5.0f); return;
    }
    case 4: {
      const int n = 6; KVec3 verts[n]; const float A = 110.0f, B = 32.0f;
      for (int i = 0; i < n; ++i) { float ang = (2 * KART_PI * i) / n; verts[i] = {A * cosf(ang), 0, B * sinf(ang)}; }
      generatePolygonTrack(verts, n, 9.0f); return;
    }
    default: generateOval(); return;
  }
}
const char* KartTrack::levelName(int level) {
  switch (level) {
    case 1: return "Rectangle";
    case 2: return "Hexagon Circuit";
    case 3: return "Octagon GP";
    case 4: return "Speed Ring";
    default: return "Oval";
  }
}
void KartTrack::generateOval() {
  const float straightHalf = 20.0f, R = 12.0f, straightLen = 2 * straightHalf, turnLen = KART_PI * R, totalLen = 2 * straightLen + 2 * turnLen;
  for (int i = 0; i < N; ++i) {
    float s = totalLen * (float)i / (float)N;
    KVec3 p, tangent;
    if (s < straightLen) { float t = s; p = {-straightHalf + t, 0, -R}; tangent = {1, 0, 0}; }
    else if (s < straightLen + turnLen) { float t = s - straightLen; float ang = -KART_PI / 2 + t / R; p = {straightHalf + R * cosf(ang), 0, R * sinf(ang)}; tangent = {-sinf(ang), 0, cosf(ang)}; }
    else if (s < 2 * straightLen + turnLen) { float t = s - (straightLen + turnLen); p = {straightHalf - t, 0, R}; tangent = {-1, 0, 0}; }
    else { float t = s - (2 * straightLen + turnLen); float ang = KART_PI / 2 + t / R; p = {-straightHalf + R * cosf(ang), 0, R * sinf(ang)}; tangent = {-sinf(ang), 0, cosf(ang)}; }
    center[i] = p;
    KVec3 perp = knormalized(kcross(tangent, KVec3{0, 1, 0}));
    left[i] = p - perp * (WIDTH * 0.5f); right[i] = p + perp * (WIDTH * 0.5f);
  }
}
void KartTrack::generateRectangle() {
  const float X_HALF = 26.0f, Z_HALF = 16.0f, R = 8.0f;
  const float straightX = 2 * X_HALF - 2 * R, straightZ = 2 * Z_HALF - 2 * R, turnLen = (KART_PI / 2) * R;
  const float s1 = straightX, s2 = s1 + turnLen, s3 = s2 + straightZ, s4 = s3 + turnLen, s5 = s4 + straightX, s6 = s5 + turnLen, s7 = s6 + straightZ, totalLen = s7 + turnLen;
  for (int i = 0; i < N; ++i) {
    float s = totalLen * (float)i / (float)N;
    KVec3 p, tangent;
    if (s < s1) { p = {-X_HALF + R + s, 0, -Z_HALF}; tangent = {1, 0, 0}; }
    else if (s < s2) { float ang = -KART_PI / 2 + (s - s1) / R; KVec3 c{X_HALF - R, 0, -Z_HALF + R}; p = {c.x + R * cosf(ang), 0, c.z + R * sinf(ang)}; tangent = {-sinf(ang), 0, cosf(ang)}; }
    else if (s < s3) { p = {X_HALF, 0, -Z_HALF + R + (s - s2)}; tangent = {0, 0, 1}; }
    else if (s < s4) { float ang = (s - s3) / R; KVec3 c{X_HALF - R, 0, Z_HALF - R}; p = {c.x + R * cosf(ang), 0, c.z + R * sinf(ang)}; tangent = {-sinf(ang), 0, cosf(ang)}; }
    else if (s < s5) { p = {X_HALF - R - (s - s4), 0, Z_HALF}; tangent = {-1, 0, 0}; }
    else if (s < s6) { float ang = KART_PI / 2 + (s - s5) / R; KVec3 c{-X_HALF + R, 0, Z_HALF - R}; p = {c.x + R * cosf(ang), 0, c.z + R * sinf(ang)}; tangent = {-sinf(ang), 0, cosf(ang)}; }
    else if (s < s7) { p = {-X_HALF, 0, Z_HALF - R - (s - s6)}; tangent = {0, 0, -1}; }
    else { float ang = KART_PI + (s - s7) / R; KVec3 c{-X_HALF + R, 0, -Z_HALF + R}; p = {c.x + R * cosf(ang), 0, c.z + R * sinf(ang)}; tangent = {-sinf(ang), 0, cosf(ang)}; }
    center[i] = p;
    KVec3 perp = knormalized(kcross(tangent, KVec3{0, 1, 0}));
    left[i] = p - perp * (WIDTH * 0.5f); right[i] = p + perp * (WIDTH * 0.5f);
  }
}
static KVec3 kartPerpLeft(const KVec3& v) { return {-v.z, 0, v.x}; }
static float kartCross2D(const KVec3& a, const KVec3& b) { return a.x * b.z - a.z * b.x; }
void KartTrack::generatePolygonTrack(const KVec3* verts, int n, float cornerRadius) {
  static const int MAX_V = 12;
  KVec3 cornerStart[MAX_V], cornerEnd[MAX_V], arcCenter[MAX_V];
  float angStart[MAX_V], angSpan[MAX_V];
  for (int i = 0; i < n; ++i) {
    KVec3 prev = verts[(i - 1 + n) % n], cur = verts[i], next = verts[(i + 1) % n];
    KVec3 dirIn = knormalized(cur - prev), dirOut = knormalized(next - cur);
    float turn = atan2f(kartCross2D(dirIn, dirOut), kdot(dirIn, dirOut));
    float tlen = cornerRadius * tanf(fabsf(turn) * 0.5f);
    cornerStart[i] = cur - dirIn * tlen;
    cornerEnd[i] = cur + dirOut * tlen;
    arcCenter[i] = cornerStart[i] + kartPerpLeft(dirIn) * cornerRadius;
    angStart[i] = atan2f(cornerStart[i].z - arcCenter[i].z, cornerStart[i].x - arcCenter[i].x);
    angSpan[i] = turn;
  }
  static const int MAX_SEG = 2 * MAX_V;
  float sBound[MAX_SEG]; float total = 0;
  for (int i = 0; i < n; ++i) {
    KVec3 prevEnd = cornerEnd[(i - 1 + n) % n];
    KVec3 d = cornerStart[i] - prevEnd;
    total += sqrtf(kdot(d, d));
    sBound[2 * i] = total;
    total += cornerRadius * angSpan[i];
    sBound[2 * i + 1] = total;
  }
  for (int k = 0; k < N; ++k) {
    float s = total * (float)k / (float)N;
    int seg = 0;
    while (seg < 2 * n - 1 && s >= sBound[seg]) seg++;
    int i = seg / 2;
    KVec3 p, tangent;
    if (seg % 2 == 0) {
      KVec3 prevEnd = cornerEnd[(i - 1 + n) % n];
      float segStart = (seg == 0) ? 0 : sBound[seg - 1];
      float t = s - segStart;
      KVec3 dir = knormalized(cornerStart[i] - prevEnd);
      p = prevEnd + dir * t; tangent = dir;
    } else {
      float segStart = sBound[seg - 1];
      float t = s - segStart;
      float ang = angStart[i] + t / cornerRadius;
      p = arcCenter[i] + KVec3{cosf(ang) * cornerRadius, 0, sinf(ang) * cornerRadius};
      tangent = {-sinf(ang), 0, cosf(ang)};
    }
    center[k] = p;
    KVec3 perp = knormalized(kcross(tangent, KVec3{0, 1, 0}));
    left[k] = p - perp * (WIDTH * 0.5f); right[k] = p + perp * (WIDTH * 0.5f);
  }
}
KVec3 KartTrack::tangentAt(int i) const { int next = (i + 1) % N, prev = (i - 1 + N) % N; return knormalized(center[next] - center[prev]); }
int KartTrack::nearestIndexLocal(const KVec3& p, int hint, int window) const {
  int best = hint; float bestD2 = 1e18f;
  for (int k = -window; k <= window; ++k) {
    int i = ((hint + k) % N + N) % N;
    float dx = p.x - center[i].x, dz = p.z - center[i].z, d2 = dx * dx + dz * dz;
    if (d2 < bestD2) { bestD2 = d2; best = i; }
  }
  return best;
}
int KartTrack::nearestIndexFull(const KVec3& p) const { return nearestIndexLocal(p, 0, N / 2); }
float KartTrack::lateralOffset(const KVec3& p, int idx) const { float dx = p.x - center[idx].x, dz = p.z - center[idx].z; return sqrtf(dx * dx + dz * dz); }

// ---- Camera (kartcam.h/.cpp) ----
struct KartCam {
  KVec3 position, right, up, forward;
  float fovY = 65.0f;
  void follow(const KartCar& car, bool firstPerson);
  bool project(const KVec3& world, int screenW, int screenH, int& sx, int& sy) const;
};
void KartCam::follow(const KartCar& car, bool firstPerson) {
  KVec3 carFwd{sinf(car.heading), 0, cosf(car.heading)};
  KVec3 worldUp{0, 1, 0};
  if (firstPerson) {
    const float eyeHeight = 0.75f, eyeForward = 0.15f;
    position = car.pos + carFwd * eyeForward + KVec3{0, eyeHeight, 0};
    forward = carFwd;
  } else {
    const float chaseDist = 3.5f, chaseHeight = 1.6f, lookAhead = 4.0f;
    position = car.pos - carFwd * chaseDist + KVec3{0, chaseHeight, 0};
    KVec3 lookAt = car.pos + carFwd * lookAhead + KVec3{0, 0.3f, 0};
    forward = knormalized(lookAt - position);
  }
  right = knormalized(kcross(forward, worldUp));
  up = kcross(right, forward);
}
bool KartCam::project(const KVec3& world, int screenW, int screenH, int& sx, int& sy) const {
  static const float DEG2RAD = 3.14159265f / 180.0f, NEAR_Z = 0.15f;
  KVec3 rel = world - position;
  float cz = kdot(rel, forward);
  if (cz < NEAR_Z) return false;
  float cx = kdot(rel, right), cyv = kdot(rel, up);
  float f = (screenH * 0.5f) / tanf(fovY * DEG2RAD * 0.5f);
  sx = (int)(screenW * 0.5f + cx * f / cz);
  sy = (int)(screenH * 0.5f - cyv * f / cz);
  return true;
}

// ---- State ----
enum KartRaceState { KART_HOME, KART_COUNTDOWN, KART_RACING, KART_RESULTS };
static const int KART_TOTAL_LAPS = 3;
static const unsigned long KART_COUNTDOWN_PHASE_MS = 800;
static const unsigned long KART_COUNTDOWN_MS = KART_COUNTDOWN_PHASE_MS * 4;

KartTrack kartTrack;
KartCar kartCar;
KartCam kartCam;
// The live 3D view draws dozens of primitives a frame; sent straight to the
// panel over SPI (as every other page in this file draws) that showed up as
// a visible black wipe each frame. Render into this off-screen buffer
// instead and blit it to tft in one shot - the same double-buffering the
// standalone CardputerKart sketch used for this exact view on this exact
// hardware.
GFXcanvas16 kartCanvas(320, 240);
KartRaceState kartRaceState = KART_HOME;
int kartSelectedLevel = 0, kartCurrentLevel = 0;
bool kartFirstPerson = false;
bool kartMusicEnabled = false, kartFxEnabled = true;
int kartLastIdx = 0;
float kartProgressAccum = 0;
int kartLapsCompleted = 0;
bool kartOffTrack = false, kartWasColliding = false, kartNewBestThisRace = false;
float kartMapMinX = 0, kartMapMaxX = 1, kartMapMinZ = 0, kartMapMaxZ = 1;
unsigned long kartRaceStartMs = 0, kartLapStartMs = 0, kartFinishMs = 0;
unsigned long kartLastFrameMs = 0, kartLastDriftBuzzMs = 0, kartCountdownStartMs = 0;
int kartCountdownLastPhase = -1;
float kartBestLapSeconds = -1;
int kartCurrentGear = 1;

// ---- Best-lap persistence (besttime.h/.cpp, adapted to this file's shared
// Preferences object instead of a second storage backend) ----
static String kartLapKey(int level) { return "b" + String(level); }
bool kartLoadBestLap(int level, float& outSeconds) {
  preferences.begin("kartlap", true);
  float val = preferences.getFloat(kartLapKey(level).c_str(), -1.0f);
  preferences.end();
  if (val < 0) return false;
  outSeconds = val;
  return true;
}
void kartSaveBestLap(int level, float seconds) {
  preferences.begin("kartlap", false);
  preferences.putFloat(kartLapKey(level).c_str(), seconds);
  preferences.end();
}

// ---- Music (music.h/.cpp) - channel 0 melody, channel 1 beeps, channel 2
// engine drone, same channel split as the standalone sketch. GAMEHUB is the
// only page active while these run, so there's no runtime conflict with
// MUSICLAB's own Speaker.tone() calls elsewhere in this file. ----
struct KartNote { float freq; uint16_t dur; };
static const KartNote kartMelody[] = {
  {220.00f, 150}, {261.63f, 150}, {329.63f, 150}, {440.00f, 150},
  {392.00f, 150}, {440.00f, 150}, {523.25f, 150}, {440.00f, 150},
  {220.00f, 150}, {261.63f, 150}, {329.63f, 150}, {440.00f, 150},
  {349.23f, 150}, {392.00f, 150}, {440.00f, 150}, {523.25f, 150},
};
static const int KART_NUM_NOTES = sizeof(kartMelody) / sizeof(kartMelody[0]);
static int kartNoteIndex = -1;
static unsigned long kartNoteEndMs = 0;
static bool kartWasPlaying = false;
static bool kartMusicOn = false, kartFxOn = true;
static float kartLastEngineFreq = -1, kartSmoothedFreq = 0;
void kartMusicInit() {
  // Master volume is this launcher's own volumeLevel/applyVolume() setting -
  // deliberately NOT overridden here (the standalone sketch forced it to 255
  // since it owned the whole device; this launcher already manages it).
  M5Cardputer.Speaker.setChannelVolume(0, 160);
  M5Cardputer.Speaker.setChannelVolume(1, 255);
  M5Cardputer.Speaker.setChannelVolume(2, 200);
}
void kartMusicUpdate(bool playing) {
  if (!kartMusicOn || !playing) {
    if (kartWasPlaying) { M5Cardputer.Speaker.stop(0); kartNoteIndex = -1; }
    kartWasPlaying = false;
    return;
  }
  unsigned long now = millis();
  if (!kartWasPlaying || now >= kartNoteEndMs) {
    kartNoteIndex = (kartNoteIndex + 1) % KART_NUM_NOTES;
    M5Cardputer.Speaker.tone(kartMelody[kartNoteIndex].freq, kartMelody[kartNoteIndex].dur, 0, true);
    kartNoteEndMs = now + kartMelody[kartNoteIndex].dur;
  }
  kartWasPlaying = true;
}
void kartMusicBeep(float freqHz, uint16_t durationMs) { if (kartFxOn) M5Cardputer.Speaker.tone(freqHz, durationMs, 1, true); }
void kartMusicEngineTone(float freqHz) {
  if (!kartFxOn) return;
  if (kartSmoothedFreq <= 0) kartSmoothedFreq = freqHz;
  kartSmoothedFreq += (freqHz - kartSmoothedFreq) * 0.15f;
  if (fabsf(kartSmoothedFreq - kartLastEngineFreq) > 2.0f) {
    M5Cardputer.Speaker.tone(kartSmoothedFreq, UINT32_MAX, 2, true);
    kartLastEngineFreq = kartSmoothedFreq;
  }
}
void kartMusicEngineStop() { M5Cardputer.Speaker.stop(2); kartLastEngineFreq = -1; kartSmoothedFreq = 0; }
void kartMusicSetEnabled(bool m, bool f) {
  kartMusicOn = m; kartFxOn = f;
  if (!kartMusicOn && kartWasPlaying) { M5Cardputer.Speaker.stop(0); kartNoteIndex = -1; kartWasPlaying = false; }
  if (!kartFxOn) { M5Cardputer.Speaker.stop(1); kartMusicEngineStop(); }
}

// ---- Rendering (renderer.h/.cpp), drawn straight onto the launcher's `tft`
// external panel instead of a canvas buffer ----
static void kartDrawSeg(const KartCam& cam, const KVec3& a, const KVec3& b, uint16_t col) {
  int ax, ay, bx, by;
  if (!cam.project(a, kartCanvas.width(), kartCanvas.height(), ax, ay)) return;
  if (!cam.project(b, kartCanvas.width(), kartCanvas.height(), bx, by)) return;
  kartCanvas.drawLine(ax, ay, bx, by, col);
}
static void kartFillQuad(const KartCam& cam, const KVec3& a, const KVec3& b, const KVec3& c, const KVec3& d, uint16_t col) {
  int ax, ay, bx, by, cx, cy, dx, dy;
  if (!cam.project(a, kartCanvas.width(), kartCanvas.height(), ax, ay)) return;
  if (!cam.project(b, kartCanvas.width(), kartCanvas.height(), bx, by)) return;
  if (!cam.project(c, kartCanvas.width(), kartCanvas.height(), cx, cy)) return;
  if (!cam.project(d, kartCanvas.width(), kartCanvas.height(), dx, dy)) return;
  kartCanvas.fillTriangle(ax, ay, bx, by, cx, cy, col);
  kartCanvas.fillTriangle(ax, ay, cx, cy, dx, dy, col);
}
static int kartIndexDelta(int a, int b, int n) { int d = a - b; if (d > n / 2) d -= n; if (d < -n / 2) d += n; return d; }
static void kartDrawWheel(const KartCam& cam, const KVec3& center, float radius, float thickness, const KVec3& fwd, const KVec3& up, const KVec3& right, uint16_t col) {
  const int SIDES = 6;
  KVec3 ringA[SIDES], ringB[SIDES];
  KVec3 offset = right * (thickness * 0.5f);
  for (int i = 0; i < SIDES; ++i) {
    float a = (2 * KART_PI * i) / SIDES;
    KVec3 rim = fwd * (cosf(a) * radius) + up * (sinf(a) * radius);
    ringA[i] = center - offset + rim; ringB[i] = center + offset + rim;
  }
  for (int i = 0; i < SIDES; ++i) {
    int j = (i + 1) % SIDES;
    kartDrawSeg(cam, ringA[i], ringA[j], col);
    kartDrawSeg(cam, ringB[i], ringB[j], col);
    kartDrawSeg(cam, ringA[i], ringB[i], col);
  }
}
static void kartDrawKartBox(const KartCam& cam, const KartCar& car) {
  KVec3 fwd{sinf(car.heading), 0, cosf(car.heading)};
  KVec3 right = knormalized(kcross(fwd, KVec3{0, 1, 0}));
  KVec3 up{0, 1, 0};
  const float wheelR = 0.22f, hw = 0.45f, hl = 0.65f;
  const float clearance = wheelR * 2.0f, bodyTop = clearance + 0.25f, rollBarTop = bodyTop + 0.35f, rollBarSpan = hw * 0.6f;
  auto at = [&](float sx, float sz, float sy) { return car.pos + right * (sx * hw) + fwd * (sz * hl) + up * sy; };
  KVec3 c[8] = { at(-1, -1, clearance), at(1, -1, clearance), at(1, 1, clearance), at(-1, 1, clearance),
                 at(-1, -1, bodyTop), at(1, -1, bodyTop), at(1, 1, bodyTop), at(-1, 1, bodyTop) };
  static const uint8_t edges[12][2] = {{0,1},{1,2},{2,3},{3,0},{4,5},{5,6},{6,7},{7,4},{0,4},{1,5},{2,6},{3,7}};
  for (auto& e : edges) kartDrawSeg(cam, c[e[0]], c[e[1]], ILI9341_WHITE);
  const float wheelThickness = 0.16f;
  KVec3 wheelCenters[4] = { at(-1, -1, wheelR), at(1, -1, wheelR), at(1, 1, wheelR), at(-1, 1, wheelR) };
  for (auto& wc : wheelCenters) kartDrawWheel(cam, wc, wheelR, wheelThickness, fwd, up, right, ILI9341_DARKGREY);
  KVec3 postL0 = at(-rollBarSpan / hw, -1, bodyTop), postR0 = at(rollBarSpan / hw, -1, bodyTop);
  KVec3 postL1 = postL0 + up * (rollBarTop - bodyTop), postR1 = postR0 + up * (rollBarTop - bodyTop);
  kartDrawSeg(cam, postL0, postL1, ILI9341_RED);
  kartDrawSeg(cam, postR0, postR1, ILI9341_RED);
  kartDrawSeg(cam, postL1, postR1, ILI9341_RED);
}
static void kartDrawDashboardOverlay() {
  int w = kartCanvas.width(), h = kartCanvas.height();
  const uint16_t DASH_COLOR = 0x2987, TRIM_COLOR = ILI9341_LIGHTGREY;
  int hoodTop = h - h / 4;
  kartCanvas.fillTriangle(0, h - 1, w - 1, h - 1, w - w / 5, hoodTop, DASH_COLOR);
  kartCanvas.fillTriangle(0, h - 1, w - w / 5, hoodTop, w / 5, hoodTop, DASH_COLOR);
  int wheelCx = w / 2, wheelCy = h - 10, wheelR = h / 6;
  kartCanvas.drawCircle(wheelCx, wheelCy, wheelR, TRIM_COLOR);
  kartCanvas.drawLine(wheelCx, wheelCy, wheelCx, wheelCy - wheelR, TRIM_COLOR);
  kartCanvas.drawLine(wheelCx, wheelCy, wheelCx - wheelR, wheelCy + wheelR / 2, TRIM_COLOR);
  kartCanvas.drawLine(wheelCx, wheelCy, wheelCx + wheelR, wheelCy + wheelR / 2, TRIM_COLOR);
  int mSize = w / 14;
  kartCanvas.fillTriangle(4, 4, 4 + mSize, 4, 4, 4 + mSize / 2, DASH_COLOR);
  kartCanvas.fillTriangle(w - 4, 4, w - 4 - mSize, 4, w - 4, 4 + mSize / 2, DASH_COLOR);
}
static void kartDrawPylon(const KartCam& cam, const KVec3& base, uint16_t col) { kartDrawSeg(cam, base, base + KVec3{0, 1.2f, 0}, col); }
static void kartDrawFinishGate(const KartCam& cam, const KartTrack& track) {
  KVec3 lTop = track.left[0] + KVec3{0, 2.0f, 0}, rTop = track.right[0] + KVec3{0, 2.0f, 0};
  kartDrawSeg(cam, track.left[0], lTop, ILI9341_YELLOW);
  kartDrawSeg(cam, track.right[0], rTop, ILI9341_YELLOW);
  kartDrawSeg(cam, lTop, rTop, ILI9341_YELLOW);
}
static void kartDrawSky(const KartCam& cam) {
  int w = kartCanvas.width(), h = kartCanvas.height();
  KVec3 farLevel = cam.position + cam.forward * 200.0f;
  farLevel.y = cam.position.y;
  int hx, horizonY = h / 2;
  if (cam.project(farLevel, w, h, hx, horizonY)) horizonY = constrain(horizonY, 0, h);
  kartCanvas.fillRect(0, 0, w, horizonY, ILI9341_NAVY);
  const int NMTN = 12; const float MTN_RADIUS = 300.0f, MTN_SPREAD = 45.0f;
  for (int i = 0; i < NMTN; ++i) {
    float ang = (2 * KART_PI * i) / NMTN;
    KVec3 dir{cosf(ang), 0, sinf(ang)};
    KVec3 tangentDir{-sinf(ang), 0, cosf(ang)};
    KVec3 baseCenter = dir * MTN_RADIUS;
    float peakHeight = 45.0f + 25.0f * ((i * 5) % 7) / 6.0f;
    KVec3 base1 = baseCenter - tangentDir * MTN_SPREAD, base2 = baseCenter + tangentDir * MTN_SPREAD;
    KVec3 peak = baseCenter + KVec3{0, peakHeight, 0};
    int x0, y0, x1, y1, x2, y2;
    if (cam.project(base1, w, h, x0, y0) && cam.project(base2, w, h, x1, y1) && cam.project(peak, w, h, x2, y2))
      kartCanvas.fillTriangle(x0, y0, x1, y1, x2, y2, ILI9341_PURPLE);
  }
}
void kartRenderScene() {
  kartCanvas.fillScreen(ILI9341_BLACK);
  kartDrawSky(kartCam);
  const float wallHeight = 0.6f; const int WINDOW = 28;
  int camIdx = kartTrack.nearestIndexFull(kartCam.position);
  for (int i = 0; i < KartTrack::N; ++i) {
    if (abs(kartIndexDelta(i, camIdx, KartTrack::N)) > WINDOW) continue;
    int j = (i + 1) % KartTrack::N;
    uint16_t stripe = ((i / 3) % 2 == 0) ? ILI9341_RED : ILI9341_WHITE;
    kartFillQuad(kartCam, kartTrack.left[i], kartTrack.left[j], kartTrack.left[j] + KVec3{0, wallHeight, 0}, kartTrack.left[i] + KVec3{0, wallHeight, 0}, stripe);
    kartFillQuad(kartCam, kartTrack.right[i], kartTrack.right[j], kartTrack.right[j] + KVec3{0, wallHeight, 0}, kartTrack.right[i] + KVec3{0, wallHeight, 0}, stripe);
    if (i % 8 == 0) {
      KVec3 perp = knormalized(kartTrack.right[i] - kartTrack.center[i]);
      KVec3 outL = kartTrack.center[i] - perp * (KartTrack::WIDTH * 0.5f + 1.5f);
      KVec3 outR = kartTrack.center[i] + perp * (KartTrack::WIDTH * 0.5f + 1.5f);
      uint16_t col = ((i / 8) % 2 == 0) ? ILI9341_ORANGE : ILI9341_WHITE;
      kartDrawPylon(kartCam, outL, col);
      kartDrawPylon(kartCam, outR, col);
    }
  }
  kartDrawFinishGate(kartCam, kartTrack);
  if (!kartFirstPerson) kartDrawKartBox(kartCam, kartCar); else kartDrawDashboardOverlay();
}
static void kartDrawMinimap(int x0, int y0, int size) {
  float spanX = max(kartMapMaxX - kartMapMinX, 1.0f), spanZ = max(kartMapMaxZ - kartMapMinZ, 1.0f);
  float scale = (size - 8) / max(spanX, spanZ);
  float cx = (kartMapMinX + kartMapMaxX) * 0.5f, cz = (kartMapMinZ + kartMapMaxZ) * 0.5f;
  auto toMap = [&](float wx, float wz, int& px, int& py) { px = x0 + size / 2 + (int)((wx - cx) * scale); py = y0 + size / 2 + (int)((wz - cz) * scale); };
  kartCanvas.fillRect(x0, y0, size, size, ILI9341_BLACK);
  kartCanvas.drawRect(x0, y0, size, size, ILI9341_DARKGREY);
  int px, py, qx, qy;
  for (int i = 0; i < KartTrack::N; ++i) {
    int j = (i + 1) % KartTrack::N;
    toMap(kartTrack.center[i].x, kartTrack.center[i].z, px, py);
    toMap(kartTrack.center[j].x, kartTrack.center[j].z, qx, qy);
    kartCanvas.drawLine(px, py, qx, qy, ILI9341_CYAN);
  }
  toMap(kartTrack.center[0].x, kartTrack.center[0].z, px, py);
  kartCanvas.fillCircle(px, py, 2, ILI9341_YELLOW);
  toMap(kartCar.pos.x, kartCar.pos.z, px, py);
  kartCanvas.fillCircle(px, py, 3, ILI9341_RED);
}
static void kartFormatTime(float seconds, char* buf, size_t n) {
  if (seconds < 0) { snprintf(buf, n, "--:--.--"); return; }
  int mm = (int)(seconds / 60); float ss = seconds - mm * 60;
  snprintf(buf, n, "%d:%05.2f", mm, ss);
}
void kartRenderHud(unsigned long now) {
  int w = kartCanvas.width();
  const int mapSize = 56;
  kartCanvas.fillRect(0, 0, 150, 68, ILI9341_BLACK);
  kartDrawMinimap(w - mapSize - 4, 4, mapSize);
  kartCanvas.setTextSize(1);
  char buf[48], tbuf[16]; int y = 3;
  auto line = [&](const char* s, uint16_t col) { kartCanvas.setTextColor(col, ILI9341_BLACK); kartCanvas.setCursor(3, y); kartCanvas.print(s); y += 11; };
  snprintf(buf, sizeof(buf), "%s LAP %d/%d", KartTrack::levelName(kartCurrentLevel), kartLapsCompleted + 1, KART_TOTAL_LAPS);
  line(buf, ILI9341_CYAN);
  kartFormatTime((now - kartLapStartMs) / 1000.0f, tbuf, sizeof(tbuf));
  snprintf(buf, sizeof(buf), "TIME %s", tbuf); line(buf, ILI9341_WHITE);
  kartFormatTime(kartBestLapSeconds, tbuf, sizeof(tbuf));
  snprintf(buf, sizeof(buf), "BEST %s", tbuf); line(buf, ILI9341_WHITE);
  snprintf(buf, sizeof(buf), "SPEED %d GEAR %d", (int)(kartCar.speed * 10), kartCurrentGear); line(buf, ILI9341_WHITE);
  if (kartCar.drifting) line("DRIFTING", ILI9341_ORANGE);
  else if (kartOffTrack) line("WALL!", ILI9341_RED);
}
static const char* kartCountdownText(int phase) { return (phase == 0) ? "3" : (phase == 1) ? "2" : (phase == 2) ? "1" : "GO!"; }
static void kartDrawCountdownOverlay(int phase) {
  const char* txt = kartCountdownText(phase);
  kartCanvas.setTextSize(6); kartCanvas.setTextColor(ILI9341_YELLOW);
  int16_t x1, y1; uint16_t tw, th;
  kartCanvas.getTextBounds(txt, 0, 0, &x1, &y1, &tw, &th);
  int w = kartCanvas.width(), h = kartCanvas.height();
  kartCanvas.setCursor(w / 2 - (int)tw / 2 - x1, h / 2 - (int)th / 2 - y1);
  kartCanvas.print(txt);
}

// ---- Engine gear/RPM state (cosmetic only, drives the engine tone; the old
// haptic hum it also fed in the standalone sketch was dropped, see note above)
static const int KART_NUM_GEARS = 5;
static void kartComputeEngineState(float speed, int& gearOut, float& rpmFracOut) {
  float speedFrac = constrain(fabsf(speed) / KartCar::MAX_SPEED, 0.0f, 1.0f);
  int gear = (int)(speedFrac * KART_NUM_GEARS);
  if (gear >= KART_NUM_GEARS) gear = KART_NUM_GEARS - 1;
  float gearLow = (float)gear / KART_NUM_GEARS, gearHigh = (float)(gear + 1) / KART_NUM_GEARS;
  rpmFracOut = (speedFrac - gearLow) / (gearHigh - gearLow);
  gearOut = gear + 1;
}

// ---- Race lifecycle ----
void startKartRace(int level) {
  kartCurrentLevel = level;
  kartTrack.generate(level);
  if (!kartLoadBestLap(level, kartBestLapSeconds)) kartBestLapSeconds = -1;
  kartMapMinX = kartMapMaxX = kartTrack.center[0].x;
  kartMapMinZ = kartMapMaxZ = kartTrack.center[0].z;
  for (int i = 1; i < KartTrack::N; ++i) {
    kartMapMinX = min(kartMapMinX, kartTrack.center[i].x); kartMapMaxX = max(kartMapMaxX, kartTrack.center[i].x);
    kartMapMinZ = min(kartMapMinZ, kartTrack.center[i].z); kartMapMaxZ = max(kartMapMaxZ, kartTrack.center[i].z);
  }
  kartCar = KartCar();
  kartCar.pos = kartTrack.center[0];
  KVec3 t = kartTrack.tangentAt(0);
  kartCar.heading = atan2f(t.x, t.z);
  kartCar.moveHeading = kartCar.heading;
  kartLastIdx = 0; kartProgressAccum = 0; kartLapsCompleted = 0;
  kartOffTrack = false; kartWasColliding = false; kartNewBestThisRace = false;
  kartCountdownStartMs = millis(); kartCountdownLastPhase = -1;
  kartLastFrameMs = millis();
  kartRaceState = KART_COUNTDOWN;
}

// Level-select and results screens - drawn through the normal header/footer
// chrome, called from drawGameHub() below (so the redrawNeeded/draw()/
// refreshLocalPage() machinery repaints them the same way as every other
// GAMEHUB submenu).
void drawKartHub() {
  lastDrawnGameMode = gameMode;
  tft.fillScreen(ui.bg); header("GAMES / KART RACER");
  tft.setTextSize(1);
  if (kartRaceState == KART_RESULTS) {
    tft.setTextColor(ILI9341_YELLOW, ui.bg); tft.setCursor(12, CONTENT_Y + 8);
    tft.print(String("FINISHED - ") + KartTrack::levelName(kartCurrentLevel));
    char tbuf[16];
    kartFormatTime((kartFinishMs - kartRaceStartMs) / 1000.0f, tbuf, sizeof(tbuf));
    tft.setTextColor(ui.text, ui.bg); tft.setCursor(12, CONTENT_Y + 32); tft.print("TOTAL TIME  "); tft.print(tbuf);
    kartFormatTime(kartBestLapSeconds, tbuf, sizeof(tbuf));
    tft.setCursor(12, CONTENT_Y + 50); tft.print("BEST LAP    "); tft.print(tbuf);
    if (kartNewBestThisRace) { tft.setTextColor(ILI9341_GREEN, ui.bg); tft.setCursor(12, CONTENT_Y + 70); tft.print("NEW BEST LAP!"); }
    footer("ENTER RACE AGAIN     H LEVELS     FN GAMES");
    return;
  }
  tft.setTextColor(ui.dim, ui.bg); tft.setCursor(12, CONTENT_Y + 6); tft.print("3 LAPS - BEST LAP TIME IS SAVED");
  for (int i = 0; i < KartTrack::NUM_LEVELS; ++i) {
    int y = CONTENT_Y + 26 + i * 20; bool selected = i == kartSelectedLevel; uint16_t bg = selected ? ui.selected : ui.bg;
    if (selected) tft.fillRoundRect(8, y - 3, 304, 16, 4, bg);
    float best; char tbuf[16];
    bool has = kartLoadBestLap(i, best);
    kartFormatTime(has ? best : -1, tbuf, sizeof(tbuf));
    tft.setTextColor(selected ? ui.text : ui.dim, bg); tft.setCursor(15, y); tft.print(selected ? "> " : "  "); tft.print(KartTrack::levelName(i));
    tft.setCursor(230, y); tft.print(tbuf);
  }
  tft.setTextColor(ui.accent, ui.bg); tft.setCursor(12, CONTENT_Y + 150);
  tft.print(String("MUSIC ") + (kartMusicEnabled ? "ON" : "OFF") + "   FX " + (kartFxEnabled ? "ON" : "OFF"));
  footer(";/. SELECT   ENTER RACE   M MUSIC   F FX   FN GAMES");
}

// Continuous per-frame race loop - called unconditionally every loop()
// iteration (like stepSnakeGame()), but only actually does anything while a
// countdown or race is live; it renders the full-screen 3D view itself and
// bypasses the header/footer redrawNeeded/draw() machinery entirely for that,
// the same way the standalone sketch drove its own external viewport.
void stepKartRace() {
  if (page != GAMEHUB || gameMode != 3) return;
  if (kartRaceState != KART_COUNTDOWN && kartRaceState != KART_RACING) return;
  // The live view blits straight to tft every call, bypassing redrawNeeded
  // entirely (see the comment above kartRenderScene()) - it would paint
  // over the quick-launch overlay every single frame if not paused here.
  if (quickMenuOpen) return;

  unsigned long now = millis();
  float dt = (now - kartLastFrameMs) / 1000.0f;
  kartLastFrameMs = now;
  if (dt > 0.1f) dt = 0.1f;
  if (dt < 0) dt = 0;

  static bool prevR = false, prevH = false, prevV = false;
  bool nowR = M5Cardputer.Keyboard.isKeyPressed('r');
  bool nowH = M5Cardputer.Keyboard.isKeyPressed('h');
  bool nowV = M5Cardputer.Keyboard.isKeyPressed('v');
  bool edgeR = nowR && !prevR, edgeH = nowH && !prevH, edgeV = nowV && !prevV;
  prevR = nowR; prevH = nowH; prevV = nowV;

  if (edgeH) { kartRaceState = KART_HOME; kartMusicEngineStop(); redrawNeeded = true; return; }
  if (edgeR) { startKartRace(kartCurrentLevel); return; }
  if (edgeV) kartFirstPerson = !kartFirstPerson;

  if (kartRaceState == KART_COUNTDOWN) {
    kartMusicUpdate(false);
    kartMusicEngineTone(40.0f);
    unsigned long elapsed = now - kartCountdownStartMs;
    int phase = (int)(elapsed / KART_COUNTDOWN_PHASE_MS);
    if (phase > 3) phase = 3;
    if (phase != kartCountdownLastPhase) {
      kartCountdownLastPhase = phase;
      if (phase < 3) { vibrate(15); kartMusicBeep(440, 120); } else { vibrate(35); kartMusicBeep(880, 300); }
    }
    kartCam.follow(kartCar, kartFirstPerson);
    kartRenderScene();
    kartDrawCountdownOverlay(phase);
    kartRenderHud(now);
    tft.drawRGBBitmap(0, 0, kartCanvas.getBuffer(), kartCanvas.width(), kartCanvas.height());
    if (elapsed >= KART_COUNTDOWN_MS) {
      unsigned long t = millis();
      kartRaceStartMs = t; kartLapStartMs = t; kartRaceState = KART_RACING;
    }
    return;
  }

  // KART_RACING
  kartMusicUpdate(true);

  int throttle = 0, steer = 0;
  if (M5Cardputer.Keyboard.isKeyPressed('w')) throttle = 1;
  else if (M5Cardputer.Keyboard.isKeyPressed('a')) throttle = -1;
  if (M5Cardputer.Keyboard.isKeyPressed(',')) steer = -1;
  else if (M5Cardputer.Keyboard.isKeyPressed('/')) steer = 1;

  bool wasDrifting = kartCar.drifting;
  kartCar.update(dt, throttle, steer);
  if (kartCar.drifting && now - kartLastDriftBuzzMs > 120) { vibrate(6); kartLastDriftBuzzMs = now; }
  if (wasDrifting && !kartCar.drifting) vibrate(30);

  int nearest = kartTrack.nearestIndexLocal(kartCar.pos, kartLastIdx, 6);
  int delta = nearest - kartLastIdx;
  if (delta > KartTrack::N / 2) delta -= KartTrack::N;
  if (delta < -KartTrack::N / 2) delta += KartTrack::N;
  kartProgressAccum += delta;
  kartLastIdx = nearest;

  KVec3 tangent = kartTrack.tangentAt(nearest);
  KVec3 wallPerp = knormalized(kcross(tangent, KVec3{0, 1, 0}));
  float signedOff = kdot(kartCar.pos - kartTrack.center[nearest], wallPerp);
  float wallLimit = KartTrack::WIDTH * 0.5f;
  kartOffTrack = false;
  if (signedOff > wallLimit || signedOff < -wallLimit) {
    kartOffTrack = true;
    float over = signedOff > wallLimit ? signedOff - wallLimit : signedOff + wallLimit;
    kartCar.pos = kartCar.pos - wallPerp * over;
    KVec3 curFwd = {sinf(kartCar.moveHeading), 0, cosf(kartCar.moveHeading)};
    float along = kdot(curFwd, tangent) >= 0 ? 1.0f : -1.0f;
    kartCar.moveHeading = atan2f(tangent.x * along, tangent.z * along);
    kartCar.heading = kartCar.moveHeading;
    if (!kartWasColliding) { kartCar.speed *= 0.6f; vibrate(30); } else { vibrate(10); }
  }
  kartWasColliding = kartOffTrack;

  int lapNow = (int)floorf(kartProgressAccum / KartTrack::N);
  if (lapNow > kartLapsCompleted) {
    kartLapsCompleted = lapNow;
    float lapTime = (now - kartLapStartMs) / 1000.0f;
    kartLapStartMs = now;
    vibrate(20);
    kartMusicBeep(660, 100);
    if (kartBestLapSeconds < 0 || lapTime < kartBestLapSeconds) {
      kartBestLapSeconds = lapTime;
      kartSaveBestLap(kartCurrentLevel, kartBestLapSeconds);
      kartNewBestThisRace = true;
    }
    if (kartLapsCompleted >= KART_TOTAL_LAPS) {
      kartRaceState = KART_RESULTS;
      kartFinishMs = now;
      vibrate(40);
      kartMusicBeep(880, 400);
      kartMusicEngineStop();
      redrawNeeded = true;
      return;
    }
  }

  float rpmFrac; int gearOut;
  kartComputeEngineState(kartCar.speed, gearOut, rpmFrac);
  kartCurrentGear = gearOut;
  kartMusicEngineTone(40.0f + (kartCurrentGear - 1) * 8.0f + rpmFrac * 70.0f);

  kartCam.follow(kartCar, kartFirstPerson);
  kartRenderScene();
  kartRenderHud(now);
  tft.drawRGBBitmap(0, 0, kartCanvas.getBuffer(), kartCanvas.width(), kartCanvas.height());
}
// ============================================================================

// Bounded redraws for the two submodes that don't already manage their own
// incremental updates (Snake draws its own moved cells directly in
// stepSnakeGame(); Kart's live view bypasses this file's redraw machinery
// entirely in stepKartRace()). Called both from drawGameHub() (full entry
// draw) and directly from refreshLocalPage() once already inside the
// submode, so a selection change never needs a fillScreen()+header()+footer().
void drawGameMenuList() {
  const char* games[] = {"SNAKE", "GRID HUNT", "KART RACER"};
  const char* details[] = {"collect dots; do not hit yourself", "find the highlighted square", "time-trial go-kart racing"};
  tft.fillRect(0, CONTENT_Y, W, H - FOOTER_H - CONTENT_Y, ui.bg);
  tft.setTextSize(1);
  tft.setTextColor(ui.dim, ui.bg); tft.setCursor(12, CONTENT_Y + 8); tft.print("THREE OFFLINE MINI GAMES");
  for (int i = 0; i < 3; ++i) {
    int y = CONTENT_Y + 34 + i * 42; bool selected = i == gameMenuSelected; uint16_t bg = selected ? ui.selected : ui.bg;
    if (selected) tft.fillRoundRect(8, y - 5, 304, 34, 4, bg);
    tft.setTextColor(selected ? ui.text : ui.dim, bg); tft.setCursor(16, y); tft.print(selected ? "> " : "  "); tft.print(games[i]);
    tft.setTextColor(selected ? ui.text : ui.dim, bg); tft.setCursor(28, y + 13); tft.print(details[i]);
  }
}
void drawGridHuntGrid() {
  constexpr int cell = 46, left = 91, top = CONTENT_Y + 25;
  tft.setTextSize(1);
  tft.fillRect(0, CONTENT_Y, W, 20, ui.bg);
  tft.setTextColor(ui.accent, ui.bg); tft.setCursor(12, CONTENT_Y + 4); tft.print("SCORE " + String(huntScore));
  tft.setTextColor(ui.dim, ui.bg); tft.setCursor(132, CONTENT_Y + 4); tft.print("move to the bright square, then ENTER");
  for (int i = 0; i < 9; ++i) {
    int x = left + (i % 3) * cell, y = top + (i / 3) * cell;
    uint16_t fill = i == huntTarget ? ui.accent : ILI9341_DARKGREY;
    tft.fillRoundRect(x, y, cell - 5, cell - 5, 4, fill);
    tft.drawRoundRect(x, y, cell - 5, cell - 5, 4, i == huntCursor ? ILI9341_WHITE : ui.dim);
  }
}
void drawGameHub() {
  lastDrawnGameMode = gameMode;
  if (gameMode == 3) { drawKartHub(); return; }
  tft.fillScreen(ui.bg); header(gameMode == 1 ? "GAMES / SNAKE" : (gameMode == 2 ? "GAMES / GRID HUNT" : "GAMES"));
  tft.setTextSize(1);
  if (gameMode == 0) { drawGameMenuList(); footer(";/. SELECT     ENTER PLAY     FN BACK"); return; }
  if (gameMode == 1) {
    constexpr int cell = SNAKE_CELL, left = SNAKE_LEFT, top = SNAKE_TOP;
    tft.setTextColor(ui.accent, ui.bg); tft.setCursor(10, CONTENT_Y + 4); tft.print("SCORE " + String(snakeScore));
    tft.setTextColor(ui.dim, ui.bg); tft.setCursor(175, CONTENT_Y + 4); tft.print(snakeRunning ? ";,. / STEER" : gameStatus);
    tft.drawRect(left - 1, top - 1, SNAKE_COLS * cell + 2, SNAKE_ROWS * cell + 2, ui.dim);
    for (int i = 0; i < snakeLength; ++i) tft.fillRect(left + snakeX[i] * cell + 1, top + snakeY[i] * cell + 1, cell - 2, cell - 2, i == 0 ? ui.accent : ui.text);
    tft.fillCircle(left + snakeFoodX * cell + cell / 2, top + snakeFoodY * cell + cell / 2, 4, ILI9341_RED);
    footer(snakeRunning ? "; UP  , LEFT  / RIGHT  . DOWN" : "ENTER RESTART     FN GAMES");
    return;
  }
  drawGridHuntGrid();
  footer("; UP  , LEFT  / RIGHT  . DOWN  ENTER CATCH");
}
void startSnakeGame() {
  snakeLength = 3; snakeScore = 0; snakeDx = 1; snakeDy = 0;
  snakeX[0] = 8; snakeY[0] = 5; snakeX[1] = 7; snakeY[1] = 5; snakeX[2] = 6; snakeY[2] = 5;
  snakeFoodX = random(0, SNAKE_COLS); snakeFoodY = random(0, SNAKE_ROWS);
  snakeRunning = true; snakeNextMoveAt = millis() + 220UL; gameStatus = "";
}
// Snake ticks fast enough (every 180ms while running) that redrawing the
// whole panel through drawGameHub() every step was the main source of the
// reported flashing - it drove a fillScreen()+header()+footer() many times a
// second. These draw only the one or two cells that actually changed, the
// same "bypass the redrawNeeded machinery for continuous updates" approach
// stepKartRace() uses for its own live view.
void snakeEraseCell(int cx, int cy) { tft.fillRect(SNAKE_LEFT + cx * SNAKE_CELL, SNAKE_TOP + cy * SNAKE_CELL, SNAKE_CELL, SNAKE_CELL, ui.bg); }
void snakeDrawCell(int cx, int cy, uint16_t color) { tft.fillRect(SNAKE_LEFT + cx * SNAKE_CELL + 1, SNAKE_TOP + cy * SNAKE_CELL + 1, SNAKE_CELL - 2, SNAKE_CELL - 2, color); }
void snakeDrawFood() { tft.fillCircle(SNAKE_LEFT + snakeFoodX * SNAKE_CELL + SNAKE_CELL / 2, SNAKE_TOP + snakeFoodY * SNAKE_CELL + SNAKE_CELL / 2, 4, ILI9341_RED); }
void snakeRedrawStatus() {
  tft.fillRect(0, CONTENT_Y, W, SNAKE_TOP - CONTENT_Y, ui.bg);
  tft.setTextSize(1);
  tft.setTextColor(ui.accent, ui.bg); tft.setCursor(10, CONTENT_Y + 4); tft.print("SCORE " + String(snakeScore));
  tft.setTextColor(ui.dim, ui.bg); tft.setCursor(175, CONTENT_Y + 4); tft.print(snakeRunning ? ";,. / STEER" : gameStatus);
}
void stepSnakeGame() {
  // Draws straight to tft (see the comment on the helpers above), so it must
  // pause while the quick-launch overlay is covering the screen too.
  if (page != GAMEHUB || gameMode != 1 || quickMenuOpen || !snakeRunning || millis() < snakeNextMoveAt) return;
  snakeNextMoveAt = millis() + 180UL;
  int nextX = snakeX[0] + snakeDx, nextY = snakeY[0] + snakeDy;
  bool hit = nextX < 0 || nextX >= SNAKE_COLS || nextY < 0 || nextY >= SNAKE_ROWS;
  for (int i = 0; i < snakeLength; ++i) if (snakeX[i] == nextX && snakeY[i] == nextY) hit = true;
  // Crash changes the footer text too (steer hint -> restart prompt), which
  // this file only ever repaints through drawGameHub(), so route it through
  // the normal redrawNeeded/refreshLocalPage() path rather than drawing here.
  if (hit) { snakeRunning = false; gameStatus = "CRASH - ENTER RESTART"; playExitSound(); redrawNeeded = true; return; }

  int oldHeadX = snakeX[0], oldHeadY = snakeY[0];
  int oldTailX = snakeX[snakeLength - 1], oldTailY = snakeY[snakeLength - 1];
  bool ate = nextX == snakeFoodX && nextY == snakeFoodY;
  bool grew = ate && snakeLength < SNAKE_MAX;
  if (grew) snakeLength++;
  for (int i = snakeLength - 1; i > 0; --i) { snakeX[i] = snakeX[i - 1]; snakeY[i] = snakeY[i - 1]; }
  snakeX[0] = nextX; snakeY[0] = nextY;

  if (!grew) snakeEraseCell(oldTailX, oldTailY);   // tail cell vacated
  snakeDrawCell(oldHeadX, oldHeadY, ui.text);       // old head is now a body segment
  snakeDrawCell(nextX, nextY, ui.accent);           // new head

  if (ate) {
    snakeScore++;
    do { snakeFoodX = random(0, SNAKE_COLS); snakeFoodY = random(0, SNAKE_ROWS); } while ([&]() { for (int i = 0; i < snakeLength; ++i) if (snakeX[i] == snakeFoodX && snakeY[i] == snakeFoodY) return true; return false; }());
    playEnterSound();
    snakeDrawFood();
    snakeRedrawStatus();
  }
}
// ---- DEVICE CHECK: manual self-test menu (screen/speaker/keyboard/IR) ----
void drawDeviceCheck() {
  static const char* tests[] = {"SCREEN", "SPEAKER", "KEYBOARD", "IR EMITTER"};
  tft.fillScreen(ui.bg); header("DEVICE CHECK");
  tft.setTextSize(1); tft.setTextColor(ui.dim, ui.bg); tft.setCursor(12, CONTENT_Y + 7); tft.print("SELECT TEST; ENTER RUNS IT");
  for (int i = 0; i < 4; ++i) { int y = CONTENT_Y + 28 + i * 25; bool sel = i == deviceCheckSelected; uint16_t bg = sel ? ui.selected : ui.bg; if (sel) tft.fillRoundRect(8, y - 4, 304, 19, 4, bg); tft.setTextColor(sel ? ui.text : ui.dim, bg); tft.setCursor(15, y); tft.print(sel ? "> " : "  "); tft.print(tests[i]); }
  tft.setTextColor(ui.accent, ui.bg); tft.setCursor(12, CONTENT_Y + 140); tft.print(deviceCheckStatus);
  footer(";/. SELECT     ENTER TEST     FN BACK");
}
// ---- QR TOOLS+: shortcuts that build a payload then hand off to QR TEXT ---
void drawQRToolsPlus() {
  tft.fillScreen(ui.bg); header("QR TOOLS +");
  tft.setTextSize(1); tft.setTextColor(ui.dim, ui.bg); tft.setCursor(12, CONTENT_Y + 7); tft.print("SELECT A SOURCE, THEN ENTER");
  for (int i = 0; i < 4; ++i) {
    int y = CONTENT_Y + 28 + i * 25; bool selected = i == qrPlusSelected; uint16_t bg = selected ? ui.selected : ui.bg;
    if (selected) tft.fillRoundRect(8, y - 4, 304, 19, 4, bg);
    tft.setTextColor(selected ? ui.text : ui.dim, bg); tft.setCursor(15, y); tft.print(selected ? "> " : "  "); tft.print(qrPlusLabels[i]);
  }
  tft.setTextColor(ui.accent, ui.bg); tft.setCursor(12, CONTENT_Y + 142); tft.print("Creates an ASCII QR in QR TEXT.");
  footer(";/. SELECT     ENTER OPEN     FN BACK");
}
// ---- MINI PAINT: a 16x12 one-bit pixel grid, ENTER toggles the cell -------
void drawMiniPaint() {
  constexpr int cell = 14, left = 48, top = 35;
  tft.fillScreen(ui.bg); header("MINI PAINT");
  tft.setTextSize(1); tft.setTextColor(ui.dim, ui.bg); tft.setCursor(8, CONTENT_Y + 2); tft.print("16 x 12   ENTER INK   DEL CLEAR ALL");
  for (int y = 0; y < 12; ++y) for (int x = 0; x < 16; ++x) {
    int px = left + x * cell, py = top + y * cell; uint16_t fill = paintPixels[y][x] ? ui.accent : ILI9341_DARKGREY;
    tft.fillRect(px, py, cell - 1, cell - 1, fill);
  }
  tft.drawRect(left + paintX * cell - 1, top + paintY * cell - 1, cell + 1, cell + 1, ILI9341_WHITE);
  footer("; UP , LEFT / RIGHT . DOWN  ENTER INK");
}
bool searchMatches(int app) {
  if (launcherSearchText.isEmpty()) return true;
  String name = appNames[app], query = launcherSearchText; name.toLowerCase(); query.toLowerCase();
  return name.indexOf(query) >= 0;
}
int searchResultCount() { int count = 0; for (int i = 0; i < APP_COUNT; ++i) if (searchMatches(i)) count++; return count; }
int searchResultApp(int result) { int found = 0; for (int i = 0; i < APP_COUNT; ++i) if (searchMatches(i) && found++ == result) return i; return -1; }
// ---- LAUNCHER SEARCH: type-ahead filter over the full app list -----------
void drawLauncherSearch() {
  tft.fillScreen(ui.bg); header("LAUNCHER SEARCH");
  tft.fillRoundRect(8, CONTENT_Y + 4, 304, 22, 4, ILI9341_DARKGREY); tft.setTextSize(1); tft.setTextColor(ui.text, ILI9341_DARKGREY); tft.setCursor(13, CONTENT_Y + 11); tft.print(launcherSearchText); tft.print("_");
  int results = searchResultCount(); launcherSearchSelected = constrain(launcherSearchSelected, 0, max(0, results - 1));
  if (!results) { tft.setTextColor(ILI9341_YELLOW, ui.bg); tft.setCursor(12, CONTENT_Y + 48); tft.print("No matching applications."); }
  for (int row = 0; row < min(results, 7); ++row) {
    int app = searchResultApp(row); int y = CONTENT_Y + 42 + row * 20; bool selected = row == launcherSearchSelected; uint16_t bg = selected ? ui.selected : ui.bg;
    if (selected) tft.fillRoundRect(8, y - 3, 304, 16, 4, bg);
    tft.setTextColor(selected ? ui.text : ui.dim, bg); tft.setCursor(14, y); tft.print(selected ? "> " : "  "); tft.print(appNames[app]);
  }
  footer("TYPE FIND  ;/. SELECT  ENTER OPEN  DEL ERASE");
}
String browserPlainText(String source) {
  String out; bool inTag = false; bool space = false;
  for (int i = 0; i < (int)source.length() && out.length() < 1800; ++i) {
    char c = source[i];
    if (c == '<') { inTag = true; continue; }
    if (c == '>') { inTag = false; if (!space) { out += ' '; space = true; } continue; }
    if (inTag) continue;
    if (c == '&') { int semi = source.indexOf(';', i); if (semi > i && semi - i < 12) { out += ' '; i = semi; space = true; continue; } }
    if (c == '\r' || c == '\n' || c == '\t') c = ' ';
    if ((uint8_t)c < 32 || (uint8_t)c > 126) continue;
    if (c == ' ') { if (!space) { out += c; space = true; } } else { out += c; space = false; }
  }
  out.trim(); return out;
}
void fetchTextBrowserPage() {
  if (WiFi.status() != WL_CONNECTED) { browserText = "Wi-Fi is not connected. Open WEB COMPANION and press ENTER first."; browserScroll = 0; return; }
  if (!browserUrl.startsWith("http://")) { browserText = "Only plain http:// URLs are supported in this compact text browser."; browserScroll = 0; return; }
  HTTPClient http; http.setTimeout(7000);
  if (!http.begin(browserUrl)) { browserText = "Invalid HTTP URL."; browserScroll = 0; return; }
  int status = http.GET();
  if (status != HTTP_CODE_OK) browserText = "HTTP request failed: " + String(status);
  else browserText = browserPlainText(http.getString());
  http.end();
  if (browserText.isEmpty()) browserText = "No readable text found on this page.";
  browserScroll = 0;
}
String zabkaTotpCode(const String& hexSecret, uint64_t counter) {
  // The original Żabka web app treats `secret` as raw bytes written in HEX,
  // not as a Base32 TOTP provisioning secret.
  String normalized = "";
  for (int i = 0; i < (int)hexSecret.length(); ++i) {
    char c = hexSecret[i];
    if (isxdigit((unsigned char)c)) normalized += c;
    else if (c != ' ' && c != ':' && c != '-') return "";
  }
  if (normalized.isEmpty() || (normalized.length() & 1) || normalized.length() > 128) return "";
  uint8_t secret[64];
  const size_t secretLength = normalized.length() / 2;
  for (size_t i = 0; i < secretLength; ++i) {
    char pair[3] = {normalized[int(i * 2)], normalized[int(i * 2 + 1)], 0};
    char* end = nullptr; long byteValue = strtol(pair, &end, 16);
    if (!end || *end != 0) return "";
    secret[i] = uint8_t(byteValue);
  }
  uint8_t message[8], digest[20];
  for (int i = 7; i >= 0; --i) { message[i] = counter & 0xFF; counter >>= 8; }
  const mbedtls_md_info_t* sha1 = mbedtls_md_info_from_type(MBEDTLS_MD_SHA1);
  if (!sha1 || mbedtls_md_hmac(sha1, secret, secretLength, message, sizeof(message), digest) != 0) return "";
  int offset = digest[19] & 0x0F;
  uint32_t otp = ((uint32_t(digest[offset]) & 0x7F) << 24) | ((uint32_t(digest[offset + 1]) & 0xFF) << 16) | ((uint32_t(digest[offset + 2]) & 0xFF) << 8) | (uint32_t(digest[offset + 3]) & 0xFF);
  char result[7]; snprintf(result, sizeof(result), "%06lu", (unsigned long)(otp % 1000000UL));
  return String(result);
}
// ---- ZABKA TOTP: an encrypted TOTP/loyalty-code vault -------------------
// The vault is AES-encrypted at rest (mbedtls) behind zabkaUnlock; unlocking
// decrypts into RAM only, never writes plaintext back to flash.
void drawZabkaTotp() {
  tft.fillScreen(ui.bg); header("ZABKA QR"); tft.setTextSize(1);
  if (zabkaUnlocking) {
    tft.setTextColor(ui.accent, ui.bg); tft.setCursor(10, CONTENT_Y + 15); tft.print("UNLOCK SAVED SECRET");
    tft.setTextColor(ui.dim, ui.bg); tft.setCursor(10, CONTENT_Y + 33); tft.print("Type vault password on Cardputer");
    tft.fillRoundRect(8, CONTENT_Y + 45, 304, 24, 4, ILI9341_DARKGREY);
    String masked; for (int i = 0; i < (int)zabkaUnlockBuffer.length(); ++i) masked += '*';
    tft.setTextColor(ui.text, ILI9341_DARKGREY); tft.setCursor(14, CONTENT_Y + 53); tft.print(masked); tft.print("_");
    tft.setTextColor(ui.dim, ui.bg); tft.setCursor(10, CONTENT_Y + 91); tft.print("Password stays only in RAM for this try.");
    footer("ENTER UNLOCK  DEL ERASE  FN CANCEL");
    return;
  }
  time_t now = time(nullptr);
  bool clockReady = now > 1700000000;
  if (zabkaSecret.isEmpty() && zabkaVaultStored) {
    tft.setTextColor(ui.accent, ui.bg); tft.setCursor(10, CONTENT_Y + 16); tft.print("SAVED SECRET IS LOCKED");
    tft.setTextColor(ui.dim, ui.bg); tft.setCursor(10, CONTENT_Y + 40); tft.print("Press ENTER and type the vault password");
    tft.setCursor(10, CONTENT_Y + 56); tft.print("on this Cardputer keyboard.");
    tft.setTextColor(ui.text, ui.bg); tft.setCursor(10, CONTENT_Y + 91); tft.print(zabkaStatus.substring(0, 50));
    footer("ENTER UNLOCK  FN BACK");
    return;
  }
  tft.setTextColor(ui.accent, ui.bg); tft.setCursor(9, CONTENT_Y + 3); tft.print("SRLN LOYALTY QR");
  if (!clockReady) {
    tft.setTextColor(ILI9341_YELLOW, ui.bg); tft.setCursor(9, CONTENT_Y + 26); tft.print("Press ENTER to sync Wi-Fi time.");
  } else if (zabkaSecret.isEmpty()) {
    tft.setTextColor(ui.dim, ui.bg); tft.setCursor(9, CONTENT_Y + 26); tft.print("Paste/type the HEX secret first.");
  } else {
    String code = zabkaTotpCode(zabkaSecret, uint64_t(now) / 30ULL);
    if (code.isEmpty()) { tft.setTextColor(ILI9341_RED, ui.bg); tft.setCursor(9, CONTENT_Y + 26); tft.print("Invalid HEX secret."); }
    else {
      // The SRLN address plus the current code requires QR version 4 at low
      // error correction; this keeps the complete URL in the QR payload.
      String loyaltyUrl = "https://srln.pl/view/dashboard?ploy=" + zabkaPloyId + "&loyal=" + code;
      uint8_t data[qrcode_getBufferSize(4)]; QRCode qr; qrcode_initText(&qr, data, 4, ECC_LOW, loyaltyUrl.c_str());
      const int scale = 4, size = qr.size * scale, x = 12, y = CONTENT_Y + 39;
      tft.fillRect(x - 4, y - 4, size + 8, size + 8, ILI9341_WHITE);
      for (int row = 0; row < qr.size; ++row) for (int col = 0; col < qr.size; ++col) if (qrcode_getModule(&qr, col, row)) tft.fillRect(x + col * scale, y + row * scale, scale, scale, ILI9341_BLACK);
      tft.setTextSize(3); tft.setTextColor(ui.text, ui.bg); tft.setCursor(188, CONTENT_Y + 63); tft.print(code);
      tft.setTextSize(1); tft.setTextColor(ui.dim, ui.bg); tft.setCursor(188, CONTENT_Y + 91); tft.printf("New code in %ld s", 30L - (long(now) % 30L));
      tft.setCursor(188, CONTENT_Y + 107); tft.print("SRLN ploy: " + zabkaPloyId.substring(0, 15)); tft.setCursor(188, CONTENT_Y + 120); tft.print("Code is sent as loyal.");
    }
  }
  tft.setTextSize(1); tft.setTextColor(ui.dim, ui.bg); tft.setCursor(9, CONTENT_Y + 166); tft.print(zabkaStatus.substring(0, 50));
  footer(zabkaVaultStored ? "ENTER SYNC  TAB LOCK  DEL CLEAR  FN BACK" : "ENTER SYNC  DEL ERASE  FN BACK");
}
// ---- INPOST TRACK: looks up a courier tracking number over Wi-Fi ---------
void drawInPostTrack() {
  tft.fillScreen(ui.bg); header("INPOST TRACK");
  tft.setTextSize(1);
  tft.setTextColor(inpostEditingPickupCode ? ui.dim : ui.accent, ui.bg); tft.setCursor(9, CONTENT_Y + 2); tft.print("PARCEL NUMBER");
  uint16_t parcelBg = inpostEditingPickupCode ? ILI9341_DARKGREY : ui.selected;
  tft.fillRoundRect(8, CONTENT_Y + 8, 304, 18, 4, parcelBg);
  tft.setTextColor(ui.text, parcelBg); tft.setCursor(13, CONTENT_Y + 13);
  String shownNumber = inpostNumber; if (shownNumber.length() > 46) shownNumber = shownNumber.substring(shownNumber.length() - 46); tft.print(shownNumber); if (!inpostEditingPickupCode) tft.print("_");
  tft.setTextColor(inpostEditingPickupCode ? ui.accent : ui.dim, ui.bg); tft.setCursor(9, CONTENT_Y + 32); tft.print("PICKUP CODE (NOT SAVED)");
  uint16_t codeBg = inpostEditingPickupCode ? ui.selected : ILI9341_DARKGREY;
  tft.fillRoundRect(8, CONTENT_Y + 38, 304, 18, 4, codeBg);
  tft.setTextColor(ui.text, codeBg); tft.setCursor(13, CONTENT_Y + 43);
  String shownCode = inpostPickupCode; if (shownCode.length() > 46) shownCode = shownCode.substring(shownCode.length() - 46); tft.print(shownCode); if (inpostEditingPickupCode) tft.print("_");
  tft.setTextColor(ui.accent, ui.bg); tft.setCursor(9, CONTENT_Y + 69); tft.print("ONLINE STATUS");
  tft.setTextColor(ui.text, ui.bg); int start = inpostScroll * 48;
  for (int row = 0; row < 7; ++row) { int at = start + row * 48; if (at >= (int)inpostStatus.length()) break; tft.setCursor(9, CONTENT_Y + 83 + row * 14); tft.print(inpostStatus.substring(at, at + 48)); }
  footer("TAB FIELD  ENTER TRACK  CTRL+Q QR LINK  DEL ERASE");
}
void fetchInPostTracking() {
  if (inpostNumber.length() < 5) { inpostStatus = "Enter a valid parcel number first."; inpostScroll = 0; return; }
  if (WiFi.status() != WL_CONNECTED) { inpostStatus = "Wi-Fi is not connected. Open WEB COMPANION and press ENTER first."; inpostScroll = 0; return; }
  // This uses TLS without certificate validation because the compact firmware
  // has no managed CA store. It is read-only tracking; do not use it for login.
  WiFiClientSecure client; client.setInsecure();
  HTTPClient http; http.setTimeout(9000);
  String url = "https://inpost.pl/sledzenie-przesylek?number=" + inpostNumber;
  if (!http.begin(client, url)) { inpostStatus = "Could not start the secure InPost request."; inpostScroll = 0; return; }
  int status = http.GET();
  if (status == HTTP_CODE_OK) {
    String pageText = browserPlainText(http.getString());
    inpostStatus = pageText.isEmpty() ? "InPost page opened, but no readable status text was found." : pageText;
  } else inpostStatus = "InPost request failed: HTTP " + String(status) + ".";
  http.end(); inpostScroll = 0;
}
// ---- TEXT BROWSER: fetches a plain http:// page and strips it to text ----
// See browserPlainText()/fetchTextBrowserPage() above - a minimal tag
// stripper, not a real HTML renderer, and http:// only (no TLS).
void drawTextBrowser() {
  tft.fillScreen(ui.bg); header("TEXT BROWSER");
  tft.fillRoundRect(8, CONTENT_Y + 3, 304, 19, 4, ILI9341_DARKGREY); tft.setTextSize(1); tft.setTextColor(ui.text, ILI9341_DARKGREY); tft.setCursor(12, CONTENT_Y + 9); String shown = browserUrl; if (shown.length() > 47) shown = shown.substring(shown.length() - 47); tft.print(shown); tft.print("_");
  tft.setTextColor(ui.dim, ui.bg); int start = browserScroll * 48;
  for (int row = 0; row < 11; ++row) { int at = start + row * 48; if (at >= (int)browserText.length()) break; tft.setCursor(9, CONTENT_Y + 34 + row * 14); tft.print(browserText.substring(at, at + 48)); }
  footer("ENTER FETCH  ;/. SCROLL  DEL ERASE  FN BACK");
}
// ---- WI-FI SETUP: pick a network and enter its password to connect -------
void drawWifiSetup() {
  tft.fillScreen(ui.bg); header("WI-FI SETUP"); tft.setTextSize(1);
  if (wifiSetupEditingPassword) {
    tft.setTextColor(ui.accent, ui.bg); tft.setCursor(10, CONTENT_Y + 5); tft.print("PASSWORD FOR: " + wifiSetupSsid.substring(0, 30));
    tft.fillRoundRect(8, CONTENT_Y + 17, 304, 24, 4, ILI9341_DARKGREY);
    String masked; for (int i = 0; i < (int)wifiSetupPassword.length(); ++i) masked += '*';
    tft.setTextColor(ui.text, ILI9341_DARKGREY); tft.setCursor(14, CONTENT_Y + 25); tft.print(masked); tft.print("_");
    tft.setTextColor(ui.dim, ui.bg); tft.setCursor(10, CONTENT_Y + 59); tft.print("Password is hidden. ENTER CONNECTS.");
    footer("ENTER CONNECT  DEL ERASE  TAB NETWORKS  FN BACK"); return;
  }
  tft.setTextColor(WiFi.status() == WL_CONNECTED ? ILI9341_GREEN : ui.dim, ui.bg); tft.setCursor(10, CONTENT_Y + 4);
  tft.print(WiFi.status() == WL_CONNECTED ? "CONNECTED: " + WiFi.SSID() : "SELECT A SCANNED NETWORK");
  if (scanRunning) { tft.setTextColor(ui.accent, ui.bg); tft.setCursor(10, CONTENT_Y + 24); tft.print("Scanning nearby networks..."); footer("FN BACK"); return; }
  if (!scanDone) { tft.setTextColor(ui.dim, ui.bg); tft.setCursor(10, CONTENT_Y + 24); tft.print("ENTER starts a network scan."); footer("ENTER SCAN  FN BACK"); return; }
  if (networkCount <= 0) { tft.setTextColor(ILI9341_YELLOW, ui.bg); tft.setCursor(10, CONTENT_Y + 24); tft.print("No networks found. ENTER rescans."); footer("ENTER RESCAN  FN BACK"); return; }
  int first = constrain(wifiSetupSelected - 5, 0, max(0, networkCount - 10));
  for (int row = 0; row < 10 && first + row < networkCount; ++row) {
    int i = first + row, y = CONTENT_Y + 22 + row * 16; bool selected = i == wifiSetupSelected; uint16_t bg = selected ? ui.selected : ui.bg;
    if (selected) tft.fillRoundRect(8, y - 3, 304, 14, 3, bg);
    String ssid = WiFi.SSID(i); if (ssid.isEmpty()) ssid = "<hidden>"; if (ssid.length() > 28) ssid = ssid.substring(0, 28);
    tft.setTextColor(selected ? ui.text : ui.dim, bg); tft.setCursor(13, y); tft.print(selected ? "> " : "  "); tft.print(ssid);
    tft.setCursor(245, y); tft.printf("%d", WiFi.RSSI(i));
  }
  footer(";/. SELECT  ENTER PASSWORD  TAB RESCAN  FN BACK");
}
// ---- WEB COMPANION: runs a tiny local web server (webServer, WebServer.h) -
// Lets a phone/laptop on the same Wi-Fi send text or C LAB code to this
// device from a browser - see webServer.handleClient()/applyWebInput() in
// loop() and the pendingWebNote/pendingWebCLab hand-off globals.
void drawWebCompanion() {
  tft.fillScreen(ui.bg); header("WEB COMPANION"); tft.setTextSize(1); tft.setTextColor(webRunning ? ILI9341_GREEN : ui.accent, ui.bg); tft.setCursor(12, CONTENT_Y + 12); tft.print(webRunning ? "STA WEB LINK ACTIVE" : "STA WEB LINK STOPPED");
  tft.setTextColor(ui.text, ui.bg); tft.setCursor(12, CONTENT_Y + 30); tft.print(webStatus);
  tft.setTextColor(ui.dim, ui.bg); tft.setCursor(12, CONTENT_Y + 55); tft.print("Phone: join the same home Wi-Fi"); tft.setCursor(12, CONTENT_Y + 68); tft.print("then open cardputer-xl.local"); tft.setCursor(12, CONTENT_Y + 94); tft.print("Remote: arrows, Enter, Back, theme,"); tft.setCursor(12, CONTENT_Y + 107); tft.print("brightness, volume + text / C LAB.");
  footer(webRunning ? "ENTER STOP     FN BACK" : "ENTER CONNECT     FN BACK");
}
// ---- NOTES: 14 plain-text lines, persisted with the rest of app state ----
void drawNotes() {
  tft.fillScreen(ui.bg); header("NOTES"); tft.setTextSize(1); tft.setTextColor(ui.dim, ui.bg); tft.setCursor(7, CONTENT_Y); tft.print("Saved in flash memory:"); int first = max(0, noteLine - 16);
  for (int i = first; i < noteCount; i++) { int y = CONTENT_Y + 14 + (i - first) * 11; if (y > H - FOOTER_H - 10) break; bool cur = i == noteLine; if (cur) tft.fillRect(4, y - 1, W - 8, 10, ILI9341_DARKGREY); tft.setTextColor(ui.text, cur ? ILI9341_DARKGREY : ui.bg); tft.setCursor(8, y); tft.print(notes[i]); if (cur) tft.print("_"); } footer("FN BACK  ENTER NEW LINE  DEL ERASE  CTRL+L CLEAR");
}
void updateNotesLine() { int first = max(0, noteLine - 16), y = CONTENT_Y + 14 + (noteLine - first) * 11; if (y > H - FOOTER_H - 10) { redrawNeeded = true; return; } tft.fillRect(4, y - 1, W - 8, 10, ILI9341_DARKGREY); tft.setTextSize(1); tft.setTextColor(ui.text, ILI9341_DARKGREY); tft.setCursor(8, y); tft.print(notes[noteLine]); tft.print("_"); }
void updateClockValue() { tft.fillRect(34, 70, 254, 38, ui.bg); tft.setTextSize(4); tft.setTextColor(ui.accent, ui.bg); tft.setCursor(38, 76); tft.print(uptime()); }
// ---- CLOCK: uptime counter (no RTC sync, so it's elapsed time since boot) -
void drawClock() { tft.fillScreen(ui.bg); header("CLOCK"); updateClockValue(); tft.setTextSize(1); tft.setTextColor(ui.dim, ui.bg); tft.setCursor(56, 128); tft.print("UPTIME CLOCK"); tft.setCursor(25, 151); tft.print("Time begins at boot; no RTC sync."); footer("FN BACK"); }
void updateCalcPanel() { tft.fillRoundRect(10, CONTENT_Y + 20, 300, 43, 4, ILI9341_DARKGREY); String s = calcInput; if (s.length() > 13) s = s.substring(s.length() - 13); tft.setTextSize(3); tft.setTextColor(ui.accent, ILI9341_DARKGREY); tft.setCursor(18, CONTENT_Y + 32); tft.print(s); tft.fillRect(10, CONTENT_Y + 122, 300, 13, ui.bg); tft.setTextSize(1); tft.setTextColor(ui.dim, ui.bg); tft.setCursor(14, CONTENT_Y + 128); tft.print(calcStatus); }
// ---- CALCULATOR: simple left-to-right +-*/, one pending operator at a time
void drawCalc() { tft.fillScreen(ui.bg); header("CALCULATOR"); tft.setTextSize(1); tft.setTextColor(ui.dim, ui.bg); tft.setCursor(12, CONTENT_Y + 8); tft.print("Expression:"); updateCalcPanel(); tft.setTextColor(ui.text, ui.bg); tft.setCursor(14, CONTENT_Y + 84); tft.print("Keys: 0-9 .  + - * /"); tft.setCursor(14, CONTENT_Y + 101); tft.print("ENTER = result     DEL = clear"); footer("FN BACK"); }

void drawQRTextField() { tft.fillRoundRect(8, CONTENT_Y + 4, 304, 20, 4, ILI9341_DARKGREY); String shown = qrText; if (shown.length() > 46) shown = shown.substring(shown.length() - 46); tft.setTextSize(1); tft.setTextColor(ui.text, ILI9341_DARKGREY); tft.setCursor(13, CONTENT_Y + 11); tft.print(shown); tft.print("_"); }
// ---- QR TEXT: renders any typed text/URL as a QR code (the "QRCode"
// library vendored next to this file - see the file-header build note) ----
void drawQRModules() {
  uint8_t data[qrcode_getBufferSize(5)]; QRCode code; qrcode_initText(&code, data, 5, ECC_LOW, qrText.c_str());
  const int scale = 4, size = code.size * scale, x = (W - size) / 2, y = CONTENT_Y + 31;
  tft.fillRect(0, y, W, size, ui.bg); tft.fillRect(x - 4, y - 4, size + 8, size + 8, ILI9341_WHITE);
  for (int row = 0; row < code.size; ++row) for (int col = 0; col < code.size; ++col) if (qrcode_getModule(&code, col, row)) tft.fillRect(x + col * scale, y + row * scale, scale, scale, ILI9341_BLACK);
}
void drawQRText() { tft.fillScreen(ui.bg); header("QR TEXT"); tft.setTextSize(1); tft.setTextColor(ui.dim, ui.bg); tft.setCursor(8, CONTENT_Y - 6); tft.print("TEXT / URL (ASCII, max 106 characters)"); drawQRTextField(); drawQRModules(); footer("FN BACK  ENTER GENERATE  DEL ERASE"); }

int findCVar(const String& name) { for (int i = 0; i < cVarCount; i++) if (cVars[i].name == name) return i; return -1; }
int findCStringVar(const String& name) { for (int i = 0; i < cStringVarCount; i++) if (cStringVars[i].name == name) return i; return -1; }

// C LAB built-ins are refreshed whenever code runs and are deliberately read-only.
bool cBuiltinNumber(const String& name, long& value) {
  if (name == "battery") { int level = M5Cardputer.Power.getBatteryLevel(); value = level < 0 ? -1 : level; return true; }
  if (name == "uptime") { value = millis() / 1000UL; return true; }
  if (name == "free_heap_kb") { value = ESP.getFreeHeap() / 1024UL; return true; }
  if (name == "total_heap_kb") { value = ESP.getHeapSize() / 1024UL; return true; }
  if (name == "flash_mb") { value = ESP.getFlashChipSize() / 1048576UL; return true; }
  if (name == "wifi_connected") { value = WiFi.status() == WL_CONNECTED ? 1 : 0; return true; }
  if (name == "wifi_rssi") { value = WiFi.status() == WL_CONNECTED ? WiFi.RSSI() : 0; return true; }
  if (name == "wifi_networks") { int result = WiFi.scanComplete(); value = result >= 0 ? result : 0; return true; }
  return false;
}
bool cBuiltinText(const String& name, String& value) {
  if (name == "device_name") { value = "M5Stack Cardputer XL"; return true; }
  if (name == "theme_name") { value = themeNames[themeIndex]; return true; }
  if (name == "uptime_text") { value = uptime(); return true; }
  if (name == "wifi_ssid") { value = WiFi.status() == WL_CONNECTED ? WiFi.SSID() : ""; return true; }
  return false;
}
bool isCBuiltinName(const String& name) { long number; String text; return cBuiltinNumber(name, number) || cBuiltinText(name, text); }

String cTextExpr(String expression, bool& ok) {
  String e = expression; e.trim(); ok = true; String result = "";
  bool inQuotes = false; int start = 0;
  for (int pos = 0; pos <= (int)e.length(); ++pos) {
    if (pos < (int)e.length() && e[pos] == '"') inQuotes = !inQuotes;
    if (pos == (int)e.length() || (!inQuotes && e[pos] == '+')) {
      String part = e.substring(start, pos); part.trim();
      if (part.isEmpty()) { ok = false; return ""; }
      if (part.length() >= 2 && part.charAt(0) == '"' && part.charAt(part.length() - 1) == '"') {
        result += part.substring(1, part.length() - 1);
      } else {
        int stringVar = findCStringVar(part);
        if (stringVar >= 0) result += cStringVars[stringVar].value;
        else { String builtinText; if (cBuiltinText(part, builtinText)) { result += builtinText; start = pos + 1; continue; }
          bool numberOk = true;
          long number = cExpr(part, numberOk);
          if (!numberOk) { ok = false; return ""; }
          result += String(number);
        }
      }
      start = pos + 1;
    }
  }
  if (inQuotes) ok = false;
  return result;
}
long cAtom(String token, bool& ok) { token.trim(); if (token.length() == 0) { ok = false; return 0; } int i = findCVar(token); if (i >= 0) return cVars[i].value; long builtinValue; if (cBuiltinNumber(token, builtinValue)) return builtinValue; char* endPtr; long n = strtol(token.c_str(), &endPtr, 10); if (*endPtr == 0) return n; ok = false; return 0; }
long cExpr(String expression, bool& ok) {
  String e = expression; e.replace(" ", ""); e.replace("\t", ""); ok = true; if (e.isEmpty()) { ok = false; return 0; } int pos = 0;
  auto readAtom = [&](long& value) -> bool { if (pos >= (int)e.length()) return false; int start = pos; if (e[pos] == '-' || e[pos] == '+') pos++; while (pos < (int)e.length() && e[pos] != '+' && e[pos] != '-' && e[pos] != '*' && e[pos] != '/' && e[pos] != '%') pos++; String token = e.substring(start, pos); bool atomOk = true; value = cAtom(token, atomOk); return atomOk; };
  long term = 0; if (!readAtom(term)) { ok = false; return 0; } long total = 0; char addOp = '+';
  while (pos < (int)e.length()) { char op = e[pos++]; long rhs = 0; if (!readAtom(rhs)) { ok = false; return 0; } if (op == '*' || op == '/' || op == '%') { if ((op == '/' || op == '%') && rhs == 0) { ok = false; return 0; } if (op == '*') term *= rhs; else if (op == '/') term /= rhs; else term %= rhs; } else if (op == '+' || op == '-') { total = addOp == '+' ? total + term : total - term; term = rhs; addOp = op; } else { ok = false; return 0; } }
  return addOp == '+' ? total + term : total - term;
}

// CardC conditions deliberately support numbers only, with no nesting or logic operators.
bool cCondition(String condition, bool& ok) {
  condition.trim();
  const char* operators[] = {"==", "!=", "<=", ">=", "<", ">"};
  for (const char* op : operators) {
    int at = condition.indexOf(op);
    if (at < 0) continue;
    bool leftOk = true, rightOk = true;
    long left = cExpr(condition.substring(0, at), leftOk);
    long right = cExpr(condition.substring(at + strlen(op)), rightOk);
    ok = leftOk && rightOk;
    if (!ok) return false;
    if (!strcmp(op, "==")) return left == right;
    if (!strcmp(op, "!=")) return left != right;
    if (!strcmp(op, "<=")) return left <= right;
    if (!strcmp(op, ">=")) return left >= right;
    if (!strcmp(op, "<")) return left < right;
    return left > right;
  }
  ok = false;
  return false;
}
void cLog(const String& line) { if (cOutputCount < 5) cOutput[cOutputCount++] = line; else { for (int i = 0; i < 4; i++) cOutput[i] = cOutput[i + 1]; cOutput[4] = line; } }

// Local scan only: it never connects, sends credentials, or stores network data.
// Built-in Cardputer ADV IR emitter is wired to GPIO44. This sends a standard
// 32-bit NEC frame from an 8-bit address and an 8-bit command; it never sends
// automatically or receives IR (the board has no built-in IR receiver).
constexpr uint8_t IR_TX_PIN = 44;
constexpr uint32_t IR_CARRIER_HZ = 38000;
void irMark(uint16_t usec) { ledcWrite(IR_TX_PIN, 128); delayMicroseconds(usec); }
void irSpace(uint16_t usec) { ledcWrite(IR_TX_PIN, 0); delayMicroseconds(usec); }
void sendNec(uint8_t address, uint8_t command) {
  uint32_t frame = uint32_t(address) | (uint32_t(uint8_t(~address)) << 8) |
                   (uint32_t(command) << 16) | (uint32_t(uint8_t(~command)) << 24);
  irMark(9000); irSpace(4500);
  for (int bit = 0; bit < 32; ++bit) {
    irMark(560);
    irSpace((frame & (uint32_t(1) << bit)) ? 1690 : 560);
  }
  irMark(560); irSpace(0);
}

void cWifiScan() {
  WiFi.mode(WIFI_STA);
  WiFi.disconnect();
  delay(100);
  WiFi.scanDelete();
  int found = WiFi.scanNetworks();
  if (found < 0) { cLog("ERR: Wi-Fi scan failed"); return; }
  cLog("Wi-Fi: " + String(found) + " network(s)");
  for (int i = 0; i < min(found, 4); ++i) {
    String ssid = WiFi.SSID(i);
    if (ssid.isEmpty()) ssid = "<hidden>";
    if (ssid.length() > 20) ssid = ssid.substring(0, 20);
    cLog(ssid + " " + String(WiFi.RSSI(i)) + "dB");
  }
}

// CardC audio is intentionally limited to a single short tone. It uses the
// Cardputer speaker and obeys the global Settings > Volume level.
void cBeep(long frequencyHz, long durationMs) {
  if (!volumeLevel) return;
  M5Cardputer.Speaker.stop();
  M5Cardputer.Speaker.tone((uint16_t)frequencyHz, (uint32_t)durationMs);
}

// ---- C LAB / CardC draw + interpreter functions (see the "C LAB" overview
// comment near C_MAX_LINES above for what this whole subsystem is) --------
// CardC Canvas colour indexes: 0 BLACK, 1 WHITE, 2 CYAN, 3 RED, 4 GREEN,
// 5 BLUE, 6 YELLOW, 7 MAGENTA, 8 ORANGE. Keeping a fixed palette avoids
// raw display-driver access while still making small visual programs useful.
uint16_t cCanvasColor(uint8_t color) {
  static const uint16_t palette[] = {ILI9341_BLACK, ILI9341_WHITE, ILI9341_CYAN,
    ILI9341_RED, ILI9341_GREEN, ILI9341_BLUE, ILI9341_YELLOW, ILI9341_MAGENTA, 0xFD20};
  return palette[constrain(color, 0, 8)];
}
void drawCardCCanvas() {
  tft.fillScreen(ILI9341_BLACK);
  tft.setTextSize(1); tft.setTextColor(ILI9341_CYAN, ILI9341_BLACK);
  tft.setCursor(6, 6); tft.print("CARDC CANVAS  FN: RETURN TO C LAB");
  tft.drawFastHLine(0, 18, W, ILI9341_DARKGREY);
}
void cCanvasBegin() { cCanvasActive = true; drawCardCCanvas(); }
void cCanvasClear(uint8_t color) { if (cCanvasActive) tft.fillRect(0, 19, W, H - 19, cCanvasColor(color)); }
void cCanvasRect(long x, long y, long w, long h, uint8_t color, bool filled) {
  if (!cCanvasActive || w < 1 || h < 1) return;
  if (filled) tft.fillRect(constrain(x, 0, W - 1), constrain(y, 19, H - 1), min(w, long(W)), min(h, long(H - 19)), cCanvasColor(color));
  else tft.drawRect(constrain(x, 0, W - 1), constrain(y, 19, H - 1), min(w, long(W)), min(h, long(H - 19)), cCanvasColor(color));
}
void cCanvasCircle(long x, long y, long radius, uint8_t color, bool filled) {
  if (!cCanvasActive || radius < 1 || radius > 160) return;
  if (filled) tft.fillCircle(constrain(x, 0, W - 1), constrain(y, 19, H - 1), radius, cCanvasColor(color));
  else tft.drawCircle(constrain(x, 0, W - 1), constrain(y, 19, H - 1), radius, cCanvasColor(color));
}
void cCanvasLine(long x1, long y1, long x2, long y2, uint8_t color) {
  if (cCanvasActive) tft.drawLine(constrain(x1, 0, W - 1), constrain(y1, 19, H - 1), constrain(x2, 0, W - 1), constrain(y2, 19, H - 1), cCanvasColor(color));
}
void cCanvasPixel(long x, long y, uint8_t color) { if (cCanvasActive && x >= 0 && x < W && y >= 19 && y < H) tft.drawPixel(x, y, cCanvasColor(color)); }
void cCanvasPause(long durationMs) { if (cCanvasActive) delay(constrain(durationMs, 1L, 1000L)); }

// Ctrl+D loads this compact, safe example. Ctrl+Enter runs it.
void loadCLabDemo() {
  const char* demo[] = {
    "// C LAB QUICK DEMO",
    "int x = 7;",
    "x++;",
    "String name = input(\"Your name?\");",
    "",
    "void greet() {",
    "  println(\"Hi \" + name + \"! x=\" + x);",
    "}",
    "",
    "void setup() {",
    "  println(\"setup runs once\");",
    "}",
    "",
    "void loop() {",
    "  if (x > 0) println(\"x is positive\");",
    "  while (x < 10) x++;",
    "  println(\"x after while: \" + x);",
    "}",
    "",
    "greet();",
    "println(device_name);",
    "println(\"Battery: \" + battery + \"%\");",
    "beep(880, 120);",
    "led(0, 80, 255);  // blue LED while the demo runs",
    "wait(150);",
    "toast(\"Canvas is opening\");",
    "canvas_begin();",
    "canvas_clear(0);",
    "fill_circle(160, 120, 28, 2);",
    "canvas_pause(400);",
    "println(\"Canvas shown\");",
    "halt(0);",
    "// prgrestart(); restarts this CardC program (max 4)."
  };
  cLineCount = sizeof(demo) / sizeof(demo[0]);
  for (int i = 0; i < cLineCount; ++i) cLines[i] = demo[i];
  for (int i = cLineCount; i < C_MAX_LINES; ++i) cLines[i] = "";
  cCursorLine = 0; cCursorColumn = 0; cScrollLine = 0; cHorizontalScroll = 0;
  cOutputCount = 0; cInputActive = false; cInputValueCount = 0; cInputReadIndex = 0;
  cLabQrActive = false; cLabQrPayload = "";
  cLog("[demo loaded: Ctrl+Enter]");
  cLabExplorerVisible = false;
  cLabDirty = true;
  markStateDirty();
  playFunctionSound();
  redrawNeeded = true;
}

void runCLab() {
  if (!cProgramRestarting) cProgramRestartCount = 0;
  cProgramRestarting = false;
  cProgramHalted = false;
  cProgramRestartRequested = false;
  cProgramExitCode = 0;
  cCanvasActive = false;
  cOutputCount = 0; cVarCount = 0; cStringVarCount = 0; cInputReadIndex = 0; cInputActive = false; bool hasError = false; int whileIterations = 0; String source;
  for (int i = 0; i < cLineCount; i++) { String sourceLine = cLines[i]; int comment = sourceLine.indexOf("//"); if (comment >= 0) sourceLine.remove(comment); source += sourceLine; source += '\n'; }
  cFunctionCount = 0; int functionAt = source.indexOf("void ");
  while (functionAt >= 0 && !hasError) {
    int openParen = source.indexOf('(', functionAt + 5), closeParen = openParen < 0 ? -1 : source.indexOf(')', openParen + 1), openBrace = closeParen < 0 ? -1 : source.indexOf('{', closeParen + 1);
    if (openParen < 0 || closeParen < 0 || openBrace < 0) { cLog("ERR: bad void function"); hasError = true; break; }
    String name = source.substring(functionAt + 5, openParen), parameters = source.substring(openParen + 1, closeParen); name.trim(); parameters.trim(); int depth = 1, closeBrace = openBrace + 1;
    while (closeBrace < (int)source.length() && depth > 0) { if (source[closeBrace] == '{') depth++; else if (source[closeBrace] == '}') depth--; closeBrace++; }
    if (depth != 0 || !parameters.isEmpty() || name.isEmpty() || cFunctionCount >= 8) { cLog("ERR: void needs name() { ... }"); hasError = true; break; }
    cFunctions[cFunctionCount++] = {name, source.substring(openBrace + 1, closeBrace - 1)}; source.remove(functionAt, closeBrace - functionAt); functionAt = source.indexOf("void ");
  }
  // If Arduino-style functions are present, run setup() once and loop() once.
  // Plain statements still run directly, so both C LAB styles remain valid.
  for (int i = 0; i < cFunctionCount; ++i) {
    if (cFunctions[i].name == "setup" || cFunctions[i].name == "loop") {
      source += "\n" + cFunctions[i].body + ";";
    }
  }
  int functionCalls = 0, from = 0;
  while (from < (int)source.length() && !hasError && !cProgramHalted && !cProgramRestartRequested) {
    int to = source.indexOf(';', from); if (to < 0) to = source.length(); String line = source.substring(from, to); line.trim(); from = to + 1; if (line.isEmpty()) continue;
    if (line == "clear()") { cOutputCount = 0; continue; } if (line == "help()") { cLog("String x=input(\"name?\"); println(x);"); continue; }
    if (line.startsWith("if (" ) || line.startsWith("while (")) {
      bool isWhile = line.startsWith("while (");
      int open = line.indexOf('('), close = line.indexOf(')', open + 1);
      if (open < 0 || close < 0) { cLog("ERR: bad if/while syntax"); hasError = true; continue; }
      String body = line.substring(close + 1); body.trim();
      bool conditionOk = true; bool trueNow = cCondition(line.substring(open + 1, close), conditionOk);
      if (!conditionOk || body.isEmpty() || body.indexOf('{') >= 0 || body.indexOf('}') >= 0) { cLog("ERR: if/while needs one statement body"); hasError = true; continue; }
      if (trueNow) {
        if (isWhile) {
          if (++whileIterations > 32) { cLog("ERR: while limit (32)"); hasError = true; continue; }
          // Queue the body first, then re-check this same while statement.
          // The original statement has already been consumed, so do not add it twice.
          source = source.substring(0, from) + body + ";" + line + ";" + source.substring(from);
        } else source = source.substring(0, from) + body + ";" + source.substring(from);
      }
      continue;
    }
    if (line == "imuread()") {
      M5.Imu.update();
      auto imu = M5.Imu.getImuData();
      cLog("ACC " + String(imu.accel.x, 2) + "," + String(imu.accel.y, 2) + "," + String(imu.accel.z, 2));
      cLog("GYR " + String(imu.gyro.x, 1) + "," + String(imu.gyro.y, 1) + "," + String(imu.gyro.z, 1));
      continue;
    }
    if (line.startsWith("ifkey(")) {
      int keyOpen = line.indexOf('('), keyClose = line.indexOf(')', keyOpen + 1), doAt = keyClose < 0 ? -1 : line.indexOf("do(", keyClose + 1), actionClose = line.lastIndexOf(')');
      String key = keyClose > keyOpen ? line.substring(keyOpen + 1, keyClose) : "";
      key.trim();
      if (key.length() != 3 || key.charAt(0) != '\"' || key.charAt(2) != '\"' || doAt < 0 || actionClose <= doAt + 3) { cLog("ERR: ifkey(\"k\")do(action())"); hasError = true; continue; }
      String action = line.substring(doAt + 3, actionClose); action.trim();
      if (M5Cardputer.Keyboard.isKeyPressed(key.charAt(1))) source = source.substring(0, from) + action + ";" + source.substring(from);
      continue;
    }
    if (line == "wifi_scan()") { cWifiScan(); continue; }
    if (line == "canvas_begin()") { cCanvasBegin(); cLog("Canvas ready (Fn returns)"); continue; }
    // Canvas numeric calls use comma-separated integer expressions only.
    // Shapes are clipped by the display driver and cannot access GPIO or files.
    auto canvasArgs = [&](const String& call, long* values, int count) -> bool {
      int open = call.indexOf('('), close = call.lastIndexOf(')');
      if (open < 0 || close <= open) return false;
      int start = open + 1;
      for (int n = 0; n < count; ++n) {
        int comma = n + 1 < count ? call.indexOf(',', start) : close;
        if (comma < start || comma > close) return false;
        bool valueOk = true; values[n] = cExpr(call.substring(start, comma), valueOk);
        if (!valueOk) return false;
        start = comma + 1;
      }
      return start == close + 1;
    };
    if (line.startsWith("canvas_clear(")) { long v[1]; if (!canvasArgs(line, v, 1) || v[0] < 0 || v[0] > 8) { cLog("ERR: canvas_clear(color 0-8)"); hasError = true; } else cCanvasClear(v[0]); continue; }
    if (line.startsWith("rect(") || line.startsWith("fill_rect(")) { long v[5]; if (!canvasArgs(line, v, 5) || v[4] < 0 || v[4] > 8) { cLog("ERR: rect(x,y,w,h,color)"); hasError = true; } else cCanvasRect(v[0], v[1], v[2], v[3], v[4], line.startsWith("fill_")); continue; }
    if (line.startsWith("circle(") || line.startsWith("fill_circle(")) { long v[4]; if (!canvasArgs(line, v, 4) || v[3] < 0 || v[3] > 8) { cLog("ERR: circle(x,y,r,color)"); hasError = true; } else cCanvasCircle(v[0], v[1], v[2], v[3], line.startsWith("fill_")); continue; }
    if (line.startsWith("line(")) { long v[5]; if (!canvasArgs(line, v, 5) || v[4] < 0 || v[4] > 8) { cLog("ERR: line(x1,y1,x2,y2,color)"); hasError = true; } else cCanvasLine(v[0], v[1], v[2], v[3], v[4]); continue; }
    if (line.startsWith("pixel(")) { long v[3]; if (!canvasArgs(line, v, 3) || v[2] < 0 || v[2] > 8) { cLog("ERR: pixel(x,y,color)"); hasError = true; } else cCanvasPixel(v[0], v[1], v[2]); continue; }
    if (line.startsWith("canvas_pause(")) { long v[1]; if (!canvasArgs(line, v, 1) || v[0] < 1 || v[0] > 1000) { cLog("ERR: canvas_pause(ms 1-1000)"); hasError = true; } else cCanvasPause(v[0]); continue; }
    if (line.startsWith("toast(")) {
      int open = line.indexOf('('), close = line.lastIndexOf(')');
      if (open < 0 || close <= open) { cLog("ERR: toast(\"text\")"); hasError = true; continue; }
      bool textOk = true;
      String message = cTextExpr(line.substring(open + 1, close), textOk);
      if (!textOk || message.isEmpty()) { cLog("ERR: toast needs text"); hasError = true; }
      else queueToast(message);
      continue;
    }
    if (line.startsWith("wait(")) {
      long v[1];
      if (!canvasArgs(line, v, 1) || v[0] < 1 || v[0] > 10000) { cLog("ERR: wait(ms 1-10000)"); hasError = true; }
      else delay((uint32_t)v[0]);
      continue;
    }
    if (line.startsWith("halt(")) {
      long v[1];
      if (!canvasArgs(line, v, 1)) { cLog("ERR: halt(exitcode)"); hasError = true; }
      else { cProgramExitCode = v[0]; cProgramHalted = true; }
      continue;
    }
    if (line == "prgrestart()") {
      if (cProgramRestartCount >= 4) { cLog("ERR: prgrestart limit (4)"); hasError = true; }
      else cProgramRestartRequested = true;
      continue;
    }
    if (line.startsWith("led(")) {
      long v[3];
      if (!canvasArgs(line, v, 3) || v[0] < 0 || v[0] > 255 || v[1] < 0 || v[1] > 255 || v[2] < 0 || v[2] > 255) {
        cLog("ERR: led(r,g,b): each value 0-255");
        hasError = true;
      } else {
        setCardCLed((uint8_t)v[0], (uint8_t)v[1], (uint8_t)v[2]);
        cLog("LED: " + String(v[0]) + "," + String(v[1]) + "," + String(v[2]));
      }
      continue;
    }
    if (line.startsWith("vibrate(")) {
      long v[1];
      if (!canvasArgs(line, v, 1) || v[0] < 1 || v[0] > 1000) {
        cLog("ERR: vibrate(ms 1-1000)");
        hasError = true;
      } else {
        vibrate((uint16_t)v[0]);
        cLog("Vibrate: " + String(v[0]) + " ms");
      }
      continue;
    }
    if (line == "beep()") {
      cBeep(1000, 80);
      cLog("Beep: 1000 Hz, 80 ms");
      continue;
    }
    if (line.startsWith("beep(")) {
      int open = line.indexOf('('), comma = line.indexOf(',', open + 1), close = line.lastIndexOf(')');
      if (open < 0 || comma < 0 || close <= comma) { cLog("ERR: beep(hz, ms)"); hasError = true; continue; }
      bool frequencyOk = true, durationOk = true;
      long frequencyHz = cExpr(line.substring(open + 1, comma), frequencyOk);
      long durationMs = cExpr(line.substring(comma + 1, close), durationOk);
      if (!frequencyOk || !durationOk || frequencyHz < 40 || frequencyHz > 8000 || durationMs < 1 || durationMs > 2000) {
        cLog("ERR: beep: Hz 40-8000, ms 1-2000"); hasError = true; continue;
      }
      cBeep(frequencyHz, durationMs);
      cLog("Beep: " + String(frequencyHz) + " Hz, " + String(durationMs) + " ms");
      continue;
    }
    if (line.startsWith("ir_nec(")) {
      int open = line.indexOf('('), comma = line.indexOf(',', open + 1), close = line.lastIndexOf(')');
      if (open < 0 || comma < 0 || close <= comma) { cLog("ERR: ir_nec(addr, cmd)"); hasError = true; continue; }
      bool addressOk = true, commandOk = true;
      long address = cExpr(line.substring(open + 1, comma), addressOk);
      long command = cExpr(line.substring(comma + 1, close), commandOk);
      if (!addressOk || !commandOk || address < 0 || address > 255 || command < 0 || command > 255) {
        cLog("ERR: IR values must be 0-255"); hasError = true; continue;
      }
      sendNec((uint8_t)address, (uint8_t)command);
      cLog("IR NEC sent: " + String(address) + ", " + String(command));
      continue;
    }
    if (line.startsWith("String ")) {
      if (cStringVarCount >= 8) { cLog("ERR: max 8 text variables"); hasError = true; continue; }
      String declaration = line.substring(7); declaration.trim(); int eq = declaration.indexOf('='); String name = eq < 0 ? declaration : declaration.substring(0, eq); name.trim();
      if (name.isEmpty() || isCBuiltinName(name) || findCStringVar(name) >= 0 || findCVar(name) >= 0) { cLog("ERR: reserved or bad String name"); hasError = true; continue; }
      String value = "";
      if (eq >= 0) { String initializer = declaration.substring(eq + 1); initializer.trim();
        if ((initializer.startsWith("input(") || initializer.startsWith("readkey(")) && initializer.endsWith(")")) {
          bool keyInput = initializer.startsWith("readkey(");
          int prefix = keyInput ? 8 : 6;
          String promptPart = initializer.substring(prefix, initializer.length() - 1); bool promptOk; String prompt = cTextExpr(promptPart, promptOk);
          if (!promptOk) { cLog("ERR: input needs text prompt"); hasError = true; continue; }
          if (cInputReadIndex >= cInputValueCount) { cInputActive = true; cInputNumeric = false; cInputReadKey = keyInput; cInputPrompt = prompt; cInputBuffer = ""; redrawNeeded = true; return; }
          value = cInputValues[cInputReadIndex++];
        } else { bool textOk; value = cTextExpr(initializer, textOk); if (!textOk) { cLog("ERR: bad String value"); hasError = true; continue; } }
      }
      cStringVars[cStringVarCount++] = {name, value};
    }
    else if (line.startsWith("int ")) {
      if (cVarCount >= 12) { cLog("ERR: max 12 variables"); hasError = true; continue; }
      String declaration = line.substring(4); declaration.trim(); int eq = declaration.indexOf('=');
      String name = eq < 0 ? declaration : declaration.substring(0, eq); name.trim();
      String initializer = eq < 0 ? "" : declaration.substring(eq + 1); initializer.trim();
      bool ok = true; long value = 0;
      if (initializer.startsWith("inputint(") && initializer.endsWith(")")) {
        String promptPart = initializer.substring(9, initializer.length() - 1); bool promptOk;
        String prompt = cTextExpr(promptPart, promptOk);
        if (!promptOk) { cLog("ERR: inputint needs text prompt"); hasError = true; continue; }
        if (cInputReadIndex >= cInputValueCount) { cInputActive = true; cInputNumeric = true; cInputReadKey = false; cInputPrompt = prompt; cInputBuffer = ""; redrawNeeded = true; return; }
        String number = cInputValues[cInputReadIndex++]; number.trim(); char* end = nullptr;
        value = strtol(number.c_str(), &end, 10); ok = !number.isEmpty() && end && *end == 0;
      } else if (eq >= 0) value = cExpr(initializer, ok);
      if (!ok || name.isEmpty() || isCBuiltinName(name) || findCVar(name) >= 0 || findCStringVar(name) >= 0) { cLog("ERR: reserved, bad int name, or inputint value"); hasError = true; continue; }
      cVars[cVarCount++] = {name, value};
    }
    else if (line.endsWith("++") || line.endsWith("--")) { String name = line.substring(0, line.length() - 2); name.trim(); int var = findCVar(name); if (var < 0) { cLog("ERR: unknown variable"); hasError = true; } else cVars[var].value += line.endsWith("++") ? 1 : -1; }
    else if (line.startsWith("qrcode(")) { int open = line.indexOf('('), close = line.lastIndexOf(')'); if (close <= open) { cLog("ERR: bad qrcode call"); hasError = true; continue; } String argument = line.substring(open + 1, close); argument.trim();
      // qrcode(input("URL?")) opens the existing safe text field. This avoids
      // having to edit long URLs inside a 48-character C LAB source line.
      String text;
      if (argument.startsWith("input(") && argument.endsWith(")")) {
        String promptPart = argument.substring(6, argument.length() - 1); bool promptOk; String prompt = cTextExpr(promptPart, promptOk);
        if (!promptOk) { cLog("ERR: input needs text prompt"); hasError = true; continue; }
        if (cInputReadIndex >= cInputValueCount) { cInputActive = true; cInputPrompt = prompt; cInputBuffer = ""; redrawNeeded = true; return; }
        text = cInputValues[cInputReadIndex++];
      } else { bool ok; text = cTextExpr(argument, ok); if (!ok) { cLog("ERR: bad qrcode text"); hasError = true; continue; } }
      if (text.isEmpty() || text.length() > QR_MAX_TEXT) { cLog("ERR: qrcode needs 1-106 ASCII chars"); hasError = true; } else { bool ascii = true; for (int i = 0; i < (int)text.length(); ++i) if ((uint8_t)text[i] < 32 || (uint8_t)text[i] > 126) ascii = false; if (!ascii) { cLog("ERR: qrcode uses ASCII only"); hasError = true; } else { cLabQrPayload = text; cLabQrActive = true; cLog("[QR ready - FN returns]"); } } }
    else if (line.startsWith("print(") || line.startsWith("println(") || line.startsWith("puts(") || line.startsWith("printf(")) { int open = line.indexOf('('), close = line.lastIndexOf(')'); if (close <= open) { cLog("ERR: bad print call"); hasError = true; continue; } String argument = line.substring(open + 1, close); bool ok; String text = cTextExpr(argument, ok); if (!ok) { cLog("ERR: bad print expression"); hasError = true; } else cLog(text); }
    else if (line.startsWith("hello.world(")) { int open = line.indexOf('('), close = line.lastIndexOf(')'); String argument = close > open ? line.substring(open + 1, close) : ""; argument.trim(); if (!argument.startsWith("\"") || !argument.endsWith("\"") || argument.length() < 2) { cLog("ERR: hello.world needs text"); hasError = true; } else cLog(argument.substring(1, argument.length() - 1)); }
    else if (line.endsWith("()")) { String functionName = line.substring(0, line.length() - 2); functionName.trim(); int functionIndex = -1; for (int i = 0; i < cFunctionCount; i++) if (cFunctions[i].name == functionName) { functionIndex = i; break; } if (functionIndex < 0 || ++functionCalls > 16) { cLog(functionIndex < 0 ? "ERR: unknown function" : "ERR: function depth"); hasError = true; } else source = source.substring(0, from) + cFunctions[functionIndex].body + ";" + source.substring(from); }
    else { int eq = line.indexOf('='); if (eq <= 0) { cLog("ERR: unknown statement"); hasError = true; continue; } String name = line.substring(0, eq); name.trim(); int var = findCVar(name); bool ok; long value = cExpr(line.substring(eq + 1), ok); if (var < 0 || !ok) { cLog("ERR: bad assignment"); hasError = true; } else cVars[var].value = value; }
  }
  if (cProgramRestartRequested && !hasError) {
    cProgramRestartCount++;
    cProgramRestarting = true;
    cLog("[restarting]");
    runCLab();
    return;
  }
  if (cProgramHalted && !hasError) cLog("[halt: exit " + String(cProgramExitCode) + "]");
  else if (!hasError) cLog("[done]");
  redrawNeeded = true;
}
void drawCLabInput() {
  tft.fillScreen(ui.bg); header(cInputReadKey ? "C LAB READKEY" : "C LAB INPUT"); tft.setTextSize(1); tft.setTextColor(ui.accent, ui.bg); tft.setCursor(12, CONTENT_Y + 12); tft.print(cInputPrompt); tft.setTextColor(ui.dim, ui.bg); tft.setCursor(12, CONTENT_Y + 30); tft.print(cInputReadKey ? "Press one printable key" : (cInputNumeric ? "Type a whole number and press ENTER" : "Type your answer and press ENTER"));
  tft.fillRoundRect(8, CONTENT_Y + 42, 304, 24, 4, ILI9341_DARKGREY); tft.setTextColor(ui.text, ILI9341_DARKGREY); tft.setCursor(14, CONTENT_Y + 50); String shown = cInputBuffer; if (shown.length() > 46) shown = shown.substring(shown.length() - 46); tft.print(shown); tft.print("_"); footer("ENTER CONFIRM     DEL ERASE     FN CANCEL");
}
void drawCLabCodeLine(int lineIndex) {
  if (lineIndex < cScrollLine || lineIndex >= cScrollLine + C_CODE_VISIBLE_LINES) return;
  int row = lineIndex - cScrollLine, codeY = CONTENT_Y + 12, y = codeY + 4 + row * 11;
  bool active = lineIndex == cCursorLine;
  uint16_t bg = active ? ui.selected : ILI9341_DARKGREY;
  String code = cLines[lineIndex].substring(cHorizontalScroll, cHorizontalScroll + C_CODE_COLUMNS_VISIBLE);
  tft.fillRect(8, y - 1, 304, 10, bg);
  tft.setTextSize(1); tft.setTextColor(ui.dim, bg); tft.setCursor(10, y); tft.printf("%02d", lineIndex + 1);
  // Lightweight Arduino-IDE-style syntax accents. This is presentation only;
  // CardC parsing and source text stay unchanged.
  int x = 30;
  bool inString = false, inComment = false;
  for (int i = 0; i < (int)code.length() && x < 306; ) {
    if (!inString && i + 1 < (int)code.length() && code[i] == '/' && code[i + 1] == '/') inComment = true;
    uint16_t color = ui.text;
    if (inComment) color = ui.dim;
    else if (code[i] == '\"') { color = ILI9341_YELLOW; inString = !inString; }
    else if (inString) color = ILI9341_YELLOW;
    else if (isDigit((unsigned char)code[i])) color = ILI9341_ORANGE;
    else if (isalpha((unsigned char)code[i]) || code[i] == '_') {
      int end = i + 1; while (end < (int)code.length() && (isalnum((unsigned char)code[end]) || code[end] == '_')) end++;
      String word = code.substring(i, end);
      if (word == "int" || word == "String" || word == "void" || word == "if" || word == "while" || word == "return") color = ui.accent;
      else if (word == "true" || word == "false" || word == "setup" || word == "loop") color = ILI9341_MAGENTA;
      else if (word == "print" || word == "println" || word == "input" || word == "qrcode" || word == "beep" || word == "vibrate" || word == "wait" || word == "canvas_begin" || word == "wifi_scan") color = ILI9341_GREEN;
      tft.setTextColor(color, bg); tft.setCursor(x, y); tft.print(word); x += word.length() * 6; i = end; continue;
    }
    tft.setTextColor(color, bg); tft.setCursor(x, y); tft.print(code[i]); x += 6; i++;
  }
  if (active) tft.drawFastVLine(30 + (cCursorColumn - cHorizontalScroll) * 6, y - 1, 10, ui.accent);
}
void drawCLabEditor() {
  tft.setTextSize(1); tft.fillRect(0, CONTENT_Y, W, 12, ui.bg); tft.setTextColor(ui.dim, ui.bg); tft.setCursor(8, CONTENT_Y); tft.printf("CODE L%d-%d/%d  COL %d", cScrollLine + 1, min(cScrollLine + C_CODE_VISIBLE_LINES, cLineCount), cLineCount, cHorizontalScroll + 1);
  const int codeY = CONTENT_Y + 12; tft.fillRoundRect(6, codeY, 308, 91, 4, ILI9341_DARKGREY); for (int row = 0; row < C_CODE_VISIBLE_LINES; row++) { int lineIndex = cScrollLine + row; if (lineIndex >= cLineCount) break; drawCLabCodeLine(lineIndex); }
}
constexpr int C_GUIDE_VISIBLE = 15;
constexpr int C_GUIDE_LINES = 54;  // Legacy in-source guide table; CardC content comes from the header.
constexpr int C_GUIDE_CONTENT_LINES = CARDC_GUIDE_LINES;
void drawCLabGuideContent() {
  static const char* guideText[] = {
    "CARDC / C LAB REFERENCE", "",
    "CardC is C LAB's safe, local C-style language.",
    "It interprets code; it does not compile native code.", "",
    "EDITOR CONTROLS",
    "Type: insert text       ENTER: split/new line",
    "DEL: erase before cursor; hold DEL: repeat erase",
    "TAB: navigation mode", ";/.: line up/down     ,/: character left/right",
    "CTRL+ENTER: run     CTRL+D: load the demo",
    "CTRL+L: clear editor     CTRL+G: this guide", "",
    "BASIC RULES",
    "End each statement with ;   // starts a comment",
    "Numbers are signed integer values (no decimals).",
    "Operators: +  -  *  /  %; * / % run before + -.",
    "Use = to assign; use + to join text.", "",
    "NUMBER VARIABLES",
    "int x;          // creates x with value 0",
    "int x = 7;      // creates x with a value",
    "x = x * 2 + 1;  x++;  x--;", "",
    "TEXT VARIABLES AND INPUT",
    "String name = input(\"Your name?\");",
    "String msg = \"Hello \" + name + \"!\";",
    "Input opens a text box; ENTER confirms it.", "",
    "OUTPUT",
    "print(x);  println(\"Hello \" + name);",
    "puts(\"text\");  printf(x);",
    "All four add one line to OUTPUT (latest five shown).",
    "clear(); clears OUTPUT.  help(); prints a short hint.", "",
    "FUNCTIONS",
    "void greet() { println(\"Hi!\"); }",
    "greet();",
    "Functions take no parameters and have a limited call depth.",
    "Arduino-like void setup() and void loop() are each run once.", "",
    "READ-ONLY SYSTEM VALUES",
    "battery  uptime  free_heap_kb  total_heap_kb  flash_mb",
    "device_name  theme_name  uptime_text", "",
    "WI-FI VALUES (LOCAL, NO CONNECTION)",
    "wifi_scan(); scans nearby networks only.",
    "wifi_networks  wifi_connected  wifi_rssi  wifi_ssid", "",
    "QR OUTPUT",
    "qrcode(\"hello\");  qrcode(\"Hi \" + name);",
    "Text must be printable ASCII and 1-60 characters.",
    "FN closes the QR screen and returns to C LAB.", "",
    "LIMITS AND SAFETY",
    "Max: 32 lines, 48 characters/line, 12 int and 8 String vars.",
    "No loops, pointers, arrays, files, GPIO, native code,",
    "Wi-Fi connections, credentials, or unrestricted system access.",
    "Errors stop the current run and appear in OUTPUT."
  };
  cGuideScroll = constrain(cGuideScroll, 0, C_GUIDE_CONTENT_LINES - C_GUIDE_VISIBLE);
  tft.fillRect(0, CONTENT_Y, W, H - CONTENT_Y - FOOTER_H, ui.bg);
  tft.setTextSize(1);
  for (int row = 0; row < C_GUIDE_VISIBLE; ++row) {
    int line = cGuideScroll + row;
    uint16_t color = String(CARDC_GUIDE_TEXT[line]).startsWith("# ") ? ui.accent : ui.text;
    tft.setTextColor(color, ui.bg);
    tft.setCursor(10, CONTENT_Y + 3 + row * 12);
    tft.print(CARDC_GUIDE_TEXT[line]);
  }
  tft.setTextColor(ui.accent, ui.bg);
  if (cGuideScroll > 0) { tft.setCursor(302, CONTENT_Y + 2); tft.print("^"); }
  if (cGuideScroll + C_GUIDE_VISIBLE < C_GUIDE_CONTENT_LINES) { tft.setCursor(302, H - FOOTER_H - 12); tft.print("v"); }
}
void drawCLabGuide() {
  tft.fillScreen(ui.bg);
  header("CARDC GUIDE");
  drawCLabGuideContent();
  footer(";/. SCROLL     CTRL+G CLOSE     FN BACK");
}
void drawCLabQR() {
  tft.fillScreen(ui.bg); header("C LAB QR");
  tft.setTextSize(1); tft.setTextColor(ui.dim, ui.bg); tft.setCursor(8, CONTENT_Y - 6); tft.print("qrcode() OUTPUT");
  uint8_t data[qrcode_getBufferSize(5)]; QRCode code; qrcode_initText(&code, data, 5, ECC_LOW, cLabQrPayload.c_str());
  const int scale = 4, size = code.size * scale, x = (W - size) / 2, y = CONTENT_Y + 13;
  tft.fillRect(x - 4, y - 4, size + 8, size + 8, ILI9341_WHITE);
  for (int row = 0; row < code.size; ++row) for (int col = 0; col < code.size; ++col) if (qrcode_getModule(&code, col, row)) tft.fillRect(x + col * scale, y + row * scale, scale, scale, ILI9341_BLACK);
  tft.setTextColor(ui.dim, ui.bg); tft.setCursor(8, H - FOOTER_H - 12); String shown = cLabQrPayload; if (shown.length() > 48) shown = shown.substring(0, 48); tft.print(shown);
  footer("FN BACK TO C LAB");
}
void drawCardCRepl() {
  tft.fillScreen(ui.bg); header("CARDC REPL");
  tft.setTextSize(1); tft.setTextColor(ui.dim, ui.bg); tft.setCursor(9, CONTENT_Y + 4);
  tft.print("One CardC statement, then ENTER");
  tft.fillRoundRect(8, CONTENT_Y + 13, 304, 24, 4, ILI9341_DARKGREY);
  String shown = replInput; if (shown.length() > 43) shown = shown.substring(shown.length() - 43);
  tft.setTextColor(ui.accent, ILI9341_DARKGREY); tft.setCursor(14, CONTENT_Y + 21); tft.print("> "); tft.print(shown); tft.print("_");
  tft.setTextColor(ui.dim, ui.bg); tft.setCursor(9, CONTENT_Y + 52); tft.print("OUTPUT (latest five)");
  tft.drawFastHLine(8, CONTENT_Y + 61, 304, ui.dim);
  for (int i = 0; i < replHistoryCount; ++i) { tft.setTextColor(replHistory[i].startsWith("ERR") ? ILI9341_RED : ILI9341_GREEN, ui.bg); tft.setCursor(12, CONTENT_Y + 71 + i * 13); tft.print(replHistory[i]); }
  footer("ENTER RUN  DEL ERASE  FN BACK");
}
void runCardCRepl() {
  String command = replInput; command.trim();
  if (command.isEmpty()) return;
  // Interactive input needs the multi-step C LAB editor, so keep this console
  // predictable and direct rather than opening a hidden editor session.
  if (command.indexOf("input(") >= 0) { replHistoryCount = 1; replHistory[0] = "ERR: input() uses C LAB"; replInput = ""; redrawNeeded = true; return; }
  String savedLines[C_MAX_LINES]; int savedCount = cLineCount;
  for (int i = 0; i < C_MAX_LINES; ++i) savedLines[i] = cLines[i];
  cLines[0] = command.endsWith(";") ? command : command + ";";
  cLineCount = 1; cardcReplRunning = true; runCLab(); cardcReplRunning = false;
  replHistoryCount = cOutputCount;
  for (int i = 0; i < replHistoryCount; ++i) replHistory[i] = cOutput[i];
  for (int i = 0; i < C_MAX_LINES; ++i) cLines[i] = savedLines[i];
  cLineCount = savedCount; replInput = ""; redrawNeeded = true;
}
constexpr int C_LAB_EXPLORER_VISIBLE = 5;
void drawCLabExplorerRow(int index) {
  int row = index - cLabExplorerScroll;
  if (row < 0 || row >= C_LAB_EXPLORER_VISIBLE) return;
  String label, detail;
  if (index < C_USER_APP_COUNT) {
    label = "USER APP " + String(index + 1);
    detail = cUserApps[index].isEmpty() ? "empty slot" : "saved CardC app";
  } else if (index == C_USER_APP_COUNT) {
    label = "[DIR] EXAMPLES";
    detail = "ready-to-run CardC projects";
  } else {
    label = "+ NEW FILE";
    detail = "start a blank document";
  }
  int y = CONTENT_Y + 29 + row * 31; bool selected = index == cLabExplorerSelected;
  uint16_t bg = selected ? ui.selected : ui.bg;
  tft.fillRect(8, y - 4, 304, 25, ui.bg);
  if (selected) tft.fillRoundRect(8, y - 4, 304, 25, 4, bg);
  tft.setTextSize(1); tft.setTextColor(selected ? ui.text : ui.dim, bg); tft.setCursor(15, y); tft.print(selected ? "> " : "  "); tft.print(label);
  tft.setTextColor(selected ? ui.text : ui.dim, bg); tft.setCursor(28, y + 11); tft.print(detail);
}
void drawCLabExplorer() {
  cLabExplorerSelected = constrain(cLabExplorerSelected, 0, C_LAB_EXPLORER_ITEMS - 1);
  cLabExplorerScroll = constrain(cLabExplorerScroll, 0, max(0, C_LAB_EXPLORER_ITEMS - C_LAB_EXPLORER_VISIBLE));
  if (cLabExplorerSelected < cLabExplorerScroll) cLabExplorerScroll = cLabExplorerSelected;
  if (cLabExplorerSelected >= cLabExplorerScroll + C_LAB_EXPLORER_VISIBLE) cLabExplorerScroll = cLabExplorerSelected - C_LAB_EXPLORER_VISIBLE + 1;
  tft.fillScreen(ui.bg); header("C LAB / FILES"); tft.setTextSize(1);
  tft.setTextColor(ui.dim, ui.bg); tft.setCursor(10, CONTENT_Y + 5); tft.print("10 USER APPS / EXAMPLES / NEW FILE");
  for (int row = 0; row < C_LAB_EXPLORER_VISIBLE; ++row) {
    int index = cLabExplorerScroll + row;
    if (index < C_LAB_EXPLORER_ITEMS) drawCLabExplorerRow(index);
  }
  footer(";/. SELECT  ENTER EDIT  CTRL+ENTER RUN");
}
void openCLabUserApp(int slot, bool runNow) {
  if (slot < 0 || slot >= C_USER_APP_COUNT) return;
  cLabActiveUserApp = slot;
  String source = cUserApps[slot];
  cLineCount = 0;
  int start = 0;
  while (cLineCount < C_MAX_LINES) {
    int end = source.indexOf('\n', start);
    cLines[cLineCount++] = (end < 0 ? source.substring(start) : source.substring(start, end)).substring(0, C_MAX_LINE_CHARS);
    if (end < 0) break;
    start = end + 1;
  }
  if (cLineCount == 0) { cLineCount = 1; cLines[0] = ""; }
  for (int i = cLineCount; i < C_MAX_LINES; ++i) cLines[i] = "";
  cLabFileName = String("user-app-") + String(slot + 1) + ".clab";
  cCursorLine = cCursorColumn = cScrollLine = cHorizontalScroll = 0;
  cOutputCount = 0; cInputValueCount = cInputReadIndex = 0; cInputActive = false;
  cLabQrActive = false; cLabQrPayload = ""; cLabDirty = false;
  cLabExplorerVisible = false; page = CLAB;
  markStateDirty();
  if (runNow) { playEnterSound(); runCLab(); }
  else playMenuSound();
  redrawNeeded = true;
}
void drawCLabSaveChoice(int index) {
  const char* choices[] = {"CANCEL", "SAVE", "DISCARD"};
  int x = 18 + index * 100; uint16_t bg = index == cLabSaveDialogSelected ? ui.selected : ILI9341_DARKGREY;
  tft.fillRoundRect(x, CONTENT_Y + 70, 84, 30, 4, bg);
  tft.setTextSize(1); tft.setTextColor(ui.text, bg); tft.setCursor(x + 10, CONTENT_Y + 82); tft.print(choices[index]);
}
void drawCLabSaveDialog() {
  tft.fillScreen(ui.bg); header("C LAB / SAVE CHANGES?");
  tft.setTextSize(1); tft.setTextColor(ui.text, ui.bg); tft.setCursor(24, CONTENT_Y + 28); tft.print("Save changes to " + cLabFileName + "?");
  for (int i = 0; i < 3; ++i) drawCLabSaveChoice(i);
  footer(",/ CHOOSE     ENTER CONFIRM");
}
void drawCLabSlotDialog() {
  tft.fillScreen(ui.bg); header("C LAB / SAVE TO SLOT");
  tft.setTextSize(1); tft.setTextColor(ui.dim, ui.bg); tft.setCursor(12, CONTENT_Y + 7);
  tft.print("CHOOSE A USER APP SLOT, THEN ENTER");
  int first = constrain(cLabSlotDialogSelected - 4, 0, C_USER_APP_COUNT - 8);
  for (int row = 0; row < 8; ++row) {
    int slot = first + row;
    int y = CONTENT_Y + 27 + row * 18;
    bool selected = slot == cLabSlotDialogSelected;
    uint16_t bg = selected ? ui.selected : ui.bg;
    if (selected) tft.fillRoundRect(8, y - 3, 304, 15, 3, bg);
    tft.setTextColor(selected ? ui.text : ui.dim, bg); tft.setCursor(14, y);
    tft.print(selected ? "> " : "  "); tft.print("USER APP " + String(slot + 1));
    tft.setTextColor(selected ? ui.text : ui.dim, bg); tft.setCursor(126, y);
    tft.print(cUserApps[slot].isEmpty() ? "empty slot" : "replace saved app");
  }
  footer(";/. SELECT     ENTER SAVE     FN CANCEL");
}
void drawCLabNameField() {
  tft.fillRoundRect(8, CONTENT_Y + 31, 304, 25, 4, ILI9341_DARKGREY);
  tft.setTextSize(1); tft.setTextColor(ui.text, ILI9341_DARKGREY); tft.setCursor(14, CONTENT_Y + 40); tft.print(cLabNameBuffer); tft.print("_");
}
void drawCLabNameDialog() {
  tft.fillScreen(ui.bg); header("C LAB / SAVE AS"); tft.setTextSize(1); tft.setTextColor(ui.dim, ui.bg); tft.setCursor(12, CONTENT_Y + 16); tft.print("TYPE A FILE NAME, THEN PRESS ENTER");
  drawCLabNameField();
  footer("ENTER SAVE     DEL ERASE     FN CANCEL");
}
void openNewCLabFile() { for (int i = 0; i < C_MAX_LINES; ++i) cLines[i] = ""; cLineCount = 1; cCursorLine = cCursorColumn = cScrollLine = cHorizontalScroll = 0; cOutputCount = 0; cLabFileName = "untitled.clab"; cLabDirty = true; cLabExplorerVisible = false; }
void saveCLabFile() { if (cLabNameBuffer.isEmpty()) cLabNameBuffer = "untitled"; cLabFileName = cLabNameBuffer; if (!cLabFileName.endsWith(".clab")) cLabFileName += ".clab"; cLabNameBuffer = ""; cLabNameDialogVisible = false; cLabDirty = false; markStateDirty(); }
void drawCLab() { if (cCanvasActive) return; if (cLabSaveDialogVisible) { drawCLabSaveDialog(); return; } if (cLabSlotDialogVisible) { drawCLabSlotDialog(); return; } if (cLabNameDialogVisible) { drawCLabNameDialog(); return; } if (cLabExplorerVisible) { drawCLabExplorer(); return; } if (cInputActive) { drawCLabInput(); return; } if (cLabGuideVisible) { drawCLabGuide(); return; } if (cLabQrActive) { drawCLabQR(); return; } tft.fillScreen(ui.bg); header("C LAB"); drawCLabEditor(); int outputY = CONTENT_Y + 111; tft.setTextColor(ui.dim, ui.bg); tft.setCursor(8, outputY); tft.print("OUTPUT"); tft.drawFastHLine(6, outputY + 9, 308, ui.dim); for (int i = 0; i < cOutputCount; i++) { tft.setTextColor(i == cOutputCount - 1 && cOutput[i].startsWith("ERR") ? ILI9341_RED : ILI9341_GREEN, ui.bg); tft.setCursor(10, outputY + 15 + i * 11); tft.print(cOutput[i]); } footer(cNavigationMode ? "NAV: ; UP . DOWN , LEFT / RIGHT" : "CTRL+S SAVE  CTRL+O FILES  TAB NAV"); }

// ---- SETTINGS: theme/rotation/backlight/sleep/volume/Wi-Fi/lock PIN/BLE --
// settingSelected < 7 are plain cyclable values (changeSetting() in
// keyboard() handles ,/  left/right); 7-10 open their own sub-flow (PIN
// change, Wi-Fi setup, or a straight on/off toggle) instead.
void updateSettingsRow(int i) { int y = SETTINGS_ROW_Y + i * SETTINGS_ROW_STEP; bool sel = i == settingSelected; tft.fillRect(8, y - 3, 304, 18, ui.bg); uint16_t fill = sel ? ui.selected : ui.bg; if (sel) tft.fillRoundRect(8, y - 3, 304, 18, 4, fill); tft.setTextSize(1); tft.setTextColor(ui.text, fill); tft.setCursor(16, y); tft.print(sel ? "> " : "  "); tft.print(settingNames[i]); tft.setTextColor(sel ? ui.accent : ui.dim, fill); tft.setCursor(165, y); tft.print(settingValue(i)); }
String settingValue(int i) { if (i == 0) return themeNames[themeIndex]; if (i == 1) return displayRotation == 3 ? "LANDSCAPE RIGHT" : "LANDSCAPE LEFT"; if (i == 2) return backlightOn ? "ON" : "OFF"; if (i == 3) return String(brightnessLevel * 10) + "%"; if (i == 4) return sleepValues[sleepIndex] == 0 ? "NEVER" : String(sleepValues[sleepIndex]) + " SEC"; if (i == 5) return lockAfterScreensaverValues[lockAfterScreensaverIndex] == 0 ? "NEVER" : String(lockAfterScreensaverValues[lockAfterScreensaverIndex]) + " SEC"; if (i == 6) return String(volumeLevel * 10) + "%"; if (i == 7) return "ENTER CHANGE"; if (i == 8) return WiFi.status() == WL_CONNECTED ? "CONNECTED" : "SCAN / CONNECT"; if (i == 9) return bleHidEnabled ? "ON" : "OFF"; return statusLedEnabled ? "ON" : "OFF"; }
void drawSettings() {
  tft.fillScreen(ui.bg); header(pinChangeActive ? "SETTINGS / LOCK PIN" : "SETTINGS"); tft.setTextSize(1);
  if (pinChangeActive) {
    tft.setTextColor(ui.accent, ui.bg); tft.setCursor(22, CONTENT_Y + 22);
    tft.print(pinChangeConfirm ? "REPEAT NEW 4-DIGIT PIN" : "TYPE A NEW 4-DIGIT PIN");
    String masked; for (int i = 0; i < (int)pinChangeInput.length(); ++i) masked += "*";
    tft.fillRoundRect(83, CONTENT_Y + 48, 154, 34, 5, ILI9341_DARKGREY);
    tft.setTextSize(2); tft.setTextColor(ui.text, ILI9341_DARKGREY); tft.setCursor(151 - masked.length() * 6, CONTENT_Y + 58); tft.print(masked); tft.print("_");
    tft.setTextSize(1); tft.setTextColor(ui.dim, ui.bg); tft.setCursor(25, CONTENT_Y + 108); tft.print(pinChangeStatus);
    footer("ENTER NEXT/SAVE  DEL ERASE  FN CANCEL");
    return;
  }
  for (int i = 0; i < SETTINGS_COUNT; i++) updateSettingsRow(i);
  footer(";/. SELECT  ,/ CHANGE  ENTER OPEN  FN BACK");
}

// Connect at boot with the last saved credentials. This deliberately starts
// only the STA connection; Web Companion remains an explicit user action.
void autoConnectWifi() {
  String ssid, password;
  preferences.begin("cyberdeck", false);
  ssid = preferences.getString("wifi_ssid", "");
  password = preferences.getString("wifi_pass", "");
  if (ssid.isEmpty()) {
    ssid = INITIAL_WIFI_SSID;
    password = INITIAL_WIFI_PASSWORD;
    preferences.putString("wifi_ssid", ssid);
    preferences.putString("wifi_pass", password);
  }
  preferences.end();
  if (ssid.isEmpty()) return;
  WiFi.mode(WIFI_STA);
  WiFi.persistent(false);
  WiFi.setAutoReconnect(true);
  // Required for Wi-Fi/BLE coexistence on this ESP32-S3 firmware.
  WiFi.setSleep(true);
  WiFi.begin(ssid.c_str(), password.c_str());
  autoWifiConnecting = true;
  autoWifiStartedAt = millis();
  autoWifiNextAttemptAt = autoWifiStartedAt + 20000UL;
  webStatus = "Wi-Fi auto-connecting";
}

// WiFi.setAutoReconnect() handles ordinary association loss, while this
// bounded retry also recovers when the first boot-time begin() fails before a
// router is ready. It never starts Web Companion and avoids interrupting scans.
void serviceAutoConnectWifi() {
  if (webRunning || scanRunning) return;
  if (WiFi.status() == WL_CONNECTED) {
    autoWifiConnecting = false;
    // NTP was previously done only when Web Companion connected. Auto-connect
    // now follows the same path, so the header, lock screen and TOTP clock
    // receive real time immediately after a saved network reconnects.
    if (!autoWifiWasConnected) {
      autoWifiWasConnected = true;
      bool timeSynced = syncNetworkTime();
      webStatus = timeSynced ? "Wi-Fi connected / NTP synced" : "Wi-Fi connected / NTP pending";
      redrawNeeded = true;
    }
    return;
  }
  autoWifiWasConnected = false;
  unsigned long now = millis();
  if (autoWifiConnecting && now - autoWifiStartedAt < 20000UL) return;
  if (now < autoWifiNextAttemptAt) return;
  autoWifiConnecting = false;
  autoConnectWifi();
}

void startWebCompanion() {
  if (webRunning) {
    webServer.stop();
    if (mdnsRunning) { MDNS.end(); mdnsRunning = false; }
    // Keep the STA driver alive. Cycling WIFI_OFF/WIFI_STA alongside BLE HID
    // was the most aggressive reset path and can destabilise radio coexistence.
    WiFi.disconnect(false, false);
    webRunning = false;
    webStatus = "Wi-Fi web link stopped";
    redrawNeeded = true;
    return;
  }

  String ssid, password;
  preferences.begin("cyberdeck", false);
  ssid = preferences.getString("wifi_ssid", "");
  password = preferences.getString("wifi_pass", "");
  if (ssid.isEmpty()) {
    // Save the supplied network once in NVS. Future boots use the stored copy.
    ssid = INITIAL_WIFI_SSID;
    password = INITIAL_WIFI_PASSWORD;
    preferences.putString("wifi_ssid", ssid);
    preferences.putString("wifi_pass", password);
  }
  preferences.end();

  // Do not erase the ESP32 Wi-Fi driver's configuration here. On the
  // Cardputer ADV that reset path can destabilize the radio immediately before
  // WiFi.begin(). Disconnect only an old live session, then begin a new STA
  // connection using the credentials retained in Preferences.
  WiFi.mode(WIFI_STA);
  WiFi.persistent(false);
  WiFi.setAutoReconnect(true);
  WiFi.scanDelete();
  if (WiFi.status() == WL_CONNECTED || WiFi.status() == WL_CONNECT_FAILED) {
    WiFi.disconnect(false, false);
    delay(100);
  }
  // ESP32-S3 requires Wi-Fi modem sleep whenever BLE is enabled. BLE HID is
  // started at boot, so disabling Wi-Fi sleep here aborts the radio driver.
  // Keep modem sleep enabled; it is also compatible with HTTP and NTP.
  WiFi.setSleep(true);
  WiFi.begin(ssid.c_str(), password.c_str());
  unsigned long started = millis();
  while (WiFi.status() != WL_CONNECTED && millis() - started < 15000UL) {
    delay(100);
    M5Cardputer.update();
  }
  if (WiFi.status() != WL_CONNECTED) {
    webStatus = "Wi-Fi connection failed (radio kept ready)";
    WiFi.disconnect(false, false);
    redrawNeeded = true;
    return;
  }
  // Once Wi-Fi is available, update the shared system clock used by the lock
  // screen and clock-dependent apps. A later Web Companion connection retries it.
  bool timeSynced = syncNetworkTime();
  mdnsRunning = MDNS.begin(WEB_MDNS_HOST);
  if (mdnsRunning) MDNS.addService("http", "tcp", 80);
  webServer.on("/", HTTP_GET, []() { webServer.send(200, "text/html", "<!doctype html><meta name=viewport content='width=device-width,initial-scale=1'><style>body{font-family:monospace;background:#08131a;color:#dff;padding:16px;max-width:560px;margin:auto}textarea{box-sizing:border-box;width:100%;height:120px;background:#111;color:#fff;border:1px solid #5cc;padding:8px}button{padding:12px;margin:3px;background:#123;color:#dff;border:1px solid #5cc;border-radius:4px}.grid{display:grid;grid-template-columns:repeat(3,1fr);gap:4px}.wide{width:100%}</style><h2>CARDPUTER XL</h2><p>Local home-Wi-Fi link. Open <b>http://cardputer-xl.local</b></p><h3>Quick apps</h3><div class=grid><button onclick=go('app-dashboard')>Dashboard</button><button onclick=go('app-dice')>Dice</button><button onclick=go('app-notes')>Notes</button><button onclick=go('app-wifi')>Wi-Fi Scan</button><button onclick=go('app-music')>Music Lab</button><button onclick=go('scan')>Scan now</button><button onclick=go('back')>Home</button></div><h3>Remote control</h3><div class=grid><span></span><button onclick=go('up')>UP</button><span></span><button onclick=go('left')>LEFT</button><button onclick=go('enter')>ENTER</button><button onclick=go('right')>RIGHT</button><span></span><button onclick=go('down')>DOWN</button><span></span></div><button class=wide onclick=go('back')>BACK / LAUNCHER</button><h3>Quick settings</h3><div class=grid><button onclick=go('theme')>Theme</button><button onclick=go('bright-')>Brightness -</button><button onclick=go('bright+')>Brightness +</button><button onclick=go('volume-')>Volume -</button><button onclick=go('volume+')>Volume +</button><button onclick=go('settings')>Open settings</button></div><h3>Send text</h3><form method=post action='/note'><textarea name=t placeholder='Text for Notes'></textarea><button class=wide>Send to Notes</button></form><h3>ZABKA QR</h3><form method=post action='/zabka'><textarea name=s autocomplete='off' autocapitalize='characters' spellcheck=false placeholder='HEX secret from the original JSON: e.g. A1B2...'></textarea><textarea name=id inputmode='numeric' autocomplete='off' spellcheck=false placeholder='ployId from the original JSON'></textarea><button class=wide>Send HEX secret to Cardputer RAM</button></form><h3>ZABKA VAULT</h3><form method=post action='/zabkavault'><textarea name=s autocomplete='off' autocapitalize='characters' spellcheck=false placeholder='HEX secret to encrypt and store'></textarea><textarea name=id inputmode='numeric' autocomplete='off' spellcheck=false placeholder='ployId from the original JSON'></textarea><textarea name=p type=password autocomplete='new-password' spellcheck=false placeholder='New vault password: 8+ characters'></textarea><button class=wide>Encrypt and save on Cardputer</button></form><form method=post action='/zabkaunlock'><textarea name=p type=password autocomplete='current-password' spellcheck=false placeholder='Vault password: 8+ characters'></textarea><button class=wide>Unlock stored secret into RAM</button></form><button class=wide onclick=go('zabka-lock')>Lock and clear TOTP from RAM</button><p>The vault stores encrypted data in Cardputer flash. Its password is never saved. Use a trusted local Wi-Fi network: this page is local HTTP, not HTTPS.</p><h3>C LAB</h3><div class=grid><button onclick=go('app-clab')>Open C LAB on Cardputer</button><button onclick=location.href='/clabide'>Open C LAB IDE</button></div><p>Web Companion can remotely navigate Home and Settings, send a short note, paste a RAM-only ZABKA TOTP secret, edit C LAB source, start a nearby-network scan, and open Dashboard, Dice or Notes. It remains a local-only control page.</p><script>function go(a){fetch('/action?a='+encodeURIComponent(a))}</script>"); });
  webServer.on("/action", HTTP_GET, []() { pendingWebAction = webServer.arg("a"); webServer.send(204); });
  webServer.on("/note", HTTP_POST, []() { pendingWebNote = webServer.arg("t"); webServer.sendHeader("Location", "/"); webServer.send(303); });
  webServer.on("/zabka", HTTP_POST, []() {
    // Limit and validate later on the device. Do not log, echo, or persist this value.
    // Accept either a bare HEX secret or the original JSON object
    // {"secret":"...","ployId":"..."}; parse it later on-device.
    pendingZabkaSecret = webServer.arg("s").substring(0, 220);
    pendingZabkaPloyId = webServer.arg("id").substring(0, 24);
    webServer.sendHeader("Location", "/"); webServer.send(303);
  });
  webServer.on("/zabkavault", HTTP_POST, []() {
    pendingZabkaVaultSecret = webServer.arg("s").substring(0, 220);
    pendingZabkaVaultPloyId = webServer.arg("id").substring(0, 24);
    pendingZabkaVaultPassphrase = webServer.arg("p").substring(0, 96);
    webServer.sendHeader("Location", "/"); webServer.send(303);
  });
  webServer.on("/zabkaunlock", HTTP_POST, []() {
    pendingZabkaUnlockPassphrase = webServer.arg("p").substring(0, 96);
    webServer.sendHeader("Location", "/"); webServer.send(303);
  });
  webServer.on("/clabide", HTTP_GET, []() {
    String currentCode;
    for (int i = 0; i < cLineCount; ++i) { if (i) currentCode += '\n'; currentCode += cLines[i]; }
    String pageHtml = "<!doctype html><meta name=viewport content='width=device-width,initial-scale=1'><style>*{box-sizing:border-box}body{font-family:Segoe UI,ui-monospace,monospace;background:#151c1f;color:#d8e1e3;margin:0;min-height:100vh}.titlebar{height:30px;background:#202629;display:flex;align-items:center;padding:0 12px;font-size:12px;color:#cbd5d7}.titlebar b{color:#79d6df;margin-right:7px}.menubar{height:36px;background:#252b2e;border-bottom:1px solid #313b3f;display:flex;align-items:center;gap:19px;padding:0 14px;font-size:13px;color:#dce5e7}.menubar span:first-child{color:#fff}.shell{display:grid;grid-template-columns:50px 1fr;min-height:calc(100vh - 66px)}.activity{background:#1d2427;border-right:1px solid #303b3f;padding-top:12px;display:flex;flex-direction:column;align-items:center;gap:17px;color:#8ca0a4}.activity span{font-size:22px;line-height:1}.activity span:first-child{color:#2dbbc7;border-left:3px solid #2dbbc7;padding-left:7px}.work{min-width:0}.toolbar{height:54px;background:#1e2528;border-bottom:1px solid #303b3f;display:flex;align-items:center;gap:10px;padding:0 15px}.round{border-radius:50%;width:30px;height:30px;margin:0;padding:0;border:0;background:#20abb8;color:#102326;font-weight:bold}.board{margin-left:8px;background:#303b3f;border:1px solid #56666b;color:#dbe6e8;padding:8px 12px;min-width:190px;font:12px inherit}.board:after{content:' ▾';float:right}.toolbar .online{margin-left:auto;color:#53d3b3;font-size:12px}.tabbar{height:39px;background:#1e2528;border-bottom:1px solid #303b3f;display:flex;align-items:end;padding-left:12px}.tab{height:39px;padding:12px 18px 0;background:#182023;border-top:2px solid #25aeba;color:#eef7f8;font-size:12px}.wrap{padding:0;max-width:none;margin:0}.hint{margin:0;padding:10px 16px;background:#212a2d;border-bottom:1px solid #303b3f;color:#91a7ac;font-size:12px}.editor{display:grid;grid-template-columns:54px 1fr;border:0;background:#182023;min-height:calc(100vh - 222px)}.lines{padding:13px 9px;text-align:right;color:#71878c;background:#20282b;border-right:1px solid #2b3538;line-height:1.55;user-select:none;white-space:pre;font-size:13px}textarea{box-sizing:border-box;width:100%;min-height:calc(100vh - 222px);resize:vertical;border:0;outline:0;padding:13px 15px;background:#182023;color:#e3ebed;font:14px/1.55 ui-monospace,Consolas,monospace;tab-size:2;caret-color:#38c5d0}textarea:focus{background:#192225}.bottom{display:flex;align-items:center;gap:14px;min-height:50px;padding:8px 16px;background:#1e2528;border-top:1px solid #303b3f}.save{padding:8px 18px;margin:0;background:#168e9a;color:#fff;border:1px solid #36bdc7;border-radius:3px;font:12px inherit;font-weight:bold}.status{margin-left:auto;color:#92a8ac;font-size:12px}.wide{width:auto}@media(max-width:560px){.activity{display:none}.shell{grid-template-columns:1fr}.menubar{gap:10px;font-size:11px}.board{min-width:140px}.toolbar .online{display:none}.editor{min-height:62vh}textarea{min-height:62vh}}</style><div class=titlebar><b>[C]</b> Cardputer C LAB IDE <span style='margin-left:auto'>- [_] [X]</span></div><div class=menubar><span>File</span><span>Edit</span><span>Sketch</span><span>Tools</span><span>Help</span></div><div class=shell><aside class=activity><span>[F]</span><span>[S]</span><span>[L]</span><span>[R]</span><span>[*]</span></aside><section class=work><div class=toolbar><button type=button class=round title='Save'>S</button><button type=button class=round title='Run on Cardputer' onclick='alert(&quot;Save the code, then use Ctrl+Enter on the Cardputer to run it.&quot;)'>R</button><div class=board>Cardputer XL / CardC</div><span class=online>* LOCAL LINK</span></div><div class=tabbar><div class=tab>sketch_cardc.clab [x]</div></div><main class=wrap><p class=hint>EXPLORER &gt; C LAB &gt; sketch_cardc.clab | 32 lines max / 48 characters per line</p><form method=post action='/clab'><div class=editor><pre class=lines id=lines>1</pre><textarea id=code name=t spellcheck=false placeholder='// Write CardC here'></textarea></div><div class=bottom><button class=save type=submit>SAVE TO C LAB</button><button class=save type=submit formaction='/clabrun'>SAVE &amp; RUN ON CARDPUTER</button><a href='/' style='color:#83d9e0;font-size:12px'>Back to Web Companion</a><span class=status>UTF-8 | LF | CardC | Ln <span id=cursor>1</span></span></div></form></main></section></div><script>const e=document.getElementById('code'),n=document.getElementById('lines');e.value=`";
    currentCode.replace("\\", "\\\\"); currentCode.replace("`", "\\`"); currentCode.replace("${", "\\${");
    pageHtml += currentCode;
    pageHtml += "`;function nums(){let c=e.value.split('\\n').length;n.textContent=Array.from({length:c},(_,i)=>i+1).join('\\n')}e.addEventListener('input',nums);e.addEventListener('keydown',x=>{if(x.key==='Tab'){x.preventDefault();let a=e.selectionStart,b=e.selectionEnd;e.setRangeText('  ',a,b,'end');nums()}});nums()</script>";
    webServer.send(200, "text/html", pageHtml);
  });
  webServer.on("/clab", HTTP_POST, []() { pendingWebCLab = webServer.arg("t"); pendingWebCLabRun = false; webServer.sendHeader("Location", "/clabide"); webServer.send(303); });
  webServer.on("/clabrun", HTTP_POST, []() { pendingWebCLab = webServer.arg("t"); pendingWebCLabRun = true; webServer.sendHeader("Location", "/clabide"); webServer.send(303); });
  webServer.begin();
  webRunning = true;
  webStatus = (mdnsRunning ? String("http://") + WEB_MDNS_HOST + ".local" : WiFi.localIP().toString()) + (timeSynced ? " / NTP OK" : " / NTP pending");
  redrawNeeded = true;
}
void applyWebAction(const String& action) {
  // Browser controls are real user input: they keep the panel awake just like
  // a key press and use the same audible feedback families as the keyboard.
  lastActivity = millis();
  if (sleeping) { sleeping = false; setBacklight(true); }

  if (action == "back") { playExitSound(); page = LAUNCHER; }
  else if (action == "settings") { playMenuSound(); page = SETTINGS; }
  else if (action == "theme") { playFunctionSound(); themeIndex = (themeIndex + 1) % THEME_COUNT; applyTheme(); markStateDirty(); }
  else if (action == "bright-") { playMenuSound(); brightnessLevel = brightnessLevel <= 1 ? 1 : brightnessLevel - 1; applyBacklight(); markStateDirty(); }
  else if (action == "bright+") { playMenuSound(); brightnessLevel = brightnessLevel >= 10 ? 10 : brightnessLevel + 1; applyBacklight(); markStateDirty(); }
  else if (action == "volume-") { volumeLevel = volumeLevel == 0 ? 0 : volumeLevel - 1; applyVolume(); playMenuSound(); markStateDirty(); }
  else if (action == "volume+") { volumeLevel = volumeLevel >= 10 ? 10 : volumeLevel + 1; applyVolume(); playMenuSound(); markStateDirty(); }
  else if (action == "scan") { playEnterSound(); startScan(); }
  else if (action == "app-dashboard") { playEnterSound(); page = DASHBOARD; }
  else if (action == "app-dice") { playEnterSound(); rollDiceRandom(); page = DICERANDOM; }
  else if (action == "app-notes") { playEnterSound(); page = NOTES; }
  else if (action == "app-wifi") { playEnterSound(); page = WIFI; }
  else if (action == "app-music") { playEnterSound(); page = MUSICLAB; }
  else if (action == "app-clab") { playEnterSound(); page = CLAB; }
  else if (action == "zabka-lock") { lockZabkaVault(); playExitSound(); page = ZABKATOTP; }
  else if (page == LAUNCHER) {
    if (launcherHome) {
      int oldHome = homeSelected;
      if (action == "left" && homeSelected % 2) { playMenuSound(); homeSelected--; }
      else if (action == "right" && homeSelected % 2 == 0) { playMenuSound(); homeSelected++; }
      else if (action == "up" && homeSelected >= 2) { playMenuSound(); homeSelected -= 2; }
      else if (action == "down" && homeSelected < 4) { playMenuSound(); homeSelected += 2; }
      else if (action == "enter") { playEnterSound(); if (homeSelected == 5) { launcherHome = false; appSelected = appScroll = 0; } else if (homeAppIndices[homeSelected] >= 0) page = static_cast<Page>(homeAppIndices[homeSelected] + 1); }
      if (homeSelected != oldHome) startHomeFocusAnimation(oldHome, homeSelected);
    } else {
      if (action == "up" || action == "left") { playMenuSound(); appSelected = (appSelected + SECONDARY_APP_COUNT - 1) % SECONDARY_APP_COUNT; syncLauncherScroll(); }
      else if (action == "down" || action == "right") { playMenuSound(); appSelected = (appSelected + 1) % SECONDARY_APP_COUNT; syncLauncherScroll(); }
      else if (action == "enter") { playEnterSound(); if (appSelected == 0) { launcherHome = true; homeSelected = 5; } else page = static_cast<Page>(secondaryAppIndices[appSelected - 1] + 1); }
    }
  } else if (page == SETTINGS && !pinChangeActive) {
    if (action == "up") { playMenuSound(); settingSelected = (settingSelected + SETTINGS_COUNT - 1) % SETTINGS_COUNT; }
    else if (action == "down") { playMenuSound(); settingSelected = (settingSelected + 1) % SETTINGS_COUNT; }
    else if (action == "left" && settingSelected != 7) { playMenuSound(); changeSetting(-1); }
    else if (action == "right" && settingSelected != 7) { playMenuSound(); changeSetting(1); }
  }
  webStatus = "Remote: " + action;
  redrawNeeded = true;
}
bool syncZabkaNetworkTime() {
  if (WiFi.status() != WL_CONNECTED) {
    zabkaStatus = "Wi-Fi disconnected; open WEB COMPANION.";
    return false;
  }
  // Two numeric server addresses bypass a failed home-router DNS resolver;
  // pool.ntp.org remains a third fallback when DNS is available.
  configTime(0, 0, "162.159.200.1", "129.6.15.28", "pool.ntp.org");
  const unsigned long deadline = millis() + 9000UL;
  while (millis() < deadline) {
    time_t now = time(nullptr);
    if (now > 1700000000) {
      zabkaStatus = "Network time synced (UTC epoch).";
      return true;
    }
    delay(100);
    M5Cardputer.update();
  }
  zabkaStatus = "NTP timed out: Wi-Fi works, but UDP time may be blocked.";
  return false;
}

void applyWebInput() {
  if (!pendingWebAction.isEmpty()) { String action = pendingWebAction; pendingWebAction = ""; applyWebAction(action); }
  if (!pendingWebNote.isEmpty()) { lastActivity = millis(); if (sleeping) { sleeping = false; setBacklight(true); } playEnterSound(); notes[0] = pendingWebNote.substring(0, 50); noteLine = 0; noteCount = 1; markStateDirty(); pendingWebNote = ""; webStatus = "Text sent to Notes"; redrawNeeded = true; }
  if (!pendingZabkaSecret.isEmpty()) {
    lastActivity = millis();
    if (sleeping) { sleeping = false; setBacklight(true); }
    // The original app saves {"secret":"HEX...","ployId":"..."}.
    // Also accept a bare HEX secret for users who paste only that field.
    String source = pendingZabkaSecret;
    int secretKey = source.indexOf("\"secret\"");
    if (secretKey >= 0) {
      int colon = source.indexOf(':', secretKey), firstQuote = colon < 0 ? -1 : source.indexOf('\"', colon + 1);
      int secondQuote = firstQuote < 0 ? -1 : source.indexOf('\"', firstQuote + 1);
      if (firstQuote >= 0 && secondQuote > firstQuote) source = source.substring(firstQuote + 1, secondQuote);
    }
    zabkaSecret = "";
    for (int i = 0; i < (int)source.length() && zabkaSecret.length() < 128; ++i) {
      char c = source[i];
      if (isxdigit((unsigned char)c)) zabkaSecret += char(toupper((unsigned char)c));
    }
    String idSource = pendingZabkaPloyId;
    int ployKey = pendingZabkaSecret.indexOf("\"ployId\"");
    if (idSource.isEmpty() && ployKey >= 0) {
      int colon = pendingZabkaSecret.indexOf(':', ployKey), firstQuote = colon < 0 ? -1 : pendingZabkaSecret.indexOf('\"', colon + 1);
      int secondQuote = firstQuote < 0 ? -1 : pendingZabkaSecret.indexOf('\"', firstQuote + 1);
      if (firstQuote >= 0 && secondQuote > firstQuote) idSource = pendingZabkaSecret.substring(firstQuote + 1, secondQuote);
    }
    String id = "";
    for (int i = 0; i < (int)idSource.length(); ++i) if (isDigit(idSource[i])) id += idSource[i];
    if (!id.isEmpty()) zabkaPloyId = id;
    pendingZabkaSecret = ""; pendingZabkaPloyId = "";
    page = ZABKATOTP;
    zabkaStatus = zabkaSecret.isEmpty() || (zabkaSecret.length() & 1) ? "Invalid HEX secret was pasted." : "HEX secret pasted to RAM only; press ENTER to sync time.";
    playEnterSound();
    webStatus = "ZABKA TOTP secret transferred to RAM only";
    redrawNeeded = true;
  }
  if (!pendingZabkaVaultSecret.isEmpty() || !pendingZabkaVaultPassphrase.isEmpty()) {
    String source = pendingZabkaVaultSecret;
    int secretKey = source.indexOf("\"secret\"");
    if (secretKey >= 0) {
      int colon = source.indexOf(':', secretKey), firstQuote = colon < 0 ? -1 : source.indexOf('\"', colon + 1);
      int secondQuote = firstQuote < 0 ? -1 : source.indexOf('\"', firstQuote + 1);
      if (firstQuote >= 0 && secondQuote > firstQuote) source = source.substring(firstQuote + 1, secondQuote);
    }
    // The encrypted vault record has room for at most 62 HEX characters.
    String cleanSecret = "";
    for (int i = 0; i < (int)source.length() && cleanSecret.length() < 62; ++i) { char c = source[i]; if (isxdigit((unsigned char)c)) cleanSecret += char(toupper((unsigned char)c)); }
    String idSource = pendingZabkaVaultPloyId;
    int ployKey = pendingZabkaVaultSecret.indexOf("\"ployId\"");
    if (idSource.isEmpty() && ployKey >= 0) {
      int colon = pendingZabkaVaultSecret.indexOf(':', ployKey), firstQuote = colon < 0 ? -1 : pendingZabkaVaultSecret.indexOf('\"', colon + 1);
      int secondQuote = firstQuote < 0 ? -1 : pendingZabkaVaultSecret.indexOf('\"', firstQuote + 1);
      if (firstQuote >= 0 && secondQuote > firstQuote) idSource = pendingZabkaVaultSecret.substring(firstQuote + 1, secondQuote);
    }
    String id = "";
    for (int i = 0; i < (int)idSource.length(); ++i) if (isDigit(idSource[i])) id += idSource[i];
    if (!id.isEmpty()) zabkaPloyId = id;
    bool saved = !(cleanSecret.length() & 1) && storeZabkaVault(cleanSecret, pendingZabkaVaultPassphrase);
    pendingZabkaVaultSecret = ""; pendingZabkaVaultPloyId = ""; pendingZabkaVaultPassphrase = ""; zabkaSecret = ""; page = ZABKATOTP;
    zabkaStatus = saved ? "Encrypted vault saved and locked. Unlock locally." : "Vault not saved: use HEX secret and 8+ character password.";
    webStatus = saved ? "ZABKA vault encrypted in flash" : "ZABKA vault save rejected"; playEnterSound(); redrawNeeded = true;
  }
  if (!pendingZabkaUnlockPassphrase.isEmpty()) {
    bool unlocked = unlockZabkaVault(pendingZabkaUnlockPassphrase); pendingZabkaUnlockPassphrase = ""; page = ZABKATOTP;
    zabkaStatus = unlocked ? "Vault unlocked to RAM; press ENTER to sync time." : "Could not unlock vault: wrong password or corrupt record.";
    webStatus = unlocked ? "ZABKA vault unlocked to RAM" : "ZABKA vault unlock failed"; playEnterSound(); redrawNeeded = true;
  }
  if (!pendingWebCLab.isEmpty()) { lastActivity = millis(); if (sleeping) { sleeping = false; setBacklight(true); } playEnterSound(); bool runAfterSave = pendingWebCLabRun; pendingWebCLabRun = false; cLineCount = 0; int start = 0; while (cLineCount < C_MAX_LINES) { int end = pendingWebCLab.indexOf('\n', start); String line = end < 0 ? pendingWebCLab.substring(start) : pendingWebCLab.substring(start, end); cLines[cLineCount++] = line.substring(0, C_MAX_LINE_CHARS); if (end < 0) break; start = end + 1; } if (!cLineCount) cLineCount = 1; cCursorLine = cCursorColumn = cScrollLine = cHorizontalScroll = 0; pendingWebCLab = ""; markStateDirty(); page = CLAB; if (runAfterSave) { cInputValueCount = 0; cLabQrActive = false; cLabQrPayload = ""; runCLab(); webStatus = cInputActive ? "C LAB waits for input on Cardputer" : "C LAB code ran on Cardputer"; } else webStatus = "C LAB code updated"; redrawNeeded = true; }
}
// The full repaint: one big dispatch to the current page's drawX() function,
// which always redraws its whole screen (header, content, footer) from
// scratch. Called from loop() whenever `page` just changed or a
// forceFullRedraw was requested (see the rendering-model note at the top of
// this file) - never called every frame, only on real scene changes.
void draw() {
  // Capture "the app before this one" for the quick-launch overlay's LAST
  // APP tile, ahead of overwriting lastDrawnPage below. System transitions
  // (lock/screensaver/the Wi-Fi setup sub-flow, on either side of the
  // change) don't count as an app worth resuming into.
  if (lastDrawnPage != page &&
      lastDrawnPage != LOCKSCREEN && lastDrawnPage != SCREENSAVER && lastDrawnPage != WIFISETUP &&
      page != LOCKSCREEN && page != SCREENSAVER && page != WIFISETUP) {
    previousPage = lastDrawnPage;
  }
  if (page == LOCKSCREEN) drawLockScreen(); else if (page == WIFISETUP) drawWifiSetup(); else if (page == LAUNCHER) drawLauncher(); else if (page == SYSTEM) drawSystem(); else if (page == WIFI) drawWifi(); else if (page == NOTES) drawNotes(); else if (page == CLOCK) drawClock(); else if (page == CALC) drawCalc(); else if (page == CLAB) drawCLab(); else if (page == CARDCREPL) { if (cLabQrActive) drawCLabQR(); else drawCardCRepl(); } else if (page == QRTEXT) drawQRText(); else if (page == SETTINGS) drawSettings(); else if (page == TEXTTOOLS) drawTextTools(); else if (page == FAVOURITES) drawFavourites(); else if (page == WIFIMONITOR) drawWifiMonitor(); else if (page == FILEBROWSER) drawFileBrowser(); else if (page == HOMEEDITOR) drawHomeEditor(); else if (page == CLABEXAMPLES) drawCLabExamples(); else if (page == DASHBOARD) drawDashboard(); else if (page == DICERANDOM) drawDiceRandom(); else if (page == GAMEHUB) drawGameHub(); else if (page == DEVICECHECK) drawDeviceCheck(); else if (page == QRTOOLSPLUS) drawQRToolsPlus(); else if (page == MINIPAINT) drawMiniPaint(); else if (page == LAUNCHERSEARCH) drawLauncherSearch(); else if (page == TEXTBROWSER) drawTextBrowser(); else if (page == INPOSTTRACK) drawInPostTrack(); else if (page == ZABKATOTP) drawZabkaTotp(); else if (page == MUSICLAB) drawMusicLab(); else if (page == MIC) drawMic(); else if (page == BLEKEYBOARD) drawBleKeyboard(); else if (page == SCREENSAVER) drawScreensaver(); else drawWebCompanion(); lastDrawnPage = page; redrawNeeded = false; updateBuiltinDisplay(true); }

// Repaint only a changed application's content. Headers and footers are kept
// intact; full draw() remains reserved for entering a different scene, modal
// dialog, QR image, lock view, or a deliberate test screen.
void refreshLocalPage() {
  switch (page) {
    case SYSTEM: updateSystemValues(); break;
    case CLOCK: updateClockValue(); break;
    case CALC: updateCalcPanel(); break;
    case SETTINGS:
      if (!pinChangeActive) for (int i = 0; i < SETTINGS_COUNT; ++i) updateSettingsRow(i);
      else drawSettings();
      break;
    case GAMEHUB:
      if (gameMode != lastDrawnGameMode) drawGameHub();
      else if (gameMode == 0) drawGameMenuList();
      else if (gameMode == 1) drawGameHub();  // rare: crash / restart, footer text changes too
      else if (gameMode == 2) drawGridHuntGrid();
      else if (gameMode == 3) drawKartHub();
      break;
    case DICERANDOM:
      tft.fillRect(12, CONTENT_Y + 25, 296, 130, ui.bg);
      tft.setTextSize(5); tft.setTextColor(ui.accent, ui.bg); tft.setCursor(22, CONTENT_Y + 31); tft.print(diceValue);
      tft.setTextSize(1); tft.setTextColor(ui.text, ui.bg); tft.setCursor(20, CONTENT_Y + 79); tft.print("D6 DICE");
      tft.setTextSize(3); tft.setCursor(143, CONTENT_Y + 40); tft.print(coinHeads ? "HEADS" : "TAILS");
      tft.setTextSize(1); tft.setTextColor(ui.dim, ui.bg); tft.setCursor(145, CONTENT_Y + 79); tft.print("COIN");
      tft.drawFastHLine(12, CONTENT_Y + 94, 296, ui.dim);
      tft.setTextSize(3); tft.setTextColor(ui.accent, ui.bg); tft.setCursor(92, CONTENT_Y + 112); tft.print(randomValue);
      tft.setTextSize(1); tft.setTextColor(ui.dim, ui.bg); tft.setCursor(92, CONTENT_Y + 145); tft.print("RANDOM 0-999");
      break;
    case MINIPAINT:
      // The paint grid is a small bounded rectangle, not a whole-panel clear.
      for (int y = 0; y < 12; ++y) for (int x = 0; x < 16; ++x) {
        int px = 48 + x * 14, py = 35 + y * 14;
        tft.fillRect(px, py, 13, 13, paintPixels[y][x] ? ui.accent : ILI9341_DARKGREY);
      }
      tft.drawRect(48 + paintX * 14 - 1, 35 + paintY * 14 - 1, 15, 15, ILI9341_WHITE);
      break;
    case MUSICLAB:
      // Playback already refreshes two cells.  A manual edit changes only the
      // grid plus its compact status strip, never the entire screen.
      for (uint8_t track = 0; track < DRUM_TRACKS; ++track) for (uint8_t step = 0; step < DRUM_STEPS; ++step) drawMusicCell(track, step);
      tft.fillRect(8, CONTENT_Y + 136, 300, 16, ui.bg);
      tft.setTextSize(1); tft.setTextColor(drumPlaying ? ILI9341_GREEN : ui.accent, ui.bg); tft.setCursor(10, CONTENT_Y + 142); tft.print(drumPlaying ? "PLAYING" : "STOPPED");
      tft.setTextColor(ui.text, ui.bg); tft.setCursor(112, CONTENT_Y + 142); tft.print("TEMPO " + String(drumTempo) + " BPM");
      break;
    case NOTES: drawNotes(); break;
    case TEXTTOOLS: drawTextTools(); break;
    case LAUNCHERSEARCH: drawLauncherSearch(); break;
    case TEXTBROWSER: drawTextBrowser(); break;
    case INPOSTTRACK: drawInPostTrack(); break;
    case ZABKATOTP: drawZabkaTotp(); break;
    case WIFISETUP: drawWifiSetup(); break;
    case WIFI: drawWifi(); break;
    case WIFIMONITOR: drawWifiMonitor(); break;
    case DASHBOARD: drawDashboard(); break;
    case DEVICECHECK: drawDeviceCheck(); break;
    case QRTOOLSPLUS: drawQRToolsPlus(); break;
    case FAVOURITES: drawFavourites(); break;
    case FILEBROWSER: drawFileBrowser(); break;
    case HOMEEDITOR: drawHomeEditor(); break;
    case CLABEXAMPLES: drawCLabExamples(); break;
    case WEBCOMPANION: drawWebCompanion(); break;
    case BLEKEYBOARD: drawBleKeyboard(); break;
    case CARDCREPL: drawCardCRepl(); break;
    case CLAB: drawCLab(); break;
    default: draw(); return;
  }
  redrawNeeded = false;
  updateBuiltinDisplay();
}
void startScan() {
  if (scanRunning) return;
  // A scan may run while STA is connected to the Web Companion network.
  // Do not disconnect or switch Wi-Fi modes in that case.
  if (!webRunning) {
    WiFi.mode(WIFI_STA);
    WiFi.disconnect();
    delay(100);
  }
  WiFi.scanDelete();
  WiFi.scanNetworks(true);
  scanRunning = true;
  scanDone = false;
  scanStarted = millis();
  redrawNeeded = true;
}
void checkScan() { if (!scanRunning) return; int r = WiFi.scanComplete(); if (r >= 0) { networkCount = r; scanRunning = false; scanDone = true; redrawNeeded = true; } else if (millis() - scanStarted > 20000UL) { WiFi.scanDelete(); networkCount = 0; scanRunning = false; scanDone = true; redrawNeeded = true; } }
void calcResult() { float v = calcInput.toFloat(); if (calcOp == 0) calcTotal = v; else if (calcOp == '+') calcTotal += v; else if (calcOp == '-') calcTotal -= v; else if (calcOp == '*') calcTotal *= v; else if (calcOp == '/') { if (v == 0) { calcStatus = "Error: division by zero"; calcInput = "0"; calcOp = 0; calcNew = true; updateCalcPanel(); return; } calcTotal /= v; } calcInput = String(calcTotal, 4); while (calcInput.endsWith("0")) calcInput.remove(calcInput.length() - 1); if (calcInput.endsWith(".")) calcInput.remove(calcInput.length() - 1); calcStatus = "Result"; calcOp = 0; calcNew = true; updateCalcPanel(); }
void changeSetting(int d) { if (settingSelected == 0) { themeIndex = (themeIndex + d + THEME_COUNT) % THEME_COUNT; applyTheme(); } else if (settingSelected == 1) { displayRotation = displayRotation == 3 ? 1 : 3; tft.setRotation(displayRotation); } else if (settingSelected == 2) setBacklight(!backlightOn); else if (settingSelected == 3) { brightnessLevel = (brightnessLevel + d + 9) % 10 + 1; applyBacklight(); } else if (settingSelected == 4) sleepIndex = (sleepIndex + d + 5) % 5; else if (settingSelected == 5) lockAfterScreensaverIndex = (lockAfterScreensaverIndex + d + 5) % 5; else if (settingSelected == 6) { volumeLevel = (volumeLevel + d + 11) % 11; applyVolume(); } markStateDirty(); redrawNeeded = true; }
void syncLauncherScroll() { if (appSelected < appScroll) appScroll = appSelected; if (appSelected >= appScroll + APP_VISIBLE) appScroll = appSelected - APP_VISIBLE + 1; appScroll = constrain(appScroll, 0, max(0, SECONDARY_APP_COUNT - APP_VISIBLE)); }
void clabBackspace() {
  bool changed = false, merged = false;
  if (cCursorColumn > 0) {
    cLines[cCursorLine].remove(cCursorColumn - 1, 1);
    cCursorColumn--;
    changed = true;
  } else if (cCursorLine > 0) {
    int oldLength = cLines[cCursorLine - 1].length();
    if (oldLength + (int)cLines[cCursorLine].length() <= C_MAX_LINE_CHARS) {
      cLines[cCursorLine - 1] += cLines[cCursorLine];
      for (int i = cCursorLine; i < cLineCount - 1; i++) cLines[i] = cLines[i + 1];
      cLineCount--;
      cLines[cLineCount] = "";
      cCursorLine--;
      cCursorColumn = oldLength;
      if (cCursorLine < cScrollLine) cScrollLine = cCursorLine;
      cHorizontalScroll = max(0, cCursorColumn - C_CODE_COLUMNS_VISIBLE + 1);
      changed = true;
      merged = true;
    }
  }
  if (!changed) return;
  playBackspaceSound();
  int maxHorizontalScroll = max(0, (int)cLines[cCursorLine].length() - C_CODE_COLUMNS_VISIBLE + 1);
  cHorizontalScroll = constrain(cHorizontalScroll, 0, maxHorizontalScroll);
  if (merged) drawCLabEditor(); else drawCLabCodeLine(cCursorLine);
  markStateDirty();
}
void moveCLabCursor(int direction) {
  int oldLine = cCursorLine, oldScroll = cScrollLine;
  if (direction == -2 && cCursorLine > 0) cCursorLine--; else if (direction == 2 && cCursorLine < cLineCount - 1) cCursorLine++; else if (direction == -1 && cCursorColumn > 0) cCursorColumn--; else if (direction == 1 && cCursorColumn < (int)cLines[cCursorLine].length()) cCursorColumn++; else return;
  cCursorColumn = min(cCursorColumn, (int)cLines[cCursorLine].length()); if (cCursorLine < cScrollLine) cScrollLine = cCursorLine; if (cCursorLine >= cScrollLine + C_CODE_VISIBLE_LINES) cScrollLine = cCursorLine - C_CODE_VISIBLE_LINES + 1;
  int maxHorizontalScroll = max(0, (int)cLines[cCursorLine].length() - C_CODE_COLUMNS_VISIBLE + 1);
  if (cCursorColumn < cHorizontalScroll) cHorizontalScroll = cCursorColumn;
  if (cCursorColumn >= cHorizontalScroll + C_CODE_COLUMNS_VISIBLE) cHorizontalScroll = cCursorColumn - C_CODE_COLUMNS_VISIBLE + 1;
  cHorizontalScroll = constrain(cHorizontalScroll, 0, maxHorizontalScroll);
  playCursorSound();
  if (page == CLAB && !sleeping) { if (oldScroll != cScrollLine) drawCLabEditor(); else { drawCLabCodeLine(oldLine); drawCLabCodeLine(cCursorLine); } } else redrawNeeded = true;
}
void handleCLabCursorKeys() {
  static int lastDirection = 0;
  static unsigned long nextMoveAt = 0;
  if (page != CLAB || sleeping || cLabGuideVisible) { lastDirection = 0; return; }
  auto k = M5Cardputer.Keyboard.keysState();
  if (!k.ctrl && !cNavigationMode) { lastDirection = 0; return; }
  int direction = 0;
  if (M5Cardputer.Keyboard.isKeyPressed(';') || M5Cardputer.Keyboard.isKeyPressed('w')) direction = -2;
  else if (M5Cardputer.Keyboard.isKeyPressed('.') || M5Cardputer.Keyboard.isKeyPressed('s')) direction = 2;
  else if (M5Cardputer.Keyboard.isKeyPressed(',') || M5Cardputer.Keyboard.isKeyPressed('a')) direction = -1;
  else if (M5Cardputer.Keyboard.isKeyPressed('/') || M5Cardputer.Keyboard.isKeyPressed('d')) direction = 1;
  if (direction == 0) { lastDirection = 0; return; }
  unsigned long now = millis();
  if (direction != lastDirection || now >= nextMoveAt) {
    bool firstPress = direction != lastDirection;
    moveCLabCursor(direction);
    lastDirection = direction;
    nextMoveAt = now + (firstPress ? 180UL : 70UL);
  }
}

// Helper: check if a char exists in the std::vector<char> word list
static bool wordContains(const std::vector<char>& word, char c) {
  for (char ch : word) { if (ch == c) return true; }
  return false;
}

// Single input dispatch, called once per loop(). Order matters: LOCKSCREEN,
// sleeping and SCREENSAVER are handled (and returned from) first since they
// intercept every key globally; then Fn (universal back) and Opt (the
// quick-launch overlay) get first refusal on every remaining keystroke,
// ahead of whatever `page` (and, for GAMEHUB, `gameMode`) specific handling
// follows below.
void keyboard() {
  auto k = M5Cardputer.Keyboard.keysState(); bool event = M5Cardputer.Keyboard.isChange(), fn = k.fn;
  if (page == LOCKSCREEN) {
    // Any key wakes the lock view; it never bypasses PIN verification.
    if (sleeping) {
      if (event || fn != fnLast) { sleeping = false; setBacklight(true); lastActivity = millis(); redrawNeeded = true; }
      fnLast = fn;
      return;
    }
    if (!event || !M5Cardputer.Keyboard.isPressed()) { fnLast = fn; return; }
    // PIN entry is real interaction: do not let the lock-screen backlight
    // timeout expire while the user is entering digits.
    lastActivity = millis();
    if (lockRetryAt > millis()) { fnLast = fn; return; }
    if (k.del) {
      if (!lockPinInput.isEmpty()) { lockPinInput.remove(lockPinInput.length() - 1); playBackspaceSound(); redrawNeeded = true; }
      fnLast = fn;
      return;
    }
    if (k.enter) {
      if (checkLockPin(lockPinInput)) {
        // Unlock is a complete state transition. This also covers the startup
        // path, where the lock scene was shown without a preceding wake key.
        lockPinInput = "";
        lockFailures = 0;
        lockRetryAt = 0;
        sleeping = false;
        lockScreenBaseDrawn = false;
        setBacklight(true);
        lastActivity = millis();
        // Show a deliberate post-login transition before the Home scene.
        // drawWelcomeScreen() owns the three-second animation and returns to
        // the normal launcher redraw without exposing the previous lock view.
        drawWelcomeScreen();
        page = LAUNCHER;
        launcherHome = true;
        cardcLedOverride = false;
        updateStatusLed();
        playEnterSound();
        redrawNeeded = true;
      } else {
        lockPinInput = ""; lockFailures++;
        // Slow repeated guessing without making a forgotten PIN permanently fatal.
        if (lockFailures >= 3) { lockRetryAt = millis() + 10000UL; lockFailures = 0; }
        playExitSound(); redrawNeeded = true;
      }
      fnLast = fn;
      return;
    }
    bool changed = false;
    for (char c : k.word) if (c >= '0' && c <= '9' && lockPinInput.length() < 4) { lockPinInput += c; changed = true; }
    if (changed) { playTypingSound(); redrawNeeded = true; }
    fnLast = fn;
    return;
  }
  if (sleeping) { if (event || fn != fnLast) { sleeping = false; setBacklight(true); lastActivity = millis(); redrawNeeded = true; } fnLast = fn; return; }
  if (!backlightOn && (event || fn != fnLast)) { setBacklight(true); lastActivity = millis(); redrawNeeded = true; fnLast = fn; return; }
  // Consume the wake key so it cannot operate the application underneath.
  if (page == SCREENSAVER) {
    if (event || fn != fnLast) {
      page = screensaverReturnPage;
      screensaverDimmed = false;
      applyBacklight();
      lastActivity = millis();
      redrawNeeded = true;
    }
    fnLast = fn;
    return;
  }
  if (event || fn != fnLast) lastActivity = millis();
  // APPS is a launcher subview, so Fn must also work there even though its
  // page value remains LAUNCHER.
  // Fn closes the quick-launch overlay first, ahead of every page's own
  // Fn/"back" behaviour below - the overlay sits on top of whatever page is
  // running and must never leak a Fn press through to it.
  if (fn && !fnLast) {
    if (quickMenuOpen) { quickMenuOpen = false; playExitSound(); closeQuickMenuAnimated(); forceFullRedraw = true; redrawNeeded = true; }
    else if (page != LAUNCHER || !launcherHome) { playExitSound(); if (page == SETTINGS && pinChangeActive) { pinChangeActive = false; pinChangeConfirm = false; pinChangeFirst = ""; pinChangeInput = ""; pinChangeStatus = "PIN change cancelled"; } else if (page == ZABKATOTP && zabkaUnlocking) { zabkaUnlocking = false; zabkaUnlockBuffer = ""; zabkaStatus = "Vault remains locked."; } else if (page == CLAB && cLabNameDialogVisible) { cLabNameDialogVisible = false; cLabNameBuffer = ""; } else if (page == CLAB && cLabSlotDialogVisible) { cLabSlotDialogVisible = false; } else if (page == CLAB && cLabSaveDialogVisible) { cLabSaveDialogVisible = false; } else if (page == CLAB && cLabExplorerVisible) { cardcLedOverride = false; updateStatusLed(); page = LAUNCHER; launcherHome = true; } else if (page == GAMEHUB && gameMode != 0) { gameMode = 0; snakeRunning = false; kartRaceState = KART_HOME; kartMusicEngineStop(); } else if (page == CLAB && cInputActive) { cInputActive = false; cInputValueCount = 0; cInputReadIndex = 0; cInputBuffer = ""; cLabExplorerVisible = true; } else if (page == CLAB && cCanvasActive) { cCanvasActive = false; } else if (page == CLAB && cLabGuideVisible) cLabGuideVisible = false; else if ((page == CLAB || page == CARDCREPL) && cLabQrActive) cLabQrActive = false; else if (page == CLAB) { if (cLabDirty) { cLabSaveDialogSelected = 0; cLabSaveDialogVisible = true; } else cLabExplorerVisible = true; } else { page = LAUNCHER; launcherHome = true; } redrawNeeded = true; }
  }
  fnLast = fn;
  // Opt toggles the same floating quick-launch overlay from any page, any
  // time - independent of whatever the current page's own keys mean. It is
  // edge-triggered the same way Fn is above (optLast mirrors fnLast) so
  // holding the key doesn't retrigger every loop() tick.
  if (k.opt && !optLast) {
    quickMenuOpen = !quickMenuOpen;
    quickMenuSelected = 0;
    playMenuSound();
    // Opening animates and paints immediately so the overlay appears the
    // instant the key is pressed, rather than waiting on the
    // redrawNeeded/draw() dispatch further down in loop() - that dispatch
    // doesn't know about this overlay and would just redraw the page
    // underneath it.
    if (quickMenuOpen) openQuickMenuAnimated();
    // Closing animates it away, then leans on the same dispatch: the panel
    // is write-only so there is no way to "erase" just the covered pixels,
    // so forceFullRedraw asks loop() for one full draw() of the current
    // page even though `page` itself never changed.
    else { closeQuickMenuAnimated(); forceFullRedraw = true; redrawNeeded = true; }
  }
  optLast = k.opt;
  if (quickMenuOpen) {
    // The overlay owns all input while open; the page underneath must not
    // also react to the same keystroke (e.g. GAMEHUB's own ;/. handling).
    if (!event || !M5Cardputer.Keyboard.isPressed()) return;
    // A single horizontal row: any of the four direction keys cycles it,
    // ;/, stepping back and ./  stepping forward, wrapping at both ends.
    int oldSelected = quickMenuSelected;
    bool prev = wordContains(k.word, ';') || wordContains(k.word, ',');
    bool next = wordContains(k.word, '.') || wordContains(k.word, '/');
    if (prev) quickMenuSelected = (quickMenuSelected + QUICKMENU_TILE_COUNT - 1) % QUICKMENU_TILE_COUNT;
    else if (next) quickMenuSelected = (quickMenuSelected + 1) % QUICKMENU_TILE_COUNT;
    if (quickMenuSelected != oldSelected) { playMenuSound(); drawQuickMenuTile(oldSelected, false); drawQuickMenuTile(quickMenuSelected, true); drawQuickMenuCaption(); }
    if (k.enter) {
      playEnterSound();
      quickMenuOpen = false;
      closeQuickMenuAnimated();
      forceFullRedraw = true;
      redrawNeeded = true;
      // Tile 6 resumes previousPage; tile 5 is always "APPS" (the full app
      // list); tiles 0-4 mirror Home's five configurable slots verbatim,
      // empty slots included.
      if (quickMenuSelected == HOME_TILE_COUNT) { page = previousPage; }
      else if (quickMenuSelected == 5) { page = LAUNCHER; launcherHome = false; appSelected = appScroll = 0; }
      else if (homeAppIndices[quickMenuSelected] >= 0) page = static_cast<Page>(homeAppIndices[quickMenuSelected] + 1);
    }
    return;
  }
  if (!event || !M5Cardputer.Keyboard.isPressed()) return;
  if (k.ctrl || k.shift || k.alt) playFunctionSound();
  bool ctrlF = k.ctrl && (wordContains(k.word, 'f') || wordContains(k.word, 'F') || M5Cardputer.Keyboard.isKeyPressed('f') || M5Cardputer.Keyboard.isKeyPressed('F'));
  if (ctrlF && page != LAUNCHER && page != FAVOURITES) { int appIndex = (int)page - 1; if (appIndex >= 0 && appIndex < APP_COUNT) { appFavourite[appIndex] = !appFavourite[appIndex]; markStateDirty(); playFunctionSound(); } return; }
  if (page == CLAB && cLabSaveDialogVisible) {
    // Match the launcher's proven word events, with direct polling retained
    // as a fallback for ADV keyboard events that omit punctuation.
    bool previous = wordContains(k.word, ',') || wordContains(k.word, ';') ||
                    M5Cardputer.Keyboard.isKeyPressed(',') || M5Cardputer.Keyboard.isKeyPressed(';');
    bool next = wordContains(k.word, '/') || wordContains(k.word, '.') ||
                M5Cardputer.Keyboard.isKeyPressed('/') || M5Cardputer.Keyboard.isKeyPressed('.');
    if (previous || next) {
      int oldSelected = cLabSaveDialogSelected;
      cLabSaveDialogSelected = previous ? (cLabSaveDialogSelected + 2) % 3 : (cLabSaveDialogSelected + 1) % 3;
      playMenuSound(); drawCLabSaveChoice(oldSelected); drawCLabSaveChoice(cLabSaveDialogSelected);
    }
    if (k.enter) { if (cLabSaveDialogSelected == 0) cLabSaveDialogVisible = false; else if (cLabSaveDialogSelected == 1) { cLabSaveDialogVisible = false; cLabSlotDialogSelected = constrain(cLabActiveUserApp, 0, C_USER_APP_COUNT - 1); cLabSlotDialogVisible = true; } else { cLabDirty = false; cLabSaveDialogVisible = false; cLabExplorerVisible = true; } playEnterSound(); redrawNeeded = true; }
    return;
  }
  if (page == CLAB && cLabSlotDialogVisible) {
    bool up = wordContains(k.word, ';') || M5Cardputer.Keyboard.isKeyPressed(';');
    bool down = wordContains(k.word, '.') || M5Cardputer.Keyboard.isKeyPressed('.');
    if (up || down) {
      cLabSlotDialogSelected = up ? (cLabSlotDialogSelected + C_USER_APP_COUNT - 1) % C_USER_APP_COUNT : (cLabSlotDialogSelected + 1) % C_USER_APP_COUNT;
      playMenuSound(); redrawNeeded = true;
    }
    if (k.enter) {
      // Build the source from the editor first. This writes only the selected
      // destination slot and never overwrites the previously active slot.
      String savedSource;
      for (int i = 0; i < cLineCount; ++i) { if (i) savedSource += '\n'; savedSource += cLines[i]; }
      cUserApps[cLabSlotDialogSelected] = savedSource;
      cLabActiveUserApp = cLabSlotDialogSelected;
      cLabFileName = String("user-app-") + String(cLabActiveUserApp + 1) + ".clab";
      cLabDirty = false;
      cLabSlotDialogVisible = false;
      cLabExplorerVisible = true;
      markStateDirty(); playEnterSound(); redrawNeeded = true;
    }
    return;
  }
  if (page == CLAB && cLabNameDialogVisible) {
    if (k.enter) { saveCLabFile(); cLabExplorerVisible = true; playEnterSound(); redrawNeeded = true; return; }
    if (k.del) { if (!cLabNameBuffer.isEmpty()) { cLabNameBuffer.remove(cLabNameBuffer.length() - 1); playBackspaceSound(); drawCLabNameField(); } return; }
    bool changed = false;
    for (char c : k.word) if ((isAlphaNumeric((unsigned char)c) || c == '_' || c == '-') && cLabNameBuffer.length() < 24) { cLabNameBuffer += c; changed = true; }
    if (changed) { playTypingSound(); drawCLabNameField(); }
    return;
  }
  if (page == CLAB && cLabExplorerVisible) {
    bool up = wordContains(k.word, ';') || M5Cardputer.Keyboard.isKeyPressed(';');
    bool down = wordContains(k.word, '.') || M5Cardputer.Keyboard.isKeyPressed('.');
    bool ctrlEnter = k.ctrl && k.enter;
    if (up || down) {
      cLabExplorerSelected = up ? (cLabExplorerSelected + C_LAB_EXPLORER_ITEMS - 1) % C_LAB_EXPLORER_ITEMS : (cLabExplorerSelected + 1) % C_LAB_EXPLORER_ITEMS;
      playMenuSound();
      // Scroll changes need a fresh five-row window; otherwise only the old
      // and new selection rows are touched.
      int nextScroll = cLabExplorerScroll;
      if (cLabExplorerSelected < nextScroll) nextScroll = cLabExplorerSelected;
      if (cLabExplorerSelected >= nextScroll + C_LAB_EXPLORER_VISIBLE) nextScroll = cLabExplorerSelected - C_LAB_EXPLORER_VISIBLE + 1;
      if (nextScroll != cLabExplorerScroll) { cLabExplorerScroll = nextScroll; redrawNeeded = true; }
      else { drawCLabExplorerRow(up ? (cLabExplorerSelected + 1) % C_LAB_EXPLORER_ITEMS : (cLabExplorerSelected + C_LAB_EXPLORER_ITEMS - 1) % C_LAB_EXPLORER_ITEMS); drawCLabExplorerRow(cLabExplorerSelected); }
    }
    if (ctrlEnter && cLabExplorerSelected < C_USER_APP_COUNT) { openCLabUserApp(cLabExplorerSelected, true); return; }
    if (k.enter) {
      playEnterSound();
      if (cLabExplorerSelected < C_USER_APP_COUNT) openCLabUserApp(cLabExplorerSelected, false);
      else if (cLabExplorerSelected == C_USER_APP_COUNT) { cExampleSelected = 0; page = CLABEXAMPLES; redrawNeeded = true; }
      else { cLabActiveUserApp = 0; openNewCLabFile(); redrawNeeded = true; }
    }
    return;
  }
  bool ctrlS = k.ctrl && (wordContains(k.word, 's') || wordContains(k.word, 'S') || M5Cardputer.Keyboard.isKeyPressed('s') || M5Cardputer.Keyboard.isKeyPressed('S'));
  if (page == CLAB && ctrlS) { cLabSlotDialogSelected = constrain(cLabActiveUserApp, 0, C_USER_APP_COUNT - 1); cLabSlotDialogVisible = true; playFunctionSound(); redrawNeeded = true; return; }
  bool ctrlO = k.ctrl && (wordContains(k.word, 'o') || wordContains(k.word, 'O') || M5Cardputer.Keyboard.isKeyPressed('o') || M5Cardputer.Keyboard.isKeyPressed('O'));
  if (page == CLAB && ctrlO) { if (cLabDirty) { cLabSaveDialogSelected = 0; cLabSaveDialogVisible = true; } else cLabExplorerVisible = true; playFunctionSound(); redrawNeeded = true; return; }
  if (page == CLAB && k.tab && !cLabGuideVisible) { playTabSound(); cNavigationMode = !cNavigationMode; redrawNeeded = true; return; }
  bool ctrlG = k.ctrl && (wordContains(k.word, 'g') || wordContains(k.word, 'G') || M5Cardputer.Keyboard.isKeyPressed('g') || M5Cardputer.Keyboard.isKeyPressed('G'));
  if (page == CLAB && ctrlG) { cLabGuideVisible = !cLabGuideVisible; if (cLabGuideVisible) cGuideScroll = 0; playFunctionSound(); redrawNeeded = true; return; }
  if (page == CLAB && cLabGuideVisible) {
    int oldScroll = cGuideScroll;
    for (char c : k.word) {
      if (c == ';') cGuideScroll = max(0, cGuideScroll - 1);
      else if (c == '.') cGuideScroll = min(C_GUIDE_CONTENT_LINES - C_GUIDE_VISIBLE, cGuideScroll + 1);
    }
    if (cGuideScroll != oldScroll) { playCursorSound(); drawCLabGuideContent(); }
    return;
  }
  if (page == CLAB && cInputActive) {
    if (cInputReadKey) {
      for (char c : k.word) if (c >= 32 && c <= 126) {
        if (cInputValueCount < 4) cInputValues[cInputValueCount++] = String(c);
        cInputReadKey = false; cInputActive = false; playEnterSound(); runCLab(); return;
      }
      return;
    }
    if (k.enter) {
      if (cInputNumeric) {
        String number = cInputBuffer; number.trim();
        char* end = nullptr; strtol(number.c_str(), &end, 10);
        if (number.isEmpty() || !end || *end != 0) { cLog("ERR: inputint needs a whole number"); playExitSound(); drawCLabInput(); return; }
      }
      if (cInputValueCount < 4) cInputValues[cInputValueCount++] = cInputBuffer;
      cInputNumeric = false; playEnterSound(); runCLab(); return;
    }
    if (k.del) { if (!cInputBuffer.isEmpty()) { cInputBuffer.remove(cInputBuffer.length() - 1); playBackspaceSound(); drawCLabInput(); } return; }
    bool changed = false; for (char c : k.word) {
      bool allowed = cInputNumeric ? (isDigit((unsigned char)c) || (c == '-' && cInputBuffer.isEmpty())) : (c >= 32 && c <= 126);
      if (allowed && cInputBuffer.length() < 48) { cInputBuffer += c; changed = true; }
    }
    if (changed) { playTypingSound(); drawCLabInput(); } return;
  }
  bool ctrlD = k.ctrl && (wordContains(k.word, 'd') || wordContains(k.word, 'D') || M5Cardputer.Keyboard.isKeyPressed('d') || M5Cardputer.Keyboard.isKeyPressed('D'));
  if (page == CLAB && ctrlD) { loadCLabDemo(); return; }
  bool ctrlE = k.ctrl && (wordContains(k.word, 'e') || wordContains(k.word, 'E') || M5Cardputer.Keyboard.isKeyPressed('e') || M5Cardputer.Keyboard.isKeyPressed('E'));
  if (page == CLAB && ctrlE) { cExampleSelected = 0; playFunctionSound(); page = CLABEXAMPLES; redrawNeeded = true; return; }
  bool ctrlL = k.ctrl && (wordContains(k.word, 'l') || wordContains(k.word, 'L') || M5Cardputer.Keyboard.isKeyPressed('l') || M5Cardputer.Keyboard.isKeyPressed('L'));
  if (ctrlL) { if (page == NOTES) { for (int i = 0; i < 15; ++i) notes[i] = ""; noteLine = 0; noteCount = 1; markStateDirty(); playFunctionSound(); redrawNeeded = true; return; } if (page == CLAB) { for (int i = 0; i < C_MAX_LINES; ++i) cLines[i] = ""; cLineCount = 1; cCursorLine = cCursorColumn = cScrollLine = cHorizontalScroll = 0; cOutputCount = 0; markStateDirty(); playFunctionSound(); redrawNeeded = true; return; } }
  if (page == CLAB && k.ctrl) { if (k.enter) { cInputValueCount = 0; cLabQrActive = false; cLabQrPayload = ""; playEnterSound(); runCLab(); } return; }
  if (page == CARDCREPL) {
    // qrcode() temporarily uses the shared QR view; only Fn closes it.
    if (cLabQrActive) return;
    if (k.enter) { playEnterSound(); runCardCRepl(); return; }
    if (k.del) { if (!replInput.isEmpty()) { replInput.remove(replInput.length() - 1); playBackspaceSound(); drawCardCRepl(); } return; }
    bool changed = false; for (char c : k.word) if (c >= 32 && c <= 126 && replInput.length() < C_MAX_LINE_CHARS) { replInput += c; changed = true; }
    if (changed) { playTypingSound(); drawCardCRepl(); }
    return;
  }
  if (page == BLEKEYBOARD) {
    // The Settings switch disables HID forwarding while leaving the shared BLE
    // controller stable for Wi-Fi/BLE coexistence; it can be re-enabled without reboot.
    if (!bleHidEnabled) { redrawNeeded = true; return; }
    // Do not forward input unless the host completed BLE pairing; this avoids
    // buffering keystrokes that might unexpectedly appear after reconnect.
    if (bleKeyboard.isPaired()) {
      // Always begin and end with an empty HID report. This prevents an old
      // key usage from remaining held after a delayed BLE connection event.
      bleKeyboard.releaseAll();
      // Special HID usages are true key taps, not printable characters.
      if (k.enter) {
        bleKeyboard.tap(KEY_RETURN);
        bleKeyboard.releaseAll();
        playEnterSound();
      } else if (k.del) {
        bleKeyboard.tap(KEY_BACKSPACE);
        bleKeyboard.releaseAll();
        playBackspaceSound();
      } else {
        // M5Cardputer's word list can contain more than one buffered character
        // after the keyboard has been idle. Forward only its newest printable
        // character for this physical key-change event, never the whole list.
        char newest = 0;
        for (char c : k.word) if (c >= 32 && c <= 126) newest = c;
        if (newest) {
          bleKeyboard.write(newest);
          bleKeyboard.releaseAll();
          playTypingSound();
        }
      }
    }
    redrawNeeded = true;
    return;
  }
  if (page == LAUNCHER) {
    if (k.enter) {
      playEnterSound();
      if (launcherHome) { if (homeSelected == 5) { launcherHome = false; appSelected = appScroll = 0; } else if (homeAppIndices[homeSelected] >= 0) page = static_cast<Page>(homeAppIndices[homeSelected] + 1); }
      else { if (appSelected == 0) { launcherHome = true; homeSelected = 5; } else page = static_cast<Page>(secondaryAppIndices[appSelected - 1] + 1); }
      redrawNeeded = true; return;
    }
    if (launcherHome) {
      int oldHome = homeSelected;
      // M5Cardputer can report more than one character in one change event.
      // Accept one direction by priority, so a physical press can never move
      // diagonally or twice because of a buffered key repeat.
      // Keep this physical punctuation cluster identical to C LAB and Settings.
      // ; = up, , = left, / = right, . = down.
      bool up = wordContains(k.word, ';');
      bool left = wordContains(k.word, ',');
      bool right = wordContains(k.word, '/');
      bool down = wordContains(k.word, '.');
      if (up && homeSelected >= 2) homeSelected -= 2;
      else if (left && homeSelected % 2) homeSelected--;
      else if (right && homeSelected % 2 == 0) homeSelected++;
      else if (down && homeSelected < 4) homeSelected += 2;
      if (homeSelected != oldHome) { playMenuSound(); startHomeFocusAnimation(oldHome, homeSelected); }
    } else {
      for (char c : k.word) {
        if (c == ',' || c == ';') { int oldSelected = appSelected, oldScroll = appScroll; appSelected = (appSelected + SECONDARY_APP_COUNT - 1) % SECONDARY_APP_COUNT; syncLauncherScroll(); playMenuSound(); updateLauncherSelection(oldSelected, oldScroll); }
        else if (c == '/' || c == '.') { int oldSelected = appSelected, oldScroll = appScroll; appSelected = (appSelected + 1) % SECONDARY_APP_COUNT; syncLauncherScroll(); playMenuSound(); updateLauncherSelection(oldSelected, oldScroll); }
      }
    }
    return;
  }
  if (page == SYSTEM) { if (k.enter) { playMenuSound(); updateSystemValues(); } return; }
  if (page == DASHBOARD) { if (k.enter) { playEnterSound(); startScan(); redrawNeeded = true; } return; }
  if (page == DICERANDOM) { if (k.enter) { rollDiceRandom(); playEnterSound(); redrawNeeded = true; } return; }
  if (page == GAMEHUB) {
    if (gameMode == 0) {
      for (char c : k.word) { if (c == ';') { gameMenuSelected = (gameMenuSelected + 2) % 3; redrawNeeded = true; } else if (c == '.') { gameMenuSelected = (gameMenuSelected + 1) % 3; redrawNeeded = true; } }
      if (k.enter) {
        if (gameMenuSelected == 0) { gameMode = 1; startSnakeGame(); }
        else if (gameMenuSelected == 1) { gameMode = 2; huntCursor = random(0, 9); huntTarget = random(0, 9); if (huntTarget == huntCursor) huntTarget = (huntTarget + 1) % 9; huntScore = 0; }
        else { gameMode = 3; kartRaceState = KART_HOME; }
        playEnterSound(); redrawNeeded = true;
      }
      return;
    }
    if (gameMode == 3) {
      if (kartRaceState == KART_RESULTS) {
        if (k.enter) { playEnterSound(); startKartRace(kartCurrentLevel); return; }
        for (char c : k.word) if (c == 'h') { kartRaceState = KART_HOME; redrawNeeded = true; }
        return;
      }
      // KART_HOME (level select). COUNTDOWN/RACING input is polled directly
      // in stepKartRace() every frame, not through this per-event handler.
      // keyboard() runs every loop() iteration regardless of whether a key
      // was actually pressed, so redrawNeeded must only be set when
      // something in k.word actually changed state here - setting it
      // unconditionally was forcing a full-panel redraw every single frame
      // even while sitting idle on this screen.
      for (char c : k.word) {
        if (c == ';') { kartSelectedLevel = (kartSelectedLevel + KartTrack::NUM_LEVELS - 1) % KartTrack::NUM_LEVELS; redrawNeeded = true; }
        else if (c == '.') { kartSelectedLevel = (kartSelectedLevel + 1) % KartTrack::NUM_LEVELS; redrawNeeded = true; }
        else if (c == 'm') { kartMusicEnabled = !kartMusicEnabled; kartMusicSetEnabled(kartMusicEnabled, kartFxEnabled); redrawNeeded = true; }
        else if (c == 'f') { kartFxEnabled = !kartFxEnabled; kartMusicSetEnabled(kartMusicEnabled, kartFxEnabled); redrawNeeded = true; }
      }
      if (k.enter) { playEnterSound(); startKartRace(kartSelectedLevel); return; }
      return;
    }
    if (gameMode == 1) {
      if (!snakeRunning && k.enter) { startSnakeGame(); playEnterSound(); redrawNeeded = true; return; }
      for (char c : k.word) { if (c == ';' && snakeDy == 0) { snakeDx = 0; snakeDy = -1; } else if (c == '.' && snakeDy == 0) { snakeDx = 0; snakeDy = 1; } else if (c == ',' && snakeDx == 0) { snakeDx = -1; snakeDy = 0; } else if (c == '/' && snakeDx == 0) { snakeDx = 1; snakeDy = 0; } }
      return;
    }
    for (char c : k.word) { if (c == ';' && huntCursor >= 3) huntCursor -= 3; else if (c == '.' && huntCursor < 6) huntCursor += 3; else if (c == ',' && huntCursor % 3) huntCursor--; else if (c == '/' && huntCursor % 3 < 2) huntCursor++; }
    if (k.enter) { if (huntCursor == huntTarget) { huntScore++; playEnterSound(); huntTarget = random(0, 9); if (huntTarget == huntCursor) huntTarget = (huntTarget + 1) % 9; } else playExitSound(); redrawNeeded = true; }
    if (!k.word.empty()) redrawNeeded = true;
    return;
  }
  if (page == DEVICECHECK) { for (char c : k.word) { if (c == ';') { deviceCheckSelected = (deviceCheckSelected + 3) % 4; redrawNeeded = true; } else if (c == '.') { deviceCheckSelected = (deviceCheckSelected + 1) % 4; redrawNeeded = true; } } if (k.enter) { playEnterSound(); if (deviceCheckSelected == 0) { tft.fillScreen(ILI9341_RED); delay(180); tft.fillScreen(ILI9341_GREEN); delay(180); tft.fillScreen(ILI9341_BLUE); delay(180); deviceCheckStatus = "Screen colour test shown"; } else if (deviceCheckSelected == 1) { if (volumeLevel) { M5Cardputer.Speaker.tone(1000, 180); deviceCheckStatus = "Speaker tone played"; } else deviceCheckStatus = "Volume is 0%; speaker muted"; } else if (deviceCheckSelected == 2) deviceCheckStatus = "Press any keyboard key to verify input"; else { sendNec(0, 16); deviceCheckStatus = "NEC test frame sent on IR"; } redrawNeeded = true; } return; }
  if (page == QRTOOLSPLUS) { for (char c : k.word) { if (c == ';') { qrPlusSelected = (qrPlusSelected + 3) % 4; redrawNeeded = true; } else if (c == '.') { qrPlusSelected = (qrPlusSelected + 1) % 4; redrawNeeded = true; } } if (k.enter) { if (qrPlusSelected == 0) qrText = String("http://") + WEB_MDNS_HOST + ".local"; else if (qrPlusSelected == 1) qrText = WiFi.status() == WL_CONNECTED ? WiFi.SSID() : "Wi-Fi not connected"; else if (qrPlusSelected == 2) qrText = notes[0]; page = QRTEXT; markStateDirty(); redrawNeeded = true; } return; }
  if (page == MINIPAINT) { for (char c : k.word) { if (c == ';' && paintY > 0) paintY--; else if (c == '.' && paintY < 11) paintY++; else if (c == ',' && paintX > 0) paintX--; else if (c == '/' && paintX < 15) paintX++; } if (k.enter) paintPixels[paintY][paintX] = !paintPixels[paintY][paintX]; if (k.del) for (int y = 0; y < 12; ++y) for (int x = 0; x < 16; ++x) paintPixels[y][x] = false; redrawNeeded = true; return; }
  if (page == LAUNCHERSEARCH) { int results = searchResultCount(); for (char c : k.word) { if (c == ';' && results) launcherSearchSelected = (launcherSearchSelected + results - 1) % results; else if (c == '.' && results) launcherSearchSelected = (launcherSearchSelected + 1) % results; else if (c >= 32 && c <= 126 && launcherSearchText.length() < 22) { launcherSearchText += c; launcherSearchSelected = 0; } } if (k.del && !launcherSearchText.isEmpty()) { launcherSearchText.remove(launcherSearchText.length() - 1); launcherSearchSelected = 0; } if (k.enter && results) { int app = searchResultApp(launcherSearchSelected); if (app >= 0) page = static_cast<Page>(app + 1); } redrawNeeded = true; return; }
  if (page == TEXTBROWSER) { if (k.enter) { playEnterSound(); fetchTextBrowserPage(); redrawNeeded = true; return; } if (k.del && !browserUrl.isEmpty()) { browserUrl.remove(browserUrl.length() - 1); redrawNeeded = true; return; } for (char c : k.word) { if (c == ';' && browserScroll > 0) browserScroll--; else if (c == '.' && (browserScroll + 1) * 48 < (int)browserText.length()) browserScroll++; else if (c >= 32 && c <= 126 && browserUrl.length() < 100) browserUrl += c; } redrawNeeded = true; return; }
  if (page == ZABKATOTP) {
    if (zabkaUnlocking) {
      if (k.enter) {
        bool unlocked = unlockZabkaVault(zabkaUnlockBuffer);
        zabkaUnlockBuffer = "";
        zabkaUnlocking = false;
        zabkaStatus = unlocked ? "Vault unlocked locally; press ENTER to sync time." : "Unlock failed: wrong password or corrupt record.";
        playEnterSound(); redrawNeeded = true; return;
      }
      if (k.del) { if (!zabkaUnlockBuffer.isEmpty()) { zabkaUnlockBuffer.remove(zabkaUnlockBuffer.length() - 1); playBackspaceSound(); } redrawNeeded = true; return; }
      bool changed = false;
      for (char c : k.word) if (c >= 32 && c <= 126 && zabkaUnlockBuffer.length() < 96) { zabkaUnlockBuffer += c; changed = true; }
      if (changed) { playTypingSound(); redrawNeeded = true; }
      return;
    }
    if (zabkaSecret.isEmpty() && zabkaVaultStored) {
      if (k.enter) { zabkaUnlocking = true; zabkaUnlockBuffer = ""; playEnterSound(); redrawNeeded = true; }
      return;
    }
    if (k.tab && zabkaVaultStored) { lockZabkaVault(); playExitSound(); redrawNeeded = true; return; }
    if (k.enter) {
      zabkaStatus = "Synchronizing network time...";
      redrawNeeded = true;
      drawZabkaTotp();
      syncZabkaNetworkTime();
      playEnterSound(); redrawNeeded = true; return;
    }
    if (k.del) { if (!zabkaSecret.isEmpty()) { zabkaSecret.remove(zabkaSecret.length() - 1); playBackspaceSound(); } redrawNeeded = true; return; }
    bool changed = false;
    for (char c : k.word) {
      char upper = toupper((unsigned char)c);
      if (isxdigit((unsigned char)c) && zabkaSecret.length() < 62) { zabkaSecret += char(toupper((unsigned char)c)); changed = true; }
    }
    if (changed) { zabkaStatus = "Secret is held only until restart."; playTypingSound(); redrawNeeded = true; }
    return;
  }
  if (page == INPOSTTRACK) {
    bool ctrlQ = k.ctrl && (wordContains(k.word, 'q') || wordContains(k.word, 'Q') || M5Cardputer.Keyboard.isKeyPressed('q') || M5Cardputer.Keyboard.isKeyPressed('Q'));
    if (k.tab) { inpostEditingPickupCode = !inpostEditingPickupCode; playTabSound(); redrawNeeded = true; return; }
    if (ctrlQ) {
      if (inpostNumber.length() < 5) inpostStatus = "Enter a parcel number before creating a tracking QR.";
      else {
        qrText = "https://inpost.pl/sledzenie-przesylek?number=" + inpostNumber;
        page = QRTEXT;
        markStateDirty();
        playFunctionSound();
      }
      redrawNeeded = true; return;
    }
    if (k.enter) { playEnterSound(); fetchInPostTracking(); redrawNeeded = true; return; }
    String& activeField = inpostEditingPickupCode ? inpostPickupCode : inpostNumber;
    if (k.del && !activeField.isEmpty()) { activeField.remove(activeField.length() - 1); playBackspaceSound(); redrawNeeded = true; return; }
    for (char c : k.word) {
      if (c == ';' && inpostScroll > 0) inpostScroll--;
      else if (c == '.' && (inpostScroll + 1) * 48 < (int)inpostStatus.length()) inpostScroll++;
      else if (c >= 32 && c <= 126 && activeField.length() < 48) { activeField += c; playTypingSound(); }
    }
    redrawNeeded = true; return;
  }
  if (page == MUSICLAB) {
    uint8_t oldTrack = drumTrack, oldCursor = drumCursor;
    for (char c : k.word) {
      if (c == ';' && drumTrack > 0) drumTrack--;
      else if (c == '.' && drumTrack + 1 < DRUM_TRACKS) drumTrack++;
      else if (c == ',' && drumCursor > 0) drumCursor--;
      else if (c == '/' && drumCursor + 1 < DRUM_STEPS) drumCursor++;
      else if (c == '[') drumTempo = max(60, int(drumTempo) - 5);
      else if (c == ']') drumTempo = min(240, int(drumTempo) + 5);
      else if (c == ' ') {
        drumPlaying = !drumPlaying;
        drumNextStepAt = millis();
        if (!drumPlaying) M5Cardputer.Speaker.stop();
        musicLastNote = drumPlaying ? "Sequencer started" : "Sequencer stopped";
      }
    }
    if (k.enter) { drumPattern[drumTrack][drumCursor] = !drumPattern[drumTrack][drumCursor]; triggerDrum(drumTrack); }
    if (k.del) { for (uint8_t step = 0; step < DRUM_STEPS; ++step) drumPattern[drumTrack][step] = false; }
    if (oldTrack != drumTrack || oldCursor != drumCursor || k.enter || k.del || !k.word.empty()) { playMenuSound(); redrawNeeded = true; }
    return;
  }
  if (page == WIFI) { if (k.enter) { playMenuSound(); startScan(); } return; }
  if (page == WIFISETUP) {
    if (wifiSetupEditingPassword) {
      if (k.tab) { wifiSetupEditingPassword = false; playTabSound(); redrawNeeded = true; return; }
      if (k.del) { if (!wifiSetupPassword.isEmpty()) { wifiSetupPassword.remove(wifiSetupPassword.length() - 1); playBackspaceSound(); redrawNeeded = true; } return; }
      if (k.enter) {
        if (wifiSetupSsid.isEmpty()) return;
        preferences.begin("cyberdeck", false); preferences.putString("wifi_ssid", wifiSetupSsid); preferences.putString("wifi_pass", wifiSetupPassword); preferences.end();
        // Start association in the background, then return immediately to
        // Home. Web Companion remains a separate explicit app action.
        autoWifiNextAttemptAt = 0;
        autoConnectWifi();
        wifiSetupEditingPassword = false;
        playEnterSound();
        page = LAUNCHER;
        launcherHome = true;
        redrawNeeded = true;
        return;
      }
      bool changed = false; for (char c : k.word) if (c >= 32 && c <= 126 && wifiSetupPassword.length() < 63) { wifiSetupPassword += c; changed = true; }
      if (changed) { playTypingSound(); redrawNeeded = true; } return;
    }
    if (k.tab) { playMenuSound(); startScan(); return; }
    if (k.enter) {
      if (!scanDone) { startScan(); return; }
      if (networkCount > 0) { wifiSetupSsid = WiFi.SSID(wifiSetupSelected); wifiSetupPassword = ""; wifiSetupEditingPassword = true; playEnterSound(); redrawNeeded = true; }
      return;
    }
    for (char c : k.word) { if (c == ';' && networkCount) { wifiSetupSelected = (wifiSetupSelected + networkCount - 1) % networkCount; playMenuSound(); redrawNeeded = true; } else if (c == '.' && networkCount) { wifiSetupSelected = (wifiSetupSelected + 1) % networkCount; playMenuSound(); redrawNeeded = true; } }
    return;
  }
  if (page == CLOCK) return;
  if (page == QRTEXT) { if (k.enter) { playEnterSound(); markStateDirty(); drawQRModules(); return; } if (k.del) { if (!qrText.isEmpty()) { qrText.remove(qrText.length() - 1); markStateDirty(); playBackspaceSound(); drawQRTextField(); } return; } for (char c : k.word) if (c >= 32 && c <= 126 && qrText.length() < QR_MAX_TEXT) { qrText += c; markStateDirty(); playTypingSound(); drawQRTextField(); } return; }
  if (page == TEXTTOOLS) {
    if (k.enter) { if (noteCount < 15) { String result = toolText; if (toolMode == 1) result.toUpperCase(); else if (toolMode == 2) result.toLowerCase(); else if (toolMode == 3) result = base64Encode(toolText); notes[noteCount++] = result.substring(0, 50); noteLine = noteCount - 1; markStateDirty(); playEnterSound(); } return; }
    if (k.del) { if (!toolText.isEmpty()) { toolText.remove(toolText.length() - 1); playBackspaceSound(); redrawNeeded = true; } return; }
    for (char c : k.word) { if (c == ';') { toolMode = (toolMode + 3) % 4; redrawNeeded = true; } else if (c == '.') { toolMode = (toolMode + 1) % 4; redrawNeeded = true; } else if (c >= 32 && c <= 126 && toolText.length() < 80) { toolText += c; playTypingSound(); redrawNeeded = true; } } return;
  }
  if (page == FAVOURITES) {
    int favCount = 0; for (int i = 0; i < APP_COUNT; ++i) if (i != 10 && appFavourite[i]) favCount++;
    for (char c : k.word) { if (favCount && c == ';') { favouriteSelected = (favouriteSelected + favCount - 1) % favCount; redrawNeeded = true; } else if (favCount && c == '.') { favouriteSelected = (favouriteSelected + 1) % favCount; redrawNeeded = true; } else if (favCount && (c == '\r' || c == '\n')) {} }
    if (k.enter && favCount) { int n = 0; for (int i = 0; i < APP_COUNT; ++i) if (i != 10 && appFavourite[i] && n++ == favouriteSelected) { page = (Page)(i + 1); redrawNeeded = true; break; } }
    if (k.del && favCount) { int n = 0; for (int i = 0; i < APP_COUNT; ++i) if (i != 10 && appFavourite[i] && n++ == favouriteSelected) { appFavourite[i] = false; favouriteSelected = 0; markStateDirty(); redrawNeeded = true; break; } } return;
  }
  if (page == WIFIMONITOR) { if (k.enter) { playMenuSound(); startScan(); } return; }
  if (page == FILEBROWSER) { if (k.enter) { String content = localFiles[fileSelected]; noteCount = 1; noteLine = 0; int start = 0; for (int i = 0; i < 15; ++i) { int end = content.indexOf('\n', start); notes[i] = end < 0 ? content.substring(start, start + 50) : content.substring(start, end).substring(0, 50); noteCount = i + 1; if (end < 0) break; start = end + 1; } markStateDirty(); page = NOTES; redrawNeeded = true; return; } if (k.del) { localFiles[fileSelected] = ""; markStateDirty(); redrawNeeded = true; return; } for (char c : k.word) if (c == ';') { fileSelected = (fileSelected + 2) % 3; redrawNeeded = true; } else if (c == '.') { fileSelected = (fileSelected + 1) % 3; redrawNeeded = true; } return; }
  if (page == WEBCOMPANION) { if (k.enter) startWebCompanion(); return; }
  if (page == CLABEXAMPLES) {
    // Examples is entered from C LAB FILES, so use its robust key path too.
    bool up = wordContains(k.word, ';') || M5Cardputer.Keyboard.isKeyPressed(';');
    bool down = wordContains(k.word, '.') || M5Cardputer.Keyboard.isKeyPressed('.');
    if (up) { cExampleSelected = (cExampleSelected + C_EXAMPLE_COUNT - 1) % C_EXAMPLE_COUNT; playMenuSound(); redrawNeeded = true; }
    else if (down) { cExampleSelected = (cExampleSelected + 1) % C_EXAMPLE_COUNT; playMenuSound(); redrawNeeded = true; }
    if (k.enter) loadCLabExample(cExampleSelected);
    return;
  }
  if (page == HOMEEDITOR) {
    if (homeEditorPicking) {
      for (char c : k.word) {
        if (c == ';') { homeEditorApp = (homeEditorApp + APP_COUNT - 1) % APP_COUNT; playMenuSound(); redrawNeeded = true; }
        else if (c == '.') { homeEditorApp = (homeEditorApp + 1) % APP_COUNT; playMenuSound(); redrawNeeded = true; }
      }
      if (k.enter && homeEditorApp != 14) { homeAppIndices[homeEditorSlot] = homeEditorApp; homeEditorPicking = false; markStateDirty(); playEnterSound(); redrawNeeded = true; }
      return;
    }
    for (char c : k.word) {
      if (c == ';') { homeEditorSlot = (homeEditorSlot + 4) % 5; playMenuSound(); redrawNeeded = true; }
      else if (c == '.') { homeEditorSlot = (homeEditorSlot + 1) % 5; playMenuSound(); redrawNeeded = true; }
      else if (c == ',' && homeEditorSlot > 0) { int swap = homeAppIndices[homeEditorSlot - 1]; homeAppIndices[homeEditorSlot - 1] = homeAppIndices[homeEditorSlot]; homeAppIndices[homeEditorSlot] = swap; homeEditorSlot--; markStateDirty(); playMenuSound(); redrawNeeded = true; }
      else if (c == '/' && homeEditorSlot < 4) { int swap = homeAppIndices[homeEditorSlot + 1]; homeAppIndices[homeEditorSlot + 1] = homeAppIndices[homeEditorSlot]; homeAppIndices[homeEditorSlot] = swap; homeEditorSlot++; markStateDirty(); playMenuSound(); redrawNeeded = true; }
    }
    if (k.enter) { homeEditorApp = homeAppIndices[homeEditorSlot] >= 0 ? homeAppIndices[homeEditorSlot] : 0; homeEditorPicking = true; playEnterSound(); redrawNeeded = true; }
    else if (k.del && homeAppIndices[homeEditorSlot] >= 0) { homeAppIndices[homeEditorSlot] = -1; markStateDirty(); playBackspaceSound(); redrawNeeded = true; }
    return;
  }
  if (page == CLAB) {
    if (cNavigationMode) return;
    if (k.enter) { playEnterSound(); if (cLineCount < C_MAX_LINES) { String tail = cLines[cCursorLine].substring(cCursorColumn); cLines[cCursorLine].remove(cCursorColumn); for (int i = cLineCount; i > cCursorLine + 1; i--) cLines[i] = cLines[i - 1]; cLines[cCursorLine + 1] = tail; cLineCount++; cCursorLine++; cCursorColumn = 0; cHorizontalScroll = 0; cLabDirty = true; markStateDirty(); if (cCursorLine >= cScrollLine + C_CODE_VISIBLE_LINES) cScrollLine++; drawCLabEditor(); } return; }
    if (k.del) return;
    bool codeChanged = false; for (char c : k.word) if (c >= 32 && c <= 126 && (int)cLines[cCursorLine].length() < C_MAX_LINE_CHARS) { cLines[cCursorLine] = cLines[cCursorLine].substring(0, cCursorColumn) + c + cLines[cCursorLine].substring(cCursorColumn); cCursorColumn++; codeChanged = true; }
      if (codeChanged) { cLabDirty = true; int maxHorizontalScroll = max(0, (int)cLines[cCursorLine].length() - C_CODE_COLUMNS_VISIBLE + 1); if (cCursorColumn >= cHorizontalScroll + C_CODE_COLUMNS_VISIBLE) cHorizontalScroll = cCursorColumn - C_CODE_COLUMNS_VISIBLE + 1; cHorizontalScroll = constrain(cHorizontalScroll, 0, maxHorizontalScroll); playTypingSound(); drawCLabEditor(); markStateDirty(); } return;
  }
  if (page == NOTES) { if (k.enter) { if (noteLine < 14) { if (noteLine == noteCount - 1) noteCount++; noteLine++; markStateDirty(); redrawNeeded = true; } return; } if (k.del) { if (!notes[noteLine].isEmpty()) { notes[noteLine].remove(notes[noteLine].length() - 1); playTypingSound(); } else if (noteLine > 0) { noteLine--; noteCount--; } markStateDirty(); updateNotesLine(); return; } for (char c : k.word) if (c >= 32 && c <= 126 && notes[noteLine].length() < 50) { notes[noteLine] += c; playTypingSound(); markStateDirty(); updateNotesLine(); } return; }
  if (page == CALC) { if (k.enter) { playMenuSound(); calcResult(); return; } if (k.del) { calcInput = "0"; calcTotal = 0; calcOp = 0; calcNew = true; calcStatus = "Cleared"; updateCalcPanel(); return; } for (char c : k.word) { if ((c >= '0' && c <= '9') || c == '.') { if (calcNew) { calcInput = ""; calcNew = false; } if (calcInput.length() < 16 && !(c == '.' && calcInput.indexOf('.') >= 0)) { calcInput += c; playTypingSound(); updateCalcPanel(); } } else if (c == '+' || c == '-' || c == '*' || c == '/') { if (calcOp != 0 && !calcNew) calcResult(); calcTotal = calcInput.toFloat(); calcOp = c; calcNew = true; calcStatus = String("Pending operator: ") + c; playMenuSound(); updateCalcPanel(); } } return; }
  if (page == SETTINGS) {
    if (pinChangeActive) {
      if (k.del) { if (!pinChangeInput.isEmpty()) { pinChangeInput.remove(pinChangeInput.length() - 1); playBackspaceSound(); redrawNeeded = true; } return; }
      if (k.enter) {
        if (pinChangeInput.length() != 4) { pinChangeStatus = "PIN must contain exactly 4 digits"; playExitSound(); redrawNeeded = true; return; }
        if (!pinChangeConfirm) { pinChangeFirst = pinChangeInput; pinChangeInput = ""; pinChangeConfirm = true; pinChangeStatus = "Repeat the same PIN to confirm"; playEnterSound(); redrawNeeded = true; return; }
        if (pinChangeInput == pinChangeFirst) { lockPinHash = lockPinHashFor(pinChangeInput); pinChangeInput = ""; pinChangeFirst = ""; pinChangeConfirm = false; pinChangeActive = false; pinChangeStatus = "PIN saved"; markStateDirty(); playEnterSound(); }
        else { pinChangeInput = ""; pinChangeFirst = ""; pinChangeConfirm = false; pinChangeStatus = "PINs differ; choose a new PIN"; playExitSound(); }
        redrawNeeded = true; return;
      }
      bool changed = false; for (char digit : k.word) if (digit >= '0' && digit <= '9' && pinChangeInput.length() < 4) { pinChangeInput += digit; changed = true; }
      if (changed) { playTypingSound(); redrawNeeded = true; }
      return;
    }
    if (k.enter && settingSelected == 7) { pinChangeActive = true; pinChangeConfirm = false; pinChangeFirst = ""; pinChangeInput = ""; pinChangeStatus = "Choose a new 4-digit PIN"; playEnterSound(); redrawNeeded = true; return; }
    if (k.enter && settingSelected == 8) { wifiSetupEditingPassword = false; wifiSetupSelected = 0; playEnterSound(); page = WIFISETUP; if (!scanDone && !scanRunning) startScan(); redrawNeeded = true; return; }
    if (k.enter && settingSelected == 9) { bleHidEnabled = !bleHidEnabled; playEnterSound(); markStateDirty(); redrawNeeded = true; return; }
    if (k.enter && settingSelected == 10) { statusLedEnabled = !statusLedEnabled; updateStatusLed(); playEnterSound(); markStateDirty(); redrawNeeded = true; return; }
    for (char c : k.word) { if (c == ';') { int old = settingSelected; settingSelected = (settingSelected + SETTINGS_COUNT - 1) % SETTINGS_COUNT; playMenuSound(); updateSettingsRow(old); updateSettingsRow(settingSelected); } else if (c == '.') { int old = settingSelected; settingSelected = (settingSelected + 1) % SETTINGS_COUNT; playMenuSound(); updateSettingsRow(old); updateSettingsRow(settingSelected); } else if (c == ',' && settingSelected < 7) { playMenuSound(); changeSetting(-1); } else if (c == '/' && settingSelected < 7) { playMenuSound(); changeSetting(1); } }
  }
}
// Runs once: bring up the board, restore saved settings from flash, then
// show the boot log / splash before handing off to the normal loop().
void setup() {
  auto cfg = M5.config(); M5Cardputer.begin(cfg, true); loadPersistentState(); applyVolume();
  pinMode(BTNA_PIN, INPUT_PULLUP);
  pinMode(HAPTIC_IN_PIN, OUTPUT);
  digitalWrite(HAPTIC_IN_PIN, LOW); // Keep the motor module off by default.
  // The ADV LED is electrically gated; enable its supply before sending the
  // WS2812-style data signal on GPIO21.
  pinMode(RGB_LED_POWER_PIN, OUTPUT);
  digitalWrite(RGB_LED_POWER_PIN, HIGH);
  playStartupRainbow();
  // PWM is required here: digitalWrite() could only turn the external TFT
  // backlight fully on or fully off, which is why Brightness changed only the
  // lower built-in Cardputer panel.
  ledcAttach(TFT_BL, TFT_BL_PWM_HZ, TFT_BL_PWM_BITS);
  // GPIO44 is the board's reserved built-in IR emitter, not an external wire.
  ledcAttach(IR_TX_PIN, IR_CARRIER_HZ, 8);
  ledcWrite(IR_TX_PIN, 0);
  btnAWasDown = digitalRead(BTNA_PIN) == LOW;
  // A saved "Backlight OFF" setting must never hide the splash or the initial
  // PIN gate. Start with both panels lit; the explicit lock/sleep action is
  // still the only path that turns them off after boot.
  backlightOn = true;
  applyBacklight();
  // BLE HID uses NimBLE and is available to pair after the boot/lock sequence.
  bleKeyboard.begin();
  kartMusicInit(); // GAMEHUB / KART RACER speaker channel volumes
  // Rejoin the last Wi-Fi network in the background. BLE stays enabled and
  // modem sleep is left on, as required by ESP32-S3 radio coexistence.
  autoConnectWifi();
  // The ILI9341 is write-only in this project, so MISO is deliberately unused.
  // G4 stays unused; G15 is the display reset line.
  SPI.begin(TFT_SCK, -1, TFT_MOSI, TFT_CS); tft.begin(); tft.setRotation(displayRotation); tft.setTextWrap(false); applyTheme(); drawLinuxBoot(); drawBootScreen();
  // One clear boot confirmation through the ready-made motor driver.
  vibrate(700);
  // Always begin at the visible PIN gate rather than exposing the last app.
  page = LOCKSCREEN; sleeping = false; setBacklight(true); updateStatusLed(); lastActivity = millis(); draw();
}
// Runs forever. Roughly: poll input (keyboard()) -> let any continuously-
// animating app draw its own frame (Snake/Kart Racer/Music Lab, all of
// which bypass the redrawNeeded dispatch - see the rendering-model note at
// the top of the file) -> handle idle/sleep/screensaver timers -> finally,
// if anything asked for a repaint (redrawNeeded), do exactly one draw() or
// refreshLocalPage() for this tick.
void loop() {
  M5Cardputer.update();
  // BtnA is the dedicated manual BL switch; it does not lock or change apps.
  bool btnANowDown = digitalRead(BTNA_PIN) == LOW;
  if (btnANowDown && !btnAWasDown) { setBacklight(!backlightOn); lastActivity = millis(); markStateDirty(); }
  btnAWasDown = btnANowDown;
    checkScan();
  serviceLowBatteryAlert();
  serviceAutoConnectWifi();
  serviceConnectionToasts();
  serviceStatusSounds();
  if (webRunning) { webServer.handleClient(); applyWebInput(); }
  keyboard();
  if (page == MIC && !sleeping) updateMicMonitor();
  auto keys = M5Cardputer.Keyboard.keysState();
  bool clabDeleteNow = page == CLAB && !sleeping && M5Cardputer.Keyboard.isPressed() && keys.del;
  if (clabDeleteNow) {
    unsigned long now = millis();
    if (!clabDeleteHeld) {
      clabBackspace();
      clabDeleteHeld = true;
      clabDeleteRepeatAt = now + 500UL;
    } else if (now >= clabDeleteRepeatAt) {
      clabBackspace();
      clabDeleteRepeatAt = now + 90UL;
    }
  } else {
    clabDeleteHeld = false;
  }
  handleCLabCursorKeys();
  stepSnakeGame();
  stepKartRace();
  // One 16th-note per tick. The grid is refreshed only on the new playhead
  // position rather than continuously redrawing a full screen.
  if (page == MUSICLAB && drumPlaying && !sleeping && !quickMenuOpen && millis() >= drumNextStepAt) {
    uint8_t oldStep = drumPlayStep;
    drumPlayStep = (drumPlayStep + 1) % DRUM_STEPS;
    playDrumStep(drumPlayStep);
    drumNextStepAt = millis() + 60000UL / (uint32_t(drumTempo) * 4UL);
    for (uint8_t track = 0; track < DRUM_TRACKS; ++track) { drawMusicCell(track, oldStep); drawMusicCell(track, drumPlayStep); }
  }
  updateHomeFocusAnimation();
  updateScreensaver();
  // The optional second stage is a real PIN gate after the selected amount
  // of time on the static, dim screensaver. It also turns both panel
  // backlights off; the next key wakes the PIN screen but never bypasses it.
  uint16_t lockAfterSaver = lockAfterScreensaverValues[lockAfterScreensaverIndex];
  if (page == SCREENSAVER && lockAfterSaver && millis() - screensaverStartedAt >= (unsigned long)lockAfterSaver * 1000UL) {
    page = LOCKSCREEN;
    cardcLedOverride = false;
    screensaverDimmed = false;
    lockPinInput = "";
    lockScreenBaseDrawn = false;
    sleeping = true;
    setBacklight(false);
    lastActivity = millis();
    redrawNeeded = true;
  }
  uint16_t timeout = sleepValues[sleepIndex];
  if (timeout && !sleeping && page != LOCKSCREEN && page != SCREENSAVER && millis() - lastActivity >= (unsigned long)timeout * 1000UL) {
    // Inactivity shows a low-refresh screensaver; only BtnA turns BL off.
    startScreensaver();
  }
  static unsigned long lastClock = 0; if (page == CLOCK && !sleeping && millis() - lastClock >= 1000UL) { lastClock = millis(); updateClockValue(); }
  // The lock view has no seconds by design. Refresh it only when one of its
  // visible values changes, so the displayed HH:MM time advances by itself
  // without a key press or a needless full-screen redraw every second.
  static String lastLockSignature = "";
  if (page == LOCKSCREEN && !sleeping) {
    int lockBattery = M5Cardputer.Power.getBatteryLevel();
    String lockSignature = lockClockText() + "|" + String(lockBattery) + "|" + (lockRetryAt > millis() ? "wait" : "ready");
    if (lockSignature != lastLockSignature) {
      lastLockSignature = lockSignature;
      redrawNeeded = true;
    }
  } else {
    lastLockSignature = "";
  }
  // Keep the external-header status live without repainting the application.
  // A one-second poll updates its small right-hand region only when a visible
  // value differs: HH:MM, battery percentage, BT pairing/enabled state, or
  // Wi-Fi connection/signal-bar threshold.
  if (!sleeping && page != LOCKSCREEN && page != SCREENSAVER && millis() - lastHeaderStatusPollAt >= 1000UL) {
    lastHeaderStatusPollAt = millis();
    const bool wifiConnected = WiFi.status() == WL_CONNECTED;
    int wifiBars = 0;
    if (wifiConnected) {
      int rssi = WiFi.RSSI();
      wifiBars = rssi >= -55 ? 4 : (rssi >= -67 ? 3 : (rssi >= -78 ? 2 : 1));
    }
    const bool btPaired = bleHidEnabled && bleKeyboard.isPaired();
    const int battery = M5Cardputer.Power.getBatteryLevel();
    String statusSignature = lockClockText() + "|" + String(battery) + "|" +
      String(bleHidEnabled ? 1 : 0) + "|" + String(btPaired ? 1 : 0) + "|" +
      String(wifiConnected ? 1 : 0) + "|" + String(wifiBars);
    if (statusSignature != lastHeaderStatusSignature) {
      lastHeaderStatusSignature = statusSignature;
      // Draws directly to the header's corner outside the redrawNeeded
      // dispatch, so it needs its own quick-menu guard like the others above.
      if (!redrawNeeded && !quickMenuOpen) drawHeaderStatus();
    }
  }
  static unsigned long lastBuiltinCheck = 0;
  if (!sleeping && millis() - lastBuiltinCheck >= 250UL) { lastBuiltinCheck = millis(); updateBuiltinDisplay(); }
  static uint64_t lastTotpWindow = UINT64_MAX;
  if (page == ZABKATOTP && !sleeping) {
    time_t now = time(nullptr);
    uint64_t window = now > 1700000000 ? uint64_t(now) : 0;
    if (window != lastTotpWindow) { lastTotpWindow = window; redrawNeeded = true; }
  }
  // Nothing below may draw while the quick-launch overlay is open - every
  // page's draw()/refreshLocalPage() is oblivious to it and would just
  // paint straight over it. The overlay is repainted directly by keyboard()
  // instead (open/close: an animation; navigate: the two changed tiles
  // plus the caption naming the new selection).
  if (redrawNeeded && !sleeping && !quickMenuOpen) {
    // A page transition (or forceFullRedraw, e.g. the overlay just closed
    // and left an arbitrary area of the screen needing a full repaint) needs
    // the complete scene. Repeated interaction inside the same page uses the
    // bounded refresh above and preserves the rest.
    if (page != lastDrawnPage || forceFullRedraw) { draw(); forceFullRedraw = false; }
    else refreshLocalPage();
    // A page refresh may have covered the toast, so paint its current frame again.
    if (toastActive) toastNeedsPaint = true;
  }
  serviceToasts();
  if (stateDirty && millis() - stateChangedAt >= 2000UL) savePersistentState();
}