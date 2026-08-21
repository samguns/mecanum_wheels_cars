# Contract: Structured Serial Protocol

**Feature**: `003-v13-configurator` | **Date**: 2026-08-21

The link between the configurator and one controller. 115200 baud, 8N1, line oriented, LF terminated.

This makes the firmware console a **machine interface as well as a human one**. Per the constitution's protocol
principle it is therefore a hard contract: the schema is versioned, and firmware and configurator change
together. The existing human prose is unchanged and continues to be emitted.

## Framing

Every machine-readable line begins with `#V13 ` and nothing else does. Prose lines never begin with `#V13`.
A reader MUST ignore any line that does not start with the prefix, which makes the stream safe to share with a
human watching the same session, and tolerant of SimpleFOC's own monitor output.

```
#V13 v=1 t=<type> <key>=<value> <key>=<value> …
```

Rules:

- `v` is the schema version and is always the first field. A reader that does not recognise `v` MUST refuse to
  interpret the record and MUST report a version mismatch (FR-004).
- `t` is the record type and is always the second field.
- Remaining fields are `key=value`, space separated, in any order. A reader MUST tolerate unknown keys, so the
  firmware can add fields without breaking an older configurator.
- Values contain no spaces. Free text is percent-encoded, so a refusal reason survives intact.
- Booleans are `0` or `1`. Absent optional numeric values are `nan`.
- One record per line. A record is never split across lines, which is the specific failure of the prose format.
- Records are emitted only from the communications context, never from the deterministic control path.

## Identification

Sent unprompted once after boot, and in response to `Q`.

```
#V13 v=1 t=id fw=002 proto=1 canid=0x201 motors=2 cfgver=2 uptime_ms=41234
```

| Key | Meaning |
|---|---|
| `fw` | Firmware feature level |
| `proto` | This protocol version, mirrors `v` |
| `canid` | Configured controller bus identity |
| `motors` | Number of motors this controller drives |
| `cfgver` | Stored configuration format version |

The configurator MUST confirm an `id` record before offering any action (FR-032c). A device that does not emit
one within the connection timeout is treated as unrecognised.

## Query commands

New single-character command `Q`, following the existing Commander convention.

| Command | Emits |
|---|---|
| `Q` | `id`, then one of every record type once, including `imp` |
| `QC` | `cal` for both motors, plus `cfg` |
| `QT` | `timing` |
| `QI` | `motor` then `imp` for both motors |
| `QP<ms>` | Sets the unsolicited telemetry period in milliseconds; `QP0` stops it. Stream is `motor`, `imp`, `bus` |

`QP` exists so the debug view does not have to poll. A period below a firmware-enforced floor is clamped and the
clamp is reported, so telemetry can never crowd out the control loop.

## Record: calibration state

One per motor, in response to `QC` and after any accepted calibration.

```
#V13 v=1 t=cal m=1 aligned=1 charac=1 pp=7 dir=1 offset=2.094395 r=0.084000 ld=0.000125 lq=0.000131 valid=1
```

| Key | Meaning | Units |
|---|---|---|
| `m` | Motor index, 1 or 2 | — |
| `aligned` | Alignment stage confirmed | bool |
| `charac` | Characterisation stage confirmed | bool |
| `pp` | Pole pairs | count |
| `dir` | Sensor direction, `1` clockwise, `-1` counter-clockwise | — |
| `offset` | Electrical offset | rad |
| `r` | Phase resistance | ohm |
| `ld`, `lq` | Axis inductances | henry |
| `valid` | Whole record passes the firmware's validity check | bool |

Both `ld` and `lq` are always present. The reference design shows a single inductance field; the device stores
two, and the contract reports what the device stores.

## Record: controller configuration

```
#V13 v=1 t=cfg canid=0x201 bw_req=2500 bw_act=1000 bw_clamped=1 rate=10000 carrier=20000 decim=2 mode1=0 mode2=0 busmin_mv=7000 busmax_mv=24000 calibrated=0
```

`bw_req` versus `bw_act` with `bw_clamped` is how the operator sees that a requested bandwidth was reduced.
`mode1`/`mode2` are `0` velocity, `1` impedance.

## Record: motor runtime state

```
#V13 v=1 t=motor m=1 armed=0 mode=0 pos_mrad=628319 vel=0.012 iq=0.214 timeout=0 limits=0 limitcount=0 pairfault=0
```

`limits` is a bitmask matching the firmware's effort-limit causes: bit 0 current, bit 1 output voltage, bit 2 bus
voltage. Reporting the raw mask plus a decoded list in the UI keeps the contract stable as causes are added.

