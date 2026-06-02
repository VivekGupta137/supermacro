"""Path alignment and variation metrics (shortest-time cap + reference modes)."""

from __future__ import annotations

import re
from typing import Any, Dict, List, Sequence, Tuple, TYPE_CHECKING

import numpy as np

if TYPE_CHECKING:
    from .capture import Attempt

N_SAMPLES = 200

TimedPoint = Tuple[float, float, int]  # rel_x, rel_y, t_us


def path_arc_length(path: Sequence[Tuple[float, float]]) -> float:
    if len(path) < 2:
        return 0.0
    xy = np.array(path, dtype=np.float64)
    return float(np.sum(np.sqrt(np.sum(np.diff(xy, axis=0) ** 2, axis=1))))


def endpoint_distance(path: Sequence[Tuple[float, float]]) -> float:
    if not path:
        return 0.0
    ex, ey = path[-1]
    return float(np.hypot(ex, ey))


def attempt_duration_ms(timed_path: Sequence[TimedPoint]) -> float:
    if not timed_path:
        return 0.0
    return timed_path[-1][2] / 1000.0


def trim_path_to_time(
    timed_path: Sequence[TimedPoint], max_t_us: int
) -> List[Tuple[float, float]]:
    """Keep points from start through max_t_us (interpolate last sample if needed)."""
    if not timed_path:
        return []
    if max_t_us <= 0:
        return [(timed_path[0][0], timed_path[0][1])]

    if timed_path[-1][2] <= max_t_us:
        return [(p[0], p[1]) for p in timed_path]

    out: List[Tuple[float, float]] = [(timed_path[0][0], timed_path[0][1])]
    for i in range(1, len(timed_path)):
        t_prev = timed_path[i - 1][2]
        t = timed_path[i][2]
        if t <= max_t_us:
            out.append((timed_path[i][0], timed_path[i][1]))
            continue
        if t > t_prev:
            u = (max_t_us - t_prev) / (t - t_prev)
            x = timed_path[i - 1][0] + u * (timed_path[i][0] - timed_path[i - 1][0])
            y = timed_path[i - 1][1] + u * (timed_path[i][1] - timed_path[i - 1][1])
            out.append((x, y))
        break
    return out


def interpolate_timed_path(timed_path: Sequence[TimedPoint], t_us: int) -> Tuple[float, float]:
    """Linear interpolation of (x, y) at timestamp t_us."""
    if not timed_path:
        return 0.0, 0.0
    if t_us <= timed_path[0][2]:
        return timed_path[0][0], timed_path[0][1]
    if t_us >= timed_path[-1][2]:
        return timed_path[-1][0], timed_path[-1][1]
    for i in range(1, len(timed_path)):
        t_prev = timed_path[i - 1][2]
        t = timed_path[i][2]
        if t_us <= t:
            if t <= t_prev:
                return timed_path[i][0], timed_path[i][1]
            u = (t_us - t_prev) / (t - t_prev)
            x = timed_path[i - 1][0] + u * (timed_path[i][0] - timed_path[i - 1][0])
            y = timed_path[i - 1][1] + u * (timed_path[i][1] - timed_path[i - 1][1])
            return x, y
    return timed_path[-1][0], timed_path[-1][1]


def first_motion_time_us(
    timed_path: Sequence[TimedPoint], threshold_px: float = 0.5
) -> int:
    """First sample time where cursor moved from origin (macro actually started)."""
    if not timed_path:
        return 0
    for x, y, t in timed_path:
        if abs(x) > threshold_px or abs(y) > threshold_px:
            return int(t)
    return int(timed_path[0][2])


def bullet_step_times_us(
    interval_us: int,
    end_t_us: int,
    *,
    phase_t_us: int = 0,
) -> List[int]:
    """Wall-clock tick times for bullet fires: phase, phase+interval, …"""
    if interval_us <= 0 or end_t_us < 0:
        return []
    times: List[int] = []
    t = phase_t_us
    while t <= end_t_us:
        times.append(t)
        t += interval_us
    return times


