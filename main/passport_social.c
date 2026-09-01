#include "passport_social.h"

#include <string.h>

static void sort_bssid(uint8_t values[3][6], size_t count)
{
    for (size_t i = 1; i < count; i++) {
        uint8_t current[6];
        memcpy(current, values[i], sizeof(current));
        size_t j = i;
        while (j > 0 && memcmp(values[j - 1], current, sizeof(current)) > 0) {
            memcpy(values[j], values[j - 1], sizeof(current));
            j--;
        }
        memcpy(values[j], current, sizeof(current));
    }
}

uint8_t passport_place_hash(const uint8_t (*bssids)[6], size_t count)
{
    if (!bssids || count == 0) return 0;
    if (count > 3) count = 3;

    uint8_t sorted[3][6] = { 0 };
    for (size_t i = 0; i < count; i++) memcpy(sorted[i], bssids[i], 6);
    sort_bssid(sorted, count);

    uint32_t hash = 2166136261u;
    for (size_t i = 0; i < count; i++) {
        for (size_t j = 0; j < 6; j++) {
            hash ^= sorted[i][j];
            hash *= 16777619u;
        }
    }
    hash ^= (uint32_t)count;
    hash *= 16777619u;
    return (uint8_t)((hash >> 24) ^ (hash >> 16) ^ (hash >> 8) ^ hash);
}

size_t passport_payload_encode(uint8_t *out, size_t out_size,
                               const passport_payload_t *payload,
                               size_t payload_len)
{
    if (!out || !payload || out_size < payload_len ||
        (payload_len != PASSPORT_PAYLOAD_STAGE1_LEN &&
         payload_len != PASSPORT_PAYLOAD_STAGE2_LEN &&
         payload_len != PASSPORT_PAYLOAD_STAGE3_LEN)) {
        return 0;
    }
    if (payload->signal_code >= PASSPORT_SIGNAL_COUNT ||
        payload->icon_index >= PASSPORT_ICON_COUNT ||
        payload->pet_stage > 5) {
        return 0;
    }

    out[0] = (uint8_t)(payload->anonymous_id & 0xffu);
    out[1] = (uint8_t)(payload->anonymous_id >> 8);
    out[2] = payload->signal_code;
    out[3] = payload->place_hash;
    if (payload_len >= PASSPORT_PAYLOAD_STAGE2_LEN) {
        out[4] = (uint8_t)(payload->tribe_code & 0xffu);
        out[5] = (uint8_t)(payload->tribe_code >> 8);
        out[6] = payload->icon_index;
    }
    if (payload_len == PASSPORT_PAYLOAD_STAGE3_LEN) out[7] = payload->pet_stage;
    return payload_len;
}

bool passport_payload_decode(passport_payload_t *out, const uint8_t *data,
                             size_t data_len)
{
    if (!out || !data ||
        (data_len != PASSPORT_PAYLOAD_STAGE1_LEN &&
         data_len != PASSPORT_PAYLOAD_STAGE2_LEN &&
         data_len != PASSPORT_PAYLOAD_STAGE3_LEN)) {
        return false;
    }

    memset(out, 0, sizeof(*out));
    out->anonymous_id = (uint16_t)data[0] | ((uint16_t)data[1] << 8);
    out->signal_code = data[2];
    out->place_hash = data[3];
    if (out->signal_code >= PASSPORT_SIGNAL_COUNT) return false;

    if (data_len >= PASSPORT_PAYLOAD_STAGE2_LEN) {
        out->tribe_code = (uint16_t)data[4] | ((uint16_t)data[5] << 8);
        out->icon_index = data[6];
        if (out->icon_index >= PASSPORT_ICON_COUNT) return false;
    }
    if (data_len == PASSPORT_PAYLOAD_STAGE3_LEN) {
        out->pet_stage = data[7];
        if (out->pet_stage > 5) out->pet_stage = 5;
    }
    return out->anonymous_id != 0;
}

bool passport_mfg_decode(passport_payload_t *out, uint16_t company_id,
                         const uint8_t *data, size_t data_len)
{
    if (company_id != PASSPORT_COMPANY_ID) return false;
    return passport_payload_decode(out, data, data_len);
}

