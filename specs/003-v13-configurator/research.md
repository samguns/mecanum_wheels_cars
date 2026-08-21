# Phase 0 Research: V13 Configurator

**Feature**: `003-v13-configurator` | **Date**: 2026-08-21

Findings are read from the firmware source in this repository and from current Tauri tooling. Items marked
**MEASURE** need confirmation on hardware before release.

## Environment baseline

| Item | Finding | Evidence |
|---|---|---|
| Firmware console | SimpleFOC `Commander`, single-character command ids, 115200 baud | sketch `setup()` |
| Existing commands | `a` `b` motor, `A`/`D` arm/disarm, `C…` calibration, `B` bandwidth, `T` timing, `Q…` records, `N` identity, `V` bus window, `M` motion mode | sketch Commander registrations; `V`/`M` added by feature 003 T093 |
| Console output style | Human prose, mixed one-line and multi-line, indented sub-fields | `printCalibrationRecord`, `reportBandwidth`, `doTiming` |
| In-stage abort | `calibrationAbortRequested()` polls **CAN only** | sketch lines 233-243 |
| Calibration blocking | Multi-second blocking `delay()` inside the command handler | `runAlignmentCalibration`, `runCharacteristicsCalibration` |
| Existing UI stack | Vue 3.5, Vite 7, TypeScript 5.9, Pinia | `jetson_xavier/webUI/package.json` |
| Serial plugin | `tauri-plugin-serialplugin` 2.22, Tauri 2.0+, Rust 1.70+, exposes control signals | plugin docs |

---

## D1. The console is prose, not a machine interface

**Decision**: Add a **structured, versioned, single-line response format** to the firmware, emitted alongside the
existing human output, and have the configurator consume only that.

**Rationale**: Scraping the current output is not viable for a safety-relevant tool. The existing format mixes
shapes in ways that break naive parsing, for example the selected motor arriving on its own line:

```439:439:v13_macnum_wheel_car.ino
  if (action == '1' || action == '2') { enterCalibrationMode(); selected_motor = action - '1'; Serial.println(F("CAL selected motor")); Serial.println(selected_motor + 1); return; }
```

`printCalibrationRecord` emits indented `  pole pairs: 7` style lines, `reportBandwidth` emits
`BW active    [Hz]: 1000   (CLAMPED: …)` with variable internal spacing, and refusals are free text such as
`BW REFUSED: disarm both motors first (D0)`. Any of these can be reworded during ordinary firmware maintenance,
and a text-scraping configurator would then silently misreport a motor's electrical parameters. The
constitution treats a machine-to-machine boundary as a hard contract, so making the console a machine interface
requires giving it an explicit, versioned schema.

**Alternatives considered**:
- *Parse the existing prose*: rejected. Zero firmware change, but the parser becomes an undocumented shadow
  contract over strings that no one is obliged to keep stable, which is precisely the silent-drift failure mode
  the constitution's protocol principle exists to prevent.
- *Replace the prose with structured output*: rejected. The console remains the primary human bench tool for
  features 001 and 002; removing readable output to serve a GUI is a net loss.
- *Emit structured output only in a mode the configurator switches on*: viable and slightly quieter, but a mode
  flag is extra state that can desynchronise. Emitting both unconditionally, with a distinguishing prefix, is
  simpler and lets a human watch the same session.

## D2. Serial cannot currently abort a running calibration stage

**Decision**: Extend the firmware's abort check to also poll the serial link for an abort byte. This is a
**mandatory safety prerequisite** for the feature, not an optional nicety.

**Rationale**: The in-stage abort path is CAN-only today:

```233:243:v13_macnum_wheel_car.ino
bool calibrationAbortRequested() {
  twai_message_t message;
  while (twai_receive(&message, 0) == ESP_OK) {
    if (message.identifier == 0x080 ||
        (message.identifier == config.can_id && message.data_length_code >= 2 && (message.data[1] & 0x08))) {
      failCalibration(F("emergency stop"));
      return true;
    }
  }
  return false;
}
```

Combined with two other facts this becomes a real hazard. Calibration stages block for seconds inside the
command handler, so the console is not being read while a motor is energised. And the chosen connection model is
direct serial with no dependency on the vehicle bus, so a bench session may have **no CAN bus present at all**.
The result is that on a bare bench, an operator who has started a stage currently has no software way to stop it.
Spec FR-015 requires a stop control reachable at all times while a powered procedure is possible, so the feature
cannot ship without closing this.

