#!/usr/bin/env python3
"""SMP bench client for EerieLeap units, over CAN, over a unit's console or over Bluetooth.

Sends SMP requests to one unit and prints the responses. Three transports:

* CAN: any CAN adapter python-can supports, with the framing of the CDMP README, section 5. The
  client takes a CDMP device ID of its own (--source, 254 by default) that no unit on the bus may
  use. Selected by --target.
* Serial: the unit's console, as MCUmgr's shell transport frames it (lines starting 0x06 0x09 and
  0x04 0x14, base64, CRC16), among the console's log lines. Only a bench build answers there
  (app/bench.conf of the gauge and of rt_daq). Selected by --serial. Opening the port does not
  reset an ESP32-S3: Linux raises DTR and RTS when it opens a USB-Serial-JTAG port, and the chip is
  held in reset while RTS is high and DTR low, so the lines are put through RTS 0 first.
* Bluetooth: the access point service of a unit with BLE (rt_core docs/bluetooth.md). Every packet is
  prefixed with a route byte; --route 0 reaches the access point itself and another CDMP device ID is
  forwarded over CAN by the access point. Selected by --ble NAME|ADDRESS.

Configurations can be written and read as YAML or JSON descriptions (config-get, config-set,
config-encode, config-decode), encoded against the CDDL schemas in this checkout; the format is in
tools/README.md. Failed SMP requests are resent: once on CAN and over Bluetooth, three times on serial,
where a log line landing in a response corrupts it. The request that completes a configuration write is
never resent blindly; the client reads the stored CRC to learn whether it was applied.

Asset files (images, animations, fonts, scripts, logs) are listed, downloaded, uploaded and deleted with the
assets commands. A download keeps several requests in flight, can continue a partial file and checks
the file's CRC32 against the unit's; the format is in tools/README.md.

Firmware images are inspected offline (image info), listed, uploaded, tested, confirmed and erased
over MCUmgr's image group, and image update runs the whole sequence of the firmware update plan:
check the file against the unit, upload, mark for test, reset, wait for the swap and watch the unit
confirm itself (dfu status) or revert. The format is in tools/README.md.

Requires cbor2; python-can for CAN, pyserial for serial, bleak for Bluetooth, PyYAML and zcbor for
descriptions.

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
    smp_can_client.py image info rt_gauge_esp32s3_touch_amoled_1_75_0.0.2.signed.bin
    smp_can_client.py --serial /dev/ttyACM0 image list
    smp_can_client.py --channel can0 --target 1 image update rt_gauge_esp32s3_touch_amoled_1_75_0.0.2.signed.bin
    smp_can_client.py --ble "EerieLeap Gauge" info
    smp_can_client.py --ble AA:BB:CC:DD:EE:FF --route 5 image upload rt_daq_0.0.2.signed.bin
    smp_can_client.py --serial /dev/ttyACM0 dfu status
"""

from __future__ import annotations

import argparse
import asyncio
import base64
import binascii
import collections
import concurrent.futures
import contextlib
import dataclasses
import datetime
import hashlib
import importlib
import io
import json
import math
import queue
import random
import re
import struct
import sys
import threading
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

# The access point service of rt_core's docs/bluetooth.md: info (read), smp (write without response, notify) and
# live (notify). `info` is [protocol version u8][largest SMP packet u16 LE][requests in flight u8].
BLE_SERVICE_UUID = "e7a1b2c3-d4e5-6f78-9a0b-c1d2e3f40000"
BLE_INFO_UUID = "e7a1b2c3-d4e5-6f78-9a0b-c1d2e3f40001"
BLE_SMP_UUID = "e7a1b2c3-d4e5-6f78-9a0b-c1d2e3f40002"
BLE_PROTOCOL_VERSION = 1
BLE_INFO = struct.Struct("<BHB")
# An ATT write carries the MTU less its 3-byte header; 23 is the MTU before any exchange.
BLE_MIN_WRITE_SIZE = 20
# The unit drops a partial packet after EERIE_LEAP_SMP_BLE_RX_TIMEOUT_MS (1 s); a gap that long ends one here too.
BLE_REASSEMBLY_TIMEOUT_S = 1.0
DEFAULT_BLE_SCAN_TIMEOUT_S = 10.0
BLE_CONNECT_TIMEOUT_S = 20.0
BLE_PAIR_TIMEOUT_S = 30.0
BLE_WRITE_TIMEOUT_S = 10.0
# A device address (BlueZ and Windows) or the UUID macOS uses in its place; anything else is a name.
BLE_ADDRESS = re.compile(
    r"(?:[0-9A-Fa-f]{2}[:-]){5}[0-9A-Fa-f]{2}|[0-9A-Fa-f]{8}(?:-[0-9A-Fa-f]{4}){3}-[0-9A-Fa-f]{12}")

DEFAULT_CAN_RETRIES = 1
DEFAULT_SERIAL_RETRIES = 3
DEFAULT_BLE_RETRIES = 1
# The request that completes a configuration write answers after the unit applied it, and applying
# restarts services that log; it gets this long, once.
FINAL_REQUEST_TIMEOUT_S = 15.0
# EERIE_LEAP_SMP_CONFIG_SESSION_TIMEOUT_MS: an idle transfer is dropped after this.
CONFIG_SESSION_TIMEOUT_S = 5.0
# How long to look for a write whose answer got lost.
CONFIRM_WINDOW_S = 8.0

# img_mgmt (group 1) commands, and the dfu group's (72).
IMG_STATE = 0
IMG_UPLOAD = 1
IMG_ERASE = 5
DFU_STATUS = 0
DFU_STATES = {0: "CONFIRMED", 1: "VERIFYING", 2: "CONFIRMING", 3: "FAILED"}
DFU_CONFIRMED = 0
DFU_FAILED = 3
# hwinfo's RESET_* bits (drivers/hwinfo.h), which the dfu STATUS reports as reset_cause.
RESET_CAUSES = ("PIN", "SOFTWARE", "BROWNOUT", "POR", "WATCHDOG", "DEBUG", "SECURITY", "LOW_POWER_WAKE", "CPU_LOCKUP",
                "PARITY", "PLL", "CLOCK", "HARDWARE", "USER", "TEMPERATURE", "BOOTLOADER", "FLASH")

# MCUboot's image format (bootutil/image.h): a 32-byte header, the image, the protected TLVs (signed with the
# image), then the unprotected TLVs; each TLV area starts with {u16 magic, u16 total length}, and each entry is
# {u16 type, u16 length} and its value. Everything is little-endian.
IMAGE_MAGIC = 0x96F3B83D
IMAGE_HEADER = struct.Struct("<IIHHIIBBHII")
IMAGE_TLV_HEADER = struct.Struct("<HH")
IMAGE_TLV_MAGIC = 0x6907
IMAGE_TLV_PROTECTED_MAGIC = 0x6908
IMAGE_TLV_SHA256 = 0x10
# D5: [format u8 = 1][product_id u16 LE][board_len u8][board ASCII], in the protected TLVs.
IMAGE_TLV_IDENTITY = 0x00A0
IMAGE_IDENTITY_FORMAT = 1
# The unit refuses an image whose protected TLVs are larger (ImageUploadGuard's buffer).
IMAGE_MAX_PROTECTED_SIZE = 128
IMAGE_TLV_NAMES = {
    0x01: "KEYHASH", 0x02: "PUBKEY", 0x10: "SHA256", 0x11: "SHA384", 0x12: "SHA512", 0x20: "RSA2048-PSS",
    0x22: "ECDSA-SIG", 0x23: "RSA3072-PSS", 0x24: "ED25519", 0x25: "SIG-PURE", 0x30: "ENC-RSA2048", 0x31: "ENC-KW",
    0x32: "ENC-EC256", 0x33: "ENC-X25519", 0x40: "DEPENDENCY", 0x50: "SEC-CNT", 0x60: "BOOT-RECORD",
    IMAGE_TLV_IDENTITY: "IDENTITY",
}
IMAGE_SIGNATURE_TLVS = (0x20, 0x22, 0x23, 0x24, 0x25)
IMAGE_KEY_HASH_TLV = 0x01
IMAGE_FLAGS = {0x01: "PIC", 0x04: "ENCRYPTED_AES128", 0x08: "ENCRYPTED_AES256", 0x10: "NON_BOOTABLE", 0x20: "RAM_LOAD",
               0x100: "ROM_FIXED"}

# The room around `data` in a later upload chunk, as the plan sizes chunks: buf_size - 64.
IMAGE_WRITE_OVERHEAD = 64
# The first chunk makes the unit compare the slot with the image's SHA-256 (it reads the old content to see
# whether the image is there already) before it answers; the last chunk waits for the slot check (D3).
IMAGE_FIRST_CHUNK_TIMEOUT_S = 10.0
IMAGE_LAST_CHUNK_TIMEOUT_S = 5.0
# A broken transfer starts over at offset 0 with the same sha, and the unit answers with the offset it has.
IMAGE_UPLOAD_ATTEMPTS = 3
IMAGE_RESUME_PAUSE_S = 1.0
IMAGE_RECONNECT_WINDOW_S = 60.0
# Answers that do not move the unit's offset forward, one after the other, before the upload gives up.
IMAGE_MAX_STALLS = 8
# After the reset command the unit answers for a moment before it restarts; one that still answers after this
# long did not restart.
IMAGE_RESTART_GRACE_S = 20.0
IMAGE_POLL_INTERVAL_S = 1.0
IMAGE_POLL_MAX_INTERVAL_S = 5.0
IMAGE_VERIFY_INTERVAL_S = 2.0
IMAGE_NOTE_INTERVAL_S = 10.0
DEFAULT_UPDATE_TIMEOUT_S = 300.0
# What a console shows when the app of a unit is up: the Zephyr boot banner (MCUboot's reads "*** Booting MCUboot",
# the swap is still to come) or the shell's prompt. The shell enables its receive interrupt a moment after either.
SERIAL_BOOT_MARKERS = ("*** Booting Zephyr", "uart:~$")
SERIAL_BOOT_SETTLE_S = 1.5
# A console unit that printed no banner within the time given is polled all the same, for this long.
IMAGE_POLL_FALLBACK_S = 60.0
# Polling after the banner: at least this long, whatever is left of the time given.
IMAGE_POST_BOOT_POLL_S = 30.0

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
    IMG = 1
    CONFIG = 64
    DEVICE = 65
    NETWORK = 66
    LIVE = 67
    CALIBRATION = 68
    FILES = 69
    ASSETS = 70
    LOGGING = 71
    DFU = 72


OS_RESET = 5