def shared_bullet_phase_us(
    timed_paths: Sequence[Sequence[TimedPoint]],
    *,
    align_to_first_motion: bool = True,
) -> int:
    """One phase for all attempts so step markers share the same tick times."""
    if not align_to_first_motion or not timed_paths:
        return 0
    phases = [first_motion_time_us(tp) for tp in timed_paths if tp]
    return min(phases) if phases else 0


def path_bullet_phase_us(
    timed_path: Sequence[TimedPoint],
    *,
    align_to_first_motion: bool = True,
    phase_t_us: int | None = None,
) -> int:
    """Recording time of bullet 1 (first macro step), matching usb-output scheduling."""
    if phase_t_us is not None:
        return phase_t_us
    if align_to_first_motion:
        return first_motion_time_us(timed_path)
    return 0


def bullet_fire_times_us(
    interval_us: int,
    end_t_us: int,
    phase_t_us: int,
) -> List[int]:
    """Wall-clock time of each bullet fire (start of that step's spread)."""
    return bullet_step_times_us(interval_us, end_t_us, phase_t_us=phase_t_us)


def bullet_landmark_in_window(
    timed_path: Sequence[TimedPoint],
    t_start_us: int,
    t_end_us: int,
    sample_us: int = 2500,
) -> Tuple[float, float]:
    """
    Position after one bullet's spread within [t_start, t_end).

    Picks the stair-step corner (leftmost X when horizontal kick exists, else end of
    window) so markers sit on treads rather than mid-drip.
    """
    if not timed_path or t_end_us <= t_start_us:
        return interpolate_timed_path(timed_path, t_start_us)
    samples: List[Tuple[int, float, float]] = []
    t = t_start_us
    while t < t_end_us:
        x, y = interpolate_timed_path(timed_path, t)
        samples.append((t, x, y))
        t += sample_us
    t_last = max(t_start_us, t_end_us - 1)
    x, y = interpolate_timed_path(timed_path, t_last)
    samples.append((t_last, x, y))
    xs = [s[1] for s in samples]
    if max(xs) - min(xs) < 1.0:
        return (x, y)
    _, bx, by = min(samples, key=lambda s: s[1])
    return (bx, by)


def bullet_markers_for_attempts(
    timed_paths: Sequence[Sequence[TimedPoint]],
    interval_us: int,
    cap_t_us: int,
    *,
    align_to_first_motion: bool = True,
    phase_t_us: int | None = None,
) -> List[List[Tuple[float, float]]]:
    """
    One marker per bullet, aligned to each step window on the path.

    Bullet ``k`` uses window ``[phase + k*interval, phase + (k+1)*interval)`` (same
    cadence as usb-output). The marker is placed at the step corner after spread,
    not at bullet-fire time (avoids mid-drip dots and mismatched guides).
    """
    if interval_us <= 0 or not timed_paths:
        return []
    markers: List[List[Tuple[float, float]]] = []
    for tp in timed_paths:
        if not tp:
            markers.append([])
            continue
        end_t = min(cap_t_us, tp[-1][2]) if cap_t_us > 0 else tp[-1][2]
        path_phase = path_bullet_phase_us(
            tp,
            align_to_first_motion=align_to_first_motion,
            phase_t_us=phase_t_us,
        )
        row: List[Tuple[float, float]] = []
        k = 0
        while True:
            t_start = path_phase + k * interval_us
            t_end = path_phase + (k + 1) * interval_us
            if t_end > end_t:
                break
            row.append(bullet_landmark_in_window(tp, t_start, t_end))
            k += 1
        markers.append(row)
    min_steps = min((len(r) for r in markers if r), default=0)
    if min_steps:
        markers = [r[:min_steps] for r in markers]
    return markers


