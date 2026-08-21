#include <SimpleFOC.h>
#include <Preferences.h>
#include <driver/twai.h>

#include "as5147_fast.h"
#include "can_protocol.h"
#include "foc_timing.h"
#include "impedance_control.h"
#include "serial_records.h"

// #include "imu_helpers.h"

#define RX_PIN 9
#define TX_PIN 10

#if CONFIG_FREERTOS_UNICORE
#define FOC_RUNNING_CORE 0
#define COMMS_RUNNING_CORE 0
#else
#define FOC_RUNNING_CORE 1
// Communications live on the other core so CAN, serial, persistence and telemetry can
// never delay a control cycle (FR-017).
#define COMMS_RUNNING_CORE 0
#endif

Preferences prefs;

// Version 1 layout, retained verbatim so a stored v1 blob can be read field-for-field
// during migration. Never edit this struct: it describes what is already in flash.
struct RobotConfigV1 {
  uint32_t magic;
  uint16_t version;
  uint16_t can_id;
  struct MotorCalibrationV1 {
    uint16_t pole_pairs;
    int8_t sensor_direction;
    uint8_t flags;
    float electrical_offset;
    float phase_resistance;
    float inductance_d;
    float inductance_q;
    float current_bandwidth;
  } motor[2];
  bool calibrated;
};

struct RobotConfig {
  uint32_t magic;
  uint16_t version;
  uint16_t can_id;
  struct MotorCalibration {
    uint16_t pole_pairs;
    int8_t sensor_direction;
    uint8_t flags; // bit 0: alignment, bit 1: characteristics
    float electrical_offset;
    float phase_resistance;
    float inductance_d;
    float inductance_q;
    float current_bandwidth; // superseded by current_bandwidth_hz; kept for compatibility
  } motor[2];
  bool calibrated;

  // Added in version 2.
  uint16_t current_bandwidth_hz;   // the REQUESTED value, never the clamped one
  uint8_t motion_mode[2];          // last accepted per-motor mode, restored disarmed
  uint16_t bus_voltage_min_mv;     // fail-closed undervoltage threshold
  uint16_t bus_voltage_max_mv;     // fail-closed overvoltage threshold
};

static const uint32_t CONFIG_MAGIC = 0x4D43414C; // "MCAL"
static const uint16_t CONFIG_VERSION = 2;

// Provisional bus-voltage protection window, replaced by board-approved measured limits
// before release. Serial-configurable while disarmed via the `V` command.
static const uint16_t DEFAULT_BUS_MIN_MV = 7000;
static const uint16_t DEFAULT_BUS_MAX_MV = 24000;
// Exact superseded provisional pair. It is migrated narrowly so operator-configured windows are
// never overwritten merely because the firmware defaults changed.
static const uint16_t LEGACY_PROVISIONAL_BUS_MIN_MV = 9000;
static const uint16_t LEGACY_PROVISIONAL_BUS_MAX_MV = 14000;

// Forward declarations. Declared here, above every definition, because the Arduino
// prototype generator does not see functions that take user-defined types.
bool saveConfig(const RobotConfig &config);
bool calibrationRecordValid(const RobotConfig::MotorCalibration &record);
void disarmAllMotors();
void focControlCycle();
void focFailClosed();
void applyTimingToMotors();
bool deriveAndReportTiming(uint16_t requested_hz, bool report);
void reportBandwidth();
void TaskComms(void *pvParams);
void commsCycle();
void reportPendingTimeouts();
void updateBusVoltageProtection(float vbus_volts);
void doBandwidth(char *cmd);
void doTiming(char *cmd);
void doQuery(char *cmd);
void doBusIdentity(char *cmd);
void doBusWindow(char *cmd);
void doMotionMode(char *cmd);
void doImpedanceReport(char *cmd);
void doImpedanceApply(char *cmd);
bool impedanceEligible(uint8_t index, const __FlashStringHelper **reason);
void applyMotorController(uint8_t index);
bool serialAbortRequested();
void ackCurrentCommand(bool ok, const __FlashStringHelper *reason);
void pumpSerial();
void emitIdRecord();
void emitCalRecord(uint8_t index);
void emitCfgRecord();
void emitMotorRecord(uint8_t index);
void emitImpRecord(uint8_t index);
void emitTimingRecord();
void emitBusRecord();
void emitAllRecords();
static const uint16_t DEFAULT_CAN_ID = 0x201;
static const float CALIBRATION_VOLTAGE = 4.0f;
static const float ALIGNMENT_VOLTAGES[] = {0.75f, 1.0f};
static const float CALIBRATION_WORKING_CURRENT = 1.0f;
static const float CALIBRATION_ABORT_CURRENT = 1.5f;
static const uint32_t ALIGNMENT_TIMEOUT_MS = 30000;
static const uint32_t CHARACTERIZATION_TIMEOUT_MS = 15000;

static const int PIN_DCBUS_S = 38;      // GPIO38
static float DIVIDER_GAIN = 11.0; // (10+1)/1 = 11
static int volt_samples = 0;
static uint32_t sum_mV = 0;
float current_vbus = 0.0f;
RobotConfig config = {};

// RobotConfig is loaded during setup(), after global construction. These
// placeholders are replaced by applyCalibrationToMotor() before motor.init().
// An uncalibrated unit remains in calibration mode and never enters normal FOC.
// BLDC motor & driver instance
BLDCMotor motor1 = BLDCMotor(1);
BLDCMotor motor2 = BLDCMotor(1);
BLDCDriver3PWM driver1 = BLDCDriver3PWM(25, 32, 26, 33);
BLDCDriver3PWM driver2 = BLDCDriver3PWM(27, 14, 13, 12);

// encoder instance
// First-party reader: SimpleFOC's MagneticSensorSPI costs ~85 us per read on ESP32
// because of a hardcoded 50 us delay, which alone caps the control loop near 4 kHz.
// See as5147_fast.h and research decisions D1/D2.
AS5147Fast sensor1 = AS5147Fast(5);
AS5147Fast sensor2 = AS5147Fast(21);

// inline current sensor instance
// ACS712-05B has the resolution of 0.185mV per Amp
InlineCurrentSense current_sense1 = InlineCurrentSense(0.01, 50.0, 34, 35, NOT_SET);
InlineCurrentSense current_sense2 = InlineCurrentSense(0.01, 50.0, 37, 36, NOT_SET);

// Derived timing configuration and per-motor impedance state. Both motors share one
// timing record (FR-028); the impedance state is per motor.
FocTimingConfig timing_config = {};
MotorImpedanceState motor_state[2] = {};
float foc_max_sustainable_rate_hz = FOC_MAX_SUSTAINABLE_RATE_HZ_PROVISIONAL;
bool timing_started = false;
volatile bool fail_closed_pending = false;
bool bus_voltage_protection_active = false;
static uint32_t bus_voltage_event_count = 0;

enum class ControllerMode : uint8_t { Normal, Calibration };
enum class CalibrationOperation : uint8_t { Idle, AlignmentPending, CharacteristicsPending };
ControllerMode controller_mode = ControllerMode::Calibration;
CalibrationOperation calibration_operation = CalibrationOperation::Idle;
int8_t selected_motor = -1;
RobotConfig::MotorCalibration pending_calibration = {};
bool foc_initialized = false;

// commander communication instance
Commander command = Commander(Serial);
// void doMotion(char* cmd){ command.motion(&motor1, cmd); }
void doMotor1(char* cmd){ if (controller_mode == ControllerMode::Normal) command.motor(&motor1, cmd); else Serial.println(F("CAL motor command rejected")); }
void doMotor2(char* cmd){ if (controller_mode == ControllerMode::Normal) command.motor(&motor2, cmd); else Serial.println(F("CAL motor command rejected")); }

// Motor power is deliberately controlled separately from the velocity target.
// Keeping this in one place makes every disarm path leave the bridge unpowered.
// Index of a motor, so arm and disarm can keep the impedance state in step.
int8_t motorIndexOf(const BLDCMotor &motor) {
  if (&motor == &motor1) return 0;
  if (&motor == &motor2) return 1;
  return -1;
}

void disarmMotor(BLDCMotor &motor) {
  motor.target = 0.0f;
  if (motor.enabled) {
    motor.disable();
  }
  const int8_t index = motorIndexOf(motor);
  if (index >= 0) {
    motor_state[index].armed = false;
    motor_state[index].timed_out = false;
    impedanceZeroEffort(motor_state[index]);
    impedanceClearPending(motor_state[index]);
  }
}

void armMotor(BLDCMotor &motor) {
  // An arm command must never apply a target that was received before disarming.
  motor.target = 0.0f;
  const int8_t index = motorIndexOf(motor);
  if (index >= 0) {
    // Arming always begins from a zero-effort state (FR-009, FR-042).
    impedanceZeroEffort(motor_state[index]);
    impedanceClearPending(motor_state[index]);
    motor_state[index].armed = true;
    motor_state[index].timed_out = false;
    motor_state[index].last_command_us = micros();
  }
  if (!motor.enabled) {
    motor.enable();
  }
}

void disarmAllMotors() {
  disarmMotor(motor1);
  disarmMotor(motor2);
}

void armAllMotors() {
  armMotor(motor1);
  armMotor(motor2);
}

void enterCalibrationMode() {
  controller_mode = ControllerMode::Calibration;
  disarmAllMotors();
  resetCalibrationSession();
}

void failCalibration(const __FlashStringHelper *reason) {
  disarmAllMotors();
  memset(&pending_calibration, 0, sizeof(pending_calibration));
  Serial.print(F("CAL ERROR: "));
  Serial.println(reason);
  calibration_operation = CalibrationOperation::Idle;
  emitFaultRecord("calibration", reason);
}

// Drains the serial input looking for the abort byte.
//
// This is the ONLY way to stop an energised calibration stage on a bench with no CAN bus. A
// calibration stage blocks for seconds inside the command handler, so the normal serial pump is not
// running; this is called from inside the stage's own loops. Any other byte arriving mid-stage is
// discarded, because no command can be honoured while a stage is running anyway.
bool serialAbortRequested() {
  bool aborted = false;
  while (Serial.available() > 0) {
    const int raw = Serial.read();
    if (raw < 0) break;
    if ((uint8_t)raw == SERIAL_ABORT_BYTE) aborted = true;
  }
  return aborted;
}

bool calibrationAbortRequested() {
  twai_message_t message;
  while (twai_receive(&message, 0) == ESP_OK) {
    if (message.identifier == 0x080 ||
        (message.identifier == config.can_id && message.data_length_code >= 2 && (message.data[1] & 0x08))) {
      failCalibration(F("emergency stop"));
      recordBegin("ack");
      recordKeyUint("tag", 0);
      recordKeyToken("cmd", "ABORT");
      recordKeyBool("ok", true);
      recordKeyText("reason", F("can emergency stop"));
      recordEnd();
      return true;
    }
  }

  // Same disarm and reporting path as the CAN abort, so behaviour is identical whichever triggers.
  if (serialAbortRequested()) {
    failCalibration(F("serial abort"));
    recordBegin("ack");
    recordKeyUint("tag", 0);
    recordKeyToken("cmd", "ABORT");
    recordKeyBool("ok", true);
    recordKeyText("reason", F(""));
    recordEnd();
    return true;
  }
  return false;
}

float calibrationCurrent(uint8_t index, float electricalAngle) {
  return fabsf(currentSenseByIndex(index).getDCCurrent(electricalAngle));
}

bool calibrationGuard(uint8_t index, BLDCMotor &motor, uint32_t startedAt, uint32_t timeoutMs) {
  if (calibrationAbortRequested()) return false;
  if (millis() - startedAt > timeoutMs) {
    failCalibration(F("timeout"));
    return false;
  }
  float measuredCurrent = calibrationCurrent(index, motor.electricalAngle());
  if (measuredCurrent >= CALIBRATION_ABORT_CURRENT) {
    failCalibration(F("over-current abort"));
    return false;
  }
  if (measuredCurrent >= CALIBRATION_WORKING_CURRENT) {
    failCalibration(F("working-current limit"));
    return false;
  }
  return true;
}

