#!/usr/bin/env python3
"""SMP over CAN bench client for EerieLeap units.

Sends SMP requests to one unit through any CAN adapter python-can supports, using the framing of
the CDMP README, section 5, and prints the responses. The client takes a CDMP device ID of its
own (--source, 254 by default) that no unit on the bus may use.

Requires python-can and cbor2.

Examples:
    smp_can_client.py --channel can0 --target 1 info
    smp_can_client.py --channel can0 --target 1 config-read 3 canbus.cbor
    smp_can_client.py --interface pcan --channel PCAN_USBBUS1 --bitrate 1000000 --target 1 bench
    smp_can_client.py --channel can0 --fd --data-bitrate 2000000 --target 1 echo hello
"""

from __future__ import annotations

import argparse
import random
import struct
import sys
import time
import zlib
from enum import IntEnum
from pathlib import Path

import can
import cbor2

DEFAULT_BASE = 0x1FF00000
DEFAULT_SOURCE = 0xFE
START_FLAG = 0x80
SEQUENCE_MASK = 0x7F
CAN_FD_LENGTHS = (12, 16, 20, 24, 32, 48, 64)
CLASSIC_FRAME_BIT_TIMES = 160
CAN_FD_NOMINAL_BIT_TIMES = 60
CAN_FD_DATA_BIT_TIMES = 700
SMP_HEADER_SIZE = 8
REASSEMBLY_TIMEOUT_S = 0.1
# Room for the header and the keys around `data` in a config write request.
CONFIG_WRITE_OVERHEAD = 64


class Op(IntEnum):
    READ = 0
    READ_RESPONSE = 1
    WRITE = 2
    WRITE_RESPONSE = 3


class Group(IntEnum):
    OS = 0
    CONFIG = 64
    DEVICE = 65
    NETWORK = 66
    LIVE = 67


CONFIG_ERRORS = {
    2: "unknown type",
    3: "too large",
    4: "bad offset",
    5: "bad token",
    6: "busy",
    7: "CRC mismatch",
    8: "apply failed",
    9: "no memory",
}
CONFIG_BAD_OFFSET = 4


class SmpError(Exception):
    def __init__(self, rc: int, group: int | None, response: dict):
        self.rc = rc
        self.group = group
        self.response = response
        reason = CONFIG_ERRORS.get(rc, "") if group == Group.CONFIG else ""
        where = f"group {group} error" if group is not None else "MCUmgr error"
        super().__init__(f"{where} {rc}" + (f" ({reason})" if reason else ""))


def padded_size(size: int, is_can_fd: bool) -> int:
    if not is_can_fd or size <= 8:
        return size
    return next(length for length in CAN_FD_LENGTHS if size <= length)


def encode_frames(packet: bytes, is_can_fd: bool) -> list[bytes]:
    """Splits a packet into SMP-CAN frames: `[S | seq][data]`, the last one padded on CAN FD."""
    data_size = 63 if is_can_fd else 7
    frames = []
    for index, offset in enumerate(range(0, len(packet), data_size)):
        control = (START_FLAG if offset == 0 else 0) | (index & SEQUENCE_MASK)
        frame = bytes([control]) + packet[offset:offset + data_size]
        frames.append(frame + bytes(padded_size(len(frame), is_can_fd) - len(frame)))
    return frames


class Reassembler:
    """Rebuilds one source's packets from frames; a gap, a stray frame or a timeout drops the packet."""

    def __init__(self) -> None:
        self._buffer: bytearray | None = None
        self._sequence = 0
        self._last_s = 0.0

    def accept(self, data: bytes, now_s: float) -> bytes | None:
        if self._buffer is not None and now_s - self._last_s > REASSEMBLY_TIMEOUT_S:
            self._buffer = None
        if not data:
            return None

        control = data[0]
        if control & START_FLAG:
            self._buffer = bytearray()
            self._sequence = 0
        if self._buffer is None or control & SEQUENCE_MASK != self._sequence:
            self._buffer = None
            return None

        self._buffer += data[1:]
        self._sequence = (self._sequence + 1) & SEQUENCE_MASK
        self._last_s = now_s

        if len(self._buffer) < SMP_HEADER_SIZE:
            return None
        size = SMP_HEADER_SIZE + int.from_bytes(self._buffer[2:4], "big")
        if len(self._buffer) < size:
            return None

        # CAN FD padding follows the packet.
        packet = bytes(self._buffer[:size])
        self._buffer = None
        return packet