def bullet_step_marker_points(
    timed_path: Sequence[TimedPoint],
    interval_us: int,
    max_t_us: int | None = None,
    *,
    phase_t_us: int | None = None,
    align_to_first_motion: bool = True,
) -> List[Tuple[float, float]]:
    """Single-attempt markers; prefer :func:`bullet_markers_for_attempts` for overlays."""
    if interval_us <= 0 or not timed_path:
        return []
    end_t = timed_path[-1][2] if max_t_us is None else min(max_t_us, timed_path[-1][2])
    phase = phase_t_us
    if phase is None:
        phase = first_motion_time_us(timed_path) if align_to_first_motion else 0
    return bullet_markers_for_attempts(
        [timed_path],
        interval_us,
        end_t,
        align_to_first_motion=align_to_first_motion,
        phase_t_us=phase if phase_t_us is not None else None,
    )[0]


def parse_bullet_step(text: str) -> int:
    """
    Parse bullet step interval from one text field (profile-agnostic).

    Examples: ``133.3 ms``, ``133300 us``, ``133300`` (µs), ``133.3`` (ms).
    """
    raw = text.strip().lower().replace("µ", "u")
    if not raw:
        raise ValueError("bullet step is empty")
    match = re.match(r"^([0-9]+(?:\.[0-9]+)?)\s*(ms|us|u)?\s*$", raw)
    if not match:
        raise ValueError(f"invalid bullet step: {text!r}")
    amount = float(match.group(1))
    if amount <= 0:
        raise ValueError("bullet step must be positive")
    unit = match.group(2)
    if unit == "ms":
        return int(round(amount * 1000.0))
    if unit in ("us", "u"):
        return int(round(amount))
    # No unit: decimals → ms; large integers → µs (firmware ``us`` fields).
    if "." in match.group(1):
        return int(round(amount * 1000.0))
    if amount >= 1000:
        return int(round(amount))
    return int(round(amount * 1000.0))


def parse_bullet_interval_us(value: str, unit: str) -> int:
    """Legacy: value + unit combobox. Prefer :func:`parse_bullet_step`."""
    raw = value.strip()
    if not raw:
        raise ValueError("interval is empty")
    if unit == "ms":
        return parse_bullet_step(f"{raw} ms")
    if unit == "us":
        return parse_bullet_step(f"{raw} us")
    raise ValueError(f"unknown unit: {unit}")


def per_bullet_step_x_stats(
    timed_paths: Sequence[Sequence[TimedPoint]],
    cap_t_us: int,
    interval_us: int,
    reference_index: int,
    *,
    align_to_first_motion: bool = True,
) -> List[Dict[str, Any]]:
    """Horizontal spread at each bullet tick (time-aligned, vs ref attempt X)."""
    if interval_us <= 0 or cap_t_us < 0 or not timed_paths:
        return []
    phase_t_us = shared_bullet_phase_us(
        timed_paths, align_to_first_motion=align_to_first_motion
    )
    rows: List[Dict[str, Any]] = []
    for step, t in enumerate(
        bullet_fire_times_us(interval_us, cap_t_us, phase_t_us)
    ):
        xs = [interpolate_timed_path(tp, t)[0] for tp in timed_paths]
        ref_x = xs[reference_index] if 0 <= reference_index < len(xs) else xs[0]
        arr = np.array(xs, dtype=np.float64)
        rows.append(
            {
                "step": step,
                "tMs": round(t / 1000.0, 3),
                "meanX": float(np.mean(arr)),
                "stdX": float(np.std(arr)),
                "rangeX": float(np.ptp(arr)),
                "refX": float(ref_x),
                "maxAbsDevX": float(np.max(np.abs(arr - ref_x))),
            }
        )
    return rows


def last_sample_time_us(timed_path: Sequence[TimedPoint]) -> int:
    if not timed_path:
        return 0
    return int(timed_path[-1][2])


