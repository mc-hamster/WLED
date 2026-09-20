#!/usr/bin/env python3
"""
Minimal client for the WLED BLE API Bridge usermod.

Examples:
  python3 client_example.py --address AA:BB:CC:DD:EE:FF get /json/state
  python3 client_example.py --address AA:BB:CC:DD:EE:FF post /json/state '{"on":true,"bri":128}'
  python3 client_example.py --address AA:BB:CC:DD:EE:FF post /json/cfg '{"hw":{"led":{"total":60}}}'
"""

from __future__ import annotations

import argparse
import asyncio
from dataclasses import dataclass
from typing import Optional

from bleak import BleakClient

SERVICE_UUID = "7c2e0001-5d2b-4fd0-b1c2-0cc8f5470101"
RX_UUID = "7c2e0002-5d2b-4fd0-b1c2-0cc8f5470101"
TX_UUID = "7c2e0003-5d2b-4fd0-b1c2-0cc8f5470101"


@dataclass
class BridgeResponse:
    status: int
    content_type: str
    body: str


class BleApiBridgeClient:
    def __init__(self, address: str, chunk_size: int = 180, timeout: float = 10.0):
        self.address = address
        self.chunk_size = chunk_size
        self.timeout = timeout
        self.client = BleakClient(address, services=[SERVICE_UUID])
        self._response_event = asyncio.Event()
        self._expected_len: Optional[int] = None
        self._buffer = bytearray()

    async def connect(self, pair: bool = False) -> None:
        await self.client.connect()
        if pair and hasattr(self.client, "pair"):
            try:
                await self.client.pair()
            except Exception:
                pass
        await self.client.start_notify(TX_UUID, self._on_tx)

    async def disconnect(self) -> None:
        if self.client.is_connected:
            try:
                await self.client.stop_notify(TX_UUID)
            except Exception:
                pass
            await self.client.disconnect()

    def _reset_response(self) -> None:
        self._expected_len = None
        self._buffer.clear()
        self._response_event.clear()

    def _on_tx(self, _characteristic, data: bytearray) -> None:
        if self._expected_len is None:
            if len(data) < 2:
                return
            self._expected_len = int.from_bytes(data[:2], "little")
            self._buffer.extend(data[2:])
        else:
            self._buffer.extend(data)

        if self._expected_len is not None and len(self._buffer) >= self._expected_len:
            self._buffer = self._buffer[: self._expected_len]
            self._response_event.set()

    async def request(self, method: str, path: str, body: str = "") -> BridgeResponse:
        payload = f"{method.upper()} {path}\n\n{body}".encode("utf-8")
        if len(payload) > 0xFFFF:
            raise ValueError("request too large for bridge framing")

        self._reset_response()

        first_chunk_budget = max(self.chunk_size - 2, 1)
        first = len(payload).to_bytes(2, "little") + payload[:first_chunk_budget]
        await self.client.write_gatt_char(RX_UUID, first, response=True)

        offset = first_chunk_budget
        while offset < len(payload):
            chunk = payload[offset : offset + self.chunk_size]
            await self.client.write_gatt_char(RX_UUID, chunk, response=True)
            offset += len(chunk)

        await asyncio.wait_for(self._response_event.wait(), timeout=self.timeout)
        return self._parse_response(bytes(self._buffer))

    @staticmethod
    def _parse_response(raw: bytes) -> BridgeResponse:
        text = raw.decode("utf-8")
        header, body = text.split("\n\n", 1)
        status_text, content_type = header.split(" ", 1)
        return BridgeResponse(status=int(status_text), content_type=content_type.strip(), body=body)


async def main() -> None:
    parser = argparse.ArgumentParser()
    parser.add_argument("--address", required=True, help="BLE MAC or platform BLE address")
    parser.add_argument("--pair", action="store_true", help="attempt pairing before use")
    parser.add_argument("--chunk-size", type=int, default=180, help="write chunk size")
    parser.add_argument("--timeout", type=float, default=10.0, help="response timeout in seconds")
    parser.add_argument("method", choices=["get", "post"])
    parser.add_argument("path")
    parser.add_argument("body", nargs="?", default="")
    args = parser.parse_args()

    bridge = BleApiBridgeClient(args.address, chunk_size=args.chunk_size, timeout=args.timeout)
    try:
        await bridge.connect(pair=args.pair)
        response = await bridge.request(args.method, args.path, args.body)
        print(f"status: {response.status}")
        print(f"content-type: {response.content_type}")
        print(response.body)
    finally:
        await bridge.disconnect()


if __name__ == "__main__":
    asyncio.run(main())
