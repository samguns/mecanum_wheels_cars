---

description: "Task list for MIT impedance control mode with deterministic current loop"
---

# Tasks: MIT Impedance Control Mode with Deterministic Current Loop

**Input**: Design documents from `/specs/002-mit-impedance-control/`

**Prerequisites**: plan.md, spec.md, research.md, data-model.md, contracts/, quickstart.md

**Tests**: No automated test tasks are generated. The specification does not request TDD, and the repository has
no test harness (`jetson_xavier/backend/can_test.py` is a manual CAN utility, and the UI has no vitest config).
Validation instead uses the constitution's change gates: `arduino-cli compile`, `npm run type-check`, focused
Python runs, and the attended bench scenarios in [quickstart.md](./quickstart.md). Those validation tasks are
mandatory, not optional.

**Organization**: Tasks are grouped by user story so each can be implemented and verified independently.

## Format: `[ID] [P?] [Story] Description`

- **[P]**: Can run in parallel (different files, no dependencies)
- **[Story]**: Which user story this task belongs to (US1-US7, matching spec.md numbering)
- Every task names the exact file path it touches

## Path Conventions

This is a three-layer project, not a single-project tree. Real paths:

- **Firmware**: `v13_macnum_wheel_car.ino` at the repository root, plus new sketch-local files beside it
  (the plan sanctions this pattern, matching how `imu_helpers.{h,cpp}` already sits next to the sketch)
- **Backend**: `jetson_xavier/backend/`
- **UI**: `jetson_xavier/webUI/src/`
- **Never touch**: `src/MPU6050/`, `src/I2Cdev/` (vendored), and the SimpleFOC install outside the repository

New firmware files introduced by this feature:

| File | Owns |
|---|---|
| `can_protocol.h` | Frame identifiers, field ranges, atomic pair pack/unpack helpers |
| `as5147_fast.{h,cpp}` | First-party pipelined AS5147 SPI reader |
| `foc_timing.{h,cpp}` | Bandwidth→timing derivation, MCPWM trigger, loop timing record |
| `impedance_control.{h,cpp}` | Per-motor impedance state and torque computation |

Splitting these out is what makes parallel work possible at all; tasks that edit
`v13_macnum_wheel_car.ino` are inherently serial with each other and are never marked `[P]`.

## ⚠️ Phase ordering deviates from strict priority order

**Phase 3 implements User Story 2, not User Story 1**, even though both are P1.

Constitution Principle I forbids producing motor torque along an unproven path. Impedance control multiplies a
stiffness gain by a position error, so an impedance law running on a loop whose rate is unknown and load
dependent is exactly the unsafe combination this feature exists to remove. US2 (determinism) is also
independently valuable and independently testable on its own: it makes the **existing velocity controller**
deterministic, which is a shippable improvement with no impedance code present.

Story labels below stay tied to spec.md numbering, so `[US2]` really is spec User Story 2.

---

## Phase 1: Setup (Shared Infrastructure)

**Purpose**: Establish the compile gate, the shared protocol definitions, and a bench sender so every firmware
story can be validated without the backend being ready.

- [X] T001 Verify the baseline compile gate passes before any change: run `arduino-cli compile --fqbn esp32:esp32:esp32 .` from the repository root and record the result
- [X] T002 [P] Create `can_protocol.h` at the repository root with the position (`0x100`/`0x110`), control (`0x120`), dynamics (`0x130`/`0x140`), and status (`0x1A0`-`0x1F0`) identifiers; signed `int32` milliradian position conversion; named dynamics ranges; fault bits; and scalar helpers per `contracts/can-protocol.md`
- [X] T003 Add big-endian position/dynamics pair pack/unpack, matching-sequence validation, capture-current-position flag handling, and applied-target acknowledgement structures to `can_protocol.h`, separate from the little-endian legacy `0x200` helper
- [X] T004 [P] Create `jetson_xavier/backend/can_frames.py` as the Python mirror of `can_protocol.h`, including atomic-pair identifiers, capture flag, applied-target acknowledgements, signed position conversion, sequencing, and pack/unpack helpers
- [X] T005 Add shared fixtures to `contracts/can-protocol.md` and a `__main__` self-check in `jetson_xavier/backend/can_frames.py`, covering position/dynamics extremes, 100 revolutions, capture flag, sequence wrap/mismatch, pair-fault bits, and `0x1E0`/`0x1F0` acknowledgement decode
- [X] T006 Create `jetson_xavier/backend/impedance_bench_sender.py`: a CLI that streams matched pairs, can set capture-current-position, deliberately omit/expire/mismatch a half, send forbidden bandwidth-write probes, control mode/arm, and stop on demand

