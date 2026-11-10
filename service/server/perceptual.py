"""When a frame exceeds the byte target, drop detail step by step by perceptual cost.

Input is ffmpeg's fixed 3-3-2 indices, sent unchanged when it fits the target. When it does not,
each pixel may reuse the value the device already shows or its left neighbor's value (when the
palette color is close enough to the reference pixel), so deflate sees more repeats; the closeness threshold is searched per frame, starting from the level the previous frame needed.

The three modes share a per-frame level search:

- `perceptual`: reuse the shown value or the left neighbor.
- `perceptual_2d`: additionally reuse the upper neighbor.
- `mosaic`: each N x N block takes the color of its top-left pixel.

Requires numpy; `AVAILABLE` says whether the import succeeded.
"""

from __future__ import annotations

try:
    import numpy as np
except ImportError:  # without numpy the server falls back to the color ladder
    np = None

from . import frames

AVAILABLE = np is not None

WIDTH, HEIGHT = frames.WIDTH, frames.HEIGHT

# Allowed distance between reference and substitute colors, as weighted squared RGB (green weighs most, blue least).
# Levels start from the smallest gap between adjacent palette colors (anything smaller has nothing to substitute); level 0 is the reference itself.
LEVELS = (0, 3900, 7800, 11700, 15600, 23400, 31200, 46800, 70000, 110000)

# Block edge length (pixels) for each mosaic level; level 0 is the reference itself.
MOSAIC_BLOCKS = (1, 2, 3, 4, 5, 6, 10)

MODES = ("perceptual", "perceptual_2d", "mosaic")

if AVAILABLE:
    # Same palette as the device (red/green step 36, blue step 85; see frames.palette_bytes),
    # with the weights folded into the coordinates so distance is plain Euclidean distance.
    _ROOT_WEIGHT = np.sqrt(np.array([3.0, 6.0, 1.0], np.float32))
    _PALETTE_W = np.array([((i >> 5) * 36, ((i >> 2) & 7) * 36, (i & 3) * 85)
                           for i in range(256)], np.float32) * _ROOT_WEIGHT


def _snap(reference: "np.ndarray", base: "np.ndarray", shown: "np.ndarray | None",
          held_distance: "np.ndarray | None", threshold: float,
          vertical: bool = False) -> "np.ndarray":
    """Replace pixels in `base` with cheaper-to-encode indices within `threshold` distance.

    Order: the value the device already shows; then the left neighbor per column; then, if `vertical`, the upper neighbor per row.
    Neighbor traversal must be sequential, since a pixel may reuse a pixel that itself just reused a neighbor.
    """
    out = base.copy()
    if shown is not None:
        keep = held_distance <= threshold
        out[keep] = shown[keep]
    columns = np.ascontiguousarray(out.T)
    source = np.ascontiguousarray(reference.transpose(1, 0, 2))
    for x in range(1, WIDTH):
        left = columns[x - 1]
        gap = _PALETTE_W[left] - source[x]
        near = (np.einsum("ij,ij->i", gap, gap) <= threshold) & (columns[x] != left)
        np.copyto(columns[x], left, where=near)
    out = np.ascontiguousarray(columns.T)
    if vertical:
        for y in range(1, HEIGHT):
            up = out[y - 1]
            gap = _PALETTE_W[up] - reference[y]
            near = (np.einsum("ij,ij->i", gap, gap) <= threshold) & (out[y] != up)
            np.copyto(out[y], up, where=near)
    return out


def _mosaic(base: "np.ndarray", block: int) -> "np.ndarray":
    """Fill each `block` x `block` square of `base` with its top-left pixel."""
    small = base[::block, ::block]
    return np.repeat(np.repeat(small, block, axis=0), block, axis=1)[:HEIGHT, :WIDTH]


def encode_within(raw: bytes, shown: bytes | None, tick: int, target: int,
                  hint: int = 0, min_diff: float = frames.DELTA_MAX_DIFF,
                  mode: str = "perceptual") -> tuple[list[bytes], bytes, int]:
    """Same contract as `frames.encode_within`; returns (stripes to send, picture the device will show, level).

    Try level 0 (the original frame) first; if it does not fit, search from `hint` (the level the previous frame needed): if it fits, try one level lower, if not, bisect among higher levels. `mode` comes from `MODES` and decides how a level becomes a picture.
    """
    if len(raw) != frames.FRAME_PIXELS:
        raise ValueError(f"frame is {len(raw)} bytes, expected {frames.FRAME_PIXELS}")
    if mode not in MODES:
        raise ValueError(f"unknown mode {mode!r}")
    top = (len(MOSAIC_BLOCKS) if mode == "mosaic" else len(LEVELS)) - 1

    def render(level: int) -> bytes:
        if mode == "mosaic":
            return _mosaic(base, MOSAIC_BLOCKS[level]).tobytes()
        return _snap(reference, base, held, held_distance, LEVELS[level],
                     vertical=mode == "perceptual_2d").tobytes()

    def attempt(level: int):
        drawn = raw if level == 0 else render(level)
        return frames.choose_stripes(drawn, shown, tick, 1 << 30, min_diff), drawn

    def fits(result) -> bool:
        return frames._wire_size(result[0]) <= target

    result = attempt(0)
    if fits(result):
        return result[0], result[1], 0

    base = np.frombuffer(raw, np.uint8).reshape(HEIGHT, WIDTH)
    reference = _PALETTE_W[base]
    held = held_distance = None
    if shown is not None:
        held = np.frombuffer(shown, np.uint8).reshape(HEIGHT, WIDTH)
        gap = _PALETTE_W[held] - reference
        held_distance = np.einsum("hwc,hwc->hw", gap, gap)

    start = min(max(hint, 1), top)
    result = attempt(start)
    if fits(result):
        if start > 1:
            lower = attempt(start - 1)
            if fits(lower):
                return lower[0], lower[1], start - 1
        return result[0], result[1], start
    low, high, best = start + 1, top, None
    while low <= high:
        probe = (low + high) // 2
        result = attempt(probe)
        if fits(result):
            best, high = (result[0], result[1], probe), probe - 1
        else:
            low = probe + 1
    if best is not None:
        return best
    drawn = attempt(top)[1]
    return frames.choose_stripes(drawn, shown, tick, target, min_diff), drawn, top
