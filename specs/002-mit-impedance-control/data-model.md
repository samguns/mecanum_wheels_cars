# Data Model: MIT Impedance Control Mode with Deterministic Current Loop

**Feature**: `002-mit-impedance-control` | **Date**: 2026-08-20

Entities are grouped by owning layer. Firmware entities are the authoritative ones; the backend and UI hold
mirrors of controller-reported state.

## Firmware: persisted configuration

Extends the existing `RobotConfig` in `v13_macnum_wheel_car.ino`. Adding fields requires a
`CONFIG_VERSION` bump and a migration path, because `loadConfig()` rejects any stored blob whose size or
version does not match and falls back to defaults.

### `RobotConfig` additions

| Field | Type | Range / values | Default | Notes |
|---|---|---|---|---|
| `current_bandwidth_hz` | `uint16_t` | 100 - 10000 | 1000 | The **requested** value, per FR-020d. Node-wide, not per motor (FR-028). |
| `motion_mode[2]` | `uint8_t` | 0 = velocity, 1 = impedance | 0 | Last accepted per-motor mode; fresh/invalid config defaults to velocity (FR-006a). |
| `bus_voltage_min_mv` | `uint16_t` | board-approved range | 7000 provisional | Fail-closed undervoltage threshold; serial-configurable only while disarmed |
| `bus_voltage_max_mv` | `uint16_t` | board-approved range | 24000 provisional | Fail-closed overvoltage threshold; serial-configurable only while disarmed |

**Validation rules**:
- `current_bandwidth_hz` outside 100-10000 → reject the write, keep the stored value, report the range (FR-024).
- A stored value outside the range on load → treat as unset and use 1000 Hz (FR-022).
- `motion_mode` value not in {0, 1} → treat as 0.
- Bus thresholds invalid, reversed, or outside the documented board range → reject the write and keep the
  previous pair. Provisional defaults are reconciled with hardware limits before release.
- An accepted disarmed mode change is written before success is reported; startup restores the mode but always
  clears arm state, targets, gains, pending pairs, and effort.

**Migration**: `CONFIG_VERSION` goes 1 → 2. A stored version-1 blob is read for its existing calibration fields,
then bandwidth, velocity modes, and provisional 7000/24000 mV bus thresholds are seeded, and the record is rewritten.
Calibration data MUST survive this migration; losing it would force a full recalibration, so the migration is a
distinct, separately verified task.

**Superseded**: `MotorCalibration.current_bandwidth` (currently hardcoded to `100.0f` at sketch line 300) is
replaced by the node-wide configurable value. The per-motor field is retained in the struct for calibration
compatibility but is no longer the source of the active bandwidth.

## Firmware: derived timing configuration

Recomputed whenever the active bandwidth changes; never persisted. All fields are reportable (FR-033).

| Field | Type | Derivation | Notes |
|---|---|---|---|
| `requested_bandwidth_hz` | `float` | From `RobotConfig` | What the operator asked for |
| `active_bandwidth_hz` | `float` | `min(requested, max_sustainable)` | Clamped value actually in use (FR-020b) |
| `clamped` | `bool` | `active < requested` | Drives the clamp report (FR-021c) |
| `sampling_multiple` | `uint8_t` | Fixed at 10 | FR-011 |
| `control_rate_hz` | `float` | `active_bandwidth × 10` | Nominal loop rate |
| `control_period_us` | `float` | `1e6 / control_rate` | Fed to every `Ts` (FR-019) |
| `carrier_hz` | `uint32_t` | `control_rate × decimation` | 20-50 kHz window (D5) |
| `decimation` | `uint8_t` | Smallest N giving carrier ≥ 20 kHz, carrier ≤ 50 kHz | ISR decimation factor |
| `max_sustainable_hz` | `float` | Measured ceiling / 10 | Set from M1; a build-time constant until measured |

**Invariants**:
- `carrier_hz == control_rate_hz × decimation` exactly, with integer `decimation`. Non-integer ratios are
  rejected at configuration time rather than rounded.
- `carrier_hz ≤ 50000`, the MCPWM library cap.
- Both motors always share one instance of this record (FR-028).

**Derived control gains**, recomputed on every bandwidth change (FR-026), from the existing per-motor
calibration:

| Gain | Formula |
|---|---|
| `PID_current_{d,q}.P` | `L_{d,q} × 2π × active_bandwidth` |
| `PID_current_{d,q}.I` | `R × 2π × active_bandwidth` |
| `LPF_current_{d,q}.Tf` | `1 / (2π × active_bandwidth × 5)` |
| every `PID.Ts`, every `LPF.Ts` | `control_period_us × 1e-6` |

## Firmware: per-motor runtime state

Not persisted. One instance per motor.

| Field | Type | Range | Notes |
|---|---|---|---|
| `mode` | enum | velocity, impedance | Active interpretation of incoming payloads |
| `p_des_mrad` | `int32_t` | Full signed range | Wire-authoritative accumulated shaft angle target at 0.001 rad resolution (FR-001a) |
| `p_des` | `float` | Derived from `p_des_mrad` | Control-domain target in radians |
| `v_des` | `float` | ±45 rad/s | Velocity target |
| `kp` | `float` | 0 - 50 N·m/rad | Stiffness; range scaled to this drive |
| `kd` | `float` | 0 - 1.0 N·m·s/rad | Damping; range scaled to this drive |
| `t_ff` | `float` | ±0.5 N·m | Feed-forward torque; range scaled to this drive |
| `velocity_target` | `float` | ±50 rad/s | Used only in velocity mode; the existing target |
| `last_command_us` | `uint32_t` | — | Timestamp of the last accepted motion command |
| `timed_out` | `bool` | — | Set when `now - last_command_us > 50 ms` (FR-029) |
| `armed` | `bool` | — | Existing arm state |
| `limit_cause` | bitmask | current, output voltage, bus voltage | Latched until reported at least once (FR-004a) |
| `limit_event_count` | `uint16_t` | saturating | Operator-visible count of effort-limiting transitions |
| `limit_was_active` | bitmask | same causes | Edge memory; prevents a sustained condition being counted again after report-latch clearing |
| `pair_fault_latched` | bool | — | Set by malformed, expired, stale, or mismatched command halves until reported |
| `applied_target_mrad` | `int32_t` | full signed range | Authoritative target, including same-cycle capture result |
| `capture_generation` | `uint8_t` | wraps | Changes whenever a capture request is applied; echoed in applied-target status |

Each motor also owns a non-persisted `PendingImpedancePair` with a position half, dynamics half, capture-current-
position flag, sequence numbers, receipt timestamps, and validity flags. A pair completed within 5 ms is applied
atomically. If capture is set, `applied_target_mrad` comes from that control cycle's measured shaft angle rather
than the transmitted position. A newer sequence or expiry discards the incomplete pair, latches the pair fault,
and does not refresh `last_command_us`.

**State transitions**:

```
                  arm cmd                    50 ms silence
   disarmed  ──────────────►  armed  ──────────────────────►  armed, timed out
      ▲                         │                                   │
      │  disarm / estop /       │  disarm / estop /                 │ valid command
      │  sustained overrun      │  sustained overrun                │ (FR-029b)
      └─────────────────────────┴───────────────────────────────────┘
```

**Rules**:
- Entering `armed` zeroes `p_des`, `v_des`, `kp`, `kd`, `t_ff`, and `velocity_target` (FR-009).
- `timed_out` produces zero commanded effort but does **not** disarm, and clears itself when commands resume
  (FR-029b). This is deliberately weaker than a disarm so a brief link glitch does not need operator action.
- A `mode` change is refused while `armed`; when disarmed it zeroes all terms, clears pending pairs, persists the
  new mode, and reports success only after persistence completes (FR-006b).
- Any of disarm, emergency stop, calibration entry, or sustained overrun forces the zero-effort path.

**Torque computation** (impedance mode), evaluated once per control cycle:

```
position_error = clamp(p_des - shaft_angle, -POSITION_ERROR_LIMIT, +POSITION_ERROR_LIMIT)   # FR-001b
torque         = kp × position_error + kd × (v_des - shaft_velocity) + t_ff
raw_current    = torque / torque_constant
current_target = clamp(raw_current, -current_limit, +current_limit)                         # FR-004
output_voltage = clamp(current_loop_output, -output_voltage_limit, +output_voltage_limit)   # FR-032
```

`POSITION_ERROR_LIMIT` is 1.0 rad (D8). `shaft_angle` is SimpleFOC's accumulated multi-turn angle. Current or
output-voltage clamping sets the corresponding per-motor cause bit and increments its counter on transition.
The new bus-voltage protection path uses the existing monitor, forces zero output and disarm outside its approved
window, and sets its cause bit. `limit_was_active` drives rising-edge counts independently from report latches.

