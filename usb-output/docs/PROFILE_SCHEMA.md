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
7. [HID keymaps (mouse & keyboard)](#hid-keymaps-mouse--keyboard)
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
| Weapon / script banks (v2/v3) | **No fixed limit**; only the **active** bank is loaded in RAM (`CONFIG_MACRO_MAX_WEAPONS` optional cap) |
| Profile file size | Menuconfig `CONFIG_MACRO_PROFILE_MAX_SIZE` (default **131072** / 128 KB) |

**Dense prefix rule:** Groups are stored in array order `list[0]`, `list[1]`, … Firmware stops at the **first entry with `size == 0`**. Do not leave empty gaps between used groups.

---

## Choosing a schema version

| Version | Best for |
|---------|----------|
| **v1** | Simple profiles: one list of macros, no weapon switching. |
| **v2** | Several weapons (any count), switch active weapon with a hotkey; one trigger per macro in the active bank. |
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

**Rejected fields** (load fails if present on any step): `mouseTo`, `interp`, `segments`, `humanize` (use profile-level `humanize` instead).

**Timing tip:** For full-auto recoil, set `us` ≈ `60_000_000 / RPM` (e.g. 600 RPM → `100000` µs per bullet).

### Profile `humanize` (optional, root object)

Tweaks **when** bullet steps fire, **how** late ticks recover, **noise** on script deltas, and **how smoothly** recoil movement is sent over USB. All fields are optional; omit the whole object to use firmware defaults (spread at **8 ms** / ~125 Hz, soft catch-up, no timing jitter).

**Requires profile load** (SPIFFS boot or `POST /api/profile`). Changing JSON on disk alone does not apply until upload/reboot.

| Field | Type | Default (if `humanize` present) | Applies to |
|-------|------|----------------------------------|------------|
| `timingPct` | number 0–25 | `0` (off) | **Bullet timer** — random ±% on each step’s `us` before the next tick. |
| `jitterUs` | `[min, max]` | none | **Bullet timer** — extra uniform random delay (µs) added to each step. |
| `mouse` | number 0–3 | `0` (off) | **Bullet tick** — ±N counts random noise on script `mouse.x` / `mouse.y` when the step fires. |
| `catchupMinMs` | number | **75** | **Late bullet timer** — minimum wait before the next tick if the device fell behind (avoids 1 ms catch-up bursts). Clamped ≥ 5 ms. |
| `dripMs` | number 0–50 | **8** | **USB mouse delivery** — milliseconds between HID slices while spreading a step’s movement. |
| `dripHz` | number | — | Same as `dripMs`, alternate units: `dripHz: 125` ≈ `dripMs: 8`. Ignored if `dripMs` is set. Clamped 1–2000 Hz. |

**Separation:** `timingPct` / `jitterUs` / `catchupMinMs` affect only the **bullet timer**. `dripMs` / `dripHz` affect only **USB movement** over the step’s nominal `us`. They do not share a delay value (avoids jitter cutting a spread window short).

#### `timingPct` (bullet cadence jitter)

Adds uniform random variation to each step delay **after** the step runs, when scheduling the next bullet.

- Example: `"timingPct": 5` on `"us": 133000` → next delay is `133000` ± up to **5%** (~±6650 µs).
- **0** or omitted: no percentage jitter (step uses scaled `us` only).
- Does **not** change how movement is split over USB; see `dripMs`.

#### `jitterUs` (extra delay per bullet)

Adds a second random delay on top of `us` (and on top of `timingPct`).

- Example: `"jitterUs": [800, 2500]` → each step waits an extra **800–2500 µs** (uniform).
- Useful to de-sync from perfectly periodic game/weapon timing.
- Omit or use `[0, 0]` for none.

#### `mouse` (pixel noise on script deltas)

When a **mouse** step fires, random noise is applied to the script delta **before** spread/immediate send.

- **0**: off (recommended for tuned recoil patterns).
- **1–3**: ±1..±3 counts on `x` and `y` independently (clamped to int8).
- Does **not** add noise to your physical aim (SPI passthrough).

#### `catchupMinMs` (late tick recovery)

If a bullet tick is **late** (CPU/USB busy), firmware schedules the next tick after at least this many milliseconds instead of catching up in **1 ms**.

- Default **75** when `catchupMinMs` is omitted **and** `timingPct` or `jitterUs` is set (not applied for `dripMs`-only blocks).
- Minimum **5** ms if you set a lower value.
- Without `humanize`: firmware uses ~**25%** of nominal `us`, at least **10 ms** (not 1 ms).

#### `dripMs` / `dripHz` (smooth USB recoil movement)

Bullet steps still fire at your pattern’s `us` (~450 RPM → ~133 ms). Each step’s `mouse` delta is **spread** across USB reports over that **nominal** `us` (time-linear lerp, same idea as Python `SmoothMove` / C++ `SmoothMoveMouse` over the bullet interval). Bullet **timing** may still jitter via `timingPct` / `jitterUs` independently.

| `dripMs` | Approx. USB rate | Typical use |
|----------|------------------|-------------|
| **8** (default) | ~125 Hz | Good balance; matches many gaming mice. |
| **4** | ~250 Hz | Smoother; slightly more CPU/USB traffic. |
| **1** | ~1000 Hz | Very smooth; use if the host polls at 1 kHz. |
| **0** | — | **Off** — full step delta in **one** report per bullet (legacy “robotic” feel). |

- Use **`dripMs`** OR **`dripHz`**, not both; if both are present, **`dripMs` wins**.
- `dripHz: 125` is equivalent to `dripMs: 8` (1 000 000 / 125 ≈ 8000 µs).
- Spread is **discarded** when you release the macro trigger (e.g. LMB), so recoil does not keep pulling.
- Your **aim** deltas on SPI are still sent; when a drip slice is due, it is **added** to the same report (int8 clamp per axis).

Without a `humanize` block, firmware still spreads at **8 ms** (`HID_MOUSE_DRIP_INTERVAL_US` in `config.h`).

```json
"humanize": {
  "timingPct": 5,
  "jitterUs": [800, 2500],
  "mouse": 0,
  "catchupMinMs": 75,
  "dripMs": 4
}
```

**Empty `steps`:** If omitted or `[]`, firmware inserts one neutral keyboard step (group still occupies a slot).

### Group / mode fields (all versions)

| Field | Type | Required | Default | Description |
|-------|------|----------|---------|-------------|
| `steps` | array | no | one neutral step | Timed HID events (see above). |
| `n` | number | no | all steps | Use only the first `n` steps (`1` ≤ `n` ≤ length of `steps`). |
| `loop` | bool | no | `false` | If `true`, restart from step 0 while the `press` chord remains held (`triggerMode: "hold"` only). |
| `triggerMode` | string | no | `"hold"` | `"hold"` = run while `press` is held (default). `"tap"` = one full sequence per press; release does not stop mid-run. |
| `press` | object | no | none | Start sequence on **rising edge** of this chord (see [Triggers](#triggers-and-matching)). |
| `release` | object | no | none | **Keyboard only:** start on release edge of chord. |
| `save` | object | no | none | **Keyboard only:** enter recording mode for this group (advanced). |
| `comment` | string | no | — | Ignored by firmware; documentation for humans. |
| `name` | string | no | — | Ignored by firmware; useful in v3 for your own reference. |
| `scale` | number | no | `1` | Per-group multiplier (v1/v2). On v3 modes, prefer **`modeScale`**. |

### Loop behaviour

1. **Start:** `press` chord appears (rising edge) → sequence resets and timer starts at step 0.
2. **Each tick:** Current step’s `mouse` / `kbd` is sent; timer schedules next step after `us`.
3. **Loop (hold):** After the last step, if `loop: true`, `triggerMode` is `"hold"` (default), and `press` still held → step 0 again.
4. **Stop (hold):** If `loop: true` and `press` no longer held → sequence resets and timer stops.

### Tap vs hold (`triggerMode`)

| `triggerMode` | Use case | Behaviour |
|---------------|----------|-----------|
| `"hold"` (default) | Full-auto recoil (AK, LR300 sustained) | Sequence stops when you release the `press` chord. `loop: true` repeats while held. |
| `"tap"` | Burst (LR300), semi-auto (SAR, pistol) | One press runs **all** steps once; releasing the button does **not** cancel mid-sequence. Each new press (rising edge) starts again. `loop` is ignored — always one pass per tap. |

Burst / semi-auto example:

```json
{
  "name": "burst",
  "triggerMode": "tap",
  "loop": false,
  "press": { "mouse": { "b": 1 } },
  "n": 3,
  "steps": [
    { "us": 120000, "mouse": { "x": 0, "y": 4 } },
    { "us": 120000, "mouse": { "x": 0, "y": 5 } },
    { "us": 120000, "mouse": { "x": -1, "y": 4 } }
  ]
}
```

### What gets sent on mouse macros

On each macro tick for mouse steps:

- **Buttons** come from your real mouse (passthrough on every SPI report).
- **Movement** (`x`, `y`, `wheel`, `pan`) from the script step is **spread across USB reports** until the next step (default **8 ms** ≈ 125 Hz; tune with `humanize.dripMs` or `humanize.dripHz`). Bullet timing (`us` between steps) is unchanged; only HID delivery is smoothed.
- **User aim** on SPI reports is unchanged; when a drip slice is due, it is **added** to the same report as your physical `x`/`y` (clamped to int8 per axis).

Pending spread is **discarded** when no mouse macro sequence is running (e.g. LMB release), so recoil does not continue after you stop firing.

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

### Game custom properties

`customprops` is an optional root object used for per-game tuning.

```json
"customprops": {
  "fov": 90,
  "ads-sens": 1.0
}
```

If `customprops` is missing (or invalid for the selected game), firmware keeps the default flow.

### Activation rules

`customprops` activates only when all are true:

1. `game` matches a supported game handler.
2. `customprops` exists and is an object.
3. Required fields for that game are present and valid.

Otherwise:

- no custom game multiplier is applied,
- runtime behavior falls back to normal eDPI/weapon/mode scaling,
- status field `customPropsActive` is `false`.

### Supported games

| Game | Support status | Effect when `customprops` valid |
|------|----------------|----------------------------------|
| `rust` | **Supported** | Adds Rust-specific custom multiplier before weapon/mode scaling. |
| `cs2` | Not implemented yet | Ignored for now (`customPropsActive=false`). |
| Any other value | Not implemented yet | Ignored for now (`customPropsActive=false`). |

### Rust customprops

Use when `game` is `"rust"`.

#### Fields

| Field | Type | Required | Notes |
|-------|------|----------|-------|
| `fov` | number | yes | Must be `> 0`. |
| `ads-sens` | number | yes | Must be `> 0`. Aliases accepted: `adsSens`, `ads_sens`. |

#### Impact

When both fields are valid, firmware applies:

```text
rustCustomScale = (1.0 * 90.0) / (adsSens * fov)
finalScale = edpiScale × rustCustomScale × weaponScale × modeScale
```

- Higher `ads-sens` => lower recoil multiplier.
- Higher `fov` => lower recoil multiplier.
- Lower `ads-sens` or lower `fov` => stronger recoil multiplier.

### CS2 customprops (placeholder)

`game: "cs2"` is reserved for future add-on support, but no CS2-specific `customprops` fields are consumed yet.

- Current behavior: `customprops` is ignored for CS2.
- Keep game-specific values only as metadata until CS2 handler is added.

---

## HID keymaps (mouse & keyboard)

Profiles use numeric **USB HID** values (same as TinyUSB `MOUSE_BUTTON_*` / `HID_KEY_*`). There are no string key names in JSON — use the decimal codes below.

```json
"press": { "mouse": { "b": 3 } },
"press": { "kbd": { "m": 2, "k": [4, 5] } }
```

### Step payload fields

**Mouse object** (`mouse`) — used in `steps[]`, `press`, `release`, hotkeys:

| Field | Type | Range | Description |
|-------|------|-------|-------------|
| `b` | number | 0–31 | Button bitmask (triggers / hotkeys). Usually **omit** on movement-only steps. |
| `x` | number | −128…127 | Relative X (int8 after clamp). |
| `y` | number | −128…127 | Relative Y — positive usually = cursor **down**. |
| `w` | number | −128…127 | Vertical wheel. |
| `p` | number | −128…127 | Horizontal wheel / pan. |

**Keyboard object** (`kbd`):

| Field | Type | Description |
|-------|------|-------------|
| `m` | number | Modifier byte bitmask (see table below). |
| `k` | array | Up to **6** key **usage IDs**; **every** listed key must be down (AND chord). |

---

### Mouse button map (`b`)

Each button is one **bit**. The value in JSON is the **sum** (bitwise OR) of every button that must be down.

| Bit | Hex | Decimal | Name | TinyUSB constant |
|-----|-----|---------|------|------------------|
| 0 | `0x01` | **1** | Left (LMB) | `MOUSE_BUTTON_LEFT` |
| 1 | `0x02` | **2** | Right (RMB) | `MOUSE_BUTTON_RIGHT` |
| 2 | `0x04` | **4** | Middle / wheel click (MMB) | `MOUSE_BUTTON_MIDDLE` |
| 3 | `0x08` | **8** | Side back | `MOUSE_BUTTON_BACKWARD` |
| 4 | `0x10` | **16** | Side forward | `MOUSE_BUTTON_FORWARD` |
| 5–7 | `0x20`…`0x80` | 32, 64, 128 | Rare / device-specific | — |

Firmware masks with the low **5 bits** (`b & 0x1F`) on the SPI path — enough for standard gaming mice.

**Formula:** `b = button₁ | button₂ | …` (add decimals).

#### All combinations (5 standard buttons)

| Buttons held | Decimal `b` | Hex |
|--------------|---------------|-----|
| LMB | **1** | `0x01` |
| RMB | **2** | `0x02` |
| LMB + RMB | **3** | `0x03` |
| MMB | **4** | `0x04` |
| LMB + MMB | **5** | `0x05` |
| RMB + MMB | **6** | `0x06` |
| LMB + RMB + MMB | **7** | `0x07` |
| Back | **8** | `0x08` |
| LMB + Back | **9** | `0x09` |
| RMB + Back | **10** | `0x0A` |
| LMB + RMB + Back | **11** | `0x0B` |
| MMB + Back | **12** | `0x0C` |
| LMB + MMB + Back | **13** | `0x0D` |
| RMB + MMB + Back | **14** | `0x0E` |
| LMB + RMB + MMB + Back | **15** | `0x0F` |
| Forward | **16** | `0x10` |
| LMB + Forward | **17** | `0x11` |
| RMB + Forward | **18** | `0x12` |
| LMB + RMB + Forward | **19** | `0x13` |
| MMB + Forward | **20** | `0x14` |
| LMB + MMB + Forward | **21** | `0x15` |
| RMB + MMB + Forward | **22** | `0x16` |
| LMB + RMB + MMB + Forward | **23** | `0x17` |
| Back + Forward | **24** | `0x18` |
| LMB + Back + Forward | **25** | `0x19` |
| RMB + Back + Forward | **26** | `0x1A` |
| LMB + RMB + Back + Forward | **27** | `0x1B` |
| MMB + Back + Forward | **28** | `0x1C` |
| LMB + MMB + Back + Forward | **29** | `0x1D` |
| RMB + MMB + Back + Forward | **30** | `0x1E` |
| LMB + RMB + MMB + Back + Forward | **31** | `0x1F` |

#### Game / profile examples (mouse)

| Use case | JSON `press` / hotkey |
|----------|----------------------|
| Hipfire (LMB only, v3 exact) | `"pressMode": "exact"`, `"mouse": { "b": 1 }` |
| ADS (LMB + RMB) | `"mouse": { "b": 3 }` |
| RMB-only macro | `"mouse": { "b": 2 }` |
| Next weapon (side button) | `"nextWeapon": { "mouse": { "b": 4 } }` or `{ "b": 8 }` — **match your mouse** |
| Toggle macros | `"toggleMacros": { "mouse": { "b": 16 } }` (forward) |

DPI/profile buttons on some mice are **not** in this bitmask; they may appear as keyboard/consumer HID elsewhere.

---

### Keyboard map (`kbd`)

JSON maps directly to the USB HID **boot keyboard** report the firmware forwards:

| JSON | HID report field | Meaning |
|------|------------------|---------|
| `m` | `modifier` byte | Left/right Ctrl, Shift, Alt, Gui bits |
| `k[]` | `keycode[0..5]` | Up to 6 simultaneous **key usage IDs** (not ASCII) |

**Matching rules (triggers):**

- Every value in `k` must appear in the key slots (order does not matter).
- If `m` is non-zero, those modifier bits must be **down**; extra modifiers are allowed.
- Firmware also treats boot usages **`224`–`231`** (`0xE0`–`0xE7`) in a key slot as modifiers (e.g. Left Shift = usage **`225`** / `0xE1`).

**Recommended:** use `m` for Shift/Ctrl/Alt; use `k` only for normal keys.

#### Modifier bitmask (`m`)

| Bit | Decimal `m` | Hex | Key |
|-----|-------------|-----|-----|
| 0 | **1** | `0x01` | Left Ctrl |
| 1 | **2** | `0x02` | **Left Shift** (Rust crouch, default layout) |
| 2 | **4** | `0x04` | Left Alt |
| 3 | **8** | `0x08` | Left Gui (Windows / Meta) |
| 4 | **16** | `0x10` | Right Ctrl |
| 5 | **32** | `0x20` | Right Shift |
| 6 | **64** | `0x40` | Right Alt |
| 7 | **128** | `0x80` | Right Gui |

**Combined modifiers:** add values (Ctrl+Shift → `m: 3`).

| Chord | `m` |
|-------|-----|
| Left Shift only | **2** |
| Left Ctrl only | **1** |
| Left Ctrl + Left Shift | **3** |
| Left Alt only | **4** |

#### Key usage IDs (`k` array)

Each number in `k` is a **USB HID keyboard usage ID** (usage page `0x07`), same as TinyUSB `HID_KEY_*` in `class/hid/hid.h`.

**Letters and digits**

| Key | Dec | Key | Dec | Key | Dec |
|-----|-----|-----|-----|-----|-----|
| A | **4** | H | **11** | O | **18** |
| B | **5** | I | **12** | P | **19** |
| C | **6** | J | **13** | Q | **20** |
| D | **7** | K | **14** | R | **21** |
| E | **8** | L | **15** | S | **22** |
| F | **9** | M | **16** | T | **23** |
| G | **10** | N | **17** | U | **24** |
| | | | | V | **25** |
| | | | | W | **26** |
| | | | | X | **27** |
| | | | | Y | **28** |
| | | | | Z | **29** |
| 1 | **30** | 4 | **33** | 7 | **36** |
| 2 | **31** | 5 | **34** | 8 | **37** |
| 3 | **32** | 6 | **35** | 9 | **38** |
| | | | | 0 | **39** |

**Editing / navigation**

| Key | Dec | Key | Dec |
|-----|-----|-----|-----|
| Enter | **40** | Insert | **73** |
| Escape | **41** | Home | **74** |
| Backspace | **42** | Page Up | **75** |
| Tab | **43** | Delete | **76** |
| Space | **44** | End | **77** |
| | | Page Down | **78** |
| | | Arrow Right | **79** |
| | | Arrow Left | **80** |
| | | Arrow Down | **81** |
| | | Arrow Up | **82** |

**Function keys:** F1 **58** … F12 **69** (each +1).

**Keypad:** Num Lock **83**, `/` **84**, `*` **85**, `-` **86**, `+` **87**, Enter **88**, digits **89**–**98**, `.` **99**.

**Punctuation (US layout):** `-` **45**, `=` **46**, `[` **47**, `]` **48**, `\` **49**, `;` **51**, `'` **52**, `` ` `` **53**, `,` **54**, `.` **55**, `/` **56**, Caps Lock **57**.

#### Keyboard chord examples

| Intent | JSON |
|--------|------|
| Hold **A** | `"kbd": { "k": [4] }` |
| **W** + **D** | `"kbd": { "k": [26, 7] }` |
| **Left Shift** only | `"kbd": { "m": 2 }` |
| **Ctrl + C** | `"kbd": { "m": 1, "k": [6] }` |
| **F1** in a step | `{ "us": 50000, "kbd": { "k": [58] } }` |
| Release on **X** | `"release": { "kbd": { "k": [27] } }` |

**v3 crouch + fire:** `"press": { "mouse": { "b": 1 }, "kbd": { "m": 2 } }` with `"pressMode": "exact"`.

**Keyboard on SPI:** `kbd` triggers need keyboard reports from **usb-input** over SPI.

#### Finding codes for other keys

1. [USB HID usage tables](https://www.usb.org/sites/default/files/hut1_12.pdf) — keyboard page `0x07`; usage ID = JSON number.
2. TinyUSB `HID_KEY_*` in `hid.h` — constant value = JSON number.
3. UART `DEBUG_LOG` — log reports while pressing keys.

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

**Keyboard path:** Combined `press` needs **keyboard reports on SPI** from **usb-input**. Firmware accepts `m:2` (Left Shift) **or** usage **225** (`0xE1`) in a key slot.

The macro starts on the **rising edge** of the full condition (e.g. Shift pressed while LMB already down, or LMB pressed while Shift already down). Releasing **either** mouse buttons or the keyboard requirement stops the mode.

---

## Schema v1

Single flat list of macro groups. All groups are **active** at once (up to 10).

### Root object

| Field | Type | Required | Description |
|-------|------|----------|-------------|
| `v` | number | **yes** | Must be `1`. |
| `name` | string | no | Shown as `profile` in status. |
| `game` | string | no | Optional game label (`"rust"`, `"cs2"`, etc.), shown as `game` in status/UI. |
| `customprops` | object | no | Optional per-game custom tuning (see [Game custom properties](#game-custom-properties)). |
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

Supports multiple script banks (weapons), **one active bank** at a time, and optional **hotkeys**.

### Root object

| Field | Type | Required | Default | Description |
|-------|------|----------|---------|-------------|
| `v` | number | **yes** | — | Must be `2`. |
| `name` | string | no | `"flash"` | Profile display name. |
| `game` | string | no | `""` | Optional game label for grouping. |
| `customprops` | object | no | — | Optional per-game custom tuning (see [Game custom properties](#game-custom-properties)). |
| `scripts` | array | yes* | — | Weapon banks (no fixed max). *Required unless root `groups` is used. |
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
| Only one script bank active | Same (any number of weapons), but each weapon has **many modes** loaded at once |
| LMB macro also runs during LMB+RMB (`chord` match) | `pressMode: "exact"` on hipfire (`b: 1`) |
| Switching ADS / hipfire | Different `press` chords + `modeSet` stops the other mode |

### Root object

All v2 root fields, plus:

| Field | Type | Description |
|-------|------|-------------|
| `v` | number | Must be `3`. |
| `weapons` | array | Preferred name for script banks (no fixed max). |
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
| `triggerMode` | string | `"hold"` | `"hold"` or `"tap"` (see [Tap vs hold](#tap-vs-hold-triggermode)). |
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
| `game` | Profile `game` (empty when unset). |
| `customPropsActive` | `true` only when game-specific `customprops` are parsed and applied. |
| `fw` | ESP-IDF version string. |
| `macrosOn` | Global macro output enabled. |
| `eDPI` | Parsed user eDPI (`0` if unset). |
| `patternEDPI` | Parsed reference eDPI. |
| `edpiScale` | `eDPI / patternEDPI` (or `1`). |

### v2 / v3 script / weapon fields

| Field | Description |
|-------|-------------|
| `scriptCount` | Number of banks. |
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
  "game": "rust",
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