bool runAlignmentCalibration(uint8_t index) {
  BLDCMotor &motor = motorByIndex(index);
  uint32_t startedAt = millis();
  disarmAllMotors();
  motor.enable();
  motor.voltage_limit = CALIBRATION_VOLTAGE;
  float movement = 0.0f;
  float selectedVoltage = 0.0f;
  const float electricalTravel = 6.0f * _PI;
  const uint16_t sweepSteps = 1885; // 0.01 rad increments, matching the SimpleFOC reference example.
  emitCalibrationProgress(index + 1, "align", 0, true);

  // Increase torque only when a lower safe excitation cannot move the rotor.
  // Every level remains protected by the 1.0 A working and 1.5 A abort guards.
  for (uint8_t level = 0; level < sizeof(ALIGNMENT_VOLTAGES) / sizeof(ALIGNMENT_VOLTAGES[0]); ++level) {
    float voltage = ALIGNMENT_VOLTAGES[level];
    Serial.print(F("CAL alignment voltage [V]: ")); Serial.println(voltage, 2);
  // Keep the selected sensor updated throughout the sweep. getAngle() only
  // includes full rotations that were observed by update(), so this is vital
  // for low pole-pair motors that turn more than once during the test.
    if (index == 0) sensor1.update(); else sensor2.update();
    motor.setPhaseVoltage(voltage, 0.0f, 0.0f);
    delay(1000);
    if (!calibrationGuard(index, motor, startedAt, ALIGNMENT_TIMEOUT_MS)) return false;
    float startAngle = index == 0 ? sensor1.getAngle() : sensor2.getAngle();
    for (uint16_t step = 0; step <= sweepSteps; ++step) {
      float angle = electricalTravel * step / sweepSteps;
      if (index == 0) sensor1.update(); else sensor2.update();
      motor.setPhaseVoltage(voltage, 0.0f, normalizeAngle(angle));
      delay(1);
      if (!calibrationGuard(index, motor, startedAt, ALIGNMENT_TIMEOUT_MS)) return false;
      // Ten-percent updates keep an energised blocked command visible without flooding serial.
      if (step % (sweepSteps / 10) == 0) {
        emitCalibrationProgress(index + 1, "align", (uint8_t)((uint32_t)step * 100 / sweepSteps), true);
      }
    }
    delay(500);
    if (!calibrationGuard(index, motor, startedAt, ALIGNMENT_TIMEOUT_MS)) return false;
    if (index == 0) sensor1.update(); else sensor2.update();
    movement = (index == 0 ? sensor1.getAngle() : sensor2.getAngle()) - startAngle;
    if (fabsf(movement) >= 0.1f) { selectedVoltage = voltage; break; }
    motor.setPhaseVoltage(0.0f, 0.0f, 0.0f);
    delay(100);
  }
  if (selectedVoltage == 0.0f) { failCalibration(F("sensor did not move")); return false; }
  int polePairs = (int)lroundf(electricalTravel / fabsf(movement));
  if (polePairs < 1 || polePairs > 64) { failCalibration(F("invalid pole pairs")); return false; }
  // Both motors on a node are the same spec. Motor 1 (right wheel) is the reference: if it
  // already has an accepted alignment, adopt its pole-pair count rather than a sweep that
  // rounded to a neighbour (Rear Left previously stored 6 against Rear Right's 7).
  if (index == 1 && (config.motor[0].flags & 0x01) && config.motor[0].pole_pairs >= 1 &&
      config.motor[0].pole_pairs <= 64 && polePairs != (int)config.motor[0].pole_pairs) {
    Serial.print(F("CAL alignment pole pairs measured "));
    Serial.print(polePairs);
    Serial.print(F(", adopting motor 1 reference "));
    Serial.println(config.motor[0].pole_pairs);
    polePairs = (int)config.motor[0].pole_pairs;
  }
  motor.pole_pairs = polePairs;
  motor.sensor_direction = movement > 0.0f ? Direction::CW : Direction::CCW;
  // Align at -90 electrical degrees and record the corresponding sensor offset.
  motor.setPhaseVoltage(selectedVoltage, 0.0f, _3PI_2);
  delay(700);
  if (!calibrationGuard(index, motor, startedAt, ALIGNMENT_TIMEOUT_MS)) return false;
  sensor1.update(); sensor2.update();
  float mechanical = index == 0 ? sensor1.getMechanicalAngle() : sensor2.getMechanicalAngle();
  // Keep any already-accepted characterisation. Alignment used to zero the whole record,
  // which made a successful accept look out of range (R=0, L=0).
  pending_calibration = config.motor[index];
  pending_calibration.pole_pairs = polePairs;
  pending_calibration.sensor_direction = motor.sensor_direction == Direction::CW ? 1 : -1;
  pending_calibration.electrical_offset = normalizeAngle((float)(pending_calibration.sensor_direction * polePairs) * mechanical - _3PI_2);
  pending_calibration.flags |= 0x01;
  motor.setPhaseVoltage(0.0f, 0.0f, 0.0f);
  disarmAllMotors();
  calibration_operation = CalibrationOperation::AlignmentPending;
  emitCalibrationProgress(index + 1, "align", 100, false);
  emitAlignmentPending(index + 1, pending_calibration.pole_pairs,
                       pending_calibration.sensor_direction, pending_calibration.electrical_offset);
  return true;
}

bool runCharacteristicsCalibration(uint8_t index) {
  BLDCMotor &motor = motorByIndex(index);
  uint32_t startedAt = millis();
  disarmAllMotors();
  motor.enable();
  motor.voltage_limit = CALIBRATION_VOLTAGE;
  float angle = motor.electricalAngle();
  // Guarded resistance ramp: unlike SimpleFOC's blocking helper, every step observes current and estop.
  motor.setPhaseVoltage(0.0f, 0.0f, angle);
  delay(100);
  float zeroCurrent = calibrationCurrent(index, angle);
  float testVoltage = 0.0f;
  float testCurrent = 0.0f;
  emitCalibrationProgress(index + 1, "charac", 0, true);
  uint8_t lastRampPct = 0;
  // Find the strongest useful test voltage without deliberately crossing the
  // 1 A working-current limit. A fixed 4 V test is unsafe for low-resistance motors.
  for (float voltage = 0.25f; voltage <= CALIBRATION_VOLTAGE; voltage += 0.05f) {
    motor.setPhaseVoltage(voltage, 0.0f, angle);
    delay(5);
    if (calibrationAbortRequested()) return false;
    if (millis() - startedAt > CHARACTERIZATION_TIMEOUT_MS) { failCalibration(F("timeout")); return false; }
    float measuredCurrent = calibrationCurrent(index, angle);
    if (measuredCurrent >= CALIBRATION_ABORT_CURRENT) { failCalibration(F("over-current abort")); return false; }
    if (measuredCurrent >= CALIBRATION_WORKING_CURRENT) break;
    float netCurrent = measuredCurrent - zeroCurrent;
    if (netCurrent >= 0.2f) {
      testVoltage = voltage;
      testCurrent = netCurrent;
    }
    const uint8_t pct = (uint8_t)(5 + (voltage / CALIBRATION_VOLTAGE) * 65.0f);
    if (pct >= lastRampPct + 10) {
      emitCalibrationProgress(index + 1, "charac", pct, true);
      lastRampPct = pct;
    }
  }
  motor.setPhaseVoltage(0.0f, 0.0f, angle);
  if (testVoltage <= 0.0f || testCurrent < 0.2f) { failCalibration(F("current too low")); return false; }
  Serial.print(F("CAL characterization voltage [V]: ")); Serial.println(testVoltage, 2);
  float resistance = testVoltage / (1.5f * testCurrent);
  if (!finiteInRange(resistance, 0.01f, 100.0f)) { failCalibration(F("invalid resistance")); return false; }
  float inductance[2] = {0.0f, 0.0f};
  for (uint8_t axis = 0; axis < 2; ++axis) {
    float axisAngle = normalizeAngle(angle + (axis ? _PI_2 : 0.0f));
    float average = 0.0f;
    for (uint8_t sample = 0; sample < 12; ++sample) {
      motor.setPhaseVoltage(0.0f, 0.0f, axisAngle);
      delayMicroseconds(200);
      float base = calibrationCurrent(index, axisAngle);
      motor.setPhaseVoltage(testVoltage, 0.0f, axisAngle);
      uint32_t pulseStart = micros();
      delayMicroseconds(200);
      float current = calibrationCurrent(index, axisAngle) - base;
      uint32_t pulseEnd = micros();
      motor.setPhaseVoltage(0.0f, 0.0f, axisAngle);
      if (!calibrationGuard(index, motor, startedAt, CHARACTERIZATION_TIMEOUT_MS)) return false;
      if ((sample % 4) == 3) {
        emitCalibrationProgress(index + 1, "charac",
                                (uint8_t)(70 + axis * 15 + ((sample + 1) * 15 / 12)), true);
      }
      if (current <= 0.0f || current >= CALIBRATION_ABORT_CURRENT) { failCalibration(F("invalid inductance current")); return false; }
      float dt = (pulseEnd - pulseStart) / 1000000.0f;
      float denominator = testVoltage - resistance * current;
      if (denominator <= 0.0f) { failCalibration(F("invalid inductance sample")); return false; }
      average += fabsf(-(resistance * dt) / logf(denominator / testVoltage)) / 1.5f;
      delay(2);
    }
    inductance[axis] = average / 12.0f;
    if (!finiteInRange(inductance[axis], 0.000001f, 0.1f)) { failCalibration(F("invalid inductance")); return false; }
  }
  pending_calibration = config.motor[index];
  pending_calibration.phase_resistance = resistance;
  pending_calibration.inductance_d = inductance[0];
  pending_calibration.inductance_q = inductance[1];
  pending_calibration.current_bandwidth = (float)config.current_bandwidth_hz;
  pending_calibration.flags |= 0x02;
  disarmAllMotors();
  calibration_operation = CalibrationOperation::CharacteristicsPending;
  emitCalibrationProgress(index + 1, "charac", 100, false);
  emitCharacteristicsPending(index + 1, pending_calibration.phase_resistance,
                             pending_calibration.inductance_d, pending_calibration.inductance_q);
  return true;
}

void printCalibrationRecord(const __FlashStringHelper *label, const RobotConfig::MotorCalibration &record) {
  Serial.print(F("CAL ")); Serial.println(label);
  Serial.print(F("  alignment: ")); Serial.println((record.flags & 0x01) ? F("confirmed") : F("not confirmed"));
  Serial.print(F("  characteristics: ")); Serial.println((record.flags & 0x02) ? F("confirmed") : F("not confirmed"));
  Serial.print(F("  pole pairs: ")); Serial.println(record.pole_pairs);
  Serial.print(F("  sensor direction: ")); Serial.println(record.sensor_direction > 0 ? F("CW") : record.sensor_direction < 0 ? F("CCW") : F("unset"));
  Serial.print(F("  electrical offset [rad]: ")); Serial.println(record.electrical_offset, 6);
  Serial.print(F("  phase resistance [ohm]: ")); Serial.println(record.phase_resistance, 6);
  Serial.print(F("  inductance D [H]: ")); Serial.println(record.inductance_d, 9);
  Serial.print(F("  inductance Q [H]: ")); Serial.println(record.inductance_q, 9);
  Serial.print(F("  current bandwidth [Hz]: ")); Serial.println(record.current_bandwidth, 2);
  if (finiteInRange(record.phase_resistance, 0.01f, 100.0f) &&
      finiteInRange(record.inductance_d, 0.000001f, 0.1f) &&
      finiteInRange(record.inductance_q, 0.000001f, 0.1f) &&
      finiteInRange(record.current_bandwidth, 10.0f, 1000.0f)) {
    float omega = _2PI * record.current_bandwidth;
    Serial.print(F("  PID current D: P=")); Serial.print(record.inductance_d * omega, 6);
    Serial.print(F(" I=")); Serial.println(record.phase_resistance * omega, 6);
    Serial.print(F("  PID current Q: P=")); Serial.print(record.inductance_q * omega, 6);
    Serial.print(F(" I=")); Serial.println(record.phase_resistance * omega, 6);
    Serial.print(F("  current LPF Tf [s]: ")); Serial.println(1.0f / (omega * 5.0f), 8);
  }
}

void printCalibrationStatus() {
  Serial.print(F("CAL MODE: ")); Serial.println(controller_mode == ControllerMode::Normal ? F("NORMAL (disarmed)") : F("CALIBRATION"));
  for (uint8_t i = 0; i < 2; ++i) {
    if (i == 0) printCalibrationRecord(F("M1 SAVED"), config.motor[i]);
    else printCalibrationRecord(F("M2 SAVED"), config.motor[i]);
  }
  if ((calibration_operation == CalibrationOperation::AlignmentPending || calibration_operation == CalibrationOperation::CharacteristicsPending) && selected_motor >= 0) {
    Serial.print(F("CAL PENDING RESULT FOR M")); Serial.println(selected_motor + 1);
    printCalibrationRecord(F("PENDING"), pending_calibration);
  }
  Serial.println(F("CAL LIMITS: 4.0V, 1.0A working, 1.5A abort"));
}

