"""Live HLS transcoding: one ffmpeg process decodes once and yields palette-index pictures the device can draw plus 16 kHz mono PCM.

Pictures are scaled to the panel geometry and quantized to a fixed 256-color palette; audio and video each use a loopback TCP connection,
and the decoder's own pts is recorded by `showinfo` / `ashowinfo` (see server/pts.py).
Two reader threads cut packets, and the session sender schedules them from the same monotonic clock. The device does no scaling; it only decompresses stripes and looks colors up in the palette.

Channel URLs come only from the channel-table whitelist; streams are not recorded and no credentials are used.
"""

from __future__ import annotations

import collections
import json
import os
import queue
import re
import socket
import subprocess
import threading
import time
from pathlib import Path

from . import adpcm, frames, perceptual, pts
from .timeline import (BASIS_COMMON_DECODE, ContentTimeline,
                       SessionClock, SourceState, VERDICT_IN_HOLE,
                       VERDICT_PLACED)
from .media import AUDIO_CHUNK_MS, FPS, default_palette
from .protocol import AUDIO_PCM_BYTES, HEADER
# Upper byte limit of a control packet (CONFIG); change it together with firmware main/av_protocol.h.
TV_CONTROL_MAX = 7168
# Limits on channel count and channel id length, in sync with the firmware; the device rejects longer lists, so the server must limit them first.
TV_CHANNEL_MAX = 128
TV_CHANNEL_ID_MAX = 16
# Byte limit of one channel-table line, used only to bound how much of channels.txt is read; leaves headroom for the four-field form.
MAX_CHANNEL_LINE_BYTES = 512
# AUDIO_CHUNK_MS comes from media.py; this module must not redefine it.
AUDIO_RATE = 16000
# How many milliseconds the audio sender may run ahead of real time; the device-side queue depth is AUDIO_LEAD_MS plus this value (see CLAUDE.md key design decision 5).
# The depth must be below the flow-control line at which the device stops reading the socket, yet larger than the time to write one video packet, otherwise audio underruns;
# without device feedback only headroom can be left, the queue must not be filled. TV_AUDIO_LOOKAHEAD_MS can override it.
AUDIO_MAX_LOOKAHEAD_MS = int(os.environ.get("TV_AUDIO_LOOKAHEAD_MS", "280"))
# Audio queue capacity (in chunks): an HLS source emits seconds of audio in bursts per segment, so the queue must absorb a burst and last through the segment gap,
# at the cost of starting several seconds behind the live edge.
PCM_QUEUE_CHUNKS = int(os.environ.get("TV_PCM_QUEUE_CHUNKS", "400"))  # 40 ms per chunk, 16 seconds in total
# Picture queue capacity, converted to a frame count from content duration. The sender takes the oldest frame at the head, so the queue depth is how far the picture lags live;
# the depth must be comparable to the audio queue, and shrinking it alone only puts picture and sound out of sync (see pop_video).
VIDEO_QUEUE_SECONDS = float(os.environ.get("TV_VIDEO_QUEUE_S", "16"))

# Sound and picture content times differing by no more than this (seconds) count as the same moment; older pictures are dropped, newer ones wait.
VIDEO_SYNC_TOLERANCE_S = float(os.environ.get("TV_SYNC_TOLERANCE_S", "0.15"))

VIDEO_QUEUE_FRAMES = int(os.environ.get("TV_VIDEO_QUEUE",
                                        str(int(VIDEO_QUEUE_SECONDS * FPS) + 32)))
# Content duration (seconds) prebuffered before the first packet is sent: it is the wait on a channel switch and the only buffer margin against a source outage.
# A larger TV_PREBUFFER_S means slower switching but better tolerance of source outages. It is capped by both queue capacities,
# otherwise the prebuffer would never be reached and the session would end with PREBUFFER_TIMEOUT_S.
PREBUFFER_SECONDS = min(float(os.environ.get("TV_PREBUFFER_S", "3")),
                        PCM_QUEUE_CHUNKS * AUDIO_CHUNK_MS / 1000,
                        VIDEO_QUEUE_FRAMES / FPS)
PREBUFFER_CHUNKS = int(PREBUFFER_SECONDS * 1000 / AUDIO_CHUNK_MS)
# Frame count needed to fill PREBUFFER_SECONDS, converted using START_FPS: the prebuffer is a duration, and the source frame rate is not yet known.
from .rate import START_FPS as _START_FPS, start_rate

PREBUFFER_FRAMES = int(PREBUFFER_SECONDS * _START_FPS)
# Only a fallback for a source that never produces data; normal start is decided by the prebuffer target.
PREBUFFER_TIMEOUT_S = 60

# Time limit (seconds) for the decoder to connect back to the two loopback sockets. ffmpeg connects soon after starting; a timeout means the process did not come up, reported in start().
DECODER_CONNECT_TIMEOUT_S = 10

# Channel table: TV_CHANNELS_FILE points to a text file, or put channels.txt in the working directory; it is read once at startup.
# Each line is `id | display name | https://... [| user-agent]`; blank lines and '#' comment lines are ignored.
CHANNELS_FILE = "channels.txt"
CHANNELS_ENV = "TV_CHANNELS_FILE"
# Built-in fallback channels, using the TV stations' own public streams.
BUILTIN_CHANNELS = {
    "cgtn": ("CGTN", "https://english-livebkali.cgtn.com/live/encgtn.m3u8"),
    "france24": ("France 24",
                 "https://live.france24.com/hls/live/2037218/F24_EN_HI_HLS/master_500.m3u8"),
    "dw": ("DW English",
           "https://dwamdstream102.akamaized.net/hls/live/2015525/dwstream102/master.m3u8"),
    "tagesschau": ("Tagesschau24",
                   "https://tagesschau.akamaized.net/hls/live/2020115/tagesschau/tagesschau_1/master.m3u8"),
}

