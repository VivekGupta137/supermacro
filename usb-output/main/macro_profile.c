#include "config.h"

#include "macro_profile.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "esp_idf_version.h"
#include "macro_ws.h"

#if CONFIG_MACRO_WEB_UI
#include <inttypes.h>

#include "cJSON.h"
#include "esp_partition.h"
#include "esp_spiffs.h"
#endif

/* Keep in sync: v1 default script label copies `s_profile_name` into `s_script_names[0]`. */
#define MACRO_PROFILE_NAME_CAP 40

static char s_profile_name[MACRO_PROFILE_NAME_CAP] = "built-in";
static bool s_macros_enabled = true;
#if CONFIG_MACRO_WEB_UI
#define MAX_MACRO_SCRIPTS 4
#define MAX_SCRIPT_NAME_LEN MACRO_PROFILE_NAME_CAP

static group_sequence_t s_script_banks[MAX_MACRO_SCRIPTS];
static uint8_t s_script_count;
static uint8_t s_active_script;
static char s_script_names[MAX_MACRO_SCRIPTS][MAX_SCRIPT_NAME_LEN];
static hid_transmit_t s_toggle_macros_trig;
static hid_transmit_t s_next_script_trig;
static bool s_has_toggle_macros_trig;
static bool s_has_next_script_trig;
static int s_schema_version = 1;
static uint32_t s_status_seq;
#endif

const char *macro_profile_get_name(void) { return s_profile_name; }

bool macro_profile_macros_enabled(void) { return s_macros_enabled; }

uint8_t macro_profile_script_count(void)
{
#if CONFIG_MACRO_WEB_UI
    return (s_script_count == 0) ? 1 : s_script_count;
#else
    return 1;
#endif
}

uint8_t macro_profile_active_script_index(void)
{
#if CONFIG_MACRO_WEB_UI
    if (s_script_count == 0) {
        return 0;
    }
    if (s_active_script >= s_script_count) {
        return 0;
    }
    return s_active_script;
#else
    return 0;
#endif
}

const char *macro_profile_script_name(uint8_t index)
{
#if CONFIG_MACRO_WEB_UI
    if (s_script_count == 0 || index >= s_script_count) {
        return macro_profile_get_name();
    }
    return s_script_names[index];
#else
    (void)index;
    return macro_profile_get_name();
#endif
}

#if CONFIG_MACRO_WEB_UI
static int count_dense_macro_groups(const group_sequence_t *gs)
{
    int n = 0;
    for (int i = 0; i < MAX_KEY_MODIFICATION_SEQUENCE; i++) {
        if (gs->list[i].size == 0) {
            break;
        }
        n++;
    }
    return n;
}
#endif

void macro_profile_build_status_json(char *buf, size_t buflen)
{
#if CONFIG_MACRO_WEB_UI
    int sn = (int)macro_profile_script_count();
    int ai = (int)macro_profile_active_script_index();
    const group_sequence_t *gs_for_count = &group_sequence;
    if (s_script_count > 0 && (uint8_t)ai < s_script_count) {
        gs_for_count = &s_script_banks[(uint8_t)ai];
    }
    int ag = count_dense_macro_groups(gs_for_count);
    uint32_t seq = ++s_status_seq;

    char fw[64];
    snprintf(fw, sizeof(fw), "IDF %s", esp_get_idf_version());

    cJSON *root = cJSON_CreateObject();
    if (!root || buflen < 8) {
        if (root) {
            cJSON_Delete(root);
        }
        if (buflen > 0) {
            buf[0] = '\0';
        }
        return;
    }
    cJSON_AddNumberToObject(root, "schemaVer", s_schema_version);
    cJSON_AddNumberToObject(root, "statusSeq", (double)seq);
    cJSON_AddStringToObject(root, "fw", fw);
    cJSON_AddStringToObject(root, "profile", macro_profile_get_name());
    cJSON_AddBoolToObject(root, "macrosOn", s_macros_enabled);
    cJSON_AddNumberToObject(root, "activeScript", ai);
    cJSON_AddNumberToObject(root, "scriptCount", sn);
    cJSON_AddStringToObject(root, "activeScriptName", macro_profile_script_name((uint8_t)ai));
    cJSON_AddNumberToObject(root, "activeGroupCount", ag);
    cJSON *names = cJSON_CreateArray();
    if (names) {
        for (int i = 0; i < sn; i++) {
            cJSON_AddItemToArray(names, cJSON_CreateString(macro_profile_script_name((uint8_t)i)));
        }
        cJSON_AddItemToObject(root, "scriptNames", names);
    }

    char *printed = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    if (!printed) {
        buf[0] = '\0';
        return;
    }
    strncpy(buf, printed, buflen - 1);
    buf[buflen - 1] = '\0';
    free(printed);
#else
    snprintf(buf, buflen,
             "{\"schemaVer\":1,\"fw\":\"IDF %s\",\"profile\":\"%s\","
             "\"macrosOn\":true,\"activeScript\":0,\"scriptCount\":1,\"scriptNames\":[\"built-in\"]}",
             esp_get_idf_version(), macro_profile_get_name());
#endif
}