void doCalibration(char* cmd) {
  char action = cmd[0];
  if (action == 'P') { printCalibrationStatus(); return; }
  if (action == '1' || action == '2') { enterCalibrationMode(); selected_motor = action - '1'; Serial.println(F("CAL selected motor")); Serial.println(selected_motor + 1); ackCurrentCommand(true, F("")); return; }
  if (action == 'X') { enterCalibrationMode(); Serial.println(F("CAL cancelled")); ackCurrentCommand(true, F("")); return; }
  if (action == 'E') {
    if (configIsComplete(config)) {
      applyCalibrationToMotor(0); applyCalibrationToMotor(1);
      if (!foc_initialized) { motor1.initFOC(); motor2.initFOC(); foc_initialized = true; }
      controller_mode = ControllerMode::Normal; selected_motor = -1; disarmAllMotors();
      Serial.println(F("CAL normal mode (disarmed)"));
      ackCurrentCommand(true, F(""));
    } else { Serial.println(F("CAL incomplete")); ackCurrentCommand(false, F("calibration incomplete")); }
    return;
  }
  if (controller_mode != ControllerMode::Calibration || selected_motor < 0) { Serial.println(F("CAL select motor with C1 or C2")); ackCurrentCommand(false, F("select motor with C1 or C2")); return; }
  uint8_t index = (uint8_t)selected_motor;
  if (action == 'A') { if (runAlignmentCalibration(index)) { Serial.print(F("CAL PENDING ALIGN M")); Serial.println(index + 1); ackCurrentCommand(true, F("")); } else ackCurrentCommand(false, F("calibration stage failed")); return; }
  if (action == 'M') { if (!(config.motor[index].flags & 0x01)) { Serial.println(F("CAL confirm alignment first")); ackCurrentCommand(false, F("confirm alignment first")); return; } if (runCharacteristicsCalibration(index)) { Serial.print(F("CAL PENDING CHARACTERISTICS M")); Serial.println(index + 1); ackCurrentCommand(true, F("")); } else ackCurrentCommand(false, F("calibration stage failed")); return; }
  if (action == 'N') { resetCalibrationSession(); disarmAllMotors(); Serial.println(F("CAL pending result rejected")); ackCurrentCommand(true, F("")); return; }
  if (action == 'Y') {
    if (calibration_operation == CalibrationOperation::AlignmentPending || calibration_operation == CalibrationOperation::CharacteristicsPending) {
      config.motor[index] = pending_calibration;
      applyCalibrationToMotor(index);
      if (calibration_operation == CalibrationOperation::CharacteristicsPending && motorByIndex(index).tuneCurrentController(config.motor[index].current_bandwidth) != 0) {
        failCalibration(F("current PID tuning failed")); return;
      }
      configIsComplete(config);
      if (!saveConfig(config)) { failCalibration(F("persistence failed")); ackCurrentCommand(false, F("persistence failed")); return; }
      resetCalibrationSession(); Serial.println(F("CAL result saved")); ackCurrentCommand(true, F(""));
    } else { Serial.println(F("CAL no pending result")); ackCurrentCommand(false, F("no pending result")); }
    return;
  }
  Serial.println(F("CAL commands: C, C1, C2, CA, CM, CY, CN, CX, CE"));
}

// Bandwidth is writable only here. No CAN command may ever set it (FR-021a, SC-009a).
void doBandwidth(char *cmd) {
  if (cmd == nullptr || cmd[0] == '\0') {
    reportBandwidth();
    return;
  }

  if (motor_state[0].armed || motor_state[1].armed) {
    Serial.println(F("BW REFUSED: disarm both motors first (D0)"));
    ackCurrentCommand(false, F("disarm both motors first"));
    return;
  }

  uint16_t requested = 0;
  if (!focParseBandwidthHz(cmd, requested)) {
    Serial.print(F("BW REFUSED: '"));
    Serial.print(cmd);
    Serial.print(F("' is not an integer in ["));
    Serial.print(FOC_BANDWIDTH_MIN_HZ);
    Serial.print(F(", "));
    Serial.print(FOC_BANDWIDTH_MAX_HZ);
    Serial.println(F("] Hz; stored value unchanged"));
    ackCurrentCommand(false, F("bandwidth outside permitted range"));
    return;
  }

  if (!calibrationRecordValid(config.motor[0]) || !calibrationRecordValid(config.motor[1])) {
    Serial.println(F("BW REFUSED: calibration required (missing or invalid R/L)"));
    ackCurrentCommand(false, F("calibration required"));
    return;
  }

  const uint16_t previous = config.current_bandwidth_hz;
  if (!deriveAndReportTiming(requested, false)) {
    Serial.println(F("BW REFUSED: no deterministic configuration; stored value unchanged"));
    ackCurrentCommand(false, F("no deterministic configuration"));
    return;
  }

  // Persist the REQUESTED value, not the clamped one, so a later ceiling increase
  // re-applies the original request (FR-020d).
  config.current_bandwidth_hz = requested;
  if (!saveConfig(config)) {
    config.current_bandwidth_hz = previous;
    Serial.println(F("BW ERROR: persistence failed; reverting"));
    deriveAndReportTiming(previous, false);
    ackCurrentCommand(false, F("persistence failed"));
    return;
  }

  if (timing_config.carrier_hz != (uint32_t)driver1.pwm_frequency) {
    if (focTimingApplyCarrier(driver1.params, driver2.params, timing_config.carrier_hz,
                              timing_config.carrier_period_ticks)) {
      driver1.pwm_frequency = (long)timing_config.carrier_hz;
      driver2.pwm_frequency = (long)timing_config.carrier_hz;
      Serial.println(F("carrier applied in place"));
    } else {
      Serial.println(F("NOTE: the new carrier takes effect after a restart"));
    }
  }

  reportBandwidth();
  ackCurrentCommand(true, F(""));
}

// Controller identity is a persisted setting. The suffix is hexadecimal with an optional 0x
// prefix, matching the way the identity is reported everywhere else in the firmware.
void doBusIdentity(char *cmd) {
  if (cmd == nullptr || cmd[0] == '\0') {
    Serial.print(F("CAN ID: 0x"));
    Serial.println(config.can_id, HEX);
    emitIdRecord();
    return;
  }

  if (motor_state[0].armed || motor_state[1].armed) {
    Serial.println(F("CAN ID REFUSED: disarm both motors first (D0)"));
    ackCurrentCommand(false, F("disarm both motors first"));
    return;
  }

  char *end = nullptr;
  const unsigned long parsed = strtoul(cmd, &end, 16);
  if (end == cmd || *end != '\0' || parsed < 0x001UL || parsed > 0x7FFUL) {
    Serial.println(F("CAN ID REFUSED: expected hexadecimal 0x001-0x7FF; stored value unchanged"));
    ackCurrentCommand(false, F("expected hexadecimal 0x001-0x7FF"));
    emitFaultRecord("protocol", F("invalid bus identity"));
    return;
  }

  RobotConfig candidate = config;
  candidate.can_id = (uint16_t)parsed;
  if (!saveConfig(candidate)) {
    Serial.println(F("CAN ID REFUSED: persistence failed; stored value unchanged"));
    ackCurrentCommand(false, F("persistence failed"));
    return;
  }

  // Publish the runtime value only after persistence succeeds.
  config = candidate;
  Serial.print(F("CAN ID saved: 0x"));
  Serial.println(config.can_id, HEX);
  ackCurrentCommand(true, F(""));
  emitIdRecord();
}

void reportBusWindow() {
  Serial.print(F("VBUS window [mV]: "));
  Serial.print(config.bus_voltage_min_mv);
  Serial.print(F(" .. "));
  Serial.println(config.bus_voltage_max_mv);
  Serial.print(F("VBUS measured [mV]: "));
  Serial.println((uint32_t)(current_vbus * 1000.0f));
  Serial.print(F("VBUS protect: "));
  Serial.println(bus_voltage_protection_active ? F("active") : F("clear"));
  Serial.print(F("VBUS events: "));
  Serial.println(bus_voltage_event_count);
  emitBusRecord();
  emitCfgRecord();
}

void doBusWindow(char *cmd) {
  if (cmd == nullptr || cmd[0] == '\0') {
    reportBusWindow();
    return;
  }

  if (motor_state[0].armed || motor_state[1].armed) {
    Serial.println(F("VBUS REFUSED: disarm both motors first (D0)"));
    ackCurrentCommand(false, F("disarm both motors first"));
    return;
  }

  char *comma = strchr(cmd, ',');
  if (comma == nullptr || comma == cmd || comma[1] == '\0') {
    Serial.println(F("VBUS REFUSED: expected V<min_mv>,<max_mv>; stored value unchanged"));
    ackCurrentCommand(false, F("expected V<min_mv>,<max_mv>"));
    return;
  }

  *comma = '\0';
  char *end_min = nullptr;
  char *end_max = nullptr;
  const unsigned long min_mv = strtoul(cmd, &end_min, 10);
  const unsigned long max_mv = strtoul(comma + 1, &end_max, 10);
  // end_min lands on the comma we temporarily nulled; check that before restoring it.
  const bool min_ok = end_min != cmd && end_min == comma;
  const bool max_ok = end_max != (comma + 1) && *end_max == '\0';
  *comma = ',';
  if (!min_ok || !max_ok || min_mv < DEFAULT_BUS_MIN_MV || max_mv > DEFAULT_BUS_MAX_MV ||
      min_mv >= max_mv) {
    Serial.print(F("VBUS REFUSED: window must be min<max inside ["));
    Serial.print(DEFAULT_BUS_MIN_MV);
    Serial.print(F(", "));
    Serial.print(DEFAULT_BUS_MAX_MV);
    Serial.print(F("] mV; parsed "));
    Serial.print((unsigned long)min_mv);
    Serial.print(',');
    Serial.print((unsigned long)max_mv);
    Serial.println(F("; stored value unchanged"));
    ackCurrentCommand(false, F("bus window outside permitted range"));
    return;
  }

  RobotConfig candidate = config;
  candidate.bus_voltage_min_mv = (uint16_t)min_mv;
  candidate.bus_voltage_max_mv = (uint16_t)max_mv;
  if (!saveConfig(candidate)) {
    Serial.println(F("VBUS REFUSED: persistence failed; stored value unchanged"));
    ackCurrentCommand(false, F("persistence failed"));
    return;
  }

  config = candidate;
  reportBusWindow();
  ackCurrentCommand(true, F(""));
}

void reportMotionModes() {
  for (uint8_t i = 0; i < 2; ++i) {
    Serial.print(F("M"));
    Serial.print(i + 1);
    Serial.print(F(" mode: "));
    Serial.println(motor_state[i].mode == MOTION_MODE_IMPEDANCE ? F("impedance") : F("velocity"));
  }
  emitCfgRecord();
}

bool applyMotionMode(uint8_t index, uint8_t mode, RobotConfig &candidate,
                     const __FlashStringHelper **reason) {
  if (motor_state[index].armed) {
    Serial.print(F("MODE REFUSED: disarm motor "));
    Serial.print(index + 1);
    Serial.println(F(" first"));
    if (reason) *reason = F("disarm required");
    return false;
  }
  if (mode == MOTION_MODE_IMPEDANCE) {
    const __FlashStringHelper *cause = nullptr;
    if (!impedanceEligible(index, &cause)) {
      Serial.print(F("MODE REFUSED: motor "));
      Serial.print(index + 1);
      Serial.print(F(": "));
      Serial.println(cause);
      if (reason) *reason = cause;
      return false;
    }
  }
  candidate.motion_mode[index] = mode;
  return true;
}

