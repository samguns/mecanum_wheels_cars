#include <SimpleFOC.h>
#include <Preferences.h>
#include <driver/twai.h>

// #include "imu_helpers.h"

#define RX_PIN 9
#define TX_PIN 10

#if CONFIG_FREERTOS_UNICORE
#define FOC_RUNNING_CORE 0
#else
#define FOC_RUNNING_CORE 1
#endif

Preferences prefs;

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
    float current_bandwidth;
  } motor[2];
  bool calibrated;
};

static const uint32_t CONFIG_MAGIC = 0x4D43414C; // "MCAL"
static const uint16_t CONFIG_VERSION = 1;
static const uint16_t DEFAULT_CAN_ID = 0x201;
static const float CALIBRATION_VOLTAGE = 4.0f;
static const float ALIGNMENT_VOLTAGES[] = {0.75f, 1.0f};
static const float CALIBRATION_WORKING_CURRENT = 1.0f;
static const float CALIBRATION_ABORT_CURRENT = 1.5f;
static const uint32_t ALIGNMENT_TIMEOUT_MS = 30000;
static const uint32_t CHARACTERIZATION_TIMEOUT_MS = 15000;
static const uint32_t COOLDOWN_MS = 30000;

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
MagneticSensorSPI sensor1 = MagneticSensorSPI(AS5147_SPI, 5);
MagneticSensorSPI sensor2 = MagneticSensorSPI(AS5147_SPI, 21);

// inline current sensor instance
// ACS712-05B has the resolution of 0.185mV per Amp
InlineCurrentSense current_sense1 = InlineCurrentSense(0.01, 50.0, 34, 35, NOT_SET);
InlineCurrentSense current_sense2 = InlineCurrentSense(0.01, 50.0, 37, 36, NOT_SET);

enum class ControllerMode : uint8_t { Normal, Calibration };
enum class CalibrationOperation : uint8_t { Idle, AlignmentPending, CharacteristicsPending, Cooldown };
ControllerMode controller_mode = ControllerMode::Calibration;
CalibrationOperation calibration_operation = CalibrationOperation::Idle;
int8_t selected_motor = -1;
RobotConfig::MotorCalibration pending_calibration = {};
uint32_t cooldown_until = 0;
bool foc_initialized = false;

// commander communication instance
Commander command = Commander(Serial);
// void doMotion(char* cmd){ command.motion(&motor1, cmd); }
void doMotor1(char* cmd){ if (controller_mode == ControllerMode::Normal) command.motor(&motor1, cmd); else Serial.println(F("CAL motor command rejected")); }
void doMotor2(char* cmd){ if (controller_mode == ControllerMode::Normal) command.motor(&motor2, cmd); else Serial.println(F("CAL motor command rejected")); }

// Motor power is deliberately controlled separately from the velocity target.
// Keeping this in one place makes every disarm path leave the bridge unpowered.
void disarmMotor(BLDCMotor &motor) {
  motor.target = 0.0f;
  if (motor.enabled) {
    motor.disable();
  }
}