**Checkpoint**: Protocol definitions exist on both sides and agree on fixtures; a bench sender is available.

---

## Phase 2: Foundational (Blocking Prerequisites)

**Purpose**: The measurement harness, the fast sensor path, the persisted config, and the timing derivation math.
Every story depends on these.

**⚠️ CRITICAL**: No user story work can begin until this phase is complete. In particular, the 1000 Hz default
is unreachable until T009-T012 land.

- [X] T007 Create `foc_timing.h`/`foc_timing.cpp` at the repository root with the `LoopTimingRecord` struct from `data-model.md` (cycle count, overrun count, consecutive overruns, worst and last cycle microseconds, measured rate) plus a debug GPIO toggle helper gated behind a `FOC_TIMING_INSTRUMENT` compile flag
- [ ] T008 Instrument the existing `TaskFOC` in `v13_macnum_wheel_car.ino` with the `LoopTimingRecord`, then measure and record the **baseline** per-cycle execution time and loop rate of the current implementation into `research.md` under gate M1 (expected near 240 µs / ~4 kHz)
- [X] T009 [P] Create `as5147_fast.h`/`as5147_fast.cpp` implementing a first-party AS5147 reader that performs a **single pipelined 16-bit SPI transfer** per read at 8 MHz, with parity and error-bit checking, and no `delayMicroseconds` in the read path
- [X] T010 Integrate the fast reader into `v13_macnum_wheel_car.ino` behind SimpleFOC's `Sensor` interface so `Sensor::update()` still accumulates `full_rotations`, keeping multi-turn angle behaviour identical
- [ ] T011 Verify angle equivalence on hardware: rotate each wheel through several full turns and confirm the fast reader's accumulated angle matches `MagneticSensorSPI` within one LSB, then record the per-read cost into `research.md` gates M2 and M3 (target under 10 µs for both encoders)
- [X] T012 Add a fallback path in `as5147_fast.cpp` that uses two transfers at 8 MHz if the pipelined single-transfer read fails verification in T011, selectable by a compile flag, per research decision D2
- [X] T013 Bump `CONFIG_VERSION` from 1 to 2 in `v13_macnum_wheel_car.ino` and add requested bandwidth, persisted per-motor modes, and provisional 7000/24000 mV bus-protection thresholds to `RobotConfig` per `data-model.md`
- [X] T014 Implement the version-1 to version-2 migration in `loadConfig()` in `v13_macnum_wheel_car.ino` so a stored v1 blob is read for its existing calibration fields, seeded with the new defaults, and rewritten — the existing size-and-version mismatch path must no longer discard calibration
- [ ] T015 Verify migration with `v13_macnum_wheel_car.ino`: power-cycle a v1-calibrated unit and confirm calibration survives, requested bandwidth seeds to 1000, both modes seed velocity, bus thresholds seed safely, and startup remains disarmed with zero motion state
- [X] T016 Implement constexpr-safe bandwidth parsing and timing derivation in `foc_timing.cpp`: fixed 10× sampling, integer carrier ratio in the 20-50 kHz window, deterministic clamping, and no path that reduces the sampling multiple, per research D5/D12
- [X] T017 Implement clamping in `foc_timing.cpp`: derive `active_bandwidth` as `min(requested, max_sustainable)`, expose a `clamped` flag, and hold `max_sustainable` as a single named constant seeded from the T008 baseline and finalised in T031
- [X] T018 Implement a gain and timestep application helper in `foc_timing.cpp` that, given the active bandwidth, sets each motor's `PID_current_{d,q}.P/I` from measured R and L, `LPF_current_{d,q}.Tf`, and `Ts` on every `PIDController` and `LowPassFilter` the motors use, per research decision D6 and the formulas in `data-model.md`
- [X] T019 Replace the hardcoded `pending_calibration.current_bandwidth = 100.0f` in `v13_macnum_wheel_car.ino` so the node-wide configured bandwidth is the source of truth, retaining the per-motor struct field for calibration compatibility. **Transferred to feature 003 and implemented 2026-08-22**: characterisation now copies `config.current_bandwidth_hz`.
- [X] T020 Set both `driver1.pwm_frequency` and `driver2.pwm_frequency` explicitly from the derived carrier in `v13_macnum_wheel_car.ino`, fixing the existing defect where `driver2` is never assigned and silently relies on a library default
- [X] T021 Create `impedance_control.h`/`impedance_control.cpp` with runtime/pending-pair state including capture flag/generation, applied target, pair-fault latch, limit causes, previous-active edge memory and counters, plus `POSITION_ERROR_LIMIT` at 1.0 rad
- [X] T022 Run the compile gate `arduino-cli compile --fqbn esp32:esp32:esp32 .` and confirm clean

