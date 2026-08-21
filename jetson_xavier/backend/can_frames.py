"""CAN frame packing for the MIT impedance control feature.

Python mirror of ``can_protocol.h`` at the repository root. The two files must stay
byte-identical; ``python can_frames.py`` runs the shared fixture self-check.

Authoritative contract: specs/002-mit-impedance-control/contracts/can-protocol.md

The impedance command pair is BIG-ENDIAN. The retained legacy 0x200 velocity frame is
LITTLE-ENDIAN and keeps its own separate helper. Never share a helper between them.
"""

from __future__ import annotations

import struct
from dataclasses import dataclass

# ---------------------------------------------------------------------------
# Frame identifier bases. Add NodeID (0x01 front, 0x02 rear).
# ---------------------------------------------------------------------------
CAN_ID_ESTOP = 0x080

CAN_ID_POSITION_M1_BASE = 0x100
CAN_ID_POSITION_M2_BASE = 0x110
CAN_ID_CONTROL_BASE = 0x120
CAN_ID_DYNAMICS_M1_BASE = 0x130
CAN_ID_DYNAMICS_M2_BASE = 0x140

CAN_ID_STATUS_VELOCITY_BASE = 0x180
CAN_ID_STATUS_CURRENT_BASE = 0x190
CAN_ID_STATUS_CONFIG_BASE = 0x1A0
CAN_ID_STATUS_GAINS_BASE = 0x1B0
CAN_ID_STATUS_POSITION_BASE = 0x1C0
CAN_ID_STATUS_LIMIT_BASE = 0x1D0
CAN_ID_STATUS_APPLIED_M1_BASE = 0x1E0
CAN_ID_STATUS_APPLIED_M2_BASE = 0x1F0

CAN_ID_LEGACY_COMMAND_BASE = 0x200
CAN_ID_HEARTBEAT_BASE = 0x700

# Position half base per motor index (1-based)
POSITION_BASE_BY_MOTOR = {1: CAN_ID_POSITION_M1_BASE, 2: CAN_ID_POSITION_M2_BASE}
DYNAMICS_BASE_BY_MOTOR = {1: CAN_ID_DYNAMICS_M1_BASE, 2: CAN_ID_DYNAMICS_M2_BASE}
APPLIED_BASE_BY_MOTOR = {1: CAN_ID_STATUS_APPLIED_M1_BASE, 2: CAN_ID_STATUS_APPLIED_M2_BASE}

# ---------------------------------------------------------------------------
# Control frame commands
# ---------------------------------------------------------------------------
CAN_CTRL_ARM = 0x10
CAN_CTRL_MODE = 0x11
# Reserved forbidden bandwidth-write probe. The controller must always reject it.
CAN_CTRL_FORBIDDEN_BANDWIDTH = 0x12

CAN_CTRL_FLAG_ENABLE = 0x01
CAN_CTRL_FLAG_BRAKE = 0x02
CAN_CTRL_FLAG_CLEAR_FAULT = 0x04
CAN_CTRL_FLAG_ESTOP = 0x08

CAN_CTRL_SELECT_BOTH = 0x00
CAN_CTRL_SELECT_M1 = 0x01
CAN_CTRL_SELECT_M2 = 0x02

MOTION_MODE_VELOCITY = 0x00
MOTION_MODE_IMPEDANCE = 0x01
MODE_NAMES = {MOTION_MODE_VELOCITY: "velocity", MOTION_MODE_IMPEDANCE: "impedance"}

# ---------------------------------------------------------------------------
# Dynamics field ranges. Provisional torque-derived values until T043 measures
# the torque constant; change here and in can_protocol.h together.
# ---------------------------------------------------------------------------
IMPEDANCE_V_MIN = -45.0
IMPEDANCE_V_MAX = 45.0
IMPEDANCE_KP_MIN = 0.0
IMPEDANCE_KP_MAX = 50.0
IMPEDANCE_KD_MIN = 0.0
IMPEDANCE_KD_MAX = 1.0
IMPEDANCE_TFF_MIN = -0.5
IMPEDANCE_TFF_MAX = 0.5

