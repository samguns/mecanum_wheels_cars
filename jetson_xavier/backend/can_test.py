#!/usr/bin/env python3
import asyncio
import time
import can

CAN_IFACE = "can0"

class AsyncioQueueListener(can.Listener):
    """
    Listener -> asyncio.Queue bridge.
    Notifier calls this from a background thread, so we use call_soon_threadsafe.
    """
    def __init__(self, loop: asyncio.AbstractEventLoop, queue: asyncio.Queue):
        super().__init__()
        self._loop = loop
        self._queue = queue

    def on_message_received(self, msg: can.Message) -> None:
        self._loop.call_soon_threadsafe(self._queue.put_nowait, msg)

async def rx_consumer(queue: asyncio.Queue) -> None:
    print("[RX] consumer started")
    while True:
        msg: can.Message = await queue.get()
        data_hex = msg.data.hex(" ", 1) if msg.data else ""

        print(
            f"[RX] ts={msg.timestamp:.6f} id=0x{msg.arbitration_id:X} "
            f"ext={msg.is_extended_id} dlc={msg.dlc} data={data_hex}"
        )

async def tx_task(bus: can.BusABC) -> None:
    """
    Periodically send a Classic CAN frame (0..8 bytes).
    bus.send() is blocking, so we offload to a thread.
    """
    print("[TX] task started")
    counter = 0
    while True:
        payload = bytes([counter & 0xFF, 0x11, 0x22, 0x33, 0x44, 0x55, 0x66, 0x77])  # 8 bytes

        msg = can.Message(
            arbitration_id=0x123,     # standard 11-bit ID
            is_extended_id=False,
            data=payload,
            # Classic CAN: do NOT set is_fd or bitrate_switch
        )

        try:
            bus.send(msg)
            print(f"[TX] sent id=0x{msg.arbitration_id:X} data={payload.hex(' ', 1)}")
        except can.CanError as e:
            print(f"[TX] send failed: {e}")
            # If buffer is full, wait a bit to let it drain
            await asyncio.sleep(1.0)

        counter = (counter + 1) & 0xFF
        await asyncio.sleep(0.1)  # 10 Hz

async def main() -> None:
    loop = asyncio.get_running_loop()
    rx_queue: asyncio.Queue[can.Message] = asyncio.Queue(maxsize=1000)

    # Classic CAN bus (no fd=True)
    bus = can.Bus(interface="socketcan", channel=CAN_IFACE)

    # Optional: kernel-side filtering (reduces load)
    # bus.set_filters([{"can_id": 0x123, "can_mask": 0x7FF, "extended": False}])

    listener = AsyncioQueueListener(loop, rx_queue)

    # Notifier spawns a reader thread and dispatches to listeners
    notifier = can.Notifier(bus, [listener], timeout=1.0)

    consumer_task = asyncio.create_task(rx_consumer(rx_queue))
    sender_task = asyncio.create_task(tx_task(bus))

    try:
        await asyncio.gather(consumer_task, sender_task)
    finally:
        notifier.stop()
        bus.shutdown()

if __name__ == "__main__":
    try:
        asyncio.run(main())
    except KeyboardInterrupt:
        print("\nExiting...")
