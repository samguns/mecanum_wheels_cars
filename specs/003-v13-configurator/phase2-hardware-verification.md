# Phase 2 firmware and safety-gate verification

Date: 2026-08-22  
Controller: ESP32-PICO-D4 on COM6, 115200 8N1  
Persisted identity: `0x202`

## Build and host-side gates

- `arduino-cli compile --fqbn esp32:esp32:esp32 .`: PASS
  - flash: 421404 / 1310720 bytes (32%)
  - RAM: 26092 / 327680 bytes (7%)
- `cargo test --manifest-path src-tauri/Cargo.toml`: PASS, 24 tests
- `npm run type-check`: PASS
- Upload to COM6: PASS; flash hashes verified by esptool.

## T026 record contract comparison

Every contracted record type was observed from the flashed controller and compared with
`contracts/serial-protocol.md`. The Rust fixture/parser test suite independently checks the same key sets,
types, percent decoding, absent optionals and malformed-input behavior.

| Type | Observed source | Required fields checked | Result |
|---|---|---|---|
| `id` | boot / `Q` / `N` | `fw proto canid motors cfgver uptime_ms` | PASS |
| `cal` | `Q` | `m aligned charac pp dir offset r ld lq valid` | PASS, both motors |
| `cfg` | `Q` | `canid bw_req bw_act bw_clamped rate carrier decim mode1 mode2 busmin_mv busmax_mv calibrated` | PASS |
| `motor` | `Q` / `QI` | `m armed mode pos_mrad vel iq timeout limits limitcount pairfault` | PASS, `pairfault=nan` as required |
| `timing` | `Q` | `rate_nom rate_meas period_us cycles overruns consec last_us worst_us duty fault` | PASS |
| `bus` | `Q` | `mv protect` | PASS |
| `ack` | abort and tagged `N` | `tag cmd ok reason` | PASS, including empty and percent-encoded reasons |
| `calprog` | alignment and characterization | `m stage pct energised` | PASS, both stages |
| `calpend` | completed, then rejected stages | alignment `m stage pp dir offset`; characterization `m stage r ld lq` | PASS |
| `fault` | bus, protocol and calibration paths | `kind reason cooldown_ms` | PASS |

Identity command checks: `N` reported `0x202`; tagged `N000` was refused with the documented range and did
not change storage; tagged `N202` persisted before returning `ok=1` and emitted a confirming `id` record.

## T027 hardware safety gate

S6a results:

| Stage | Motor | Trials | Energised progress seen | Abort ack | Calibration fault | Both motors disarmed after |
|---|---:|---:|---:|---:|---:|---:|
| alignment | 1 | 10 | 10/10 | 10/10 | 10/10 | 10/10 |
| alignment | 2 | 10 | 10/10 | 10/10 | 10/10 | 10/10 |
| characterization | 1 | 10 | 10/10 | 10/10 | 10/10 | 10/10 |
| characterization | 2 | 10 | 10/10 | 10/10 | 10/10 | 10/10 |

S6b results: 10/10 idle trials acknowledged raw `0x18`, discarded a deliberately half-typed `BROKEN`
command, accepted the following `Q`, and reported both motors disarmed. A final post-flash powered smoke test
also produced `calprog energised=1`, `fault reason=serial%20abort`, `ack cmd=ABORT ok=1`, a failed-stage
acknowledgement, and two `armed=0` motor records.

The measured bus was approximately 19.74 V. The controller initially held the superseded provisional 9-14 V
window, correctly surfaced a `kind=bus` fault and disarmed. Follow-up on 2026-08-22 changed the provisional
window to 7-24 V and added a narrow persisted-config migration for the exact old provisional pair. After flashing
COM6, `cfg` reported `busmin_mv=7000 busmax_mv=24000` and `bus` reported `mv=19744 protect=0`. Operator-defined
windows other than the exact old provisional pair are not migrated.
