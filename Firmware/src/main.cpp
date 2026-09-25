#include <Arduino.h>
#include <string.h>
#include <stdio.h>
#include <algorithm>
#include "config.hpp"
#include <Adafruit_NeoPixel.h>
#include <FatFS.h>
#include <FatFSUSB.h>
#include <hardware/structs/qmi.h>
#include <hardware/gpio.h>
#include <hardware/structs/sio.h>
#include <hardware/structs/pads_bank0.h>

// --- Hardware ---------------------------------------------------------------
static constexpr uint8_t  LED_PIN   = 22;   // WS2812 data line (GPIO22)
static constexpr uint16_t LED_COUNT = 13;   // visible string: LED 1 through 13
static constexpr uint16_t LED_CHAIN = LED_COUNT + 1;  // +1 extra: test DOUT of LED 13
static constexpr uint8_t  ADC_PIN   = A0;   // ADC0 = GPIO26 -> VU meter (LED 6..13)
static constexpr uint8_t  ADC1_PIN  = A1;   // ADC1 = GPIO27 -> balance
static constexpr uint8_t  ADC2_PIN  = A2;   // ADC2 = GPIO28 -> balance

// --- Buttons -----------------------------------------------------------------
static constexpr uint8_t  POWER_HOLD_PIN       = 25;   // load-switch latch: HIGH = power on
static constexpr uint8_t  BUTTON_PIN           = 20;   // power button AND volume + (GPIO20)
static constexpr bool     BUTTON_ACTIVE_HIGH   = true;  // pressed = GPIO20 HIGH
static constexpr uint32_t POWER_OFF_HOLD_MS    = 2000;  // hold 2s -> power off
static constexpr uint8_t  VOL_DOWN_PIN         = 21;   // 2nd button = volume - (GPIO21)
static constexpr bool     VOL_DOWN_ACTIVE_HIGH = true;  // assumption; flip if needed
static constexpr uint32_t CENTER_HOLD_MS       = 2000;  // hold volume - 2s -> centre the balance

// --- Battery (1-cell Li-Po) --------------------------------------------------
static constexpr uint8_t  VBAT_ADC_PIN       = A3;   // ADC3 = GPIO29, Vbat via 10k+10k divider
static constexpr uint8_t  CHARGE_STAT_PIN    = 23;   // MCP73830 STAT: LOW = charging
static constexpr float    ADC_VREF           = 3.3f; // ADC reference (VREF pin)
static constexpr float    VBAT_DIVIDER       = 2.0f; // 10k+10k -> ADC3 reads Vbat/2
// Vbat calibration factor lives in calibration.txt as vbat_cal (default in config.hpp).
static constexpr float    VBAT_EMPTY         = 3.70f;// 0% on the meter
static constexpr float    VBAT_FULL          = 4.10f;// 100% on the meter
static constexpr float    VBAT_CUTOFF        = 3.65f;// < this for 2s (on battery) -> off
static constexpr uint32_t STARTUP_BATTERY_MS = 2500;  // show battery briefly after boot
static constexpr uint32_t VBAT_CUTOFF_MS     = 2000;  // 2s below cutoff -> shut down

// --- Audio amplifier ---------------------------------------------------------
static constexpr uint8_t  AMP_EN_PIN         = 1;    // HIGH = amp on; LOW while charging

// --- MCP4011 digital pot (increment/decrement interface) ---------------------
static constexpr uint8_t  POT_CS_PIN         = 16;   // ChipSelect (active-low)
static constexpr uint8_t  POT_UD_PIN         = 17;   // Up/Down (direction + step pulse)
static constexpr uint8_t  POT_TAPS           = 64;   // MCP4011: 64 wiper positions (0..63)

Adafruit_NeoPixel strip(LED_CHAIN, LED_PIN, NEO_GRB + NEO_KHZ800);

// --- VU meter ---------------------------------------------------------------
// Human numbering (1-based) -> colour:
//   LED 6         = red
//   LED 7, 8      = orange
//   LED 9 through 13 = green
// Strip index = LED number - 1, so the VU meter occupies indices 5 through 12.
//
// The bar fills from the green end (LED 13) toward red (LED 6):
// little signal = green only, full signal = through red (peak).
//
// vuOrder[] = strip indices in the order they light,
// from first (low level) to last (maximum level).
static const uint8_t vuOrder[8] = { 12, 11, 10, 9, 8, 7, 6, 5 };
//                                   13  12  11 10  9  8  7  6  (LED number)
static constexpr uint8_t VU_SEGMENTS = 8;

static uint32_t colorForLed(uint8_t stripIndex) {
  const uint8_t ledNumber = stripIndex + 1;   // back to 1-based
  if (ledNumber == 6)                    return strip.Color(255, 0, 0);   // red
  if (ledNumber == 7 || ledNumber == 8)  return strip.Color(255, 60, 0);  // orange
  return strip.Color(0, 255, 0);                                          // green (9..13)
}

// Set the whole chain (13 visible + 1 extra) to one colour. The extra pixel
// hangs off LED 13 DOUT: if it lights, the last NeoPixel is forwarding data.
static void setAllLeds(uint8_t r, uint8_t g, uint8_t b) {
  const uint32_t c = strip.Color(r, g, b);
  for (uint16_t i = 0; i < LED_CHAIN; i++) {
    strip.setPixelColor(i, c);
  }
  strip.show();
}

