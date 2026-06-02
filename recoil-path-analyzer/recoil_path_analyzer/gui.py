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
from matplotlib.backends.backend_tkagg import FigureCanvasTkAgg, NavigationToolbar2Tk
from matplotlib.figure import Figure

from .capture import Attempt, chord_ads_down, record_attempt
from .plot_paths import (
    DISPLAY_DPI,
    EXPORT_DPI,
    FIG_SIZE_INCHES,
    auto_axis_limits,
    draw_capped_paths,
    save_path_plot,
)
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


class _HighResToolbar(NavigationToolbar2Tk):
    """Matplotlib toolbar that saves figures at export DPI."""

    def save_figure(self, *args) -> None:
        fig = self.canvas.figure
        orig_dpi = fig.dpi
        fig.set_dpi(EXPORT_DPI)
        try:
            super().save_figure(*args)
        finally:
            fig.set_dpi(orig_dpi)


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

        self.fig = Figure(figsize=FIG_SIZE_INCHES, dpi=DISPLAY_DPI)
        self.ax = self.fig.add_subplot(111)
        self.canvas = FigureCanvasTkAgg(self.fig, master=plot_frame)

        toolbar_frame = ttk.Frame(plot_frame)
        toolbar_frame.pack(side=tk.BOTTOM, fill=tk.X)
        self.canvas.get_tk_widget().pack(side=tk.TOP, fill=tk.BOTH, expand=True)
        self._nav_toolbar = _HighResToolbar(self.canvas, toolbar_frame)
        self._nav_toolbar.update()
        self._plot_axis_limits: Optional[tuple] = None

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

    def _live_path_for_plot(self) -> Optional[list]:
        if self._live_attempt and len(self._live_attempt.points) >= 1:
            return self._live_attempt.relative_points()
        return None

    def _plot_attempts(self, *, auto_fit: bool = True) -> None:
        live_path = self._live_path_for_plot()

        if not self._attempts and not live_path:
            self._default_xlim = None
            self._default_ylim = None
            self._plot_axis_limits = None
            draw_capped_paths(
                self.ax,
                [],
                ref_idx=-1,
                ref_path=[],
                cap_ms=0.0,
                metrics_mode=self._metrics_mode,
            )
            self.fig.tight_layout()
            self.canvas.draw_idle()
            return

        capped, ref_idx, ref_path, cap_ms = self._capped_paths_for_plot()
        if auto_fit:
            xlim, ylim = auto_axis_limits(capped, live_path)
            self._default_xlim = xlim
            self._default_ylim = ylim
        elif self._default_xlim and self._default_ylim:
            xlim, ylim = self._default_xlim, self._default_ylim
        else:
            xlim, ylim = auto_axis_limits(capped, live_path)

        self._plot_axis_limits = (xlim, ylim)
        draw_capped_paths(
            self.ax,
            capped,
            ref_idx=ref_idx,
            ref_path=ref_path,
            cap_ms=cap_ms,
            metrics_mode=self._metrics_mode,
            live_path=live_path,
            xlim=xlim,
            ylim=ylim,
        )
        self.fig.tight_layout()
        self.canvas.draw_idle()

    def _export_plot_pngs(self, base: Path) -> list[Path]:
        if not self._attempts:
            return []

        timed = [a.relative_points_timed() for a in self._attempts]
        capped, ref_idx, cap_ms = cap_paths_to_shortest_time(timed)[:3]
        stacked = attempts_to_arrays(capped, N_SAMPLES)
        mean_ref = [(float(x), float(y)) for x, y in mean_path_from_stack(stacked)]
        shortest_ref = (
            [(float(x), float(y)) for x, y in stacked[ref_idx]] if ref_idx >= 0 else []
        )

        xlim, ylim = self._plot_axis_limits or auto_axis_limits(capped)
        saved: list[Path] = []
        exports = (
            ("shortestTimeRef", shortest_ref, "shortest_time"),
            ("meanPathRef", mean_ref, "mean_path"),
        )
        for mode, ref_path, suffix in exports:
            out = base.with_name(base.name + f"_plot_{suffix}.png")
            save_path_plot(
                out,
                capped,
                ref_idx=ref_idx,
                ref_path=ref_path,
                cap_ms=cap_ms,
                metrics_mode=mode,
                xlim=xlim,
                ylim=ylim,
            )
            saved.append(out)
        return saved

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
            plot_paths = self._export_plot_pngs(base)
            export_lines = [json_path.name, csv_path.name, metrics_path.name]
            export_lines.extend(p.name for p in plot_paths)
            export_lines.append(f"→ {self._output_dir}")
            self.var_export.set("\n".join(export_lines))
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
