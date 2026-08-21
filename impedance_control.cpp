#include "impedance_control.h"

void impedanceZeroEffort(MotorImpedanceState &s) {
  s.p_des_mrad = 0;
  s.p_des = 0.0f;
  s.v_des = 0.0f;
  s.kp = 0.0f;
  s.kd = 0.0f;
  s.t_ff = 0.0f;
  s.velocity_target = 0.0f;
  s.target_active = false;
  s.serial_hold = false;
  s.last_position_error = 0.0f;
  s.last_torque_cmd = 0.0f;
}

void impedanceClearPending(MotorImpedanceState &s) {
  s.pending.has_position = false;
  s.pending.has_dynamics = false;
  s.pending.position_us = 0;
  s.pending.dynamics_us = 0;
}

void impedanceResetState(MotorImpedanceState &s) {
  s = MotorImpedanceState();
  s.mode = MOTION_MODE_VELOCITY;
  s.armed = false;
  s.timed_out = false;
  impedanceZeroEffort(s);
  impedanceClearPending(s);
}

static bool pairComplete(const MotorImpedanceState &s) {
  return s.pending.has_position && s.pending.has_dynamics &&
         s.pending.position.seq == s.pending.dynamics.seq;
}

// A newer sequence supersedes an incomplete older pair, so a lost half cannot block the
// stream forever. Sequence equality is exact: 255 does not match 0 (no wrap tolerance).
static void supersedeIfStale(MotorImpedanceState &s, uint8_t incoming_seq) {
  const bool have_other =
      (s.pending.has_position && s.pending.position.seq != incoming_seq) ||
      (s.pending.has_dynamics && s.pending.dynamics.seq != incoming_seq);
  if (have_other) {
    s.pair_fault_latched = true;
    impedanceClearPending(s);
  }
}

bool impedanceStagePosition(MotorImpedanceState &s, const ImpedancePositionHalf &half,
                            uint32_t now_us) {
  supersedeIfStale(s, half.seq);
  s.pending.position = half;
  s.pending.has_position = true;
  s.pending.position_us = now_us;
  return pairComplete(s);
}

bool impedanceStageDynamics(MotorImpedanceState &s, const ImpedanceDynamicsHalf &half,
                            uint32_t now_us) {
  supersedeIfStale(s, half.seq);
  s.pending.dynamics = half;
  s.pending.has_dynamics = true;
  s.pending.dynamics_us = now_us;
  return pairComplete(s);
}

void impedanceApplyPair(MotorImpedanceState &s, int32_t measured_position_mrad,
                        uint32_t now_us) {
  if (!pairComplete(s)) return;

  const ImpedanceDynamicsHalf &d = s.pending.dynamics;

  if (d.capture_current_position) {
    // The controller substitutes its own measured accumulated position in the same cycle
    // that applies the pair, then advances the generation so the sender can confirm the
    // handshake completed rather than inferring a target.
    s.p_des_mrad = measured_position_mrad;
    s.capture_generation++;
    s.capture_applied = true;
  } else {
    s.p_des_mrad = s.pending.position.p_des_mrad;
    s.capture_applied = false;
  }

  s.p_des = canMradToRad(s.p_des_mrad);
  s.v_des = d.v_des;
  s.kp = d.kp;
  s.kd = d.kd;
  s.t_ff = d.t_ff;

  s.applied_target_mrad = s.p_des_mrad;
  s.last_applied_seq = d.seq;
  s.has_applied_seq = true;
  s.target_active = true;

  // Only a complete, validated pair refreshes the timeout.
  s.last_command_us = now_us;
  s.timed_out = false;

  impedanceClearPending(s);
}

