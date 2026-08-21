# Feature Specification: Guided Motor Calibration

**Feature Branch**: `001-motor-calibration`  
**Created**: 2026-08-19  
**Status**: Draft  
**Input**: User description: "Implement a separate motor calibration mode that starts when the saved configuration is not calibrated, supports deliberate per-motor calibration, measures electrical/sensor alignment and motor characteristics, persists the results, and tunes current-loop PID values."

## User Scenarios & Testing *(mandatory)*

### User Story 1 - Safe first-time calibration (Priority: P1)

As an operator starting an uncalibrated controller, I am placed in a dedicated calibration mode rather than motion-control mode, so that I can prepare each motor safely before it can drive the vehicle.

**Why this priority**: A controller must not use unverified electrical and sensor settings to energize a drive motor.

**Independent Test**: Start with a valid saved configuration whose calibration state is false and verify that both motors remain disarmed, normal velocity control is unavailable, and the calibration choices are presented.

**Acceptance Scenarios**:

1. **Given** a saved configuration whose calibration state is false, **When** the controller completes startup, **Then** it enters calibration mode and both motors are disarmed.
2. **Given** calibration mode is active, **When** a normal serial or CAN motion command is received, **Then** no motor motion is commanded.
3. **Given** a fully calibrated saved configuration, **When** the controller completes startup, **Then** it starts in its normal, disarmed operating mode rather than calibration mode.

---

### User Story 2 - Calibrate electrical alignment for one motor (Priority: P1)

As an operator, I can deliberately select either motor and run its electrical alignment calibration to determine its pole-pair count, sensor direction, and electrical offset.

**Why this priority**: These are prerequisites for reliable field-oriented control and must be correct before any electrical characterization is accepted.

**Independent Test**: From calibration mode, select motor 1 and then motor 2 in separate runs; complete or cancel each run and inspect the reported result and saved motor-specific state.

**Acceptance Scenarios**:

1. **Given** calibration mode is active, **When** the operator selects a motor for electrical alignment, **Then** the controller runs the alignment procedure only for that selected motor and keeps the other motor disarmed.
2. **Given** electrical alignment completes successfully, **When** the result is confirmed, **Then** the motor's pole-pair count, sensor direction, and electrical offset are shown to the operator and saved as that motor's pending calibration data.
3. **Given** electrical alignment fails, is cancelled, or produces an invalid result, **When** the procedure ends, **Then** its new values are not accepted, the motor is disarmed, and the operator receives a clear failure reason or retry instruction.

---

### User Story 3 - Characterize motor and tune its current loop (Priority: P2)

As an operator, after confirming a motor's pole-pair count, I can measure its phase resistance and inductance and have the controller derive safe starting current-loop PID settings.

**Why this priority**: The electrical model is required to tune the current controller predictably and reduce trial-and-error setup.

**Independent Test**: Attempt characterization before and after a confirmed pole-pair calibration, then validate that a successful run produces persisted resistance, inductance, and calculated current-loop tuning for only the selected motor.

**Acceptance Scenarios**:

1. **Given** the selected motor has no confirmed pole-pair result, **When** the operator requests characteristics calibration, **Then** the request is rejected with an instruction to complete electrical alignment first.
2. **Given** the selected motor has a confirmed pole-pair result, **When** the operator starts characteristics calibration, **Then** the controller measures phase resistance and inductance while both motors remain disarmed outside the controlled test sequence.
3. **Given** characteristics calibration succeeds, **When** the operator confirms the result, **Then** resistance, inductance, and current-loop PID values derived from them are displayed and saved for that motor.
4. **Given** a measurement is unsafe, inconsistent, or fails, **When** the procedure ends, **Then** no replacement motor-characteristic or PID values are saved and both motors remain disarmed.

---

### User Story 4 - Recalibrate an installed motor (Priority: P3)

As an operator, I can intentionally re-enter calibration mode for either motor after normal operation, so that replacement motors, encoders, or mechanical changes can be calibrated without falsely trusting old values.

**Why this priority**: Hardware service must not require erasing all settings or bypassing safety controls.

**Independent Test**: Start from a fully calibrated normal state, deliberately select recalibration for one motor, and verify the system returns to a safe calibration state and invalidates only the affected motor's completed calibration until it is confirmed again.

**Acceptance Scenarios**:

1. **Given** normal operation is available, **When** the operator deliberately requests calibration for motor 1 or motor 2, **Then** the controller disarms both motors before entering calibration mode.
2. **Given** the operator starts recalibration for one motor, **When** its previous alignment or characterization result is replaced, **Then** that motor is no longer considered calibrated until the replacement sequence is completed and confirmed.

### Edge Cases

- Saved calibration data is missing, structurally incompatible, incomplete for either motor, or contains physically invalid values: the controller treats the configuration as uncalibrated and enters calibration mode disarmed.
- Power is lost, reset occurs, or the operator cancels at any point: partial measurements are not treated as confirmed calibration and the controller restarts disarmed.
- The operator selects a motor that is not electrically safe to test due to a detected sensor/current-sense/driver fault: the test is blocked, both motors remain disarmed, and the fault is reported.
- A CAN emergency stop arrives during calibration: the active procedure stops and both motors remain disarmed until the operator restarts a permitted calibration action.
- One motor is fully calibrated but the other is not: normal dual-motor operation remains unavailable; the operator can continue with the remaining motor.

