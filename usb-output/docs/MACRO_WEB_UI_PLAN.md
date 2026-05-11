# Macro system: web UI, profiles, scripting, and humanization

**Scope:** `usb-output` firmware only (ESP32-S3, TinyUSB to PC, SPI from `usb-input`).

**Goals:**

- Multiple **profiles** stored on device; switch active profile from the browser.
- **Replaceable** macro configuration/scripts via web UI (upload → validate → persist → reload).
- **Additive mouse:** while a macro moves the cursor, **real user motion from SPI still adds** to macro deltas in the same reporting window (macro must not silently replace human input).
- **Humanization:** configurable timing jitter, non-linear mouse paths, bounded randomness.
- **Phased delivery:** simple web UI first; declarative config; then a real scripting runtime with sandboxing.

---

## Phase 0 — Constraints and current codebase

- Macros today are largely **compile-time** (`macro_defaults.c` / `macro_sequence_default`, included from `config.h` when `!CUSTOM_CONFIG`) with timer-driven sequences in `macpass_macro.c`.
- **`macro_profile_init()`** runs before **`macro_init()`** and fills **`group_sequence`** from SPIFFS JSON when **`CONFIG_MACRO_WEB_UI`** is enabled and `profile.json` is valid; otherwise it copies the compile-time default.
- HID to the PC is sent through `macpass_hid.c` (queue + `tud_hid_*`).
- SPI HID ingress is in `macpass_spi.c` → `hid_add_report` / macro hooks.
- **Additive movement** requires a single **composer** path (see Phase 4); macro code should not call TinyUSB directly in the long term.

---

## Phase 1 — Minimal web UI (no scripting)

**Status (implemented):** With **`CONFIG_MACRO_WEB_UI=y`** (menuconfig: *Macro profile / web UI*), firmware uses **`partitions.csv`** (SPIFFS `storage`), mounts at **`CONFIG_MACRO_SPIFFS_MOUNT`** (default `/spiffs`), starts **SoftAP** (`CONFIG_MACRO_WIFI_AP_*`), and serves:

- `GET /` — minimal HTML + status fetch + JSON upload form  
- `GET /api/status` — `fw` (app version), `profile` (name from JSON or `built-in`)  
- `POST /api/profile` — body = full profile JSON; validated, written to **`CONFIG_MACRO_PROFILE_JSON`**, then **`esp_restart()`**  

Threat model: open SoftAP with WPA2 password from Kconfig; no HTTPS (same as plan).

**Deliverable:** Device exposes a small HTTP server; browser can open a simple page (SoftAP or STA Wi‑Fi).

**Features:**

- Show firmware version, active profile name.
- Upload a file (initially **JSON profile**, Phase 2 schema).
- “Apply” writes to flash filesystem and triggers reload or controlled reboot.

**ESP-IDF building blocks:**

- `esp_http_server` for `/` and `/api/*`
- Static assets (HTML/CSS/JS) embedded or in a flash partition
- **LittleFS** (or SPIFFS) partition for `profiles/` and config
- **NVS** for Wi‑Fi mode, credentials, last active profile id

**Security (v1):**

- Document threat model: open AP with WPA2 password or isolated LAN; no HTTPS in first iteration.

---

## Phase 2 — Declarative profile model + loader

**Status (partial):** **v1 JSON** is parsed once at boot with **cJSON** (no runtime re-parse on HID hot path). Schema mirrors internal groups/steps (see **`profiles/example_profile.json`** and **`tools/macro_profile_emit.py`**). Invalid JSON or bad structure falls back to **compile-time default** (device stays usable). **Not yet implemented:** A/B commit, `GET /api/profiles`, multi-profile NVS pointer, structured validation errors in HTTP response.

**Performance notes:** Profile load is **O(file size)** at boot only; RAM spike is bounded by **`CONFIG_MACRO_PROFILE_MAX_SIZE`**. Upload handler validates into a **heap-allocated** `group_sequence_t` once, then writes the raw body to SPIFFS.

**Layout constraint:** Macro evaluation in `macpass_macro.c` stops at the **first sequence with `size == 0`**, so JSON **`groups`** must map to a **dense prefix** of non-empty sequences (no gaps).

**Deliverable:** Macros described as **data** (JSON), loaded at runtime; invalid files rejected without bricking the last good profile.

**Example schema (illustrative):**

```json
{
  "version": 1,
  "name": "comp",
  "humanize": {
    "timingJitterMs": [2, 12],
    "mouseCurve": true
  },
  "bindings": [
    { "on": { "mouseButtonsMask": 16 }, "run": "sequenceA" }
  ],
  "sequences": {
    "sequenceA": [
      { "type": "delay", "ms": 50 },
      { "type": "keyTap", "key": "A", "holdMs": 20 },
      { "type": "mouseMove", "dx": 10, "dy": -3, "steps": 5 }
    ]
  }
}
```

