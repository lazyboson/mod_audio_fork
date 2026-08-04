#!/usr/bin/env python3
"""Controllable WebSocket server for the mod_audio_fork rig.

Standard library only, so it runs anywhere FreeSWITCH does. Accepts fork
connections, validates the wire protocol, and reports what it received. Fault
injection (stall, drop, burst) is driven by env vars so compose files and CI can
script scenarios without editing code.

  MOCK_WS_PORT        listen port (default 9099)
  MOCK_WS_STALL_AFTER stop reading after N binary frames (exercises backpressure)
  MOCK_WS_DROP_AFTER  close the connection after N binary frames
  MOCK_WS_REPORT      path to write a JSON report per connection
"""
from __future__ import annotations

import base64
import hashlib
import json
import os
import socket
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


class Report:
    def __init__(self) -> None:
        self.lock = threading.Lock()
        self.connections = 0
        self.hello_count = 0
        self.resume_count = 0
        self.bye_count = 0
        self.audio_bytes = 0
        self.audio_frames = 0
        self.protocol_errors: list[str] = []
        self.first_hello: dict | None = None

    def snapshot(self) -> dict:
        with self.lock:
            return {
                "connections": self.connections,
                "hello_count": self.hello_count,
                "resume_count": self.resume_count,
                "bye_count": self.bye_count,
                "audio_bytes": self.audio_bytes,
                "audio_frames": self.audio_frames,
                "protocol_errors": list(self.protocol_errors),
                "first_hello": self.first_hello,
            }


REPORT = Report()
STALL_AFTER = int(os.environ.get("MOCK_WS_STALL_AFTER", "0"))
DROP_AFTER = int(os.environ.get("MOCK_WS_DROP_AFTER", "0"))
REPORT_PATH = os.environ.get("MOCK_WS_REPORT", "")


def _handle(conn: socket.socket) -> None:
    conn.settimeout(30)
    frames = 0
    saw_hello = False
    try:
        _handshake(conn)
        with REPORT.lock:
            REPORT.connections += 1
        while True:
            opcode, payload = _read_frame(conn)
            if opcode == OPCODE_CLOSE:
                return
            if opcode == OPCODE_PING:
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
                        if REPORT.first_hello is None:
                            REPORT.first_hello = message
                        for field in ("version", "callSid", "rate", "channels", "encoding"):
                            if field not in message:
                                REPORT.protocol_errors.append(f"hello missing {field}")
                    elif kind == "resume":
                        REPORT.resume_count += 1
                    elif kind == "bye":
                        REPORT.bye_count += 1
                continue
            if opcode == OPCODE_BINARY:
                frames += 1
                with REPORT.lock:
                    if not saw_hello:
                        REPORT.protocol_errors.append("audio arrived before hello")
                    REPORT.audio_bytes += len(payload)
                    REPORT.audio_frames += 1
                    if len(payload) % 2 != 0:
                        REPORT.protocol_errors.append("audio frame is not 16-bit aligned")
                if DROP_AFTER and frames >= DROP_AFTER:
                    return
                if STALL_AFTER and frames >= STALL_AFTER:
                    # hold the socket open without reading: the module must
                    # buffer, then drop oldest, and never wedge the call
                    time.sleep(3600)
    except (ConnectionError, socket.timeout, OSError):
        return
    finally:
        try:
            conn.close()
        except OSError:
            pass
        if REPORT_PATH:
            with open(REPORT_PATH, "w", encoding="utf-8") as handle:
                json.dump(REPORT.snapshot(), handle)


def main() -> int:
    port = int(os.environ.get("MOCK_WS_PORT", "9099"))
    listener = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
    listener.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
    listener.bind(("0.0.0.0", port))
    listener.listen(128)
    print(f"mock ws server listening on {port}", flush=True)
    try:
        while True:
            conn, _ = listener.accept()
            threading.Thread(target=_handle, args=(conn,), daemon=True).start()
    except KeyboardInterrupt:
        return 0
    finally:
        listener.close()


if __name__ == "__main__":
    sys.exit(main())