// LED 14 (index LED_COUNT) is the extra pixel on LED 13 DOUT. Light it
// bright green on every normal display so a dead last LED is obvious.
static void showStrip() {
  strip.setPixelColor(LED_COUNT, strip.Color(0, 255, 0));
  strip.show();
}

// --- Calibration -------------------------------------------------------------
// Values come from calibration.txt (on the USB disk); fallbacks in config.hpp.
// -----------------------------------------------------------------------------
struct Calibration {
  uint16_t adcMin    = CalDefaults::adcMin;
  uint16_t adcMax    = CalDefaults::adcMax;
  uint8_t  smoothing = CalDefaults::smoothing;
  float    vbatCal   = CalDefaults::vbatCal;
};
static Calibration cal;
static bool stQuiet = false;   // after ST: START: no unsolicited serial
enum class DiagMode : uint8_t { None, PinTest, Sweep, Gpio29, Adc };
static DiagMode diagMode = DiagMode::None;
enum class LoopMode : uint8_t { Diag, Quiet, ShowVolume, ShowBattery, Emi };

// --- Battery display ---------------------------------------------------------
// Colours exactly INVERTED vs EMI mode: the first segment (nearly empty)
// is red, the next two orange, the rest green. At 100% all 8 are on.
static uint32_t batteryColor(uint8_t pos) {   // pos 0..7 in fill order
  if (pos == 0) return strip.Color(255, 0, 0);    // 1x red  (nearly empty)
  if (pos <= 2) return strip.Color(255, 60, 0);   // 2x orange
  return strip.Color(0, 255, 0);                  // 5x green
}

// The Arduino core switches the ADC mux and converts immediately, so the first
// conversion after a channel change still holds charge from the previous
// channel's sample-and-hold. One throwaway read settles it on the new channel.
static uint16_t adcSettledRead(uint8_t pin) {
  analogRead(pin);
  return analogRead(pin);
}

// Read Vbat (V) via ADC3 + the 10k/10k divider.
// A3 is sampled at most ~2x/sec and with a single sample: fast/continuous
// sampling charge-injects the high-impedance divider node so Vbat reads a
// volt too high. Sampling rarely gives the node time to settle. Value is
// cached between measurements.
static float readVbat() {
  if constexpr (!Features::readVbatAdc) return 0.0f;
  static float cachedV = -1.0f;
  static uint32_t lastMs = 0;
  const uint32_t now = millis();
  if (cachedV < 0.0f || (now - lastMs) >= 500) {
    lastMs = now;
    const uint16_t raw = adcSettledRead(VBAT_ADC_PIN);  // low rate, mux settled
    cachedV = raw / 4095.0f * ADC_VREF * VBAT_DIVIDER * cal.vbatCal;
  }
  return cachedV;
}

static bool readCharging() {
  if constexpr (Features::ignoreCharging) return false;
  return digitalRead(CHARGE_STAT_PIN) == LOW;
}

// Mute the amp while charging (EN low); enable it on battery (EN high).
static void updateAmpEnable(bool charging) {
  digitalWrite(AMP_EN_PIN, charging ? LOW : HIGH);
}

// Number of lit segments (1..8) for a given Vbat.
static uint8_t batteryLevel(float vbat) {
  float frac = (vbat - VBAT_EMPTY) / (VBAT_FULL - VBAT_EMPTY);
  if (frac < 0.0f) frac = 0.0f;
  if (frac > 1.0f) frac = 1.0f;
  return 1 + (uint8_t)lroundf(frac * (VU_SEGMENTS - 1));  // 1..8
}

// Static battery level on the VU meter (LED 6..13); LED 1..5 stay off.
static void showBattery(float vbat) {
  const uint8_t level = batteryLevel(vbat);
  strip.clear();
  for (uint8_t i = 0; i < level; i++) {
    strip.setPixelColor(vuOrder[i], batteryColor(i));
  }
  showStrip();
}

// Charge animation: solid bar up to current charge + a segment that "walks"
// upward toward full. At 100% everything is solid (full).
static void showBatteryCharging(float vbat) {
  const uint8_t level = batteryLevel(vbat);
  strip.clear();
  // Solid part = current charge
  for (uint8_t i = 0; i < level; i++) {
    strip.setPixelColor(vuOrder[i], batteryColor(i));
  }
  // Walking segment upward (only if not yet full)
  if (level < VU_SEGMENTS) {
    const uint8_t span = VU_SEGMENTS - level;          // free segments above charge
    const uint8_t step = (millis() / 150) % span;      // 0..span-1, walks up
    strip.setPixelColor(vuOrder[level + step], strip.Color(80, 80, 80));  // dim white
  }
  showStrip();
}