void doMotionMode(char *cmd) {
  if (cmd == nullptr || cmd[0] == '\0') {
    reportMotionModes();
    return;
  }

  if (strlen(cmd) != 2) {
    Serial.println(F("MODE REFUSED: expected M, M1V, M1I, M2V, M2I, M0V, M0I"));
    ackCurrentCommand(false, F("expected M1V/M1I/M2V/M2I/M0V/M0I"));
    return;
  }

  const char selector = cmd[0];
  const char letter = cmd[1];
  uint8_t mode = 0xFF;
  if (letter == 'V' || letter == 'v') mode = MOTION_MODE_VELOCITY;
  else if (letter == 'I' || letter == 'i') mode = MOTION_MODE_IMPEDANCE;
  if (mode == 0xFF || (selector != '0' && selector != '1' && selector != '2')) {
    Serial.println(F("MODE REFUSED: expected M, M1V, M1I, M2V, M2I, M0V, M0I"));
    ackCurrentCommand(false, F("expected M1V/M1I/M2V/M2I/M0V/M0I"));
    return;
  }

  RobotConfig candidate = config;
  bool ok = true;
  const __FlashStringHelper *reason = F("disarm required or calibration required");
  if (selector == '0' || selector == '1') ok = applyMotionMode(0, mode, candidate, &reason) && ok;
  if (ok && (selector == '0' || selector == '2')) {
    ok = applyMotionMode(1, mode, candidate, &reason) && ok;
  }
  if (!ok) {
    ackCurrentCommand(false, reason);
    return;
  }

  if (!saveConfig(candidate)) {
    Serial.println(F("MODE REFUSED: persistence failed; stored value unchanged"));
    ackCurrentCommand(false, F("persistence failed"));
    return;
  }

  config = candidate;
  if (selector == '0' || selector == '1') {
    motor_state[0].mode = mode;
    impedanceZeroEffort(motor_state[0]);
    impedanceClearPending(motor_state[0]);
    applyMotorController(0);
  }
  if (selector == '0' || selector == '2') {
    motor_state[1].mode = mode;
    impedanceZeroEffort(motor_state[1]);
    impedanceClearPending(motor_state[1]);
    applyMotorController(1);
  }
  reportMotionModes();
  ackCurrentCommand(true, F(""));
}

static void printLimitCauses(uint8_t mask) {
  bool any = false;
  if (mask & LIMIT_CAUSE_CURRENT) {
    Serial.print(F("current"));
    any = true;
  }
  if (mask & LIMIT_CAUSE_OUTPUT_VOLTAGE) {
    if (any) Serial.print(' ');
    Serial.print(F("output-voltage"));
    any = true;
  }
  if (mask & LIMIT_CAUSE_BUS_VOLTAGE) {
    if (any) Serial.print(' ');
    Serial.print(F("bus-voltage"));
    any = true;
  }
  if (!any) Serial.print(F("none"));
}

void doImpedanceReport(char *cmd) {
  (void)cmd;
  for (uint8_t i = 0; i < 2; ++i) {
    const MotorImpedanceState &s = motor_state[i];
    BLDCMotor &motor = motorByIndex(i);
    Serial.print(F("M"));
    Serial.print(i + 1);
    Serial.print(F(" mode: "));
    Serial.print(s.mode == MOTION_MODE_IMPEDANCE ? F("impedance") : F("velocity"));
    Serial.print(F("  armed: "));
    Serial.print(s.armed ? F("yes") : F("no"));
    Serial.print(F("  timed_out: "));
    Serial.println(s.timed_out ? F("yes") : F("no"));

    Serial.print(F("M"));
    Serial.print(i + 1);
    Serial.print(F(" p_des [mrad]: "));
    Serial.print(s.p_des_mrad);
    Serial.print(F("   p_meas [mrad]: "));
    Serial.print(canRadToMrad(motor.shaft_angle));
    Serial.print(F("   applied target [mrad]: "));
    Serial.println(s.applied_target_mrad);

    Serial.print(F("M"));
    Serial.print(i + 1);
    Serial.print(F(" v_des [rad/s]: "));
    Serial.print(s.v_des, 3);
    Serial.print(F("   v_meas [rad/s]: "));
    Serial.println(motor.shaft_velocity, 3);

    Serial.print(F("M"));
    Serial.print(i + 1);
    Serial.print(F(" kp: "));
    Serial.print(s.kp, 3);
    Serial.print(F("  kd: "));
    Serial.print(s.kd, 3);
    Serial.print(F("  t_ff [Nm]: "));
    Serial.println(s.t_ff, 4);

    Serial.print(F("M"));
    Serial.print(i + 1);
    Serial.print(F(" pos err [rad]: "));
    Serial.print(s.last_position_error, 5);
    Serial.print(F(" (sat limit "));
    Serial.print(POSITION_ERROR_LIMIT, 5);
    Serial.println(F(")"));

    Serial.print(F("M"));
    Serial.print(i + 1);
    Serial.print(F(" torque cmd [Nm]: "));
    Serial.print(s.last_torque_cmd, 4);
    Serial.print(F("   Iq meas [A]: "));
    Serial.println(motor.current.q, 3);

    Serial.print(F("M"));
    Serial.print(i + 1);
    Serial.print(F(" limit causes: "));
    printLimitCauses(s.limit_cause);
    Serial.print(F("   limit events: "));
    Serial.println(s.limit_event_count);

    Serial.print(F("M"));
    Serial.print(i + 1);
    Serial.print(F(" pair fault: "));
    Serial.print(s.pair_fault_latched ? F("yes") : F("no"));
    Serial.print(F("   last pair seq: "));
    if (s.has_applied_seq) Serial.print(s.last_applied_seq);
    else Serial.print(F("none"));
    Serial.print(F("   capture generation: "));
    Serial.println(s.capture_generation);

    impedanceClearReportedLimits(motor_state[i]);
    emitImpRecord(i);
  }
  ackCurrentCommand(true, F(""));
}

void doImpedanceApply(char *cmd) {
  if (cmd == nullptr || cmd[0] == '\0' || cmd[1] != ',') {
    Serial.println(F("K REFUSED: expected K<n>,<p_mrad>,<v>,<kp>,<kd>,<tff>"));
    ackCurrentCommand(false, F("expected K1,<p_mrad>,<v>,<kp>,<kd>,<tff>"));
    return;
  }

  const uint8_t motor = (uint8_t)(cmd[0] - '0');
  if (motor < 1 || motor > 2) {
    Serial.println(F("K REFUSED: motor must be 1 or 2"));
    ackCurrentCommand(false, F("motor must be 1 or 2"));
    return;
  }
  const uint8_t index = motor - 1;
  if (motor_state[index].mode != MOTION_MODE_IMPEDANCE) {
    Serial.print(F("K REFUSED: motor "));
    Serial.print(motor);
    Serial.println(F(" is not in impedance mode"));
    ackCurrentCommand(false, F("motor not in impedance mode"));
    return;
  }

  long p_des = 0;
  float v_des = 0.0f;
  float kp = 0.0f;
  float kd = 0.0f;
  float t_ff = 0.0f;
  if (sscanf(cmd + 2, "%ld,%f,%f,%f,%f", &p_des, &v_des, &kp, &kd, &t_ff) != 5) {
    Serial.println(F("K REFUSED: expected K<n>,<p_mrad>,<v>,<kp>,<kd>,<tff>"));
    ackCurrentCommand(false, F("expected K1,<p_mrad>,<v>,<kp>,<kd>,<tff>"));
    return;
  }

  if (p_des < -IMPEDANCE_P_MRAD_LIMIT || p_des > IMPEDANCE_P_MRAD_LIMIT) {
    Serial.println(F("K REFUSED: p_des must be -628319 to 628319 mrad"));
    ackCurrentCommand(false, F("p_des must be -628319 to 628319 mrad"));
    return;
  }
  if (!finiteInRange(v_des, IMPEDANCE_V_MIN, IMPEDANCE_V_MAX)) {
    Serial.println(F("K REFUSED: v_des must be -45 to 45 rad/s"));
    ackCurrentCommand(false, F("v_des must be -45 to 45 rad/s"));
    return;
  }
  if (!finiteInRange(kp, IMPEDANCE_KP_MIN, IMPEDANCE_KP_MAX)) {
    Serial.println(F("K REFUSED: kp must be 0 to 50"));
    ackCurrentCommand(false, F("kp must be 0 to 50"));
    return;
  }
  if (!finiteInRange(kd, IMPEDANCE_KD_MIN, IMPEDANCE_KD_MAX)) {
    Serial.println(F("K REFUSED: kd must be 0 to 1"));
    ackCurrentCommand(false, F("kd must be 0 to 1"));
    return;
  }
  if (!finiteInRange(t_ff, IMPEDANCE_TFF_MIN, IMPEDANCE_TFF_MAX)) {
    Serial.println(F("K REFUSED: t_ff must be -0.5 to 0.5 Nm"));
    ackCurrentCommand(false, F("t_ff must be -0.5 to 0.5 Nm"));
    return;
  }

  impedanceApplySerial(motor_state[index], (int32_t)p_des, v_des, kp, kd, t_ff, micros());
  emitImpRecord(index);
  ackCurrentCommand(true, F(""));
}

void doTiming(char *cmd) {
  if (cmd != nullptr && (cmd[0] == 'S' || cmd[0] == 's')) {
    const uint32_t failures = focTimingSelfTest(Serial);
    Serial.print(F("self-test failures: "));
    Serial.println(failures);
    return;
  }

  Serial.print(F("control rate nominal [Hz]: "));
  Serial.println(timing_config.control_rate_hz);
  Serial.print(F("control rate measured [Hz]: "));
  Serial.println(foc_timing.measured_rate_hz, 1);
  Serial.print(F("control period [us]: "));
  Serial.println(timing_config.control_period_us, 2);
  Serial.print(F("cycles: "));
  Serial.print(foc_timing.cycle_count);
  Serial.print(F("   overruns: "));
  Serial.print(foc_timing.overrun_count);
  Serial.print(F("   consecutive: "));
  Serial.println(foc_timing.consecutive_overruns);
  Serial.print(F("last cycle [us]: "));
  Serial.print(foc_timing.last_cycle_us);
  Serial.print(F("   worst cycle [us]: "));
  Serial.println(foc_timing.worst_cycle_us);
  if (timing_config.control_period_us > 0.0f) {
    Serial.print(F("duty [%]: "));
    Serial.println((float)foc_timing.worst_cycle_us * 100.0f / timing_config.control_period_us, 1);
  }
  Serial.print(F("carrier [Hz]: "));
  Serial.print(timing_config.carrier_hz);
  Serial.print(F("  decimation: "));
  Serial.println(timing_config.decimation);
  Serial.print(F("trigger attached: "));
  Serial.println(timing_started ? F("yes") : F("NO"));
  Serial.print(F("timing fault: "));
  if (foc_timing.overrun_fault) Serial.println(F("sustained overruns"));
  else if (foc_timing.timing_fault) Serial.println(F("rate divergence"));
  else Serial.println(F("none"));
  Serial.print(F("sustainable rate ceiling [Hz]: "));
  Serial.print(foc_max_sustainable_rate_hz, 0);
  Serial.println(F("  (PROVISIONAL until measured, task T031)"));
}

// Serial commands: A1/A2/A0 arm motor 1/motor 2/both; D1/D2/D0 disarm them.
// The existing SimpleFOC commands remain available as a ... and b ... .
void doArm(char* cmd) {
  if (controller_mode != ControllerMode::Normal) { Serial.println(F("CAL motion command rejected")); ackCurrentCommand(false, F("controller is in calibration mode")); return; }
  if (cmd == nullptr || cmd[0] == '\0') {
    Serial.println(F("Usage: A1, A2, or A0"));
    ackCurrentCommand(false, F("expected A1, A2, or A0"));
    return;
  }
  const char selector = cmd[0];
  if (selector != '0' && selector != '1' && selector != '2') {
    Serial.println(F("Usage: A1, A2, or A0"));
    ackCurrentCommand(false, F("expected A1, A2, or A0"));
    return;
  }

  const __FlashStringHelper *cause = nullptr;
  if ((selector == '0' || selector == '1') && motor_state[0].mode == MOTION_MODE_IMPEDANCE &&
      !impedanceEligible(0, &cause)) {
    Serial.print(F("ARM REFUSED M1: "));
    Serial.println(cause);
    ackCurrentCommand(false, cause);
    return;
  }
  if ((selector == '0' || selector == '2') && motor_state[1].mode == MOTION_MODE_IMPEDANCE &&
      !impedanceEligible(1, &cause)) {
    Serial.print(F("ARM REFUSED M2: "));
    Serial.println(cause);
    ackCurrentCommand(false, cause);
    return;
  }

  switch (selector) {
    case '1': armMotor(motor1); break;
    case '2': armMotor(motor2); break;
    case '0': armAllMotors(); break;
  }
  Serial.println(F("Motor armed"));
  ackCurrentCommand(true, F(""));
}

