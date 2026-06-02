# Recoil Path Analyzer — metrics & features

## What the tool does

While you hold **LMB+RMB** (ADS chord), the app samples the Windows cursor at high rate (default 500 Hz) and records each spray as a path in **screen pixels**, relative to the point where you pressed the buttons.

After several attempts, paths are **capped** and compared so longer sprays do not dominate the stats.

---

## Capping: shortest time cutoff

All paths are trimmed to the **shortest spray duration** among attempts.

1. Each attempt records a timestamp (`t_us`) on every sample from button press to release.
2. Find the attempt whose **last sample** is earliest — that defines the time cutoff.
3. Trim every path at that timestamp, interpolating the last point if it falls between samples.

**Example (3 attempts):**

| Attempt | Duration | Notes |
|---------|----------|-------|
| #1 | 820 ms | Shortest — defines cap |
| #2 | 910 ms | Trimmed at 820 ms |
| #3 | 880 ms | Trimmed at 820 ms |

Cutoff = **820 ms** (attempt #1). Attempts #2 and #3 keep only samples up to 820 ms. Metrics and the plot use only these **capped** segments.

Longer sprays show `tail+Nms` in per-attempt lines — how much recording time was excluded.

---

## Reference modes (tabs)

The same capped paths are analyzed against two different references:

### Shortest time (ref attempt)

- The attempt with the shortest duration is the **reference path** (drawn slightly thicker, labeled `(ref)`).
- **Deviation RMS** = average, across attempts, of the root-mean-square distance from each capped path to the reference path (paths resampled to 200 points by arc length).
- Use this when you want to know how much each spray diverges from your quickest baseline.

### Mean path

- Build a **mean path** by averaging X and Y at each resample step across all capped attempts.
- Deviations are measured from this mean line (shown in black on the Mean tab).
- Use this when you want overall spread around the typical spray shape, not one specific attempt.

Both tabs share the same time cap; only the reference line and deviation numbers change.

---

## Metric definitions

| Metric | Meaning |
|--------|---------|
| **Attempts** | Valid recorded sprays in the session |
| **Cap (ms)** | Shortest attempt duration used to trim all paths |
| **Capped endpoint σ X / Y** | Standard deviation of capped endpoints across attempts (lower = tighter landing) |
| **Capped endpoint range X / Y** | Max − min capped endpoint on each axis |
| **Deviation RMS** | Mean of per-attempt RMS distance to the reference (mean or ref path) |
| **Max deviation** | Worst per-attempt peak distance to the reference |
| **Raw duration mean** | Average full spray duration before capping |
| **Duration CV** | Coefficient of variation of spray durations (release timing consistency) |
| **Per-attempt RMS** | That attempt’s RMS distance to the active reference |
| **tail+Nms** | Duration trimmed off because the spray ran past the cap |

---

## GUI features

| Feature | Description |
|---------|-------------|
| **Live trace** | Dashed **live** line while LMB+RMB is held |
| **Capped overlay** | Completed attempts shown trimmed to shortest time |
| **Metrics tabs** | Switch between shortest-time ref and mean-path ref |
| **Docs** | Rendered markdown help (popup) |
| **Matplotlib toolbar** | Zoom and pan the plot |
| **Session export** | JSON + points CSV + metrics CSV in `sessions/` |

---

## Interpreting results (recoil tuning)

- **Low endpoint σ/range on Y** → consistent vertical recoil at the cap time.
- **Low endpoint σ/range on X** → less horizontal drift between sprays.
- **Low deviation RMS** → paths match the reference (mean or shortest time) closely.
- **High duration CV** → inconsistent release timing; check how long you hold LMB+RMB.
- Compare **Mean path** vs **Shortest time** tabs: if mean RMS is low but ref RMS is high, attempts agree on average but one short spray is an outlier shape.

---

## Hardware note

For repeatable captures, enable `"debugMode": true` in the device profile so manual mouse movement is ignored during macro spread. The checkbox in the app is metadata only — set the flag in the JSON on the ESP profile.