// Show volume (0..63) on the VU meter: 8 LEDs x 8 brightness levels = 64.
// The lower 'full' LEDs are fully on (blue); the next LED uses one of 8
// brightnesses. Every one of the 64 volume steps is uniquely visible.
// ----------------------------------------------------------------------------
// drawVolume only sets the VU LEDs, so the EMI loop can keep the balance LED.
static void drawVolume(uint8_t vol) {
  vol = (uint16_t)vol * 63 / Settings::volumeMax;   // scale: volumeMax = all 8 LEDs full
  if (vol > POT_TAPS - 1) vol = POT_TAPS - 1;
  const uint8_t full    = vol / 8;          // 0..7 fully-on LEDs
  const uint8_t partial = (vol % 8) + 1;    // 1..8 brightness of the next LED
  for (uint8_t i = 0; i < full; i++) {
    strip.setPixelColor(vuOrder[i], strip.Color(0, 0, 255));           // full = blue
  }
  if (full < VU_SEGMENTS) {
    strip.setPixelColor(vuOrder[full], strip.Color(0, 0, 255 * partial / 8));
  }
}

static void showVolume(uint8_t vol) {
  strip.clear();
  drawVolume(vol);
  showStrip();
}

static const char* CAL_PATH = "/calibration.txt";

// USB disk volume label. FAT labels are max 11 characters and are
// (deliberately) uppercase: only then is the display identical on
// Windows and Linux (x86 and ARM). Mixed-case is only possible on exFAT/NTFS.
static const char* VOL_LABEL = "EMISNIFFER";

// Contents used to create the file on the very first start (values from
// CalDefaults). Write it with printf only: that formats into RAM first. FatFS
// can hand a plain print() buffer straight to the flash write, which cannot
// read from flash (XIP is off) -> bus fault.
static const char DEFAULT_CAL_FORMAT[] =
  "# VU meter calibration\r\n"
  "#\r\n"
  "# Edit the values below, save, and EJECT THE DISK;\r\n"
  "# the board then reloads calibration immediately.\r\n"
  "#\r\n"
  "# adc_min = ADC0 value that maps to 0 lit segments\r\n"
  "#           (threshold / noise floor). Anything below = all off.\r\n"
  "# adc_max = ADC0 value that maps to 8 lit segments (full).\r\n"
  "# smoothing = bar damping, 0..95.\r\n"
  "#             0  = no damping (fast but nervous)\r\n"
  "#             %u = calm/smooth (default), higher = slower.\r\n"
  "# vbat_cal  = battery calibration factor (0.5..2.0). Increase if\r\n"
  "#             displayed Vbat is too low (measured V / read V).\r\n"
  "# ADC0 range is 0..4095 (12 bit). Lines starting with # are comments.\r\n"
  "\r\n"
  "adc_min=%u\r\n"
  "adc_max=%u\r\n"
  "smoothing=%u\r\n"
  "vbat_cal=%.2f\r\n";


// Parse a "key=value" line and apply it to calibration.
static void applyCalLine(const String& raw) {
  String line = raw;
  line.trim();
  if (line.length() == 0 || line.startsWith("#")) return;

  const int eq = line.indexOf('=');
  if (eq < 0) return;

  String key = line.substring(0, eq);   key.trim();
  String val = line.substring(eq + 1);  val.trim();

  // Float parameter separately (outside the 0..4095 check below).
  if (key == "vbat_cal") {
    const float f = val.toFloat();
    if (f > 0.5f && f < 2.0f) cal.vbatCal = f;   // plausible range
    return;
  }

  const long v = val.toInt();
  if (v < 0 || v > 4095) return;

  if (key == "adc_min") cal.adcMin = (uint16_t)v;
  else if (key == "adc_max") cal.adcMax = (uint16_t)v;
  else if (key == "smoothing") cal.smoothing = (uint8_t)(v > 95 ? 95 : v);
}

// Read /calibration.txt from FatFS (must already be mounted) and apply cal.
// Falls back to defaults if the file is missing or the range is invalid.
static void loadCalibration() {
  cal = Calibration();   // start from defaults

  File f = FatFS.open(CAL_PATH, "r");
  if (!f) {
    if (!stQuiet) Serial.println("calibration.txt not found -> using defaults");
    return;
  }
  while (f.available()) {
    applyCalLine(f.readStringUntil('\n'));
  }
  f.close();

  if (cal.adcMax <= cal.adcMin) {
    if (!stQuiet) Serial.println("Invalid range (adc_max <= adc_min) -> restored defaults");
    cal = Calibration();
  }
  if (!stQuiet) {
    Serial.printf("Calibration loaded: adc_min=%u adc_max=%u smoothing=%u vbat_cal=%.2f\n",
                  cal.adcMin, cal.adcMax, cal.smoothing, cal.vbatCal);
  }
}

// Map an ADC value to a continuous segment level (0.0 .. 8.0).
static float adcToLevelF(float adc) {
  if (adc <= cal.adcMin) return 0.0f;
  if (adc >= cal.adcMax) return (float)VU_SEGMENTS;
  return (adc - cal.adcMin) * (float)VU_SEGMENTS / (cal.adcMax - cal.adcMin);
}

// --- USB disk (FatFSUSB) ----------------------------------------------------
// Callbacks run from the USB interrupt: do NOT print to Serial here and
// do as little as possible. Real work (reload) happens in loop().
// ----------------------------------------------------------------------------
static volatile bool driveConnected = false;   // PC has mounted the disk
static volatile bool reloadPending  = false;   // disk ejected -> reload