## Requirements *(mandatory)*

### Functional Requirements

- **FR-001**: The system MUST load the saved robot configuration before deciding whether to enter normal operation or calibration mode.
- **FR-002**: The system MUST enter calibration mode by default whenever the loaded configuration is not fully calibrated, invalid, missing, or incomplete for either installed drive motor.
- **FR-003**: The system MUST begin and remain with both motors disarmed when entering, exiting, cancelling, or failing calibration mode.
- **FR-004**: The system MUST prevent normal serial and CAN motion control while calibration mode is active, while continuing to honor an emergency stop.
- **FR-005**: The system MUST let an operator deliberately enter calibration mode and select motor 1 or motor 2 independently.
- **FR-006**: The system MUST offer an electrical-alignment calibration option that determines, presents, and requires confirmation of the selected motor's pole-pair count, sensor direction, and electrical offset.
- **FR-007**: The system MUST save confirmed electrical-alignment results separately for each motor and mark the result unconfirmed if the procedure fails, is cancelled, or is superseded by recalibration.
- **FR-008**: The system MUST offer motor-characteristics calibration only when the selected motor has a confirmed, saved pole-pair result.
- **FR-009**: The system MUST measure, present, and require confirmation of the selected motor's phase resistance and phase inductance before accepting motor-characteristic calibration.
- **FR-010**: The system MUST persist confirmed resistance and inductance for each motor in `RobotConfig` together with the motor's alignment values and completion state.
- **FR-011**: After confirmed resistance and inductance are available, the system MUST calculate and apply current-loop PID settings using those motor characteristics and a safe current-loop bandwidth selection; it MUST present the resulting settings to the operator.
- **FR-012**: The system MUST reject a calculated tuning result that is outside the controller's safe operating limits, retain the previous confirmed values, and report the reason.
- **FR-012a**: Calibration excitation MUST not exceed 4.0 V; normal calibration current MUST remain at or below 1.0 A, and a measured current of 1.5 A or more MUST immediately abort the session, zero targets, and disarm both motors.
- **FR-012b**: Electrical alignment MUST time out after 30 seconds and characteristics calibration after 15 seconds; after any failure, the controller MUST disarm both motors and permit an immediate operator-initiated retry.
- **FR-012c**: The system MUST not start a calibration excitation unless the selected motor's current-sensing safety guard is operational; if the chosen characterization routine cannot enforce the 1.5 A abort threshold, the system MUST use a guarded alternative or reject the operation.
- **FR-013**: The system MUST set the overall `calibrated` state to true only when both installed motors have confirmed, valid alignment and characteristic-calibration results; otherwise it MUST remain false.
- **FR-014**: The system MUST retain prior confirmed calibration data if a later calibration attempt fails or is cancelled, unless the operator explicitly confirms replacement data.
- **FR-015**: The system MUST report the active mode, selected motor, prerequisite status, calibration progress, result values, saved/unsaved state, and actionable error messages through the operator control interface.

### Key Entities

- **Robot configuration**: The persisted controller state, including global calibrated status and per-motor calibration records.
- **Motor calibration record**: The independently validatable values for one motor: pole-pair count, sensor direction, electrical offset, phase resistance, phase inductance, derived current-loop PID settings, and completion/confirmation state.
- **Calibration session**: A temporary, disarmed operator-guided procedure for one selected motor that has a type, progress state, result, and failure/cancellation outcome.

## Success Criteria *(mandatory)*

### Measurable Outcomes

- **SC-001**: In 100 consecutive startups with incomplete or invalid calibration data, both motors are disarmed before calibration controls become available and normal motion control is unavailable.
- **SC-002**: An operator can select either motor and begin electrical-alignment calibration in no more than three deliberate control actions after entering calibration mode.
- **SC-003**: For a successful calibration session, 100% of confirmed values survive a power cycle and are restored to the same motor without cross-motor substitution.
- **SC-004**: In bench tests using motors with known parameters, the recorded pole-pair count, direction, and offset produce a successful alignment result for both motors, and the resulting current-loop tuning operates without sustained current-loop oscillation at the selected safe bandwidth.
- **SC-005**: In 100 cancelled, failed, over-current, or emergency-stopped calibration sessions, no session enables normal motion control or marks incomplete calibration as complete; all over-current tests disarm both motors before a retry is permitted.

## Assumptions

- The controller continues to have working rotor position sensing, driver control, and phase-current sensing for each motor; failed hardware is surfaced as a calibration failure rather than worked around.
- The vehicle is secured so a controlled calibration test cannot cause injury or vehicle movement; calibration is an operator-attended bench procedure.
- The selected current-loop bandwidth starts conservatively relative to the observed control-loop rate and may be adjusted only within safe limits.
- Calibration uses a maximum 4.0 V excitation, a 1.0 A working-current limit, and a 1.5 A measured-current abort threshold, derived conservatively from the 2 A motor continuous-current rating.
- Calibration data is motor-specific because the two drive motors and their sensor installations can differ.
- This feature does not change the normal CAN velocity message layout; calibration commands and normal motion control are mutually exclusive by controller mode.
