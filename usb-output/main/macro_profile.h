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
uint8_t macro_profile_script_count(void);
uint8_t macro_profile_active_script_index(void);
const char *macro_profile_script_name(uint8_t index);
void macro_profile_build_status_json(char *buf, size_t buflen);

/** Per-step delay after humanize jitter (µs). */
uint32_t macro_profile_step_delay_us(uint32_t nominal_us);
/** Delay when a tick is late — avoids 1 ms catch-up bursts. */
uint32_t macro_profile_catchup_delay_us(uint32_t nominal_us);
/** Optional ±1..N pixel noise on macro mouse deltas when enabled in profile. */
void macro_profile_apply_mouse_jitter(hid_mouse_report_t *m);

#if CONFIG_MACRO_WEB_UI
void macro_profile_try_action_hotkeys(const hid_keyboard_report_t *prev_k,
                                      const hid_keyboard_report_t *cur_k,
                                      const hid_mouse_report_t *prev_m,
                                      const hid_mouse_report_t *cur_m);

bool macro_profile_parse_json(const char *json, group_sequence_t *out);
esp_err_t macro_profile_ensure_spiffs_mounted(void);

/** Same as physical `nextScript` hotkey: advance active script bank (no-op if only one script). */
void macro_profile_http_next_script(void);
/** Same as physical `toggleMacros` hotkey: flip global macro output on/off. */
void macro_profile_http_toggle_macros(void);
#endif