static void onPlug(uint32_t) {
  driveConnected = true;
  FatFS.end();               // PC takes over the blocks; we let go
}

static void onUnplug(uint32_t) {
  driveConnected = false;
  reloadPending  = true;     // remount + reread in loop()
}

static bool driveMountable(uint32_t) {
  return true;               // always safe: we do not write ourselves while mounted
}

static void reloadFsIfNeeded() {
  if constexpr (!Features::filesystem) return;
  if (reloadPending && !driveConnected) {
    FatFS.begin();
    loadCalibration();
    reloadPending = false;
  }
}

static void setupFilesystem() {
  if constexpr (!Features::filesystem) return;

  if (!FatFS.begin()) {
    Serial.println("FatFS not formatted yet -> formatting...");
    FatFS.format();
    FatFS.begin();
  }

  // Set the volume label and default calibration.txt; flash is only written when missing.
  if constexpr (Features::provisionAtBoot) {
    char lbl[16] = {0};
    fatfs::f_getlabel("", lbl, nullptr);
    if (strcmp(lbl, VOL_LABEL) != 0) {
      const int r = (int)fatfs::f_setlabel(VOL_LABEL);
      Serial.printf("Volume label set to '%s' (result=%d)\n", VOL_LABEL, r);
    }
    if (!FatFS.exists(CAL_PATH)) {
      File f = FatFS.open(CAL_PATH, "w");
      if (f) {
        f.printf(DEFAULT_CAL_FORMAT, (unsigned)CalDefaults::smoothing,
                 (unsigned)CalDefaults::adcMin, (unsigned)CalDefaults::adcMax,
                 (unsigned)CalDefaults::smoothing, (double)CalDefaults::vbatCal);
        f.close();
      }
      Serial.println("calibration.txt created with defaults");
    }
  }

  loadCalibration();
  FatFSUSB.onPlug(onPlug);
  FatFSUSB.onUnplug(onUnplug);
  FatFSUSB.driveReady(driveMountable);
  FatFSUSB.begin();
  delay(2000);  // TinyUSB race-condition workaround (see arduino-pico example)
}

// --- MCP4011 digital pot -----------------------------------------------------
// Increment/decrement interface: CS active-low, U/D = direction + step pulse.
// Increment on a L->H edge of U/D, decrement on a H->L edge (with CS low).
// ----------------------------------------------------------------------------
static int16_t potPos = -1;         // current WIPER tap (-1 = unknown)
static uint32_t volDisplayUntil = 0; // show volume on the VU meter until this time
static constexpr uint32_t VOL_DISPLAY_MS = 1200;  // how long to show after a change

static void potBegin() {
  pinMode(POT_CS_PIN, OUTPUT);
  digitalWrite(POT_CS_PIN, HIGH);   // deselected
  pinMode(POT_UD_PIN, OUTPUT);
  digitalWrite(POT_UD_PIN, HIGH);
}

// Move the wiper `steps` steps. up=true -> increment (higher tap).
static void potMove(bool up, uint8_t steps) {
  digitalWrite(POT_UD_PIN, up ? HIGH : LOW);   // set direction first
  delayMicroseconds(5);
  digitalWrite(POT_CS_PIN, LOW);               // select
  delayMicroseconds(5);
  for (uint8_t i = 0; i < steps; i++) {
    digitalWrite(POT_UD_PIN, up ? LOW : HIGH); // opposite edge
    delayMicroseconds(5);
    digitalWrite(POT_UD_PIN, up ? HIGH : LOW); // this edge = 1 step
    delayMicroseconds(5);
  }
  digitalWrite(POT_CS_PIN, HIGH);              // deselect
  delayMicroseconds(5);
}

// Force the wiper to tap 0 (min) from an unknown position.
static void potHome() {
  potMove(false, POT_TAPS - 1);   // 63 steps down -> guaranteed 0
  potPos = 0;
}

// Set the wiper to an absolute tap (0..63).
static void potSetTap(int16_t target) {
  if (target < 0) target = 0;
  if (target > POT_TAPS - 1) target = POT_TAPS - 1;
  if (potPos < 0) potHome();
  if (target > potPos)      potMove(true,  target - potPos);
  else if (target < potPos) potMove(false, potPos - target);
  potPos = target;
}

// Volume level 0..63 (higher = louder). Inverted vs the wiper tap.
static uint8_t volumeLevel() { return (uint8_t)(POT_TAPS - 1 - potPos); }

static_assert(Settings::volumeMax <= POT_TAPS - 1, "volumeMax above the pot range");
static_assert(Settings::volumeMin <= Settings::volumeDefault &&
              Settings::volumeDefault <= Settings::volumeMax, "volumeDefault outside min..max");

// Set the volume volumeMin..volumeMax (clamped).
static void setVolume(int16_t vol) {
  potSetTap(POT_TAPS - 1 - constrain(vol, (int16_t)Settings::volumeMin, (int16_t)Settings::volumeMax));
}

// Volume +: louder = wiper tap DOWN (reversed vs the tap).
static void potUp() {
  setVolume(volumeLevel() + Settings::volumeStep);
  volDisplayUntil = millis() + VOL_DISPLAY_MS;
  if (!stQuiet) Serial.printf("TAP %d/%d\n", potPos, POT_TAPS - 1);
}
// Volume -: quieter = wiper tap UP.
static void potDown() {
  setVolume(volumeLevel() - Settings::volumeStep);
  volDisplayUntil = millis() + VOL_DISPLAY_MS;
  if (!stQuiet) Serial.printf("TAP %d/%d\n", potPos, POT_TAPS - 1);
}


