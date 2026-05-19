# USB device identity

The **usb-output** firmware presents itself as a generic USB HID keyboard+mouse device. Defaults are chosen from **public USB ID databases**, not Espressif/TinyUSB and not retail brands (Logitech, Razer, etc.).

## Default identity (after flash)

| Field | Value | Source |
|-------|--------|--------|
| **VID** | `04D9` | [Holtek Semiconductor, Inc.](https://usb-ids.gowdy.us/read/UD/04d9) (USB-IF assigned vendor) |
| **PID** | `1400` | Documented as **PS/2 keyboard + mouse controller** |
| **Manufacturer** | `Holtek Semiconductor, Inc.` | usb.ids vendor name |
| **Product** | `PS/2 keyboard + mouse controller` | [linux-hardware.org](https://linux-hardware.org/?id=usb%3A04d9-1400), DeviceHunt, pci-db |
| **Serial** | 12 hex digits from chip MAC | Firmware-generated (per board) |

**Why this pair:** `04D9:1400` is a real, widely listed **combined keyboard + mouse** controller used on many OEM systems (desktop boards, laptops, etc.). It matches this project (one HID interface, keyboard + mouse reports) better than a mouse-only or keyboard-only ID.

Windows often still labels the node **USB Input Device** under standard `hidusb.sys` — that is normal.

## What NOT to use

| VID:PID | Why avoid |
|---------|-----------|
| `303A:****` | Espressif — obvious dev board |
| `046D:****` | Logitech — branded; driver/fingerprint mismatch risk |
| `1532:****` / `258A:****` etc. | Razer / gaming OEM — same problem |
| Random PID on `04D9` | Stick to **`1400`** for combo controller semantics, or pick another **documented** Holtek PID from [usb.ids](https://usb-ids.gowdy.us/read/UD/04d9) |

## Customize

```text
usb-output/main/config_custom.h.example  →  usb-output/main/config_custom.h
```

Edit `USB_DESC_*` in `config_custom.h`, rebuild, flash.

### Other generic vendors (research only)

These are also common on cheap peripherals (verify PID in usb.ids before use):

| VID | Vendor | Typical use |
|-----|--------|-------------|
| `04D9` | Holtek | Keyboards, mice, **combo controllers** (default here) |
| `1A2C` | China Resource Semico | Budget keyboards (e.g. PID `0021`, `0024`) |
| `0C45` | Microdia / Sonix | Keyboards, 2.4G receivers (many PIDs) |

Use a PID that matches your device type (combo vs keyboard-only vs mouse-only).

### Open-source–only alternative

If you prefer not to use a silicon-vendor ID: [pid.codes](https://pid.codes/) **VID `1209`** + your own PID (see commented preset in `config_custom.h.example`). That is legitimate for open hardware but **less** like a mass-market generic mouse.

### ESP32-S3 “native” mice?

**There are no mass-market gaming or office mice whose sole MCU is an ESP32-S3 with a single, copyable USB identity.**

What exists instead:

| Source | Typical VID:PID | Notes |
|--------|-----------------|--------|
| Espressif IDF / TinyUSB examples | `303A:4004` | **Worse** for your goal — flags as Espressif immediately |
| [ArtiomSu BtKBi](https://github.com/ArtiomSu/bluetooth-to-keyboard-input-esp32-s3-zero) (ESP32-S3 HID) | `303A:1001` default | Still **Espressif VID**; VID/PID are user-configurable over BLE, not a fixed “mouse product” |
| DIY articles (e.g. ESP32 + joystick as mouse) | Often `303A:****` | One-off builds, not a product ID |
| [Ploopy](https://ploopy.co/mouse/) open mouse | `1209:3A00` | Legitimate [pid.codes](https://pid.codes/) ID, but MCU is **ATmega32u4 / RP2040**, not ESP32-S3; also **mouse-only**, not keyboard+mouse combo |

Retail mice use Holtek, PixArt, Sonix, Nuvoton, ARM MCUs, etc. — not ESP32-S3. Cloning a “famous ESP32-S3 mouse” VID/PID is therefore not an option.

**Recommendation:** keep the default **`04D9:1400`** (generic OEM combo controller that matches this firmware’s HID layout). Do **not** switch defaults to `303A:****` just because the firmware runs on ESP32-S3 — that makes the device *more* obvious, not less.

If you want an identity tied to **your** open ESP32 project (not Holtek), register a PID at [pid.codes](https://pid.codes/) and use the commented `1209` preset in `config_custom.h.example`.

## Limits (important)

- Descriptor identity **does not** make anti-cheat trust the device.
- Using Holtek’s VID/PID on non-Holtek silicon is what many generic clones do; it is **not** the same as impersonating Logitech, but it is still not a USB-IF certificate for your board.
- UART log tag can still say `MacroPassthrough` (`LOG_TITLE` in `config.h`) — not visible over USB.
- Wi-Fi SoftAP default SSID: `USB-Config` (menuconfig).

## Verify on Windows

Device Manager → device → **Details** → **Hardware Ids**:

```text
USB\VID_04D9&PID_1400
```

PowerShell:

```powershell
Get-PnpDevice -PresentOnly | Where-Object { $_.InstanceId -match 'VID_04D9&PID_1400' }
```

Properties → **Device instance path** must **not** contain `VID_303A`.

## Files

| File | Role |
|------|------|
| `usb_desc_config.h` | Default VID/PID/strings |
| `config_custom.h.example` | Local overrides template |
| `usb_identity.c` | Device descriptor + MAC serial |