LOGGING_STATUS = 0
LOGGING_START = 1
LOGGING_STOP = 2
# LoggingStatusReason (logging_status_reason.h): add values, never renumber.
LOGGING_REASONS = {0: "stopped", 1: "write failed", 2: "no space", 3: "rotation failed", 4: "unavailable", 5: "firmware update"}
# <prefix>_YYYYMMDDTHHMMSSZ_NNNN.mf4: the start of a log file's session, UTC.
LOG_NAME_PATTERN = re.compile(r"_(\d{8}T\d{6}Z)_\d+\.mf4$")

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

# Zephyr's img_mgmt_err_code_t (img_mgmt.h), by number. Group 1 reports them as SMP v2 {group, rc}.
IMG_ERROR_NAMES = (
    "OK", "UNKNOWN", "FLASH_CONFIG_QUERY_FAIL", "NO_IMAGE", "NO_TLVS", "INVALID_TLV", "TLV_MULTIPLE_HASHES_FOUND",
    "TLV_INVALID_SIZE", "HASH_NOT_FOUND", "NO_FREE_SLOT", "FLASH_OPEN_FAILED", "FLASH_READ_FAILED",
    "FLASH_WRITE_FAILED", "FLASH_ERASE_FAILED", "INVALID_SLOT", "NO_FREE_MEMORY", "FLASH_CONTEXT_ALREADY_SET",
    "FLASH_CONTEXT_NOT_SET", "FLASH_AREA_DEVICE_NULL", "INVALID_PAGE_OFFSET", "INVALID_OFFSET", "INVALID_LENGTH",
    "INVALID_IMAGE_HEADER", "INVALID_IMAGE_HEADER_MAGIC", "INVALID_HASH", "INVALID_FLASH_ADDRESS",
    "VERSION_GET_FAILED", "CURRENT_VERSION_IS_NEWER", "IMAGE_ALREADY_PENDING", "INVALID_IMAGE_VECTOR_TABLE",
    "INVALID_IMAGE_TOO_LARGE", "INVALID_IMAGE_DATA_OVERRUN", "IMAGE_CONFIRMATION_DENIED",
    "IMAGE_SETTING_TEST_TO_ACTIVE_DENIED", "ACTIVE_SLOT_NOT_KNOWN",
)
IMG_ERROR_HINTS = {
    3: "the slot holds no image",
    8: "no slot holds an image with this hash",
    9: "slot 1 is in use: a test image runs, or an update is pending",
    22: "the first chunk is shorter than an MCUboot header",
    23: "not an MCUboot image",
    27: "the unit runs this version or a newer one, and the upload asked for an upgrade only",
    28: "an image operation is pending; reset the unit, or erase the slot",
    30: "the image does not fit the slot (less the swap trailer)",
    32: "the image cannot be confirmed now",
}
IMG_ERRORS = {code: name + (f": {IMG_ERROR_HINTS[code]}" if code in IMG_ERROR_HINTS else "")
              for code, name in enumerate(IMG_ERROR_NAMES) if code}

# The dfu group's errors (SmpGroupId::DFU), which the unit's upload check raises while an image arrives.
DFU_ERRORS = {
    2: "NO_IDENTITY: the image carries no product and board identity (TLV 0x00A0)",
    3: "WRONG_PRODUCT: the image is for another product",
    4: "WRONG_BOARD: the image is for another board",
    5: "BAD_IDENTITY: the identity TLV of the image is malformed",
}

GROUP_ERRORS = {Group.CONFIG: CONFIG_ERRORS, Group.ASSETS: ASSETS_ERRORS, Group.IMG: IMG_ERRORS,
                Group.DFU: DFU_ERRORS}
# MCUmgr's MGMT_ERR_ENOTSUP, which a unit answers at the top level for a group it does not serve.
MGMT_ERR_ENOTSUP = 8
MISSING_GROUP_HINTS = {
    Group.ASSETS: "the unit has no assets group (older firmware)",
    Group.IMG: "the unit has no image management (built without DFU)",
    Group.DFU: "the unit has no dfu group (older firmware, or built without DFU)",
}


class AssetsCommand(IntEnum):
    KINDS = 0
    LIST = 1
    READ = 2
    WRITE = 3
    DELETE = 4
    STAT = 5
    RENAME = 6


ASSET_KINDS = {"images": 0, "animations": 1, "sensor_scripts": 2, "can_scripts": 3, "logs": 4, "fonts": 5}


class SmpError(Exception):
    def __init__(self, rc: int, group: int | None, response: dict, request_group: int | None = None):
        self.rc = rc
        self.group = group
        self.response = response
        reason = GROUP_ERRORS.get(group, {}).get(rc, "")
        if group is None and rc == MGMT_ERR_ENOTSUP and request_group in MISSING_GROUP_HINTS:
            reason = MISSING_GROUP_HINTS[request_group]
        where = f"group {group} error" if group is not None else "MCUmgr error"
        message = f"{where} {rc}" + (f" ({reason})" if reason else "")
        # APPLY_FAILED carries the validator's reason in `msg`, img_mgmt's verbose errors in `rsn`.
        for key in ("msg", "rsn"):
            if isinstance(response.get(key), str) and response[key]:
                message += f": {response[key]}"
                break
        super().__init__(message)

    def is_config_error(self, rc: int) -> bool:
        return self.group == Group.CONFIG and self.rc == rc

    def is_assets_error(self, rc: int) -> bool:
        return self.group == Group.ASSETS and self.rc == rc

    def is_missing_group(self) -> bool:
        """Whether the unit answered that it does not serve the group of the request."""
        return self.group is None and self.rc == MGMT_ERR_ENOTSUP


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

    # Image chunks in flight by default: the target erases flash with interrupts off, and a second request on
    # the wire would arrive during the erase and be lost (firmware update plan, D3).
    image_window = 1
    max_packet_size: int | None = None

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

    def reconnect(self) -> None:
        """Nothing to do: the bus stays up while a unit restarts."""

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

    def pending_text(self) -> bytes:
        """The console line being assembled, such as a prompt that has no newline yet."""
        return self._text

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

    # See SmpCanLink.
    image_window = 1
    max_packet_size: int | None = None

    def __init__(self, port: str, baud: int = DEFAULT_BAUD, line_delay_s: float = 0.0,
                 show_console: bool = False) -> None:
        self._serial_module = require("serial", "pyserial")
        self._port_name = port
        self._baud = baud
        self._line_delay_s = line_delay_s
        self._show_console = show_console
        self._console: list[str] = []
        self._packets: collections.deque[bytes] = collections.deque()
        self._decoder = SerialFrameDecoder(self._on_console_line)
        self._boot_seen = False
        self._port = open_serial_port(self._serial_module, port, baud)

    def __enter__(self) -> "SmpSerialLink":
        return self

    def __exit__(self, *exc_info) -> None:
        self.close()

    def close(self) -> None:
        self._port.close()
        if self._show_console and self._decoder.corrupt_lines:
            print(f"console: {self._decoder.corrupt_lines} corrupt line(s), "
                  f"{self._decoder.dropped_packets} dropped packet(s)", file=sys.stderr)

    def reconnect(self) -> None:
        """Opens the port again: a unit that restarted may have dropped off the USB bus and come back."""
        with contextlib.suppress(OSError):
            self._port.close()
        self._port = open_serial_port(self._serial_module, self._port_name, self._baud)
        self._packets.clear()
        self._decoder = SerialFrameDecoder(self._on_console_line)

    @property
    def boot_seen(self) -> bool:
        """Whether the console showed a boot banner or the shell's prompt since expect_boot()."""
        pending = ANSI_ESCAPE.sub("", self._decoder.pending_text().decode("utf-8", errors="replace"))
        return self._boot_seen or any(marker in pending for marker in SERIAL_BOOT_MARKERS)

    def expect_boot(self) -> None:
        """Forgets the boot markers read so far; call it before whatever may reset the unit.

        The S3's USB-Serial-JTAG keeps its OUT FIFO over a warm reset, and a line written between the reset
        and the moment the app enables the receive interrupt stays in it: the driver does not drain it, no
        further interrupt comes, and every later write times out until a hardware reset (which reverts a test
        image). So after a reset nothing is written until wait_for_boot() says the unit is up; reading is harmless.
        """
        with contextlib.suppress(OSError):
            while self._port.in_waiting:
                self._pump()
        self._boot_seen = False

    def wait_for_boot(self, timeout_s: float, *, what: str = "waiting for the boot banner",
                      settle_s: float | None = None) -> bool:
        """Listens without writing until the console shows the boot banner or the prompt, then for the settle time.

        Returns False when nothing showed within @p timeout_s. A port that left the USB bus is opened again.
        """
        settle_s = SERIAL_BOOT_SETTLE_S if settle_s is None else settle_s
        started_s = time.monotonic()
        next_note_s = started_s + IMAGE_NOTE_INTERVAL_S
        while not self.boot_seen:
            now_s = time.monotonic()
            if now_s >= started_s + timeout_s:
                return False
            if now_s >= next_note_s:
                print(f"{what}: {format_duration(now_s - started_s)} so far", file=sys.stderr)
                next_note_s = now_s + IMAGE_NOTE_INTERVAL_S
            self._listen()
        settle_until_s = time.monotonic() + settle_s
        while time.monotonic() < settle_until_s:
            self._listen()
        return True

    def _listen(self) -> None:
        try:
            self._pump()
        except OSError:
            # The port is gone while the unit restarts (or a read failed): open it again; the next read says
            # whether that worked.
            with contextlib.suppress(OSError):
                self.reconnect()
            time.sleep(0.2)

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
        if any(marker in line for marker in SERIAL_BOOT_MARKERS):
            self._boot_seen = True
        self._console.append(line)
        if self._show_console:
            print(f"console: {line}", file=sys.stderr)


class BleReassembler:
    """Rebuilds `[route][SMP packet]` from the bytes of the notifications on the smp characteristic.

    Only the first fragment of a packet carries the route; the length in the SMP header ends the packet.
    A gap longer than BLE_REASSEMBLY_TIMEOUT_S inside a packet drops it, and so does a length no packet
    can have: a unit that fails to notify the middle of a response drops the rest of it, and the next
    packet would otherwise be read as the end of that one.
    """

    def __init__(self, max_packet: int | None = None) -> None:
        self._buffer = bytearray()
        self._last_s = 0.0
        self._max_packet = max_packet
        self.dropped = 0

    def accept(self, data: bytes, now_s: float) -> list[tuple[int, bytes]]:
        if self._buffer and now_s - self._last_s > BLE_REASSEMBLY_TIMEOUT_S:
            self._drop()
        self._last_s = now_s
        self._buffer += data

        packets = []
        while len(self._buffer) >= 1 + SMP_HEADER_SIZE:
            size = SMP_HEADER_SIZE + int.from_bytes(self._buffer[3:5], "big")
            if self._max_packet is not None and size > self._max_packet:
                self._drop()
                break
            if len(self._buffer) < 1 + size:
                break
            packets.append((self._buffer[0], bytes(self._buffer[1:1 + size])))
            del self._buffer[:1 + size]
        return packets

    def _drop(self) -> None:
        self.dropped += 1
        self._buffer.clear()