void impedanceApplySerial(MotorImpedanceState &s, int32_t p_des_mrad, float v_des, float kp,
                          float kd, float t_ff, uint32_t now_us) {
  s.p_des_mrad = p_des_mrad;
  s.p_des = canMradToRad(p_des_mrad);
  s.v_des = v_des;
  s.kp = kp;
  s.kd = kd;
  s.t_ff = t_ff;
  s.applied_target_mrad = p_des_mrad;
  s.capture_applied = false;
  if (s.has_applied_seq) {
    s.last_applied_seq = (uint8_t)(s.last_applied_seq + 1);
  } else {
    s.last_applied_seq = 0;
    s.has_applied_seq = true;
  }
  s.target_active = true;
  s.serial_hold = true;
  s.last_command_us = now_us;
  s.timed_out = false;
  impedanceClearPending(s);
}

void impedanceExpirePending(MotorImpedanceState &s, uint32_t now_us) {
  if (pairComplete(s)) return;

  if (s.pending.has_position &&
      (uint32_t)(now_us - s.pending.position_us) > IMPEDANCE_PAIR_MATCH_WINDOW_US) {
    s.pair_fault_latched = true;
    impedanceClearPending(s);
    return;
  }
  if (s.pending.has_dynamics &&
      (uint32_t)(now_us - s.pending.dynamics_us) > IMPEDANCE_PAIR_MATCH_WINDOW_US) {
    s.pair_fault_latched = true;
    impedanceClearPending(s);
  }
}

bool impedanceUpdateTimeout(MotorImpedanceState &s, uint32_t now_us) {
  if (!s.armed || s.serial_hold) return false;
  const bool expired =
      (uint32_t)(now_us - s.last_command_us) > IMPEDANCE_COMMAND_TIMEOUT_US;

  if (expired && !s.timed_out) {
    s.timed_out = true;
    return true;  // caller reports the transition from the communications context
  }
  if (!expired && s.timed_out) {
    s.timed_out = false;  // automatic recovery, no operator action (FR-029b)
  }
  return false;
}

float impedanceComputeTorque(MotorImpedanceState &s, float shaft_angle, float shaft_velocity) {
  if (!s.armed || s.timed_out) {
    s.last_position_error = 0.0f;
    s.last_torque_cmd = 0.0f;
    return 0.0f;
  }

  float position_error = s.p_des - shaft_angle;
  if (position_error > POSITION_ERROR_LIMIT) position_error = POSITION_ERROR_LIMIT;
  if (position_error < -POSITION_ERROR_LIMIT) position_error = -POSITION_ERROR_LIMIT;

  const float torque =
      s.kp * position_error + s.kd * (s.v_des - shaft_velocity) + s.t_ff;

  s.last_position_error = position_error;
  s.last_torque_cmd = torque;
  return torque;
}

void impedanceNoteLimit(MotorImpedanceState &s, uint8_t cause_bit, bool active) {
  if (active) {
    s.limit_cause |= cause_bit;
    if ((s.limit_was_active & cause_bit) == 0) {
      s.limit_was_active |= cause_bit;
      if (s.limit_event_count < 0xFFFFu) s.limit_event_count++;
    }
  } else {
    s.limit_was_active &= (uint8_t)~cause_bit;
  }
}

float impedanceTorqueToCurrent(MotorImpedanceState &s, float torque, float current_limit,
                               float voltage_limit, float phase_resistance) {
  const float constant = (MOTOR_TORQUE_CONSTANT_NM_PER_A > 1e-6f)
                             ? MOTOR_TORQUE_CONSTANT_NM_PER_A
                             : 1e-6f;
  float current = torque / constant;

  bool current_limited = false;
  if (current > current_limit) {
    current = current_limit;
    current_limited = true;
  } else if (current < -current_limit) {
    current = -current_limit;
    current_limited = true;
  }
  impedanceNoteLimit(s, LIMIT_CAUSE_CURRENT, current_limited);

  bool voltage_limited = false;
  if (phase_resistance > 1e-6f && voltage_limit > 0.0f) {
    const float v_needed = fabsf(current) * phase_resistance;
    if (v_needed > voltage_limit) {
      current *= voltage_limit / v_needed;
      voltage_limited = true;
    }
  }
  impedanceNoteLimit(s, LIMIT_CAUSE_OUTPUT_VOLTAGE, voltage_limited);
  return current;
}

void impedanceClearReportedLimits(MotorImpedanceState &s) { s.limit_cause = 0; }
