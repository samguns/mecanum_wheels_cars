# Phase 0 Research: MIT Impedance Control Mode with Deterministic Current Loop

**Feature**: `002-mit-impedance-control` | **Date**: 2026-08-20

All measurements below are read from the installed toolchain and library source. Values marked **MEASURE** are
estimates that must be confirmed on hardware before the bandwidth ceiling is published; the plan gates on them.

## Environment baseline

| Item | Finding | Evidence |
|---|---|---|
| Target | Classic ESP32, FQBN `esp32:esp32:esp32`, core 3.3.7 | `specs/001-motor-calibration/research.md` |
| Motor library | Simple FOC 2.4.0 | `C:\Users\guns_\Documents\Arduino\libraries\Simple_FOC\library.properties` |
| PWM peripheral | MCPWM (ESP-IDF 5.x path), 160 MHz timebase | `esp32_driver_mcpwm.h` |
| Present carrier | 25 kHz on driver 1; driver 2 left unset and defaults to 25 kHz | sketch lines 779-781 |
| Current sensing | `InlineCurrentSense`, custom fast `adcRead()` via direct SAR registers | `esp32_adc_driver.cpp` |
| Position sensing | AS5147 over SPI at 1 MHz, multi-turn accumulated by `Sensor::full_rotations` | `MagneticSensorSPI.cpp`, `Sensor.cpp` |
| Control loop today | One FreeRTOS task, priority 1, core 1, `taskYIELD()` only | sketch lines 871-894 |
| CAN | TWAI at 1 Mbit/s, alert-polled from `loop()` at 5 ms | sketch lines 714, 920 |

---

## D1. What actually limits the control rate

**Decision**: The binding constraint is CPU time spent in sensor acquisition, not the PWM carrier.

**Rationale**: Per control cycle the present code path costs, for two motors:

| Work | Cost | Source |
|---|---|---|
| 4 × `adcRead()` (2 phases × 2 motors) | ~9 µs each → **~36 µs** | `esp32_adc_driver.cpp` lines 16-17 state the fast path went "from 12us to 9us" |
| 2 × `MagneticSensorSPI::read()` | ~85 µs each → **~170 µs** | Two 16-bit transfers at 1 MHz (~16 µs each) plus a hardcoded `delayMicroseconds(50)` |
| FOC math, 2 motors | **~30-40 µs** MEASURE | 2 current PIDs + 2 LPFs + velocity PID + sin/cos + space-vector modulation per motor |

That totals roughly **240 µs**, a ceiling near **4 kHz** with zero headroom. At the ten-times sampling rule this
supports only about **400 Hz** of current-loop bandwidth, which is **below the 1000 Hz default the spec requires**.

The single worst offender is this, in the vendored library:

```153:157:C:\Users\guns_\Documents\Arduino\libraries\Simple_FOC\src\sensors\MagneticSensorSPI.cpp
#if defined(ESP_H) && defined(ARDUINO_ARCH_ESP32) // if ESP32 board
  delayMicroseconds(50); // why do we need to delay 50us on ESP32? In my experience no extra delays are needed, on any of the architectures I've tested...
#else
  delayMicroseconds(1); // delay 1us, the minimum time possible in plain arduino. 350ns is the required time for AMS sensors, 80ns for MA730, MA702
#endif
```

100 µs of the ~240 µs budget is pure busy-wait that the library author's own comment doubts is necessary.

**Alternatives considered**:
- *Accept ~400 Hz and lower the default*: rejected, the spec fixes the default at 1000 Hz (FR-022).
- *Patch the installed SimpleFOC*: rejected. The library lives outside the repository, so a local edit is
  invisible to version control and unreproducible for anyone else building this firmware. It also conflicts with
  the constitution's preference for changing first-party code before vendored code.
- *Drop to one motor per controller*: rejected, that is a hardware redesign, not a firmware feature.

## D2. First-party AS5147 reader

**Decision**: Implement a project-owned AS5147 SPI reader in the repository that performs one pipelined 16-bit
transfer per read at 8 MHz, and feed its angle into SimpleFOC through the existing sensor interface.

**Rationale**: AS5147 SPI is pipelined, so a single transfer both issues the command and returns the previous
command's result. One transfer at 8 MHz costs about 2 µs plus chip-select overhead, taking both encoders from
~170 µs to under **~10 µs** MEASURE. The AS5147 datasheet permits up to 10 MHz, and SimpleFOC's own config
comment concedes this ("AMS should be able to accept up to 10MHz"). Keeping this in-repo satisfies the
constitution's ownership rule and keeps the change reviewable.

