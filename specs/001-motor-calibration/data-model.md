# Data Model: Guided Motor Calibration

## RobotConfig

The persisted configuration is an atomic snapshot. It includes deployment settings that must survive migration, a schema version, two motor records, and a derived overall readiness state.

| Field | Type/units | Rules |
|-------|------------|-------|
| `schema_version` | unsigned integer | Must equal the current supported version. Other values are migrated when safe or reset to uncalibrated defaults. |
| `can_id` | 11-bit standard CAN identifier | Preserve existing valid value during migration. |
| `motor[2]` | `MotorCalibrationRecord` | One record per physical motor; never copy values between indexes. |
| `calibrated` | boolean | True only when both records are complete and valid; recomputed before save and after load. |

## MotorCalibrationRecord

| Field | Type/units | Validation |
|-------|------------|------------|
| `alignment_confirmed` | boolean | Required before characterisation or normal FOC use. |
| `characteristics_confirmed` | boolean | Required together with alignment for normal FOC use. |
| `pole_pairs` | positive integer | Must be in a supported motor range and non-zero. |
| `sensor_direction` | enumerated CW/CCW | Must be one of the two supported values. |
| `electrical_offset` | radians | Must be finite and normalized to one electrical revolution. |
| `phase_resistance` | ohms | Must be finite, positive, and within the configured safety range. |
| `inductance_d` | henries | Must be finite, positive, and within the configured safety range. |
| `inductance_q` | henries | Must be finite, positive, and within the configured safety range. |
| `current_bandwidth` | hertz | Must be positive and safe relative to observed FOC-loop rate. |
| `current_pid_d/q` | controller gains | Must be finite, non-negative, and derived from the confirmed electrical record. |
| `current_lpf_d/q` | seconds | Must be finite, positive, and derived from the confirmed bandwidth. |

## CalibrationSession

This is RAM-only and is never restored after reset.

| Field | Values | Rules |
|-------|--------|-------|
| `mode` | normal, calibration | Normal requires valid complete configuration. |
| `selected_motor` | motor 1, motor 2, none | Exactly one motor may be tested at a time. |
| `operation` | idle, alignment, characterization, pending confirmation, failed, cancelled | Calibration transitions always disarm on exit or failure. |
| `pending_record` | candidate motor record | May be displayed and confirmed, but is never applied to persistent configuration until confirmation. |
| `failure_reason` | diagnostic enum/text | Set on prerequisite, hardware, validation, cancellation, or estop failure. |
| `cooldown_until` | monotonic time | Set to 30 seconds after over-current or characterization failure. |

## Calibration Safety Envelope

| Limit | Value | Enforcement |
|-------|-------|-------------|
| Excitation voltage | ≤ 4.0 V | Validate before every alignment or characterization excitation. |
| Working current | ≤ 1.0 A | Configure the calibration path not to intentionally exceed this current. |
| Current abort | ≥ 1.5 A | Immediately zero targets, disable both drivers, discard pending data, and start cooldown. |
| Alignment duration | ≤ 30 seconds | Timeout fails closed. |
| Characterization duration | ≤ 15 seconds | Timeout fails closed. |
| Failed-test cooldown | 30 seconds | Applies after over-current or characterization failure. |

## State Transitions

```text
load config
  ├─ both records valid ──> NORMAL / both disarmed
  └─ otherwise ──────────> CALIBRATION-IDLE / both disarmed

NORMAL -- deliberate calibrate(motor) --> CALIBRATION-IDLE / selected record invalidated in RAM
CALIBRATION-IDLE -- align(motor) -----> ALIGNMENT-RUNNING
ALIGNMENT-RUNNING -- valid result ----> ALIGNMENT-PENDING -- confirm --> CALIBRATION-IDLE
ALIGNMENT-RUNNING -- fail/cancel/estop -> CALIBRATION-IDLE / disarmed
CALIBRATION-IDLE -- characterize(motor with confirmed alignment) --> CHARACTERIZATION-RUNNING
CHARACTERIZATION-RUNNING -- valid result --> CHARACTERISTICS-PENDING -- confirm --> CALIBRATION-IDLE
CHARACTERIZATION-RUNNING -- over-current/fail --> COOLDOWN / disarmed
CHARACTERIZATION-RUNNING -- cancel/estop --> CALIBRATION-IDLE / disarmed
COOLDOWN -- 30 seconds elapsed --> CALIBRATION-IDLE
CALIBRATION-IDLE -- both records valid --> NORMAL / both disarmed
```

## Persistence Rules

1. Validate the complete candidate record before presenting it for confirmation.
2. On confirmation, update only the selected motor record, recompute global `calibrated`, and write one complete configuration snapshot.
3. On cancellation, failure, reset, or estop, discard only the pending record; preserve the last confirmed snapshot.
4. On configuration migration failure, retain any valid `can_id` if possible and save an explicitly uncalibrated current-schema snapshot.