class SmpCanLink:
    """Sends packets to one unit, paced to a share of the bus, and receives its packets to us."""

    def __init__(self, bus: can.BusABC, *, target: int, source: int, base: int, is_can_fd: bool,
                 bitrate: int, data_bitrate: int, share_percent: int) -> None:
        self._bus = bus
        self._is_can_fd = is_can_fd
        self._tx_id = base | target << 8 | source
        self._rx_id = base | source << 8 | target
        self._reassembler = Reassembler()

        frame_time_s = (CAN_FD_NOMINAL_BIT_TIMES / bitrate + CAN_FD_DATA_BIT_TIMES / data_bitrate
                        if is_can_fd else CLASSIC_FRAME_BIT_TIMES / bitrate)
        self._frame_interval_s = frame_time_s * 100 / share_percent
        self._next_send_s = 0.0

        bus.set_filters([{"can_id": self._rx_id, "can_mask": 0x1FFFFFFF, "extended": True}])

    def send(self, packet: bytes) -> None:
        for data in encode_frames(packet, self._is_can_fd):
            self._pace()
            message = can.Message(arbitration_id=self._tx_id, is_extended_id=True, data=data,
                                  is_fd=self._is_can_fd, bitrate_switch=self._is_can_fd)
            self._send_frame(message)

    def receive(self, timeout_s: float) -> bytes | None:
        deadline_s = time.monotonic() + timeout_s
        while (remaining_s := deadline_s - time.monotonic()) > 0:
            message = self._bus.recv(remaining_s)
            if message is None:
                return None
            if not message.is_extended_id or message.arbitration_id != self._rx_id:
                continue
            packet = self._reassembler.accept(bytes(message.data), time.monotonic())
            if packet is not None:
                return packet
        return None

    def _pace(self) -> None:
        now_s = time.monotonic()
        self._next_send_s = max(self._next_send_s, now_s)
        # Sleeping for less than a millisecond is mostly scheduler noise.
        if self._next_send_s - now_s > 0.001:
            time.sleep(self._next_send_s - now_s)
        self._next_send_s += self._frame_interval_s

    def _send_frame(self, message: can.Message) -> None:
        deadline_s = time.monotonic() + 1.0
        while True:
            try:
                self._bus.send(message, timeout=0.1)
                return
            except can.CanOperationError:
                if time.monotonic() > deadline_s:
                    raise
                time.sleep(0.001)


class SmpClient:
    def __init__(self, link: SmpCanLink, timeout_s: float, retries: int) -> None:
        self._link = link
        self._timeout_s = timeout_s
        self._retries = retries
        self._sequence = random.randrange(256)

    def read(self, group: int, command: int, body: dict | None = None) -> dict:
        return self.request(Op.READ, group, command, body)

    def write(self, group: int, command: int, body: dict | None = None) -> dict:
        return self.request(Op.WRITE, group, command, body)

    def request(self, op: Op, group: int, command: int, body: dict | None) -> dict:
        payload = cbor2.dumps(body or {})
        self._sequence = (self._sequence + 1) & 0xFF
        # SMP v2: version 1 in bits 3-4 of the first byte.
        header = struct.pack(">BBHHBB", op | 1 << 3, 0, len(payload), group, self._sequence, command)

        for _ in range(self._retries + 1):
            self._link.send(header + payload)
            response = self._wait_for_response(group, command)
            if response is not None:
                return response

        raise TimeoutError(f"no response to group {group} command {command}")

    def _wait_for_response(self, group: int, command: int) -> dict | None:
        deadline_s = time.monotonic() + self._timeout_s
        while (remaining_s := deadline_s - time.monotonic()) > 0:
            packet = self._link.receive(remaining_s)
            if packet is None:
                return None

            _, _, length, response_group, sequence, response_command = struct.unpack(">BBHHBB", packet[:8])
            if (sequence, response_group, response_command) != (self._sequence, group, command):
                continue

            response = cbor2.loads(packet[8:8 + length]) if length else {}
            if response.get("rc", 0):
                raise SmpError(response["rc"], None, response)
            error = response.get("err")
            if error and error.get("rc", 0):
                raise SmpError(error["rc"], error.get("group"), response)
            return response
        return None


