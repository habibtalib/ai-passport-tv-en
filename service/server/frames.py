"""Indexed picture: frame striping, compression and packing.

The picture is 256-color palette indices, 1 byte per pixel, with each stripe deflate-compressed independently.
For geometry see GEOMETRIES: default 320x180, 12 rows per stripe, 15 stripes.

Video packet payload format (lengths are big-endian, in bytes of the compressed data that follows):

    [u8 first stripe][u8 stripe count][u16 length * stripe count][compressed stripes]

A stripe of length 0 means unchanged; the device does not redraw it.
"""

from __future__ import annotations

import os
import struct
import zlib

# Picture geometry: name -> (width, height, rows per stripe). Must match the geometry compiled into
# the firmware, otherwise the device rejects the session at CONFIG validation. All four have 15
# stripes; 320x180 is the 16:9 default, the rest are 4:3.
GEOMETRIES = {"320x240": (320, 240, 16), "280x210": (280, 210, 14),
              "240x180": (240, 180, 12), "320x180": (320, 180, 12)}
_NAME = os.environ.get("TV_GEOMETRY", "320x180")
if _NAME not in GEOMETRIES:
    raise SystemExit(f"TV_GEOMETRY={_NAME} is not a geometry this firmware has: "
                     f"choose one of {', '.join(sorted(GEOMETRIES))}")
WIDTH, HEIGHT, STRIPE_ROWS = GEOMETRIES[_NAME]
STRIPES = HEIGHT // STRIPE_ROWS
STRIPE_PIXELS = WIDTH * STRIPE_ROWS
FRAME_PIXELS = WIDTH * HEIGHT

# ffmpeg outputs rgb8: the fixed 3-3-2 grid is the palette (see LiveChannel.build_palette),
# each frame is exactly FRAME_PIXELS bytes, with no palette tail.
TRAILER_BYTES = 0
FRAME_BYTES = FRAME_PIXELS + TRAILER_BYTES

# Video packet limit; must equal firmware AV_VIDEO_MAX. An oversize packet ends the session; it is not truncated.
VIDEO_MAX = 22528

# Packing target, deliberately below VIDEO_MAX: an oversize packet leaves audio unread while it is
# read. See CLAUDE.md key design decision 3.
PACKET_TARGET_BYTES = int(os.environ.get("TV_PACKET_TARGET", "12288"))

# Must equal firmware AV_PALETTE_ENTRIES.
PALETTE_ENTRIES = 256

_HEADER = struct.Struct(">BB")

# deflate level; see CLAUDE.md key design decision 2.
_COMPRESS_LEVEL = 6


def compress_stripes(frame: bytes, send: list[bool] | None = None) -> list[bytes]:
    """Split into stripes and compress each; stripes with `send` False output an empty string (length 0, device does not redraw)."""
    if len(frame) != FRAME_PIXELS:
        raise ValueError(f"frame is {len(frame)} bytes, expected {FRAME_PIXELS}")
    return [
        zlib.compress(frame[at * STRIPE_PIXELS:(at + 1) * STRIPE_PIXELS], _COMPRESS_LEVEL)
        if send is None or send[at] else b""
        for at in range(STRIPES)
    ]


# Incremental send: only stripes that changed relative to what the device already shows. TV_DELTA=0 disables it.
# Relies on the firmware accepting zero-length stripes; older firmware ends the session on one.
# Change is measured by the number of differing indices, not by equality; stripes whose differing
# share is at most DELTA_MAX_DIFF are not sent, and the rest compete for the byte budget from
# largest difference down.
DELTA = os.environ.get("TV_DELTA", "1") != "0"
DELTA_MAX_DIFF = float(os.environ.get("TV_DELTA_MAX_DIFF", "0.01"))


