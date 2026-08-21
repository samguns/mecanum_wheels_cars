#pragma once

// CAN protocol definitions for the MIT impedance control feature.
// Authoritative contract: specs/002-mit-impedance-control/contracts/can-protocol.md
// jetson_xavier/backend/can_frames.py mirrors this file and must stay byte-identical.
//
// The impedance command pair is BIG-ENDIAN. The retained legacy 0x200 velocity frame is
// LITTLE-ENDIAN. These two formats never share a helper; see legacy handling in the sketch.

#include <math.h>
#include <stdint.h>
#include <string.h>

// ---------------------------------------------------------------------------
// Frame identifier bases. Add NodeID (low byte of the configured CAN id).
// ---------------------------------------------------------------------------
static const uint16_t CAN_ID_ESTOP = 0x080;

static const uint16_t CAN_ID_POSITION_M1_BASE = 0x100;
static const uint16_t CAN_ID_POSITION_M2_BASE = 0x110;
static const uint16_t CAN_ID_CONTROL_BASE = 0x120;
static const uint16_t CAN_ID_DYNAMICS_M1_BASE = 0x130;
static const uint16_t CAN_ID_DYNAMICS_M2_BASE = 0x140;

static const uint16_t CAN_ID_STATUS_VELOCITY_BASE = 0x180;
static const uint16_t CAN_ID_STATUS_CURRENT_BASE = 0x190;
static const uint16_t CAN_ID_STATUS_CONFIG_BASE = 0x1A0;
static const uint16_t CAN_ID_STATUS_GAINS_BASE = 0x1B0;
static const uint16_t CAN_ID_STATUS_POSITION_BASE = 0x1C0;
static const uint16_t CAN_ID_STATUS_LIMIT_BASE = 0x1D0;
static const uint16_t CAN_ID_STATUS_APPLIED_M1_BASE = 0x1E0;
static const uint16_t CAN_ID_STATUS_APPLIED_M2_BASE = 0x1F0;

static const uint16_t CAN_ID_LEGACY_COMMAND_BASE = 0x200;
static const uint16_t CAN_ID_HEARTBEAT_BASE = 0x700;

// ---------------------------------------------------------------------------
// Control frame command bytes (0x120 + NodeID)
// ---------------------------------------------------------------------------
static const uint8_t CAN_CTRL_ARM = 0x10;
static const uint8_t CAN_CTRL_MODE = 0x11;
// 0x12 is reserved as a forbidden bandwidth-write probe. It MUST always be rejected:
// no CAN command may ever write bandwidth (FR-021a, SC-009a).
static const uint8_t CAN_CTRL_FORBIDDEN_BANDWIDTH = 0x12;

static const uint8_t CAN_CTRL_FLAG_ENABLE = 0x01;
static const uint8_t CAN_CTRL_FLAG_BRAKE = 0x02;
static const uint8_t CAN_CTRL_FLAG_CLEAR_FAULT = 0x04;
static const uint8_t CAN_CTRL_FLAG_ESTOP = 0x08;

static const uint8_t CAN_CTRL_SELECT_BOTH = 0x00;
static const uint8_t CAN_CTRL_SELECT_M1 = 0x01;
static const uint8_t CAN_CTRL_SELECT_M2 = 0x02;

// ---------------------------------------------------------------------------
// Motion modes. Persisted in RobotConfig and reported in status byte 0.
// ---------------------------------------------------------------------------
static const uint8_t MOTION_MODE_VELOCITY = 0x00;
static const uint8_t MOTION_MODE_IMPEDANCE = 0x01;

// ---------------------------------------------------------------------------
// Dynamics field ranges. Scaled to this drive (12 V bus, 3.0 A per-motor limit,
// 50 rad/s velocity limit), NOT to a Mini Cheetah leg actuator.
// Torque-derived ranges are PROVISIONAL until the torque constant is measured (T043).
// ---------------------------------------------------------------------------
static const float IMPEDANCE_V_MIN = -45.0f;
static const float IMPEDANCE_V_MAX = 45.0f;
static const float IMPEDANCE_KP_MIN = 0.0f;
static const float IMPEDANCE_KP_MAX = 50.0f;
static const float IMPEDANCE_KD_MIN = 0.0f;
static const float IMPEDANCE_KD_MAX = 1.0f;
static const float IMPEDANCE_TFF_MIN = -0.5f;
static const float IMPEDANCE_TFF_MAX = 0.5f;
// 100 shaft revolutions in milliradians. Matches the SC-013 signed-32-bit position contract.
static const int32_t IMPEDANCE_P_MRAD_LIMIT = 628319;

// A matched pair must complete inside this window or it is discarded and reported.
static const uint32_t IMPEDANCE_PAIR_MATCH_WINDOW_US = 5000;