void doDisarm(char* cmd) {
  switch (cmd[0]) {
    case '1': disarmMotor(motor1); break;
    case '2': disarmMotor(motor2); break;
    case '0': disarmAllMotors(); break;
    default:
      Serial.println(F("Usage: D1, D2, or D0"));
      ackCurrentCommand(false, F("expected D1, D2, or D0"));
      return;
  }
  Serial.println(F("Motor disarmed"));
  ackCurrentCommand(true, F(""));
}

// Time tracking for CAN
unsigned long last_can_status_time = 0;
const unsigned long CAN_STATUS_INTERVAL_MS = 10; // 100Hz = 10ms
static const int POLLING_RATE_MS = 5; // Polling rate for CAN alerts in 200Hz
static uint8_t status_sent_count = 0;

void TaskFOC(void *pvParams);

float normalizeAngle(float angle) {
  while (angle < 0.0f) angle += _2PI;
  while (angle >= _2PI) angle -= _2PI;
  return angle;
}

bool finiteInRange(float value, float low, float high) {
  return isfinite(value) && value >= low && value <= high;
}

bool calibrationRecordValid(const RobotConfig::MotorCalibration &record) {
  return (record.flags & 0x03) == 0x03 && record.pole_pairs >= 1 && record.pole_pairs <= 64 &&
         (record.sensor_direction == 1 || record.sensor_direction == -1) &&
         finiteInRange(record.electrical_offset, 0.0f, _2PI) &&
         finiteInRange(record.phase_resistance, 0.01f, 100.0f) &&
         finiteInRange(record.inductance_d, 0.000001f, 0.1f) &&
         finiteInRange(record.inductance_q, 0.000001f, 0.1f) &&
         finiteInRange(record.current_bandwidth, 10.0f, 1000.0f);
}

bool impedanceEligible(uint8_t index, const __FlashStringHelper **reason) {
  const RobotConfig::MotorCalibration &record = config.motor[index];
  if ((record.flags & 0x01) == 0) {
    if (reason) *reason = F("alignment not confirmed");
    return false;
  }
  if ((record.flags & 0x02) == 0) {
    if (reason) *reason = F("characterisation not confirmed");
    return false;
  }
  if (record.pole_pairs < 1 || record.pole_pairs > 64) {
    if (reason) *reason = F("pole pairs missing or invalid");
    return false;
  }
  if (!(record.sensor_direction == 1 || record.sensor_direction == -1)) {
    if (reason) *reason = F("sensor direction missing or invalid");
    return false;
  }
  if (!finiteInRange(record.electrical_offset, 0.0f, _2PI)) {
    if (reason) *reason = F("electrical offset missing or invalid");
    return false;
  }
  if (!finiteInRange(record.phase_resistance, 0.01f, 100.0f)) {
    if (reason) *reason = F("phase resistance missing or invalid");
    return false;
  }
  if (!finiteInRange(record.inductance_d, 0.000001f, 0.1f) ||
      !finiteInRange(record.inductance_q, 0.000001f, 0.1f)) {
    if (reason) *reason = F("inductance missing or invalid");
    return false;
  }
  if (motorByIndex(index).current_sense == nullptr) {
    if (reason) *reason = F("current sense missing or invalid");
    return false;
  }
  if (MOTOR_TORQUE_CONSTANT_NM_PER_A <= 1e-6f) {
    if (reason) *reason = F("torque constant missing or invalid");
    return false;
  }
  return true;
}

void applyMotorController(uint8_t index) {
  BLDCMotor &motor = motorByIndex(index);
  motor.torque_controller = TorqueControlType::foc_current;
  if (motor_state[index].mode == MOTION_MODE_IMPEDANCE) {
    motor.controller = MotionControlType::torque;
    motor.PID_velocity.reset();
  } else {
    motor.controller = MotionControlType::velocity;
  }
}

void resetConfig(RobotConfig &value, uint16_t canId = DEFAULT_CAN_ID) {
  memset(&value, 0, sizeof(value));
  value.magic = CONFIG_MAGIC;
  value.version = CONFIG_VERSION;
  value.can_id = (canId >= 0x001 && canId <= 0x7FF) ? canId : DEFAULT_CAN_ID;
  value.current_bandwidth_hz = FOC_BANDWIDTH_DEFAULT_HZ;
  value.motion_mode[0] = MOTION_MODE_VELOCITY;
  value.motion_mode[1] = MOTION_MODE_VELOCITY;
  value.bus_voltage_min_mv = DEFAULT_BUS_MIN_MV;
  value.bus_voltage_max_mv = DEFAULT_BUS_MAX_MV;
}

// Brings the version-2 fields into a valid state without touching calibration.
bool sanitizeConfigV2Fields(RobotConfig &value) {
  bool changed = false;
  if (value.current_bandwidth_hz < FOC_BANDWIDTH_MIN_HZ ||
      value.current_bandwidth_hz > FOC_BANDWIDTH_MAX_HZ) {
    value.current_bandwidth_hz = FOC_BANDWIDTH_DEFAULT_HZ;
    changed = true;
  }
  for (uint8_t i = 0; i < 2; ++i) {
    if (value.motion_mode[i] != MOTION_MODE_VELOCITY &&
        value.motion_mode[i] != MOTION_MODE_IMPEDANCE) {
      value.motion_mode[i] = MOTION_MODE_VELOCITY;
      changed = true;
    }
  }
  const bool window_invalid = value.bus_voltage_min_mv == 0 || value.bus_voltage_max_mv == 0 ||
                              value.bus_voltage_min_mv >= value.bus_voltage_max_mv;
  const bool legacy_provisional =
      value.bus_voltage_min_mv == LEGACY_PROVISIONAL_BUS_MIN_MV &&
      value.bus_voltage_max_mv == LEGACY_PROVISIONAL_BUS_MAX_MV;
  if (window_invalid || legacy_provisional) {
    value.bus_voltage_min_mv = DEFAULT_BUS_MIN_MV;
    value.bus_voltage_max_mv = DEFAULT_BUS_MAX_MV;
    changed = true;
  }
  return changed;
}

// Reads a stored version-1 blob and carries its calibration forward. Losing calibration
// here would force a full recalibration of both motors, so this path is deliberately
// explicit rather than falling through to resetConfig().
bool migrateConfigV1ToV2(RobotConfig &out) {
  RobotConfigV1 legacy = {};
  if (prefs.getBytesLength("cfg") != sizeof(RobotConfigV1)) return false;
  if (prefs.getBytes("cfg", &legacy, sizeof(legacy)) != sizeof(legacy)) return false;
  if (legacy.magic != CONFIG_MAGIC || legacy.version != 1) return false;

  resetConfig(out, legacy.can_id);
  out.calibrated = legacy.calibrated;
  for (uint8_t i = 0; i < 2; ++i) {
    out.motor[i].pole_pairs = legacy.motor[i].pole_pairs;
    out.motor[i].sensor_direction = legacy.motor[i].sensor_direction;
    out.motor[i].flags = legacy.motor[i].flags;
    out.motor[i].electrical_offset = legacy.motor[i].electrical_offset;
    out.motor[i].phase_resistance = legacy.motor[i].phase_resistance;
    out.motor[i].inductance_d = legacy.motor[i].inductance_d;
    out.motor[i].inductance_q = legacy.motor[i].inductance_q;
    out.motor[i].current_bandwidth = legacy.motor[i].current_bandwidth;
  }
  sanitizeConfigV2Fields(out);
  return true;
}

bool configIsComplete(RobotConfig &value) {
  value.calibrated = calibrationRecordValid(value.motor[0]) && calibrationRecordValid(value.motor[1]);
  return value.calibrated;
}

BLDCMotor &motorByIndex(uint8_t index) { return index == 0 ? motor1 : motor2; }
InlineCurrentSense &currentSenseByIndex(uint8_t index) { return index == 0 ? current_sense1 : current_sense2; }

void resetCalibrationSession() {
  calibration_operation = CalibrationOperation::Idle;
  memset(&pending_calibration, 0, sizeof(pending_calibration));
}

void applyCalibrationToMotor(uint8_t index) {
  BLDCMotor &motor = motorByIndex(index);
  const RobotConfig::MotorCalibration &record = config.motor[index];
  motor.pole_pairs = record.pole_pairs;
  motor.sensor_direction = record.sensor_direction > 0 ? Direction::CW : Direction::CCW;
  motor.zero_electric_angle = record.electrical_offset;
  motor.phase_resistance = record.phase_resistance;
  motor.axis_inductance = {record.inductance_d, record.inductance_q};
}

void send_can_status() {
  unsigned long now = millis();
  if (now - last_can_status_time < CAN_STATUS_INTERVAL_MS) {
    // Serial.print(F("Now: "));
    // Serial.print(now);
    // Serial.print(F(" Last CAN Status: "));
    // Serial.println(last_can_status_time);
    return;
  }
  // Serial.print(F("Sending CAN status at: "));
  // Serial.println(now);
  last_can_status_time = now;

  // twai_message_t message;
  // message.identifier = 0x0f1; // 0x201 or 0x202
  // message.data_length_code = 4;
  // for (int i = 0; i < 4; i++) {
  //   message.data[i] = 0; // Fill with zeros or some status info if needed
  // }
  // if (twai_transmit(&message, pdMS_TO_TICKS(1000)) != ESP_OK) { // Timeout of 10ms for transmission
  //   Serial.println(F("TWAI Transmit Failed"));
  // } else {
  //   Serial.println(F("Message queued for transmission"));
  // }

  // Derive NodeID from config.can_id (0x201 -> 1, 0x202 -> 2)
  uint8_t node_id = config.can_id & 0xFF; // Simple extraction if config.can_id is 0x201 or 0x202

  // --- Status #1 (0x180 + NodeID) : Velocity ---
  twai_message_t msg1;
  msg1.identifier = 0x180 + node_id;
  // msg1.extd = 0;
  // msg1.rtr = 0;
  msg1.data_length_code = 8;
  
  // Left Wheel Speed (Motor2)
  int16_t speed_left = (int16_t)(motor2.shaft_velocity * 100.0f);
  msg1.data[0] = speed_left & 0xFF;
  msg1.data[1] = (speed_left >> 8) & 0xFF;

  // Right Wheel Speed (Motor1)
  int16_t speed_right = (int16_t)(motor1.shaft_velocity * 100.0f);
  msg1.data[2] = speed_right & 0xFF;
  msg1.data[3] = (speed_right >> 8) & 0xFF;
  
  // Angle data? Or reserved? Protocol didn't specify bytes 4-7 for Status #1.
  // Assuming 0 for now or putting something useful.
  // Protocol says "states + measured velocity". 
  // Maybe bytes 4-7 are states? Let's just zero them for now unless specified.
  msg1.data[4] = 0; 
  msg1.data[5] = 0;
  msg1.data[6] = 0;
  msg1.data[7] = 0;

  twai_transmit(&msg1, 0);

  // --- Status #2 (0x190 + NodeID) : Current + Bus Voltage ---
  twai_message_t msg2;
  msg2.identifier = 0x190 + node_id;
  // msg2.extd = 0;
  // msg2.rtr = 0;
  msg2.data_length_code = 8;

  // Current (Left/Motor2) - scaling? Assuming 0.01A or similar? 
  // Protocol doesn't specify scaling for current. Assuming amps * 100 (int16).
  int16_t current_left = (int16_t)(current_sense2.getDCCurrent(motor2.electrical_angle) * 100.0f); 
  msg2.data[0] = current_left & 0xFF;
  msg2.data[1] = (current_left >> 8) & 0xFF;

  // Current (Right/Motor1)
  int16_t current_right = (int16_t)(current_sense1.getDCCurrent(motor1.electrical_angle) * 100.0f);
  msg2.data[2] = current_right & 0xFF;
  msg2.data[3] = (current_right >> 8) & 0xFF;

  // Bus Voltage (0.1V scaling matching typical CAN protocols? Or 0.01V?)
  // Protocol says "bus voltage". Let's assume 0.1V for now (`vbus_volts * 10`).
  // Or stick to 0.01 units like velocity? Let's use 100 scaling.
  // float vSense = (sum_mV / (volt_samples > 0 ? (float)volt_samples : 1.0f)) / 1000.0f;
  // float vbus = vSense * DIVIDER_GAIN;
  extern float current_vbus; // Defined in main part of file or below
  int16_t bus_voltage = (int16_t)(current_vbus * 100.0f);
  msg2.data[4] = bus_voltage & 0xFF;
  msg2.data[5] = (bus_voltage >> 8) & 0xFF;
  
  msg2.data[6] = 0;
  msg2.data[7] = 0;

  twai_transmit(&msg2, 0);

  // Heartbeat message will be sent at around 1Hz rate
  if (status_sent_count < 100) { // Send status messages at 100Hz for the first second, then switch to heartbeat
    status_sent_count++;
    return;
  }
  // --- Heartbeat (0x700 + NodeID) ---
  // Protocol says "Heartbeat". Usually 1 byte state.
  twai_message_t msgHb = {};
  msgHb.identifier = 0x700 + node_id;
  // msgHb.extd = 0;
  // msgHb.rtr = 0;
  msgHb.data_length_code = 1;
  msgHb.data[0] = 0x0;
  twai_transmit(&msgHb, 0);

  status_sent_count = 0; // Reset count to switch back to status messages after sending heartbeat
}

