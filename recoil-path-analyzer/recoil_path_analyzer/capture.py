"""Windows cursor capture and LMB+RMB trigger."""

from __future__ import annotations

import ctypes
import time
from dataclasses import dataclass, field
from typing import Callable, List, Optional, Tuple

# VK_LBUTTON=0x01, VK_RBUTTON=0x02
VK_LBUTTON = 0x01
VK_RBUTTON = 0x02

user32 = ctypes.windll.user32


class POINT(ctypes.Structure):
    _fields_ = [("x", ctypes.c_long), ("y", ctypes.c_long)]


def _buttons_down() -> Tuple[bool, bool]:
    lmb = bool(user32.GetAsyncKeyState(VK_LBUTTON) & 0x8000)
    rmb = bool(user32.GetAsyncKeyState(VK_RBUTTON) & 0x8000)
    return lmb, rmb


def chord_ads_down() -> bool:
    lmb, rmb = _buttons_down()
    return lmb and rmb


def get_cursor_pos() -> Tuple[int, int]:
    pt = POINT()
    if user32.GetCursorPos(ctypes.byref(pt)):
        return int(pt.x), int(pt.y)
    return 0, 0


@dataclass
class PathPoint:
    t_us: int
    x: int
    y: int


@dataclass
class Attempt:
    index: int
    started_at: float
    duration_ms: float
    origin_x: int
    origin_y: int
    points: List[PathPoint] = field(default_factory=list)

    def relative_points(self) -> List[Tuple[float, float]]:
        return [(p.x - self.origin_x, p.y - self.origin_y) for p in self.points]

    def relative_points_timed(self) -> List[Tuple[float, float, int]]:
        return [(p.x - self.origin_x, p.y - self.origin_y, p.t_us) for p in self.points]


def _append_sample(
    attempt: Attempt,
    t0: float,
    now: float,
    x: int,
    y: int,
    on_point: Optional[Callable[[Attempt], None]],
) -> None:
    attempt.points.append(PathPoint(int((now - t0) * 1_000_000), x, y))
    if on_point:
        on_point(attempt)


def record_attempt(
    poll_interval_s: float,
    stop_check,
    debounce_s: float = 0.03,
    on_point: Optional[Callable[[Attempt], None]] = None,
) -> Attempt | None:
    """
    Wait for LMB+RMB, record until either released.
    stop_check: callable returning True to abort wait/recording.
    on_point: optional callback after each sampled point (for live UI).

    Samples on every position change (fast poll) plus periodic heartbeats at
    poll_interval_s when the cursor is stationary.
    """
    debounce_until = 0.0
    while not stop_check():
        if chord_ads_down():
            if debounce_until == 0.0:
                debounce_until = time.perf_counter() + debounce_s
            elif time.perf_counter() >= debounce_until:
                break
        else:
            debounce_until = 0.0
        time.sleep(0.002)

    if stop_check():
        return None

    t0 = time.perf_counter()
    ox, oy = get_cursor_pos()
    attempt = Attempt(
        index=0,
        started_at=time.time(),
        duration_ms=0.0,
        origin_x=ox,
        origin_y=oy,
    )
    heartbeat_s = max(poll_interval_s, 1e-5)
    last_x, last_y = ox, oy
    last_sample_t = t0
    _append_sample(attempt, t0, t0, ox, oy, on_point)

    while chord_ads_down() and not stop_check():
        now = time.perf_counter()
        x, y = get_cursor_pos()
        moved = x != last_x or y != last_y
        due = (now - last_sample_t) >= heartbeat_s
        if moved or due:
            _append_sample(attempt, t0, now, x, y, on_point)
            last_x, last_y = x, y
            last_sample_t = now
            if moved:
                continue
        time.sleep(0.00005)

    now = time.perf_counter()
    x, y = get_cursor_pos()
    if x != last_x or y != last_y or (now - last_sample_t) >= heartbeat_s * 0.5:
        _append_sample(attempt, t0, now, x, y, on_point)

    attempt.duration_ms = (time.perf_counter() - t0) * 1000.0
    return attempt if len(attempt.points) >= 2 else None
