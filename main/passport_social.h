#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define PASSPORT_COMPANY_ID              0xF017u
#define PASSPORT_PAYLOAD_STAGE1_LEN      4u
#define PASSPORT_PAYLOAD_STAGE2_LEN      7u
#define PASSPORT_PAYLOAD_STAGE3_LEN      8u
#define PASSPORT_PAYLOAD_QUIET_REPLY_LEN 10u
#define PASSPORT_SIGNAL_COUNT            5u
#define PASSPORT_ICON_COUNT              8u
#define PASSPORT_QUIET_ICON              7u
#define PASSPORT_REGULAR_MAX             32u
#define PASSPORT_CROWD_BITMAP_BYTES      128u
#define PASSPORT_CROWD_THRESHOLD         10u
#define PASSPORT_MEMORIAL_MAX            8u
#define PASSPORT_PLACE_NONE              0xffu
#define PASSPORT_QUIET_REPLY_PROMPT_MS     30000u
#define PASSPORT_QUIET_REPLY_SEND_MS      180000u
#define PASSPORT_QUIET_REPLY_SUPPRESS_MS  300000u
#define PASSPORT_QUIET_REPLY_ATTEMPTS          3u

typedef struct {
    uint16_t anonymous_id;
    uint8_t signal_code;
    uint8_t place_hash;
    uint16_t tribe_code;
    uint8_t icon_index;
    uint8_t pet_stage;
    uint16_t quiet_reply_to_id;
} passport_payload_t;

typedef enum {
    PASSPORT_FEEDBACK_IGNORE = 0,
    PASSPORT_FEEDBACK_OTHER_TRIBE,
    PASSPORT_FEEDBACK_ENCOUNTER,
    PASSPORT_FEEDBACK_QUIET,
} passport_feedback_t;

typedef struct {
    uint16_t prompt_peer_id;
    uint16_t outbound_peer_id;
    uint16_t suppressed_peer_id;
    uint8_t outbound_attempts;
    uint8_t reserved;
    uint32_t prompt_until_ms;
    uint32_t outbound_until_ms;
    uint32_t suppressed_until_ms;
} passport_quiet_reply_state_t;

typedef struct {
    uint16_t anonymous_id;
    uint16_t encounters;
    uint8_t favorite_place;
    uint8_t challenger_place;
    uint16_t favorite_place_count;
    uint16_t challenger_place_count;
    uint32_t last_seen_ms;
} passport_regular_t;

typedef struct {
    uint8_t count;
    passport_regular_t records[PASSPORT_REGULAR_MAX];
} passport_regular_db_t;

typedef struct {
    uint8_t bits[PASSPORT_CROWD_BITMAP_BYTES];
    uint16_t unique_count;
    uint16_t icon_counts[PASSPORT_ICON_COUNT];
} passport_crowd_counter_t;

typedef struct {
    uint16_t unique_count;
    uint8_t place_hash;
    uint8_t reserved;
    uint32_t duration_seconds;
    uint16_t icon_counts[PASSPORT_ICON_COUNT];
} passport_memorial_t;

typedef struct {
    uint8_t count;
    passport_memorial_t records[PASSPORT_MEMORIAL_MAX];
} passport_memorial_db_t;

uint8_t passport_place_hash(const uint8_t (*bssids)[6], size_t count);

size_t passport_payload_encode(uint8_t *out, size_t out_size,
                               const passport_payload_t *payload,
                               size_t payload_len);
bool passport_payload_decode(passport_payload_t *out, const uint8_t *data,
                             size_t data_len);
bool passport_mfg_decode(passport_payload_t *out, uint16_t company_id,
                         const uint8_t *data, size_t data_len);

passport_feedback_t passport_feedback_route(uint16_t local_tribe,
                                            const passport_payload_t *peer);
bool passport_quiet_reply_matches(uint16_t local_id,
                                  const passport_payload_t *peer);
bool passport_quiet_reply_observe(passport_quiet_reply_state_t *state,
                                  uint16_t peer_id, uint32_t now_ms);
uint16_t passport_quiet_reply_prompt(
    const passport_quiet_reply_state_t *state, uint32_t now_ms);
uint16_t passport_quiet_reply_accept(passport_quiet_reply_state_t *state,
                                     uint32_t now_ms);
uint16_t passport_quiet_reply_outbound(
    const passport_quiet_reply_state_t *state, uint32_t now_ms);
void passport_quiet_reply_mark_sent(passport_quiet_reply_state_t *state);

int passport_regular_observe(passport_regular_db_t *db, uint16_t anonymous_id,
                             uint8_t place_index, uint32_t now_ms);

void passport_crowd_reset(passport_crowd_counter_t *counter);
bool passport_crowd_observe(passport_crowd_counter_t *counter,
                            uint16_t anonymous_id, uint8_t icon_index);

void passport_memorial_add(passport_memorial_db_t *db,
                           const passport_memorial_t *record);