// Setup ----------------------------------------------------------------------
// ----------------------------------------------------------------------------
void setup() {
  // FIRST: take over the power latch so the power button can be released.
  // GPIO25 HIGH holds the load switch on; GPIO20 reads the button.
  pinMode(POWER_HOLD_PIN, OUTPUT);
  digitalWrite(POWER_HOLD_PIN, HIGH);
  pinMode(BUTTON_PIN, BUTTON_ACTIVE_HIGH ? INPUT_PULLDOWN : INPUT_PULLUP);
  pinMode(VOL_DOWN_PIN, VOL_DOWN_ACTIVE_HIGH ? INPUT_PULLDOWN : INPUT_PULLUP);  // volume -
  pinMode(CHARGE_STAT_PIN, INPUT_PULLUP);   // MCP73830 STAT (LOW = charging)

  pinMode(AMP_EN_PIN, OUTPUT);
  updateAmpEnable(readCharging());

  // MCP4011 digital pot: init, home, and set the default position.
  potBegin();
  potHome();
  setVolume(Settings::volumeDefault);

  // More conservative QMI/XIP flash timing: slower clock + extra RX sample delay.
  // Gives more margin for a marginal flash on the XIP read right after a
  // flash write (possible cause of the IBUSERR lockup on a blank board).
  // Raising CLKDIV only is safe (slower); do this before any flash op.
  qmi_hw->m[0].timing = (qmi_hw->m[0].timing & ~0x000007ffu) | (2u << 8) | 12u; // RXDELAY=2, CLKDIV=12
  __asm volatile("" ::: "memory");

  analogReadResolution(12);

  if constexpr (!Features::readVbatAdc) {
    gpio_init(29);
    gpio_set_dir(29, GPIO_IN);
    gpio_disable_pulls(29);
  }

  // LEDs on immediately and show battery at boot (stays during the
  // FS-init/USB delay below; loop() holds it a bit longer).
  strip.begin();
  strip.setBrightness(16);   // 0-255, 75% lower than the earlier 64
  showBattery(readVbat());

  Serial.begin(115200);
  setupFilesystem();
}

// Power the unit off: LEDs off and release the load switch (GPIO25). Power
// drops once the button is released (until then the button itself still
// holds the rail); on battery with no button held it drops immediately.
static void powerOff() {
  strip.clear();
  strip.show();
  digitalWrite(POWER_HOLD_PIN, LOW);
  while (true) { /* wait until the supply collapses */ }
}

// GPIO20 has a dual function:
//  - SHORT press (release < 2s) = volume + (pot 1 step up)
//  - hold 2s = power off
// Detection is only armed after the boot press is released
// (otherwise holding at power-on would immediately shut down).
static void checkPowerButton() {
  static bool armed = false;
  static bool wasPressed = false;
  static uint32_t pressStart = 0;
  static bool shutdownArmed = false;
  const bool pressed = digitalRead(BUTTON_PIN) == (BUTTON_ACTIVE_HIGH ? HIGH : LOW);

  if (!armed) {
    if (!pressed) armed = true;   // button released after boot -> arm now
    wasPressed = pressed;
    return;
  }

  if (pressed && !wasPressed) {          // press edge
    pressStart = millis();
    shutdownArmed = true;
  }
  if (pressed && shutdownArmed && (millis() - pressStart >= POWER_OFF_HOLD_MS)) {
    powerOff();                          // held 2s -> off
  }
  if (!pressed && wasPressed) {          // release edge
    if (shutdownArmed && (millis() - pressStart < POWER_OFF_HOLD_MS)) {
      potUp();                           // short press -> volume +
    }
    shutdownArmed = false;
  }
  wasPressed = pressed;
}

// Watch the volume-down button (GPIO21): each press edge = pot 1 step down.
// Trimmed mean of 9 ADC samples: sort, drop the 2 lowest + 2 highest, average
// the middle 5. Kills single-sample outliers (RP2350 ADC DNL steps, LED-current
// transients on the 3V3 rail) that a plain average would drag along.
static constexpr uint8_t ADC_SAMPLES = 9;
static constexpr uint8_t ADC_TRIM    = 2;   // dropped at each end
static uint16_t readAdcAvg(uint8_t pin) {
  uint16_t s[ADC_SAMPLES];
  analogRead(pin);                                  // settle the mux, discard
  for (uint8_t i = 0; i < ADC_SAMPLES; i++) s[i] = analogRead(pin);
  std::sort(s, s + ADC_SAMPLES);
  uint32_t sum = 0;
  for (uint8_t i = ADC_TRIM; i < ADC_SAMPLES - ADC_TRIM; i++) sum += s[i];
  return (uint16_t)(sum / (ADC_SAMPLES - 2 * ADC_TRIM));
}

// ADC2 gain vs ADC1 for the balance. Set by centreBalance(); not saved, so
// centre again after power-on (like the tare on a scale).
static float balTrim = 1.0f;