# Filled by load_channels(); the server and the CONFIG payload read these module-level names directly.
CHANNELS: dict[str, str] = {}
CHANNEL_LABELS: dict[str, str] = {}
# Mapping from channel id to User-Agent.
CHANNEL_AGENTS: dict[str, str] = {}
DEFAULT_CHANNEL = ""


def parse_channels(text: str) -> tuple[dict[str, str], dict[str, str]]:
    """Parse the channel table and reject entries the device would discard.

    The firmware skips empty, non-printable or over-long ids and reads only a limited number of channels (TV_CHANNEL_ID_MAX, TV_CHANNEL_MAX);
    an error here keeps channels from silently disappearing on the device.
    """
    channels: dict[str, str] = {}
    labels: dict[str, str] = {}
    agents: dict[str, str] = {}
    dropped = 0
    global CHANNEL_AGENTS
    for number, raw in enumerate(text.splitlines(), start=1):
        line = raw.strip()
        if not line or line.startswith("#"):
            continue
        parts = [part.strip() for part in line.split("|")]
        # The fourth field is the User-Agent the source requires; some mirror sources accept only a specific player and return 403 to the rest.
        if len(parts) == 3:
            key, label, url = parts
            agent = ""
        elif len(parts) == 4:
            key, label, url, agent = parts
        else:
            raise ValueError(f"line {number}: expected 'id | name | url [| user-agent]', "
                             f"got {len(parts)} fields")
        if not key or len(key) >= TV_CHANNEL_ID_MAX:
            raise ValueError(f"line {number}: id must be 1..{TV_CHANNEL_ID_MAX - 1} characters")
        if not all("!" <= char <= "~" for char in key):
            raise ValueError(f"line {number}: id must be printable ASCII")
        if key in channels:
            raise ValueError(f"line {number}: duplicate id {key!r}")
        # Channels beyond the device limit are only ignored with a warning at the end, not treated as fatal.
        if len(channels) >= TV_CHANNEL_MAX:
            dropped += 1
            continue
        if not label:
            raise ValueError(f"line {number}: missing display name")
        if not url.startswith(("http://", "https://")):
            raise ValueError(f"line {number}: url must start with http:// or https://")
        channels[key] = url
        labels[key] = label
        if agent:
            agents[key] = agent
        elif url.startswith(("http://", "https://")):
            agents[key] = frames.DEFAULT_USER_AGENT
    if not channels:
        raise ValueError("channel table contains no channels")
    # The count is within limits but the list exceeds the CONFIG packet limit, so the device receives no channels (after JSON escaping each CJK character takes 6 bytes);
    # like an excess count, this only warns and does not refuse to start.
    packet = len(json.dumps({"channel_list": [{"id": k, "name": labels[k]}
                                              for k in channels]},
                            ensure_ascii=True, separators=(",", ":")).encode("ascii"))
    if packet > TV_CONTROL_MAX or dropped:
        total = len(channels) + dropped
        # Too many channels and names that are too long are reported separately, so the operator is not misled.
        if dropped:
            print(f"Warning: channels.txt has {total} channels, the device accepts at most "
                  f"{TV_CHANNEL_MAX}.", flush=True)
            print(f"         The extra {dropped} are ignored; the device will show only the first "
                  f"{len(channels)}.", flush=True)
        if packet > TV_CONTROL_MAX:
            if not dropped:
                print(f"Warning: {total} channels, the count is fine but the channel names are too long.", flush=True)
            print(f"         The list sent to the device is {packet} bytes, over the device's "
                  f"{TV_CONTROL_MAX}-byte limit; the device may receive no channels at all.", flush=True)
            print(f"         Channel names average {packet // max(1, len(channels))} bytes; "
                  f"shortening channel names (especially CJK ones) is the effective fix.", flush=True)
        print("         Please edit channels.txt.", flush=True)
    return channels, labels, agents


def load_channels(path: Path | None = None) -> None:
    """Load the channel table from a file (given or found); use the built-in channels when there is no file.

    Called once at import; main() calls it again when it receives --channels-file.
    """
    global CHANNELS, CHANNEL_LABELS, CHANNEL_AGENTS, DEFAULT_CHANNEL
    source = path
    if source is None:
        environment = os.environ.get(CHANNELS_ENV)
        if environment:
            source = Path(environment)
        elif Path(CHANNELS_FILE).is_file():
            source = Path(CHANNELS_FILE)
        elif (Path(__file__).resolve().parent.parent / CHANNELS_FILE).is_file():
            source = Path(__file__).resolve().parent.parent / CHANNELS_FILE
    if source is None:
        channels = {key: url for key, (_, url) in BUILTIN_CHANNELS.items()}
        labels = {key: label for key, (label, _) in BUILTIN_CHANNELS.items()}
        agents = {}
    else:
        if not source.is_file():
            raise ValueError(f"channel file not found: {source}")
        # The read limit is set by the channel count and the per-line limit, not TV_CONTROL_MAX or VIDEO_MAX (those are single-packet wire limits):
        # a legal file is never truncated, and an oversized file stops at a known size.
        limit = TV_CHANNEL_MAX * MAX_CHANNEL_LINE_BYTES
        with source.open(encoding="utf-8", errors="strict") as handle:
            text = handle.read(limit + 1)
        if len(text) > limit:
            raise ValueError(f"{source} is larger than {limit} bytes; the device "
                             f"cannot hold more than {TV_CHANNEL_MAX} channels")
        channels, labels, agents = parse_channels(text)
    CHANNELS.clear()
    CHANNELS.update(channels)
    CHANNEL_LABELS.clear()
    CHANNEL_LABELS.update(labels)
    CHANNEL_AGENTS.clear()
    CHANNEL_AGENTS.update(agents)
    # Keep the original default channel if it is still in the table, otherwise take the first entry; a default channel not in the table would make every connection without a channel fail.
    if DEFAULT_CHANNEL not in channels:
        DEFAULT_CHANNEL = next(iter(channels))


