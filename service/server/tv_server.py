"""Single-client LAN server for live channels; standard library only, transcoded by ffmpeg."""

from __future__ import annotations

import argparse
import errno
import hmac
import ipaddress
import os
import secrets
import select
import signal
import socket
import stat
import sys
import threading
import time
from pathlib import Path

import enum
from typing import Any, Dict, List

class SessionState(str, enum.Enum):
    PLAYING = "PLAYING"
    STARVED_REBUFFERING = "STARVED_REBUFFERING"
    RECOVERING = "RECOVERING"
    CLOSED = "CLOSED"

from . import frames, netident
from .timeline import SourceState
from .rate import ADAPTIVE, ByteRate, FramePacer
from .live import (CHANNELS, CHANNEL_AGENTS, DEFAULT_CHANNEL, LiveChannel, LiveError,
                   AUDIO_MAX_LOOKAHEAD_MS, PREBUFFER_TIMEOUT_S, channel_list)
# Timing constants all come from media.py; live.py imports them from there too.
from .media import (AUDIO_CHUNK_MS, AUDIO_LEAD_MS, DURATION_MS, FPS,
                    START_DELAY_MS, VIDEO_LEAD_MS)
from .protocol import (IO_TIMEOUT, VIDEO_CONTINUES, Kind, Packet,
                       ProtocolError, json_bytes, json_object, receive_packet,
                       send_packet, _write_slice, WritePacer)
from . import reconnect

# Environment variable name used instead of --token-file; only the token value must match the firmware, not the variable name.
TOKEN_ENV = "TV_PAIRING_TOKEN"
# All values are the constants the sender actually uses; no separate copies are kept.
CONFIG = {"width": frames.WIDTH, "height": frames.HEIGHT, "fps": FPS,
          "sample_rate": 16000,
          "channels": 1, "sample_bits": 16, "audio_codec": "ima_adpcm",
          "audio_chunk_ms": AUDIO_CHUNK_MS,
          "video_max_bytes": frames.VIDEO_MAX,
          # The device checks its own geometry against this; on mismatch it refuses to talk, so stripes are not drawn on the wrong rows.
          "stripe_rows": frames.STRIPE_ROWS,
          "duration_ms": DURATION_MS,
          "start_delay_ms": START_DELAY_MS, "audio_lead_ms": AUDIO_LEAD_MS,
          "video_lead_ms": VIDEO_LEAD_MS}
PRIVATE_NETWORKS = tuple(ipaddress.ip_network(net) for net in
                         ("10.0.0.0/8", "172.16.0.0/12", "192.168.0.0/16", "127.0.0.0/8"))

# Per-frame write deadline (seconds); the session is abandoned on timeout. Must be below the ~3 s no-data threshold at which the device disconnects.
FRAME_WRITE_DEADLINE_S = float(os.environ.get("TV_FRAME_DEADLINE_S", "2.5"))

# Socket send buffer size; see CLAUDE.md key design decision 4.
SEND_BUFFER_BYTES = int(os.environ.get("TV_SEND_BUFFER", "16384"))
# Linux per-socket pacing, bytes/s including both media. Zero keeps kernel defaults.
TCP_PACING_RATE = int(os.environ.get("TV_TCP_PACING_RATE", "0"))
WRITE_PACING_RATE = int(os.environ.get("TV_WRITE_PACING_RATE", "0"))
# Keep the byte budget fixed while allowing a controlled TCP batching comparison.
WRITE_PACING_CHUNK = int(os.environ.get("TV_WRITE_PACING_CHUNK", "1200"))
if not 1 <= WRITE_PACING_CHUNK <= 16384:
    raise ValueError("TV_WRITE_PACING_CHUNK must be between 1 and 16384")


def configure_tcp_pacing(connection: socket.socket) -> None:
    """Spread a frame's TCP burst without sleeping in the audio sender."""
    if not TCP_PACING_RATE:
        return
    if not sys.platform.startswith("linux") or not 0 < TCP_PACING_RATE < 2**31:
        raise ValueError("TV_TCP_PACING_RATE requires Linux and a positive byte rate")
    # Linux UAPI asm-generic/socket.h; some Python builds omit this constant.
    connection.setsockopt(socket.SOL_SOCKET,
                          getattr(socket, "SO_MAX_PACING_RATE", 47), TCP_PACING_RATE)

# Unpaced fallback. Audio may interleave only between complete protocol packets.
VIDEO_SLICE_BYTES = 4096

# When a single write takes longer than this many seconds, log its size and location; 0 logs every write.
PROBE_SLOW_S = float(os.environ.get("TV_PROBE_SLOW_S", "0.05"))

# How long a new session waits for the first media; after the timeout the device stays on the reconnect notice screen.
STARTUP_WAIT_S = min(PREBUFFER_TIMEOUT_S, 10)


WILDCARD_BIND = "0.0.0.0"


def local_ipv4(address: str) -> bool:
    try:
        ip = ipaddress.IPv4Address(address)
    except ipaddress.AddressValueError:
        return False
    return any(ip in network for network in PRIVATE_NETWORKS)


def validate_token(token: str) -> bytes:
    if not isinstance(token, str) or not 16 <= len(token) <= 128:
        raise ValueError("pairing token must be 16..128 printable ASCII characters")
    if any(not 33 <= ord(char) <= 126 for char in token):
        raise ValueError("pairing token must be 16..128 printable ASCII characters")
    return token.encode("ascii")


def load_token(path: Path | None = None) -> bytes | None:
    """Return the pairing token; None when unset (HELLO content is not checked, the caller shows a notice)."""
    environment = os.environ.get(TOKEN_ENV)
    if path is not None and environment is not None:
        raise ValueError("choose either token environment or token file")
    if path is None:
        if environment is None:
            return None
        return validate_token(environment)
    flags = os.O_RDONLY | os.O_NONBLOCK | getattr(os, "O_NOFOLLOW", 0)
    fd = os.open(path, flags)
    with os.fdopen(fd, "rb") as stream:
        info = os.fstat(stream.fileno())
        if (not stat.S_ISREG(info.st_mode) or info.st_uid != os.getuid() or
                stat.S_IMODE(info.st_mode) not in (0o400, 0o600)):
            raise ValueError("token file must be owner-only, regular, mode 0400 or 0600")
        raw = stream.read(130)
    # Allow one trailing LF; the read length is capped.
    try:
        return validate_token(raw.removesuffix(b"\n").decode("ascii"))
    except UnicodeError:
        raise ValueError("token file must contain printable ASCII") from None


