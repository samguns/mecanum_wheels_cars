# Contract: CAN Protocol

**Feature**: `002-mit-impedance-control` | **Date**: 2026-08-20

Bus: Classic CAN, 11-bit identifiers, **1 Mbit/s** (`TWAI_TIMING_CONFIG_1MBITS()`).
`NodeID` is the low byte of the controller's configured CAN id: front = `0x01`, rear = `0x02`.

Lower identifiers win arbitration. New motion frames are deliberately placed **below** the existing status
identifiers so a motion command is never delayed by telemetry, and **above** the emergency stop so nothing
outranks a stop.

## Identifier map

| Identifier | Direction | Rate | Purpose | Status |
|---|---|---|---|---|
| `0x080` | sender → node | on demand | Emergency stop, all nodes | Existing, unchanged |
| `0x100 + NodeID` | sender → node | 200 Hz | **Motor 1 absolute-position half** | New |
| `0x110 + NodeID` | sender → node | 200 Hz | **Motor 2 absolute-position half** | New |
| `0x120 + NodeID` | sender → node | on demand | **Arm / disarm / mode selection** | New |
| `0x130 + NodeID` | sender → node | 200 Hz | **Motor 1 dynamics half** | New |
| `0x140 + NodeID` | sender → node | 200 Hz | **Motor 2 dynamics half** | New |
| `0x180 + NodeID` | node → sender | 100 Hz | Status 1: measured velocities | Existing, unchanged |
| `0x190 + NodeID` | node → sender | 100 Hz | Status 2: currents + bus voltage | Existing, unchanged |
| `0x1A0 + NodeID` | node → sender | 10 Hz | **Mode, bandwidth, carrier, fault flags** | New |
| `0x1B0 + NodeID` | node → sender | 10 Hz | **Applied stiffness and damping** | New |
| `0x1C0 + NodeID` | node → sender | 10 Hz | **Measured accumulated positions** | New |
| `0x1D0 + NodeID` | node → sender | 10 Hz | **Per-motor effort-limit causes** | New |
| `0x1E0 + NodeID` | node → sender | 10 Hz | **Motor 1 applied target / capture acknowledgement** | New |
| `0x1F0 + NodeID` | node → sender | 10 Hz | **Motor 2 applied target / capture acknowledgement** | New |
| `0x200 + NodeID` | sender → node | 200 Hz | Legacy velocity command | Existing, **retained permanently** |
| `0x700 + NodeID` | node → sender | 1 Hz | Heartbeat | Existing, unchanged |

Concrete new identifiers: `0x101`, `0x102`, `0x111`, `0x112`, `0x121`, `0x122`, `0x131`, `0x132`, `0x141`,
`0x142`, and `0x1A1` through `0x1F2` for the six status families. None collide with the existing map.

## Scalar encoding

The four dynamics fields are unsigned integers spanning their full ranges, following MIT-style scalar mapping.
The accumulated-position field is the signed `int32` milliradian value defined separately below:

```
to_uint(x, min, max, bits)   = round( (clamp(x, min, max) - min) * ((2^bits - 1) / (max - min)) )
from_uint(u, min, max, bits) = u * ((max - min) / (2^bits - 1)) + min
```

### Field ranges

Scaled to this drive (12 V bus, 3.0 A per-motor limit, 50 rad/s velocity limit), **not** to a Mini Cheetah leg
actuator. Torque ranges are **provisional** until the torque constant is measured on the bench; they are named
constants shared by firmware and backend so a revision is a one-line change in each.

| Field | Bits | Min | Max | Resolution |
|---|---|---|---|---|
| `p_des` (mrad) | 32 signed | −2,147,483,648 | +2,147,483,647 | 0.001 rad |
| `v_des` (rad/s) | 12 | −45.0 | +45.0 | 0.022 rad/s |
| `kp` (N·m/rad) | 12 | 0.0 | 50.0 | 0.012 |
| `kd` (N·m·s/rad) | 12 | 0.0 | 1.0 | 0.00024 |
| `t_ff` (N·m) | 12 | −0.5 | +0.5 | 0.00024 |