**Checkpoint**: Fast sensing verified, config migrated safely, timing math available. Loop rate target reachable.

---

## Phase 3: User Story 2 - Deterministic sampling and control execution (Priority: P1) 🎯 MVP

**Goal**: Current sampling and control computation run at a fixed rate derived from the bandwidth, driven by a
hardware time base and phase-locked to the PWM carrier, with overruns detected and failing closed.

**Independent Test**: With motors disarmed, confirm on a scope that the control period is fixed and that the
measured rate is within 1% of nominal with zero overruns over 10 minutes under full communication load. Then
arm in the **existing velocity mode** and confirm the vehicle still drives, proving the deterministic loop is a
standalone improvement with no impedance code involved.

### Implementation for User Story 2

- [X] T023 [US2] Implement the MCPWM `on_full` event callback registration in `foc_timing.cpp`, following the pattern SimpleFOC uses for low-side sync (`mcpwm_timer_register_event_callbacks` with the timer FSM workaround), obtaining the timer handle from the driver params of `driver1`
- [X] T024 [US2] Implement decimation inside the ISR in `foc_timing.cpp`: count `on_full` events and release the control task once per `decimation` events, keeping the ISR minimal and `IRAM_ATTR`
- [X] T025 [US2] Create the deterministic control task in `foc_timing.cpp`: high priority, pinned to core 1, blocking on direct task notification from the ISR, with the notification-based wait replacing any polling
- [X] T026 [US2] Rewrite `TaskFOC` in `v13_macnum_wheel_car.ino` to be the notified control task body: read both sensors, read both current senses, run `loopFOC()` and the motion update for both motors, and record timing — removing the `taskYIELD()` free-run loop entirely
- [X] T027 [US2] Apply the fixed `Ts` helper from T018 at startup and on every bandwidth change in `v13_macnum_wheel_car.ino`, so no PID or filter uses adaptive `_micros()` timing
- [X] T028 [US2] Implement overrun detection in `foc_timing.cpp`: detect a notification arriving while the previous cycle is still executing, increment `overrun_count` and `consecutive_overruns`, reset the consecutive counter on a clean cycle, and track `worst_cycle_us` — the ISR must keep its own phase regardless, so a late cycle never shifts subsequent cycle timing
- [X] T029 [US2] Implement the fail-closed rules in `foc_timing.cpp` and wire them in `v13_macnum_wheel_car.ino`: more than 10 consecutive overruns, or a measured rate deviating from nominal by more than 5% while the overrun count is unchanged, disarms all motors and reports the cause
- [X] T030 [US2] Give control work precedence over communications in `v13_macnum_wheel_car.ino`: move the TWAI alert handling, `command.run()`, bus-voltage sampling, and status transmission off core 1 into a dedicated task on core 0, leaving `loop()` idle or minimal
- [ ] T031 [US2] Execute S1 in `specs/002-mit-impedance-control/quickstart.md`: capture every timing edge for 10 minutes under full load, calculate the 99.9% ±5% statistic and missing gaps, record rate/worst/duty into `research.md`, and publish the 60%-duty sustainable ceiling in `foc_timing.cpp`
- [X] T032 [US2] Add compile-time exhaustive checks in `foc_timing.cpp` for every integer request 100-10000 plus malformed/out-of-range parser cases, proving fixed 10× sampling, integer carrier ratio, deterministic clamp invariants, and default-request clamping when T031's ceiling is below 1000; run the compile gate
- [ ] T033 [US2] Execute the current-sense portion of S2 in `specs/002-mit-impedance-control/quickstart.md` at the lowest, default-derived, and highest active carrier points, compare with the default reference within 5%, and record research gate M4 results
- [ ] T034 [US2] Arm in velocity mode using the existing `0x200` path and confirm the vehicle still drives forward, reverse, strafe, and rotation with correct wheel directions on the deterministic loop (quickstart S8 procedure), then run `arduino-cli compile --fqbn esp32:esp32:esp32 .`

**Checkpoint**: The control loop is deterministic and the existing velocity controller runs on it. Shippable alone.

---

## Phase 4: User Story 1 - Command a wheel with an impedance law (Priority: P1)

**Goal**: Each motor atomically accepts a sequence-matched position-and-dynamics command pair, produces the
resulting compliant behavior, and remains bounded by all specified effort protections.

**Independent Test**: Using `impedance_bench_sender.py`, prove damping, stiffness, and feed-forward behavior;
verify a 100-revolution signed-milliradian target saturates safely while torn/mismatched pairs do nothing; then
trigger current, output-voltage, and bus-voltage protection and confirm bounded output plus serial/CAN causes.
No production backend or UI changes are required.

### Implementation for User Story 1

