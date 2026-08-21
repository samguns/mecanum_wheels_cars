#include "foc_timing.h"

#include <soc/soc_caps.h>

#if defined(ESP_H) && defined(ARDUINO_ARCH_ESP32) && defined(SOC_MCPWM_SUPPORTED) && \
    !defined(SIMPLEFOC_ESP32_USELEDC)
#define FOC_TIMING_MCPWM 1
#include "driver/mcpwm_prelude.h"
#include "drivers/hardware_specific/esp32/esp32_driver_mcpwm.h"
#include "drivers/hardware_specific/esp32/mcpwm_private.h"
#include "esp_idf_version.h"
#endif

#if CONFIG_FREERTOS_UNICORE
#define FOC_TIMING_CORE 0
#else
#define FOC_TIMING_CORE 1
#endif

// Priority well above the Arduino loop task (which runs at 1) so communications work can
// never delay a control cycle (FR-017).
#define FOC_TIMING_TASK_PRIORITY 20
#define FOC_TIMING_TASK_STACK 4096

LoopTimingRecord foc_timing = {};

namespace {

FocCycleFn g_cycle = nullptr;
FocFailClosedFn g_fail_closed = nullptr;
TaskHandle_t g_task = nullptr;

volatile uint8_t g_decimation = 1;
volatile uint8_t g_isr_count = 0;

uint32_t g_nominal_rate_hz = 0;
bool g_running = false;

#ifdef FOC_TIMING_INSTRUMENT
int g_instrument_pin = -1;
bool g_instrument_level = false;
#endif

#ifdef FOC_TIMING_MCPWM
// Fired once per PWM period by the MCPWM timer. Kept minimal: count, decimate, release
// the control task. The timer keeps its own phase, so a late cycle never shifts the
// phase of subsequent cycles (FR-014).
bool IRAM_ATTR focOnFull(mcpwm_timer_handle_t timer, const mcpwm_timer_event_data_t *edata,
                         void *user_data) {
  (void)timer;
  (void)edata;
  (void)user_data;

  const uint8_t decim = g_decimation;
  uint8_t count = g_isr_count + 1;
  if (count < decim) {
    g_isr_count = count;
    return false;
  }
  g_isr_count = 0;

  BaseType_t higher_priority_woken = pdFALSE;
  if (g_task != nullptr) {
    vTaskNotifyGiveFromISR(g_task, &higher_priority_woken);
  }
  return higher_priority_woken == pdTRUE;
}
#endif

void focControlTask(void *arg) {
  (void)arg;

  uint32_t window_start_us = micros();
  uint32_t window_cycles = 0;

  for (;;) {
    // Returns the notification value BEFORE decrementing. A value above one means the
    // ISR fired again while the previous cycle was still executing: an overrun.
    const uint32_t pending = ulTaskNotifyTake(pdFALSE, portMAX_DELAY);

    const uint32_t cycle_start_us = micros();

#ifdef FOC_TIMING_INSTRUMENT
    if (g_instrument_pin >= 0) {
      g_instrument_level = !g_instrument_level;
      digitalWrite(g_instrument_pin, g_instrument_level ? HIGH : LOW);
    }
#endif

    if (g_cycle != nullptr) g_cycle();

    const uint32_t elapsed_us = micros() - cycle_start_us;
    foc_timing.last_cycle_us = elapsed_us;
    if (elapsed_us > foc_timing.worst_cycle_us) foc_timing.worst_cycle_us = elapsed_us;
    foc_timing.cycle_count++;
    window_cycles++;

    if (pending > 1) {
      const uint32_t missed = pending - 1;
      foc_timing.overrun_count += missed;
      const uint32_t consecutive = (uint32_t)foc_timing.consecutive_overruns + missed;
      foc_timing.consecutive_overruns =
          (consecutive > 0xFFFFu) ? 0xFFFFu : (uint16_t)consecutive;

      if (foc_timing.consecutive_overruns > FOC_MAX_CONSECUTIVE_OVERRUNS &&
          !foc_timing.overrun_fault) {
        foc_timing.overrun_fault = true;
        if (g_fail_closed != nullptr) g_fail_closed();
      }
    } else {
      foc_timing.consecutive_overruns = 0;
    }

    // Measured rate over a rolling one second window.
    const uint32_t window_elapsed_us = cycle_start_us - window_start_us;
    if (window_elapsed_us >= 1000000UL) {
      const float rate = (float)window_cycles * 1000000.0f / (float)window_elapsed_us;
      foc_timing.measured_rate_hz = rate;
      window_start_us = cycle_start_us;
      window_cycles = 0;

      // A rate that diverges without any overrun points at the time base itself, not at
      // execution time, so it is a distinct fault (FR-016).
      if (g_nominal_rate_hz > 0) {
        const float nominal = (float)g_nominal_rate_hz;
        const float deviation = fabsf(rate - nominal) / nominal;
        if (deviation > FOC_RATE_DEVIATION_TOLERANCE && foc_timing.overrun_count == 0) {
          if (!foc_timing.timing_fault) {
            foc_timing.timing_fault = true;
            if (g_fail_closed != nullptr) g_fail_closed();
          }
        }
      }
    }
  }
}

}  // namespace

