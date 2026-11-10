"""FAV1 fixed-length bounded packets; an illegal header drops the connection, with no resynchronization."""

from __future__ import annotations

import json
import select
import socket
import struct
import time
from dataclasses import dataclass
from enum import IntEnum

HEADER = struct.Struct("!4sBBHIIII")
MAGIC = b"FAV1"
VERSION = 1
# Control packet payload limit, matching TV_CONTROL_MAX in firmware main/av_protocol.h; the channel list goes in a single CONFIG packet.
CONTROL_MAX = 24 * 1024
# Video packet limit; must match firmware AV_VIDEO_MAX. Taken from frames.VIDEO_MAX, not redefined.
from .frames import VIDEO_MAX
# One 40 ms block of ffmpeg output: 640 mono s16le samples.
AUDIO_PCM_BYTES = 1280
# On-wire size of that block: 4-byte IMA ADPCM header plus 640 nibbles (see adpcm.py); must match firmware AV_AUDIO_BYTES.
AUDIO_BYTES = 324
UINT32_MAX = 0xFFFFFFFF
IO_TIMEOUT = 0.25


class Kind(IntEnum):
    HELLO = 1
    CONFIG = 2
    PCM = 3
    # Picture: one index image cut into stripes. The name JPEG is kept to match the firmware value; the payload is not JPEG.
    JPEG = 4
    END = 5
    ERROR = 6
    # 256-color palette the indices point to; sent once before each channel's first frame.
    PALETTE = 7

# The only flag in use, for video only: this packet is a continuation stripe of the frame the previous packet started.
# Packets of one frame share a timestamp, so this flag is needed to tell them from repeated frames.
VIDEO_CONTINUES = 0x01


class ProtocolError(ValueError):
    """Illegal peer input; the message must not contain the peer's payload."""


@dataclass(frozen=True)
class Packet:
    kind: Kind
    session: int
    seq: int
    pts_ms: int
    payload: bytes = b""
    # Video only, and only VIDEO_CONTINUES; a plain integer, extra bits are caught by the range check.
    flags: int = 0

    def encode(self) -> bytes:
        validate_length(self.kind, len(self.payload))
        for value in (self.session, self.seq, self.pts_ms):
            if type(value) is not int or not 0 <= value <= UINT32_MAX:
                raise ProtocolError("integer out of range")
        if type(self.flags) is not int or not 0 <= self.flags <= 0xFF:
            raise ProtocolError("flags out of range")
        if self.flags and not (self.kind == Kind.JPEG and self.flags == VIDEO_CONTINUES):
            raise ProtocolError("flags are only for a continued video frame")
        return HEADER.pack(MAGIC, VERSION, self.kind, self.flags, self.session,
                           self.seq, self.pts_ms, len(self.payload)) + self.payload


def validate_length(kind: Kind, size: int) -> None:
    if kind in (Kind.HELLO, Kind.CONFIG, Kind.ERROR):
        valid = 0 < size <= CONTROL_MAX
    elif kind == Kind.PCM:
        valid = size == AUDIO_BYTES
    elif kind in (Kind.JPEG, Kind.PALETTE):
        valid = 0 < size <= VIDEO_MAX
    elif kind == Kind.END:
        valid = size == 0
    else:
        valid = False
    if not valid:
        raise ProtocolError("invalid payload length or type")


def json_bytes(value: dict) -> bytes:
    raw = json.dumps(value, separators=(",", ":"), ensure_ascii=True).encode("ascii")
    validate_length(Kind.CONFIG, len(raw))
    return raw


def json_object(raw: bytes) -> dict:
    def unique(pairs):
        result = {}
        for key, value in pairs:
            if key in result:
                raise ProtocolError("duplicate JSON key")
            result[key] = value
        return result

    def reject_constant(_):
        raise ProtocolError("nonstandard JSON constant")

    try:
        value = json.loads(raw.decode("utf-8"), object_pairs_hook=unique,
                           parse_constant=reject_constant)
    except (UnicodeError, ValueError, RecursionError):
        raise ProtocolError("invalid control JSON") from None
    if not isinstance(value, dict):
        raise ProtocolError("control JSON must be an object")
    return value


