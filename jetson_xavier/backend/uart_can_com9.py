"""COM9 Waveshare UART-CAN helper and 002 hardware-verification listen pass.

The adapter (USB VID 2E88 PID 4603) speaks the USB-CAN-A framing:
AA | 0x11 | DLC | ID little-endian 4 bytes | data[DLC] | 55
USB serial baud is 2 000 000. CAN on the wire is 1 Mbit/s.

Listen and estop stay passive. `velocity-burst` is an attended powered test: it arms
the pair through the retained 0x200+NodeID frame, streams a low speed, then zeroes,
silences (timeout), disarms, and sends 0x080. Abort immediately if current exceeds
CURRENT_ABORT_A.
"""
from __future__ import annotations

import argparse
import collections
import os
import struct
import sys
import time
from dataclasses import dataclass

import serial

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import can_frames as cf

LEGACY_COMMAND_ID = 0x202  # rear node, matches bench identity 0x202
CURRENT_ABORT_A = 1.5
DRIVE_RAD_S = 1.5

DEFAULT_PORT = "COM9"
SERIAL_BAUD = 2_000_000


@dataclass(frozen=True)
class CanFrame:
    arbitration_id: int
    data: bytes
    timestamp: float


def encode_frame(arbitration_id: int, data: bytes) -> bytes:
    if arbitration_id > 0x7FF:
        raise ValueError("extended IDs are not used by this feature")
    if len(data) > 8:
        raise ValueError("DLC > 8")
    payload = data + bytes(8 - len(data))
    return (
        bytes([0xAA, 0x11, len(data)])
        + arbitration_id.to_bytes(4, "little")
        + payload
        + bytes([0x55])
    )


def parse_frames(buf: bytearray) -> list[CanFrame]:
    """USB-CAN-A on this dongle always emits 16-byte packets, DLC unused bytes ignored."""
    frames: list[CanFrame] = []
    now = time.time()
    packet = 16
    while True:
        start = buf.find(b"\xaa")
        if start < 0:
            buf.clear()
            break
        if start:
            del buf[:start]
        if len(buf) < packet:
            break
        if buf[1] != 0x11 or buf[2] > 8 or buf[15] != 0x55:
            del buf[0]
            continue
        dlc = buf[2]
        arb = int.from_bytes(buf[3:7], "little") & 0x7FF
        frames.append(CanFrame(arb, bytes(buf[7 : 7 + dlc]), now))
        del buf[:packet]
    return frames


class WaveshareBus:
    def __init__(self, port: str = DEFAULT_PORT) -> None:
        self.ser = serial.Serial(
            port,
            SERIAL_BAUD,
            timeout=0.05,
            write_timeout=0.5,
            dsrdtr=False,
            rtscts=False,
        )
        self.ser.dtr = False
        self.ser.rts = False
        time.sleep(0.05)
        self.ser.reset_input_buffer()
        self._buf = bytearray()

    def configure_normal_1mbit(self) -> None:
        """Fixed 20-byte USB-CAN-A settings: 1 Mbit/s, standard, normal (not silent)."""
        cmd = bytearray([0xAA, 0x55, 0x12, 0x01, 0x01])
        cmd.extend([0] * 8)
        cmd.append(0x00)
        cmd.extend([0x01, 0, 0, 0, 0])
        cmd.append(sum(cmd[2:19]) & 0xFF)
        self.ser.write(bytes(cmd))
        self.ser.flush()
        time.sleep(0.05)
        self.ser.reset_input_buffer()
        self._buf.clear()

    def close(self) -> None:
        self.ser.close()

    def recv(self, timeout_s: float) -> list[CanFrame]:
        deadline = time.time() + timeout_s
        out: list[CanFrame] = []
        while time.time() < deadline:
            chunk = self.ser.read(4096)
            if chunk:
                self._buf.extend(chunk)
                out.extend(parse_frames(self._buf))
            elif out:
                break
        return out

    def send(self, arbitration_id: int, data: bytes) -> None:
        payload = data + bytes(8 - len(data))
        frame = bytearray(20)
        frame[0], frame[1], frame[2] = 0xAA, 0x55, 0x01
        frame[3], frame[4] = 0x01, 0x01
        frame[5] = (arbitration_id >> 24) & 0xFF
        frame[6] = (arbitration_id >> 16) & 0xFF
        frame[7] = (arbitration_id >> 8) & 0xFF
        frame[8] = arbitration_id & 0xFF
        frame[9] = len(data)
        frame[10:18] = payload
        frame[18] = 0x00
        frame[19] = sum(frame[2:19]) & 0xFF
        self.ser.write(bytes(frame))
        self.ser.flush()


