#!/usr/bin/env python3
"""Self-test of the image, dfu and Bluetooth parts of smp_can_client.py, against simulated units.

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
            IMAGE_RESTART_GRACE_S=0.5)


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
        self.silent_after_reset = 4

    @staticmethod
    def _slot(version, content: bytes, **flags) -> dict:
        slot = dict(version=version, hash=hashlib.sha256(content).digest(), active=False, confirmed=False,
                    pending=False, permanent=False, filesha=b"")
        slot.update(flags)
        return slot

    def handle(self, packet: bytes) -> list[bytes]:
        if self.revert_next:
            self.revert_next = False
            self._revert()
        if self.silent_requests > 0:
            self.silent_requests -= 1
            return []

        length, group, sequence, command = client.unpack_header(packet)
        op = packet[0] & 7
        body = cbor2.loads(packet[8:8 + length]) if length else {}
        self.max_request = max(self.max_request, len(packet))

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
        if group == 1 and command == 0:
            return self._state(body) if op == 2 else self._images()
        if group == 1 and command == 5:
            if self.slots[1] is not None and self.slots[1]["pending"]:
                return self._error(1, 9)
            self.slots[1], self.upload = None, None
            return {}
        if group == 72 and command == 0 and self.has_dfu:
            return self._dfu_status()
        return {"rc": client.MGMT_ERR_ENOTSUP}

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
    """A stand-in for bleak: devices, a client, and an access point behind the smp characteristic."""

    def __init__(self, unit: FakeUnit, *, write_size: int = 30, notify_size: int = 23, window: int = 2,
                 max_packet: int = 512) -> None:
        self.unit, self.write_size, self.notify_size = unit, write_size, notify_size
        self.info = bytes([1]) + struct.pack("<H", max_packet) + bytes([window])
        self.writes: list[bytes] = []
        self.stream = bytearray()
        self.paired = 0
        self.connects = 0
        self.callback = None
        self.client = None
        self.scans: list[str] = []
        fake = self

        class BleakError(Exception):
            pass

        class Characteristic:
            max_write_without_response_size = fake.write_size

        class Services:
            def get_characteristic(self, uuid):
                return Characteristic() if uuid == client.BLE_SMP_UUID else None

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

    def test_update_options(self) -> None:
        args = self.parse("--serial", "/dev/null", "image", "update", "f.bin", "--no-reset", "--timeout-s", "120")
        self.assertEqual((args.no_reset, args.timeout_s, args.in_flight), (True, 120.0, None))
        self.assertIn("--in-flight must be",
                      self.rejected("--serial", "/dev/null", "image", "upload", "f", "--in-flight", "0"))
        args = self.parse("--serial", "/dev/null", "image", "test", "ab" * 32)
        self.assertEqual(args.hash, bytes.fromhex("ab" * 32))


if __name__ == "__main__":
    unittest.main()