void armMotor(BLDCMotor &motor) {
  // An arm command must never apply a target that was received before disarming.
  motor.target = 0.0f;
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

bool calibrationCoolingDown() {
  if (calibration_operation != CalibrationOperation::Cooldown) return false;
  if ((int32_t)(millis() - cooldown_until) >= 0) {
    resetCalibrationSession();
    return false;
  }
  return true;
}

void failCalibration(const __FlashStringHelper *reason, bool cooldown = false) {
  disarmAllMotors();
  memset(&pending_calibration, 0, sizeof(pending_calibration));
  Serial.print(F("CAL ERROR: "));
  Serial.println(reason);
  if (cooldown) {
    calibration_operation = CalibrationOperation::Cooldown;
    cooldown_until = millis() + COOLDOWN_MS;
    Serial.println(F("CAL COOLDOWN: 30 seconds"));
  } else {
    calibration_operation = CalibrationOperation::Idle;
  }
}

bool calibrationAbortRequested() {
  twai_message_t message;
  while (twai_receive(&message, 0) == ESP_OK) {
    if (message.identifier == 0x080 ||
        (message.identifier == config.can_id && message.data_length_code >= 2 && (message.data[1] & 0x08))) {
      failCalibration(F("emergency stop"));
      return true;
    }
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
    failCalibration(F("over-current abort"), true);
    return false;
  }
  if (measuredCurrent >= CALIBRATION_WORKING_CURRENT) {
    failCalibration(F("working-current limit"), true);
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
  motor.pole_pairs = polePairs;
  motor.sensor_direction = movement > 0.0f ? Direction::CW : Direction::CCW;
  // Align at -90 electrical degrees and record the corresponding sensor offset.
  motor.setPhaseVoltage(selectedVoltage, 0.0f, _3PI_2);
  delay(700);
  if (!calibrationGuard(index, motor, startedAt, ALIGNMENT_TIMEOUT_MS)) return false;
  sensor1.update(); sensor2.update();
  float mechanical = index == 0 ? sensor1.getMechanicalAngle() : sensor2.getMechanicalAngle();
  pending_calibration = {};
  pending_calibration.pole_pairs = polePairs;
  pending_calibration.sensor_direction = motor.sensor_direction == Direction::CW ? 1 : -1;
  pending_calibration.electrical_offset = normalizeAngle((float)(pending_calibration.sensor_direction * polePairs) * mechanical - _3PI_2);
  pending_calibration.flags = 0x01;
  motor.setPhaseVoltage(0.0f, 0.0f, 0.0f);
  disarmAllMotors();
  calibration_operation = CalibrationOperation::AlignmentPending;
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
  // Find the strongest useful test voltage without deliberately crossing the
  // 1 A working-current limit. A fixed 4 V test is unsafe for low-resistance motors.
  for (float voltage = 0.25f; voltage <= CALIBRATION_VOLTAGE; voltage += 0.05f) {
    motor.setPhaseVoltage(voltage, 0.0f, angle);
    delay(5);
    if (calibrationAbortRequested()) return false;
    if (millis() - startedAt > CHARACTERIZATION_TIMEOUT_MS) { failCalibration(F("timeout")); return false; }
    float measuredCurrent = calibrationCurrent(index, angle);
    if (measuredCurrent >= CALIBRATION_ABORT_CURRENT) { failCalibration(F("over-current abort"), true); return false; }
    if (measuredCurrent >= CALIBRATION_WORKING_CURRENT) break;
    float netCurrent = measuredCurrent - zeroCurrent;
    if (netCurrent >= 0.2f) {
      testVoltage = voltage;
      testCurrent = netCurrent;
    }
  }
  motor.setPhaseVoltage(0.0f, 0.0f, angle);
  if (testVoltage <= 0.0f || testCurrent < 0.2f) { failCalibration(F("current too low"), true); return false; }
  Serial.print(F("CAL characterization voltage [V]: ")); Serial.println(testVoltage, 2);
  float resistance = testVoltage / (1.5f * testCurrent);
  if (!finiteInRange(resistance, 0.01f, 100.0f)) { failCalibration(F("invalid resistance"), true); return false; }
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
      if (current <= 0.0f || current >= CALIBRATION_ABORT_CURRENT) { failCalibration(F("invalid inductance current"), true); return false; }
      float dt = (pulseEnd - pulseStart) / 1000000.0f;
      float denominator = testVoltage - resistance * current;
      if (denominator <= 0.0f) { failCalibration(F("invalid inductance sample"), true); return false; }
      average += fabsf(-(resistance * dt) / logf(denominator / testVoltage)) / 1.5f;
      delay(2);
    }
    inductance[axis] = average / 12.0f;
    if (!finiteInRange(inductance[axis], 0.000001f, 0.1f)) { failCalibration(F("invalid inductance"), true); return false; }
  }
  pending_calibration = config.motor[index];
  pending_calibration.phase_resistance = resistance;
  pending_calibration.inductance_d = inductance[0];
  pending_calibration.inductance_q = inductance[1];
  pending_calibration.current_bandwidth = 100.0f;
  pending_calibration.flags |= 0x02;
  disarmAllMotors();
  calibration_operation = CalibrationOperation::CharacteristicsPending;
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
  if (calibrationCoolingDown()) { Serial.print(F("CAL COOLDOWN MS: ")); Serial.println(cooldown_until - millis()); }
  Serial.println(F("CAL LIMITS: 4.0V, 1.0A working, 1.5A abort"));
}

