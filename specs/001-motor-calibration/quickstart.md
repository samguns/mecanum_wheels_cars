# Quickstart: Validate Guided Motor Calibration

## Safety prerequisites

1. Secure the vehicle so wheels cannot propel it; remove drivetrain load if practical.
2. Keep an accessible physical power disconnect and CAN emergency-stop sender ready.
3. Verify correct phase wiring, sensor connection, and current-sense wiring for both motors.
4. Connect serial at 115200 baud and record the configuration state reported at boot.
5. Verify live current sensing before calibration. Calibration is capped at 4.0 V and 1.0 A working current; a measured 1.5 A must abort and disarm both motors.

## Build validation

1. Build the firmware for the deployed ESP32 board: `arduino-cli compile --fqbn esp32:esp32:esp32 .`. Verified on 2026-08-19 with ESP32 core 3.3.7 and Simple FOC 2.4.0: success, 407008 bytes (31%) program storage and 25804 bytes (7%) dynamic memory.
2. Start with erased or deliberately incompatible calibration storage.
3. Reset the controller. Expected: `CALIBRATION` state, both motors disarmed, and the command summary is printed.
4. Send a normal CAN velocity frame. Expected: no target update and no enable transition.
5. Send emergency stop. Expected: both motors are disarmed and the active calibration session is cancelled.

## Motor 1 workflow

1. Send `C1`, then `CA`.
2. Observe only motor 1's controlled alignment procedure; motor 2 remains disarmed.
3. Review the `PENDING` pole pairs, direction, and offset. Send `CY` to confirm or `CN` to reject.
4. Send `CM`. Verify it is accepted only after the alignment confirmation.
5. Keep the motor shaft still during measurement. Review the pending resistance, inductances, and calculated tuning.
6. Send `CY` only if results are plausible; otherwise send `CN` and retry after correcting wiring or test conditions.
7. Verify that alignment stops within 30 seconds and characterization within 15 seconds. After an over-current or characterization failure, verify a 30-second serial-reported cooldown before retry.

## Motor 2 workflow and normal-mode admission

1. Repeat the Motor 1 workflow with `C2`.
2. Send `C` and verify both motor records report complete and valid.
3. Send `CE`. Expected: normal mode is admitted but both motors remain disarmed.
4. Reset the controller. Expected: the valid per-motor data reloads, normal mode starts disarmed, and no calibration prompt is required.

## Negative and recovery checks

- Request `CM` before `CA`/`CY`: expected prerequisite rejection.
- Cancel each operation with `CX`: expected disarm and no pending data saved.
- Reset or send emergency stop during each operation: expected no partial record becomes confirmed.
- Simulate or safely inject an over-current indication at 1.5 A: expected immediate zero-target/disarm, no persistence, 30-second cooldown, and a serial-only error report.
- Force one record invalid: expected the next boot returns to calibration mode and rejects normal motion.
- Verify that an existing normal CAN velocity frame works only after both records are valid and normal mode is active.

See [data-model.md](data-model.md) for record validity and [serial-calibration.md](contracts/serial-calibration.md) for the command contract.
