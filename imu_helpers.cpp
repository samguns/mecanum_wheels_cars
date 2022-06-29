#include "imu_helpers.h"
#include "./src/I2Cdev/I2Cdev.h"
#include "./src/MPU6050/MPU6050_6Axis_MotionApps612.h"
#include "Wire.h"

// IMU instance
MPU6050 mpu;
// MPU control/status vars
bool imuReady = false;  // set true if DMP init was successful
uint8_t mpuIntStatus;   // holds actual interrupt status byte from MPU
uint8_t devStatus;      // return status after each device operation (0 = success, !0 = error)
uint16_t packetSize;    // expected DMP packet size (default is 42 bytes)
uint16_t fifoCount;     // count of all bytes currently in FIFO
uint8_t fifoBuffer[64]; // FIFO storage buffer

// imu related finctions
// check if IMU has received data
int hasDataIMU(){
  return imuReady && mpu.dmpGetCurrentFIFOPacket(fifoBuffer);
}

// read the pitch value from the IMU
float getPitchIMU(){
  // static variable used for debouncing
  static float pitch;
  Quaternion q;           // [w, x, y, z]         quaternion container
  
  // read the package
  mpu.dmpGetQuaternion(&q, fifoBuffer);
  
  // calculate the angle of the robot
  float pitch_new = -_PI_2 + atan2(q.w * q.w - q.x * q.x - q.y * q.y + q.z * q.z, 2 * (q.y * q.z + q.w * q.x));
  
  // a bit of debouncing
  if (abs(pitch_new - pitch) > 0.1) pitch += _sign(pitch_new - pitch) * 0.01;
  else pitch = pitch_new;
  
  return pitch; 
}

// 从IMU获取经过防抖处理的横滚角（Roll Angle）
float getRollIMU(){
  // static变量：跨函数调用保存上一次的横滚角，用于防抖逻辑（数据持久化）
  static float roll;
  Quaternion q;           // [w, x, y, z] 四元数容器，用于存储IMU姿态数据
  
  // 从IMU的DMP（数字运动处理器）中读取四元数数据到q结构体
  mpu.dmpGetQuaternion(&q, fifoBuffer);
  
  // 四元数转换为原始横滚角（roll_new），核心姿态解算公式
  float roll_new = atan2(2 * (q.x * q.z - q.w * q.y), q.w * q.w - q.x * q.x + q.y * q.y - q.z * q.z);
  
  // 与getPitchIMU一致的防抖（debouncing）逻辑，保证输出平稳
  if (abs(roll_new - roll) > 0.1) roll += _sign(roll_new - roll) * 0.01;
  else roll = roll_new;
  
  // 返回经过防抖处理后的平稳横滚角
  return roll; 
}

void getPitchRoll(float *pitch_val, float *roll_val) {
  static float pitch;
  static float roll;
  Quaternion q;           // [w, x, y, z]         quaternion container
  
  // read the package
  mpu.dmpGetQuaternion(&q, fifoBuffer);
  
  // calculate the angle of the robot
  float pitch_new = -_PI_2 + atan2(q.w * q.w - q.x * q.x - q.y * q.y + q.z * q.z, 2 * (q.y * q.z + q.w * q.x));
  float roll_new = atan2(2 * (q.x * q.z - q.w * q.y), q.w * q.w - q.x * q.x + q.y * q.y - q.z * q.z);
  
  // a bit of debouncing
  if (abs(pitch_new - pitch) > 0.1) pitch += _sign(pitch_new - pitch) * 0.01;
  else pitch = pitch_new;

  if (abs(roll_new - roll) > 0.1) roll += _sign(roll_new - roll) * 0.01;
  else roll = roll_new;

  *pitch_val = pitch;
  *roll_val = roll;
}