class SmpBleLink:
    """SMP through a unit's Bluetooth access point, as the configurator app talks to it.

    Every packet on the smp characteristic is `[route][SMP packet]`: route 0 reaches the access point
    itself, another CDMP device ID is forwarded over CAN, and the route of a response is the unit that
    answered. A request goes out as ATT writes without response of at most MTU - 3 bytes; the route
    is in the first only, and the length in the SMP header ends the packet. Responses arrive as
    notifications. bleak is asyncio; its loop runs on a thread of its own so that the synchronous
    SmpClient and SmpPipeline work as they do on the other links.
    """

    max_packet_size: int | None = None

    def __init__(self, target: str, *, route: int = 0, pair: bool = True,
                 scan_timeout_s: float = DEFAULT_BLE_SCAN_TIMEOUT_S) -> None:
        self._bleak = require("bleak", "bleak")
        self._bleak_exc = require("bleak.exc", "bleak")
        self.route = route
        self.stray_packets = 0
        self._target = target
        self._pair = pair
        self._scan_timeout_s = scan_timeout_s
        self._client = None
        self._smp = None
        self._write_size = BLE_MIN_WRITE_SIZE
        self._info_window = 1
        self._reassembler = BleReassembler()
        self._packets: queue.Queue = queue.Queue()
        self._lost = threading.Event()
        self._closing = False

        self._loop = asyncio.new_event_loop()
        self._thread = threading.Thread(target=self._run_loop, name="ble", daemon=True)
        self._thread.start()
        try:
            self._connect_blocking()
        except BaseException:
            self.close()
            raise

    def __enter__(self) -> "SmpBleLink":
        return self

    def __exit__(self, *exc_info) -> None:
        self.close()

    @property
    def image_window(self) -> int:
        """Image chunks in flight by default: the access point's window, 1 for a unit behind CAN (D3)."""
        return self._info_window if self.route == 0 else 1

    def close(self) -> None:
        self._closing = True
        with contextlib.suppress(Exception):
            self._run(self._disconnect(), BLE_WRITE_TIMEOUT_S)
        self._loop.call_soon_threadsafe(self._loop.stop)
        self._thread.join(5.0)
        if not self._thread.is_alive():
            self._loop.close()
        if self.stray_packets or self._reassembler.dropped:
            print(f"bluetooth: {self.stray_packets} packet(s) from another route, "
                  f"{self._reassembler.dropped} dropped partial packet(s)", file=sys.stderr)

    def reconnect(self) -> None:
        """Connects again, after the unit restarted or the link dropped; scans for the unit once more."""
        with contextlib.suppress(Exception):
            self._run(self._disconnect(), BLE_WRITE_TIMEOUT_S)
        self._connect_blocking()

    def send(self, packet: bytes) -> None:
        if self._lost.is_set() or self._client is None:
            raise ConnectionError("the Bluetooth link was lost")
        data = bytes([self.route]) + packet
        pieces = [data[offset:offset + self._write_size] for offset in range(0, len(data), self._write_size)]
        self._run(self._write(pieces), BLE_WRITE_TIMEOUT_S)

    def receive(self, timeout_s: float) -> bytes | None:
        deadline_s = time.monotonic() + timeout_s
        while True:
            try:
                item = self._packets.get(timeout=max(deadline_s - time.monotonic(), 0.0))
            except queue.Empty:
                if self._lost.is_set() or self._client is None:
                    raise ConnectionError("the Bluetooth link was lost") from None
                return None
            if item is None:
                raise ConnectionError("the Bluetooth link was lost")
            route, packet = item
            if route != self.route:
                self.stray_packets += 1
                continue
            return packet

    def _run_loop(self) -> None:
        asyncio.set_event_loop(self._loop)
        self._loop.run_forever()

    def _run(self, coroutine, timeout_s: float):
        """Runs a coroutine on the loop's thread; whatever goes wrong on the link is a ConnectionError."""
        future = asyncio.run_coroutine_threadsafe(coroutine, self._loop)
        try:
            return future.result(timeout_s)
        except (concurrent.futures.TimeoutError, asyncio.TimeoutError):
            future.cancel()
            raise ConnectionError("a Bluetooth operation timed out") from None
        except ConnectionError:
            raise
        except self._bleak_exc.BleakError as error:
            raise ConnectionError(f"Bluetooth: {error}") from None
        except OSError as error:
            raise ConnectionError(f"Bluetooth: {error} (is there an adapter, and BlueZ running?)") from None

    def _connect_blocking(self) -> None:
        while not self._packets.empty():
            self._packets.get_nowait()
        self._lost.clear()
        self._reassembler = BleReassembler()
        timeout_s = self._scan_timeout_s + BLE_CONNECT_TIMEOUT_S + BLE_PAIR_TIMEOUT_S + BLE_WRITE_TIMEOUT_S
        try:
            self._run(self._connect(), timeout_s)
        except BaseException:
            # A connect that failed (the unit is still swapping and does not advertise) leaves no client: the
            # link stays lost, and the next send or receive says so instead of finding nothing to write to.
            self._lost.set()
            raise

    async def _find_device(self):
        scanner = self._bleak.BleakScanner
        if BLE_ADDRESS.fullmatch(self._target):
            device = await scanner.find_device_by_address(self._target, timeout=self._scan_timeout_s)
        else:
            wanted = self._target.lower()
            device = await scanner.find_device_by_filter(
                lambda device, advertisement: wanted in ((device.name or "").lower(),
                                                         (advertisement.local_name or "").lower()),
                timeout=self._scan_timeout_s)
        if device is None:
            raise ConnectionError(f"no Bluetooth device {self._target!r} found within {self._scan_timeout_s:.0f} s")
        return device

    async def _connect(self) -> None:
        device = await self._find_device()
        client = self._bleak.BleakClient(device, disconnected_callback=self._on_disconnected,
                                         timeout=BLE_CONNECT_TIMEOUT_S)
        await client.connect()
        self._client = client
        try:
            if self._pair:
                await self._pair_link(client)

            info = bytes(await client.read_gatt_char(BLE_INFO_UUID))
            if len(info) < BLE_INFO.size:
                raise ConnectionError(f"the info characteristic holds {len(info)} B, expected {BLE_INFO.size}")
            version, max_packet, window = BLE_INFO.unpack_from(info)
            if version != BLE_PROTOCOL_VERSION:
                print(f"warning: the unit speaks access point protocol {version}, this client {BLE_PROTOCOL_VERSION}",
                      file=sys.stderr)
            self.max_packet_size = max_packet or None
            self._info_window = max(1, window)
            self._reassembler = BleReassembler(self.max_packet_size)

            self._smp = client.services.get_characteristic(BLE_SMP_UUID)
            if self._smp is None:
                raise ConnectionError("the unit has no smp characteristic: it is not an EerieLeap access point")
            self._write_size = self._get_write_size(client, self._smp)
            await client.start_notify(self._smp, self._on_notification)
        except BaseException:
            await self._disconnect()
            raise

    async def _pair_link(self, client) -> None:
        """Pairs (Just Works). The smp characteristic refuses writes on a link that is not encrypted, and a
        write without response that is refused is not reported, so the pairing has to be done first."""
        try:
            await client.pair()
        except NotImplementedError:
            pass  # CoreBluetooth pairs when the first encrypted access needs it.
        except self._bleak_exc.BleakError as error:
            raise ConnectionError(
                f"pairing failed: {error}; a unit that was reflashed needs its old pairing removed on this host "
                f"(Linux: bluetoothctl remove ADDRESS), and --ble-no-pair skips the step") from None

    @staticmethod
    def _get_write_size(client, characteristic) -> int:
        size = None
        with contextlib.suppress(Exception):
            size = characteristic.max_write_without_response_size
        if not isinstance(size, int) or size < BLE_MIN_WRITE_SIZE:
            size = max(getattr(client, "mtu_size", 0) - 3, BLE_MIN_WRITE_SIZE)
        return size

    async def _write(self, pieces: list[bytes]) -> None:
        for piece in pieces:
            client = self._client
            if client is None:
                raise ConnectionError("the Bluetooth link was lost")
            await client.write_gatt_char(self._smp, piece, response=False)

    async def _disconnect(self) -> None:
        client, self._client = self._client, None
        if client is not None:
            with contextlib.suppress(Exception):
                await client.disconnect()

    def _on_notification(self, characteristic, data: bytearray) -> None:
        for route, packet in self._reassembler.accept(bytes(data), time.monotonic()):
            self._packets.put((route, packet))

    def _on_disconnected(self, client) -> None:
        # A client that was dropped on purpose (a reconnect, a failed connect) is no longer self._client.
        if self._closing or client is not self._client:
            return
        self._lost.set()
        self._packets.put(None)


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
    def __init__(self, link: SmpCanLink | SmpSerialLink | SmpBleLink, timeout_s: float, retries: int) -> None:
        self._link = link
        self._timeout_s = timeout_s
        self._retries = retries
        self._sequence = random.randrange(256)

    @property
    def link(self) -> SmpCanLink | SmpSerialLink | SmpBleLink:
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
    # free and used are sent only while the volume is mounted; free is a hint, WRITE stays the authority.
    space = f"  free {entry['free']} B  used {entry['used']} B" if "free" in entry else ""
    return (f"kind {entry['kind']}  {entry['name']}  dir {entry['dir']}  access {format_access(entry['access'])}  "
            f"max {maximum}  ext {' '.join(entry['ext']) or '-'}  avail {'yes' if entry['avail'] else 'no'}"
            f"{space}")


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


class ImageError(RuntimeError):
    """A file that is not an MCUboot image, or whose structure is damaged."""


@dataclasses.dataclass(frozen=True)
class ImageIdentity:
    product_id: int
    board: str


@dataclasses.dataclass(frozen=True)
class ImageTlv:
    protected: bool
    tag: int
    value: bytes