def authenticate(packet: Packet, token: bytes | None) -> None:
    """Validate the first HELLO; also validate the token when one is configured."""
    if (packet.kind != Kind.HELLO or packet.session != 0 or
            packet.seq != 0 or packet.pts_ms != 0):
        raise ProtocolError("expected initial HELLO")
    hello = json_object(packet.payload)
    if type(hello.get("version")) is not int or hello["version"] != 1:
        raise ProtocolError("unsupported HELLO version")
    if token is None:
        # No token configured: HELLO content is not checked, but the HELLO shape was validated above.
        return
    supplied = hello.get("token")
    if not isinstance(supplied, str):
        raise ProtocolError("authentication failed")
    try:
        supplied_bytes = validate_token(supplied)
    except ValueError:
        raise ProtocolError("authentication failed") from None
    if not hmac.compare_digest(supplied_bytes, token):
        raise ProtocolError("authentication failed")


class AVServer:
    """One thread handles accept and I/O, extra connections are closed at once; blocking ops have deadlines, and the stop flag is polled every 50 ms."""

    def __init__(self, token: bytes | None, bind: str = "127.0.0.1",
                 port: int = 8096, logger=None):
        if bind != WILDCARD_BIND and not local_ipv4(bind):
            raise ValueError("bind must be 0.0.0.0 or a loopback or RFC1918 IPv4 address")
        if token is not None:
            # Validated at construction: a malformed token fails here; None means no validation.
            validate_token(token.decode("ascii"))
        self.token = token
        self.bind, self.port = bind, port
        self.stop = threading.Event()
        self.ready = threading.Event()
        self.listener: socket.socket | None = None
        self.completed = self.rejected = self.failed = self.dropped_video = 0
        # Connections closed during the handshake (channel switch, device sleep, port probe) are counted separately and not in failed.
        self.abandoned = 0
        self.audio_sent = self.video_sent = 0
        self.session_id = 0
        self.phase = "idle"
        self.live_channel: LiveChannel | None = None
        # Empty string sends both audio and video; audio or video alone is for diagnostics only, see _pace_live.
        self.media_filter = ""
        self.channel_name = ""
        self.live_note = ""
        self.ffmpeg = "ffmpeg"
        # Injectable, so the send loop can be tested without ffmpeg or the network.
        self.channel_factory = LiveChannel
        self.frame_deadline_s = FRAME_WRITE_DEADLINE_S
        self.logger = logger or (lambda message: None)
        self.session_state: SessionState = SessionState.CLOSED
        self.state_history: List[Dict[str, Any]] = []

    def set_session_state(self, state: SessionState, session: int, reason: str = "", **kwargs: Any) -> None:
        """Record session state transitions and the send counters at that moment."""
        old_state = getattr(self, "session_state", SessionState.CLOSED)
        self.session_state = state
        now_m = time.monotonic()
        rec = {
            "timestamp": round(now_m, 4),
            "session": session,
            "from_state": old_state.value if isinstance(old_state, SessionState) else str(old_state),
            "to_state": state.value if isinstance(state, SessionState) else str(state),
            "reason": reason,
            "audio_sent": getattr(self, "audio_sent", 0),
            "video_sent": getattr(self, "video_sent", 0),
            "dropped_video": getattr(self, "dropped_video", 0),
            **kwargs
        }
        if not hasattr(self, "state_history") or self.state_history is None:
            self.state_history = []
        self.state_history.append(rec)
        detail_str = " ".join(f"{k}={v}" for k, v in kwargs.items())
        self.logger(
            f"session={session} STATE_CHANGE {rec['from_state']} -> {rec['to_state']} "
            f"reason='{reason}' {detail_str}".strip()
        )

    def serve(self) -> None:
        with socket.socket(socket.AF_INET, socket.SOCK_STREAM) as listener:
            self.listener = listener
            listener.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
            listener.bind((self.bind, self.port))
            self.port = listener.getsockname()[1]
            # The device retries once a second; a backlog queue must remain while one connection is served, otherwise retries are refused outright.
            listener.listen(8)
            listener.setblocking(False)
            self.ready.set()
            while not self.stop.is_set():
                if not select.select([listener], [], [], 0.05)[0]:
                    continue
                connection, peer = listener.accept()
                with connection:
                    if not local_ipv4(peer[0]):
                        self.rejected += 1
                        continue
                    connection.setblocking(False)
                    connection.setsockopt(socket.IPPROTO_TCP, socket.TCP_NODELAY, 1)
                    # The send buffer only needs to hold one picture packet; see CLAUDE.md key design decision 4.
                    connection.setsockopt(socket.SOL_SOCKET, socket.SO_SNDBUF,
                                          SEND_BUFFER_BYTES)

                    connection.setsockopt(socket.SOL_SOCKET, socket.SO_RCVBUF, 4096)
                    configure_tcp_pacing(connection)
                    self.session_id = self.audio_sent = self.video_sent = 0
                    outcome, reason = "closed", "none"
                    self.phase = "hello_receive"
                    try:
                        self._session(connection)
                        self.completed += 1
                    except (OSError, EOFError, ProtocolError, LiveError) as error:
                        # The write may be incomplete, so the connection must be closed; the log has only the exception class name, no payload, token or peer address.
                        # A disconnect before authentication counts as abandoned, after it as failed.
                        if self.session_id == 0:
                            self.abandoned += 1
                            outcome = "abandoned"
                        else:
                            self.failed += 1
                            outcome = "failed"
                        reason = type(error).__name__
                        # No ffmpeg diagnostics are collected here: the finally in _live_session already logs and clears live_channel.
                    except Exception as error:
                        if self.session_id == 0:
                            self.abandoned += 1
                            outcome = "abandoned"
                        else:
                            self.failed += 1
                            outcome = "failed"
                        reason = type(error).__name__
                        import traceback
                        tb = traceback.extract_tb(error.__traceback__)
                        sanitized_tb = " -> ".join([f"{Path(f.filename).name}:{f.lineno}:{f.name}" for f in tb[-3:]])
                        self.live_note = f"unexpected={type(error).__name__}[{sanitized_tb}]"
                    finally:
                        # Log only stage names defined in code and exception class names, never str/repr(error).
                        note = f" note={self.live_note}" if self.live_note else ""
                        self.logger(f"session={self.session_id} state={outcome} "
                                    f"reason={reason} phase={self.phase} "
                                    f"authenticated={int(self.session_id != 0)} "
                                    f"audio_sent={self.audio_sent} video_sent={self.video_sent}{note}")
                        self.live_note = ""
                # Reject connections that piled up during the old session; later sessions are unaffected.
                self._reject_waiting()
        self.listener = None

    def _reject_waiting(self) -> None:
        assert self.listener is not None
        # Bound the number of handled connections even under a connection flood.
        for _ in range(8):
            try:
                connection, _ = self.listener.accept()
            except BlockingIOError:
                return
            connection.close()
            self.rejected += 1

    def _device_left(self, connection: socket.socket, session: int) -> bool:
        """Return True when the device has ended this session: it sent END, or closed the connection.

        Called before writes and between the writes of one frame, so a normal channel switch is not counted as an error. A packet other than END raises ProtocolError;
        a disconnect without END (EOFError) is raised as is, and serve() classifies it by whether the handshake completed.
        """
        if not select.select([connection], [], [], 0)[0]:
            return False
        try:
            packet = receive_packet(connection, IO_TIMEOUT, expected_session=session)
        except TimeoutError:
            # select reports readable but there is less than one full packet: the device is still there, continue the session.
            return False
        except EOFError:
            # A disconnect without END is raised as is; serve() classifies it.
            raise
        if packet.kind != Kind.END:
            raise ProtocolError("only END allowed while playing")
        return True

    def _wait_until(self, connection: socket.socket, target: float, session: int) -> bool:
        self.phase = "schedule_wait"
        while not self.stop.is_set():
            delay = max(0.0, min(0.05, target - time.monotonic()))
            watch = [connection]
            if self.listener is not None:
                watch.append(self.listener)
            readable = select.select(watch, [], [], delay)[0]
            if self.listener is not None and self.listener in readable:
                self._reject_waiting()
            if connection in readable:
                self.phase = "control_receive"
                try:
                    packet = receive_packet(connection, expected_session=session)
                except TimeoutError:
                    # select reports readable but data is not yet available: not a peer error, keep waiting.
                    self.phase = "schedule_wait"
                    continue
                if packet.kind != Kind.END:
                    raise ProtocolError("only END allowed after HELLO")
                return False
            if time.monotonic() >= target:
                return True
        return False

    def make_channel(self) -> LiveChannel:
        """Create a new, unstarted channel for the session's channel id."""
        return self.channel_factory(CHANNELS[self.channel_name], self.ffmpeg,
                                    CHANNEL_AGENTS.get(self.channel_name, ""))

    def session_channels(self):
        return channel_list()

    def _live_session(self, connection: socket.socket, channel_id: str) -> None:
        """Schedule one live channel from a single monotonic clock origin.

        A channel switch is a new session; each session owns one transcoder process and one time origin;
        video later than its presentation time is dropped, and audio must never block on video.
        """
        self.phase = "live_start"
        # The channel's User-Agent is passed along too: many mirrors only answer the player used when they were scraped.
        channel = self.channel_factory(CHANNELS[channel_id], self.ffmpeg,
                                       CHANNEL_AGENTS.get(channel_id, ""))
        self.channel_name = channel_id
        try:
            session = secrets.randbelow(0xFFFFFFFF) + 1
            self.session_id = session
            # Send CONFIG before requesting data from the source: the device handshake allows only ~2 s, and sampling the palette first would time it out.
            # CONFIG describes only the format and is independent of the palette.
            send_packet(connection, Packet(Kind.CONFIG, session, 0, 0,
                                           json_bytes(dict(CONFIG, session=session,
                                                           **({'fps': self.profile.fps, 'quality': self.profile.name}
                                                              if getattr(self, 'profile', None) is not None else {}),
                                                           channel=channel_id,
                                                           channel_list=self.session_channels()))))
            self.phase = "live_palette"
            # A palette is required before the first frame; without one the device has no colors and the picture is all black.
            try:
                channel.build_palette()
            except LiveError as error:
                # Tell the device about the failure explicitly, otherwise it waits for the palette forever.
                try:
                    send_packet(connection, Packet(Kind.ERROR, session, 0, 0,
                                                   json_bytes({"reason": "source unavailable"})))
                except OSError:
                    pass
                raise LiveError(str(error)) from None
            if not channel.palette:
                raise LiveError("channel has no palette")
            # start() is inside the try so that a Popen failure does not leak the pipes and the stderr file.
            # A source that fails to start does not end the session; the device stays on the reconnect notice screen and retries.
            failed = ""
            try:
                channel.start()
            except (LiveError, OSError) as error:
                self.logger(f"session={session} source start failed: {type(error).__name__}")
                failed = "start_failed"
            if not failed:
                self.live_channel = channel
                # A calibration failure only warns; the session is not refused.
                if not channel.timeline.calibrated:
                    self.logger(
                        f"session={session} WARNING no clock calibration: "
                        f"{channel.timeline.calibration_note}; the picture will not "
                        f"be paired and no frames will be sent")
            # PALETTE follows CONFIG and precedes all frames; sent once per channel in each session.
            send_packet(connection, Packet(Kind.PALETTE, session, 1, 0,
                                           channel.palette))
            self.logger(f"session={session} state=live channel={channel_id}")
            self.phase = "live_prebuffer"
            deadline = time.monotonic() + STARTUP_WAIT_S
            while not failed and not channel.prebuffered() and time.monotonic() < deadline:
                if channel.failure():
                    failed = "decoder_stopped"
                    break
                # Poll the connection and the stop flag instead of sleeping the whole span: otherwise stop requests are ignored and a device reconnect has to queue.
                if self.stop.is_set():
                    return
                # _wait_until rewrites phase; restore it after the call, otherwise a session ending here would be recorded as waiting for the schedule.
                waiting = self._wait_until(connection, min(deadline, time.monotonic() + 0.02), session)
                self.phase = "live_prebuffer"
                if not waiting:
                    return
            if not failed and not channel.prebuffered():
                failed = "no_media"
            resume = None
            if failed:
                wire = reconnect.WireState(seq=2, audio_pts=0)
                now = time.monotonic()
                channel = reconnect.hold(self, connection, session, channel, wire,
                                         now + AUDIO_LEAD_MS / 1000, now,
                                         restart_reason=failed)
                if channel is None:
                    return
                self.live_channel = channel
                resume = wire
            self.set_session_state(SessionState.PLAYING, session, reason="session_started")
            self._pace_live(connection, channel, session, resume)
        finally:
            self.set_session_state(SessionState.CLOSED, session, reason="session_closed",
                                   audio_sent=self.audio_sent, video_sent=self.video_sent)
            try:
                if channel.failure() is not None:
                    try:
                        self.live_note = channel.diagnostics()
                    except Exception as diag_err:
                        self.logger(f"session={session} WARNING: channel diagnostics failed: {diag_err}")
            finally:
                try:
                    channel.close()
                except Exception as close_err:
                    self.logger(f"session={session} WARNING: channel close failed: {close_err}")
                finally:
                    live, self.live_channel = self.live_channel, None
                    if live is not None and live is not channel:
                        try:
                            live.close()
                        except Exception as close_err:
                            self.logger(f"session={session} WARNING: channel close failed: {close_err}")

    def _pace_live(self, connection: socket.socket, channel: LiveChannel,
                   session: int, resume: reconnect.WireState | None = None) -> None:
        # The origin must not be earlier than now by more than the audio lead, otherwise data prefilled by ffmpeg expires at once and is sent in a burst.
        # Prebuffer depth is not counted in the origin; the opening burst is limited by AUDIO_MAX_LOOKAHEAD_MS and one slot per frame.
        # The origin is the larger of START_DELAY_MS and AUDIO_LEAD_MS, which sets how far audio timestamps may lead the clock.
        origin = time.monotonic() + max(START_DELAY_MS, AUDIO_LEAD_MS) / 1000
        audio_pts = 0
        # Video timestamps come per frame from pop_video(); they are not maintained here.
        # Slot in which the previous picture was sent, -1 if none yet; another frame may be sent only once the clock's slot exceeds it.
        # Slots are used instead of due times, so a slow loop, a pause or a burst cannot make it drift from the current clock.
        last_slot = -1
        # The source frame rate is the send cap; full-frame mode meets the budget through the interval, delta mode adjusts per-frame bytes.
        # Stand-in channels without an fps attribute are scheduled at the nominal frame rate.
        # With TV_ADAPTIVE=0 the byte rate is fixed too.
        fps = getattr(channel, "fps", FPS)
        controller = ByteRate(fps, adaptive=ADAPTIVE)
        video_pacer = FramePacer(controller.rate, time.monotonic())
        write_pacer = WritePacer(WRITE_PACING_RATE, WRITE_PACING_CHUNK) if WRITE_PACING_RATE else None
        # Do not silently cap a configured 12 KiB write at the old 4 KiB slice.
        video_slice_bytes = write_pacer.chunk_bytes if write_pacer else VIDEO_SLICE_BYTES
        # Also kept on the server so tests can read the final byte rate of the session.
        self._last_controller = controller
        # Controller per-second window totals: picture bytes, frames actually sent, longest single write, dropped frames.
        window_video_bytes = 0
        window_frames = 0
        window_worst_write = 0.0
        window_dropped = 0
        window_started = time.monotonic()
        unwritable_since = None
        self._recovery_start = 0.0
        # CONFIG takes sequence number 0, PALETTE 1, the first media packet 2; the device rejects non-consecutive sequence numbers.
        seq = 2
        last_report = 0.0
        last_audio = last_video = 0
        # Send timestamp = channel timestamp + stamp_base; the notice frames and silence advanced the device clock during a stream outage,
        # and a new process after a channel switch continues from it.
        stamp_base = 0
        last_audio_wire = -AUDIO_CHUNK_MS
        last_video_wire = -1
        if resume is not None:
            seq, audio_pts = resume.seq, resume.audio_pts
            last_audio_wire, last_video_wire = resume.audio_stamp, resume.video_stamp
            stamp_base = (last_audio_wire + AUDIO_CHUNK_MS
                          - int(round(channel.session_clock.audio_items * AUDIO_CHUNK_MS)))
            origin = time.monotonic() - (audio_pts - AUDIO_LEAD_MS) / 1000.0
        # Longest no-output gap in this loop and the write time within it; compared with the device-side report it shows which end stalled.
        gap_start = time.monotonic()
        max_send_gap = 0.0
        busy_us = 0.0

        def note_send(took: float) -> None:
            nonlocal gap_start, max_send_gap, busy_us
            at = time.monotonic()
            idle = at - gap_start
            if idle > max_send_gap:
                max_send_gap = idle
            busy_us += took * 1e6
            gap_start = at

        def hold_on_notice(empty_since: float) -> bool:
            """Stay on the reconnect notice screen until the source has data again.

            Returns False when the session ends (the device left or asked to stop).
            """
            nonlocal channel, seq, audio_pts, origin, now, slot_now, last_slot, stamp_base
            nonlocal last_audio_wire, last_video_wire, window_started, window_worst_write
            nonlocal window_video_bytes, window_frames, window_dropped
            self.set_session_state(
                SessionState.STARVED_REBUFFERING,
                session,
                reason="channel_audio_empty",
                empty_since=round(empty_since, 4)
            )
            wire = reconnect.WireState(seq, audio_pts, last_audio_wire, last_video_wire)
            held = reconnect.hold(self, connection, session, channel, wire, origin, empty_since)
            if held is None:
                return False
            if held is not channel:
                channel = held
                self.live_channel = held
                if hasattr(channel, "set_video_rate"):
                    channel.set_video_rate(controller.rate)
            seq, audio_pts = wire.seq, wire.audio_pts
            last_audio_wire, last_video_wire = wire.audio_stamp, wire.video_stamp
            stamp_base = (last_audio_wire + AUDIO_CHUNK_MS
                          - int(round(channel.session_clock.audio_items * AUDIO_CHUNK_MS)))
            now = time.monotonic()
            origin = now - (audio_pts - AUDIO_LEAD_MS) / 1000.0
            slot_now = int((now - origin) * fps)
            last_slot = slot_now - 1
            self._recovery_start = now
            self.set_session_state(
                SessionState.RECOVERING,
                session,
                reason="channel_data_resumed",
                starve_duration_s=round(now - empty_since, 4),
                realigned_origin=round(origin, 4),
                audio_pts=audio_pts,
                slot_now=slot_now
            )
            window_started = now
            window_worst_write = 0.0
            window_video_bytes = 0
            window_frames = 0
            window_dropped = 0
            return True

        self.wire_bytes = 0
        while not self.stop.is_set():
            if (channel.failure() is not None and not channel.audio_pending()
                    and self.media_filter != "video"):
                if not hold_on_notice(time.monotonic()):
                    return
                continue
            now = time.monotonic()
            self._loops = getattr(self, "_loops", 0) + 1
            # Picture timestamps come per frame from pop_video() (content timeline, see server/timeline.py); this loop does not compute them.
            # The device presents pictures by the audio clock; frames with timestamps too far ahead occupy the receive buffer until audio underruns.
            audio_at = origin + (audio_pts - AUDIO_LEAD_MS) / 1000
            # Pictures are sent in their own slots, one slot being one frame interval; a frame may be sent once the clock's slot is greater than the slot of the last send.
            # It must not be tied to audio due times: audio has priority and pictures would never get a turn.
            # Nor a due-time variable: pulling the due time toward the present would make the test always true on every pass.
            slot_now = int((now - origin) * fps)
            # Audio owns the timeline and is not dropped; video must be sent when its own slot is due, and waiting for the audio queue to drain would starve video.
            # Limit how far audio leads the wall clock, so a slow loop pass is not followed by a burst the device cannot hold.
            audio_lookahead_ms = (audio_pts - AUDIO_LEAD_MS) - (now - origin) * 1000
            if audio_lookahead_ms < -400.0:
                # Write backpressure left sending behind real time: re-anchor the origin to remove the negative lead.
                origin = now - (audio_pts - AUDIO_LEAD_MS) / 1000.0
                slot_now = int((now - origin) * fps)
                last_slot = min(last_slot, slot_now - 1)
                audio_lookahead_ms = (audio_pts - AUDIO_LEAD_MS) - (now - origin) * 1000.0
            # One trace log line per second; it must come after the variables it uses have been computed.
            if now - getattr(self, "_last_trace", now) >= 1.0:
                self._last_trace = now
                self.logger(
                    f"SEND t={now-origin:6.1f}s loops={self._loops} "
                    f"a={self.audio_sent} v={self.video_sent} "
                    f"qa={len(channel.audio)} qv={len(channel.video)} "
                    f"look={audio_lookahead_ms:7.0f} "
                    f"a_due={int(audio_at<=now)} "
                    f"v_due={int(slot_now > last_slot)}")
            # The socket must be writable now: blocking means the device queue is full, which is the backpressure that makes sending follow the device clock.
            # Due audio that cannot be written is left for the next pass; audio is not lost, and neither audio_pts nor seq advances.
            # Read before writing: when the device switches channel it sends END and closes at once, and writing first would hit a broken pipe mid-frame.
            readable, writable_now, _ = select.select([connection], [connection], [], 0)
            if connection in readable:
                self.phase = "control_receive"
                self._device_left(connection, session)
                return
            writable = bool(writable_now)
            if writable:
                unwritable_since = None
            elif channel.audio_pending() or channel.video_pending():
                if unwritable_since is None:
                    unwritable_since = now
                window_worst_write = max(window_worst_write, now - unwritable_since)
            else:
                unwritable_since = None
            # Due audio is strictly preferred over pictures. A looser rule letting audio yield to pictures would make audio almost never go out.
            # Pictures do not starve: the loop runs far more often than audio falls due, and each audio block delays a picture by about 1 ms at most.
            self.wire_bytes = getattr(self, "wire_bytes", 0)
            # Whether audio may be sent before its due time. TV_AUDIO_FILL is on by default: the sender tops the device queue up to
            # AUDIO_MAX_LOOKAHEAD_MS and holds it there; when off, audio goes out exactly in real time and the device queue
            # is not refilled while a picture packet occupies the line, so it underruns.
            audio_fill = os.environ.get("TV_AUDIO_FILL", "1") != "0"
            audio_due = (channel.audio_pending()
                         and audio_lookahead_ms <= AUDIO_MAX_LOOKAHEAD_MS
                         and (audio_fill or now >= audio_at))
            # Due audio does not block pictures: no if/elif, audio first and then the picture in the same pass;
            # otherwise pictures could only be sent in the gap between two audio blocks, and that gap does not exist.
            send_audio = audio_due and writable
            send_video = (slot_now > last_slot and writable and channel.video_pending()
                          and (not getattr(channel, "full_frames", False)
                               or video_pacer.ready(now)))
            # For diagnostics only: --media audio or video turns off one of the two to locate a fault; both are on by default.
            if self.media_filter == "audio":
                send_video = False
            elif self.media_filter == "video":
                # Pictures are scheduled by audio position: advance the audio timeline by hand rather than freezing it, otherwise all frames fall due at once.
                send_audio = False
                audio_pts += AUDIO_CHUNK_MS
            self._trace = getattr(self, "_trace", 0) + 1
            if os.environ.get("TV_TRACE") == "1":
                self.logger(
                    f"TRACE n={self._trace} t={(now-origin):6.2f}s "
                    f"due={int(audio_due)} wr={int(writable)} qa={len(channel.audio)} "
                    f"qv={len(channel.video)} look={audio_lookahead_ms:7.0f} "
                    f"sa={self.audio_sent} sv={self.video_sent} "
                    f"late={self.dropped_video} "
                    f"frameK={sum(self._frame_sizes[-3:]) // 1024 if getattr(self,'_frame_sizes',None) else 0} "
                    f"wire_kbps={(self.wire_bytes*8/1000)/max(0.001, now-origin):7.1f}")
            if send_audio:
                # Send all audio blocks that are due, not one per pass: several audio slots pile up while a picture packet is written,
                # and catching up one per pass would keep the lead negative. The cap of 32 blocks (640 ms) only stops a pathological queue from monopolizing the loop;
                # it is also bounded by the clock and AUDIO_MAX_LOOKAHEAD_MS.
                for _ in range(32):
                    if not ((audio_fill or audio_at <= now)
                            and channel.audio_pending()
                            and audio_pts - AUDIO_LEAD_MS - (now - origin) * 1000
                                <= AUDIO_MAX_LOOKAHEAD_MS
                            and select.select([], [connection], [], 0)[1]):
                        break
                    taken = channel.pop_audio()
                    if taken is None:
                        break
                    block, stamp = taken
                    stamp += stamp_base
                    last_audio_wire = stamp
                    self.phase = "live_pcm_send"
                    send_packet(connection, Packet(Kind.PCM, session, seq, stamp, block))
                    self.wire_bytes += len(block) + 24
                    # stamp is the on-wire timestamp (from the sound's own content timeline); audio_pts is the next block's due time
                    # derived by the sender from the number of blocks sent. The two are not merged.
                    audio_pts += AUDIO_CHUNK_MS
                    seq += 1
                    self.audio_sent += 1
                    audio_at = origin + (audio_pts - AUDIO_LEAD_MS) / 1000
            sent_something = send_audio
            if send_video:
                sent_something = True
                # The picture queue depth matches the audio one, see VIDEO_QUEUE_SECONDS in live.py.
                chosen = channel.pop_video()
                frame, video_pts = chosen if chosen is not None else (None, None)
                # No frame matching the head sound does not mean the frame is late: no slot is consumed, and the next pass tries again.
                if frame is None:
                    # No frame to send this pass, but the waiting, controller update and periodic report below must still run, so no continue.
                    sent_something = False
                else:
                    # The slot is consumed whether or not the frame goes out: a congestion drop must not hand the slot to the next frame, or the backlog would be released at once.
                    # Take the value at decision time; do not re-read the clock.
                    last_slot = slot_now
                    self._frame_sizes = getattr(self, "_frame_sizes", [])
                    self._frame_sizes.append(sum(len(p) for p in frame))
                    # No late veto: the device draws on arrival, a slightly late frame is still worth sending; dropping it would make the index jump and cause continuous drops.
                    # Staleness is bounded by the queue: it has fixed length and drops the oldest when full.
                    if writable:
                        # When not writable, drop the whole frame instead of trying: a write that cannot finish would block the single-threaded loop and audio, and must not leave half a packet on the wire.
                        # The packets of one frame are either all sent or none.
                        self.phase = "live_video_send"
                        # One write deadline for the whole frame, max(frame interval, FRAME_WRITE_DEADLINE_S); it is a failure limit, not scheduling;
                        # it must be below the device's ~3 s disconnect threshold, and not so short that a normal frame times out in transit.
                        # Expired frames are dropped by the scheduler; a slow frame should cost only that frame, not end the session.
                        frame_deadline = time.monotonic() + max(1.0 / fps, FRAME_WRITE_DEADLINE_S)
                        if getattr(channel, "full_frames", False):
                            video_pacer.sent(sum(len(part) + 24 for part in frame),
                                             time.monotonic())
                        video_pts = max(video_pts + stamp_base, last_video_wire + 1)
                        last_video_wire = video_pts
                        for n, part in enumerate(frame):
                            # Check between the writes of a frame whether the device has left, so a normal channel switch does not show up as a broken pipe.
                            self.phase = "control_receive"
                            if self._device_left(connection, session):
                                return
                            # Let audio through between picture packets: when a picture packet is stuck on the device window it holds up the whole frame deadline, and no audio goes out meanwhile, so it underruns.
                            # Each packet carries its own length and timestamp; the format does not require the packets of a frame to be adjacent, only the bytes of a single packet to be contiguous.
                            # Catch up on all due audio blocks (cap 32); catching up one block at a time would let the shortfall keep growing.
                            for _ in range(32):
                                now_in_frame = time.monotonic()
                                audio_at = origin + (audio_pts - AUDIO_LEAD_MS) / 1000
                                lookahead = ((audio_pts - AUDIO_LEAD_MS)
                                             - (now_in_frame - origin) * 1000)
                                if not ((audio_fill or now_in_frame >= audio_at)
                                        and channel.audio_pending()
                                        and lookahead <= AUDIO_MAX_LOOKAHEAD_MS
                                        and select.select([], [connection], [], 0)[1]):
                                    break
                                taken = channel.pop_audio()
                                if taken is None:
                                    break
                                block, stamp = taken
                                stamp += stamp_base
                                last_audio_wire = stamp
                                self.phase = "live_pcm_send"
                                _audio_t0 = time.monotonic()
                                send_packet(connection, Packet(Kind.PCM, session, seq,
                                                               stamp, block),
                                            deadline=frame_deadline)
                                note_send(time.monotonic() - _audio_t0)
                                audio_pts += AUDIO_CHUNK_MS
                                seq += 1
                                self.audio_sent += 1
                            self.phase = "live_video_send"
                            # All packets of a frame share the timestamp and, except the first, declare it with the VIDEO_CONTINUES flag, otherwise the device rejects them because the timestamp does not advance.
                            # A single picture packet must be written contiguously with no audio packet inserted: after reading the header the device reads the payload by length,
                            # and inserted audio bytes would be taken as picture data and end the session. So audio is sent only between packets,
                            # and within a packet _write_slice writes in slices, only so the loop is not held by a whole packet and can respond to stop requests.
                            wire = Packet(Kind.JPEG, session, seq, video_pts, part,
                                          VIDEO_CONTINUES if n else 0).encode()
                            sent = 0
                            while sent < len(wire):
                                self.phase = "live_video_send"
                                data = memoryview(wire)[sent:sent + video_slice_bytes]
                                _probe_t0 = time.monotonic()
                                if not _write_slice(connection, data, frame_deadline,
                                                    pacer=write_pacer):
                                    raise TimeoutError("video packet deadline expired")
                                _probe_dt = time.monotonic() - _probe_t0
                                note_send(_probe_dt)
                                # The rate controller judges the window by slices: that is the unit the socket accepts at once, and its duration reflects link congestion.
                                if getattr(self, "session_state", None) != SessionState.RECOVERING and _probe_dt > window_worst_write:
                                    window_worst_write = _probe_dt
                                if _probe_dt > PROBE_SLOW_S and os.environ.get("TV_TRACE") == "1":
                                    self.logger(
                                        f"PROBE slice n={n} bytes={len(data)} "
                                        f"took_ms={_probe_dt * 1000:.0f} phase={self.phase}")
                                sent += len(data)
                            self.wire_bytes += len(part) + 24
                            # Count picture bytes only: sound is a fixed 32 kB/s, and counting it would make all channels look equally heavy.
                            window_video_bytes += len(part) + 24
                            seq += 1
                            self.video_sent += 1
                        # Counted once per frame (outside the packet loop): the controller divides the window byte count by this to get the per-frame cost.
                        window_frames += 1
                        if getattr(self, "session_state", None) == SessionState.RECOVERING:
                            rec_start = getattr(self, "_recovery_start", window_started)
                            # Leave RECOVERING only after at least 1.5 s and 10 frames, waiting for link fluctuation to settle.
                            if (now - rec_start >= 1.5) and window_frames >= 10:
                                self.set_session_state(
                                    SessionState.PLAYING,
                                    session,
                                    reason="recovered_to_steady_state",
                                    video_pts=video_pts,
                                    realigned_origin=round(origin, 4),
                                    current_fps=fps
                                )
                                window_started = time.monotonic()
                                window_worst_write = 0.0
                                window_video_bytes = 0
                                window_frames = 0
                                window_dropped = 0
                    else:
                        # Congestion: give up the frame to protect the audio timeline; the timestamp comes from the clock, so a drop costs only one picture.
                        self.dropped_video += 1
                        window_dropped += 1

            if not sent_something:
                # Nothing due yet: sleep briefly instead of blocking until the next moment, so END or a disconnect is noticed in time.
                if not channel.has_data():
                    empty_since = time.monotonic()
                    empty_threshold = 0.5 if getattr(self, "session_state", None) == SessionState.RECOVERING else 0.3
                    # First wait for normal inter-chunk arrival, then declare a stream outage.
                    while not channel.audio_pending() and self.media_filter != "video" and (time.monotonic() - empty_since < empty_threshold):
                        time.sleep(0.005)

                    if not channel.audio_pending() and self.media_filter != "video":
                        # No data for a sustained period: the device stays on the notice screen.
                        if not hold_on_notice(empty_since):
                            return
                    else:
                        time.sleep(0.005)
                    window_frames = 0
                    window_dropped = 0
                else:
                    time.sleep(0.005)
            # One decision per second, based on the window that just ended; the window must be long enough to hold several frames and short enough to decide several times before the device gives up.
            now = time.monotonic()
            if now - window_started >= 1.0:
                # First decide whether the source is still producing, then notify the controller: a stopped source and sending faster than the link reading are the same reading but call for opposite responses.
                # Stand-in channels may have no source_state; treat that as FLOWING.
                if hasattr(channel, "source_state"):
                    source = channel.source_state(
                        queue_over_bound=len(channel.video) >= channel.video.maxlen)
                else:
                    source = SourceState.FLOWING
                # Used only for the periodic report, not as the controller's master switch: a stopped source may still have queued content,
                # whose writes are still valid evidence of downstream capacity.
                self._last_source_state = source
                if getattr(self, "session_state", None) != SessionState.RECOVERING:
                    before = controller.rate
                    controller.observe(window_video_bytes,
                                       window_worst_write * 1000,
                                       window_dropped,
                                       frames=window_frames,
                                       window_s=now - window_started)
                    if controller.rate != before:
                        video_pacer.set_rate(controller.rate, now)
                        if hasattr(channel, "set_video_rate"):
                            channel.set_video_rate(controller.rate)
                        self.logger(f"RATE {controller.rate // 1000} kB/s: {controller.describe()}")
                window_video_bytes, window_worst_write = 0, 0.0
                window_dropped = window_frames = 0
                window_started = now
            if now - last_report >= 5:
                elapsed = now - last_report if last_report else 5.0
                self.logger(
                    f"live t={now-origin:6.1f}s fps={fps} "
                    f"rate={controller.rate // 1000}kB/s ({controller.reason}) source={getattr(self, '_last_source_state', '?')} "
                    # Offset between the picture and sound content clocks at the pairing point; a_lag and v_lag are only queue depths.
                    f"clock=({channel.session_clock.diagnostics()}) "
                    # Reference used for alignment; getattr keeps the report itself from ending the session (test stand-ins have no timeline).
                    f"basis=({getattr(getattr(channel, 'timeline', None), 'basis', None) or 'none'}) "
                    f"audio_q={len(channel.audio):4d} "
                    f"video_q={len(channel.video):4d} "
                    # How many content seconds the next packet lags the live edge; audio and video should agree.
                    f"a_lag={len(channel.audio) * AUDIO_CHUNK_MS / 1000:5.1f}s "
                    f"v_lag={channel.picture_lag_s():5.1f}s "
                    f"lookahead_ms={audio_lookahead_ms:6.0f} "
                    f"audio_sent={self.audio_sent} video_sent={self.video_sent} "
                    f"audio_pps={(self.audio_sent - last_audio) / elapsed:5.1f} "
                    f"video_pps={(self.video_sent - last_video) / elapsed:5.1f} "
                    f"late_video={self.dropped_video} prod_drop={channel.dropped_video} "
                    f"full_frames={int(getattr(channel, 'full_frames', False))} "
                    f"coarse_frames={getattr(channel, 'coarse_frames', 0)} "
                    f"encode_total_ms={getattr(channel, 'encode_seconds', 0) * 1000:.0f} "
                    f"send_gap_max_ms={max_send_gap * 1000:5.0f} "
                    f"busy={busy_us / 1e6:4.1f}s")
                last_report = now
                last_audio, last_video = self.audio_sent, self.video_sent
                max_send_gap = 0.0
                busy_us = 0.0
        # A server-side stop is a normal ending, not a stream fault.
        if self.stop.is_set():
            return
        raise TimeoutError("live session cancelled")

    def _session(self, connection: socket.socket) -> None:
        self.phase = "hello_receive"
        hello = receive_packet(connection, expected_session=0)
        self.phase = "authenticate"
        authenticate(hello, self.token)
        # The device selects a channel by name and a channel switch is a new connection; a missing name or one not in the table falls back to the default channel given by --channel.
        requested = json_object(hello.payload).get("channel")
        if isinstance(requested, str) and requested in CHANNELS:
            channel_id = requested
        else:
            channel_id = self.channel_name or DEFAULT_CHANNEL
        self._live_session(connection, channel_id)