bool focTimingBegin(void *driver_params, const FocTimingConfig &cfg, FocCycleFn cycle,
                    FocFailClosedFn fail_closed) {
  if (!cfg.valid || cycle == nullptr) return false;
  if (g_running) return false;

  g_cycle = cycle;
  g_fail_closed = fail_closed;
  g_decimation = (cfg.decimation < 1) ? 1 : cfg.decimation;
  g_isr_count = 0;
  g_nominal_rate_hz = cfg.control_rate_hz;
  foc_timing = LoopTimingRecord();

#ifndef FOC_TIMING_MCPWM
  (void)driver_params;
  return false;  // no MCPWM time base available on this target
#else
  if (driver_params == nullptr) return false;

  ESP32MCPWMDriverParams *params = (ESP32MCPWMDriverParams *)driver_params;
  mcpwm_timer_t *timer = (mcpwm_timer_t *)params->timers[0];
  if (timer == nullptr) return false;

  // The low-side current-sense path would claim the same callback. This firmware uses
  // inline sensing, so it must be free; refuse rather than silently fight over it.
  if (timer->on_full != nullptr) return false;

  if (xTaskCreatePinnedToCore(focControlTask, "FocControl", FOC_TIMING_TASK_STACK, nullptr,
                              FOC_TIMING_TASK_PRIORITY, &g_task,
                              FOC_TIMING_CORE) != pdPASS) {
    g_task = nullptr;
    return false;
  }

  // Registering a callback on a running timer is not supported by the public API, so the
  // timer is briefly walked back to its init state. This mirrors exactly what SimpleFOC
  // does for low-side sync in current_sense/hardware_specific/esp32/esp32_mcpwm_mcu.cpp.
  mcpwm_timer_event_callbacks_t callbacks = {};
  callbacks.on_full = focOnFull;

  const mcpwm_timer_fsm_t saved_fsm = timer->fsm;
  timer->fsm = MCPWM_TIMER_FSM_INIT;
  const esp_err_t registered =
      mcpwm_timer_register_event_callbacks(timer, &callbacks, nullptr);
  timer->fsm = saved_fsm;

  if (registered != ESP_OK) {
    vTaskDelete(g_task);
    g_task = nullptr;
    return false;
  }

  if (esp_intr_enable(timer->intr) != ESP_OK) {
    vTaskDelete(g_task);
    g_task = nullptr;
    return false;
  }

  g_running = true;
  return true;
#endif
}

void focTimingSetDecimation(uint8_t decimation) {
  g_decimation = (decimation < 1) ? 1 : decimation;
}