@dataclasses.dataclass(frozen=True)
class ImageInfo:
    size: int                               # The file's size.
    load_address: int
    header_size: int
    image_size: int
    protected_size: int                     # The protected TLV area, with its 4-byte header; 0 if none.
    flags: int
    version: tuple[int, int, int, int]      # major, minor, revision, build number
    tlvs: tuple[ImageTlv, ...]
    trailing: int                           # Bytes after the last TLV area (padding, the swap trailer).
    digest: bytes                           # SHA-256 of the header, the image and the protected TLVs.

    def find(self, tag: int, protected: bool) -> bytes | None:
        return next((tlv.value for tlv in self.tlvs if tlv.tag == tag and tlv.protected == protected), None)

    @property
    def hash(self) -> bytes | None:
        """The image hash img_mgmt reports and `image test` takes: the SHA-256 TLV."""
        return self.find(IMAGE_TLV_SHA256, protected=False)

    @property
    def signature_tag(self) -> int | None:
        return next((tlv.tag for tlv in self.tlvs if not tlv.protected and tlv.tag in IMAGE_SIGNATURE_TLVS), None)

    @property
    def identity(self) -> ImageIdentity | None:
        value = self.find(IMAGE_TLV_IDENTITY, protected=True)
        return None if value is None else parse_identity(value)

    @property
    def has_unprotected_identity(self) -> bool:
        return self.find(IMAGE_TLV_IDENTITY, protected=False) is not None


def parse_identity(value: bytes) -> ImageIdentity:
    if len(value) < 4:
        raise ImageError(f"the identity TLV holds {len(value)} B, its header is 4 B")
    if value[0] != IMAGE_IDENTITY_FORMAT:
        raise ImageError(f"the identity TLV has format {value[0]}, this client reads {IMAGE_IDENTITY_FORMAT}")
    board_length = value[3]
    if len(value) != 4 + board_length:
        raise ImageError(f"the identity TLV holds {len(value) - 4} B of board name, its length says {board_length}")
    try:
        board = value[4:].decode("ascii")
    except UnicodeDecodeError:
        raise ImageError("the board name of the identity TLV is not ASCII") from None
    return ImageIdentity(int.from_bytes(value[1:3], "little"), board)


def read_image_tlvs(data: bytes, start: int, magic: int, protected: bool) -> tuple[list[ImageTlv], int]:
    """Reads one TLV area at @p start. Returns its entries and the offset after it."""
    name = "protected" if protected else "unprotected"
    if start + IMAGE_TLV_HEADER.size > len(data):
        raise ImageError(f"the file ends at {len(data)} B, before the {name} TLV area at {start}")
    found, total = IMAGE_TLV_HEADER.unpack_from(data, start)
    if found != magic:
        raise ImageError(f"the {name} TLV area at {start} has magic {found:#06x}, expected {magic:#06x}")
    end = start + total
    if total < IMAGE_TLV_HEADER.size or end > len(data):
        raise ImageError(f"the {name} TLV area at {start} claims {total} B, the file holds {len(data) - start}")

    tlvs = []
    position = start + IMAGE_TLV_HEADER.size
    while position < end:
        if position + IMAGE_TLV_HEADER.size > end:
            raise ImageError(f"a {name} TLV entry at {position} runs past its area")
        tag, length = IMAGE_TLV_HEADER.unpack_from(data, position)
        position += IMAGE_TLV_HEADER.size
        if position + length > end:
            raise ImageError(f"the {name} TLV {tag:#06x} at {position - 4} is {length} B and runs past its area")
        tlvs.append(ImageTlv(protected, tag, data[position:position + length]))
        position += length
    return tlvs, end


def parse_image(data: bytes) -> ImageInfo:
    """Reads the header and TLVs of an MCUboot image (a .signed.bin) and checks that they fit the file."""
    if len(data) < IMAGE_HEADER.size:
        raise ImageError(f"{len(data)} B is shorter than an MCUboot header ({IMAGE_HEADER.size} B)")
    (magic, load_address, header_size, protected_size, image_size, flags,
     major, minor, revision, build, _) = IMAGE_HEADER.unpack_from(data)
    if magic != IMAGE_MAGIC:
        raise ImageError(f"magic {magic:#010x}, not an MCUboot image ({IMAGE_MAGIC:#010x})")
    if header_size < IMAGE_HEADER.size:
        raise ImageError(f"header size {header_size} B is smaller than the header")

    tlvs: list[ImageTlv] = []
    position = header_size + image_size
    if protected_size:
        protected, end = read_image_tlvs(data, position, IMAGE_TLV_PROTECTED_MAGIC, protected=True)
        if end - position != protected_size:
            raise ImageError(f"the header says {protected_size} B of protected TLVs, the area holds {end - position} B")
        tlvs += protected
        position = end
    unprotected, position = read_image_tlvs(data, position, IMAGE_TLV_MAGIC, protected=False)
    tlvs += unprotected

    signed_end = header_size + image_size + protected_size
    return ImageInfo(len(data), load_address, header_size, image_size, protected_size, flags,
                     (major, minor, revision, build), tuple(tlvs), len(data) - position,
                     hashlib.sha256(data[:signed_end]).digest())


def format_image_version(version: tuple[int, int, int, int]) -> str:
    """imgtool's spelling: major.minor.revision+build."""
    return "{}.{}.{}+{}".format(*version)


def parse_version_string(text: str) -> tuple[int, int, int, int]:
    """img_mgmt's version string, `major.minor.revision` with `.build` when the build number is not 0."""
    try:
        numbers = [int(part) for part in text.split(".")]
    except ValueError:
        raise ValueError(f"{text!r} is not a version") from None
    if not 3 <= len(numbers) <= 4:
        raise ValueError(f"{text!r} is not a version")
    return (numbers[0], numbers[1], numbers[2], numbers[3] if len(numbers) == 4 else 0)


def format_image_flags(flags: int) -> str:
    names = [name for bit, name in IMAGE_FLAGS.items() if flags & bit]
    rest = flags & ~sum(IMAGE_FLAGS)
    if rest:
        names.append(f"{rest:#x}")
    return f"{flags:#010x} ({' '.join(names) or 'none'})"


def format_tlv(tlv: ImageTlv) -> str:
    name = IMAGE_TLV_NAMES.get(tlv.tag, "unknown")
    line = f"  {tlv.tag:#06x} {name}  {len(tlv.value)} B"
    if tlv.tag in (IMAGE_KEY_HASH_TLV, IMAGE_TLV_SHA256, 0x11, 0x12):
        line += f"  {tlv.value.hex()}"
    return line


def print_image_info(path: Path, info: ImageInfo) -> bool:
    """Prints what is known about an image. Returns False when its own digest does not match its hash TLV."""
    print(f"file: {path} ({info.size} B)")
    print(f"magic: {IMAGE_MAGIC:#010x}")
    print(f"version: {format_image_version(info.version)}")
    print(f"load address: {info.load_address:#010x}")
    print(f"header size: {info.header_size} B")
    print(f"image size: {info.image_size} B")
    print(f"protected TLV size: {info.protected_size} B")
    print(f"flags: {format_image_flags(info.flags)}")

    for protected in (True, False):
        entries = [tlv for tlv in info.tlvs if tlv.protected == protected]
        if entries or not protected:
            magic = IMAGE_TLV_PROTECTED_MAGIC if protected else IMAGE_TLV_MAGIC
            print(f"{'protected' if protected else 'unprotected'} TLVs (magic {magic:#06x}):")
            for tlv in entries:
                print(format_tlv(tlv))
    if info.trailing:
        print(f"trailing bytes: {info.trailing} B (a padded image; an upload sends them too)")

    try:
        identity = info.identity
        if identity is not None:
            print(f"identity: product {identity.product_id:#06x}, board {identity.board}")
        else:
            print("identity: none (a unit with the dfu group refuses the image)"
                  + ("; the identity TLV is in the unprotected area, where it is not signed"
                     if info.has_unprotected_identity else ""))
    except ImageError as error:
        print(f"identity: malformed ({error})")

    signature = info.signature_tag
    if signature is None:
        print("signature: none")
    else:
        print(f"signature: {IMAGE_TLV_NAMES.get(signature, 'unknown')} ({signature:#04x})")
    key_hash = info.find(IMAGE_KEY_HASH_TLV, protected=False)
    if key_hash is not None:
        print(f"key hash: {key_hash.hex()}")
    if info.protected_size > IMAGE_MAX_PROTECTED_SIZE:
        print(f"warning: {info.protected_size} B of protected TLVs; the unit refuses more than "
              f"{IMAGE_MAX_PROTECTED_SIZE} B")

    image_hash = info.hash
    if image_hash is None:
        print("hash: none (no SHA256 TLV)")
        return False
    print(f"hash: {image_hash.hex()}")
    if image_hash != info.digest:
        print(f"digest of header, image and protected TLVs: {info.digest.hex()}  DOES NOT MATCH the hash")
        return False
    print("digest of header, image and protected TLVs: matches the hash")
    return True


@dataclasses.dataclass(frozen=True)
class ImageSlot:
    """One entry of img_mgmt's state read. A flag that is absent is false (the list may be frugal)."""
    image: int
    slot: int
    version: str
    hash: bytes
    bootable: bool
    pending: bool
    confirmed: bool
    active: bool
    permanent: bool

    @classmethod
    def from_entry(cls, entry: dict) -> "ImageSlot":
        flags = ("bootable", "pending", "confirmed", "active", "permanent")
        return cls(entry.get("image", 0), entry["slot"], entry.get("version", "?"), bytes(entry.get("hash", b"")),
                   *(bool(entry.get(flag, False)) for flag in flags))

    @property
    def version_tuple(self) -> tuple[int, int, int, int] | None:
        try:
            return parse_version_string(self.version)
        except ValueError:
            return None

    def describe(self) -> str:
        flags = [name for name in ("active", "confirmed", "pending", "permanent", "bootable") if getattr(self, name)]
        where = f"image {self.image} slot {self.slot}" if self.image else f"slot {self.slot}"
        line = f"{where}  version {self.version}  hash {self.hash.hex()}  {' '.join(flags) or 'no flags'}"
        if self.pending:
            line += "  (permanent swap at the next reset)" if self.permanent else "  (test boot at the next reset)"
        elif not self.active and not self.confirmed and not self.permanent:
            line += "  (not selected)"
        return line


def images_from(response: dict) -> list[ImageSlot]:
    return [ImageSlot.from_entry(entry) for entry in response.get("images", [])]


def read_images(client: "SmpClient") -> list[ImageSlot]:
    return images_from(client.read(Group.IMG, IMG_STATE))


def find_running(images: list[ImageSlot]) -> ImageSlot | None:
    return next((entry for entry in images if entry.active), None)


def find_by_hash(images: list[ImageSlot], image_hash: bytes) -> ImageSlot | None:
    return next((entry for entry in images if entry.hash == image_hash), None)


def parse_image_hash(text: str) -> bytes:
    """An image hash as 64 hex digits, as `image list` prints it."""
    digits = text[2:] if text.lower().startswith("0x") else text
    try:
        value = bytes.fromhex(digits)
    except ValueError:
        value = b""
    if len(value) != 32:
        raise argparse.ArgumentTypeError(f"{text!r} is not an image hash (64 hex digits, as `image list` shows it)")
    return value


