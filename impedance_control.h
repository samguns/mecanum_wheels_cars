#pragma once

// MIT-style impedance control: per-motor state, atomic command-pair staging, the torque
// law with position-error saturation, and effort-limit cause tracking.
//
// Contract: specs/002-mit-impedance-control/contracts/can-protocol.md
// Model:    specs/002-mit-impedance-control/data-model.md
// Design:   specs/002-mit-impedance-control/research.md decisions D7, D8, D9

#include <Arduino.h>

#include "can_protocol.h"

// Position error is saturated BEFORE being multiplied by stiffness, so an arbitrarily
// large target cannot request unbounded torque (FR-001b, research D8). This is a first
// guard, not a replacement for the current and voltage limits downstream.
static const float POSITION_ERROR_LIMIT = 1.0f;

// Per-motor command timeout. A timed-out motor produces zero effort but stays armed and
// recovers on its own, which is deliberately weaker than a disarm (FR-029, FR-029b).
static const uint32_t IMPEDANCE_COMMAND_TIMEOUT_US = 50000;

// PROVISIONAL until measured on the bench (T043). Converts commanded torque into a
// current setpoint. Published in contracts/can-protocol.md alongside the t_ff range.
static const float MOTOR_TORQUE_CONSTANT_NM_PER_A = 0.05f;

// Staging record for one motor's in-flight command pair. A logical command is only
// applied once both halves are present, validated, and carry the same sequence.
struct PendingImpedancePair {
  ImpedancePositionHalf position;
  ImpedanceDynamicsHalf dynamics;
  bool has_position;
  bool has_dynamics;
  uint32_t position_us;
  uint32_t dynamics_us;
};

struct MotorImpedanceState {
  uint8_t mode;  // MOTION_MODE_VELOCITY or MOTION_MODE_IMPEDANCE

  // Applied five-term state, copied atomically from a validated matched pair.
  int32_t p_des_mrad;
  float p_des;
  float v_des;
  float kp;
  float kd;
  float t_ff;

  float velocity_target;  // velocity mode only

  uint32_t last_command_us;
  bool timed_out;
  bool armed;

  // Effort limiting. limit_cause latches until reported at least once; limit_was_active
  // drives rising-edge counting independently of that report latch (FR-004a).
  uint8_t limit_cause;
  uint8_t limit_was_active;
  uint16_t limit_event_count;

  bool pair_fault_latched;
  uint8_t last_applied_seq;
  bool has_applied_seq;
  // Serial `K` is operator-paced, not a 200 Hz stream. While set, the 50 ms CAN timeout
  // does not fire; disarm, mode change, and other zero-effort paths clear it.
  bool serial_hold;

  int32_t applied_target_mrad;
  uint8_t capture_generation;
  bool capture_applied;
  bool target_active;

  PendingImpedancePair pending;

  // Most recent computation, for reporting only. Written by the control task.
  float last_position_error;
  float last_torque_cmd;
};

// Zeroes every effort-producing term. Used by disarm, emergency stop, calibration entry,
// mode change, and arming (FR-008, FR-009, FR-041, FR-042).
void impedanceZeroEffort(MotorImpedanceState &s);

// Also discards any in-flight pair, so a partially received command cannot survive.
void impedanceClearPending(MotorImpedanceState &s);

// Full reset for startup: velocity mode, disarmed, zero everything.
void impedanceResetState(MotorImpedanceState &s);

// Stages one half. Returns true when this half completed a valid matched pair, in which
// case the caller applies it. A rejected half latches pair_fault_latched and never
// touches applied state or the command timeout (FR-007a).
bool impedanceStagePosition(MotorImpedanceState &s, const ImpedancePositionHalf &half,
                            uint32_t now_us);
bool impedanceStageDynamics(MotorImpedanceState &s, const ImpedanceDynamicsHalf &half,
                            uint32_t now_us);

// Applies the staged pair atomically. measured_position_mrad is used instead of the
// transmitted target when the capture flag is set, which is what lets a sender enable
// stiffness without inventing a hold target.
void impedanceApplyPair(MotorImpedanceState &s, int32_t measured_position_mrad,
                        uint32_t now_us);

// Serial stand-in for a matched CAN pair: applies the five terms atomically and refreshes
// the command timeout. Does not touch pair_fault_latched.
void impedanceApplySerial(MotorImpedanceState &s, int32_t p_des_mrad, float v_des, float kp,
                          float kd, float t_ff, uint32_t now_us);

// Discards an expired incomplete pair. Call once per control cycle.
void impedanceExpirePending(MotorImpedanceState &s, uint32_t now_us);

// Evaluates the command timeout. Returns true on the transition into timeout so the
// caller can report it from the communications context, never from the control path.
bool impedanceUpdateTimeout(MotorImpedanceState &s, uint32_t now_us);

// The impedance law. Returns the commanded torque in N*m.
float impedanceComputeTorque(MotorImpedanceState &s, float shaft_angle, float shaft_velocity);

// Converts torque to a current setpoint and clamps it, recording the cause and counting
// the rising edge (FR-004).
float impedanceTorqueToCurrent(MotorImpedanceState &s, float torque, float current_limit,
                               float voltage_limit, float phase_resistance);

// Records an effort-limit cause and counts it only on a rising edge.
void impedanceNoteLimit(MotorImpedanceState &s, uint8_t cause_bit, bool active);

// Clears the report latch after communications has published it at least once.
void impedanceClearReportedLimits(MotorImpedanceState &s);