// ---------------------------------------------------------------------------
// Status bit layouts
// ---------------------------------------------------------------------------
// 0x1A0 byte 0
static const uint8_t CAN_CFG0_M1_MODE_SHIFT = 0;
static const uint8_t CAN_CFG0_M2_MODE_SHIFT = 2;
static const uint8_t CAN_CFG0_M1_ARMED = 1 << 4;
static const uint8_t CAN_CFG0_M2_ARMED = 1 << 5;
static const uint8_t CAN_CFG0_M1_TIMED_OUT = 1 << 6;
static const uint8_t CAN_CFG0_M2_TIMED_OUT = 1 << 7;
// 0x1A0 byte 1
static const uint8_t CAN_CFG1_OVERRUN_FAULT = 1 << 0;
static const uint8_t CAN_CFG1_BANDWIDTH_CLAMPED = 1 << 1;
static const uint8_t CAN_CFG1_CALIBRATION_REQUIRED = 1 << 2;
static const uint8_t CAN_CFG1_TIMING_FAULT = 1 << 3;
static const uint8_t CAN_CFG1_M1_PAIR_FAULT = 1 << 4;
static const uint8_t CAN_CFG1_M2_PAIR_FAULT = 1 << 5;

// 0x1D0 per-motor limit cause bits
static const uint8_t LIMIT_CAUSE_CURRENT = 1 << 0;
static const uint8_t LIMIT_CAUSE_OUTPUT_VOLTAGE = 1 << 1;
static const uint8_t LIMIT_CAUSE_BUS_VOLTAGE = 1 << 2;

// 0x1E0 / 0x1F0 byte 6
static const uint8_t CAN_APPLIED_CAPTURE_APPLIED = 1 << 0;
static const uint8_t CAN_APPLIED_TARGET_ACTIVE = 1 << 1;

// ---------------------------------------------------------------------------
// Scalar mapping (MIT-style), shared by every 12-bit dynamics field
// ---------------------------------------------------------------------------
inline uint32_t canFloatToUint(float x, float lo, float hi, uint8_t bits) {
  const float span = hi - lo;
  const uint32_t full = (bits >= 32) ? 0xFFFFFFFFu : ((1UL << bits) - 1UL);
  if (x < lo) x = lo;
  if (x > hi) x = hi;
  if (span <= 0.0f) return 0;
  float scaled = (x - lo) * ((float)full / span);
  if (scaled < 0.0f) scaled = 0.0f;
  if (scaled > (float)full) scaled = (float)full;
  return (uint32_t)lroundf(scaled);
}

inline float canUintToFloat(uint32_t u, float lo, float hi, uint8_t bits) {
  const uint32_t full = (bits >= 32) ? 0xFFFFFFFFu : ((1UL << bits) - 1UL);
  if (full == 0) return lo;
  if (u > full) u = full;
  return (float)u * ((hi - lo) / (float)full) + lo;
}

// Accumulated shaft angle is carried as signed int32 milliradians so continuous
// rotation stays representable (about +/-341,000 revolutions at 0.001 rad).
inline int32_t canRadToMrad(float radians) { return (int32_t)lroundf(radians * 1000.0f); }
inline float canMradToRad(int32_t mrad) { return (float)mrad * 0.001f; }

// ---------------------------------------------------------------------------
// Big-endian primitives (impedance pair and position status only)
// ---------------------------------------------------------------------------
inline void canPackInt32BE(uint8_t *out, int32_t value) {
  const uint32_t u = (uint32_t)value;
  out[0] = (uint8_t)((u >> 24) & 0xFF);
  out[1] = (uint8_t)((u >> 16) & 0xFF);
  out[2] = (uint8_t)((u >> 8) & 0xFF);
  out[3] = (uint8_t)(u & 0xFF);
}

inline int32_t canUnpackInt32BE(const uint8_t *in) {
  const uint32_t u = ((uint32_t)in[0] << 24) | ((uint32_t)in[1] << 16) |
                     ((uint32_t)in[2] << 8) | (uint32_t)in[3];
  return (int32_t)u;
}

// Little-endian primitives (status frames that follow the existing convention)
inline void canPackUint16LE(uint8_t *out, uint16_t value) {
  out[0] = (uint8_t)(value & 0xFF);
  out[1] = (uint8_t)((value >> 8) & 0xFF);
}

inline uint16_t canUnpackUint16LE(const uint8_t *in) {
  return (uint16_t)((uint16_t)in[0] | ((uint16_t)in[1] << 8));
}

// ---------------------------------------------------------------------------
// Impedance command halves
// ---------------------------------------------------------------------------
struct ImpedancePositionHalf {
  int32_t p_des_mrad;
  uint8_t seq;
};

struct ImpedanceDynamicsHalf {
  float v_des;
  float kp;
  float kd;
  float t_ff;
  uint8_t seq;
  bool capture_current_position;
};

