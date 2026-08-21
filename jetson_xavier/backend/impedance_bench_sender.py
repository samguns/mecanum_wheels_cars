#!/usr/bin/env python3
"""Bench sender for the MIT impedance control feature.

Exists so firmware stories (US1-US6) can be validated on hardware before the production
backend changes land. It can also deliberately produce the fault cases the contract
requires the controller to reject.

Safety: every command here can energise a motor. Use with the vehicle secured and the
wheels clear of the ground, with the serial console open and the estop reachable.

Examples
--------
Stream a damped, zero-stiffness pair to motor 1 on the front node at 200 Hz::

    python impedance_bench_sender.py stream --node 1 --motor 1 --kd 0.3

Hold position with stiffness, letting the controller capture its own position::

    python impedance_bench_sender.py stream --node 1 --motor 1 --kp 12 --kd 0.3 --capture

The SC-013 safety anchor, a target 100 revolutions away at maximum stiffness::

    python impedance_bench_sender.py stream --node 1 --motor 1 --kp 50 --hundred-rev

Fault injection the controller must reject without changing state::

    python impedance_bench_sender.py fault --node 1 --motor 1 --kind position-only
    python impedance_bench_sender.py fault --node 1 --motor 1 --kind mismatch
    python impedance_bench_sender.py fault --node 1 --motor 1 --kind expire
    python impedance_bench_sender.py fault --node 1 --motor 1 --kind bad-reserved
    python impedance_bench_sender.py fault --node 1 --kind forbidden-bandwidth

Mode and arm control::

    python impedance_bench_sender.py mode --node 1 --motor 0 --mode impedance
    python impedance_bench_sender.py arm  --node 1 --motor 0 --enable
    python impedance_bench_sender.py estop
"""

from __future__ import annotations

import argparse
import sys
import time

try:
    import can
except ImportError:  # pragma: no cover - surfaced at runtime on the bench
    print("python-can is required: pip install -r requirements.txt", file=sys.stderr)
    raise

import can_frames as cf

DEFAULT_CHANNEL = "can0"
DEFAULT_RATE_HZ = 200.0


def open_bus(channel: str) -> can.BusABC:
    return can.Bus(interface="socketcan", channel=channel)


def send(bus: can.BusABC, arbitration_id: int, data: bytes) -> None:
    bus.send(can.Message(arbitration_id=arbitration_id, is_extended_id=False, data=data))


def position_id(node: int, motor: int) -> int:
    return cf.POSITION_BASE_BY_MOTOR[motor] + node


def dynamics_id(node: int, motor: int) -> int:
    return cf.DYNAMICS_BASE_BY_MOTOR[motor] + node


def send_pair(
    bus: can.BusABC,
    node: int,
    motor: int,
    p_des_mrad: int,
    v_des: float,
    kp: float,
    kd: float,
    t_ff: float,
    seq: int,
    capture: bool,
) -> None:
    """Send one atomic pair: position half immediately followed by its dynamics half."""
    send(bus, position_id(node, motor), cf.pack_position_half(cf.PositionHalf(p_des_mrad, seq)))
    send(
        bus,
        dynamics_id(node, motor),
        cf.pack_dynamics_half(cf.DynamicsHalf(v_des, kp, kd, t_ff, seq, capture)),
    )


def cmd_stream(args: argparse.Namespace) -> int:
    bus = open_bus(args.channel)
    motors = [1, 2] if args.motor == 0 else [args.motor]
    period = 1.0 / args.rate
    seq = 0

    target_mrad = args.position_mrad
    if args.hundred_rev:
        target_mrad = cf.ONE_HUNDRED_REV_MRAD
        print(f"SC-013 anchor: target {target_mrad} mrad (100 revolutions)")

    print(
        f"streaming node 0x{node_hex(args.node)} motors {motors} at {args.rate:g} Hz "
        f"kp={args.kp} kd={args.kd} t_ff={args.t_ff} v_des={args.v_des} "
        f"capture={args.capture} (ctrl-c to stop)"
    )
    sent = 0
    next_tick = time.perf_counter()
    try:
        while True:
            for motor in motors:
                send_pair(
                    bus,
                    args.node,
                    motor,
                    target_mrad,
                    args.v_des,
                    args.kp,
                    args.kd,
                    args.t_ff,
                    seq,
                    args.capture,
                )
            seq = (seq + 1) & 0xFF
            sent += 1
            if args.count and sent >= args.count:
                break
            next_tick += period
            sleep_for = next_tick - time.perf_counter()
            if sleep_for > 0:
                time.sleep(sleep_for)
            else:
                next_tick = time.perf_counter()
    except KeyboardInterrupt:
        print(f"\nstopped after {sent} ticks")
        if args.zero_on_exit:
            for motor in motors:
                send_pair(bus, args.node, motor, 0, 0.0, 0.0, 0.0, 0.0, seq, False)
            print("sent explicit zero-effort pair")
    finally:
        bus.shutdown()
    return 0