`p_des` is an **absolute accumulated shaft angle since power-up** (FR-001a), not a wrapped angle. Signed 32-bit
milliradians cover about ±341,000 revolutions at 0.001 rad resolution. The sender obtains the controller's measured
accumulated position from `0x1C0 + NodeID` before establishing a hold target.

## Atomic impedance command pair

One logical impedance command is two DLC-8 frames with the same sequence number. Numeric fields are big-endian;
the retained legacy velocity frame remains little-endian. The two formats MUST use separate helpers.

### Absolute-position half

`0x100 + NodeID` (motor 1) and `0x110 + NodeID` (motor 2).

| Byte | Content |
|---|---|
| 0-3 | `p_des_mrad`, signed `int32`, big-endian two's complement |
| 4 | Sequence counter, wraps at 255 |
| 5-7 | Reserved, zero |

### Dynamics half

`0x130 + NodeID` (motor 1) and `0x140 + NodeID` (motor 2). The four 12-bit unsigned values use the scalar
mapping above and occupy exactly six bytes.

| Byte | Content |
|---|---|
| 0 | `v_des[11:4]` |
| 1 | `v_des[3:0]` (high nibble), `kp[11:8]` (low nibble) |
| 2 | `kp[7:0]` |
| 3 | `kd[11:4]` |
| 4 | `kd[3:0]` (high nibble), `t_ff[11:8]` (low nibble) |
| 5 | `t_ff[7:0]` |
| 6 | Sequence counter, wraps at 255 |
| 7 | Flags: bit 0 capture current position in the applying control cycle; bits 1-7 reserved, zero |

**Receiver rules**:
- Each motor owns an independent staging record for the most recent position and dynamics halves.
- A logical command is accepted only when both DLCs are 8, reserved bytes are zero, all scalar values are valid,
  both sequence numbers match, and the addressed motor is in impedance mode and outside calibration mode.
- The complete five-term state is copied atomically only after validation of the matched pair. Neither half may
  partially update active state or reset the command timeout.
- If the capture flag is set, the applying control cycle replaces transmitted `p_des_mrad` with its same-cycle
  measured accumulated position, increments the motor's capture generation, and exposes the result through its
  applied-target status frame.
- A newer sequence discards any incomplete older pair. A pair not completed within 5 ms is discarded and reported.
- A complete pair resets only that motor's command timeout and is applied without waiting for the other motor.

### Shared wire fixtures

Both layers MUST reproduce these exact bytes. `can_protocol.h` and
`jetson_xavier/backend/can_frames.py` are checked against them; run
`python jetson_xavier/backend/can_frames.py` to verify.

Position half (`p_des_mrad`, sequence):

| Case | `p_des_mrad` | Seq | Wire bytes (hex) |
|---|---|---|---|
| Zero | 0 | 0 | `0000000000000000` |
| +1 rad | 1000 | 1 | `000003e801000000` |
| −1 rad | −1000 | 2 | `fffffc1802000000` |
| 100 revolutions | 628319 | 42 | `0009965f2a000000` |
| −100 revolutions | −628319 | 43 | `fff669a12b000000` |
| `int32` max | 2147483647 | 254 | `7ffffffffe000000` |
| `int32` min | −2147483648 | 255 | `80000000ff000000` |

100 revolutions is `200π` rad, which rounds to **628319 mrad**. This is the SC-013 anchor and the
reason the field is 32-bit: it is unrepresentable in a 16-bit ±12.5 rad field.

Dynamics half:

| Case | `v_des` | `kp` | `kd` | `t_ff` | Seq | Capture | Wire bytes (hex) |
|---|---|---|---|---|---|---|---|
| All minimum | −45.0 | 0.0 | 0.0 | −0.5 | 0 | no | `0000000000000000` |
| All maximum | +45.0 | 50.0 | 1.0 | +0.5 | 1 | no | `ffffffffffff0100` |
| Capture flag set | 0.0 | 0.0 | 0.0 | 0.0 | 9 | yes | byte 7 = `0x01` |

