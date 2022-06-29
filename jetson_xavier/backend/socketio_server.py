from __future__ import annotations

import json
import asyncio
from contextlib import asynccontextmanager
from datetime import datetime, timezone
from typing import Any

import can
import socketio
from fastapi import FastAPI
from pydantic import BaseModel, Field

HOST = "0.0.0.0"
PORT = 8080
JOYSTICK_EVENT = "joystick_command"
CAN_RX_EVENT = "can_message"
CAN_BUS_VOLTAGE_EVENT = "bus_voltage"
E_STOP_EVENT = "estop"
CAN_IFACE = "can0"
CAN_FRONT_NODE_ID = 0x201
CAN_REAR_NODE_ID = 0x202
HALF_LENGTH_M = 0.08
HALF_WIDTH_M = 0.05
WHEEL_RADIUS_M = 0.03
MAX_VX_MPS = 1.0
MAX_VY_MPS = 1.0
MAX_OMEGA_RAD_PER_SEC = 3.0
MAX_WHEEL_RAD_PER_SEC = 50.0
COMMAND_SCALE = 100


def extract_bus_voltage_from_status(message: can.Message) -> tuple[str, float] | None:
    if message.dlc < 6:
        return None

    node_id = message.arbitration_id - 0x190
    if node_id not in (0x01, 0x02):
        return None

    controller_can_id = f"0x{(0x200 + node_id):X}"
    raw = int.from_bytes(message.data[4:6], byteorder="little", signed=True)
    bus_voltage = raw / 100.0
    return controller_can_id, bus_voltage


class CanPublisher:
    def __init__(self, channel: str) -> None:
        self.channel = channel
        self.bus: can.BusABC | None = None
        self.seq = 0
        self.half_length_m = HALF_LENGTH_M
        self.half_width_m = HALF_WIDTH_M

    @staticmethod
    def _clamp(value: float, low: float, high: float) -> float:
        return max(low, min(high, value))

    @staticmethod
    def _to_int16_little_endian(value: float) -> tuple[int, int]:
        scaled = int(round(value * COMMAND_SCALE))
        scaled = max(-32768, min(32767, scaled))
        lo = scaled & 0xFF
        hi = (scaled >> 8) & 0xFF
        return lo, hi

    def _compute_wheel_targets(self, x: float, y: float, rotation: float) -> tuple[float, float, float, float]:
        x = self._clamp(x, -1.0, 1.0)
        y = self._clamp(y, -1.0, 1.0)
        rotation = self._clamp(rotation, -1.0, 1.0)

        vx = x * MAX_VX_MPS
        vy = y * MAX_VY_MPS
        omega_z = rotation * MAX_OMEGA_RAD_PER_SEC
        rotational_term = (self.half_length_m + self.half_width_m) * omega_z

        front_left = (-vy + vx + rotational_term) / WHEEL_RADIUS_M
        front_right = (-vy - vx - rotational_term) / WHEEL_RADIUS_M
        rear_left = (vy - vx + rotational_term) / WHEEL_RADIUS_M
        rear_right = (vy + vx - rotational_term) / WHEEL_RADIUS_M

        peak = max(abs(front_left), abs(front_right), abs(rear_left), abs(rear_right), MAX_WHEEL_RAD_PER_SEC)
        scale = MAX_WHEEL_RAD_PER_SEC / peak
        return (
            front_left * scale,
            front_right * scale,
            rear_left * scale,
            rear_right * scale,
        )

    def set_geometry(self, half_length_m: float, half_width_m: float) -> None:
        self.half_length_m = half_length_m
        self.half_width_m = half_width_m

    def get_geometry(self) -> dict[str, float]:
        return {
            "L": self.half_length_m,
            "W": self.half_width_m,
        }

    def _ensure_bus(self) -> bool:
        if self.bus is not None:
            return True
        try:
            self.bus = can.Bus(interface="socketcan", channel=self.channel)
            print(f"[CAN] opened interface {self.channel}")
            return True
        except Exception as exc:
            print(f"[CAN] failed to open interface {self.channel}: {exc}")
            self.bus = None
            return False

    def ensure_bus(self) -> bool:
        return self._ensure_bus()

    def _build_command_message(self, arbitration_id: int, left_target: float, right_target: float, enable: bool) -> can.Message:
        flags = 0x01
        left_lo, left_hi = self._to_int16_little_endian(left_target)
        right_lo, right_hi = self._to_int16_little_endian(right_target)

        payload = bytes([
            0x01,
            flags,
            0x00,
            self.seq,
            left_lo,
            left_hi,
            right_lo,
            right_hi,
        ])

        return can.Message(
            arbitration_id=arbitration_id,
            is_extended_id=False,
            data=payload,
        )

    def publish_joystick(self, payload: dict[str, Any]) -> None:
        if not self._ensure_bus():
            return

        x = float(payload.get("x", 0.0))
        y = float(payload.get("y", 0.0))
        rotation = float(payload.get("rotation", 0.0))

        front_left, front_right, rear_left, rear_right = self._compute_wheel_targets(x, y, rotation)
        moving = any(abs(value) > 1e-3 for value in (front_left, front_right, rear_left, rear_right))

        front_msg = self._build_command_message(CAN_FRONT_NODE_ID, front_left, front_right, moving)
        rear_msg = self._build_command_message(CAN_REAR_NODE_ID, rear_left, rear_right, moving)

        try:
            self.bus.send(front_msg)
            self.bus.send(rear_msg)
            # print(
            #     "[CAN] sent "
            #     f"front(id=0x{CAN_FRONT_NODE_ID:X}, left={front_left:.2f}, right={front_right:.2f}) "
            #     f"rear(id=0x{CAN_REAR_NODE_ID:X}, left={rear_left:.2f}, right={rear_right:.2f}) "
            #     f"seq={self.seq} enable={moving}"
            # )
            self.seq = (self.seq + 1) & 0xFF
        except can.CanError as exc:
            print(f"[CAN] send failed: {exc}")

    def close(self) -> None:
        if self.bus is not None:
            self.bus.shutdown()
            self.bus = None
            print(f"[CAN] closed interface {self.channel}")