void getPitchRollYaw(float *pitch_val, float *roll_val, float *yaw_val) {
  // 使用静态变量来保存上一次的姿态角，实现状态保持和滤波
  static float pitch = 0.0f;
  static float roll = 0.0f;
  static float yaw = 0.0f;
  
  Quaternion q; // [w, x, y, z] quaternion container
  
  // 1. 从FIFO缓冲区读取四元数
  mpu.dmpGetQuaternion(&q, fifoBuffer);
  
  // 2. 使用DMP内置函数直接计算欧拉角，参数顺序是 [roll, pitch, yaw]
  float euler[3];
  mpu.dmpGetEuler(euler, &q);
  
  // 3. 对三个轴的角度都应用平滑滤波（状态机/低通滤波）
  const float threshold = 0.1f;  // 抖动阈值
  const float step = 0.01f;      // 步进值

  // --- 处理俯仰角 (Pitch) ---
  if (fabs(euler[1] - pitch) > threshold) {
    pitch += _sign(euler[1] - pitch) * step;
  } else {
    pitch = euler[1];
  }

  // --- 处理横滚角 (Roll) ---
  if (fabs(euler[0] - roll) > threshold) {
    roll += _sign(euler[0] - roll) * step;
  } else {
    roll = euler[0];
  }

  // --- 处理偏航角 (Yaw) ---
  if (fabs(euler[2] - yaw) > threshold) {
    yaw += _sign(euler[2] - yaw) * step;
  } else {
    yaw = euler[2];
  }
  
  // 4. 将最终结果通过指针返回
  *pitch_val = pitch;
  *roll_val = roll;
  *yaw_val = yaw;
}

// initialise and configure the IMU with DMP
int initIMU() {
  Wire.begin(15, 22, 400000);
  // Wire.setClock(400000); // 400kHz I2C clock. Comment this line if having compilation difficulties
  // initialize device
  Serial.println(F("Initializing I2C devices..."));
  mpu.initialize();
  mpu.setClockSource(MPU6050_CLOCK_PLL_ZGYRO);
  mpu.setFullScaleGyroRange(MPU6050_GYRO_FS_2000);
  mpu.setFullScaleAccelRange(MPU6050_ACCEL_FS_2);
  mpu.setSleepEnabled(false);

  // verify connection
  Serial.println(F("Testing device connections..."));
  Serial.println(mpu.testConnection() ? F("MPU6050 connection successful") : F("MPU6050 connection failed"));

  // load and configure the DMP
  Serial.println(F("Initializing DMP..."));
  devStatus = mpu.dmpInitialize();
  // make sure it worked (returns 0 if so)
  if (devStatus == 0) {
    // Calibration Time: generate offsets and calibrate our MPU6050
    // mpu.CalibrateAccel(6);
    mpu.CalibrateGyro(6);
    Serial.println();
    mpu.PrintActiveOffsets();
    // turn on the DMP, now that it's ready
    Serial.println(F("Enabling DMP..."));
    mpu.setDMPEnabled(true);

    // set our DMP Ready flag so the main loop() function knows it's okay to use it
    Serial.println(F("DMP ready! Waiting for first interrupt..."));
    mpuIntStatus = mpu.getIntStatus();
    imuReady = true;

    // get expected DMP packet size for later comparison
    packetSize = mpu.dmpGetFIFOPacketSize();
  } else {
    // ERROR!
    // 1 = initial memory load failed
    // 2 = DMP configuration updates failed
    // (if it's going to break, usually the code will be 1)
    Serial.print(F("DMP Initialization failed (code "));
    Serial.print(devStatus);
    Serial.println(F(")"));
  }
  delay(2000);
  Serial.println(F("Adjusting DMP sensor fusion gain..."));
  mpu.setMemoryBank(0);
  mpu.setMemoryStartAddress(0x60);
  mpu.writeMemoryByte(0);
  mpu.writeMemoryByte(0x20);
  mpu.writeMemoryByte(0);
  mpu.writeMemoryByte(0);
  
  return imuReady;
}
