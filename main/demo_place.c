// Places + nearby passports + Bubu pet.
//
// One persistent worker owns Wi-Fi, NimBLE, NVS, audio and all mutable model
// data. Button callbacks only update small UI selectors and enqueue commands.
// Each 60-second radio cycle is:
//   Wi-Fi scan and full deinit -> 1 second RF guard -> bounded BLE window and
//   full deinit -> idle remainder.
#include "demo.h"
#include "demo_radio.h"
#include "passport_ble.h"
#include "passport_pet.h"
#include "passport_social.h"
#include "place_fp.h"
#include "pet_sprites.h"
#include "tribe_icon_pack.h"
#include "ui_pixel.h"
#include "wifi_portal.h"

#include "bsp_audio.h"
#include "bsp_battery.h"
#include "bsp_display.h"
#include "bsp_wifi_scan.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_pm.h"
#include "esp_random.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"
#include "lvgl.h"
#include "nvs.h"
#include "nvs_flash.h"

#include <inttypes.h>
#include <stdio.h>
#include <string.h>

static const char *TAG = "places_social";

#define CYCLE_MS                60000u
#define RF_GUARD_MS              1000u
#define QUICK_RESCAN_MS          4000u
#define FLUSH_THROTTLE_MS      300000u
#define NVS_NAMESPACE        "placemem"
#define NVS_KEY_PLACES             "db"
#define NVS_KEY_SOCIAL         "social"
#define PLACE_DB_MAGIC       0x504c4331u
#define PLACE_DB_VERSION              2u
#define SOCIAL_MAGIC         0x534f4331u
#define SOCIAL_VERSION                1u
#define DEFAULT_TRIBE_CODE       0x4d44u
#define TRAIL_MAX                    24u
#define NVS_ENTRY_BYTES              32u
#define NVS_BLOB_ENTRY_COUNT(bytes)  (2u + (((bytes) + 31u) / 32u))

typedef enum {
    CMD_ACTIVE = 1,
    CMD_IDLE,
    CMD_SCAN,
    CMD_RENDER,
    CMD_NAME_PLACE,
    CMD_NEXT_ARCHIVE,
    CMD_NEXT_SIGNAL,
    CMD_NEXT_ICON,
    CMD_TOGGLE_STEALTH,
    CMD_TOGGLE_REGULARS,
    CMD_NEXT_REGULAR,
    CMD_RESET_ID,
    CMD_QUIET_REPLY,
} command_type_t;

typedef struct {
    uint8_t type;
    uint16_t value;
} command_t;

typedef enum {
    VIEW_LIVE = 0,
    VIEW_ARCHIVE,
    VIEW_TRAIL,
    VIEW_SIGNAL,
    VIEW_ICON,
    VIEW_STEALTH,
    VIEW_REGULARS,
    VIEW_PET,
    VIEW_RESET_ID,
    VIEW_COUNT,
} place_view_t;

typedef struct {
    uint8_t icon;
    uint8_t reserved[3];
    uint32_t stay_seconds;
} place_meta_t;

typedef struct {
    uint32_t magic;
    uint16_t version;
    uint16_t size;
    uint16_t anonymous_id;
    uint16_t tribe_code;
    uint8_t signal_code;
    uint8_t icon_index;
    uint8_t stealth;
    uint8_t regulars_enabled;
    uint8_t pet_stage;
    uint8_t crowd_events;
    uint16_t reserved;
    uint32_t same_tribe_encounters;
    uint32_t runtime_without_new_place_seconds;
    place_meta_t place_meta[PLACE_MAX_COUNT];
    passport_regular_db_t regulars;
    passport_memorial_db_t memorials;
} social_state_t;

typedef struct {
    uint8_t place_index;
    uint8_t icon;
} trail_event_t;

_Static_assert(sizeof(social_state_t) < 2048, "social NVS blob must stay compact");

static const char *const PLACE_NAMES[] = {
    "HOME", "WORK", "CAFE", "METRO", "GYM", "CUSTOM",
};
static const uint32_t PLACE_COLORS[] = {
    0xE85D5D, 0x4C78A8, 0xD9983D, 0x845EC2, 0x3C9D72, 0x64748B,
};
static const char *const SIGNAL_NAMES[] = {
    "HELLO", "COFFEE BREAK", "DDL - BUSY", "LET'S PLAY", "NEED HELP",
};
static const char *const ICON_NAMES[] = {
    "CARROT", "RABBIT", "PINK", "RED", "GREEN", "BLUE", "YELLOW", "NINEBALL",
};
static const char *const PET_FORM_NAMES[] = {
    "SEED", "SPROUT", "YOUNG", "BUBU", "TRAVELER", "WORLD",
};
static const char *const PET_TRAIT_NAMES[] = {
    "HOMEBODY", "ROVER", "SOCIAL", "SOLO", "ARENA",
};

static place_db_t s_db;
static social_state_t s_social;
static nvs_handle_t s_nvs;
static bool s_nvs_ok;
static bool s_dirty;

static QueueHandle_t s_queue;
static TaskHandle_t s_worker;
static volatile bool s_active;
static volatile bool s_scanning;
static volatile uint32_t s_next_due;
static volatile place_view_t s_view;
static volatile int s_pending_name = -1;
static volatile uint8_t s_name_choice;
static uint8_t s_archive_rank;
static uint8_t s_regular_cursor;

static int s_current_place = -1;
static uint32_t s_last_account_ms;
static uint8_t s_current_place_hash;
static place_session_t s_match_session;
static trail_event_t s_trail[TRAIL_MAX];
static uint8_t s_trail_count;
static bool s_crowd_active;
static uint32_t s_crowd_started_ms;
static passport_crowd_counter_t s_crowd_peak;
static passport_payload_t s_last_peer;
static bool s_have_peer;
static uint16_t s_other_tribe_count;
static uint32_t s_encounter_until_ms;
static bool s_quiet_message;
static passport_quiet_reply_state_t s_quiet_reply;
static int s_last_score = -1;
static char s_live_status[32] = "STARTING...";
static char s_live_detail[96] = "Waiting for first scan";

static lv_obj_t *s_scr;
static lv_obj_t *s_title;
static lv_obj_t *s_status;
static lv_obj_t *s_detail;
static lv_obj_t *s_footer;
static lv_obj_t *s_soc;
static lv_obj_t *s_dot;
static lv_obj_t *s_local_pet;
static lv_obj_t *s_peer_pet;
static lv_obj_t *s_icon;
static lv_obj_t *s_idle_mark;
static lv_obj_t *s_trail_blocks[12];
static lv_timer_t *s_timer;
static bool s_breath_up;
static uint8_t s_encounter_phase;
static uint8_t s_animation_divider;

