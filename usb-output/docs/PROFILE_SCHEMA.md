# Macro profile JSON schema

The `usb-output` firmware loads a profile from SPIFFS (`profile.json` by default) at boot and can **hot-apply** a new profile over HTTP `POST /api/profile` without reboot.

Live status (profile name, active script, macros on/off) is available as JSON from `GET /api/status` and is pushed over **WebSocket** `GET /ws` whenever the profile is applied or hotkeys change.

---

## Versions

| `v` | Meaning |
|-----|---------|
| `1` | Single list of macro **groups** (original schema). |
| `2` | Optional **multi-script banks**, **global macro enable/disable**, and **next-script** hotkeys. |

The HTTP status JSON field `schemaVer` mirrors the profile `v` you last loaded (or `1` for built-in fallback).

---

## Common rules (v1 and v2)

### `groups` array

Each element is a **macro group** (one timed sequence):

| Field | Type | Required | Description |
|-------|------|----------|-------------|
| `steps` | array | no | Timed HID steps. If omitted or empty, a single neutral keyboard step is used. |
| `n` | number | no | Use only the first `n` steps (must be ≤ number of `steps`). |
| `loop` | bool | no | If true, sequence repeats while the physical **press** trigger is still held (see loop checks in firmware). |
| `press` | object | no | HID **chord** that must be **newly pressed** (rising edge) to start the sequence. |
| `release` | object | no | Keyboard-only: fires on **release** edge of this chord. |
| `save` | object | no | Keyboard: starts “recording” mode for this sequence. |
| `comment` | string | no | Ignored by firmware; for humans only. |

**Dense prefix rule:** In the firmware loop, processing stops at the **first group with `size == 0`**. Put all used groups first; do not leave gaps.

### Step object (`steps[]`)

| Field | Type | Required | Description |
|-------|------|----------|-------------|
| `us` | number | yes | Duration of this step in microseconds. |
| `mouse` | object | one of | Relative mouse HID payload. |
| `kbd` | object | one of | Keyboard HID payload. |

### Mouse payload (`mouse`)

| Field | Type | Description |
|-------|------|-------------|
| `b` | number | **Button bitmask** (see table below). Multiple bits = **chord** (e.g. left+right = `3`). Matching uses “all set bits must be pressed”. |
| `x`, `y` | number | int8 movement. |
| `w` | number | int8 wheel delta. |
| `p` | number | int8 pan/horizontal wheel if exposed. |

### Keyboard payload (`kbd`)

| Field | Type | Description |
|-------|------|-------------|
| `m` | number | Modifier bitmask (USB HID modifiers). |
| `k` | array of numbers | Up to six USB keycodes that must all be present (chord). |

### Press / release / save triggers

Same shape as a step’s `mouse` or `kbd` wrapper, for example:

```json
"press": { "mouse": { "b": 6 } }
```

**Rising edge:** `press` and v2 hotkeys fire when the chord becomes **newly** true this frame, not every HID poll while held.

---

## USB HID mouse button bitmask (`b`)

Typical gaming mice follow the standard HID button bits in the first byte:

| Bit (hex) | Decimal `b` | Button |
|-----------|-------------|--------|
| `0x01` | `1` | Left |
| `0x02` | `2` | Right |
| `0x04` | `4` | Middle (wheel click) |
| `0x08` | `8` | Back |
| `0x10` | `16` | Forward |

**Examples**

| Combination | `b` value |
|-------------|-----------|
| Left + right | `3` |
| Right + middle | `6` |
| Back + forward | `24` |

---

## Schema v1

```json
{
  "v": 1,
  "name": "my-profile",
  "groups": [ /* ... */ ]
}
```

| Field | Type | Description |
|-------|------|-------------|
| `v` | number | Must be `1`. |
| `name` | string | Shown in `/api/status` as `profile`. |
| `groups` | array | Macro groups (see above). |

---

## Schema v2

```json
{
  "v": 2,
  "name": "multi-weapon",
  "macrosOn": true,
  "activeScript": 0,
  "scripts": [
    { "name": "rifle", "groups": [ /* ... */ ] },
    { "name": "smg", "groups": [ /* ... */ ] }
  ],
  "nextScript": { "mouse": { "b": 4 } },
  "toggleMacros": { "mouse": { "b": 6 } }
}
```

| Field | Type | Description |
|-------|------|-------------|
| `v` | number | Must be `2`. |
| `name` | string | Profile name (global). |
| `macrosOn` | bool | Initial global macro enable (default true if omitted). |
| `activeScript` | number | Index into `scripts` (0-based). Clamped on load. |
| `scripts` | array | Each item: `name` (optional) and `groups` (same as v1). Max **4** scripts (firmware limit). |
| `groups` | array | If `scripts` is omitted, a single bank uses this root `groups` (same as v1). |
| `nextScript` | object | Rising-edge hotkey: advance `activeScript` circularly and **hot-apply** that bank. |
| `toggleMacros` | object | Rising-edge hotkey: flip global **macros on/off** (passthrough HID still runs; timed macro output stops). |

**Notes**

- `nextScript` / `toggleMacros` use the same `{ "mouse": {...} }` or `{ "kbd": {...} }` shape as `press`.
- Omit a hotkey or set it to `false` to disable that feature.
- Each script’s `groups` array follows the same rules as v1 (max length, dense prefix).

---

## Status JSON (`GET /api/status` and WebSocket payload)

Example:

```json
{
  "schemaVer": 2,
  "fw": "IDF v5.5.1",
  "profile": "multi-weapon",
  "macrosOn": true,
  "activeScript": 0,
  "scriptCount": 2,
  "scriptNames": ["rifle", "smg"]
}
```

---

## Web UI

- Open the device IP in a browser (SoftAP or STA per your build).
- Quick walkthrough page is available at `/documentation` (rendered from `docs/QUICK_WALKTHROUGH.md` at build time).
- The page shows **active script** and **macros on/off**, polls `/api/status`, and opens **`ws://<host>/ws`** for live updates when **`CONFIG_HTTPD_WS_SUPPORT`** is enabled in sdkconfig (enabling **Macro profile / web UI** in menuconfig selects this for you).
- After `POST /api/profile`, the WebSocket receives a fresh status object when WS is enabled; otherwise only polling updates the page.

For a deeper conceptual guide and scenario-based examples, see `docs/MACRO_STRUCTURE_DETAILED_GUIDE.md`.

---

## Minimal v1 example

See `profiles/mouse_only_example.json` in the repo.

---

## Minimal v2 example (two scripts + hotkeys)

See `profiles/example_v2_multiscript.json`.

---

## Design notes (why JSON, not Lua)

- **Chords** are expressed naturally as bitmask `b` or multi-key `k` arrays; the matcher already requires all bits/keys to be present.
- **Rising-edge** behaviour avoids toggles or weapon cycling firing every millisecond while a button is held.
- **Multi-script** keeps each “gun script” as an isolated `groups` list while sharing one SPIFFS file and one upload path.

If you need behaviour that cannot be expressed (e.g. time-of-day logic), extend the C firmware or add a small domain-specific field in v3 rather than embedding a full scripting language on the device.