Rejection cases, all of which MUST leave active state untouched:

| Case | Expected |
|---|---|
| Position half with DLC ≠ 8 | Rejected |
| Position half with any of bytes 5-7 non-zero | Rejected |
| Dynamics half with DLC ≠ 8 | Rejected |
| Dynamics half with any of flag bits 1-7 set | Rejected |
| Sequence 200 paired with 201 | Not a match |
| Sequence 255 paired with 0 | Not a match (no wrap tolerance) |

## Arm / disarm / mode frame

`0x120 + NodeID`. DLC 8.

| Byte | Content |
|---|---|
| 0 | Command: `0x10` = arm/disarm, `0x11` = set motion mode; `0x12` is reserved as a forbidden bandwidth-write probe for SC-009a |
| 1 | Flags: bit 0 enable, bit 1 brake, bit 2 clear fault, bit 3 emergency stop |
| 2 | Motor selector: `0x00` both, `0x01` motor 1, `0x02` motor 2 |
| 3 | Motion mode: `0x00` velocity, `0x01` impedance (used only when byte 0 is `0x11`) |
| 4 | Sequence counter, wraps at 255 |
| 5-7 | Reserved, zero |

**Receiver rules**:
- Emergency-stop flag takes precedence over everything in the frame and over any pending motion command.
- A mode change is refused while the addressed motor is armed, and the refusal is reported (FR-006b).
- An accepted mode change zeroes that motor's targets and gains.
- Arm and disarm semantics are unchanged from today: a disarm leaves the bridge unpowered with targets zeroed.
- Any `0x12` probe or other unknown command is rejected and reported without changing requested/active bandwidth
  or persisted configuration. No valid CAN command can write bandwidth (FR-021a, SC-009a).

## Configuration and fault status frame

`0x1A0 + NodeID`. DLC 8. Sent at 10 Hz.

| Byte | Content |
|---|---|
| 0 | bits 0-1 motor 1 mode, bits 2-3 motor 2 mode, bit 4 motor 1 armed, bit 5 motor 2 armed, bit 6 motor 1 timed out, bit 7 motor 2 timed out |
| 1 | bit 0 overrun fault, bit 1 bandwidth clamped, bit 2 calibration required, bit 3 timing-source fault, bit 4 motor 1 pair fault, bit 5 motor 2 pair fault, bits 6-7 reserved |
| 2-3 | Active bandwidth in Hz, `uint16` little-endian |
| 4-5 | Requested bandwidth in Hz, `uint16` little-endian |
| 6-7 | Active switching carrier in Hz ÷ 10, `uint16` little-endian |

Carrier is divided by ten so the 50 kHz ceiling fits a `uint16`. Bit 1 of byte 1 is the operator-visible signal
that a request was reduced (FR-021c).

## Applied gains status frame

`0x1B0 + NodeID`. DLC 8. Sent at 10 Hz. All values `uint16` little-endian.

| Byte | Content | Scale |
|---|---|---|
| 0-1 | Motor 1 stiffness | × 100 |
| 2-3 | Motor 1 damping | × 10000 |
| 4-5 | Motor 2 stiffness | × 100 |
| 6-7 | Motor 2 damping | × 10000 |

These are the gains the controller is **actually applying**, which is what lets the UI show a mismatch when a
requested value was refused (FR-040).

## Accumulated-position status frame

`0x1C0 + NodeID`. DLC 8. Sent at 10 Hz. Values are signed `int32` milliradians, big-endian two's complement.

| Byte | Content |
|---|---|
| 0-3 | Motor 1 measured absolute accumulated position since power-up |
| 4-7 | Motor 2 measured absolute accumulated position since power-up |

