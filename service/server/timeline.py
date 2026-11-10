"""Content time, session time, and pairing of picture with sound.

Content time is an item's position in the program, taken from the decoder's own pts (see pts.py) and independent of arrival time,
so delivery delay never changes which sound a picture belongs to.

Both streams come from the same ffmpeg process and share one clock, so the offset is zero; each session records it once through `calibrate()` with
`BASIS_COMMON_DECODE`. `ContentTimeline` holds that offset and decides whether the picture is early, late relative to the sound,
or in sync; `SessionClock` maps content time to the wire timestamps the device requires.

This module only does timestamp arithmetic; it involves no sockets or ffmpeg.
"""
from __future__ import annotations



# How far apart two content times may be and still count as the same moment: one audio chunk, the sound's slicing granularity.
DEFAULT_TOLERANCE_MS = 40

# Where the offset came from, recorded alongside the offset; see `calibrate`.
# The server uses `BASIS_COMMON_DECODE`: both streams come from the same ffmpeg process with the same clock, so the offset is zero.
BASIS_COMMON_DECODE = "one decode, the decoder's own timestamps"
BASIS_MEDIA_START = "media stream start times"

# How certain the relation between the two clocks is: not established, established from a non-media source, established from the media's own timestamps.
# Only the last may claim to be synchronized; `APPROXIMATE` is still usable for pairing but must not be taken as a measurement.
STATE_UNKNOWN = "unknown"
STATE_APPROXIMATE = "approximate"
STATE_FROM_MEDIA = "from media timestamps"


def state_of_basis(basis: str) -> str:
    """Calibration state for a given basis; only the two media-derived bases count as synchronized."""
    if basis in (BASIS_COMMON_DECODE, BASIS_MEDIA_START):
        return STATE_FROM_MEDIA
    return STATE_APPROXIMATE


class ContentTimeline:
    """Content clocks of the two streams, and the offset between them.

    The offset is set once by `calibrate()` and then frozen; until then `offset_ms` is None and any pairing that needs it is refused rather than guessed.
    """

    def __init__(self, tolerance_ms: float = DEFAULT_TOLERANCE_MS):
        self.tolerance_ms = float(tolerance_ms)
        # Audio content time minus video content time.
        self.offset_ms: float | None = None
        # Calibration note, written to the log.
        self.calibration_note = "not attempted"
        # Where the offset came from; see the BASIS_* constants.
        self.basis = ""
        self.recalibrations_refused = 0
        self.state = STATE_UNKNOWN

    # -- Calibration -------------------------------------------------------

    def calibrate(self, video_start_s: float, audio_start_s: float,
                  basis: str = BASIS_MEDIA_START) -> bool:
        """Record the offset between the streams; takes effect only once, repeat calls are refused and counted in `recalibrations_refused`.

        `audio_start_s` and `video_start_s` are each stream's first timestamp on the common clock; the server passes
        0.0 with `BASIS_COMMON_DECODE`. Returns True if this call set the offset, False if it was already set.
        """
        if self.offset_ms is not None:
            self.recalibrations_refused += 1
            return False
        self.offset_ms = (float(audio_start_s) - float(video_start_s)) * 1000.0
        self.basis = basis
        self.state = state_of_basis(basis)
        self.calibration_note = f"calibrated from {basis}"
        return True

    @property
    def calibrated(self) -> bool:
        """Whether the relation comes from the media's own timestamps; this is the basis for claiming synchronization, see `usable` for whether pairing may be used."""
        return self.state == STATE_FROM_MEDIA

    @property
    def usable(self) -> bool:
        """Whether the mapping may be used to place items; approximate values count as usable."""
        return self.state != STATE_UNKNOWN

    # -- Content time ------------------------------------------------------



    def audio_in_video_units(self, audio_content_ms: float) -> float:
        """Convert audio content time to the video clock: add `offset_ms`; `video_in_audio_units` is its inverse.

        For a single decode the offset is 0 and both are identity transforms.
        """
        if self.offset_ms is None:
            raise ValueError("the two streams have not been related yet")
        return audio_content_ms + self.offset_ms

    def video_in_audio_units(self, video_content_ms: float) -> float:
        if self.offset_ms is None:
            raise ValueError("the two streams have not been related yet")
        return video_content_ms - self.offset_ms

    # -- Judgments the queues need -----------------------------------------


    def verdict(self, video_content_ms: float | None,
                audio_content_ms: float | None) -> str:
        """Decide which stream is ahead and return a word suitable for logging.

        - `"no-video"` / `"no-audio"`: the corresponding queue is empty, nothing to pair.
        - `"unrelated"`: both streams have content but the offset is not recorded; refuse rather than guess.
        - `"video-early"`: picture is ahead of the sound; wait.
        - `"video-late"`: picture is behind; the moment has passed.
        - `"together"`: within tolerance.
        """
        if video_content_ms is None:
            return "no-video"
        if audio_content_ms is None:
            return "no-audio"
        if self.offset_ms is None:
            return "unrelated"
        # Positive: the picture at the queue head is newer than the current sound, not yet time, wait; negative: stale, cannot be recovered.
        delta = video_content_ms - self.audio_in_video_units(audio_content_ms)
        if delta > self.tolerance_ms:
            return "video-early"
        if delta < -self.tolerance_ms:
            return "video-late"
        return "together"


