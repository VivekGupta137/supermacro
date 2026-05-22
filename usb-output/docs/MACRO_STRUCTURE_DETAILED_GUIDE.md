# Macro Structure Detailed Guide

This guide explains how macro profiles are interpreted by firmware, how timed sequences run, and how to model common gameplay/automation scenarios.

For field-by-field schema reference, see `docs/PROFILE_SCHEMA.md`.

**Ready-made JSON:** see **Usage examples** in `docs/PROFILE_SCHEMA.md` and the files under `profiles/` (`mouse_only_example.json`, `example_v2_multiscript.json`, `example_interp_additive.json`).

---

## 1) Mental Model

A profile is a JSON document that defines one or more **script banks**.  
Each script bank contains **groups**.  
Each group is a **timed sequence** of HID steps (mouse or keyboard events).

At runtime:

1. Input passthrough runs continuously.
2. Macro engine checks each group trigger.
3. Matching groups can start (or restart) their timed sequence.
4. Sequence steps emit synthetic HID reports over time.

---

## 2) Top-Level Profile Modes

## v1 (single script)

- One `groups` array at root.
- Good for simple setups where you do not need weapon/gun profiles.

## v2 (multi-script)

- `scripts[]` array with multiple named banks.
- `activeScript` selects which bank is live.
- Optional hotkeys:
  - `nextScript` (cycle bank)
  - `toggleMacros` (global on/off)

If `scripts[]` is omitted in v2, root `groups` acts as one bank.

---

## 3) Core Building Blocks

## 3.1 Groups

Each group describes:

- **Trigger**: `press`, `release`, and/or `save`
- **Behavior**:
  - `steps[]`: timed outputs
  - `n`: step count clamp
  - `loop`: repeat while trigger remains held

Think of each group as:

> “When this trigger edge happens, play this step timeline.”

## 3.2 Steps

Each step has:

- `us`: duration in microseconds
- One payload:
  - `mouse`: `{ b, x, y, w, p }`
  - `kbd`: `{ m, k }`

The engine holds that synthetic HID state for `us`, then advances.

## 3.3 Triggers

- `press`: rising edge (newly true now, false previous frame)
- `release`: falling edge for keyboard chord
- `save`: mode-oriented trigger for recording workflow (if used)

Rising-edge behavior prevents repeated retrigger while the button/key is still held.

---

## 4) Runtime Rules That Matter

## 4.1 Dense Prefix Rule

The firmware processes groups until the first empty slot (`size == 0`), then stops.

Implication:

- Put all active groups first.
- Do not leave gaps between used groups.

## 4.2 Chord Matching

## Mouse (`b`)

- `b` is a bitmask.
- A trigger matches when all bits in mask are currently set.
- Examples:
  - left: `1`
  - right: `2`
  - middle: `4`
  - right+middle: `6`

## Keyboard (`m` + `k`)

- `m`: modifier bitmask
- `k`: up to six keycodes that must all be present

## 4.3 Macro On/Off

`macrosOn` controls whether macro output is emitted.

- When off: passthrough still works.
- When on: matching groups can produce synthetic sequence output.

## 4.4 Active Script (v2)

Only groups from active script bank are evaluated.

- Switching script hot-applies that bank immediately.
- UI status reflects `activeScript`, `activeScriptName`, and `activeGroupCount`.

---

## 5) Design Patterns

## Pattern A: Recoil pull while RMB held

Use a mouse trigger chord and a looping step sequence.

Why:

- Holding ADS fire path can continuously apply a small downward movement.

Sketch:

```json
{
  "press": { "mouse": { "b": 2 } },
  "loop": true,
  "steps": [
    { "us": 16000, "mouse": { "x": 0, "y": 1, "b": 0 } },
    { "us": 16000, "mouse": { "x": 0, "y": 1, "b": 0 } }
  ]
}
```

## Pattern B: Burst fire tap sequence

Use `"triggerMode": "tap"` with `"loop": false` and a finite `steps` list (optionally trim with `"n"`).

