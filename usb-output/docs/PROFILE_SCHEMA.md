# Macro profile JSON schema (complete reference)

This document describes the JSON profile format consumed by **usb-output** firmware. Profiles define timed HID macro sequences (mouse movement, keys) triggered by button chords.

**Related docs:** [QUICK_WALKTHROUGH.md](./QUICK_WALKTHROUGH.md) · [MACRO_STRUCTURE_DETAILED_GUIDE.md](./MACRO_STRUCTURE_DETAILED_GUIDE.md)

---

## Table of contents

1. [Overview](#overview)
2. [How profiles are loaded](#how-profiles-are-loaded)
3. [Firmware limits](#firmware-limits)
4. [Choosing a schema version](#choosing-a-schema-version)
5. [Shared concepts (all versions)](#shared-concepts-all-versions)
6. [Sensitivity scaling (`eDPI`)](#sensitivity-scaling-edpi)
7. [Mouse and keyboard payloads](#mouse-and-keyboard-payloads)
8. [Triggers and matching](#triggers-and-matching)
9. [Schema v1](#schema-v1)
10. [Schema v2](#schema-v2)
11. [Schema v3](#schema-v3)
12. [Status API](#status-api)
13. [Web UI and HTTP API](#web-ui-and-http-api)
14. [Example profiles in the repo](#example-profiles-in-the-repo)
15. [Troubleshooting](#troubleshooting)

---

## Overview

A **profile** is a single JSON file (default name: `profile.json` on SPIFFS) containing:

- One or more **timed sequences** of HID events (`steps`)
- **Triggers** that start each sequence (usually mouse button chords)
- Optional **sensitivity scaling** and per-weapon / per-mode multipliers
- (v2/v3) Multiple **weapon banks** and **hotkeys** to switch or disable macros

The device sits between your mouse/keyboard and the PC. When a macro runs, it injects **script mouse deltas** on a timer while still forwarding your real button state.

```
Profile JSON  →  parse at boot / upload  →  group_sequence in RAM  →  esp_timer ticks  →  USB HID out
```

---

## How profiles are loaded

| Method | When |
|--------|------|
| **SPIFFS at boot** | If `CONFIG_MACRO_WEB_UI` is enabled and `profile.json` exists on the `storage` partition, it is parsed in `macro_profile_init()`. |
| **HTTP POST** | `POST /api/profile` with `Content-Type: application/json` body — validates, saves to SPIFFS, applies immediately (no reboot). |
| **Built-in fallback** | If SPIFFS load fails, compile-time default from `macro_defaults.c` is used. |

**Important:** Mouse step values are **scaled when the profile is parsed** (boot or upload). Editing a file on your PC does not change the device until you **upload** again or replace SPIFFS and reboot.

---

## Firmware limits

| Limit | Value |
|-------|--------|
| Schema versions accepted | `1`, `2`, `3` |
| Groups / modes per active bank | **10** (`MAX_KEY_MODIFICATION_SEQUENCE`) |
| Steps per group / mode | **100** (`MAX_KEY_MODIFICATION_EVENT`) |
| `n` vs `steps` | **`n` must be ≤ `steps.length`** (strict: if `n` > step count, upload/parse fails) |
| Weapon / script banks (v2/v3) | **4** (`MAX_MACRO_SCRIPTS`) |
| Profile file size | Menuconfig `CONFIG_MACRO_PROFILE_MAX_SIZE` |

**Dense prefix rule:** Groups are stored in array order `list[0]`, `list[1]`, … Firmware stops at the **first entry with `size == 0`**. Do not leave empty gaps between used groups.

---

## Choosing a schema version

| Version | Best for |
|---------|----------|
| **v1** | Simple profiles: one list of macros, no weapon switching. |
| **v2** | Several weapons (up to 4), switch active weapon with a hotkey; one trigger per macro in the active bank. |
| **v3** | Same as v2 plus **multiple firing modes per weapon** (e.g. hipfire LMB-only vs ADS LMB+RMB) active at once, with exact button matching. |

```text
v1 ──►  groups[]
v2 ──►  scripts[] → groups[]     + activeScript + hotkeys
v3 ──►  weapons[] → modes[]       + activeWeapon + pressMode + modeScale
         (aliases: scripts / groups still accepted)
```

**Migration:** v2 profiles work unchanged as v3 (`"v": 3`). Add `pressMode: "exact"` on hipfire modes when ADS should use a different pattern.

---

## Shared concepts (all versions)

### Group / mode / sequence

A **group** (v1/v2) or **mode** (v3) is one **macro sequence**:

- A list of **steps** run in order on a timer
- Optional **`press`** trigger (rising edge) to start
- Optional **`loop`**: repeat while the trigger chord stays held

Each group becomes one `key_modification_sequence_t` in firmware.

### Step object (`steps[]`)

Each step is one timed HID output.

| Field | Type | Required | Description |
|-------|------|----------|-------------|
| `us` | number | **yes** | How long to wait **after** this step before the next one, in **microseconds** (µs). `100000` = 100 ms. |
| `mouse` | object | one of | Relative mouse report for this tick. |
| `kbd` | object | one of | Keyboard report for this tick. |

**Rejected fields** (load fails if present on any step): `mouseTo`, `interp`, `segments`, `humanize`.

**Timing tip:** For full-auto recoil, set `us` ≈ `60_000_000 / RPM` (e.g. 600 RPM → `100000` µs per bullet).

**Empty `steps`:** If omitted or `[]`, firmware inserts one neutral keyboard step (group still occupies a slot).

### Group / mode fields (all versions)

| Field | Type | Required | Default | Description |
|-------|------|----------|---------|-------------|
| `steps` | array | no | one neutral step | Timed HID events (see above). |
| `n` | number | no | all steps | Use only the first `n` steps (`1` ≤ `n` ≤ length of `steps`). |
| `loop` | bool | no | `false` | If `true`, restart from step 0 while the `press` chord remains held. |
| `press` | object | no | none | Start sequence on **rising edge** of this chord (see [Triggers](#triggers-and-matching)). |
| `release` | object | no | none | **Keyboard only:** start on release edge of chord. |
| `save` | object | no | none | **Keyboard only:** enter recording mode for this group (advanced). |
| `comment` | string | no | — | Ignored by firmware; documentation for humans. |
| `name` | string | no | — | Ignored by firmware; useful in v3 for your own reference. |
| `scale` | number | no | `1` | Per-group multiplier (v1/v2). On v3 modes, prefer **`modeScale`**. |

### Loop behaviour

1. **Start:** `press` chord appears (rising edge) → sequence resets and timer starts at step 0.
2. **Each tick:** Current step’s `mouse` / `kbd` is sent; timer schedules next step after `us`.
3. **Loop:** After the last step, if `loop: true` and `press` still held → step 0 again.
4. **Stop:** If `loop: true` and `press` no longer held → sequence resets and timer stops.

### What gets sent on mouse macros

On each macro tick for mouse steps:

- **Buttons** come from your real mouse (passthrough).
- **Movement** (`x`, `y`, `wheel`, `pan`) comes from the **script step only** (user aim deltas are not added on macro ticks).

The JSON flag `additiveMouse` is parsed for compatibility but does not change this behaviour.

---

## Sensitivity scaling (`eDPI`)

### Two different roles

| Field | Who sets it | Meaning |
|-------|-------------|---------|
| **`patternEDPI`** | Profile **author** | eDPI the step patterns were **written/tuned** at. Default if omitted: **800**. |
| **`eDPI`** | **You** (player) | Your real eDPI: **mouse DPI × in-game sensitivity**. |

Firmware computes:

```text
edpiScale = eDPI / patternEDPI   (if eDPI is set and > 0; else edpiScale = 1)
```

Then per step (at profile load):

```text
finalScale = edpiScale × weaponScale × modeScale
```

Each `mouse` delta in `steps` is multiplied by `finalScale` and rounded (clamped to int8 per axis).

### Why not only one field?

They are only redundant when **your sens equals the author’s** (e.g. both 800).

| patternEDPI | eDPI | Result |
|-------------|------|--------|
| 800 | 800 | 1.0× — no global scaling |
| 800 | 960 | 1.2× — stronger pull without rewriting every `y` |
| 1200 | 800 | 0.67× — weaker pull |

**Practical rule:** Leave `patternEDPI` at the value the profile README/samples assume (usually **800**). Change only **`eDPI`** to match your setup.

### Layered multipliers (v2/v3)

| Layer | JSON location | Field name |
|-------|----------------|------------|
| Global sens | profile root | `eDPI` / `patternEDPI` |
| Weapon | `scripts[]` / `weapons[]` item | `scale` |
| Mode / group | `groups[]` / `modes[]` item | `modeScale` (v3) or `scale` |

All layers **multiply**. Example: `800/800 × 1.0 × 0.85` (ADS `modeScale`) = **0.85×** on that mode only.

---

## Mouse and keyboard payloads

### Mouse (`mouse`)

| Field | Type | Range | Description |
|-------|------|-------|-------------|
| `b` | number | 0–31 (typical) | Button **bitmask** for triggers; usually omitted on movement steps. |
| `x` | number | −128…127 | Relative X (int8 after clamp). |
| `y` | number | −128…127 | Relative Y — positive usually = move cursor **down**. |
| `w` | number | −128…127 | Vertical wheel. |
| `p` | number | −128…127 | Horizontal wheel / pan if exposed. |

### USB mouse button bitmask (`b`)

| Bit | Hex | Decimal | Button |
|-----|-----|---------|--------|
| 0 | `0x01` | **1** | Left (LMB) |
| 1 | `0x02` | **2** | Right (RMB) |
| 2 | `0x04` | **4** | Middle (MMB) |
| 3 | `0x08` | **8** | Back |
| 4 | `0x10` | **16** | Forward |

**Common chords**

| Intent | `b` |
|--------|-----|
| LMB only | `1` |
| RMB only | `2` |
| LMB + RMB (ADS) | `3` |
| RMB + MMB | `6` |
| Back + Forward | `24` |

### Keyboard (`kbd`)

| Field | Type | Description |
|-------|------|-------------|
| `m` | number | HID modifier bitmask. |
| `k` | array | Up to **6** USB keycodes; **all** must be present (chord). |

### HID modifier bitmask (`m`)

| Bit | Hex | Name |
|-----|-----|------|
| 0 | `0x01` | Left Ctrl |
| 1 | `0x02` | **Left Shift** (Rust crouch on default bindings) |
| 2 | `0x04` | Left Alt |
| 3 | `0x08` | Left GUI |
| 4 | `0x10` | Right Ctrl |
| 5 | `0x20` | Right Shift |
| 6 | `0x40` | Right Alt |
| 7 | `0x80` | Right GUI |

---

## Triggers and matching

Triggers use the same `{ "mouse": { ... } }` and/or `{ "kbd": { ... } }` wrapper as steps. **Both** may appear in one `press` object (v3): every part must be held to start and keep the macro running.

### Rising edge (`press`)

The macro **starts once** when the chord becomes newly true, not every USB poll while held.

### Mouse chord modes (v1/v2/v3)

| `pressMode` | Versions | Rule |
|-------------|----------|------|
| **`chord`** (default) | all | Every bit in `b` must be **down**; extra buttons **allowed**. |
| **`exact`** | v3 (recommended for hipfire) | `buttons` must **equal** `b` exactly — no extra bits. |

**Why v3 needs `exact`:** With default `chord`, `press: { "b": 1 }` (LMB) still matches when **LMB+RMB** (`3`) is held, so hipfire and ADS macros would conflict. Use:

- Hipfire: `"pressMode": "exact"`, `"b": 1`
- ADS: `"press": { "mouse": { "b": 3 } }` (chord is fine)

### v3 `modeSet` (peer stop)

Modes in the same weapon share a **`modeSet`** id (default: weapon index + 1). When one mode **starts**, any **other running** mode in the same set is **stopped**. When you release a chord, that mode stops on the next mouse or keyboard report sync.

### Combined mouse + keyboard `press` (v3)

Use when a mode should only run with **both** a mouse chord and a modifier/key held (e.g. crouch + fire):

```json
"pressMode": "exact",
"press": { "mouse": { "b": 1 }, "kbd": { "m": 2 } }
```

| Example | `press` |
|---------|---------|
| Hipfire standing | `{ "mouse": { "b": 1 } }` + `"pressMode": "exact"` |
| Hipfire crouch (LMB + LShift) | `{ "mouse": { "b": 1 }, "kbd": { "m": 2 } }` + `"exact"` |
| ADS standing | `{ "mouse": { "b": 3 } }` |
| ADS crouch | `{ "mouse": { "b": 3 }, "kbd": { "m": 2 } }` |

The macro starts on the **rising edge** of the full condition (e.g. Shift pressed while LMB already down, or LMB pressed while Shift already down). Releasing **either** mouse buttons or the keyboard requirement stops the mode.

---

## Schema v1

Single flat list of macro groups. All groups are **active** at once (up to 10).

### Root object

| Field | Type | Required | Description |
|-------|------|----------|-------------|
| `v` | number | **yes** | Must be `1`. |
| `name` | string | no | Shown as `profile` in status. |
| `groups` | array | **yes** | Macro groups (see [shared fields](#group--mode-fields-all-versions)). |
| `eDPI` | number | no | Your eDPI (see [scaling](#sensitivity-scaling-edpi)). |
| `patternEDPI` | number | no | Author reference eDPI (default `800`). |
| `additiveMouse` | bool | no | Legacy; no effect on tick blending. |

### Minimal v1 example — RMB pull

```json
{
  "v": 1,
  "name": "simple-rmb-pull",
  "eDPI": 800,
  "patternEDPI": 800,
  "groups": [
    {
      "comment": "Hold RMB: repeat down pull",
      "press": { "mouse": { "b": 2 } },
      "loop": true,
      "steps": [
        { "us": 200000, "mouse": { "x": 0, "y": 8 } }
      ]
    }
  ]
}
```

### v1 — multiple groups, different triggers

Each group can use a different `press` chord. **Do not** leave empty groups in the middle of the list.

```json
{
  "v": 1,
  "name": "multi-trigger",
  "groups": [
    {
      "comment": "LMB spray pattern",
      "press": { "mouse": { "b": 1 } },
      "loop": true,
      "n": 2,
      "steps": [
        { "us": 100000, "mouse": { "x": 0, "y": 4 } },
        { "us": 100000, "mouse": { "x": -1, "y": 4 } }
      ]
    },
    {
      "comment": "LMB+RMB ADS pattern",
      "press": { "mouse": { "b": 3 } },
      "loop": true,
      "steps": [
        { "us": 100000, "mouse": { "x": 0, "y": 3 } }
      ]
    }
  ]
}
```

**Note:** Without v3 `pressMode: "exact"`, the LMB group still matches during ADS (`chord` mode). Prefer **v3** for hipfire + ADS on the same weapon.

**Sample file:** `profiles/mouse_only_example.json`, `profiles/cs2_recoil_v1_multigun.json`

---

## Schema v2

Adds up to **4 script banks** (weapons), **one active bank** at a time, and optional **hotkeys**.

### Root object

| Field | Type | Required | Default | Description |
|-------|------|----------|---------|-------------|
| `v` | number | **yes** | — | Must be `2`. |
| `name` | string | no | `"flash"` | Profile display name. |
| `scripts` | array | yes* | — | Weapon banks (max 4). *Required unless root `groups` is used. |
| `groups` | array | yes* | — | Single-bank shortcut (same as one script with these groups). |
| `activeScript` | number | no | `0` | Index into `scripts` loaded at boot (0-based). |
| `macrosOn` | bool | no | `true` | Initial global enable for timed macro output. |
| `nextScript` | object / `false` | no | disabled | Rising-edge hotkey: cycle `activeScript` and reload that bank. |
| `toggleMacros` | object / `false` | no | disabled | Rising-edge hotkey: flip `macrosOn`. |
| `eDPI`, `patternEDPI`, `additiveMouse` | | no | | Same as v1. |

### Script object (`scripts[]`)

| Field | Type | Description |
|-------|------|-------------|
| `name` | string | Label in status (`scriptNames`, `activeScriptName`). |
| `scale` | number | Weapon-level multiplier (default `1`). |
| `groups` | array | Same as v1 `groups` (max 10 per script). |

Only **`scripts[activeScript].groups`** are loaded into the runtime `group_sequence`. Other scripts are stored but inactive until you cycle with `nextScript` or change `activeScript` and re-upload.

### Hotkey objects (`nextScript`, `toggleMacros`)

Same shape as `press`:

```json
"nextScript": { "mouse": { "b": 4 } }
"toggleMacros": { "mouse": { "b": 16 } }
```

Set to `false` or omit to disable. Hotkeys are evaluated on **rising edge** only.

### Full v2 example

```json
{
  "v": 2,
  "name": "cs2-recoil-samples",
  "eDPI": 960,
  "patternEDPI": 800,
  "macrosOn": true,
  "activeScript": 0,
  "nextScript": { "mouse": { "b": 4 } },
  "toggleMacros": { "mouse": { "b": 16 } },
  "scripts": [
    {
      "name": "ak47",
      "scale": 1.0,
      "groups": [
        {
          "press": { "mouse": { "b": 1 } },
          "loop": true,
          "n": 30,
          "steps": [
            { "us": 100000, "mouse": { "x": 0, "y": 4 } }
          ]
        }
      ]
    },
    {
      "name": "m4a4",
      "scale": 0.85,
      "groups": [
        {
          "press": { "mouse": { "b": 1 } },
          "loop": true,
          "steps": [
            { "us": 90000, "mouse": { "x": 0, "y": 3 } }
          ]
        }
      ]
    }
  ]
}
```

| Action | Effect |
|--------|--------|
| Mouse **back** (`b: 4`) | Switch AK → M4 → … |
| Mouse **forward** (`b: 16`) | Toggle all macros on/off |
| Hold **LMB** | Run active weapon’s pattern |

**Sample files:** `profiles/example_v2_multiscript.json`, `profiles/cs2_recoil_v2.json`, `profiles/rust_recoil_v2_ak.json`

---

## Schema v3

**Superset of v2.** Parser accepts v1/v2 fields and names. v3 adds **multiple modes per weapon** loaded **together**, with **`pressMode`**, **`modeScale`**, and **`modeSet`** peer handling.

### What v3 fixes vs v2

| Problem in v2 | v3 approach |
|---------------|-------------|
| Only one script bank active | Same (up to 4 weapons), but each weapon has **many modes** loaded at once |
| LMB macro also runs during LMB+RMB (`chord` match) | `pressMode: "exact"` on hipfire (`b: 1`) |
| Switching ADS / hipfire | Different `press` chords + `modeSet` stops the other mode |

### Root object

All v2 root fields, plus:

| Field | Type | Description |
|-------|------|-------------|
| `v` | number | Must be `3`. |
| `weapons` | array | Preferred name for script banks (max 4). |
| `scripts` | array | **Alias** for `weapons`. |
| `activeWeapon` | number | **Alias** for `activeScript`. |
| `nextWeapon` | object | **Alias** for `nextScript` (used if `nextScript` omitted). |

### Weapon object (`weapons[]` / `scripts[]`)

| Field | Type | Description |
|-------|------|-------------|
| `name` | string | `activeWeaponName` in status. |
| `scale` | number | Weapon-level multiplier. |
| `modes` | array | Firing modes (max 10 per weapon). |
| `groups` | array | **Alias** for `modes`. |

### Mode object (`modes[]` / `groups[]`)

All [shared group fields](#group--mode-fields-all-versions), plus:

| Field | Type | Default | Description |
|-------|------|---------|-------------|
| `modeScale` | number | `1` | Per-mode multiplier (hipfire vs ADS tuning). Alias: `scale` on the same object (`modeScale` wins if both set). |
| `pressMode` | string | `"chord"` | `"chord"` or `"exact"` (see [Triggers](#triggers-and-matching)). |
| `modeSet` | number | weapon index + 1 | Modes with the same id stop peers when one starts. Override only if you know you need cross-weapon grouping. |

### Complete v3 example — Rust AK hipfire + ADS

```json
{
  "v": 3,
  "name": "rust-ak-v3",
  "eDPI": 800,
  "patternEDPI": 800,
  "macrosOn": true,
  "activeWeapon": 0,
  "nextWeapon": { "mouse": { "b": 4 } },
  "toggleMacros": { "mouse": { "b": 16 } },
  "weapons": [
    {
      "name": "ak47",
      "scale": 1.0,
      "modes": [
        {
          "name": "hipfire",
          "modeScale": 1.0,
          "pressMode": "exact",
          "press": { "mouse": { "b": 1 } },
          "loop": true,
          "n": 30,
          "steps": [
            { "us": 133000, "mouse": { "x": 0, "y": 5 } },
            { "us": 133000, "mouse": { "x": 0, "y": 6 } }
          ]
        },
        {
          "name": "ads",
          "modeScale": 0.85,
          "press": { "mouse": { "b": 3 } },
          "loop": true,
          "n": 28,
          "steps": [
            { "us": 145000, "mouse": { "x": 0, "y": 4 } },
            { "us": 145000, "mouse": { "x": 0, "y": 5 } }
          ]
        }
      ]
    },
    {
      "name": "ak-holo",
      "scale": 0.95,
      "modes": [
        {
          "name": "hipfire",
          "modeScale": 0.95,
          "pressMode": "exact",
          "press": { "mouse": { "b": 1 } },
          "loop": true,
          "steps": [{ "us": 133000, "mouse": { "x": 0, "y": 5 } }]
        },
        {
          "name": "ads",
          "modeScale": 0.8,
          "press": { "mouse": { "b": 3 } },
          "loop": true,
          "steps": [{ "us": 145000, "mouse": { "x": 0, "y": 4 } }]
        }
      ]
    }
  ]
}
```

### v3 runtime flow (diagram)

```text
                    ┌─────────────────────────────────────┐
                    │  activeWeapon → load all modes      │
                    │  (hipfire + ads + … up to 10)       │
                    └─────────────────────────────────────┘
                                      │
         Mouse report ────────────────┼──────────────────────────────►
                                      ▼
              ┌───────────────────────────────────────────┐
              │ For each mode: press rising edge?         │
              │   exact: buttons == mask                  │
              │   chord:  (buttons & mask) == mask        │
              └───────────────────────────────────────────┘
                    │ yes                    │
                    ▼                        ▼
         Stop peers in same          Start timer /
         modeSet (other mode)        run steps
                    │
         Hold loop while chord held; stop when released
```

| User action | Active mode |
|-------------|-------------|
| Hold **LMB only** | `hipfire` (`exact`, `b: 1`) |
| Hold **LMB + RMB** | `ads` (`b: 3`); hipfire stopped |
| Release RMB, keep LMB | `ads` stops; `hipfire` can start |
| Mouse **back** | Switch `ak47` ↔ `ak-holo` weapon bank |

**Sample file:** `profiles/rust_ak_v3.json`

---

## Status API

`GET /api/status` returns JSON (also pushed on WebSocket `/ws` after profile/hotkey changes when enabled).

### Common fields

| Field | Description |
|-------|-------------|
| `schemaVer` | `1`, `2`, or `3` from profile `v`. |
| `profile` | Profile `name`. |
| `fw` | ESP-IDF version string. |
| `macrosOn` | Global macro output enabled. |
| `eDPI` | Parsed user eDPI (`0` if unset). |
| `patternEDPI` | Parsed reference eDPI. |
| `edpiScale` | `eDPI / patternEDPI` (or `1`). |

### v2 / v3 script / weapon fields

| Field | Description |
|-------|-------------|
| `scriptCount` | Number of banks (1–4). |
| `scriptNames` | Array of bank names. |
| `activeScript` | Active bank index. |
| `activeWeapon` | Same as `activeScript` (v3). |
| `activeScriptName` / `activeWeaponName` | Name of active bank. |
| `activeGroupCount` / `activeModeCount` | Number of loaded groups/modes. |
| `activeMouseScale` | Combined scale of first loaded mode. |
| `activeModeScales` | Array of combined scales per mode slot. |

### Example (v3)

```json
{
  "schemaVer": 3,
  "statusSeq": 12,
  "fw": "IDF v5.5.1",
  "profile": "rust-ak-v3",
  "macrosOn": true,
  "eDPI": 800,
  "patternEDPI": 800,
  "edpiScale": 1,
  "activeWeapon": 0,
  "activeWeaponName": "ak47",
  "activeModeCount": 2,
  "activeModeScales": [1, 0.85],
  "scriptCount": 2,
  "scriptNames": ["ak47", "ak-holo"]
}
```

---

## Web UI and HTTP API

Requires `CONFIG_MACRO_WEB_UI` in menuconfig (*Macro profile / web UI*).

| Endpoint | Method | Description |
|----------|--------|-------------|
| `/` | GET | Upload form + live status. |
| `/api/profile` | GET | Download current SPIFFS profile JSON. |
| `/api/profile` | POST | Upload new profile (body = raw JSON). |
| `/api/status` | GET | Status object (above). |
| `/api/next-script` | POST | Same as `nextScript` hotkey. |
| `/api/toggle-macros` | POST | Same as `toggleMacros` hotkey. |
| `/ws` | WebSocket | Status push on change (if `CONFIG_HTTPD_WS_SUPPORT`). |
| `/docs/profile-schema` | GET | This document (HTML render). |
| `/api/docs/profile-schema.md` | GET | This document (raw Markdown). |

---

## Example profiles in the repo

| File | Version | Description |
|------|---------|-------------|
| `profiles/mouse_only_example.json` | v1 | Minimal mouse macro. |
| `profiles/example_interp_additive.json` | v1 | Simple RMB loop (legacy filename). |
| `profiles/example_v2_multiscript.json` | v2 | Two scripts + hotkeys. |
| `profiles/cs2_recoil_v2.json` | v2 | CS2 rifles (script per gun). |
| `profiles/cs2_recoil_v1_multigun.json` | v1 | Many guns, different `press` chords. |
| `profiles/rust_recoil_v2_ak.json` | v2 | Rust AK attachments as separate scripts. |
| `profiles/rust_ak_v3.json` | v3 | Rust AK hipfire + ADS modes per weapon. |

---

## Troubleshooting

### Profile upload returns 400 `invalid profile`

- Check `"v"` is `1`, `2`, or `3`.
- Ensure no forbidden step fields (`mouseTo`, `interp`, …).
- Max 10 groups/modes per bank, 100 steps each, 4 banks.
- Valid JSON (trailing commas are invalid).

### Changes to `eDPI` / `scale` do nothing

- Re-**upload** the profile or reboot after editing SPIFFS.
- Confirm serial log shows your profile name (not `built-in`).
- Check `/api/status`: `edpiScale`, `activeModeScales` should reflect your values.

### Wrong macro runs (hipfire during ADS)

- Upgrade to **v3** and set hipfire to `"pressMode": "exact"`, `"b": 1`.
- ADS should use `"b": 3` (LMB+RMB).

### `activeScript` / weapon switch does nothing

- Need **2+** entries in `scripts` / `weapons`.
- `nextScript` / `nextWeapon` must be a valid rising-edge chord (e.g. mouse back `b: 4`).

### Macros never start

- `macrosOn` must be true (or toggle hotkey).
- `press` chord must match your mouse (Logitech paths use standard HID button bits on SPI).
- Group list must start at index `0` with no gaps (`dense prefix` rule).

### Movement too weak / strong

1. Set `eDPI` to your real DPI×sens.
2. Tune `modeScale` (v3) or weapon `scale` (v2).
3. Edit raw `y` values in `steps` if needed.

---

## Version history (summary)

| `v` | Added |
|-----|--------|
| **1** | `groups`, steps, press/loop, mouse/kbd payloads. |
| **2** | `scripts[]`, `activeScript`, `macrosOn`, `nextScript`, `toggleMacros`, weapon `scale`, `eDPI` / `patternEDPI`. |
| **3** | `weapons` / `modes` naming, `pressMode`, `modeScale`, `modeSet`, `activeWeapon` / `nextWeapon` aliases, multi-mode peer stop, exact chord matching. |

For behaviour that cannot be expressed in JSON (complex conditionals, scripting), extend the C firmware or propose a small v4 field rather than embedding a full language on the device.
