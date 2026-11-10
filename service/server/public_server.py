"""FoloTalk TV public entry: one listener, one AVServer session per device.

Every accepted connection gets its own session and sender thread. Optional shared
live encoding reuses only encoded media; authentication and wire clocks remain
per connection. The listener adds what a
public deployment needs and the LAN server deliberately lacks:

* Signed, short-lived viewing tickets instead of one shared token. The FoloTalk
  service issues them to authenticated devices; this process only verifies the
  HMAC with a key it shares with that service (TV_TICKET_KEY_FILE).
* A cap on concurrent connections (TV_MAX_SESSIONS), with an ERROR "busy" reply.
* A per-address cap so one client cannot take every slot.

Ticket format (ASCII): "<device>.<expires_unix>.<hex hmac-sha256 of device.expires>".
"""

from __future__ import annotations

import argparse
import hashlib
import hmac
import os
import select
import signal
import socket
import stat
import sys
import threading
import time
from pathlib import Path

from . import tv_server, quality
from .live import CHANNELS, DEFAULT_CHANNEL, LiveError
from .replay import Replay
from .protocol import (Kind, Packet, ProtocolError, json_bytes, json_object,
                       receive_packet, send_packet)

MAX_SESSIONS = int(os.environ.get("TV_MAX_SESSIONS", "2"))
MAX_PER_ADDRESS = int(os.environ.get("TV_MAX_PER_ADDRESS", "2"))
# Admission/HTTP work has its own fixed bound, independent of media sessions.
MAX_DISPATCH = 8
# Upper bound on a ticket's remaining life. Production tickets live 10 minutes.
TICKET_MAX_AGE_S = int(os.environ.get("TV_TICKET_MAX_AGE_S", str(15 * 60)))


def load_key(path: Path) -> bytes:
    """Read the shared ticket key; owner-only regular file, 32..128 bytes."""
    fd = os.open(path, os.O_RDONLY | getattr(os, "O_NOFOLLOW", 0))
    with os.fdopen(fd, "rb") as stream:
        info = os.fstat(stream.fileno())
        if not stat.S_ISREG(info.st_mode) or stat.S_IMODE(info.st_mode) & 0o077:
            raise ValueError("ticket key file must be a regular file readable by its owner only")
        key = stream.read(129).strip()
    if not 32 <= len(key) <= 128:
        raise ValueError("ticket key must be 32..128 bytes")
    return key


def sign(key: bytes, device: str, expires: int) -> str:
    message = f"{device}.{expires}".encode("ascii")
    return hmac.new(key, message, hashlib.sha256).hexdigest()


def verify_ticket(key: bytes, ticket: object, now: float | None = None) -> str:
    """Return the device id of a valid ticket, else raise ProtocolError."""
    if not isinstance(ticket, str) or len(ticket) > 160:
        raise ProtocolError("authentication failed")
    parts = ticket.split(".")
    if len(parts) != 3 or not parts[0] or not parts[1].isdigit():
        raise ProtocolError("authentication failed")
    device, expires_text, mac = parts
    if not all("0" <= c <= "9" or "a" <= c <= "f" for c in device) or len(device) > 32:
        raise ProtocolError("authentication failed")
    expires = int(expires_text)
    now = time.time() if now is None else now
    if not hmac.compare_digest(mac, sign(key, device, expires)):
        raise ProtocolError("authentication failed")
    if expires < now or expires > now + TICKET_MAX_AGE_S:
        raise ProtocolError("authentication failed")
    return device


