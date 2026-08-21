# Contract: Serial Console

**Feature**: `002-mit-impedance-control` | **Date**: 2026-08-20

Serial at 115200 baud, dispatched by the existing SimpleFOC `Commander` on single-character command ids.
The bandwidth parameter is **writable only here** (FR-021a); the bus can read it but never set it.

## Existing commands, unchanged

| Id | Purpose |
|---|---|
| `a` | Motor 1 SimpleFOC commands (normal mode only) |
| `b` | Motor 2 SimpleFOC commands (normal mode only) |
| `A` | Arm: `A1`, `A2`, `A0` |
| `D` | Disarm: `D1`, `D2`, `D0` |
| `C` | Calibration: `C`, `C1`, `C2`, `CA`, `CM`, `CY`, `CN`, `CX`, `CE` |

## New commands

### `B` — current-loop bandwidth

| Form | Action |
|---|---|
| `B` | Report requested value, active value, clamp state, sampling multiple, control rate, carrier, and derived gains |
| `B<value>` | Set the requested bandwidth in Hz, integer, 100-10000 inclusive |

Accept rules:
- Refused unless **both motors are disarmed** (FR-025); reports that a disarm is required.
- Value outside 100-10000, malformed, or non-numeric is refused; the stored value stays and the valid range is
  reported (FR-024).
- An accepted value is persisted as the **requested** value (FR-020d), then the active value is derived and
  clamped if the required carrier would exceed the permissible maximum (FR-020b).
- On acceptance, current-loop gains, filter constants, and every `Ts` are recomputed and reported (FR-026).
- Refused with a calibration-required message when per-motor resistance and inductance are missing or invalid
  (FR-027).

Example output:

```
BW requested [Hz]: 2500
BW active    [Hz]: 1000   (CLAMPED: sustainable rate ceiling)
sampling multiple: 10
control rate [Hz]: 10000
control period [us]: 100.00
carrier [Hz]: 20000  (decimation 2)
M1 PID current D: P=0.006283 I=1.256637
M1 PID current Q: P=0.006912 I=1.256637
M1 current LPF Tf [s]: 0.00003183
M2 ...
```

The carrier shown follows the smallest-N rule: 1000 Hz of bandwidth needs a 10 kHz control rate, and
the smallest integer decimation putting the carrier at or above 20 kHz is 2, giving 20 kHz.

### `M` — motion mode

| Form | Action |
|---|---|
| `M` | Report each motor's active mode |
| `M1V` / `M1I` | Set motor 1 to velocity / impedance |
| `M2V` / `M2I` | Set motor 2 to velocity / impedance |
| `M0V` / `M0I` | Set both motors |

Accept rules:
- Refused while the addressed motor is armed (FR-006b), reporting that a disarm is required.
- An accepted change zeroes that motor's targets and gains, persists the new mode, and reports success only
  after the configuration write completes.

### `V` — bus-voltage protection window

| Form | Action |
|---|---|
| `V` | Report minimum, maximum, measured bus voltage, protection state, and event count |
| `V<min_mv>,<max_mv>` | Set the persisted protection window in millivolts |

Writes are refused while either motor is armed. Invalid, reversed, or out-of-board-range thresholds leave the
stored pair unchanged. The provisional 7000/24000 mV defaults are replaced by board-approved measured limits
before release; this configuration surface also supports the bounded bench test without overvolting hardware.

### `T` — timing and determinism report

`T` prints the loop timing record (FR-033):

```
control rate nominal [Hz]: 10000
control rate measured [Hz]: 9998
control period [us]: 100.00
cycles: 5991234   overruns: 0   consecutive: 0
last cycle [us]: 71   worst cycle [us]: 88
duty [%]: 71
carrier [Hz]: 30000  decimation: 3
timing fault: none
```

### `I` — impedance state report

`I` prints per-motor state (FR-034):

```
M1 mode: impedance  armed: yes  timed_out: no
M1 p_des [mrad]: 3142   p_meas [mrad]: 3138   applied target [mrad]: 3142
M1 v_des [rad/s]: 0.000   v_meas [rad/s]: 0.012
M1 kp: 12.000  kd: 0.300  t_ff [Nm]: 0.0000
M1 pos err [rad]: 0.00359 (sat limit 1.00000)
M1 torque cmd [Nm]: 0.0431   Iq meas [A]: 0.214
M1 limit causes: current   limit events: 1
M1 pair fault: no   last pair seq: 42   capture generation: 4
M2 ...
```

The measured accumulated position is also published in CAN status `0x1C0 + NodeID` for sender alignment. Limit
causes match `0x1D0 + NodeID` and remain latched until communications has reported them at least once.

## Reporting rules

- All reports are emitted from the communications context, never from the periodic control path (FR-035).
- Every refusal states the reason and the accepted range or precondition, so no failure is silent.
- No diagnostic printing is added inside the control task.

## Startup banner additions

The existing startup report gains the active bandwidth, control rate, carrier, and each motor's mode, so a
freshly powered unit reveals its timing configuration without a query.
