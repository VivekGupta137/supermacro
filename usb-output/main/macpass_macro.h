#pragma once

// Define size of sequence structure. Set as lower as possible. Can impact performance.
#define HISTORY_SIZE 2
#define MAX_KEY_MODIFICATION_EVENT 100
#define MAX_KEY_MODIFICATION_SEQUENCE 10

typedef struct {
    unsigned int duration;
    hid_transmit_t event;
    /** Per-step mouse deltas (int16 at parse); USB sends int8 slices via drip. */
    int16_t mouse_x;
    int16_t mouse_y;
    int16_t mouse_wheel;
    int16_t mouse_pan;
} key_modification_event_t;

typedef struct {
    // --- User defined variable
    key_modification_event_t list[MAX_KEY_MODIFICATION_EVENT]; // List of HID event to send
    uint8_t size; // Size of the list of event
    hid_transmit_t event_press; // Detect on press (mouse mask and/or legacy kbd-only)
    hid_transmit_t event_release; // Detect on release
    hid_transmit_t save_press; // Press to save a sequence
    /** v3 combined press: require mouse chord in event_press.event.mouse. */
    bool press_mouse_required;
    /** v3 combined press: require keyboard state in press_kbd. */
    bool press_kbd_required;
    hid_keyboard_report_t press_kbd;
    bool loop; // Play the sequence on a loop
    /** (eDPI / patternEDPI) * script/group `scale`; 1.0 = no scaling. */
    float mouse_scale;
    /** v3: true = button mask must match exactly (LMB-only vs LMB+RMB). */
    bool press_exact;
    /** true = run full sequence on press rising edge; release does not stop mid-run. */
    bool press_tap;
    /** v3: peers with same non-zero set stop each other when a new mode starts. */
    uint8_t mode_set;

    // --- Computed
    hid_transmit_t previous_key;
    uint8_t pos;
    int64_t started_time;
    int64_t waited_sum;
    bool is_recording;
    esp_timer_handle_t timer;
    esp_timer_create_args_t timer_args;
} key_modification_sequence_t;

typedef struct {
    key_modification_sequence_t list[MAX_KEY_MODIFICATION_SEQUENCE];
} group_sequence_t;

extern group_sequence_t group_sequence;
extern hid_mouse_report_t last_mouse_report;

/** Drop pending macro mouse spread when no mouse sequence is running. */
void macro_after_sequence_reset(void);

bool macro_prehook_transmission(hid_transmit_t* report);
void macro_posthook_transmission(hid_transmit_t* report);
void macro_sequence_callback(void* arg);
void macro_init(void);
void macro_sequences_apply(const group_sequence_t *src);
void start_sequence(key_modification_sequence_t *sequence);
/** Schedule next bullet tick after `step_us` (jittered timer; spread uses nominal step `us`). */
void start_sequence_with_delay(key_modification_sequence_t *sequence, uint32_t step_us);