def unpack_version(version: int) -> str:
    return f"{version >> 24}.{version >> 16 & 0xFF}.{version & 0xFFFF}"


def max_chunk_size(client: SmpClient) -> int:
    params = client.read(Group.OS, 6)
    return params["buf_size"] - CONFIG_WRITE_OVERHEAD


def command_echo(client: SmpClient, args: argparse.Namespace) -> None:
    print(client.write(Group.OS, 0, {"d": args.text})["r"])


def command_params(client: SmpClient, args: argparse.Namespace) -> None:
    print(client.read(Group.OS, 6))


def command_info(client: SmpClient, args: argparse.Namespace) -> None:
    info = client.read(Group.DEVICE, 0)
    for key in ("hw", "sw"):
        info[key] = unpack_version(info[key])
    for key, value in info.items():
        print(f"{key}: {value:#010x}" if key == "uid" else f"{key}: {value}")


def command_devices(client: SmpClient, args: argparse.Namespace) -> None:
    offset = 0
    while True:
        response = client.read(Group.NETWORK, 0, {"off": offset})
        for device in response["devices"]:
            print(f"id {device['id']:3}  uid {device['uid']:#010x}  type {device['type']}  status {device['status']}")
        offset += len(response["devices"])
        if not response["devices"] or offset >= response["total"]:
            return


def command_config_list(client: SmpClient, args: argparse.Namespace) -> None:
    for entry in client.read(Group.CONFIG, 0)["types"]:
        print(f"type {entry['type']}  max {entry['max']} B")


def command_config_crc(client: SmpClient, args: argparse.Namespace) -> None:
    response = client.read(Group.CONFIG, 1, {"type": args.type})
    print(f"type {response['type']}  len {response['len']}  crc {response['crc']:#010x}")


def command_config_read(client: SmpClient, args: argparse.Namespace) -> None:
    data = bytearray()
    length, crc = None, None
    while length is None or len(data) < length:
        response = client.read(Group.CONFIG, 2, {"type": args.type, "off": len(data)})
        if response["off"] == 0:
            length, crc = response["len"], response["crc"]
        if not response["data"] and len(data) < length:
            raise RuntimeError(f"empty chunk at offset {len(data)}")
        data += response["data"]

    if zlib.crc32(data) != crc:
        raise RuntimeError(f"CRC mismatch: got {zlib.crc32(data):#010x}, expected {crc:#010x}")

    args.file.write_bytes(data)
    print(f"read {len(data)} B, crc {crc:#010x}")


def command_config_write(client: SmpClient, args: argparse.Namespace) -> None:
    data = args.file.read_bytes()
    crc = zlib.crc32(data)
    chunk_size = max_chunk_size(client)

    first = data[:chunk_size]
    response = client.write(Group.CONFIG, 3, {"type": args.type, "off": 0, "len": len(data), "crc": crc, "data": first})
    token, offset = response["tok"], response["off"]

    while offset < len(data):
        chunk = data[offset:offset + chunk_size]
        try:
            response = client.write(Group.CONFIG, 3, {"type": args.type, "off": offset, "tok": token, "data": chunk})
        except SmpError as error:
            # A lost response: the unit reports where it expects to continue.
            if error.group != Group.CONFIG or error.rc != CONFIG_BAD_OFFSET or "off" not in error.response:
                raise
            offset = error.response["off"]
            continue
        offset = response["off"]

    print(f"wrote {len(data)} B, crc {crc:#010x}")


