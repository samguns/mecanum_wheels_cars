---

description: "Task list for the V13 Configurator desktop application"
---

# Tasks: V13 Configurator

**Input**: Design documents from `/specs/003-v13-configurator/`

**Prerequisites**: plan.md, spec.md, research.md, data-model.md, contracts/, quickstart.md

**Tests**: Test tasks **are** included. This feature's highest-consequence failure is a string-to-data
transformation that misreports a motor's electrical parameters, and unlike features 001 and 002 that failure is
fully testable on a laptop. Research decision D7 makes a tested parser a design decision, so `cargo test` and
Vitest tasks are mandatory here.

**Organization**: Grouped by user story so each can be implemented and verified independently.

## Format: `[ID] [P?] [Story] Description`

- **[P]**: Can run in parallel (different files, no dependencies)
- **[Story]**: Which user story this task belongs to (US1-US7, matching spec.md numbering)
- Every task names the exact file path or command it touches

## Path Conventions

Two layers change together: the firmware protocol addition and the new desktop app.

- **Firmware**: `v13_macnum_wheel_car.ino` plus the new sketch-local `serial_records.{h,cpp}` beside it
- **Desktop app**: `v13-configurator/` at the repository root, a sibling of `jetson_xavier/`
- **Never touch**: `src/MPU6050/`, `src/I2Cdev/`, the vendored SimpleFOC install, and `jetson_xavier/`

## ⚠️ Powered-work safety gate is cleared

**T001, the governance gate, is CLEARED.** Constitution **1.1.0** (2026-08-21).

**T027, the hardware abort gate, is CLEARED** (2026-08-22, COM6). S6a and S6b passed; evidence is in
`phase2-hardware-verification.md`. Powered tasks in Phases 5–9 may run.

## ⚠️ Phase ordering deviates from strict priority order

**Phase 5 implements User Story 6 (P2) before Phase 6 implements User Story 2 (P1).** US2 is the only story that
energises a motor, and US6 owns the stop control and the intent token. Constitution Principle I forbids offering
a powered procedure before its stop path exists. Story labels stay tied to spec.md numbering.

## ⚠️ Cross-feature dependency

Feature 002 is 42 of 91 tasks complete. **T093 transferred ownership of the serial `V` and `M` commands into
this feature** (2026-08-22). They are registered in `v13_macnum_wheel_car.ino`. Feature 002 still owns the
remaining T036 limiting/`0x1D0` work and T037 eligibility routing. See
`contracts/serial-protocol.md` for the verified command inventory.

---

## Phase 0: Governance Prerequisite (Blocking)

- [X] T001 Obtain an approved MINOR amendment to `.specify/memory/constitution.md` adding the desktop configurator to the Runtime and Deployment Constraints, following the constitution's own amendment requirements: state why the current enumeration is insufficient, identify the migration impact on firmware, backend, UI and deployment, and bump the version. Record the approval, or a written waiver, before Phase 1 begins. **Blocks every subsequent task.** — **DONE 2026-08-21, constitution 1.1.0.** Adds the configurator to the supported stack, adds a Commissioning Tool Constraints section, and adds a desktop-app Change Gates entry. Rationale and migration impact are recorded in the Sync Impact Report at the top of the constitution.

**Checkpoint**: Cleared. The feature is permitted as a fifth runtime component, and the five new commissioning-tool
constraints are mapped to tasks in `plan.md`.

---

## Phase 1: Setup (Shared Infrastructure)

**Purpose**: Scaffold the app, wire the serial plugin, establish every build gate.

- [X] T002 Scaffold a Tauri 2 application with a Vue 3 + TypeScript + Vite frontend in `v13-configurator/`, matching the existing repo frontend versions (Vue 3.5, Vite 7, TypeScript 5.9)
- [X] T003 Add `tauri-plugin-serialplugin` to `v13-configurator/src-tauri/Cargo.toml` and `tauri-plugin-serialplugin-api` to `v13-configurator/package.json`, then register the plugin in `v13-configurator/src-tauri/src/lib.rs`
- [X] T004 [P] Grant only the serial permissions the app needs in `v13-configurator/src-tauri/capabilities/default.json` (available ports, open, close, read, write, control signals), listed explicitly rather than using the blanket default
- [X] T005 [P] Configure Vitest in `v13-configurator/vitest.config.ts` and add `test`, `type-check` and `tauri` scripts to `v13-configurator/package.json`
- [X] T006 [P] Create the firmware module skeleton `serial_records.h` and `serial_records.cpp` at the repository root, included from `v13_macnum_wheel_car.ino`
- [X] T007 [P] Create the shared fixture corpus at `v13-configurator/src-tauri/tests/fixtures/records.txt`, containing one valid line per record type from `contracts/serial-protocol.md` plus real firmware prose that MUST be ignored, including `CAL PENDING ALIGN M1`, `CAL selected motor`, a bare `1` on its own line, and `BW active    [Hz]: 1000   (CLAMPED: sustainable rate ceiling)`
- [X] T008 [P] Add `v13-configurator/` build outputs (`node_modules`, `dist`, `src-tauri/target`) to `.gitignore`
- [X] T009 Establish all baseline gates per quickstart S0 in `specs/003-v13-configurator/quickstart.md`: `arduino-cli compile --fqbn esp32:esp32:esp32 .`, plus `npm run type-check` and `cargo test --manifest-path src-tauri/Cargo.toml` in `v13-configurator/`, recording the results

