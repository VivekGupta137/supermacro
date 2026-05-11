#pragma once

#include "sdkconfig.h"
#include "class/hid/hid_device.h"
#include "macpass_macro.h"

void macro_profile_init(const group_sequence_t *fallback);

const char *macro_profile_get_name(void);

bool macro_profile_macros_enabled(void);
uint8_t macro_profile_script_count(void);
uint8_t macro_profile_active_script_index(void);
const char *macro_profile_script_name(uint8_t index);
void macro_profile_build_status_json(char *buf, size_t buflen);

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