can_publisher = CanPublisher(CAN_IFACE)
sio = socketio.AsyncServer(
    async_mode="asgi",
    cors_allowed_origins="*",
)

# Global E-Stop state. When True, joystick commands will be ignored
estop_state = False


async def can_rx_loop(stop_event: asyncio.Event) -> None:
    while not stop_event.is_set():
        if not can_publisher.ensure_bus():
            await asyncio.sleep(1.0)
            continue

        assert can_publisher.bus is not None
        try:
            message = await asyncio.to_thread(can_publisher.bus.recv, 0.2)
        except Exception as exc:
            print(f"[CAN] receive error: {exc}")
            await asyncio.sleep(0.2)
            continue

        if message is None:
            continue

        payload = {
            "id": f"0x{message.arbitration_id:X}",
            "dlc": message.dlc,
            "data": [int(b) for b in message.data],
            "timestamp": message.timestamp,
            "extended": message.is_extended_id,
        }
        # print(
        #     "[CAN] received "
        #     f"id={payload['id']} dlc={payload['dlc']} "
        #     f"data={' '.join(f'{byte:02X}' for byte in payload['data'])}"
        # )
        await sio.emit(CAN_RX_EVENT, payload)

        voltage = extract_bus_voltage_from_status(message)
        if voltage is not None:
            controller_can_id, bus_voltage = voltage
            await sio.emit(
                CAN_BUS_VOLTAGE_EVENT,
                {
                    "canId": controller_can_id,
                    "busVoltage": bus_voltage,
                    "sourceStatusId": payload["id"],
                    "timestamp": message.timestamp,
                },
            )