**Checkpoint**: The app builds, the firmware still compiles, every gate runs.

---

## Phase 2: Foundational (Blocking Prerequisites)

**Purpose**: The wire protocol on both sides. No user story works until the firmware emits structured records and
the app can parse them.

### Firmware: structured records

- [X] T010 Implement the record framing helpers in `serial_records.cpp`: the `#V13 v=1 t=<type>` prefix, `key=value` emission, percent-encoding for free text, `nan` for absent optionals, and a guarantee that a record is never split across lines
- [X] T011 Implement the `id` record in `serial_records.cpp` and emit it unprompted once after boot from `v13_macnum_wheel_car.ino`, carrying firmware level, protocol version, CAN id, motor count and config version
- [X] T012 Register the `Q` command family in `v13_macnum_wheel_car.ino` (`Q`, `QC`, `QT`, `QI`, `QP<ms>`) per `contracts/serial-protocol.md`, dispatched from the communications context only
- [X] T013 Implement the per-motor `cal` record in `serial_records.cpp`: pole pairs, direction, electrical offset, resistance, **both** axis inductances, per-stage completion and the validity verdict
- [X] T014 Implement the `cfg` record in `serial_records.cpp`: requested and active bandwidth, the clamp flag, control rate, carrier, decimation, both motor modes, the bus window and the calibrated rollup
- [X] T015 Implement the per-motor `motor` record in `serial_records.cpp`, emitting `nan` or an explicit unavailable marker for the output-voltage limit bit and the pair-fault field, which feature 002 does not yet populate, per the degraded-fields table in `contracts/serial-protocol.md`
- [X] T016 Implement the `timing` record in `serial_records.cpp` from the existing `LoopTimingRecord` and `FocTimingConfig`
- [X] T017 Implement the `bus` record in `serial_records.cpp`, reporting bus millivolts and the protection state
- [X] T018 Implement request tagging in `v13_macnum_wheel_car.ino`: strip a leading `#<tag>;` prefix before the Commander sees the command, retain the tag, and emit exactly one `ack` per state-changing command carrying the tag, command, outcome and percent-encoded refusal reason; an untagged command MUST still work and acknowledge with `tag=0`

### Firmware: the safety gate

- [X] T019 Extend `calibrationAbortRequested()` in `v13_macnum_wheel_car.ino` to treat the serial abort byte `0x18` as an abort at every point it already checks the CAN path, routing through the existing `failCalibration()` so the disarm and reporting behaviour is identical, and emitting an `ack` with `cmd=ABORT`. **Without this there is no software way to stop an energised stage on a bench with no CAN bus.**
- [X] T020 Intercept and consume `0x18` in `v13_macnum_wheel_car.ino` **before the SimpleFOC Commander reads it**, for the case where no calibration stage is running: discard any partially assembled command line, disarm, and emit the same `cmd=ABORT` acknowledgement. Without this the byte is buffered as command text, produces a nonsense command, can corrupt a legitimate command mid-line, and the operator's stop press is met with silence
- [X] T021 Emit `calprog` progress records from inside both calibration loops in `v13_macnum_wheel_car.ino`, at the existing loop points in the alignment sweep and the characterisation ramp, carrying stage, percent and the energised flag

### Firmware: remaining protocol surface

- [X] T022 Implement the `calpend` record in `serial_records.cpp` and emit it when a stage completes with a result awaiting a decision, for both stages
- [X] T023 Implement the `fault` record in `serial_records.cpp`, covering calibration, timing, bus and protocol kinds, with a zero-valued legacy `cooldown_ms` compatibility field
- [X] T024 Add the `N` and `N<hex>` bus identity command to `v13_macnum_wheel_car.ino`, validating the `0x001`-`0x7FF` range, refusing while either motor is armed, persisting before acknowledging success, and reporting a stated reason on refusal
- [X] T025 Enforce a firmware-side floor on the `QP` telemetry period in `v13_macnum_wheel_car.ino`, clamping a too-fast request and reporting the applied value, so telemetry cannot crowd out the deterministic control loop
- [X] T026 Run `arduino-cli compile --fqbn esp32:esp32:esp32 .`, then capture a console session exercising every record type and diff it field by field against `contracts/serial-protocol.md`
- [X] T027 **Hardware safety gate**: execute quickstart S6a and S6b in `specs/003-v13-configurator/quickstart.md`, covering the abort inside a running stage and with no stage running, confirming the motor de-energises and an acknowledgement is returned in both cases, and that a half-typed command is discarded rather than merged. 10 trials per stage per motor. **Blocks every powered task in Phases 5 through 9.**

