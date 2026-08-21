# Hardware Verification Record

**Feature**: `002-mit-impedance-control` | **Started**: 2026-08-21 | **Status**: IN PROGRESS

Required by the constitution's change gate for motion-affecting work.

## Expected wheel behaviour

| Mode | Command | Expected |
|---|---|---|
| Velocity (retained, power-up default) | `0x200 + NodeID`, `cmd 0x01` | Unchanged from before this feature: forward, reverse, strafe and rotation all as previously, on the new deterministic loop |
| Impedance, zero stiffness, non-zero damping | Matched pair | Damped velocity follower; feels viscous to the hand, resists speed not displacement |
| Impedance, non-zero stiffness | Matched pair | Restoring effort proportional to displacement, monotonic, never exceeding the 3.0 A per-motor limit |
| Impedance, zero gains, non-zero `t_ff` | Matched pair | Steady commanded torque independent of position and speed |
| Any mode, commands stop | none | Zero effort within 50 ms, still armed, automatic recovery when commands resume |
| Any mode, emergency stop `0x080` | — | Zero output and both motors disarmed |

## Validation artifact per layer

| Layer | Gate | Result |
|---|---|---|
| Firmware compile | `arduino-cli compile --fqbn esp32:esp32:esp32 .` | **PASS.** Baseline 408060 bytes flash / 25804 bytes RAM; after this work 416144 / 25988 |
| Protocol agreement | `python jetson_xavier/backend/can_frames.py` | **PASS.** All published fixtures in `contracts/can-protocol.md` reproduce byte-exactly |
| Timing derivation | Exhaustive check of all 9901 integer requests, 100-10000 Hz | **PASS.** Zero invariant failures; also available on device as `TS` |
| Backend focused run | Frame-byte assertions | Not yet run: the production backend changes (T071-T079) are not implemented |
| UI type-check | `npm run type-check` | Not yet run: the UI changes (T080-T083) are not implemented |
| On-hardware motion | quickstart S1-S9 | **PARTIAL / T034 FAIL on COM9.** Passive status PASS. Powered velocity burst did not track; see below |

## Hardware verification: PARTIAL (COM9 UART-CAN, 2026-08-22)

A UART-to-CAN adapter is attached to this machine on **COM9** (USB `VID_2E88`/`PID_4603`,
Xiaohua CDC). It speaks Waveshare USB-CAN-A 16-byte frames (`AA 11 DLC ID[4 LE] data[8] 55`)
at 2 Mbaud on the USB serial side. CAN on the wire is the firmware's 1 Mbit/s Classic CAN.

Helper used: `jetson_xavier/backend/uart_can_com9.py`.

### What this adapter can prove

Passive listen, `0x080`, and a bounded velocity burst on `0x202` (`uart_can_com9.py velocity-burst`).
Abort threshold 1.5 A. Impedance pairs were not sent: the firmware still has no `0x100`/`0x130` receiver.

| Check | Result |
|---|---|
| Bus live, one controller | **PASS.** Only node `0x02` (legacy IDs `0x182` / `0x192` / `0x702`), matching the COM6 identity `0x202` already recorded for this bench |
| Front node `0x01` | **ABSENT.** No `0x181` / `0x191` / `0x701` in a 10 s capture |
| Existing status `0x180+id` / `0x190+id` | **PASS, unchanged map.** ~124 Hz each over 10 s (2543 frames: 1264 + 1266). Contract says 100 Hz; the on-wire rate is the communications loop, not a protocol-map break |
| Heartbeat `0x700+id` | **PASS.** `0x702` DLC 1, payload `00`, ~1.28 Hz (13 frames / 10 s) |
| Bus voltage vs serial | **PASS.** `0x192` bytes 4-5 = 1973 × 0.01 V = **19.73 V**, agreeing with the serial `bus mv=19744` record from the 003 COM6 session |
| Effort / arm from status | Status 1/2 do not carry an arm bit. Idle currents 0.01–0.05 A |
| New 002 status `0x1A0`–`0x1F0` | **ABSENT.** Expected: `v13_macnum_wheel_car.ino` still transmits only the three legacy frames |
| Forbidden bandwidth probe `0x12` on `0x120+id` | **NOT RUN.** Firmware does not receive `0x120` yet |
| Emergency stop `0x080` | **TX ATTEMPTED.** Heartbeat continued; currents stayed ~0.02 A. Armed→disarmed (S3 / T063) is still unverified |
| Velocity burst T034 fragment | **FAIL on this path.** See below |
| Impedance motion | **NOT RUN.** No pair receiver in firmware |

### Powered velocity burst (2026-08-22, COM9)

Command: `0x202` cmd `0x01`, both wheels ±1.5 rad/s at 100 Hz, then explicit zeros, 400 ms silence, disarm, `0x080`. Two USB-CAN-A TX encodings were tried (16-byte `AA 11…55` matching RX, then fixed 20-byte `AA 55 01…` after a 1 Mbit/s normal-mode config). Host never saw `0x202` echoed (this adapter may not loop back TX).

