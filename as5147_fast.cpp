#include "as5147_fast.h"

// AS5147 frame layout on the read response:
//   bit 15    : even parity over bits 14..0
//   bit 14    : error flag (EF)
//   bits 13..0: data
static const uint16_t AS5147_PARITY_MASK = 0x8000;
static const uint16_t AS5147_ERROR_MASK = 0x4000;
static const uint16_t AS5147_DATA_MASK = 0x3FFF;

AS5147Fast::AS5147Fast(int chip_select_pin, SPIClass *spi, uint32_t clock_hz)
    : spi_(spi),
      settings_(clock_hz, MSBFIRST, SPI_MODE1),
      cs_pin_(chip_select_pin),
      last_count_(0),
      parity_errors_(0),
      framing_errors_(0),
      last_read_valid_(false) {}

bool AS5147Fast::evenParityOk(uint16_t value) {
  // Parity bit 15 makes the whole 16-bit word have even parity.
  uint16_t v = value;
  v ^= v >> 8;
  v ^= v >> 4;
  v ^= v >> 2;
  v ^= v >> 1;
  return (v & 1) == 0;
}

uint16_t AS5147Fast::transfer(uint16_t command) {
  spi_->beginTransaction(settings_);
  digitalWrite(cs_pin_, LOW);
  const uint16_t response = spi_->transfer16(command);
  digitalWrite(cs_pin_, HIGH);
  spi_->endTransaction();
  return response;
}

void AS5147Fast::init() {
  pinMode(cs_pin_, OUTPUT);
  digitalWrite(cs_pin_, HIGH);
  spi_->begin();

  // Prime the pipeline: the first transfer's response belongs to whatever command the
  // sensor last held, so issue the angle command twice and discard the first response.
  transfer(AS5147_ANGLECOM_READ);
  delayMicroseconds(10);
  transfer(AS5147_ANGLECOM_READ);

  last_count_ = 0;
  parity_errors_ = 0;
  framing_errors_ = 0;
  last_read_valid_ = false;

  Sensor::init();
}

uint16_t AS5147Fast::getRawCount() {
#ifdef AS5147_FAST_TWO_TRANSFER
  // Fallback path (T012): command then read, without SimpleFOC's 50 us delay. Still much
  // cheaper than the library path because the clock is 8 MHz rather than 1 MHz.
  transfer(AS5147_ANGLECOM_READ);
  const uint16_t response = transfer(AS5147_NOP_READ);
#else
  // Pipelined single transfer: this command's response carries the previous command's
  // angle. Because the same command is issued every cycle, the response is always the
  // angle sampled one control cycle earlier, which is a fixed one-cycle latency rather
  // than an error, and is identical for both motors.
  const uint16_t response = transfer(AS5147_ANGLECOM_READ);
#endif

  if (!evenParityOk(response)) {
    parity_errors_++;
    last_read_valid_ = false;
    return last_count_;  // hold the previous good value rather than injecting a jump
  }
  if (response & AS5147_ERROR_MASK) {
    framing_errors_++;
    last_read_valid_ = false;
    return last_count_;
  }

  last_count_ = response & AS5147_DATA_MASK;
  last_read_valid_ = true;
  return last_count_;
}

float AS5147Fast::getSensorAngle() {
  return ((float)getRawCount() / (float)AS5147_FAST_CPR) * _2PI;
}
