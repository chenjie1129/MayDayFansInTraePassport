#pragma once

#include <stdbool.h>
#include <stdint.h>

#define PASSPORT_PET_FORM_COUNT 6u
#define PASSPORT_PET_TRAIT_NONE 0xffu
#define PASSPORT_PET_IDLE_AFTER_SECONDS (72u * 60u * 60u)

typedef enum {
    PASSPORT_PET_TRAIT_HOMEBODY = 0,
    PASSPORT_PET_TRAIT_ROVER,
    PASSPORT_PET_TRAIT_SOCIAL,
    PASSPORT_PET_TRAIT_SOLO,
    PASSPORT_PET_TRAIT_ARENA,
} passport_pet_trait_t;

typedef struct {
    uint8_t place_count;
    uint32_t total_visits;
    uint64_t total_stay_seconds;
    uint32_t longest_place_seconds;
    uint32_t same_tribe_encounters;
    uint8_t crowd_events;
} passport_pet_stats_t;

uint8_t passport_pet_stage_for_places(uint8_t place_count);
uint8_t passport_pet_stage_update(uint8_t stored_stage, uint8_t place_count);
uint8_t passport_pet_trait(const passport_pet_stats_t *stats);
bool passport_pet_is_idle(uint32_t runtime_seconds,
                          uint32_t last_new_place_seconds);