**Firmware:**

- Parser (`cJSON` or hand-rolled) → internal representation (similar to today’s `group_sequence_t` / timer lists).
- **Limits:** max steps, max duration, max file size, max simultaneous sequences.
- **A/B storage:** write new profile to temp, validate, then commit; keep previous copy.

**API (examples):**

- `GET /api/profile/active`
- `GET /api/profiles`
- `POST /api/profile/upload?name=...`
- `POST /api/profile/activate?name=...`
- `POST /api/reload`

---

## Phase 3 — Scripting language (embedded)

**User expectation:** “macros like people use on PC” often implies **loops, variables, conditions**. On ESP, full desktop runtimes are too heavy.

**Realistic options:**

| Approach | Notes |
|----------|--------|
| **Lua** | Common embedded choice; bind a small API; sandbox via hook + instruction/time limits |
| **Restricted DSL + expressions** | Smallest, safest; enough for many users; not full Turing completeness without extensions |
| **Tiny JS** (e.g. QuickJS/mJS) | Familiar syntax; larger flash/RAM |

**Recommendation:** Start with **declarative JSON (Phase 2)**; add **Lua** only when JSON proves insufficient, with strict sandbox:

- No raw filesystem/network from scripts
- Exposed API only: `tap_key`, `hold`, `move_mouse`, `sleep`, `log`, `get_button_mask`, etc.
- Watchdog / step budget per invocation

---

## Phase 4 — Additive mouse motion

**Requirement:** If the user moves the physical mouse **while** a macro emits movement, the PC sees **user delta + macro delta** (within HID `int8` clamping policy).

**Model:**

- Split contributions each tick/frame:
  - `user`: last human report from SPI (normalized to `hid_mouse_report_t`)
  - `macro`: pending macro delta / partial report
- **Composer** outputs one report:

  - `dx = clamp(user.dx + macro.dx)`, `dy = clamp(user.dy + macro.dy)`
  - **Buttons:** explicit policy per profile (e.g. OR with mask, or macro may only add certain bits)
  - **Wheel / pan:** additive with clamp; document pan policy (e.g. force 0 if undesired)

**Implementation direction:**

- Centralize send path: macro engine publishes **macro layer state**; `macpass_hid.c` (or new `hid_compose.c`) merges **user queue + macro state** immediately before `tud_hid_mouse_report`.

---

## Phase 5 — Humanization

**Tunables per profile:**

- **Delay jitter:** uniform or log-normal; min/max ms; optional “anti-periodic” guard
- **Mouse paths:** break large moves into steps; small perpendicular noise; cap total deviation
- **Presets:** `tight` vs `relaxed` jitter curves
- **Debug:** optional fixed RNG seed for reproducible captures

---

## Phase 6 — Web UI maturity

**v1:** Single HTML form + fetch to API.

**v2:** Lightweight SPA; optional mDNS (`macro.local`).

**v3:** In-browser editor (e.g. CodeMirror) for JSON/Lua; server-side **validation** endpoint returns structured errors before flash commit.

---

## Phase 7 — Hardening

- Watchdog around script / sequence runner
- Rate limit uploads; max body size
- Corruption-safe profile storage (checksum, dual copy)
- Metrics: dropped reports, merge clamp hits, script faults (optional UART log)

---

## Suggested implementation order

1. Partition table + LittleFS + minimal HTTP server + static “hello” page  
2. JSON profile upload + NVS active pointer + loader replacing static macros for one profile  
3. **HID composer** for additive mouse + keyboard policy  
4. Multi-profile list + activate + reload  
5. Humanization on delays and mouse steps  
6. (Optional) Lua sandbox + API bindings  
7. Rich editor + validation + safer OTA-style profile replace  

---

## Open decisions (product)

1. **Wi‑Fi mode:** SoftAP-only (simplest onboarding) vs STA (join home network).  
2. **Scripting:** Lua vs DSL-only for first “Turing-complete” release.  

---

## Reference: Razer DeathAdder V3 button bitmask (host `b0 & 0x1F`)

Documented from user testing on `usb-input` wire format (low 5 bits):

| Button | Hex |
|--------|-----|
| Left | `0x01` |
| Right | `0x02` |
| Middle (wheel click) | `0x04` |
| Side back | `0x08` |
| Side forward | `0x10` |

DPI / mode buttons may arrive on **other HID interfaces** (keyboard/consumer), not necessarily this bitmask.

---

## File layout (proposal, under `usb-output`)

```
usb-output/
  main/
    macro_web/          # HTTP server, API handlers (new)
    macro_profile/      # JSON/Lua load, validate, persist (new)
    hid_compose.c       # user + macro merge (new or split from macpass_hid.c)
    ...
  docs/
    MACRO_WEB_UI_PLAN.md
```

Partition table: add `storage` for LittleFS; increase factory app if needed for assets + Lua.