- [X] T035 [US1] Implement the torque computation in `impedance_control.cpp`: saturate the position error at `POSITION_ERROR_LIMIT` **before** multiplying by stiffness, then add the damping term on the velocity error and the feed-forward torque, per the formula in `data-model.md`
- [ ] T036 [US1] Implement current/output-voltage limiting in `impedance_control.cpp` and a new fail-closed bus-voltage window in `v13_macnum_wheel_car.ino`; track cause rising edges separately from report latches, add disarmed-only serial `V` configuration, minimal `I` cause reporting, and 10 Hz `0x1D0` status from communications context. **Serial `V` transferred to feature 003 T093 (2026-08-22).** **Current/output-voltage limiting, cause latches, and serial `I` cause reporting transferred to feature 003 T116 (2026-08-22)** and implemented in `impedance_control.cpp` / `v13_macnum_wheel_car.ino`. Remaining here: 10 Hz CAN `0x1D0`.
- [ ] T037 [US1] Implement a shared impedance-eligibility guard in `impedance_control.cpp` and `v13_macnum_wheel_car.ino` that validates per-motor resistance, inductance, current-sense calibration, and torque constant, returns a motor-specific missing/invalid-field cause without changing mode or arm state, and is mandatory for every impedance mode-entry and arm caller; switch eligible motors to `MotionControlType::torque` with `TorqueControlType::foc_current` and no outer velocity PID, per FR-027 and research D7. **Serial path transferred to feature 003 T117 (2026-08-22)**: `M*I` and serial `A` use per-field causes and switch the SimpleFOC controller to torque/foc_current. Remaining here: CAN `0x120` callers (T053).
- [X] T038 [US1] Call the impedance computation once per control cycle inside the deterministic control task in `v13_macnum_wheel_car.ino`, using the same cycle's sensor and current readings for both motors
- [ ] T039 [US1] Implement receipt/staging of `0x100/0x110` position and `0x130/0x140` dynamics halves in `v13_macnum_wheel_car.ino`: validate DLC/reserved/value fields, latch and report per-motor protocol faults for malformed/expired/stale/mismatched halves, and never change active state on rejection
- [ ] T040 [US1] Complete atomic pair application in `v13_macnum_wheel_car.ino`: match within 5 ms, never refresh timeout on incomplete pairs, replace older pending sequences, apply motors independently, and when capture is set use the same-cycle measured position, update applied target/generation, and ignore transmitted `p_des`
- [X] T041 [US1] Implement the zero-effort paths for impedance state in `impedance_control.cpp` and wire them in `v13_macnum_wheel_car.ino`: disarm, emergency stop, and calibration entry all clear `p_des`, `v_des`, `kp`, `kd`, and `t_ff`
- [X] T042 [US1] Ensure arming begins from a zero-effort state in `v13_macnum_wheel_car.ino` so impedance terms received while disarmed never carry into the armed state
- [ ] T043 [US1] Finalize every provisional safety value before acceptance testing: measure the torque constant and reconcile `t_ff`/`kp`/`kd` ranges, position-error saturation, and current/output-voltage/bus-voltage thresholds across `v13_macnum_wheel_car.ino`, `impedance_control.cpp`, `can_protocol.h`, `jetson_xavier/backend/can_frames.py`, `spec.md`, `plan.md`, `research.md`, and `contracts/can-protocol.md`; preserve the fixed sampling multiple at 10x or higher and run the compile and Python frame self-check gates
- [ ] T044 [US1] Execute S4, S4a, and S4b in `specs/002-mit-impedance-control/quickstart.md` using only T043-finalized values: verify direct-torque and monotonic behavior, the bidirectional five-point SC-008 speed sweep within 10%, three idle versus three loaded steps within 10%, and five bounded trials for each protection path in minimal `I` and `0x1D0`; defer UI presentation to T084
- [ ] T045 [US1] Execute S5 in `specs/002-mit-impedance-control/quickstart.md` across 100 trials using only T043-finalized values: verify the 100-revolution target saturates safely and missing/expired/mismatched pairs neither update state nor refresh timeout while setting the correct pair-fault report. **Highest-consequence check**

**Checkpoint**: Impedance control works on a deterministic loop, exercised from a bench sender.

---

## Phase 5: User Story 3 - Configure the current-loop bandwidth (Priority: P2)

**Goal**: An operator sets the bandwidth from 100 to 10000 Hz on the serial console, it persists, it clamps
when unreachable, and it re-derives every dependent value.

**Independent Test**: Exhaustively prove pure derivation in T032, then power-cycle every unique S2 hardware-matrix
value and confirm requested/active/clamp/rate/carrier/gains plus invalid-input storage invariants.

### Implementation for User Story 3