bool focTimingApplyCarrier(void *driver_params_a, void *driver_params_b, uint32_t carrier_hz,
                           uint32_t carrier_period_ticks) {
#ifndef FOC_TIMING_MCPWM
  (void)driver_params_a;
  (void)driver_params_b;
  (void)carrier_hz;
  (void)carrier_period_ticks;
  return false;
#else
#if ESP_IDF_VERSION < ESP_IDF_VERSION_VAL(5, 1, 0)
  // mcpwm_timer_set_period is unavailable; the caller reports "restart required".
  (void)driver_params_a;
  (void)driver_params_b;
  (void)carrier_hz;
  (void)carrier_period_ticks;
  return false;
#else
  if (driver_params_a == nullptr || carrier_period_ticks == 0) return false;
  if (carrier_hz < FOC_CARRIER_MIN_HZ || carrier_hz > FOC_CARRIER_MAX_HZ) return false;

  ESP32MCPWMDriverParams *pa = (ESP32MCPWMDriverParams *)driver_params_a;
  ESP32MCPWMDriverParams *pb = (ESP32MCPWMDriverParams *)driver_params_b;
  mcpwm_timer_handle_t timer = pa->timers[0];
  if (timer == nullptr) return false;

  if (mcpwm_timer_set_period(timer, carrier_period_ticks) != ESP_OK) return false;

  // SimpleFOC scales every comparator against params->mcpwm_period, and stores half the
  // period because the timer counts up and down (center aligned). Both drivers share the
  // timer, so both records must follow it or duty cycles silently rescale.
  const uint32_t half_period = carrier_period_ticks / 2u;
  pa->mcpwm_period = half_period;
  pa->pwm_frequency = (long)carrier_hz;
  if (pb != nullptr && pb->timers[0] == timer) {
    pb->mcpwm_period = half_period;
    pb->pwm_frequency = (long)carrier_hz;
  }
  return true;
#endif
#endif
}

void focTimingResetCounters() {
  const float rate = foc_timing.measured_rate_hz;
  foc_timing = LoopTimingRecord();
  foc_timing.measured_rate_hz = rate;
}

void focApplyMotorTuning(BLDCMotor &motor, float resistance, float inductance_d,
                         float inductance_q, float bandwidth_hz, float control_period_us) {
  const float omega = _2PI * bandwidth_hz;

  motor.PID_current_d.P = inductance_d * omega;
  motor.PID_current_d.I = resistance * omega;
  motor.PID_current_q.P = inductance_q * omega;
  motor.PID_current_q.I = resistance * omega;

  const float tf = 1.0f / (omega * 5.0f);
  motor.LPF_current_d.Tf = tf;
  motor.LPF_current_q.Tf = tf;

  focApplyMotorTimestep(motor, control_period_us);
}

void focApplyMotorTimestep(BLDCMotor &motor, float control_period_us) {
  const float ts = control_period_us * 1e-6f;

  // Setting Ts is what stops PIDController and LowPassFilter measuring their own elapsed
  // time, which is how loop jitter used to leak into effective gain (research D6).
  motor.PID_current_d.Ts = ts;
  motor.PID_current_q.Ts = ts;
  motor.PID_velocity.Ts = ts;
  motor.P_angle.Ts = ts;
  motor.LPF_current_d.Ts = ts;
  motor.LPF_current_q.Ts = ts;
  motor.LPF_velocity.Ts = ts;
  motor.LPF_angle.Ts = ts;
}

