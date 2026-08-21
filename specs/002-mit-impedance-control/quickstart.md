# Quickstart: Validation Guide

**Feature**: `002-mit-impedance-control` | **Date**: 2026-08-20

Runnable validation for the impedance mode and the deterministic control loop. Scenarios are ordered so each
one gates the next: do not run a powered scenario until the bench-safe ones above it pass.

> **Safety**: every powered scenario is an attended bench procedure with the vehicle secured and wheels clear of
> the ground. Keep the serial console open and the emergency stop reachable. This matches the constitution's
> safety-critical motion gate.

## Prerequisites

| Item | Detail |
|---|---|
| Board | Classic ESP32, FQBN `esp32:esp32:esp32` |
| Library | Simple FOC 2.4.0 at `C:\Users\guns_\Documents\Arduino\libraries\Simple_FOC` |
| Calibration | Both motors must already be calibrated (feature 001). `C` reports the state. |
| Backend | Python 3.12, `jetson_xavier/backend/requirements.txt` |
| UI | Node 20.19+ or 22.12+, `jetson_xavier/webUI` |
| Bus | `can0` up at 1 Mbit/s |
| Instrument | Oscilloscope or logic analyser on a spare GPIO for loop-timing proof |

## S0. Compile gate (bench-safe, no hardware)

```bash
arduino-cli compile --fqbn esp32:esp32:esp32 .
```

Expected: clean compile. This is the narrowest boundary for every firmware change and must pass before anything
is flashed.

## S1. Loop timing determinism (powered, motors disarmed)

The core claim of the feature. Motors stay disarmed, so no torque is produced.

1. Flash and open serial at 115200.
2. Confirm the startup banner reports the active bandwidth, control rate, carrier, and both motor modes.
3. Run `T` and record `control rate measured`, `worst cycle [us]`, and `duty [%]`.
4. Toggle the instrumented GPIO once per control cycle and capture every edge for 10 minutes with a logic
   analyser export; compute total intervals, intervals within ±5% nominal, and missing-period gaps.
5. Drive load onto the system: stream joystick input, keep the UI open, and run `I` repeatedly.
6. Run `T` again.

Expected:
- Measured rate within **1%** of nominal (SC-002).
- At least **99.9%** of captured intervals within ±5% nominal and zero missing-period gaps (SC-001).
- `overruns: 0` across a 10-minute run.
- `duty` below about 70%, leaving overrun headroom.

**This scenario produces the number that sets the published bandwidth ceiling** (research M1). Record it.

## S2. Bandwidth configuration and clamping (powered, disarmed)

1. Run the host-side pure-helper check for every integer request from 100 through 10000 plus `99`, `10001`,
   malformed, and non-numeric inputs. Assert sampling multiple remains 10, integer carrier ratio holds, and clamp
   results are deterministic.
2. After T031 publishes the measured ceiling, build the unique ordered matrix `{100, 500, 1000, ceiling,
   ceiling + 1, 10000}`. For each value, issue `B<value>`, record requested/active/clamp/rate/carrier/gains, power
   cycle, and confirm the requested value survives unchanged.
3. At the lowest, default-derived, and highest active carrier points, compare phase current with the reference.
4. Send `B99`, `B10001`, malformed, and non-numeric inputs; confirm the previous stored value remains.
5. Send 100 `0x120 + NodeID` control frames with forbidden command `0x12` and varied payload values; confirm every
   probe is rejected/reported and requested, active, and persisted bandwidth remain unchanged (SC-009a).

Expected: exhaustive software invariants pass; every unique hardware matrix value persists and re-derives; phase
current stays within 5%; invalid inputs do not modify storage (SC-006, SC-012).
Arm a motor with `A1`, then try `B2000`: refused with a disarm-first message (FR-025).

## S3. Deterministic zero-output paths (powered, armed, wheels clear)

Each path must reach zero effort. Verify with `I` and by hand-feel on the wheel.

| Path | Action | Expected |
|---|---|---|
| Disarm | `D0` | Bridge unpowered, targets zeroed |
| Normal UI stop | Release input / request stop | Sender continues 200 Hz explicit zero-effort commands; no timeout |
| Emergency stop | Engage UI estop | Sender immediately sends `0x080`, suspends motion frames, all effort clears and both motors disarm |
| Command timeout | Kill sender or disconnect link | Frames cease; zero effort within **50 ms**, still armed, recovers when valid frames resume (SC-004, FR-029b) |
| Mode change | `M1V` while disarmed | Targets and gains cleared |
| Sustained overrun | Inject overruns in a debug build | All motors disarmed, cause reported |

## S4. Impedance behaviour (powered, armed, one motor, wheels clear)

1. `M1I` while disarmed, then `A1`.
2. Send a matching `0x101` position half and `0x131` dynamics half with `kd` = 0.3, `kp` = 0, `v_des` = 0,
   `t_ff` = 0, and the same sequence. Turn the wheel by hand: it should feel
   viscous, resisting speed but not displacement.
3. Set `kp` = 12.0 with `p_des` at the present angle. Displace the wheel: it should push back proportionally to
   displacement and return.
4. Sweep displacement across at least five points, recording commanded current from `I`.
5. Set `kp` = 0, `kd` = 0, `t_ff` = 0.05 N·m. The wheel should produce steady torque regardless of position.
6. For SC-008, set `kp` = 0, a fixed non-zero `kd`, and zero feed-forward torque. Command at least five
   velocity targets spanning the documented operating range in both directions. At steady state, compare each
   measured speed with the retained velocity-mode baseline at the same target; every impedance-mode result must
   remain within 10%, with no effort-limit event contaminating a run.

