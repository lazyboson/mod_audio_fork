#!/usr/bin/env python3
"""Controllable WebSocket server for the mod_audio_fork rig.

Standard library only, so it runs anywhere FreeSWITCH does. Accepts fork
connections, validates the wire protocol, and reports what it received. Fault
injection (stall, drop, burst) is driven by env vars so compose files and CI can
script scenarios without editing code.

  MOCK_WS_PORT        listen port (default 9099)
  MOCK_WS_STALL_AFTER stop reading after N binary frames (exercises backpressure)
  MOCK_WS_DROP_AFTER  close the connection after N binary frames
  MOCK_WS_PLAYBACK_MS after hello, stream N ms of 16 kHz mono L16 tone back at
                      real-time pace, then a "rig-end" mark
  MOCK_WS_REPORT      path to write a JSON report per connection
  MOCK_WS_TLS_CERT    server certificate (PEM); set with MOCK_WS_TLS_KEY to
  MOCK_WS_TLS_KEY     serve wss:// instead of ws://
"""
from __future__ import annotations

import array
import base64
import hashlib
import json
import math
import os
import socket
import ssl
import struct
import sys
import threading
import time

GUID = "258EAFA5-E914-47DA-95CA-C5AB0DC85B11"
OPCODE_TEXT = 0x1
OPCODE_BINARY = 0x2
OPCODE_CLOSE = 0x8
OPCODE_PING = 0x9
OPCODE_PONG = 0xA

PLAYBACK_RATE = 16000
PLAYBACK_CHUNK_MS = 20
PLAYBACK_TONE_HZ = 400
PLAYBACK_AMPLITUDE = 12000


def _recv_exact(conn: socket.socket, count: int) -> bytes:
    chunks = []
    remaining = count
    while remaining > 0:
        chunk = conn.recv(remaining)
        if not chunk:
            raise ConnectionError("peer closed mid-frame")
        chunks.append(chunk)
        remaining -= len(chunk)
    return b"".join(chunks)


def _handshake(conn: socket.socket) -> None:
    request = b""
    while b"\r\n\r\n" not in request:
        chunk = conn.recv(4096)
        if not chunk:
            raise ConnectionError("peer closed during handshake")
        request += chunk
    key = ""
    for line in request.decode("latin-1").split("\r\n"):
        if line.lower().startswith("sec-websocket-key:"):
            key = line.split(":", 1)[1].strip()
    if not key:
        raise ConnectionError("no Sec-WebSocket-Key")
    accept = base64.b64encode(hashlib.sha1((key + GUID).encode()).digest()).decode()
    conn.sendall(
        b"HTTP/1.1 101 Switching Protocols\r\n"
        b"Upgrade: websocket\r\n"
        b"Connection: Upgrade\r\n"
        b"Sec-WebSocket-Accept: " + accept.encode() + b"\r\n"
        b"Sec-WebSocket-Protocol: audiofork\r\n\r\n"
    )


def _read_frame(conn: socket.socket) -> tuple[int, bytes]:
    header = _recv_exact(conn, 2)
    opcode = header[0] & 0x0F
    masked = bool(header[1] & 0x80)
    length = header[1] & 0x7F
    if length == 126:
        length = struct.unpack("!H", _recv_exact(conn, 2))[0]
    elif length == 127:
        length = struct.unpack("!Q", _recv_exact(conn, 8))[0]
    mask = _recv_exact(conn, 4) if masked else b""
    payload = _recv_exact(conn, length) if length else b""
    if masked:
        payload = bytes(byte ^ mask[i % 4] for i, byte in enumerate(payload))
    return opcode, payload


def _send_frame(conn: socket.socket, opcode: int, payload: bytes = b"") -> None:
    header = bytearray([0x80 | opcode])
    length = len(payload)
    if length < 126:
        header.append(length)
    elif length < (1 << 16):
        header.append(126)
        header += struct.pack("!H", length)
    else:
        header.append(127)
        header += struct.pack("!Q", length)
    conn.sendall(bytes(header) + payload)


def _tone_chunk() -> bytes:
    # 400 Hz at 16 kHz is 40 samples per period and 320 samples per 20 ms chunk,
    # so repeating one chunk is a continuous tone with no phase discontinuity.
    samples = PLAYBACK_RATE * PLAYBACK_CHUNK_MS // 1000
    pcm = array.array(
        "h",
        (
            int(PLAYBACK_AMPLITUDE * math.sin(2 * math.pi * PLAYBACK_TONE_HZ * i / PLAYBACK_RATE))
            for i in range(samples)
        ),
    )
    return pcm.tobytes()