def cap_paths_to_shortest_time(
    timed_paths: Sequence[Sequence[TimedPoint]],
    durations_ms: Sequence[float] | None = None,
) -> Tuple[List[List[Tuple[float, float]]], int, float]:
    """
    Trim every path to the last sample time of the shortest attempt.
    Uses recorded sample timestamps (t_us), not release duration_ms, so every
    path ends at the same wall-clock moment in the spray.
    Returns (capped_paths_xy, reference_index, cap_time_ms).
    """
    if not timed_paths:
        return [], -1, 0.0

    last_times_us = [last_sample_time_us(p) for p in timed_paths]
    ref_idx = int(np.argmin(last_times_us))
    cap_t_us = last_times_us[ref_idx]
    cap_ms = cap_t_us / 1000.0
    if cap_t_us <= 0:
        return [[(p[0], p[1]) for p in tp] for tp in timed_paths], ref_idx, cap_ms

    capped = [trim_path_to_time(p, cap_t_us) for p in timed_paths]
    return capped, ref_idx, cap_ms


def _resample_by_arc_length(xy: np.ndarray, n_samples: int) -> np.ndarray:
    if len(xy) < 2:
        return np.zeros((n_samples, 2), dtype=np.float64)
    seg = np.sqrt(np.sum(np.diff(xy, axis=0) ** 2, axis=1))
    arc = np.concatenate([[0.0], np.cumsum(seg)])
    total = arc[-1]
    if total <= 1e-9:
        return np.tile(xy[0], (n_samples, 1))
    targets = np.linspace(0.0, total, n_samples)
    out = np.zeros((n_samples, 2), dtype=np.float64)
    for i, t in enumerate(targets):
        j = int(np.searchsorted(arc, t, side="right")) - 1
        j = max(0, min(j, len(xy) - 2))
        a0, a1 = arc[j], arc[j + 1]
        if a1 <= a0:
            out[i] = xy[j]
        else:
            u = (t - a0) / (a1 - a0)
            out[i] = xy[j] * (1.0 - u) + xy[j + 1] * u
    return out


def attempts_to_arrays(
    relative_paths: Sequence[Sequence[Tuple[float, float]]], n_samples: int = N_SAMPLES
) -> np.ndarray:
    rows = []
    for path in relative_paths:
        if len(path) < 2:
            rows.append(np.zeros((n_samples, 2), dtype=np.float64))
            continue
        xy = np.array(path, dtype=np.float64)
        rows.append(_resample_by_arc_length(xy, n_samples))
    return np.array(rows, dtype=np.float64)


def timed_paths_to_arrays(
    timed_paths: Sequence[Sequence[TimedPoint]],
    cap_t_us: int,
    n_samples: int = N_SAMPLES,
) -> np.ndarray:
    """Resample each path at uniform time steps through cap_t_us (for bullet-timed comparison)."""
    if cap_t_us <= 0:
        return np.zeros((len(timed_paths), n_samples, 2), dtype=np.float64)
    ts = np.linspace(0.0, float(cap_t_us), n_samples)
    rows = []
    for tp in timed_paths:
        if not tp:
            rows.append(np.zeros((n_samples, 2), dtype=np.float64))
            continue
        row = np.array(
            [interpolate_timed_path(tp, int(t)) for t in ts], dtype=np.float64
        )
        rows.append(row)
    return np.array(rows, dtype=np.float64)


def mean_path_from_stack(stacked: np.ndarray) -> np.ndarray:
    return np.mean(stacked, axis=0)


def _path_spread_axis(stacked: np.ndarray) -> Tuple[float, float, float, float]:
    """Max and mean across-sample std of X/Y across attempts (capped, time-aligned)."""
    if stacked.shape[0] < 2:
        return 0.0, 0.0, 0.0, 0.0
    x_std = np.std(stacked[:, :, 0], axis=0)
    y_std = np.std(stacked[:, :, 1], axis=0)
    return float(np.max(x_std)), float(np.mean(x_std)), float(np.max(y_std)), float(np.mean(y_std))