class ByteBudget:
    """Picture byte rate and frame rate, and the per-frame byte target derived from them."""

    def __init__(self, rate: float, fps: float):
        self.rate, self.fps = float(rate), float(fps)

    def set_rate(self, rate: float) -> None:
        self.rate = float(rate)

    def set_fps(self, fps: float) -> None:
        self.fps = float(fps)

    def target(self) -> int:
        return int(self.rate / self.fps)


def _ladder() -> list[bytes]:
    """3-3-2 index mapping tables ordered coarse to fine, [0] lossless; each level rounds every color channel to a multiple of the step."""
    def table(rg: int, b: int) -> bytes:
        def snap(v: int, top: int, step: int) -> int:
            return min(top, (v + step // 2) // step * step)
        return bytes((snap(i >> 5, 7, rg) << 5) | (snap((i >> 2) & 7, 7, rg) << 2)
                     | snap(i & 3, 3, b) for i in range(256))
    return [bytes(range(256))] + [table(rg, b) for rg, b in
                                  ((2, 1), (3, 1), (3, 2), (4, 2), (7, 3))]


LADDER = _ladder()


def encode_within(raw: bytes, shown: bytes | None, tick: int, target: int,
                  min_diff: float = DELTA_MAX_DIFF) -> tuple[list[bytes], bytes, int]:
    """Compress `raw` to at most `target` bytes; returns (stripes to send, picture the device will show, level).

    Try as-is first, then lower quality level by level and take the first that fits; only if even the coarsest does not fit, drop stripes by amount of change.
    """
    chosen, drawn = [], raw
    for rung, table in enumerate(LADDER):
        drawn = raw.translate(table) if rung else raw
        chosen = choose_stripes(drawn, shown, tick, 1 << 30, min_diff)
        if _wire_size(chosen) <= target:
            return chosen, drawn, rung
    return (choose_stripes(drawn, shown, tick, target, min_diff), drawn,
            len(LADDER) - 1)


def _wire_size(chosen: list[bytes]) -> int:
    return _HEADER.size + 2 * len(chosen) + sum(map(len, chosen))


def differing_pixels(a: bytes, b: bytes) -> int:
    """Count differing indices between two frames: XOR then count zero bytes, standard library only."""
    x = int.from_bytes(a, "big") ^ int.from_bytes(b, "big")
    return len(a) - x.to_bytes(len(a), "big").count(0)


def _deflate(raw: bytes, at: int) -> bytes:
    return zlib.compress(raw[at * STRIPE_PIXELS:(at + 1) * STRIPE_PIXELS], _COMPRESS_LEVEL)


def choose_stripes(raw: bytes, shown: bytes | None, tick: int, allowed: int,
                   min_diff: float = DELTA_MAX_DIFF) -> list[bytes]:
    """Pick the compressed stripes worth sending within `allowed` bytes; unselected stripes are empty strings.

    `shown` is the picture the device already has; None means the first frame, send everything.
    One stripe is force-refreshed in rotation by `tick`, not counted against `allowed`, to repair
    stripes the device lost. The rest are sent in order of differing count versus `shown`, largest
    first, until they no longer fit; stripes left out need no bookkeeping, since the difference
    can only grow by the next frame.
    """
    if len(raw) != FRAME_PIXELS:
        raise ValueError(f"frame is {len(raw)} bytes, expected {FRAME_PIXELS}")
    if shown is None:
        return compress_stripes(raw)
    floor = int(STRIPE_PIXELS * min_diff)
    refresh = tick % STRIPES
    out = [b""] * STRIPES
    out[refresh] = _deflate(raw, refresh)
    spent = _HEADER.size + 2 * STRIPES + len(out[refresh])
    ranked = []
    for at in range(STRIPES):
        if at == refresh:
            continue
        lo, hi = at * STRIPE_PIXELS, (at + 1) * STRIPE_PIXELS
        changed = differing_pixels(raw[lo:hi], shown[lo:hi])
        if changed > floor:
            ranked.append((-changed, at))
    ranked.sort()
    for _, at in ranked:
        z = _deflate(raw, at)
        if spent + len(z) > allowed:
            continue
        out[at] = z
        spent += len(z)
    return out


def fill_stripes(raw: bytes, shown: bytes | None, chosen: list[bytes],
                 allowed: int) -> list[bytes]:
    """Beyond `chosen`, use the remaining `allowed` bytes to send skipped stripes that differ, largest difference first."""
    if shown is None:
        return chosen
    spent = _wire_size(chosen)
    ranked = []
    for at in range(STRIPES):
        if chosen[at]:
            continue
        lo, hi = at * STRIPE_PIXELS, (at + 1) * STRIPE_PIXELS
        changed = differing_pixels(raw[lo:hi], shown[lo:hi])
        if changed:
            ranked.append((-changed, at))
    ranked.sort()
    out = list(chosen)
    for _, at in ranked:
        z = _deflate(raw, at)
        if spent + len(z) > allowed:
            continue
        out[at] = z
        spent += len(z)
    return out


def apply_stripes(raw: bytes, shown: bytes | None, sent: list[bytes]) -> bytes:
    """The picture the device shows after receiving `sent`: `shown` overlaid with the matching stripes from `raw`."""
    merged = bytearray(shown if shown is not None else raw)
    for at, stripe in enumerate(sent):
        if stripe:
            lo = at * STRIPE_PIXELS
            merged[lo:lo + STRIPE_PIXELS] = raw[lo:lo + STRIPE_PIXELS]
    return bytes(merged)


def packet(first: int, compressed: list[bytes]) -> bytes:
    """Build one payload from consecutive compressed stripes."""
    if not compressed:
        raise ValueError("a packet carries at least one stripe")
    if first + len(compressed) > STRIPES:
        raise ValueError(f"stripes {first}+{len(compressed)} run past the frame")
    table = struct.pack(f">{len(compressed)}H", *(len(s) for s in compressed))
    payload = _HEADER.pack(first, len(compressed)) + table + b"".join(compressed)
    if len(payload) > VIDEO_MAX:
        raise ValueError(f"packet is {len(payload)} bytes, over the {VIDEO_MAX} limit")
    return payload


def frame_packets(frame: bytes) -> list[bytes]:
    """Compress and pack a whole frame."""
    return pack_stripes(compress_stripes(frame))


def pack_stripes(compressed: list[bytes]) -> list[bytes]:
    """Pack compressed stripes into multiple payloads by byte budget; an empty stripe takes 2 bytes of the length table.

    A single stripe over budget gets its own packet; over VIDEO_MAX it raises.
    """
    packets: list[bytes] = []
    run: list[bytes] = []
    run_bytes = 0
    at = 0
    for stripe in compressed:
        # The length table adds 2 bytes per stripe; the budget first subtracts the header and length table.
        overhead = _HEADER.size + 2 * (len(run) + 1)
        if run and overhead + run_bytes + len(stripe) > PACKET_TARGET_BYTES:
            packets.append(packet(at, run))
            at += len(run)
            run, run_bytes = [], 0
            overhead = _HEADER.size + 2
        if overhead + len(stripe) > VIDEO_MAX:
            raise ValueError(
                f"stripe {at + len(run)} is {len(stripe)} bytes and does not fit "
                f"in a {VIDEO_MAX}-byte packet even alone")
        run.append(stripe)
        run_bytes += len(stripe)
    if run:
        packets.append(packet(at, run))
    return packets


def read_exactly(stream, size: int) -> bytes:
    """Read `size` bytes from the pipe, possibly fewer at pipe end; a single pipe read may return short data, hence the loop."""
    data = bytearray()
    while len(data) < size:
        chunk = stream.read(size - len(data))
        if not chunk:
            break
        data.extend(chunk)
    return bytes(data)


def read_frame(stream, stop) -> bytes | None:
    """Read one frame from the rawvideo pipe; returns None at end of stream or on a partial frame, which is not padded."""
    if stop.is_set():
        return None
    raw = read_exactly(stream, FRAME_PIXELS)
    if len(raw) != FRAME_PIXELS:
        return None
    if TRAILER_BYTES:
        if len(read_exactly(stream, TRAILER_BYTES)) != TRAILER_BYTES:
            return None
    return raw


# --- Payload unpacking (for tests and verification) ---

def unpack(payload: bytes) -> list[bytes]:
    """Split a payload into decompressed stripes."""
    if len(payload) < _HEADER.size:
        raise ValueError("payload shorter than its own header")
    first, count = _HEADER.unpack_from(payload)
    if count == 0 or first + count > STRIPES:
        raise ValueError(f"stripe range {first}+{count} outside the frame")
    end = _HEADER.size + 2 * count
    if len(payload) < end:
        raise ValueError("payload truncated inside its length table")
    lengths = struct.unpack_from(f">{count}H", payload, _HEADER.size)
    if sum(lengths) != len(payload) - end:
        raise ValueError("length table does not match the bytes that follow")
    out = []
    at = end
    for n, length in enumerate(lengths):
        raw = zlib.decompress(payload[at:at + length])
        if len(raw) != STRIPE_PIXELS:
            raise ValueError(f"stripe {first + n} is {len(raw)} bytes, not {STRIPE_PIXELS}")
        out.append(raw)
        at += length
    return out


def palette_bytes(rgb24: bytes) -> bytes:
    """Convert 256 RGB triples to the big-endian RGB565 the device wants.

    ffmpeg's 3-3-2 grid scales by multiples of 36 and 85; after quantizing to RGB565 it matches the shift results below.
    """
    if len(rgb24) < PALETTE_ENTRIES * 3:
        raise ValueError("a palette is 256 RGB triples")
    out = bytearray()
    for i in range(PALETTE_ENTRIES):
        r, g, b = rgb24[3 * i], rgb24[3 * i + 1], rgb24[3 * i + 2]
        out += struct.pack(">H", ((r >> 3) << 11) | ((g >> 2) << 5) | (b >> 3))
    return bytes(out)


# --- Scaling and padding ---

# Color sampling and output share one filter chain, so both see the same padded picture.
SCALE_MODES = ("bicubic", "lanczos", "bilinear", "area", "neighbor")
SCALE = os.environ.get("TV_SCALE", "").strip() or "bicubic"
if SCALE not in SCALE_MODES:
    raise SystemExit(f"TV_SCALE={SCALE!r} is invalid; choices: {' / '.join(SCALE_MODES)}")
SCALER_FLAGS = SCALE
FIT = (f"scale=iw*sar:ih,setsar=1,"
       f"scale={WIDTH}:{HEIGHT}:force_original_aspect_ratio=decrease:flags={SCALER_FLAGS},"
       f"pad={WIDTH}:{HEIGHT}:(ow-iw)/2:(oh-ih)/2,setsar=1")

DEFAULT_USER_AGENT = "AptvPlayer-UA"


def input_options(url: str, user_agent: str = "", paced: bool = False,
                  loop: bool = False) -> list[str]:
    """ffmpeg input arguments for reading the source.

    The reconnect options work only for network sources; a local file fails to open on the unknown option.
    """
    options: list[str] = []
    if paced:
        # -re reads at native rate; otherwise the HLS window is decoded all at once, the queue fills quickly and supply stalls.
        options += ["-re", "-flags", "low_delay"]
    if url.startswith(("http://", "https://", "rtsp://", "rtmp://")):
        options += ["-reconnect", "1", "-reconnect_streamed", "1",
                    "-reconnect_delay_max", "5", "-rw_timeout", "15000000"]
    if loop or os.environ.get("TV_STREAM_LOOP") == "1":
        options += ["-stream_loop", "-1"]
    effective_ua = user_agent or (DEFAULT_USER_AGENT if url.startswith(("http://", "https://")) else "")
    if effective_ua:
        options += ["-user_agent", effective_ua]
    return options
