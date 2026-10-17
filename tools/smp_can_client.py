#!/usr/bin/env python3
"""SMP bench client for EerieLeap units, over CAN or over a unit's console.

Sends SMP requests to one unit and prints the responses. Two transports:

* CAN: any CAN adapter python-can supports, with the framing of the CDMP README, section 5. The
  client takes a CDMP device ID of its own (--source, 254 by default) that no unit on the bus may
  use. Selected by --target.
* Serial: the unit's console, as MCUmgr's shell transport frames it (lines starting 0x06 0x09 and
  0x04 0x14, base64, CRC16), among the console's log lines. Only a bench build answers there
  (app/bench.conf of the gauge and of rt_daq). Selected by --serial. Opening the port does not
  reset an ESP32-S3: Linux raises DTR and RTS when it opens a USB-Serial-JTAG port, and the chip is
  held in reset while RTS is high and DTR low, so the lines are put through RTS 0 first.

Configurations can be written and read as YAML or JSON descriptions (config-get, config-set,
config-encode, config-decode), encoded against the CDDL schemas in this checkout; the format is in
tools/README.md. Failed SMP requests are resent: once on CAN, three times on serial, where a log
line landing in a response corrupts it. The request that completes a configuration write is never
resent blindly; the client reads the stored CRC to learn whether it was applied.

Asset files (images, animations, scripts, logs) are listed, downloaded, uploaded and deleted with the
assets commands. A download keeps several requests in flight, can continue a partial file and checks
the file's CRC32 against the unit's; the format is in tools/README.md.

Requires cbor2; python-can for CAN, pyserial for serial, PyYAML and zcbor for descriptions.

Examples:
    smp_can_client.py --channel can0 --target 1 info
    smp_can_client.py --channel can0 --target 1 config-read 3 canbus.cbor
    smp_can_client.py --interface pcan --channel PCAN_USBBUS1 --bitrate 1000000 --target 1 bench
    smp_can_client.py --channel can0 --fd --data-bitrate 2000000 --target 1 echo hello
    smp_can_client.py --channel can0 --target 1 config-get canbus
    smp_can_client.py --serial /dev/ttyACM0 info
    smp_can_client.py --serial /dev/ttyACM0 config-get sensors daq_sensors.yaml
    smp_can_client.py --serial /dev/ttyACM0 config-set logging_control triggers.yaml
    smp_can_client.py --serial /dev/ttyACM0 shell "sensor set bench_switch 1"
    smp_can_client.py --channel can0 --target 1 assets kinds
    smp_can_client.py --channel can0 --target 1 assets ls logs
    smp_can_client.py --channel can0 --target 1 assets get logs log_data_0001.mf4 --resume --in-flight 2
    smp_can_client.py --serial /dev/ttyACM0 assets put images logo.bin ./logo.bin
    smp_can_client.py config-decode sensors sensors_config.cbor
"""

from __future__ import annotations

import argparse
import base64
import binascii
import collections
import contextlib
import dataclasses
import importlib
import io
import json
import math
import random
import re
import struct
import sys
import time
import types
import zlib
from enum import IntEnum
from pathlib import Path
from typing import TYPE_CHECKING, Callable, Iterator

import cbor2

if TYPE_CHECKING:
    import can

REPO = Path(__file__).resolve().parent.parent

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
# The same for an assets write request, at its worst, without the name: the header, the map head, kind,
# the key of `name`, off, len, crc and tok (a text key and an unsigned 32-bit integer each) and the key
# and head of `data`. The name adds its head and characters, and counts as at least the unit's longest
# name (EERIE_LEAP_ASSETS_MAX_NAME_LENGTH), so that the chunks of an upload have one size for any name.
ASSET_NAME_LIMIT = 64
ASSET_WRITE_FIXED_OVERHEAD = SMP_HEADER_SIZE + 1 + (5 + 5) + 5 + 4 * (4 + 5) + (5 + 5)
ASSET_WRITE_OVERHEAD = ASSET_WRITE_FIXED_OVERHEAD + 2 + ASSET_NAME_LIMIT

# MCUmgr's shell transport: MCUMGR_SERIAL_HDR_PKT and _FRAG, and MCUMGR_SERIAL_MAX_FRAME, which is also
# the size of the unit's receive buffer for one line.
SERIAL_PACKET_MARKER = b"\x06\x09"
SERIAL_FRAGMENT_MARKER = b"\x04\x14"
SERIAL_MAX_LINE = 127
# The length and the CRC16 around a packet count against the unit's buffer, as the packet does.
SERIAL_FRAME_OVERHEAD = 4
# Raw bytes per line: 124 base64 characters between the marker and the newline. A multiple of 3,
# so that only the last line carries padding.
SERIAL_LINE_DATA = (SERIAL_MAX_LINE - 3) // 4 * 3
SERIAL_BASE64 = re.compile(rb"[A-Za-z0-9+/=]*")
DEFAULT_BAUD = 115200
ANSI_ESCAPE = re.compile(r"\x1b\[[0-9;?]*[A-Za-z]")

DEFAULT_CAN_RETRIES = 1
DEFAULT_SERIAL_RETRIES = 3
# The request that completes a configuration write answers after the unit applied it, and applying
# restarts services that log; it gets this long, once.
FINAL_REQUEST_TIMEOUT_S = 15.0
# EERIE_LEAP_SMP_CONFIG_SESSION_TIMEOUT_MS: an idle transfer is dropped after this.
CONFIG_SESSION_TIMEOUT_S = 5.0
# How long to look for a write whose answer got lost.
CONFIRM_WINDOW_S = 8.0

DEFAULT_ASSET_IN_FLIGHT = 2
MAX_ASSET_IN_FLIGHT = 255
# How often a download that is stuck at one byte asks for it before it gives up.
MAX_ASSET_RESTARTS = 16
# EERIE_LEAP_SMP_ASSETS_SESSION_TIMEOUT_MS: an idle download or upload is dropped after this, and an
# upload that was stored answers its last chunk again for this long.
ASSET_SESSION_TIMEOUT_S = 5.0
# A download continued at an offset makes the unit read the file up to there to seed its CRC32 before
# it answers; this is the least the first request waits for then.
ASSET_SEED_TIMEOUT_S = 30.0


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
    CALIBRATION = 68
    FILES = 69
    ASSETS = 70


OS_RESET = 5

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
CONFIG_BAD_TOKEN = 5
CONFIG_BUSY = 6

ASSETS_ERRORS = {
    1: "unknown error",
    2: "unknown kind",
    3: "unavailable: the volume is not mounted",
    4: "not found",
    5: "bad name",
    6: "denied: the kind does not allow this",
    7: "in use",
    8: "too large",
    9: "no space",
    10: "bad offset",
    11: "bad token",
    12: "busy: another upload is open",
    13: "CRC mismatch",
    14: "I/O error",
    15: "no memory",
}
ASSETS_BAD_OFFSET = 10
ASSETS_BAD_TOKEN = 11
ASSETS_BUSY = 12
ASSETS_NOT_FOUND = 4

GROUP_ERRORS = {Group.CONFIG: CONFIG_ERRORS, Group.ASSETS: ASSETS_ERRORS}
# MCUmgr's MGMT_ERR_ENOTSUP, which a unit answers at the top level for a group it does not serve.
MGMT_ERR_ENOTSUP = 8


class AssetsCommand(IntEnum):
    KINDS = 0
    LIST = 1
    READ = 2
    WRITE = 3
    DELETE = 4


ASSET_KINDS = {"images": 0, "animations": 1, "sensor_scripts": 2, "can_scripts": 3, "logs": 4}


class SmpError(Exception):
    def __init__(self, rc: int, group: int | None, response: dict, request_group: int | None = None):
        self.rc = rc
        self.group = group
        self.response = response
        reason = GROUP_ERRORS.get(group, {}).get(rc, "")
        if group is None and rc == MGMT_ERR_ENOTSUP and request_group == Group.ASSETS:
            reason = "the unit has no assets group (older firmware)"
        where = f"group {group} error" if group is not None else "MCUmgr error"
        message = f"{where} {rc}" + (f" ({reason})" if reason else "")
        # APPLY_FAILED carries the validator's reason.
        if isinstance(response.get("msg"), str) and response["msg"]:
            message += f": {response['msg']}"
        super().__init__(message)

    def is_config_error(self, rc: int) -> bool:
        return self.group == Group.CONFIG and self.rc == rc

    def is_assets_error(self, rc: int) -> bool:
        return self.group == Group.ASSETS and self.rc == rc


