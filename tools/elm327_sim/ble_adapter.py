"""Optional macOS Bleak ELM test adapter; explicit peripheral identity only.

Never scans or selects by advertisement name. Importing this module does not
connect. The runner must require separate isolated-bench authorization.
"""
from __future__ import annotations
import asyncio
import threading
import time

GATT = {
    "m5dial": {
        "service": "6E400001-B5A3-F393-E0A9-E50E24DCCA9E",
        "write": "6E400002-B5A3-F393-E0A9-E50E24DCCA9E",
        "notify": "6E400003-B5A3-F393-E0A9-E50E24DCCA9E",
        "response": True,
    },
    "kw905": {"service": "FFF0", "write": "FFF2", "notify": "FFF1", "response": False},
}


class ExplicitBLEELMAdapter:
    def __init__(self, profile: str, identifier: str):
        if profile not in GATT or not identifier or len(identifier) > 128:
            raise ValueError("explicit BLE identifier and supported profile required")
        self.profile = GATT[profile]
        self.identifier = identifier
        self.loop = asyncio.new_event_loop()
        self.thread = threading.Thread(target=self._run, name="ttm-elm-ble-bench", daemon=True)
        self.thread.start()
        self.client = None
        try:
            self._submit(self._open(), 20)
        except BaseException:
            self.close()
            raise

    def _run(self):
        asyncio.set_event_loop(self.loop)
        self.loop.run_forever()

    def _submit(self, coroutine, timeout):
        return asyncio.run_coroutine_threadsafe(coroutine, self.loop).result(timeout=timeout)

    async def _open(self):
        try:
            from bleak import BleakClient
        except ImportError as error:
            raise RuntimeError("BLE bench needs explicit: pip install bleak") from error
        self.data = asyncio.Queue(maxsize=256)
        self.client = BleakClient(self.identifier, timeout=10)
        await self.client.connect()
        # Service resolution is a hard boundary, not inferred by advertisement.
        service = self.client.services.get_service(self.profile["service"])
        if service is None:
            raise RuntimeError("expected ELM GATT service absent")
        if service.get_characteristic(self.profile["write"]) is None or (
            service.get_characteristic(self.profile["notify"]) is None
        ):
            raise RuntimeError("expected ELM GATT characteristic absent")

        def receive(_sender, data):
            try:
                self.data.put_nowait(bytes(data))
            except asyncio.QueueFull:
                # Never silently misinterpret truncated ELM data.
                self.overflow = True

        self.overflow = False
        await self.client.start_notify(self.profile["notify"], receive)

    async def _exchange(self, command: str):
        if self.client is None or not self.client.is_connected:
            raise AssertionError("BLE disconnected")
        while not self.data.empty():
            self.data.get_nowait()
        self.overflow = False
        await self.client.write_gatt_char(self.profile["write"],
                                          command.encode("ascii") + b"\r",
                                          response=self.profile["response"])
        gathered = bytearray()
        deadline = time.monotonic() + 8
        while time.monotonic() < deadline:
            fragment = await asyncio.wait_for(self.data.get(), timeout=max(0.001, deadline - time.monotonic()))
            gathered.extend(fragment)
            if self.overflow or len(gathered) > 8192:
                raise AssertionError("BLE ELM notification loss or response overflow")
            if b">" in gathered:
                return bytes(gathered)
        raise AssertionError("BLE ELM response prompt timeout")

    def exchange(self, command: str):
        return self._submit(self._exchange(command), 10)

    async def _shutdown(self):
        if self.client:
            if self.client.is_connected:
                await self.client.disconnect()

    def close(self):
        try:
            self._submit(self._shutdown(), 8)
        except Exception:
            pass
        self.loop.call_soon_threadsafe(self.loop.stop)
        self.thread.join(timeout=5)
        self.loop.close()