### App: protocol and session core

- [X] T028 [P] Define the typed record structures in `v13-configurator/src-tauri/src/protocol/records.rs`, one per record type, modelling the degraded fields as explicitly unavailable rather than as false
- [X] T029 [P] Implement the record parser in `v13-configurator/src-tauri/src/protocol/parser.rs` as a pure function with no I/O: ignore any line lacking the `#V13` prefix, refuse an unrecognised `v`, tolerate unknown keys, decode percent-encoding and `nan`
- [X] T030 Write parser unit tests in `v13-configurator/src-tauri/src/protocol/parser.rs` against `tests/fixtures/records.txt`, satisfying the full checklist in quickstart S1 of `specs/003-v13-configurator/quickstart.md`: every record type round-tripped, firmware prose ignored, unrecognised version refused, unknown key tolerated, and truncated, missing-`=`, doubled-field and empty inputs rejected without panicking
- [X] T031 [P] Implement the command encoder in `v13-configurator/src-tauri/src/protocol/encode.rs`, including the `#<tag>;` prefix and the raw abort byte
- [X] T032 Implement exclusive port ownership and connection in `v13-configurator/src-tauri/src/session/port.rs`, setting DTR and RTS to the non-resetting state on open so connecting does not reboot the controller, and reporting a reset if the board forces one
- [X] T033 Implement the session state machine in `v13-configurator/src-tauri/src/session/state.rs` per `data-model.md`, with the `disconnected`, `identifying`, `ready`, `busy` and `lost` states
- [X] T034 Write session state machine unit tests in `v13-configurator/src-tauri/src/session/state.rs`, covering that leaving `ready` clears the in-flight request and invalidates the intent token, and that entering `lost` marks mirrored data stale rather than discarding it
- [X] T035 Implement single-in-flight request handling with tag matching and the contract's timeouts in `v13-configurator/src-tauri/src/session/mod.rs`, using the 2 s normal window and the 60 s calibration window
- [X] T036 Make `abort` the only request permitted while the session is `busy` in `v13-configurator/src-tauri/src/session/mod.rs`, so the stop path can never queue behind the stage it needs to interrupt
- [X] T037 [P] Implement the error model in `v13-configurator/src-tauri/src/session/error.rs` with the contract's kinds, keeping `refused` and `timeout` distinct so an unknown outcome is never reported as a decision
- [X] T038 Implement the Tauri command and event surface in `v13-configurator/src-tauri/src/commands.rs` per `contracts/app-ipc.md`, wiring events for connection, telemetry, progress, pending results, faults, protocol errors and staleness
- [X] T039 Implement the identification gate in `v13-configurator/src-tauri/src/session/mod.rs`: require an `id` record within the connection timeout, and refuse to offer any action against an unrecognised device or unsupported protocol or config version
- [X] T040 Verify the app gates: `cargo test --manifest-path src-tauri/Cargo.toml` and `npm run type-check` in `v13-configurator/`

**Checkpoint**: The firmware speaks a versioned machine protocol, the app parses it with tests, and both abort
paths are proven on hardware. Powered work is now permitted.

---

## Phase 3: User Story 1 - See what a controller currently believes (Priority: P1) 🎯 MVP

**Goal**: Connect to a controller and read its stored motor parameters and calibration state, replacing the need
for a serial terminal to inspect a unit.

**Independent Test**: Connect a calibrated controller, read it, and confirm every displayed field matches the
controller's own `C` and `B` console reports. Then connect an uncalibrated controller and confirm it is clearly
marked as requiring calibration. Produces no motion at all.

