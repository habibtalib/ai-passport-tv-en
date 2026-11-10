"""Byte-rate control for a live session.

The source frame rate is fixed for the session; what varies is the picture byte rate (`ByteRate`). In full-frame mode FramePacer
lowers the send frequency to meet the budget, while incremental mode adjusts the per-frame budget. The controller takes the previous window's
send cost (slowest write and number of abandoned frames) and yields the byte rate for `frames.ByteBudget`;
it holds no socket, clock or cross-session state.
"""

from __future__ import annotations

import os

# Prebuffering converts to a reserve using this nominal frame rate.
START_FPS = int(os.environ.get("TV_START_FPS", "5"))

# Write time (ms): above SLOW the link is judged backlogged, below FAST it has headroom; in between is the hysteresis band.
SLOW_WRITE_MS = 250.0
FAST_WRITE_MS = 120.0

# Number of consecutive relaxed windows before the byte rate is raised; lowering takes effect immediately.
WINDOWS_BEFORE_UP = 2


# Adaptive byte rate is off by default; TV_ADAPTIVE=1 enables it.
ADAPTIVE = os.environ.get("TV_ADAPTIVE", "0") == "1"


# --- Frame rate is fixed, byte rate is variable ---
#
# The target is per frame: byte rate = FRAME_BYTES * fps, used as the session's fixed rate when adaptive is off;
# TV_RATE_START sets the byte rate directly and does not change with frame rate.
# MIN and MAX only bound the adaptive range; MAX defaults to the start value and only probes upward with TV_ADAPTIVE=1 and a higher setting.
FRAME_BYTES = int(os.environ.get("TV_FRAME_BYTES", "20000"))
MIN_RATE_BPS = int(os.environ.get("TV_RATE_MIN", "100000"))


def start_rate(fps: int) -> int:
    """Fixed byte rate at `fps`."""
    explicit = os.environ.get("TV_RATE_START")
    return int(explicit) if explicit else FRAME_BYTES * fps


def max_rate(fps: int) -> int:
    explicit = os.environ.get("TV_RATE_MAX")
    return max(int(explicit), start_rate(fps)) if explicit else start_rate(fps)


RATE_STEP_DOWN = 0.85
RATE_STEP_UP = 1.05


class FramePacer:
    """Byte budget a full frame takes; does not block audio and does not bank burst credit from idle periods.

    The first frame may go immediately, then the next frame waits until that frame's byte debt is paid. When the budget is short,
    the content timeline skips stale frames; every picture actually sent is still complete and independent.
    """

    def __init__(self, rate: int, now: float):
        self.rate = rate
        self.debt = 0.0
        self.updated = now

    def _advance(self, now: float) -> None:
        self.debt = max(0.0, self.debt - max(0.0, now - self.updated) * self.rate)
        self.updated = now

    def ready(self, now: float) -> bool:
        self._advance(now)
        return self.debt == 0.0

    def sent(self, wire_bytes: int, now: float) -> None:
        self._advance(now)
        self.debt += wire_bytes

    def set_rate(self, rate: int, now: float) -> None:
        self._advance(now)
        self.rate = rate


class ByteRate:
    """Decide the picture byte rate; the frame rate is constant.

    Based on the slowest write in the window and the frames the sender itself abandoned: lowers immediately, raises only after
    `WINDOWS_BEFORE_UP` consecutive relaxed windows. Raising also requires the window to use at least 80% of the byte rate,
    since a still picture sends almost no data and fast writes alone cannot prove the link has headroom.
    """

    def __init__(self, fps: int, start: int | None = None,
                 minimum: int | None = None, maximum: int | None = None,
                 adaptive: bool = True):
        if fps < 1:
            raise ValueError("a frame rate below one is not a rate")
        if start is None:
            start = start_rate(fps)
            minimum = min(MIN_RATE_BPS, start) if minimum is None else minimum
            maximum = max_rate(fps) if maximum is None else maximum
        minimum = MIN_RATE_BPS if minimum is None else minimum
        maximum = max(start, max_rate(fps)) if maximum is None else maximum
        if not 0 < minimum <= start <= maximum:
            raise ValueError("start rate is outside the range")
        self.fps = fps
        self.rate = start
        self.minimum, self.maximum = minimum, maximum
        self.adaptive = adaptive
        self.bytes = 0
        self.worst_write_ms = 0.0
        self.dropped = 0
        self.window_s = 1.0
        self.reason = "starting" if adaptive else "fixed"
        self._comfortable = 0
        self._dropped_last_window = False

    def observe(self, video_bytes: int, worst_write_ms: float,
                dropped: int = 0, frames: int = 0,
                window_s: float = 1.0) -> int:
        """Record one window's measurements and return the constant frame rate; see `self.rate` for the byte rate."""
        self.bytes, self.worst_write_ms, self.dropped = video_bytes, worst_write_ms, dropped
        self.window_s = max(0.05, window_s)
        drop_running = bool(dropped) and self._dropped_last_window
        self._dropped_last_window = bool(dropped)
        if not self.adaptive:
            return self.fps
        measured = bool(frames or video_bytes or worst_write_ms > 0.0 or dropped)
        if not measured:
            # An empty window means the source stopped supplying, not that the link has headroom.
            self._comfortable = 0
            self.reason = "no measurement in the window; holding"
            return self.fps
        achieved = video_bytes / self.window_s
        if worst_write_ms > SLOW_WRITE_MS or drop_running:
            self.reason = ("write was %.0f ms" % worst_write_ms
                           if worst_write_ms > SLOW_WRITE_MS
                           else "dropped %d frame%s, second window running"
                           % (dropped, "" if dropped == 1 else "s"))
            self._comfortable = 0
            if self.rate > self.minimum:
                # One slow write does not establish link capacity: startup,
                # encoding, or an isolated pause can leave a partial window.
                self.rate = max(self.minimum, int(self.rate * RATE_STEP_DOWN))
            else:
                self.reason += "; already at the floor"
            return self.fps
        if dropped:
            # A single isolated dropped frame is a normal burst near the operating point.
            self.reason = "dropped %d frame%s" % (dropped, "" if dropped == 1 else "s")
            return self.fps
        if worst_write_ms < FAST_WRITE_MS and achieved >= self.rate * 0.8:
            self._comfortable += 1
            if self._comfortable >= WINDOWS_BEFORE_UP and self.rate < self.maximum:
                self.rate = min(self.maximum, int(self.rate * RATE_STEP_UP))
                self._comfortable = 0
                self.reason = "used the rate and wrote fast for %d windows" % WINDOWS_BEFORE_UP
                return self.fps
            self.reason = "comfortable (%d/%d)" % (self._comfortable, WINDOWS_BEFORE_UP)
        else:
            self._comfortable = 0
            self.reason = "holding"
        return self.fps

    def describe(self) -> str:
        return (f"fps={self.fps} rate={self.rate // 1000}kB/s "
                f"video={self.bytes // 1000}kB/{self.window_s:.2f}s "
                f"worst_write={self.worst_write_ms:.0f}ms "
                f"dropped={self.dropped} ({self.reason})")