#if CONFIG_MACRO_WEB_UI

static bool trigger_rising_edge(const hid_transmit_t *trig,
                                const hid_keyboard_report_t *prev_k,
                                const hid_keyboard_report_t *cur_k,
                                const hid_mouse_report_t *prev_m,
                                const hid_mouse_report_t *cur_m)
{
    if (trig->header == HEADER_HID_MOUSE && prev_m != NULL && cur_m != NULL) {
        return mouse_report_contains_event(*cur_m, trig->event.mouse) &&
               !mouse_report_contains_event(*prev_m, trig->event.mouse);
    }
    if (trig->header == HEADER_HID_KEYBOARD && prev_k != NULL && cur_k != NULL) {
        return keyboard_report_contains_event(*cur_k, trig->event.keyboard) &&
               !keyboard_report_contains_event(*prev_k, trig->event.keyboard);
    }
    return false;
}

static void advance_active_script(void)
{
    if (s_script_count <= 1) {
        return;
    }
    s_active_script = (uint8_t)((s_active_script + 1) % s_script_count);
    macro_sequences_apply(&s_script_banks[s_active_script]);
    macro_ws_request_broadcast();
}

void macro_profile_http_next_script(void)
{
    if (s_script_count <= 1) {
        macro_ws_request_broadcast();
        return;
    }
    advance_active_script();
}

void macro_profile_http_toggle_macros(void)
{
    s_macros_enabled = !s_macros_enabled;
    macro_ws_request_broadcast();
}

void macro_profile_try_action_hotkeys(const hid_keyboard_report_t *prev_k,
                                      const hid_keyboard_report_t *cur_k,
                                      const hid_mouse_report_t *prev_m,
                                      const hid_mouse_report_t *cur_m)
{
    if (s_has_toggle_macros_trig &&
        trigger_rising_edge(&s_toggle_macros_trig, prev_k, cur_k, prev_m, cur_m)) {
        s_macros_enabled = !s_macros_enabled;
        macro_ws_request_broadcast();
    }
    if (s_has_next_script_trig &&
        trigger_rising_edge(&s_next_script_trig, prev_k, cur_k, prev_m, cur_m)) {
        advance_active_script();
    }
}

static void profile_runtime_reset_parsed(void)
{
    memset(s_script_banks, 0, sizeof(s_script_banks));
    s_script_count = 0;
    s_active_script = 0;
    s_has_toggle_macros_trig = false;
    s_has_next_script_trig = false;
    memset(&s_toggle_macros_trig, 0, sizeof(s_toggle_macros_trig));
    memset(&s_next_script_trig, 0, sizeof(s_next_script_trig));
}