def decode_status(frames: list[CanFrame]) -> dict:
    counts: collections.Counter[int] = collections.Counter()
    last: dict[int, CanFrame] = {}
    t0 = frames[0].timestamp if frames else 0.0
    t1 = frames[-1].timestamp if frames else 0.0
    for fr in frames:
        counts[fr.arbitration_id] += 1
        last[fr.arbitration_id] = fr
    duration = max(t1 - t0, 1e-6)
    rates = {arb: n / duration for arb, n in sorted(counts.items())}
    decoded = {}
    for arb, fr in last.items():
        d = fr.data
        if (arb & 0xFF0) == 0x180 and len(d) >= 4:
            left, right = struct.unpack_from("<hh", d, 0)
            decoded[arb] = {
                "kind": "velocity",
                "left_x100": left,
                "right_x100": right,
                "rad_s": (left / 100.0, right / 100.0),
            }
        elif (arb & 0xFF0) == 0x190 and len(d) >= 6:
            i_left, i_right, bus = struct.unpack_from("<hhh", d, 0)
            decoded[arb] = {
                "kind": "current_bus",
                "i_left_x100": i_left,
                "i_right_x100": i_right,
                "bus_x100": bus,
                "amps": (i_left / 100.0, i_right / 100.0),
                "bus_v": bus / 100.0,
            }
        elif (arb & 0xF00) == 0x700:
            decoded[arb] = {"kind": "heartbeat", "data": d.hex()}
        elif (arb & 0xFF0) in (0x1A0, 0x1B0, 0x1C0, 0x1D0, 0x1E0, 0x1F0):
            decoded[arb] = {"kind": "002_status", "data": d.hex()}
        else:
            decoded[arb] = {"kind": "other", "data": d.hex()}
    return {
        "n": len(frames),
        "duration_s": duration,
        "counts": dict(sorted(counts.items())),
        "rates_hz": {f"0x{k:03X}": round(v, 2) for k, v in rates.items()},
        "last": {f"0x{k:03X}": v for k, v in decoded.items()},
        "ids": [f"0x{k:03X}" for k in sorted(counts)],
    }


def cmd_listen(args: argparse.Namespace) -> int:
    bus = WaveshareBus(args.port)
    try:
        t_end = time.time() + args.seconds
        collected: list[CanFrame] = []
        while time.time() < t_end:
            collected.extend(bus.recv(0.2))
        report = decode_status(collected)
        print(f"captured {report['n']} frames in {args.seconds:.1f}s window")
        print("ids", report["ids"])
        print("counts", report["counts"])
        print("rates_hz", report["rates_hz"])
        print("last", report["last"])
        expected_legacy = {"0x182", "0x192", "0x702"}
        seen = set(report["ids"])
        print("legacy_status_heartbeat", expected_legacy <= seen)
        new_002 = [i for i in report["ids"] if i.startswith("0x1") and i not in ("0x182", "0x192")]
        print("new_002_status_ids", new_002)
        return 0
    finally:
        bus.close()


def cmd_estop(args: argparse.Namespace) -> int:
    bus = WaveshareBus(args.port)
    try:
        before = []
        t_end = time.time() + 1.0
        while time.time() < t_end:
            before.extend(bus.recv(0.2))
        print("pre_estop", decode_status(before)["last"])
        bus.send(0x080, bytes(8))
        print("sent 0x080 emergency stop")
        after = []
        t_end = time.time() + 2.0
        while time.time() < t_end:
            after.extend(bus.recv(0.2))
        print("post_estop", decode_status(after)["last"])
        return 0
    finally:
        bus.close()


def _mean(vals: list[float]) -> float:
    return sum(vals) / len(vals) if vals else 0.0


def _send_velocity(bus: WaveshareBus, left: float, right: float, enable: bool, seq: int) -> None:
    bus.send(LEGACY_COMMAND_ID, cf.pack_legacy_velocity_frame(left, right, enable, seq))