void doCalibration(char* cmd) {
  char action = cmd[0];
  if (action == 'P') { printCalibrationStatus(); return; }
  if (action == '1' || action == '2') { enterCalibrationMode(); selected_motor = action - '1'; Serial.println(F("CAL selected motor")); Serial.println(selected_motor + 1); return; }
  if (action == 'X') { enterCalibrationMode(); Serial.println(F("CAL cancelled")); return; }
  if (action == 'E') {
    if (configIsComplete(config)) {
      applyCalibrationToMotor(0); applyCalibrationToMotor(1);
      if (!foc_initialized) { motor1.initFOC(); motor2.initFOC(); foc_initialized = true; }
      controller_mode = ControllerMode::Normal; selected_motor = -1; disarmAllMotors();
      Serial.println(F("CAL normal mode (disarmed)"));
    } else Serial.println(F("CAL incomplete"));
    return;
  }
  if (controller_mode != ControllerMode::Calibration || selected_motor < 0) { Serial.println(F("CAL select motor with C1 or C2")); return; }
  if (calibrationCoolingDown()) { Serial.println(F("CAL cooldown active")); return; }
  uint8_t index = (uint8_t)selected_motor;
  if (action == 'A') { if (runAlignmentCalibration(index)) { Serial.print(F("CAL PENDING ALIGN M")); Serial.println(index + 1); } return; }
  if (action == 'M') { if (!(config.motor[index].flags & 0x01)) { Serial.println(F("CAL confirm alignment first")); return; } if (runCharacteristicsCalibration(index)) { Serial.print(F("CAL PENDING CHARACTERISTICS M")); Serial.println(index + 1); } return; }
  if (action == 'N') { resetCalibrationSession(); disarmAllMotors(); Serial.println(F("CAL pending result rejected")); return; }
  if (action == 'Y') {
    if (calibration_operation == CalibrationOperation::AlignmentPending || calibration_operation == CalibrationOperation::CharacteristicsPending) {
      config.motor[index] = pending_calibration;
      applyCalibrationToMotor(index);
      if (calibration_operation == CalibrationOperation::CharacteristicsPending && motorByIndex(index).tuneCurrentController(config.motor[index].current_bandwidth) != 0) {
        failCalibration(F("current PID tuning failed")); return;
      }
      configIsComplete(config); saveConfig(config); resetCalibrationSession(); Serial.println(F("CAL result saved"));
    } else Serial.println(F("CAL no pending result"));
    return;
  }
  Serial.println(F("CAL commands: C, C1, C2, CA, CM, CY, CN, CX, CE"));
}

// Serial commands: A1/A2/A0 arm motor 1/motor 2/both; D1/D2/D0 disarm them.
// The existing SimpleFOC commands remain available as a ... and b ... .
void doArm(char* cmd) {
  if (controller_mode != ControllerMode::Normal) { Serial.println(F("CAL motion command rejected")); return; }
  switch (cmd[0]) {
    case '1': armMotor(motor1); break;
    case '2': armMotor(motor2); break;
    case '0': armAllMotors(); break;
    default:
      Serial.println(F("Usage: A1, A2, or A0"));
      return;
  }
  Serial.println(F("Motor armed"));
}

void doDisarm(char* cmd) {
  switch (cmd[0]) {
    case '1': disarmMotor(motor1); break;
    case '2': disarmMotor(motor2); break;
    case '0': disarmAllMotors(); break;
    default:
      Serial.println(F("Usage: D1, D2, or D0"));
      return;
  }
  Serial.println(F("Motor disarmed"));
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

void resetConfig(RobotConfig &value, uint16_t canId = DEFAULT_CAN_ID) {
  memset(&value, 0, sizeof(value));
  value.magic = CONFIG_MAGIC;
  value.version = CONFIG_VERSION;
  value.can_id = (canId >= 0x001 && canId <= 0x7FF) ? canId : DEFAULT_CAN_ID;
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

         // Bytes 4..5: target_left (Motor2)
         int16_t target_left_int = (int16_t)(message.data[4] | (message.data[5] << 8));
         float target_left = (float)target_left_int / 100.0f;

         // Bytes 6..7: target_right (Motor1)
         int16_t target_right_int = (int16_t)(message.data[6] | (message.data[7] << 8));
         float target_right = (float)target_right_int / 100.0f;

         if (enable) {
             motor2.target = target_left;
             motor1.target = target_right;
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
  if (loaded.magic != CONFIG_MAGIC || loaded.version != CONFIG_VERSION || loaded.can_id == 0 || loaded.can_id > 0x7FF) {
    uint16_t legacyCanId = DEFAULT_CAN_ID;
    if (n >= sizeof(legacyCanId)) prefs.getBytes("cfg", &legacyCanId, sizeof(legacyCanId));
    resetConfig(config, legacyCanId);
    prefs.end();
    return false;
  }
  config = loaded;
  configIsComplete(config);
  prefs.end();
  return true;
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

  // driver config
  // power supply voltage [V]
  driver1.voltage_power_supply = 12;
  driver2.voltage_power_supply = 12;
  driver1.pwm_frequency = 25000;
  driver1.init();
  driver2.init();
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

  xTaskCreatePinnedToCore(
    TaskFOC, "TaskFOC", 2048, NULL, 1, NULL, FOC_RUNNING_CORE
  );

  // setupFocTimer(); // 10kHz FOC loop
  // setupFocCommander(); // Commander task

  _delay(1000);
}

void TaskFOC(void *pvParams) {
  for (;;) {
    if (controller_mode == ControllerMode::Normal) {
      // iterative setting FOC phase voltage
      motor1.loopFOC();
      motor2.loopFOC();

      // iterative function setting the outter loop target
      motor1.move();
      motor2.move();
    }
    taskYIELD();
  }
}

void loop() {
  

  volt_samples++;
  sum_mV += analogReadMilliVolts(PIN_DCBUS_S);
  if (volt_samples == 3000) {
    float vSense = (sum_mV / 3000.0) / 1000.0f;
    float vbus_volts = vSense * DIVIDER_GAIN;
    current_vbus = vbus_volts;
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

  // user communication
  command.run();

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
