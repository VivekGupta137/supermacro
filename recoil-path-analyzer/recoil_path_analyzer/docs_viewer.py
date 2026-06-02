"""Markdown documentation popup for the GUI."""

from __future__ import annotations

import re
import tkinter as tk
from pathlib import Path
from tkinter import ttk

import markdown
from tkhtmlview import HTMLScrolledText

_DOCS_FILENAME = "METRICS.md"

# tkhtmlview only applies inline style= attributes (no <style> blocks or document wrapper).
_TAG_STYLES: dict[str, str] = {
    "h1": "font-size: 22px; font-weight: bold;",
    "h2": "font-size: 18px; font-weight: bold;",
    "h3": "font-size: 15px; font-weight: bold;",
    "p": "font-size: 13px;",
    "li": "font-size: 13px;",
    "code": "font-family: Consolas; font-size: 12px; background-color: #f0f0f0;",
    "pre": "font-family: Consolas; font-size: 12px; background-color: #f5f5f5;",
    "th": "font-weight: bold; background-color: #eeeeee; font-size: 13px;",
    "td": "font-size: 13px;",
    "table": "font-size: 13px;",
}


def _docs_candidates() -> list[Path]:
    pkg_root = Path(__file__).resolve().parent.parent
    cwd = Path.cwd()
    return [
        pkg_root / "docs" / _DOCS_FILENAME,
        cwd / "docs" / _DOCS_FILENAME,
        cwd / "recoil-path-analyzer" / "docs" / _DOCS_FILENAME,
    ]


def load_docs_markdown() -> tuple[str, Path | None]:
    for path in _docs_candidates():
        if path.is_file():
            return path.read_text(encoding="utf-8"), path
    fallback = (
        f"# Documentation not found\n\n"
        f"Expected `{_DOCS_FILENAME}` under `recoil-path-analyzer/docs/`.\n\n"
        f"Tried:\n"
        + "\n".join(f"- `{p}`" for p in _docs_candidates())
    )
    return fallback, None


def _inject_tag_style(html: str, tag: str, style: str) -> str:
    return re.sub(
        rf"<{tag}(?=\s|>|/)",
        f'<{tag} style="{style}"',
        html,
        flags=re.IGNORECASE,
    )


def _enhance_html_for_tkhtmlview(html: str) -> str:
    """Convert markdown HTML into a tkhtmlview-friendly fragment with inline styles."""
    html = re.sub(
        r"<hr\s*/?>",
        '<p style="color: #aaaaaa;">────────────────</p>',
        html,
        flags=re.IGNORECASE,
    )
    for wrapper in ("thead", "tbody", "html", "head", "body"):
        html = re.sub(rf"</?{wrapper}\s*>", "", html, flags=re.IGNORECASE)
    html = re.sub(r"<!DOCTYPE[^>]*>", "", html, flags=re.IGNORECASE)
    html = re.sub(r"<style[^>]*>.*?</style>", "", html, flags=re.IGNORECASE | re.DOTALL)
    for tag, style in _TAG_STYLES.items():
        html = _inject_tag_style(html, tag, style)
    return html.strip()


def markdown_to_html(md_text: str) -> str:
    body = markdown.markdown(
        md_text,
        extensions=["tables", "fenced_code", "sane_lists"],
    )
    return _enhance_html_for_tkhtmlview(body)


def show_docs_modal(parent: tk.Misc) -> None:
    md_text, _source = load_docs_markdown()
    html = markdown_to_html(md_text)

    modal = tk.Toplevel(parent)
    modal.title("Documentation — Recoil Path Analyzer")
    modal.geometry("760x680")
    modal.minsize(520, 400)
    modal.transient(parent.winfo_toplevel())
    modal.grab_set()

    toolbar = ttk.Frame(modal, padding=(8, 8, 8, 0))
    toolbar.pack(fill=tk.X)
    ttk.Label(toolbar, text="Metrics, capping, and GUI features", font=("", 10, "bold")).pack(
        side=tk.LEFT
    )

    viewer = HTMLScrolledText(
        modal,
        html=html,
        padx=10,
        pady=10,
        background="white",
        borderwidth=0,
    )
    viewer.pack(fill=tk.BOTH, expand=True, padx=8, pady=(4, 0))
    viewer.config(state=tk.DISABLED)

    btn_row = ttk.Frame(modal, padding=8)
    btn_row.pack(fill=tk.X)
    ttk.Button(btn_row, text="Close", command=modal.destroy).pack(side=tk.RIGHT)

    modal.focus_set()
    modal.protocol("WM_DELETE_WINDOW", modal.destroy)