def format_reset_cause(bits: int) -> str:
    names = [name for index, name in enumerate(RESET_CAUSES) if bits & 1 << index]
    rest = bits & ~((1 << len(RESET_CAUSES)) - 1)
    if rest:
        names.append(f"{rest:#x}")
    return f"{bits:#010x} ({' '.join(names) or 'none'})"


def read_dfu_status(client: "SmpClient") -> dict:
    return client.read(Group.DFU, DFU_STATUS)


def format_dfu_state(state: int) -> str:
    return f"{DFU_STATES.get(state, 'unknown')} ({state})"


def image_chunk_sizes(buf_size: int, *, is_serial: bool, max_packet: int | None, upgrade: bool) -> tuple[int, int]:
    """The data of the first upload chunk and of the others, for a unit whose packets hold @p buf_size bytes.

    The others are buf_size - 64 as the plan has it. The first carries the whole image's size and SHA-256
    besides, so it is encoded to see how much room is left. A request that does not fit is dropped by the unit's
    transport without an answer.
    """
    available = buf_size - (SERIAL_FRAME_OVERHEAD if is_serial else 0)
    if max_packet:
        available = min(available, max_packet)
    first_body = {"image": 0, "len": 0xFFFFFFFF, "sha": bytes(32), "off": 0, "data": b""}
    if upgrade:
        first_body["upgrade"] = True
    # The empty `data` has a 1-byte head; up to 65535 B take 3.
    first_overhead = len(encode_request(Op.WRITE, Group.IMG, IMG_UPLOAD, first_body, 0)) + 2
    first, rest = available - first_overhead, available - IMAGE_WRITE_OVERHEAD
    if first < IMAGE_HEADER.size or rest < 1:
        raise RuntimeError(f"the unit's packets of {available} B are too small for image chunks "
                           f"(the first needs more than {first_overhead + IMAGE_HEADER.size} B)")
    return first, rest


class UploadLost(Exception):
    """The transfer broke off. It can start over: the unit answers a first chunk with the offset it has."""

    def __init__(self, message: str, after_last_chunk: bool = False) -> None:
        super().__init__(message)
        self.after_last_chunk = after_last_chunk


def send_image(client: "SmpClient", image: bytes, sha: bytes, *, window: int, first_size: int, chunk_size: int,
               upgrade: bool, progress: Progress) -> int:
    """Sends the image from the offset the unit reports. Returns the unit's offset after the first chunk.

    That is the first chunk's size for a new transfer, more for one that resumed, and the image's size when the
    slot held the image already.

    The first chunk goes alone, at offset 0 with the size and the SHA-256 of the whole file: a unit that holds
    a partial upload with that sha answers with its offset, and one whose slot holds the image already answers
    with the image's size. The other chunks follow in order, @p window in flight. The unit writes a chunk
    only at the offset it expects and otherwise answers with that offset, so an answer that does not lead to
    the next chunk (a lost chunk, a repeated one) makes the client let the window drain and continue from the
    unit's offset.
    """
    total = len(image)

    def chunk_at(offset: int) -> bytes:
        return image[offset:offset + (first_size if offset == 0 else chunk_size)]

    def body(offset: int) -> dict:
        request = {"off": offset, "data": chunk_at(offset)}
        if offset == 0:
            request |= {"image": 0, "len": total, "sha": sha}
            if upgrade:
                request["upgrade"] = True
        return request

    def check(response: dict) -> int:
        offset = response.get("off")
        if not isinstance(offset, int) or not 0 <= offset <= total:
            raise RuntimeError(f"the unit expects offset {offset!r} of an image of {total} B")
        if response.get("match") is False:
            raise RuntimeError("the unit's SHA-256 check of the uploaded slot failed: the image did not arrive intact")
        return offset

    def send_first() -> int:
        """The first chunk, alone: the unit may hash the old slot content before it answers."""
        try:
            response = client.write(Group.IMG, IMG_UPLOAD, body(0),
                                    timeout_s=max(client.timeout_s, IMAGE_FIRST_CHUNK_TIMEOUT_S))
        except TimeoutError:
            raise UploadLost("no response to the first chunk") from None
        offset = check(response)
        progress.update(offset, total)
        return offset

    start = unit_offset = send_first()

    pipeline = SmpPipeline(client)
    last_timeout_s = max(client.timeout_s, IMAGE_LAST_CHUNK_TIMEOUT_S)
    next_offset = unit_offset
    draining = False
    rewound_at, stalls = -1, 0

    def submit(offset: int) -> int:
        end = offset + len(chunk_at(offset))
        is_last = end >= total
        # The answer to the last chunk waits for the slot check, and a chunk sent again after it would meet
        # a transfer that is over, so it is sent once.
        pipeline.submit(Op.WRITE, Group.IMG, IMG_UPLOAD, body(offset), (offset, end),
                        timeout_s=last_timeout_s if is_last else client.timeout_s * window,
                        retries=0 if is_last else None)
        return end

    while True:
        while not draining and pipeline.outstanding < window and next_offset < total:
            next_offset = submit(next_offset)

        if not pipeline.outstanding:
            if unit_offset >= total:
                return start
            # Everything sent was answered and the unit is behind: continue from where it says it is.
            stalls = stalls + 1 if unit_offset <= rewound_at else 0
            if stalls > IMAGE_MAX_STALLS:
                raise RuntimeError(f"the unit keeps answering offset {unit_offset} of {total}: "
                                   f"the upload makes no progress")
            rewound_at, draining = unit_offset, False
            if unit_offset == 0:
                # The unit has no transfer (it lost it, or finished one): a new one starts with the first chunk
                # alone, which also lets the unit tell that the slot holds the image already. Pipelined behind
                # it, the next chunk would meet the state that first chunk resets.
                unit_offset = send_first()
            next_offset = unit_offset
            continue

        completion = pipeline.next_completion()
        offset, end = completion.tag
        if isinstance(completion.error, TimeoutError):
            where = "the last chunk" if end >= total else f"the chunk at offset {offset}"
            raise UploadLost(f"no response to {where}", after_last_chunk=end >= total)
        if completion.error is not None:
            raise completion.error

        unit_offset = check(completion.response)
        progress.update(unit_offset, total)
        if unit_offset >= total:
            # The unit has all of it (its SHA-256 check did not fail, or check() would have raised). Answers to
            # chunks still in flight come from a transfer that is over, and would say offset 0.
            return start
        if unit_offset != end:
            draining = True


def reconnect_link(link, window_s: float) -> None:
    """Brings a link back up after the unit restarted or the connection dropped, trying for @p window_s."""
    deadline_s = time.monotonic() + window_s
    delay_s = IMAGE_POLL_INTERVAL_S
    while True:
        try:
            link.reconnect()
            return
        except OSError:
            if time.monotonic() + delay_s > deadline_s:
                raise
            time.sleep(delay_s)
            delay_s = min(delay_s * 1.5, IMAGE_POLL_MAX_INTERVAL_S)


def upload_image(client: "SmpClient", image: bytes, sha: bytes, *, window: int, first_size: int, chunk_size: int,
                 upgrade: bool, progress: Progress) -> int:
    """Uploads an image and returns the unit's offset after the first chunk (see send_image).

    A transfer that broke off, or whose link dropped, starts over at offset 0 with the same sha, and the unit
    answers with the offset it has. The last chunk's answer can be lost after the unit stored the image: the
    slot list then says whether it did.
    """
    for attempt in range(IMAGE_UPLOAD_ATTEMPTS):
        reconnect = False
        try:
            return send_image(client, image, sha, window=window, first_size=first_size, chunk_size=chunk_size,
                              upgrade=upgrade, progress=progress)
        except UploadLost as error:
            problem = str(error)
            if error.after_last_chunk and slot_holds_image(client, image):
                progress.finish()
                print(f"{problem}, but the slot holds the image")
                return 0
        except OSError as error:
            problem, reconnect = f"the link failed: {error}", True

        progress.finish()
        if attempt + 1 == IMAGE_UPLOAD_ATTEMPTS:
            raise RuntimeError(f"the upload was lost: {problem}")
        print(f"{problem}; resuming", file=sys.stderr)
        if reconnect:
            reconnect_link(client.link, IMAGE_RECONNECT_WINDOW_S)
        else:
            time.sleep(IMAGE_RESUME_PAUSE_S)

    raise AssertionError("unreachable")


def slot_holds_image(client: "SmpClient", image: bytes) -> bool:
    """Whether a slot of the unit holds this image, by the hash TLV of the file."""
    try:
        wanted = parse_image(image).hash
        return wanted is not None and find_by_hash(read_images(client), wanted) is not None
    except (SmpError, TimeoutError, OSError, ImageError):
        return False


@dataclasses.dataclass(frozen=True)
class UploadPlan:
    window: int
    first_size: int
    chunk_size: int


def plan_upload(client: "SmpClient", in_flight: int | None, upgrade: bool) -> UploadPlan:
    """Chunk sizes from the unit's packet size, and the window: 1 on CAN and serial and for a BLE route to a unit
    behind CAN, else the access point's window; either way below the unit's packet count."""
    params = client.read(Group.OS, 6)
    link = client.link
    first, rest = image_chunk_sizes(params["buf_size"], is_serial=isinstance(link, SmpSerialLink),
                                    max_packet=link.max_packet_size, upgrade=upgrade)
    requested = in_flight if in_flight is not None else link.image_window
    window = limit_window(requested, params.get("buf_count"))
    if window != requested:
        print(f"warning: the unit has {params.get('buf_count')} packets, shared by requests and responses: "
              f"{window} in flight instead of {requested}", file=sys.stderr)
    if in_flight is not None and in_flight > link.image_window:
        print(f"warning: {in_flight} chunks in flight; the default for this link is {link.image_window}",
              file=sys.stderr)
    return UploadPlan(window, first, rest)