The change is small and mirrors the existing structure: check `Serial.available()` inside
`calibrationAbortRequested()` and treat a designated abort byte the same way the CAN emergency stop is treated,
routing through the existing `failCalibration()` so the disarm and reporting behaviour is unchanged.

**Alternatives considered**:
- *Disclose the limitation and rely on the physical power switch*: rejected. It knowingly ships a UI that offers
  a stop button which cannot stop anything mid-stage, which is worse than having no button.
- *Require a CAN interface for calibration*: rejected. It contradicts the clarified direct-serial decision and
  the requirement to work on a bench without the vehicle.
- *Make calibration non-blocking so the console keeps being served*: correct in the long run and much larger.
  Recorded as future work; the abort poll achieves the safety property now.

## D3. Progress reporting is stage-level, not continuous

**Decision**: Report calibration progress at **stage boundaries plus periodic in-stage heartbeats** emitted from
inside the existing calibration loops.

**Rationale**: FR-011 requires the active stage and its progress to be visible. Because the stages block, the
firmware must volunteer progress rather than answer a poll. The alignment sweep already iterates 1885 steps with
a 1 ms delay, and characterisation iterates a voltage ramp and 12 inductance samples, so both have natural loop
points at which a compact progress record can be emitted cheaply. Anything finer would add serial traffic inside
a timing-sensitive measurement for no operator benefit.

**Alternatives considered**:
- *Stage boundaries only*: rejected as the sole mechanism. A multi-second silence with an energised motor and a
  frozen progress indicator is exactly when an operator most needs to know the tool is alive.
- *Have the configurator infer progress from elapsed time*: rejected, it invents information.

## D4. Desktop app shape

**Decision**: Tauri 2 shell with a Vue 3 + TypeScript + Vite frontend, serial access through
`tauri-plugin-serialplugin`, as a standalone desktop application.

**Rationale**: This is the user's stated choice and it fits the constraints well. A desktop app removes the
browser-permission and origin problems that a web page hitting a local serial port would face, gives native
port enumeration on Windows, macOS and Linux, and lets the serial session live in Rust where it can be a single
owner of the port. Vue 3 with TypeScript and Vite also matches the existing operator UI's stack, so the team is
not learning a second frontend toolchain.

**Alternatives considered**:
- *Browser page using Web Serial*: rejected. Limited to Chromium browsers, requires a user gesture per
  connection, and cannot hold a port across a page reload, which is poor for a bench tool.
- *Extend the existing Jetson-hosted UI*: rejected by the clarification, and it would have reversed the
  deliberate decision that calibration lives on the direct console.
- *A terminal application*: cheaper, but the whole point of the feature is to replace console interaction.

## D5. Port ownership and the ESP32 reset problem

**Decision**: Own the port exclusively in the Rust layer, and explicitly control DTR and RTS on open.

**Rationale**: On most ESP32 boards the USB-serial bridge wires DTR and RTS to EN and IO0, so opening the port
with the default asserted signals **reboots the controller**. For a commissioning tool this is unacceptable
twice over: it interrupts whatever the controller was doing, and it means the tool's first act is an unannounced
reset. The plugin exposes control-signal setting, so the open sequence must set both signals to the non-resetting
state before or immediately as the port opens, then verify the link by identification rather than by assuming a
fresh boot banner. Whether a reset can be fully suppressed is board-dependent and must be confirmed
(**MEASURE**); if it cannot, the configurator must announce the reset rather than hide it.

A single owner in Rust also gives one place to serialise requests. The firmware console is a single
request-at-a-time interface with no request identifiers, so overlapping commands would interleave their prose and
their structured records unpredictably.

**Alternatives considered**:
- *Open the port from the frontend*: not possible with this plugin's model, and would scatter ownership.
- *Ignore control signals*: rejected, it makes an unannounced controller reset the normal case.

## D6. Request serialisation and identification

**Decision**: The Rust layer maintains a single in-flight request at a time, with a per-request tag echoed by the
firmware in its structured acknowledgement, and a timeout.

**Rationale**: The console has no request identity today, so a late response cannot be distinguished from a
response to the current request. Adding a short echoed tag to the structured acknowledgement makes matching
explicit and lets the configurator surface a timeout honestly instead of attributing an old reply to a new
request. This matters most around calibration, where a stage response can arrive many seconds after its command.

## D7. Parsing is the main correctness risk, and it is cheaply testable

**Decision**: Implement the structured-record parser as a pure function with no I/O, and introduce **Vitest** for
it as the feature's automated test surface.