def cmd_fault(args: argparse.Namespace) -> int:
    """Inject the pair faults the contract requires the controller to reject."""
    bus = open_bus(args.channel)
    node, motor, kind = args.node, args.motor, args.kind
    try:
        if kind == "position-only":
            send(bus, position_id(node, motor), cf.pack_position_half(cf.PositionHalf(1000, 7)))
            print("sent position half only; expect no state change and no timeout refresh")

        elif kind == "dynamics-only":
            send(
                bus,
                dynamics_id(node, motor),
                cf.pack_dynamics_half(cf.DynamicsHalf(0, 12.0, 0.3, 0, 7, False)),
            )
            print("sent dynamics half only; expect no state change and no timeout refresh")

        elif kind == "mismatch":
            send(bus, position_id(node, motor), cf.pack_position_half(cf.PositionHalf(1000, 10)))
            send(
                bus,
                dynamics_id(node, motor),
                cf.pack_dynamics_half(cf.DynamicsHalf(0, 12.0, 0.3, 0, 11, False)),
            )
            print("sent sequence-mismatched pair (10 vs 11); expect atomic rejection")

        elif kind == "wrap-mismatch":
            send(bus, position_id(node, motor), cf.pack_position_half(cf.PositionHalf(1000, 255)))
            send(
                bus,
                dynamics_id(node, motor),
                cf.pack_dynamics_half(cf.DynamicsHalf(0, 12.0, 0.3, 0, 0, False)),
            )
            print("sent 255/0 wrap mismatch; expect rejection, no wrap tolerance")

        elif kind == "expire":
            send(bus, position_id(node, motor), cf.pack_position_half(cf.PositionHalf(1000, 20)))
            delay = (cf.IMPEDANCE_PAIR_MATCH_WINDOW_US / 1e6) * 3
            print(f"sent position half, waiting {delay:.3f}s to exceed the 5 ms match window")
            time.sleep(delay)
            send(
                bus,
                dynamics_id(node, motor),
                cf.pack_dynamics_half(cf.DynamicsHalf(0, 12.0, 0.3, 0, 20, False)),
            )
            print("sent late dynamics half; expect expiry rejection and a pair fault")

        elif kind == "bad-reserved":
            bad = bytearray(cf.pack_position_half(cf.PositionHalf(1000, 30)))
            bad[5] = 0x01  # reserved byte must be zero
            send(bus, position_id(node, motor), bytes(bad))
            print("sent position half with non-zero reserved byte; expect rejection")

        elif kind == "bad-flags":
            bad = bytearray(
                cf.pack_dynamics_half(cf.DynamicsHalf(0, 12.0, 0.3, 0, 31, False))
            )
            bad[7] = 0x02  # reserved flag bits must be zero
            send(bus, dynamics_id(node, motor), bytes(bad))
            print("sent dynamics half with reserved flag bit set; expect rejection")

        elif kind == "forbidden-bandwidth":
            send(
                bus,
                cf.CAN_ID_CONTROL_BASE + node,
                cf.pack_control_frame(cf.CAN_CTRL_FORBIDDEN_BANDWIDTH, seq=1),
            )
            print(
                "sent forbidden bandwidth-write probe 0x12; expect rejection with "
                "requested/active bandwidth and stored config unchanged (SC-009a)"
            )

        elif kind == "legacy-in-impedance":
            send(
                bus,
                cf.CAN_ID_LEGACY_COMMAND_BASE + node,
                cf.pack_legacy_velocity_frame(5.0, 5.0, True, seq=1),
            )
            print("sent legacy velocity frame; expect rejection if the motor is in impedance mode")

        else:  # pragma: no cover - argparse restricts choices
            raise ValueError(kind)
    finally:
        bus.shutdown()
    return 0