static void dump_partition_table(void)
{
    esp_partition_iterator_t it =
        esp_partition_find(ESP_PARTITION_TYPE_ANY, ESP_PARTITION_SUBTYPE_ANY, NULL);
    if (it == NULL) {
        ESP_LOGW(LOG_TITLE, "Partition iterator: no entries (unexpected)");
        return;
    }
    esp_partition_iterator_t head = it;
    ESP_LOGW(LOG_TITLE, "Partitions on this chip (SPIFFS = type DATA subtype 0x82):");
    while (it != NULL) {
        const esp_partition_t *p = esp_partition_get(it);
        ESP_LOGW(LOG_TITLE,
                 "  label=\"%s\" type=%u subtype=0x%02x addr=0x%08" PRIx32 " size=%" PRIu32,
                 p->label != NULL ? p->label : "",
                 (unsigned)p->type,
                 (unsigned)p->subtype,
                 (uint32_t)p->address,
                 (uint32_t)p->size);
        it = esp_partition_next(it);
    }
    esp_partition_iterator_release(head);
}

static esp_err_t spiffs_register_at(const char *partition_label)
{
    esp_vfs_spiffs_conf_t conf = {
        .base_path = CONFIG_MACRO_SPIFFS_MOUNT,
        .partition_label = partition_label,
        .max_files = 5,
        .format_if_mount_failed = true,
    };
    return esp_vfs_spiffs_register(&conf);
}

static esp_err_t spiffs_mount_once(void)
{
    static bool mounted;
    if (mounted) {
        return ESP_OK;
    }
    esp_err_t err = spiffs_register_at("storage");
    if (err == ESP_ERR_NOT_FOUND) {
        ESP_LOGW(LOG_TITLE, "No SPIFFS partition \"storage\"; trying first SPIFFS in table");
        err = spiffs_register_at(NULL);
    }
    if (err == ESP_OK) {
        mounted = true;
    } else {
        dump_partition_table();
        ESP_LOGE(LOG_TITLE,
                 "SPIFFS mount failed: %s (path=%s). "
                 "Fix: menuconfig → Partition Table → Custom = partitions.csv; Flash size = chip size; "
                 "then: idf.py erase-flash partition-table-flash flash",
                 esp_err_to_name(err), CONFIG_MACRO_SPIFFS_MOUNT);
    }
    return err;
}

esp_err_t macro_profile_ensure_spiffs_mounted(void) { return spiffs_mount_once(); }

static int clamp_i32_to_i8(int v)
{
    if (v > 127) {
        return 127;
    }
    if (v < -128) {
        return -128;
    }
    return v;
}

static bool fill_keyboard_event(hid_transmit_t *t, const cJSON *kbd)
{
    t->header = HEADER_HID_KEYBOARD;
    memset(&t->event.keyboard, 0, sizeof(t->event.keyboard));
    if (!kbd || !cJSON_IsObject(kbd)) {
        return true;
    }
    const cJSON *mod = cJSON_GetObjectItem(kbd, "m");
    if (mod && cJSON_IsNumber(mod)) {
        t->event.keyboard.modifier = (uint8_t)mod->valuedouble;
    }
    const cJSON *keys = cJSON_GetObjectItem(kbd, "k");
    if (!keys || !cJSON_IsArray(keys)) {
        return true;
    }
    int i = 0;
    const cJSON *it;
    cJSON_ArrayForEach(it, keys)
    {
        if (i >= 6) {
            return false;
        }
        if (!cJSON_IsNumber(it)) {
            return false;
        }
        t->event.keyboard.keycode[i++] = (uint8_t)it->valuedouble;
    }
    return true;
}

static bool fill_mouse_event(hid_transmit_t *t, const cJSON *mouse)
{
    t->header = HEADER_HID_MOUSE;
    memset(&t->event.mouse, 0, sizeof(t->event.mouse));
    if (!mouse || !cJSON_IsObject(mouse)) {
        return true;
    }
    const cJSON *b = cJSON_GetObjectItem(mouse, "b");
    if (b && cJSON_IsNumber(b)) {
        t->event.mouse.buttons = (uint8_t)b->valuedouble;
    }
    const cJSON *x = cJSON_GetObjectItem(mouse, "x");
    if (x && cJSON_IsNumber(x)) {
        t->event.mouse.x = (int8_t)clamp_i32_to_i8((int)x->valuedouble);
    }
    const cJSON *y = cJSON_GetObjectItem(mouse, "y");
    if (y && cJSON_IsNumber(y)) {
        t->event.mouse.y = (int8_t)clamp_i32_to_i8((int)y->valuedouble);
    }
    const cJSON *w = cJSON_GetObjectItem(mouse, "w");
    if (w && cJSON_IsNumber(w)) {
        t->event.mouse.wheel = (int8_t)clamp_i32_to_i8((int)w->valuedouble);
    }
    const cJSON *p = cJSON_GetObjectItem(mouse, "p");
    if (p && cJSON_IsNumber(p)) {
        t->event.mouse.pan = (int8_t)clamp_i32_to_i8((int)p->valuedouble);
    }
    return true;
}