IMPEDANCE_PAIR_MATCH_WINDOW_US = 5000

# ---------------------------------------------------------------------------
# Status bit layouts
# ---------------------------------------------------------------------------
CAN_CFG0_M1_MODE_SHIFT = 0
CAN_CFG0_M2_MODE_SHIFT = 2
CAN_CFG0_M1_ARMED = 1 << 4
CAN_CFG0_M2_ARMED = 1 << 5
CAN_CFG0_M1_TIMED_OUT = 1 << 6
CAN_CFG0_M2_TIMED_OUT = 1 << 7

CAN_CFG1_OVERRUN_FAULT = 1 << 0
CAN_CFG1_BANDWIDTH_CLAMPED = 1 << 1
CAN_CFG1_CALIBRATION_REQUIRED = 1 << 2
CAN_CFG1_TIMING_FAULT = 1 << 3
CAN_CFG1_M1_PAIR_FAULT = 1 << 4
CAN_CFG1_M2_PAIR_FAULT = 1 << 5

LIMIT_CAUSE_CURRENT = 1 << 0
LIMIT_CAUSE_OUTPUT_VOLTAGE = 1 << 1
LIMIT_CAUSE_BUS_VOLTAGE = 1 << 2
LIMIT_CAUSE_NAMES = (
    (LIMIT_CAUSE_CURRENT, "current"),
    (LIMIT_CAUSE_OUTPUT_VOLTAGE, "outputVoltage"),
    (LIMIT_CAUSE_BUS_VOLTAGE, "busVoltage"),
)

CAN_APPLIED_CAPTURE_APPLIED = 1 << 0
CAN_APPLIED_TARGET_ACTIVE = 1 << 1

# Legacy velocity frame
LEGACY_CMD_SET_VELOCITY = 0x01
LEGACY_COMMAND_SCALE = 100


# ---------------------------------------------------------------------------
# Scalar mapping
# ---------------------------------------------------------------------------
def float_to_uint(value: float, lo: float, hi: float, bits: int) -> int:
    full = (1 << bits) - 1
    span = hi - lo
    if span <= 0:
        return 0
    clamped = max(lo, min(hi, float(value)))
    scaled = round((clamped - lo) * (full / span))
    return max(0, min(full, int(scaled)))


def uint_to_float(raw: int, lo: float, hi: float, bits: int) -> float:
    full = (1 << bits) - 1
    if full == 0:
        return lo
    raw = max(0, min(full, int(raw)))
    return raw * ((hi - lo) / full) + lo


def rad_to_mrad(radians: float) -> int:
    """Signed int32 milliradians. Covers about +/-341,000 revolutions."""
    return int(round(radians * 1000.0))


def mrad_to_rad(mrad: int) -> float:
    return mrad * 0.001


def _pack_int32_be(value: int) -> bytes:
    return struct.pack(">i", value)


def _unpack_int32_be(data: bytes) -> int:
    return struct.unpack(">i", data)[0]


# ---------------------------------------------------------------------------
# Impedance command pair
# ---------------------------------------------------------------------------
@dataclass
class PositionHalf:
    p_des_mrad: int
    seq: int


@dataclass
class DynamicsHalf:
    v_des: float
    kp: float
    kd: float
    t_ff: float
    seq: int
    capture_current_position: bool = False


def pack_position_half(half: PositionHalf) -> bytes:
    """Bytes 0-3 int32 BE milliradians, byte 4 sequence, bytes 5-7 reserved zero."""
    return _pack_int32_be(half.p_des_mrad) + bytes([half.seq & 0xFF, 0, 0, 0])


def unpack_position_half(data: bytes) -> PositionHalf | None:
    """Returns None for a malformed frame; a rejected half must not change state."""
    if len(data) != 8:
        return None
    if data[5] or data[6] or data[7]:
        return None
    return PositionHalf(p_des_mrad=_unpack_int32_be(data[0:4]), seq=data[4])


