# Macro Web UI Quick Walkthrough

This is the fastest path to get a working profile on the device.

## 1) Connect to the device

- If built in SoftAP mode, join the configured SSID and open `http://192.168.4.1/`
- If built in STA mode, open the printed IP from serial logs

## 2) Check live status

- Open the home page `/`
- Confirm:
  - Active script name/index
  - Macro state (`on` / `off`)
- The page uses polling and WebSocket push (when enabled)

## 3) Load existing saved profile

- Click **Fetch existing script**
- The saved JSON profile is loaded into the textarea

## 4) Switch scripts and macro state

- Click **Next gun script** to cycle active script bank
- Click **Toggle macros** to enable/disable macro output

## 5) Upload a new profile

- Paste valid JSON into the textarea
- Click **Upload profile**
- Device applies immediately and stores to SPIFFS

## 6) Schema references

- Profile schema details: `docs/PROFILE_SCHEMA.md`
- Example profiles:
  - `profiles/mouse_only_example.json`
  - `profiles/example_v2_multiscript.json`

## 7) All docs

- `docs/README.md` (index of all docs)
- `docs/QUICK_WALKTHROUGH.md`
- `docs/PROFILE_SCHEMA.md`
- `docs/MACRO_STRUCTURE_DETAILED_GUIDE.md`
- `docs/MACRO_WEB_UI_PLAN.md`

## Endpoints

- `GET /api/status` -> live status JSON
- `POST /api/profile` -> upload/apply/save profile
- `GET /api/profile` -> fetch saved profile JSON
- `POST /api/next-script` -> cycle active script
- `POST /api/toggle-macros` -> toggle macro output
- `GET /api/documentation.md` -> raw walkthrough markdown
- `GET /documentation` -> rendered walkthrough page