static uint32_t now_ms(void)
{
    return (uint32_t)(xTaskGetTickCount() * portTICK_PERIOD_MS);
}

static void set_idle_power_mode(bool enabled)
{
    esp_pm_config_t config = {
        .max_freq_mhz = CONFIG_ESP_DEFAULT_CPU_FREQ_MHZ,
        .min_freq_mhz = enabled ? 40 : CONFIG_ESP_DEFAULT_CPU_FREQ_MHZ,
        .light_sleep_enable = enabled,
    };
    esp_err_t err = esp_pm_configure(&config);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "automatic light sleep %s failed: %s",
                 enabled ? "enable" : "disable", esp_err_to_name(err));
    } else {
        ESP_LOGI(TAG, "automatic light sleep %s",
                 enabled ? "enabled for radio idle" : "disabled");
    }
}

static uint16_t random_nonzero_id(void)
{
    uint16_t id = 0;
    while (id == 0) id = (uint16_t)esp_random();
    return id;
}

static void place_db_reset(void)
{
    memset(&s_db, 0, sizeof(s_db));
    s_db.magic = PLACE_DB_MAGIC;
    s_db.version = PLACE_DB_VERSION;
}

static void social_reset(void)
{
    memset(&s_social, 0, sizeof(s_social));
    s_social.magic = SOCIAL_MAGIC;
    s_social.version = SOCIAL_VERSION;
    s_social.size = sizeof(s_social);
    s_social.anonymous_id = random_nonzero_id();
    s_social.tribe_code = DEFAULT_TRIBE_CODE;
}

static bool place_db_valid(const place_db_t *places)
{
    if (places->magic != PLACE_DB_MAGIC ||
        places->version != PLACE_DB_VERSION ||
        places->count > PLACE_MAX_COUNT) {
        return false;
    }
    for (uint16_t i = 0; i < places->count; i++) {
        if (places->places[i].fp.count > PLACE_FP_BSSID_COUNT) return false;
    }
    return true;
}

static void social_sanitize(void)
{
    if (s_social.anonymous_id == 0) s_social.anonymous_id = random_nonzero_id();
    if (s_social.signal_code >= PASSPORT_SIGNAL_COUNT) s_social.signal_code = 0;
    if (s_social.icon_index >= PASSPORT_ICON_COUNT) s_social.icon_index = 0;
    if (s_social.regulars.count > PASSPORT_REGULAR_MAX) s_social.regulars.count = 0;
    if (s_social.memorials.count > PASSPORT_MEMORIAL_MAX) s_social.memorials.count = 0;
    for (uint8_t i = 0; i < PLACE_MAX_COUNT; i++) {
        if (s_social.place_meta[i].icon >= 6) s_social.place_meta[i].icon = 5;
    }
}

static void storage_load(void)
{
    esp_err_t err = nvs_flash_init();
    if (err != ESP_OK ||
        nvs_open(NVS_NAMESPACE, NVS_READWRITE, &s_nvs) != ESP_OK) {
        s_nvs_ok = false;
        place_db_reset();
        social_reset();
        ESP_LOGE(TAG, "NVS unavailable; running with volatile state");
        return;
    }
    s_nvs_ok = true;

    place_db_t places;
    size_t len = sizeof(places);
    err = nvs_get_blob(s_nvs, NVS_KEY_PLACES, &places, &len);
    if (err == ESP_OK && len == sizeof(places) && place_db_valid(&places)) {
        s_db = places;
        for (uint16_t i = 0; i < s_db.count; i++) {
            s_db.places[i].name[PLACE_NAME_MAX - 1] = '\0';
        }
    } else {
        place_db_reset();
    }

    social_state_t social;
    len = sizeof(social);
    err = nvs_get_blob(s_nvs, NVS_KEY_SOCIAL, &social, &len);
    if (err == ESP_OK && len == sizeof(social) &&
        social.magic == SOCIAL_MAGIC &&
        social.version == SOCIAL_VERSION &&
        social.size == sizeof(social)) {
        s_social = social;
    } else {
        social_reset();
        s_dirty = true;
    }
    social_sanitize();
    s_social.pet_stage =
        passport_pet_stage_update(s_social.pet_stage, (uint8_t)s_db.count);
    ESP_LOGI(TAG, "loaded places=%u regulars=%u memorials=%u nvs_raw=%u bytes",
             (unsigned)s_db.count, (unsigned)s_social.regulars.count,
             (unsigned)s_social.memorials.count,
             (unsigned)(sizeof(s_db) + sizeof(s_social)));
}

static void storage_flush(void)
{
    if (!s_nvs_ok || !s_dirty) return;
    esp_err_t a = nvs_set_blob(s_nvs, NVS_KEY_PLACES, &s_db, sizeof(s_db));
    esp_err_t b = nvs_set_blob(s_nvs, NVS_KEY_SOCIAL, &s_social, sizeof(s_social));
    esp_err_t c = (a == ESP_OK && b == ESP_OK) ? nvs_commit(s_nvs) : ESP_FAIL;
    if (a == ESP_OK && b == ESP_OK && c == ESP_OK) {
        s_dirty = false;
        const unsigned feature_entries =
            1u + NVS_BLOB_ENTRY_COUNT(sizeof(s_db)) +
            NVS_BLOB_ENTRY_COUNT(sizeof(s_social));
        nvs_stats_t stats;
        if (nvs_get_stats(NULL, &stats) == ESP_OK) {
            ESP_LOGI(TAG,
                     "NVS feature raw=%u allocated=%u bytes (%u entries); "
                     "partition_used=%u bytes",
                     (unsigned)(sizeof(s_db) + sizeof(s_social)),
                     feature_entries * NVS_ENTRY_BYTES, feature_entries,
                     (unsigned)(stats.used_entries * NVS_ENTRY_BYTES));
        }
    } else {
        ESP_LOGE(TAG, "NVS flush failed: %s/%s/%s",
                 esp_err_to_name(a), esp_err_to_name(b), esp_err_to_name(c));
    }
}

static void heap_report(const char *stage, size_t before_min, size_t before_largest)
{
    size_t min_now = heap_caps_get_minimum_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    size_t largest = heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    ESP_LOGI(TAG, "HEAP %-8s min=%u delta=%d largest=%u delta=%d",
             stage, (unsigned)min_now, (int)min_now - (int)before_min,
             (unsigned)largest, (int)largest - (int)before_largest);
}