# FoloTalk devices keep at most this many entries and parse CONFIG into a 4 KiB
# buffer; the list sent is the first TV_DEVICE_CHANNELS of channels.txt.
DEVICE_CHANNELS = int(os.environ.get("TV_DEVICE_CHANNELS", "40"))


def channel_list() -> list[dict]:
    keys = list(CHANNELS)[:DEVICE_CHANNELS]
    return [{"id": key, "name": CHANNEL_LABELS.get(key, key)} for key in keys]


load_channels()


class LiveError(RuntimeError):
    """Transcoding or packet cutting failed."""



# Samples per device audio chunk; the ffmpeg audio frame length is fixed to this value, so one `ashowinfo` line corresponds to exactly one chunk.
AUDIO_SAMPLES = AUDIO_PCM_BYTES // 2

# ffmpeg video filter appended after frames.FIT and before format=rgb8; empty by default.
PRE_FILTER = os.environ.get("TV_PRE_FILTER", "").strip()
# How a frame is degraded when it exceeds the byte target: a mode from perceptual.MODES (needs numpy), or "ladder" (reduce colors step by step, see frames.encode_within).
# Frames that fit are sent as they are.
DEGRADE_MODES = (*perceptual.MODES, "ladder")
DEGRADE = os.environ.get("TV_DEGRADE", "").strip() or "perceptual"
if DEGRADE not in DEGRADE_MODES:
    raise SystemExit(f"TV_DEGRADE={DEGRADE!r} is invalid, valid values: {' / '.join(DEGRADE_MODES)}")
if DEGRADE in perceptual.MODES and not perceptual.AVAILABLE:
    raise SystemExit(f"TV_DEGRADE={DEGRADE} needs numpy, please install it first (pip install -r requirements.txt)")
_VIDEO_CHAIN = f"{frames.FIT},{PRE_FILTER}" if PRE_FILTER else frames.FIT
# How leftover bytes are used once a frame is within the byte target: delta sends only stripes that changed by more than TV_DELTA_MAX_DIFF,
# fill also sends the skipped stripes; full compresses the complete original frame in the producer thread, and the budget only limits the send rate.
ENCODE_MODES = ("delta", "fill", "full")
ENCODE = os.environ.get("TV_ENCODE", "").strip() or "delta"
if ENCODE not in ENCODE_MODES:
    raise SystemExit(f"TV_ENCODE={ENCODE!r} is invalid, valid values: {' / '.join(ENCODE_MODES)}")
def source_graph(fps: int, profile=None) -> str:
    """A single filter graph with two outputs, picture and audio.

    format=rgb8 comes before showinfo so the recorded frame is the written frame; aformat=mono must come before asetnsamples,
    otherwise a stereo frame holds two device chunks, one ashowinfo line maps to two chunks, and timestamps and payloads are misaligned.
    """
    extra = profile.video_filter() if profile is not None else ''
    chain = f'{_VIDEO_CHAIN},{extra}' if extra else _VIDEO_CHAIN
    return (
        f"[0:v]setpts=PTS-STARTPTS,fps={fps},{chain},format=rgb8,showinfo[v];"
        f"[0:a]asetpts=PTS-STARTPTS,aresample={AUDIO_RATE},aformat=channel_layouts=mono,"
        f"asetnsamples=n={AUDIO_SAMPLES},ashowinfo[a]"
    )



def source_command(url: str, video_port: int, audio_port: int, ffmpeg: str,
                   user_agent: str = "", fps: int = FPS, *, loop: bool = False, profile=None) -> list[str]:
    """Build the ffmpeg command: read the source once, output picture and audio through two loopback TCP ports, with timestamps given by the showinfo log.

    The picture is quantized to ffmpeg's fixed 3-3-2 palette (same as firmware av_palette_rgb565) with dithering off (see CLAUDE.md key design decision 1);
    `-sws_dither none` must be an output option, placed before `-i` it is ignored.
    `-loglevel info` must not be removed, showinfo prints pts at INFO level; `-nostats` keeps progress lines out of the same output.
    Output goes to `tcp://127.0.0.1:<port>` rather than a pipe (see CLAUDE.md key design decision 7), ffmpeg is the client, and the parent must already be listening on both ports before the call.
    """
    return [
        ffmpeg, "-nostdin", "-hide_banner", "-loglevel", "info", "-nostats",
        *frames.input_options(url, user_agent, paced=True, loop=loop),
        "-i", url,
        "-filter_complex", source_graph(fps, profile),
        "-map", "[v]", "-pix_fmt", "rgb8", "-sws_dither", "none",
        "-threads", "1", "-f", "rawvideo", f"tcp://127.0.0.1:{video_port}",
        "-map", "[a]", "-f", "s16le", f"tcp://127.0.0.1:{audio_port}",
    ]