passport_feedback_t passport_feedback_route(uint16_t local_tribe,
                                            const passport_payload_t *peer)
{
    if (!peer) return PASSPORT_FEEDBACK_IGNORE;
    if (peer->tribe_code != local_tribe) return PASSPORT_FEEDBACK_OTHER_TRIBE;
    if (peer->icon_index == PASSPORT_QUIET_ICON) return PASSPORT_FEEDBACK_QUIET;
    return PASSPORT_FEEDBACK_ENCOUNTER;
}

static bool older(uint32_t lhs, uint32_t rhs)
{
    return (int32_t)(lhs - rhs) < 0;
}

static void update_favorite(passport_regular_t *record, uint8_t place_index)
{
    if (record->favorite_place == PASSPORT_PLACE_NONE) {
        record->favorite_place = place_index;
        record->favorite_place_count = 1;
        return;
    }
    if (record->favorite_place == place_index) {
        if (record->favorite_place_count != UINT16_MAX) record->favorite_place_count++;
        return;
    }
    if (record->challenger_place != place_index) {
        record->challenger_place = place_index;
        record->challenger_place_count = 1;
    } else if (record->challenger_place_count != UINT16_MAX) {
        record->challenger_place_count++;
    }
    if (record->challenger_place_count > record->favorite_place_count) {
        uint8_t old_place = record->favorite_place;
        uint16_t old_count = record->favorite_place_count;
        record->favorite_place = record->challenger_place;
        record->favorite_place_count = record->challenger_place_count;
        record->challenger_place = old_place;
        record->challenger_place_count = old_count;
    }
}

int passport_regular_observe(passport_regular_db_t *db, uint16_t anonymous_id,
                             uint8_t place_index, uint32_t now_ms)
{
    if (!db || anonymous_id == 0 || place_index == PASSPORT_PLACE_NONE) return -1;

    int index = -1;
    for (uint8_t i = 0; i < db->count && i < PASSPORT_REGULAR_MAX; i++) {
        if (db->records[i].anonymous_id == anonymous_id) {
            index = i;
            break;
        }
    }

    if (index < 0) {
        if (db->count < PASSPORT_REGULAR_MAX) {
            index = db->count++;
        } else {
            index = 0;
            for (uint8_t i = 1; i < PASSPORT_REGULAR_MAX; i++) {
                if (older(db->records[i].last_seen_ms,
                          db->records[index].last_seen_ms)) {
                    index = i;
                }
            }
        }
        memset(&db->records[index], 0, sizeof(db->records[index]));
        db->records[index].anonymous_id = anonymous_id;
        db->records[index].favorite_place = PASSPORT_PLACE_NONE;
        db->records[index].challenger_place = PASSPORT_PLACE_NONE;
    }

    passport_regular_t *record = &db->records[index];
    if (record->encounters != UINT16_MAX) record->encounters++;
    record->last_seen_ms = now_ms;
    update_favorite(record, place_index);
    return index;
}

void passport_crowd_reset(passport_crowd_counter_t *counter)
{
    if (counter) memset(counter, 0, sizeof(*counter));
}

bool passport_crowd_observe(passport_crowd_counter_t *counter,
                            uint16_t anonymous_id, uint8_t icon_index)
{
    if (!counter || anonymous_id == 0 || icon_index >= PASSPORT_ICON_COUNT) return false;
    uint16_t mixed = (uint16_t)(anonymous_id * 40503u);
    uint16_t slot = (uint16_t)((mixed ^ (mixed >> 6)) & 1023u);
    uint8_t mask = (uint8_t)(1u << (slot & 7u));
    uint8_t *byte = &counter->bits[slot >> 3];
    if ((*byte & mask) != 0) return false;
    *byte |= mask;
    if (counter->unique_count != UINT16_MAX) counter->unique_count++;
    if (counter->icon_counts[icon_index] != UINT16_MAX) {
        counter->icon_counts[icon_index]++;
    }
    return true;
}

void passport_memorial_add(passport_memorial_db_t *db,
                           const passport_memorial_t *record)
{
    if (!db || !record) return;
    if (db->count < PASSPORT_MEMORIAL_MAX) {
        db->records[db->count++] = *record;
        return;
    }
    memmove(&db->records[0], &db->records[1],
            (PASSPORT_MEMORIAL_MAX - 1) * sizeof(db->records[0]));
    db->records[PASSPORT_MEMORIAL_MAX - 1] = *record;
}