**Rationale**: Features 001 and 002 had no automated test harness because their risk lived in physical behaviour
that only a bench can exercise. This feature is different: its highest-consequence failure is misreporting a
motor's electrical parameters, and that failure lives entirely in a string-to-data transformation which can be
exhaustively tested on a laptop in milliseconds. Declining to test it would be a deliberate choice to leave the
one cheaply verifiable risk unverified. The repository has no Vitest today, though `webUI/tsconfig.node.json`
already references a `vitest.config.*` include pattern, so the toolchain expectation is not foreign.

**Alternatives considered**:
- *Rust-side parsing with `cargo test`*: also good, and preferable if the parser lives in Rust. The decision is
  that the parser is tested, wherever it lives; the plan places it in Rust so the frontend receives typed data.

## D8. Read-only values still need a firmware read path

**Decision**: The structured configuration record must expose every field the spec requires to be displayed,
including both axis inductances and the electrical offset.

**Rationale**: FR-005 requires displaying pole pairs, direction, electrical offset, resistance, both axis
inductances, derived current-loop settings and per-motor completion state. The firmware already holds all of
these in `RobotConfig`, and `printCalibrationRecord` already prints all of them, so this is a formatting
addition rather than new measurement. No new firmware capability is needed, which is consistent with the spec's
assumption that a missing value would have to be identified here.

## D9. Writable settings and their guards

**Decision**: The writable set is exactly the four settings the spec names, each written through the firmware's
existing command surface, with the firmware remaining the authority on refusal.

| Setting | Firmware surface | Guard already enforced |
|---|---|---|
| Current-loop bandwidth | `B<hz>` | Disarm required, range 100-10000, calibration required |
| Bus-voltage window | `V<min>,<max>` | Disarm required, ordering and range validation |
| Per-motor control mode | `M1I`/`M1V`/`M2I`/`M2V` | Disarm required, calibration eligibility |
| Controller bus identity | `N` / `N<hex>` | Disarm required, range `0x001`-`0x7FF`, persist-before-ack |

**Consequence**: the four writable settings now have firmware commands. Feature 003 T093 implemented `V` and `M`
and recorded the transfer in feature 002's T036/T057 notes. Feature 002 still owns limiting/`0x1D0` and T037
eligibility routing.

## D10. Change records live on the operator's machine

**Decision**: The configurator writes its change log locally on the technician's machine, not on the controller.

**Rationale**: FR-036 requires recording accepted calibrations and writes with the acting operator. The
controller has limited NVS and no clock, so it cannot timestamp or store a session history meaningfully. The
operator's machine has both. This keeps a firmware-side storage change out of scope.

**Alternatives considered**:
- *Store a change counter on the controller*: rejected for now, though it would help correlate a unit with its
  history if logs are lost. Recorded as future work.

## D11. Identity scope

**Decision**: Local operator profile for **attribution only**, with no authentication and no access control.

**Rationale**: The reference design shows a signed-in user, and the spec requires the identity to be visible and
recorded, but nothing in the spec requires that access be *restricted*. A bench tool that runs on a technician's
own machine and talks to a device over a physical cable gains little from a login: physical access to the cable
is already the real gate. Introducing an identity provider would add a dependency and an offline-failure mode to
a tool that must work on a bench with no network.

**Alternatives considered**:
- *Full authentication against a workshop directory*: rejected as disproportionate, and it would break the
  no-network requirement.
- *No identity at all*: rejected, the spec requires attribution and the design shows it.

---

## Open items gated on hardware

| # | Item | Observed 2026-08-22 | Remaining gate |
|---|---|---|---|
| M1 | Whether DTR/RTS control can fully suppress the ESP32 reset on the target board | Phase 2 COM6 identify did not force a reboot when signals were deasserted; `uptime_ms < 2000` is reported as a reset | Re-confirm on the Tauri connect path (T055) |
| M2 | Worst-case latency from command to structured acknowledgement, including during a blocked stage | Normal 2 s / calibration 60 s windows. Tagged `N` and abort acks were inside the normal window on COM6 | Keep these timeouts unless a blocked-stage ack is measured slower |
| M3 | That the serial abort byte reliably interrupts both calibration stages | S6a 10/10 per stage per motor; S6b 10/10 idle | Closed for firmware; app Stop uses the same byte |
| M4 | Whether structured records emitted inside calibration loops perturb the measurements | Not re-measured this pass | T113 / S5 |
| M5 | Telemetry rate sustainable over the console without disturbing the control loop | Firmware floor 50 ms; app default 200 ms and returns the applied period | T092 overrun check against controller `T` |