- [X] T041 [US1] Create the device store in `v13-configurator/src/stores/device.ts`, holding only controller-reported values with their freshness, and never any parsed serial text
- [X] T042 [US1] Wire `list_ports`, `connect` and `disconnect` from the frontend in `v13-configurator/src/stores/session.ts`, with no automatic connection on launch
- [X] T043 [US1] Implement `read_all` end to end, from `v13-configurator/src-tauri/src/commands.rs` to the device store, returning a full snapshot of calibration, configuration and motor state
- [X] T044 [US1] Build the connection panel in `v13-configurator/src/components/ConnectionPanel.vue`, showing port selection, connection state and the identified device
- [X] T045 [US1] Build the motor parameters card in `v13-configurator/src/components/MotorParametersCard.vue` following the reference design, rendering pole pairs, direction, resistance and **both** axis inductances as read-only display fields with no editable control
- [X] T046 [US1] Display per-motor calibration state for both motors in `v13-configurator/src/components/CalibrationStatus.vue`, naming which motor is incomplete when a unit is uncalibrated
- [X] T047 [US1] Display the electrical offset and the derived current-loop settings in `v13-configurator/src/components/MotorParametersCard.vue`, as read-only reported values
- [X] T048 [US1] Add a unit to every physical quantity in `v13-configurator/src/components/MotorParametersCard.vue`, and state next to the read-only values that changing them requires running calibration
- [X] T049 [US1] Distinguish device-reported values from unwritten operator edits visually in `v13-configurator/src/views/ConfigView.vue`
- [X] T050 [US1] Distinguish controller-reported measurements from configurator-derived values in `v13-configurator/src/views/ConfigView.vue` and `v13-configurator/src/components/`, marking exactly the four derived quantities named in spec FR-027: wheel label, duty percentage, freshness age, and out-of-range flag
- [X] T051 [US1] Add a last-refreshed indicator and an explicit re-read action in `v13-configurator/src/views/ConfigView.vue`
- [X] T052 [US1] Render the unrecognised-device and version-mismatch states in `v13-configurator/src/views/ConfigView.vue`, presenting no values as trustworthy in either case
- [X] T053 [US1] Display a reported value outside its expected range as reported but flagged in `v13-configurator/src/components/MotorParametersCard.vue`, never clamped into something plausible
- [X] T054 [US1] Write frontend tests in `v13-configurator/src/components/MotorParametersCard.spec.ts` for unit rendering, the read-only guarantee, out-of-range flagging, and the derived-versus-reported distinction
- [X] T055 [US1] Execute quickstart S2 and S3 in `specs/003-v13-configurator/quickstart.md`, confirming the controller does not reboot on connect and every field matches the console — **DONE 2026-08-22 COM6.** DTR/RTS deasserted open (same as `session/port.rs`): uptime 146608→158635, no reset. `Q` cal/cfg matched `B` (1000 Hz, 10 kHz/20 kHz). See `hardware-verification.md`.
- [X] T056 [US1] Execute the remaining rows of quickstart S4 in `specs/003-v13-configurator/quickstart.md` that no other task claims: port-loss mid-session, telemetry staleness within 2 seconds, the non-controller device, and the unsupported protocol version — **DONE 2026-08-22.** Staleness (`QP0` 3.56 s gap). Version mismatch in Rust tests. COM9 USB-CAN-A: no `#V13` id. Unplug mid-session on the live CH340 (then COM7): `CONNECTION lost`, last `id`/`cfg` kept, stale, `can_act=false`, reopen refused, only COM9 remained.

**Checkpoint**: A working read-only configuration viewer. Shippable alone, with zero motion risk.

---

## Phase 4: User Story 3 - Know which wheel you are about to energise (Priority: P1)

**Goal**: Make the connected controller and the selected motor unmistakable, so no powered action can be aimed at
the wrong wheel.

**Independent Test**: Connect one controller, select each of its two motors in turn, read each, and confirm the
displayed values and the named wheel correspond to the physically expected wheel.

- [X] T057 [US3] Implement wheel label derivation in `v13-configurator/src-tauri/src/session/identity.rs` from the CAN id and motor index, following `contracts/serial-protocol.md` where motor 1 is the right wheel, motor 2 the left, node `0x01` the front pair and `0x02` the rear
- [X] T058 [US3] Build the motor selector in `v13-configurator/src/components/MotorSelector.vue`, labelling each motor by its wheel rather than by index alone
- [X] T059 [US3] Show the connected controller's identity and the selected motor persistently in `v13-configurator/src/App.vue`, visible on every screen without scrolling
- [X] T060 [US3] Present both motors' calibration state simultaneously in `v13-configurator/src/views/ConfigView.vue`, so an operator can see which of the pair still needs work without switching selection
- [X] T061 [US3] Handle a selection whose controller has become unreachable in `v13-configurator/src/stores/session.ts`, showing the session as disconnected and withdrawing every action
- [X] T062 [US3] Write tests for the wheel label mapping in `v13-configurator/src-tauri/src/session/identity.rs`, covering both node ids and both motor indices
- [X] T063 [US3] Verify spec criterion SC-007 across 10 trials covering both motors, using `specs/003-v13-configurator/spec.md` as the acceptance reference — **DONE 2026-08-22.** Operator named the moving wheel after a ≥10 s alignment rotate-then-abort (no `CY`). 10/10, no hedging: M1→Rear Right (trials 1,4,6,7,9), M2→Rear Left (2,3,5,8,10). Trial 3 spoken as `read left`. Stored cal unchanged; both motors `armed=0`.

**Checkpoint**: Wheel identity is unambiguous, a precondition for any powered procedure.

---

## Phase 5: User Story 6 - Be prevented from doing something unsafe (Priority: P2)

**Goal**: Build the guard layer before anything can energise a motor.

**Prerequisite**: T027 must have passed.

**Independent Test**: Attempt each guarded action in a state where the controller refuses it, and confirm the app
refuses for the same stated reason without sending a conflicting request. Inspect every screen for a motion
control and find none.