def print_where_to_connect(bind: str, port: int, token: bytes | None) -> None:
    """Last thing printed at startup: first the address the device should be given, then two notes."""
    shown = bind
    if bind == WILDCARD_BIND:
        detected = netident.lan_address()
        shown = detected if detected and local_ipv4(detected) else None
    for line in netident.describe(shown, port):
        print(line, flush=True)

    # Without a token, any device on the same subnet can watch; say so.
    if token is None:
        print("Note: other devices on this network can also watch the channels relayed by this computer.", flush=True)

    # Closing the window stops the service; the window looks like a log, so users rarely connect the two.
    print("Note: closing this window stops the service and the TV will be interrupted.", flush=True)


def main(argv: list[str] | None = None) -> int:
    # Output is fixed to UTF-8: under a pipe the default is the local encoding (GBK on Chinese Windows), and log_stamp decoding it as UTF-8 would garble.
    for stream in (sys.stdout, sys.stderr):
        stream.reconfigure(encoding="utf-8", errors="replace")
    parser = argparse.ArgumentParser(description=__doc__)
    commands = parser.add_subparsers(dest="command", required=True)
    live_parser = commands.add_parser("live", help="transcode one allowlisted public channel in real time")
    live_parser.add_argument("--channel", required=True, choices=sorted(CHANNELS))
    live_parser.add_argument("--ffmpeg", default="ffmpeg")
    # For diagnostics only: turn off one stream to tell whether the problem is picture or sound.
    live_parser.add_argument("--media", choices=("both", "audio", "video"),
                             default="both")
    live_parser.add_argument("--bind", default=WILDCARD_BIND,
                             help="address to listen on (default: all interfaces; peers outside private networks are refused)")
    live_parser.add_argument("--port", type=int, default=8096)
    live_parser.add_argument("--token-file", type=Path)
    args = parser.parse_args(argv)
    try:
        token = load_token(args.token_file)
        server = AVServer(token, args.bind, args.port,
                          logger=lambda message: print(message, flush=True))
        server.ffmpeg = args.ffmpeg
        server.media_filter = "" if args.media == "both" else args.media
        server.channel_name = args.channel
        # The signal handler must set the flag the accept loop actually reads.
        for signum in (signal.SIGINT, signal.SIGTERM):
            signal.signal(signum, lambda *_: server.stop.set())
        # Each device connection starts its own transcoder process; an ffmpeg exit ends only that session, and the accept loop needs no restart.
        print_where_to_connect(args.bind, args.port, token)
        server.serve()
        # Print the counters only when something went wrong; connections abandoned during the handshake are not counted, and every channel switch produces one.
        if server.failed or server.rejected or server.dropped_video:
            print(f"Stopped. Errors: {server.failed}, rejected: {server.rejected}, "
                  f"dropped frames: {server.dropped_video}.", flush=True)
        else:
            print("Stopped.", flush=True)
        return 0
    except OSError as error:
        # A bind failure is hard to tell from token or media problems, and a busy port looks like a silent no-op; errno contains no paths or credentials.
        # Use errno.EADDRINUSE rather than a number: the numeric value differs between platforms.
        if error.errno == errno.EADDRINUSE:
            print("Port 8096 is already in use - most likely the previous run has not closed completely.", file=sys.stderr)
            print("Close the earlier window, or restart the computer and try again.", file=sys.stderr)
        else:
            print(f"Cannot open the network port (errno={error.errno}).", file=sys.stderr)
        return 1
    except Exception:
        # Subprocess errors, paths and environment variables may contain sensitive values and are not printed.
        print("The program could not start. Common causes: ffmpeg is missing or the channel list has a problem;", file=sys.stderr)
        print("send the last few lines of output above to the project maintainer to locate it.", file=sys.stderr)
        if os.environ.get("TV_TRACEBACK") == "1":
            import traceback
            traceback.print_exc()
        return 1


if __name__ == "__main__":
    raise SystemExit(main())
