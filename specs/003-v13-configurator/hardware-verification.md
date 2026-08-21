# Hardware verification: V13 Configurator

**Feature**: `003-v13-configurator`  
**Date**: 2026-08-22  
**Controller**: ESP32-PICO-D4 on COM6, 115200 8N1, CAN id `0x202`  
**Bus**: ~19.73 V, window 7000–24000 mV, `protect=0`

This note is the constitution change-gate record for the desktop configurator.

## Constitution review (T115)

| Constraint | Status |
|---|---|
| Motion safety and both stop paths | Firmware `0x18` still disarms. Idle abort on this flash: `ack cmd=ABORT ok=1`. Mid-alignment abort left both motors `armed=0`. App Stop is wired to the same byte; the Vue click path is not yet a recorded trial. |
| Wheel identity | Derived from CAN id + motor index. Rust tests cover the mapping. Live `0x202` labels 10/10 (M1 Rear Right, M2 Rear Left). SC-007 still needs a person looking at the wheels (T063). |
| Serial protocol contract | `#V13` records + tagged commands. `Q` acks. `V` and `M` registered. |
| Hard-coded values | Bus board range 7000–24000 mV. QP floor 50 ms. Intent TTL 30 s. Staleness 2 s. |
| Approved amendments | T001: constitution **1.1.0** (configurator). **1.2.0** (2026-08-22) adds Principle VI (Rust must be written as Rust). |
| Vendored content | No edits under `src/MPU6050/`, `src/I2Cdev/`, vendored SimpleFOC, or `jetson_xavier/`. |

## Flash

Two uploads to COM6 on 2026-08-22. Final compiled size after the `V` parser fix: **423620** flash / **26100** RAM. esptool hashes verified. Chip ESP32-PICO-D4 rev v1.1, MAC `14:08:08:5d:ea:0c`.

The first `V<min>,<max>` implementation restored the comma before checking the min end-pointer, so every write looked out of range. Fixed: treat `end_min == comma` as a complete min token.

## Protocol session (same tagged commands the configurator sends)

DTR/RTS deasserted on open, matching `session/port.rs`.

### S2 — connect without reboot

| Sample | `uptime_ms` |
|---|---:|
| First `Q` after settle | 146608 |
| Reopen + `Q` | 158635 |
| Delta | +12027 ms |

`reset_on_reconnect=false`. M1 PASS at the serial-open layer. The Tauri window itself was not launched (T055 GUI row still open).

### S3 — read vs console

`Q` snapshot matched `B` (1000 Hz requested/active, 10 kHz / 20 kHz / decim 2) and the `cal` records. `C` alone (no motor selected) correctly refused with `select motor with C1 or C2`. Both motors reported valid stored calibration before the powered run.

### S8 — writable settings (disarmed)

| Command | Result |
|---|---|
| `V20000,8000` | `ok=0` reversed window, stored pair unchanged |
| `V8000,20000` | `ok=1`, `cfg busmin_mv=8000 busmax_mv=20000` |
| `V7000,24000` | `ok=1`, restored board defaults |
| `M` / `M1V` | velocity/velocity, `ok=1` |
| `N202` | `ok=1`, identity unchanged |
| Idle `0x18` then `Q` | abort ack, then `Q` ack |

Identity persistence of a *changed* CAN id is now recorded in the continue pass (T101). Identity was left at `0x202`.

Arming `A1` on this flash immediately produced a timing-fault fail-closed (`overruns=16`) and disarmed. That is why a subsequent `B1000` was accepted: the motor was no longer armed.

### S5 — guided calibration, powered (operator confirmed wheels clear)

Tagged `C1`/`C2`, `CA`, `CN`, `CY`, `CM`. Not the Vue dialog (T084 GUI row still open).