- [X] T064 [US6] Surface the controller's own refusal reason from the `ack` record in `v13-configurator/src/components/RefusalNotice.vue`, naming the affected motor and never reducing it to a generic error
- [X] T065 [US6] Explain a visibly unmet precondition before an attempt in `v13-configurator/src/views/ConfigView.vue`, and never substitute a local verdict for the controller's or suppress a refusal the controller returns, per spec FR-016
- [X] T066 [US6] Implement the global stop control in `v13-configurator/src/components/StopControl.vue`, reachable at all times while any powered procedure is possible, wired to the `abort` command that bypasses the in-flight queue
- [X] T067 [US6] Show each motor's arm state on the same view as any powered action in `v13-configurator/src/App.vue`, visible without scrolling or changing view
- [X] T068 [US6] Audit every view under `v13-configurator/src/views/` and `v13-configurator/src/components/` and confirm no control can command vehicle motion, and that the raw SimpleFOC `a` and `b` passthrough commands are never sent
- [X] T069 [US6] Implement the single-use expiring intent token in `v13-configurator/src-tauri/src/session/intent.rs`, recording the motor, the wheel name shown to the operator and the acknowledged safety precondition, required structurally for any powered command
- [X] T070 [US6] Write intent token tests in `v13-configurator/src-tauri/src/session/intent.rs` covering single use, expiry, and rejection of a token issued for a different motor
- [X] T071 [US6] Verify refusal parity per quickstart S4 in `specs/003-v13-configurator/quickstart.md`, confirming across 20 attempts that the app's stated reason matches the controller's own — **DONE 2026-08-22.** 20/20 tagged refusals returned `ok=0` with a stated reason (`B50`/`B99999` range, reversed/OOR `V`, `N000`/`N800`, `M9V`, bare `C`/`CY`). The app surfaces `ack.reason` verbatim.
- [X] T072 [US6] Execute quickstart S12a in `specs/003-v13-configurator/quickstart.md`, verifying spec criterion SC-013: across 5 trials, an operator not told where to look identifies the energised indication and the selected motor's arm state within 2 seconds — **DONE 2026-08-22.** Retry 5/5: energised and selected always correct; arm `disarmed` on 1/2/4/5 and `armed` on trial 3 (operator set that motor armed on purpose). No scroll, answers immediate.

**Checkpoint**: The guard layer exists. Calibration may now be built on top of it.

---

## Phase 6: User Story 2 - Run a guided calibration without the serial console (Priority: P1)

**Goal**: Start, watch, and resolve a calibration for a selected motor entirely from the app.

**Prerequisite**: T027 passed and Phase 5 complete. This phase energises motors.

**Independent Test**: With the vehicle secured and wheels clear, run a full alignment and characterisation
sequence for one motor from the app, accept the result, and confirm the controller reports the same saved values.

- [X] T073 [US2] Implement the calibration session state machine in `v13-configurator/src-tauri/src/session/calibration.rs` per `data-model.md`, with the confirming, running, pending, resolved and failed phases
- [X] T074 [US2] Wire `confirm_intent` and `calibrate_start` in `v13-configurator/src/views/ConfigView.vue`, with a confirmation that names the selected wheel and states the physical safety precondition before anything energises
- [X] T075 [US2] Display calibration progress and the energised indication in `v13-configurator/src/components/CalibrationProgress.vue`, driven only by `calprog` records and never inferred from elapsed time
- [X] T076 [US2] Present a completed stage as a pending result in `v13-configurator/src/components/CalibrationPending.vue`, showing the measured values without treating them as stored
- [X] T077 [US2] Wire accept and reject to `CY` and `CN` in `v13-configurator/src-tauri/src/session/calibration.rs`, reporting success only on an acknowledgement with a successful outcome
- [X] T078 [US2] Trigger and display a verification re-read after an accepted result in `v13-configurator/src/views/ConfigView.vue`
- [X] T079 [US2] Display calibration failure and over-current abort with the controller's reason, permitting an immediate operator-confirmed retry
- [X] T080 [US2] Handle interruption in `v13-configurator/src-tauri/src/session/calibration.rs`: a link loss, an app closure or an abort leaves the outcome unknown rather than failed, discards pending values, and prompts a re-read
- [X] T081 [US2] Warn before discarding a pending result when the operator changes the selected motor, in `v13-configurator/src/components/MotorSelector.vue`
- [X] T082 [US2] Scope every powered request to the single selected motor in `v13-configurator/src-tauri/src/session/calibration.rs`, asserting the other motor of the pair is left de-energised, per spec FR-034b
- [X] T083 [US2] Write calibration state machine tests in `v13-configurator/src-tauri/src/session/calibration.rs`, covering every transition, asserting a pending result is never treated as stored without a successful accept acknowledgement, and asserting the unselected motor is never included in a powered request
- [X] T084 [US2] Execute quickstart S5 and S7 in `specs/003-v13-configurator/quickstart.md`: a full two-motor calibration with no console typing, then 20 interrupted procedures confirming no result is ever shown as stored and every motor ends de-energised — **DONE 2026-08-22.** S5 earlier (tagged C/CA/CN/CY/CM both motors). S7: 20 mid-alignment `0x18` aborts, 20/20 both motors `armed=0`, stored cal offsets unchanged (no CY).

**Checkpoint**: Calibration works from the app, on top of a proven stop path.

