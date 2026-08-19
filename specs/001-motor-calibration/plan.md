# Implementation Plan: Guided Motor Calibration

**Branch**: `001-motor-calibration` | **Date**: 2026-08-19 | **Spec**: [spec.md](spec.md)

## Summary

Add a firmware-owned, serial-operated calibration mode for the two motor controller. Startup will validate the persisted, per-motor calibration record before configuration of normal FOC operation. Invalid or incomplete records enter calibration mode with both drivers disarmed. The operator can select either motor to identify pole pairs, sensor direction, and electrical offset; only after confirming that result can the operator measure resistance and D/Q inductance, derive current-loop PI/LPF settings, and persist the record. Normal CAN velocity frames are ignored until both records are valid; emergency stop remains active.

## Technical Context

**Language/Version**: C++ in an Arduino sketch, built for ESP32; exact SimpleFOC version must be confirmed from the installed toolchain before coding.

**Primary Dependencies**: SimpleFOC (BLDC motor, magnetic SPI sensor, inline current sense, Commander); ESP32 Preferences; ESP32 TWAI/CAN driver; FreeRTOS.

**Storage**: ESP32 non-volatile Preferences namespace `robot_config`; replace the raw unversioned configuration payload with a versioned validation-aware record and a migration/reset path.

**Testing**: Arduino compile for the supported ESP32 target; focused host-testable validation helpers where feasible; serial/CAN message-shape checks; mandatory secured bench verification for alignment, characterization, cancellation, reset, and emergency stop.

**Target Platform**: ESP32 motor controller board using two 3-PWM drivers, AS5147 SPI magnetic encoders, and inline current sensing.

**Project Type**: Embedded firmware with a Python CAN publisher and Vue operator interface outside this feature's ownership boundary.

**Performance Goals**: Preserve the existing FOC task cadence and CAN status cadence; serial status must make each calibration state change observable without blocking normal emergency-stop handling.

**Constraints**: Both motors must be disarmed outside an intentionally bounded calibration excitation; excitation is capped at 4.0 V, working current at 1.0 A, and measured current abort at 1.5 A; alignment times out at 30 seconds, characterization at 15 seconds, and failed/over-current attempts impose a 30-second cooldown before retry; no normal velocity target may be accepted in calibration mode; CAN velocity frame layout and scaling remain unchanged; any partial, invalid, cancelled, or interrupted calibration must fail closed.

**Scale/Scope**: One firmware node controlling two independent motors. Initial operator interface is the existing serial Commander; no backend/UI change is required.

## Constitution Check

| Principle | Design response | Gate |
|-----------|-----------------|------|
| I. Safety-Critical Motion First | Calibration has explicit state transitions, zero-target/disarm entry and exit, CAN lockout, and an emergency-stop transition. Bench-safe validation is mandatory. | Pass, pending hardware verification |
| II. Protocol Compatibility | Existing command ID, eight-byte velocity format, target scaling, and estop meaning are untouched. Firmware discards velocity frames only while calibration mode is active. | Pass |
| III. Validate at the Boundary You Changed | Firmware compile, configuration validation tests, serial command tests, CAN lockout payload checks, and a bench checklist cover changed boundaries. | Pass |
| IV. Configuration Must Not Be Hard-Coded | Per-motor measurements, offset, direction, controller tuning, and mode readiness are persisted configuration, not constants. Calibration voltage/bandwidth are named safety parameters with documented defaults. | Pass |
| V. Preserve Clear Ownership Boundaries | Only root firmware and its calibration documentation change. Backend and UI behavior are preserved. | Pass |

**Post-design check**: Pass. No constitution violation or complexity exception is introduced.

## Project Structure

### Documentation (this feature)

```text
specs/001-motor-calibration/
├── spec.md
├── plan.md
├── research.md
├── data-model.md
├── quickstart.md
└── contracts/
    └── serial-calibration.md
```

### Source Code

```text
v13_macnum_wheel_car.ino             # Firmware, configuration, FOC, CAN, serial Commander
jetson_xavier/backend/socketio_server.py  # Existing velocity CAN publisher; unchanged
```

**Structure Decision**: Implement calibration in the existing firmware sketch to retain ownership of the motor hardware and the existing Commander control surface. Do not add a backend or UI calibration path in this feature.

## Implementation Outline

1. Add versioned per-motor configuration records, validation, and migration/reset handling.
2. Separate static hardware setup from normal FOC activation so calibration can initialize only the selected motor while both normal motors remain disarmed.
3. Add an explicit mode/session state machine, serial contract, and centralized safe transitions.
4. Add electrical-alignment and gated characterisation workflows, including verified live-current abort enforcement, result review, and confirmation before persistence.
5. Apply persisted parameters during normal initialization; derive and validate current-loop tuning after characterization.
6. Gate CAN motion and existing serial arm/motion commands by mode; preserve emergency stop.
7. Validate through compile, focused message/configuration checks, and secured bench scenarios.

## Complexity Tracking

No constitution violations require justification.