def _deviation_vs_reference(
    stacked: np.ndarray, ref: np.ndarray
) -> Tuple[List[float], List[float], List[float], List[float], List[float], List[float]]:
    rms_list: List[float] = []
    max_list: List[float] = []
    rms_x_list: List[float] = []
    rms_y_list: List[float] = []
    max_x_list: List[float] = []
    max_y_list: List[float] = []
    for i in range(stacked.shape[0]):
        diff = stacked[i] - ref
        dist = np.sqrt(np.sum(diff**2, axis=1))
        rms_list.append(float(np.sqrt(np.mean(dist**2))))
        max_list.append(float(np.max(dist)))
        rms_x_list.append(float(np.sqrt(np.mean(diff[:, 0] ** 2))))
        rms_y_list.append(float(np.sqrt(np.mean(diff[:, 1] ** 2))))
        max_x_list.append(float(np.max(np.abs(diff[:, 0]))))
        max_y_list.append(float(np.max(np.abs(diff[:, 1]))))
    return rms_list, max_list, rms_x_list, rms_y_list, max_x_list, max_y_list


def _build_mode_metrics(
    *,
    mode: str,
    reference_index: int | None,
    capped: List[List[Tuple[float, float]]],
    stacked: np.ndarray,
    ref_stack: np.ndarray,
    raw_durations_ms: Sequence[float],
    last_times_ms: Sequence[float],
    cap_time_ms: float,
) -> Dict[str, Any]:
    n_attempts = stacked.shape[0]
    endpoints = np.array([p[-1] if len(p) else (0.0, 0.0) for p in capped])
    capped_lengths = [path_arc_length(p) for p in capped]
    rms_list, max_list, rms_x_list, rms_y_list, max_x_list, max_y_list = _deviation_vs_reference(
        stacked, ref_stack
    )
    spread_max_x, spread_mean_x, spread_max_y, spread_mean_y = _path_spread_axis(stacked)

    ref_path = [(float(x), float(y)) for x, y in ref_stack]
    return {
        "metricsMode": mode,
        "referenceIndex": reference_index,
        "attemptCount": n_attempts,
        "capTimeMs": cap_time_ms,
        "endpointMeanX": float(np.mean(endpoints[:, 0])),
        "endpointMeanY": float(np.mean(endpoints[:, 1])),
        "endpointStdX": float(np.std(endpoints[:, 0])),
        "endpointStdY": float(np.std(endpoints[:, 1])),
        "endpointRangeX": float(np.ptp(endpoints[:, 0])),
        "endpointRangeY": float(np.ptp(endpoints[:, 1])),
        "deviationRmsPx": float(np.mean(rms_list)) if rms_list else 0.0,
        "deviationRmsXPx": float(np.mean(rms_x_list)) if rms_x_list else 0.0,
        "deviationRmsYPx": float(np.mean(rms_y_list)) if rms_y_list else 0.0,
        "maxDeviationPx": float(np.max(max_list)) if max_list else 0.0,
        "maxDeviationXPx": float(np.max(max_x_list)) if max_x_list else 0.0,
        "maxDeviationYPx": float(np.max(max_y_list)) if max_y_list else 0.0,
        "pathSpreadMaxXPx": spread_max_x,
        "pathSpreadMeanXPx": spread_mean_x,
        "pathSpreadMaxYPx": spread_max_y,
        "pathSpreadMeanYPx": spread_mean_y,
        "durationMeanMs": float(np.mean(raw_durations_ms)),
        "durationCv": (
            float(np.std(raw_durations_ms) / np.mean(raw_durations_ms))
            if raw_durations_ms and np.mean(raw_durations_ms) > 1e-9
            else 0.0
        ),
        "referencePath": ref_path,
        "perAttempt": [
            {
                "endpointX": float(endpoints[i, 0]),
                "endpointY": float(endpoints[i, 1]),
                "endpointDistPx": float(endpoint_distance(capped[i])),
                "durationMs": raw_durations_ms[i],
                "cappedLengthPx": capped_lengths[i],
                "deviationRmsPx": rms_list[i],
                "deviationRmsXPx": rms_x_list[i],
                "deviationRmsYPx": rms_y_list[i],
                "maxDeviationPx": max_list[i],
                "maxDeviationXPx": max_x_list[i],
                "maxDeviationYPx": max_y_list[i],
                "excludedTailMs": max(0.0, last_times_ms[i] - cap_time_ms),
                "isReference": reference_index is not None and i == reference_index,
            }
            for i in range(n_attempts)
        ],
    }