def cmd_mode(args: argparse.Namespace) -> int:
    bus = open_bus(args.channel)
    mode = cf.MOTION_MODE_IMPEDANCE if args.mode == "impedance" else cf.MOTION_MODE_VELOCITY
    selector = {0: cf.CAN_CTRL_SELECT_BOTH, 1: cf.CAN_CTRL_SELECT_M1, 2: cf.CAN_CTRL_SELECT_M2}[
        args.motor
    ]
    try:
        send(
            bus,
            cf.CAN_ID_CONTROL_BASE + args.node,
            cf.pack_control_frame(cf.CAN_CTRL_MODE, selector=selector, mode=mode, seq=1),
        )
        print(f"requested mode {args.mode} for motor selector {args.motor}")
        print("refused while armed; disarm first and check the serial report")
    finally:
        bus.shutdown()
    return 0


def cmd_arm(args: argparse.Namespace) -> int:
    bus = open_bus(args.channel)
    selector = {0: cf.CAN_CTRL_SELECT_BOTH, 1: cf.CAN_CTRL_SELECT_M1, 2: cf.CAN_CTRL_SELECT_M2}[
        args.motor
    ]
    flags = cf.CAN_CTRL_FLAG_ENABLE if args.enable else 0x00
    try:
        send(
            bus,
            cf.CAN_ID_CONTROL_BASE + args.node,
            cf.pack_control_frame(cf.CAN_CTRL_ARM, flags=flags, selector=selector, seq=1),
        )
        print(f"{'armed' if args.enable else 'disarmed'} motor selector {args.motor}")
    finally:
        bus.shutdown()
    return 0


def cmd_estop(args: argparse.Namespace) -> int:
    bus = open_bus(args.channel)
    try:
        send(bus, cf.CAN_ID_ESTOP, bytes(8))
        print("sent emergency stop 0x080")
    finally:
        bus.shutdown()
    return 0


def node_hex(node: int) -> str:
    return f"{0x200 + node:X}"


def build_parser() -> argparse.ArgumentParser:
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--channel", default=DEFAULT_CHANNEL, help="SocketCAN channel")
    parser.add_argument("--node", type=int, default=1, choices=(1, 2), help="1 front, 2 rear")
    sub = parser.add_subparsers(dest="command", required=True)

    stream = sub.add_parser("stream", help="stream matched pairs at a fixed rate")
    stream.add_argument("--motor", type=int, default=1, choices=(0, 1, 2), help="0 = both")
    stream.add_argument("--rate", type=float, default=DEFAULT_RATE_HZ)
    stream.add_argument("--count", type=int, default=0, help="stop after N ticks, 0 = forever")
    stream.add_argument("--position-mrad", type=int, default=0)
    stream.add_argument("--hundred-rev", action="store_true", help="SC-013 anchor target")
    stream.add_argument("--v-des", type=float, default=0.0)
    stream.add_argument("--kp", type=float, default=0.0)
    stream.add_argument("--kd", type=float, default=0.0)
    stream.add_argument("--t-ff", type=float, default=0.0)
    stream.add_argument("--capture", action="store_true", help="set capture-current-position")
    stream.add_argument("--no-zero-on-exit", dest="zero_on_exit", action="store_false")
    stream.set_defaults(func=cmd_stream, zero_on_exit=True)

    fault = sub.add_parser("fault", help="inject a pair fault the controller must reject")
    fault.add_argument("--motor", type=int, default=1, choices=(1, 2))
    fault.add_argument(
        "--kind",
        required=True,
        choices=(
            "position-only",
            "dynamics-only",
            "mismatch",
            "wrap-mismatch",
            "expire",
            "bad-reserved",
            "bad-flags",
            "forbidden-bandwidth",
            "legacy-in-impedance",
        ),
    )
    fault.set_defaults(func=cmd_fault)

    mode = sub.add_parser("mode", help="select motion mode")
    mode.add_argument("--motor", type=int, default=0, choices=(0, 1, 2))
    mode.add_argument("--mode", required=True, choices=("velocity", "impedance"))
    mode.set_defaults(func=cmd_mode)

    arm = sub.add_parser("arm", help="arm or disarm")
    arm.add_argument("--motor", type=int, default=0, choices=(0, 1, 2))
    arm.add_argument("--enable", action="store_true")
    arm.set_defaults(func=cmd_arm)

    estop = sub.add_parser("estop", help="send emergency stop 0x080")
    estop.set_defaults(func=cmd_estop)

    return parser


def main() -> int:
    args = build_parser().parse_args()
    return args.func(args)


if __name__ == "__main__":
    raise SystemExit(main())