**Alternatives considered**:
- *Keep `MagneticSensorSPI` but raise `clock_speed` via the constructor's third argument*: helps the two
  transfers (16 µs → 2 µs each) but leaves the 50 µs delay, so two encoders still cost ~108 µs. Insufficient
  alone, though it is the fallback if the pipelined read misbehaves.
- *Subclass `MagneticSensorSPI` and override `getSensorAngle()`*: viable and slightly less code, but the base
  class's `read()` is what carries the delay, so the override must bypass it anyway.

## D3. Revised loop budget and the bandwidth ceiling

With D2 applied, the per-cycle budget becomes:

| Work | Cost |
|---|---|
| 2 × pipelined encoder read at 8 MHz | ~10 µs MEASURE |
| 4 × `adcRead()` | ~36 µs (hard floor without ADC DMA) |
| FOC math, 2 motors | ~30-40 µs MEASURE |
| Timing, notification, bookkeeping | ~5 µs MEASURE |
| **Total** | **~85-90 µs → ~11 kHz theoretical** |

Applying a **60% duty budget** so overruns stay impossible under load, the sustainable rate is about
**8-10 kHz**, giving a **maximum bandwidth near 800-1000 Hz** at the ten-times rule.

**Consequence the plan must surface**: the requested 100-10000 Hz range is honoured as an input range, but
everything above roughly 1000 Hz will **clamp**. This is exactly the behaviour chosen during clarification
(FR-020b), so the feature is coherent, but the operator-visible outcome is that the default is also close to the
ceiling. `ADC1` continuous DMA sampling is the only route to materially more headroom and is deliberately
**out of scope** here; it is recorded as future work.

**Alternatives considered**:
- *Use a five-times sampling multiple instead of ten*: rejected for all configurations. It would make a requested
  bandwidth appear reachable by weakening the control-design margin and directly violate clarified FR-011.
  Requests above the measured ceiling clamp; ADC DMA is future work if a higher active ceiling is required.

## D4. Deterministic trigger: PWM-locked ISR plus high-priority task

**Decision**: Register an MCPWM timer `on_full` event callback, decimate it in the ISR by an integer factor, and
release a high-priority FreeRTOS task pinned to core 1 by direct task notification. The task performs the
sensor reads, the impedance law, and the FOC update.

**Rationale**: This satisfies FR-010 (hardware time base), FR-014 (timing reference independent of execution
time, because the ISR fires from the PWM timer regardless of how long the task took) and FR-018 (fixed,
repeatable phase relationship between the sampling instant and the switching cycle). SimpleFOC already proves
the mechanism is available on this exact peripheral for low-side sensing:

```224:233:C:\Users\guns_\Documents\Arduino\libraries\Simple_FOC\src\current_sense\hardware_specific\esp32\esp32_mcpwm_mcu.cpp
    auto cbs = mcpwm_timer_event_callbacks_t{
      .on_full = _mcpwmTriggerADCCallback,
    };
    ...
    CHECK_CS_ERR(mcpwm_timer_register_event_callbacks(t, &cbs, cs_params), "Failed to set low side callback");
```

SPI and the Arduino ADC helpers are not safe to run in ISR context, so the work belongs in a task; the
notification pattern costs a few microseconds of latency but keeps the *period* exact.

**Alternatives considered**:
- *Do everything in the ISR*: rejected, SPI transactions in an ISR risk deadlock and long ISRs block WiFi/TWAI.
- *A free-running `gptimer` or `esp_timer` at the control rate*: simpler, and deterministic in period, but it
  drifts in phase relative to the PWM carrier, so the sampling instant lands at a different point in the
  switching cycle each cycle. That fails FR-018.
- *Keep `taskYIELD()` and raise priority*: rejected, it has no time base at all.

## D5. Carrier derived from bandwidth

**Decision**: Derive both the control rate and the carrier from the active bandwidth:

1. `f_sample = 10 × bandwidth`, clamped to the measured sustainable maximum.
2. `carrier = f_sample × N`, choosing the smallest integer `N ≥ 1` that puts the carrier at or above 20 kHz,
   subject to the 50 kHz library cap.
3. The ISR decimates the `on_full` event by `N`.

**Rationale**: Integer decimation is what makes the phase relationship exact. Staying at or above 20 kHz keeps
the carrier out of the audible band. The 50 kHz cap is a hard library constant:

```59:60:C:\Users\guns_\Documents\Arduino\libraries\Simple_FOC\src\drivers\hardware_specific\esp32\esp32_driver_mcpwm.h
#define _PWM_FREQUENCY 25000 // 25khz
#define _PWM_FREQUENCY_MAX 50000 // 50kHz
```