def require(module: str, package: str) -> types.ModuleType:
    """Imports an optional dependency, naming the package to install when it is missing."""
    try:
        return importlib.import_module(module)
    except ImportError as error:
        raise RuntimeError(f"this command needs {package}: pip install {package} ({error})") from None


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
        self._can = require("can", "python-can")
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
            message = self._can.Message(arbitration_id=self._tx_id, is_extended_id=True, data=data,
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
            except self._can.CanOperationError:
                if time.monotonic() > deadline_s:
                    raise
                time.sleep(0.001)


def encode_serial_lines(packet: bytes) -> list[bytes]:
    """Frames a packet for MCUmgr's shell transport: the length, the packet and its CRC16, in lines."""
    # The CRC covers the packet, not the length; the length counts the packet and the CRC.
    body = struct.pack(">H", len(packet) + 2) + packet + struct.pack(">H", binascii.crc_hqx(packet, 0))
    lines = []
    for offset in range(0, len(body), SERIAL_LINE_DATA):
        marker = SERIAL_PACKET_MARKER if offset == 0 else SERIAL_FRAGMENT_MARKER
        lines.append(marker + base64.b64encode(body[offset:offset + SERIAL_LINE_DATA]) + b"\n")
    return lines


class SerialFrameDecoder:
    """Separates SMP frames from console text and rebuilds packets; a bad line drops its packet.

    The unit writes a response byte by byte, so a log line from another thread can land inside it.
    Such a line fails the base64 check and its packet is dropped; the client's retry recovers it.
    """

    def __init__(self, on_console_line: Callable[[str], None] | None = None) -> None:
        self._on_console_line = on_console_line or (lambda line: None)
        self._buffer = b""
        self._marker: bytes | None = None       # The marker of the line being read, or None in console text.
        self._text = b""                        # The console line being assembled.
        self._packet: bytearray | None = None
        self._expected = 0
        self.corrupt_lines = 0
        self.dropped_packets = 0

    def feed(self, data: bytes) -> list[bytes]:
        """Returns the SMP packets (header included, CRC removed) completed by @p data."""
        packets: list[bytes] = []
        self._buffer += data

        while True:
            if self._marker is None:
                start = self._find_marker(self._buffer)
                if start < 0:
                    # A marker can straddle two reads; hold back its first byte.
                    keep = 1 if self._buffer[-1:] in (b"\x06", b"\x04") else 0
                    self._emit_text(self._buffer[:len(self._buffer) - keep])
                    self._buffer = self._buffer[len(self._buffer) - keep:]
                    return packets
                self._emit_text(self._buffer[:start])
                self._marker = self._buffer[start:start + 2]
                self._buffer = self._buffer[start + 2:]
                continue

            newline = self._buffer.find(b"\n")
            next_marker = self._find_marker(self._buffer)
            if next_marker >= 0 and (newline < 0 or next_marker < newline):
                # The line was cut by another frame: what is before it is corrupt, the frame is not.
                self._corrupt()
                self._buffer = self._buffer[next_marker:]
                self._marker = None
                continue
            if newline < 0:
                if len(self._buffer) > 4 * SERIAL_MAX_LINE:
                    self._corrupt()
                    self._buffer = b""
                    self._marker = None
                return packets

            body = self._buffer[:newline].rstrip(b"\r")
            marker = self._marker
            self._buffer = self._buffer[newline + 1:]
            self._marker = None
            packet = self._accept_line(marker, body)
            if packet is not None:
                packets.append(packet)

    def flush_text(self) -> None:
        """Hands over the console line still being assembled, such as a prompt without a newline."""
        if self._text:
            self._deliver(self._text)
            self._text = b""

    @staticmethod
    def _find_marker(data: bytes) -> int:
        positions = [position for position in (data.find(SERIAL_PACKET_MARKER), data.find(SERIAL_FRAGMENT_MARKER))
                     if position >= 0]
        return min(positions) if positions else -1

    def _emit_text(self, text: bytes) -> None:
        self._text += text
        while (newline := self._text.find(b"\n")) >= 0:
            self._deliver(self._text[:newline])
            self._text = self._text[newline + 1:]

    def _deliver(self, line: bytes) -> None:
        self._on_console_line(line.decode("utf-8", errors="replace").rstrip("\r"))

    def _corrupt(self) -> None:
        self.corrupt_lines += 1
        self._drop_packet()

    def _drop_packet(self) -> None:
        if self._packet is not None:
            self.dropped_packets += 1
        self._packet = None

    def _accept_line(self, marker: bytes, body: bytes) -> bytes | None:
        if not SERIAL_BASE64.fullmatch(body):
            self._corrupt()
            # What follows the base64 prefix of the cut line is the log text that cut it; its tail is
            # the next line and stays console text too, since a line of base64 characters can be real.
            self._deliver(body[SERIAL_BASE64.match(body).end():])
            return None
        try:
            raw = base64.b64decode(body, validate=True)
        except (binascii.Error, ValueError):
            self._corrupt()
            return None

        if marker == SERIAL_PACKET_MARKER:
            self._drop_packet()
            if len(raw) < 2:
                self.corrupt_lines += 1
                return None
            self._expected = int.from_bytes(raw[:2], "big")
            self._packet = bytearray(raw[2:])
        elif self._packet is None:
            # The start of this packet was lost.
            return None
        else:
            self._packet += raw

        if len(self._packet) < self._expected:
            return None

        packet, self._packet = bytes(self._packet), None
        if len(packet) != self._expected or self._expected <= 2 or binascii.crc_hqx(packet, 0) != 0:
            self.corrupt_lines += 1
            self.dropped_packets += 1
            return None
        return packet[:-2]


def open_serial_port(serial_module: types.ModuleType, name: str, baud: int):
    """Opens a unit's console without resetting an ESP32-S3.

    Linux raises DTR and RTS in one request when it opens a CDC ACM port, and pyserial then applies
    its DTR preset before its RTS preset. The USB-Serial-JTAG holds the chip in reset while RTS is
    high and DTR low, so presetting both low passes through exactly that state. DTR high and RTS low
    goes (1,1) -> (1,0), and DTR is lowered after the open: (0,0), never RTS high with DTR low.
    """
    port = serial_module.Serial()
    port.port, port.baudrate, port.timeout, port.write_timeout = name, baud, 0.05, 2.0
    port.exclusive = True
    port.dtr, port.rts = True, False
    port.open()
    try:
        port.dtr = False
    except (OSError, serial_module.SerialException):
        pass  # A pty (native_sim) has no modem lines.
    port.reset_input_buffer()
    return port


class SmpSerialLink:
    """SMP over a unit's shell console (MCUMGR_TRANSPORT_SHELL), as a bench build serves it."""

    def __init__(self, port: str, baud: int = DEFAULT_BAUD, line_delay_s: float = 0.0,
                 show_console: bool = False) -> None:
        serial_module = require("serial", "pyserial")
        self._line_delay_s = line_delay_s
        self._show_console = show_console
        self._console: list[str] = []
        self._packets: collections.deque[bytes] = collections.deque()
        self._decoder = SerialFrameDecoder(self._on_console_line)
        self._port = open_serial_port(serial_module, port, baud)

    def __enter__(self) -> "SmpSerialLink":
        return self

    def __exit__(self, *exc_info) -> None:
        self.close()

    def close(self) -> None:
        self._port.close()
        if self._show_console and self._decoder.corrupt_lines:
            print(f"console: {self._decoder.corrupt_lines} corrupt line(s), "
                  f"{self._decoder.dropped_packets} dropped packet(s)", file=sys.stderr)

    def send(self, packet: bytes) -> None:
        for line in encode_serial_lines(packet):
            self._port.write(line)
            self._port.flush()
            if self._line_delay_s > 0:
                time.sleep(self._line_delay_s)

    def receive(self, timeout_s: float) -> bytes | None:
        deadline_s = time.monotonic() + timeout_s
        while True:
            if self._packets:
                return self._packets.popleft()
            if deadline_s - time.monotonic() <= 0:
                return None
            self._pump()

    def send_text(self, text: str) -> None:
        """Types a shell command line."""
        self._console.clear()
        self._port.write(text.encode() + b"\n")
        self._port.flush()

    def read_console(self, duration_s: float) -> list[str]:
        """Collects the console lines for @p duration_s; SMP frames are taken out."""
        deadline_s = time.monotonic() + duration_s
        while deadline_s - time.monotonic() > 0:
            self._pump()
        self._decoder.flush_text()
        lines, self._console = self._console, []
        return lines

    def _pump(self) -> None:
        data = self._port.read(max(1, self._port.in_waiting))
        if data:
            self._packets.extend(self._decoder.feed(data))

    def _on_console_line(self, line: str) -> None:
        line = ANSI_ESCAPE.sub("", line)
        self._console.append(line)
        if self._show_console:
            print(f"console: {line}", file=sys.stderr)


def encode_request(op: Op, group: int, command: int, body: dict | None, sequence: int) -> bytes:
    payload = cbor2.dumps(body or {})
    # SMP v2: version 1 in bits 3-4 of the first byte.
    header = struct.pack(">BBHHBB", op | 1 << 3, 0, len(payload), group, sequence, command)
    return header + payload


def unpack_header(packet: bytes) -> tuple[int, int, int, int]:
    """Returns the body length, group, sequence number and command of a packet."""
    _, _, length, group, sequence, command = struct.unpack(">BBHHBB", packet[:SMP_HEADER_SIZE])
    return length, group, sequence, command


def decode_response(packet: bytes, length: int, request_group: int) -> dict:
    """Decodes the body of a response and raises the error it carries."""
    response = cbor2.loads(packet[SMP_HEADER_SIZE:SMP_HEADER_SIZE + length]) if length else {}
    if response.get("rc", 0):
        raise SmpError(response["rc"], None, response, request_group)
    error = response.get("err")
    if error and error.get("rc", 0):
        raise SmpError(error["rc"], error.get("group"), response, request_group)
    return response


class SmpClient:
    def __init__(self, link: SmpCanLink | SmpSerialLink, timeout_s: float, retries: int) -> None:
        self._link = link
        self._timeout_s = timeout_s
        self._retries = retries
        self._sequence = random.randrange(256)

    @property
    def link(self) -> SmpCanLink | SmpSerialLink:
        return self._link

    @property
    def timeout_s(self) -> float:
        return self._timeout_s

    @property
    def retries(self) -> int:
        return self._retries

    def next_sequence(self) -> int:
        self._sequence = (self._sequence + 1) & 0xFF
        return self._sequence

    def read(self, group: int, command: int, body: dict | None = None, **options) -> dict:
        return self.request(Op.READ, group, command, body, **options)

    def write(self, group: int, command: int, body: dict | None = None, **options) -> dict:
        return self.request(Op.WRITE, group, command, body, **options)

    def request(self, op: Op, group: int, command: int, body: dict | None, *, retries: int | None = None,
                timeout_s: float | None = None) -> dict:
        """Sends a request and waits for its response.

        @p retries and @p timeout_s override the client's for one request: a request that is not
        idempotent is sent once.
        """
        retries = self._retries if retries is None else retries
        timeout_s = self._timeout_s if timeout_s is None else timeout_s
        packet = encode_request(op, group, command, body, self.next_sequence())

        for _ in range(retries + 1):
            self._link.send(packet)
            response = self._wait_for_response(group, command, timeout_s)
            if response is not None:
                return response

        raise TimeoutError(f"no response to group {group} command {command}")

    def _wait_for_response(self, group: int, command: int, timeout_s: float) -> dict | None:
        deadline_s = time.monotonic() + timeout_s
        while (remaining_s := deadline_s - time.monotonic()) > 0:
            packet = self._link.receive(remaining_s)
            if packet is None:
                return None

            length, response_group, sequence, response_command = unpack_header(packet)
            if (sequence, response_group, response_command) != (self._sequence, group, command):
                continue

            return decode_response(packet, length, group)
        return None


@dataclasses.dataclass
class PendingRequest:
    tag: object
    group: int
    command: int
    packet: bytes
    timeout_s: float
    resends_left: int
    deadline_s: float = 0.0


@dataclasses.dataclass
class Completion:
    """How a pipelined request ended: with a response, or with the error the unit or the wait produced."""
    tag: object
    response: dict | None = None
    error: SmpError | TimeoutError | None = None


class SmpPipeline:
    """Several requests of one client in flight at once, matched to their responses by sequence number.

    Whichever link carries them hands over one packet at a time (the CAN link the first one completed,
    the serial link the ones it queued), and a response is told by its sequence number, group and
    command, so the responses of the requests may arrive in any order. A request that gets no response
    within its timeout is sent again under its own sequence number, as many times as the client's
    retries, and then completes with a TimeoutError. A late response to a request that is not
    outstanding any more is dropped. The window is the caller's to keep: sequence numbers are one byte.
    """

    def __init__(self, client: SmpClient) -> None:
        self._client = client
        self._pending: dict[int, PendingRequest] = {}

    @property
    def outstanding(self) -> int:
        return len(self._pending)

    def submit(self, op: Op, group: int, command: int, body: dict | None, tag: object, *,
               retries: int | None = None, timeout_s: float | None = None) -> None:
        """Sends a request. @p tag comes back with its completion."""
        if len(self._pending) > 0xFF:
            raise ValueError("too many requests in flight")
        sequence = self._client.next_sequence()
        while sequence in self._pending:
            sequence = self._client.next_sequence()

        request = PendingRequest(
            tag, group, command, encode_request(op, group, command, body, sequence),
            self._client.timeout_s if timeout_s is None else timeout_s,
            self._client.retries if retries is None else retries)
        self._pending[sequence] = request
        self._send(request)

    def next_completion(self) -> Completion:
        """Waits until one of the outstanding requests ends, and returns how."""
        if not self._pending:
            raise RuntimeError("no request is in flight")

        while True:
            wait_s = min(request.deadline_s for request in self._pending.values()) - time.monotonic()
            # A response that is there already wins over a deadline that just passed.
            packet = self._client.link.receive(max(wait_s, 0.001))
            if packet is not None:
                completion = self._accept(packet)
                if completion is not None:
                    return completion
            completion = self._expire(time.monotonic())
            if completion is not None:
                return completion

    def _send(self, request: PendingRequest) -> None:
        self._client.link.send(request.packet)
        request.deadline_s = time.monotonic() + request.timeout_s

    def _accept(self, packet: bytes) -> Completion | None:
        if len(packet) < SMP_HEADER_SIZE:
            return None
        length, group, sequence, command = unpack_header(packet)
        request = self._pending.get(sequence)
        if request is None or (group, command) != (request.group, request.command):
            return None

        try:
            completion = Completion(request.tag, response=decode_response(packet, length, group))
        except SmpError as error:
            completion = Completion(request.tag, error=error)
        except cbor2.CBORDecodeError:
            return None  # As good as lost: its deadline sends the request again.
        del self._pending[sequence]
        return completion

    def _expire(self, now_s: float) -> Completion | None:
        for sequence, request in list(self._pending.items()):
            if request.deadline_s > now_s:
                continue
            if request.resends_left > 0:
                request.resends_left -= 1
                self._send(request)
                continue
            del self._pending[sequence]
            return Completion(request.tag,
                              error=TimeoutError(f"no response to group {request.group} command {request.command}"))
        return None


def unpack_version(version: int) -> str:
    return f"{version >> 24}.{version >> 16 & 0xFF}.{version & 0xFFFF}"


def max_chunk_size(client: SmpClient) -> int:
    params = client.read(Group.OS, 6)
    return params["buf_size"] - CONFIG_WRITE_OVERHEAD


def read_config(client: SmpClient, number: int) -> bytes:
    """Reads a stored configuration chunk by chunk and checks its CRC32.

    The unit releases its snapshot with the last chunk, so a last chunk whose response got lost
    cannot be asked for again: the read starts over once.
    """
    for attempt in range(2):
        data = bytearray()
        length, crc = None, None
        restart = False

        while length is None or len(data) < length:
            try:
                response = client.read(Group.CONFIG, 2, {"type": number, "off": len(data)})
            except SmpError as error:
                if attempt == 0 and data and error.is_config_error(CONFIG_BAD_OFFSET):
                    restart = True
                    break
                raise
            if response["off"] == 0:
                length, crc = response["len"], response["crc"]
            if not response["data"] and len(data) < length:
                raise RuntimeError(f"empty chunk at offset {len(data)}")
            data += response["data"]

        if restart:
            continue

        if zlib.crc32(data) != crc:
            raise RuntimeError(f"CRC mismatch: got {zlib.crc32(data):#010x}, expected {crc:#010x}")
        return bytes(data)

    raise RuntimeError(f"configuration {number} could not be read")


def is_stored(client: SmpClient, number: int, length: int, crc: int) -> bool:
    """Whether the unit stores these bytes and applied them: its length, CRC32 and applied flag say so."""
    response = client.read(Group.CONFIG, 1, {"type": number})
    return response["len"] == length and response["crc"] == crc and response.get("applied", True)


def stores_already(client: SmpClient, number: int, length: int, crc: int) -> bool:
    """Whether the unit already stores and runs these bytes, so that finding them later proves nothing."""
    try:
        return is_stored(client, number, length, crc)
    except SmpError:
        return False  # The write reports the unit's refusal itself.


def confirm_stored(client: SmpClient, number: int, length: int, crc: int) -> bool:
    """Looks for a write whose response got lost. The unit stores the bytes as received."""
    deadline_s = time.monotonic() + CONFIRM_WINDOW_S
    while True:
        try:
            if is_stored(client, number, length, crc):
                return True
        except TimeoutError:
            pass
        if time.monotonic() >= deadline_s:
            return False
        time.sleep(1.0)


class TransferLost(Exception):
    """A write whose outcome is unknown, or that the unit dropped; the transfer starts over."""


def write_chunks(client: SmpClient, number: int, data: bytes, crc: int, chunk_size: int,
                 can_confirm: bool = True) -> None:
    """Sends the chunks. @p can_confirm is false when the unit stored these bytes before the write."""
    offset = 0
    token = None

    while True:
        chunk = data[offset:offset + chunk_size]
        is_final = offset + len(chunk) >= len(data)
        body = {"type": number, "off": offset, "data": chunk}
        if offset == 0:
            body |= {"len": len(data), "crc": crc}
        else:
            body["tok"] = token

        # The final request makes the unit apply the configuration and answers afterwards. It is
        # sent once, with time to answer: a second one would stage and apply the bytes again, or
        # meet a closed session.
        options = {"retries": 0, "timeout_s": max(client.timeout_s, FINAL_REQUEST_TIMEOUT_S)} if is_final else {}
        try:
            response = client.write(Group.CONFIG, 3, body, **options)
        except TimeoutError:
            if not is_final:
                raise TransferLost(f"no response to the chunk at offset {offset} of configuration {number}") from None
            if can_confirm and confirm_stored(client, number, len(data), crc):
                return
            reason = "" if can_confirm else "; the unit stored these bytes already, so a match proves nothing"
            raise TransferLost(f"no response to the last chunk of configuration {number}{reason}") from None
        except SmpError as error:
            if error.is_config_error(CONFIG_BAD_OFFSET) and "off" in error.response:
                # A lost response: the unit reports where it expects to continue.
                offset = error.response["off"]
                continue
            if (error.is_config_error(CONFIG_BAD_TOKEN) and is_final and can_confirm
                    and confirm_stored(client, number, len(data), crc)):
                return
            if error.is_config_error(CONFIG_BAD_TOKEN) or error.is_config_error(CONFIG_BUSY):
                raise TransferLost(str(error)) from None
            raise

        if is_final:
            return
        if offset == 0:
            token = response["tok"]
        offset = response["off"]


def write_config(client: SmpClient, number: int, data: bytes) -> int:
    """Writes a configuration and returns its CRC32.

    A transfer the unit lost or dropped starts over once, after its session timed out.
    """
    crc = zlib.crc32(data)
    chunk_size = max_chunk_size(client)
    # A write that stores what the unit holds already cannot be told from one that was lost.
    can_confirm = not stores_already(client, number, len(data), crc)

    for attempt in range(2):
        try:
            write_chunks(client, number, data, crc, chunk_size, can_confirm)
            return crc
        except TransferLost as error:
            if attempt:
                raise RuntimeError(f"configuration {number} was not confirmed: {error}") from None
            time.sleep(CONFIG_SESSION_TIMEOUT_S + 0.5)

    raise AssertionError("unreachable")


def parse_asset_kind(text: str) -> int:
    """An asset kind by name or number; a number the table lacks passes through (the unit says what it serves)."""
    name = text.lower().replace("-", "_")
    if name in ASSET_KINDS:
        return ASSET_KINDS[name]
    try:
        kind = int(text, 0)
    except ValueError:
        kind = -1
    if kind < 0:
        names = ", ".join(ASSET_KINDS)
        raise argparse.ArgumentTypeError(f"{text!r} is not an asset kind number or one of: {names}")
    return kind


def format_access(access: int) -> str:
    """The AssetAccess bits as letters: R read, W write, D delete."""
    return "".join(letter if access & bit else "-" for letter, bit in (("R", 1), ("W", 2), ("D", 4)))


def format_kind(entry: dict) -> str:
    maximum = f"{entry['max']} B" if entry["max"] else "none"
    return (f"kind {entry['kind']}  {entry['name']}  dir {entry['dir']}  access {format_access(entry['access'])}  "
            f"max {maximum}  ext {' '.join(entry['ext']) or '-'}  avail {'yes' if entry['avail'] else 'no'}")


def asset_write_overhead(name: str) -> int:
    """The room around `data` in an assets write request for @p name, at its worst."""
    length = max(ASSET_NAME_LIMIT, len(name.encode()))
    return ASSET_WRITE_FIXED_OVERHEAD + len(encode_cbor_head(3, length)) + length


def asset_chunk_size(buf_size: int, name: str, is_serial: bool = False) -> int:
    """The data an assets write request for @p name can carry in a unit's packet of @p buf_size bytes.

    A request that does not fit is dropped by the unit's transport without an answer, so a name that
    leaves no room for data is refused here.
    """
    available = buf_size - (SERIAL_FRAME_OVERHEAD if is_serial else 0)
    size = available - asset_write_overhead(name)
    if size < 1:
        raise RuntimeError(f"the unit's packets of {available} B cannot carry an upload request for the name {name!r} "
                           f"({len(name.encode())} B); it needs more than {asset_write_overhead(name)} B")
    return size


def limit_window(requested: int, buf_count: int | None) -> int:
    """The requests to keep in flight: below the unit's packet count, since requests and responses share them."""
    if not buf_count or requested < buf_count:
        return requested
    return max(1, buf_count - 1)


def file_crc32(path: Path) -> int:
    crc = 0
    with path.open("rb") as file:
        while block := file.read(1 << 16):
            crc = zlib.crc32(block, crc)
    return crc


class Progress:
    """A transfer's progress, rewritten in place on stderr when that is a terminal and silent otherwise."""

    def __init__(self, label: str) -> None:
        self._label = label
        self._enabled = sys.stderr.isatty()
        self._last_s = 0.0
        self._width = 0

    def update(self, done: int, total: int | None) -> None:
        now_s = time.monotonic()
        if not self._enabled or now_s - self._last_s < 0.25:
            return
        self._last_s = now_s
        text = f"{self._label}: {done} B"
        if total is not None:
            text += f" / {total} B ({done * 100 // total if total else 100} %)"
        self._width = max(self._width, len(text))
        sys.stderr.write(f"\r{text}")
        sys.stderr.flush()

    def finish(self) -> None:
        if self._enabled and self._width:
            sys.stderr.write("\r" + " " * self._width + "\r")
            sys.stderr.flush()
        self._width = 0


def list_assets(client: SmpClient, kind: int) -> list[dict]:
    """Lists the files of a kind. A page can be empty and still have a next one."""
    entries: list[dict] = []
    offset = 0
    while True:
        response = client.read(Group.ASSETS, AssetsCommand.LIST, {"kind": kind, "off": offset})
        entries += response["entries"]
        following = response.get("next")
        if following is None:
            return entries
        if following <= offset:
            raise RuntimeError(f"the list did not advance past position {offset}")
        offset = following


class AssetDownload:
    """What a pipelined download knows: which offsets to ask for next, and the bytes that arrived, in order.

    Every chunk but the last has the length of the first, whatever its offset, so once the first answer
    is in, the offsets to ask for are known without waiting for the rest. The size of the asset comes
    with the chunk at offset 0 (`len`), or with the last chunk (`crc`), or with the error of a request
    past the end; a chunk that is short and not the last leaves a gap, which is asked for again.
    """

    MAX_STRAY_RESPONSES = 16

    def __init__(self, start: int) -> None:
        self.written = start                    # The bytes before it are all there.
        self.length: int | None = None          # The size of the asset, once known.
        self.crc: int | None = None             # CRC32 of the whole asset, from its last chunk.
        self._chunk_size = 0
        self._next = start                      # The first offset not asked for yet.
        self._chunks: dict[int, bytes] = {}     # Arrived, but not yet next in line.
        self._again: list[int] = []             # Offsets to ask for before the next new one.
        self._stray = 0

    @property
    def is_complete(self) -> bool:
        return self.length is not None and self.crc is not None and self.written >= self.length

    def accept(self, offset: int, response: dict) -> None:
        """Takes the response to the request for @p offset."""
        if response["off"] != offset:
            # The answer to another request, such as a late one under a sequence number that came round.
            self._stray += 1
            if self._stray > self.MAX_STRAY_RESPONSES:
                raise RuntimeError(f"the unit keeps answering for another offset than {offset}")
            self._again.append(offset)
            return

        data = response["data"]
        is_last = "crc" in response
        if "len" in response:
            self.set_length(response["len"])
        if is_last:
            self.crc = response["crc"]
            self.set_length(offset + len(data))
        elif not data:
            raise RuntimeError(f"empty chunk at offset {offset}")

        if self._chunk_size == 0 and data:
            self._chunk_size = len(data)
            self._next = offset + len(data)
        elif not is_last and len(data) < self._chunk_size:
            self._again.append(offset + len(data))
        if data:
            self._chunks[offset] = data

    def set_length(self, length: int) -> None:
        if self.length is not None and self.length != length:
            raise RuntimeError(f"the asset changed during the download: {self.length} B, then {length} B")
        self.length = length

    def next_offset(self) -> int | None:
        """The offset to ask for next, or None when every byte is asked for."""
        while self._again:
            # A gap may have been filled in part since, by a chunk that overlapped it: the hole starts later.
            offset = max(self._again.pop(0), self.written)
            if self.length is None or offset < self.length:
                return offset
        if self.length is not None and self._next >= self.length:
            return None
        offset = self._next
        self._next += self._chunk_size
        return offset

    def take_ready(self) -> list[bytes]:
        """The bytes that now follow what was written, in order."""
        ready = []
        progressed = True
        while progressed:
            progressed = False
            for offset in sorted(self._chunks):
                if offset > self.written:
                    break
                data = self._chunks.pop(offset)
                if offset + len(data) > self.written:
                    ready.append(data[self.written - offset:])
                    self.written = offset + len(data)
                progressed = True
                break
        if self.length is not None and self.written > self.length:
            raise RuntimeError(
                f"the asset changed during the download: it is {self.length} B, {self.written} B arrived")
        return ready


def download_asset(client: SmpClient, kind: int, name: str, path: Path, *, resume: bool, in_flight: int,
                   progress: Progress) -> tuple[int, int, int]:
    """Downloads an asset into @p path and checks its CRC32. Returns the size, the CRC32 and the offset it began at.

    The first request goes alone, to learn the chunk size, and the rest follow at consecutive offsets,
    @p in_flight at a time. A resumed download asks for the bytes after the local file's, and the file
    is checked whole at the end. The file is only touched once the unit has answered.

    The unit answers the requests one after another, so the last of a full window waits for the
    responses before it: each pipelined request is given the client's timeout once per request in
    flight, or a slow bus would have every request sent again.
    """
    start = path.stat().st_size if resume and path.is_file() else 0

    def body(offset: int) -> dict:
        return {"kind": kind, "name": name, "off": offset}

    try:
        # The unit reads the file up to the offset before it answers a resumed download.
        first = client.read(Group.ASSETS, AssetsCommand.READ, body(start),
                            timeout_s=max(client.timeout_s, ASSET_SEED_TIMEOUT_S) if start else None)
    except SmpError as error:
        if start and error.is_assets_error(ASSETS_BAD_OFFSET) and "len" in error.response:
            raise RuntimeError(f"{path} is {start} B, more than the asset's {error.response['len']} B; "
                               f"it cannot be resumed") from None
        raise
    if first["off"] != start:
        raise RuntimeError(f"asked for offset {start}, got offset {first['off']}")

    download = AssetDownload(start)
    download.accept(start, first)
    pipeline = SmpPipeline(client)
    asked: set[int] = set()     # The offsets in flight.
    stuck_at, restarts = -1, 0

    def ask(offset: int) -> None:
        asked.add(offset)
        pipeline.submit(Op.READ, Group.ASSETS, AssetsCommand.READ, body(offset), offset,
                        timeout_s=client.timeout_s * in_flight)

    with path.open("ab" if start else "wb") as file:
        while True:
            for data in download.take_ready():
                file.write(data)
            progress.update(download.written, download.length)
            if download.is_complete:
                break

            while pipeline.outstanding < in_flight and (offset := download.next_offset()) is not None:
                if offset not in asked:
                    ask(offset)
            if not pipeline.outstanding:
                # Everything was asked for and answered, and a hole is left or the last chunk never
                # came: ask for what is the first byte missing.
                restarts = restarts + 1 if stuck_at == download.written else 1
                stuck_at = download.written
                if restarts > MAX_ASSET_RESTARTS:
                    raise RuntimeError(f"the download stopped at byte {download.written}")
                ask(download.written)

            completion = pipeline.next_completion()
            asked.discard(completion.tag)
            if completion.error is None:
                download.accept(completion.tag, completion.response)
            elif (isinstance(completion.error, SmpError) and completion.error.is_assets_error(ASSETS_BAD_OFFSET)
                  and "len" in completion.error.response):
                # A request past the end: the error says where the asset ends.
                download.set_length(completion.error.response["len"])
            else:
                raise completion.error

    size = path.stat().st_size
    crc = file_crc32(path)
    if size != download.length or crc != download.crc:
        hint = ("it continued a file that may be of another version: download it again without --resume"
                if start else "the asset may have changed during the download")
        raise RuntimeError(f"CRC mismatch: {path} holds {size} B with crc {crc:#010x}, the unit reports "
                           f"{download.length} B with crc {download.crc:#010x}; {hint}. The file was kept")
    return download.length, crc, start


def upload_asset(client: SmpClient, kind: int, name: str, data: bytes, crc: int, chunk_size: int,
                 progress: Progress) -> None:
    """Sends the chunks of an upload. The unit stores the file, under @p name, when the last one arrives."""
    offset = 0
    token = None

    while True:
        chunk = data[offset:offset + chunk_size]
        is_final = offset + len(chunk) >= len(data)
        body = {"kind": kind, "name": name, "off": offset, "data": chunk}
        if offset == 0:
            body |= {"len": len(data), "crc": crc}
        else:
            body["tok"] = token

        # A chunk is sent again after a timeout under the same sequence number, and the unit answers
        # the repeat: the first chunk with its token, a chunk it holds with where it expects the next
        # one, and, for CONFIG_EERIE_LEAP_SMP_ASSETS_SESSION_TIMEOUT_MS after the file was stored, the
        # last chunk with the same answer again.
        try:
            response = client.write(Group.ASSETS, AssetsCommand.WRITE, body)
        except TimeoutError:
            if is_final:
                raise RuntimeError(f"no response to the last chunk of {name}: the unit may have stored it, "
                                   f"check with `assets ls`") from None
            raise TransferLost(f"no response to the chunk at offset {offset}") from None
        except SmpError as error:
            if error.is_assets_error(ASSETS_BAD_OFFSET) and error.response.get("off") not in (None, offset):
                # A lost response: the unit says where it expects to continue.
                offset = error.response["off"]
                if offset > len(data):
                    raise RuntimeError(f"the unit expects offset {offset}, past the end of the file") from None
                continue
            if error.is_assets_error(ASSETS_BAD_TOKEN) and is_final:
                raise RuntimeError(
                    f"the unit refused the last chunk of {name} with BAD_TOKEN, so it has no upload open for it: "
                    f"it dropped the upload, or it stored the file and the answer to the chunk got lost; "
                    f"check with `assets ls` or `assets get`") from None
            if error.is_assets_error(ASSETS_BAD_TOKEN) or error.is_assets_error(ASSETS_BUSY):
                raise TransferLost(str(error)) from None
            raise

        if is_final:
            progress.update(len(data), len(data))
            return
        if offset == 0:
            token = response["tok"]
        offset = response["off"]
        if not 0 <= offset <= len(data):
            raise RuntimeError(f"the unit expects offset {offset} of a file of {len(data)} B")
        progress.update(offset, len(data))


def write_asset(client: SmpClient, kind: int, name: str, data: bytes, progress: Progress) -> int:
    """Uploads a file and returns its CRC32.

    A transfer the unit dropped, or that another upload blocked, starts over once, after the unit's
    session timeout.
    """
    if len(data) > 0xFFFFFFFF:
        raise RuntimeError("the unit counts file sizes in 32 bits")
    crc = zlib.crc32(data)
    chunk_size = asset_chunk_size(client.read(Group.OS, 6)["buf_size"], name, isinstance(client.link, SmpSerialLink))

    for attempt in range(2):
        try:
            upload_asset(client, kind, name, data, crc, chunk_size, progress)
            return crc
        except TransferLost as error:
            progress.finish()
            if attempt:
                raise RuntimeError(f"{name} was not uploaded: {error}") from None
            print(f"{error}; trying again after the unit's session timeout", file=sys.stderr)
            time.sleep(ASSET_SESSION_TIMEOUT_S + 0.5)

    raise AssertionError("unreachable")


def delete_asset(client: SmpClient, kind: int, name: str) -> None:
    """Deletes an asset. A delete sent again after a lost answer meets NOT_FOUND: that means it was done."""
    for attempt in range(client.retries + 1):
        try:
            client.write(Group.ASSETS, AssetsCommand.DELETE, {"kind": kind, "name": name}, retries=0)
            return
        except TimeoutError:
            continue
        except SmpError as error:
            if attempt and error.is_assets_error(ASSETS_NOT_FOUND):
                return
            raise

    raise TimeoutError(f"no response to group {Group.ASSETS} command {AssetsCommand.DELETE}")


@dataclasses.dataclass(frozen=True)
class ConfigType:
    number: int
    name: str          # snake_case of ConfigurationService::Type
    schema: Path
    root: str          # CDDL root type


RT_CORE_SCHEMAS = REPO / "modules/eerie_leap_rt_core/src/configuration/cbor/cbor_schemas"
GAUGE_SCHEMAS = REPO / "app/src/configuration/cbor/cbor_schemas"
CONFIG_TYPES = (
    ConfigType(1, "system", RT_CORE_SCHEMAS / "system_config.cddl", "CborSystemConfig"),
    ConfigType(2, "adc", RT_CORE_SCHEMAS / "adc_config.cddl", "CborAdcConfig"),
    ConfigType(3, "canbus", RT_CORE_SCHEMAS / "canbus_config.cddl", "CborCanbusConfig"),
    ConfigType(4, "sensors", RT_CORE_SCHEMAS / "sensors_config.cddl", "CborSensorsConfig"),
    ConfigType(5, "logging", RT_CORE_SCHEMAS / "logging_config.cddl", "CborLoggingConfig"),
    ConfigType(6, "ui", GAUGE_SCHEMAS / "ui_config.cddl", "CborUiConfig"),
    ConfigType(7, "display", GAUGE_SCHEMAS / "display_config.cddl", "CborDisplayConfig"),
    ConfigType(8, "logging_control", RT_CORE_SCHEMAS / "logging_control_config.cddl", "CborLoggingControlConfig"),
)


def parse_type_number(text: str) -> int:
    """A configuration type by number or name; a number the table lacks passes through."""
    for config_type in CONFIG_TYPES:
        if text.lower().replace("-", "_") == config_type.name:
            return config_type.number
    try:
        return int(text, 0)
    except ValueError:
        names = ", ".join(config_type.name for config_type in CONFIG_TYPES)
        raise argparse.ArgumentTypeError(f"{text!r} is not a configuration type number or one of: {names}") from None


def parse_config_type(text: str) -> ConfigType:
    number = parse_type_number(text)
    for config_type in CONFIG_TYPES:
        if config_type.number == number:
            return config_type
    raise argparse.ArgumentTypeError(
        f"no schema for configuration type {number}; config-read and config-write take any type")


class ConfigError(Exception):
    """A description, or stored CBOR, that does not match the schema."""


class Float32(float):
    """A number written as a single-precision CBOR float, which the firmware's float32 members accept."""


def import_zcbor() -> tuple[type, type]:
    """Returns zcbor's DataTranslator and CddlValidationError."""
    cbor2_module = require("cbor2", "cbor2")
    # zcbor 0.9.1 imports CBORDecodeValueError, which cbor2 6 removed (it was a subclass of
    # CBORDecodeError). The one place zcbor uses it, its YAML conversion, is not called here.
    if not hasattr(cbor2_module, "CBORDecodeValueError"):
        cbor2_module.CBORDecodeValueError = cbor2_module.CBORDecodeError
    zcbor_module = require("zcbor", "zcbor")
    return zcbor_module.DataTranslator, zcbor_module.CddlValidationError


def encode_cbor_head(major: int, value: int) -> bytes:
    if value < 24:
        return bytes([major << 5 | value])
    for additional, size in ((24, 1), (25, 2), (26, 4), (27, 8)):
        if value < 1 << (8 * size):
            return bytes([major << 5 | additional]) + value.to_bytes(size, "big")
    raise ConfigError(f"{value} does not fit in 64 bits")


def encode_cbor(item: object) -> bytes:
    """Encodes definite-length CBOR; Float32 as 0xfa, other floats as 0xfb."""
    if item is None:
        return b"\xf6"
    if isinstance(item, bool):
        return b"\xf5" if item else b"\xf4"
    if isinstance(item, int):
        return encode_cbor_head(0, item) if item >= 0 else encode_cbor_head(1, -1 - item)
    if isinstance(item, Float32):
        try:
            return b"\xfa" + struct.pack(">f", item)
        except OverflowError:
            raise ConfigError(f"{float(item)} does not fit in a single-precision float") from None
    if isinstance(item, float):
        return b"\xfb" + struct.pack(">d", item)
    if isinstance(item, str):
        text = item.encode()
        return encode_cbor_head(3, len(text)) + text
    if isinstance(item, bytes):
        return encode_cbor_head(2, len(item)) + item
    if isinstance(item, (list, tuple)):
        return encode_cbor_head(4, len(item)) + b"".join(encode_cbor(element) for element in item)
    if isinstance(item, dict):
        return encode_cbor_head(5, len(item)) + b"".join(
            encode_cbor(key) + encode_cbor(value) for key, value in item.items())
    raise ConfigError(f"cannot encode {item!r}")


def shortest_float32(value: float) -> float:
    """The fewest digits (1 to 9) that read back as the same single-precision float."""
    if not math.isfinite(value):
        return value
    try:
        bits = struct.pack(">f", value)
        for digits in range(1, 10):
            candidate = float(f"{value:.{digits}g}")
            if struct.pack(">f", candidate) == bits:
                return candidate
    except OverflowError:
        pass
    return value


def plain(item: object) -> object:
    """Items with Float32 replaced by float, which is what zcbor's validation expects."""
    if isinstance(item, Float32):
        return float(item)
    if isinstance(item, (list, tuple)):
        return [plain(element) for element in item]
    if isinstance(item, dict):
        return {plain(key): plain(value) for key, value in item.items()}
    return item


def is_number(value: object) -> bool:
    return isinstance(value, (int, float)) and not isinstance(value, bool)


def join_path(path: str, label: object) -> str:
    return f"{path}.{label}" if path else str(label)


class ConfigCodec:
    """Encodes and decodes one configuration type between named descriptions and CBOR.

    The schema is the type's CDDL, parsed by zcbor, which also validates every encoded item. A
    description names the members of each array (a mapping, in any order) and lists the repeated
    ones; see tools/README.md for the rules.
    """

    def __init__(self, config_type: ConfigType) -> None:
        data_translator, self._validation_error = import_zcbor()
        # 1024 bounds `*`; the firmware's validators hold the real caps.
        self._types = data_translator.from_cddl(config_type.schema.read_text(), 1024).my_types
        self._root = self._types[config_type.root]
        self._name = config_type.root

    def encode(self, description: object) -> bytes:
        items = self._to_items(self._root, description, "")
        self._validate(plain(items))
        return encode_cbor(items)

    def decode(self, data: bytes) -> object:
        stream = io.BytesIO(data)
        try:
            item = cbor2.CBORDecoder(stream).decode()
        except (cbor2.CBORDecodeError, EOFError) as error:
            raise ConfigError(f"{self._name}: not CBOR ({error})") from None
        if stream.read(1):
            raise ConfigError(f"{self._name}: bytes follow the CBOR item")
        self._validate(item)
        return self._from_items(self._root, item, "")

    def _validate(self, item: object) -> None:
        # zcbor prints what it found wrong when a validation fails; the exception is enough here.
        with contextlib.redirect_stdout(io.StringIO()):
            try:
                self._root.validate_obj(item)
            except self._validation_error as error:
                raise ConfigError(f"{self._name}: {error}") from None
            except (AssertionError, KeyError, TypeError) as error:
                raise ConfigError(f"{self._name}: does not match the schema ({error!r})") from None

    def _resolve(self, node):
        while node.type == "OTHER":
            node = self._types[node.value]
        return node

    @staticmethod
    def _is_float32(node) -> bool:
        return node.min_size == 4 and node.max_size == 4

    @staticmethod
    def _unsupported(node, path: str) -> ConfigError:
        return ConfigError(f"{path or 'root'}: the schema uses {node.type}, which descriptions do not cover")

    # Description -> positional items.

    def _to_items(self, node, value: object, path: str) -> object:
        node = self._resolve(node)
        where = path or "root"

        if node.type == "UINT":
            if type(value) is not int or value < 0:
                raise ConfigError(f"{where}: expected an unsigned integer, got {value!r}")
            return value
        if node.type in ("INT", "NINT"):
            if type(value) is not int:
                raise ConfigError(f"{where}: expected an integer, got {value!r}")
            return value
        if node.type == "BOOL":
            if type(value) is not bool:
                raise ConfigError(f"{where}: expected true or false, got {value!r}")
            return value
        if node.type == "TSTR":
            if not isinstance(value, str):
                raise ConfigError(f"{where}: expected a string, got {value!r}")
            return value
        if node.type == "FLOAT":
            if not is_number(value):
                raise ConfigError(f"{where}: expected a number, got {value!r}")
            return Float32(value) if self._is_float32(node) else float(value)
        if node.type == "LIST":
            return self._list_to_items(node, value, path)
        if node.type == "MAP":
            return self._map_to_items(node, value, path)
        if node.type == "UNION":
            reasons = []
            for alternative in node.value:
                try:
                    return self._to_items(alternative, value, path)
                except ConfigError as error:
                    reasons.append(str(error))
            raise ConfigError(f"{where}: {value!r} matches none of the alternatives ({'; '.join(reasons)})")
        raise self._unsupported(node, path)

    def _list_to_items(self, node, value: object, path: str) -> list:
        members = node.value
        where = path or "root"

        if members and all(member.label for member in members):
            if not isinstance(value, dict):
                labels = ", ".join(member.label for member in members)
                raise ConfigError(f"{where}: expected a mapping with {labels}, got {type(value).__name__}")
            unknown = [key for key in value if key not in {member.label for member in members}]
            if unknown:
                raise ConfigError(f"{join_path(path, unknown[0])}: unknown member")

            items = []
            for member in members:
                if member.max_qty != 1:
                    raise self._unsupported(member, join_path(path, member.label))
                member_path = join_path(path, member.label)
                if value.get(member.label) is not None:
                    items.append(self._to_items(member, value[member.label], member_path))
                elif member.min_qty == 0:
                    continue
                else:
                    raise ConfigError(f"{member_path}: missing")
            return items

        if len(members) == 1 and not members[0].label:
            member = members[0]
            if not isinstance(value, (list, tuple)):
                raise ConfigError(f"{where}: expected a list, got {type(value).__name__}")
            if not member.min_qty <= len(value) <= member.max_qty:
                raise ConfigError(
                    f"{where}: {len(value)} entries, the schema allows {member.min_qty} to {member.max_qty}")
            return [self._to_items(member, element, f"{path}[{index}]") for index, element in enumerate(value)]

        raise self._unsupported(node, path)

    def _map_to_items(self, node, value: object, path: str) -> dict:
        where = path or "root"
        if len(node.value) != 1 or node.value[0].key is None:
            raise self._unsupported(node, path)
        member = node.value[0]

        if isinstance(value, dict):
            pairs = list(value.items())
        elif isinstance(value, (list, tuple)) and all(
                isinstance(pair, (list, tuple)) and len(pair) == 2 for pair in value):
            pairs = [tuple(pair) for pair in value]
        else:
            raise ConfigError(f"{where}: expected a mapping or a list of [key, value] pairs, got {value!r}")

        if not member.min_qty <= len(pairs) <= member.max_qty:
            raise ConfigError(f"{where}: {len(pairs)} entries, the schema allows {member.min_qty} to {member.max_qty}")

        items: dict = {}
        for key, element in pairs:
            key_item = self._key_to_item(member.key, key, f"{path}[{key!r}]")
            if key_item in items:
                raise ConfigError(f"{where}: key {key!r} twice")
            items[key_item] = self._to_items(member, element, f"{path}[{key!r}]")
        return items

    def _key_to_item(self, node, key: object, path: str) -> object:
        node = self._resolve(node)
        # JSON object keys are always text.
        if isinstance(key, str) and node.type in ("UINT", "INT", "NINT", "FLOAT"):
            try:
                key = float(key) if node.type == "FLOAT" else int(key, 0)
            except ValueError:
                raise ConfigError(f"{path}: {key!r} is not a number") from None
        return self._to_items(node, key, path)

    # Positional items -> description.

    def _from_items(self, node, item: object, path: str) -> object:
        node = self._resolve(node)
        where = path or "root"

        if node.type == "UINT":
            if type(item) is not int or item < 0:
                raise ConfigError(f"{where}: expected an unsigned integer, found {item!r}")
            return item
        if node.type in ("INT", "NINT"):
            if type(item) is not int:
                raise ConfigError(f"{where}: expected an integer, found {item!r}")
            return item
        if node.type == "BOOL":
            if type(item) is not bool:
                raise ConfigError(f"{where}: expected a bool, found {item!r}")
            return item
        if node.type == "TSTR":
            if not isinstance(item, str):
                raise ConfigError(f"{where}: expected a string, found {item!r}")
            return item
        if node.type == "FLOAT":
            if type(item) is not float:
                raise ConfigError(f"{where}: expected a float, found {item!r}")
            return shortest_float32(item) if self._is_float32(node) else item
        if node.type == "LIST":
            return self._list_from_items(node, item, path)
        if node.type == "MAP":
            return self._map_from_items(node, item, path)
        if node.type == "UNION":
            reasons = []
            for alternative in node.value:
                try:
                    return self._from_items(alternative, item, path)
                except ConfigError as error:
                    reasons.append(str(error))
            raise ConfigError(f"{where}: {item!r} matches none of the alternatives ({'; '.join(reasons)})")
        raise self._unsupported(node, path)

    def _list_from_items(self, node, item: object, path: str) -> object:
        members = node.value
        where = path or "root"
        if not isinstance(item, list):
            raise ConfigError(f"{where}: expected an array, found {item!r}")

        if members and all(member.label for member in members):
            description: dict = {}
            position = 0
            for member in members:
                if member.max_qty != 1:
                    raise self._unsupported(member, join_path(path, member.label))
                member_path = join_path(path, member.label)
                if position < len(item):
                    try:
                        description[member.label] = self._from_items(member, item[position], member_path)
                        position += 1
                        continue
                    except ConfigError:
                        # An optional member the array does not carry; the next one may match.
                        if member.min_qty != 0:
                            raise
                if member.min_qty != 0:
                    raise ConfigError(f"{member_path}: missing")
            if position != len(item):
                raise ConfigError(f"{where}: {len(item) - position} unexpected trailing entries")
            return description

        if len(members) == 1 and not members[0].label:
            member = members[0]
            if not member.min_qty <= len(item) <= member.max_qty:
                raise ConfigError(
                    f"{where}: {len(item)} entries, the schema allows {member.min_qty} to {member.max_qty}")
            return [self._from_items(member, element, f"{path}[{index}]") for index, element in enumerate(item)]

        raise self._unsupported(node, path)

    def _map_from_items(self, node, item: object, path: str) -> dict:
        where = path or "root"
        if len(node.value) != 1 or node.value[0].key is None:
            raise self._unsupported(node, path)
        member = node.value[0]
        if not isinstance(item, dict):
            raise ConfigError(f"{where}: expected a map, found {item!r}")
        if not member.min_qty <= len(item) <= member.max_qty:
            raise ConfigError(f"{where}: {len(item)} entries, the schema allows {member.min_qty} to {member.max_qty}")

        description = {}
        for key, element in item.items():
            key_description = self._from_items(member.key, key, f"{path}[{key!r}]")
            description[key_description] = self._from_items(member, element, f"{path}[{key_description!r}]")
        return description


def load_description(path: str) -> object:
    """Reads a YAML or JSON description; '-' is standard input, and a .json name selects JSON."""
    as_json = path != "-" and path.lower().endswith(".json")
    yaml = None if as_json else require("yaml", "PyYAML")
    syntax_errors = (ValueError,) if as_json else (ValueError, yaml.YAMLError)  # A JSON or UTF-8 error is a ValueError.
    try:
        text = sys.stdin.read() if path == "-" else Path(path).read_text()
        return json.loads(text) if as_json else yaml.safe_load(text)
    except syntax_errors as error:
        raise ConfigError(f"{'standard input' if path == '-' else path}: {error}") from None


def write_description(description: object, path: str | None, as_json: bool = False) -> None:
    """Writes a description as YAML, or JSON for a .json name or as_json; None or '-' is standard output."""
    as_json = as_json or bool(path and path.lower().endswith(".json"))
    if as_json:
        text = json.dumps(description, indent=2) + "\n"
    else:
        text = require("yaml", "PyYAML").safe_dump(description, sort_keys=False, default_flow_style=False)

    if path in (None, "-"):
        sys.stdout.write(text)
    else:
        Path(path).write_text(text)


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
            print(f"id {device['id']:3}  uid {device['uid']:#010x}  features {device['features']:#06x}  status {device['status']}")
        offset += len(response["devices"])
        if not response["devices"] or offset >= response["total"]:
            return


def command_reset(client: SmpClient, args: argparse.Namespace) -> None:
    # The unit restarts shortly after it answers; it is not asked twice, and a lost answer is no failure.
    try:
        client.write(Group.OS, OS_RESET, {}, retries=0)
    except TimeoutError:
        print("no response; the unit may have restarted")
        return
    print("reset requested")


def command_config_list(client: SmpClient, args: argparse.Namespace) -> None:
    for entry in client.read(Group.CONFIG, 0)["types"]:
        print(f"type {entry['type']}  max {entry['max']} B")


def command_config_crc(client: SmpClient, args: argparse.Namespace) -> None:
    response = client.read(Group.CONFIG, 1, {"type": args.type})
    applied = "yes" if response.get("applied", True) else "no"
    print(f"type {response['type']}  len {response['len']}  crc {response['crc']:#010x}  applied {applied}")


def command_config_read(client: SmpClient, args: argparse.Namespace) -> None:
    data = read_config(client, args.type)
    args.file.write_bytes(data)
    print(f"read {len(data)} B, crc {zlib.crc32(data):#010x}")


def command_config_write(client: SmpClient, args: argparse.Namespace) -> None:
    data = args.file.read_bytes()
    crc = write_config(client, args.type, data)
    print(f"wrote {len(data)} B, crc {crc:#010x}")


def command_config_get(client: SmpClient, args: argparse.Namespace) -> None:
    description = ConfigCodec(args.type).decode(read_config(client, args.type.number))
    write_description(description, args.file)


def command_config_set(client: SmpClient, args: argparse.Namespace) -> None:
    data = ConfigCodec(args.type).encode(load_description(args.file))
    crc = write_config(client, args.type.number, data)
    print(f"wrote {len(data)} B, crc {crc:#010x}")


def command_config_encode(client: None, args: argparse.Namespace) -> None:
    data = ConfigCodec(args.type).encode(load_description(args.description))
    args.output.write_bytes(data)
    print(f"encoded {len(data)} B, crc {zlib.crc32(data):#010x}")


def command_config_decode(client: None, args: argparse.Namespace) -> None:
    write_description(ConfigCodec(args.type).decode(args.input.read_bytes()), args.output)


def command_assets_kinds(client: SmpClient, args: argparse.Namespace) -> None:
    for entry in client.read(Group.ASSETS, AssetsCommand.KINDS)["kinds"]:
        print(format_kind(entry))


def command_assets_ls(client: SmpClient, args: argparse.Namespace) -> None:
    for entry in list_assets(client, args.kind):
        print(f"{entry['size']:>10}  {entry['name']}")


def command_assets_get(client: SmpClient, args: argparse.Namespace) -> None:
    path = args.file or Path(Path(args.name).name)
    if path.is_dir():
        path = path / Path(args.name).name

    in_flight = args.in_flight
    if in_flight > 1:
        try:
            buf_count = client.read(Group.OS, 6).get("buf_count")
        except SmpError:
            buf_count = None  # No packet count to go by.
        in_flight = limit_window(in_flight, buf_count)
        if in_flight != args.in_flight:
            print(f"warning: the unit has {buf_count} packets, shared by requests and responses: "
                  f"{in_flight} in flight instead of {args.in_flight}", file=sys.stderr)

    progress = Progress(args.name)
    start_s = time.monotonic()
    try:
        length, crc, start = download_asset(client, args.kind, args.name, path, resume=args.resume,
                                            in_flight=in_flight, progress=progress)
    finally:
        progress.finish()
    elapsed_s = max(time.monotonic() - start_s, 1e-9)

    resumed = f", resumed at {start} B" if start else ""
    print(f"read {args.name}: {length} B to {path}, crc {crc:#010x}; {length - start} B in {elapsed_s:.2f} s "
          f"({(length - start) / elapsed_s / 1024:.1f} KB/s{resumed})")


def command_assets_put(client: SmpClient, args: argparse.Namespace) -> None:
    data = args.file.read_bytes()
    progress = Progress(args.name)
    start_s = time.monotonic()
    try:
        crc = write_asset(client, args.kind, args.name, data, progress)
    finally:
        progress.finish()
    elapsed_s = max(time.monotonic() - start_s, 1e-9)

    print(f"wrote {args.name}: {len(data)} B, crc {crc:#010x}; "
          f"{elapsed_s:.2f} s ({len(data) / elapsed_s / 1024:.1f} KB/s)")


def command_assets_rm(client: SmpClient, args: argparse.Namespace) -> None:
    delete_asset(client, args.kind, args.name)
    print(f"deleted {args.name}")


ASSETS_HANDLERS = {
    "kinds": command_assets_kinds,
    "ls": command_assets_ls,
    "get": command_assets_get,
    "put": command_assets_put,
    "rm": command_assets_rm,
}


def command_assets(client: SmpClient, args: argparse.Namespace) -> None:
    ASSETS_HANDLERS[args.assets_command](client, args)


def command_shell(client: SmpClient, args: argparse.Namespace) -> None:
    if args.text:
        client.link.send_text(args.text)
    for line in client.link.read_console(args.wait):
        print(line)


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
    serial = parser.add_argument_group("serial transport")
    serial.add_argument("--serial", metavar="PORT", help="reach the unit on its console, such as /dev/ttyACM0")
    serial.add_argument("--baud", type=int, default=DEFAULT_BAUD, help=f"console baud rate (default: {DEFAULT_BAUD})")
    serial.add_argument("--line-delay", type=float, default=0.0, metavar="S",
                        help="seconds between the lines of a request, for a unit with few receive buffers (default: 0)")
    serial.add_argument("--show-console", action="store_true",
                        help="print the console text between SMP frames to stderr")

    can = parser.add_argument_group("CAN transport")
    can.add_argument("--interface", default="socketcan", help="python-can interface (default: socketcan)")
    can.add_argument("--channel", default="can0", help="python-can channel (default: can0)")
    can.add_argument("--bitrate", type=int, default=500000, help="nominal bitrate, also used for pacing")
    can.add_argument("--fd", action="store_true", help="send CAN FD frames with bitrate switching")
    can.add_argument("--data-bitrate", type=int, default=2000000, help="CAN FD data bitrate")
    can.add_argument("--base", type=lambda value: int(value, 0), default=DEFAULT_BASE, help="SMP ID base")
    can.add_argument("--target", type=int, help="CDMP device ID of the unit")
    can.add_argument("--source", type=int, default=DEFAULT_SOURCE, help="CDMP device ID of this client")
    can.add_argument("--share", type=int, default=25, help="bus share in percent for the sent frames")

    parser.add_argument("--timeout", type=float, default=2.0, help="seconds to wait for a response")
    parser.add_argument("--retries", type=int, help=f"resends after a timeout (default: {DEFAULT_CAN_RETRIES} on CAN, "
                                                   f"{DEFAULT_SERIAL_RETRIES} on serial)")

    commands = parser.add_subparsers(dest="command", required=True)
    commands.add_parser("echo", help="os echo").add_argument("text")
    commands.add_parser("params", help="os mcumgr params: buffer size and count")
    commands.add_parser("info", help="device info")
    commands.add_parser("devices", help="network devices, as the unit sees them")
    commands.add_parser("reset", help="os reset: restart the unit")
    commands.add_parser("config-list", help="configuration types and their size caps")
    for name in ("config-crc", "config-read", "config-write"):
        sub = commands.add_parser(name, help=name.replace("-", " ") + " (raw bytes)")
        sub.add_argument("type", type=parse_type_number, help="configuration type, a number or a name")
        if name != "config-crc":
            sub.add_argument("file", type=Path)
    get = commands.add_parser("config-get", help="read a configuration as a YAML or JSON description")
    get.add_argument("type", type=parse_config_type, help="configuration type, a number or a name")
    get.add_argument("file", nargs="?", help="output file (.json for JSON); standard output by default")
    set_ = commands.add_parser("config-set", help="write a configuration from a YAML or JSON description")
    set_.add_argument("type", type=parse_config_type, help="configuration type, a number or a name")
    set_.add_argument("file", help="description file (.json for JSON); - for standard input")
    encode = commands.add_parser("config-encode", help="offline: description to CBOR file")
    encode.add_argument("type", type=parse_config_type)
    encode.add_argument("description")
    encode.add_argument("output", type=Path)
    decode = commands.add_parser("config-decode", help="offline: CBOR file to description")
    decode.add_argument("type", type=parse_config_type)
    decode.add_argument("input", type=Path)
    decode.add_argument("output", nargs="?")
    assets = commands.add_parser("assets", help="asset files: images, animations, scripts, logs")
    assets_commands = assets.add_subparsers(dest="assets_command", required=True)
    assets_commands.add_parser("kinds", help="the asset kinds the unit serves")
    asset_ls = assets_commands.add_parser("ls", help="list the files of a kind")
    asset_ls.add_argument("kind", type=parse_asset_kind, help="asset kind, a name or a number")
    asset_get = assets_commands.add_parser("get", help="download a file, checking its CRC32")
    asset_get.add_argument("kind", type=parse_asset_kind, help="asset kind, a name or a number")
    asset_get.add_argument("name")
    asset_get.add_argument("file", nargs="?", type=Path,
                           help="local file, or the directory to put NAME in (default: NAME here)")
    asset_get.add_argument("--in-flight", type=int, default=DEFAULT_ASSET_IN_FLIGHT, metavar="N",
                           help=f"requests outstanding at once (default: {DEFAULT_ASSET_IN_FLIGHT}); fewer than the "
                                "unit's packet count, and each waits --timeout once per request in flight")
    asset_get.add_argument("--resume", action="store_true", help="continue a partial local file from its size")
    asset_put = assets_commands.add_parser("put", help="upload a file")
    asset_put.add_argument("kind", type=parse_asset_kind, help="asset kind, a name or a number")
    asset_put.add_argument("name", help="the asset's name on the unit")
    asset_put.add_argument("file", type=Path)
    asset_rm = assets_commands.add_parser("rm", help="delete a file")
    asset_rm.add_argument("kind", type=parse_asset_kind, help="asset kind, a name or a number")
    asset_rm.add_argument("name")
    shell = commands.add_parser("shell", help="serial only: type a shell command and print the console")
    shell.add_argument("text", help="the command line; empty to only listen")
    shell.add_argument("--wait", type=float, default=1.0, help="seconds to print the console for (default: 1)")
    bench = commands.add_parser("bench", help="echo round trips, for throughput")
    bench.add_argument("--size", type=int, default=256, help="echo payload in bytes")
    bench.add_argument("--count", type=int, default=50)

    args = parser.parse_args(argv)
    if args.command in OFFLINE_COMMANDS:
        return args

    if args.command == "assets" and args.assets_command == "get" and not 1 <= args.in_flight <= MAX_ASSET_IN_FLIGHT:
        parser.error(f"--in-flight must be 1-{MAX_ASSET_IN_FLIGHT}")

    if args.serial:
        if args.target is not None:
            parser.error("--target is for the CAN transport; the serial transport reaches the unit on the port")
    else:
        if args.command == "shell":
            parser.error("shell needs --serial")
        if args.target is None:
            parser.error("an SMP command needs --target (CAN) or --serial")
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
    "reset": command_reset,
    "config-list": command_config_list,
    "config-crc": command_config_crc,
    "config-read": command_config_read,
    "config-write": command_config_write,
    "config-get": command_config_get,
    "config-set": command_config_set,
    "config-encode": command_config_encode,
    "config-decode": command_config_decode,
    "assets": command_assets,
    "shell": command_shell,
    "bench": command_bench,
}
OFFLINE_COMMANDS = ("config-encode", "config-decode")