// Position half: bytes 0-3 int32 BE milliradians, byte 4 sequence, bytes 5-7 reserved zero.
inline void canPackPositionHalf(uint8_t *data, const ImpedancePositionHalf &half) {
  memset(data, 0, 8);
  canPackInt32BE(&data[0], half.p_des_mrad);
  data[4] = half.seq;
}

// Returns false when the frame is malformed. A rejected half must never touch active state.
inline bool canUnpackPositionHalf(const uint8_t *data, uint8_t dlc, ImpedancePositionHalf &half) {
  if (dlc != 8) return false;
  if (data[5] != 0 || data[6] != 0 || data[7] != 0) return false;
  half.p_des_mrad = canUnpackInt32BE(&data[0]);
  half.seq = data[4];
  return true;
}

// Dynamics half: four 12-bit fields in bytes 0-5, byte 6 sequence, byte 7 flags.
inline void canPackDynamicsHalf(uint8_t *data, const ImpedanceDynamicsHalf &half) {
  memset(data, 0, 8);
  const uint32_t v = canFloatToUint(half.v_des, IMPEDANCE_V_MIN, IMPEDANCE_V_MAX, 12);
  const uint32_t kp = canFloatToUint(half.kp, IMPEDANCE_KP_MIN, IMPEDANCE_KP_MAX, 12);
  const uint32_t kd = canFloatToUint(half.kd, IMPEDANCE_KD_MIN, IMPEDANCE_KD_MAX, 12);
  const uint32_t tff = canFloatToUint(half.t_ff, IMPEDANCE_TFF_MIN, IMPEDANCE_TFF_MAX, 12);

  data[0] = (uint8_t)((v >> 4) & 0xFF);
  data[1] = (uint8_t)(((v << 4) & 0xF0) | ((kp >> 8) & 0x0F));
  data[2] = (uint8_t)(kp & 0xFF);
  data[3] = (uint8_t)((kd >> 4) & 0xFF);
  data[4] = (uint8_t)(((kd << 4) & 0xF0) | ((tff >> 8) & 0x0F));
  data[5] = (uint8_t)(tff & 0xFF);
  data[6] = half.seq;
  data[7] = half.capture_current_position ? 0x01 : 0x00;
}

inline bool canUnpackDynamicsHalf(const uint8_t *data, uint8_t dlc, ImpedanceDynamicsHalf &half) {
  if (dlc != 8) return false;
  if ((data[7] & 0xFE) != 0) return false;  // bits 1-7 reserved, must be zero

  const uint32_t v = ((uint32_t)data[0] << 4) | ((uint32_t)data[1] >> 4);
  const uint32_t kp = (((uint32_t)data[1] & 0x0F) << 8) | (uint32_t)data[2];
  const uint32_t kd = ((uint32_t)data[3] << 4) | ((uint32_t)data[4] >> 4);
  const uint32_t tff = (((uint32_t)data[4] & 0x0F) << 8) | (uint32_t)data[5];

  half.v_des = canUintToFloat(v, IMPEDANCE_V_MIN, IMPEDANCE_V_MAX, 12);
  half.kp = canUintToFloat(kp, IMPEDANCE_KP_MIN, IMPEDANCE_KP_MAX, 12);
  half.kd = canUintToFloat(kd, IMPEDANCE_KD_MIN, IMPEDANCE_KD_MAX, 12);
  half.t_ff = canUintToFloat(tff, IMPEDANCE_TFF_MIN, IMPEDANCE_TFF_MAX, 12);
  half.seq = data[6];
  half.capture_current_position = (data[7] & 0x01) != 0;
  return true;
}

// ---------------------------------------------------------------------------
// Control frame (0x120 + NodeID)
// ---------------------------------------------------------------------------
struct CanControlFrame {
  uint8_t command;
  uint8_t flags;
  uint8_t selector;
  uint8_t mode;
  uint8_t seq;
};

inline void canPackControlFrame(uint8_t *data, const CanControlFrame &frame) {
  memset(data, 0, 8);
  data[0] = frame.command;
  data[1] = frame.flags;
  data[2] = frame.selector;
  data[3] = frame.mode;
  data[4] = frame.seq;
}

inline bool canUnpackControlFrame(const uint8_t *data, uint8_t dlc, CanControlFrame &frame) {
  if (dlc != 8) return false;
  if (data[5] != 0 || data[6] != 0 || data[7] != 0) return false;
  frame.command = data[0];
  frame.flags = data[1];
  frame.selector = data[2];
  frame.mode = data[3];
  frame.seq = data[4];
  return true;
}