TONE_CHUNK = _tone_chunk()


def _min_of(values: list[int]) -> int:
    return min(values) if values else 0


def _max_of(values: list[int]) -> int:
    return max(values) if values else 0


class Report:
    def __init__(self) -> None:
        self.lock = threading.Lock()
        self.connections = 0
        self.hello_count = 0
        self.resume_count = 0
        self.bye_count = 0
        self.audio_bytes = 0
        self.audio_frames = 0
        self.playback_bytes_sent = 0
        self.protocol_errors: list[str] = []
        self.hello_sids: list[str] = []
        self.bye_sids: list[str] = []
        self.first_hello: dict | None = None
        self.per_connection: list[dict] = []

    def new_connection(self) -> dict:
        stats = {
            "audio_bytes": 0,
            "first_frame_bytes": 0,
            "first_frame_ms": 0,
            "last_frame_ms": 0,
            "abs_sum": 0,
            "sample_count": 0,
            "playback_bytes": 0,
        }
        with self.lock:
            self.connections += 1
            self.per_connection.append(stats)
        return stats

    def snapshot(self) -> dict:
        with self.lock:
            audio_bytes = [c["audio_bytes"] for c in self.per_connection]
            playback_bytes = [c["playback_bytes"] for c in self.per_connection]
            spans = [c["last_frame_ms"] - c["first_frame_ms"] for c in self.per_connection]
            # The first frame is however much the module had coalesced when the
            # connection opened, so it is excluded from the rate it defines.
            rates = [
                (c["audio_bytes"] - c["first_frame_bytes"]) * 1000
                // (c["last_frame_ms"] - c["first_frame_ms"])
                for c in self.per_connection
                if c["last_frame_ms"] > c["first_frame_ms"]
            ]
            mean_abs = [
                c["abs_sum"] // c["sample_count"]
                for c in self.per_connection
                if c["sample_count"] > 0
            ]
            return {
                "connections": self.connections,
                "hello_count": self.hello_count,
                "resume_count": self.resume_count,
                "bye_count": self.bye_count,
                "audio_bytes": self.audio_bytes,
                "audio_frames": self.audio_frames,
                "audio_bytes_min": _min_of(audio_bytes),
                "audio_bytes_max": _max_of(audio_bytes),
                "audio_span_ms_min": _min_of(spans),
                "audio_rate_min": _min_of(rates),
                "audio_rate_max": _max_of(rates),
                "audio_mean_abs_min": _min_of(mean_abs),
                "audio_mean_abs_max": _max_of(mean_abs),
                "playback_bytes_sent": self.playback_bytes_sent,
                "playback_bytes_min": _min_of(playback_bytes),
                "protocol_errors": list(self.protocol_errors),
                "hellos": list(self.hello_sids),
                "byes": list(self.bye_sids),
                "first_hello": self.first_hello,
            }


REPORT = Report()
STALL_AFTER = int(os.environ.get("MOCK_WS_STALL_AFTER", "0"))
DROP_AFTER = int(os.environ.get("MOCK_WS_DROP_AFTER", "0"))
PLAYBACK_MS = int(os.environ.get("MOCK_WS_PLAYBACK_MS", "0"))
REPORT_PATH = os.environ.get("MOCK_WS_REPORT", "")
TLS_CERT = os.environ.get("MOCK_WS_TLS_CERT", "")
TLS_KEY = os.environ.get("MOCK_WS_TLS_KEY", "")
REPORT_WRITE_LOCK = threading.Lock()


def _write_report() -> None:
    if not REPORT_PATH:
        return
    payload = json.dumps(REPORT.snapshot(), separators=(",", ":"))
    temp = REPORT_PATH + ".tmp"
    # Connections close together at teardown, so the write is serialized and
    # renamed into place: a half-written report would fail the smoke spuriously.
    with REPORT_WRITE_LOCK:
        with open(temp, "w", encoding="utf-8") as handle:
            handle.write(payload)
        os.replace(temp, REPORT_PATH)


