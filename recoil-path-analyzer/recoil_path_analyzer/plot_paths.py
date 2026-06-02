"""Path plot rendering for display and high-resolution export."""

from __future__ import annotations

from pathlib import Path
from typing import List, Optional, Sequence, Tuple

import matplotlib.cm as mpl_cm
from matplotlib.axes import Axes
from matplotlib.figure import Figure
import numpy as np

DISPLAY_DPI = 100
EXPORT_DPI = 300
FIG_SIZE_INCHES = (7.0, 5.5)

Point = Tuple[float, float]


def auto_axis_limits(
    capped: Sequence[Sequence[Point]],
    live_path: Optional[Sequence[Point]] = None,
) -> Tuple[Tuple[float, float], Tuple[float, float]]:
    xs, ys = [], []
    for path in capped:
        for x, y in path:
            xs.append(x)
            ys.append(y)
    if live_path:
        for x, y in live_path:
            xs.append(x)
            ys.append(y)
    if not xs:
        return (-10.0, 10.0), (10.0, -10.0)
    mx = max((max(xs) - min(xs)) * 0.05, 8.0)
    my = max((max(ys) - min(ys)) * 0.05, 8.0)
    return (min(xs) - mx, max(xs) + mx), (max(ys) + my, min(ys) - my)


def draw_capped_paths(
    ax: Axes,
    capped: Sequence[Sequence[Point]],
    *,
    ref_idx: int,
    ref_path: Sequence[Point],
    cap_ms: float,
    metrics_mode: str,
    live_path: Optional[Sequence[Point]] = None,
    xlim: Optional[Tuple[float, float]] = None,
    ylim: Optional[Tuple[float, float]] = None,
    scale: float = 1.0,
) -> None:
    """Draw capped path overlay on ax. scale>1 thickens lines/markers for export."""
    ax.clear()
    mode_label = "mean path ref" if metrics_mode == "meanPathRef" else "shortest time ref"
    ax.set_xlabel("ΔX (px)", fontsize=10 * scale)
    ax.set_ylabel("ΔY (px)", fontsize=10 * scale)
    ax.grid(True, alpha=0.3)
    ax.invert_yaxis()

    if not capped and not live_path:
        ax.set_title("Capped paths (shortest time cutoff)", fontsize=11 * scale)
        return

    n_series = len(capped) + (1 if live_path else 0)
    colors = mpl_cm.tab10(np.linspace(0, 1, max(n_series, 1)))
    cap_note = f" @ {cap_ms:.0f} ms" if cap_ms > 0 else ""
    ax.set_title(f"Capped paths{cap_note} — {mode_label}", fontsize=11 * scale)

    for i, path in enumerate(capped):
        if len(path) < 2:
            continue
        xs = [p[0] for p in path]
        ys = [p[1] for p in path]
        is_ref = metrics_mode == "shortestTimeRef" and i == ref_idx
        lw = (1.4 if is_ref else 0.9) * scale
        alpha = 1.0 if is_ref else 0.55
        label = f"#{i + 1} (ref)" if is_ref else f"#{i + 1}"
        ax.plot(xs, ys, color=colors[i], alpha=alpha, linewidth=lw, label=label)

    if ref_path and len(ref_path) >= 2 and metrics_mode == "meanPathRef":
        ax.plot(
            [p[0] for p in ref_path],
            [p[1] for p in ref_path],
            color="black",
            linewidth=1.4 * scale,
            alpha=0.9,
            label="mean path",
            zorder=4,
        )

    if live_path and len(live_path) >= 2:
        ax.plot(
            [p[0] for p in live_path],
            [p[1] for p in live_path],
            color=colors[len(capped) % len(colors)],
            linewidth=1.2 * scale,
            linestyle="--",
            alpha=0.95,
            label="live",
        )
    elif live_path and len(live_path) == 1:
        ax.scatter(
            [live_path[0][0]],
            [live_path[0][1]],
            s=36 * scale,
            c="lime",
            zorder=6,
        )

    ends = np.array([p[-1] for p in capped if p])
    if len(ends):
        ax.scatter(
            ends[:, 0],
            ends[:, 1],
            c="red",
            s=48 * scale,
            zorder=5,
            label="capped endpoints",
        )

    ax.legend(loc="best", fontsize=9 * scale)
    if xlim is not None and ylim is not None:
        ax.set_xlim(xlim)
        ax.set_ylim(ylim)
    else:
        xlim_auto, ylim_auto = auto_axis_limits(capped, live_path)
        ax.set_xlim(xlim_auto)
        ax.set_ylim(ylim_auto)


def save_path_plot(
    path: Path,
    capped: Sequence[Sequence[Point]],
    *,
    ref_idx: int,
    ref_path: Sequence[Point],
    cap_ms: float,
    metrics_mode: str,
    xlim: Optional[Tuple[float, float]] = None,
    ylim: Optional[Tuple[float, float]] = None,
    dpi: int = EXPORT_DPI,
) -> None:
    """Render and save a high-resolution PNG of the path plot."""
    export_scale = dpi / DISPLAY_DPI
    fig = Figure(figsize=FIG_SIZE_INCHES, dpi=dpi)
    ax = fig.add_subplot(111)
    draw_capped_paths(
        ax,
        capped,
        ref_idx=ref_idx,
        ref_path=ref_path,
        cap_ms=cap_ms,
        metrics_mode=metrics_mode,
        xlim=xlim,
        ylim=ylim,
        scale=export_scale,
    )
    fig.tight_layout()
    path.parent.mkdir(parents=True, exist_ok=True)
    fig.savefig(path, dpi=dpi, bbox_inches="tight", facecolor="white", edgecolor="none")