Expected: restoring effort is monotonic in displacement and never exceeds the 3.0 A limit (SC-007); the
zero-stiffness impedance velocity sweep stays within 10% of its retained-mode baseline (SC-008).

### S4a. Effort-limit protection and reporting

In impedance mode, trigger current limiting with a bounded torque request and output-voltage limiting with a low
temporary motor limit. For bus protection, save the production `V` window, disarm, set a temporary 11500/14000 mV
window, use a current-limited programmable supply initially at 12.0 V, arm at zero effort, and lower in at most
0.25 V steps without exceeding 12.0 V until the measured bus crosses 11.5 V. Confirm immediate zero-output disarm,
then restore 12.0 V, restore the saved production window while disarmed, and power-cycle to confirm restoration.

For five consecutive trials per path, verify the applied current/voltage stays within its configured bound, the
`I` report names the correct motor and cause, `0x1D0 + NodeID` carries the same cause and increments its counter,
and the UI displays it within one second (SC-007a). Restore production limits after the test.

### S4b. Load-independent impedance response

Command the same bounded impedance step three times with communications otherwise idle, then three times while
streaming normal CAN status, UI traffic, serial `I` requests, and voltage monitoring. Record position versus time,
calculate settling time and overshoot for each run, and compare the idle and loaded groups.

Expected: settling time and overshoot agree within 10%, with no effort-limit event contaminating a run (SC-003).

## S5. Position error saturation (powered, armed, wheels clear)

With maximum stiffness, send a valid matched pair whose signed `int32` milliradian `p_des` is offset by 100
revolutions. Repeat 100 times. Then repeat with a missing half and with mismatched sequence numbers.

Expected: a valid pair commands no more current than the same stiffness at the 1.0 rad saturation limit. Invalid
pairs do not change any applied term and do not refresh the timeout (SC-013). **This is the most safety-relevant
single check in the feature** — a failure means a bad or torn target can command unintended torque. Each invalid
pair must also set the addressed motor's pair-fault status before its latch is cleared.

## S6. Mode isolation (powered, armed)

| Setup | Send | Expected |
|---|---|---|
| Motor 1 in impedance | Legacy `0x201` velocity command | Rejected and reported, no motion |
| Motor 1 in velocity | Matched impedance pair `0x101` + `0x131` | Rejected and reported, no motion |

100 trials of each, zero motions (SC-011).

While disarmed, store impedance for motor 1 and velocity for motor 2, power-cycle, and confirm both modes restore
while both motors remain disarmed with zero targets and no pending pair. Restore all motors to velocity before S8.

For each required electrical field (resistance, inductance, current-sense calibration, and torque constant), make
that field missing and then invalid on a bench configuration. Through both CAN and serial paths, attempt impedance
selection and arming. Each attempt must be rejected without changing mode or arm state and must report the same
motor-specific calibration cause. Restore the valid production calibration after each case (FR-027).

## S7. Backend and UI end to end (powered, on stands)

```bash
cd jetson_xavier/backend && pip install -r requirements.txt && python socketio_server.py
cd jetson_xavier/webUI && npm install && npm run type-check && npm run dev -- --host 0.0.0.0
```

1. Open the UI, confirm the status panel shows both nodes' mode, gains, bandwidth, and clamp state read back
   from the controllers.
2. Confirm bandwidth is displayed but has no editable control (FR-040a).
3. Set stiffness and damping; confirm the values echoed back come from the controllers.
4. While driving with `kp=0`, select non-zero stiffness. Confirm capture flags repeat until `0x1E0/0x1F0`
   generations advance, the echoed applied targets seed the holds, and no position-error saturation transient occurs.
5. Enter an out-of-range gain; confirm refusal with the valid range and no change in effect.
6. Select velocity mode, drive forward, reverse, strafe, rotate. Confirm correct wheel directions.
7. Select impedance mode, repeat the same four motions (SC-015).
8. Verify normal stop keeps zero frames flowing; then verify emergency stop sends `0x080` and suspends motion.

Expected: readback within 1 second (SC-014); impedance-mode driving matches velocity-mode directions.

## S8. Backward compatibility (powered, on stands)

Set and persist all four motors to velocity, power-cycle, confirm the restored modes and disarmed zero state, then
run the **pre-feature** backend against the new firmware.

Expected: the stored velocity configuration drives with no observable behavior change across all four motions
(SC-010). This proves retained-mode compatibility without assuming prior mode history.

## S9. Bus load check

With the vehicle driving in impedance mode:

```bash
# frame rate and load
candump -t d can0 | pv -l -i 1 > /dev/null
ip -details -statistics link show can0
```

Expected: about 2120 frames/s and roughly 28% utilisation, below the 50% contract ceiling, with no error frames
or bus-off events.

## Regression checklist

- [ ] `arduino-cli compile` clean
- [ ] Calibration flow (feature 001) still reachable and unchanged
- [ ] Stored calibration survives the `CONFIG_VERSION` migration
- [ ] Existing status frames `0x180`/`0x190` and heartbeat `0x700` unchanged
- [ ] Bus voltage still displayed in the UI
- [ ] Current, output-voltage, and bus-voltage limit causes verified and visible in serial, CAN, and UI
- [ ] `npm run type-check` clean
- [ ] Emergency stop verified in both modes
- [ ] Hardware verification note recorded per the constitution's change gate