def pack_dynamics_half(half: DynamicsHalf) -> bytes:
    v = float_to_uint(half.v_des, IMPEDANCE_V_MIN, IMPEDANCE_V_MAX, 12)
    kp = float_to_uint(half.kp, IMPEDANCE_KP_MIN, IMPEDANCE_KP_MAX, 12)
    kd = float_to_uint(half.kd, IMPEDANCE_KD_MIN, IMPEDANCE_KD_MAX, 12)
    tff = float_to_uint(half.t_ff, IMPEDANCE_TFF_MIN, IMPEDANCE_TFF_MAX, 12)
    return bytes(
        [
            (v >> 4) & 0xFF,
            ((v << 4) & 0xF0) | ((kp >> 8) & 0x0F),
            kp & 0xFF,
            (kd >> 4) & 0xFF,
            ((kd << 4) & 0xF0) | ((tff >> 8) & 0x0F),
            tff & 0xFF,
            half.seq & 0xFF,
            0x01 if half.capture_current_position else 0x00,
        ]
    )


def unpack_dynamics_half(data: bytes) -> DynamicsHalf | None:
    if len(data) != 8:
        return None
    if data[7] & 0xFE:
        return None
    v = (data[0] << 4) | (data[1] >> 4)
    kp = ((data[1] & 0x0F) << 8) | data[2]
    kd = (data[3] << 4) | (data[4] >> 4)
    tff = ((data[4] & 0x0F) << 8) | data[5]
    return DynamicsHalf(
        v_des=uint_to_float(v, IMPEDANCE_V_MIN, IMPEDANCE_V_MAX, 12),
        kp=uint_to_float(kp, IMPEDANCE_KP_MIN, IMPEDANCE_KP_MAX, 12),
        kd=uint_to_float(kd, IMPEDANCE_KD_MIN, IMPEDANCE_KD_MAX, 12),
        t_ff=uint_to_float(tff, IMPEDANCE_TFF_MIN, IMPEDANCE_TFF_MAX, 12),
        seq=data[6],
        capture_current_position=bool(data[7] & 0x01),
    )


def pairs_match(position: PositionHalf, dynamics: DynamicsHalf) -> bool:
    """Both halves of one logical command carry the same sequence number."""
    return position.seq == dynamics.seq


# ---------------------------------------------------------------------------
# Control frame
# ---------------------------------------------------------------------------
def pack_control_frame(
    command: int, flags: int = 0, selector: int = CAN_CTRL_SELECT_BOTH, mode: int = 0, seq: int = 0
) -> bytes:
    return bytes([command & 0xFF, flags & 0xFF, selector & 0xFF, mode & 0xFF, seq & 0xFF, 0, 0, 0])


def pack_legacy_velocity_frame(
    left_target: float, right_target: float, enable: bool, seq: int = 0
) -> bytes:
    """Retained legacy 0x200 frame. LITTLE-endian, deliberately separate from the pair."""
    flags = CAN_CTRL_FLAG_ENABLE if enable else 0x00
    left = max(-32768, min(32767, int(round(left_target * LEGACY_COMMAND_SCALE))))
    right = max(-32768, min(32767, int(round(right_target * LEGACY_COMMAND_SCALE))))
    return bytes([LEGACY_CMD_SET_VELOCITY, flags, 0x00, seq & 0xFF]) + struct.pack(
        "<hh", left, right
    )


