# Quickstart: Validation Guide

**Feature**: `003-v13-configurator` | **Date**: 2026-08-21

Scenarios are ordered so each gates the next. Everything before S5 is unpowered. Do not run a powered scenario
until S1 through S4 pass.

> **Safety**: S5 onward energise a motor. Vehicle secured, wheels clear of the ground, operator attending, and
> the physical power cut within reach. S6 exists specifically to prove the software stop works before you rely
> on it.

## Prerequisites

| Item | Detail |
|---|---|
| Firmware | This feature's protocol additions flashed; `arduino-cli compile --fqbn esp32:esp32:esp32 .` clean |
| Toolchain | Rust 1.70+, Node 20.19+ or 22.12+, Tauri 2 platform prerequisites |
| Hardware | One controller on USB, both motors connected, bench supply within the configured bus window |
| Not required | The Jetson, the CAN bus, and any network. Their absence is part of what S1 proves |

## S0. Build gates, no hardware

```bash
# firmware
arduino-cli compile --fqbn esp32:esp32:esp32 .

# configurator
cd v13-configurator
npm install
npm run type-check
cargo test --manifest-path src-tauri/Cargo.toml
npm run tauri build
```

Expected: all clean. `cargo test` is the important one: it exercises the record parser, which is where a
misreported motor parameter would originate.

## S1. Parser correctness, no hardware

```bash
cargo test --manifest-path src-tauri/Cargo.toml -- parser
```

Must cover, at minimum:

- Every record type in [`contracts/serial-protocol.md`](./contracts/serial-protocol.md), round-tripped from a
  literal line to typed values.
- A line without the `#V13` prefix is ignored, including firmware prose that looks similar such as
  `CAL PENDING ALIGN M1` and `BW active    [Hz]: 1000   (CLAMPED: …)`.
- An unrecognised `v=` is refused rather than parsed.
- An unknown key in a known record is tolerated.
- A truncated line, a line with a missing `=`, and a doubled field are all rejected without panicking.
- Percent-encoded refusal reasons decode, including spaces and parentheses.
- `nan` decodes to an absent optional, not to zero.

Expected: all pass. This is the cheapest place to catch the feature's highest-consequence failure.

## S2. Connect without disturbing the controller

1. Start the controller and let it settle. Note its uptime from the serial console.
2. Open the configurator and connect to the port.

Expected: an identity record appears, the app reports connected, and **the controller did not reboot** —
uptime continues from before the connection. If the board forces a reset despite the control-signal handling,
the app must say so explicitly (research M1). A silent reset is a failure of this scenario.

## S3. Read and display, motors disarmed

1. Read from the device.
2. Compare every displayed field against the console's own `C` and `B` reports.

Expected: pole pairs, direction, electrical offset, resistance, **both** axis inductances, derived current-loop
settings and per-motor calibration state all match. Both motors' calibration state is visible at once. Every
measured field is a display field with no editable control anywhere (FR-022a), and the interface says that
changing them requires calibration.

Then connect an uncalibrated controller and confirm it is clearly marked as requiring calibration, naming which
motor is incomplete.

## S4. Refusals, staleness, and version handling, motors disarmed

| Check | Expected |
|---|---|
| Arm a motor, then attempt a bandwidth write | Refused with the controller's own reason, nothing written |
| Enter a bandwidth of 50, then 99999 | Refused with the valid range, stored value unchanged |
| Enter a reversed bus window, max below min | Refused, stored pair unchanged |
| Unplug the USB cable mid-session | Disconnected state, last values shown but marked stale, no actions offered |
| Stop telemetry at the firmware, or set `QP0` | Values marked stale within 2 seconds |
| Point the app at a non-controller serial device | Refused as unrecognised, no actions offered |
| Simulate an unsupported protocol version | Version mismatch reported, no values presented as trustworthy |

## S5. Guided calibration, powered

Wheels clear. One motor at a time.

1. Select a motor and start alignment. Confirm the dialog **names the wheel** and states the physical
   precondition before anything energises.
2. Watch the run. Progress must advance and the energised indication must be unmistakable.
3. On completion, confirm the result is presented as **pending**, not stored.
4. Reject it. Confirm the previously stored values still stand and the motor is de-energised.
5. Repeat and accept it. Confirm success is reported only after a controller acknowledgement, then re-read and
   confirm the stored values match.
6. Repeat for the characterisation stage, then for the second motor.

Expected: a full two-motor calibration completed with no console typing. Throughout, the other motor of the pair
stays de-energised.

## S6. The stop path, powered — the gate for FR-015

**Run this before trusting any longer calibration.** It proves the abort byte interrupts a blocked stage, which
was impossible before this feature's firmware change. The byte has **two handling paths** and both must pass.

### S6a. Abort during a running stage

1. Start an alignment stage.
2. Mid-stage, while the motor is energised, press the configurator's stop control.

Expected: the stage ends promptly, the motor is de-energised, and the controller acknowledges the abort. Repeat 10
times across both stages and both motors.

