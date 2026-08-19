# Research: Guided Motor Calibration

## Decision: Use a two-stage per-motor workflow

**Decision**: Run electrical alignment first (pole pairs, direction, electrical offset); enable resistance/inductance characterization only after its confirmed result is persisted.

**Rationale**: SimpleFOC's FOC workflow identifies sensor direction, validates pole pairs, and records the zero electrical angle as distinct alignment steps. The existing `Motor-find_pole_pair` and `Encoder-find_offset_and_direction` references supplied with the feature request follow the same separation. Characterisation requires initialized motor, driver, and current sensing, and the motor must remain still.

**Alternatives considered**:

- Run all measurements in one irreversible command: rejected because it obscures failed prerequisites and risks saving ambiguous results.
- Use only `initFOC()` auto-alignment: rejected because it does not provide the requested operator review and independent pole-pair discovery workflow.

## Decision: Store independent calibration records for both motors

**Decision**: Persist an independent record for motor 1 and motor 2, with individual validity/completion bits; the global calibrated state is derived only when both records validate.

**Rationale**: The motors use independent drivers, sensors, and current sensing. A single pole-pair value cannot faithfully represent two potentially different motors, and a global boolean cannot distinguish a replaced or unfinished motor.

**Alternatives considered**:

- One shared record: rejected because it permits cross-motor application of offset/direction/characteristics.
- Treat `calibrated` alone as authoritative: rejected because it cannot detect incomplete or corrupt motor fields.

## Decision: Version and validate persisted configuration

**Decision**: Store a schema version, record validity markers, and bounded numeric values. Unknown/old/corrupt data is reset to an uncalibrated default rather than reinterpreted as calibrated.

**Rationale**: The current raw-structure byte storage rejects a changed struct size. Calibration adds multiple floating-point parameters and must fail closed after a firmware update or interrupted write.

**Alternatives considered**:

- Continue size-only validation: rejected because compatible-size corruption and stale values may look valid.
- Automatically map every prior configuration: rejected because prior data contains no trustworthy complete per-motor calibration record; preserving CAN ID while resetting calibration is safer.

## Decision: Use SimpleFOC characterization and tuning with bounded inputs

**Decision**: Use the library's motor characterization to obtain phase resistance and D/Q inductance only if its runtime path can enforce live current protection; otherwise use a guarded alternative or reject characterization. Cap excitation at 4.0 V, operate at no more than 1.0 A, and abort at a measured 1.5 A. Start with a conservative bandwidth derived from the measured FOC-loop rate; require it to remain within the library's stability limit.

**Rationale**: SimpleFOC documents that `characteriseMotor()` measures resistance and D/Q inductance with linked current sensing. A voltage cap alone cannot ensure safe current on an unknown low-resistance winding, so the 2 A continuous rating requires a verified measured-current guard. Its tuning guidance derives current PI gains from resistance, inductance, and bandwidth, and recommends a bandwidth about 5–10% of the FOC loop rate.

**Alternatives considered**:

- Hard-code PID values: rejected because it does not meet the requested parameter-based tuning and cannot adapt to motor replacement.
- Let the operator supply arbitrary PID values: deferred; it is out of scope for the guided first version and provides no safe default.

## Decision: Serial-only calibration control; CAN motion lockout

**Decision**: Extend the existing serial Commander with calibration commands. During calibration, reject all normal CAN motor command frames and existing arm/motion serial commands; accept emergency stop at all times.

**Rationale**: Serial is already the local maintenance interface. The existing backend emits command `0x01` velocity frames continuously while a joystick is active, so accepting them during calibration would violate the safety model. No CAN payload change is needed.

**Alternatives considered**:

- Add new CAN calibration frames: rejected for v1 because it expands the backend protocol and operator safety surface without being required.
- Stop TWAI during calibration: rejected because it would prevent emergency-stop reception and status visibility.

## Decision: Confirmation before persistence, preserve known-good results

**Decision**: Treat calibration output as a pending session result. Save it only on explicit serial confirmation; on cancel/failure/estop, discard pending values and keep the prior confirmed record untouched.

**Rationale**: This meets the no-partial-write requirement and lets an operator retry unsafe or surprising results without destroying a known-good configuration.

**Alternatives considered**:

- Save immediately after measurement: rejected because a transient bad measurement becomes active without review.

## Sources

- [SimpleFOC FOC workflow](https://docs.simplefoc.com/foc_implementation): sensor direction, pole-pair check, zero electrical angle, and alignment ordering.
- [SimpleFOC motor characterisation](https://docs.simplefoc.com/motor_characterisation): requirements, resistance, and D/Q inductance measurement behavior.
- [SimpleFOC current-loop tuning](https://docs.simplefoc.com/tuning_current_loop): parameter-based PI/LPF tuning and bandwidth guidance.

## Implementation Verification

- Deployed build target: `esp32:esp32:esp32` (ESP32 Arduino core 3.3.7).
- Installed library: Simple FOC 2.4.0.
- Compatibility result: SimpleFOC 2.4.0 exposes `characteriseMotor()`, but its blocking implementation does not expose a live current-abort callback. The firmware therefore uses a guarded local resistance/inductance measurement sequence that samples current and CAN emergency-stop state during every ramp/pulse, aborting at 1.5 A.