# Upper limit of the picture output frame rate, i.e. the device's fps limit (main/av_player.c, 1..30); faster sources are reduced below it, not rejected.
MAX_SOURCE_FPS = 30
# Time limit (seconds) for ffprobe frame-rate detection: it runs before the first frame of every channel switch, and on timeout playback uses the nominal frame rate.
FPS_PROBE_TIMEOUT_S = float(os.environ.get("TV_FPS_PROBE_TIMEOUT_S", "5"))
_FPS_CACHE: dict[str, int] = {}


def parse_rate(text: str) -> float | None:
    """Parse ffprobe frame-rate text (such as "25/1"); return None when it cannot be determined.

    "0/0" means unknown, common on live HLS, and is treated as None rather than an error.
    """
    try:
        num, _, den = str(text).partition("/")
        value = float(num) / float(den or 1)
    except (ValueError, ZeroDivisionError):
        return None
    return value if value > 0 else None


def choose_fps(avg: float | None, real: float | None) -> int | None:
    """Pick the output frame rate (an integer) from the frame rate reported by ffprobe.

    The average frame rate is preferred: when a 25 fps program is carried as 50 fields, r_frame_rate is 50 and avg_frame_rate is 25.
    With only a container frame rate above the limit, halve it repeatedly rather than clamping to the limit.
    """
    rate = avg if avg else real
    if not rate:
        return None
    while rate > MAX_SOURCE_FPS:
        rate /= 2
    return max(1, round(rate))


def probe_source_fps(url: str, ffmpeg: str, user_agent: str = "") -> int | None:
    """Probe the source picture frame rate; None if unknown (play at the nominal rate); the result is cached by URL."""
    if url in _FPS_CACHE:
        return _FPS_CACHE[url]
    probe = os.path.join(os.path.dirname(ffmpeg), "ffprobe")
    if not os.path.exists(probe):
        probe = "ffprobe"
    command = [probe, "-v", "error"]
    effective_ua = user_agent or (frames.DEFAULT_USER_AGENT if url.startswith(("http://", "https://")) else "")
    if effective_ua:
        command += ["-user_agent", effective_ua]
    command += ["-select_streams", "v:0",
                "-show_entries", "stream=avg_frame_rate,r_frame_rate",
                "-of", "json", url]
    try:
        done = subprocess.run(command, capture_output=True, text=True,
                              timeout=FPS_PROBE_TIMEOUT_S)
        if done.returncode != 0:
            return None
        streams = json.loads(done.stdout).get("streams", [])
    except (OSError, subprocess.SubprocessError, ValueError):
        return None
    if not streams:
        return None
    fps = choose_fps(parse_rate(streams[0].get("avg_frame_rate", "")),
                     parse_rate(streams[0].get("r_frame_rate", "")))
    if fps is not None:
        _FPS_CACHE[url] = fps
    return fps


class _QueuedFrame(list):
    """Packet list of one frame, with the original frame before packetization (TV_DELTA)."""
    raw: bytes | None = None


