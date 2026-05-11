
#define HEADER_HID_KEYBOARD 0xA1
#define HEADER_HID_MOUSE 0xA3
#define HEADER_PC_TRANSMISSION 0xA2

typedef struct
{
    uint8_t modifier;   /**< Keyboard modifier (KEYBOARD_MODIFIER_* masks). */
    uint8_t reserved;   /**< Reserved for OEM use, always set to 0. */
    uint8_t keycode[6]; /**< Key codes of the currently pressed keys. */
} hid_keyboard_report_t;

typedef struct
{
    uint8_t buttons; /**< buttons mask for currently pressed buttons in the mouse. */
    int8_t x;        /**< Current delta x movement of the mouse. */
    int8_t y;        /**< Current delta y movement on the mouse. */
    int8_t wheel;    /**< Current delta wheel movement on the mouse. */
    int8_t pan;      // using AC Pan
} hid_mouse_report_t;

typedef union
{
    hid_keyboard_report_t keyboard;
    hid_mouse_report_t mouse;
} hid_report_t;

/**
 * @brief Check if a specific key is present in the keyboard report
 *
 * @param report Keyboard HID report
 * @param keycode The keycode to search for
 * @return true if the keycode is found, false otherwise
 */
static inline bool keycode_contains_key(hid_keyboard_report_t report, uint8_t keycode)
{
    for (int i = 0; i < 6; i++)
    {
        if (report.keycode[i] == keycode)
        {
            return true;
        }
    }
    return false;
}