@contextlib.contextmanager
def open_link(args: argparse.Namespace) -> Iterator[SmpCanLink | SmpSerialLink]:
    if args.serial:
        with SmpSerialLink(args.serial, args.baud, args.line_delay, args.show_console) as link:
            yield link
        return

    can = require("can", "python-can")
    bus_options = {"interface": args.interface, "channel": args.channel, "bitrate": args.bitrate}
    if args.fd:
        bus_options |= {"fd": True, "data_bitrate": args.data_bitrate}

    with can.Bus(**bus_options) as bus:
        yield SmpCanLink(bus, target=args.target, source=args.source, base=args.base, is_can_fd=args.fd,
                         bitrate=args.bitrate, data_bitrate=args.data_bitrate, share_percent=args.share)


def main(argv: list[str]) -> int:
    args = parse_args(argv)

    try:
        if args.command in OFFLINE_COMMANDS:
            HANDLERS[args.command](None, args)
            return 0

        retries = args.retries
        if retries is None:
            retries = DEFAULT_SERIAL_RETRIES if args.serial else DEFAULT_CAN_RETRIES
        with open_link(args) as link:
            HANDLERS[args.command](SmpClient(link, args.timeout, retries), args)
    except TimeoutError as error:
        hint = f"; is a bench build (app/bench.conf) running on {args.serial}?" if args.serial else ""
        print(f"error: {error}{hint}", file=sys.stderr)
        return 1
    except (SmpError, RuntimeError, ConfigError, OSError) as error:
        print(f"error: {error}", file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