static void append_trail(uint8_t place_index)
{
    if (s_trail_count > 0 &&
        s_trail[s_trail_count - 1].place_index == place_index) return;
    if (s_trail_count == TRAIL_MAX) {
        memmove(&s_trail[0], &s_trail[1],
                (TRAIL_MAX - 1) * sizeof(s_trail[0]));
        s_trail_count--;
    }
    s_trail[s_trail_count].place_index = place_index;
    s_trail[s_trail_count].icon = s_social.place_meta[place_index].icon;
    s_trail_count++;
}

static void account_current_place(uint32_t current_ms)
{
    if (s_current_place < 0 || s_current_place >= (int)s_db.count ||
        s_last_account_ms == 0) {
        s_last_account_ms = current_ms;
        return;
    }
    uint32_t elapsed = (uint32_t)(current_ms - s_last_account_ms) / 1000u;
    if (elapsed > 0) {
        uint32_t *stay = &s_social.place_meta[s_current_place].stay_seconds;
        *stay = UINT32_MAX - *stay < elapsed ? UINT32_MAX : *stay + elapsed;
        s_social.runtime_without_new_place_seconds =
            UINT32_MAX - s_social.runtime_without_new_place_seconds < elapsed
                ? UINT32_MAX
                : s_social.runtime_without_new_place_seconds + elapsed;
        s_dirty = true;
        s_last_account_ms = current_ms;
    }
}

static int archive_index_for_rank(uint8_t rank)
{
    if (s_db.count == 0) return -1;
    uint16_t used = 0;
    int chosen = -1;
    for (uint8_t r = 0; r <= rank % s_db.count; r++) {
        chosen = -1;
        for (uint8_t i = 0; i < s_db.count; i++) {
            if ((used & (1u << i)) != 0) continue;
            if (chosen < 0 ||
                s_social.place_meta[i].stay_seconds >
                    s_social.place_meta[chosen].stay_seconds) {
                chosen = i;
            }
        }
        if (chosen >= 0) used |= (uint16_t)(1u << chosen);
    }
    return chosen;
}

static passport_pet_stats_t pet_stats(void)
{
    passport_pet_stats_t stats;
    memset(&stats, 0, sizeof(stats));
    stats.place_count = (uint8_t)s_db.count;
    stats.same_tribe_encounters = s_social.same_tribe_encounters;
    stats.crowd_events = s_social.crowd_events;
    for (uint8_t i = 0; i < s_db.count; i++) {
        stats.total_visits += s_db.places[i].visits;
        stats.total_stay_seconds += s_social.place_meta[i].stay_seconds;
        if (s_social.place_meta[i].stay_seconds > stats.longest_place_seconds) {
            stats.longest_place_seconds = s_social.place_meta[i].stay_seconds;
        }
    }
    return stats;
}

