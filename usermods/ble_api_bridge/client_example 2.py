#!/usr/bin/env python3
"""Reference client: pip install -r requirements-client.txt; use --help for commands."""
from __future__ import annotations
import argparse
import asyncio
from dataclasses import dataclass
import sys
from bleak import BleakClient, BleakScanner

SERVICE_UUID = "7c2e0001-5d2b-4fd0-b1c2-0cc8f5470101"
RX_UUID = "7c2e0002-5d2b-4fd0-b1c2-0cc8f5470101"
TX_UUID = "7c2e0003-5d2b-4fd0-b1c2-0cc8f5470101"
LIVE_UUID = "7c2e0004-5d2b-4fd0-b1c2-0cc8f5470101"


@dataclass
class BridgeResponse:
    status: int
    content_type: str
    body: str


class BleApiBridgeClient:
    def __init__(self, address: str, chunk_size: int = 244, timeout: float = 120):
        if not 3 <= chunk_size <= 244:
            raise ValueError("chunk size must be between 3 and 244")
        self.chunk_size = chunk_size
        self.timeout = timeout
        self.client = BleakClient(address, services=[SERVICE_UUID], disconnected_callback=self._disconnected, timeout=90)
        self._response: asyncio.Future[BridgeResponse] | None = None
        self._expected: int | None = None
        self._buffer = bytearray()
        self._request_lock = asyncio.Lock()

    async def connect(self, pair: bool = False) -> None:
        await self.client.connect()
        if pair and sys.platform != "darwin":
            await self.client.pair()
        # macOS, like iOS, owns pairing. This protected read presents the system prompt.
        async with asyncio.timeout(90):
            if await self.client.read_gatt_char(TX_UUID) != b"ready":
                raise ValueError("not a compatible WLED Bluetooth bridge")
            await self.client.start_notify(TX_UUID, self._on_tx)

    async def disconnect(self) -> None:
        if self.client.is_connected:
            await self.client.disconnect()

    def _disconnected(self, _client) -> None:
        if self._response is not None and not self._response.done():
            self._response.set_exception(ConnectionError("WLED disconnected"))

    def _on_tx(self, _characteristic, data: bytearray) -> None:
        if self._response is None or self._response.done():
            return
        try:
            if self._expected is None:
                if len(data) < 2:
                    raise ValueError("missing BLE frame length")
                self._expected = int.from_bytes(data[:2], "little")
                self._buffer.extend(data[2:])
            else:
                self._buffer.extend(data)
            if not self._expected or not data or len(self._buffer) > self._expected:
                raise ValueError("invalid BLE frame length")
            if len(self._buffer) == self._expected:
                self._response.set_result(self._parse_response(bytes(self._buffer)))
        except (ValueError, UnicodeDecodeError) as error:
            self._response.set_exception(error)

    async def request(self, method: str, path: str, body: str = "") -> BridgeResponse:
        payload = f"{method.upper()} {path}\n\n{body}".encode()
        if len(payload) > 4096:
            raise ValueError("request exceeds bridge limit (4096 bytes)")
        async with self._request_lock:
            characteristic = self.client.services.get_characteristic(RX_UUID)
            if characteristic is None:
                raise ValueError("WLED RX characteristic missing")
            budget = min(self.chunk_size, characteristic.max_write_without_response_size)
            if budget < 3:
                raise ValueError("invalid ATT write budget")
            self._expected = None
            self._buffer.clear()
            self._response = asyncio.get_running_loop().create_future()
            framed = len(payload).to_bytes(2, "little") + payload
            try:
                async with asyncio.timeout(self.timeout):
                    for offset in range(0, len(framed), budget):
                        await self.client.write_gatt_char(characteristic, framed[offset:offset + budget], response=True)
                    return await self._response
            except BaseException:
                # No request IDs exist on this wire protocol; reconnect before retrying after any failure.
                # Cancel first so a disconnect callback cannot leave an unobserved exception on this future.
                if not self._response.done():
                    self._response.cancel()
                elif not self._response.cancelled():
                    self._response.exception()
                await self.disconnect()
                raise
            finally:
                if self._response is not None and not self._response.done():
                    self._response.cancel()
                self._response = None

    @staticmethod
    def _parse_response(raw: bytes) -> BridgeResponse:
        header, body = raw.decode().split("\n\n", 1)
        status_text, content_type = header.split(" ", 1)
        status = int(status_text)
        if not 100 <= status <= 599 or not content_type:
            raise ValueError("invalid response status")
        return BridgeResponse(status, content_type, body)


async def main() -> None:
    parser = argparse.ArgumentParser(description="Control WLED using its encrypted Bluetooth JSON bridge.")
    parser.add_argument("--scan", action="store_true", help="list nearby bridge devices")
    parser.add_argument("--address", help="BLE address, or macOS peripheral UUID from --scan")
    parser.add_argument("--pair", action="store_true", help="explicit pairing on Linux/Windows; macOS pairs automatically")
    parser.add_argument("--chunk-size", type=int, default=244)
    parser.add_argument("--timeout", type=float, default=120)
    parser.add_argument("method", nargs="?", choices=["get", "post"], default="get")
    parser.add_argument("path", nargs="?", default="/json/info")
    parser.add_argument("body", nargs="?", default="")
    args = parser.parse_args()
    if args.scan:
        for device in await BleakScanner.discover(service_uuids=[SERVICE_UUID]):
            print(f"{device.address}\t{device.name or 'WLED'}")
        return
    if not args.address:
        parser.error("use --scan or provide --address")
    bridge = BleApiBridgeClient(args.address, args.chunk_size, args.timeout)
    try:
        await bridge.connect(args.pair)
        response = await bridge.request(args.method, args.path, args.body)
        print(f"{response.status} {response.content_type}\n\n{response.body}")
    finally:
        await bridge.disconnect()


if __name__ == "__main__":
    asyncio.run(main())