# ---------------------------------------------------------------------------
# Status decoding
# ---------------------------------------------------------------------------
def decode_config_status(data: bytes) -> dict | None:
    if len(data) != 8:
        return None
    b0, b1 = data[0], data[1]
    return {
        "modes": [
            MODE_NAMES.get((b0 >> CAN_CFG0_M1_MODE_SHIFT) & 0x03, "unknown"),
            MODE_NAMES.get((b0 >> CAN_CFG0_M2_MODE_SHIFT) & 0x03, "unknown"),
        ],
        "armed": [bool(b0 & CAN_CFG0_M1_ARMED), bool(b0 & CAN_CFG0_M2_ARMED)],
        "timedOut": [bool(b0 & CAN_CFG0_M1_TIMED_OUT), bool(b0 & CAN_CFG0_M2_TIMED_OUT)],
        "pairFault": [bool(b1 & CAN_CFG1_M1_PAIR_FAULT), bool(b1 & CAN_CFG1_M2_PAIR_FAULT)],
        "faults": {
            "overrun": bool(b1 & CAN_CFG1_OVERRUN_FAULT),
            "calibrationRequired": bool(b1 & CAN_CFG1_CALIBRATION_REQUIRED),
            "timingSource": bool(b1 & CAN_CFG1_TIMING_FAULT),
        },
        "bandwidthClamped": bool(b1 & CAN_CFG1_BANDWIDTH_CLAMPED),
        "activeBandwidthHz": struct.unpack("<H", data[2:4])[0],
        "requestedBandwidthHz": struct.unpack("<H", data[4:6])[0],
        "carrierHz": struct.unpack("<H", data[6:8])[0] * 10,
    }


def decode_gains_status(data: bytes) -> dict | None:
    if len(data) != 8:
        return None
    kp1, kd1, kp2, kd2 = struct.unpack("<HHHH", data)
    return {"kp": [kp1 / 100.0, kp2 / 100.0], "kd": [kd1 / 10000.0, kd2 / 10000.0]}


def decode_position_status(data: bytes) -> dict | None:
    if len(data) != 8:
        return None
    return {"positionMrad": [_unpack_int32_be(data[0:4]), _unpack_int32_be(data[4:8])]}


def decode_limit_status(data: bytes) -> dict | None:
    if len(data) != 8:
        return None

    def causes(bits: int) -> list[str]:
        return [name for bit, name in LIMIT_CAUSE_NAMES if bits & bit]

    return {
        "limitCauses": [causes(data[0]), causes(data[1])],
        "limitCount": [
            struct.unpack("<H", data[2:4])[0],
            struct.unpack("<H", data[4:6])[0],
        ],
    }


def decode_applied_target_status(data: bytes) -> dict | None:
    if len(data) != 8:
        return None
    return {
        "appliedTargetMrad": _unpack_int32_be(data[0:4]),
        "lastPairSeq": data[4],
        "captureGeneration": data[5],
        "captureApplied": bool(data[6] & CAN_APPLIED_CAPTURE_APPLIED),
        "targetActive": bool(data[6] & CAN_APPLIED_TARGET_ACTIVE),
    }


# ---------------------------------------------------------------------------
# Shared fixture self-check (T005). Fixtures are published in
# contracts/can-protocol.md; both layers must agree on these exact bytes.
# ---------------------------------------------------------------------------
# 100 revolutions = 200*pi rad; the SC-013 safety anchor.
ONE_HUNDRED_REV_MRAD = rad_to_mrad(200.0 * 3.141592653589793)