## Firmware: loop timing record

Not persisted. One instance per node. All fields reportable (FR-015, FR-033).

| Field | Type | Notes |
|---|---|---|
| `cycle_count` | `uint32_t` | Total completed control cycles |
| `overrun_count` | `uint32_t` | Cycles that did not finish before the next trigger |
| `consecutive_overruns` | `uint16_t` | Reset on any clean cycle; drives the fail-closed threshold |
| `worst_cycle_us` | `uint32_t` | Longest observed execution time |
| `last_cycle_us` | `uint32_t` | Most recent execution time |
| `measured_rate_hz` | `float` | Completed cycles over a one-second window |

**Fail-closed rules** (FR-016):
- `consecutive_overruns` exceeding **10** disarms all motors and reports the cause.
- `measured_rate_hz` deviating from `control_rate_hz` by more than **5%** while `overrun_count` is unchanged
  indicates a timing-source fault; disarm and report.

## Backend: command state

In `jetson_xavier/backend/socketio_server.py`.

| Field | Type | Default | Notes |
|---|---|---|---|
| `motion_mode` | `"velocity" \| "impedance"` | `"velocity"` | Vehicle-wide operator selection |
| `kp` | `float` | 0.0 | Applied to all four wheels (spec assumption) |
| `kd` | `float` | 0.3 | Applied to all four wheels |
| `latest_targets` | `tuple[float, float, float, float]` | zeros | Most recent mixed per-wheel targets |
| `estop_state` | `bool` | `False` | Existing |
| `controller_status` | `dict[str, dict]` | `{}` | Per-node mirror of controller-reported mode, gains, bandwidth, accumulated positions, and limit causes |
| `capture_pending[4]` | `bool` | `False` | Set while stiffness enable awaits controller-applied target confirmation |
| `confirmed_target_mrad[4]` | `int32 | None` | `None` | Target echoed by `0x1E0`; never inferred from cached measured position |

**Rules**:
- The 200 Hz transmit loop sends current targets during motion and explicit zero-effort commands during normal
  idle/stop; the Socket.IO joystick handler only updates target state.
- On emergency stop, send `0x080` immediately and suspend motion frames until the existing clear path succeeds.
  A sender/link failure emits nothing and is the only normal way to exercise the 50 ms timeout.
- A zero-to-nonzero stiffness transition sets `capture_pending` for every wheel. While pending, each pair carries
  capture-current-position. Receipt of matching `0x1E0` applied targets clears pending and seeds the confirmed
  absolute targets used on later pairs.
- `controller_status` is populated only from received status frames, never from requested values (FR-040).

**Mecanum mixer is unchanged.** In impedance mode the mixer output becomes `v_des` per wheel, with `kp` normally
0 for driving, so the wheel is a damped velocity follower. The existing per-wheel sign convention carries over
untouched, which is what SC-015 verifies.

## UI: view state

In `jetson_xavier/webUI/src/App.vue`.

| Field | Type | Notes |
|---|---|---|
| `motionMode` | ref string | Operator selection, sent to backend |
| `kp`, `kd` | ref number | Validated against the contract ranges before emit (FR-039) |
| `controllerStatus` | ref object keyed by CAN id | Controller-reported mode, applied targets, gains, bandwidth, timeout/overrun, protocol and limit state |
| `estopped` | ref bool | Existing |

**Rules**:
- Bandwidth is display-only (FR-040a); the UI has no control that writes it.
- Displayed mode and gains come from `controllerStatus`, not from the local refs, so a refused or clamped value
  is visible as a mismatch.

## Entity relationships

```
RobotConfig (persisted, per node)
  ├── current_bandwidth_hz ──derives──► TimingConfig (per node)
  │                                       ├── control_period_us ──► every PID.Ts / LPF.Ts
  │                                       └── carrier_hz, decimation ──► MCPWM + ISR
  ├── motion_mode[2] ────────────────────► MotorRuntimeState.mode (per motor)
  └── MotorCalibration[2] (R, L) ─────────► derived current-loop gains

MotorRuntimeState (per motor) ◄──── ImpedanceCommand (one CAN frame per motor)
                              ◄──── VelocityCommand (retained legacy frame)

LoopTimingRecord (per node) ────► fail-closed disarm, serial + bus status reports
```
