"""Kiem thu hop dong TCP qua executable that; chi dung Python standard library.

Server bind loopback va port 0; moi process/file tam deu duoc thu hoi.
Chay qua make test hoac truyen --server/--client de thu ban sanitizer.
"""

import argparse
import concurrent.futures
import json
from pathlib import Path
import select
import socket
import struct
import subprocess
import tempfile
import threading
import time
import unittest


SERVER_BIN = Path("bin/ltm-server").resolve()
CLIENT_BIN = Path("bin/ltm-client").resolve()
MAX_PAYLOAD = 65536


def payload(text="Xin chào 😀", request_id="test-1", message_type="ECHO"):
    """Tao envelope UTF-8 compact, length-prefix duoc them rieng."""
    return json.dumps(
        {"version": 1, "type": message_type, "request_id": request_id, "data": {"text": text}},
        ensure_ascii=False,
        separators=(",", ":"),
    ).encode("utf-8")


def wire(data):
    """Tao frame big endian, khong them NUL."""
    return struct.pack("!I", len(data)) + data


def receive_exact(peer, length):
    """Tich luy so byte can doc; EOF som la loi test."""
    result = bytearray()
    while len(result) < length:
        chunk = peer.recv(length - len(result))
        if not chunk:
            raise AssertionError(f"Unexpected EOF after {len(result)}/{length} bytes")
        result.extend(chunk)
    return bytes(result)


def receive_frame(peer):
    """Doc mot frame, assert gioi han truoc khi doc payload."""
    length = struct.unpack("!I", receive_exact(peer, 4))[0]
    if not 1 <= length <= MAX_PAYLOAD:
        raise AssertionError(f"Invalid response length: {length}")
    return receive_exact(peer, length)


class ServerProcess:
    """Quan ly server executable va log rieng; luon dung process khi test ket thuc."""

    def __init__(self, *options):
        self.options = options
        self.process = None
        self.directory = None
        self.log_stream = None

    def start(self):
        self.directory = tempfile.TemporaryDirectory(prefix="ltm-phase1-")
        self.log_path = Path(self.directory.name) / "server.log"
        self.log_stream = self.log_path.open("w", encoding="utf-8")
        self.process = subprocess.Popen(
            [str(SERVER_BIN), "-p", "0", *self.options],
            stdout=subprocess.PIPE, stderr=self.log_stream, text=True,
        )
        if not select.select([self.process.stdout], [], [], 10)[0]:
            self.stop(validate=False)
            raise AssertionError("Server startup timeout")
        ready = self.process.stdout.readline().strip().split()
        if len(ready) != 3 or ready[0] != "LISTENING":
            logs = self.logs()
            self.stop(validate=False)
            raise AssertionError(f"Server startup failed: {ready}\n{logs}")
        self.port = int(ready[2])
        return self

    def connect(self, receive_buffer=None):
        peer = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
        if receive_buffer is not None:
            peer.setsockopt(socket.SOL_SOCKET, socket.SO_RCVBUF, receive_buffer)
        peer.settimeout(3)
        peer.connect(("127.0.0.1", self.port))
        return peer

    def logs(self):
        return self.log_path.read_text(encoding="utf-8", errors="replace")

    def stop(self, validate=True):
        logs = ""
        returncode = None
        if self.process is not None:
            if self.process.poll() is None:
                self.process.terminate()
                try:
                    self.process.wait(timeout=5)
                except subprocess.TimeoutExpired:
                    self.process.kill()
                    self.process.wait(timeout=5)
            returncode = self.process.returncode
            self.process.stdout.close()
            self.log_stream.close()
            logs = self.logs()
            self.directory.cleanup()
            self.process = None
        if validate and returncode is not None:
            if returncode != 0:
                raise AssertionError(f"Server exit {returncode}\n{logs}")
            if any(marker in logs for marker in ("AddressSanitizer", "LeakSanitizer", "runtime error:")):
                raise AssertionError(f"Sanitizer reported errors\n{logs}")

    def __enter__(self):
        return self.start()

    def __exit__(self, kind, value, traceback):
        self.stop(validate=kind is None)