def poll_device_info(client: "SmpClient", *, deadline_s: float, expect_restart: bool, what: str) -> dict:
    """Asks for device info until the unit answers, with a growing pause between the tries.

    With @p expect_restart the unit must go silent first: right after the reset command it still answers for a
    moment. A unit that never goes silent within IMAGE_RESTART_GRACE_S did not restart, and its answer is
    returned all the same. A link that dropped (Bluetooth, a console that left the USB bus) is brought back up.
    """
    started_s = time.monotonic()
    seen_silence = not expect_restart
    delay_s = IMAGE_POLL_INTERVAL_S
    next_note_s = started_s + IMAGE_NOTE_INTERVAL_S
    while True:
        now_s = time.monotonic()
        if now_s > deadline_s:
            raise TimeoutError(f"the unit did not answer again within the time given ({what})")
        try:
            info = client.read(Group.DEVICE, 0, retries=0)
        except (TimeoutError, SmpError, OSError) as error:
            seen_silence = True
            if isinstance(error, OSError) and not isinstance(error, TimeoutError):
                with contextlib.suppress(OSError):
                    client.link.reconnect()
        else:
            if seen_silence or now_s - started_s > IMAGE_RESTART_GRACE_S:
                if not seen_silence:
                    print("warning: the unit never went silent; it may not have restarted", file=sys.stderr)
                return info
            delay_s = IMAGE_POLL_INTERVAL_S

        if now_s >= next_note_s:
            print(f"{what}: {format_duration(now_s - started_s)} so far", file=sys.stderr)
            next_note_s = now_s + IMAGE_NOTE_INTERVAL_S
        time.sleep(delay_s)
        if seen_silence:
            delay_s = min(delay_s * 1.5, IMAGE_POLL_MAX_INTERVAL_S)


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
        if key in ("event_channels", "settings"):
            print(f"{key}: {' '.join(str(item) for item in value) or '-'}")
        else:
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
    listens = hasattr(client.link, "wait_for_boot")
    if listens:
        client.link.expect_boot()
    try:
        client.write(Group.OS, OS_RESET, {}, retries=0)
    except TimeoutError:
        print("no response; the unit may have restarted")
    else:
        print("reset requested")
    if not listens:
        return

    # On a console nothing may be written while the unit boots (see SmpSerialLink.expect_boot); a command that
    # follows this one is safe once it returns.
    print(f"listening for the boot banner (up to {format_duration(args.timeout_s)}; a swap takes up to ~2.5 min)")
    if client.link.wait_for_boot(args.timeout_s, what="waiting for the boot banner"):
        print("the unit is up")
    else:
        print(f"warning: no boot banner within {format_duration(args.timeout_s)}; give the unit time before the "
              f"next command", file=sys.stderr)


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


def format_utc_ms(unix_ms: int) -> str:
    moment = datetime.datetime.fromtimestamp(unix_ms / 1000, datetime.timezone.utc)
    return moment.strftime("%Y-%m-%dT%H:%M:%SZ")


def format_duration(seconds: float) -> str:
    seconds = max(0, int(round(seconds)))
    if seconds < 60:
        return f"{seconds} s"
    if seconds < 3600:
        return f"{seconds // 60} min"
    return f"{seconds // 3600} h {seconds % 3600 // 60} min"


def format_size(size: int) -> str:
    if size < 1024:
        return f"{size} B"
    value = float(size)
    for unit in ("KiB", "MiB", "GiB"):
        value /= 1024
        if value < 1024 or unit == "GiB":
            return f"{value:.1f} {unit}"
    raise AssertionError("unreachable")


def log_start_ms(name: str) -> int | None:
    """The session start a log file name carries, as Unix ms UTC; None when the name does not follow the naming."""
    match = LOG_NAME_PATTERN.search(name)
    if not match:
        return None
    try:
        moment = datetime.datetime.strptime(match.group(1), "%Y%m%dT%H%M%SZ").replace(tzinfo=datetime.timezone.utc)
    except ValueError:
        return None
    return int(moment.timestamp() * 1000)


def format_asset_entry(entry: dict) -> str:
    line = f"{entry['size']:>10}  {entry['name']}"
    # end_ms (logs kind): when the file was last written, Unix ms UTC.
    if "end_ms" in entry:
        line += f"  end {format_utc_ms(entry['end_ms'])}"
        start_ms = log_start_ms(entry["name"])
        if start_ms is not None and entry["end_ms"] >= start_ms:
            line += f"  duration {format_duration((entry['end_ms'] - start_ms) / 1000)}"
    return line


def command_assets_ls(client: SmpClient, args: argparse.Namespace) -> None:
    for entry in list_assets(client, args.kind):
        print(format_asset_entry(entry))


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


def command_assets_stat(client: SmpClient, args: argparse.Namespace) -> None:
    response = client.read(Group.ASSETS, AssetsCommand.STAT, {"kind": args.kind, "name": args.name})
    # No crc for a file above the unit's STAT limit that no checkpoint covers.
    crc = f"{response['crc']:#010x}" if "crc" in response else "-"
    print(f"len: {response['len']}")
    print(f"crc: {crc}")


def command_assets_rename(client: SmpClient, args: argparse.Namespace) -> None:
    client.write(Group.ASSETS, AssetsCommand.RENAME, {"kind": args.kind, "name": args.name, "to": args.to})
    print(f"renamed {args.name} to {args.to}")


ASSETS_HANDLERS = {
    "kinds": command_assets_kinds,
    "ls": command_assets_ls,
    "get": command_assets_get,
    "put": command_assets_put,
    "rm": command_assets_rm,
    "stat": command_assets_stat,
    "rename": command_assets_rename,
}


def command_assets(client: SmpClient, args: argparse.Namespace) -> None:
    ASSETS_HANDLERS[args.assets_command](client, args)


def command_logging_status(client: SmpClient, args: argparse.Namespace) -> None:
    response = client.read(Group.LOGGING, LOGGING_STATUS)
    active = bool(response["active"])
    print(f"active: {'yes' if active else 'no'}")
    if not active:
        reason = response.get("reason", 0)
        print(f"reason: {LOGGING_REASONS.get(reason, f'unknown({reason})')}")
    if "since_ms" in response:
        since_ms = response["since_ms"]
        elapsed = format_duration(time.time() - since_ms / 1000)
        print(f"since: {format_utc_ms(since_ms)} ({elapsed} ago)")
    if "file" in response:
        print(f"file: {response['file']}")
    if "bytes" in response:
        print(f"bytes: {response['bytes']} ({format_size(response['bytes'])})")


def logging_request(args: argparse.Namespace) -> dict | None:
    return {"uid": args.uid} if args.uid is not None else None


def command_logging_start(client: SmpClient, args: argparse.Namespace) -> None:
    client.write(Group.LOGGING, LOGGING_START, logging_request(args))
    print("logging start requested" + (f" for unit {args.uid:#010x}" if args.uid is not None else ""))


def command_logging_stop(client: SmpClient, args: argparse.Namespace) -> None:
    client.write(Group.LOGGING, LOGGING_STOP, logging_request(args))
    print("logging stop requested" + (f" for unit {args.uid:#010x}" if args.uid is not None else ""))


LOGGING_HANDLERS = {
    "status": command_logging_status,
    "start": command_logging_start,
    "stop": command_logging_stop,
}


def command_logging(client: SmpClient, args: argparse.Namespace) -> None:
    LOGGING_HANDLERS[args.logging_command](client, args)


def command_ble_scan(client: None, args: argparse.Namespace) -> None:
    bleak = require("bleak", "bleak")

    async def scan() -> dict:
        return await bleak.BleakScanner.discover(timeout=args.time, return_adv=True)

    try:
        found = asyncio.run(scan())
    except (OSError, require("bleak.exc", "bleak").BleakError) as error:
        raise RuntimeError(f"Bluetooth: {error} (is there an adapter, and BlueZ running?)") from None
    shown = 0
    for device, advertisement in sorted(found.values(), key=lambda entry: -(entry[1].rssi or -999)):
        is_unit = BLE_SERVICE_UUID in [uuid.lower() for uuid in advertisement.service_uuids]
        if not is_unit and not args.all:
            continue
        shown += 1
        manufacturer = " ".join(f"{company:#06x}:{data.hex()}"
                                for company, data in advertisement.manufacturer_data.items())
        print(f"{device.address}  {advertisement.local_name or device.name or '-':24}  rssi {advertisement.rssi}"
              f"  {'access point' if is_unit else '-'}" + (f"  manufacturer {manufacturer}" if manufacturer else ""))
    if not shown:
        print("no access point found" + ("" if args.all else "; --all lists every device"))


def command_image_info(client: None, args: argparse.Namespace) -> None:
    info = parse_image(args.file.read_bytes())
    if not print_image_info(args.file, info):
        raise ImageError("the image has no hash TLV, or its hash does not match its content")


def command_image_list(client: SmpClient, args: argparse.Namespace) -> None:
    images = read_images(client)
    for entry in images:
        print(entry.describe())
    if not images:
        print("no images")


def upload_with_report(client: SmpClient, image: bytes, in_flight: int | None, upgrade: bool) -> int:
    """Uploads an image, printing the plan and the result. Returns the unit's offset after the first chunk."""
    sha = hashlib.sha256(image).digest()
    plan = plan_upload(client, in_flight, upgrade)
    print(f"uploading {len(image)} B, sha256 {sha.hex()}, chunks of {plan.chunk_size} B, {plan.window} in flight")

    progress = Progress("image")
    start_s = time.monotonic()
    try:
        began = upload_image(client, image, sha, window=plan.window, first_size=plan.first_size,
                             chunk_size=plan.chunk_size, upgrade=upgrade, progress=progress)
    finally:
        progress.finish()
    elapsed_s = max(time.monotonic() - start_s, 1e-9)

    if began >= len(image) > plan.first_size:
        print("the slot holds this image already; nothing was sent")
        return began
    resumed_at = began if began > min(plan.first_size, len(image)) else 0
    sent = len(image) - resumed_at
    resumed = f", resumed at {resumed_at} B" if resumed_at else ""
    print(f"uploaded {sent} B in {elapsed_s:.1f} s ({sent / elapsed_s / 1024:.1f} KB/s{resumed})")
    return began


def command_image_upload(client: SmpClient, args: argparse.Namespace) -> None:
    image = args.file.read_bytes()
    info = parse_image(image)
    upload_with_report(client, image, args.in_flight, args.upgrade)

    try:
        slot = find_by_hash(read_images(client), info.hash) if info.hash else None
    except (SmpError, TimeoutError):
        slot = None
    if slot is not None:
        print(slot.describe())
    else:
        print("warning: the unit does not list an image with the hash of this file", file=sys.stderr)


def print_state(response: dict) -> list[ImageSlot]:
    images = images_from(response)
    for entry in images:
        print(entry.describe())
    return images


def command_image_test(client: SmpClient, args: argparse.Namespace) -> None:
    image_hash = args.hash
    if image_hash is None:
        backup = next((entry for entry in read_images(client) if entry.slot == 1), None)
        if backup is None:
            raise RuntimeError("slot 1 holds no image; upload one, or give the hash of an image")
        image_hash = backup.hash

    images = print_state(client.write(Group.IMG, IMG_STATE, {"hash": image_hash, "confirm": False}))
    entry = find_by_hash(images, image_hash)
    if entry is None or not entry.pending:
        print("warning: the image is not pending after the request", file=sys.stderr)
        return
    print("marked for test: it boots once after the next reset, and MCUboot reverts to the old image unless it "
          "is confirmed")


def command_image_confirm(client: SmpClient, args: argparse.Namespace) -> None:
    body: dict = {"confirm": True}
    if args.hash is not None:
        body["hash"] = args.hash
    print_state(client.write(Group.IMG, IMG_STATE, body))


