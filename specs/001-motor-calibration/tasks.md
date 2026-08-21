# Tasks: Guided Motor Calibration

**Input**: Design documents from `/specs/001-motor-calibration/`

**Prerequisites**: [plan.md](plan.md), [spec.md](spec.md), [research.md](research.md), [data-model.md](data-model.md), [serial contract](contracts/serial-calibration.md), [quickstart.md](quickstart.md)

**Tests**: This safety-critical feature requires compile validation, focused configuration/CAN checks where feasible, and secured bench validation. Add automated checks only where the repository/toolchain supports them without inventing a new runtime test framework.

**Organization**: Tasks are grouped by user story. All firmware tasks target the existing root sketch to retain hardware ownership.

## Phase 1: Setup

**Purpose**: Establish actual library/toolchain compatibility and safety validation baseline.

- [X] T001 Record the deployed ESP32 FQBN and installed SimpleFOC version/API capabilities, including whether characterization supports a live 1.5 A abort, in `specs/001-motor-calibration/research.md` before selecting characterization/tuning calls.
- [ ] T002 Capture the pre-change serial/CAN command behavior and a secured bench safety baseline in `specs/001-motor-calibration/quickstart.md`.

---

## Phase 2: Foundational Safety and Persistence

**Purpose**: Implement the shared configuration and state boundaries that block all calibration stories.

- [X] T003 Replace the single calibration fields with a versioned, per-motor `RobotConfig` calibration record and bounded validation helpers in `v13_macnum_wheel_car.ino`.
- [X] T004 Add safe configuration migration/reset logic that preserves only a valid CAN ID and defaults all unknown/incomplete calibration records to disarmed calibration mode in `v13_macnum_wheel_car.ino`.
- [X] T005 Add a calibration-mode/session state machine, centralized zero-target/disarm transitions, emergency-stop cancellation, and immediate retry after a failed test in `v13_macnum_wheel_car.ino`.
- [X] T006 Refactor startup configuration in `v13_macnum_wheel_car.ino` so valid per-motor settings are applied before normal FOC initialization and invalid settings never admit normal operation.
- [X] T007 Add the base serial calibration command dispatcher and state/status reporting defined in `specs/001-motor-calibration/contracts/serial-calibration.md` to `v13_macnum_wheel_car.ino`.
- [X] T008 Gate existing serial motor/arm commands and normal CAN velocity/enable frames by calibration mode while preserving immediate CAN emergency stop in `v13_macnum_wheel_car.ino`.

**Checkpoint**: A missing, corrupt, or incomplete configuration starts in a disarmed calibration state; neither serial nor CAN can command normal motion.

---

## Phase 3: User Story 1 - Safe first-time calibration (Priority: P1) MVP

**Goal**: An uncalibrated controller enters a visible, safely locked calibration mode at startup.

**Independent Test**: Use missing, old-schema, invalid, and complete configuration snapshots; verify startup mode, disarm state, serial status, CAN lockout, and emergency stop behavior.

- [X] T009 [US1] Implement startup mode selection and user-visible `C` status output for valid, invalid, and incomplete two-motor records in `v13_macnum_wheel_car.ino`.
- [X] T010 [US1] Add focused configuration-validation and CAN-lockout test vectors or a reproducible manual check section in `specs/001-motor-calibration/quickstart.md`.
- [ ] T011 [US1] Run the secured first-boot, normal-CAN-frame, serial-command, and emergency-stop bench checks; record outcomes and blockers in `specs/001-motor-calibration/quickstart.md`.

**Checkpoint**: User Story 1 is demonstrable without executing any calibration measurement.

---

## Phase 4: User Story 2 - Calibrate electrical alignment for one motor (Priority: P1)

**Goal**: The operator can select either motor, produce a pending pole-pair/direction/offset result, and confirm it without affecting the other motor.

**Independent Test**: On each motor separately, select calibration, run alignment, verify only that motor is energized during the controlled procedure, reject one result, confirm another, and power-cycle to validate persistence.

- [X] T012 [US2] Implement `C1`, `C2`, `CA`, `CY`, `CN`, and `CX` session transitions for per-motor electrical alignment in `v13_macnum_wheel_car.ino`.
- [X] T013 [US2] Implement or adapt the pole-pair discovery and sensor direction/electrical-offset procedure using the verified SimpleFOC API in `v13_macnum_wheel_car.ino`.
- [X] T014 [US2] Validate pending alignment values, present them with motor identity, and atomically persist only a confirmed selected-motor alignment record in `v13_macnum_wheel_car.ino`.
- [ ] T015 [US2] Add and execute secured motor-1/motor-2 alignment, reject, confirm, reset, and estop scenarios in `specs/001-motor-calibration/quickstart.md`.

**Checkpoint**: Each motor can independently have a durable, confirmed alignment record; failures and cancellations leave both motors disarmed.

---

## Phase 5: User Story 3 - Characterize motor and tune its current loop (Priority: P2)

**Goal**: A motor with confirmed pole pairs can be characterized, reviewed, and given safe parameter-derived current-loop tuning.

**Independent Test**: Try `CM` before alignment confirmation (rejected), then characterize each aligned motor while stationary, reject and confirm results, and verify reloaded motor parameters/tuning.