Worked examples, corrected during implementation to follow the smallest-N rule exactly:
100 Hz → 1 kHz sampling → N=20 → 20 kHz carrier. 500 Hz → 5 kHz sampling → N=4 → 20 kHz carrier.
1000 Hz → 10 kHz sampling → N=2 → 20 kHz carrier. 2000 Hz → 20 kHz sampling → N=1 → 20 kHz carrier
(CPU-limited well before this point).

An earlier draft of this section quoted N=5/25 kHz and N=3/30 kHz for 500 and 1000 Hz. Those were
arithmetically inconsistent with the smallest-N rule stated above and with `data-model.md`; the
exhaustive check in `focTimingSelfTest()` confirms the values quoted here.

**Alternatives considered**:
- *Fix the carrier at 25 kHz permanently and pick any control rate*: rejected, non-integer ratios break phase
  lock, which is the whole point of FR-018.

## D6. Fixed timestep in the controllers

**Decision**: Set `Ts` explicitly on every `PIDController` and `LowPassFilter` the motors use, to the nominal
control period, whenever the active bandwidth is applied.

**Rationale**: SimpleFOC 2.4.0 already supports this and the sketch simply never uses it. Today every PID and
filter measures its own elapsed time:

```18:29:C:\Users\guns_\Documents\Arduino\libraries\Simple_FOC\src\common\pid.cpp
float PIDController::operator() (float error){
    // initalise the elapsed time with the fixed sampling tims Ts
    float dt = Ts; 
    // if Ts is not set, use adaptive sampling time
    // calculate the ellapsed time dt
    if(!_isset(dt)){
        unsigned long timestamp_now = _micros();
        dt = (timestamp_now - timestamp_prev) * 1e-6f;
        // quick fix for strange cases (micros overflow)
        if(dt <= 0 || dt > 0.5f) dt = 1e-3f;
        timestamp_prev = timestamp_now;
    }
```

Setting `Ts` satisfies FR-019 with no library change: `motor.PID_current_q.Ts`, `PID_current_d.Ts`,
`PID_velocity.Ts`, `LPF_current_q.Ts`, `LPF_current_d.Ts`, `LPF_velocity.Ts`.

**Alternatives considered**:
- *Leave adaptive dt*: rejected, it converts jitter into gain error, defeating the feature.

## D7. Where the impedance law runs

**Decision**: Compute the impedance law in the same control cycle as the FOC update, then hand the result to
SimpleFOC as a torque target with `MotionControlType::torque`, replacing the current
`MotionControlType::velocity` and its outer velocity PID.

**Rationale**: The impedance law *is* the outer loop; running SimpleFOC's velocity PID underneath it would
create two nested position/velocity controllers fighting each other. `TorqueControlType::foc_current` stays,
because that is the current loop the bandwidth parameter tunes.

**Note for velocity mode**: the retained velocity mode keeps `MotionControlType::velocity` and the existing
velocity PID, so the two modes differ in which SimpleFOC controller is active. This is why FR-006b requires
clearing state on a mode change and FR-006b/FR-025 require a disarm first.

## D8. Position error saturation limit

**Decision**: Saturate the position error at **±1.0 rad** before multiplying by stiffness, and expose the
saturation as a named constant.

**Rationale**: The per-motor current limit is 3.0 A (sketch line 821). Choosing the limit so that maximum
stiffness at full saturation asks for roughly the current limit keeps the term from ever dominating: at the
MIT-convention stiffness ceiling this bounds the request rather than relying on downstream clipping. One radian
is also large enough that normal compliant operation never reaches it, so the saturation is invisible in
ordinary use and only acts as a guard. Exact value confirmed on the bench against measured torque constant.

**Alternatives considered**:
- *Rely only on the existing current limit*: rejected. Clipping at the current limit means a bad target
  silently commands maximum torque, which is the hazard FR-001b exists to remove.
- *Wrap the position error to ±π*: rejected, it makes a multi-turn position hold impossible and introduces a
  direction ambiguity at the wrap point.

The saturation is a first guard, not a replacement for downstream protection. The impedance path preserves the
per-motor current and output-voltage limits and adds a fail-closed protection path to the existing bus-voltage
monitor. The control task latches compact per-motor cause bits, previous-active edge state, and saturating event
counters; communications code publishes them without serial or CAN work in the deterministic path.

## D9. CAN frame layout

**Decision**: Use one atomic two-frame pair per motor. The position half carries signed `int32` milliradians and
the dynamics half carries the 12-bit velocity, stiffness, damping, and feed-forward fields plus a capture-current-
position flag. Both carry the same sequence number; firmware stages and applies them only as a valid matched pair.

