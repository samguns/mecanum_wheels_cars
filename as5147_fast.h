#pragma once

// First-party AS5147 SPI reader.
//
// Why this exists (research D1/D2): SimpleFOC's MagneticSensorSPI::read() contains a
// hardcoded delayMicroseconds(50) on ESP32 and issues two 16-bit transfers at 1 MHz, so
// two encoders cost about 170 us per control cycle. That alone caps the loop near 4 kHz
// and puts the required 1000 Hz bandwidth default out of reach.
//
// The AS5147 SPI interface is pipelined: one transfer both issues a command and returns
// the previous command's result. A single transfer at 8 MHz costs roughly 2 us, taking
// both encoders under about 10 us.
//
// The installed SimpleFOC is deliberately NOT patched. It lives outside this repository,
// so an edit there would be invisible to version control, lost on library update, and
// unreproducible on another build host.
//
// This class derives from SimpleFOC's Sensor base so Sensor::update() keeps accumulating
// full_rotations and multi-turn angle behaviour is unchanged.

#include <Arduino.h>
#include <SPI.h>
#include <SimpleFOC.h>

// Set AS5147_FAST_TWO_TRANSFER to fall back to the safe two-transfer exchange if the
// pipelined single-transfer read fails hardware verification (task T011/T012).
// #define AS5147_FAST_TWO_TRANSFER

static const uint32_t AS5147_FAST_DEFAULT_CLOCK_HZ = 8000000;  // datasheet allows 10 MHz
static const uint16_t AS5147_FAST_CPR = 16384;                 // 14 bit
static const uint16_t AS5147_ANGLECOM_READ = 0xFFFF;           // read 0x3FFF, R/W=1, parity
static const uint16_t AS5147_NOP_READ = 0xC000;                // read 0x0000

class AS5147Fast : public Sensor {
 public:
  explicit AS5147Fast(int chip_select_pin, SPIClass *spi = &SPI,
                      uint32_t clock_hz = AS5147_FAST_DEFAULT_CLOCK_HZ);

  // Configures the chip select pin and primes the pipeline. SPI.begin() must already
  // have been called, or is called here when it has not been.
  void init();

  // Sensor interface. Returns the single-turn mechanical angle in radians; the base class
  // turns that into an accumulated multi-turn angle.
  float getSensorAngle() override;

  // Raw 14-bit count, for diagnostics and the equivalence check against MagneticSensorSPI.
  uint16_t getRawCount();

  // Error counters, readable from the communications context only.
  uint32_t parityErrors() const { return parity_errors_; }
  uint32_t framingErrors() const { return framing_errors_; }
  bool lastReadValid() const { return last_read_valid_; }

 private:
  uint16_t transfer(uint16_t command);
  static bool evenParityOk(uint16_t value);

  SPIClass *spi_;
  SPISettings settings_;
  int cs_pin_;
  uint16_t last_count_;
  uint32_t parity_errors_;
  uint32_t framing_errors_;
  bool last_read_valid_;
};