def _wait(sock: socket.socket, deadline: float, writable: bool = False) -> None:
    remaining = deadline - time.monotonic()
    if remaining <= 0:
        raise TimeoutError("packet deadline expired")
    ready = select.select([] if writable else [sock], [sock] if writable else [],
                          [], remaining)
    if not (ready[1] if writable else ready[0]):
        raise TimeoutError("packet deadline expired")


def _read_exact(sock: socket.socket, size: int, deadline: float) -> bytes:
    data = bytearray()
    while len(data) < size:
        _wait(sock, deadline)
        try:
            chunk = sock.recv(size - len(data))
        except BlockingIOError:
            continue
        if not chunk:
            raise EOFError("peer disconnected")
        data.extend(chunk)
    return bytes(data)


def receive_packet(sock: socket.socket, timeout: float = IO_TIMEOUT,
                   expected_session: int | None = None) -> Packet:
    """Header and payload share one overall deadline; length is validated before allocating a buffer."""
    deadline = time.monotonic() + timeout
    raw = _read_exact(sock, HEADER.size, deadline)
    magic, version, kind, flags, session, seq, pts, length = HEADER.unpack(raw)
    if magic != MAGIC or version != VERSION:
        raise ProtocolError("unsupported packet header")
    try:
        kind = Kind(kind)
    except ValueError:
        raise ProtocolError("unknown packet type") from None
    # Same as the device: only video packets may carry the single VIDEO_CONTINUES flag.
    if flags and not (kind == Kind.JPEG and flags == VIDEO_CONTINUES):
        raise ProtocolError("unsupported packet flags")
    validate_length(kind, length)
    if expected_session is not None and session != expected_session:
        raise ProtocolError("session mismatch")
    return Packet(kind, session, seq, pts, _read_exact(sock, length, deadline),
                  flags)


def send_packet(sock: socket.socket, packet: Packet,
                timeout: float = IO_TIMEOUT, deadline: float | None = None) -> None:
    """No application-layer send queue; writes at most one bounded packet at a time under one overall deadline.

    The socket is non-blocking. A timeout midway through a write is fatal; no ERROR may be appended after a partly written packet.
    `deadline` is shared by all packets of one frame, so the deadline binds the whole frame rather than each packet.
    """
    data = memoryview(packet.encode())
    if deadline is None:
        deadline = time.monotonic() + timeout
    while data:
        _wait(sock, deadline, writable=True)
        try:
            sent = sock.send(data)
        except BlockingIOError:
            continue
        if not sent:
            raise EOFError("peer disconnected")
        data = data[sent:]


class WritePacer:
    """Space video writes without accumulating credit while the socket stalls.

    This is a fixed byte rate, not an adaptive quality or frame-rate controller.
    Audio remains eligible between complete video packets.
    """

    def __init__(self, rate: int, chunk_bytes: int = 1200):
        if rate <= 0:
            raise ValueError("write pacing needs a positive byte rate")
        if not 1 <= chunk_bytes <= 16384:
            raise ValueError("write pacing chunk must be between 1 and 16384 bytes")
        self.rate = rate
        self.chunk_bytes = chunk_bytes
        self.next_at = 0.0

    def wait(self, deadline: float) -> bool:
        while True:
            now = time.monotonic()
            if now >= deadline:
                return False
            if now >= self.next_at:
                return True
            time.sleep(min(self.next_at, deadline) - now)

    def sent(self, size: int, started: float) -> None:
        self.next_at = started + size / self.rate


def _write_slice(sock: socket.socket, data: memoryview, deadline: float,
                 pacer: WritePacer | None = None) -> bool:
    """Write one slice of an encoded packet; return False if the deadline has passed.

    The caller encodes the packet once and writes it in several slices, so the same loop can still send audio while a large video packet goes out.
    A timeout returns False instead of raising; the caller decides what to do with the whole packet.
    """
    remaining = memoryview(data)
    while remaining:
        if pacer is not None and not pacer.wait(deadline):
            return False
        try:
            _wait(sock, deadline, writable=True)
        except TimeoutError:
            return False
        try:
            started = time.monotonic()
            sent = sock.send(remaining[:pacer.chunk_bytes] if pacer else remaining)
        except BlockingIOError:
            continue
        if not sent:
            raise EOFError("peer disconnected")
        if pacer is not None:
            pacer.sent(sent, started)
        remaining = remaining[sent:]
    return True
