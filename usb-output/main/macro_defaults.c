#include "config.h"

#if !CUSTOM_CONFIG

const group_sequence_t macro_sequence_default = {
    .list = {
        {
            .list = {
                {1000 * 1000, EMPTY_KEYBOARD},
                {1000 * 1000, ONE_KEYBOARD_KEY(HID_KEY_KEYPAD_2)},
                {0, ONE_KEYBOARD_KEY(HID_KEY_KEYPAD_6)},
                {10 * 1000, EMPTY_KEYBOARD},
                {1000 * 1000, ONE_KEYBOARD_KEY(HID_KEY_KEYPAD_2)},
                {10 * 1000, EMPTY_KEYBOARD},
            },
            .size = 6,
            .event_press = ONE_KEYBOARD_KEY(HID_KEY_KEYPAD_3),
        },
        {
            .list = {
                {0, EMPTY_KEYBOARD},
                {1000 * 1000, ONE_KEYBOARD_KEY(HID_KEY_A)},
                {0, ONE_KEYBOARD_KEY(HID_KEY_B)},
                {2000 * 1000, EMPTY_KEYBOARD},
                {1000 * 1000, ONE_KEYBOARD_KEY(HID_KEY_A)},
                {10 * 1000, EMPTY_KEYBOARD},
            },
            .size = 6,
            .event_press = ONE_KEYBOARD_KEY(HID_KEY_KEYPAD_3),
        },
        {
            .list = {
                {0, ONE_KEYBOARD_KEY(HID_KEY_A)},
                {150 * 1000, EMPTY_KEYBOARD},
                {0, ONE_KEYBOARD_KEY(HID_KEY_D)},
                {150 * 1000, EMPTY_KEYBOARD},
            },
            .size = 4,
            .loop = true,
            .event_press = ONE_KEYBOARD_KEY(HID_KEY_X),
        },
        {
            .list = {
                {3 * 1000, MOUSE_MOUVEMENT(-2, 0)},
            },
            .size = 1,
            .loop = true,
            .event_press = ONE_KEYBOARD_KEY(HID_KEY_ARROW_LEFT),
        },
        {
            .list = {
                {3 * 1000, MOUSE_MOUVEMENT(2, 0)},
            },
            .size = 1,
            .loop = true,
            .event_press = ONE_KEYBOARD_KEY(HID_KEY_ARROW_RIGHT),
        },
        {
            .list = {
                {3 * 1000, MOUSE_MOUVEMENT(0, -2)},
            },
            .size = 1,
            .loop = true,
            .event_press = ONE_KEYBOARD_KEY(HID_KEY_ARROW_UP),
        },
        {
            .list = {
                {3 * 1000, MOUSE_MOUVEMENT(0, 2)},
            },
            .size = 1,
            .loop = true,
            .event_press = ONE_KEYBOARD_KEY(HID_KEY_ARROW_DOWN),
        },
        {
            .list = {
                {50 * 1000, MOUSE_MOUVEMENT(0, 3)},
            },
            .size = 1,
            .loop = true,
            .event_press = ONE_MOUSE_KEY(MOUSE_BUTTON_RIGHT),
        },
        {
            .size = 0,
            .save_press = ONE_KEYBOARD_KEY(HID_KEY_KEYPAD_SUBTRACT),
            .event_press = ONE_KEYBOARD_KEY(HID_KEY_KEYPAD_ADD),
        },
        {
            .list = {
                {50 * 1000, MOUSE_MOUVEMENT(calc(-4), calc(7))},
                {99 * 1000, MOUSE_MOUVEMENT(calc(4), calc(19))},
                {99 * 1000, MOUSE_MOUVEMENT(calc(-3), calc(29))},
                {99 * 1000, MOUSE_MOUVEMENT(calc(-1), calc(31))},
                {99 * 1000, MOUSE_MOUVEMENT(calc(13), calc(31))},
                {99 * 1000, MOUSE_MOUVEMENT(calc(8), calc(28))},
                {99 * 1000, MOUSE_MOUVEMENT(calc(13), calc(21))},
                {99 * 1000, MOUSE_MOUVEMENT(calc(-17), calc(12))},
                {99 * 1000, MOUSE_MOUVEMENT(calc(-42), calc(-3))},
                {99 * 1000, MOUSE_MOUVEMENT(calc(-21), calc(2))},
                {99 * 1000, MOUSE_MOUVEMENT(calc(12), calc(11))},
                {99 * 1000, MOUSE_MOUVEMENT(calc(-15), calc(7))},
                {99 * 1000, MOUSE_MOUVEMENT(calc(-26), calc(-8))},
                {99 * 1000, MOUSE_MOUVEMENT(calc(-3), calc(4))},
                {99 * 1000, MOUSE_MOUVEMENT(calc(40), calc(1))},
                {99 * 1000, MOUSE_MOUVEMENT(calc(19), calc(7))},
                {99 * 1000, MOUSE_MOUVEMENT(calc(14), calc(10))},
                {99 * 1000, MOUSE_MOUVEMENT(calc(27), calc(0))},
                {99 * 1000, MOUSE_MOUVEMENT(calc(33), calc(-10))},
                {99 * 1000, MOUSE_MOUVEMENT(calc(-21), calc(-2))},
                {99 * 1000, MOUSE_MOUVEMENT(calc(7), calc(3))},
                {99 * 1000, MOUSE_MOUVEMENT(calc(-7), calc(9))},
                {99 * 1000, MOUSE_MOUVEMENT(calc(-8), calc(4))},
                {99 * 1000, MOUSE_MOUVEMENT(calc(19), calc(-3))},
                {99 * 1000, MOUSE_MOUVEMENT(calc(5), calc(6))},
                {99 * 1000, MOUSE_MOUVEMENT(calc(-20), calc(-1))},
                {99 * 1000, MOUSE_MOUVEMENT(calc(-33), calc(-4))},
                {99 * 1000, MOUSE_MOUVEMENT(calc(-45), calc(-21))},
                {99 * 1000, MOUSE_MOUVEMENT(calc(-14), calc(1))},
            },
            .size = 29,
            .loop = true,
            .event_press = ONE_MOUSE_KEY(MOUSE_BUTTON_RIGHT),
        },
    },
};

#endif /* !CUSTOM_CONFIG */