// ---------------------------------------------------------------------------
// Status frames
// ---------------------------------------------------------------------------
struct CanConfigStatus {
  uint8_t motor_mode[2];
  bool armed[2];
  bool timed_out[2];
  bool overrun_fault;
  bool bandwidth_clamped;
  bool calibration_required;
  bool timing_fault;
  bool pair_fault[2];
  uint16_t active_bandwidth_hz;
  uint16_t requested_bandwidth_hz;
  uint32_t carrier_hz;
};

inline void canPackConfigStatus(uint8_t *data, const CanConfigStatus &s) {
  memset(data, 0, 8);
  uint8_t b0 = 0;
  b0 |= (uint8_t)((s.motor_mode[0] & 0x03) << CAN_CFG0_M1_MODE_SHIFT);
  b0 |= (uint8_t)((s.motor_mode[1] & 0x03) << CAN_CFG0_M2_MODE_SHIFT);
  if (s.armed[0]) b0 |= CAN_CFG0_M1_ARMED;
  if (s.armed[1]) b0 |= CAN_CFG0_M2_ARMED;
  if (s.timed_out[0]) b0 |= CAN_CFG0_M1_TIMED_OUT;
  if (s.timed_out[1]) b0 |= CAN_CFG0_M2_TIMED_OUT;

  uint8_t b1 = 0;
  if (s.overrun_fault) b1 |= CAN_CFG1_OVERRUN_FAULT;
  if (s.bandwidth_clamped) b1 |= CAN_CFG1_BANDWIDTH_CLAMPED;
  if (s.calibration_required) b1 |= CAN_CFG1_CALIBRATION_REQUIRED;
  if (s.timing_fault) b1 |= CAN_CFG1_TIMING_FAULT;
  if (s.pair_fault[0]) b1 |= CAN_CFG1_M1_PAIR_FAULT;
  if (s.pair_fault[1]) b1 |= CAN_CFG1_M2_PAIR_FAULT;

  data[0] = b0;
  data[1] = b1;
  canPackUint16LE(&data[2], s.active_bandwidth_hz);
  canPackUint16LE(&data[4], s.requested_bandwidth_hz);
  canPackUint16LE(&data[6], (uint16_t)(s.carrier_hz / 10u));
}

struct CanAppliedTargetStatus {
  int32_t applied_target_mrad;
  uint8_t last_pair_seq;
  uint8_t capture_generation;
  bool capture_applied;
  bool target_active;
};

inline void canPackAppliedTargetStatus(uint8_t *data, const CanAppliedTargetStatus &s) {
  memset(data, 0, 8);
  canPackInt32BE(&data[0], s.applied_target_mrad);
  data[4] = s.last_pair_seq;
  data[5] = s.capture_generation;
  uint8_t flags = 0;
  if (s.capture_applied) flags |= CAN_APPLIED_CAPTURE_APPLIED;
  if (s.target_active) flags |= CAN_APPLIED_TARGET_ACTIVE;
  data[6] = flags;
}

inline bool canUnpackAppliedTargetStatus(const uint8_t *data, uint8_t dlc,
                                         CanAppliedTargetStatus &s) {
  if (dlc != 8) return false;
  s.applied_target_mrad = canUnpackInt32BE(&data[0]);
  s.last_pair_seq = data[4];
  s.capture_generation = data[5];
  s.capture_applied = (data[6] & CAN_APPLIED_CAPTURE_APPLIED) != 0;
  s.target_active = (data[6] & CAN_APPLIED_TARGET_ACTIVE) != 0;
  return true;
}

// Applied gains status (0x1B0): stiffness x100, damping x10000, little-endian.
inline void canPackGainsStatus(uint8_t *data, float kp1, float kd1, float kp2, float kd2) {
  memset(data, 0, 8);
  canPackUint16LE(&data[0], (uint16_t)lroundf(kp1 * 100.0f));
  canPackUint16LE(&data[2], (uint16_t)lroundf(kd1 * 10000.0f));
  canPackUint16LE(&data[4], (uint16_t)lroundf(kp2 * 100.0f));
  canPackUint16LE(&data[6], (uint16_t)lroundf(kd2 * 10000.0f));
}

// Measured accumulated positions (0x1C0): two int32 BE milliradians.
inline void canPackPositionStatus(uint8_t *data, int32_t m1_mrad, int32_t m2_mrad) {
  canPackInt32BE(&data[0], m1_mrad);
  canPackInt32BE(&data[4], m2_mrad);
}

// Effort-limit causes (0x1D0).
inline void canPackLimitStatus(uint8_t *data, uint8_t cause_m1, uint8_t cause_m2,
                               uint16_t count_m1, uint16_t count_m2) {
  memset(data, 0, 8);
  data[0] = cause_m1;
  data[1] = cause_m2;
  canPackUint16LE(&data[2], count_m1);
  canPackUint16LE(&data[4], count_m2);
}