def command_bench(client: SmpClient, args: argparse.Namespace) -> None:
    text = "".join(chr(ord("a") + index % 26) for index in range(args.size))
    timeouts = 0
    start_s = time.monotonic()
    for _ in range(args.count):
        try:
            if client.write(Group.OS, 0, {"d": text})["r"] != text:
                raise RuntimeError("echo differs")
        except TimeoutError:
            timeouts += 1
    elapsed_s = time.monotonic() - start_s

    completed = args.count - timeouts
    print(f"{completed}/{args.count} echoes of {args.size} B in {elapsed_s:.2f} s: "
          f"{completed / elapsed_s:.1f} req/s, {completed * args.size / elapsed_s / 1024:.2f} KB/s each way, "
          f"{timeouts} timeouts")


def parse_args(argv: list[str]) -> argparse.Namespace:
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--interface", default="socketcan", help="python-can interface (default: socketcan)")
    parser.add_argument("--channel", default="can0", help="python-can channel (default: can0)")
    parser.add_argument("--bitrate", type=int, default=500000, help="nominal bitrate, also used for pacing")
    parser.add_argument("--fd", action="store_true", help="send CAN FD frames with bitrate switching")
    parser.add_argument("--data-bitrate", type=int, default=2000000, help="CAN FD data bitrate")
    parser.add_argument("--base", type=lambda value: int(value, 0), default=DEFAULT_BASE, help="SMP ID base")
    parser.add_argument("--target", type=int, required=True, help="CDMP device ID of the unit")
    parser.add_argument("--source", type=int, default=DEFAULT_SOURCE, help="CDMP device ID of this client")
    parser.add_argument("--share", type=int, default=25, help="bus share in percent for the sent frames")
    parser.add_argument("--timeout", type=float, default=2.0, help="seconds to wait for a response")
    parser.add_argument("--retries", type=int, default=1, help="resends after a timeout")

    commands = parser.add_subparsers(dest="command", required=True)
    commands.add_parser("echo", help="os echo").add_argument("text")
    commands.add_parser("params", help="os mcumgr params: buffer size and count")
    commands.add_parser("info", help="device info")
    commands.add_parser("devices", help="network devices, as the unit sees them")
    commands.add_parser("config-list", help="configuration types and their size caps")
    for name in ("config-crc", "config-read", "config-write"):
        sub = commands.add_parser(name, help=name.replace("-", " "))
        sub.add_argument("type", type=int, help="configuration type")
        if name != "config-crc":
            sub.add_argument("file", type=Path)
    bench = commands.add_parser("bench", help="echo round trips, for throughput")
    bench.add_argument("--size", type=int, default=256, help="echo payload in bytes")
    bench.add_argument("--count", type=int, default=50)

    args = parser.parse_args(argv)
    for name in ("target", "source"):
        if not 1 <= getattr(args, name) <= 254:
            parser.error(f"--{name} must be a CDMP device ID, 1-254")
    if args.target == args.source:
        parser.error("--target and --source must differ")
    if not 1 <= args.share <= 100:
        parser.error("--share must be 1-100")
    return args


HANDLERS = {
    "echo": command_echo,
    "params": command_params,
    "info": command_info,
    "devices": command_devices,
    "config-list": command_config_list,
    "config-crc": command_config_crc,
    "config-read": command_config_read,
    "config-write": command_config_write,
    "bench": command_bench,
}


def main(argv: list[str]) -> int:
    args = parse_args(argv)

    bus_options = {"interface": args.interface, "channel": args.channel, "bitrate": args.bitrate}
    if args.fd:
        bus_options |= {"fd": True, "data_bitrate": args.data_bitrate}

    with can.Bus(**bus_options) as bus:
        link = SmpCanLink(bus, target=args.target, source=args.source, base=args.base, is_can_fd=args.fd,
                          bitrate=args.bitrate, data_bitrate=args.data_bitrate, share_percent=args.share)
        client = SmpClient(link, args.timeout, args.retries)
        try:
            HANDLERS[args.command](client, args)
        except (SmpError, TimeoutError, RuntimeError) as error:
            print(f"error: {error}", file=sys.stderr)
            return 1
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