- [X] T046 [US3] Add the `B` command to the Commander in `v13_macnum_wheel_car.ino` per `contracts/serial-console.md`: bare `B` reports requested value, active value, clamp state, sampling multiple, control rate, control period, carrier, decimation, and derived gains
- [X] T047 [US3] Implement `B<value>` acceptance rules in `v13_macnum_wheel_car.ino`: reject outside 100-10000 or non-numeric while retaining the stored value and reporting the range; refuse while any motor is armed with a disarm-first message; refuse with a calibration-required message when resistance or inductance is missing or invalid
- [X] T048 [US3] Persist the **requested** bandwidth (not the clamped value) via `saveConfig()` in `v13_macnum_wheel_car.ino`, and re-derive the active value on every startup so a later ceiling change re-applies the original request
- [X] T049 [US3] Recompute and report derived gains, filter constants, and every `Ts` on each accepted bandwidth change in `v13_macnum_wheel_car.ino`, reusing the T018 helper
- [ ] T050 [US3] Refuse any carrier change while a motor is armed in `foc_timing.cpp`, and apply a new carrier to both drivers only from the disarmed state
- [X] T051 [US3] Report requested/default and active bandwidth separately in `v13_macnum_wheel_car.ino`, including an explicit startup clamp when an unconfigured 1000 Hz request exceeds the measured ceiling
- [ ] T052 [US3] Execute the unique hardware matrix and invalid-input portions of S2 in `specs/002-mit-impedance-control/quickstart.md`, verifying power-cycle persistence, correct clamp derivation, armed refusal, and unchanged storage after invalid input; run the compile gate

**Checkpoint**: Bandwidth is fully configurable, persistent, and honest about clamping.

---

## Phase 6: User Story 4 - Choose between impedance and velocity mode (Priority: P2)

**Goal**: Each motor persists and restores its explicit mode while always starting disarmed/zero; fresh or invalid
config defaults velocity, and mismatched payloads are rejected rather than reinterpreted.

**Independent Test**: Verify matching/mismatched frames, mixed-mode persistence with zero-state startup, forbidden
bandwidth probes, then persist velocity for all motors and prove unchanged pre-feature sender behavior.

### Implementation for User Story 4

- [ ] T053 [US4] Implement `0x120 + NodeID` control parsing in `v13_macnum_wheel_car.ino` for `0x10` arm/disarm and `0x11` mode selection, route every impedance selection and arm request through T037's eligibility guard, and explicitly reject/report calibration failure, forbidden bandwidth probe `0x12`, or unknown commands without changing mode, arm state, or configuration
- [ ] T054 [US4] Enforce per-motor mode gating in `v13_macnum_wheel_car.ino`: either half of an impedance pair addressed to a velocity-mode motor and a `0x01` velocity command addressed to an impedance-mode motor are rejected and reported, never staged or reinterpreted
- [ ] T055 [US4] Implement mode-change rules in `impedance_control.cpp` and `v13_macnum_wheel_car.ino`: refuse while armed; otherwise zero targets/gains/pending pairs, persist the accepted per-motor mode before success, and preserve independent motor selections
- [X] T056 [US4] Restore valid persisted modes in `v13_macnum_wheel_car.ino` while always starting both motors disarmed with zero targets/gains/effort/pending pairs; default only fresh or invalid mode fields to velocity
- [ ] T057 [US4] Add the `M` command to the Commander in `v13_macnum_wheel_car.ino` per `contracts/serial-console.md` (`M`, `M1V`, `M1I`, `M2V`, `M2I`, `M0V`, `M0I`) with the same disarm-first rule, route impedance selection and subsequent serial arming through T037's eligibility guard, and report the same motor-specific calibration causes as CAN. **Serial `M` transferred to feature 003 T093 (2026-08-22).** **Eligibility routing transferred to feature 003 T117 (2026-08-22).** Remaining here: matching CAN cause reports from T053.
- [ ] T058 [US4] Execute S6/S8 in `specs/002-mit-impedance-control/quickstart.md`: for every required calibration field, verify both CAN and serial impedance entry/arming reject its missing or invalid value without changing mode or arm state and report the same motor-specific cause; then verify 100 mode-mismatch trials, mixed per-motor mode persistence with disarmed zero startup, 100 forbidden bandwidth probes with unchanged storage, and stored-velocity legacy behavior across all four motions; run the compile gate

**Checkpoint**: Both modes coexist safely and the legacy sender is provably unaffected.

---

## Phase 7: User Story 5 - Fail safe when commands stop arriving (Priority: P2)

**Goal**: Normal stop remains explicit zero traffic, while a true sender/link silence reaches zero effort within
50 ms and recovers when valid commands resume.

