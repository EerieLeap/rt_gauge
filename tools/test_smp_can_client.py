#!/usr/bin/env python3
"""Self-test of the image, dfu, log following, clock and Bluetooth parts of smp_can_client.py, against simulated units.

`FakeUnit` answers the SMP requests of the firmware update plan the way Zephyr's img_mgmt and rt_core's dfu group
do (offsets, resumption by sha, the identity check, test/confirm flags, a swap on reset, self-confirmation or a
revert), and `FakeLink` lets the client talk to it without a bus. A fake `bleak` module stands in for the
Bluetooth stack, with an access point that reassembles the writes and notifies the answers in fragments.

Needs cbor2 only (imgtool is used when it is installed). Run: python3 tools/test_smp_can_client.py
"""

from __future__ import annotations

import argparse
import collections
import contextlib
import hashlib
import io
import os
import pty
import queue
import random
import select
import shutil
import struct
import subprocess
import sys
import tempfile
import threading
import time
import types
import unittest
import zlib
from pathlib import Path
from unittest import mock

import cbor2

sys.path.insert(0, str(Path(__file__).resolve().parent))
import smp_can_client as client  # noqa: E402

BOARD = "esp32s3_touch_amoled_1_75/esp32s3/procpu"
PRODUCT = 0xA401
KEY = Path(__file__).resolve().parent.parent / "modules/eerie_leap_rt_core/keys/dev-ec-p256.pem"

# Fast clocks for the waits; the client's real values are minutes.
FAST = dict(IMAGE_FIRST_CHUNK_TIMEOUT_S=0.1, IMAGE_LAST_CHUNK_TIMEOUT_S=0.1, IMAGE_RESUME_PAUSE_S=0.0,
            IMAGE_POLL_INTERVAL_S=0.001, IMAGE_POLL_MAX_INTERVAL_S=0.004, IMAGE_VERIFY_INTERVAL_S=0.001,
            IMAGE_RESTART_GRACE_S=0.5, SERIAL_BOOT_SETTLE_S=0.5, IMAGE_POLL_FALLBACK_S=2.0, IMAGE_POST_BOOT_POLL_S=3.0)


def build_image(size: int = 3000, *, version=(0, 0, 2, 0), product: int = PRODUCT, board: str = BOARD,
                identity: bool = True, seed: int = 1) -> bytes:
    """An MCUboot image laid out as imgtool lays it out, with a made-up signature."""
    rng = random.Random(seed)
    payload = bytes(rng.getrandbits(8) for _ in range(size))
    protected = b""
    if identity:
        value = bytes([1]) + struct.pack("<H", product) + bytes([len(board)]) + board.encode()
        entries = struct.pack("<HH", 0x00A0, len(value)) + value
        protected = struct.pack("<HH", 0x6908, 4 + len(entries)) + entries
    header = struct.pack("<IIHHIIBBHII", 0x96F3B83D, 0, 32, len(protected), len(payload), 0, *version, 0)
    signed = header + payload + protected
    entries = b"".join(struct.pack("<HH", tag, len(value)) + value for tag, value in (
        (0x10, hashlib.sha256(signed).digest()), (0x01, bytes(range(32))),
        (0x22, bytes(rng.getrandbits(8) for _ in range(71)))))
    return signed + struct.pack("<HH", 0x6907, 4 + len(entries)) + entries


class FakeLog:
    """A log file the unit writes: it grows, then the writer closes it and rewrites the first bytes."""

    CHUNK = 100

    def __init__(self, name: str = "log_0001.mf4", size: int = 250) -> None:
        self.name = name
        self.content = bytearray(b"UNFINALIZED" + bytes(range(256)) * 4)[:size]
        self.closed = False
        self.followed = False       # A request with growing:true was served while the file was open.
        self.changed_mode = "error" # "error": CHANGED at off > 0 after the close; "silent": the file is just served.
        self.requests: list[dict] = []
        self.changed_sent = 0
        self.before_request: dict[int, object] = {}     # Request number (1-based) -> a call made before it is served.

    def append(self, count: int) -> None:
        self.content += bytes((len(self.content) + index) % 251 for index in range(count))

    def close(self) -> None:
        self.content[:11] = b"FINALIZED!!"
        self.closed = True

    def read(self, body: dict) -> dict:
        """The answer to an assets READ, or {"err": rc}."""
        self.requests.append(body)
        hook = self.before_request.get(len(self.requests))
        if hook is not None:
            hook()
        growing = bool(body.get("growing"))
        offset = body["off"]
        if not self.closed and not growing:
            return {"err": 7}
        if self.closed and growing and offset > 0 and self.followed and self.changed_mode == "error":
            self.changed_sent += 1
            self.followed = False
            return {"err": 17}
        if offset > len(self.content):
            return {"err": 10, "len": len(self.content)}
        data = bytes(self.content[offset:offset + self.CHUNK])
        reply: dict = {"off": offset, "data": data}
        if offset == 0:
            reply["len"] = len(self.content)
        if offset + len(data) == len(self.content):
            reply["crc"] = zlib.crc32(self.content)
        if not self.closed:
            reply["growing"] = True
            self.followed = self.followed or growing
        return reply


class FakeUnit:
    """The unit side of the update: os, device, img_mgmt (group 1) and dfu (group 72)."""

    def __init__(self, *, buf_size: int = 512, buf_count: int = 4, product: int = PRODUCT, board: str = BOARD) -> None:
        self.product, self.board = product, board
        self.sw_version, self.build = (0, 0, 1), 0
        self.uid = 0x1234ABCD
        self.buf_size, self.buf_count = buf_size, buf_count
        self.slots: list[dict | None] = [
            self._slot((0, 0, 1, 0), b"old image", active=True, confirmed=True), None]
        self.upload: dict | None = None
        self.upload_requests = 0
        self.chunk_offsets: list[int] = []
        self.max_request = 0
        self.drop_requests: set[int] = set()        # Upload requests (1-based) the unit never sees.
        self.drop_responses: set[int] = set()       # Upload requests it handles and does not answer.
        self.forget_after: int | None = None        # The unit loses its upload after this many requests.
        self.silent_requests = 0                    # Requests it swallows (it is restarting).
        self.reset_cause = 1 << 3                   # POR
        self.resets = 0
        self.fails_new_image = False
        self.has_dfu = True
        self.frugal = False
        self.dfu_polls = 0
        self.revert_next = False
        self.auto_revert = True                     # False: a ConsoleServer reverts it, as a watchdog would.
        self.reboots = 0
        self.silent_after_reset = 4
        self.log: FakeLog | None = None
        self.has_time = True
        self.clock_ms = 1761106217000               # The unit's clock, fixed (it never steps).
        self.wall_offset_ms: int | None = None
        self.drop_set_time = 0                      # SET_TIME requests it never answers.
        self.set_times: list[int] = []
        self.can: FakeCanMonitor | None = None      # The canmon group (74), when the unit has one.

    @staticmethod
    def _slot(version, content: bytes, **flags) -> dict:
        slot = dict(version=version, hash=hashlib.sha256(content).digest(), active=False, confirmed=False,
                    pending=False, permanent=False, filesha=b"")
        slot.update(flags)
        return slot

    def handle(self, packet: bytes) -> list[bytes]:
        if self.revert_next and self.auto_revert:
            self.revert_next = False
            self._revert()
        if self.silent_requests > 0:
            self.silent_requests -= 1
            return []

        length, group, sequence, command = client.unpack_header(packet)
        op = packet[0] & 7
        body = cbor2.loads(packet[8:8 + length]) if length else {}
        self.max_request = max(self.max_request, len(packet))
        if (group, command) == (65, 2) and self.drop_set_time > 0:
            self.drop_set_time -= 1
            return []

        if group == 1 and command == 1:
            self.upload_requests += 1
            if self.upload_requests in self.drop_requests:
                return []
            reply = self._upload(body)
            if self.forget_after is not None and self.upload_requests >= self.forget_after:
                self.forget_after, self.upload = None, None
            if self.upload_requests in self.drop_responses:
                return []
        else:
            reply = self._dispatch(op, group, command, body)
        return [self._pack(op, group, command, sequence, reply)]

    @staticmethod
    def _pack(op: int, group: int, command: int, sequence: int, body: dict) -> bytes:
        payload = cbor2.dumps(body)
        return struct.pack(">BBHHBB", (op + 1) | 1 << 3, 0, len(payload), group, sequence, command) + payload

    @staticmethod
    def _error(group: int, rc: int) -> dict:
        return {"err": {"group": group, "rc": rc}}

    def _dispatch(self, op: int, group: int, command: int, body: dict) -> dict:
        if (group, command) == (0, 6):
            return {"buf_size": self.buf_size, "buf_count": self.buf_count}
        if (group, command) == (0, 5):
            self._reset()
            return {}
        if (group, command) == (65, 0):
            major, minor, patch = self.sw_version
            return {"product": self.product, "revision": 1, "features": 0, "hw": 1,
                    "sw": major << 24 | minor << 16 | patch, "build": self.build, "board": self.board,
                    "cdmp_id": 5, "uid": self.uid, "status": 1}
        if group == 65 and command in (1, 2) and self.has_time:
            return self._time(command, body)
        if group == 70 and command == 2 and self.log is not None and body["name"] == self.log.name:
            reply = self.log.read(body)
            return self._error(70, reply["err"]) if "err" in reply else reply
        if (group, command) == (71, 0):
            return {"active": 1, "since_ms": self.clock_ms - 125000, "file": "log_0001.mf4", "bytes": 2048}
        if group == 1 and command == 0:
            return self._state(body) if op == 2 else self._images()
        if group == 1 and command == 5:
            if self.slots[1] is not None and self.slots[1]["pending"]:
                return self._error(1, 9)
            self.slots[1], self.upload = None, None
            return {}
        if group == 72 and command == 0 and self.has_dfu:
            return self._dfu_status()
        if group == 74 and self.can is not None:
            return self.can.handle(command, body)
        return {"rc": client.MGMT_ERR_ENOTSUP}

    def _time(self, command: int, body: dict) -> dict:
        if command == 2:
            if not 1577836800000 <= body["wall_ms"] < 4102444800000:     # 2020-01-01 to 2100-01-01
                return {"rc": 3}
            self.set_times.append(body["wall_ms"])
            self.wall_offset_ms = body["wall_ms"] - self.clock_ms
        reply = {"now_ms": self.clock_ms, "uptime_ms": 4321, "boot_id": 7}
        if self.wall_offset_ms is not None:
            reply["wall_ms"] = self.clock_ms + self.wall_offset_ms
        return reply

    def _images(self) -> dict:
        images = []
        for index, slot in enumerate(self.slots):
            if slot is None:
                continue
            entry = {"slot": index, "version": ".".join(str(n) for n in slot["version"][:3])
                     + (f".{slot['version'][3]}" if slot["version"][3] else ""), "hash": slot["hash"]}
            for flag in ("bootable", "pending", "confirmed", "active", "permanent"):
                value = True if flag == "bootable" else slot[flag]
                if value or not self.frugal:
                    entry[flag] = value
            images.append(entry)
        return {"images": images}

    def _state(self, body: dict) -> dict:
        image_hash, confirm = body.get("hash", b""), body.get("confirm", False)
        if not image_hash:
            if not confirm:
                return self._error(1, 24)
            self.slots[0]["confirmed"] = True
            return self._images()
        slot = next((s for s in self.slots if s is not None and s["hash"] == image_hash), None)
        if slot is None:
            return self._error(1, 8)
        if slot["active"]:
            slot["confirmed"] = slot["confirmed"] or confirm
        else:
            slot["pending"], slot["permanent"] = True, confirm
        return self._images()

    def _upload(self, body: dict) -> dict:
        offset, data = body["off"], body.get("data", b"")
        if offset == 0:
            if data[:4] != struct.pack("<I", client.IMAGE_MAGIC):
                return self._error(1, 23)
            if self.upload is not None and self.upload["sha"] == body["sha"]:
                return {"off": self.upload["off"]}
            slot = self.slots[1]
            if slot is not None and slot["filesha"] == body.get("sha") and not slot["pending"]:
                return {"off": body["len"], "match": True}
            test_image_runs = self.slots[0]["active"] and not self.slots[0]["confirmed"]
            if self.slots[1] is not None and (self.slots[1]["pending"] or test_image_runs):
                return self._error(1, 9)
            self.upload = dict(sha=body["sha"], size=body["len"], off=0, data=bytearray())
        elif self.upload is None:
            return {"off": 0}
        if offset != self.upload["off"]:
            return {"off": self.upload["off"]}

        self.chunk_offsets.append(offset)
        self.upload["data"] += data
        self.upload["off"] += len(data)
        if self.upload["off"] < self.upload["size"]:
            return {"off": self.upload["off"]}

        # The last chunk: the identity check, then the slot check.
        image = bytes(self.upload["data"])
        info = client.parse_image(image)
        identity = info.identity
        rc = None
        if identity is None:
            rc = 2
        elif identity.product_id != self.product:
            rc = 3
        elif identity.board != self.board:
            rc = 4
        if rc is not None:
            self.upload = None
            return self._error(72, rc)
        match = hashlib.sha256(image).digest() == self.upload["sha"]
        self.slots[1] = self._slot(info.version, image, filesha=self.upload["sha"])
        self.slots[1]["hash"] = info.hash
        self.upload = None
        return {"off": len(image), "match": match}

    def _reset(self) -> None:
        self.resets += 1
        self.reboots += 1
        self.reset_cause = 1 << 1
        self.silent_requests = self.silent_after_reset
        self.dfu_polls = 0
        new = self.slots[1]
        if new is not None and new["pending"]:
            old = self.slots[0]
            old.update(active=False, pending=False)
            new.update(active=True, pending=False, confirmed=new["permanent"])
            self.slots = [new, old]
            if new["permanent"]:
                old["confirmed"] = False

    def _revert(self) -> None:
        self.reboots += 1
        new, old = self.slots
        new.update(active=False, confirmed=False)
        old.update(active=True, confirmed=True)
        self.slots = [old, new]
        self.reset_cause = 1 << 4
        self.silent_requests = self.silent_after_reset

    def _dfu_status(self) -> dict:
        running = self.slots[0]
        if running["confirmed"]:
            return {"confirmed": True, "state": 0, "reset_cause": self.reset_cause}
        self.dfu_polls += 1
        if self.fails_new_image and self.dfu_polls >= 3:
            self.revert_next = True
            return {"confirmed": False, "state": 3, "reset_cause": self.reset_cause}
        if not self.fails_new_image and self.dfu_polls >= 5:
            running["confirmed"] = True
            return {"confirmed": True, "state": 0, "reset_cause": self.reset_cause}
        if not self.fails_new_image and self.dfu_polls >= 3:
            return {"confirmed": False, "state": 2, "confirm_in_ms": 1000 * (5 - self.dfu_polls),
                    "reset_cause": self.reset_cause}
        return {"confirmed": False, "state": 1, "reset_cause": self.reset_cause}