@asynccontextmanager
async def lifespan(_: FastAPI):
    stop_event = asyncio.Event()
    rx_task = asyncio.create_task(can_rx_loop(stop_event))
    try:
        yield
    finally:
        stop_event.set()
        rx_task.cancel()
        try:
            await rx_task
        except asyncio.CancelledError:
            pass
        can_publisher.close()


api = FastAPI(title="Mecanum Wheel Car Backend", lifespan=lifespan)


class GeometryConfig(BaseModel):
    L: float = Field(gt=0.0, description="Half-length from robot center to wheel in meters")
    W: float = Field(gt=0.0, description="Half-width from robot center to wheel in meters")


@api.get("/")
async def health() -> dict[str, str]:
    can_ready = "yes" if can_publisher.bus is not None else "no"
    return {"status": "ok", "service": "socketio-backend", "can_ready": can_ready}


@api.get("/config")
async def get_config() -> dict[str, float]:
    return can_publisher.get_geometry()


@api.post("/config")
async def update_config(config: GeometryConfig) -> dict[str, float]:
    can_publisher.set_geometry(config.L, config.W)
    print(f"[CFG] updated geometry L={config.L:.4f}m W={config.W:.4f}m")
    return can_publisher.get_geometry()


@sio.event
async def connect(sid: str, environ: dict[str, Any], auth: Any) -> None:
    timestamp = datetime.now(timezone.utc).isoformat()
    print(f"[{timestamp}] client connected: sid={sid}")


@sio.event
async def disconnect(sid: str) -> None:
    timestamp = datetime.now(timezone.utc).isoformat()
    print(f"[{timestamp}] client disconnected: sid={sid}")


@sio.on(JOYSTICK_EVENT)
async def on_joystick_command(sid: str, data: Any) -> None:
    timestamp = datetime.now(timezone.utc).isoformat()
    payload = json.dumps(data, ensure_ascii=False)
    # print(f"[{timestamp}] {JOYSTICK_EVENT} sid={sid} payload={payload}")
    if estop_state:
        print(f"[{timestamp}] joystick ignored: estop engaged")
        return

    if isinstance(data, dict):
        can_publisher.publish_joystick(data)


@sio.on(E_STOP_EVENT)
async def on_estop(sid: str, data: Any) -> None:
    """Handle E-Stop toggle from clients.

    Expected payload: either a boolean True/False or a dict with key 'engaged'.
    When engaged, immediately send zero/disabled commands to motor controllers
    and broadcast the estop state to all clients.
    """
    global estop_state
    timestamp = datetime.now(timezone.utc).isoformat()

    engaged = False
    if isinstance(data, bool):
        engaged = data
    elif isinstance(data, dict):
        engaged = bool(data.get("engaged", False))

    estop_state = engaged
    print(f"[{timestamp}] {E_STOP_EVENT} sid={sid} engaged={estop_state}")

    # Broadcast estop state to all connected clients
    await sio.emit("estop_state", {"engaged": estop_state})

    # If engaged, send a disable/zero command to controllers to ensure motors stop
    if estop_state and can_publisher._ensure_bus():
        # Build and send command messages with enable flag False and zero targets
        try:
            front_msg = can_publisher._build_command_message(CAN_FRONT_NODE_ID, 0.0, 0.0, False)
            rear_msg = can_publisher._build_command_message(CAN_REAR_NODE_ID, 0.0, 0.0, False)
            if can_publisher.bus is not None:
                can_publisher.bus.send(front_msg)
                can_publisher.bus.send(rear_msg)
                print(f"[CAN] sent estop disable messages")
        except can.CanError as exc:
            print(f"[CAN] estop send failed: {exc}")


app = socketio.ASGIApp(
    sio,
    other_asgi_app=api,
)


if __name__ == "__main__":
    import uvicorn

    uvicorn.run("socketio_server:app", host=HOST, port=PORT, reload=False)