def compute_session_metrics(
    attempts: Sequence[Attempt],
    *,
    bullet_step_us: int | None = None,
    bullet_step_input: str | None = None,
    align_bullet_to_first_motion: bool = True,
) -> Dict[str, Any]:
    if not attempts:
        return {}

    timed_paths = [a.relative_points_timed() for a in attempts]
    last_times_ms = [last_sample_time_us(p) / 1000.0 for p in timed_paths]
    raw_durations_ms = [a.duration_ms for a in attempts]
    capped, ref_idx, cap_ms = cap_paths_to_shortest_time(timed_paths, raw_durations_ms)
    if ref_idx < 0 or not capped:
        return {}

    cap_t_us = int(cap_ms * 1000)
    stacked = timed_paths_to_arrays(timed_paths, cap_t_us, N_SAMPLES)
    ref_stack = stacked[ref_idx]
    mean_stack = mean_path_from_stack(stacked)

    shortest_ref = _build_mode_metrics(
        mode="shortest_time_reference",
        reference_index=ref_idx,
        capped=capped,
        stacked=stacked,
        ref_stack=ref_stack,
        raw_durations_ms=raw_durations_ms,
        last_times_ms=last_times_ms,
        cap_time_ms=cap_ms,
    )
    mean_ref = _build_mode_metrics(
        mode="mean_path_reference",
        reference_index=None,
        capped=capped,
        stacked=stacked,
        ref_stack=mean_stack,
        raw_durations_ms=raw_durations_ms,
        last_times_ms=last_times_ms,
        cap_time_ms=cap_ms,
    )

    result: Dict[str, Any] = {
        "capReference": {
            "shortestTimeIndex": ref_idx,
            "capTimeMs": cap_ms,
            "capTimeUs": int(cap_ms * 1000),
            "capArcLengthPx": path_arc_length(capped[ref_idx]),
        },
        "cappedPaths": capped,
        "shortestTimeRef": shortest_ref,
        "meanPathRef": mean_ref,
        # Legacy top-level keys for CSV/export consumers
        "metricsMode": shortest_ref["metricsMode"],
        "attemptCount": shortest_ref["attemptCount"],
        "shortestPathIndex": ref_idx,
        "shortestTimeIndex": ref_idx,
        "capTimeMs": cap_ms,
        "capArcLengthPx": path_arc_length(capped[ref_idx]),
        "endpointMeanX": shortest_ref["endpointMeanX"],
        "endpointMeanY": shortest_ref["endpointMeanY"],
        "endpointStdX": shortest_ref["endpointStdX"],
        "endpointStdY": shortest_ref["endpointStdY"],
        "endpointRangeX": shortest_ref["endpointRangeX"],
        "endpointRangeY": shortest_ref["endpointRangeY"],
        "deviationRmsPx": shortest_ref["deviationRmsPx"],
        "deviationRmsXPx": shortest_ref["deviationRmsXPx"],
        "deviationRmsYPx": shortest_ref["deviationRmsYPx"],
        "maxDeviationPx": shortest_ref["maxDeviationPx"],
        "maxDeviationXPx": shortest_ref["maxDeviationXPx"],
        "maxDeviationYPx": shortest_ref["maxDeviationYPx"],
        "pathSpreadMaxXPx": shortest_ref["pathSpreadMaxXPx"],
        "pathSpreadMeanXPx": shortest_ref["pathSpreadMeanXPx"],
        "durationMeanMs": shortest_ref["durationMeanMs"],
        "durationCv": shortest_ref["durationCv"],
        "perAttempt": shortest_ref["perAttempt"],
    }
    if bullet_step_us and bullet_step_us > 0:
        steps = per_bullet_step_x_stats(
            timed_paths,
            cap_t_us,
            bullet_step_us,
            ref_idx,
            align_to_first_motion=align_bullet_to_first_motion,
        )
        result["alignBulletToFirstMotion"] = align_bullet_to_first_motion
        result["bulletStepUs"] = bullet_step_us
        if bullet_step_input:
            result["bulletStepInput"] = bullet_step_input.strip()
        result["perBulletStep"] = steps
        if steps:
            worst = max(steps, key=lambda r: r["stdX"])
            result["worstBulletStepSpreadX"] = dict(worst)
    return result