class PhaseOneTests(unittest.TestCase):
    """Kiem thu thanh cong, frame/protocol loi va su co transport doc lap."""

    @classmethod
    def setUpClass(cls):
        cls.server = ServerProcess("-t", "1500", "-w", "1500").start()

    @classmethod
    def tearDownClass(cls):
        cls.server.stop()

    def assert_closed(self, peer):
        try:
            self.assertEqual(peer.recv(1), b"")
        except ConnectionResetError:
            pass

    def assert_healthy(self, server=None):
        server = server or self.server
        with server.connect() as peer:
            data = payload("healthy")
            peer.sendall(wire(data))
            self.assertEqual(receive_frame(peer), data)

    def test_fragmented_header_and_payload(self):
        with self.server.connect() as peer:
            data = payload()
            for byte in wire(data):
                peer.sendall(bytes([byte]))
            self.assertEqual(receive_frame(peer), data)

    def test_coalesced_frames_and_partial_tail(self):
        with self.server.connect() as peer:
            first, second, third = [payload(str(i), f"r-{i}") for i in range(3)]
            last = wire(third)
            peer.sendall(wire(first) + wire(second) + last[:7])
            self.assertEqual(receive_frame(peer), first)
            self.assertEqual(receive_frame(peer), second)
            peer.sendall(last[7:])
            self.assertEqual(receive_frame(peer), third)

    def test_exact_maximum_payload(self):
        data = payload("x" * (MAX_PAYLOAD - len(payload(""))))
        self.assertEqual(len(data), MAX_PAYLOAD)
        with self.server.connect() as peer:
            peer.sendall(wire(data))
            self.assertEqual(receive_frame(peer), data)

    def test_bad_length_disconnects_only_offender(self):
        for length in (0, MAX_PAYLOAD + 1, 0xFFFFFFFF):
            with self.subTest(length=length), self.server.connect() as peer:
                peer.sendall(struct.pack("!I", length))
                self.assert_closed(peer)
        self.assert_healthy()

    def test_invalid_protocol_and_recovery(self):
        cases = [b"{", b"{} trailing", b"[]", b"{}\x00", b"\xff",
                 payload().replace(b'"version":1', b'"version":2'),
                 payload().replace(b'"version":1', b'"version":01'),
                 payload().replace(b'"version":1', b'"version":1.'),
                 payload().replace(b'"version":1,', b'"version":1,\x01'),
                 payload().replace(b'"type":"ECHO"', b'"type":"ECHO","type":"SYS_PING"'),
                 payload().replace(b'"text":', b'"text":"\\u0000","ignored":')]
        with self.server.connect() as peer:
            for data in cases:
                with self.subTest(data=data[:30]):
                    peer.sendall(wire(data))
                    reply = json.loads(receive_frame(peer))
                    self.assertEqual(reply["type"], "SYS_ERROR")
                    self.assertEqual(reply["data"]["code"], "INVALID_MESSAGE")
            valid = payload("connection still aligned")
            peer.sendall(wire(valid))
            self.assertEqual(receive_frame(peer), valid)

    def test_ping_and_unknown_type(self):
        with self.server.connect() as peer:
            peer.sendall(wire(payload(message_type="SYS_PING")))
            reply = json.loads(receive_frame(peer))
            self.assertEqual((reply["type"], reply["request_id"]), ("SYS_PONG", "test-1"))
            peer.sendall(wire(payload(message_type="NOT_IMPLEMENTED")))
            reply = json.loads(receive_frame(peer))
            self.assertEqual((reply["data"]["code"], reply["request_id"]), ("UNKNOWN_TYPE", "test-1"))

    def test_half_close_flushes_all_replies(self):
        with self.server.connect() as peer:
            messages = [payload(f"half-close-{i}", f"half-{i}") for i in range(30)]
            peer.sendall(b"".join(map(wire, messages)))
            peer.shutdown(socket.SHUT_WR)
            self.assertEqual([receive_frame(peer) for _ in messages], messages)
            self.assert_closed(peer)

    def test_eof_mid_frame_preserves_prior_reply(self):
        for tail in (b"\0\0", struct.pack("!I", 40) + b"short"):
            with self.subTest(tail=tail), self.server.connect() as peer:
                valid = payload("before truncated frame")
                peer.sendall(wire(valid) + tail)
                peer.shutdown(socket.SHUT_WR)
                self.assertEqual(receive_frame(peer), valid)
                self.assert_closed(peer)
        self.assert_healthy()

    def test_reset_does_not_kill_server(self):
        peer = self.server.connect()
        peer.sendall(wire(payload("reset")))
        peer.setsockopt(socket.SOL_SOCKET, socket.SO_LINGER, struct.pack("ii", 1, 0))
        peer.close()
        self.assert_healthy()

    def test_sixteen_concurrent_clients(self):
        barrier = threading.Barrier(16)

        def chat(index):
            with self.server.connect() as peer:
                barrier.wait(timeout=5)
                for number in range(20):
                    data = payload(f"user {index}: tiếng Việt {number}", f"u{index}-{number}")
                    peer.sendall(wire(data))
                    if receive_frame(peer) != data:
                        raise AssertionError("Messages crossed connections")

        with concurrent.futures.ThreadPoolExecutor(max_workers=16) as workers:
            list(workers.map(chat, range(16)))

    def test_frame_deadline_and_slot_reuse(self):
        with ServerProcess("-t", "250") as server:
            with server.connect() as peer:
                peer.sendall(b"\0")
                self.assert_closed(peer)
            self.assertIn("reason=frame_timeout", server.logs())
            self.assert_healthy(server)

    def test_connection_capacity_and_reuse(self):
        with ServerProcess("-m", "2") as server:
            with server.connect() as first, server.connect() as second:
                for peer in (first, second):
                    data = payload("occupy slot")
                    peer.sendall(wire(data))
                    self.assertEqual(receive_frame(peer), data)
                with server.connect() as rejected:
                    self.assert_closed(rejected)
            deadline = time.monotonic() + 2
            while True:
                try:
                    self.assert_healthy(server)
                    break
                except (ConnectionError, AssertionError):
                    if time.monotonic() >= deadline:
                        raise
                    time.sleep(0.02)

    def test_slow_reader_queue_limit_keeps_other_clients_alive(self):
        with ServerProcess() as server, server.connect(receive_buffer=1024) as slow:
            packet = wire(payload("s" * 60000))

            def flood():
                try:
                    slow.sendall(packet * 256)
                except (ConnectionError, OSError):
                    pass

            with concurrent.futures.ThreadPoolExecutor(max_workers=1) as worker:
                future = worker.submit(flood)
                for _ in range(5):
                    self.assert_healthy(server)
                future.result(timeout=5)
            self.assertIn("reason=tx_queue_limit", server.logs())
            self.assert_healthy(server)

    def test_slow_reader_write_timeout(self):
        with ServerProcess("-w", "200") as server, server.connect(receive_buffer=1024) as slow:
            packet = wire(payload("w" * 60000))
            deadline = time.monotonic() + 7
            while time.monotonic() < deadline and "reason=write_timeout" not in server.logs():
                try:
                    slow.sendall(packet)
                except (ConnectionError, OSError):
                    break
                time.sleep(0.06)
            self.assertIn("reason=write_timeout", server.logs())
            self.assert_healthy(server)

    def test_cli_utf8_ping_and_interactive(self):
        base = [str(CLIENT_BIN), "-p", str(self.server.port)]
        result = subprocess.run(base + ["-m", "Xin chào Việt Nam 😀"],
                                capture_output=True, text=True, timeout=8)
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertEqual(json.loads(result.stdout)["data"]["text"], "Xin chào Việt Nam 😀")
        result = subprocess.run(base + ["-P"], capture_output=True, text=True, timeout=8)
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertEqual(json.loads(result.stdout)["type"], "SYS_PONG")
        result = subprocess.run(base, input="một\nhai\n/ping\n/quit\n",
                                capture_output=True, text=True, timeout=8)
        self.assertEqual(result.returncode, 0, result.stderr)
        replies = [json.loads(line) for line in result.stdout.splitlines()]
        self.assertEqual([row["type"] for row in replies], ["ECHO", "ECHO", "SYS_PONG"])
        self.assertEqual([row["data"]["text"] for row in replies[:2]], ["một", "hai"])


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--server", type=Path, default=SERVER_BIN)
    parser.add_argument("--client", type=Path, default=CLIENT_BIN)
    options, remaining = parser.parse_known_args()
    SERVER_BIN = options.server.resolve()
    CLIENT_BIN = options.client.resolve()
    unittest.main(argv=[__file__, *remaining], verbosity=2)