class SourceState:
    """Tell whether an empty-picture window is the source stopping or link congestion.

    From the sender's side both look like an empty window, but the right response is opposite. The distinction is whether content is still arriving:
    when the source stops, neither counter advances; under link congestion the counters advance as usual while the send queue grows and no data goes out.
    """

    #: Content is arriving normally and the queue is within bounds.
    FLOWING = "flowing"
    #: Content has stopped arriving; this window has no usable measurement, so the rate is not adjusted.
    STARVED = "starved"
    #: Content is arriving but the queue is over bound; the rate must be lowered.
    CONGESTED = "congested"

    def __init__(self, stalled_windows_before_starved: int = 2):
        self.stalled_windows_before_starved = stalled_windows_before_starved
        self._stalled = 0
        self.state = self.FLOWING
        self.note = "start"

    def observe(self, video_advanced: bool, audio_advanced: bool,
                queue_over_bound: bool) -> str:
        """Classify the window that just ended.

        `video_advanced` and `audio_advanced` say whether that stream produced output; `queue_over_bound` says whether the send queue is over bound.
        A single quiet window is not a stall: HLS sources arrive in bursts per segment, so several consecutive windows are needed to call it stalled.
        """
        if video_advanced or audio_advanced:
            self._stalled = 0
            if queue_over_bound:
                self.state = self.CONGESTED
                self.note = "content arriving and the queue is over its bound"
            else:
                self.state = self.FLOWING
                self.note = "content arriving"
            return self.state
        self._stalled += 1
        if self._stalled >= self.stalled_windows_before_starved:
            self.state = self.STARVED
            self.note = f"no content for {self._stalled} windows"
        else:
            # Insufficient evidence: this is a window with no measurement and must not be reported to the controller as flowing.
            self.state = self.FLOWING if not queue_over_bound else self.CONGESTED
            self.note = f"quiet window {self._stalled}/{self.stalled_windows_before_starved}"
        return self.state

    @property
    def measured(self) -> bool:
        """Whether the previous window carried a measurement usable for rate decisions."""
        return self.state != self.STARVED


# The mapping's verdict for one picture:
#   PLACED: a session timestamp was obtained;
#   IN_HOLE: the content falls in a sound interval the sender dropped; drop the frame;
#   NOT_YET: the corresponding sound has not been taken yet; keep the frame and wait;
#   NO_ANCHOR: no sound has been taken yet, so session time is undefined.
VERDICT_PLACED = "placed"
VERDICT_IN_HOLE = "in-dropped-sound"
VERDICT_NOT_YET = "sound-not-taken-yet"
VERDICT_NO_ANCHOR = "no-sound-taken"


