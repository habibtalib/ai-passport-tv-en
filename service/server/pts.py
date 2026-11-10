"""Separate audio and video timestamps, taken from the same decoder process that produces them.

One ffmpeg process decodes once and outputs both payloads; `showinfo` and `ashowinfo` in its
filter graph record the `pts_time` of every entry. Timestamps and payloads come from the same
decode on the same timeline, so no offset needs estimating and gaps in the source are visible.

Each entry has one log line, checked by sequence number. `showinfo` logs before writing out, so
the matching line is always ready once the payload has been read. Each line carries `n`, which the
reader compares with the number of entries read; a mismatch means a line was lost and every later
timestamp would be off by one entry, so the reader must refuse to continue. `asetnsamples=n=640`
plus forced mono makes one log line correspond to exactly one device block.
"""

from __future__ import annotations

import collections
import re
import threading

# ffmpeg outputs for each entry:
#   [Parsed_showinfo_3 @ 0x...] n:   0 pts:      0 pts_time:0       duration: ...
#   [Parsed_ashowinfo_6 @ 0x...] n:0 pts:0 pts_time:0 fmt:s16 channels:1 ...
# The two lines have the same field shape and differ only by filter name; `Parsed_ashowinfo` does not contain `Parsed_showinfo`.
VIDEO_FILTER = b"Parsed_showinfo"
AUDIO_FILTER = b"Parsed_ashowinfo"
_FIELDS = re.compile(rb"\] n:\s*(\d+)\s+pts:\s*(-?\d+)\s+pts_time:([-\d.eE+]+)")


def parse(line: bytes) -> tuple[str, int, float] | None:
    """Return (stream kind, sequence number, time); None if the line is not a target line.

    `showinfo` also prints lines with the same prefix but no `n:`; the full field sequence is required to exclude them.
    """
    if AUDIO_FILTER in line:
        kind = "audio"
    elif VIDEO_FILTER in line:
        kind = "video"
    else:
        return None
    found = _FIELDS.search(line)
    if found is None:
        return None
    return kind, int(found.group(1)), float(found.group(3))


class Misaligned(RuntimeError):
    """The decoder's sequence number does not match the entries read; all later timestamps would be misaligned, so it is rejected rather than repaired."""


class Timestamps:
    """Timestamps stored in decoder output order; after reading entry N, take the Nth timestamp and check the sequence number."""

    def __init__(self) -> None:
        self._condition = threading.Condition()
        self._video: collections.deque[tuple[int, float]] = collections.deque()
        self._audio: collections.deque[tuple[int, float]] = collections.deque()
        self._expected = {"video": 0, "audio": 0}
        self.received = {"video": 0, "audio": 0}
        self._closed = False
        # Keep the decoder's most recent log lines for diagnosis when a session ends abnormally.
        self.tail: collections.deque[bytes] = collections.deque(maxlen=64)

    def note(self, line: bytes) -> None:
        """Record one log line; called from the stderr reader thread."""
        self.tail.append(line)
        found = parse(line)
        if found is None:
            return
        kind, index, seconds = found
        with self._condition:
            queue = self._video if kind == "video" else self._audio
            queue.append((index, seconds))
            self.received[kind] += 1
            self._condition.notify_all()

    def take(self, kind: str, timeout: float = 5.0) -> float | None:
        """Return the next timestamp (seconds) for `kind`; None on timeout, or when closed with no data.

        The filter logs before the payload is written, so normally the line has arrived by the time the
        payload is read and this does not block; the timeout only covers a stopped decoder, so a
        stalled source cannot hang the server.
        """
        with self._condition:
            queue = self._video if kind == "video" else self._audio
            deadline = _now() + timeout
            while not queue and not self._closed:
                remaining = deadline - _now()
                if remaining <= 0:
                    return None
                self._condition.wait(remaining)
            if not queue:
                return None
            index, seconds = queue.popleft()
            expected = self._expected[kind]
            if index != expected:
                raise Misaligned(
                    f"{kind} timestamp index {index} arrived where {expected} "
                    f"was due; a decoder log line was lost and every later "
                    f"timestamp would be one item out")
            self._expected[kind] += 1
            return seconds

    def close(self) -> None:
        """Mark closed and wake any thread waiting in take()."""
        with self._condition:
            self._closed = True
            self._condition.notify_all()



def _now() -> float:
    import time
    return time.monotonic()


class Reader(threading.Thread):
    """Continuously read the decoder's stderr and write it into `Timestamps`.

    Needs its own thread: ffmpeg blocks when nobody reads the log, which looks the same as a stalled source.
    """

    def __init__(self, stream, timestamps: Timestamps):
        super().__init__(daemon=True, name="decoder-log")
        self._stream = stream
        self._timestamps = timestamps
        self.error: Exception | None = None

    def run(self) -> None:
        try:
            for line in iter(self._stream.readline, b""):
                self._timestamps.note(line)
        except (OSError, ValueError) as error:
            self.error = error
