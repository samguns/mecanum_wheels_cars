#pragma once

// Deterministic FOC loop timing: bandwidth -> control rate -> PWM carrier derivation,
// a PWM-phase-locked trigger, and the loop timing record.
//
// Contracts: specs/002-mit-impedance-control/contracts/serial-console.md
// Design:    specs/002-mit-impedance-control/research.md decisions D4, D5, D6, D12
// Model:     specs/002-mit-impedance-control/data-model.md
//
// The derivation helpers below are PURE (no hardware, no globals) so they can be
// exhaustively exercised for every integer request in the supported range.

#include <Arduino.h>
#include <SimpleFOC.h>

// ---------------------------------------------------------------------------
// Fixed design constants
// ---------------------------------------------------------------------------

// FIXED at 10 and never reduced. Clarified FR-011 and research D3 forbid lowering the
// multiple to make a requested bandwidth appear reachable; an unreachable request clamps.
static const uint8_t FOC_SAMPLING_MULTIPLE = 10;

static const uint16_t FOC_BANDWIDTH_MIN_HZ = 100;
static const uint16_t FOC_BANDWIDTH_MAX_HZ = 10000;
static const uint16_t FOC_BANDWIDTH_DEFAULT_HZ = 1000;

// Carrier window. The lower bound keeps switching out of the audible band; the upper
// bound is the hard SimpleFOC MCPWM cap (_PWM_FREQUENCY_MAX in esp32_driver_mcpwm.h).
static const uint32_t FOC_CARRIER_MIN_HZ = 20000;
static const uint32_t FOC_CARRIER_MAX_HZ = 50000;

// MCPWM timebase, matching _PWM_TIMEBASE_RESOLUTION_HZ.
static const uint32_t FOC_MCPWM_TIMEBASE_HZ = 160000000UL;

// Fail-closed thresholds (data-model.md).
static const uint16_t FOC_MAX_CONSECUTIVE_OVERRUNS = 10;
static const float FOC_RATE_DEVIATION_TOLERANCE = 0.05f;

// Duty budget used when converting a measured worst-case cycle time into a sustainable
// rate, so overruns stay impossible under communication load.
static const float FOC_DUTY_BUDGET = 0.60f;

// PROVISIONAL until T031 measures the real ceiling on hardware. Seeded at the rate that
// makes the 1000 Hz default exactly reachable; T031 replaces it with the measured value.
// If the measured ceiling is lower, the default request still stays 1000 Hz and the
// active bandwidth clamps and reports (FR-020b), rather than weakening the 10x multiple.
static const float FOC_MAX_SUSTAINABLE_RATE_HZ_PROVISIONAL = 10000.0f;

// ---------------------------------------------------------------------------
// Derived timing configuration
// ---------------------------------------------------------------------------
struct FocTimingConfig {
  uint16_t requested_bandwidth_hz;
  uint16_t active_bandwidth_hz;
  bool clamped;
  uint8_t sampling_multiple;
  uint32_t control_rate_hz;
  float control_period_us;
  uint32_t carrier_hz;
  uint8_t decimation;
  uint32_t carrier_period_ticks;
  bool valid;
};

// ---------------------------------------------------------------------------
// Pure derivation (exhaustively verifiable, no hardware dependency)
// ---------------------------------------------------------------------------

// Strict integer parse. Rejects empty text, signs, whitespace-only, non-digits, and
// anything outside the supported range. Never partially writes out_hz on failure.
inline bool focParseBandwidthHz(const char *text, uint16_t &out_hz) {
  if (text == nullptr) return false;
  uint32_t value = 0;
  uint8_t digits = 0;
  for (const char *p = text; *p != '\0'; ++p) {
    if (*p == ' ' || *p == '\t' || *p == '\r' || *p == '\n') {
      if (digits == 0) continue;  // leading whitespace only
      return false;               // embedded or trailing junk
    }
    if (*p < '0' || *p > '9') return false;
    value = value * 10u + (uint32_t)(*p - '0');
    if (++digits > 5) return false;  // cannot be in range
  }
  if (digits == 0) return false;
  if (value < FOC_BANDWIDTH_MIN_HZ || value > FOC_BANDWIDTH_MAX_HZ) return false;
  out_hz = (uint16_t)value;
  return true;
}