---

## Phase 7: User Story 4 - Review live debug telemetry (Priority: P2)

**Goal**: A continuously refreshing diagnostic view of measured values, timing and fault causes.

**Independent Test**: Confirm measured position, velocity, current, bus voltage, arm state and mode refresh
continuously and agree with the console, and that values are marked stale when updates stop.

- [X] T085 [US4] Implement `set_telemetry` and the telemetry event stream in `v13-configurator/src-tauri/src/commands.rs`, returning the applied period after any firmware clamp
- [X] T086 [US4] Build the debug view in `v13-configurator/src/views/DebugView.vue`, matching the reference design's DEBUG sidebar section
- [X] T087 [US4] Display per-motor measured position, velocity, current, arm state and mode in `v13-configurator/src/components/MotorTelemetry.vue`
- [X] T088 [US4] Display bus voltage and the bus-voltage protection state in `v13-configurator/src/components/BusVoltagePanel.vue`, from the `bus` record, completing spec FR-023's required field set
- [X] T089 [US4] Display the timing panel in `v13-configurator/src/components/TimingPanel.vue`: nominal and measured rate, overruns, worst cycle, duty, carrier, and requested versus active bandwidth with its clamp state
- [X] T090 [US4] Decode and display fault and effort-limit causes distinguishably in `v13-configurator/src/components/FaultPanel.vue`, covering current limiting, voltage limiting, protection trips, timing faults and command timeouts, and labelling the two feature-002 degraded fields as unavailable rather than as a confident negative
- [X] T091 [US4] Mark telemetry stale within 2 seconds of updates stopping in `v13-configurator/src/stores/device.ts`, showing staleness as a state rather than blanking the values
- [X] T092 [US4] Execute quickstart S9 in `specs/003-v13-configurator/quickstart.md`, and confirm via the controller's own `T` report that streaming telemetry introduces no new control-loop overruns — **DONE 2026-08-22.** `QP200` then `T`: measured rate 10000.0 Hz, overruns 0, consecutive 0, timing fault none.

**Checkpoint**: A diagnostic view good enough to identify an induced fault without the console.

---

## Phase 8: User Story 5 - Write the controller's operating settings (Priority: P2)

**Goal**: Write the four writable settings with confirmation and acknowledged persistence.

**⚠️ Blocked on feature 002.** The `V` and `M` commands do not exist in firmware. T093 resolves that before the
rest of this phase can complete.

**Independent Test**: Change each writable setting, write it, power cycle, re-read, and confirm it survived; then
attempt a write with a motor armed and confirm refusal.

- [X] T093 [US5] Resolve the feature 002 dependency before proceeding: confirm feature 002 tasks T036 (bus-voltage window `V` command) and T057 (motion mode `M` command) are complete in `v13_macnum_wheel_car.ino`, or implement those two commands here and record the transfer of ownership in `specs/002-mit-impedance-control/tasks.md`
- [X] T094 [US5] Build the writable settings form in `v13-configurator/src/components/SettingsForm.vue`, covering exactly the four writable settings: bus identity, current-loop bandwidth, bus-voltage window and per-motor control mode
- [X] T095 [US5] Validate every entry against its permitted range before submitting, in `v13-configurator/src/components/SettingsForm.vue`, showing the valid range on refusal and leaving the stored value unchanged
- [X] T096 [US5] Prevent a write from being submitted while any motor is armed, in `v13-configurator/src/components/SettingsForm.vue`, stating that a disarm is required, while still surfacing the controller's own refusal if a write nevertheless reaches it, per spec FR-020
- [X] T097 [US5] Show a before-and-after comparison and require confirmation before writing, in `v13-configurator/src/components/SettingsForm.vue`
- [X] T098 [US5] Wire `write_setting` in `v13-configurator/src-tauri/src/commands.rs` to the corresponding firmware commands (`N`, `B`, `V`, `M`), reporting success only on an acknowledgement
- [X] T099 [US5] Report an interrupted or failed write as leaving the stored state unknown in `v13-configurator/src-tauri/src/session/mod.rs`, prompting a re-read rather than displaying the attempted values as stored
- [X] T100 [US5] Write validation tests in `v13-configurator/src/components/SettingsForm.spec.ts`, covering each setting's boundaries, the reversed bus-window case, and the armed-write prevention
- [X] T101 [US5] Execute quickstart S8 in `specs/003-v13-configurator/quickstart.md`, including the bus identity change followed by a reconnect showing the new identity — **DONE 2026-08-22.** `N201`+`V7500,23000` persisted across DTR reset (boot `CAN ID: 0x201` / `id canid=0x201`). Restored `N202` and `V7000,24000`; final `Q` `canid=0x202`.

**Checkpoint**: All four settings writable, persistent, and refused when they should be.

---

## Phase 9: User Story 7 - Know who is connected and what they changed (Priority: P3)

**Goal**: Display an operator identity and record what a session changed, for attribution only.