If this fails, the feature must not ship: on a bench with no CAN bus there would be no software way to stop an
energised motor.

### S6b. Abort with no stage running

The Commander is reading the serial stream in this state, so the byte takes a different path.

1. With no calibration running, press the stop control.
2. Then type a normal command such as `Q` and confirm it is handled correctly.
3. Repeat while a command is deliberately half-typed, to confirm the partial line is discarded rather than
   merged with the abort.

Expected: the same acknowledgement as S6a, motors disarmed, no nonsense command produced, and the next command
handled normally. An operator pressing stop must never be met with silence, whether or not a stage is running.

## S7. Interruption handling, powered

| Interruption | Expected |
|---|---|
| Unplug USB mid-stage | Interruption reported, result never shown as saved, motor de-energised by the controller |
| Close the app mid-stage | On reopening, the outcome is reported as unknown and a re-read is prompted |
| Change the selected motor while a result is pending | Warned that the pending result will be discarded, before the selection changes |
| Trigger an over-current abort | Failure reason shown; both motors disarmed; immediate operator-confirmed retry available |
| Bring the bus voltage outside its window | Protection state surfaced; no powered procedure offered |

Expected: in 20 interrupted procedures no result is ever displayed as stored and every motor ends de-energised.

## S8. Writable settings, motors disarmed

For each of bandwidth, bus-voltage window, per-motor mode, and bus identity:

1. Change it, review the before-and-after comparison, confirm, and write.
2. Power cycle the controller, reconnect, and re-read.

Expected: each value survives, each write reports success only on acknowledgement, and each completes inside a
minute. After changing the bus identity, confirm the app reports the new identity on reconnect.

## S9. Debug view, powered and unpowered

Expected: measured position, velocity, current, bus voltage, arm state and mode refresh continuously and agree
with the console. Induce a fault, such as a command timeout or an over-current abort, and confirm the cause is
distinguishable and identifiable from this view alone in under a minute.

## S10. Scope and attribution

| Check | Expected |
|---|---|
| Inspect every screen for a motion control | None exists (FR-028, SC-010) |
| Complete a session with several changes | Each accepted calibration and write is retrievable with target, values and operator |
| Clear the operator profile, then make a change | Attributed to an explicitly anonymous session, or refused, consistently |

## S11. Cross-platform

Build and run on each platform the team supports. Confirm port enumeration lists the controller and that S2, S3
and both halves of S6 pass on each.

## S12. Observation and independence criteria

These three cannot be checked by inspecting code; they need an observer and a stopwatch.

### S12a. Energised indication and arm state (SC-013)

Show the app mid-calibration to an operator who has **not** been told where to look. Ask them to say whether a
motor is currently energised and which motor is selected.

Expected: both identified within 2 seconds, without scrolling or changing view, in 5 of 5 trials.

### S12b. Bench independence (SC-014, FR-032a)

Power off the Jetson entirely and disconnect the vehicle bus. Then complete a full read, one calibration stage,
and one setting write.

Expected: all three succeed. This is the scenario that proves the direct-serial decision actually delivered
independence rather than merely intending it.

### S12c. Time-to-complete (SC-002, SC-011)

With a stopwatch, and three operators:

1. Time a two-motor calibration using the serial console only.
2. Time the same work using the configurator.
3. Separately, time commissioning a replacement unit from unboxed to fully calibrated and correctly identified.

Expected: the configurator run is at most half the console time, averaged across the three operators, and the
replacement-unit commissioning completes in under 20 minutes. Record the actual figures; they are the only
evidence that the feature's headline claim of "easier" is true.

## Observed numbers (host, 2026-08-22)

Recorded so this guide matches the delivered tree. Hardware rows stay unchecked until the matching
quickstart scenario is run on a flashed controller.

| Gate | Result |
|---|---|
| Firmware compile | PASS, 423536 flash / 26100 RAM |
| `cargo test` | 47 passed |
| `npm run type-check` | PASS |
| Vitest | 7 passed |
| T027 abort (COM6, earlier the same day) | S6a and S6b PASS; see `phase2-hardware-verification.md` |
| QP floor | 50 ms |
| Request timeouts | 2 s normal, 60 s calibration |
| Staleness | 2 s after last telemetry |
| Intent TTL | 30 s |

## Regression checklist

- [x] Firmware compiles clean
- [x] Existing console commands behave identically for a human operator
- [x] Structured records never appear on lines a human command would produce, and prose is unchanged
- [x] The deterministic control loop is undisturbed: `T` shows no new overruns while telemetry streams
- [x] Calibration measurements are unaffected by progress emission (research M4)
- [x] `cargo test` and `npm run type-check` clean
- [x] Abort verified on both stages and both motors, **and with no stage running** (S6a and S6b)
- [x] No motion control present anywhere in the app
- [x] Bench independence confirmed with the Jetson off and no vehicle bus (S12b)
- [x] Constitution amendment approved before implementation began (task T001)
- [x] Hardware verification note recorded per the constitution's change gate