# Erasing a slot of 4 MB takes the flash a while.
IMAGE_ERASE_TIMEOUT_S = 180.0


def command_image_erase(client: SmpClient, args: argparse.Namespace) -> None:
    print("erasing slot 1 (the slot that does not run)")
    client.write(Group.IMG, IMG_ERASE, {}, retries=0, timeout_s=max(client.timeout_s, IMAGE_ERASE_TIMEOUT_S))
    print("erased")


def command_dfu_status(client: SmpClient, args: argparse.Namespace) -> None:
    status = read_dfu_status(client)
    print(f"confirmed: {'yes' if status.get('confirmed') else 'no'}")
    print(f"state: {format_dfu_state(status.get('state', -1))}")
    if "confirm_in_ms" in status:
        print(f"confirm_in_ms: {status['confirm_in_ms']} ({format_duration(status['confirm_in_ms'] / 1000)})")
    print(f"reset_cause: {format_reset_cause(status.get('reset_cause', 0))}")


DFU_HANDLERS = {
    "status": command_dfu_status,
}


def command_dfu(client: SmpClient, args: argparse.Namespace) -> None:
    DFU_HANDLERS[args.dfu_command](client, args)


def describe_unit(device: dict) -> str:
    text = f"product {device['product']:#06x}, board {device.get('board', '?')}, sw {unpack_version(device['sw'])}"
    text += f" build {device['build']}"
    if "uid" in device:
        text += f", uid {device['uid']:#010x}"
    return text


def device_version(device: dict) -> tuple[int, int, int, int]:
    sw = device["sw"]
    return (sw >> 24, sw >> 16 & 0xFF, sw & 0xFFFF, device["build"])


def check_image_fits_unit(info: ImageInfo, device: dict) -> None:
    """The client's side of D5: the image's identity must be the unit's product and board."""
    try:
        identity = info.identity
    except ImageError as error:
        raise ImageError(f"the identity TLV of the image is malformed: {error}") from None
    if identity is None:
        raise ImageError("the image carries no identity TLV (0x00A0), so the unit would refuse it; "
                         "was it built with rt_core's signing wrapper?")
    if device["product"] != identity.product_id:
        raise RuntimeError(f"the image is for product {identity.product_id:#06x}, the unit is product "
                           f"{device['product']:#06x}")
    board = device.get("board")
    if board is None:
        print("warning: the unit does not report its board (older firmware); the board check is skipped",
              file=sys.stderr)
    elif board != identity.board:
        raise RuntimeError(f"the image is for board {identity.board}, the unit is {board}")

    unit_version = device_version(device)
    if info.version < unit_version:
        print(f"warning: downgrade, {format_image_version(info.version)} is older than the unit's "
              f"{format_image_version(unit_version)}", file=sys.stderr)
    elif info.version == unit_version:
        print(f"note: the unit runs version {format_image_version(unit_version)} already")


def check_slot_is_free(images: list[ImageSlot]) -> None:
    """Refuses what img_mgmt would: a test image runs (slot 1 is its backup), or an update is pending."""
    running = find_running(images)
    if running is None:
        raise RuntimeError("the unit reports no running image")
    if not running.confirmed:
        raise RuntimeError(f"the running image ({running.version}) is not confirmed, so slot 1 holds its backup and "
                           f"the unit refuses an upload: wait until it confirms itself (`dfu status`), or run "
                           f"`image confirm`")
    pending = next((entry for entry in images if entry.pending), None)
    if pending is not None:
        raise RuntimeError(f"an update is pending (slot {pending.slot}, version {pending.version}): reset the unit "
                           f"to apply it, or run `image erase`")


def request_reset(client: SmpClient) -> None:
    """Resets the unit. It restarts shortly after it answers, or its link goes away first: neither is a failure."""
    try:
        client.write(Group.OS, OS_RESET, {}, retries=0)
    except OSError:
        pass


def restart_unit(client: SmpClient, timeout_s: float, what: str) -> bool | None:
    """Resets the unit. On a console it then stops writing and listens until the unit prints its boot banner.

    Returns True when the unit announced its boot, False when the console showed nothing within @p timeout_s,
    and None on a link with no console to listen to (CAN, Bluetooth), where nothing changes.
    """
    link = client.link
    listens = hasattr(link, "wait_for_boot")
    if listens:
        link.expect_boot()
    request_reset(client)
    if not listens:
        return None
    print("not writing to the console until the unit prints its boot banner")
    return link.wait_for_boot(timeout_s, what=what)


def watch_confirmation(client: SmpClient, *, new_hash: bytes, timeout_s: float) -> bool:
    """Polls until the running image is the new one and confirmed. Raises when it was reverted or never confirms.

    A new image that fails is reset by its watchdog and MCUboot swaps back, which silences the unit for the
    swap again: the wait then starts over, and so does the time limit. A unit that answers but not the image
    and dfu requests does not start it over.

    Returns whether the image is confirmed. A unit without a dfu group never confirms itself, so the new image
    can be running and not confirmed, and the caller says so.
    """
    has_dfu = True
    last_state = None
    last_error: Exception | None = None
    deadline_s = time.monotonic() + timeout_s
    listens = hasattr(client.link, "wait_for_boot")
    while True:
        if time.monotonic() > deadline_s:
            detail = f" (last error: {last_error})" if last_error else ""
            raise TimeoutError(f"the new image did not confirm within {format_duration(timeout_s)}{detail}")
        if listens:
            client.link.expect_boot()
        status = None
        try:
            images = read_images(client)
            if has_dfu:
                try:
                    status = read_dfu_status(client)
                except SmpError as error:
                    if not error.is_missing_group():
                        raise
                    has_dfu = False
                    print("note: the unit has no dfu group, so the image will not confirm itself: run `image confirm`")
        except (TimeoutError, SmpError, OSError) as error:
            last_error = error
            if listens:
                # Silent: the unit may be rebooting (a failed image is reset by its watchdog, and MCUboot
                # swaps back). A console must not be written to meanwhile, so listen first.
                if client.link.wait_for_boot(timeout_s, what="the unit went silent; listening for its boot"):
                    deadline_s = time.monotonic() + timeout_s
                    continue
            try:
                client.read(Group.DEVICE, 0, retries=0)
            except (TimeoutError, SmpError, OSError):
                pass
            else:
                # The unit answers, so it did not go away; only these requests fail. Try again after a pause,
                # under the time limit already running.
                time.sleep(IMAGE_VERIFY_INTERVAL_S)
                continue
            poll_device_info(client, deadline_s=time.monotonic() + timeout_s, expect_restart=False,
                             what="the unit went silent again; waiting")
            deadline_s = time.monotonic() + timeout_s
            continue

        running = find_running(images)
        if running is None:
            raise RuntimeError("the unit reports no running image")
        cause = f"; reset cause of this boot: {format_reset_cause(status['reset_cause'])}" if status else ""

        if running.hash != new_hash:
            new = find_by_hash(images, new_hash)
            if new is None:
                raise RuntimeError(f"the unit runs version {running.version} and slot 1 no longer holds the new "
                                   f"image: MCUboot refused it (signature, or the key it trusts?){cause}")
            if new.pending:
                raise RuntimeError(f"the unit runs version {running.version} and the new image is still pending: "
                                   f"the unit did not restart into it{cause}")
            raise RuntimeError(f"the update was reverted: version {running.version} runs again and the new image "
                               f"({new.version}) sits unconfirmed in slot {new.slot}{cause}")

        if status is None:
            return running.confirmed
        if status.get("confirmed") and status.get("state") == DFU_CONFIRMED:
            return True
        state = status.get("state")
        note = format_dfu_state(state)
        if state == DFU_FAILED:
            note += ", the unit resets and MCUboot reverts the image"
        elif "confirm_in_ms" in status:
            note += f", confirms in {format_duration(status['confirm_in_ms'] / 1000)}"
        if note != last_state:
            print(f"the new image runs: {note}")
            last_state = note
        if state == DFU_FAILED and listens:
            # A reset follows: write nothing until the unit is up again. The markers have been forgotten at
            # the top of this pass, so a banner that came since counts.
            if client.link.wait_for_boot(timeout_s, what="the new image failed; listening for the unit's boot"):
                deadline_s = time.monotonic() + timeout_s
            continue
        time.sleep(IMAGE_VERIFY_INTERVAL_S)


def command_image_update(client: SmpClient, args: argparse.Namespace) -> None:
    """The client flow of the firmware update plan: check, upload, test, reset, wait, verify."""
    started_s = time.monotonic()
    image = args.file.read_bytes()
    info = parse_image(image)
    new_hash = info.hash
    if new_hash is None or new_hash != info.digest:
        raise ImageError("the image has no hash TLV, or its hash does not match its content; "
                         f"`image info {args.file}` shows more")

    # 1 and 2: the unit, and whether the file is for it.
    device = client.read(Group.DEVICE, 0)
    print(f"unit: {describe_unit(device)}")
    print(f"image: version {format_image_version(info.version)}, hash {new_hash.hex()}")
    check_image_fits_unit(info, device)
    images = read_images(client)
    for entry in images:
        print(entry.describe())
    running = find_running(images)
    if running is not None and running.hash == new_hash:
        print("the unit runs this image already; nothing to do")
        return
    check_slot_is_free(images)

    # 3 and 4: size the chunks and upload.
    upload_with_report(client, image, args.in_flight, args.upgrade)
    images = read_images(client)
    if find_by_hash(images, new_hash) is None:
        raise RuntimeError("the unit does not list an image with the hash of this file after the upload")

    # 5: mark it for a test boot.
    test_response = client.write(Group.IMG, IMG_STATE, {"hash": new_hash, "confirm": False})
    marked = find_by_hash(images_from(test_response), new_hash)
    if marked is None or not marked.pending:
        raise RuntimeError("the unit did not mark the image for test")
    if args.no_reset:
        print("marked for test; reset the unit to boot it (`reset`), or run `image erase` to drop it")
        return

    # 6 and 7: reset, and wait for the swap. The unit has the same UID afterwards, or it is another unit.
    uid = device.get("uid")
    print("resetting; the swap keeps the unit silent for up to ~2.5 min")
    reset_s = time.monotonic()
    what = "waiting for the swap (up to ~2.5 min on the gauge, ~1.5 min on rt_daq)"
    booted = restart_unit(client, args.timeout_s, what)
    now_s = time.monotonic()
    if booted is None:
        after = poll_device_info(client, deadline_s=reset_s + args.timeout_s, expect_restart=True, what=what)
    elif booted:
        after = poll_device_info(client, deadline_s=max(reset_s + args.timeout_s, now_s + IMAGE_POST_BOOT_POLL_S),
                                 expect_restart=False, what="the unit booted; asking it")
    else:
        print(f"warning: no boot banner within {format_duration(args.timeout_s)}; asking the unit anyway",
              file=sys.stderr)
        after = poll_device_info(client, deadline_s=now_s + IMAGE_POLL_FALLBACK_S, expect_restart=False,
                                 what="no boot banner; asking the unit")
    print(f"the unit answers again after {format_duration(time.monotonic() - reset_s)}: {describe_unit(after)}")
    if uid is not None and after.get("uid") != uid:
        raise RuntimeError(f"another unit answers now (uid {after.get('uid', 0):#010x} instead of {uid:#010x}); "
                           f"after a restart a unit may claim another CDMP ID")
    if after["product"] != device["product"] or after.get("board") != device.get("board"):
        raise RuntimeError("the unit answers with another product or board than before")

    # 8: the new image confirms itself, or the unit reverts.
    if not watch_confirmation(client, new_hash=new_hash, timeout_s=args.timeout_s):
        raise RuntimeError(f"the new image ({format_image_version(info.version)}) runs but is not confirmed, and the "
                           f"unit has no dfu group to confirm it: run `image confirm`, or a reset reverts it")
    print(f"updated to {format_image_version(info.version)}, confirmed; "
          f"{format_duration(time.monotonic() - started_s)} in all")