`pairfault` is the latched pair-match fault (`0`/`1`). Serial `K` applies atomically and does not set it; CAN
pair RX (feature 002 T039/T040) still can. A board flashing older firmware may still emit `nan`; the configurator
MUST treat `nan` as unavailable rather than as false, and MUST treat the output-voltage bit as unevaluated in
that case.

## Record: impedance state

One per motor, in response to `Q`, `QI`, `I`, and each `QP` telemetry tick.

```
#V13 v=1 t=imp m=1 pdes_mrad=3142 vdes=0.000 kp=12.000 kd=0.300 tff=0.0000 perr=0.00359 tq=0.0431 applied_mrad=3142 capgen=4 seq=42 pairfault=0 eligible=1 hold=1
```

| Key | Meaning | Units |
|---|---|---|
| `m` | Motor index, 1 or 2 | — |
| `pdes_mrad` | Applied position target | mrad |
| `vdes` | Applied velocity target | rad/s |
| `kp`, `kd`, `tff` | Applied stiffness, damping, feed-forward | N·m/rad, N·m·s/rad, N·m |
| `perr` | Saturated position error from the last control cycle | rad |
| `tq` | Commanded torque from the last control cycle | N·m |
| `applied_mrad` | Last applied target, including capture substitution | mrad |
| `capgen` | Capture-generation handshake counter | count |
| `seq` | Last accepted sequence, or `-1` if none | — |
| `pairfault` | Latched pair-match fault | bool |
| `eligible` | Impedance eligibility guard result | bool |
| `hold` | Serial apply is held (not subject to the 50 ms CAN timeout) | bool |

## Human `I` and serial apply `K`

Transferred from feature 002 (2026-08-22). These are the serial stand-in for CAN pairs on a bench with no
vehicle bus.

| Command | Action |
|---|---|
| `I` | Human prose per `specs/002-mit-impedance-control/contracts/serial-console.md`, plus one `imp` record per motor |
| `K<n>,<p_mrad>,<v>,<kp>,<kd>,<tff>` | Apply the five terms atomically to motor `n` |

`K` is refused unless that motor is already in impedance mode. Ranges: `p_mrad` ±628319, `v` ±45 rad/s, `kp` 0–50,
`kd` 0–1, `t_ff` ±0.5 N·m. Applying while armed produces torque. Arming in impedance mode, and `M*I`, both go
through the per-field eligibility guard.

## Record: timing and determinism

```
#V13 v=1 t=timing rate_nom=10000 rate_meas=9998 period_us=100.00 cycles=5991234 overruns=0 consec=0 last_us=71 worst_us=88 duty=71.0 fault=none
```

`fault` is one of `none`, `overrun`, `rate`. This is what backs the debug view's timing panel (FR-026).

## Record: bus voltage

```
#V13 v=1 t=bus mv=12040 protect=0
```

## Record: acknowledgement

Every command that changes state produces exactly one `ack`, whether it succeeded or was refused.

```
#V13 v=1 t=ack tag=7 cmd=CY ok=1 reason=
#V13 v=1 t=ack tag=8 cmd=B2500 ok=0 reason=disarm%20both%20motors%20first
```

| Key | Meaning |
|---|---|
| `tag` | Echo of the request tag supplied by the configurator |
| `cmd` | The command as received |
| `ok` | `1` accepted, `0` refused or failed |
| `reason` | Percent-encoded refusal reason, empty on success |

`reason` carries the firmware's own wording so the configurator can satisfy FR-030 without inventing text.

### Request tagging

The configurator prefixes a command with `#<tag>;`, for example `#7;CY`. The firmware strips the prefix, records
the tag, and echoes it in the `ack`. An untagged command is still accepted and acknowledged with `tag=0`, so a
human typing at the same console is unaffected.

This exists because the console has no request identity, so a late reply is otherwise indistinguishable from a
reply to the current request. It matters most for calibration, where an `ack` can arrive seconds later.

## Record: calibration progress

Emitted from inside the calibration stages so a blocked, energised procedure is visibly alive (research D3).

```
#V13 v=1 t=calprog m=1 stage=align pct=42 energised=1
#V13 v=1 t=calprog m=1 stage=charac pct=100 energised=0
```

`stage` is `align` or `charac`. `energised` states plainly whether the motor is currently powered, which the UI
must surface prominently (FR-011).

## Record: pending calibration result

Emitted when a stage completes and a result awaits an accept or reject decision.

```
#V13 v=1 t=calpend m=1 stage=align pp=7 dir=1 offset=2.094395
#V13 v=1 t=calpend m=1 stage=charac r=0.084000 ld=0.000125 lq=0.000131
```

The configurator MUST present these as pending and MUST NOT treat them as stored until a `CY` is acknowledged
with `ok=1` (FR-012, FR-013).