static bool fill_step(const cJSON *step, key_modification_event_t *ev)
{
    if (!step || !cJSON_IsObject(step)) {
        return false;
    }
    const cJSON *us = cJSON_GetObjectItem(step, "us");
    if (!us || !cJSON_IsNumber(us) || us->valuedouble < 0) {
        return false;
    }
    ev->duration = (unsigned int)us->valuedouble;
    const cJSON *mouse = cJSON_GetObjectItem(step, "mouse");
    const cJSON *kbd = cJSON_GetObjectItem(step, "kbd");
    if (mouse) {
        return fill_mouse_event(&ev->event, mouse);
    }
    if (kbd) {
        return fill_keyboard_event(&ev->event, kbd);
    }
    ev->event.header = HEADER_HID_KEYBOARD;
    memset(&ev->event.event.keyboard, 0, sizeof(ev->event.event.keyboard));
    return true;
}

static bool fill_trigger(hid_transmit_t *t, const cJSON *obj)
{
    if (!obj || cJSON_IsNull(obj)) {
        memset(t, 0, sizeof(*t));
        return true;
    }
    if (!cJSON_IsObject(obj)) {
        return false;
    }
    const cJSON *mouse = cJSON_GetObjectItem(obj, "mouse");
    const cJSON *kbd = cJSON_GetObjectItem(obj, "kbd");
    if (mouse) {
        return fill_mouse_event(t, mouse);
    }
    if (kbd) {
        return fill_keyboard_event(t, kbd);
    }
    memset(t, 0, sizeof(*t));
    return true;
}

static bool parse_one_group(const cJSON *g, key_modification_sequence_t *seq)
{
    const cJSON *steps = cJSON_GetObjectItem(g, "steps");
    int nsteps = 0;
    if (steps && cJSON_IsArray(steps)) {
        nsteps = cJSON_GetArraySize(steps);
    }
    if (nsteps == 0) {
        seq->list[0].duration = 0;
        seq->list[0].event.header = HEADER_HID_KEYBOARD;
        memset(&seq->list[0].event.event.keyboard, 0, sizeof(seq->list[0].event.event.keyboard));
        seq->size = 1;
    } else {
        if (nsteps > MAX_KEY_MODIFICATION_EVENT) {
            return false;
        }
        const cJSON *n = cJSON_GetObjectItem(g, "n");
        if (n && cJSON_IsNumber(n)) {
            int want = (int)n->valuedouble;
            if (want > 0 && want <= nsteps) {
                nsteps = want;
            } else if (want > nsteps || want < 0) {
                return false;
            }
        }
        for (int i = 0; i < nsteps; i++) {
            if (!fill_step(cJSON_GetArrayItem(steps, i), &seq->list[i])) {
                return false;
            }
        }
        seq->size = (uint8_t)nsteps;
    }
    const cJSON *loop = cJSON_GetObjectItem(g, "loop");
    seq->loop = cJSON_IsTrue(loop);
    if (!fill_trigger(&seq->event_press, cJSON_GetObjectItem(g, "press"))) {
        return false;
    }
    if (!fill_trigger(&seq->event_release, cJSON_GetObjectItem(g, "release"))) {
        return false;
    }
    if (!fill_trigger(&seq->save_press, cJSON_GetObjectItem(g, "save"))) {
        return false;
    }
    return true;
}