**Independent Test**: Confirm 200 Hz zero pairs do not timeout, kill the sender and prove zero effort within 50 ms
and automatic recovery, then verify 100 emergency-stop trials at carrier extremes.

### Implementation for User Story 5

- [ ] T059 [US5] Record `last_command_us` per motor on every accepted motion command, in both modes, in `v13_macnum_wheel_car.ino`
- [X] T060 [US5] Implement the 50 ms per-motor timeout in `impedance_control.cpp` evaluated inside the control cycle: set `timed_out`, force zero commanded effort, and leave the motor armed, applying independently per motor so one silent motor does not affect the other
- [X] T061 [US5] Implement automatic recovery in `impedance_control.cpp` so a motor resumes acting on commands with no operator intervention once valid commands resume while still armed
- [X] T062 [US5] Report the timeout transition once per occurrence from the communications context in `v13_macnum_wheel_car.ino`, never from the control task, so reporting cannot cause an overrun
- [ ] T063 [US5] Execute the emergency-stop row of S3 in `specs/002-mit-impedance-control/quickstart.md` for 100 consecutive trials at both lowest and highest active bandwidths, confirming `0x080` precedence and zero-output disarm
- [ ] T064 [US5] Execute normal-stop and sender/link-failure rows of S3 in `specs/002-mit-impedance-control/quickstart.md`: confirm 200 Hz zero pairs never time out, true silence reaches zero within 50 ms across 100 trials, and nine-frame loss tolerance; run the compile gate

**Checkpoint**: The impedance-specific stale-target hazard is closed by an independently verified failsafe.

---

## Phase 8: User Story 6 - Observe realized timing and impedance state (Priority: P3)

**Goal**: Determinism, impedance state, pair faults, applied targets and capture generations are readable on serial
and over the bus for the sender/operator interface.

**Independent Test**: Query during a sustained run and confirm timing, per-motor terms, pair faults, applied targets,
capture generations and edge-counted protection events are visible and monotonic where applicable.

### Implementation for User Story 6

- [X] T065 [US6] Add the `T` command to the Commander in `v13_macnum_wheel_car.ino` reporting nominal and measured control rate, control period, cycle and overrun counts, consecutive overruns, last and worst cycle time, duty percentage, carrier, decimation, and timing-fault state, per `contracts/serial-console.md`
- [X] T066 [US6] Extend the minimal `I` command in `v13_macnum_wheel_car.ino` with full per-motor mode/arm/timeout, transmitted and applied targets, measured position/velocity/current, gains/torque/error, pair fault, last sequence, capture generation, limit causes and edge-counted events. **Transferred to feature 003 T118 (2026-08-22)** and implemented as human `I` plus `#V13 t=imp` records.
- [ ] T067 [US6] Implement 10 Hz `0x1A0 + NodeID` status in `v13_macnum_wheel_car.ino` with mode/arm/timeout, timing/calibration/clamp faults, latched per-motor pair faults, requested/active bandwidth and carrier; clear report latches only after transmission
- [ ] T068 [US6] Implement 10 Hz `0x1B0` applied gains, `0x1C0` measured positions, and per-motor `0x1E0`/`0x1F0` applied-target/capture-generation acknowledgements in `v13_macnum_wheel_car.ino`, retaining T036's `0x1D0` effort-limit status
- [X] T069 [US6] Extend the startup banner in `v13_macnum_wheel_car.ino` with the active bandwidth, control rate, carrier, and each motor's mode
- [ ] T070 [US6] Audit the control task in `v13_macnum_wheel_car.ino` and `foc_timing.cpp` for any serial output or blocking call and remove it, confirming diagnostics cannot cause a control-cycle overrun; re-run the T031 timing check to prove the reporting additions cost nothing, and run the compile gate

**Checkpoint**: Determinism claims are measurable, which the constitution's review gate requires.

---

## Phase 9: User Story 7 - Set impedance behavior from the operator interface (Priority: P3)

**Goal**: An operator selects mode/gains, safely captures a controller-local hold target when enabling stiffness,
sees confirmed values, and drives in impedance mode.

**Independent Test**: While driving at zero stiffness, enable stiffness and prove capture-generation acknowledgement
precedes hold with no saturated transient; verify display readback, all directions, normal stop, and estop suspension.

### Implementation for User Story 7