Why:

- One click runs the full recoil burst (LR300) or one semi-auto correction (SAR/pistol); release does not cancel mid-sequence.
- Deterministic finite output each trigger.

## Pattern C: Two weapon families (v2 scripts)

- Script 0: rifle recoil pattern
- Script 1: SMG recoil pattern
- `nextScript` on a convenient button (e.g. middle click)

Why:

- Keep logic isolated per weapon without reflashing.

## Pattern D: Panic disable

Assign `toggleMacros` to a deliberate chord (example: right+middle).

Why:

- Immediate macro output kill switch while preserving normal input passthrough.

---

## 6) Scenario Templates

## Scenario 1: Single-weapon minimal setup

Use `v=1`:

- 1–3 groups
- clear `press` triggers
- no script switching complexity

Good for first validation and tuning.

## Scenario 2: Multi-weapon profile

Use `v=2` with `scripts[]`:

- One script per weapon class
- Shared hotkeys (`nextScript`, `toggleMacros`)
- Use readable script names for UI confirmation

Good for games with multiple recoil styles.

## Scenario 3: Hybrid mouse + keyboard triggers

Example:

- Mouse chord starts recoil
- Keyboard key triggers utility macro (e.g. crouch/peek timing)

Good when you want role-based groups in one script.

## Scenario 4: Safety-first tournament profile

- Conservative trigger chords
- Keep `toggleMacros` accessible
- Avoid ambiguous overlap between triggers

Good for minimizing accidental activation.

---

## 7) Tuning Strategy

1. Start with one group and two steps.
2. Validate trigger edge behavior (single activation on press).
3. Tune `us` first, then `x/y` deltas.
4. Add loops only after finite sequence is stable.
5. Add second script bank only when first is reliable.
6. Keep comments in JSON for intent documentation.

---

## 8) Common Mistakes

- Gaps in groups array (breaks dense prefix processing).
- Trigger chord too broad (fires unexpectedly).
- Very short `us` values causing unstable feel.
- Forgetting `macrosOn` is false after toggle.
- Mismatched script index/name expectations in UI.

---

## 9) Practical Validation Checklist

- `GET /api/status` updates after every action.
- Web UI active script text matches expected bank.
- `activeGroupCount` matches non-empty groups in active bank.
- `nextScript` cycles correctly and wraps.
- `toggleMacros` flips on/off and blocks sequence output when off.
- Profile reboots cleanly and reloads from SPIFFS.

---

## 10) Example Skeletons

## v1 skeleton

```json
{
  "v": 1,
  "name": "single-weapon",
  "groups": [
    {
      "comment": "RMB hold recoil control",
      "press": { "mouse": { "b": 2 } },
      "loop": true,
      "steps": [
        { "us": 16000, "mouse": { "x": 0, "y": 1 } },
        { "us": 16000, "mouse": { "x": 0, "y": 1 } }
      ]
    }
  ]
}
```

## v2 skeleton

```json
{
  "v": 2,
  "name": "multi-weapon",
  "macrosOn": true,
  "activeScript": 0,
  "nextScript": { "mouse": { "b": 4 } },
  "toggleMacros": { "mouse": { "b": 6 } },
  "scripts": [
    {
      "name": "rifle",
      "groups": [
        {
          "press": { "mouse": { "b": 2 } },
          "loop": true,
          "steps": [
            { "us": 15000, "mouse": { "x": 0, "y": 1 } }
          ]
        }
      ]
    },
    {
      "name": "smg",
      "groups": [
        {
          "press": { "mouse": { "b": 2 } },
          "loop": true,
          "steps": [
            { "us": 12000, "mouse": { "x": 0, "y": 1 } }
          ]
        }
      ]
    }
  ]
}
```

---

## 11) Choosing v1 vs v2

Use v1 when:

- One behavior set is enough.
- You want minimal complexity.

Use v2 when:

- You need multiple weapon/script banks.
- You need runtime script cycling.
- You need dedicated global macro toggle workflow.

