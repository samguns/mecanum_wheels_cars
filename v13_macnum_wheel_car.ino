#include <WiFi.h>
#include <AsyncTCP.h>
#include <SimpleFOC.h>
#include <ESPAsyncWebServer.h>
#include <WebSerialLite.h>
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
  // CAN ID, default to 0x201 for the front wheels
  uint16_t can_id = 0x201;
};

AsyncWebServer server(80);
const char* ssid = "";
const char* password = "";
static const int PIN_DCBUS_S = 38;      // GPIO38
static float DIVIDER_GAIN = 11.0; // (10+1)/1 = 11
static int volt_samples = 0;
static uint32_t sum_mV = 0;
float current_vbus = 0.0f;
RobotConfig config = RobotConfig { can_id: 0x201 };

// BLDC motor & driver instance
BLDCMotor motor1 = BLDCMotor(7);
BLDCMotor motor2 = BLDCMotor(7);
BLDCDriver3PWM driver1 = BLDCDriver3PWM(25, 32, 26, 33);
BLDCDriver3PWM driver2 = BLDCDriver3PWM(27, 14, 13, 12);

// encoder instance
MagneticSensorSPI sensor1 = MagneticSensorSPI(AS5147_SPI, 5);
MagneticSensorSPI sensor2 = MagneticSensorSPI(AS5147_SPI, 21);

// inline current sensor instance
// ACS712-05B has the resolution of 0.185mV per Amp
InlineCurrentSense current_sense1 = InlineCurrentSense(0.01, 50.0, 34, 35, NOT_SET);
InlineCurrentSense current_sense2 = InlineCurrentSense(0.01, 50.0, 37, 36, NOT_SET);

// commander communication instance
Commander command = Commander(Serial);
// void doMotion(char* cmd){ command.motion(&motor1, cmd); }
void doMotor1(char* cmd){ command.motor(&motor1, cmd); }
void doMotor2(char* cmd){ command.motor(&motor2, cmd); }

// Time tracking for CAN
unsigned long last_can_status_time = 0;
const unsigned long CAN_STATUS_INTERVAL_MS = 10; // 100Hz = 10ms
static const int POLLING_RATE_MS = 5; // Polling rate for CAN alerts in 200Hz
static uint8_t status_sent_count = 0;

void TaskFOC(void *pvParams);

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
      motor1.target = 0;
      motor2.target = 0;
      motor1.disable(); // Or just set target to 0? Protocol says latch.
      motor2.disable();
    }
    // 2. Command Message (0x200 + NodeID)
    else if (message.identifier == config.can_id) {
       if (message.data_length_code == 8) {
         // Byte 0: cmd (0x01 = set velocity)
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
            if(motor1.enabled) motor1.disable();
            if(motor2.enabled) motor2.disable();
            motor1.target = 0;
            motor2.target = 0;
         } else if (enable) {
            if(!motor1.enabled) motor1.enable();
            if(!motor2.enabled) motor2.enable();
         } else {
             // If not enabled, maybe disable? Or just coast?
             // Usually if enable bit is low, we disable the motors.
             if(motor1.enabled) motor1.disable();
             if(motor2.enabled) motor2.disable();
         }

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

         if (enable && !estop) {
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
  if (n != sizeof(struct RobotConfig)) {
    Serial.print(F("Invalid config size n: "));
    Serial.println(n);
    Serial.print(F("struct RobotConfig size: "));
    Serial.println(sizeof(struct RobotConfig));
    prefs.end();
    return false;
  }

  prefs.getBytes("cfg", &config, sizeof(struct RobotConfig));
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

void onWebSerialMsg(uint8_t *data, size_t len) {
  String d = "";
  for (int i = 0; i < len; i++) {
    d += char(data[i]);
  }
  if (data[0] == 'a') {
    doMotor1((char *)(&data[1]));
  } else if (data[0] == 'b') {
    doMotor2((char *)(&data[1]));
  } else if (data[0] == 'c') {
    if (data[1] == '1') {
      config.can_id = 0x201;
      saveConfig(config);
    } else if (data[1] == '2') {
      config.can_id = 0x202;
      saveConfig(config);
    }
  } else {
    WebSerial.print(F("Unknown command: "));
  }
  WebSerial.println(d);
}

bool setup_webserial() {
  WiFi.mode(WIFI_STA);
  WiFi.begin(ssid, password);

  if (WiFi.waitForConnectResult() != WL_CONNECTED) {
    Serial.printf(F("WiFi Failed!\n"));
    return false;
  }
  Serial.print(F("IP: "));
  Serial.println(WiFi.localIP());

  WebSerial.begin(&server);
  WebSerial.onMessage(onWebSerialMsg);
  server.begin();

  return true;
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
    Serial.println(F("Config loaded successfully"));
  } else {
    Serial.println(F("No valid config, save and use defaults"));
    saveConfig(config);
  }

  if (!setup_webserial()) {
    return;
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

  // motor1.zero_electric_angle = 5.779;
  motor1.sensor_direction = Direction::CW;
  motor2.sensor_direction = Direction::CCW;

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
  // align encoder and start FOC
  motor1.initFOC();
  motor2.initFOC();
  // set the inital target value
  // motor1.target = 0;

  // subscribe motor to the commander
  // command.add('p', doMotion, "motion control");
  command.add('a', doMotor1, "motor1");
  command.add('b', doMotor2, "motor2");

  // comment out if not needed
  motor1.useMonitoring(Serial);
  // motor1.monitor_downsample = 0;
  // motor1.monitor_variables = _MON_TARGET | _MON_VEL;

  motor2.useMonitoring(Serial);
  // motor2.monitor_downsample = 0;
  // motor2.monitor_variables = _MON_TARGET | _MON_VEL;

  // Run user commands to configure and the motor (find the full command list in docs.simplefoc.com)
  Serial.print(F("Motors are ready. Current CAN ID: 0x"));
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
    // iterative setting FOC phase voltage
    motor1.loopFOC();
    motor2.loopFOC();

    // iterative function setting the outter loop target
    motor1.move();
    motor2.move();
  }
}

void loop() {
  

  volt_samples++;
  sum_mV += analogReadMilliVolts(PIN_DCBUS_S);
  if (volt_samples == 3000) {
    float vSense = (sum_mV / 3000.0) / 1000.0f;
    float vbus_volts = vSense * DIVIDER_GAIN;
    current_vbus = vbus_volts;
    WebSerial.print(F("VBUS Volts: "));
    WebSerial.println(vbus_volts);
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