class Session(tv_server.AVServer):
    """One device. Authenticates with a ticket instead of the shared token."""

    def __init__(self, key: bytes, stop: threading.Event, logger, replay: Replay | None = None,
                 shared=None, shared_loops=frozenset()):
        # The parent constructor validates the bind address only; this instance
        # never listens, so loopback is passed and ignored.
        super().__init__(None, "127.0.0.1", 0, logger=logger)
        self.key = key
        self.stop = stop
        self.device = ""
        self.replay = replay
        self.replay_active = False
        self.shared, self.shared_loops = shared, shared_loops

    def _session(self, connection: socket.socket) -> None:
        self.phase = "hello_receive"
        hello = receive_packet(connection, expected_session=0)
        self.phase = "authenticate"
        tv_server.authenticate(hello, None)
        payload = json_object(hello.payload)
        self.device = self.authenticate_hello(hello, payload)
        self.profile = quality.requested(payload.get('quality'))
        requested = payload.get("channel")
        channel_id = requested if isinstance(requested, str) and requested in CHANNELS else DEFAULT_CHANNEL
        # The firmware explicitly sends "" to request the default at boot.
        # Missing/invalid channel values must still never enable a replay.
        replay_channel = channel_id if requested == "" else requested
        if self.replay and self.replay.matches(self.device, replay_channel):
            self.profile = None  # diagnostic replay owns its announced profile
            self.channel_factory = self.replay.channel
            self.replay_active = True
            self.logger(f"fixed_replay device={self.device} sha256={self.replay.sha256} "
                        f"until={self.replay.until}")
        elif self.shared is not None:
            options = {'profile': self.profile} if self.profile is not None else {}
            self.channel_factory = self.shared.channel_factory(
                channel_id, loop=channel_id in self.shared_loops, **options)
        elif self.profile is not None:
            factory = self.channel_factory
            def selected(*args):
                channel = factory(*args)
                channel.profile = self.profile
                return channel
            self.channel_factory = selected
        self._live_session(connection, channel_id)

    def authenticate_hello(self, hello, payload):
        return verify_ticket(self.key, payload.get("token"))

    def session_channels(self):
        entries = super().session_channels()
        if self.replay_active:
            entries = [dict(row, name="Video Test") if row['id'] == self.replay.channel_id
                       else row for row in entries]
        return entries


class PublicServer:
    def __init__(self, key: bytes, bind: str, port: int, logger=print,
                 replay: Replay | None = None, shared=None, shared_loops=frozenset(),
                 session_factory=Session, http_handler=None):
        self.session_factory = session_factory
        self.key, self.bind, self.port, self.logger = key, bind, port, logger
        self.stop = threading.Event()
        self.lock = threading.Lock()
        self.active: dict[int, str] = {}
        self.next_id = 0
        self.replay = replay
        self.shared, self.shared_loops = shared, shared_loops
        self.http_handler = http_handler
        self.dispatch_slots = threading.BoundedSemaphore(MAX_DISPATCH)

    def _dispatch(self, connection: socket.socket, address: str) -> None:
        handed_off = False
        try:
            if self.http_handler is not None:
                # Peek only; FAV1 retains its existing packet reader and clocks.
                deadline = time.monotonic() + 2
                connection.settimeout(0.2)
                while True:
                    try:
                        prefix = connection.recv(4, socket.MSG_PEEK)
                    except socket.timeout:
                        if time.monotonic() >= deadline or self.stop.is_set():
                            return
                        continue
                    if not prefix:
                        return
                    if len(prefix) == 4:
                        break
                    if time.monotonic() >= deadline or self.stop.is_set():
                        return
                    time.sleep(0.005)
                if prefix != b'FAV1':
                    self.http_handler(connection, self.port)
                    return
            handed_off = self._admit(connection, address)
            if not handed_off:
                self._busy(connection)
        except Exception as error:
            # Handlers must not leak a connection, a slot, or request secrets.
            self.logger(f"dispatch failed: {type(error).__name__}")
        finally:
            if not handed_off:
                connection.close()
            self.dispatch_slots.release()

    def _admit(self, connection: socket.socket, address: str) -> bool:
        with self.lock:
            total = len(self.active)
            same = sum(1 for a in self.active.values() if a == address)
            if total >= MAX_SESSIONS or same >= MAX_PER_ADDRESS:
                return False
            self.next_id += 1
            ident = self.next_id
            self.active[ident] = address
        try:
            threading.Thread(target=self._run, args=(ident, connection, address),
                             name=f"tv-{ident}", daemon=True).start()
        except RuntimeError:
            with self.lock:
                self.active.pop(ident, None)
            raise
        return True

    def _reject(self, connection: socket.socket, address: str) -> None:
        try:
            self._busy(connection)
        finally:
            connection.close()
            self.dispatch_slots.release()

    def _busy(self, connection: socket.socket) -> None:
        # Answer the HELLO with ERROR so the device can say "full" rather than
        # treat the server as unreachable.
        try:
            connection.settimeout(1.0)
            receive_packet(connection, expected_session=0)
            send_packet(connection, Packet(Kind.ERROR, 0, 0, 0, json_bytes({"reason": "busy"})))
        except (OSError, EOFError, ProtocolError):
            pass

    def _run(self, ident: int, connection: socket.socket, peer: str) -> None:
        session = self.session_factory(self.key, self.stop,
                          lambda message: self.logger(f"[{ident}] {message}"), self.replay,
                          self.shared, self.shared_loops)
        outcome = "closed"
        try:
            with connection:
                connection.setblocking(False)
                connection.setsockopt(socket.IPPROTO_TCP, socket.TCP_NODELAY, 1)
                connection.setsockopt(socket.SOL_SOCKET, socket.SO_SNDBUF,
                                      tv_server.SEND_BUFFER_BYTES)
                connection.setsockopt(socket.SOL_SOCKET, socket.SO_RCVBUF, 4096)
                tv_server.configure_tcp_pacing(connection)
                session._session(connection)
        except (OSError, EOFError, ProtocolError, LiveError) as error:
            outcome = type(error).__name__
        except Exception as error:  # one session must never stop the listener
            outcome = "unexpected:" + type(error).__name__
        finally:
            with self.lock:
                self.active.pop(ident, None)
            # Device id is a hardware id, not a secret; the address is not logged.
            self.logger(f"[{ident}] end device={session.device or '-'} outcome={outcome} "
                        f"phase={session.phase} audio={session.audio_sent} video={session.video_sent}")

    def serve(self) -> None:
        with socket.socket(socket.AF_INET, socket.SOCK_STREAM) as listener:
            listener.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
            listener.bind((self.bind, self.port))
            self.port = listener.getsockname()[1]
            listener.listen(16)
            self.logger(f"FoloTalk TV listening on {self.bind}:{self.port} "
                        f"max_sessions={MAX_SESSIONS} channels={len(CHANNELS)}")
            while not self.stop.is_set():
                if not select.select([listener], [], [], 0.2)[0]:
                    continue
                connection, peer = listener.accept()
                address = peer[0]
                # The signed entry keeps its original direct admission path.
                if self.http_handler is None and self._admit(connection, address):
                    continue
                if not self.dispatch_slots.acquire(blocking=False):
                    connection.close()
                    continue
                handler = self._dispatch if self.http_handler is not None else self._reject
                try:
                    threading.Thread(target=handler, args=(connection, address),
                                     name="tv-dispatch", daemon=True).start()
                except RuntimeError:
                    connection.close()
                    self.dispatch_slots.release()
            # Give sessions a moment to close their ffmpeg children.
            deadline = time.monotonic() + 5
            while self.active and time.monotonic() < deadline:
                time.sleep(0.1)