void handle_can() {
  twai_message_t message;
  while (twai_receive(&message, 0) == ESP_OK) {
    // Serial.println(message.identifier, HEX);
    // 1. Emergency Stop (0x080)
    if (message.identifier == 0x080) {
      if (controller_mode == ControllerMode::Calibration) failCalibration(F("emergency stop"));
      else disarmAllMotors();
    }
    // 2. Command Message (0x200 + NodeID)
    else if (message.identifier == config.can_id) {
       if (controller_mode == ControllerMode::Calibration) {
         // Calibration feedback is serial-only. Never accept a normal motion frame here.
         continue;
       }
       if (message.data_length_code == 8) {
         // Byte 0: cmd
         //   0x01 = set both velocity targets and arm/disarm both (legacy format)
         //   0x02 = arm/disarm motor1 only
         //   0x03 = arm/disarm motor2 only
         //   0x04 = arm/disarm both motors without changing targets
         uint8_t cmd = message.data[0];

         // Byte 1: flags
         uint8_t flags = message.data[1];
         bool enable = flags & 0x01;
         bool brake = flags & 0x02;
         bool clear_fault = flags & 0x04; // bit 2
         bool estop = flags & 0x08;       // bit 3

         Serial.print(F("Received flags: "));
         Serial.println(flags, HEX);

         if (estop) {
            disarmAllMotors();
            continue;
         }

         if (cmd == 0x02) {
           if (enable) armMotor(motor1);
           else disarmMotor(motor1);
           continue;
         }

         if (cmd == 0x03) {
           if (enable) armMotor(motor2);
           else disarmMotor(motor2);
           continue;
         }

         if (cmd == 0x04) {
           if (enable) armAllMotors();
           else disarmAllMotors();
           continue;
         }

         if (cmd != 0x01) {
           Serial.println(F("Ignoring unknown CAN motor command"));
           continue;
         }

         // A velocity command always controls the pair, preserving the
         // original CAN protocol used by the companion controller.
         if (enable) armAllMotors();
         else disarmAllMotors();

         if (clear_fault) {
             // Reset errors if any
         }

         // Byte 2: mode
         // uint8_t mode = message.data[2]; // 0 = rad/s

         // Byte 3: seq

         // A velocity payload addressed to an impedance-mode motor is rejected, never
         // reinterpreted: the two formats mean different things in the same bytes
         // (FR-006a, FR-054).
         if (motor_state[0].mode == MOTION_MODE_IMPEDANCE ||
             motor_state[1].mode == MOTION_MODE_IMPEDANCE) {
           Serial.println(F("CAN REJECT: velocity command while a motor is in impedance mode"));
           continue;
         }

         // Bytes 4..5: target_left (Motor2)
         int16_t target_left_int = (int16_t)(message.data[4] | (message.data[5] << 8));
         float target_left = (float)target_left_int / 100.0f;

         // Bytes 6..7: target_right (Motor1)
         int16_t target_right_int = (int16_t)(message.data[6] | (message.data[7] << 8));
         float target_right = (float)target_right_int / 100.0f;

         if (enable) {
             const uint32_t now_us = micros();
             motor_state[1].velocity_target = target_left;
             motor_state[0].velocity_target = target_right;
             motor_state[0].last_command_us = now_us;
             motor_state[1].last_command_us = now_us;
             motor_state[0].timed_out = false;
             motor_state[1].timed_out = false;
         }
       }
    }
  }
}


bool loadConfig(RobotConfig &config) {
  if (!prefs.begin("robot_config", true)) {
    Serial.println(F("Failed to open preferences"));
    return false;
  }

  size_t n = prefs.getBytesLength("cfg");
  RobotConfig loaded = {};
  if (n == sizeof(RobotConfig)) prefs.getBytes("cfg", &loaded, sizeof(RobotConfig));

  if (loaded.magic == CONFIG_MAGIC && loaded.version == CONFIG_VERSION && loaded.can_id != 0 &&
      loaded.can_id <= 0x7FF) {
    config = loaded;
    const bool sanitized = sanitizeConfigV2Fields(config);
    configIsComplete(config);
    prefs.end();
    if (sanitized) {
      if (saveConfig(config)) Serial.println(F("CONFIG: updated provisional defaults"));
      else Serial.println(F("CONFIG WARN: updated defaults could not be persisted"));
    }
    return true;
  }

  // Version-2 read failed. Before discarding anything, try to carry a version-1 record
  // forward so an already-calibrated unit does not need recalibrating.
  RobotConfig migrated = {};
  if (migrateConfigV1ToV2(migrated)) {
    config = migrated;
    configIsComplete(config);
    prefs.end();
    if (!saveConfig(config)) {
      Serial.println(F("CONFIG WARN: migrated v1->v2 but the rewrite failed"));
    } else {
      Serial.println(F("CONFIG: migrated v1 -> v2, calibration preserved"));
    }
    return true;
  }

  uint16_t legacyCanId = DEFAULT_CAN_ID;
  if (n >= sizeof(legacyCanId)) prefs.getBytes("cfg", &legacyCanId, sizeof(legacyCanId));
  resetConfig(config, legacyCanId);
  prefs.end();
  return false;
}

bool saveConfig(const RobotConfig &config) {
  if (!prefs.begin("robot_config", false)) {
    Serial.println(F("Failed to open preferences"));
    return false;
  }

  if (!prefs.putBytes("cfg", &config, sizeof(struct RobotConfig))) {
    Serial.println(F("Failed to save config"));
    prefs.end();
    return false;
  }

  prefs.end();
  return true;
}

void eraseConfig() {
  if (!prefs.begin("robot_config", false)) {
    Serial.println(F("Failed to open preferences"));
    return;
  }

  prefs.clear();
  prefs.end();
}

bool setup_twai() {
  twai_general_config_t g_config = TWAI_GENERAL_CONFIG_DEFAULT((gpio_num_t)TX_PIN, (gpio_num_t)RX_PIN, TWAI_MODE_NORMAL);
  twai_timing_config_t t_config = TWAI_TIMING_CONFIG_1MBITS();  //Look in the api-reference for other speed sets.
  twai_filter_config_t f_config = TWAI_FILTER_CONFIG_ACCEPT_ALL();
  // Accept E-stop and messages with self NodeID, reject others
  // f_config.acceptance_code = (0x080 << 21) | (config.can_id << 21); // Shifted to match the position in the acceptance code
  // f_config.acceptance_mask = (0x7FF << 21) | (0x7FF << 21); // Mask to check only the relevant bits for E-stop and NodeID
  // f_config.single_filter = true; // Use single filter mode

  if (twai_driver_install(&g_config, &t_config, &f_config) == ESP_OK) {
    Serial.println(F("TWAI driver installed"));
  } else {
    Serial.println(F("Failed to install TWAI driver"));
    return false;
  }

  if (twai_start() == ESP_OK) {
    Serial.println(F("TWAI driver started"));
  } else {
    Serial.println(F("Failed to start TWAI driver"));
    return false;
  }

  // Reconfigure alerts to detect frame receiving and bus errors
  uint32_t alerts = TWAI_ALERT_ALL;
  if (twai_reconfigure_alerts(alerts, NULL) == ESP_OK) {
    Serial.println(F("TWAI alerts reconfigured"));
  } else {
    Serial.println(F("Failed to reconfigure TWAI alerts"));
    return false;
  }

  return true;
}

void setup() {

  // use monitoring with serial 
  Serial.begin(115200);
  // delay(200);

  if (loadConfig(config)) {
    Serial.println(config.calibrated ? F("Config loaded: calibrated") : F("Config loaded: calibration required"));
  } else {
    Serial.println(F("No compatible calibration config; using safe defaults"));
    saveConfig(config);
  }

  if (!setup_twai()) {
    return;
  }

  // enable more verbose output for debugging
  // comment out if not needed
  SimpleFOCDebug::enable(&Serial);

  // initialize encoder sensor hardware
  sensor1.init();
  sensor2.init();
  // link the motor to the sensor
  motor1.linkSensor(&sensor1);
  motor2.linkSensor(&sensor2);

  // Derive the timing configuration before the drivers are initialised, because the
  // carrier is a derived value and MCPWM only accepts it at init time.
  if (!focDeriveTiming(config.current_bandwidth_hz, foc_max_sustainable_rate_hz,
                       timing_config)) {
    Serial.println(F("TIMING ERROR: falling back to the default bandwidth"));
    if (!focDeriveTiming(FOC_BANDWIDTH_DEFAULT_HZ, foc_max_sustainable_rate_hz,
                         timing_config)) {
      Serial.println(F("TIMING FATAL: no deterministic configuration available"));
      return;
    }
  }

  // driver config
  // power supply voltage [V]
  driver1.voltage_power_supply = 12;
  driver2.voltage_power_supply = 12;
  // Both drivers MUST carry the same frequency. They share one MCPWM timer, and
  // SimpleFOC refuses to initialise a second driver whose requested frequency differs
  // from the timer already configured (esp32_driver_mcpwm.cpp). Leaving driver2 unset,
  // as the previous code did, only worked because the default happened to match.
  driver1.pwm_frequency = (long)timing_config.carrier_hz;
  driver2.pwm_frequency = (long)timing_config.carrier_hz;
  if (driver1.init() != 1) Serial.println(F("DRIVER ERROR: driver1 init failed"));
  if (driver2.init() != 1) Serial.println(F("DRIVER ERROR: driver2 init failed"));
  // link driver
  motor1.linkDriver(&driver1);
  motor2.linkDriver(&driver2);
  // link current sense and the driver
  current_sense1.linkDriver(&driver1);
  current_sense2.linkDriver(&driver2);

  // set control loop type to be used
  motor1.foc_modulation = FOCModulationType::SpaceVectorPWM;
  motor1.torque_controller = TorqueControlType::foc_current;
  motor1.controller = MotionControlType::velocity;

  motor2.foc_modulation = FOCModulationType::SpaceVectorPWM;
  motor2.torque_controller = TorqueControlType::foc_current;
  motor2.controller = MotionControlType::velocity;

  // controller configuration based on the control type
  motor1.PID_velocity.P = 0.1f;
  motor1.PID_velocity.I = 1.0f;
  motor1.PID_velocity.D = 0;

  motor2.PID_velocity.P = 0.1f;
  motor2.PID_velocity.I = 1.0f;
  motor2.PID_velocity.D = 0;

  // velocity low pass filtering time constant
  motor1.LPF_velocity.Tf = 0.01f;
  motor2.LPF_velocity.Tf = 0.01f;

  // angle loop controller
  // motor1.P_angle.P = 20.0f;
  // motor2.P_angle.P = 20.0f;

  // default voltage_power_supply
  motor1.voltage_limit = 12;
  motor2.voltage_limit = 12;
  // angle loop velocity limit
  motor1.velocity_limit = 50;
  motor2.velocity_limit = 50;
  motor1.current_limit = 3.0f;
  motor2.current_limit = 3.0f;

  if (config.calibrated) { applyCalibrationToMotor(0); applyCalibrationToMotor(1); }

  // current sense init and linking
  current_sense1.init();
  current_sense2.init();
  // current_sense1.gain_a *= -1;
  current_sense1.skip_align = false;
  current_sense2.skip_align = false;
  motor1.linkCurrentSense(&current_sense1);
  motor2.linkCurrentSense(&current_sense2);
  // initialise motor
  motor1.init();
  motor2.init();
  // A non-calibrated unit deliberately does not run initFOC(), which would energize it.
  if (config.calibrated) {
    motor1.tuneCurrentController(config.motor[0].current_bandwidth);
    motor2.tuneCurrentController(config.motor[1].current_bandwidth);
    motor1.initFOC();
    motor2.initFOC();
    foc_initialized = true;
    controller_mode = ControllerMode::Normal;
  } else {
    enterCalibrationMode();
  }
  disarmAllMotors();

  // subscribe motor to the commander
  // command.add('p', doMotion, "motion control");
  command.add('a', doMotor1, "motor1");
  command.add('b', doMotor2, "motor2");
  command.add('A', doArm, "arm: A1 motor1, A2 motor2, A0 both");
  command.add('D', doDisarm, "disarm: D1 motor1, D2 motor2, D0 both");
  command.add('C', doCalibration, "calibration: C, C1, C2, CA, CM, CY, CN, CX, CE");
  command.add('B', doBandwidth, "bandwidth: B reports, B<hz> sets 100-10000");
  command.add('T', doTiming, "timing report; TS runs the derivation self-test");
  command.add('Q', doQuery, "structured records: Q, QC, QT, QI, QP<ms>");
  command.add('N', doBusIdentity, "bus identity: N reports, N<hex> sets 0x001-0x7FF");
  command.add('V', doBusWindow, "bus window: V reports, V<min_mv>,<max_mv> sets");
  command.add('M', doMotionMode, "motion mode: M reports, M1V/M1I/M2V/M2I/M0V/M0I");
  command.add('I', doImpedanceReport, "impedance state report");
  command.add('K', doImpedanceApply, "impedance apply: K1,<p_mrad>,<v>,<kp>,<kd>,<tff>");

  // comment out if not needed
  motor1.useMonitoring(Serial);
  // motor1.monitor_downsample = 0;
  // motor1.monitor_variables = _MON_TARGET | _MON_VEL;

  motor2.useMonitoring(Serial);
  // motor2.monitor_downsample = 0;
  // motor2.monitor_variables = _MON_TARGET | _MON_VEL;

  // Run user commands to configure and the motor (find the full command list in docs.simplefoc.com)
  Serial.print(controller_mode == ControllerMode::Normal ? F("Motors ready and disarmed. Current CAN ID: 0x") : F("Calibration required; both motors disarmed. Current CAN ID: 0x"));
  Serial.println(config.can_id, HEX);

  // Restore the persisted per-motor modes, but always start disarmed with every target,
  // gain and pending pair cleared (FR-006a, data-model startup rule).
  for (uint8_t i = 0; i < 2; ++i) {
    impedanceResetState(motor_state[i]);
    motor_state[i].mode = config.motion_mode[i];
    motor_state[i].armed = false;
    applyMotorController(i);
  }
  applyTimingToMotors();

  // Start the deterministic trigger: MCPWM on_full, decimated in the ISR, releasing a
  // high-priority task pinned to the FOC core. Replaces the old taskYIELD() free-run.
  timing_started = focTimingBegin(driver1.params, timing_config, focControlCycle, focFailClosed);
  if (!timing_started) {
    Serial.println(F("TIMING FATAL: could not attach the PWM-locked control trigger"));
    disarmAllMotors();
  }

  // Communications run on the other core so they can never delay a control cycle.
  xTaskCreatePinnedToCore(TaskComms, "TaskComms", 4096, NULL, 1, NULL, COMMS_RUNNING_CORE);

  Serial.print(F("Startup timing: active BW "));
  Serial.print(timing_config.active_bandwidth_hz);
  Serial.print(F(" Hz, control rate "));
  Serial.print(timing_config.control_rate_hz);
  Serial.print(F(" Hz, carrier "));
  Serial.print(timing_config.carrier_hz);
  Serial.print(F(" Hz, M1 mode "));
  Serial.print(motor_state[0].mode == MOTION_MODE_IMPEDANCE ? F("impedance") : F("velocity"));
  Serial.print(F(", M2 mode "));
  Serial.println(motor_state[1].mode == MOTION_MODE_IMPEDANCE ? F("impedance") : F("velocity"));
  if (timing_config.clamped) {
    Serial.print(F("Startup clamp: requested "));
    Serial.print(timing_config.requested_bandwidth_hz);
    Serial.print(F(" Hz exceeds the sustainable ceiling; active "));
    Serial.print(timing_config.active_bandwidth_hz);
    Serial.println(F(" Hz"));
  }

  // Unprompted identification, so a tool attaching later can recognise the device without asking.
  emitIdRecord();

  _delay(1000);
}