## Record: fault

```
#V13 v=1 t=fault kind=calibration reason=over-current%20abort cooldown_ms=0
```

`cooldown_ms` is retained as a zero-valued version-1 compatibility field. Calibration failures return
to an immediately retryable idle state.

`kind` is one of `calibration`, `timing`, `bus`, `protocol`.

## Emergency abort over serial

**This is a mandatory safety addition, not a convenience.** Calibration stages block for seconds inside the
command handler, and the existing in-stage abort polls the CAN bus only. Because the configurator connects by
serial and may run on a bench with no CAN bus present, without this there is no software way to stop an
energised stage.

| Property | Value |
|---|---|
| Abort byte | `0x18` (CAN, "cancel") |
| Accepted | At any time, including inside a blocked calibration stage |
| Effect | Same path as the existing CAN emergency stop: `failCalibration()`, motors disarmed |
| Acknowledgement | `#V13 v=1 t=ack tag=0 cmd=ABORT ok=1 reason=` |

A single raw byte is used rather than a text command because it must be recognisable without waiting for a line
terminator and without the Commander's line assembly, which is not running during a blocked stage.

### Two distinct handling paths are required

"At any time" needs two implementations, because the byte arrives in two different contexts:

**Inside a calibration stage.** The firmware MUST check for the byte at every point it currently checks for a
CAN abort, so the disarm and reporting behaviour is identical whichever path triggers.

**Outside a calibration stage.** The SimpleFOC Commander is reading the serial stream and assembling lines. A
raw `0x18` would otherwise be buffered as command text and emerge as a nonsense command, and it could corrupt a
legitimate command that is mid-line. The firmware MUST therefore intercept and consume `0x18` **before the
Commander sees it**, discard any partially assembled command line, disarm, and acknowledge.

Both paths MUST emit the same acknowledgement, so the configurator's stop control behaves identically whether or
not a stage is running. An operator pressing stop must never be met with silence.

## New writable setting: controller bus identity

The spec makes the bus identity writable, but no firmware command exists for it today (research D9). Adding one
is in scope.

| Command | Action |
|---|---|
| `N` | Report the current identity |
| `N<hex>` | Set it, range `0x001`-`0x7FF` |

Guards MUST match the existing settings commands: refused while either motor is armed, validated against range,
persisted before success is acknowledged, and refused with a stated reason otherwise.

## Firmware commands this contract depends on

**Verified against the firmware, which currently registers only `a b A D C B T`.** Do not assume a command
exists because a design document mentions it.

| Command | Use by the configurator | Status in firmware today |
|---|---|---|
| `C`, `C1`, `C2`, `CA`, `CM`, `CY`, `CN`, `CX`, `CE` | The calibration workflow | **Exists** (feature 001) |
| `A0`/`A1`/`A2`, `D0`/`D1`/`D2` | Arm and disarm | **Exists** |
| `B`, `B<hz>` | Bandwidth read and write | **Exists** (feature 002) |
| `T` | Human timing report; `QT` is the machine equivalent | **Exists** (feature 002) |
| `V`, `V<min>,<max>` | Bus-voltage window write | **Exists** (transferred from 002 T036) |
| `M`, `M1I`, `M1V`, `M2I`, `M2V` | Per-motor control mode write | **Exists** (transferred from 002 T057; eligibility-gated) |
| `I` | Human impedance report plus `imp` records | **Exists** (transferred from 002 T066) |
| `K<n>,<p>,<v>,<kp>,<kd>,<tff>` | Serial impedance apply | **Exists** (serial stand-in for 002 CAN pairs) |
| `Q` family, `N`, request tags, abort byte | This feature | Added by this feature |

The configurator MUST NOT use `a` or `b`, the raw SimpleFOC motor command passthroughs. Impedance apply uses
`K` only, from the debug view, never a chassis velocity or joystick command.

## Compatibility and versioning

- Adding a key to an existing record, or adding a new record type, does **not** bump `v`.
- Removing or renaming a key, changing a unit, or changing a value's meaning **does** bump `v`.
- The configurator refuses to operate against an unrecognised `v` rather than guessing (FR-004).
- Firmware and configurator changes to this contract land together, and this file is updated in the same change.

## Timing contract

| Parameter | Value |
|---|---|
| Baud | 115200, 8N1 |
| Connection identification timeout | 2 s to receive an `id` record |
| Normal command acknowledgement timeout | 2 s |
| Calibration stage acknowledgement timeout | 60 s, covering the firmware's own 30 s alignment timeout plus margin |
| In-flight requests | Exactly one; the configurator serialises |
| Default telemetry period | 200 ms, floor enforced by firmware |