def _self_check() -> int:
    failures: list[str] = []

    def check(name: str, got, want) -> None:
        if got != want:
            failures.append(f"{name}: got {got!r} want {want!r}")

    # --- Position half: extremes, zero, 100 revolutions, sequence wrap ---
    position_cases = [
        ("zero", PositionHalf(0, 0)),
        ("plus_one_rad", PositionHalf(1000, 1)),
        ("minus_one_rad", PositionHalf(-1000, 2)),
        ("hundred_rev", PositionHalf(ONE_HUNDRED_REV_MRAD, 42)),
        ("neg_hundred_rev", PositionHalf(-ONE_HUNDRED_REV_MRAD, 43)),
        ("int32_max", PositionHalf(2147483647, 254)),
        ("int32_min", PositionHalf(-2147483648, 255)),
    ]
    for name, half in position_cases:
        raw = pack_position_half(half)
        check(f"position len {name}", len(raw), 8)
        check(f"position reserved {name}", raw[5:8], b"\x00\x00\x00")
        back = unpack_position_half(raw)
        check(f"position roundtrip mrad {name}", back.p_des_mrad, half.p_des_mrad)
        check(f"position roundtrip seq {name}", back.seq, half.seq)

    # Exact wire bytes published in contracts/can-protocol.md "Shared wire fixtures".
    check("hundred rev mrad", ONE_HUNDRED_REV_MRAD, 628319)
    published_position = [
        (0, 0, "0000000000000000"),
        (1000, 1, "000003e801000000"),
        (-1000, 2, "fffffc1802000000"),
        (ONE_HUNDRED_REV_MRAD, 42, "0009965f2a000000"),
        (-ONE_HUNDRED_REV_MRAD, 43, "fff669a12b000000"),
        (2147483647, 254, "7ffffffffe000000"),
        (-2147483648, 255, "80000000ff000000"),
    ]
    for mrad, seq, want_hex in published_position:
        check(
            f"published position bytes {mrad}/{seq}",
            pack_position_half(PositionHalf(mrad, seq)).hex(),
            want_hex,
        )

    # --- Dynamics half: extremes and midpoints ---
    dynamics_cases = [
        ("all_min", DynamicsHalf(IMPEDANCE_V_MIN, IMPEDANCE_KP_MIN, IMPEDANCE_KD_MIN, IMPEDANCE_TFF_MIN, 0)),
        ("all_max", DynamicsHalf(IMPEDANCE_V_MAX, IMPEDANCE_KP_MAX, IMPEDANCE_KD_MAX, IMPEDANCE_TFF_MAX, 1)),
        ("zeros", DynamicsHalf(0.0, 0.0, 0.0, 0.0, 2)),
        ("driving", DynamicsHalf(12.5, 0.0, 0.3, 0.0, 3)),
        ("stiff_hold", DynamicsHalf(0.0, 12.0, 0.3, 0.0, 4)),
        ("capture", DynamicsHalf(0.0, 12.0, 0.3, 0.0, 5, True)),
    ]
    for name, half in dynamics_cases:
        raw = pack_dynamics_half(half)
        check(f"dynamics len {name}", len(raw), 8)
        back = unpack_dynamics_half(raw)
        check(f"dynamics seq {name}", back.seq, half.seq)
        check(f"dynamics capture {name}", back.capture_current_position, half.capture_current_position)
        for field, lo, hi in (
            ("v_des", IMPEDANCE_V_MIN, IMPEDANCE_V_MAX),
            ("kp", IMPEDANCE_KP_MIN, IMPEDANCE_KP_MAX),
            ("kd", IMPEDANCE_KD_MIN, IMPEDANCE_KD_MAX),
            ("t_ff", IMPEDANCE_TFF_MIN, IMPEDANCE_TFF_MAX),
        ):
            want = getattr(half, field)
            got = getattr(back, field)
            resolution = (hi - lo) / 4095.0
            if abs(got - want) > resolution:
                failures.append(f"dynamics {field} {name}: got {got} want {want}")

    check(
        "dynamics bytes all_min",
        pack_dynamics_half(
            DynamicsHalf(IMPEDANCE_V_MIN, IMPEDANCE_KP_MIN, IMPEDANCE_KD_MIN, IMPEDANCE_TFF_MIN, 0)
        ).hex(),
        "0000000000000000",
    )
    check(
        "dynamics bytes all_max",
        pack_dynamics_half(
            DynamicsHalf(IMPEDANCE_V_MAX, IMPEDANCE_KP_MAX, IMPEDANCE_KD_MAX, IMPEDANCE_TFF_MAX, 1)
        ).hex(),
        "ffffffffffff0100",
    )
    check(
        "dynamics capture flag byte",
        pack_dynamics_half(DynamicsHalf(0.0, 0.0, 0.0, 0.0, 9, True))[7],
        0x01,
    )

    # --- Malformed rejection ---
    check("position bad dlc", unpack_position_half(b"\x00" * 7), None)
    check("position reserved nonzero", unpack_position_half(bytes([0, 0, 0, 0, 0, 1, 0, 0])), None)
    check("dynamics bad dlc", unpack_dynamics_half(b"\x00" * 9), None)
    check("dynamics reserved flag bits", unpack_dynamics_half(bytes([0, 0, 0, 0, 0, 0, 0, 0x02])), None)

    # --- Sequence matching and wrap ---
    check("pair match", pairs_match(PositionHalf(0, 200), DynamicsHalf(0, 0, 0, 0, 200)), True)
    check("pair mismatch", pairs_match(PositionHalf(0, 200), DynamicsHalf(0, 0, 0, 0, 201)), False)
    check("pair wrap match", pairs_match(PositionHalf(0, 255), DynamicsHalf(0, 0, 0, 0, 255)), True)
    check("pair wrap mismatch", pairs_match(PositionHalf(0, 255), DynamicsHalf(0, 0, 0, 0, 0)), False)

    # --- Status round-trips ---
    cfg = bytes([0x05 | CAN_CFG0_M1_ARMED, CAN_CFG1_BANDWIDTH_CLAMPED]) + struct.pack(
        "<HHH", 1000, 2500, 3000
    )
    decoded = decode_config_status(cfg)
    check("cfg active bw", decoded["activeBandwidthHz"], 1000)
    check("cfg requested bw", decoded["requestedBandwidthHz"], 2500)
    check("cfg carrier", decoded["carrierHz"], 30000)
    check("cfg clamped", decoded["bandwidthClamped"], True)
    check("cfg m1 armed", decoded["armed"][0], True)
    check("cfg m1 mode", decoded["modes"][0], "impedance")
    check("cfg m2 mode", decoded["modes"][1], "impedance")

    gains = struct.pack("<HHHH", 1200, 3000, 0, 0)
    check("gains kp1", decode_gains_status(gains)["kp"][0], 12.0)
    check("gains kd1", decode_gains_status(gains)["kd"][0], 0.3)

    positions = _pack_int32_be(ONE_HUNDRED_REV_MRAD) + _pack_int32_be(-1000)
    check(
        "position status m1", decode_position_status(positions)["positionMrad"][0], ONE_HUNDRED_REV_MRAD
    )
    check("position status m2", decode_position_status(positions)["positionMrad"][1], -1000)

    limits = bytes([LIMIT_CAUSE_CURRENT, LIMIT_CAUSE_BUS_VOLTAGE]) + struct.pack("<HHH", 3, 7, 0)
    check("limit m1 causes", decode_limit_status(limits)["limitCauses"][0], ["current"])
    check("limit m2 causes", decode_limit_status(limits)["limitCauses"][1], ["busVoltage"])
    check("limit m1 count", decode_limit_status(limits)["limitCount"][0], 3)

    ack = _pack_int32_be(ONE_HUNDRED_REV_MRAD) + bytes(
        [42, 4, CAN_APPLIED_CAPTURE_APPLIED | CAN_APPLIED_TARGET_ACTIVE, 0]
    )
    decoded_ack = decode_applied_target_status(ack)
    check("ack target", decoded_ack["appliedTargetMrad"], ONE_HUNDRED_REV_MRAD)
    check("ack seq", decoded_ack["lastPairSeq"], 42)
    check("ack generation", decoded_ack["captureGeneration"], 4)
    check("ack capture applied", decoded_ack["captureApplied"], True)
    check("ack target active", decoded_ack["targetActive"], True)

    # --- Legacy frame stays little-endian and separate ---
    legacy = pack_legacy_velocity_frame(1.5, -1.5, True, seq=9)
    check("legacy len", len(legacy), 8)
    check("legacy cmd", legacy[0], LEGACY_CMD_SET_VELOCITY)
    check("legacy left LE", struct.unpack("<h", legacy[4:6])[0], 150)
    check("legacy right LE", struct.unpack("<h", legacy[6:8])[0], -150)

    if failures:
        print(f"FAIL: {len(failures)} fixture mismatch(es)")
        for line in failures:
            print(f"  - {line}")
        return 1
    print("OK: all CAN frame fixtures agree with contracts/can-protocol.md")
    return 0


if __name__ == "__main__":
    raise SystemExit(_self_check())
