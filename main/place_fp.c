// main/place_fp.c —— 离线场所指纹纯逻辑实现(见 place_fp.h,V0.2)。
// 仅使用标准 C,可在主机侧无硬件编译与测试。
#include "place_fp.h"

#include <string.h>

static bool bssid_eq(const uint8_t a[6], const uint8_t b[6])
{
    return memcmp(a, b, 6) == 0;
}

void place_fp_build(place_fingerprint_t *fp, const place_ap_t *aps, size_t n)
{
    fp->count = 0;
    if (!aps || n == 0) return;

    // 选择排序:每轮挑出 RSSI 最强且未入选的 BSSID。
    // n 很小(BSP 侧上限 16),重复扫描的开销可忽略;同 BSSID 去重,
    // RSSI 相同时按列表顺序取先出现的(稳定结果)。
    while (fp->count < PLACE_FP_BSSID_COUNT) {
        int best = -1;
        for (size_t i = 0; i < n; i++) {
            bool used = false;
            for (uint8_t k = 0; k < fp->count; k++) {
                if (bssid_eq(fp->bssid[k], aps[i].bssid)) {
                    used = true;
                    break;
                }
            }
            if (used) continue;
            if (best < 0 || aps[i].rssi > aps[best].rssi) {
                best = (int)i;
            }
        }
        if (best < 0) break;  // 剩余 AP 全部重复,没有新 BSSID 了
        memcpy(fp->bssid[fp->count], aps[best].bssid, 6);
        fp->count++;
    }
}

int place_fp_intersection(const place_fingerprint_t *a, const place_fingerprint_t *b)
{
    int inter = 0;
    for (uint8_t i = 0; i < a->count; i++) {
        for (uint8_t j = 0; j < b->count; j++) {
            if (bssid_eq(a->bssid[i], b->bssid[j])) {
                inter++;
                break;
            }
        }
    }
    return inter;
}

int place_fp_union_count(const place_fingerprint_t *a, const place_fingerprint_t *b)
{
    return (int)a->count + (int)b->count - place_fp_intersection(a, b);
}

int place_fp_jaccard_permille(const place_fingerprint_t *a, const place_fingerprint_t *b)
{
    int inter = place_fp_intersection(a, b);
    int uni   = (int)a->count + (int)b->count - inter;
    if (uni <= 0) return 0;
    return inter * 1000 / uni;
}

int place_fp_overlap_permille(const place_fingerprint_t *a, const place_fingerprint_t *b)
{
    int inter = place_fp_intersection(a, b);
    int min_cnt = (a->count < b->count) ? a->count : b->count;
    if (min_cnt <= 0) return 0;
    return inter * 1000 / min_cnt;
}

bool place_fp_match(const place_fingerprint_t *a, const place_fingerprint_t *b)
{
    int inter = place_fp_intersection(a, b);
    int min_cnt = (a->count < b->count) ? a->count : b->count;
    if (min_cnt <= 0) return false;
    // Overlap = inter/min(|A|,|B|) >= 50%  <=>  inter*1000 >= min*500,纯整数。
    return inter * 1000 >= min_cnt * PLACE_MATCH_PERMILLE;
}

int place_db_find(const place_db_t *db, const place_fingerprint_t *fp,
                  int *score_permille)
{
    int best_idx   = -1;
    int best_score = -1;
    for (uint16_t i = 0; i < db->count && i < PLACE_MAX_COUNT; i++) {
        int score = place_fp_overlap_permille(&db->places[i].fp, fp);
        if (score > best_score) {
            best_score = score;
            best_idx   = (int)i;
        }
    }
    if (best_score < PLACE_MATCH_PERMILLE) {
        if (score_permille) *score_permille = best_score;  // -1 或未达阈值的最高分
        return -1;
    }
    if (score_permille) *score_permille = best_score;
    return best_idx;
}

place_obs_t place_fp_observe(place_session_t *s, int best_score_permille)
{
    // 命中或落入灰区:复位"新地点连击"。灰区可能是同址抖动,绝不向建新点推进。
    if (best_score_permille >= PLACE_MATCH_PERMILLE) {
        s->new_streak = 0;
        return PLACE_OBS_MATCH;
    }
    if (best_score_permille >= PLACE_UNSURE_PERMILLE) {
        s->new_streak = 0;
        return PLACE_OBS_UNSURE;
    }
    // 明确不像(<350‰ 或库为空传 -1):累计连击,连续确认后才允许建新地点。
    if (s->new_streak < 255) s->new_streak++;
    if (s->new_streak >= PLACE_NEW_CONFIRM_SCANS) {
        s->new_streak = 0;
        return PLACE_OBS_NEW_COMMIT;
    }
    return PLACE_OBS_NEW_PENDING;
}
