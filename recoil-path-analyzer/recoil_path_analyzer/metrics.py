"""Path alignment and variation metrics (shortest-time cap + reference modes)."""

from __future__ import annotations

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


def mean_path_from_stack(stacked: np.ndarray) -> np.ndarray:
    return np.mean(stacked, axis=0)


def _deviation_vs_reference(stacked: np.ndarray, ref: np.ndarray) -> Tuple[List[float], List[float]]:
    rms_list: List[float] = []
    max_list: List[float] = []
    for i in range(stacked.shape[0]):
        diff = stacked[i] - ref
        dist = np.sqrt(np.sum(diff**2, axis=1))
        rms_list.append(float(np.sqrt(np.mean(dist**2))))
        max_list.append(float(np.max(dist)))
    return rms_list, max_list


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
    rms_list, max_list = _deviation_vs_reference(stacked, ref_stack)

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
        "maxDeviationPx": float(np.max(max_list)) if max_list else 0.0,
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
                "maxDeviationPx": max_list[i],
                "excludedTailMs": max(0.0, last_times_ms[i] - cap_time_ms),
                "isReference": reference_index is not None and i == reference_index,
            }
            for i in range(n_attempts)
        ],
    }


def compute_session_metrics(attempts: Sequence[Attempt]) -> Dict[str, Any]:
    if not attempts:
        return {}

    timed_paths = [a.relative_points_timed() for a in attempts]
    last_times_ms = [last_sample_time_us(p) / 1000.0 for p in timed_paths]
    raw_durations_ms = [a.duration_ms for a in attempts]
    capped, ref_idx, cap_ms = cap_paths_to_shortest_time(timed_paths, raw_durations_ms)
    if ref_idx < 0 or not capped:
        return {}

    stacked = attempts_to_arrays(capped, N_SAMPLES)
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

    return {
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
        "maxDeviationPx": shortest_ref["maxDeviationPx"],
        "durationMeanMs": shortest_ref["durationMeanMs"],
        "durationCv": shortest_ref["durationCv"],
        "perAttempt": shortest_ref["perAttempt"],
    }


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
        f"Deviation RMS: {block.get('deviationRmsPx', 0):.2f} px",
        f"Max deviation: {block.get('maxDeviationPx', 0):.2f} px",
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
            f"rms={p['deviationRmsPx']:.2f}{tail_note}"
        )
    return "\n".join(lines)