CAN_LENGTHS = (0, 1, 2, 3, 4, 5, 6, 7, 8, 12, 16, 20, 24, 32, 48, 64)


def leb(value: int) -> bytes:
    """An unsigned LEB128, written here apart from the client's encoder."""
    out = bytearray()
    while True:
        byte, value = value & 0x7F, value >> 7
        out.append(byte | (0x80 if value else 0))
        if not value:
            return bytes(out)


def frame_record(dt: int, can_id: int, data: bytes = b"", *, ide: bool = False, fdf: bool = False, brs: bool = False,
                 rtr: bool = False, esi: bool = False, tx: bool = False, failed: bool = False,
                 dlc: int | None = None) -> bytes:
    """A kind 0 record as rt_core's EncodeFrame() writes it."""
    dlc = CAN_LENGTHS.index(len(data)) if dlc is None else dlc
    flags = (1 if rtr else 0) | (2 if esi else 0) | (4 if tx else 0) | (8 if failed else 0)
    head = dlc | (0x10 if ide else 0) | (0x20 if fdf else 0) | (0x40 if brs else 0)
    out = bytes([head | 0x80, flags]) if flags else bytes([head])
    return out + leb(dt) + can_id.to_bytes(4 if ide else 2, "little") + (b"" if rtr else data)


