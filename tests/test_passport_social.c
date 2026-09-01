#include <stdio.h>
#include <string.h>

#include "passport_social.h"

static int s_failures;

#define CHECK(cond)                                                        \
    do {                                                                   \
        if (!(cond)) {                                                     \
            printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);         \
            s_failures++;                                                  \
        }                                                                  \
    } while (0)

static void make_bssid(uint8_t out[6], uint8_t id)
{
    memset(out, 0, 6);
    out[0] = 0x02;
    out[5] = id;
}

static void test_place_hash(void)
{
    uint8_t a[3][6], b[3][6], c[3][6];
    make_bssid(a[0], 1);
    make_bssid(a[1], 2);
    make_bssid(a[2], 3);
    memcpy(b[0], a[2], 6);
    memcpy(b[1], a[0], 6);
    memcpy(b[2], a[1], 6);
    memcpy(c, a, sizeof(c));
    make_bssid(c[2], 9);

    CHECK(passport_place_hash(a, 3) == passport_place_hash(b, 3));
    CHECK(passport_place_hash(a, 3) != passport_place_hash(c, 3));

    unsigned collisions = 0;
    bool seen[256] = { false };
    for (uint8_t i = 1; i < 100; i++) {
        make_bssid(c[0], i);
        make_bssid(c[1], (uint8_t)(i + 41));
        make_bssid(c[2], (uint8_t)(i + 97));
        uint8_t hash = passport_place_hash(c, 3);
        if (seen[hash]) collisions++;
        seen[hash] = true;
    }
    CHECK(collisions < 30);
}

static void test_payloads(void)
{
    passport_payload_t input = {
        .anonymous_id = 0x1234,
        .signal_code = 3,
        .place_hash = 0xa5,
        .tribe_code = 0x4d44,
        .icon_index = 6,
        .pet_stage = 4,
    };
    uint8_t encoded[8];
    const size_t lengths[] = { 4, 7, 8 };
    for (size_t i = 0; i < sizeof(lengths) / sizeof(lengths[0]); i++) {
        passport_payload_t output;
        CHECK(passport_payload_encode(encoded, sizeof(encoded), &input, lengths[i]) ==
              lengths[i]);
        CHECK(passport_payload_decode(&output, encoded, lengths[i]));
        CHECK(output.anonymous_id == input.anonymous_id);
        CHECK(output.signal_code == input.signal_code);
        CHECK(output.place_hash == input.place_hash);
        if (lengths[i] >= 7) {
            CHECK(output.tribe_code == input.tribe_code);
            CHECK(output.icon_index == input.icon_index);
        }
        if (lengths[i] == 8) CHECK(output.pet_stage == input.pet_stage);
    }

    passport_payload_t output;
    CHECK(!passport_mfg_decode(&output, 0xbeef, encoded, 8));
    CHECK(passport_mfg_decode(&output, PASSPORT_COMPANY_ID, encoded, 8));
    encoded[6] = 8;
    CHECK(!passport_payload_decode(&output, encoded, 8));
    encoded[6] = 1;
    encoded[7] = 99;
    CHECK(passport_payload_decode(&output, encoded, 8));
    CHECK(output.pet_stage == 5);
}

static void test_regulars(void)
{
    passport_regular_db_t db;
    memset(&db, 0, sizeof(db));
    CHECK(passport_regular_observe(&db, 0x1001, 2, 10) == 0);
    CHECK(passport_regular_observe(&db, 0x1001, 2, 20) == 0);
    CHECK(passport_regular_observe(&db, 0x1001, 4, 30) == 0);
    CHECK(db.records[0].encounters == 3);
    CHECK(db.records[0].favorite_place == 2);
    CHECK(db.records[0].favorite_place_count == 2);

    CHECK(passport_regular_observe(&db, 0x1002, 4, 40) == 1);
    CHECK(passport_regular_observe(&db, 0x1002, 4, 50) == 1);
    CHECK(db.records[1].favorite_place == 4);
    CHECK(db.records[1].favorite_place_count == 2);

    memset(&db, 0, sizeof(db));
    for (uint16_t i = 0; i < PASSPORT_REGULAR_MAX; i++) {
        CHECK(passport_regular_observe(&db, (uint16_t)(i + 1), 0,
                                       (uint32_t)(100 + i)) == i);
    }
    CHECK(db.count == PASSPORT_REGULAR_MAX);
    CHECK(passport_regular_observe(&db, 0x7777, 1, 9999) == 0);
    CHECK(db.records[0].anonymous_id == 0x7777);
    CHECK(db.records[1].anonymous_id == 2);
}

static void test_feedback_and_crowd(void)
{
    passport_payload_t peer = {
        .anonymous_id = 7,
        .tribe_code = 0x4d44,
        .icon_index = 2,
    };
    CHECK(passport_feedback_route(0x4d44, &peer) == PASSPORT_FEEDBACK_ENCOUNTER);
    peer.icon_index = PASSPORT_QUIET_ICON;
    CHECK(passport_feedback_route(0x4d44, &peer) == PASSPORT_FEEDBACK_QUIET);
    CHECK(passport_feedback_route(0x9999, &peer) == PASSPORT_FEEDBACK_OTHER_TRIBE);

    passport_crowd_counter_t crowd;
    passport_crowd_reset(&crowd);
    for (uint16_t i = 1; i <= 2000; i++) {
        passport_crowd_observe(&crowd, i, (uint8_t)(i & 7u));
    }
    CHECK(sizeof(crowd.bits) == 128);
    CHECK(crowd.unique_count > 800);
    CHECK(crowd.unique_count <= 1024);
    uint16_t before = crowd.unique_count;
    CHECK(!passport_crowd_observe(&crowd, 42, 2));
    CHECK(crowd.unique_count == before);
}

static void test_memorials(void)
{
    passport_memorial_db_t db;
    memset(&db, 0, sizeof(db));
    for (uint16_t i = 1; i <= 10; i++) {
        passport_memorial_t record;
        memset(&record, 0, sizeof(record));
        record.unique_count = i;
        record.place_hash = (uint8_t)i;
        passport_memorial_add(&db, &record);
    }
    CHECK(db.count == PASSPORT_MEMORIAL_MAX);
    CHECK(db.records[0].unique_count == 3);
    CHECK(db.records[7].unique_count == 10);
}

int main(void)
{
    test_place_hash();
    test_payloads();
    test_regulars();
    test_feedback_and_crowd();
    test_memorials();

    if (s_failures) {
        printf("test_passport_social: %d FAILURES\n", s_failures);
        return 1;
    }
    printf("test_passport_social: ALL PASS\n");
    return 0;
}