- [ ] T071 [US7] Add an atomic pair builder to `CanPublisher` in `jetson_xavier/backend/socketio_server.py`, emitting adjacent position/dynamics halves with per-motor sequence and capture flag for all wheels while keeping the legacy velocity builder separate
- [ ] T072 [US7] Add vehicle-wide command and per-wheel handshake state to `jetson_xavier/backend/socketio_server.py`: mode/gains/targets, controller status, capture-pending flags, pre-request generations, and confirmed applied targets per `data-model.md`
- [ ] T073 [US7] Change `on_joystick_command` in `jetson_xavier/backend/socketio_server.py` to only update `latest_targets`, removing CAN transmission from the event handler
- [ ] T074 [US7] Implement the 200 Hz transmit state machine in `jetson_xavier/backend/socketio_server.py`: motion targets while driving, explicit zero-effort commands during normal idle/stop, immediate `0x080` then suspended motion while estopped, and natural silence only on sender/link failure
- [ ] T075 [US7] Add `set_motion_mode` and `set_gains` handlers to `jetson_xavier/backend/socketio_server.py`, validating ranges, starting capture-pending on zero-to-nonzero stiffness, and emitting `command_rejected` rather than clamping silently
- [ ] T076 [US7] Send `0x120 + NodeID` mode frames to both nodes from `jetson_xavier/backend/socketio_server.py` when the mode changes, and reject the request when any controller reports an armed motor
- [ ] T077 [US7] Decode `0x1A0` through `0x1F0` in `jetson_xavier/backend/socketio_server.py` and emit coalesced `controller_status` with measured/applied positions, capture generations, pair faults and limit causes/counts while preserving existing emissions
- [ ] T078 [US7] Implement zero-to-nonzero stiffness capture handshake in `jetson_xavier/backend/socketio_server.py`: mark each wheel pending, repeat capture flags, ignore cached `0x1C0` as a hold target, accept only post-request `0x1E0/0x1F0` generation changes, then use confirmed targets
- [ ] T079 [US7] Run focused Python checks against `contracts/can-protocol.md` for exact pair/capture/ack bytes, 100-revolution round-trip, sequence wrap/mismatch, eight frames per tick, estop suspension versus normal zero frames, and capture-generation acknowledgement
- [ ] T080 [US7] Add mode selection and stiffness and damping inputs to `jetson_xavier/webUI/src/App.vue`, validating against the contract ranges before emitting and surfacing `command_rejected` reasons to the operator
- [ ] T081 [US7] Add a controller status panel to `jetson_xavier/webUI/src/App.vue` displaying mode, applied gains/targets, measured positions, capture pending/generation, requested/active bandwidth, pair/timing faults, carrier and limit causes/counts from `controller_status`
- [ ] T082 [US7] Render bandwidth as read-only in `jetson_xavier/webUI/src/App.vue` with no control capable of writing it, and keep the existing emergency stop control and its precedence unchanged
- [ ] T083 [US7] Run `npm run type-check` in `jetson_xavier/webUI` and confirm clean
- [ ] T084 [US7] Execute S7 in `specs/002-mit-impedance-control/quickstart.md`: verify same-cycle stiffness capture without saturation transient, acknowledgement before hold, status/limit readback within one second, gain refusal, all directions, normal zero-stop, and estop suspension

**Checkpoint**: All seven stories functional across firmware, backend, and UI.

---

## Phase 10: Polish & Cross-Cutting Concerns

- [ ] T085 Execute S9 in `specs/002-mit-impedance-control/quickstart.md`: confirm roughly 2120 frames/s and 28% utilisation on `can0`, below 50% with no errors/bus-off, and record the measured figure in `contracts/can-protocol.md`
- [ ] T086 [P] Audit `spec.md`, `plan.md`, `research.md`, `contracts/can-protocol.md`, and the implemented constants for unresolved provisional safety values; confirm all published values match those finalized before and validated by T044/T045, never reduce the fixed sampling multiple below 10x, and if any validated value must change, invalidate and repeat every affected acceptance test before release
- [ ] T087 [P] Update `specs/002-mit-impedance-control/quickstart.md` with the actual measured numbers observed during S1 through S9 so the guide reflects the delivered system
- [ ] T088 Gate or remove the `FOC_TIMING_INSTRUMENT` debug GPIO path in `foc_timing.cpp` so no instrumentation cost remains in a release build, then re-run the timing check
- [ ] T089 Work the regression checklist in `specs/002-mit-impedance-control/quickstart.md`: compile/calibration/migration, legacy status, bus display, type-check, limit causes, normal stop, sender-failure timeout and emergency-stop suspension in both modes
- [ ] T090 Write `specs/002-mit-impedance-control/hardware-verification.md` with expected wheel behavior, the validation artifact for each layer, and whether hardware verification was completed, deferred, or blocked
- [ ] T091 Record the constitution review in `specs/002-mit-impedance-control/hardware-verification.md`, covering motion safety and estop, wheel sign conventions, CAN compatibility, hard-coded environment values, and vendored/generated content

---

## Dependencies & Execution Order

### Phase Dependencies