**Rationale**: A 16-bit ±12.5 rad MIT position field contradicts the selected absolute accumulated-position
semantics after roughly two wheel revolutions and cannot express SC-013's 100-revolution offset. Signed 32-bit
milliradians cover about 341,000 revolutions at 0.001 rad resolution. Keeping the four dynamics values at 12 bits
preserves their chosen resolution. Sequence matching makes the two-frame update atomic: an incomplete or stale
pair cannot partially change torque state or refresh the command timeout. When the capture flag is set, firmware
uses the same-cycle measured accumulated position instead of the transmitted target. Per-motor `0x1E0`/`0x1F0`
frames report the applied target and capture generation so the backend can finish the handshake safely.

**Alternatives considered**:
- *Rolling position origin with the original 8-byte MIT frame*: lower traffic, rejected because origin updates
  can race motion frames and reinterpret an in-flight target after loss or reordering.
- *Relative position-error command*: simplest, rejected because it abandons absolute-position semantics and
  moves a safety-critical error calculation to the sender.
- *Widen the 16-bit position range*: rejected because the resolution loss makes useful stiffness control
  impossible long before it covers prolonged wheel rotation.

## D10. Bus budget at 200 Hz

**Decision**: The chosen 200 Hz paired-command rate fits within a hard 50% nominal-utilisation ceiling.

**Rationale**: A standard 8-byte CAN frame with stuffing is about 130 bits, so ~130 µs at 1 Mbit/s. Eight motion
frames per update at 200 Hz is 1600 frames/s ≈ **20.8% utilisation**. Existing status traffic contributes about
5.2%, and the six new 10 Hz status families contribute about 1.6%. Total stays near **27.6%**, leaving material
arbitration and fault-recovery headroom below the 50% ceiling.

## D11. Backend and UI shape

**Decision**: Extend `CanPublisher` with an impedance frame builder and add a fixed-rate 200 Hz transmit loop;
add mode and gain controls plus a controller-reported status panel to `App.vue`.

**Rationale**: Today transmission is purely event-driven off Socket.IO joystick events with no throttling, so
frame timing follows pointer-move events. The 50 ms command timeout makes that unsafe: a slow-moving pointer
would let motors time out mid-drive. A fixed 200 Hz asyncio send loop that transmits the latest target is
required by FR-038.

Normal idle/stop uses explicit zero-effort pairs at 200 Hz. Emergency stop first emits `0x080` and then suspends
motion frames until cleared. Only a sender/link failure goes silent and exercises the 50 ms timeout. When `kp`
transitions from zero to non-zero, the loop sets capture-current-position on every pair until the motor's
`0x1E0`/`0x1F0` generation advances, then reuses that confirmed absolute value.

**Alternatives considered**:
- *Keep event-driven sending and lengthen the timeout*: rejected, it makes the failsafe depend on operator
  pointer behaviour.

## D12. Layered bandwidth validation

**Decision**: Exhaustively exercise the pure timing derivation for all integer requests from 100 through 10000 Hz
plus malformed/out-of-range inputs. Reserve hardware power-cycle and current-sense tests for the unique fixed
matrix `{100, 500, 1000, measured ceiling, ceiling + 1, 10000}` and carrier extrema.

**Rationale**: Exhaustive hardware persistence would require roughly ten thousand attended power cycles without
adding coverage beyond the pure helper's deterministic mapping. Layering gives complete arithmetic/boundary
coverage and keeps hardware work focused on NVS, carrier application, clamp reporting, and analog validity.

**Alternatives considered**:
- *Exhaust every value on hardware*: rejected as disproportionate and mechanically risky.
- *Test only representative values everywhere*: rejected because integer-ratio and clamp edge bugs can hide
  between representatives and are cheap to exhaust in the pure helper.

---

## Open items gated on hardware measurement

| # | Item | Gate |
|---|---|---|
| M1 | Per-cycle execution time for the full two-motor cycle | Sets the published maximum bandwidth |
| M2 | Pipelined single-transfer AS5147 read correctness at 8 MHz | Falls back to two transfers at 8 MHz if angles are corrupt |
| M3 | Whether `adcRead` really costs ~9 µs on this board | If materially worse, the ten-times multiple may need revisiting |
| M4 | Current-sense validity at a 30-50 kHz carrier | Inline sensing has no PWM sync, so ripple aliasing must be checked |
| M5 | Actual sustainable rate with WiFi and TWAI active | Sets the overrun threshold and the duty budget |