This is the authoritative reference used by a sender to establish an absolute hold target (FR-001c).

## Effort-limit status frame

`0x1D0 + NodeID`. DLC 8. Sent at 10 Hz. A cause remains latched until it has appeared in at least one transmitted
status frame, so a short limiting event is observable without writing from the periodic control path.

| Byte | Content |
|---|---|
| 0 | Motor 1 bits: bit 0 current limited, bit 1 output-voltage limited, bit 2 bus-voltage protection active |
| 1 | Motor 2 bits: bit 0 current limited, bit 1 output-voltage limited, bit 2 bus-voltage protection active |
| 2-3 | Motor 1 limit-event count, `uint16` little-endian, saturating |
| 4-5 | Motor 2 limit-event count, `uint16` little-endian, saturating |
| 6-7 | Reserved, zero |

The control task updates only the in-memory cause bits and counters. Communications code owns CAN reporting
(FR-004a, FR-032a, FR-035).

## Applied-target and capture-acknowledgement frames

`0x1E0 + NodeID` reports motor 1 and `0x1F0 + NodeID` reports motor 2. Each is DLC 8 at 10 Hz.

| Byte | Content |
|---|---|
| 0-3 | Applied absolute target, signed `int32` milliradians, big-endian |
| 4 | Last applied matched-pair sequence |
| 5 | Capture generation, incremented only when capture-current-position is applied |
| 6 | bit 0 capture has been applied; bit 1 target is active; bits 2-7 reserved |
| 7 | Reserved, zero |

The backend clears a capture-pending state only after observing a post-request generation change for the expected
motor. Until then it continues sending capture requests and does not infer a target from `0x1C0`.

## Retained legacy velocity frame

`0x200 + NodeID`, DLC 8, unchanged and permanently supported (FR-006):

| Byte | Content |
|---|---|
| 0 | Command: `0x01` set both velocity targets, `0x02` motor 1 arm, `0x03` motor 2 arm, `0x04` both arm |
| 1 | Flags: bit 0 enable, bit 1 brake, bit 2 clear fault, bit 3 emergency stop |
| 2 | Mode byte, `0x00` = rad/s |
| 3 | Sequence |
| 4-5 | Left target (motor 2), `int16` little-endian, × 100 |
| 6-7 | Right target (motor 1), `int16` little-endian, × 100 |

Newly enforced: a `0x01` velocity command addressed to a motor whose active mode is impedance is **rejected and
reported** rather than applied (FR-006a). Arm and disarm commands on this frame keep working in both modes.

## Motor-to-wheel mapping

Unchanged from the existing firmware and backend. Motor 1 is the **right** wheel of its node, motor 2 is the
**left** wheel. Node `0x01` is the front pair, node `0x02` the rear pair. The mecanum mixer's per-wheel sign
convention is unchanged, which is what SC-015 verifies.

## Bus budget

| Traffic | Frames/s | Utilisation at 1 Mbit/s |
|---|---|---|
| Impedance command pairs, 8 frames per update at 200 Hz | 1600 | ~20.8% |
| Status 1 and 2, 2 nodes at 100 Hz | 400 | ~5.2% |
| New status `0x1A0`-`0x1F0`, 2 nodes at 10 Hz | 120 | ~1.6% |
| Heartbeat, 2 nodes at 1 Hz | 2 | negligible |
| **Total** | **~2122** | **~27.6%** |

Assumes ~130 bits per standard 8-byte frame including stuffing. The margin to the ~50% point where CAN latency
becomes unpredictable is large.

## Timing contract

| Parameter | Value | Requirement |
|---|---|---|
| Motion command rate, per motor | 200 Hz nominal | FR-038 |
| Command timeout, per motor | 50 ms | FR-029 |
| Consecutive lost frames tolerated | 9 | SC-004a |
| Status frame rate | 100 Hz existing, 10 Hz new | — |

On timeout a motor produces zero effort but stays armed, and recovers automatically when commands resume
(FR-029b).