**Independent Test**: Complete a session that changes a configuration, then confirm the record shows which
operator acted, which motor was affected, and what changed.

- [X] T102 [US7] Implement the local operator profile in `v13-configurator/src-tauri/src/storage/settings.rs`, for attribution only, with no credentials stored and no authentication
- [X] T103 [US7] Display the operator identity and offer ending the session in `v13-configurator/src/components/OperatorFooter.vue`, following the reference design's sidebar footer
- [X] T104 [US7] Implement the append-only change log in `v13-configurator/src-tauri/src/storage/changelog.rs`, appending an entry after each acknowledged write or accepted calibration with the target, wheel label, before and after values read from the device, and the acting operator
- [X] T105 [US7] Build the change log view in `v13-configurator/src/views/HistoryView.vue`, filterable by controller and motor
- [X] T106 [US7] Handle the no-identity case consistently in `v13-configurator/src-tauri/src/storage/changelog.rs`, either attributing to an explicitly anonymous session or refusing the change, visibly and the same way every time
- [X] T107 [US7] Execute quickstart S10 in `specs/003-v13-configurator/quickstart.md`, confirming a 10-change session is fully retrievable and that no motion control exists anywhere — **DONE 2026-08-22.** Ten acknowledged writes (`V`/`M`/`B`/`N202`) stored in `%APPDATA%\v13-configurator\changelog.jsonl` with target, wheel, before/after, operator. Last write used a cleared profile → `anonymous`. History view now renders those values. Vue screens have no arm/setpoint/motion control (vitest SC-010). Bench restored to `0x202`, 7000–24000 mV, 1000 Hz.

**Checkpoint**: All seven stories functional across firmware and app.

---

## Phase 10: Polish & Cross-Cutting Concerns

- [ ] T108 Execute quickstart S11 in `specs/003-v13-configurator/quickstart.md`: build and run on each supported platform, confirming port enumeration, non-resetting connect and both abort paths pass on each
- [X] T109 Execute quickstart S12b in `specs/003-v13-configurator/quickstart.md`, verifying spec criterion SC-014 and FR-032a: a full read, a calibration and a setting write with the Jetson powered off and no vehicle bus connected — **DONE 2026-08-22.** Direct COM6 serial only; Jetson and vehicle CAN were not in the path. `Q` read, two-motor `CA`/`CM`/`CY`, and `V`/`M`/`N` writes all succeeded.
- [ ] T110 Execute quickstart S12c in `specs/003-v13-configurator/quickstart.md`, measuring and recording spec criteria SC-002 and SC-011: the configurator against the console across three operators, and replacement-unit commissioning end to end
- [X] T111 [P] Reconcile every measured value with its documented estimate across `specs/003-v13-configurator/research.md` gates M1 through M5: observed reset behaviour, acknowledgement latency, abort reliability on both paths, measurement perturbation, and sustainable telemetry rate
- [X] T112 [P] Update `specs/003-v13-configurator/quickstart.md` with the actual observed numbers so the guide reflects the delivered system
- [X] T113 Work the regression checklist at the end of `specs/003-v13-configurator/quickstart.md`, confirming the console behaves identically for a human, prose is unchanged, structured records never collide with human output, and the deterministic control loop is undisturbed — **DONE 2026-08-22.** `B`/`V`/`C` prose still printed; `#V13` stayed on its own lines; `T` during `QP200` showed 0 overruns.
- [X] T114 Write `specs/003-v13-configurator/hardware-verification.md` with the expected behaviour, the validation artifact for each layer, and whether hardware verification was completed, deferred or blocked
- [X] T115 Record the constitution review in `specs/003-v13-configurator/hardware-verification.md`, covering motion safety and both stop paths, wheel identity, the serial protocol contract, hard-coded values, the approved amendment from T001, and that no vendored content was edited

---

## Phase 11: Feature 002 serial leftovers (2026-08-22)

Serial-facing work that was still open in `002-mit-impedance-control`, adapted to the `#V13` protocol and the
configurator. CAN pair RX, status frames, Jetson UI, and hardware measurement gates stay in 002.

- [X] T116 [US5] Implement current and output-voltage limiting with rising-edge cause counts in `impedance_control.cpp`, report those causes from serial `I` / `t=motor` `limits`, and keep `pairfault` as a real 0/1 latch
- [X] T117 [US5] Add the per-field impedance eligibility guard in `v13_macnum_wheel_car.ino`, require it for `M*I` and impedance `A`, and switch eligible motors to `MotionControlType::torque` / `TorqueControlType::foc_current`
- [X] T118 [US5] Add the human `I` report and `#V13 t=imp` records; emit `imp` from `Q`, `QI`, `QP`, and `I`
- [X] T119 [US5] Add serial apply `K<n>,<p_mrad>,<v>,<kp>,<kd>,<tff>` in `v13_macnum_wheel_car.ino` as the bench stand-in for CAN pairs, with the 002 field ranges
- [X] T120 [US5] Parse `t=imp` and evaluate the output-voltage limit bit when `pairfault` is 0/1, in `v13-configurator/src-tauri/src/protocol/`
- [X] T121 [US5] Add `apply_impedance` in `v13-configurator/src-tauri/src/commands.rs`, range-checked before send
- [X] T122 [US5] Build the debug impedance panel in `v13-configurator/src/components/ImpedancePanel.vue`
- [X] T123 [US5] Replace the hardcoded characterisation bandwidth seed so `config.current_bandwidth_hz` is the source of truth (002 T019)

