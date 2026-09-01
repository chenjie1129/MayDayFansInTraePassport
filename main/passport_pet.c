#include "passport_pet.h"

uint8_t passport_pet_stage_for_places(uint8_t place_count)
{
    if (place_count >= 16) return 5;
    if (place_count >= 12) return 4;
    if (place_count >= 8) return 3;
    if (place_count >= 4) return 2;
    if (place_count >= 2) return 1;
    return 0;
}

uint8_t passport_pet_stage_update(uint8_t stored_stage, uint8_t place_count)
{
    if (stored_stage >= PASSPORT_PET_FORM_COUNT) stored_stage = 0;
    uint8_t derived = passport_pet_stage_for_places(place_count);
    return derived > stored_stage ? derived : stored_stage;
}

uint8_t passport_pet_trait(const passport_pet_stats_t *stats)
{
    if (!stats) return PASSPORT_PET_TRAIT_NONE;

    /*
     * Explicit priority: arena > social > solo > rover > homebody.
     * Rare social achievements win over passive movement characteristics.
     */
    if (stats->crowd_events > 0) return PASSPORT_PET_TRAIT_ARENA;
    if (stats->same_tribe_encounters >= 30) return PASSPORT_PET_TRAIT_SOCIAL;
    if (stats->place_count >= 8 && stats->same_tribe_encounters < 5) {
        return PASSPORT_PET_TRAIT_SOLO;
    }
    if (stats->place_count >= 6 && stats->total_visits > 0 &&
        stats->total_stay_seconds / stats->total_visits < 30u * 60u) {
        return PASSPORT_PET_TRAIT_ROVER;
    }
    if (stats->total_stay_seconds > 0 &&
        (uint64_t)stats->longest_place_seconds * 100u >
            stats->total_stay_seconds * 70u) {
        return PASSPORT_PET_TRAIT_HOMEBODY;
    }
    return PASSPORT_PET_TRAIT_NONE;
}

bool passport_pet_is_idle(uint32_t runtime_seconds,
                          uint32_t last_new_place_seconds)
{
    return (uint32_t)(runtime_seconds - last_new_place_seconds) >=
           PASSPORT_PET_IDLE_AFTER_SECONDS;
}
