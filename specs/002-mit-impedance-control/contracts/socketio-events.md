# Contract: Socket.IO Events

**Feature**: `002-mit-impedance-control` | **Date**: 2026-08-20

Between the Vue operator UI and the FastAPI + Socket.IO backend on port 8080.

## Existing events, unchanged

| Event | Direction | Payload |
|---|---|---|
| `joystick_command` | UI → backend | `{x: float, y: float, rotation: float}`, each in [−1, 1] |
| `estop` | UI → backend | `bool` or `{engaged: bool}` |
| `estop_state` | backend → UI | `{engaged: bool}` |
| `can_message` | backend → UI | `{id: str, dlc: int, data: int[], timestamp: float, extended: bool}` |
| `bus_voltage` | backend → UI | `{canId: str, busVoltage: float, sourceStatusId: str, timestamp: float}` |

**Behaviour change on `joystick_command`**: the handler now only updates the backend's latest target state. The
actual CAN transmission moves to a fixed 200 Hz loop (FR-038), so frame timing no longer follows pointer-move
event frequency.

## New events

### `set_motion_mode` (UI → backend)

```json
{ "mode": "velocity" | "impedance" }
```

Vehicle-wide. The backend sends a mode frame (`0x120 + NodeID`) to both nodes for both motors. Rejected with
`command_rejected` if any controller reports an armed motor, mirroring the firmware's disarm-first rule.

### `set_gains` (UI → backend)

```json
{ "kp": float, "kd": float }
```

Applied to all four wheels. The backend validates against the contract ranges (`kp` 0-50, `kd` 0-1.0) before
use and rejects out-of-range values with `command_rejected` rather than clamping silently, so the operator sees
the refusal (FR-039).

### `controller_status` (backend → UI)

Emitted from the coalesced `0x1A0` through `0x1F0` status set at most 10 Hz per node.

```json
{
  "canId": "0x201",
  "motors": [
    { "index": 1, "mode": "impedance", "armed": true, "timedOut": false, "positionMrad": 628318, "appliedTargetMrad": 628300, "captureGeneration": 4, "kp": 12.0, "kd": 0.3, "limitCauses": [], "limitCount": 0, "pairFault": false },
    { "index": 2, "mode": "impedance", "armed": true, "timedOut": false, "positionMrad": -314159, "appliedTargetMrad": -314200, "captureGeneration": 7, "kp": 12.0, "kd": 0.3, "limitCauses": ["current"], "limitCount": 1, "pairFault": false }
  ],
  "requestedBandwidthHz": 2500,
  "activeBandwidthHz": 1000,
  "bandwidthClamped": true,
  "carrierHz": 30000,
  "faults": {
    "overrun": false,
    "calibrationRequired": false,
    "timingSource": false
  },
  "timestamp": 1755678901.234
}
```

This is the only source the UI uses to display mode, gains, bandwidth, measured accumulated position, and effort
limit causes (FR-001c, FR-004a, FR-040). Bandwidth is display-only; no event exists to set it, by design.

### `command_rejected` (backend → UI)

```json
{ "command": "set_motion_mode" | "set_gains", "reason": "string" }
```

Carries the human-readable reason so the UI can show why a request did not take effect.

## Backend transmit loop

| Property | Value |
|---|---|
| Rate | 200 Hz fixed (FR-038) |
| Payload | The latest mixed per-wheel targets, resent every tick |
| In velocity mode | Legacy `0x200 + NodeID` frames, two per update |
| In impedance mode | Sequence-matched `0x100/0x110` position and `0x130/0x140` dynamics halves, eight frames per update |
| While e-stop engaged | Send `0x080` on engagement, then suspend all motion frames until cleared |
| On no joystick input | Zero targets, still transmitting |

Normal idle/stop transmits zeros rather than relying on the timeout. Emergency stop is distinct: `0x080` is sent
immediately and motion transmission remains suspended while engaged. Only a backend/link failure goes silent and
lets the controller's 50 ms timeout act as a fault detector.
Every 200 Hz tick allocates one sequence value per motor and sends its position half immediately followed by its
dynamics half. A new tick never reuses an incomplete pair's sequence.

On a vehicle-wide `kp` transition from zero to non-zero, all four wheels enter `capturePending`. Their dynamics
halves set capture-current-position on every tick. A wheel leaves pending only after `0x1E0`/`0x1F0` reports a
capture-generation change after the request; the echoed applied target then becomes its held `p_des`.

## Gain mapping in impedance mode

The mecanum mixer is unchanged. Its per-wheel output becomes `v_des`. For ordinary driving the operator's
stiffness is expected to be **0**, making each wheel a damped velocity follower whose behaviour matches the
retained velocity mode (SC-008). A non-zero stiffness with a held position target is the compliant/parked case.

| Impedance field | Source in driving |
|---|---|
| `p_des` | Ignored by capture when stiffness is first enabled; controller-confirmed applied target afterward |
| `v_des` | Mixer output for that wheel |
| `kp` | Operator setting, normally 0 for driving |
| `kd` | Operator setting |
| `t_ff` | 0 |

## REST endpoints, unchanged

`GET /` health, `GET /config` and `POST /config` for the `{L, W}` geometry.