def _play(conn: socket.socket, send_lock: threading.Lock, stats: dict, stop: threading.Event) -> None:
    deadline = time.monotonic()
    try:
        for _ in range(PLAYBACK_MS // PLAYBACK_CHUNK_MS):
            if stop.is_set():
                return
            with send_lock:
                _send_frame(conn, OPCODE_BINARY, TONE_CHUNK)
            with REPORT.lock:
                stats["playback_bytes"] += len(TONE_CHUNK)
                REPORT.playback_bytes_sent += len(TONE_CHUNK)
            deadline += PLAYBACK_CHUNK_MS / 1000
            time.sleep(max(0.0, deadline - time.monotonic()))
        if stop.is_set():
            return
        with send_lock:
            _send_frame(
                conn,
                OPCODE_TEXT,
                json.dumps({"type": "mark", "name": "rig-end"}).encode("utf-8"),
            )
    except (ConnectionError, socket.timeout, OSError):
        return


def _handle(conn: socket.socket, tls: ssl.SSLContext | None) -> None:
    conn.settimeout(30)
    frames = 0
    saw_hello = False
    call_sid = ""
    send_lock = threading.Lock()
    stop = threading.Event()
    stats = None
    try:
        if tls is not None:
            conn = tls.wrap_socket(conn, server_side=True)
        _handshake(conn)
        stats = REPORT.new_connection()
        while True:
            opcode, payload = _read_frame(conn)
            if opcode == OPCODE_CLOSE:
                return
            if opcode == OPCODE_PING:
                with send_lock:
                    _send_frame(conn, OPCODE_PONG, payload)
                continue
            if opcode == OPCODE_TEXT:
                try:
                    message = json.loads(payload.decode("utf-8"))
                except ValueError:
                    with REPORT.lock:
                        REPORT.protocol_errors.append("text frame was not JSON")
                    continue
                kind = message.get("type")
                with REPORT.lock:
                    if kind == "hello":
                        REPORT.hello_count += 1
                        saw_hello = True
                        call_sid = str(message.get("callSid", ""))
                        REPORT.hello_sids.append(call_sid)
                        if REPORT.first_hello is None:
                            REPORT.first_hello = message
                        for field in ("version", "callSid", "rate", "channels", "encoding"):
                            if field not in message:
                                REPORT.protocol_errors.append(f"hello missing {field}")
                    elif kind == "resume":
                        REPORT.resume_count += 1
                    elif kind == "bye":
                        REPORT.bye_count += 1
                        # attributed to the connection's hello: bye carries no id
                        REPORT.bye_sids.append(call_sid)
                if kind == "hello" and PLAYBACK_MS > 0:
                    threading.Thread(
                        target=_play, args=(conn, send_lock, stats, stop), daemon=True
                    ).start()
                continue
            if opcode == OPCODE_BINARY:
                frames += 1
                now_ms = int(time.monotonic() * 1000)
                samples = array.array("h")
                samples.frombytes(payload[: len(payload) - len(payload) % 2])
                with REPORT.lock:
                    if not saw_hello:
                        REPORT.protocol_errors.append("audio arrived before hello")
                    REPORT.audio_bytes += len(payload)
                    REPORT.audio_frames += 1
                    if len(payload) % 2 != 0:
                        REPORT.protocol_errors.append("audio frame is not 16-bit aligned")
                    stats["audio_bytes"] += len(payload)
                    stats["abs_sum"] += sum(map(abs, samples))
                    stats["sample_count"] += len(samples)
                    if frames == 1:
                        stats["first_frame_bytes"] = len(payload)
                        stats["first_frame_ms"] = now_ms
                    stats["last_frame_ms"] = now_ms
                if DROP_AFTER and frames >= DROP_AFTER:
                    return
                if STALL_AFTER and frames >= STALL_AFTER:
                    # hold the socket open without reading: the module must
                    # buffer, then drop oldest, and never wedge the call
                    time.sleep(3600)
    except (ConnectionError, socket.timeout, OSError):
        return
    finally:
        stop.set()
        try:
            conn.close()
        except OSError:
            pass
        if stats is not None:
            _write_report()


def main() -> int:
    port = int(os.environ.get("MOCK_WS_PORT", "9099"))
    tls = None
    if TLS_CERT and TLS_KEY:
        tls = ssl.SSLContext(ssl.PROTOCOL_TLS_SERVER)
        tls.load_cert_chain(TLS_CERT, TLS_KEY)
    listener = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
    listener.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
    listener.bind(("0.0.0.0", port))
    listener.listen(128)
    print(f"mock ws server listening on {port} ({'wss' if tls else 'ws'})", flush=True)
    try:
        while True:
            conn, _ = listener.accept()
            threading.Thread(target=_handle, args=(conn, tls), daemon=True).start()
    except KeyboardInterrupt:
        return 0
    finally:
        listener.close()


if __name__ == "__main__":
    sys.exit(main())
