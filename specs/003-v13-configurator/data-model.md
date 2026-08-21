# Data Model: V13 Configurator

**Feature**: `003-v13-configurator` | **Date**: 2026-08-21

Three owners. The **controller** holds the authoritative device state. The **Rust core** holds the live session
and its typed mirror of that state. The **frontend** holds view state and never holds anything the controller did
not report. Anything persisted lives on the technician's machine, except the firmware additions noted below.

## Controller: firmware additions

This feature adds no new stored configuration field except one, and adds no new measurement.

| Addition | Type | Why |
|---|---|---|
| Structured record emitters | behaviour | Makes the console machine readable (research D1) |
| Serial abort byte check | behaviour | Mandatory: closes the in-stage abort gap (research D2) |
| Calibration progress emission | behaviour | Blocked stages must show they are alive (research D3) |
| Request tag capture | 1 byte transient | Matches an acknowledgement to its request (research D6) |
| Bus identity write command | none new | `can_id` already exists in `RobotConfig`; only a command is missing (research D9) |

No `CONFIG_VERSION` bump is required: every value the configurator displays is already stored.

## Rust core: session state

Not persisted. One instance; the app addresses one controller at a time (FR-034).

| Field | Type | Notes |
|---|---|---|
| `port_name` | string | The chosen serial port |
| `state` | enum | `disconnected`, `identifying`, `ready`, `busy`, `lost` |
| `identity` | `DeviceIdentity` | From the `id` record; absent until identified |
| `in_flight` | optional request | At most one; carries tag, command, deadline |
| `next_tag` | u8 | Wraps; `0` reserved for untagged human input |
| `last_telemetry_at` | instant | Drives staleness |
| `telemetry_period_ms` | u16 | Applied value after any firmware clamp |
| `intent_token` | optional token | Single use, expiring, required for a powered procedure |

**State transitions**:

```
disconnected ──connect──► identifying ──id record──► ready ⇄ busy
     ▲                         │                       │
     │  disconnect / port_lost │  timeout              │  port_lost
     └─────────────────────────┴───────────────────────┘
```

Rules:
- Leaving `ready` for any reason clears `in_flight` and invalidates `intent_token`.
- `abort` is the only request permitted while `busy`, because it must be able to interrupt a blocked stage.
- Entering `lost` marks all mirrored data stale; it is never silently discarded, so the operator can still read
  the last known values while clearly seeing they are not live.

## Rust core: device identity

| Field | Type | Validation |
|---|---|---|
| `firmware_level` | string | Must be recognised |
| `protocol_version` | u16 | Must equal a supported version, else refuse (FR-004) |
| `can_id` | u16 | `0x001`-`0x7FF` |
| `motor_count` | u8 | Expected 2 |
| `config_version` | u16 | Recognised set only |

An unrecognised `protocol_version` or `config_version` puts the session in a refusing state rather than a
degraded-guessing one.

## Rust core: mirrored controller state

Populated only from parsed records, never from a requested value (FR-007, FR-027).

### Per-motor calibration record

| Field | Type | Range | Notes |
|---|---|---|---|
| `motor_index` | u8 | 1-2 | |
| `wheel_label` | string | — | Derived from `can_id` and index; front/rear and left/right |
| `aligned`, `characterised` | bool | — | Per-stage completion |
| `pole_pairs` | u16 | 1-64 | Read-only (FR-022) |
| `direction` | i8 | ±1 | Read-only |
| `electrical_offset` | f32 | 0-2π rad | Read-only |
| `phase_resistance` | f32 | 0.01-100 ohm | Read-only |
| `inductance_d`, `inductance_q` | f32 | 1e-6 to 0.1 H | Both always present |
| `valid` | bool | — | The controller's own verdict |
| `out_of_range` | bool | — | Set by the configurator when a reported value falls outside the expected range, or when motor 2 disagrees with motor 1 on pole pairs / R / L; the value is still displayed as reported |

`out_of_range` exists so a suspicious device value is visible rather than clamped into something plausible. Both
motors on a node are the same spec: motor 1 (right wheel) is the pole-pair reference, and R/L should stay within
25% of that reference. Direction and electrical offset stay mount-specific.

### Controller configuration

| Field | Type | Writable | Guard |
|---|---|---|---|
| `can_id` | u16 | yes | Disarmed, `0x001`-`0x7FF` |
| `bandwidth_requested_hz` | u16 | yes | Disarmed, 100-10000, calibration required |
| `bandwidth_active_hz` | u16 | no | Derived by the controller |
| `bandwidth_clamped` | bool | no | Drives the "reduced from requested" display |
| `control_rate_hz`, `carrier_hz`, `decimation` | numeric | no | Derived |
| `bus_min_mv`, `bus_max_mv` | u16 | yes | Disarmed, min < max, board range |
| `motion_mode[2]` | enum | yes | Disarmed, calibration eligibility |
| `calibrated` | bool | no | Unit-level rollup |