static bool fill_groups_from_json(const cJSON *groups, group_sequence_t *out_seq)
{
    if (!groups || !cJSON_IsArray(groups)) {
        return false;
    }
    int ng = cJSON_GetArraySize(groups);
    if (ng <= 0 || ng > MAX_KEY_MODIFICATION_SEQUENCE) {
        return false;
    }
    for (int i = 0; i < ng; i++) {
        if (!parse_one_group(cJSON_GetArrayItem(groups, i), &out_seq->list[i])) {
            return false;
        }
    }
    return true;
}

static bool assign_hotkey_trigger(hid_transmit_t *dst, bool *has_out, const cJSON *root, const char *key)
{
    memset(dst, 0, sizeof(*dst));
    *has_out = false;
    const cJSON *node = cJSON_GetObjectItem(root, key);
    if (!node || cJSON_IsNull(node) || cJSON_IsFalse(node)) {
        return true;
    }
    if (!cJSON_IsObject(node)) {
        return false;
    }
    if (!fill_trigger(dst, node)) {
        return false;
    }
    *has_out = (dst->header != 0);
    return true;
}

bool macro_profile_parse_json(const char *json, group_sequence_t *out)
{
    memset(out, 0, sizeof(*out));
    cJSON *root = cJSON_Parse(json);
    if (!root) {
        return false;
    }
    const cJSON *ver = cJSON_GetObjectItem(root, "v");
    if (!ver || !cJSON_IsNumber(ver)) {
        cJSON_Delete(root);
        return false;
    }
    int vn = (int)ver->valuedouble;
    if (vn != 1 && vn != 2) {
        cJSON_Delete(root);
        return false;
    }

    profile_runtime_reset_parsed();
    s_schema_version = vn;

    const cJSON *name = cJSON_GetObjectItem(root, "name");
    if (name && cJSON_IsString(name) && name->valuestring) {
        strncpy(s_profile_name, name->valuestring, sizeof(s_profile_name) - 1);
        s_profile_name[sizeof(s_profile_name) - 1] = '\0';
    } else {
        strncpy(s_profile_name, "flash", sizeof(s_profile_name) - 1);
        s_profile_name[sizeof(s_profile_name) - 1] = '\0';
    }

    if (vn == 1) {
        const cJSON *groups = cJSON_GetObjectItem(root, "groups");
        if (!fill_groups_from_json(groups, out)) {
            cJSON_Delete(root);
            memset(out, 0, sizeof(*out));
            profile_runtime_reset_parsed();
            return false;
        }
        memcpy(&s_script_banks[0], out, sizeof(group_sequence_t));
        s_script_count = 1;
        strncpy(s_script_names[0], s_profile_name, sizeof(s_script_names[0]) - 1);
        s_script_names[0][sizeof(s_script_names[0]) - 1] = '\0';
        s_active_script = 0;
        s_macros_enabled = true;
        s_has_toggle_macros_trig = false;
        s_has_next_script_trig = false;
        cJSON_Delete(root);
        return true;
    }

    const cJSON *scripts = cJSON_GetObjectItem(root, "scripts");
    if (scripts && cJSON_IsArray(scripts)) {
        int ns = cJSON_GetArraySize(scripts);
        if (ns <= 0 || ns > MAX_MACRO_SCRIPTS) {
            cJSON_Delete(root);
            memset(out, 0, sizeof(*out));
            profile_runtime_reset_parsed();
            return false;
        }
        for (int si = 0; si < ns; si++) {
            const cJSON *sp = cJSON_GetArrayItem(scripts, si);
            const cJSON *grp = cJSON_GetObjectItem(sp, "groups");
            if (!fill_groups_from_json(grp, &s_script_banks[si])) {
                cJSON_Delete(root);
                memset(out, 0, sizeof(*out));
                profile_runtime_reset_parsed();
                return false;
            }
            const cJSON *sn = cJSON_GetObjectItem(sp, "name");
            if (sn && cJSON_IsString(sn) && sn->valuestring && sn->valuestring[0]) {
                strncpy(s_script_names[si], sn->valuestring, sizeof(s_script_names[si]) - 1);
                s_script_names[si][sizeof(s_script_names[si]) - 1] = '\0';
            } else {
                snprintf(s_script_names[si], sizeof(s_script_names[si]), "script%d", si);
            }
        }
        s_script_count = (uint8_t)ns;
    } else {
        const cJSON *groups = cJSON_GetObjectItem(root, "groups");
        if (!fill_groups_from_json(groups, &s_script_banks[0])) {
            cJSON_Delete(root);
            memset(out, 0, sizeof(*out));
            profile_runtime_reset_parsed();
            return false;
        }
        s_script_count = 1;
        strncpy(s_script_names[0], s_profile_name, sizeof(s_script_names[0]) - 1);
        s_script_names[0][sizeof(s_script_names[0]) - 1] = '\0';
    }

    const cJSON *active = cJSON_GetObjectItem(root, "activeScript");
    if (active && cJSON_IsNumber(active)) {
        int a = (int)active->valuedouble;
        if (a < 0) {
            a = 0;
        }
        if (a >= (int)s_script_count) {
            a = (int)s_script_count - 1;
        }
        s_active_script = (uint8_t)a;
    } else {
        s_active_script = 0;
    }

    const cJSON *mon = cJSON_GetObjectItem(root, "macrosOn");
    s_macros_enabled = !cJSON_IsFalse(mon);

    if (!assign_hotkey_trigger(&s_toggle_macros_trig, &s_has_toggle_macros_trig, root, "toggleMacros")) {
        cJSON_Delete(root);
        memset(out, 0, sizeof(*out));
        profile_runtime_reset_parsed();
        return false;
    }
    if (!assign_hotkey_trigger(&s_next_script_trig, &s_has_next_script_trig, root, "nextScript")) {
        cJSON_Delete(root);
        memset(out, 0, sizeof(*out));
        profile_runtime_reset_parsed();
        return false;
    }

    memcpy(out, &s_script_banks[s_active_script], sizeof(*out));
    cJSON_Delete(root);
    return true;
}