def changed_record(dt: int, can_id: int, data: bytes, previous: bytes, *, ide: bool = False) -> bytes:
    """A kind 1 record: a mask byte for each 8 payload bytes, then the bytes that differ from @p previous."""
    masks, changed = bytearray((len(data) + 7) // 8), bytearray()
    for index, (now, before) in enumerate(zip(data, previous)):
        if now != before:
            masks[index // 8] |= 1 << index % 8
            changed.append(now)
    head = CAN_LENGTHS.index(len(data)) | (0x10 if ide else 0) | (0x20 if len(data) > 8 else 0) | 0x80
    return bytes([head, 0x10]) + leb(dt) + can_id.to_bytes(4 if ide else 2, "little") + bytes(masks) + bytes(changed)


def other_record(kind: int, body: bytes) -> bytes:
    return bytes([0x80, kind << 4]) + leb(0) + body


def stats_record(values: list[int]) -> bytes:
    return other_record(3, b"".join(leb(value) for value in values))


def notification(records: list[bytes], *, seq: int = 0, base_us: int = 1_000_000, port: int = 0) -> bytes:
    return struct.pack("<BBHQB", 1, port, seq, base_us, len(records)) + b"".join(records)


class FakeCanMonitor:
    """The canmon group of an access point, as rt_core serves it, with one session. Each STATS lets out the next of
    `batches` as a notification; SEND echoes its frames as the unit's own; the replay buffer plays 200 B a STATS."""

    TOKEN = 0x1234ABCD

    def __init__(self, *, tx: bool = True, synthetic: bool = True, capacity: int = 1024) -> None:
        self.ports = [{"ch": 0, "types": [1, 2], "configured": True, "state": 0, "bitrate": 500000,
                       "data_bitrate": 0, "session": False}]
        self.tx, self.synthetic = tx, synthetic
        self.listening = False
        self.session: dict | None = None
        self.requests: list[tuple[int, dict]] = []
        self.outbox: list[bytes] = []
        self.batches: list[list[bytes]] = []
        self.seq = 0
        self.port = 0
        self.capacity = self.room = capacity
        self.replay_seq = 0
        self.no_room_once = False
        self.refuse_open: int | None = None

    @staticmethod
    def error(rc: int) -> dict:
        return {"err": {"group": 74, "rc": rc}}

    def emit(self, records: list[bytes]) -> None:
        self.outbox.append(notification(records, seq=self.seq, port=self.port))
        self.seq = (self.seq + 1) & 0xFFFF

    def handle(self, command: int, body: dict) -> dict:
        self.requests.append((command, body))
        if command == 0:
            return {"version": 1, "ports": self.ports, "filters_max": 16, "ring_bytes": 262144,
                    "notify_max": 495 if self.listening else 0, "tx": self.tx, "synthetic": self.synthetic,
                    "delta": False}
        if command == 1:
            if self.refuse_open is not None:
                return self.error(self.refuse_open)
            if not self.listening:
                return self.error(8)
            synthetic = "synthetic" in body
            self.session, self.port = {"armed": False, "replay": False}, 255 if synthetic else body["ch"]
            return {"tok": self.TOKEN, "ch": self.port, "mode": 0 if synthetic else body["mode"],
                    "type": 0 if synthetic else 1, "bitrate": 0 if synthetic else 500000, "data_bitrate": 0,
                    "lease_ms": 5000}
        if self.session is None or body.get("tok") != self.TOKEN:
            return self.error(2)
        if command == 4:
            if self.batches:
                self.emit(self.batches.pop(0))
            if self.session["replay"]:
                self.room = min(self.capacity, self.room + 200)
            return {"state": 0, "tec": 0, "rec": 0, "rx": 7, "tx": 0, "busy_us": 900, "errors": [0] * 5,
                    "lost": [0] * 4, "filtered": 0}
        if command == 2:
            self.emit([stats_record([0] * 13), other_record(5, bytes([1]))])
            self.session = None
            return {}
        if not self.tx:
            return self.error(15)
        if command == 5:
            self.session["armed"] = body["on"]
            return {"armed": body["on"], "share_pct": 50, "reserved": [[0x700, 0x763, False]]}
        if not self.session["armed"]:
            return self.error(10)
        if command == 6:
            self.emit([frame_record(0, can_id, data, ide=bool(flags & 1), tx=True)
                       for can_id, flags, data in body["frames"]])
            return {"sent": len(body["frames"]), "refused": 0}
        if command == 7:
            return {}
        return self._replay(body)

    def _replay(self, body: dict) -> dict:
        if body["op"] == 0:
            self.session["replay"], self.room, self.replay_seq = True, self.capacity, 0
        elif body["op"] == 1:
            if self.no_room_once:
                self.no_room_once = False
                return self.error(14)
            if body["seq"] != self.replay_seq:
                return self.error(16)
            if len(body["data"]) > self.room:
                return self.error(14)
            self.replay_seq += 1
            self.room -= len(body["data"])
        elif body["op"] == 3:
            self.emit([other_record(6, bytes([4]) + leb(self.replay_seq))])
        else:
            self.session["replay"], self.room = False, self.capacity
        return {"room": self.room}


class FakeLink:
    """A link straight to a FakeUnit."""

    max_packet_size = None

    def __init__(self, unit: FakeUnit, window: int = 1) -> None:
        self.unit = unit
        self.image_window = window
        self.inbox: collections.deque[bytes] = collections.deque()
        self.sends = 0
        self.fail_sends: set[int] = set()
        self.reconnects = 0

    def send(self, packet: bytes) -> None:
        self.sends += 1
        if self.sends in self.fail_sends:
            raise ConnectionError("simulated link failure")
        self.inbox.extend(self.unit.handle(packet))

    def receive(self, timeout_s: float) -> bytes | None:
        if self.inbox:
            return self.inbox.popleft()
        time.sleep(min(timeout_s, 0.001))
        return None

    def reconnect(self) -> None:
        self.reconnects += 1


def run(handler, smp: client.SmpClient, **options):
    """Runs a command handler and returns what it printed to stdout and stderr."""
    out, err = io.StringIO(), io.StringIO()
    defaults = dict(in_flight=None, upgrade=False, no_reset=False, timeout_s=5.0, hash=None)
    with contextlib.redirect_stdout(out), contextlib.redirect_stderr(err):
        handler(smp, argparse.Namespace(**(defaults | options)))
    return out.getvalue(), err.getvalue()


class ClientCase(unittest.TestCase):
    def setUp(self) -> None:
        patcher = mock.patch.multiple(client, **FAST)
        patcher.start()
        self.addCleanup(patcher.stop)
        tmp = tempfile.TemporaryDirectory()
        self.addCleanup(tmp.cleanup)
        self.tmp = Path(tmp.name)
        self.unit = FakeUnit()
        self.link = FakeLink(self.unit)
        self.smp = client.SmpClient(self.link, 0.03, 1)

    def image_file(self, **options) -> Path:
        path = self.tmp / "image.bin"
        path.write_bytes(build_image(**options))
        return path


class ImageInfoTest(ClientCase):
    def test_reads_header_tlvs_identity_and_hash(self) -> None:
        data = build_image(version=(1, 2, 3, 4))
        info = client.parse_image(data)
        self.assertEqual(info.version, (1, 2, 3, 4))
        self.assertEqual(info.identity, client.ImageIdentity(PRODUCT, BOARD))
        self.assertEqual(info.hash, info.digest)
        self.assertEqual(info.signature_tag, 0x22)
        self.assertEqual(info.protected_size, 4 + 4 + 4 + len(BOARD))
        self.assertEqual(info.trailing, 0)

    def test_info_command_prints_and_checks_the_digest(self) -> None:
        path = self.image_file()
        out, _ = run(client.command_image_info, None, file=path)
        self.assertIn(f"identity: product 0xa401, board {BOARD}", out)
        self.assertIn("matches the hash", out)

        damaged = bytearray(path.read_bytes())
        damaged[100] ^= 0xFF
        path.write_bytes(damaged)
        with self.assertRaises(client.ImageError):
            run(client.command_image_info, None, file=path)

    def test_structural_errors(self) -> None:
        data = build_image()
        for broken, message in ((data[:20], "shorter"), (b"\0\0\0\0" + data[4:], "not an MCUboot image"),
                                (data[:3100], "TLV area")):
            with self.assertRaisesRegex(client.ImageError, message):
                client.parse_image(broken)

    def test_identity_errors_and_unsigned_location(self) -> None:
        self.assertIsNone(client.parse_image(build_image(identity=False)).identity)
        with self.assertRaisesRegex(client.ImageError, "format 2"):
            client.parse_identity(bytes([2, 1, 0xA4, 0]))
        with self.assertRaisesRegex(client.ImageError, "board name"):
            client.parse_identity(bytes([1, 1, 0xA4, 5]) + b"abc")

    @unittest.skipUnless(shutil.which("imgtool") and KEY.is_file(), "imgtool or the development key is missing")
    def test_matches_imgtool(self) -> None:
        payload = self.tmp / "payload.bin"
        payload.write_bytes(bytes(random.Random(2).getrandbits(8) for _ in range(5000)))
        board = BOARD.encode()
        identity = bytes([1]) + struct.pack("<H", PRODUCT) + bytes([len(board)]) + board
        signed = self.tmp / "signed.bin"
        subprocess.run(["imgtool", "sign", "--header-size", "0x20", "--pad-header", "--align", "4", "--slot-size",
                        "0x400000", "--version", "0.0.7+3", "--custom-tlv", "0xa0", "0x" + identity.hex(),
                        "--key", str(KEY), str(payload), str(signed)], check=True, capture_output=True)
        dump = subprocess.run(["imgtool", "dumpinfo", str(signed)], check=True, capture_output=True, text=True).stdout

        info = client.parse_image(signed.read_bytes())
        self.assertEqual(info.version, (0, 0, 7, 3))
        self.assertEqual(info.identity, client.ImageIdentity(PRODUCT, BOARD))
        self.assertEqual(info.hash, info.digest)
        # imgtool prints the hash as "0x.. 0x.." bytes after "type: SHA256".
        shown = dump.split("type: SHA256")[1].split("data:")[1].split("----")[0].replace("0x", "").split()
        self.assertEqual(info.hash.hex(), "".join(shown))


class UploadTest(ClientCase):
    def upload(self, image: bytes, window: int = 1, *, in_flight=None, link_window=None) -> int:
        if link_window is not None:
            self.link.image_window = link_window
        sha = hashlib.sha256(image).digest()
        plan = client.plan_upload(self.smp, in_flight if in_flight is not None else window, False)
        return client.upload_image(self.smp, image, sha, window=plan.window, first_size=plan.first_size,
                                   chunk_size=plan.chunk_size, upgrade=False, progress=client.Progress("t"))

    def assert_stored(self, image: bytes) -> None:
        slot = self.unit.slots[1]
        self.assertIsNotNone(slot)
        self.assertEqual(slot["filesha"], hashlib.sha256(image).digest())

    def test_chunk_sizes_fit_the_unit_buffer(self) -> None:
        image = build_image(5000)
        self.upload(image)
        self.assert_stored(image)
        self.assertLessEqual(self.unit.max_request, self.unit.buf_size)
        first, rest = client.image_chunk_sizes(512, is_serial=False, max_packet=None, upgrade=False)
        self.assertEqual(rest, 512 - 64)
        self.assertLess(first, rest)
        self.assertLess(client.image_chunk_sizes(512, is_serial=False, max_packet=None, upgrade=True)[0], first)
        self.assertEqual(self.unit.chunk_offsets[:3], [0, first, first + rest])
        # Over the console the frame's length and CRC count against the buffer; a BLE packet limit does too.
        self.assertEqual(client.image_chunk_sizes(512, is_serial=True, max_packet=None, upgrade=False)[1], 512 - 4 - 64)
        self.assertEqual(client.image_chunk_sizes(512, is_serial=False, max_packet=300, upgrade=False)[1], 300 - 64)

    def test_window_of_three_sends_in_order(self) -> None:
        image = build_image(6000)
        self.upload(image, 3)
        self.assert_stored(image)
        self.assertEqual(self.unit.chunk_offsets, sorted(self.unit.chunk_offsets))

    def test_window_stays_below_the_unit_packet_count(self) -> None:
        self.unit.buf_count = 2
        with contextlib.redirect_stderr(io.StringIO()) as err:
            plan = client.plan_upload(self.smp, 4, False)
        self.assertEqual(plan.window, 1)
        self.assertIn("1 in flight instead of 4", err.getvalue())

    def test_window_defaults_to_the_links(self) -> None:
        self.link.image_window = 2
        self.assertEqual(client.plan_upload(self.smp, None, False).window, 2)
        self.link.image_window = 1
        self.assertEqual(client.plan_upload(self.smp, None, False).window, 1)

    def test_lost_response_is_asked_for_again(self) -> None:
        image = build_image(4000)
        self.unit.drop_responses = {3}
        self.upload(image)
        self.assert_stored(image)

    def test_lost_request_in_a_window_rewinds(self) -> None:
        image = build_image(6000)
        self.unit.drop_requests = {4}
        self.upload(image, 3)
        self.assert_stored(image)
        # The three chunks of the window (the lost one, and the two behind it that the unit answers with the
        # offset it expects) go out twice; nothing new is sent while the window drains.
        first, rest = client.image_chunk_sizes(512, is_serial=False, max_packet=None, upgrade=False)
        chunks = 1 + -(-(len(image) - first) // rest)
        self.assertEqual(self.unit.upload_requests, chunks + 3)

    def test_unit_that_forgets_the_upload_starts_over(self) -> None:
        image = build_image(4000)
        self.unit.forget_after = 4
        self.upload(image)
        self.assert_stored(image)
        self.assertEqual(self.unit.chunk_offsets.count(0), 2)

    def test_a_finished_transfer_is_not_restarted_by_the_answers_that_trail_it(self) -> None:
        # Window 2, the answer to the second-to-last chunk lost: the last chunk completes the image and the unit
        # forgets the transfer, so a chunk sent again would be answered with offset 0. The upload is done at the
        # last chunk's answer and sends nothing more.
        image = build_image(3000)
        first, rest = client.image_chunk_sizes(512, is_serial=False, max_packet=None, upgrade=False)
        chunks = 1 + -(-(len(image) - first) // rest)
        self.link.image_window = 2
        self.unit.drop_responses = {chunks - 1}
        self.upload(image, 2)
        self.assert_stored(image)
        self.assertEqual(self.unit.upload_requests, chunks)
        self.assertEqual(self.unit.chunk_offsets.count(0), 1)

    def test_a_rewind_to_the_start_sends_the_first_chunk_alone(self) -> None:
        image = build_image(6000)
        self.unit.forget_after = 4
        pipelined_first = []
        submit = client.SmpPipeline.submit

        def watching_submit(pipeline, op, group, command, body, tag, **options):
            if group == client.Group.IMG and body.get("off") == 0:
                pipelined_first.append(tag)
            return submit(pipeline, op, group, command, body, tag, **options)

        with mock.patch.object(client.SmpPipeline, "submit", watching_submit):
            self.upload(image, 3)
        self.assert_stored(image)
        self.assertEqual(self.unit.chunk_offsets.count(0), 2)
        self.assertEqual(pipelined_first, [])

    def test_resumes_after_the_link_dropped(self) -> None:
        image = build_image(8000)
        self.link.fail_sends = {9}
        began = self.upload(image)
        self.assert_stored(image)
        self.assertEqual(self.link.reconnects, 1)
        # The second attempt's first chunk was answered with the offset the unit held, and the earlier bytes
        # were not sent again.
        self.assertGreater(began, client.image_chunk_sizes(512, is_serial=False, max_packet=None, upgrade=False)[0])
        self.assertEqual(self.unit.chunk_offsets.count(0), 1)

    def test_lost_answer_to_the_last_chunk_is_checked_in_the_list(self) -> None:
        image = build_image(2500)
        total = -(-(len(image) - client.image_chunk_sizes(512, is_serial=False, max_packet=None, upgrade=False)[0])
                  // (512 - 64)) + 1
        self.unit.drop_responses = {total}
        with contextlib.redirect_stdout(io.StringIO()) as out:
            self.upload(image)
        self.assert_stored(image)
        self.assertIn("the slot holds the image", out.getvalue())
        self.assertEqual(self.unit.upload_requests, total)

    def test_a_slot_that_holds_the_image_is_not_sent_again(self) -> None:
        image = build_image(3000)
        self.upload(image)
        sent = len(self.unit.chunk_offsets)
        began = self.upload(image)
        self.assertEqual(len(self.unit.chunk_offsets), sent)
        self.assertGreaterEqual(began, len(image))
        out, _ = run(client.command_image_upload, self.smp, file=self.write(image))
        self.assertIn("holds this image already", out)

    def write(self, image: bytes) -> Path:
        path = self.tmp / "image.bin"
        path.write_bytes(image)
        return path

    def test_unit_refusals_are_named(self) -> None:
        for kwargs, name in ((dict(product=0xBEEF), "WRONG_PRODUCT"), (dict(board="other/board"), "WRONG_BOARD"),
                             (dict(identity=False), "NO_IDENTITY")):
            self.unit.slots[1] = None
            with self.assertRaisesRegex(client.SmpError, name):
                self.upload(build_image(**kwargs))

    def test_group_one_errors_are_named(self) -> None:
        with self.assertRaisesRegex(client.SmpError, "HASH_NOT_FOUND"):
            run(client.command_image_test, self.smp, hash=bytes(32))
        self.assertIn("IMAGE_ALREADY_PENDING", client.IMG_ERRORS[28])
        self.assertEqual(client.IMG_ERRORS[34], "ACTIVE_SLOT_NOT_KNOWN")

    def test_stalled_transfer_gives_up(self) -> None:
        class StuckUnit(FakeUnit):
            def _upload(self, body: dict) -> dict:
                return super()._upload(body) if body["off"] == 0 else {"off": 0}

        self.unit = StuckUnit()
        self.link = FakeLink(self.unit)
        self.smp = client.SmpClient(self.link, 0.03, 1)
        with self.assertRaisesRegex(RuntimeError, "makes no progress"):
            self.upload(build_image(4000))


class CommandTest(ClientCase):
    def test_list_test_confirm_erase(self) -> None:
        out, _ = run(client.command_image_list, self.smp)
        self.assertIn("slot 0  version 0.0.1", out)
        self.assertIn("active confirmed bootable", out)

        image = build_image()
        path = self.tmp / "image.bin"
        path.write_bytes(image)
        out, _ = run(client.command_image_upload, self.smp, file=path)
        self.assertIn("slot 1  version 0.0.2", out)
        self.assertIn("(not selected)", out)

        out, _ = run(client.command_image_test, self.smp)
        self.assertIn("pending bootable  (test boot at the next reset)", out)
        with self.assertRaisesRegex(client.SmpError, "NO_FREE_SLOT"):
            run(client.command_image_erase, self.smp)

    def test_confirm_the_running_image_and_a_hash(self) -> None:
        self.unit.slots[0]["confirmed"] = False
        out, _ = run(client.command_image_confirm, self.smp)
        self.assertTrue(self.unit.slots[0]["confirmed"])
        self.assertIn("active confirmed", out)

        self.unit.slots[1] = FakeUnit._slot((0, 0, 2, 0), b"x")
        out, _ = run(client.command_image_confirm, self.smp, hash=self.unit.slots[1]["hash"])
        self.assertIn("(permanent swap at the next reset)", out)

    def test_frugal_lists_leave_out_false_flags(self) -> None:
        self.unit.frugal = True
        out, _ = run(client.command_image_list, self.smp)
        self.assertIn("active confirmed bootable", out)

    def test_dfu_status(self) -> None:
        out, _ = run(client.command_dfu_status, self.smp)
        self.assertIn("confirmed: yes", out)
        self.assertIn("state: CONFIRMED (0)", out)
        self.assertIn("reset_cause: 0x00000008 (POR)", out)
        self.assertEqual(client.format_reset_cause(0b10010), "0x00000012 (SOFTWARE WATCHDOG)")
        self.assertEqual(client.format_reset_cause(0), "0x00000000 (none)")

    def test_unit_without_the_groups_says_so(self) -> None:
        self.unit.has_dfu = False
        with self.assertRaisesRegex(client.SmpError, "no dfu group"):
            run(client.command_dfu_status, self.smp)

    def test_hash_argument(self) -> None:
        self.assertEqual(client.parse_image_hash("ab" * 32), bytes.fromhex("ab" * 32))
        for bad in ("abc", "zz" * 32, "ab" * 31):
            with self.assertRaises(argparse.ArgumentTypeError):
                client.parse_image_hash(bad)

    def test_version_strings(self) -> None:
        self.assertEqual(client.parse_version_string("1.2.3"), (1, 2, 3, 0))
        self.assertEqual(client.parse_version_string("1.2.3.4"), (1, 2, 3, 4))
        self.assertEqual(client.format_image_version((1, 2, 3, 0)), "1.2.3+0")


class UpdateTest(ClientCase):
    def update(self, path: Path, **options):
        return run(client.command_image_update, self.smp, file=path, **options)

    def test_an_image_uploaded_before_is_not_sent_again(self) -> None:
        path = self.image_file()
        run(client.command_image_upload, self.smp, file=path)
        sent = len(self.unit.chunk_offsets)
        out, _ = self.update(path)
        self.assertEqual(len(self.unit.chunk_offsets), sent)
        self.assertIn("holds this image already", out)
        self.assertIn("updated to 0.0.2+0, confirmed", out)

    def test_updates_swaps_and_waits_for_the_confirmation(self) -> None:
        path = self.image_file()
        out, err = self.update(path)
        self.assertIn("updated to 0.0.2+0, confirmed", out)
        self.assertEqual(self.unit.resets, 1)
        self.assertTrue(self.unit.slots[0]["active"] and self.unit.slots[0]["confirmed"])
        self.assertEqual(self.unit.slots[0]["version"], (0, 0, 2, 0))
        self.assertIn("the new image runs: VERIFYING (1)", out)
        self.assertIn("CONFIRMING (2), confirms in", out)
        self.assertIn("the unit answers again after", out)

    def test_a_failing_image_is_reported_as_reverted(self) -> None:
        self.unit.fails_new_image = True
        with self.assertRaises(RuntimeError) as caught:
            self.update(self.image_file())
        self.assertIn("the update was reverted: version 0.0.1 runs again and the new image (0.0.2) sits "
                      "unconfirmed in slot 1", str(caught.exception))
        self.assertIn("0x00000010 (WATCHDOG)", str(caught.exception))

    def test_no_reset_stops_after_the_test_mark(self) -> None:
        out, _ = self.update(self.image_file(), no_reset=True)
        self.assertEqual(self.unit.resets, 0)
        self.assertTrue(self.unit.slots[1]["pending"])
        self.assertIn("marked for test", out)

    def test_identity_is_checked_before_anything_is_sent(self) -> None:
        for kwargs, message in ((dict(product=0xBEEF), "product 0xbeef"),
                                (dict(board="other/board"), "board other/board"),
                                (dict(identity=False), "no identity TLV")):
            with self.assertRaisesRegex(RuntimeError, message):
                self.update(self.image_file(**kwargs))
        self.assertEqual(self.unit.upload_requests, 0)

    def test_old_firmware_without_board_skips_that_check(self) -> None:
        original = self.unit._dispatch

        def without_board(op, group, command, body):
            reply = original(op, group, command, body)
            reply.pop("board", None)
            return reply

        self.unit._dispatch = without_board
        out, err = self.update(self.image_file(), no_reset=True)
        self.assertIn("does not report its board", err)

    def test_downgrade_and_same_version_are_noted(self) -> None:
        self.unit.sw_version = (0, 1, 0)
        _, err = self.update(self.image_file(), no_reset=True)
        self.assertIn("downgrade, 0.0.2+0 is older than the unit's 0.1.0+0", err)

    def test_refuses_while_a_test_image_runs(self) -> None:
        self.unit.slots[0]["confirmed"] = False
        with self.assertRaisesRegex(RuntimeError, "not confirmed"):
            self.update(self.image_file())
        self.assertEqual(self.unit.upload_requests, 0)

    def test_refuses_a_pending_update(self) -> None:
        self.unit.slots[1] = FakeUnit._slot((0, 0, 3, 0), b"y", pending=True)
        with self.assertRaisesRegex(RuntimeError, "update is pending"):
            self.update(self.image_file())

    def test_the_running_image_is_not_installed_again(self) -> None:
        image = build_image(version=(0, 0, 1, 0))
        info = client.parse_image(image)
        self.unit.slots[0]["hash"] = info.hash
        path = self.tmp / "same.bin"
        path.write_bytes(image)
        out, _ = self.update(path)
        self.assertIn("runs this image already", out)
        self.assertEqual(self.unit.upload_requests, 0)

    def test_another_unit_answering_is_an_error(self) -> None:
        original = self.unit._reset

        def reset_to_another_unit() -> None:
            original()
            self.unit.uid = 0x0BADF00D

        self.unit._reset = reset_to_another_unit
        with self.assertRaisesRegex(RuntimeError, "another unit answers"):
            self.update(self.image_file())

    def test_a_link_that_drops_in_the_swap_is_brought_back(self) -> None:
        self.link.window = 1
        original_send = self.link.send
        state = {"down": 0}

        def send(packet: bytes) -> None:
            if self.unit.silent_requests and state["down"] < 2:
                state["down"] += 1
                raise ConnectionError("link lost")
            original_send(packet)

        self.link.send = send
        out, _ = self.update(self.image_file())
        self.assertIn("updated to", out)
        self.assertGreaterEqual(self.link.reconnects, 1)

    def test_a_unit_without_the_dfu_group_leaves_the_confirmation_to_the_client(self) -> None:
        # The update itself went through, but the exit status says the image is not confirmed (main() turns the
        # RuntimeError into exit 1, and prints no "confirmed").
        self.unit.has_dfu = False
        with contextlib.redirect_stdout(io.StringIO()) as out, self.assertRaises(RuntimeError) as caught:
            self.update(self.image_file())
        self.assertIn("runs but is not confirmed", str(caught.exception))
        self.assertIn("image confirm", str(caught.exception))
        self.assertNotIn("confirmed;", out.getvalue())
        self.assertTrue(self.unit.slots[0]["active"])
        self.assertFalse(self.unit.slots[0]["confirmed"])

    def test_watching_without_a_dfu_group_reports_whether_the_image_is_confirmed(self) -> None:
        self.unit.has_dfu = False
        new_hash = self.unit.slots[0]["hash"]
        with contextlib.redirect_stdout(io.StringIO()):
            self.assertTrue(client.watch_confirmation(self.smp, new_hash=new_hash, timeout_s=1.0))
            self.unit.slots[0]["confirmed"] = False
            self.assertFalse(client.watch_confirmation(self.smp, new_hash=new_hash, timeout_s=1.0))

    def test_a_unit_that_answers_but_not_the_image_requests_does_not_loop_forever(self) -> None:
        class NoImageStateUnit(FakeUnit):
            def handle(self, packet: bytes) -> list[bytes]:
                self.requests = getattr(self, "requests", 0) + 1
                return [] if client.unpack_header(packet)[1] in (1, 72) else super().handle(packet)

        unit = NoImageStateUnit()
        smp = client.SmpClient(FakeLink(unit), 0.01, 0)
        outcome: list = []

        def watch() -> None:
            try:
                client.watch_confirmation(smp, new_hash=bytes(32), timeout_s=0.3)
            except Exception as error:  # noqa: BLE001
                outcome.append(error)

        thread = threading.Thread(target=watch, daemon=True)
        thread.start()
        thread.join(5.0)
        self.assertFalse(thread.is_alive(), "the wait does not end")
        self.assertIsInstance(outcome[0], TimeoutError)
        self.assertIn("did not confirm", str(outcome[0]))
        self.assertIn("last error", str(outcome[0]))
        # It slept between the tries: a spin would have asked thousands of times in 0.3 s.
        self.assertLess(unit.requests, 400)


class FakeBle:
    """A stand-in for bleak: devices, a client, and an access point behind the smp characteristic, whose bus
    characteristic carries the stream of the unit's FakeCanMonitor."""

    def __init__(self, unit: FakeUnit, *, write_size: int = 30, notify_size: int = 23, window: int = 2,
                 max_packet: int = 512) -> None:
        self.unit, self.write_size, self.notify_size = unit, write_size, notify_size
        self.info = bytes([1]) + struct.pack("<H", max_packet) + bytes([window])
        self.writes: list[bytes] = []
        self.stream = bytearray()
        self.paired = 0
        self.connects = 0
        self.callback = None
        self.bus_callback = None
        self.has_bus = True
        self.client = None
        self.scans: list[str] = []
        fake = self

        class BleakError(Exception):
            pass

        class Characteristic:
            max_write_without_response_size = fake.write_size

            def __init__(self, uuid: str) -> None:
                self.uuid = uuid

        class Services:
            def get_characteristic(self, uuid):
                if uuid == client.BLE_SMP_UUID or (uuid == client.BLE_BUS_UUID and fake.has_bus):
                    return Characteristic(uuid)
                return None

        class Client:
            mtu_size = 247

            def __init__(self, device, disconnected_callback=None, timeout=None):
                self.device, self.disconnected = device, disconnected_callback
                self.services = Services()
                fake.client = self

            async def connect(self):
                fake.connects += 1

            async def disconnect(self):
                if self.disconnected:
                    self.disconnected(self)

            async def pair(self):
                fake.paired += 1

            async def read_gatt_char(self, uuid):
                assert uuid == client.BLE_INFO_UUID
                return bytearray(fake.info)

            async def start_notify(self, characteristic, callback):
                if characteristic.uuid == client.BLE_BUS_UUID:
                    fake.bus_callback = callback
                    if fake.unit.can is not None:
                        fake.unit.can.listening = True
                else:
                    fake.callback = callback

            async def write_gatt_char(self, characteristic, data, response=None):
                assert response is False
                fake.on_write(bytes(data))

        class Scanner:
            @staticmethod
            async def discover(timeout=None, return_adv=False):
                assert return_adv
                unit = (types.SimpleNamespace(name="EerieLeap Gauge", address="AA:BB:CC:DD:EE:FF"),
                        types.SimpleNamespace(local_name="EerieLeap Gauge", rssi=-60,
                                              manufacturer_data={0xFFFF: b"\x01\xa4"},
                                              service_uuids=[client.BLE_SERVICE_UUID.upper()]))
                other = (types.SimpleNamespace(name="Headphones", address="11:22:33:44:55:66"),
                         types.SimpleNamespace(local_name=None, rssi=-40, manufacturer_data={}, service_uuids=[]))
                return {"a": unit, "b": other}

            @staticmethod
            async def find_device_by_address(address, timeout=None):
                fake.scans.append(address)
                return types.SimpleNamespace(name="EerieLeap", address=address)

            @staticmethod
            async def find_device_by_filter(predicate, timeout=None):
                fake.scans.append("filter")
                device = types.SimpleNamespace(name="EerieLeap Gauge", address="AA:BB:CC:DD:EE:FF")
                advertisement = types.SimpleNamespace(local_name="EerieLeap Gauge")
                return device if predicate(device, advertisement) else None

        self.exc = types.ModuleType("bleak.exc")
        self.exc.BleakError = BleakError
        self.module = types.ModuleType("bleak")
        self.module.BleakClient, self.module.BleakScanner, self.module.exc = Client, Scanner, self.exc

    def on_write(self, data: bytes) -> None:
        self.writes.append(data)
        self.stream += data
        while len(self.stream) >= 9:
            size = 8 + int.from_bytes(self.stream[3:5], "big")
            if len(self.stream) < 1 + size:
                return
            route, packet = self.stream[0], bytes(self.stream[1:1 + size])
            del self.stream[:1 + size]
            for response in self.unit.handle(packet):
                self.notify(route, response)
            # What the request let out of the unit's stream follows its answer.
            while self.unit.can is not None and self.unit.can.outbox:
                notification = self.unit.can.outbox.pop(0)
                if self.bus_callback is not None:
                    self.bus_callback(None, bytearray(notification))

    def notify(self, route: int, packet: bytes) -> None:
        data = bytes([route]) + packet
        for offset in range(0, len(data), self.notify_size):
            self.callback(None, bytearray(data[offset:offset + self.notify_size]))

    @contextlib.contextmanager
    def installed(self):
        with mock.patch.dict(sys.modules, {"bleak": self.module, "bleak.exc": self.exc}):
            yield self


class BleTest(ClientCase):
    def open(self, fake: FakeBle, target: str = "EerieLeap Gauge", **options) -> client.SmpBleLink:
        link = client.SmpBleLink(target, scan_timeout_s=1.0, **options)
        self.addCleanup(link.close)
        return link

    def test_requests_are_split_with_the_route_in_the_first_write_only(self) -> None:
        fake = FakeBle(self.unit, write_size=20)
        with fake.installed():
            link = self.open(fake, route=5)
            smp = client.SmpClient(link, 1.0, 1)
            padding = {"pad": "x" * 100}
            params = smp.request(client.Op.READ, client.Group.OS, 6, padding)
        self.assertEqual(params["buf_size"], 512)
        self.assertTrue(all(len(piece) <= 20 for piece in fake.writes))
        self.assertGreater(len(fake.writes), 5)
        self.assertEqual(fake.writes[0][0], 5)
        # One packet, one route byte: the writes joined are the route and the request.
        joined = b"".join(fake.writes)
        sequence = smp.next_sequence() - 1 & 0xFF
        self.assertEqual(joined[1:], client.encode_request(client.Op.READ, 0, 6, padding, sequence))

    def test_responses_are_reassembled_from_notifications(self) -> None:
        fake = FakeBle(self.unit, notify_size=23)
        with fake.installed():
            smp = client.SmpClient(self.open(fake), 1.0, 1)
            info = smp.read(client.Group.DEVICE, 0)
        self.assertEqual(info["board"], BOARD)
        self.assertEqual(info["uid"], self.unit.uid)

    def test_pairs_unless_told_not_to(self) -> None:
        fake = FakeBle(self.unit)
        with fake.installed():
            self.open(fake)
            self.open(fake, pair=False)
        self.assertEqual(fake.paired, 1)

    def test_window_comes_from_info_and_is_one_behind_a_route(self) -> None:
        fake = FakeBle(self.unit, window=3, max_packet=400)
        with fake.installed():
            self.assertEqual(self.open(fake).image_window, 3)
            self.assertEqual(self.open(fake, route=7).image_window, 1)
            link = self.open(fake)
            self.assertEqual(link.max_packet_size, 400)
            smp = client.SmpClient(link, 1.0, 1)
            plan = client.plan_upload(smp, None, False)
        # min(info window 3, buf_count - 1 = 3), and chunks below the access point's packet size.
        self.assertEqual(plan.window, 3)
        self.assertEqual(plan.chunk_size, 400 - 64)

    def test_address_and_name_selection(self) -> None:
        fake = FakeBle(self.unit)
        with fake.installed():
            self.open(fake, "AA:BB:CC:DD:EE:FF")
            self.assertEqual(fake.scans, ["AA:BB:CC:DD:EE:FF"])
            self.open(fake, "eerieleap gauge")
            self.assertEqual(fake.scans[-1], "filter")
            with self.assertRaisesRegex(ConnectionError, "no Bluetooth device"):
                self.open(fake, "Another")

    def test_packets_of_other_routes_are_skipped(self) -> None:
        fake = FakeBle(self.unit)
        with fake.installed():
            link = self.open(fake, route=5)
            fake.notify(9, client.encode_request(client.Op.READ_RESPONSE, 0, 6, {"x": 1}, 7))
            fake.notify(5, client.encode_request(client.Op.READ_RESPONSE, 0, 6, {"x": 2}, 8))
            packet = link.receive(1.0)
        self.assertEqual(cbor2.loads(packet[8:]), {"x": 2})
        self.assertEqual(link.stray_packets, 1)

    def test_a_lost_connection_is_a_connection_error_and_reconnect_restores_it(self) -> None:
        fake = FakeBle(self.unit)
        with fake.installed():
            link = self.open(fake)
            smp = client.SmpClient(link, 1.0, 1)
            smp.read(client.Group.OS, 6)
            fake.client.disconnected(fake.client)
            with self.assertRaises(ConnectionError):
                link.receive(1.0)
            with self.assertRaises(ConnectionError):
                smp.read(client.Group.OS, 6)
            link.reconnect()
            smp.read(client.Group.OS, 6)
        self.assertEqual(fake.connects, 2)

    def test_a_failed_reconnect_keeps_the_link_lost(self) -> None:
        # The unit resets and does not advertise while it swaps: the scan finds nothing. The next send must
        # say the link is lost, not fail on a missing client, and a later reconnect must work.
        fake = FakeBle(self.unit)
        with fake.installed():
            link = self.open(fake)
            smp = client.SmpClient(link, 0.2, 1)
            smp.read(client.Group.OS, 6)

            misses = {"n": 2}
            found = fake.module.BleakScanner.find_device_by_filter

            async def flaky(predicate, timeout=None):
                if misses["n"] > 0:
                    misses["n"] -= 1
                    return None
                return await found(predicate, timeout)

            fake.module.BleakScanner.find_device_by_filter = staticmethod(flaky)
            fake.client.disconnected(fake.client)
            with self.assertRaises(ConnectionError):
                link.reconnect()
            with self.assertRaises(ConnectionError):
                smp.read(client.Group.OS, 6)
            with self.assertRaises(ConnectionError):
                link.receive(0.05)

            info = client.poll_device_info(smp, deadline_s=time.monotonic() + 5, expect_restart=True, what="test")
            self.assertEqual(info["uid"], self.unit.uid)
            self.assertEqual(misses["n"], 0)

    def test_scan_lists_access_points(self) -> None:
        fake = FakeBle(self.unit)
        with fake.installed():
            out, _ = run(client.command_ble_scan, None, time=0.1, all=False)
            self.assertIn("AA:BB:CC:DD:EE:FF", out)
            self.assertIn("access point", out)
            self.assertIn("0xffff:01a4", out)
            self.assertNotIn("Headphones", out)
            out, _ = run(client.command_ble_scan, None, time=0.1, all=True)
            self.assertIn("Headphones", out)

    def test_upload_through_the_access_point(self) -> None:
        fake = FakeBle(self.unit, window=2)
        image = build_image(5000)
        with fake.installed():
            link = self.open(fake)
            smp = client.SmpClient(link, 1.0, 1)
            run(client.command_image_upload, smp, file=self.write(image))
        self.assertEqual(self.unit.slots[1]["filesha"], hashlib.sha256(image).digest())

    def write(self, image: bytes) -> Path:
        path = self.tmp / "image.bin"
        path.write_bytes(image)
        return path


class SerialTest(ClientCase):
    """The console transport against a pty with a FakeUnit behind it: the frames, the smaller chunks, a reopen."""

    def setUp(self) -> None:
        super().setUp()
        self.master, slave = pty.openpty()
        self.name = os.ttyname(slave)
        self.addCleanup(os.close, slave)
        self.stop = threading.Event()
        self.thread = threading.Thread(target=self.serve, daemon=True)
        self.thread.start()
        self.addCleanup(self.thread.join, 2.0)
        self.addCleanup(self.stop.set)
        self.addCleanup(os.close, self.master)

    def serve(self) -> None:
        decoder = client.SerialFrameDecoder()
        while not self.stop.is_set():
            if not select.select([self.master], [], [], 0.05)[0]:
                continue
            try:
                data = os.read(self.master, 4096)
            except OSError:
                return
            for packet in decoder.feed(data):
                for response in self.unit.handle(packet):
                    for line in client.encode_serial_lines(response):
                        os.write(self.master, line)

    def test_upload_over_the_console_and_a_reopen(self) -> None:
        try:
            link = client.SmpSerialLink(self.name)
        except OSError as error:
            self.skipTest(f"no usable pty: {error}")
        self.addCleanup(link.close)
        smp = client.SmpClient(link, 1.0, 3)
        image = build_image(3000)
        run(client.command_image_upload, smp, file=self.write(image))
        self.assertEqual(self.unit.slots[1]["filesha"], hashlib.sha256(image).digest())
        # The frame's length and CRC16 count against the unit's buffer: no request is larger than it.
        self.assertLessEqual(self.unit.max_request, self.unit.buf_size - client.SERIAL_FRAME_OVERHEAD)
        link.reconnect()
        self.assertEqual(smp.read(client.Group.OS, 6)["buf_size"], 512)

    def write(self, image: bytes) -> Path:
        path = self.tmp / "image.bin"
        path.write_bytes(image)
        return path


class ConsoleServer:
    """A pty console with a FakeUnit behind it that boots as the S3's USB-Serial-JTAG does over a reset.

    From the reset until `rx_delay_s` after the banner, the app is not reading. Any data the host writes in that
    time stays in the peripheral's FIFO, the driver never drains it, and from then on the unit hears nothing
    (`wedged`), whatever the host writes. MCUboot prints its own banner first (a swap follows, silent).
    """

    BANNER = b"*** Booting Zephyr OS build test ***\r\n"

    def __init__(self, unit: FakeUnit, *, announce: bytes | None = BANNER, mcuboot: bool = True,
                 boot_s: float = 0.4, rx_delay_s: float = 0.3, revert_delay_s: float = 0.15) -> None:
        self.unit, self.announce, self.mcuboot = unit, announce, mcuboot
        self.boot_s, self.rx_delay_s, self.revert_delay_s = boot_s, rx_delay_s, revert_delay_s
        self.master, self.slave = pty.openpty()
        self.name = os.ttyname(self.slave)
        self.wedged = False
        self.booted_at: list[float] = []
        self.stop = threading.Event()
        self.thread = threading.Thread(target=self.serve, daemon=True)
        self.thread.start()

    def close(self) -> None:
        self.stop.set()
        self.thread.join(2.0)
        os.close(self.slave)
        os.close(self.master)

    def serve(self) -> None:
        decoder = client.SerialFrameDecoder()
        reboots = self.unit.reboots
        rx_from, announce_at, revert_at = 0.0, None, None
        while not self.stop.is_set():
            now = time.monotonic()
            if self.unit.revert_next and revert_at is None:
                revert_at = now + self.revert_delay_s       # The watchdog resets the unit a moment after FAILED.
            if revert_at is not None and now >= revert_at:
                self.unit.revert_next, revert_at = False, None
                self.unit._revert()
            if self.unit.reboots != reboots:
                reboots = self.unit.reboots
                self.booted_at.append(now)
                if self.mcuboot:
                    os.write(self.master, b"*** Booting MCUboot v2.2.0 ***\r\n")
                announce_at = now + self.boot_s
                rx_from = announce_at + self.rx_delay_s
            if announce_at is not None and now >= announce_at:
                announce_at = None
                if self.announce:
                    os.write(self.master, self.announce)
            if not select.select([self.master], [], [], 0.01)[0]:
                continue
            try:
                data = os.read(self.master, 4096)
            except OSError:
                return
            if time.monotonic() < rx_from:
                self.wedged = True
            if self.wedged:
                continue
            for packet in decoder.feed(data):
                for response in self.unit.handle(packet):
                    for line in client.encode_serial_lines(response):
                        os.write(self.master, line)


class SerialBootTest(ClientCase):
    """After a reset a console is not written to until the unit says it is up (the S3 would wedge for good)."""

    def setUp(self) -> None:
        super().setUp()
        self.unit.silent_after_reset = 0

    def console(self, **options) -> tuple[ConsoleServer, client.SmpClient]:
        server = ConsoleServer(self.unit, **options)
        self.addCleanup(server.close)
        try:
            link = client.SmpSerialLink(server.name)
        except OSError as error:
            self.skipTest(f"no usable pty: {error}")
        self.addCleanup(link.close)
        return server, client.SmpClient(link, 0.3, 1)

    def test_a_write_while_the_unit_boots_wedges_the_fake(self) -> None:
        server, smp = self.console()
        client.request_reset(smp)
        with self.assertRaises(TimeoutError):
            smp.read(client.Group.DEVICE, 0, retries=0)
        time.sleep(1.2)
        with self.assertRaises(TimeoutError):
            smp.read(client.Group.DEVICE, 0, retries=0)
        self.assertTrue(server.wedged)

    def test_reset_listens_for_the_banner_and_writes_nothing_meanwhile(self) -> None:
        server, smp = self.console()
        started_s = time.monotonic()
        out, err = run(client.command_reset, smp)
        self.assertGreater(time.monotonic() - started_s, 0.4 + client.SERIAL_BOOT_SETTLE_S - 0.1)
        self.assertIn("reset requested", out)
        self.assertIn("the unit is up", out)
        self.assertFalse(server.wedged)
        self.assertEqual(smp.read(client.Group.DEVICE, 0)["uid"], self.unit.uid)

    def test_the_mcuboot_banner_is_not_the_apps(self) -> None:
        # MCUboot prints first, and the swap that follows is silent for longer than the settle time.
        server, smp = self.console(boot_s=0.9)
        run(client.command_reset, smp)
        self.assertFalse(server.wedged)
        self.assertEqual(smp.read(client.Group.DEVICE, 0)["uid"], self.unit.uid)

    def test_the_prompt_alone_ends_the_wait(self) -> None:
        # A prompt has no newline, and a terminal colors it.
        server, smp = self.console(announce=b"\x1b[1;32muart:~$ \x1b[m", mcuboot=False)
        out, _ = run(client.command_reset, smp)
        self.assertIn("the unit is up", out)
        self.assertFalse(server.wedged)

    def test_without_a_banner_the_wait_ends_with_the_time_given(self) -> None:
        server, smp = self.console(announce=None)
        started_s = time.monotonic()
        out, err = run(client.command_reset, smp, timeout_s=0.7)
        self.assertGreaterEqual(time.monotonic() - started_s, 0.7)
        self.assertNotIn("the unit is up", out)
        self.assertIn("no boot banner within 1 s", err)
        self.assertFalse(server.wedged)

    def test_update_over_the_console_does_not_write_while_the_unit_boots(self) -> None:
        server, smp = self.console()
        out, _ = run(client.command_image_update, smp, file=self.image_file())
        self.assertIn("not writing to the console until the unit prints its boot banner", out)
        self.assertIn("updated to 0.0.2+0, confirmed", out)
        self.assertFalse(server.wedged)
        self.assertEqual(self.unit.resets, 1)

    def test_update_without_a_banner_asks_the_unit_after_the_time_given(self) -> None:
        server, smp = self.console(announce=None)
        out, err = run(client.command_image_update, smp, file=self.image_file(), timeout_s=0.7)
        self.assertIn("warning: no boot banner within 1 s; asking the unit anyway", err)
        self.assertIn("updated to 0.0.2+0, confirmed", out)
        self.assertFalse(server.wedged)

    def test_a_failing_image_is_reverted_without_writes_to_the_unit_that_resets(self) -> None:
        self.unit.fails_new_image = True
        self.unit.auto_revert = False
        server, smp = self.console()
        with self.assertRaises(RuntimeError) as caught:
            run(client.command_image_update, smp, file=self.image_file())
        self.assertIn("the update was reverted", str(caught.exception))
        self.assertEqual(len(server.booted_at), 2)
        self.assertFalse(server.wedged)

    def test_other_links_do_not_listen(self) -> None:
        self.assertFalse(hasattr(FakeLink(self.unit), "wait_for_boot"))
        self.assertIsNone(client.restart_unit(client.SmpClient(FakeLink(self.unit), 0.03, 1), 1.0, "test"))
        self.assertFalse(hasattr(client.SmpCanLink, "wait_for_boot"))
        self.assertFalse(hasattr(client.SmpBleLink, "wait_for_boot"))


class ReassemblerTest(unittest.TestCase):
    def packet(self, size: int = 40) -> bytes:
        return client.encode_request(client.Op.READ_RESPONSE, 65, 0, {"d": "x" * size}, 3)

    def test_packets_split_anywhere_and_packed_together(self) -> None:
        first, second = self.packet(), self.packet(60)
        stream = bytes([4]) + first + bytes([9]) + second
        for cut in range(1, len(stream)):
            reassembler = client.BleReassembler()
            packets = reassembler.accept(stream[:cut], 0.0) + reassembler.accept(stream[cut:], 0.1)
            self.assertEqual(packets, [(4, first), (9, second)])

    def test_a_gap_inside_a_packet_drops_it(self) -> None:
        reassembler = client.BleReassembler()
        packet = self.packet()
        self.assertEqual(reassembler.accept(bytes([4]) + packet[:20], 0.0), [])
        later = client.BLE_REASSEMBLY_TIMEOUT_S + 0.5
        self.assertEqual(reassembler.accept(bytes([4]) + packet, later), [(4, packet)])
        self.assertEqual(reassembler.dropped, 1)

    def test_an_impossible_length_drops_the_buffer(self) -> None:
        reassembler = client.BleReassembler(max_packet=100)
        self.assertEqual(reassembler.accept(bytes([4]) + self.packet(500)[:30], 0.0), [])
        self.assertEqual(reassembler.dropped, 1)
        self.assertEqual(reassembler.accept(bytes([4]) + self.packet(), 0.1), [(4, self.packet())])


class FollowTest(ClientCase):
    def setUp(self) -> None:
        super().setUp()
        self.log = self.unit.log = FakeLog()
        self.path = self.tmp / "copy.mf4"
        self.steps: list = []
        patcher = mock.patch.object(client, "wait", side_effect=self.wait)
        patcher.start()
        self.addCleanup(patcher.stop)

    def wait(self, seconds: float) -> None:
        self.assertTrue(self.steps, "the client kept waiting for a file that is not changing")
        self.steps.pop(0)()

    def get(self, **options):
        defaults = dict(kind=4, name=self.log.name, file=self.path, resume=False, growing=False, follow=False,
                        interval=0.0, in_flight=1)
        return run(client.command_assets_get, self.smp, **(defaults | options))

    def test_a_growing_file_is_refused_without_the_key(self) -> None:
        with self.assertRaisesRegex(client.SmpError, "in use"):
            self.get()

    def test_growing_saves_the_synced_part(self) -> None:
        out, _ = self.get(growing=True)
        self.assertEqual(self.path.read_bytes(), bytes(self.log.content))
        self.assertIn("still being written", out)
        self.assertIn("250 B", out)
        self.assertTrue(all(request.get("growing") for request in self.log.requests))
        self.assertEqual(len(self.log.requests), 3)     # 100 + 100 + 50

    def test_growing_with_pipelining(self) -> None:
        self.get(growing=True, in_flight=3)
        self.assertEqual(self.path.read_bytes(), bytes(self.log.content))

    def test_an_empty_file_is_a_copy_too(self) -> None:
        self.log.content = bytearray()
        out, _ = self.get(growing=True)
        self.assertEqual(self.path.read_bytes(), b"")
        self.assertIn("still being written", out)

    def test_a_closed_file_is_not_called_growing(self) -> None:
        self.log.close()
        out, _ = self.get(growing=True)
        self.assertEqual(self.path.read_bytes(), bytes(self.log.content))
        self.assertNotIn("still being written", out)

    def test_follow_reads_on_and_rereads_the_closed_file_after_changed(self) -> None:
        for in_flight in (1, 3):
            with self.subTest(in_flight=in_flight):
                self.log.__init__()
                self.steps = [lambda: self.log.append(300), lambda: self.log.append(10), self.log.close]
                out, _ = self.get(follow=True, in_flight=in_flight)
                self.assertEqual(self.path.read_bytes(), bytes(self.log.content))
                self.assertTrue(self.path.read_bytes().startswith(b"FINALIZED!!"))
                self.assertEqual(self.log.changed_sent, 1)
                self.assertIn("(+300 B), still being written", out)
                self.assertIn("(+10 B), still being written", out)
                self.assertIn("closed; reading it again", out)
                self.assertNotIn("is still being written", out)
                self.assertEqual(self.steps, [])

    def test_follow_continues_from_its_own_length(self) -> None:
        self.steps = [lambda: self.log.append(120), self.log.close]
        self.get(follow=True)
        offsets = [request["off"] for request in self.log.requests]
        self.assertEqual(offsets[:5], [0, 100, 200, 250, 350])     # The second pass starts at its own length.
        self.assertEqual(offsets.count(0), 2)                      # Then the closed file is read again.

    def test_follow_rereads_after_an_answer_without_growing(self) -> None:
        self.log.changed_mode = "silent"
        self.steps = [lambda: self.log.append(40), self.log.close]
        out, _ = self.get(follow=True)
        self.assertEqual(self.path.read_bytes(), bytes(self.log.content))
        self.assertEqual(self.log.changed_sent, 0)
        self.assertIn("closed; reading it again", out)

    def test_follow_of_a_closed_file_is_one_read(self) -> None:
        self.log.close()
        out, _ = self.get(follow=True)
        self.assertEqual(self.path.read_bytes(), bytes(self.log.content))
        self.assertNotIn("still being written", out)

    def test_changed_is_named(self) -> None:
        self.get(growing=True)
        self.log.close()
        with self.assertRaisesRegex(client.SmpError, "changed"):
            self.get(growing=True, resume=True)

    def test_a_close_inside_the_first_pass_reads_the_file_again(self) -> None:
        for in_flight in (1, 3):
            for mode in ("error", "silent"):
                with self.subTest(in_flight=in_flight, mode=mode):
                    self.log.__init__()
                    self.log.changed_mode = mode
                    self.log.before_request[2] = self.log.close     # The second chunk is served from the closed file.
                    self.steps = []
                    out, _ = self.get(follow=True, in_flight=in_flight)
                    self.assertEqual(self.path.read_bytes(), bytes(self.log.content))
                    self.assertTrue(self.path.read_bytes().startswith(b"FINALIZED!!"))
                    self.assertIn("closed; reading it again", out)

    def test_a_file_that_grows_during_a_pass_is_read_to_its_new_end(self) -> None:
        for in_flight in (1, 3):
            with self.subTest(in_flight=in_flight):
                self.log.__init__()
                self.log.before_request[2] = lambda: self.log.append(130)
                self.steps = []
                self.get(growing=True, in_flight=in_flight)
                self.assertEqual(self.path.read_bytes(), bytes(self.log.content))
                self.assertGreater(len(self.log.content), 250)

    def test_a_listed_growing_file_is_marked(self) -> None:
        self.assertIn("growing", client.format_asset_entry({"name": "a.mf4", "size": 5, "growing": True}))
        self.assertNotIn("growing", client.format_asset_entry({"name": "a.mf4", "size": 5}))


class TimeTest(ClientCase):
    def test_logging_status_age_is_on_the_unit_clock(self) -> None:
        out, _ = run(client.command_logging_status, self.smp)
        self.assertIn("(unit clock; 2 min ago)", out)
        self.assertNotIn("started:", out)
        self.unit.wall_offset_ms = 5000000000
        out, _ = run(client.command_logging_status, self.smp)
        self.assertIn(f"started: {client.format_utc_ms(self.unit.clock_ms - 125000 + 5000000000)}", out)

    def test_logging_status_without_time_has_no_age(self) -> None:
        self.unit.has_time = False
        out, _ = run(client.command_logging_status, self.smp)
        self.assertIn("(unit clock)", out)
        self.assertNotIn("ago", out)

    def test_unset_clock_says_so(self) -> None:
        out, _ = run(client.command_time, self.smp, time_command=None)
        self.assertIn(f"now_ms: {self.unit.clock_ms}  ({client.format_unix_ms(self.unit.clock_ms)}", out)
        self.assertIn("uptime_ms: 4321", out)
        self.assertIn("boot_id: 7", out)
        self.assertIn("wall_ms: not set", out)

    def test_set_takes_the_pc_clock(self) -> None:
        before_ms = time.time() * 1000
        out, _ = run(client.command_time, self.smp, time_command="set", wall_ms=None)
        after_ms = time.time() * 1000
        wall_ms = self.unit.clock_ms + self.unit.wall_offset_ms
        self.assertTrue(before_ms - 1 <= wall_ms <= after_ms + 1000, (before_ms, wall_ms, after_ms))
        self.assertIn("set wall_ms", out)
        self.assertIn(f"wall_ms: {wall_ms}  (", out)
        out, _ = run(client.command_time, self.smp, time_command=None)
        self.assertNotIn("not set", out)

    def test_set_with_a_given_time(self) -> None:
        out, _ = run(client.command_time, self.smp, time_command="set", wall_ms=1893456000123)
        self.assertEqual(self.unit.clock_ms + self.unit.wall_offset_ms, 1893456000123)
        self.assertIn("2030-01-01T00:00:00.123Z", out)

    def test_a_time_before_2020_is_refused(self) -> None:
        for wall_ms in (1000, 4102444800000):
            with self.assertRaisesRegex(RuntimeError, "EINVAL.*2020-01-01"):
                run(client.command_time, self.smp, time_command="set", wall_ms=wall_ms)
        self.assertIsNone(self.unit.wall_offset_ms)

    def test_an_unanswered_set_is_made_again_with_a_fresh_time(self) -> None:
        self.unit.drop_set_time = 1
        out, _ = run(client.command_time, self.smp, time_command="set", wall_ms=None)
        self.assertEqual(len(self.unit.set_times), 1)       # The first never reached the unit; one request, not a resend.
        self.assertIn("difference from this PC:", out)
        self.unit.drop_set_time = 3
        with self.assertRaises(TimeoutError):
            run(client.command_time, self.smp, time_command="set", wall_ms=None)

    def test_a_unit_without_a_time_service_says_so(self) -> None:
        self.unit.has_time = False
        for options in (dict(time_command=None), dict(time_command="set", wall_ms=None)):
            with self.assertRaisesRegex(RuntimeError, "no time service"):
                run(client.command_time, self.smp, **options)


class CanRecordsTest(unittest.TestCase):
    """The stream's records against an encoder written here from rt_core's docs/can_monitor.md."""

    def decode(self, *notifications: bytes) -> tuple[client.CanStreamDecoder, list[client.CanRecord]]:
        decoder = client.CanStreamDecoder()
        records = [record for data in notifications for record in decoder.decode(data)]
        return decoder, records

    def test_every_kind_and_its_time(self) -> None:
        fd_payload = bytes(range(12))
        _, records = self.decode(notification([
            frame_record(5, 0x123, bytes(range(8))),
            frame_record(200, 0x18DA00F1, fd_payload, ide=True, fdf=True, brs=True),
            frame_record(0, 0x7FF, rtr=True, dlc=3),
            frame_record(10, 0x100, b"\x01", tx=True, failed=True),
            other_record(2, bytes([3, 255, 7]) + leb(500000)),
            stats_record(list(range(1, 14))),
            other_record(4, leb(300) + bytes([1])),
            other_record(5, bytes([4])),
            other_record(6, bytes([3]) + leb(2)),
            other_record(7, bytes([2])),
        ], base_us=1000))
        kinds = [record.kind for record in records]
        self.assertEqual(kinds, [0, 0, 0, 0, 2, 3, 4, 5, 6, 7])
        self.assertEqual([record.time_us for record in records[:4]], [1005, 1205, 1205, 1215])
        self.assertEqual(records[0].frame_text(), "123#0001020304050607")
        self.assertEqual(records[1].frame_text(), "18DA00F1##1" + fd_payload.hex().upper())
        self.assertEqual(records[2].frame_text(), "7FF#R3")
        self.assertTrue(records[3].tx and records[3].tx_failed)
        self.assertEqual((records[4].state, records[4].tec, records[4].rec, records[4].bitrate), (3, 255, 7, 500000))
        self.assertEqual(records[5].values, tuple(range(1, 14)))
        self.assertEqual((records[6].count, records[6].code), (300, 1))
        self.assertEqual((records[7].code, records[8].code, records[8].count, records[9].code), (4, 3, 2, 2))
        self.assertTrue(all(record.time_us == 1215 for record in records[4:]))

    def test_leb128_at_its_limits(self) -> None:
        for value in (0, 127, 128, 16383, 16384, 2**63, 2**64 - 1):
            self.assertEqual(client.encode_leb128(value), leb(value))
            self.assertEqual(client.decode_leb128(b"\xff" + leb(value), 1), (value, 1 + len(leb(value))))
        for bad in (b"\x80" * 10 + b"\x00", b"\xff" * 9 + b"\x02", b"\x80\x80"):
            with self.assertRaises(client.CanRecordError):
                client.decode_leb128(bad, 0)

    def test_changed_bytes_are_rebuilt_and_gap_and_tap_empty_the_table(self) -> None:
        first, second, third = bytes(range(8)), bytes([0, 9, 2, 3, 4, 5, 6, 0xEE]), bytes([7] * 8)
        decoder, records = self.decode(notification([
            frame_record(1, 0x10, first),
            changed_record(1, 0x10, second, first),
            other_record(4, leb(1) + bytes([1])),
            changed_record(1, 0x10, third, second),
            frame_record(1, 0x10, third),
            other_record(7, bytes([1])),
            changed_record(1, 0x10, second, third),
        ]))
        self.assertEqual(records[1].data, second)
        self.assertIsNone(records[3].data)
        self.assertEqual(records[3].frame_text(), "010#??")
        self.assertIsNone(records[6].data)
        self.assertEqual(decoder.unknown_changes, 2)

    def test_seq_holes(self) -> None:
        decoder, _ = self.decode(*(notification([], seq=seq) for seq in (65534, 65535, 0, 4)))
        self.assertEqual(decoder.seq_holes, 3)

    def test_malformed_notifications(self) -> None:
        for data in (b"\x01\x00", notification([frame_record(0, 1, b"\x01")])[:-1],
                     notification([frame_record(0, 1, b"\x01")]) + b"\x00",
                     notification([bytes([0x81, 0x20, 0, 0, 0, 0])]),
                     notification([frame_record(0, 0xFFFF, b"\x01")]),
                     bytes([2]) + notification([])[1:]):
            with self.subTest(data=data.hex()), self.assertRaises(client.CanRecordError):
                client.CanStreamDecoder().decode(data)

    def test_frames_in_candumps_notation(self) -> None:
        frame = client.parse_can_frame("123#DEADBEEF")
        self.assertEqual((frame.can_id, frame.extended, frame.data, frame.flags),
                         (0x123, False, bytes.fromhex("DEADBEEF"), 0))
        self.assertEqual(client.parse_can_frame("18DA00F1#00").flags, 1)
        remote = client.parse_can_frame("123#R4")
        self.assertEqual((remote.data, remote.flags, str(remote)), (bytes(4), 8, "123#R4"))
        fd = client.parse_can_frame("123##1" + "AB" * 12)
        self.assertEqual((fd.flags, len(fd.data), str(fd)), (6, 12, "123##1" + "AB" * 12))
        self.assertEqual(str(client.parse_can_frame("00000123#11.22")), "00000123#1122")
        for bad in ("123#" + "00" * 9, "800#00", "123##1" + "00" * 10, "xyz#00", "123", "123#R9", "123#0"):
            with self.subTest(bad=bad), self.assertRaises(ValueError):
                client.parse_can_frame(bad)

    def test_replay_records_byte_for_byte(self) -> None:
        self.assertEqual(client.encode_can_frame_record(5, client.parse_can_frame("123#0001020304050607")),
                         bytes([0x08, 0x05, 0x23, 0x01]) + bytes(range(8)))
        self.assertEqual(client.encode_can_frame_record(200, client.parse_can_frame("18DA00F1##1" + "00" * 12)),
                         bytes([0x79, 0xC8, 0x01, 0xF1, 0x00, 0xDA, 0x18]) + bytes(12))
        self.assertEqual(client.encode_can_frame_record(0, client.parse_can_frame("7FF#R4")),
                         bytes([0x84, 0x01, 0x00, 0xFF, 0x07]))

    def test_candump_logs(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory) / "capture.log"
            path.write_text("# a comment\n(1700000000.000100) can0 123#00 R\n(1700000000.5) can1 00000456#R T\n"
                            "\n(1700000001.000000) vcan0 123##0" + "11" * 16 + "\n")
            frames = client.read_candump_log(path)
            self.assertEqual([time_us for time_us, _ in frames], [1700000000000100, 1700000000500000, 1700000001000000])
            self.assertEqual([str(frame) for _, frame in frames], ["123#00", "00000456#R", "123##0" + "11" * 16])
            self.assertEqual(len(client.read_candump_log(path, rx_only=True)), 2)
            path.write_text("(1.0) can0 123#00\n123#00\n")
            with self.assertRaisesRegex(RuntimeError, "capture.log:2: not a candump log line"):
                client.read_candump_log(path)

    def test_counter_check(self) -> None:
        check = client.CanCounterCheck(0x123)
        for number in (0, 1, 2, 5, 3, 6):
            check.add(client.CanRecord(client.CanRecordKind.FRAME, 0, can_id=0x123,
                                       data=client.generated_payload(number, 8)))
        check.add(client.CanRecord(client.CanRecordKind.FRAME, 0, can_id=0x124, data=bytes(8)))
        check.add(client.CanRecord(client.CanRecordKind.FRAME, 0, can_id=0x123, data=bytes(12)))
        self.assertEqual((check.checked, check.missing, check.repeated, check.foreign), (6, 2, 1, 1))
        self.assertEqual(client.generated_payload(0x0102, 12),
                         bytes([2, 1, 0, 0, 0, 0, 0, 0]) + bytes(0xA5 ^ index for index in range(8, 12)))


class CanMonitorTest(ClientCase):
    """The can commands through the fake bleak against a FakeCanMonitor."""

    def setUp(self) -> None:
        super().setUp()
        patcher = mock.patch.multiple(client, CAN_MONITOR_STATS_INTERVAL_S=0.01, CAN_MONITOR_END_WAIT_S=0.5,
                                      CAN_MONITOR_TICK_S=0.005, CAN_REPLAY_RETRY_S=0.001)
        patcher.start()
        self.addCleanup(patcher.stop)
        self.can = self.unit.can = FakeCanMonitor()
        self.fake = FakeBle(self.unit)

    def command(self, handler, **options) -> tuple[str, str]:
        defaults = dict(port=0, mode="auto", port_type=None, port_bitrate=None, port_data_bitrate=None, filter=None,
                        delta=False, out=None, print_frames=False, duration=0.05, expect=None, generate=None,
                        generate_id=0x123, synthetic=None)
        with self.fake.installed():
            link = client.SmpBleLink("EerieLeap Gauge", scan_timeout_s=1.0)
            self.addCleanup(link.close)
            return run(handler, client.SmpClient(link, 1.0, 1), **(defaults | options))

    def commands(self, number: int | None = None) -> list:
        return [command if number is None else body for command, body in self.can.requests
                if number is None or command == number]

    def test_monitor_keeps_the_lease_logs_and_closes(self) -> None:
        self.can.batches = [[frame_record(100, 0x123, bytes(range(8))), stats_record([1, 0, 230] + [0] * 10)],
                            [frame_record(50, 0x18DA00F1, b"\x01", ide=True), frame_record(1, 0x100, b"\x02", tx=True)]]
        log = self.tmp / "can.log"
        out, _ = self.command(client.command_can_monitor, out=log, duration=0.1)
        self.assertEqual(self.commands()[:2], [0, 1])
        self.assertEqual(self.commands(1), [{"ch": 0, "mode": 0}])
        self.assertGreaterEqual(self.commands().count(4), 2)
        self.assertEqual(self.commands()[-1], 2)
        self.assertTrue(all(body["tok"] == FakeCanMonitor.TOKEN for body in self.commands(4) + self.commands(2)))
        self.assertIn("session 1234abcd on port 0 (tap, classical CAN, 500000 bit/s), lease 5000 ms", out)
        self.assertIn("ended: closed", out)
        self.assertIn("3 frames", out)
        self.assertEqual(log.read_text(), "(0000000001.000100) can0 123#0001020304050607 R\n"
                                          "(0000000001.000050) can0 18DA00F1#01 R\n"
                                          "(0000000001.000051) can0 100#02 T\n")

    def test_synthetic_frames_are_counted_against_the_gaps(self) -> None:
        frames = [frame_record(10, number % 4, number.to_bytes(8, "little")) for number in range(20)]
        self.can.batches = [frames[:5] + [other_record(4, leb(3) + bytes([1]))] + frames[8:]]
        synthetic = {"fps": 1000, "ids": 4, "dlc": 8}
        out, _ = self.command(client.command_can_monitor, port=None, synthetic=synthetic)
        self.assertEqual(self.commands(1), [{"synthetic": synthetic}])
        self.assertIn("on the synthetic source", out)
        self.assertIn("gap: 3 frames lost (ring)", out)
        self.assertIn("counter check (every frame): 17 frames, 3 missing in 1 hole, 0 repeated, 0 not the generator's; "
                      "the gap records count 3 frames lost (all of them)", out)

    def test_frames_lost_after_the_last_one_received_are_told_apart(self) -> None:
        frames = [frame_record(10, number % 4, number.to_bytes(8, "little")) for number in range(10)]
        gap = [other_record(4, leb(2) + bytes([1]))]
        self.can.batches = [frames[:5] + gap + frames[7:] + [other_record(4, leb(6) + bytes([1]))]]
        out, _ = self.command(client.command_can_monitor, port=None, synthetic={"fps": 1000, "ids": 4, "dlc": 8})
        self.assertIn("8 frames, 2 missing in 1 hole, 0 repeated, 0 not the generator's; the gap records count 8 frames "
                      "lost, 6 of them after the last frame received", out)

    def test_missing_frames_without_a_gap_record_never_reached_the_unit(self) -> None:
        self.can.batches = [[frame_record(10, 0x123, number.to_bytes(8, "little")) for number in (0, 1, 2, 5, 6)]]
        out, _ = self.command(client.command_can_monitor, expect=0x123)
        self.assertIn("5 frames, 2 missing in 1 hole", out)
        self.assertIn("2 of the missing frames have no gap record: the unit never received them", out)

    def test_holes_are_counted_beside_the_controller_overruns(self) -> None:
        def frames(numbers):
            return [frame_record(10, number % 4, number.to_bytes(8, "little")) for number in numbers]

        # The gap record comes before frames the controller still held: an overrun is counted after them.
        overrun = [other_record(4, leb(1) + bytes([3]))]
        self.can.batches = [frames(range(3)) + overrun + frames(range(3, 5)) + frames(range(10, 15))
                            + frames(range(18, 20))]
        out, _ = self.command(client.command_can_monitor, port=None, synthetic={"fps": 1000, "ids": 4, "dlc": 8})
        self.assertIn("12 frames, 8 missing in 2 holes, 0 repeated", out)
        self.assertIn("controller: 1 overrun, each counted once however many frames it dropped; the holes hold 4.0 "
                      "frames on average", out)

    def test_gaps_are_summed_until_the_next_statistics_line(self) -> None:
        self.can.batches = [[other_record(4, leb(2) + bytes([1])), other_record(4, leb(3) + bytes([1])),
                             stats_record([0] * 13)]]
        out, _ = self.command(client.command_can_monitor)
        self.assertIn("gap: 5 frames lost in 2 gaps (ring)\n", out)
        self.assertLess(out.index("gap: 5"), out.index("rx/s"))
        self.assertNotIn("gap: 2 frames", out)

    def test_refusals_and_missing_parts_are_named(self) -> None:
        self.can.refuse_open = 4
        with self.assertRaisesRegex(client.SmpError, "BUSY"):
            self.command(client.command_can_monitor)
        self.can.refuse_open = None
        self.can.tx = False
        with self.assertRaisesRegex(RuntimeError, "no CAN monitor transmit"):
            self.command(client.command_can_send, frames=[client.parse_can_frame("123#00")])
        self.fake.has_bus = False
        with self.assertRaisesRegex(RuntimeError, "no bus characteristic"):
            self.command(client.command_can_monitor)

    def test_a_port_the_unit_does_not_have(self) -> None:
        with self.assertRaisesRegex(RuntimeError, "no CAN port 3; it has 0"):
            self.command(client.command_can_monitor, port=3)

    def test_a_generator_at_another_bitrate_is_refused_before_open(self) -> None:
        with self.assertRaisesRegex(RuntimeError, "port 0 runs at 500000 bit/s, the adapter at 1000000 bit/s"):
            self.command(client.command_can_monitor, generate=100, bitrate=1000000, fd=False, data_bitrate=0)
        self.assertEqual(self.commands(1), [])

    def test_generated_frames_before_the_first_one_received_are_told(self) -> None:
        class Generator:
            def __init__(self, *args, **kwargs) -> None:
                pass

            def start(self) -> None:
                pass

            @staticmethod
            def stop() -> str:
                return "generator: 9 frames"

        self.can.batches = [[frame_record(10, 0x123, number.to_bytes(8, "little")) for number in range(3, 9)]]
        with mock.patch.object(client, "open_can_bus", lambda args: contextlib.nullcontext()), \
                mock.patch.object(client, "CanGenerator", Generator):
            out, _ = self.command(client.command_can_monitor, generate=100, bitrate=500000, fd=False, data_bitrate=0,
                                  interface="slcan", channel="COM6")
        self.assertIn("counter check (ID 123): 6 frames, 0 missing, 0 repeated", out)
        self.assertIn("the first frame received was number 3: the 3 sent before it are missing too", out)

    def test_auto_mode_follows_the_configuration(self) -> None:
        self.can.ports[0]["configured"] = False
        self.command(client.command_can_monitor, port_bitrate=0)
        self.command(client.command_can_periodic, frames=[client.can_periodic_argument("123#00@10")], duration=0.01)
        self.assertEqual(self.commands(1), [{"ch": 0, "mode": 1, "bitrate": 0}, {"ch": 0, "mode": 2}])

    def test_send_arms_first_and_sends_16_frames_a_request(self) -> None:
        frames = [client.parse_can_frame(f"{0x100 + number:03X}#{number:02X}") for number in range(20)]
        out, _ = self.command(client.command_can_send, frames=frames, duration=0.02)
        self.assertLess(self.commands().index(5), self.commands().index(6))
        self.assertEqual([len(body["frames"]) for body in self.commands(6)], [16, 4])
        self.assertEqual(self.commands(6)[0]["frames"][0], [0x100, 0, b"\x00"])
        self.assertIn("armed: up to 50 % of the bus time; reserved IDs 700-763", out)
        self.assertIn("sent 20 of 20 frames", out)
        self.assertIn("TX         113#13", out)

    def test_periodic_frames_and_their_stop(self) -> None:
        specs = [client.can_periodic_argument("123#11@100"), client.can_periodic_argument("18DA00F1#2233@10x5")]
        self.command(client.command_can_periodic, frames=specs, duration=0.03)
        lists = [body["list"] for body in self.commands(7)]
        self.assertEqual(lists[0], [{"slot": 0, "id": 0x123, "flags": 0, "data": b"\x11", "period_ms": 100},
                                    {"slot": 1, "id": 0x18DA00F1, "flags": 1, "data": b"\x22\x33", "period_ms": 10,
                                     "count": 5}])
        self.assertEqual(lists[-1], [])

    def test_replay_keeps_the_buffer_filled_and_waits_for_the_end(self) -> None:
        lines = [f"(1700000000.{number * 10000:06d}) can0 {0x200 + number % 3:03X}#{number:016X} "
                 f"{'T' if number == 7 else 'R'}" for number in range(40)]
        path = self.tmp / "capture.log"
        path.write_text("\n".join(lines) + "\n")
        self.can.capacity = 100
        self.can.no_room_once = True
        out, _ = self.command(client.command_can_replay, file=path, speed=200, loop=False, rx_only=True, duration=5.0)
        replay = self.commands(8)
        self.assertEqual((replay[0]["op"], replay[0]["speed_pct"], replay[0]["loop"]), (0, 200, False))
        self.assertTrue(all(len(body["data"]) <= 100 for body in replay if body["op"] == 1))
        # The last chunk of a seq is the one the unit took (the first was refused for room).
        accepted = {body["seq"]: body["data"] for body in replay if body["op"] == 1}
        self.assertEqual(sorted(accepted), list(range(len(accepted))))
        self.assertEqual(replay[-1]["op"], 3)
        data = b"".join(accepted[seq] for seq in sorted(accepted))
        records = client.CanStreamDecoder().decode(struct.pack("<BBHQB", 1, 0, 0, 0, 39) + data)
        numbers = [number for number in range(40) if number != 7]
        self.assertEqual([record.can_id for record in records], [0x200 + number % 3 for number in numbers])
        self.assertEqual([int.from_bytes(record.data, "big") for record in records], numbers)
        self.assertEqual(records[7].time_us - records[6].time_us, 20000)
        self.assertIn("replaying 39 frames, 0.4 s of capture, at 200 %", out)
        self.assertIn("tx: replay done", out)

    def test_replay_chunks_go_a_window_at_a_time(self) -> None:
        class DeferredLink(FakeLink):
            """Answers only when the client reads, so the requests sent before a read were in flight together."""

            def __init__(self, unit: FakeUnit, window: int) -> None:
                super().__init__(unit, window)
                self.waiting: collections.deque[bytes] = collections.deque()
                self.most_in_flight = 0

            def send(self, packet: bytes) -> None:
                self.waiting.append(packet)
                self.most_in_flight = max(self.most_in_flight, len(self.waiting))

            def receive(self, timeout_s: float) -> bytes | None:
                while self.waiting:
                    self.inbox.extend(self.unit.handle(self.waiting.popleft()))
                return super().receive(timeout_s)

        link = DeferredLink(self.unit, 3)
        session = client.CanMonitorSession(client.SmpClient(link, 1.0, 0), queue.Queue(), client.CanStreamReport())
        self.can.listening, self.can.capacity = True, 10_000
        session.open({"ch": 0, "mode": 2})
        session.call(client.CanMonitorCommand.ARM, {"on": True})
        room = session.call(client.CanMonitorCommand.REPLAY, {"op": 0, "speed_pct": 100, "loop": False})["room"]
        # The first chunk finds no room, and the two behind it are out of order: the feeder starts again from it.
        self.can.no_room_once = True
        records = [client.encode_can_frame_record(10, client.parse_can_frame(f"123#{number:016X}"))
                   for number in range(60)]
        feeder = client.CanReplayFeeder(session, records, 100, room, window=3)
        while not feeder.finished:
            feeder.feed()
        self.assertEqual(link.most_in_flight, 3)
        chunks = [body for body in self.commands(8) if body["op"] == 1]
        self.assertEqual([body["seq"] for body in chunks[:3]], [0, 1, 2])
        self.assertTrue(all(len(body["data"]) <= 100 for body in chunks))
        accepted = {body["seq"]: body["data"] for body in chunks}
        self.assertEqual(sorted(accepted), list(range(len(accepted))))
        self.assertEqual(b"".join(accepted[seq] for seq in sorted(accepted)), b"".join(records))
        self.assertEqual(self.commands(8)[-1]["op"], 3)

    def test_ports(self) -> None:
        out, _ = run(client.command_can_ports, client.SmpClient(FakeLink(self.unit), 0.1, 0))
        self.assertIn("notifications up to 0 B; firmware has tx, synthetic", out)
        self.assertIn("port 0: classical CAN and CAN FD; configured; error-active; 500000 bit/s; free", out)
        out, _ = self.command(client.command_can_ports)
        self.assertIn("notifications up to 495 B", out)
        self.unit.can = None
        with self.assertRaisesRegex(client.SmpError, "no CAN monitor"):
            run(client.command_can_ports, client.SmpClient(FakeLink(self.unit), 0.1, 0))


class CanGeneratorTest(unittest.TestCase):
    def test_numbered_frames_at_the_rate(self) -> None:
        sent = []

        class Bus:
            @staticmethod
            def send(message, timeout=None) -> None:
                sent.append(message)

            @staticmethod
            def recv(timeout=None):
                return None

        can = types.ModuleType("can")
        can.Message = lambda **fields: types.SimpleNamespace(**fields)
        can.CanError = type("CanError", (Exception,), {})
        with mock.patch.dict(sys.modules, {"can": can}):
            generator = client.CanGenerator(Bus(), 1000, 0x18DA00F1)
            generator.start()
            time.sleep(0.2)
            summary = generator.stop()
        self.assertTrue(100 <= len(sent) <= 400, len(sent))
        self.assertEqual([message.data for message in sent[:3]], [client.generated_payload(n, 8) for n in range(3)])
        self.assertEqual((sent[0].arbitration_id, sent[0].is_extended_id, sent[0].is_fd), (0x18DA00F1, True, False))
        self.assertIn(f"{len(sent)} frames of ID 18DA00F1 handed to the adapter", summary)

    def test_a_coarse_sleep_keeps_the_rate(self) -> None:
        sent = []

        class Bus:
            @staticmethod
            def send(message, timeout=None) -> None:
                sent.append(message)

            @staticmethod
            def recv(timeout=None):
                return None

        can = types.ModuleType("can")
        can.Message = lambda **fields: types.SimpleNamespace(**fields)
        can.CanError = type("CanError", (Exception,), {})
        # 64 frames a 30 ms pass would be about 2,100 a second.
        with mock.patch.dict(sys.modules, {"can": can}), mock.patch.object(client, "CAN_GENERATOR_PAUSE_S", 0.03):
            generator = client.CanGenerator(Bus(), 4000, 0x123)
            generator.start()
            time.sleep(0.3)
            generator.stop()
        self.assertGreater(len(sent) / generator.elapsed_s, 3000)


class ArgumentTest(unittest.TestCase):
    def parse(self, *argv: str) -> argparse.Namespace:
        with contextlib.redirect_stderr(io.StringIO()) as err:
            try:
                return client.parse_args(list(argv))
            except SystemExit:
                raise AssertionError(err.getvalue()) from None

    def rejected(self, *argv: str) -> str:
        with contextlib.redirect_stderr(io.StringIO()) as err, self.assertRaises(SystemExit):
            client.parse_args(list(argv))
        return err.getvalue()

    def test_transports_exclude_each_other(self) -> None:
        self.assertIn("exclude each other", self.rejected("--ble", "x", "--serial", "/dev/null", "info"))
        self.assertIn("--target is for the CAN", self.rejected("--ble", "x", "--target", "1", "info"))
        self.assertIn("need --ble", self.rejected("--route", "3", "--serial", "/dev/null", "info"))
        self.assertIn("--route must be", self.rejected("--ble", "x", "--route", "255", "info"))
        self.assertIn("shell needs --serial", self.rejected("--ble", "x", "shell", "ls"))

    def test_ble_defaults(self) -> None:
        args = self.parse("--ble", "EerieLeap", "image", "list")
        self.assertEqual((args.ble, args.route), ("EerieLeap", 0))
        self.assertEqual(self.parse("--ble", "x", "--route", "5", "info").route, 5)

    def test_image_info_is_offline(self) -> None:
        args = self.parse("image", "info", "file.bin")
        self.assertTrue(client.is_offline(args))
        self.assertFalse(client.is_offline(self.parse("--serial", "/dev/null", "image", "list")))

    def test_kinds_print_the_pixel_format_of_a_raw_image_kind(self) -> None:
        entry = {"kind": 0, "name": "images", "dir": "ui_assets/images", "access": 7, "max": 4096,
                 "ext": [".bin", ".svg"], "avail": True}
        self.assertNotIn("fmt", client.format_kind(entry))
        self.assertIn("avail yes  fmt RGB565A8 (0x14)", client.format_kind(entry | {"fmt": 0x14}))
        self.assertIn("fmt AL88 (0x15)", client.format_kind(entry | {"fmt": 0x15, "free": 1, "used": 2}))
        self.assertIn("fmt 0x2a", client.format_kind(entry | {"fmt": 0x2A}))

    def test_get_options(self) -> None:
        args = self.parse("--serial", "/dev/null", "assets", "get", "logs", "a.mf4")
        self.assertEqual((args.growing, args.follow, args.interval), (False, False, None))
        args = self.parse("--serial", "/dev/null", "assets", "get", "logs", "a.mf4", "--growing")
        self.assertEqual((args.growing, args.follow), (True, False))
        args = self.parse("--serial", "/dev/null", "assets", "get", "logs", "a.mf4", "--follow")
        self.assertEqual((args.growing, args.follow, args.interval), (True, True, client.DEFAULT_FOLLOW_INTERVAL_S))
        args = self.parse("--serial", "/dev/null", "assets", "get", "logs", "a.mf4", "--follow", "--interval", "0.5")
        self.assertEqual(args.interval, 0.5)
        self.assertIn("--interval needs --follow",
                      self.rejected("--serial", "/dev/null", "assets", "get", "logs", "a", "--interval", "1"))
        self.assertIn("cannot --resume",
                      self.rejected("--serial", "/dev/null", "assets", "get", "logs", "a", "--follow", "--resume"))

    def test_time_commands(self) -> None:
        self.assertIsNone(self.parse("--serial", "/dev/null", "time").time_command)
        args = self.parse("--serial", "/dev/null", "time", "set")
        self.assertEqual((args.time_command, args.wall_ms), ("set", None))
        self.assertEqual(self.parse("--serial", "/dev/null", "time", "set", "--wall-ms", "5").wall_ms, 5)

    def test_update_options(self) -> None:
        args = self.parse("--serial", "/dev/null", "image", "update", "f.bin", "--no-reset", "--timeout-s", "120")
        self.assertEqual((args.no_reset, args.timeout_s, args.in_flight), (True, 120.0, None))
        self.assertIn("--in-flight must be",
                      self.rejected("--serial", "/dev/null", "image", "upload", "f", "--in-flight", "0"))
        args = self.parse("--serial", "/dev/null", "image", "test", "ab" * 32)
        self.assertEqual(args.hash, bytes.fromhex("ab" * 32))

    def test_can_commands(self) -> None:
        args = self.parse("--ble", "x", "can", "monitor", "0", "--filter", "123/7FF", "--filter", "18DA0000/1FFF0000",
                          "--filter", "10/7F0/ext")
        self.assertEqual(args.filter, [[0x123, 0x7FF, False], [0x18DA0000, 0x1FFF0000, True], [0x10, 0x7F0, True]])
        self.assertEqual(self.parse("--ble", "x", "can", "monitor", "--synthetic", "5000:4:15:fd").synthetic,
                         {"fps": 5000, "ids": 4, "dlc": 15, "fd": True})
        self.assertIn("needs --ble", self.rejected("--serial", "/dev/null", "can", "monitor", "0"))
        self.assertIn("leave out --route", self.rejected("--ble", "x", "--route", "3", "can", "send", "0", "123#00"))
        self.assertIn("a PORT or --synthetic",
                      self.rejected("--ble", "x", "can", "monitor", "0", "--synthetic", "1:1:1"))
        self.assertIn("--speed must be", self.rejected("--ble", "x", "can", "replay", "0", "f.log", "--speed", "0"))
        self.assertIn("ID/MASK", self.rejected("--ble", "x", "can", "monitor", "0", "--filter", "123"))
        generate = self.parse("--interface", "slcan", "--channel", "/dev/ttyACM1", "can", "generate", "4000")
        self.assertTrue(client.is_offline(generate))
        self.assertEqual((generate.fps, generate.id, generate.length), (4000, 0x123, 8))
        self.assertIn("--length", self.rejected("can", "generate", "100", "--length", "9"))
        self.assertEqual(self.parse("--serial", "/dev/null", "can", "ports").can_command, "ports")


if __name__ == "__main__":
    unittest.main()
