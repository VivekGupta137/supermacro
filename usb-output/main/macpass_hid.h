
#define HEADER_HID_KEYBOARD 0xA1
#define HEADER_HID_MOUSE 0xA3
#define HEADER_PC_TRANSMISSION 0xA2

void hid_init_multiplexer();
void hid_wake_pump(void);
void hid_add_report(hid_transmit_t report);

/** Queue a macro step delta; drips out on a timer and on SPI mouse reports (rate-limited). */
void hid_macro_feed_mouse_step(int16_t x, int16_t y, int16_t wheel, int16_t pan, uint32_t step_us);
/** Send all pending spread remainder immediately; clears segment. */
void hid_macro_flush_mouse_spread(void);
void hid_macro_cancel_mouse_spread(void);
bool hid_macro_mouse_spread_active(void);
/** Restart periodic drip timer using `macro_profile_mouse_drip_interval_us()`. */
void hid_mouse_drip_apply_interval(void);