| Phase | n (`0x182`) | mean vel L,R (rad/s) | peak \|i\| (A) |
|---|---:|---|---:|
| idle | 81 | 0.18, 0.09 | 0.19 |
| fwd +1.5 | 204 | 0.01, 0.01 | 0.16 |
| rev −1.5 | 156 | −0.03, 0.03 | 0.17 |
| zero / silence / estop | — | noise, same as idle | ≤0.21 |

The wheels did **not** track the command. Mean speed stayed inside idle encoder jitter. Current never left the noise floor. That is not a T034 pass. Likely causes, in order: USB-CAN-A transmit not actually reaching the bus; firmware ignoring `0x202` (calibration mode or bus-voltage protection); missing TX loopback so we cannot tell those apart from COM9 alone.

Next proof needs either serial `I`/`C` on COM6 during a burst, or a CAN analyser that shows whether `0x202` is on the wire.

### What is still blocked

Oscilloscope / logic analyser, serial `T`/`B`/`I`/`M`/`C`, torque-constant measurement, impedance, and a velocity pass that actually moves the shaft. COM9 RX works; COM9 TX to the controller is unproven.

Blocked tasks, all still requiring an attended powered bench:

| Task | What it must establish | Why it blocks others |
|---|---|---|
| T008 | Baseline per-cycle time of the pre-existing loop | Reference point for the improvement claim |
| T011 | Pipelined AS5147 read correctness at 8 MHz, and its real cost | If it fails, `AS5147_FAST_TWO_TRANSFER` is the fallback (T012, already implemented) |
| T015 | Version-1 to version-2 migration preserves calibration | A failure here forces full recalibration of both motors |
| T031 | **The measured sustainable ceiling** | Publishes the real maximum bandwidth. Everything downstream assumes 1000 Hz is reachable |
| T033 | Current-sense validity at the derived carrier | Inline sensing has no PWM synchronisation |
| T034 | Velocity mode still drives correctly on the deterministic loop | The MVP acceptance check |
| T043-T045 | Torque constant, then the impedance and saturation acceptance tests | T045 is the highest-consequence check in the feature |
| T052, T058, T063-T064, T084-T085, T089 | Remaining acceptance and regression scenarios | Release gates |

### The most important blocked number

`FOC_MAX_SUSTAINABLE_RATE_HZ_PROVISIONAL` in `foc_timing.h` is currently **10000.0 Hz**,
seeded so the 1000 Hz default is exactly reachable. It is a **provisional estimate from
research D3, not a measurement.** T031 must replace it.

If the measured ceiling is lower, the behaviour is already correct and honest: the
requested value stays 1000 Hz, the active bandwidth clamps, and both the `B` report and
the startup banner say so. The sampling multiple is **not** reduced to compensate, which
the follow-up clarification and research D3 both forbid. With the current provisional
ceiling, 9000 of the 9901 valid requests clamp; that ratio will change once measured.

## Deviations recorded during implementation

1. **T032 mechanism.** Implemented as `focTimingSelfTest()`, a runtime exhaustive check
   over every integer request plus the malformed parser cases, rather than a literal
   compile-time construct: a `static_assert` loop over 9901 values is not practical. The
   same exhaustive check was additionally run on the host during implementation with zero
   failures, so the coverage D12 asks for has been obtained.

2. **Carrier examples corrected in the design docs.** `research.md` D5 and the
   `serial-console.md` sample output quoted 500 Hz → 25 kHz (N=5) and 1000 Hz → 30 kHz
   (N=3). Both contradict the normative smallest-N rule stated in the same decision and in
   `data-model.md`, which yields 20 kHz (N=4) and 20 kHz (N=2). The normative rule was
   implemented and the illustrative examples were corrected.

3. **`driver2.pwm_frequency` was a latent defect, not a cosmetic gap.** Both drivers share
   one MCPWM timer, and `esp32_driver_mcpwm.cpp` refuses to initialise a second driver
   whose requested frequency differs from the already-configured timer. The old code set
   only `driver1` and worked purely because the unset default happened to match. At any
   derived carrier other than 25 kHz it would have failed `driver2` init.

## Constitution review

To be completed at release. Current status of each review priority:

| Priority | Status |
|---|---|
| Motion safety and estop correctness | Firmware path implemented. On 2026-08-22 a live `0x080` was sent on COM9 while the node was already at ~0 A; heartbeat continued and current did not rise. Armed-to-disarmed estop (S3, 100 trials) is still unverified. |
| Left/right and front/rear sign conventions | Untouched. The mecanum mixer and the motor-to-wheel mapping are unchanged, which is what SC-015 and T034 verify. |
| CAN payload compatibility | Velocity mode retained permanently and is the power-up default, so an unmodified sender is unaffected. New identifiers do not collide. Big-endian pair and little-endian legacy frame use separate helpers by contract. |
| Hard-coded environment values | Improved: the bandwidth is now persisted configuration rather than the hardcoded `100.0f`, and the bus-voltage window is configurable. `MOTOR_TORQUE_CONSTANT_NM_PER_A` and the torque-derived ranges remain **provisional** pending T043. |
| Vendored and generated content | No edits to `src/MPU6050`, `src/I2Cdev`, or the SimpleFOC install. The 50 µs SPI delay was bypassed by adding a first-party reader, which is the constitution-preferred direction. |