// Measure ADC1/ADC2 for ~1 s (the channels are noisy) and trim ADC2 so the
// balance LED rests in the middle in the current surroundings.
static void centreBalance() {
  uint32_t s1 = 0, s2 = 0;
  for (uint16_t i = 0; i < 250; i++) {
    s1 += readAdcAvg(ADC1_PIN);
    s2 += readAdcAvg(ADC2_PIN);
    delay(4);
  }
  if (s2 > 0) balTrim = constrain((float)s1 / s2, 0.5f, 2.0f);
  if (!stQuiet) Serial.printf("Balance centred: trim=%.3f\n", balTrim);

  strip.clear();                                         // confirm: middle
  strip.setPixelColor(2, strip.Color(255, 255, 255));    // balance LED white
  showStrip();
  delay(300);
}

// Volume - button: short press = volume -, hold 2 s = centre the balance.
// Acts on release for a short press, so a long press does not also turn down.
static void checkVolumeDown() {
  static bool     wasPressed = false, held = false;
  static uint32_t pressStart = 0;
  const bool pressed = digitalRead(VOL_DOWN_PIN) == (VOL_DOWN_ACTIVE_HIGH ? HIGH : LOW);
  if (pressed && !wasPressed) { pressStart = millis(); held = false; }
  if (pressed && !held && millis() - pressStart >= CENTER_HOLD_MS) {
    held = true;
    centreBalance();
  }
  if (!pressed && wasPressed && !held && millis() - pressStart > 40) potDown();  // debounced
  wasPressed = pressed;
}

// Bring-up tests. Active until ST: QUIT or another test.
static void diagPinTest() {
  static bool s = false;
  s = !s;
  digitalWrite(POT_CS_PIN, s ? HIGH : LOW);
  digitalWrite(POT_UD_PIN, s ? LOW  : HIGH);
  Serial.printf("PINTEST CS(16)=%d  UD(17)=%d\n", s ? 1 : 0, s ? 0 : 1);
  delay(1000);
}

static void diagPotSweep() {
  static int dir = +1;
  potMove(dir > 0, 1);
  potPos += dir;
  if (potPos >= POT_TAPS - 1) { potPos = POT_TAPS - 1; dir = -1; }
  if (potPos <= 0)            { potPos = 0;            dir = +1; }
  static uint32_t lastPrint = 0;
  if (millis() - lastPrint >= 250) {
    lastPrint = millis();
    Serial.printf("POT tap=%d/%d\n", potPos, POT_TAPS - 1);
  }
  delay(4000 / (POT_TAPS - 1));
}

static void diagGpio29() {
  const uint32_t func = gpio_get_function(29);
  const uint32_t oe   = (sio_hw->gpio_oe  >> 29) & 1u;
  const uint32_t out  = (sio_hw->gpio_out >> 29) & 1u;
  const uint32_t pad  = pads_bank0_hw->io[29];
  const bool pue = pad & PADS_BANK0_GPIO0_PUE_BITS;
  const bool pde = pad & PADS_BANK0_GPIO0_PDE_BITS;
  const bool ie  = pad & PADS_BANK0_GPIO0_IE_BITS;
  const bool od  = pad & PADS_BANK0_GPIO0_OD_BITS;
  const int  a3  = adcSettledRead(A3);
  const bool charging = digitalRead(CHARGE_STAT_PIN) == LOW;
  Serial.printf("GPIO29 func=%lu OE=%lu OUT=%lu PUE=%d PDE=%d IE=%d OD=%d | A3=%4d | %s\n",
                (unsigned long)func, (unsigned long)oe, (unsigned long)out,
                pue, pde, ie, od, a3, charging ? "CHARGING" : "not");
  delay(200);
}

static void diagAdc() {
  Serial.printf("A0=%4d  A1=%4d  A2=%4d  A3=%4d  temp=%.1fC\n",
                adcSettledRead(A0), adcSettledRead(A1), adcSettledRead(A2), adcSettledRead(A3),
                analogReadTemp());
  delay(100);
}

// true = this loop iteration was consumed by a bring-up test.
static bool runDiagMode() {
  switch (diagMode) {
    case DiagMode::PinTest: diagPinTest(); return true;
    case DiagMode::Sweep:   diagPotSweep(); return true;
    case DiagMode::Gpio29:  diagGpio29(); return true;
    case DiagMode::Adc:     diagAdc(); return true;
    case DiagMode::None:    return false;
  }
  return false;
}