| Motor | Stage | Reject path | Accept path | Other motor armed |
|---|---|---|---|---|
| 1 (Rear Right) | align | `calpend` then `CN`; stored unchanged | `CY` stored `pp=7 dir=1 offset=5.668059` | no |
| 1 | charac | `calpend` then `CN` | `CY` stored `r=2.654628 ld=703µH lq=864µH valid=1` | no |
| 2 (Rear Left) | align | `calpend` then `CN`; stored unchanged | `CY` stored `pp=6 dir=-1 offset=2.206632` | no |
| 2 | charac | `calpend` then `CN` | `CY` stored `r=2.551296 ld=653µH lq=816µH valid=1` | no |

Energised `calprog` records were seen on every started stage. After each accept, `Q` showed the pending values as stored. Both motors `armed=0` at the end. `cfg calibrated=1`.

**Stored values changed.** Motor 2 pole pairs were previously 7 and are now **6** because that alignment was accepted. Motor 1 resistance moved from ~2.09 Ω to ~2.65 Ω.

### S7 — interruption (smoke, not 20 trials)

| Event | Result |
|---|---|
| Idle `0x18` | `ack cmd=ABORT ok=1`; following `Q` succeeded |
| `0x18` ~1.5 s into M1 alignment | both motors `armed=0`; stage failed rather than stored |
| `CN` on pending align/charac (4 times) | pending discarded; previous stored values unchanged |

T084's 20 mid-alignment `0x18` aborts are recorded in the continue pass. The Vue confirm dialog was not clicked.

## Layer artifacts

| Layer | Verification |
|---|---|
| Firmware compile / flash | Completed (423620 / 26100); hashes verified |
| `V` / `M` / `Q` ack on hardware | Completed |
| S2/S3 at the serial-open layer | Completed |
| S5 two-motor cal over tagged commands | Completed; Vue UI not used |
| S7 20-trial / S8 power-cycle identity | Completed at the tagged-command layer; Vue UI not used |
| Tauri GUI (T072, T084 Vue path, T107 changelog UI, T108) | Deferred |
| S12 operator timings | S12b done; S12a/S12c still need people |

## Research gates M1–M5

| Gate | Observed this session |
|---|---|
| M1 | Reopen with DTR/RTS false did not reset (uptime continued) |
| M2 | Tagged `Q`/`V`/`N` acks inside 2 s; calibration stages finished inside 70 s |
| M3 | Idle abort + one in-stage abort; T027 10/10 still the formal count |
| M4 | New accepted measurements differ from the previous stored pair (M2 pp 7→6). Treat as a new characterisation, not as proof of zero perturbation |
| M5 | `QP200` then `T`: measured 10000.0 Hz, overruns 0. Separate from the earlier `A1` timing-fault trip |

## Continue pass (same day, later)

`cargo test` after Principle VI cleanup: **48 passed** (added a 2 s staleness unit test and a 10-trial wheel-label matrix).

Arduino Serial Monitor was holding COM6 (`PermissionError`). After it was closed, the continue script finished T071 / T084 / T092 / T101.

| Item | Result |
|---|---|
| T071 refusals | 20/20 `ok=0` with a stated reason (`B50`/`B99999`, reversed/OOR `V`, `N000`/`N800`, `M9V`, bare `C`/`CY`) |
| T084 S7 | 20/20 mid-alignment `0x18` aborts; both motors `armed=0`; stored cal offsets unchanged |
| T092 S9 | `QP200` then `T`: 10000.0 Hz, overruns 0, consecutive 0, timing fault none |
| T056 staleness | `QP200` then `QP0`: 3.56 s gap, no unsolicited motor/bus after stop |
| T101 S8 identity | `N201` + `V7500,23000` persisted across DTR reset (boot `CAN ID: 0x201`). Restored `N202` and `V7000,24000` |
| T113 | `B`/`V`/`C` prose still printed; `#V13` stayed on its own lines; `T` during stream showed 0 overruns |

First T101 post-reset `Q` was too early (empty `id`). A second identify after ~5 s confirmed `0x201` on boot, then restore.

Final live `Q` after restore: `canid=0x202`, window 7000/24000, both motors `armed=0`, `calibrated=1`, uptime 32194 ms (board had reset during T101, as intended).

COM9 (USB-CAN-A) is now present and was used as the non-controller device.

## COM9 + S10 continue (same day)