static void hide_optional_locked(void)
{
    lv_obj_add_flag(s_local_pet, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(s_peer_pet, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(s_icon, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(s_idle_mark, LV_OBJ_FLAG_HIDDEN);
    for (size_t i = 0; i < 12; i++) lv_obj_add_flag(s_trail_blocks[i], LV_OBJ_FLAG_HIDDEN);
}

static void render_name_locked(void)
{
    lv_label_set_text(s_title, "NAME NEW PLACE");
    lv_label_set_text_fmt(s_status, "<  %s  >", PLACE_NAMES[s_name_choice]);
    lv_label_set_text(s_detail, "UP/DOWN: choose preset\nOK: confirm");
    lv_label_set_text(s_footer, "No date or location leaves device");
    hide_optional_locked();
}

static void render_view_locked(void)
{
    if (!s_scr) return;
    if (s_pending_name >= 0) {
        render_name_locked();
        return;
    }

    hide_optional_locked();
    lv_label_set_text(s_footer, "UP/DOWN: view   OK: action");
    lv_label_set_text_fmt(s_dot, "%u", (unsigned)s_other_tribe_count);
    if (s_other_tribe_count) lv_obj_clear_flag(s_dot, LV_OBJ_FLAG_HIDDEN);
    else lv_obj_add_flag(s_dot, LV_OBJ_FLAG_HIDDEN);

    switch (s_view) {
    case VIEW_LIVE:
        lv_label_set_text(s_title, "PLACES + NEARBY");
        lv_label_set_text(s_status, s_live_status);
        lv_label_set_text(s_detail, s_live_detail);
        if (passport_quiet_reply_prompt(&s_quiet_reply, now_ms()) != 0) {
            lv_label_set_text(s_footer, "OK: stay with them");
        }
        if (s_have_peer && (int32_t)(s_encounter_until_ms - now_ms()) > 0) {
            uint8_t local_stage = s_social.pet_stage < 6 ? s_social.pet_stage : 5;
            uint8_t peer_stage = s_last_peer.pet_stage < 6 ? s_last_peer.pet_stage : 5;
            lv_image_set_src(s_local_pet, pet_form_sprites[local_stage]);
            lv_image_set_src(s_peer_pet, pet_form_sprites[peer_stage]);
            lv_obj_set_pos(s_local_pet, 28, 78);
            lv_obj_set_pos(s_peer_pet, 126, 78);
            lv_obj_clear_flag(s_local_pet, LV_OBJ_FLAG_HIDDEN);
            lv_obj_clear_flag(s_peer_pet, LV_OBJ_FLAG_HIDDEN);
        }
        break;
    case VIEW_ARCHIVE: {
        lv_label_set_text(s_title, "PLACE ARCHIVE");
        int idx = archive_index_for_rank(s_archive_rank);
        if (idx < 0) {
            lv_label_set_text(s_status, "NO SAVED PLACE");
            lv_label_set_text(s_detail, "Run a scan first");
        } else {
            uint32_t seconds = s_social.place_meta[idx].stay_seconds;
            lv_label_set_text_fmt(s_status, "#%d  %s", idx + 1, s_db.places[idx].name);
            lv_label_set_text_fmt(s_detail,
                                  "STAY %" PRIu32 "m  VISITS %u\nsorted by total stay",
                                  seconds / 60u, (unsigned)s_db.places[idx].visits);
        }
        break;
    }
    case VIEW_TRAIL:
        lv_label_set_text(s_title, "THIS BOOT TRAIL");
        lv_label_set_text_fmt(s_status, "%u transitions", (unsigned)s_trail_count);
        lv_label_set_text(s_detail, "Oldest                         Newest");
        for (uint8_t i = 0; i < s_trail_count && i < 12; i++) {
            uint8_t source = s_trail_count > 12 ? (uint8_t)(s_trail_count - 12 + i) : i;
            uint8_t icon = s_trail[source].icon;
            lv_obj_set_style_bg_color(s_trail_blocks[i],
                                      lv_color_hex(PLACE_COLORS[icon % 6]), 0);
            lv_obj_clear_flag(s_trail_blocks[i], LV_OBJ_FLAG_HIDDEN);
        }
        break;
    case VIEW_SIGNAL:
        lv_label_set_text(s_title, "SIGNAL");
        lv_label_set_text_fmt(s_status, "%u/5  %s",
                              (unsigned)s_social.signal_code + 1,
                              SIGNAL_NAMES[s_social.signal_code]);
        lv_label_set_text(s_detail, "OK: broadcast next signal");
        break;
    case VIEW_ICON:
        lv_label_set_text(s_title, "TRIBE ICON");
        lv_label_set_text(s_status, ICON_NAMES[s_social.icon_index]);
        lv_label_set_text(s_detail,
                          s_social.icon_index == PASSPORT_QUIET_ICON
                              ? "Quiet mood: no sound/animation"
                              : "OK: choose next icon");
        lv_image_set_src(s_icon,
                         tribe_icon_get(&tribe_icon_pack_active,
                                        s_social.icon_index));
        lv_obj_align(s_icon, LV_ALIGN_BOTTOM_MID, 0, -8);
        lv_obj_clear_flag(s_icon, LV_OBJ_FLAG_HIDDEN);
        break;
    case VIEW_STEALTH:
        lv_label_set_text(s_title, "STEALTH");
        lv_label_set_text(s_status, s_social.stealth ? "ON: SCAN ONLY" : "OFF: VISIBLE");
        lv_label_set_text(s_detail, "OK: toggle (saved in NVS)");
        break;
    case VIEW_REGULARS:
        lv_label_set_text(s_title, "REGULARS");
        lv_label_set_text(s_status, s_social.regulars_enabled ? "OPTED IN" : "OFF BY DEFAULT");
        if (!s_social.regulars_enabled) {
            lv_label_set_text(s_detail, "No profiles are stored\nOK: opt in");
        } else if (s_social.regulars.count == 0) {
            lv_label_set_text(s_detail, "0/32 profiles\nOK: turn off");
        } else {
            passport_regular_t *regular =
                &s_social.regulars.records[s_regular_cursor % s_social.regulars.count];
            const char *place_name =
                regular->favorite_place < s_db.count
                    ? s_db.places[regular->favorite_place].name
                    : "UNKNOWN";
            lv_label_set_text_fmt(s_detail,
                                  "#%04X met %u times\nmostly %s: %u / OK next",
                                  regular->anonymous_id,
                                  (unsigned)regular->encounters, place_name,
                                  (unsigned)regular->favorite_place_count);
        }
        break;
    case VIEW_PET: {
        passport_pet_stats_t stats = pet_stats();
        uint8_t trait = passport_pet_trait(&stats);
        bool idle = passport_pet_is_idle(s_social.runtime_without_new_place_seconds, 0);
        lv_label_set_text(s_title, "BUBU");
        lv_label_set_text_fmt(s_status, "%s  %u/16 places",
                              PET_FORM_NAMES[s_social.pet_stage], (unsigned)s_db.count);
        if (trait == PASSPORT_PET_TRAIT_NONE) {
            lv_label_set_text(s_detail, idle ? "DAYDREAMING... no progress lost" : "Keep discovering places");
        } else {
            lv_label_set_text_fmt(s_detail, "%s%s",
                                  PET_TRAIT_NAMES[trait],
                                  idle ? " / DAYDREAMING" : "");
        }
        lv_image_set_src(s_local_pet, pet_form_sprites[s_social.pet_stage]);
        lv_obj_set_pos(s_local_pet, 72, 76);
        lv_obj_clear_flag(s_local_pet, LV_OBJ_FLAG_HIDDEN);
        if (trait != PASSPORT_PET_TRAIT_NONE) {
            lv_image_set_src(s_icon, pet_acc_sprites[trait]);
            lv_obj_set_pos(s_icon,
                           72 + pet_form_anchors[s_social.pet_stage].x,
                           76 + pet_form_anchors[s_social.pet_stage].y);
            lv_obj_clear_flag(s_icon, LV_OBJ_FLAG_HIDDEN);
        }
        if (idle) lv_obj_clear_flag(s_idle_mark, LV_OBJ_FLAG_HIDDEN);
        break;
    }
    case VIEW_RESET_ID:
        lv_label_set_text(s_title, "ANONYMOUS ID");
        lv_label_set_text_fmt(s_status, "%04X", s_social.anonymous_id);
        lv_label_set_text(s_detail, "OK: generate a new ID\nBLE MAC is never used");
        break;
    default:
        break;
    }
}

static void render_view(void)
{
    if (!bsp_lvgl_lock(300)) return;
    render_view_locked();
    bsp_lvgl_unlock();
}

static void update_battery_from_worker(void)
{
    int soc = bsp_battery_soc();
    if (!bsp_lvgl_lock(300)) return;
    if (s_soc) {
        if (soc >= 0) lv_label_set_text_fmt(s_soc, "%d%%", soc);
        else lv_label_set_text(s_soc, "");
    }
    bsp_lvgl_unlock();
}

static void show_live(const char *status, const char *detail)
{
    snprintf(s_live_status, sizeof(s_live_status), "%s", status);
    snprintf(s_live_detail, sizeof(s_live_detail), "%s", detail);
    if (s_view == VIEW_LIVE) render_view();
}

static void play_encounter_tone(void)
{
    int16_t pcm[800];
    for (size_t i = 0; i < sizeof(pcm) / sizeof(pcm[0]); i++) {
        pcm[i] = ((i / 20u) & 1u) ? 5500 : -5500;
    }
    if (bsp_audio_set_format(16000, 16, 1) == ESP_OK) {
        bsp_audio_set_volume(55);
        bsp_audio_write(pcm, sizeof(pcm));
    }
}

static void finish_crowd(uint32_t current_ms);

static void process_ble_result(const passport_ble_result_t *result,
                               uint32_t current_ms)
{
    s_other_tribe_count = result->other_tribe_count;
    bool crowd = result->crowd.unique_count > PASSPORT_CROWD_THRESHOLD;
    if (crowd) {
        if (!s_crowd_active) {
            s_crowd_active = true;
            s_crowd_started_ms = current_ms;
        }
        if (result->crowd.unique_count >= s_crowd_peak.unique_count) {
            s_crowd_peak = result->crowd;
        }
        snprintf(s_live_status, sizeof(s_live_status), "GATHERING: %u",
                 (unsigned)result->crowd.unique_count);
        snprintf(s_live_detail, sizeof(s_live_detail),
                 "0:%u 1:%u 2:%u 3:%u\n4:%u 5:%u 6:%u 7:%u",
                 (unsigned)result->crowd.icon_counts[0],
                 (unsigned)result->crowd.icon_counts[1],
                 (unsigned)result->crowd.icon_counts[2],
                 (unsigned)result->crowd.icon_counts[3],
                 (unsigned)result->crowd.icon_counts[4],
                 (unsigned)result->crowd.icon_counts[5],
                 (unsigned)result->crowd.icon_counts[6],
                 (unsigned)result->crowd.icon_counts[7]);
        s_dirty = true;
        return;
    }

    finish_crowd(current_ms);

    for (uint8_t i = 0; i < result->peer_count; i++) {
        const passport_payload_t *peer = &result->peers[i];
        if (peer->tribe_code == s_social.tribe_code &&
            passport_quiet_reply_matches(s_social.anonymous_id, peer)) {
            s_quiet_message = true;
            s_have_peer = false;
            s_quiet_reply.prompt_peer_id = 0;
            s_quiet_reply.prompt_until_ms = 0;
            s_encounter_until_ms = current_ms + 4000u;
            snprintf(s_live_status, sizeof(s_live_status), "SOMEONE STAYED");
            snprintf(s_live_detail, sizeof(s_live_detail),
                     "A nearby wmls stayed with you");
            return;
        }
    }

    for (uint8_t i = 0; i < result->peer_count; i++) {
        const passport_payload_t *peer = &result->peers[i];
        if (peer->quiet_reply_to_id != 0 ||
            passport_feedback_route(s_social.tribe_code, peer) !=
                PASSPORT_FEEDBACK_QUIET) {
            continue;
        }
        if (!passport_quiet_reply_observe(&s_quiet_reply,
                                          peer->anonymous_id, current_ms)) {
            continue;
        }
        s_last_peer = *peer;
        if (s_social.same_tribe_encounters != UINT32_MAX) {
            s_social.same_tribe_encounters++;
        }
        if (s_social.regulars_enabled && s_current_place >= 0) {
            passport_regular_observe(&s_social.regulars, peer->anonymous_id,
                                     (uint8_t)s_current_place, current_ms);
        }
        s_quiet_message = true;
        s_have_peer = false;
        snprintf(s_live_status, sizeof(s_live_status), "QUIET SIGNAL");
        snprintf(s_live_detail, sizeof(s_live_detail),
                 "Quiet signal from #%04X\nOK: stay with them",
                 peer->anonymous_id);
        s_dirty = true;
        return;
    }

    for (uint8_t i = 0; i < result->peer_count; i++) {
        const passport_payload_t *peer = &result->peers[i];
        if (peer->quiet_reply_to_id != 0 ||
            passport_feedback_route(s_social.tribe_code, peer) ==
                PASSPORT_FEEDBACK_QUIET) {
            continue;
        }
        s_last_peer = *peer;
        if (s_social.same_tribe_encounters != UINT32_MAX) {
            s_social.same_tribe_encounters++;
        }
        if (s_social.regulars_enabled && s_current_place >= 0) {
            passport_regular_observe(&s_social.regulars, peer->anonymous_id,
                                     (uint8_t)s_current_place, current_ms);
        }
        s_quiet_message = false;
        s_have_peer = true;
        s_encounter_until_ms = current_ms + 4000u;
        s_encounter_phase = 0;
        snprintf(s_live_status, sizeof(s_live_status), "SAME TRIBE NEARBY");
        snprintf(s_live_detail, sizeof(s_live_detail),
                 "Met #%04X / %s / %s",
                 peer->anonymous_id, ICON_NAMES[peer->icon_index],
                 SIGNAL_NAMES[peer->signal_code]);
        play_encounter_tone();
        s_dirty = true;
    }
}

static void finish_crowd(uint32_t current_ms)
{
    if (!s_crowd_active) return;
    passport_memorial_t memorial;
    memset(&memorial, 0, sizeof(memorial));
    memorial.unique_count = s_crowd_peak.unique_count;
    memorial.place_hash = s_current_place_hash;
    memorial.duration_seconds = (current_ms - s_crowd_started_ms) / 1000u;
    memcpy(memorial.icon_counts, s_crowd_peak.icon_counts,
           sizeof(memorial.icon_counts));
    passport_memorial_add(&s_social.memorials, &memorial);
    if (s_social.crowd_events != UINT8_MAX) s_social.crowd_events++;
    s_crowd_active = false;
    s_dirty = true;
}

static uint32_t wifi_stage(void)
{
    size_t min_before =
        heap_caps_get_minimum_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    size_t largest_before =
        heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    uint32_t started = now_ms();
    s_scanning = true;
    show_live("WI-FI SCAN", "Building location fingerprint");

    bsp_wifi_ap_t aps[BSP_WIFI_SCAN_MAX];
    size_t n = 0;
    esp_err_t err = bsp_wifi_scan_once(aps, BSP_WIFI_SCAN_MAX, &n);
    s_scanning = false;
    heap_report("wifi-off", min_before, largest_before);
    if (!s_active) return CYCLE_MS;
    if (err != ESP_OK) {
        show_live("WI-FI FAILED", esp_err_to_name(err));
        return CYCLE_MS;
    }

    place_ap_t input[BSP_WIFI_SCAN_MAX];
    for (size_t i = 0; i < n; i++) {
        memcpy(input[i].bssid, aps[i].bssid, 6);
        input[i].rssi = aps[i].rssi;
    }
    place_fingerprint_t fp;
    place_fp_build(&fp, input, n);
    if (fp.count == 0) {
        show_live("NO ACCESS POINT", "Move and try again");
        return CYCLE_MS;
    }

    uint32_t current_ms = now_ms();
    account_current_place(current_ms);
    int score = -1;
    int index = place_db_find(&s_db, &fp, &score);
    place_obs_t observation;
    if (index >= 0) {
        s_match_session.new_streak = 0;
        observation = PLACE_OBS_MATCH;
    } else if (s_db.count == 0) {
        observation = PLACE_OBS_NEW_COMMIT;
    } else {
        observation = place_fp_observe(&s_match_session, score);
    }

    bool newly_created = false;
    bool checking = false;
    if (observation == PLACE_OBS_NEW_COMMIT && s_db.count < PLACE_MAX_COUNT) {
        index = s_db.count++;
        memset(&s_db.places[index], 0, sizeof(s_db.places[index]));
        snprintf(s_db.places[index].name, sizeof(s_db.places[index].name),
                 "PLACE %02d", index + 1);
        s_db.places[index].visits = 1;
        s_db.places[index].fp = fp;
        s_social.place_meta[index].icon = 5;
        s_social.runtime_without_new_place_seconds = 0;
        s_social.pet_stage =
            passport_pet_stage_update(s_social.pet_stage, (uint8_t)s_db.count);
        s_pending_name = index;
        s_name_choice = 0;
        newly_created = true;
        s_dirty = true;
        storage_flush();
    } else if (observation == PLACE_OBS_UNSURE ||
               observation == PLACE_OBS_NEW_PENDING) {
        checking = true;
    }

    if (index >= 0) {
        if (s_current_place != index) {
            if (!newly_created && s_db.places[index].visits != UINT16_MAX) {
                s_db.places[index].visits++;
            }
            s_current_place = index;
            append_trail((uint8_t)index);
            s_dirty = true;
        }
        s_last_account_ms = current_ms;
        s_current_place_hash = passport_place_hash(fp.bssid, fp.count);
        s_last_score = score;
        char detail[64];
        snprintf(detail, sizeof(detail), "%s / MATCH %d%% / %u saved",
                 s_db.places[index].name, score < 0 ? 100 : score / 10,
                 (unsigned)s_db.count);
        show_live(newly_created ? "NEW PLACE" : "KNOWN PLACE", detail);
    } else if (checking) {
        s_last_score = score;
        char detail[64];
        snprintf(detail, sizeof(detail), "CHECKING %d%% / rescan in 4s",
                 score < 0 ? 0 : score / 10);
        show_live("VERIFYING PLACE", detail);
    } else {
        show_live("PLACE LIBRARY FULL", "16/16 saved");
    }

    ESP_LOGI(TAG, "Wi-Fi stage=%ums APs=%u fp=%u idx=%d score=%d hash=%02x",
             (unsigned)(now_ms() - started), (unsigned)n, (unsigned)fp.count,
             index, score, s_current_place_hash);
    return checking ? QUICK_RESCAN_MS : CYCLE_MS;
}

static void ble_stage(void)
{
    size_t min_before =
        heap_caps_get_minimum_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    size_t largest_before =
        heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    uint32_t started = now_ms();
    show_live("BLE NEARBY", s_social.stealth ? "Stealth: scan only" : "Advertise + scan");

    uint32_t current_ms = now_ms();
    uint16_t quiet_reply_target =
        s_social.stealth
            ? 0
            : passport_quiet_reply_outbound(&s_quiet_reply, current_ms);
    bool quiet_reply_pending = quiet_reply_target != 0;
    passport_payload_t local = {
        .anonymous_id = s_social.anonymous_id,
        .signal_code = s_social.signal_code,
        .place_hash = s_current_place_hash,
        .tribe_code = s_social.tribe_code,
        .icon_index = s_social.icon_index,
        .pet_stage = s_social.pet_stage,
        .quiet_reply_to_id = quiet_reply_target,
    };
    passport_ble_result_t result;
    esp_err_t err = passport_ble_window(&local, s_social.stealth != 0, &result);
    heap_report("ble-off", min_before, largest_before);
    if (!s_active) return;
    if (err == ESP_OK) {
        process_ble_result(&result, now_ms());
        if (quiet_reply_pending) {
            passport_quiet_reply_mark_sent(&s_quiet_reply);
            snprintf(s_live_status, sizeof(s_live_status), "QUIET REPLY SENT");
            snprintf(s_live_detail, sizeof(s_live_detail),
                     "They will see: someone stayed");
        }
        update_battery_from_worker();
        ESP_LOGI(TAG, "BLE stage=%ums same=%u other=%u crowd=%u",
                 (unsigned)(now_ms() - started), (unsigned)result.peer_count,
                 (unsigned)result.other_tribe_count,
                 (unsigned)result.crowd.unique_count);
    } else {
        show_live("BLE FAILED", esp_err_to_name(err));
    }
}

static uint32_t run_radio_cycle(void)
{
    uint32_t cycle_started = now_ms();
    uint32_t next_delay = wifi_stage();
    if (!s_active || next_delay == QUICK_RESCAN_MS) return next_delay;

    show_live("RF GUARD", "Wi-Fi is off / waiting 1s");
    vTaskDelay(pdMS_TO_TICKS(RF_GUARD_MS));
    if (s_active) ble_stage();

    uint32_t elapsed = now_ms() - cycle_started;
    ESP_LOGI(TAG, "cycle active=%ums idle_target=%ums", (unsigned)elapsed,
             elapsed < CYCLE_MS ? (unsigned)(CYCLE_MS - elapsed) : 0u);
    return elapsed < CYCLE_MS ? CYCLE_MS - elapsed : 1000u;
}

static void apply_command(const command_t *command)
{
    switch (command->type) {
    case CMD_NAME_PLACE:
        if (s_pending_name >= 0 && s_pending_name < (int)s_db.count &&
            command->value < 6) {
            snprintf(s_db.places[s_pending_name].name,
                     sizeof(s_db.places[s_pending_name].name), "%s",
                     PLACE_NAMES[command->value]);
            s_social.place_meta[s_pending_name].icon = command->value;
            s_pending_name = -1;
            s_dirty = true;
            storage_flush();
        }
        break;
    case CMD_NEXT_ARCHIVE:
        if (s_db.count) s_archive_rank = (uint8_t)((s_archive_rank + 1) % s_db.count);
        break;
    case CMD_NEXT_SIGNAL:
        s_social.signal_code =
            (uint8_t)((s_social.signal_code + 1) % PASSPORT_SIGNAL_COUNT);
        s_dirty = true;
        storage_flush();
        break;
    case CMD_NEXT_ICON:
        s_social.icon_index =
            (uint8_t)((s_social.icon_index + 1) % PASSPORT_ICON_COUNT);
        s_dirty = true;
        storage_flush();
        break;
    case CMD_TOGGLE_STEALTH:
        s_social.stealth = !s_social.stealth;
        s_dirty = true;
        storage_flush();
        break;
    case CMD_TOGGLE_REGULARS:
        s_social.regulars_enabled = !s_social.regulars_enabled;
        s_dirty = true;
        storage_flush();
        break;
    case CMD_NEXT_REGULAR:
        if (s_social.regulars.count) {
            s_regular_cursor =
                (uint8_t)((s_regular_cursor + 1) % s_social.regulars.count);
        }
        break;
    case CMD_RESET_ID:
        s_social.anonymous_id = random_nonzero_id();
        memset(&s_social.regulars, 0, sizeof(s_social.regulars));
        s_dirty = true;
        storage_flush();
        break;
    case CMD_QUIET_REPLY: {
        uint32_t current_ms = now_ms();
        if (s_social.stealth) {
            show_live("STEALTH IS ON", "Turn it off before replying");
        } else if (command->value != 0 &&
                   command->value ==
                       passport_quiet_reply_prompt(&s_quiet_reply,
                                                   current_ms) &&
                   passport_quiet_reply_accept(&s_quiet_reply,
                                               current_ms) != 0) {
            s_next_due = current_ms;
            show_live("SENDING QUIET REPLY", "Stay nearby for a moment");
        }
        break;
    }
    default:
        break;
    }
    render_view();
}

static void worker_task(void *arg)
{
    (void)arg;
    storage_load();
    uint32_t last_flush = now_ms();
    command_t command;
    while (1) {
        uint32_t wait_ms = 500;
        if (s_active && !s_scanning) {
            uint32_t current = now_ms();
            int32_t remaining = (int32_t)(s_next_due - current);
            if (remaining <= 0) wait_ms = 0;
            else if ((uint32_t)remaining < wait_ms) wait_ms = (uint32_t)remaining;
        }

        if (xQueueReceive(s_queue, &command, pdMS_TO_TICKS(wait_ms)) == pdTRUE) {
            esp_err_t wifi_err;
            if (command.type == CMD_ACTIVE) {
                wifi_err = wifi_portal_suspend();
                if (wifi_err != ESP_OK) {
                    ESP_LOGW(TAG, "Persistent Wi-Fi suspend failed: %s",
                             esp_err_to_name(wifi_err));
                }
                s_active = true;
                s_match_session.new_streak = 0;
                s_last_account_ms = now_ms();
                s_next_due = now_ms();
                set_idle_power_mode(true);
                render_view();
            } else if (command.type == CMD_IDLE) {
                account_current_place(now_ms());
                finish_crowd(now_ms());
                s_active = false;
                s_current_place = -1;
                set_idle_power_mode(false);
                storage_flush();
                wifi_err = wifi_portal_resume();
                if (wifi_err != ESP_OK &&
                    wifi_err != ESP_ERR_INVALID_STATE) {
                    ESP_LOGW(TAG, "Persistent Wi-Fi resume failed: %s",
                             esp_err_to_name(wifi_err));
                }
            } else if (command.type == CMD_SCAN && s_active) {
                s_next_due = now_ms();
            } else if (command.type == CMD_RENDER) {
                render_view();
            } else {
                apply_command(&command);
            }
        }

        uint32_t current = now_ms();
        if (s_active && !s_scanning &&
            (int32_t)(current - s_next_due) >= 0) {
            uint32_t delay = run_radio_cycle();
            s_next_due = now_ms() + delay;
            render_view();
        }
        if (s_dirty && (uint32_t)(current - last_flush) >= FLUSH_THROTTLE_MS) {
            storage_flush();
            last_flush = current;
        }
    }
}

static void build_ui(void)
{
    s_scr = ui_pixel_screen_create("PASSPORT");

    s_title = lv_label_create(s_scr);
    lv_obj_set_width(s_title, 216);
    lv_obj_set_style_text_font(s_title, &lv_font_montserrat_14, 0);
    lv_obj_set_style_text_align(s_title, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_style_text_color(s_title, lv_color_hex(UI_INK), 0);
    lv_obj_align(s_title, LV_ALIGN_TOP_MID, 0, 48);

    lv_obj_t *panel = ui_pixel_panel_create(s_scr, 10, 72, 220, 150, UI_PAPER);
    s_status = lv_label_create(panel);
    lv_obj_set_width(s_status, 194);
    lv_obj_set_style_text_font(s_status, &lv_font_montserrat_20, 0);
    lv_obj_set_style_text_align(s_status, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_style_text_color(s_status, lv_color_hex(UI_SKY_DARK), 0);
    lv_obj_align(s_status, LV_ALIGN_TOP_MID, 0, 12);

    s_detail = lv_label_create(panel);
    lv_obj_set_width(s_detail, 194);
    lv_obj_set_style_text_font(s_detail, &lv_font_montserrat_14, 0);
    lv_obj_set_style_text_align(s_detail, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_style_text_color(s_detail, lv_color_hex(UI_INK), 0);
    lv_obj_align(s_detail, LV_ALIGN_TOP_MID, 0, 52);

    s_local_pet = lv_image_create(panel);
    lv_obj_set_pos(s_local_pet, 38, 78);
    s_peer_pet = lv_image_create(panel);
    lv_obj_set_pos(s_peer_pet, 112, 78);
    s_icon = lv_image_create(panel);
    lv_obj_align(s_icon, LV_ALIGN_BOTTOM_MID, 0, -8);
    s_idle_mark = lv_label_create(panel);
    lv_obj_set_style_text_font(s_idle_mark, &lv_font_montserrat_14, 0);
    lv_obj_set_style_text_color(s_idle_mark, lv_color_hex(UI_GRASS_DARK), 0);
    lv_obj_set_pos(s_idle_mark, 88, 66);
    lv_label_set_text(s_idle_mark, "^^  ...");

    for (size_t i = 0; i < 12; i++) {
        s_trail_blocks[i] = lv_obj_create(panel);
        lv_obj_remove_style_all(s_trail_blocks[i]);
        lv_obj_set_size(s_trail_blocks[i], 14, 28);
        lv_obj_set_pos(s_trail_blocks[i], 4 + (int)i * 16, 88);
        lv_obj_set_style_border_width(s_trail_blocks[i], 1, 0);
        lv_obj_set_style_border_color(s_trail_blocks[i], lv_color_hex(UI_INK), 0);
    }

    s_dot = lv_label_create(s_scr);
    lv_obj_set_style_bg_opa(s_dot, LV_OPA_COVER, 0);
    lv_obj_set_style_bg_color(s_dot, lv_color_hex(UI_ORANGE), 0);
    lv_obj_set_style_text_color(s_dot, lv_color_hex(UI_PAPER), 0);
    lv_obj_set_style_radius(s_dot, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_pad_all(s_dot, 4, 0);
    lv_obj_align(s_dot, LV_ALIGN_TOP_LEFT, 8, 26);

    s_soc = lv_label_create(s_scr);
    lv_obj_set_style_text_font(s_soc, &lv_font_montserrat_14, 0);
    lv_obj_set_style_text_color(s_soc, lv_color_hex(UI_INK), 0);
    lv_obj_align(s_soc, LV_ALIGN_TOP_RIGHT, -8, 27);
    lv_label_set_text(s_soc, "");

    s_footer = lv_label_create(s_scr);
    lv_obj_set_width(s_footer, 220);
    lv_obj_set_style_text_font(s_footer, &lv_font_montserrat_14, 0);
    lv_obj_set_style_text_align(s_footer, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_style_text_color(s_footer, lv_color_hex(UI_INK), 0);
    lv_obj_align(s_footer, LV_ALIGN_TOP_MID, 0, 236);

    ui_pixel_mascot_create(s_scr, 101, 272);
    render_view_locked();
}

static void timer_tick(lv_timer_t *timer)
{
    (void)timer;
    if (!s_scr) return;
    s_animation_divider++;
    if (s_view == VIEW_PET && (s_animation_divider % 4u) == 0 &&
        !lv_obj_has_flag(s_local_pet, LV_OBJ_FLAG_HIDDEN)) {
        lv_obj_set_y(s_local_pet, lv_obj_get_y(s_local_pet) + (s_breath_up ? -1 : 1));
        s_breath_up = !s_breath_up;
    }
    if (s_view == VIEW_LIVE && s_have_peer &&
        (int32_t)(s_encounter_until_ms - now_ms()) > 0) {
        if (s_encounter_phase < 8) {
            lv_obj_set_x(s_local_pet, lv_obj_get_x(s_local_pet) + 4);
            lv_obj_set_x(s_peer_pet, lv_obj_get_x(s_peer_pet) - 4);
        } else if (s_encounter_phase < 16) {
            lv_obj_set_x(s_local_pet, lv_obj_get_x(s_local_pet) - 4);
            lv_obj_set_x(s_peer_pet, lv_obj_get_x(s_peer_pet) + 4);
        }
        if (s_encounter_phase < 16) s_encounter_phase++;
    }
    if (s_view == VIEW_LIVE && s_have_peer &&
        (int32_t)(now_ms() - s_encounter_until_ms) >= 0) {
        s_have_peer = false;
        render_view_locked();
    }
    if (s_view == VIEW_LIVE && s_quiet_reply.prompt_peer_id &&
        passport_quiet_reply_prompt(&s_quiet_reply, now_ms()) == 0) {
        s_quiet_reply.prompt_peer_id = 0;
        s_quiet_reply.prompt_until_ms = 0;
        render_view_locked();
    }
}

static void send_command(uint8_t type, uint16_t value)
{
    command_t command = { .type = type, .value = value };
    if (s_queue) xQueueSend(s_queue, &command, 0);
}

void demo_place_enter(void)
{
    if (!s_worker) {
        s_queue = xQueueCreate(12, sizeof(command_t));
    }
    s_view = VIEW_LIVE;
    build_ui();
    s_timer = lv_timer_create(timer_tick, 250, NULL);
    lv_screen_load(s_scr);
    if (!s_worker) xTaskCreate(worker_task, "places", 7168, NULL, 3, &s_worker);
    send_command(CMD_ACTIVE, 0);
}

void demo_place_exit(void)
{
    if (s_timer) {
        lv_timer_delete(s_timer);
        s_timer = NULL;
    }
    send_command(CMD_IDLE, 0);
    if (s_scr) {
        lv_obj_delete(s_scr);
        s_scr = NULL;
    }
    s_title = s_status = s_detail = s_footer = s_soc = s_dot = NULL;
    s_local_pet = s_peer_pet = s_icon = NULL;
    s_idle_mark = NULL;
    memset(s_trail_blocks, 0, sizeof(s_trail_blocks));
}

void demo_place_key(bsp_btn_t button, bsp_btn_ev_t event)
{
    if (event == BSP_BTN_DOUBLE && s_pending_name < 0 &&
        s_view == VIEW_REGULARS) {
        send_command(CMD_TOGGLE_REGULARS, 0);
        return;
    }
    if (event != BSP_BTN_CLICK) return;
    if (s_pending_name >= 0) {
        if (button == BSP_BTN_UP) {
            s_name_choice = (uint8_t)((s_name_choice + 5) % 6);
            render_name_locked();
        } else if (button == BSP_BTN_DOWN) {
            s_name_choice = (uint8_t)((s_name_choice + 1) % 6);
            render_name_locked();
        } else if (button == BSP_BTN_OK) {
            send_command(CMD_NAME_PLACE, s_name_choice);
        }
        return;
    }

    if (button == BSP_BTN_UP || button == BSP_BTN_DOWN) {
        int direction = button == BSP_BTN_DOWN ? 1 : -1;
        s_view = (place_view_t)((s_view + VIEW_COUNT + direction) % VIEW_COUNT);
        send_command(CMD_RENDER, 0);
        return;
    }
    if (button != BSP_BTN_OK) return;

    switch (s_view) {
    case VIEW_LIVE:
        {
            uint16_t peer_id =
                passport_quiet_reply_prompt(&s_quiet_reply, now_ms());
            if (peer_id != 0) {
                send_command(CMD_QUIET_REPLY, peer_id);
            } else {
                send_command(CMD_SCAN, 0);
            }
        }
        break;
    case VIEW_ARCHIVE: send_command(CMD_NEXT_ARCHIVE, 0); break;
    case VIEW_SIGNAL: send_command(CMD_NEXT_SIGNAL, 0); break;
    case VIEW_ICON: send_command(CMD_NEXT_ICON, 0); break;
    case VIEW_STEALTH: send_command(CMD_TOGGLE_STEALTH, 0); break;
    case VIEW_REGULARS:
        send_command(s_social.regulars_enabled && s_social.regulars.count
                         ? CMD_NEXT_REGULAR
                         : CMD_TOGGLE_REGULARS,
                     0);
        break;
    case VIEW_RESET_ID: send_command(CMD_RESET_ID, 0); break;
    default: break;
    }
}
