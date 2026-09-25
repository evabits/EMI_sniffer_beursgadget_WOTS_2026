#pragma once

#include <stdint.h>

// Production and bring-up switches. Dead branches are compiled out via
// if constexpr; change behaviour here, not with -D flags in loop()/setup().

struct Features {
  static constexpr bool filesystem      = true;   // FatFS + USB disk
  static constexpr bool provisionAtBoot = true;   // label + calibration.txt at boot (only written when missing)
  static constexpr bool battCutoff      = true;   // shut down on low Vbat
  static constexpr bool readVbatAdc     = true;   // ADC3 / GPIO29; false = high-Z, Vbat=0
  static constexpr bool ignoreCharging  = false;  // true = ignore STAT, always EMI display
};

// Fallback calibration: used when calibration.txt lacks a value, and written
// into a new calibration.txt on first boot.
struct CalDefaults {
  static constexpr uint16_t adcMin    = 50;     // ADC0 -> 0 segments (noise floor)
  static constexpr uint16_t adcMax    = 200;    // ADC0 -> 8 segments (full)
  static constexpr uint8_t  smoothing = 85;     // 0 (fast/nervous) .. 95 (slow/smooth)
  static constexpr float    vbatCal   = 1.17f;  // Vbat calibration factor; tune per unit
};

struct Settings {
  static constexpr uint32_t autoOffMinutes = 5;   // idle power-off on battery; 0 = never
  // Balance (LED 1..5) sensitivity: 1 = plain ADC1/ADC2 ratio; 2 = half the
  // difference already moves the dot as far; higher = more sensitive.
  static constexpr float    balanceGain    = 4.0f;
  // Volume (input gain, MCP4011 has 0..63): + / - move volumeStep at a time.
  static constexpr uint8_t  volumeMin      = 10;   // floor
  static constexpr uint8_t  volumeMax      = 50;   // cap
  static constexpr uint8_t  volumeStep     = 5;    // per press
  static constexpr uint8_t  volumeDefault  = 30;   // at power-on (a multiple of volumeStep)
};