static bool load_profile_from_flash(void)
{
    char path[128];
    snprintf(path, sizeof(path), "%s/%s", CONFIG_MACRO_SPIFFS_MOUNT, CONFIG_MACRO_PROFILE_JSON);
    FILE *f = fopen(path, "rb");
    if (!f) {
        return false;
    }
    fseek(f, 0, SEEK_END);
    long sz = ftell(f);
    rewind(f);
    if (sz <= 0 || sz > CONFIG_MACRO_PROFILE_MAX_SIZE) {
        fclose(f);
        return false;
    }
    char *buf = malloc((size_t)sz + 1);
    if (!buf) {
        fclose(f);
        return false;
    }
    if (fread(buf, 1, (size_t)sz, f) != (size_t)sz) {
        free(buf);
        fclose(f);
        return false;
    }
    buf[sz] = '\0';
    fclose(f);
    bool ok = macro_profile_parse_json(buf, &group_sequence);
    free(buf);
    return ok;
}

#endif /* CONFIG_MACRO_WEB_UI */

void macro_profile_init(const group_sequence_t *fallback)
{
    memset(&group_sequence, 0, sizeof(group_sequence));
    strncpy(s_profile_name, "built-in", sizeof(s_profile_name) - 1);
    s_profile_name[sizeof(s_profile_name) - 1] = '\0';

#if CONFIG_MACRO_WEB_UI
    esp_err_t mnt = spiffs_mount_once();
    if (mnt == ESP_OK && load_profile_from_flash()) {
        ESP_LOGI(LOG_TITLE, "Macro profile loaded from SPIFFS (%s)", s_profile_name);
        return;
    }
    if (mnt != ESP_OK) {
        ESP_LOGW(LOG_TITLE, "SPIFFS unavailable (%s); profile upload needs a \"storage\" SPIFFS partition",
                 esp_err_to_name(mnt));
    }
#endif
    group_sequence = *fallback;
#if CONFIG_MACRO_WEB_UI
    memcpy(&s_script_banks[0], &group_sequence, sizeof(group_sequence_t));
    s_script_count = 1;
    s_active_script = 0;
    snprintf(s_script_names[0], sizeof(s_script_names[0]), "built-in");
    s_has_toggle_macros_trig = false;
    s_has_next_script_trig = false;
    s_macros_enabled = true;
    s_schema_version = 1;
#endif
}