IMAGE_HANDLERS = {
    "info": command_image_info,
    "list": command_image_list,
    "upload": command_image_upload,
    "test": command_image_test,
    "confirm": command_image_confirm,
    "erase": command_image_erase,
    "update": command_image_update,
}


def command_image(client: SmpClient | None, args: argparse.Namespace) -> None:
    IMAGE_HANDLERS[args.image_command](client, args)


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

    ble = parser.add_argument_group("Bluetooth transport")
    ble.add_argument("--ble", metavar="NAME|ADDRESS",
                     help="reach the unit through the access point of the unit with this device name or address")
    ble.add_argument("--route", type=int, metavar="ID",
                     help="CDMP device ID of the unit to reach through the access point (default: 0, the access "
                          "point itself)")
    ble.add_argument("--ble-no-pair", action="store_true", help="do not pair (the host paired the unit already)")
    ble.add_argument("--scan-timeout", type=float, default=DEFAULT_BLE_SCAN_TIMEOUT_S, metavar="S",
                     help=f"seconds to look for the unit (default: {DEFAULT_BLE_SCAN_TIMEOUT_S:g})")

    parser.add_argument("--timeout", type=float, default=2.0, help="seconds to wait for a response")
    parser.add_argument("--retries", type=int, help=f"resends after a timeout (default: {DEFAULT_CAN_RETRIES} on CAN, "
                                                   f"{DEFAULT_SERIAL_RETRIES} on serial, {DEFAULT_BLE_RETRIES} on "
                                                   f"Bluetooth)")

    commands = parser.add_subparsers(dest="command", required=True)
    commands.add_parser("echo", help="os echo").add_argument("text")
    commands.add_parser("params", help="os mcumgr params: buffer size and count")
    commands.add_parser("info", help="device info")
    commands.add_parser("devices", help="network devices, as the unit sees them")
    reset = commands.add_parser("reset", help="os reset: restart the unit")
    reset.add_argument("--timeout-s", type=float, default=DEFAULT_UPDATE_TIMEOUT_S, metavar="N",
                       help=f"serial: seconds to listen for the boot banner before the command returns "
                            f"(default: {DEFAULT_UPDATE_TIMEOUT_S:g}; a swap takes up to ~2.5 min)")
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
    assets = commands.add_parser("assets", help="asset files: images, animations, fonts, scripts, logs")
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
    asset_stat = assets_commands.add_parser("stat", help="a file's size and CRC32, without downloading it")
    asset_stat.add_argument("kind", type=parse_asset_kind, help="asset kind, a name or a number")
    asset_stat.add_argument("name")
    asset_rename = assets_commands.add_parser("rename", help="rename a file, replacing one of the new name")
    asset_rename.add_argument("kind", type=parse_asset_kind, help="asset kind, a name or a number")
    asset_rename.add_argument("name")
    asset_rename.add_argument("to", help="the new name")
    logging = commands.add_parser("logging", help="logging status, and start or stop logging on the units that log")
    logging_commands = logging.add_subparsers(dest="logging_command", required=True)
    logging_commands.add_parser("status", help="whether logging is active, why not, and the session")
    for name in ("start", "stop"):
        sub = logging_commands.add_parser(name, help=f"{name} logging on every unit that can log, or on one")
        sub.add_argument("--uid", type=lambda text: int(text, 0), metavar="N",
                         help="the unit's UID (decimal or 0x hex); default: every unit")
    image = commands.add_parser("image", help="firmware images: info, list, upload, test, confirm, erase, update")
    image_commands = image.add_subparsers(dest="image_command", required=True)
    image_info = image_commands.add_parser("info", help="offline: the header, TLVs, identity and hash of an image file")
    image_info.add_argument("file", type=Path)
    image_commands.add_parser("list", help="the image slots: versions, hashes and flags (img_mgmt state)")
    for name, text in (("upload", "upload an image into slot 1, resuming a transfer the unit holds"),
                       ("update", "check, upload, test, reset, wait for the swap and for the confirmation")):
        sub = image_commands.add_parser(name, help=text)
        sub.add_argument("file", type=Path)
        sub.add_argument("--in-flight", type=int, metavar="N",
                         help="chunks outstanding at once (default: 1 on CAN, on serial and for a BLE route to a "
                              "unit behind CAN; else the access point's window; always below the unit's packet count)")
        sub.add_argument("--upgrade", action="store_true", help="have the unit refuse an image that is not newer")
        if name == "update":
            sub.add_argument("--no-reset", action="store_true", help="stop after marking the image for test")
            sub.add_argument("--timeout-s", type=float, default=DEFAULT_UPDATE_TIMEOUT_S, metavar="N",
                             help=f"seconds to wait for the unit after the reset, and for the confirmation "
                                  f"(default: {DEFAULT_UPDATE_TIMEOUT_S:g})")
    for name, text in (("test", "mark an image for a test boot at the next reset (default: the image in slot 1)"),
                       ("confirm", "make an image permanent (default: the running image)")):
        sub = image_commands.add_parser(name, help=text)
        sub.add_argument("hash", nargs="?", type=parse_image_hash,
                         help="image hash, 64 hex digits as `image list` shows")
    image_commands.add_parser("erase", help="erase slot 1, the slot that does not run")
    dfu = commands.add_parser("dfu", help="firmware update confirmation state of the unit")
    dfu_commands = dfu.add_subparsers(dest="dfu_command", required=True)
    dfu_commands.add_parser("status", help="confirmed or not, the confirmation state and the last reset's cause")
    ble_scan = commands.add_parser("ble-scan", help="list the access points in range (needs bleak)")
    ble_scan.add_argument("--time", type=float, default=DEFAULT_BLE_SCAN_TIMEOUT_S, help="scan seconds")
    ble_scan.add_argument("--all", action="store_true", help="list every device, not only access points")
    shell = commands.add_parser("shell", help="serial only: type a shell command and print the console")
    shell.add_argument("text", help="the command line; empty to only listen")
    shell.add_argument("--wait", type=float, default=1.0, help="seconds to print the console for (default: 1)")
    bench = commands.add_parser("bench", help="echo round trips, for throughput")
    bench.add_argument("--size", type=int, default=256, help="echo payload in bytes")
    bench.add_argument("--count", type=int, default=50)

    args = parser.parse_args(argv)
    if is_offline(args):
        return args

    if args.command == "assets" and args.assets_command == "get" and not 1 <= args.in_flight <= MAX_ASSET_IN_FLIGHT:
        parser.error(f"--in-flight must be 1-{MAX_ASSET_IN_FLIGHT}")
    if (args.command == "image" and args.image_command in ("upload", "update") and args.in_flight is not None
            and not 1 <= args.in_flight <= MAX_ASSET_IN_FLIGHT):
        parser.error(f"--in-flight must be 1-{MAX_ASSET_IN_FLIGHT}")
    if args.command == "image" and args.image_command == "update" and args.timeout_s <= 0:
        parser.error("--timeout-s must be positive")

    if args.ble is not None:
        if args.serial:
            parser.error("--ble and --serial exclude each other; pick one transport")
        if args.target is not None:
            parser.error("--target is for the CAN transport; over Bluetooth, --route names the unit behind the "
                         "access point")
        if args.command == "shell":
            parser.error("shell needs --serial")
        args.route = 0 if args.route is None else args.route
        if not 0 <= args.route <= 254:
            parser.error("--route must be 0 (the access point) or a CDMP device ID, 1-254")
    elif args.route is not None or args.ble_no_pair:
        parser.error("--route and --ble-no-pair need --ble")
    elif args.serial:
        if args.target is not None:
            parser.error("--target is for the CAN transport; the serial transport reaches the unit on the port")
    else:
        if args.command == "shell":
            parser.error("shell needs --serial")
        if args.target is None:
            parser.error("an SMP command needs --target (CAN), --serial or --ble")
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
    "logging": command_logging,
    "image": command_image,
    "dfu": command_dfu,
    "ble-scan": command_ble_scan,
    "shell": command_shell,
    "bench": command_bench,
}
OFFLINE_COMMANDS = ("config-encode", "config-decode", "ble-scan")


def is_offline(args: argparse.Namespace) -> bool:
    """Whether the command talks to no unit: it then needs no transport."""
    return args.command in OFFLINE_COMMANDS or (args.command == "image" and args.image_command == "info")


@contextlib.contextmanager
def open_link(args: argparse.Namespace) -> Iterator[SmpCanLink | SmpSerialLink | SmpBleLink]:
    if args.serial:
        with SmpSerialLink(args.serial, args.baud, args.line_delay, args.show_console) as link:
            yield link
        return
    if args.ble:
        with SmpBleLink(args.ble, route=args.route, pair=not args.ble_no_pair,
                        scan_timeout_s=args.scan_timeout) as link:
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
        if is_offline(args):
            HANDLERS[args.command](None, args)
            return 0

        retries = args.retries
        if retries is None:
            retries = (DEFAULT_SERIAL_RETRIES if args.serial else DEFAULT_BLE_RETRIES if args.ble
                       else DEFAULT_CAN_RETRIES)
        with open_link(args) as link:
            HANDLERS[args.command](SmpClient(link, args.timeout, retries), args)
    except TimeoutError as error:
        hint = ""
        if args.serial:
            hint = f"; is a bench build (app/bench.conf) running on {args.serial}?"
        elif args.ble:
            hint = f"; can the access point reach route {args.route}?"
        print(f"error: {error}{hint}", file=sys.stderr)
        return 1
    except (SmpError, RuntimeError, ConfigError, OSError) as error:
        print(f"error: {error}", file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