// One deterministic control cycle, released by the MCPWM-derived trigger.
//
// Runs in the high-priority control task. It MUST NOT print, block, or touch the CAN
// driver: every diagnostic is latched in memory and published by the communications
// context instead (FR-035).
void focControlCycle() {
  if (controller_mode != ControllerMode::Normal) return;

  const uint32_t now_us = micros();

  // Both motors are sampled and updated inside the same cycle so their control intervals
  // are identical (FR-012, FR-013).
  for (uint8_t i = 0; i < 2; ++i) {
    MotorImpedanceState &s = motor_state[i];
    BLDCMotor &motor = motorByIndex(i);

    impedanceExpirePending(s, now_us);
    impedanceUpdateTimeout(s, now_us);

    if (s.mode == MOTION_MODE_IMPEDANCE) {
      const float torque = impedanceComputeTorque(s, motor.shaft_angle, motor.shaft_velocity);
      const float current = impedanceTorqueToCurrent(s, torque, motor.current_limit,
                                                     motor.voltage_limit, config.motor[i].phase_resistance);
      motor.target = (s.armed && !s.timed_out && !bus_voltage_protection_active) ? current : 0.0f;
    } else {
      motor.target =
          (s.armed && !s.timed_out && !bus_voltage_protection_active) ? s.velocity_target : 0.0f;
    }
  }

  motor1.loopFOC();
  motor2.loopFOC();
  motor1.move();
  motor2.move();
}

// Called from the control task when determinism is lost. Disarming writes the driver
// registers directly, which is safe here; the reason is reported by communications.
void focFailClosed() {
  disarmMotor(motor1);
  disarmMotor(motor2);
  impedanceZeroEffort(motor_state[0]);
  impedanceZeroEffort(motor_state[1]);
  fail_closed_pending = true;
}

// Retained for the legacy declaration; the deterministic task now lives in foc_timing.
void TaskFOC(void *pvParams) {
  (void)pvParams;
  vTaskDelete(NULL);
}

// Pushes the active bandwidth and fixed timestep into both motors' controllers. Uses the
// per-motor measured resistance and inductance, so it requires valid calibration.
void applyTimingToMotors() {
  if (!timing_config.valid) return;
  for (uint8_t i = 0; i < 2; ++i) {
    const RobotConfig::MotorCalibration &record = config.motor[i];
    if (calibrationRecordValid(record)) {
      focApplyMotorTuning(motorByIndex(i), record.phase_resistance, record.inductance_d,
                          record.inductance_q, (float)timing_config.active_bandwidth_hz,
                          timing_config.control_period_us);
    } else {
      // No electrical model available, so only the timestep can be fixed.
      focApplyMotorTimestep(motorByIndex(i), timing_config.control_period_us);
    }
  }
}

void reportBandwidth() {
  Serial.print(F("BW requested [Hz]: "));
  Serial.println(timing_config.requested_bandwidth_hz);
  Serial.print(F("BW active    [Hz]: "));
  Serial.print(timing_config.active_bandwidth_hz);
  if (timing_config.clamped) {
    Serial.println(F("   (CLAMPED: sustainable rate ceiling)"));
  } else {
    Serial.println();
  }
  Serial.print(F("sampling multiple: "));
  Serial.println(timing_config.sampling_multiple);
  Serial.print(F("control rate [Hz]: "));
  Serial.println(timing_config.control_rate_hz);
  Serial.print(F("control period [us]: "));
  Serial.println(timing_config.control_period_us, 2);
  Serial.print(F("carrier [Hz]: "));
  Serial.print(timing_config.carrier_hz);
  Serial.print(F("  (decimation "));
  Serial.print(timing_config.decimation);
  Serial.println(F(")"));

  for (uint8_t i = 0; i < 2; ++i) {
    BLDCMotor &motor = motorByIndex(i);
    Serial.print(F("M"));
    Serial.print(i + 1);
    Serial.print(F(" PID current D: P="));
    Serial.print(motor.PID_current_d.P, 6);
    Serial.print(F(" I="));
    Serial.println(motor.PID_current_d.I, 6);
    Serial.print(F("M"));
    Serial.print(i + 1);
    Serial.print(F(" PID current Q: P="));
    Serial.print(motor.PID_current_q.P, 6);
    Serial.print(F(" I="));
    Serial.println(motor.PID_current_q.I, 6);
    Serial.print(F("M"));
    Serial.print(i + 1);
    Serial.print(F(" current LPF Tf [s]: "));
    Serial.println(motor.LPF_current_q.Tf, 8);
  }
}

// The control task only sets a flag; the transition is reported here so no serial work
// ever happens in the deterministic path (FR-035, FR-062).
void reportPendingTimeouts() {
  static bool reported[2] = {false, false};
  for (uint8_t i = 0; i < 2; ++i) {
    const bool timed_out = motor_state[i].timed_out;
    if (timed_out && !reported[i]) {
      reported[i] = true;
      Serial.print(F("M"));
      Serial.print(i + 1);
      Serial.println(F(" COMMAND TIMEOUT: zero effort, still armed"));
    } else if (!timed_out && reported[i]) {
      reported[i] = false;
      Serial.print(F("M"));
      Serial.print(i + 1);
      Serial.println(F(" commands resumed"));
    }
  }
}

// Fail-closed bus-voltage window. Outside the approved range every motor is forced to
// zero output and disarmed, and the cause is recorded per motor (FR-032a).
void updateBusVoltageProtection(float vbus_volts) {
  const uint32_t mv = (uint32_t)(vbus_volts * 1000.0f);
  const bool out_of_window =
      (mv < config.bus_voltage_min_mv) || (mv > config.bus_voltage_max_mv);

  for (uint8_t i = 0; i < 2; ++i) {
    impedanceNoteLimit(motor_state[i], LIMIT_CAUSE_BUS_VOLTAGE, out_of_window);
  }

  if (out_of_window && !bus_voltage_protection_active) {
    bus_voltage_protection_active = true;
    bus_voltage_event_count++;
    disarmAllMotors();
    impedanceZeroEffort(motor_state[0]);
    impedanceZeroEffort(motor_state[1]);
    Serial.print(F("BUS VOLTAGE PROTECTION: "));
    Serial.print(vbus_volts, 2);
    Serial.print(F(" V outside ["));
    Serial.print(config.bus_voltage_min_mv);
    Serial.print(F(", "));
    Serial.print(config.bus_voltage_max_mv);
    Serial.println(F("] mV; motors disarmed"));
    emitFaultRecord("bus", F("voltage outside configured window"));
  } else if (!out_of_window && bus_voltage_protection_active) {
    bus_voltage_protection_active = false;
    Serial.println(F("BUS VOLTAGE PROTECTION cleared; motors remain disarmed"));
  }
}

// Derives the timing configuration for a requested bandwidth. Returns false when no
// valid configuration exists, in which case the previous configuration is untouched.
bool deriveAndReportTiming(uint16_t requested_hz, bool report) {
  FocTimingConfig candidate;
  if (!focDeriveTiming(requested_hz, foc_max_sustainable_rate_hz, candidate)) {
    if (report) {
      Serial.print(F("BW ERROR: no deterministic configuration for "));
      Serial.print(requested_hz);
      Serial.println(F(" Hz"));
    }
    return false;
  }
  timing_config = candidate;
  applyTimingToMotors();
  focTimingSetDecimation(timing_config.decimation);
  if (report) reportBandwidth();
  return true;
}

// All communications work: CAN, serial, bus-voltage monitoring, telemetry and reporting.
// Runs on the non-FOC core at ordinary priority.
void TaskComms(void *pvParams) {
  (void)pvParams;
  for (;;) {
    commsCycle();
  }
}

void loop() {
  // Deliberately idle. Control runs in the PWM-triggered task on the FOC core and
  // communications run in TaskComms on the other core.
  vTaskDelay(pdMS_TO_TICKS(1000));
}

// ---------------------------------------------------------------------------
// Structured record emitters. Communications context only, never the control path (FR-035).
// Contract: specs/003-v13-configurator/contracts/serial-protocol.md
// ---------------------------------------------------------------------------

// Tag of the command currently being dispatched, echoed in its acknowledgement so a late reply can
// never be mistaken for a reply to a newer request. 0 means an untagged command, typed by a human.
static uint8_t active_command_tag = 0;
static char active_command_text[24] = {0};

