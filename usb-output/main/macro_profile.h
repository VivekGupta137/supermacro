#pragma once

#include "sdkconfig.h"
#include "class/hid/hid_device.h"
#include "macpass_macro.h"

void macro_profile_init(const group_sequence_t *fallback);

const char *macro_profile_get_name(void);

bool macro_profile_macros_enabled(void);
/** When true (default), macro mouse deltas add to the user's current mouse deltas on each tick. */
bool macro_profile_additive_mouse_enabled(void);
/** User eDPI from profile (0 = unset). */
float macro_profile_get_edpi(void);
/** Reference eDPI patterns were authored for (default 800). */
float macro_profile_get_pattern_edpi(void);
/** Global multiplier: user eDPI / pattern eDPI (1.0 if unset). */
float macro_profile_get_edpi_scale(void);
/** Combined scale for macro group index (0..MAX_KEY_MODIFICATION_SEQUENCE-1). */
float macro_profile_sequence_mouse_scale(int group_index);
/** Refresh runtime scale table after `group_sequence` is updated. */
void macro_profile_sync_active_group_scales(const group_sequence_t *gs);
uint16_t macro_profile_script_count(void);
uint16_t macro_profile_active_script_index(void);
const char *macro_profile_script_name(uint16_t index);
/** Status JSON for /api/status and WebSocket push (weapon names, mode scales, etc.). */
#define MACRO_PROFILE_STATUS_JSON_MAX 4096
void macro_profile_build_status_json(char *buf, size_t buflen);

/** Next bullet timer delay (nominal step `us` + timingPct/jitterUs when humanize enabled). */
uint32_t macro_profile_step_delay_us(uint32_t nominal_us);
/** Delay when a tick is late — avoids 1 ms catch-up bursts. */
uint32_t macro_profile_catchup_delay_us(uint32_t nominal_us);
/** Optional ±1..N pixel noise on macro mouse deltas when enabled in profile. */
void macro_profile_apply_mouse_jitter(hid_mouse_report_t *m);
/** Same for int16 recoil step deltas (`x` / `y` only). */
void macro_profile_apply_mouse_step_jitter(int16_t *x, int16_t *y);
/** USB spread interval between macro drip slices (µs). 0 = one report per bullet tick (no spread). */
uint32_t macro_profile_mouse_drip_interval_us(void);
/** Apply profile `humanize` drip interval to the HID drip timer (after profile load). */
void macro_profile_sync_mouse_drip(void);

#if CONFIG_MACRO_WEB_UI
void macro_profile_try_action_hotkeys(const hid_keyboard_report_t *prev_k,
                                      const hid_keyboard_report_t *cur_k,
                                      const hid_mouse_report_t *prev_m,
                                      const hid_mouse_report_t *cur_m);

bool macro_profile_parse_json(const char *json, group_sequence_t *out);
esp_err_t macro_profile_ensure_spiffs_mounted(void);
#if CONFIG_MACRO_WEB_UI
/** Last failure from macro_profile_parse_json (empty if none). */
const char *macro_profile_get_parse_error(void);
#endif

/** Same as physical `nextScript` hotkey: advance active script bank (no-op if only one script). */
void macro_profile_http_next_script(void);
/** Select active weapon/script bank by index (clamped). */
void macro_profile_http_set_active_weapon(uint16_t index);
/** Same as physical `toggleMacros` hotkey: flip global macro output on/off. */
void macro_profile_http_toggle_macros(void);
#endif