The writable column is the whole of FR-017's scope. Everything else is display only.

### Per-motor runtime state

| Field | Type | Notes |
|---|---|---|
| `armed` | bool | Shown wherever a powered action is offered (FR-029) |
| `mode` | enum | velocity or impedance |
| `position_mrad` | i32 | Accumulated shaft angle |
| `velocity` | f32 | rad/s |
| `current_q` | f32 | A |
| `timed_out` | bool | Command timeout active |
| `limit_causes` | set | Decoded from the reported bitmask. Output-voltage is evaluated on current firmware; `nan` `pairfault` still marks it unavailable |
| `limit_count` | u16 | Saturating |
| `pair_fault` | tri-state | Latched pair-match fault. `nan` only on older flashes |

### Per-motor impedance state

| Field | Type | Notes |
|---|---|---|
| `p_des_mrad`, `v_des`, `kp`, `kd`, `t_ff` | numeric | Last applied five-term |
| `position_error`, `torque_cmd` | f32 | Last control-cycle computation |
| `applied_target_mrad`, `capture_generation`, `last_seq` | numeric | Handshake / apply acknowledgement |
| `eligible` | bool | Firmware eligibility guard |

Writable from the debug view via `K` / `apply_impedance`. Not persisted.

### Timing state

`rate_nominal`, `rate_measured`, `period_us`, `cycles`, `overruns`, `consecutive`, `last_us`, `worst_us`,
`duty_percent`, `fault`. Backs the debug view's timing panel (FR-026).

### Freshness

Every mirrored group carries `updated_at` and a derived `stale` flag. Staleness is a displayed state, not an
absence of data (FR-024).

## Rust core: calibration session

| Field | Type | Notes |
|---|---|---|
| `motor_index` | u8 | The single motor in scope; the other stays de-energised (FR-034b) |
| `stage` | enum | `align` or `characterise` |
| `phase` | enum | `confirming`, `running`, `pending`, `resolved`, `failed` |
| `percent` | u8 | From progress records only; never inferred from elapsed time |
| `energised` | bool | Reported by the controller |
| `pending_values` | optional record | Present only in `pending` |
| `failure_reason` | optional string | The controller's own wording |

**Transitions**:

```
confirming ──start ack──► running ──stage ack──► pending ──accept ack──► resolved
     │                       │                      │
     │                       │ abort / fail         │ reject
     └──────────────────────►failed ◄────┘
```

Rules:
- `pending_values` are never treated as stored until an accept is acknowledged with success (FR-012, FR-013).
- Changing the selected motor, disconnecting, or aborting while in `pending` discards `pending_values` after a
  warning.
- Every terminal phase leaves both motors de-energised (FR-031).
- A timeout in `running` produces an unknown outcome, not a failure: the configurator re-reads rather than
  assuming (FR-021 pattern applied to calibration).

## Local persistence, on the technician's machine

### App settings

| Field | Notes |
|---|---|
| `last_port` | Convenience only; never auto-connects without an explicit action |
| `telemetry_period_ms` | Preferred period, subject to firmware clamp |
| `operator_profile` | Display name and optional email, for attribution only (research D11) |
| `theme` | Presentation |

No credentials are stored, because no authentication exists.

### Change log

Append only. One entry per accepted calibration or acknowledged setting write (FR-036).

| Field | Notes |
|---|---|
| `timestamp` | Local clock; the controller has none |
| `operator` | From the profile, or explicitly anonymous |
| `can_id`, `motor_index`, `wheel_label` | What was affected |
| `kind` | `calibration_accepted` or `setting_written` |
| `before`, `after` | Values, as reported before and after |
| `firmware_level`, `protocol_version` | Provenance of the session |

`before` and `after` both come from device reads, so a log entry records what the device confirmed rather than
what the operator requested.

## Frontend view state

| Field | Notes |
|---|---|
| `selected_motor` | 1 or 2 |
| `pending_edits` | Only ever the four writable settings; marked distinctly from device values |
| `confirmation_dialog` | Names the wheel and the physical precondition |
| `route` | Config or debug, mirroring the reference design's sidebar |

The frontend holds no parsed serial text and no measured value it computed itself.

## Entity relationships

```
Serial link (one port, one controller)
  └── DeviceIdentity ──gates──► every offered action
        ├── CalibrationRecord[2]  (read-only, changed only by an accepted calibration)
        ├── ControllerConfiguration (four writable settings)
        ├── MotorRuntimeState[2] ──┐
        └── TimingState ───────────┴──► debug view, each with its own freshness

CalibrationSession (one at a time, one motor) ──on accept──► CalibrationRecord
                                              ──always────► ChangeLogEntry
ControllerConfiguration ──on write ack───────────────────► ChangeLogEntry
```
