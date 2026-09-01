#include <stdio.h>
#include <string.h>

#include "passport_pet.h"

static int s_failures;

#define CHECK(cond)                                                        \
    do {                                                                   \
        if (!(cond)) {                                                     \
            printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);         \
            s_failures++;                                                  \
        }                                                                  \
    } while (0)

static void test_stages(void)
{
    const uint8_t counts[] = { 1, 2, 3, 4, 8, 11, 12, 15, 16, 17 };
    const uint8_t expect[] = { 0, 1, 1, 2, 3, 3, 4, 4, 5, 5 };
    for (size_t i = 0; i < sizeof(counts); i++) {
        CHECK(passport_pet_stage_for_places(counts[i]) == expect[i]);
    }
    CHECK(passport_pet_stage_update(4, 2) == 4);
    CHECK(passport_pet_stage_update(2, 16) == 5);
    CHECK(passport_pet_stage_update(99, 4) == 2);
}

static void test_traits(void)
{
    passport_pet_stats_t stats;
    memset(&stats, 0, sizeof(stats));
    stats.place_count = 1;
    stats.total_stay_seconds = 1000;
    stats.longest_place_seconds = 701;
    CHECK(passport_pet_trait(&stats) == PASSPORT_PET_TRAIT_HOMEBODY);

    memset(&stats, 0, sizeof(stats));
    stats.place_count = 6;
    stats.total_visits = 10;
    stats.total_stay_seconds = 10 * 29 * 60;
    CHECK(passport_pet_trait(&stats) == PASSPORT_PET_TRAIT_ROVER);

    memset(&stats, 0, sizeof(stats));
    stats.same_tribe_encounters = 30;
    CHECK(passport_pet_trait(&stats) == PASSPORT_PET_TRAIT_SOCIAL);

    memset(&stats, 0, sizeof(stats));
    stats.place_count = 8;
    stats.same_tribe_encounters = 4;
    CHECK(passport_pet_trait(&stats) == PASSPORT_PET_TRAIT_SOLO);

    stats.crowd_events = 1;
    CHECK(passport_pet_trait(&stats) == PASSPORT_PET_TRAIT_ARENA);

    stats.same_tribe_encounters = 100;
    CHECK(passport_pet_trait(&stats) == PASSPORT_PET_TRAIT_ARENA);
}

static void test_idle(void)
{
    CHECK(!passport_pet_is_idle(PASSPORT_PET_IDLE_AFTER_SECONDS - 1, 0));
    CHECK(passport_pet_is_idle(PASSPORT_PET_IDLE_AFTER_SECONDS, 0));
    CHECK(!passport_pet_is_idle(PASSPORT_PET_IDLE_AFTER_SECONDS + 10,
                                PASSPORT_PET_IDLE_AFTER_SECONDS + 10));

    uint8_t stage = passport_pet_stage_update(3, 8);
    (void)passport_pet_is_idle(PASSPORT_PET_IDLE_AFTER_SECONDS + 1, 0);
    CHECK(stage == 3);
}

int main(void)
{
    test_stages();
    test_traits();
    test_idle();
    if (s_failures) {
        printf("test_passport_pet: %d FAILURES\n", s_failures);
        return 1;
    }
    printf("test_passport_pet: ALL PASS\n");
    return 0;
}