- [X] T016 [US3] Enforce the confirmed-alignment prerequisite and safe stationary-test setup for `CM` in `v13_macnum_wheel_car.ino`.
- [X] T017 [US3] Invoke the verified SimpleFOC resistance/D-Q inductance characterization path only when it enforces live current protection; otherwise implement or select a guarded alternative that aborts at 1.5 A in `v13_macnum_wheel_car.ino`.
- [X] T018 [US3] Enforce the 4.0 V excitation cap, 1.0 A working-current limit, 1.5 A measured-current abort, 15-second characterization timeout, and then validate resistance/inductance and derive bounded current-loop PI/LPF settings in `v13_macnum_wheel_car.ino`.
- [X] T019 [US3] Present characterization and tuning as pending data; confirm atomically into the selected `RobotConfig` record and recompute global readiness in `v13_macnum_wheel_car.ino`.
- [ ] T020 [US3] Add and execute prerequisite, stationary-characterization, 1.5 A abort, timeout, immediate-retry, out-of-range, cancel, reset, and emergency-stop validation scenarios in `specs/001-motor-calibration/quickstart.md`.

**Checkpoint**: A confirmed motor record contains valid alignment, characteristics, and current-loop tuning; an invalid result cannot become active.

---

## Phase 6: User Story 4 - Recalibrate an installed motor (Priority: P3)

**Goal**: The operator can intentionally recalibrate one motor from normal mode without accidentally running it on stale settings.

**Independent Test**: Start fully calibrated, select one motor with `C1` or `C2`, confirm normal motion becomes unavailable, cancel to preserve known-good data, then confirm replacement values and verify only that record changes.

- [X] T021 [US4] Implement deliberate normal-to-calibration transitions that disarm both motors and safely invalidate only the selected motor's in-progress record in `v13_macnum_wheel_car.ino`.
- [X] T022 [US4] Implement `CE` admission checks so normal mode resumes only with two complete valid records and always begins disarmed in `v13_macnum_wheel_car.ino`.
- [ ] T023 [US4] Add and execute recalibration, cancellation-preservation, replacement-confirmation, and normal-mode-admission checks in `specs/001-motor-calibration/quickstart.md`.

**Checkpoint**: A serviced motor cannot silently reuse unverified replacement data, and the untouched motor record is preserved.

---

## Phase 7: Polish and Cross-Cutting Validation

**Purpose**: Validate the safety/protocol contract and leave a reproducible hardware handoff.

- [X] T024 Verify that every command and response implemented in `v13_macnum_wheel_car.ino` conforms to `specs/001-motor-calibration/contracts/serial-calibration.md`; update the contract only for intentional reviewed deviations.
- [X] T025 Verify the normal CAN `0x01` payload layout, scaling, arm/disarm semantics, calibration-mode rejection, and estop behavior against `jetson_xavier/backend/socketio_server.py` and `v13_macnum_wheel_car.ino`.
- [X] T026 Compile `v13_macnum_wheel_car.ino` with the deployed ESP32 FQBN and record the exact command/result in `specs/001-motor-calibration/quickstart.md`.
- [ ] T027 Perform and record the complete secured hardware validation matrix in `specs/001-motor-calibration/quickstart.md`, including both motors, persistence, normal-mode admission, CAN lockout, cancellation, and estop.

## Dependencies & Execution Order

### Phase Dependencies

- Phase 1 has no dependency.
- Phase 2 depends on the API/toolchain decision in T001 and blocks every user story.
- US1 depends on Phase 2.
- US2 depends on US1's mode/session safety behavior.
- US3 depends on confirmed alignment from US2.
- US4 depends on the persistence and mode transitions from US1–US3.
- Polish depends on every intended user story.

### Requirement Coverage

| Requirement | Tasks |
|-------------|-------|
| FR-001–FR-004 | T003–T011 |
| FR-005–FR-007 | T007, T012–T015 |
| FR-008–FR-012 | T016–T020 |
| FR-013–FR-014 | T003–T006, T019, T021–T023 |
| FR-015 | T007, T009, T014, T019, T024 |
| SC-001–SC-005 | T010–T011, T015, T020, T023, T026–T027 |

### Parallel Opportunities

- After T001, the documentation baseline in T002 can proceed while configuration design begins, but source edits in `v13_macnum_wheel_car.ino` are intentionally serial to avoid unsafe merge conflicts.
- T024 and T025 can run in parallel after implementation because they inspect distinct contract boundaries.
- Motor-1 and motor-2 bench executions in T015/T020 can be performed separately once the relevant workflow is implemented.

## Implementation Strategy

### MVP First

1. Complete Phases 1–2.
2. Complete US1 and validate startup lockout and estop behavior before any calibration excitation is added.
3. Add US2 alignment and validate one motor at a time.
4. Add US3 characterization/tuning only after alignment persistence is trustworthy.

### Incremental Delivery

1. Safety mode and persistence foundation.
2. Electrical alignment for both motors.
3. Characteristics/tuning for both motors.
4. Deliberate service recalibration.
5. Compile and hardware matrix sign-off.