class SessionClock:
    """Map content time to wire time piecewise.

    Content time is an item's position in the program; it does not start at 0 and varies with the source. Session time is the position in what has been heard, starting at 0,
    advancing exactly one chunk length for every chunk received; the device requires each audio timestamp to equal the previous one plus one chunk length.

    Sound the sender dropped was never heard: it occupies content time but not session time, so the mapping is piecewise: every discontinuity starts
    a new segment, within which session time runs parallel to content time.

    A picture's timestamp is its own content position converted to session time. A correct timestamp is necessary but not sufficient:
    the moment it is actually drawn also depends on sending, buffering, frame drops and device playback. This module does not measure the offset between what the viewer sees and hears,
    and `diagnostics()` is not such a measurement.

    `audio_items` is the number of audio chunks taken so far; the sender uses it to pace itself.
    """

    __slots__ = ("chunk_ms", "tolerance_ms", "audio_items", "video_items", "last_video_ms",
                 "pairs", "residual_sum_ms", "residual_worst_ms",
                 "content_gap_ms", "audio_gaps", "frames_unplaceable",
                 "frames_in_hole", "last_verdict",
                 "_segments", "_prev_audio_content")

    def __init__(self, chunk_ms: float, tolerance_ms: float = 350.0):
        if chunk_ms <= 0:
            raise ValueError("chunk_ms must be positive")
        self.chunk_ms = float(chunk_ms)
        # How far a picture's session position may lead the sound position at that moment; same value as the sender's pairing tolerance, used as the out-of-range guard in `placement`.
        self.tolerance_ms = float(tolerance_ms)
        self.audio_items = 0
        self.video_items = 0
        self.last_video_ms = -1
        # Statistics of picture content minus sound at pairing time; positive means the picture is ahead.
        self.pairs = 0
        self.residual_sum_ms = 0.0
        self.residual_worst_ms = 0.0
        # Duration (ms) and count of content the source skipped.
        self.content_gap_ms = 0
        self.audio_gaps = 0
        # Number of frames the mapping could not place (in a dropped interval or not yet due).
        self.frames_unplaceable = 0
        # Of these, frames in a dropped interval; only these are a loss, the rest are merely waiting.
        self.frames_in_hole = 0
        self.last_verdict = VERDICT_NO_ANCHOR
        # Each segment is (content start, session start, content end), end exclusive; the newest segment is still receiving audio, so its end is None.
        # The end keeps content inside a dropped interval from extrapolating along the previous segment.
        self._segments: list[tuple[float, float, float | None]] = []
        self._prev_audio_content: float | None = None


    @property
    def anchored(self) -> bool:
        """Whether any sound has been taken yet; before that session time is meaningless."""
        return bool(self._segments)

    def session_of(self, content_ms: float) -> float | None:
        """Session time for a content moment, or None if there is none.

        Within a segment: session time runs parallel to content time. In a gap: the sound was dropped by the sender, there is no matching time in the session, returns None.
        Before the first segment: extend the first segment backward; the result may be negative and is clamped to 0 by `placement()`.
        """
        if not self._segments:
            return None
        first = self._segments[0]
        if content_ms < first[0]:
            # Earlier than the session's first sound: extend the first segment backward.
            return float(content_ms) - first[0] + first[1]
        for start_content, start_session, end_content in self._segments:
            if start_content <= content_ms and (end_content is None
                                                or content_ms < end_content):
                # Falls within this segment; the newest segment has no end and only extends forward.
                return float(content_ms) - start_content + start_session
        # In no segment: it lies between an ended segment and the next one, i.e. sound the sender dropped, with no session moment.
        return None

    def audio(self, content_ms: float) -> int:
        """Return the session timestamp of an audio chunk and count it as taken.

        Timestamps are contiguous chunk by chunk (the device requires it); a source discontinuity only changes the content mapping and is counted in `content_gap_ms`.
        """
        content_ms = float(content_ms)
        # This chunk's session time, computed before the mapping is modified: a new segment starts at this chunk's session time.
        value = int(round(self.audio_items * self.chunk_ms))
        if self._prev_audio_content is None:
            # Anchor: the session's first sound, session time 0.
            self._segments = [(content_ms, float(value), None)]
        else:
            step = content_ms - self._prev_audio_content
            if step > self.chunk_ms * 1.5:
                missed = int(round(step - self.chunk_ms))
                self.content_gap_ms += missed
                self.audio_gaps += 1
                # Close the old segment at the end of the sound actually received, so content in the gap does not extrapolate along the old segment.
                previous = self._segments[-1]
                self._segments[-1] = (previous[0], previous[1],
                                      self._prev_audio_content + self.chunk_ms)
                # Start a new segment at this chunk's session time.
                self._segments.append((content_ms, float(value), None))
        self._prev_audio_content = content_ms
        self.audio_items += 1
        return value

    def placement(self, content_ms: float,
                  pending_audio_ms: float | None = None) -> tuple[str, int | None]:
        """Judge a picture's position; returns (verdict, session timestamp or None).

        `content_ms` is the picture's content moment, already converted to the sound clock (see `ContentTimeline.video_in_audio_units`).
        `pending_audio_ms` is the content position of the next audio chunk the sender will take (queue head), used to close the current segment
        early and detect a gap before that chunk is taken.

        `IN_HOLE` is a loss and the frame is dropped; `NOT_YET` is a wait and the frame stays in the queue.
        With no anchor there is no session time and the timestamp is None.
        Timestamps are strictly increasing (the device rejects a video timestamp that does not advance); an equal one gets +1 ms; clamped to be no less than 0.
        """
        self.last_verdict = VERDICT_NO_ANCHOR
        if not self.anchored:
            return VERDICT_NO_ANCHOR, None
        # The current segment's frontier, and whether the next sound continues it; the queue head is visible before it is taken, so a gap can be found early.
        edge = None
        if self._prev_audio_content is not None and pending_audio_ms is not None:
            edge = self._prev_audio_content + self.chunk_ms
            if pending_audio_ms <= edge:
                # The next sound continues this segment; the segment simply extends.
                edge = None
        if edge is not None and content_ms >= edge:
            if content_ms < pending_audio_ms:
                # Falls in sound the sender dropped; no usable session time.
                self.frames_unplaceable += 1
                self.frames_in_hole += 1
                self.last_verdict = VERDICT_IN_HOLE
                return VERDICT_IN_HOLE, None
            # Even the next sound has been passed; not yet time, keep the frame.
            self.last_verdict = VERDICT_NOT_YET
            return VERDICT_NOT_YET, None
        place = self.session_of(content_ms)
        if place is not None and place > (self.audio_items * self.chunk_ms
                                          + self.tolerance_ms):
            # Second guard, for a gap not yet visible: before the sound after the gap is taken, a frame paired now would cross the hole along the still-open segment.
            # One-way check: a picture may lag the sound already taken (the sound is a queue depth ahead of the picture) but must not lead it;
            # the lead allowance is the pairing tolerance, since the queue head has not been taken yet.
            self.frames_unplaceable += 1
            self.last_verdict = VERDICT_NOT_YET
            return VERDICT_NOT_YET, None
        if place is None:
            # Content falls in a dropped interval with no session moment; emitting a timestamp here would push the strictly-increasing clamp below past the recovery frame.
            self.frames_unplaceable += 1
            self.frames_in_hole += 1
            self.last_verdict = VERDICT_IN_HOLE
            return VERDICT_IN_HOLE, None
        value = int(round(place))
        if value < 0:
            value = 0
        if value <= self.last_video_ms:
            value = self.last_video_ms + 1
        self.last_video_ms = value
        self.video_items += 1
        self.last_verdict = VERDICT_PLACED
        return VERDICT_PLACED, value

    def video(self, content_ms: float,
              pending_audio_ms: float | None = None) -> int | None:
        """Return only the picture's session timestamp, or None; the sender needs to tell dropping from waiting, so use `placement()`."""
        return self.placement(content_ms, pending_audio_ms)[1]

    def note_pair(self, video_content_ms: float, audio_content_ms: float) -> None:
        """Record picture content minus sound (ms) at pairing time, positive meaning the picture is ahead; both arguments are on the same clock.

        This difference reflects the depth difference of the two queues, not lip-sync error: a constant value means the picture queue holds that much extra content; what needs attention is persistent drift.
        """
        residual = float(video_content_ms) - float(audio_content_ms)
        self.pairs += 1
        self.residual_sum_ms += residual
        if abs(residual) > abs(self.residual_worst_ms):
            self.residual_worst_ms = residual

    def diagnostics(self) -> str:
        """One-line statistics for the periodic report."""
        mean = self.residual_sum_ms / self.pairs if self.pairs else 0.0
        return (f"audio={self.audio_items} video={self.video_items}"
                f" pair={self.pairs}"
                f" drift={mean:+.0f}ms worst={self.residual_worst_ms:+.0f}ms"
                f" gaps={self.audio_gaps}/{self.content_gap_ms}ms"
                f" unplaceable={self.frames_unplaceable}"
                f" hole={self.frames_in_hole}")