// Simple serial command-response (lines end with \n, \r is ignored):
//   ST: START -> ST: OK, then command replies only (no debug)
//   ST: QUIT  -> leave quiet mode + bring-up tests, LEDs and debug normal again
//   BATT      -> Vbat as integer millivolts
//   TAP=n     -> MCP4011 wiper tap, 0..63 (raw; 0 = loudest)
//   ADC=ALL   -> ADC: {ADC0}, {ADC1}, {ADC2}
//   ADC=0     -> ADC0 (GPIO26), 12-bit 0..4095; same for ADC=1 and ADC=2
//   LED=r,g,b -> all 13 LEDs + 1 extra (DOUT test) that colour; reply LED: OK
//   PINTEST   -> toggle pot CS/UD ~1 Hz (multimeter)
//   SWEEP     -> digital pot back and forth
//   GPIO29    -> live pad config of ADC3 + charge status
//   ADCDBG    -> A0..A3 + chip temp, continuous
static void pollSerialCommands() {
  static char buf[32];
  static uint8_t len = 0;
  while (Serial.available() > 0) {
    const char c = (char)Serial.read();
    if (c == '\r') continue;
    if (c == '\n') {
      buf[len] = '\0';
      len = 0;
      if (strcmp(buf, "ST: START") == 0) {
        diagMode = DiagMode::None;
        stQuiet = true;
        setAllLeds( 200, 200, 0 );
        Serial.print("ST: OK\n");
        Serial.flush();
      } else if (strcmp(buf, "ST: QUIT") == 0) {
        diagMode = DiagMode::None;
        stQuiet = false;
      } else if (strcmp(buf, "PINTEST") == 0) {
        diagMode = DiagMode::PinTest;
        Serial.print("PINTEST: OK\n");
      } else if (strcmp(buf, "SWEEP") == 0) {
        diagMode = DiagMode::Sweep;
        Serial.print("SWEEP: OK\n");
      } else if (strcmp(buf, "GPIO29") == 0) {
        diagMode = DiagMode::Gpio29;
        Serial.print("GPIO29: OK\n");
      } else if (strcmp(buf, "ADCDBG") == 0) {
        diagMode = DiagMode::Adc;
        Serial.print("ADCDBG: OK\n");
      } else if (strcmp(buf, "BATT") == 0) {
        Serial.printf("%d\n", (int) readAdcAvg(VBAT_ADC_PIN) );
      } else if (strncmp(buf, "TAP=", 4) == 0) {
        // Raw wiper tap, not setVolume(): the test station needs the whole pot
        // range, not just volumeMin..volumeMax.
        int tap = -1;
        if (sscanf(buf + 4, "%d", &tap) == 1 && tap >= 0 && tap < POT_TAPS) {
          potSetTap((int16_t)tap);
          Serial.print("TAP: OK\n");
        }
      } else if (strcmp(buf, "ADC=ALL") == 0) {
        Serial.printf("ADC: %u, %u, %u\n",
                      readAdcAvg(ADC_PIN), readAdcAvg(ADC1_PIN), readAdcAvg(ADC2_PIN));
      } else if (strcmp(buf, "ADC=0") == 0) {
        Serial.printf("ADC: 0, %u\n", readAdcAvg( ADC_PIN));
      } else if (strcmp(buf, "ADC=1") == 0) {
        Serial.printf("ADC: 1, %u\n", readAdcAvg(ADC1_PIN));
      } else if (strcmp(buf, "ADC=2") == 0) {
        Serial.printf("ADC: 2, %u\n", readAdcAvg(ADC2_PIN));
      } else if (strncmp(buf, "LED=", 4) == 0) {
        int r = -1, g = -1, b = -1;
        if (sscanf(buf + 4, "%d,%d,%d", &r, &g, &b) == 3 &&
            r >= 0 && r <= 255 && g >= 0 && g <= 255 && b >= 0 && b <= 255) {
          setAllLeds((uint8_t)r, (uint8_t)g, (uint8_t)b);
          Serial.print("LED: OK\n");
        }
      }
      continue;
    }
    if (len < sizeof(buf) - 1) buf[len++] = c;
    else len = 0;   // overflow -> discard line
  }
}

// Watch battery voltage: 2s continuously under cutoff (and NOT charging)
// -> shut down to protect the Li-Po.
static void checkBatteryCutoff(float vbat, bool charging) {
  static uint32_t lowStart = 0;
  if (!charging && vbat < VBAT_CUTOFF) {
    if (lowStart == 0) lowStart = millis();
    else if (millis() - lowStart >= VBAT_CUTOFF_MS) powerOff();
  } else {
    lowStart = 0;
  }
}

// Auto power-off after Settings::autoOffMinutes on battery. The timer is zeroed at
// switch-on (millis() starts at 0) and kept zeroed while charging. When
// charging stops, that edge restarts the countdown.
static void checkAutoOff(bool charging) {
  if constexpr (Settings::autoOffMinutes == 0) return;
  constexpr uint32_t AUTO_OFF_MS = Settings::autoOffMinutes * 60UL * 1000UL;

  static uint32_t autoOffStart = 0;
  static bool     wasCharging  = false;

  if (charging || wasCharging) {
    autoOffStart = millis();   // charging, or the loop that saw charging stop
  } else if (millis() - autoOffStart >= AUTO_OFF_MS) {
    powerOff();
  }
  wasCharging = charging;
}

static LoopMode selectLoopMode(bool charging) {
  if (diagMode != DiagMode::None) return LoopMode::Diag;
  if (stQuiet) return LoopMode::Quiet;
  if (millis() < STARTUP_BATTERY_MS || charging) {
    return millis() < volDisplayUntil ? LoopMode::ShowVolume : LoopMode::ShowBattery;
  }
  return LoopMode::Emi;   // shows the volume itself, next to the balance
}