def format_metrics_text(m: Dict[str, Any], mode: str = "shortestTimeRef") -> str:
    if not m:
        return "No metrics (no attempts)."

    if mode == "shortestEndpointRef":
        mode = "shortestTimeRef"

    block = m.get(mode) or m
    cap = m.get("capReference", {})
    ref_idx = block.get("referenceIndex")
    cap_ms = cap.get("capTimeMs", block.get("capTimeMs", m.get("capTimeMs", 0)))

    if mode == "meanPathRef":
        header = f"Reference: mean path (cap {cap_ms:.0f} ms)"
    else:
        ref_num = (ref_idx if ref_idx is not None else m.get("shortestTimeIndex", 0)) + 1
        header = f"Reference: attempt #{ref_num} — shortest time (cap {cap_ms:.0f} ms)"

    lines = [
        header,
        f"Attempts: {block.get('attemptCount', 0)}",
        f"Capped endpoint σ  X: {block.get('endpointStdX', 0):.2f} px   Y: {block.get('endpointStdY', 0):.2f} px",
        f"Capped endpoint range  X: {block.get('endpointRangeX', 0):.2f} px   Y: {block.get('endpointRangeY', 0):.2f} px",
        f"Path spread (time-aligned)  max σ X: {block.get('pathSpreadMaxXPx', 0):.2f} px   Y: {block.get('pathSpreadMaxYPx', 0):.2f} px",
        f"Deviation RMS (time-aligned)  2D: {block.get('deviationRmsPx', 0):.2f} px   X: {block.get('deviationRmsXPx', 0):.2f}   Y: {block.get('deviationRmsYPx', 0):.2f}",
        f"Max deviation  2D: {block.get('maxDeviationPx', 0):.2f} px   X: {block.get('maxDeviationXPx', 0):.2f}   Y: {block.get('maxDeviationYPx', 0):.2f}",
        f"Raw duration mean: {block.get('durationMeanMs', 0):.1f} ms   CV: {block.get('durationCv', 0):.4f}",
        "",
        "Per attempt (capped):",
    ]
    for i, p in enumerate(block.get("perAttempt", [])):
        tag = " ref" if p.get("isReference") else ""
        tail = p.get("excludedTailMs", 0)
        tail_note = f"  tail+{tail:.0f}ms" if tail > 0.5 else ""
        lines.append(
            f"  #{i + 1}{tag}: end=({p['endpointX']:+.1f}, {p['endpointY']:+.1f})  "
            f"rms={p['deviationRmsPx']:.2f} (X={p.get('deviationRmsXPx', 0):.2f} Y={p.get('deviationRmsYPx', 0):.2f}){tail_note}"
        )
    steps = m.get("perBulletStep") or []
    if steps and mode in ("shortestTimeRef", "shortestEndpointRef"):
        bullet_in = m.get("bulletStepInput") or f"{m.get('bulletStepUs', 0) / 1000:.3g} ms"
        lines.extend(["", f"Per bullet step (input: {bullet_in}, vs shortest-time ref):"])
        worst = m.get("worstBulletStepSpreadX")
        if worst:
            lines.append(
                f"  Worst sigma X: step {worst['step']} @ {worst['tMs']:.0f} ms  "
                f"std={worst['stdX']:.2f} px  range={worst['rangeX']:.2f} px"
            )
        for row in steps:
            lines.append(
                f"  step {row['step']:2d} @ {row['tMs']:6.1f} ms: "
                f"sigma X={row['stdX']:.2f}  range={row['rangeX']:.2f}  "
                f"max |dX|={row['maxAbsDevX']:.2f}"
            )
    return "\n".join(lines)