---

## Dependencies & Execution Order

### Phase Dependencies

- **Governance (Phase 0)**: No dependencies. BLOCKS everything
- **Setup (Phase 1)**: Depends on Phase 0
- **Foundational (Phase 2)**: Depends on Setup. BLOCKS every user story
- **US1 (Phase 3)**: Depends on Foundational. The MVP
- **US3 (Phase 4)**: Depends on US1 for the views it extends
- **US6 (Phase 5)**: Depends on US1, US3, and T027
- **US2 (Phase 6)**: Depends on US6 for the stop control and intent token, and on T027
- **US4 (Phase 7)**: Depends on Foundational only. Can run parallel to US2 once US1 exists
- **US5 (Phase 8)**: Depends on US6, and on the feature 002 dependency resolved by T093
- **US7 (Phase 9)**: Depends on US2 and US5, because it records what they change
- **Polish (Phase 10)**: Depends on all desired stories

### Critical path

```text
T001 governance -> Setup -> Foundational (T019, T020 -> T027 safety gate) -> US1 -> US3 -> US6 -> US2 -> US7 -> Polish
                                                                              \-> US4        \-> US5 (needs T093)
```

**T001** is a human decision and should be started immediately, in parallel with nothing, because everything
waits on it.

**T027** proves both abort paths. Until it passes, Phases 5 through 9 must not run a powered scenario.

**T030** is the correctness gate. The parser is where a silently misreported motor parameter would originate, and
it is the one high-consequence failure catchable on a laptop.

### Within each story

- Rust core before the frontend that consumes it
- Typed records and parsing before any view renders them
- Tests alongside the pure logic they cover
- Bench validation last within a story, after the build gates

### Parallel Opportunities

- Phase 1: T004 through T008 are different files and run in parallel
- Phase 2 splits by layer: the firmware chain T010-T027 and the app chain T028-T040 are independent until
  integration, so two people can work simultaneously
- Phase 2 within the app: T028, T029, T031 and T037 are separate files
- Phase 7 (US4) needs only the Foundational layer, so it can proceed alongside US6 and US2
- Phase 10: T111 and T112 are documentation only

---

## Parallel Example: Phase 2 by layer

```bash
# Developer A, firmware:
Task: "Implement record framing helpers in serial_records.cpp"
Task: "Implement the serial abort byte in calibrationAbortRequested()"

# Developer B, app core:
Task: "Define typed record structures in protocol/records.rs"
Task: "Implement the record parser in protocol/parser.rs"
```

---

## Implementation Strategy

### MVP scope: User Story 1 only

The MVP is **US1, the read-only configuration viewer**. It replaces the need for a serial terminal to inspect a
controller, produces no motion whatsoever, and still requires the full protocol foundation, so it proves the
riskiest plumbing without energising anything. It also depends on nothing missing from feature 002.

1. Complete Phase 0 governance
2. Complete Phase 1 Setup
3. Complete Phase 2 Foundational, including the T027 safety gate
4. Complete Phase 3 US1
5. **STOP and VALIDATE**: quickstart S2, S3 and the remaining S4 rows
6. Shippable: a read-only configuration and diagnostic viewer

### Incremental delivery

1. Governance + Setup + Foundational → versioned protocol both sides, tested parser, both abort paths proven
2. US1 → read-only viewer (**MVP**)
3. US3 → unambiguous wheel identity
4. US6 → guard layer, stop control, intent token
5. US2 → guided calibration, the feature's headline value
6. US4 → live debug telemetry
7. US5 → the four writable settings, once feature 002 unblocks two of them
8. US7 → operator identity and change history

### Parallel team strategy

- Phase 0 is a human approval; start it first and immediately
- Phase 2 divides by layer: one developer on the firmware protocol chain, one on the Rust core
- After US1, a second developer can take US4 in full, since it depends only on the Foundational layer
- US6 and US2 are best done by the same person, because the stop control and the procedure that needs it are one
  safety argument

---

## Notes

- `[P]` means different files with no dependency on incomplete work
- Story labels map to spec.md numbering, so Phase 5 carrying `[US6]` before Phase 6 carrying `[US2]` is
  intentional and explained above
- Every powered task is an attended bench procedure with the vehicle secured and the wheels clear
- Test tasks are included because research D7 makes a tested parser a design decision
- Never edit `src/MPU6050/`, `src/I2Cdev/`, the vendored SimpleFOC install, or anything under `jetson_xavier/`
- The firmware and the app change the serial contract together; `contracts/serial-protocol.md` is updated in the
  same change as any protocol edit
