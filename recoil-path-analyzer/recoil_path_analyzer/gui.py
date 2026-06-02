"""Tkinter GUI for recoil path recording and visualization."""

from __future__ import annotations

import threading
import time
import tkinter as tk
from datetime import datetime
from pathlib import Path
from tkinter import messagebox, ttk
from typing import List, Optional

import matplotlib

matplotlib.use("TkAgg")
import matplotlib.cm as mpl_cm
from matplotlib.backends.backend_tkagg import FigureCanvasTkAgg, NavigationToolbar2Tk
from matplotlib.figure import Figure
import numpy as np

from .capture import Attempt, chord_ads_down, record_attempt
from .metrics import (
    N_SAMPLES,
    attempts_to_arrays,
    cap_paths_to_shortest_time,
    compute_session_metrics,
    format_metrics_text,
    mean_path_from_stack,
)
from .docs_viewer import show_docs_modal
from .session_io import (
    build_session_document,
    save_metrics_csv,
    save_session_csv,
    save_session_json,
)


class RecoilPathApp(tk.Tk):
    def __init__(self) -> None:
        super().__init__()
        self.title("Recoil Path Analyzer")
        self.geometry("1100x780")
        self.minsize(900, 600)

        self._stop = threading.Event()
        self._worker: Optional[threading.Thread] = None
        self._attempts: List[Attempt] = []
        self._metrics: dict = {}
        self._output_dir = Path(__file__).resolve().parent.parent / "sessions"
        self._default_xlim: Optional[tuple] = None
        self._default_ylim: Optional[tuple] = None
        self._live_attempt: Optional[Attempt] = None
        self._live_fit_done = False
        self._metrics_mode = "shortestTimeRef"

        self._build_ui()
        self.protocol("WM_DELETE_WINDOW", self._on_close)

    def _build_ui(self) -> None:
        top = ttk.Frame(self, padding=8)
        top.pack(fill=tk.X)

        ttk.Label(top, text="Profile label:").grid(row=0, column=0, sticky=tk.W, padx=(0, 4))
        self.var_profile = tk.StringVar(value="rust-ak-smooth-v3")
        ttk.Entry(top, textvariable=self.var_profile, width=28).grid(row=0, column=1, sticky=tk.W)

        ttk.Label(top, text="Profile path:").grid(row=0, column=2, sticky=tk.W, padx=(12, 4))
        self.var_profile_path = tk.StringVar(value="usb-output/profiles/rust-ak-smooth-v3.json")
        ttk.Entry(top, textvariable=self.var_profile_path, width=40).grid(row=0, column=3, sticky=tk.W)

        ttk.Label(top, text="Attempts:").grid(row=1, column=0, sticky=tk.W, pady=(6, 0))
        self.var_attempts = tk.IntVar(value=5)
        ttk.Spinbox(top, from_=1, to=50, textvariable=self.var_attempts, width=6).grid(
            row=1, column=1, sticky=tk.W, pady=(6, 0)
        )

        ttk.Label(top, text="Poll Hz:").grid(row=1, column=2, sticky=tk.W, padx=(12, 4), pady=(6, 0))
        self.var_poll_hz = tk.IntVar(value=500)
        ttk.Spinbox(top, from_=50, to=2000, textvariable=self.var_poll_hz, width=6).grid(
            row=1, column=3, sticky=tk.W, pady=(6, 0)
        )

        self.var_debug = tk.BooleanVar(value=True)
        ttk.Checkbutton(
            top,
            text="Profile debugMode (note only — hands-off on device)",
            variable=self.var_debug,
        ).grid(row=2, column=0, columnspan=4, sticky=tk.W, pady=(6, 0))

        btn_row = ttk.Frame(self, padding=(8, 0))
        btn_row.pack(fill=tk.X)
        self.btn_start = ttk.Button(btn_row, text="Start session", command=self._start_session)
        self.btn_start.pack(side=tk.LEFT, padx=(0, 6))
        self.btn_stop = ttk.Button(btn_row, text="Stop", command=self._stop_session, state=tk.DISABLED)
        self.btn_stop.pack(side=tk.LEFT, padx=(0, 6))
        ttk.Button(btn_row, text="Open sessions folder", command=self._open_sessions).pack(side=tk.LEFT)
        ttk.Button(btn_row, text="Docs", command=lambda: show_docs_modal(self)).pack(side=tk.LEFT, padx=(8, 0))

        self.var_status = tk.StringVar(
            value="Ready. Hold LMB+RMB together to record each attempt (ADS chord)."
        )
        ttk.Label(self, textvariable=self.var_status, padding=8).pack(fill=tk.X)

        paned = ttk.PanedWindow(self, orient=tk.HORIZONTAL)
        paned.pack(fill=tk.BOTH, expand=True, padx=8, pady=(0, 8))

        plot_frame = ttk.Frame(paned)
        paned.add(plot_frame, weight=3)

        self.fig = Figure(figsize=(6, 5), dpi=100)
        self.ax = self.fig.add_subplot(111)
        self.ax.set_title("Capped paths (shortest time cutoff)")
        self.ax.set_xlabel("ΔX (px)")
        self.ax.set_ylabel("ΔY (px)")
        self.ax.grid(True, alpha=0.3)
        self.ax.invert_yaxis()
        self.canvas = FigureCanvasTkAgg(self.fig, master=plot_frame)
        self.canvas.get_tk_widget().pack(fill=tk.BOTH, expand=True)
        self._nav_toolbar = NavigationToolbar2Tk(self.canvas, plot_frame)
        self._nav_toolbar.update()

        right = ttk.Frame(paned)
        paned.add(right, weight=1)

        self._metrics_notebook = ttk.Notebook(right)
        self._metrics_notebook.pack(fill=tk.BOTH, expand=True)

        self.txt_shortest = self._make_metrics_text(self._metrics_notebook, "Shortest time")
        self.txt_mean = self._make_metrics_text(self._metrics_notebook, "Mean path")

        self._metrics_notebook.bind("<<NotebookTabChanged>>", self._on_metrics_tab_changed)

        ttk.Label(right, text="Last export", font=("", 10, "bold")).pack(anchor=tk.W, pady=(8, 0))
        self.var_export = tk.StringVar(value="—")
        ttk.Label(right, textvariable=self.var_export, wraplength=280).pack(anchor=tk.W, pady=(4, 0))

    def _make_metrics_text(self, parent: ttk.Notebook, title: str) -> tk.Text:
        frame = ttk.Frame(parent)
        parent.add(frame, text=title)
        txt = tk.Text(frame, height=18, width=36, wrap=tk.WORD, font=("Consolas", 9))
        txt.pack(fill=tk.BOTH, expand=True, padx=2, pady=2)
        txt.insert("1.0", "No session yet.")
        txt.config(state=tk.DISABLED)
        return txt

    def _on_metrics_tab_changed(self, _event=None) -> None:
        tab_id = self._metrics_notebook.select()
        tab_text = self._metrics_notebook.tab(tab_id, "text")
        if tab_text == "Mean path":
            self._metrics_mode = "meanPathRef"
        elif tab_text == "Shortest time":
            self._metrics_mode = "shortestTimeRef"
        else:
            return
        self._plot_attempts(auto_fit=True)

    def _open_sessions(self) -> None:
        self._output_dir.mkdir(parents=True, exist_ok=True)
        import os
        import subprocess
        import sys

        path = str(self._output_dir.resolve())
        if sys.platform == "win32":
            os.startfile(path)
        elif sys.platform == "darwin":
            subprocess.run(["open", path], check=False)
        else:
            subprocess.run(["xdg-open", path], check=False)

    def _set_metrics_text(self, text: str, mode: str = "shortestTimeRef") -> None:
        target = self.txt_shortest if mode == "shortestTimeRef" else self.txt_mean
        target.config(state=tk.NORMAL)
        target.delete("1.0", tk.END)
        target.insert("1.0", text)
        target.config(state=tk.DISABLED)

    def _refresh_all_metrics_text(self) -> None:
        if not self._metrics:
            self._set_metrics_text("No session yet.", "shortestTimeRef")
            self._set_metrics_text("No session yet.", "meanPathRef")
            return
        self._set_metrics_text(
            format_metrics_text(self._metrics, "shortestTimeRef"),
            "shortestTimeRef",
        )
        self._set_metrics_text(
            format_metrics_text(self._metrics, "meanPathRef"),
            "meanPathRef",
        )

    def _capped_paths_for_plot(self) -> tuple[list, int, list, float]:
        if not self._attempts:
            return [], -1, [], 0.0

        timed = [a.relative_points_timed() for a in self._attempts]
        capped, ref, cap_ms = cap_paths_to_shortest_time(timed)

        ref_path = self._reference_path_for_mode()
        if not ref_path and capped:
            stacked = attempts_to_arrays(capped, N_SAMPLES)
            if self._metrics_mode == "meanPathRef":
                ref_path = [(float(x), float(y)) for x, y in mean_path_from_stack(stacked)]
            elif ref >= 0:
                ref_path = [(float(x), float(y)) for x, y in stacked[ref]]
        return capped, ref, ref_path, cap_ms

    def _reference_path_for_mode(self) -> list:
        if not self._metrics:
            return []
        block = self._metrics.get(self._metrics_mode) or self._metrics
        return block.get("referencePath") or []

    def _auto_axis_limits(self, capped: list, live_path: Optional[list] = None) -> None:
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
            return
        mx = max((max(xs) - min(xs)) * 0.05, 8.0)
        my = max((max(ys) - min(ys)) * 0.05, 8.0)
        self.ax.set_xlim(min(xs) - mx, max(xs) + mx)
        self.ax.set_ylim(max(ys) + my, min(ys) - my)

    def _plot_attempts(self, *, auto_fit: bool = True) -> None:
        self.ax.clear()
        mode_label = "mean path ref" if self._metrics_mode == "meanPathRef" else "shortest time ref"
        self.ax.set_xlabel("ΔX (px)")
        self.ax.set_ylabel("ΔY (px)")
        self.ax.grid(True, alpha=0.3)
        self.ax.invert_yaxis()

        if not self._attempts and not self._live_attempt:
            self._default_xlim = None
            self._default_ylim = None
            self.canvas.draw_idle()
            return

        capped, ref_idx, ref_path, cap_ms = self._capped_paths_for_plot()
        n_series = len(capped) + (1 if self._live_attempt else 0)
        colors = mpl_cm.tab10(np.linspace(0, 1, max(n_series, 1)))

        if cap_ms > 0:
            cap_note = f" @ {cap_ms:.0f} ms"
        else:
            cap_note = ""
        self.ax.set_title(f"Capped paths{cap_note} — {mode_label}")

        for i, path in enumerate(capped):
            if len(path) < 2:
                continue
            xs = [p[0] for p in path]
            ys = [p[1] for p in path]
            is_ref_attempt = (
                self._metrics_mode == "shortestTimeRef" and i == ref_idx
            )
            lw = 1.0 if is_ref_attempt else 0.6
            alpha = 1.0 if is_ref_attempt else 0.55
            if is_ref_attempt:
                label = f"#{i + 1} (ref)"
            else:
                label = f"#{i + 1}"
            self.ax.plot(xs, ys, color=colors[i], alpha=alpha, linewidth=lw, label=label)

        if ref_path and len(ref_path) >= 2 and self._metrics_mode == "meanPathRef":
            self.ax.plot(
                [p[0] for p in ref_path],
                [p[1] for p in ref_path],
                color="black",
                linewidth=1.0,
                alpha=0.9,
                label="mean path",
                zorder=4,
            )

        live_path = None
        if self._live_attempt and len(self._live_attempt.points) >= 1:
            live_path = self._live_attempt.relative_points()
            if len(live_path) >= 2:
                self.ax.plot(
                    [p[0] for p in live_path],
                    [p[1] for p in live_path],
                    color=colors[len(capped) % len(colors)],
                    linewidth=0.9,
                    linestyle="--",
                    alpha=0.95,
                    label="live",
                )
            elif len(live_path) == 1:
                self.ax.scatter([live_path[0][0]], [live_path[0][1]], s=30, c="lime", zorder=6)

        ends = np.array([p[-1] for p in capped if p])
        if len(ends):
            self.ax.scatter(ends[:, 0], ends[:, 1], c="red", s=40, zorder=5, label="capped endpoints")

        self.ax.legend(loc="best", fontsize=8)
        self.fig.tight_layout()
        if auto_fit:
            self._auto_axis_limits(capped, live_path)
            self._default_xlim = self.ax.get_xlim()
            self._default_ylim = self.ax.get_ylim()
        self.canvas.draw_idle()

    def _start_session(self) -> None:
        if self._worker and self._worker.is_alive():
            return
        self._stop.clear()
        self._attempts.clear()
        self._metrics = {}
        self._live_attempt = None
        self._live_fit_done = False
        self._plot_attempts()
        self._refresh_all_metrics_text()
        self._set_metrics_text("Recording…", "shortestTimeRef")
        self._set_metrics_text("Recording…", "meanPathRef")
        self.btn_start.config(state=tk.DISABLED)
        self.btn_stop.config(state=tk.NORMAL)
        self._worker = threading.Thread(target=self._session_worker, daemon=True)
        self._worker.start()

    def _stop_session(self) -> None:
        self._stop.set()
        self.var_status.set("Stopping…")

    def _session_worker(self) -> None:
        try:
            n = int(self.var_attempts.get())
            poll_hz = max(50, int(self.var_poll_hz.get()))
            poll_s = 1.0 / poll_hz
            recorded: List[Attempt] = []

            for i in range(n):
                if self._stop.is_set():
                    break
                self._ui_status(f"Attempt {i + 1}/{n}: hold LMB+RMB to record…")
                while not self._stop.is_set():
                    if chord_ads_down():
                        break
                    time.sleep(0.02)

                if self._stop.is_set():
                    break

                self._ui_status(f"Attempt {i + 1}/{n}: recording…")
                last_live_ui = [0.0]
                self._live_fit_done = False

                def on_point(att: Attempt) -> None:
                    now = time.perf_counter()
                    if now - last_live_ui[0] < 1.0 / 30.0:
                        return
                    last_live_ui[0] = now
                    self._ui_plot_live(att)

                att = record_attempt(poll_s, self._stop.is_set, on_point=on_point)
                self._live_attempt = None
                self._ui_plot_live(None)
                if att is None:
                    if self._stop.is_set():
                        break
                    self._ui_status(f"Attempt {i + 1} skipped (too short). Retry chord.")
                    continue
                att.index = len(recorded)
                recorded.append(att)
                self._attempts = list(recorded)
                self._metrics = compute_session_metrics(recorded)
                self._ui_plot()
                self._ui_refresh_metrics()

                if i + 1 < n and not self._stop.is_set():
                    self._ui_status(f"Attempt {i + 1} done. Release buttons, then next ({i + 2}/{n})…")
                    while chord_ads_down() and not self._stop.is_set():
                        time.sleep(0.02)
                    for _ in range(15):
                        if self._stop.is_set() or not chord_ads_down():
                            break
                        time.sleep(0.05)

            self._attempts = recorded
            self._metrics = compute_session_metrics(recorded) if recorded else {}
            self._ui_finish()
        except Exception as exc:
            self._ui_error(str(exc))

    def _ui_status(self, msg: str) -> None:
        self.after(0, lambda: self.var_status.set(msg))

    def _ui_refresh_metrics(self) -> None:
        self.after(0, self._refresh_all_metrics_text)

    def _ui_plot(self) -> None:
        self.after(0, lambda: self._plot_attempts(auto_fit=True))

    def _ui_plot_live(self, att: Optional[Attempt]) -> None:
        def update() -> None:
            self._live_attempt = att
            auto_fit = False
            if att and len(att.points) >= 2 and not self._live_fit_done:
                auto_fit = True
                self._live_fit_done = True
            self._plot_attempts(auto_fit=auto_fit)

        self.after(0, update)

    def _ui_finish(self) -> None:
        def done() -> None:
            self.btn_start.config(state=tk.NORMAL)
            self.btn_stop.config(state=tk.DISABLED)
            if not self._attempts:
                self.var_status.set("Session ended with no valid attempts.")
                self._refresh_all_metrics_text()
                return

            self._metrics = compute_session_metrics(self._attempts)
            self._plot_attempts()
            self._refresh_all_metrics_text()

            stamp = datetime.now().strftime("%Y%m%d_%H%M%S")
            label = self.var_profile.get().strip() or "session"
            safe = "".join(c if c.isalnum() or c in "-_" else "_" for c in label)
            base = self._output_dir / f"{safe}_{stamp}"
            doc = build_session_document(
                self._attempts,
                profile_label=self.var_profile.get(),
                profile_path=self.var_profile_path.get(),
                debug_mode=bool(self.var_debug.get()),
                poll_hz=float(self.var_poll_hz.get()),
                metrics=self._metrics,
            )
            json_path = base.with_suffix(".json")
            csv_path = base.with_name(base.name + "_points").with_suffix(".csv")
            metrics_path = base.with_name(base.name + "_metrics").with_suffix(".csv")
            save_session_json(json_path, doc)
            save_session_csv(csv_path, self._attempts)
            save_metrics_csv(metrics_path, self._metrics)
            self.var_export.set(
                f"{json_path.name}\n{csv_path.name}\n{metrics_path.name}\n→ {self._output_dir}"
            )
            self.var_status.set(
                f"Done — {len(self._attempts)} attempt(s). Files saved to sessions/."
            )

        self.after(0, done)

    def _ui_error(self, msg: str) -> None:
        def err() -> None:
            self.btn_start.config(state=tk.NORMAL)
            self.btn_stop.config(state=tk.DISABLED)
            messagebox.showerror("Error", msg)
            self.var_status.set(f"Error: {msg}")

        self.after(0, err)

    def _on_close(self) -> None:
        self._stop.set()
        if self._worker and self._worker.is_alive():
            self._worker.join(timeout=2.0)
        self.destroy()


def run_app() -> None:
    app = RecoilPathApp()
    app.mainloop()
