# Contract: Desktop App Internal Interface

**Feature**: `003-v13-configurator` | **Date**: 2026-08-21

The boundary between the Rust core and the Vue frontend inside the Tauri app. The Rust side owns the serial port
exclusively and is the only place that speaks the wire protocol; the frontend receives typed data and never
parses a line of serial text.

This split exists so the highest-consequence failure in the feature, misreading a motor's electrical parameters,
lives in one testable pure function rather than being spread across UI components (research D7).

## Commands, frontend to Rust

All are async and return either typed data or a structured error carrying the controller's own reason.

| Command | Input | Returns | Notes |
|---|---|---|---|
| `list_ports` | — | port descriptors | Enumerates candidates; does not open anything |
| `connect` | port name | `DeviceIdentity` | Opens with non-resetting control signals, waits for identification, refuses an unrecognised device |
| `disconnect` | — | — | Idempotent; stops telemetry first |
| `read_all` | — | `ControllerSnapshot` | One full read of calibration, config and motor state |
| `set_telemetry` | period ms, or off | applied period | Firmware may clamp; the applied value is returned |
| `select_motor` | motor index | — | Frontend-side selection only; sends nothing |
| `arm` / `disarm` | motor selector | `Ack` | Disarm is always permitted. Impedance arm is refused by the controller if that motor fails eligibility |
| `apply_impedance` | motor, five terms | `Ack` | Serial `K`. Range-checked in Rust before send. Produces torque if that motor is armed |
| `calibrate_start` | motor, stage | `Ack` | Requires a prior confirmed intent token, see below |
| `calibrate_accept` | motor | `Ack` then `CalibrationRecord` | Success reflects a controller acknowledgement |
| `calibrate_reject` | motor | `Ack` | |
| `abort` | — | `Ack` | Sends the raw abort byte; never queued behind another request |
| `write_setting` | setting, value | `Ack` | One of the four writable settings |
| `change_log_read` | filter | change records | Local log |

### Intent token for powered procedures

`calibrate_start` requires a token issued by a separate `confirm_intent` command that records the motor, the
wheel name shown to the operator, and the acknowledged safety precondition. The token is single use and expires.

This makes FR-010 structurally enforced rather than a UI convention: no code path can start a powered procedure
without a confirmation having happened, even if a future UI change forgets a dialog.

## Events, Rust to frontend

| Event | Payload | When |
|---|---|---|
| `connection_changed` | state, identity or reason | Connect, disconnect, or unexpected port loss |
| `telemetry` | `TelemetrySnapshot` | Each unsolicited telemetry record set |
| `calibration_progress` | motor, stage, percent, energised | Each progress record from inside a stage |
| `calibration_pending` | motor, stage, measured values | A stage completed and awaits a decision |
| `fault` | kind, reason, legacy cooldown ms (always zero) | Any fault record |
| `protocol_error` | detail | Unparseable or version-mismatched input |
| `staleness` | last update age | Telemetry has stopped arriving |

Events rather than polling, because the firmware volunteers progress during blocked stages and the frontend must
not be asking questions the controller cannot answer while a motor is energised.

## Rust-side responsibilities

- Sole owner of the serial port. Exactly one request in flight, tagged and matched to its acknowledgement.
- Parsing every `#V13` record into typed values, and rejecting an unrecognised schema version outright.
- Refusing to interpret any line lacking the `#V13` prefix, so human prose is inert.
- Timeout enforcement per the protocol contract, with distinct handling for the long calibration case.
- Setting non-resetting control signals on open, and reporting a reset if the board forces one.
- Marking data stale when telemetry stops, rather than leaving the last value looking current.
- Appending change records to the local log after a write or accepted calibration is acknowledged.
- Never synthesising a value the controller did not report, and never clamping an out-of-range reported value
  into something plausible.

## Frontend responsibilities

- Presenting the two motors of the connected controller, with the selected one and its wheel unmistakable.
- Rendering measured quantities as read-only display fields, never as inputs (FR-022a).
- Range-validating the four writable settings before submitting, and surfacing the controller's refusal reason
  verbatim in meaning when one comes back.
- Keeping an abort control reachable at all times while any powered procedure is possible.
- Offering no chassis velocity or joystick control. The debug view may apply a per-wheel impedance five-term
  via `apply_impedance` (`K`); that is bench impedance, not vehicle motion.
- Showing staleness, disconnection, and version mismatch as first-class states rather than as empty values.

## Error model

Every failure carries a machine-readable kind and a human reason:

| Kind | Meaning |
|---|---|
| `not_connected` | No live session |
| `refused` | The controller declined; `reason` is the controller's own text |
| `timeout` | No acknowledgement inside the contract's window |
| `protocol` | Unparseable input or an unrecognised schema version |
| `port_lost` | The port disappeared mid-session |
| `busy` | Another request is in flight |
| `needs_confirmation` | A powered procedure was attempted without a valid intent token |

`refused` and `timeout` are deliberately distinct: a refusal means the controller decided, while a timeout means
the outcome is unknown and the operator must re-read rather than assume either way (FR-021).