uint32_t focTimingSelfTest(Print &out) {
  uint32_t failures = 0;

  // --- Parser rejection cases ---
  struct {
    const char *text;
    bool expect_ok;
    uint16_t expect_value;
  } parse_cases[] = {
      {"100", true, 100},        {"1000", true, 1000},   {"10000", true, 10000},
      {"  500", true, 500},      {"99", false, 0},       {"10001", false, 0},
      {"0", false, 0},           {"", false, 0},         {"-100", false, 0},
      {"+100", false, 0},        {"1e3", false, 0},      {"1000x", false, 0},
      {"12 34", false, 0},       {"abc", false, 0},      {"1000.5", false, 0},
      {"999999", false, 0},      {" ", false, 0},        {"1 000", false, 0},
  };
  for (const auto &c : parse_cases) {
    uint16_t parsed = 0xFFFF;
    const bool ok = focParseBandwidthHz(c.text, parsed);
    if (ok != c.expect_ok || (ok && parsed != c.expect_value)) {
      out.print(F("SELFTEST parse FAIL: '"));
      out.print(c.text);
      out.print(F("' ok="));
      out.print(ok);
      out.print(F(" value="));
      out.println(parsed);
      failures++;
    }
  }
  uint16_t null_sink = 0;
  if (focParseBandwidthHz(nullptr, null_sink)) {
    out.println(F("SELFTEST parse FAIL: null accepted"));
    failures++;
  }

  // --- Exhaustive derivation across every integer request in range (D12) ---
  const float ceiling = FOC_MAX_SUSTAINABLE_RATE_HZ_PROVISIONAL;
  uint32_t clamped_count = 0;
  for (uint32_t bw = FOC_BANDWIDTH_MIN_HZ; bw <= FOC_BANDWIDTH_MAX_HZ; ++bw) {
    FocTimingConfig cfg;
    if (!focDeriveTiming((uint16_t)bw, ceiling, cfg)) {
      out.print(F("SELFTEST derive FAIL: no solution for "));
      out.println(bw);
      failures++;
      continue;
    }
    bool bad = false;
    bad |= (cfg.sampling_multiple != FOC_SAMPLING_MULTIPLE);
    bad |= (cfg.control_rate_hz !=
            (uint32_t)cfg.active_bandwidth_hz * (uint32_t)FOC_SAMPLING_MULTIPLE);
    bad |= (cfg.carrier_hz != cfg.control_rate_hz * (uint32_t)cfg.decimation);
    bad |= (cfg.carrier_hz < FOC_CARRIER_MIN_HZ) || (cfg.carrier_hz > FOC_CARRIER_MAX_HZ);
    bad |= (cfg.active_bandwidth_hz > bw);
    bad |= (cfg.clamped != (cfg.active_bandwidth_hz < bw));
    bad |= (cfg.decimation < 1);
    if (bad) {
      out.print(F("SELFTEST invariant FAIL at bw="));
      out.print(bw);
      out.print(F(" active="));
      out.print(cfg.active_bandwidth_hz);
      out.print(F(" rate="));
      out.print(cfg.control_rate_hz);
      out.print(F(" carrier="));
      out.print(cfg.carrier_hz);
      out.print(F(" decim="));
      out.println(cfg.decimation);
      failures++;
    }
    if (cfg.clamped) clamped_count++;
  }

  // --- Out-of-range derivation is refused outright ---
  FocTimingConfig cfg;
  if (focDeriveTiming(99, ceiling, cfg) || focDeriveTiming(10001, ceiling, cfg) ||
      focDeriveTiming(0, ceiling, cfg)) {
    out.println(F("SELFTEST derive FAIL: out-of-range request accepted"));
    failures++;
  }

  // --- A ceiling below the minimum bandwidth must fail closed, not clamp to nothing ---
  if (focDeriveTiming(1000, 500.0f, cfg)) {
    out.println(F("SELFTEST derive FAIL: accepted an impossible ceiling"));
    failures++;
  }

  // --- The default request must never be silently weakened ---
  if (focDeriveTiming(FOC_BANDWIDTH_DEFAULT_HZ, ceiling, cfg)) {
    if (cfg.sampling_multiple != FOC_SAMPLING_MULTIPLE) {
      out.println(F("SELFTEST FAIL: sampling multiple reduced for the default"));
      failures++;
    }
  }

  out.print(F("SELFTEST derivation: "));
  out.print(FOC_BANDWIDTH_MAX_HZ - FOC_BANDWIDTH_MIN_HZ + 1);
  out.print(F(" requests, "));
  out.print(clamped_count);
  out.print(F(" clamped, failures="));
  out.println(failures);
  return failures;
}

void focTimingInstrumentBegin(int pin) {
#ifdef FOC_TIMING_INSTRUMENT
  g_instrument_pin = pin;
  if (pin >= 0) {
    pinMode(pin, OUTPUT);
    digitalWrite(pin, LOW);
  }
#else
  (void)pin;
#endif
}
