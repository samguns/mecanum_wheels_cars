# Serial Calibration Contract

## Transport

- 115200 baud serial connection, one newline-terminated command per request.
- Commands are accepted only after startup diagnostics complete.
- Responses are human-readable, prefixed with calibration state, and contain no binary framing.

## Commands

| Command | Allowed mode | Behavior |
|---------|--------------|----------|
| `C` | normal or calibration | Print current mode, each motor's prerequisite/completion state, and the permitted next commands. |
| `C1` / `C2` | normal or calibration | Enter calibration mode for the selected motor. Disarm both motors first. |
| `CA` | calibration, selected motor | Start electrical alignment for the selected motor. |
| `CM` | calibration, selected motor with confirmed alignment | Start characteristics measurement for the selected motor. |
| `CY` | calibration, pending result | Confirm and persist the pending result. |
| `CN` | calibration, pending result | Reject the pending result and retain the last confirmed snapshot. |
| `CX` | calibration | Cancel the active/pending session and disarm both motors. |
| `CE` | calibration | Leave calibration mode only when both records validate; otherwise print unmet prerequisites. |

## Command Rejection Rules

- `CA` is rejected unless a motor is selected and no calibration action is running.
- `CM` is rejected unless the selected motor has a confirmed, saved alignment result.
- `CY` and `CN` are rejected unless there is a valid pending result.
- Existing normal-operation arm (`A*`), disarm (`D*`), and motor-control (`a ...`, `b ...`) commands are rejected in calibration mode, except that disarm remains idempotently safe.
- Any syntax error, invalid state, or unavailable hardware produces an error response with the next permitted action.
- Calibration requests are rejected only while the 30-second cooldown is active after an over-current or characterization failure.

## CAN Boundary

- The existing standard-ID, eight-byte `0x01` velocity command format is unchanged.
- In normal mode, existing arm/disarm and velocity semantics remain unchanged.
- In calibration mode, normal CAN motor command frames are silently discarded and never change motor targets or enable state. Calibration feedback is serial-only.
- CAN emergency stop continues to disarm both motors immediately and cancels any active calibration action.

## Result Reporting

Successful alignment reports motor number, pole pairs, sensor direction, and electrical offset. Successful characterization reports motor number, resistance, D/Q inductance, requested/effective bandwidth, current-loop PI gains, and low-pass filter time constants. Results are labeled `PENDING` until `CY` confirms persistence. Every calibration response reports voltage/current limits, timeout/cooldown state, and any abort reason.