`connect` now sends a tagged `Q` during identify. Firmware emits an unsolicited `id` only at boot; `QI` is motors, not identity.

| Item | Result |
|---|---|
| Port list | COM6 CH340 (controller), COM9 USB serial (USB-CAN-A) |
| T056 non-controller | COM9 + identify `Q`: 27536 bytes, **no** `#V13`, no `id` → unrecognised, no session |
| T107 S10 | 10/10 writes acked. Changelog 10 entries: 9× `bench-s10`, last `anonymous` after clearing the profile. Each has can_id `0x202`, wheel label, before/after cfg |
| Motion controls | Vue has no `arm`/`disarm` invoke and no velocity/torque setpoint (vitest) |
| Restore | `canid=0x202`, window 7000/24000, `bw_req=1000`, both modes velocity. Uptime 979600→1028860 (no reset) |

History view now shows before/after so those ten rows are retrievable from the app log, not only from the JSONL file.

## T056 unplug (same day)

Live session on the CH340 after it re-enumerated as COM7 (`canid=0x202`, `QP200`). Operator unplugged that USB cable.

| Check | Result |
|---|---|
| Link | `GetOverlappedResult` access denied, then COM7 gone |
| Connection | `lost` |
| Last values | `id`/`cfg` retained (`0x202`, 7000–24000 mV, 1000 Hz) |
| Actions | `can_act=false`, reopen refused (`FileNotFoundError`) |
| Ports left | COM9 only (USB-CAN-A) |

App contract matches: `mark_lost` sets `Lost`, marks the mirror stale, and `canAct` is false so Re-read and writes stay disabled while last values remain visible.

## T063 SC-007 (same day, powered)

Operator at the vehicle. Each trial: one motor, alignment rotate ≥10 s, abort, no accept. Label was not announced before the answer.

| Trial | Motor | Operator named | Result |
|---:|---|---|---|
| 1 | 1 | rear right | pass |
| 2 | 2 | rear left | pass |
| 3 | 2 | read left (rear left) | pass |
| 4 | 1 | rear right | pass |
| 5 | 2 | rear left | pass |
| 6 | 1 | rear right | pass |
| 7 | 1 | rear right | pass |
| 8 | 2 | rear left | pass |
| 9 | 1 | rear right | pass |
| 10 | 2 | rear left | pass |

10/10. Live identity `0x202`. After the last nudge, `Q` still showed stored cal M1 offset 5.668059 / M2 2.206632, both `armed=0`.

## T072 / S12a (same day, configurator window)

Operator not told where to look. Sticky chrome now shows energised plus M1/M2 arm. `calibrate_start` sends `C{n}` then `CA`. Five mid-alignment looks, then Stop, no accept.

| Trial | Selected | Energised | Arm said | Arm expected |
|---:|---|---|---|---|
| 1 | rear right | yes | armed | disarmed |
| 2 | rear left | yes | armed | disarmed |
| 3 | rear right | yes | armed | disarmed |
| 4 | rear left | yes | armed | disarmed |
| 5 | rear right | yes | armed | disarmed |

SC-013 **not met**. Energised indication and wheel selection were immediate and correct. Arm state was read as the moving/energised condition every time. Alignment calls `motor.enable()` after `disarmAllMotors()`, so `t=motor armed=0` while `calprog energised=1`.

Vue Stop was used on these five runs (T084 click path still short of 20).

Retry (same window, after the arm/energised distinction was stated but not pointed):

| Trial | Selected | Energised | Arm said | Result |
|---:|---|---|---|---|
| 1 | rear right | yes | disarmed | pass |
| 2 | rear left | yes | disarmed | pass |
| 3 | rear right | yes | armed | fail |
| 4 | rear left | yes | disarmed | pass |
| 5 | rear right | yes | disarmed | pass |

Operator later confirmed trial 3 was left **armed on purpose**, so that `armed` reading matched the label. SC-013 **5/5**. T072 cleared.

## Still open

T084 Vue confirm dialog (20 interruptions), T108 (non-Windows platforms), T110 (three operators + replacement-unit timing).