static void loopShowBattery(float vbat, bool charging) {
  if (charging) showBatteryCharging(vbat);
  else          showBattery(vbat);

  static uint32_t lastBatPrint = 0;
  if (millis() - lastBatPrint >= 500) {
    lastBatPrint = millis();
    const uint16_t a0 = readAdcAvg(ADC_PIN);
    const uint16_t a1 = readAdcAvg(ADC1_PIN);
    const uint16_t a2 = readAdcAvg(ADC2_PIN);
    const float bal = (a1 + a2 > 0) ? (float)a2 / (a1 + a2) : 0.5f;
    long seg = lroundf(adcToLevelF(a0));
    if (seg < 1) seg = 1; else if (seg > VU_SEGMENTS) seg = VU_SEGMENTS;
    Serial.printf("BATT %.2fV | ADC0=%4u level=%ld/%u | ADC1=%4u ADC2=%4u bal=%.2f %s\n",
                  vbat, a0, seg, VU_SEGMENTS, a1, a2, bal, charging ? "(charging)" : "(startup)");
  }
}

static void loopEmi(float vbat) {
  const uint16_t adc = readAdcAvg(ADC_PIN);

  // Low-pass filter (exponential moving average) over the ADC:
  //   alpha small -> lots of damping / slow,  alpha ~1 -> almost no damping.
  // smoothing 0..95 maps to alpha 1.00..0.05.
  const float alpha = (100 - cal.smoothing) / 100.0f;
  static float emaAdc = -1.0f;
  if (emaAdc < 0.0f) emaAdc = adc;
  emaAdc += alpha * (adc - emaAdc);

  // Continuous level + hysteresis: only change the shown segment count when
  // the value clearly crosses the boundary, so it does not flicker.
  // At least 1 segment: the VU meter always shows at least 1 lamp, however quiet.
  const float lvlF = adcToLevelF(emaAdc);
  static uint8_t shown = 1;
  while (shown < VU_SEGMENTS && lvlF >= shown + 0.6f) shown++;
  while (shown > 1            && lvlF <= shown - 0.4f) shown--;

  // Balance ADC1 vs ADC2 on LED 1..5 (strip index 0..4).
  // Ratio 0..1: 0 = ADC1 only (LED1), 0.5 = equal (LED3), 1 = ADC2 only (LED5).
  // balanceGain scales the difference around the middle (1 = plain a2/(a1+a2)).
  const uint16_t a1 = readAdcAvg(ADC1_PIN), a2 = readAdcAvg(ADC2_PIN);
  const float    a2t = a2 * balTrim;   // equalise the two channels (long press volume -)
  float bal = 0.5f;
  if (a1 + a2t > 0)
    bal = constrain(0.5f + Settings::balanceGain * 0.5f * (a2t - a1) / (a1 + a2t), 0.0f, 1.0f);

  static float emaBal = 0.5f;
  emaBal += alpha * (bal - emaBal);

  const float contPos = emaBal * 4.0f;
  static int8_t balPos = 2;
  while (balPos < 4 && contPos >= balPos + 0.7f) balPos++;
  while (balPos > 0 && contPos <= balPos - 0.7f) balPos--;

  const uint8_t balLed = 4 - balPos;   // LEDs 1..5 are physically reversed
  const uint16_t balMag = a1 > a2 ? a1 : a2;
  const uint8_t  balR = (uint16_t)balMag * 255 / 4095;
  const uint8_t  balG = 255 - balR;

  strip.clear();
  strip.setPixelColor(balLed, strip.Color(balR, balG, 0));
  if (millis() < volDisplayUntil) {
    drawVolume(volumeLevel());   // after + / -: volume on the VU LEDs, balance stays
  } else {
    for (uint8_t i = 0; i < shown; i++) {
      const uint8_t idx = vuOrder[i];
      strip.setPixelColor(idx, colorForLed(idx));
    }
  }
  showStrip();

  static uint32_t lastPrint = 0;
  if (millis() - lastPrint >= 200) {
    lastPrint = millis();
    Serial.printf("ADC0=%4u ema=%4d level=%u/%u | ADC1=%4u ADC2=%4u bal=%.2f LED%u | Vbat=%.2f chg=%d | (min=%u max=%u sm=%u)%s\n",
                  adc, (int)emaAdc, shown, VU_SEGMENTS,
                  a1, a2, emaBal, balLed + 1,
                  vbat, digitalRead(CHARGE_STAT_PIN) == LOW,
                  cal.adcMin, cal.adcMax, cal.smoothing,
                  driveConnected ? "  [disk mounted]" : "");
  }
}

void loop() {
  checkPowerButton();
  pollSerialCommands();
  if (diagMode == DiagMode::None) checkVolumeDown();

  const bool  charging = readCharging();
  const float vbat     = readVbat();
  updateAmpEnable(charging);
  if constexpr (Features::battCutoff) checkBatteryCutoff(vbat, charging);
  checkAutoOff(charging);
  reloadFsIfNeeded();

  switch (selectLoopMode(charging)) {
    case LoopMode::Diag:        runDiagMode(); break;
    case LoopMode::Quiet:       break;
    case LoopMode::ShowVolume:  showVolume(volumeLevel()); break;
    case LoopMode::ShowBattery: loopShowBattery(vbat, charging); break;
    case LoopMode::Emi:         loopEmi(vbat); break;
  }
  delay(20);
}
