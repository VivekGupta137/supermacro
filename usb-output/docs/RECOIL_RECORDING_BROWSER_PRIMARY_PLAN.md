# Recoil Recording Plan (Browser-Primary)

## Objective

Implement recoil spray recording with high accuracy while keeping ESP32 memory usage low by making the browser the primary storage and processing layer.

## Scope

- New recording UI page (`/record`) for configuration, live logs, and session management.
- Device captures and streams only (no long-term recording persistence on ESP).
- Browser stores only the latest 10 sessions in `localStorage`.

## Final Architecture

### Device (ESP32S3)

- Detect recording trigger from LMB according to selected mode (`tap` / `hold`).
- Sample mouse spray data at configured interval (`sampleUs`, primary) or `sampleRateHz` (derived UI helper).
- Batch and stream samples over existing `/ws` every 500 ms (default) or 1000 ms.
- Emit explicit session boundary and state events.
- Do not persist recording history on device.

### Browser

- Receive stream, assemble sessions by `sessionId` and `batchSeq`.
- Render log output in textarea with clear separators between sessions.
- Persist sessions locally; keep newest 10 only.
- Export/convert sessions to macro `steps` for customization.

## Why Browser-Primary

### Pros

- Removes tight ESP memory cap constraints for long recordings.
- Simpler on-device memory lifecycle.
- Faster iteration for tooling (logs, export, visualization).
- Easier retention and editing in browser.

### Tradeoffs

- WS disconnect may lose unsent data.
- Requires sequence/gap handling for correctness.
- Browser storage is limited (but larger than ESP RAM).

## Sampling Model (Improved)

Use **time-interval sampling in microseconds** as the source-of-truth:

- Primary config: `sampleUs` (for example `1000`, `2000`, `4000`, `8000`).
- Optional UI convenience: `sampleRateHz` mirrors `sampleUs` (`Hz = 1_000_000 / sampleUs`).
- On device, lock `sampleUs` for the whole session once recording starts.

Why this is better for tracking:

- Matches your final output format (`steps[].us`) directly.
- Avoids float/rounding drift from repeated Hz conversions.
- Makes high-resolution capture deterministic over long sessions.

## Accuracy and Reliability Rules

To maintain long-term recording quality:

1. Use device monotonic time (`esp_timer`) for timestamps.
2. Include `batchSeq` in each batch and enforce order on browser side.
3. Detect and mark missing batches as gaps (never silently smooth over).
4. Prefer 500 ms batch interval for lower loss window.
5. Keep `sampleUs` fixed per session (locked while recording).
6. Capture from raw incoming mouse HID before macro transforms (source accuracy).
7. Aggregate deltas between sample ticks, then emit one sample per tick (`dtUs`, `dx`, `dy`).
8. Add optional overflow marker events if queue pressure causes skipped chunks.

## Recording Behavior

- Arm recording from UI.
- Trigger starts on LMB according to mode:
  - `tap`: record one tap cycle.
  - `hold`: record while LMB is held.
- Starting a new session creates a new session header in logs and local history.
- While recording is active, `sampleUs`/`sampleRateHz` controls are disabled.

## WebSocket Event Contract (Planned)

- `record_cfg_ack`
- `record_state`
- `record_session_start`
- `record_batch`
- `record_session_end`
- `record_gap`
- `record_error`

### `record_batch` minimum fields

- `sessionId`
- `batchSeq`
- `startUs`
- `endUs`
- `sampleUs`
- `sampleRateHz` (optional, derived helper)
- `sampleCount`
- `samples`: `[{ dtUs, dx, dy, wheel? }]`
- `lostSamples` (optional, for diagnostics)

## Browser Storage Model

`localStorage` key: `macro_record_sessions_v1`

Each saved session contains:

- `id`, `startedAt`, `endedAt`
- `config` (`triggerMode`, `sampleUs`, `sampleRateHz`, `batchMs`)
- `samples`
- `summary` (`count`, `durationUs`, `hasGap`, `gaps`)
- `logsText`

Retention policy:

- Sort by newest.
- Keep last 10 sessions.
- Drop older entries.

## UI Plan (`/record`)

### Controls

- Record arm toggle
- Trigger mode (`tap` / `hold`)
- Sample interval (`us`) as primary control
- Sample rate (Hz) as read-only mirror (or optional helper input auto-converted to `us`)
- Batch interval (500/1000 ms)

Recommended presets:

- 1000 us (1000 Hz) -> highest fidelity, highest traffic
- 2000 us (500 Hz) -> balanced default for long sessions
- 4000 us (250 Hz) -> lighter transport
- 8000 us (125 Hz) -> minimal bandwidth mode

### Live View

- Current state (`armed`, `recording`)
- Session id
- Elapsed time
- Sample count
- Gap count

### Logs Textarea

Format:

- Session start separator
- Config line
- Batch summaries
- Gap markers (if any)
- Session end summary

## Export / Customization Flow

- Convert recorded samples to profile `steps`:
  - `{ "us": dtUs, "mouse": { "x": dx, "y": dy } }`
- Optional browser-side processing:
  - smoothing
  - outlier clamp
  - gain scaling
  - downsample

Tracking-quality addition:

- Keep both:
  - raw recorded samples (immutable source),
  - processed samples (derived output for profile generation).
- Always allow re-generating processed output from raw without re-recording.

## Implementation status (initial)

- Firmware: `macro_record.c` streams `record_*` WS events; default `sampleUs=2000` (500 Hz), `batchMs=500`.
- HTTP: `/record`, `/api/recording/status`, `/api/recording/config`, `/api/recording/arm`.
- UI: `web/record.html` (browser storage, last 10 sessions, gap detection, export steps).

## Implementation Phases

1. Firmware recorder core + WS streaming events. **(done)**
2. Browser receiver + session assembler + gap detection.
3. `/record` page controls + live logs.
4. LocalStorage retention (last 10).
5. Export-to-steps utilities.

## Acceptance Criteria

- Recording works in both `tap` and `hold`.
- `sampleUs` affects output density and generated `steps[].us` exactly as expected.
- `sampleUs` / `sampleRateHz` controls are disabled while recording.
- Session logs are clearly separated in UI.
- Only latest 10 sessions remain in `localStorage`.
- Missing data is explicitly marked as gaps.