// Derives the control rate, carrier and integer decimation for a requested bandwidth.
//
// Invariants guaranteed on success:
//   control_rate_hz == active_bandwidth_hz * FOC_SAMPLING_MULTIPLE   (multiple never reduced)
//   carrier_hz      == control_rate_hz * decimation                 (exact integer ratio)
//   FOC_CARRIER_MIN_HZ <= carrier_hz <= FOC_CARRIER_MAX_HZ
//   active_bandwidth_hz <= requested_bandwidth_hz
//   clamped == (active_bandwidth_hz < requested_bandwidth_hz)
inline bool focDeriveTiming(uint16_t requested_hz, float max_sustainable_rate_hz,
                           FocTimingConfig &out) {
  out = FocTimingConfig();
  out.sampling_multiple = FOC_SAMPLING_MULTIPLE;
  out.valid = false;

  if (requested_hz < FOC_BANDWIDTH_MIN_HZ || requested_hz > FOC_BANDWIDTH_MAX_HZ) return false;
  out.requested_bandwidth_hz = requested_hz;

  // Clamp on an integer Hz boundary so the rate and carrier stay integers.
  uint32_t ceiling_bw = (max_sustainable_rate_hz <= 0.0f)
                            ? 0u
                            : (uint32_t)(max_sustainable_rate_hz / (float)FOC_SAMPLING_MULTIPLE);
  if (ceiling_bw < FOC_BANDWIDTH_MIN_HZ) return false;  // hardware cannot host the feature

  uint32_t active_bw = requested_hz;
  if (active_bw > ceiling_bw) active_bw = ceiling_bw;

  // The carrier ceiling imposes its own bandwidth limit: carrier >= rate, so
  // rate <= FOC_CARRIER_MAX_HZ and therefore bandwidth <= carrier_max / multiple.
  const uint32_t carrier_limited_bw = FOC_CARRIER_MAX_HZ / FOC_SAMPLING_MULTIPLE;
  if (active_bw > carrier_limited_bw) active_bw = carrier_limited_bw;
  if (active_bw < FOC_BANDWIDTH_MIN_HZ) return false;

  const uint32_t rate = active_bw * (uint32_t)FOC_SAMPLING_MULTIPLE;

  // Smallest integer decimation putting the carrier at or above the audible floor.
  uint32_t decimation = (FOC_CARRIER_MIN_HZ + rate - 1u) / rate;  // ceil
  if (decimation < 1u) decimation = 1u;
  const uint32_t carrier = rate * decimation;
  if (carrier > FOC_CARRIER_MAX_HZ) return false;
  if (decimation > 255u) return false;

  out.active_bandwidth_hz = (uint16_t)active_bw;
  out.clamped = (active_bw < (uint32_t)requested_hz);
  out.control_rate_hz = rate;
  out.control_period_us = 1000000.0f / (float)rate;
  out.carrier_hz = carrier;
  out.decimation = (uint8_t)decimation;
  out.carrier_period_ticks = FOC_MCPWM_TIMEBASE_HZ / carrier;
  out.valid = true;
  return true;
}

// Converts a measured worst-case cycle time into a sustainable control rate.
inline float focSustainableRateFromCycleUs(uint32_t worst_cycle_us) {
  if (worst_cycle_us == 0) return FOC_MAX_SUSTAINABLE_RATE_HZ_PROVISIONAL;
  return (FOC_DUTY_BUDGET * 1000000.0f) / (float)worst_cycle_us;
}

// ---------------------------------------------------------------------------
// Loop timing record
// ---------------------------------------------------------------------------
struct LoopTimingRecord {
  uint32_t cycle_count;
  uint32_t overrun_count;
  uint16_t consecutive_overruns;
  uint32_t worst_cycle_us;
  uint32_t last_cycle_us;
  float measured_rate_hz;
  bool overrun_fault;  // sustained overruns tripped the fail-closed path
  bool timing_fault;   // measured rate diverged from nominal without overruns
};

// Single writer: the deterministic control task. Readers are the communications context.
// Fields are word sized, so a torn read can only ever report a stale count, never a
// corrupt one, and no reader takes a lock in the control path.
extern LoopTimingRecord foc_timing;

// ---------------------------------------------------------------------------
// Runtime control (hardware dependent)
// ---------------------------------------------------------------------------

// The sketch supplies the per-cycle work and the fail-closed action. Neither may print
// or block: they run in the deterministic control path (FR-035).
typedef void (*FocCycleFn)(void);
typedef void (*FocFailClosedFn)(void);

// Registers the MCPWM on_full callback on the driver's timer, creates the high-priority
// control task pinned to the FOC core, and starts triggering.
// driver_params is BLDCDriver3PWM::params after a successful init().
bool focTimingBegin(void *driver_params, const FocTimingConfig &cfg, FocCycleFn cycle,
                    FocFailClosedFn fail_closed);

// Updates the ISR decimation factor atomically. Safe while running.
void focTimingSetDecimation(uint8_t decimation);

// Applies a new carrier to the shared MCPWM timer and both drivers' duty scaling.
// MUST only be called while every motor is disarmed (FR-020c). Returns false when the
// carrier cannot be changed in place, in which case the caller reports that the new
// value takes effect after a restart.
bool focTimingApplyCarrier(void *driver_params_a, void *driver_params_b, uint32_t carrier_hz,
                           uint32_t carrier_period_ticks);

// Resets the counters. Disarmed use only.
void focTimingResetCounters();

// Applies the derived gains, filter constants and fixed timesteps to one motor.
// Setting Ts on every PID and filter is what removes adaptive _micros() timing (D6).
void focApplyMotorTuning(BLDCMotor &motor, float resistance, float inductance_d,
                         float inductance_q, float bandwidth_hz, float control_period_us);

// Applies only the fixed timestep, for callers that have not changed the gains.
void focApplyMotorTimestep(BLDCMotor &motor, float control_period_us);

// Exhaustive self-test of the pure derivation across every integer request in range plus
// the malformed parser cases (D12). Returns the failure count and writes a report to
// the supplied stream. Communications context only.
uint32_t focTimingSelfTest(Print &out);

// Optional scope instrumentation, compiled out unless FOC_TIMING_INSTRUMENT is defined.
void focTimingInstrumentBegin(int pin);