class LiveChannel:
    """A single channel session: palette, decoder process, and the bounded queues between it and the sender."""

    @property
    def full_frames(self) -> bool:
        return ENCODE == "full" or not frames.DELTA

    def __init__(self, url: str, ffmpeg: str = "ffmpeg", user_agent: str = ""):
        if url not in CHANNELS.values():
            raise ValueError("channel URL is not in the local allowlist")
        self.url = url
        self.ffmpeg = ffmpeg
        self.user_agent = user_agent
        self.loop_input = False
        self.profile = None
        self.stop = threading.Event()
        self.audio = collections.deque(maxlen=PCM_QUEUE_CHUNKS)
        self.video = collections.deque(maxlen=VIDEO_QUEUE_FRAMES)
        # Picture already sent to the device and the rotation counter for forced-refresh stripes (see frames.choose_stripes);
        # per channel, a new channel starts from a blank panel. Unused when delta is not enabled.
        self._shown: bytes | None = None
        self._delta_tick = 0
        self._snap_level = 0
        # Content timeline: alignment is based on decoder pts rather than arrival time (see server/timeline.py).
        self.timeline = ContentTimeline()
        # Mapping of content time to this session's on-wire clock; one per channel, and the device requires audio to start from zero in every session.
        self.session_clock = SessionClock(chunk_ms=AUDIO_CHUNK_MS)
        self.video_content: collections.deque = collections.deque(maxlen=VIDEO_QUEUE_FRAMES)
        self.audio_content: collections.deque = collections.deque(maxlen=PCM_QUEUE_CHUNKS)
        # Whether the source is still producing, kept separate from whether the link keeps up; see SourceState.
        self.source = SourceState()
        self._video_advanced = False
        self._audio_advanced = False
        # Picture frame rate, fixed within a session; the nominal value FPS before start() probes it, see resolve_fps.
        self.fps = FPS
        # Picture byte rate, set by the sender controller; each frame is compressed to within rate / fps, see frames.ByteBudget.
        self._budget = frames.ByteBudget(start_rate(FPS), FPS)
        self.lock = threading.Lock()
        self.error: Exception | None = None
        self.dropped_video = 0
        self.skipped_audio = 0
        self.produced_audio = 0
        self.produced_video = 0
        self.encoded_video_bytes = 0
        self.encode_seconds = 0.0
        self.coarse_frames = 0
        #: The only decoder process, producing both payloads and timestamps.
        self.decoder: subprocess.Popen | None = None
        self.timestamps: pts.Timestamps | None = None
        self.pts_reader: pts.Reader | None = None
        self.threads: list[threading.Thread] = []
        # The two loopback connections the decoder connects back on: file objects for reading, kept together with the underlying sockets so close() can release them deterministically.
        self._video: object | None = None
        self._audio: object | None = None
        self._video_sock: socket.socket | None = None
        self._audio_sock: socket.socket | None = None
        self._adpcm = adpcm.Encoder()
        self._raw_video: queue.Queue[bytes | None] = queue.Queue(maxsize=120)
        self._raw_audio: queue.Queue[bytes | None] = queue.Queue(maxsize=300)
        # Palette the device needs before drawing, filled by build_palette() and sent by the session before the first frame.
        self.palette: bytes | None = None



    def calibrate_from_source(self) -> bool:
        """Record the association of the two content clocks.

        A single decoder produces both timestamp streams, which are already on the same clock, so the offset is always zero; it is still called to record the basis
        (BASIS_COMMON_DECODE), since a session that cannot say where its time comes from should not be accepted.
        """
        return self.timeline.calibrate(0.0, 0.0, basis=BASIS_COMMON_DECODE)

    def build_palette(self) -> bytes:
        """Fixed 3-3-2 palette, not sampled from the source (see CLAUDE.md key design decision 1)."""
        self.palette = default_palette()
        return self.palette

    def _accept_decoder(self, listener: socket.socket) -> socket.socket:
        """Accept the decoder's connections; if the decoder has already exited (e.g. the source cannot be opened), fail at once without waiting for the full timeout."""
        deadline = time.monotonic() + DECODER_CONNECT_TIMEOUT_S
        listener.settimeout(0.1)
        while True:
            try:
                return listener.accept()[0]
            except socket.timeout:
                pass
            if self.decoder is not None and self.decoder.poll() is not None:
                raise ConnectionAbortedError("decoder exited")
            if time.monotonic() > deadline:
                raise TimeoutError("decoder connect timeout")

    def start(self) -> None:
        if self.palette is None:
            raise LiveError("start() before build_palette()")
        self.resolve_fps()
        # Picture and audio come from a single decoder, output through two loopback TCP connections (see source_command for why).
        # The parent listens on both ports before starting ffmpeg, and ffmpeg connects back as the client;
        # on failure all resources must be closed, otherwise repeated device reconnects exhaust sockets.
        video_listener = audio_listener = None
        video_sock = audio_sock = None
        success = False
        try:
            video_listener = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
            video_listener.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
            video_listener.bind(("127.0.0.1", 0))
            video_listener.listen(1)
            video_port = video_listener.getsockname()[1]

            audio_listener = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
            audio_listener.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
            audio_listener.bind(("127.0.0.1", 0))
            audio_listener.listen(1)
            audio_port = audio_listener.getsockname()[1]

            # stderr carries the timestamps over a pipe and is read continuously by a dedicated thread: when nobody reads the pipe ffmpeg blocks, which cannot be told apart from a source outage.
            self.timestamps = pts.Timestamps()
            self.decoder = subprocess.Popen(
                source_command(self.url, video_port, audio_port, self.ffmpeg,
                               self.user_agent, self.fps, loop=self.loop_input, profile=self.profile),
                stdin=subprocess.DEVNULL, stdout=subprocess.DEVNULL,
                stderr=subprocess.PIPE)
            self.pts_reader = pts.Reader(self.decoder.stderr, self.timestamps)
            self.pts_reader.start()
            self.calibrate_from_source()

            # ffmpeg opens both outputs before decoding anything, so a timeout here means the decoder did not come up, not that the channel is slow.
            try:
                video_sock = self._accept_decoder(video_listener)
                audio_sock = self._accept_decoder(audio_listener)
            except OSError as error:
                raise LiveError(
                    f"decoder did not connect: {type(error).__name__}") from None
            # The listening sockets have done their job; close them.
            video_listener.close()
            audio_listener.close()
            video_listener = audio_listener = None

            # Turn off the Nagle algorithm: small writes would be held up to 40 ms, exactly one audio chunk, adding random delay the sender cannot see.
            for sock in (video_sock, audio_sock):
                try:
                    sock.setsockopt(socket.IPPROTO_TCP, socket.TCP_NODELAY, 1)
                except OSError:
                    pass
            # Enlarge the video receive buffer (best effort): the default on some platforms cannot hold one raw frame; if setting fails there is only less margin during bursts.
            try:
                video_sock.setsockopt(socket.SOL_SOCKET, socket.SO_RCVBUF, 1048576)
            except OSError:
                pass

            self._video_sock = video_sock
            self._audio_sock = audio_sock
            self._video = video_sock.makefile("rb", buffering=0)
            self._audio = audio_sock.makefile("rb", buffering=0)

            self.threads = [
                threading.Thread(target=self._drain_video, daemon=True, name="live-video-drain"),
                threading.Thread(target=self._process_video, daemon=True, name="live-video-process"),
                threading.Thread(target=self._drain_audio, daemon=True, name="live-audio-drain"),
                threading.Thread(target=self._process_audio, daemon=True, name="live-audio-process"),
            ]
            for thread in self.threads:
                thread.start()
            success = True
        finally:
            for listener in (video_listener, audio_listener):
                if listener is not None:
                    try:
                        listener.close()
                    except OSError:
                        pass
            if not success:
                for sock in (video_sock, audio_sock):
                    if sock is not None:
                        try:
                            sock.close()
                        except OSError:
                            pass
                self.close()

    def _note(self, error: Exception) -> None:
        with self.lock:
            if self.error is None:
                self.error = error
        self.stop.set()

    def _decoder_time(self, kind: str) -> float:
        """Get the decoder timestamp (milliseconds) for the entry just read.

        Raise when it is unavailable and do not substitute another clock: timestamps must come from the decoder pts (CLAUDE.md core design principle 5).
        """
        seconds = self.timestamps.take(kind) if self.timestamps else None
        if seconds is None:
            if self.stop.is_set():
                return 0.0
            raise LiveError(
                f"the decoder produced a {kind} item without logging its "
                f"timestamp; the session cannot be timed and is being ended")
        return seconds * 1000.0

    def _drain_video(self) -> None:
        """Read the ffmpeg picture output into the in-memory queue immediately.

        Reading is decoupled from waiting on the timestamp log: once reading stalls, ffmpeg's writes block and the whole transcode freezes.
        """
        try:
            while not self.stop.is_set():
                frame = frames.read_frame(self._video, self.stop)
                if frame is None:
                    if not self.stop.is_set():
                        self._raw_video.put(None)
                    break
                while not self.stop.is_set():
                    try:
                        self._raw_video.put(frame, timeout=0.1)
                        break
                    except queue.Full:
                        continue
        except (OSError, ValueError) as error:
            self._note(error)

    def _process_video(self) -> None:
        try:
            while not self.stop.is_set():
                try:
                    frame = self._raw_video.get(timeout=0.1)
                except queue.Empty:
                    continue
                if frame is None:
                    if self.stop.is_set():
                        break
                    raise LiveError("transcode video pipe closed")
                self._push_video(frame, self._decoder_time("video"))
        except (LiveError, OSError, ValueError, pts.Misaligned) as error:
            self._note(error)

    def _drain_audio(self) -> None:
        """Read the ffmpeg audio output into the in-memory queue immediately."""
        try:
            while not self.stop.is_set():
                block = frames.read_exactly(self._audio, AUDIO_PCM_BYTES)
                if len(block) != AUDIO_PCM_BYTES:
                    if not self.stop.is_set():
                        self._raw_audio.put(None)
                    break
                while not self.stop.is_set():
                    try:
                        self._raw_audio.put(block, timeout=0.1)
                        break
                    except queue.Full:
                        continue
        except (OSError, ValueError) as error:
            self._note(error)

    def _process_audio(self) -> None:
        try:
            while not self.stop.is_set():
                try:
                    block = self._raw_audio.get(timeout=0.1)
                except queue.Empty:
                    continue
                if block is None:
                    if self.stop.is_set():
                        break
                    raise LiveError("transcode audio pipe closed")
                self._push_audio(self._adpcm.encode_pcm(block), self._decoder_time("audio"))
        except (LiveError, OSError, ValueError, pts.Misaligned) as error:
            self._note(error)



    def _push_video(self, frame: bytes, content_ms: float) -> None:
        """Enqueue one frame; `content_ms` is the frame timestamp given by the decoder, queued with the payload and not recomputed.

        The frame is enqueued only after being fully processed, so the sender never sees half a picture; when delta is not enabled it is compressed here, so as not to occupy the send thread.
        """
        encode_started = time.monotonic()
        if not self.full_frames:
            # In delta mode only the original frame is queued and stripes are chosen at send time from what was sent and the remaining byte budget;
            # deciding here would base the choice on a frame the queue may drop.
            packets = _QueuedFrame()
            packets.raw = frame
        else:
            # A standalone full frame does not depend on whether the previous frame was sent or drawn. Compression is done in the existing video producer thread,
            # outside the shared lock, so audio production and sending need not wait for the perceptual degradation search.
            packets = frames.frame_packets(frame)
        with self.lock:
            self.produced_video = getattr(self, "produced_video", 0) + 1
            self.encoded_video_bytes = getattr(self, "encoded_video_bytes", 0) + sum(map(len, packets))
            self.encode_seconds = getattr(self, "encode_seconds", 0.0) + time.monotonic() - encode_started
            # When the queue is full drop the oldest frame. The criterion is queue depth, not the frame interval: when the source is slower than the target there are no spare frames and nothing may be throttled.
            if len(self.video) == self.video.maxlen:
                self.video.popleft()
                self.dropped_video += 1
            self.video.append(packets)
            self.video_content.append(content_ms)
            self._video_advanced = True

    def _push_audio(self, block: bytes, content_ms: float) -> None:
        """Enqueue one audio chunk; `content_ms` is the decoder timestamp, not a chunk count (a count cannot show audio the decoder skipped)."""
        with self.lock:
            if len(self.audio) == self.audio.maxlen:
                self.audio.popleft()
                if self.audio_content:
                    self.audio_content.popleft()
                self.skipped_audio += 1
            self.audio.append(block)
            self.produced_audio += 1
            self.audio_content.append(content_ms)
            self._audio_advanced = True

    def pop_audio(self) -> tuple[bytes, int] | None:
        """Take the next audio chunk and its session timestamp for sending.

        The device requires each chunk's timestamp to be exactly the previous one plus one chunk length, so a chunk's session position is its sequence number;
        the content time is handed to `SessionClock.audio` to detect and measure source discontinuities. Picture timestamps must not be derived from this sequence number.
        """
        with self.lock:
            if not self.audio or not self.audio_content:
                # The two queues enter and leave together, so a non-empty payload with empty content time cannot occur; if it does, reject it and do not substitute a count for the timestamp.
                return None
            content = self.audio_content.popleft()
            return self.audio.popleft(), self.session_clock.audio(content)

    def picture_lag_s(self) -> float:
        """Seconds by which the head of the picture queue lags the head of the sound queue; positive means the picture lags.

        Both are content times; the difference reflects the depth difference between the two queues, not the lip-sync offset a viewer perceives.
        """
        with self.lock:
            if not self.video_content or not self.audio_content:
                return 0.0
            if not self.timeline.calibrated:
                return 0.0
            sound_at = self.timeline.audio_in_video_units(self.audio_content[0])
            return (self.video_content[0] - sound_at) / 1000.0


    def set_video_rate(self, rate: int) -> None:
        """Set the picture bytes-per-second budget, called once a second by the sender; the frame rate is not adjustable, so a slow link can only sacrifice detail."""
        with self.lock:
            self._budget.set_rate(rate)

    def resolve_fps(self) -> int:
        """Decide the picture frame rate once, before any content is produced.

        An explicit `TV_FPS` is used directly, otherwise the source frame rate is probed and the nominal value is used when probing fails;
        the value is used both for ffmpeg output and for the send cadence.
        """
        if self.profile is not None:
            fps = self.profile.fps
        elif "TV_FPS" in os.environ:
            fps = FPS
        else:
            fps = probe_source_fps(self.url, self.ffmpeg, self.user_agent) or FPS
        self.fps = fps
        with self.lock:
            self._budget.set_fps(fps)
            self._budget.set_rate(start_rate(fps))
        return fps

    def pop_video(self, keep: int = 0) -> tuple[list[bytes], int] | None:
        """Take the picture frame aligned with the sound content about to be sent, returning its packets and session timestamp, or None if there is none.

        Alignment is by content time, not arrival time (CLAUDE.md core design principle 5): frames older than the sound have already been heard past and are dropped;
        newer frames are not yet due and return None to wait, with sound as the time backbone. The frame's session timestamp is determined by its own content position,
        independent of the paired sound position; packets and timestamp are decided together here, and the sender does not derive them again.
        """
        with self.lock:
            if not self.video:
                return None
            if not self.audio_content and self.session_clock._prev_audio_content is None:
                return None
            if not self.timeline.usable:
                # Until the two clocks are associated there is no way to tell whether a frame and a chunk are the same moment, so no guessing.
                # The criterion is usable, not calibrated: an approximate mapping can be used for pairing, it just cannot be reported as a measurement.
                return None
            # The program moment the sound belongs to, converted to the picture clock for direct comparison.
            ref_audio = self.audio_content[0] if self.audio_content else self.session_clock._prev_audio_content
            head_audio = self.timeline.audio_in_video_units(ref_audio)
            # Drop an unplaceable frame and try the next one; handling only one frame per call would need several calls to clear consecutive frames inside a hole.
            while self.video and self.video_content:
                content = self.video_content[0]
                # Content older than this sound has already been heard by the viewer; drop it.
                if content < head_audio - VIDEO_SYNC_TOLERANCE_S * 1000:
                    self._give_up_frame()
                    continue
                # Not yet due: the frame matching this sound has not arrived.
                if content > head_audio + VIDEO_SYNC_TOLERANCE_S * 1000:
                    return None
                # The on-wire timestamp takes the frame's own content position, not that of the paired sound.
                frame_at = self.timeline.video_in_audio_units(content)
                # The head content time is passed in too, so the clock can detect a hole before later sound is taken.
                verdict, stamp = self.session_clock.placement(
                    frame_at, ref_audio)
                if verdict == VERDICT_IN_HOLE:
                    # The sender dropped this stretch of sound and the viewer never heard it, so there is no moment at which to draw this frame.
                    self._give_up_frame()
                    continue
                if verdict != VERDICT_PLACED:
                    # Not yet due, or the session has no anchor yet: both are waiting, not loss, and the frame stays in the queue.
                    return None
                self.video_content.popleft()
                packets = self._delta_encode(self.video.popleft())
                self.session_clock.note_pair(frame_at, ref_audio)
                return packets, stamp
            return None

    def _delta_encode(self, packets):
        """Turn a queued original frame into on-wire packets.

        Runs under the lock when sending: stripe choice depends on what was sent and the remaining byte budget, which are only fixed now; when delta is not enabled the packets are already compressed and returned as they are.
        """
        raw = getattr(packets, "raw", None)
        if raw is None:
            return packets
        started = time.monotonic()
        if DEGRADE in perceptual.MODES:
            chosen, drawn, rung = perceptual.encode_within(
                raw, self._shown, self._delta_tick, self._budget.target(), self._snap_level,
                mode=DEGRADE)
            self._snap_level = rung
        else:
            chosen, drawn, rung = frames.encode_within(raw, self._shown, self._delta_tick,
                                                       self._budget.target())
        if ENCODE == "fill":
            chosen = frames.fill_stripes(drawn, self._shown, chosen, self._budget.target())
        self._delta_tick += 1
        self._shown = frames.apply_stripes(drawn, self._shown, chosen)
        out = frames.pack_stripes(chosen)
        wire = sum(len(part) + HEADER.size for part in out)
        self.coarse_frames += rung > 0
        self.encoded_video_bytes += wire
        self.encode_seconds += time.monotonic() - started
        return out

    def _give_up_frame(self) -> None:
        """Drop the frame at the head of the picture queue; the caller holds the lock."""
        self.video.popleft()
        self.video_content.popleft()
        self.dropped_video += 1

    def source_state(self, queue_over_bound: bool = False) -> str:
        """Classify the window that just ended and clear its flags, called once by the sender before notifying the controller.

        Flags are consumed per window, so a stale True does not make an idle window look busy; `starved` keeps an empty window from being treated as spare capacity,
        and `congested` is the case the rate adjustment must respond to.
        """
        with self.lock:
            state = self.source.observe(self._video_advanced, self._audio_advanced,
                                        queue_over_bound)
            self._video_advanced = False
            self._audio_advanced = False
            return state

    @property
    def shown(self) -> bytes | None:
        """The picture currently shown on the device, None before the first frame."""
        with self.lock:
            return self._shown

    @shown.setter
    def shown(self, picture: bytes | None) -> None:
        with self.lock:
            self._shown = picture

    def has_data(self) -> bool:
        with self.lock:
            # Pairing is possible when the sound queue has data, or when sound is anchored and the picture queue has data.
            return bool(self.audio or (self.video and self.session_clock._prev_audio_content is not None))

    def audio_pending(self) -> bool:
        with self.lock:
            return bool(self.audio)

    def video_pending(self) -> bool:
        with self.lock:
            return bool(self.video)

    def failure(self) -> Exception | None:
        with self.lock:
            return self.error

    def prebuffered(self) -> bool:
        with self.lock:
            return len(self.audio) >= PREBUFFER_CHUNKS and len(self.video) >= PREBUFFER_FRAMES


    @staticmethod
    def _sanitize_diagnostics(text: str) -> str:
        """Remove tokens, passwords and sensitive parameters from diagnostic text."""
        # Erase URL query parameters (such as ?token=...)
        text = re.sub(r"([?&][a-zA-Z0-9_.-]+=)[^\s&'\"<>]+", r"\1<redacted>", text)
        # Erase user:pass in URLs
        text = re.sub(r"(https?://)([^:@\s/]+:[^:@\s/]+@)", r"\1<auth>@", text)
        return text


    def diagnostics(self) -> str:
        """Transcoding error text for logs: only ffmpeg stderr, no media content."""
        parts = []
        error = self.failure()
        if error is not None:
            parts.append(type(error).__name__)
        proc = getattr(self, "decoder", None)
        if proc is not None and proc.poll() is not None:
            parts.append(f"decoder_exit={proc.returncode}")
        pts_reader = getattr(self, "pts_reader", None)
        if pts_reader is not None and pts_reader.error is not None:
            parts.append(f"pts_reader_error={type(pts_reader.error).__name__}")
        ts = getattr(self, "timestamps", None)
        if ts is not None and ts.tail:
            tail_lines = [line.decode("utf-8", errors="replace").strip() for line in ts.tail]
            tail_text = " ".join(line for line in tail_lines if line)
            if tail_text:
                tail_text = self._sanitize_diagnostics(tail_text)
                parts.append(f"decoder: {tail_text.replace(chr(10), ' ')[-300:]}")
        return " | ".join(parts)


    def close(self) -> None:
        self.stop.set()
        ts = getattr(self, "timestamps", None)
        if ts is not None:
            try:
                ts.close()
            except Exception:
                pass
        # Close the file objects first so the reader threads see EOF, then close the sockets explicitly: closing only the file objects does not release the fd until garbage collection.
        for stream in (getattr(self, "_video", None), getattr(self, "_audio", None)):
            if stream is not None:
                try:
                    stream.close()
                except OSError:
                    pass
        self._video = None
        self._audio = None
        for sock in (getattr(self, "_video_sock", None), getattr(self, "_audio_sock", None)):
            if sock is not None:
                try:
                    sock.close()
                except OSError:
                    pass
        self._video_sock = None
        self._audio_sock = None
        proc = getattr(self, "decoder", None)
        if proc is not None:
            if proc.poll() is None:
                try:
                    proc.terminate()
                except OSError:
                    pass
            try:
                proc.wait(timeout=1.0)
            except subprocess.TimeoutExpired:
                try:
                    proc.kill()
                    proc.wait(timeout=1.0)
                except OSError:
                    pass
            except OSError:
                pass
        all_threads = list(getattr(self, "threads", []))
        pts_reader = getattr(self, "pts_reader", None)
        if pts_reader is not None:
            all_threads.append(pts_reader)
        for thread in all_threads:
            if thread.is_alive():
                try:
                    thread.join(timeout=1.0)
                except Exception:
                    pass
        if proc is not None and getattr(proc, "stderr", None) is not None:
            try:
                proc.stderr.close()
            except OSError:
                pass