def main(argv: list[str] | None = None) -> int:
    for stream in (sys.stdout, sys.stderr):
        stream.reconfigure(encoding="utf-8", errors="replace")
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--bind", default="127.0.0.1")
    parser.add_argument("--port", type=int, default=18096)
    parser.add_argument("--ticket-key-file", type=Path,
                        default=os.environ.get("TV_TICKET_KEY_FILE"))
    args = parser.parse_args(argv)
    if not args.ticket_key_file:
        print("TV_TICKET_KEY_FILE is required", file=sys.stderr)
        return 2
    try:
        key = load_key(Path(args.ticket_key_file))
    except (OSError, ValueError) as error:
        print(f"ticket key unusable: {type(error).__name__}", file=sys.stderr)
        return 2
    if not CHANNELS:
        print("no channels configured", file=sys.stderr)
        return 2
    try:
        replay = Replay.from_env()
    except (OSError, ValueError) as error:
        print(f"replay configuration unusable: {type(error).__name__}", file=sys.stderr)
        return 2
    server = PublicServer(key, args.bind, args.port,
                          logger=lambda message: print(message, flush=True), replay=replay)
    from .shared import from_env
    try:
        server.shared, server.shared_loops = from_env(server.logger)
    except ValueError:
        print('shared live configuration invalid', file=sys.stderr)
        return 2
    for signum in (signal.SIGINT, signal.SIGTERM):
        signal.signal(signum, lambda *_: server.stop.set())
    try:
        server.serve()
    finally:
        if server.shared is not None:
            server.shared.close()
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
