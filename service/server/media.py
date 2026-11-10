"""Send-timing constants and the fixed palette."""

from __future__ import annotations

import os

from . import frames

# Nominal frame rate declared in CONFIG; not the live channel's actual send rate. The device only
# checks that it is sane (1..30) and does not schedule by it: every frame carries its own
# timestamp and is presented on the audio clock. A live channel's frame rate is probed with ffprobe
# at startup (`LiveChannel.fps`) and held for the whole session; setting `TV_FPS` explicitly
# overrides the probe and forces one rate.
FPS = int(os.environ.get("TV_FPS", "25"))
DURATION_MS = 10000
AUDIO_CHUNK_MS = 40
# Audio send lead: how far the sender runs ahead of the wall clock; sets the steady-state depth
# of the device audio queue. See CLAUDE.md key design decision 5. Adjustable via env var.
AUDIO_LEAD_MS = int(os.environ.get("TV_AUDIO_LEAD_MS", "320"))
# Video send lead, set independently of audio: the audio lead sets device queue depth, while the
# video lead only decides how early a frame is released before its own slot; too large a video
# lead bunches up with audio and drops more frames.
VIDEO_LEAD_MS = int(os.environ.get("TV_VIDEO_LEAD_MS", "200"))
START_DELAY_MS = 200


def default_palette() -> bytes:
    """Fixed 3-3-2 palette that live-picture indices point to.

    Steps of 36 and 85 match ffmpeg's AV_PIX_FMT_RGB8; after quantizing to RGB565 all 256 entries are identical.
    """
    return frames.palette_bytes(bytes(
        value
        for k in range(256)
        for value in (36 * ((k >> 5) & 7), 36 * ((k >> 2) & 7), 85 * (k & 3))
    ))