void ackCurrentCommand(bool ok, const __FlashStringHelper *reason) {
  recordBegin("ack");
  recordKeyUint("tag", active_command_tag);
  recordKeyToken("cmd", active_command_text);
  recordKeyBool("ok", ok);
  recordKeyText("reason", reason);
  recordEnd();
}

void emitIdRecord() {
  recordBegin("id");
  recordKeyToken("fw", "002");
  recordKeyUint("proto", SERIAL_RECORD_VERSION);
  recordKeyHex("canid", config.can_id);
  recordKeyUint("motors", 2);
  recordKeyUint("cfgver", config.version);
  recordKeyUint("uptime_ms", millis());
  recordEnd();
}

void emitCalRecord(uint8_t index) {
  const RobotConfig::MotorCalibration &r = config.motor[index];
  recordBegin("cal");
  recordKeyUint("m", index + 1);
  recordKeyBool("aligned", (r.flags & 0x01) != 0);
  recordKeyBool("charac", (r.flags & 0x02) != 0);
  recordKeyUint("pp", r.pole_pairs);
  recordKeyInt("dir", r.sensor_direction);
  recordKeyFloat("offset", r.electrical_offset, 6);
  recordKeyFloat("r", r.phase_resistance, 6);
  recordKeyFloat("ld", r.inductance_d, 9);
  recordKeyFloat("lq", r.inductance_q, 9);
  recordKeyBool("valid", calibrationRecordValid(r));
  recordEnd();
}

void emitCfgRecord() {
  recordBegin("cfg");
  recordKeyHex("canid", config.can_id);
  recordKeyUint("bw_req", timing_config.requested_bandwidth_hz);
  recordKeyUint("bw_act", timing_config.active_bandwidth_hz);
  recordKeyBool("bw_clamped", timing_config.clamped);
  recordKeyUint("rate", timing_config.control_rate_hz);
  recordKeyUint("carrier", timing_config.carrier_hz);
  recordKeyUint("decim", timing_config.decimation);
  recordKeyUint("mode1", motor_state[0].mode);
  recordKeyUint("mode2", motor_state[1].mode);
  recordKeyUint("busmin_mv", config.bus_voltage_min_mv);
  recordKeyUint("busmax_mv", config.bus_voltage_max_mv);
  recordKeyBool("calibrated", config.calibrated);
  recordEnd();
}

void emitMotorRecord(uint8_t index) {
  const MotorImpedanceState &s = motor_state[index];
  BLDCMotor &motor = motorByIndex(index);

  recordBegin("motor");
  recordKeyUint("m", index + 1);
  recordKeyBool("armed", s.armed);
  recordKeyUint("mode", s.mode);
  recordKeyInt("pos_mrad", canRadToMrad(motor.shaft_angle));
  recordKeyFloat("vel", motor.shaft_velocity, 3);
  recordKeyFloat("iq", motor.current.q, 3);
  recordKeyBool("timeout", s.timed_out);
  recordKeyUint("limits", s.limit_cause);
  recordKeyUint("limitcount", s.limit_event_count);
  recordKeyOptionalBool("pairfault", s.pair_fault_latched, true);
  recordEnd();
}

void emitImpRecord(uint8_t index) {
  const MotorImpedanceState &s = motor_state[index];
  const __FlashStringHelper *ignored = nullptr;

  recordBegin("imp");
  recordKeyUint("m", index + 1);
  recordKeyInt("pdes_mrad", s.p_des_mrad);
  recordKeyFloat("vdes", s.v_des, 3);
  recordKeyFloat("kp", s.kp, 3);
  recordKeyFloat("kd", s.kd, 3);
  recordKeyFloat("tff", s.t_ff, 4);
  recordKeyFloat("perr", s.last_position_error, 5);
  recordKeyFloat("tq", s.last_torque_cmd, 4);
  recordKeyInt("applied_mrad", s.applied_target_mrad);
  recordKeyUint("capgen", s.capture_generation);
  recordKeyInt("seq", s.has_applied_seq ? (int32_t)s.last_applied_seq : -1);
  recordKeyOptionalBool("pairfault", s.pair_fault_latched, true);
  recordKeyBool("eligible", impedanceEligible(index, &ignored));
  recordKeyBool("hold", s.serial_hold);
  recordEnd();
}

void emitTimingRecord() {
  recordBegin("timing");
  recordKeyUint("rate_nom", timing_config.control_rate_hz);
  recordKeyFloat("rate_meas", foc_timing.measured_rate_hz, 1);
  recordKeyFloat("period_us", timing_config.control_period_us, 2);
  recordKeyUint("cycles", foc_timing.cycle_count);
  recordKeyUint("overruns", foc_timing.overrun_count);
  recordKeyUint("consec", foc_timing.consecutive_overruns);
  recordKeyUint("last_us", foc_timing.last_cycle_us);
  recordKeyUint("worst_us", foc_timing.worst_cycle_us);
  const float duty = (timing_config.control_period_us > 0.0f)
                         ? ((float)foc_timing.worst_cycle_us * 100.0f / timing_config.control_period_us)
                         : 0.0f;
  recordKeyFloat("duty", duty, 1);
  recordKeyToken("fault", foc_timing.overrun_fault ? "overrun"
                                                   : (foc_timing.timing_fault ? "rate" : "none"));
  recordEnd();
}

void emitBusRecord() {
  recordBegin("bus");
  recordKeyUint("mv", (uint32_t)(current_vbus * 1000.0f));
  recordKeyBool("protect", bus_voltage_protection_active);
  recordEnd();
}

void emitAllRecords() {
  emitIdRecord();
  emitCalRecord(0);
  emitCalRecord(1);
  emitCfgRecord();
  emitMotorRecord(0);
  emitMotorRecord(1);
  emitImpRecord(0);
  emitImpRecord(1);
  emitTimingRecord();
  emitBusRecord();
}

// Telemetry streaming period. 0 disables. Floored so telemetry can never crowd out the loop.
static const uint16_t QP_MIN_PERIOD_MS = 50;
static uint16_t telemetry_period_ms = 0;
static uint32_t telemetry_last_ms = 0;

void doQuery(char *cmd) {
  const char action = (cmd == nullptr) ? '\0' : cmd[0];
  switch (action) {
    case '\0':
      emitAllRecords();
      ackCurrentCommand(true, F(""));
      return;
    case 'C':
      emitCalRecord(0);
      emitCalRecord(1);
      emitCfgRecord();
      ackCurrentCommand(true, F(""));
      return;
    case 'T':
      emitTimingRecord();
      ackCurrentCommand(true, F(""));
      return;
    case 'I':
      emitMotorRecord(0);
      emitMotorRecord(1);
      emitImpRecord(0);
      emitImpRecord(1);
      ackCurrentCommand(true, F(""));
      return;
    case 'P': {
      const long requested = atol(cmd + 1);
      if (requested <= 0) {
        telemetry_period_ms = 0;
      } else {
        telemetry_period_ms = (requested < QP_MIN_PERIOD_MS) ? QP_MIN_PERIOD_MS : (uint16_t)requested;
      }
      Serial.print(F("QP applied [ms]: "));
      Serial.println(telemetry_period_ms);
      ackCurrentCommand(true, F(""));
      return;
    }
    default:
      ackCurrentCommand(false, F("unknown query"));
      emitFaultRecord("protocol", F("unknown query"));
      return;
  }
}

// Owns the serial input stream. Replaces Commander::run() so that the abort byte can be intercepted
// before line assembly, and so a request tag can be stripped and remembered.
void pumpSerial() {
  static char line[64];
  static uint8_t length = 0;

  while (Serial.available() > 0) {
    const int raw = Serial.read();
    if (raw < 0) break;
    const uint8_t byte = (uint8_t)raw;

    // Abort takes precedence over everything, including a half-typed command. Without this branch
    // the byte would be buffered as command text, emerge as a nonsense command, and the operator's
    // stop press would be met with silence.
    if (byte == SERIAL_ABORT_BYTE) {
      length = 0;
      disarmAllMotors();
      impedanceZeroEffort(motor_state[0]);
      impedanceZeroEffort(motor_state[1]);
      recordBegin("ack");
      recordKeyUint("tag", 0);
      recordKeyToken("cmd", "ABORT");
      recordKeyBool("ok", true);
      recordKeyText("reason", F(""));
      recordEnd();
      Serial.println(F("ABORT: motors disarmed"));
      continue;
    }

    if (byte == '\n' || byte == '\r') {
      if (length == 0) continue;
      line[length] = '\0';

      char *body = line;
      active_command_tag = 0;

      // Optional "#<tag>;" prefix. A human typing at the same console omits it and is unaffected.
      if (body[0] == '#') {
        char *semi = strchr(body, ';');
        if (semi != nullptr) {
          *semi = '\0';
          active_command_tag = (uint8_t)atoi(body + 1);
          body = semi + 1;
        }
      }

      strncpy(active_command_text, body, sizeof(active_command_text) - 1);
      active_command_text[sizeof(active_command_text) - 1] = '\0';

      command.run(body);
      length = 0;
      continue;
    }

    if (length < sizeof(line) - 1) line[length++] = (char)byte;
  }
}

void commsCycle() {
  if (fail_closed_pending) {
    fail_closed_pending = false;
    Serial.print(F("TIMING FAULT: motors disarmed ("));
    if (foc_timing.overrun_fault) Serial.print(F("sustained control-cycle overruns"));
    else if (foc_timing.timing_fault) Serial.print(F("measured rate diverged from nominal"));
    else Serial.print(F("unknown cause"));
    Serial.print(F("), overruns="));
    Serial.print(foc_timing.overrun_count);
    Serial.print(F(" measured rate [Hz]="));
    Serial.println(foc_timing.measured_rate_hz, 1);
    emitFaultRecord("timing", foc_timing.overrun_fault
                                  ? F("sustained control-cycle overruns")
                                  : (foc_timing.timing_fault ? F("measured rate divergence")
                                                             : F("unknown timing fault")));
  }

  reportPendingTimeouts();

  volt_samples++;
  sum_mV += analogReadMilliVolts(PIN_DCBUS_S);
  if (volt_samples == 3000) {
    float vSense = (sum_mV / 3000.0) / 1000.0f;
    float vbus_volts = vSense * DIVIDER_GAIN;
    current_vbus = vbus_volts;
    updateBusVoltageProtection(vbus_volts);
    // Serial.print(F("VBUS Volts: "));
    // Serial.println(vbus_volts);
    sum_mV = 0;
    volt_samples = 0;
    // if (vbus_volts <= 11.2) {
    //   steering = 0;
    //   throttle = 0;
    //   stop = true;
    // }
  }

  // User communication. pumpSerial() owns the input stream so the abort byte is intercepted before
  // line assembly and a request tag can be stripped; it calls command.run(line) per command.
  pumpSerial();

  if (telemetry_period_ms > 0 && (millis() - telemetry_last_ms) >= telemetry_period_ms) {
    telemetry_last_ms = millis();
    emitMotorRecord(0);
    emitMotorRecord(1);
    emitImpRecord(0);
    emitImpRecord(1);
    emitBusRecord();
  }

  uint32_t alerts_triggered = 0;
  twai_read_alerts(&alerts_triggered, pdMS_TO_TICKS(POLLING_RATE_MS));
  twai_status_info_t status_info;
  twai_get_status_info(&status_info);
  if (alerts_triggered & TWAI_ALERT_ERR_PASS) {
    Serial.println(F("TWAI Error Passive Alert Triggered"));
  }

  if (alerts_triggered & TWAI_ALERT_BUS_OFF) {
    Serial.println(F("TWAI Bus Off Alert Triggered"));
    Serial.printf(F("Bus error Count: %lu\n"), status_info.bus_error_count);
  }

  if (alerts_triggered & TWAI_ALERT_TX_FAILED) {
    Serial.println(F("TWAI Transmit Failed Alert Triggered"));
    Serial.printf(F("TX buffered: %lu\t"), status_info.msgs_to_tx);
    Serial.printf(F("TX errors: %lu\t"), status_info.tx_error_counter);
    Serial.printf(F("TX failed: %lu\n"), status_info.tx_failed_count);
  }

  // if (alerts_triggered & TWAI_ALERT_TX_SUCCESS) {
  //   Serial.println(F("TWAI Transmit Success Alert Triggered"));
  //   Serial.printf(F("TX buffered: %lu\t"), status_info.msgs_to_tx);
  // }

  if (alerts_triggered & TWAI_ALERT_RX_DATA) {
    handle_can();
  }
  // Serial.print(F("alerts_triggered: 0x"));
  // Serial.println(alerts_triggered, HEX);
  send_can_status();
}
