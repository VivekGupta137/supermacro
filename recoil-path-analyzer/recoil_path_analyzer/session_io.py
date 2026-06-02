"""Save/load sessions as JSON and CSV."""

from __future__ import annotations

import csv
import json
from datetime import datetime, timezone
from pathlib import Path
from typing import Any, Dict, List, Optional, Sequence

from .capture import Attempt


SCHEMA = "recoil-path-analyzer/v1"


def attempt_to_dict(a: Attempt, index: int) -> Dict[str, Any]:
    rel = a.relative_points()
    total_dx = rel[-1][0] if rel else 0.0
    total_dy = rel[-1][1] if rel else 0.0
    return {
        "index": index,
        "startedAt": datetime.fromtimestamp(a.started_at, tz=timezone.utc).isoformat(),
        "durationMs": round(a.duration_ms, 2),
        "origin": {"x": a.origin_x, "y": a.origin_y},
        "pointCount": len(a.points),
        "totalDx": total_dx,
        "totalDy": total_dy,
        "points": [{"tUs": p.t_us, "x": p.x, "y": p.y} for p in a.points],
    }


def build_session_document(
    attempts: Sequence[Attempt],
    *,
    profile_label: str,
    profile_path: str,
    debug_mode: bool,
    poll_hz: float,
    metrics: Optional[Dict[str, Any]] = None,
    exports: Optional[Dict[str, str]] = None,
    bullet_step_input: Optional[str] = None,
    bullet_step_us: Optional[int] = None,
) -> Dict[str, Any]:
    doc: Dict[str, Any] = {
        "schema": SCHEMA,
        "createdAt": datetime.now(timezone.utc).isoformat(),
        "profileLabel": profile_label,
        "profilePath": profile_path,
        "debugMode": debug_mode,
        "pollHz": poll_hz,
        "trigger": {"mouseMask": 3, "description": "LMB+RMB (ADS)"},
        "metrics": _metrics_for_export(metrics),
        "attempts": [attempt_to_dict(a, i) for i, a in enumerate(attempts)],
    }
    if bullet_step_input:
        doc["bulletStepInput"] = bullet_step_input
    if bullet_step_us and bullet_step_us > 0:
        doc["bulletStepUs"] = bullet_step_us
    if exports:
        doc["exports"] = exports
    return doc


def _strip_paths_block(block: Optional[Dict[str, Any]]) -> Dict[str, Any]:
    if not block:
        return {}
    out = dict(block)
    out.pop("referencePath", None)
    return out


def _metrics_for_export(metrics: Optional[Dict[str, Any]]) -> Dict[str, Any]:
    if not metrics:
        return {}
    out = dict(metrics)
    out.pop("cappedPaths", None)
    if "shortestTimeRef" in out:
        out["shortestTimeRef"] = _strip_paths_block(out.get("shortestTimeRef"))
    if "meanPathRef" in out:
        out["meanPathRef"] = _strip_paths_block(out.get("meanPathRef"))
    return out


def save_session_json(path: Path, doc: Dict[str, Any]) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text(json.dumps(doc, indent=2), encoding="utf-8")


def save_session_csv(path: Path, attempts: Sequence[Attempt]) -> None:
    """One row per point — easy for pandas/Excel."""
    path.parent.mkdir(parents=True, exist_ok=True)
    with path.open("w", newline="", encoding="utf-8") as f:
        w = csv.writer(f)
        w.writerow(
            [
                "attempt",
                "t_us",
                "screen_x",
                "screen_y",
                "rel_x",
                "rel_y",
                "duration_ms",
            ]
        )
        for ai, att in enumerate(attempts):
            for p in att.points:
                w.writerow(
                    [
                        ai,
                        p.t_us,
                        p.x,
                        p.y,
                        p.x - att.origin_x,
                        p.y - att.origin_y,
                        round(att.duration_ms, 2),
                    ]
                )


def _write_mode_summary(w: csv.writer, prefix: str, block: Dict[str, Any]) -> None:
    w.writerow([f"{prefix}_metric", "value"])
    for key in (
        "metricsMode",
        "referenceIndex",
        "attemptCount",
        "capTimeMs",
        "endpointMeanX",
        "endpointMeanY",
        "endpointStdX",
        "endpointStdY",
        "endpointRangeX",
        "endpointRangeY",
        "deviationRmsPx",
        "deviationRmsXPx",
        "deviationRmsYPx",
        "maxDeviationPx",
        "maxDeviationXPx",
        "maxDeviationYPx",
        "pathSpreadMaxXPx",
        "pathSpreadMeanXPx",
        "pathSpreadMaxYPx",
        "pathSpreadMeanYPx",
        "durationMeanMs",
        "durationCv",
    ):
        if key in block:
            w.writerow([f"{prefix}_{key}", block[key]])
    w.writerow([])


def save_metrics_csv(path: Path, metrics: Dict[str, Any]) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    with path.open("w", newline="", encoding="utf-8") as f:
        w = csv.writer(f)
        cap = metrics.get("capReference", {})
        w.writerow(["cap_shortestTimeIndex", cap.get("shortestTimeIndex", "")])
        w.writerow(["cap_timeMs", cap.get("capTimeMs", metrics.get("capTimeMs", ""))])
        w.writerow(["cap_arcLengthPx", cap.get("capArcLengthPx", metrics.get("capArcLengthPx", ""))])
        if metrics.get("bulletStepInput") is not None:
            w.writerow(["bulletStepInput", metrics.get("bulletStepInput", "")])
        if metrics.get("bulletStepUs") is not None:
            w.writerow(["bulletStepUs", metrics.get("bulletStepUs", "")])
        w.writerow([])

        steps = metrics.get("perBulletStep") or []
        if steps:
            w.writerow(["perBulletStep"])
            w.writerow(
                ["step", "tMs", "meanX", "stdX", "rangeX", "refX", "maxAbsDevX"]
            )
            for row in steps:
                w.writerow(
                    [
                        row.get("step"),
                        row.get("tMs"),
                        row.get("meanX"),
                        row.get("stdX"),
                        row.get("rangeX"),
                        row.get("refX"),
                        row.get("maxAbsDevX"),
                    ]
                )
            w.writerow([])

        shortest = metrics.get("shortestTimeRef") or metrics
        mean = metrics.get("meanPathRef") or {}
        _write_mode_summary(w, "shortestTime", shortest)
        if mean:
            _write_mode_summary(w, "meanPath", mean)

        for prefix, block in (("shortestTime", shortest), ("meanPath", mean)):
            if not block:
                continue
            w.writerow([f"{prefix}_perAttempt"])
            w.writerow(
                [
                    "attempt",
                    "endpointX",
                    "endpointY",
                    "endpointDistPx",
                    "durationMs",
                    "deviationRmsPx",
                    "deviationRmsXPx",
                    "deviationRmsYPx",
                    "maxDeviationPx",
                    "maxDeviationXPx",
                    "maxDeviationYPx",
                    "excludedTailMs",
                    "isReference",
                ]
            )
            for i, p in enumerate(block.get("perAttempt", [])):
                w.writerow(
                    [
                        i,
                        p.get("endpointX"),
                        p.get("endpointY"),
                        p.get("endpointDistPx"),
                        p.get("durationMs"),
                        p.get("deviationRmsPx"),
                        p.get("deviationRmsXPx"),
                        p.get("deviationRmsYPx"),
                        p.get("maxDeviationPx"),
                        p.get("maxDeviationXPx"),
                        p.get("maxDeviationYPx"),
                        p.get("excludedTailMs"),
                        p.get("isReference"),
                    ]
                )
            w.writerow([])
