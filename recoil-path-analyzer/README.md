# Recoil Path Analyzer

Windows tool to record **cursor paths** while holding **LMB+RMB** (ADS chord, mouse mask `b: 3`) and compare multiple spray attempts. Used to tune `usb-output` recoil JSON profiles.

## Requirements

- Windows 10/11
- Python 3.11+

```bash
cd recoil-path-analyzer
pip install -r requirements.txt
```

## Run

```bash
python -m recoil_path_analyzer
```

1. Set profile label / path (for session metadata).
2. Set number of attempts and poll rate (default 500 Hz).
3. Click **Start session**.
4. For each attempt: **hold LMB+RMB together**, release when done. The current spray draws live on the chart (dashed **live** line) while recording.
5. View **capped** overlay plot (trimmed to shortest time) + metrics; files are written to `sessions/`.

The chart auto-fits to trimmed paths after each attempt. Use the **matplotlib toolbar** (below chart) to zoom/pan. Click **Docs** for rendered help on metrics and features.

## Metrics mode

Paths are **capped** at the **shortest spray duration** among attempts (click **Docs** in the app or see `docs/METRICS.md`):

- The quickest attempt defines the time cutoff; longer sprays are trimmed at that timestamp.
- **Shortest time** tab: deviation vs that reference attempt.
- **Mean path** tab: deviation vs the average capped shape.

## Output files

Each session creates three files in `sessions/`:

| File | Purpose |
|------|---------|
| `{profile}_{timestamp}.json` | Full session (points + metrics) — tools/scripts |
| `{profile}_{timestamp}_points.csv` | One row per sample — Excel, pandas |
| `{profile}_{timestamp}_metrics.csv` | Summary + per-attempt metrics |

### JSON schema

`schema`: `recoil-path-analyzer/v1`

### CSV points columns

`attempt`, `t_us`, `screen_x`, `screen_y`, `rel_x`, `rel_y`, `duration_ms`

## Hardware setup

Use the normal supermacro chain (usb-input → usb-output) with the profile loaded on the device.

For repeatability testing, set `"debugMode": true` in the profile JSON (hands-off mouse during macro spread).

## See also

- [docs/METRICS.md](docs/METRICS.md) — metric definitions, examples, and GUI features
- [RECOIL_PATH_ANALYZER_PLAN.md](../RECOIL_PATH_ANALYZER_PLAN.md)