- **Setup (Phase 1)**: No dependencies, can start immediately
- **Foundational (Phase 2)**: Depends on Setup, BLOCKS every user story
- **US2 (Phase 3)**: Depends on Foundational. Blocks US1, because torque must not be produced on an unproven loop
- **US1 (Phase 4)**: Depends on US2
- **US3 (Phase 5)**: Depends on Foundational. Can run parallel to US1 once US2 is done
- **US4 (Phase 6)**: Depends on Foundational for `motion_mode`. Naturally follows US1 so both payload formats exist to gate between
- **US5 (Phase 7)**: Depends on US1 for impedance state to time out
- **US6 (Phase 8)**: Depends on US2 for the timing record and US1 for impedance state
- **US7 (Phase 9)**: Depends on US1/US4 for control behavior and US6 for `0x1A0`-`0x1F0` status and capture acknowledgements
- **Polish (Phase 10)**: Depends on all desired stories

### Critical path

```text
Setup -> Foundational -> US2 (T031 measurement gate) -> US1 -> US4 -> US6 -> US7 -> Polish
                       \-> US3                  \-> US5
```

This diagram is authoritative; the phase dependency list above defines the complete branch prerequisites.

**T031 is the schedule risk.** It publishes the measured ceiling. If 1000 Hz is unreachable, the requested
default remains 1000, the active value clamps honestly, and T032 proves the fixed 10x invariants.

### Within each story

- Firmware tasks touching `v13_macnum_wheel_car.ino` are serial with each other
- Computation and state live in the new sketch-local files; wiring lives in the sketch
- Bench validation comes last within a story, after the compile gate

### Parallel Opportunities

- Phase 1: T002 (`can_protocol.h`) and T004 (`can_frames.py`) can start in parallel from the contract; T003 then follows T002, while T005 and T006 follow T004
- Phase 2: T009 (`as5147_fast.*`) runs in parallel with the config work in the sketch
- Once Foundational completes, US3 can proceed alongside US1 by a second person
- Phase 9: T080 and T081 both edit `App.vue` and therefore run serially
- Phase 10: T086 and T087 are documentation-only and parallel

---

## Parallel Example: Phase 1 Setup

```bash
# Independent first implementations from the same published contract:
Task: "Create can_protocol.h with atomic-pair identifiers, ranges and helpers"
Task: "Create jetson_xavier/backend/can_frames.py Python mirror"
```

## Parallel Example: Phase 2 Foundational

```bash
# as5147_fast.* is a new file, independent of the sketch's config work:
Task: "Implement as5147_fast.{h,cpp} pipelined single-transfer reader at 8 MHz"
Task: "Bump CONFIG_VERSION to 2 and add bandwidth and motion_mode fields"
```

---

## Implementation Strategy

### MVP scope: User Story 2 only

The MVP is **US2, the deterministic control loop**, not US1. It is the smaller, safer, independently shippable
increment: it makes the existing velocity controller's bandwidth knowable and repeatable without introducing any
impedance code or any new torque path. Impedance control layered on a non-deterministic loop would be the
opposite trade.

1. Complete Phase 1 Setup
2. Complete Phase 2 Foundational (critical, and where the active ceiling is measured without weakening 10×)
3. Complete Phase 3 US2
4. **STOP and VALIDATE**: quickstart S1, then drive in velocity mode
5. Shippable: a deterministic velocity controller

### Incremental delivery

1. Setup + Foundational → fast sensing, safe config migration, timing math
2. US2 → deterministic loop, existing behaviour preserved (**MVP**)
3. US1 → impedance control, exercised from the bench sender
4. US3 → operator-configurable bandwidth with honest clamping
5. US4 → both modes coexisting, legacy sender provably unaffected
6. US5 → stale-target failsafe closed
7. US6 → determinism, pair faults, and capture acknowledgements observable
8. US7 → operator interface with confirmed-target stiffness capture

### Parallel team strategy

Determinism is a hard serial prerequisite, so the useful split starts after Phase 3:

- Developer A: US1 then US5 (firmware control path)
- Developer B: US3 then US6 (firmware configuration and reporting)
- Developer C: US7 backend and UI, against the bench sender and the contracts, before US1 lands

---

## Notes

- `[P]` means different files with no dependency on incomplete work
- Story labels map to spec.md numbering, so Phase 3 carrying `[US2]` is intentional and explained above
- Every powered task is an attended bench procedure with the vehicle secured and wheels clear
- Commit after each task or logical group; stop at any checkpoint to validate a story independently
- Never edit `src/MPU6050/`, `src/I2Cdev/`, or the SimpleFOC install outside the repository
- Avoid marking two tasks that both edit `v13_macnum_wheel_car.ino` as parallel