def cmd_velocity_burst(args: argparse.Namespace) -> int:
    """Short attended velocity smoke test on the rear pair (T034 / S8 fragment)."""
    bus = WaveshareBus(args.port)
    bus.configure_normal_1mbit()
    seq = 0
    log: list[tuple[str, dict]] = []
    tx_echo: collections.Counter[str] = collections.Counter()
    abort_reason = None

    def pump(seconds: float, left: float | None, right: float | None, enable: bool, phase: str) -> None:
        nonlocal seq, abort_reason
        period = 1.0 / args.rate
        t_end = time.perf_counter() + seconds
        next_tx = time.perf_counter()
        while time.perf_counter() < t_end:
            now = time.perf_counter()
            if left is not None and now >= next_tx:
                _send_velocity(bus, left, right, enable, seq)
                seq = (seq + 1) & 0xFF
                next_tx += period
                if next_tx < now:
                    next_tx = now
            waiting = bus.ser.in_waiting
            frames = bus.recv(0.005) if waiting or True else []
            for fr in frames:
                tx_echo[f"{phase}:0x{fr.arbitration_id:03X}"] += 1
                sample = None
                if fr.arbitration_id == 0x182 and len(fr.data) >= 4:
                    left_v, right_v = struct.unpack_from("<hh", fr.data, 0)
                    sample = {"vel": (left_v / 100.0, right_v / 100.0), "cur": None}
                elif fr.arbitration_id == 0x192 and len(fr.data) >= 6:
                    i_left, i_right, bus_v = struct.unpack_from("<hhh", fr.data, 0)
                    sample = {"vel": None, "cur": (i_left / 100.0, i_right / 100.0, bus_v / 100.0)}
                if sample:
                    log.append((phase, sample))
                    if sample["cur"]:
                        peak = max(abs(sample["cur"][0]), abs(sample["cur"][1]))
                        if peak > CURRENT_ABORT_A:
                            abort_reason = f"{phase} current {peak:.2f} A"
                            return

    try:
        print(
            f"velocity-burst id=0x{LEGACY_COMMAND_ID:03X} rate={args.rate:g} Hz "
            f"drive={DRIVE_RAD_S} rad/s abort>{CURRENT_ABORT_A} A"
        )
        pump(0.8, None, None, False, "idle")
        if abort_reason:
            raise RuntimeError(abort_reason)
        print("idle last", next((s for p, s in reversed(log) if p == "idle"), None))

        pump(2.0, DRIVE_RAD_S, DRIVE_RAD_S, True, "fwd")
        if abort_reason:
            raise RuntimeError(abort_reason)
        pump(1.5, -DRIVE_RAD_S, -DRIVE_RAD_S, True, "rev")
        if abort_reason:
            raise RuntimeError(abort_reason)
        pump(1.0, 0.0, 0.0, True, "zero")
        if abort_reason:
            raise RuntimeError(abort_reason)
        silence_start = time.perf_counter()
        pump(0.4, None, None, False, "silence")
        silence_s = time.perf_counter() - silence_start
        pump(0.3, 0.0, 0.0, False, "disarm")
        bus.send(cf.CAN_ID_ESTOP, bytes(8))
        pump(0.5, None, None, False, "post_estop")

        def vel_stats(phase: str) -> dict:
            vals = [s["vel"] for p, s in log if p == phase and s.get("vel")]
            if not vals:
                return {"n": 0, "mean": (0.0, 0.0), "peak": (0.0, 0.0)}
            return {
                "n": len(vals),
                "mean": (round(_mean([v[0] for v in vals]), 3), round(_mean([v[1] for v in vals]), 3)),
                "peak": (
                    round(max(vals, key=lambda v: abs(v[0]))[0], 3),
                    round(max(vals, key=lambda v: abs(v[1]))[1], 3),
                ),
            }

        def peak_i(phase: str) -> float:
            vals = [s["cur"] for p, s in log if p == phase and s.get("cur")]
            if not vals:
                return 0.0
            return max(max(abs(v[0]), abs(v[1])) for v in vals)

        summary = {
            "idle": vel_stats("idle"),
            "idle_peak_i": round(peak_i("idle"), 3),
            "fwd": vel_stats("fwd"),
            "fwd_peak_i": round(peak_i("fwd"), 3),
            "rev": vel_stats("rev"),
            "rev_peak_i": round(peak_i("rev"), 3),
            "zero_peak_i": round(peak_i("zero"), 3),
            "silence_peak_i": round(peak_i("silence"), 3),
            "silence_window_s": round(silence_s, 3),
            "post_estop_peak_i": round(peak_i("post_estop"), 3),
            "rx_ids": dict(tx_echo),
            "samples": len(log),
        }
        print("summary", summary)
        echo_202 = sum(n for k, n in tx_echo.items() if k.endswith("0x202"))
        print("tx_echo_0x202", echo_202)
        fwd_ok = abs(summary["fwd"]["mean"][0]) > 0.7 and abs(summary["fwd"]["mean"][1]) > 0.7
        rev_ok = summary["rev"]["mean"][0] < -0.4 and summary["rev"]["mean"][1] < -0.4
        print("motion_seen_fwd", fwd_ok, "motion_seen_rev", rev_ok)
        return 0 if fwd_ok and rev_ok else 2
    except Exception:
        try:
            bus.send(cf.CAN_ID_ESTOP, bytes(8))
            _send_velocity(bus, 0.0, 0.0, False, seq)
        except Exception:
            pass
        print("ABORT", abort_reason)
        raise
    finally:
        bus.close()


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--port", default=DEFAULT_PORT)
    sub = parser.add_subparsers(dest="command", required=True)
    listen = sub.add_parser("listen")
    listen.add_argument("--seconds", type=float, default=10.0)
    listen.set_defaults(func=cmd_listen)
    estop = sub.add_parser("estop")
    estop.set_defaults(func=cmd_estop)
    burst = sub.add_parser("velocity-burst")
    burst.add_argument("--rate", type=float, default=100.0)
    burst.set_defaults(func=cmd_velocity_burst)
    args = parser.parse_args()
    return args.func(args)


if __name__ == "__main__":
    raise SystemExit(main())
