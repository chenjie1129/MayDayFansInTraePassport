// tests/test_place_fp.c —— 场所指纹纯逻辑的主机侧单元测试(无硬件依赖,V0.2)。
//
// 编译运行:
//   cc -std=c11 -Wall -Wextra -Werror -Imain tests/test_place_fp.c main/place_fp.c -o /tmp/test_place_fp
//   /tmp/test_place_fp
//
// 覆盖:
//   * 完全相同 / 部分重叠 / 完全不同 / 边界 500‰(Overlap 系数);
//   * V0.1 误判回归:两个 8 元素集合共享 5 个(Jaccard 454‰ 旧版误建新地点,
//     Overlap 625‰ 新版必须判同址);
//   * 指纹构造(RSSI 最强 12 个 + 同 BSSID 去重);
//   * 地点库最佳匹配(Overlap 得分);
//   * 迟滞状态机 place_fp_observe:MATCH/UNSURE/NEW_PENDING/NEW_COMMIT 转换、
//     灰区与命中复位连击、COMMIT 后连击清零、阈值边界。
#include <stdio.h>
#include <string.h>

#include "place_fp.h"

static int s_failures;

#define CHECK(cond)                                                       \
    do {                                                                  \
        if (!(cond)) {                                                    \
            printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);        \
            s_failures++;                                                 \
        }                                                                 \
    } while (0)

// 构造编号 n 的确定性 BSSID(本地管理位置 1,避免与真实 MAC 规则冲突)。
static void bssid(uint8_t out[6], uint8_t n)
{
    memset(out, 0, 6);
    out[0] = 0x02;
    out[5] = n;
}

static place_ap_t make_ap(uint8_t n, int8_t rssi)
{
    place_ap_t ap;
    bssid(ap.bssid, n);
    ap.rssi = rssi;
    return ap;
}

// 用编号列表构造指纹。
static void fp_of(place_fingerprint_t *fp, const uint8_t *ids, uint8_t count)
{
    fp->count = count;
    for (uint8_t i = 0; i < count; i++) bssid(fp->bssid[i], ids[i]);
}

int main(void)
{
    /* ---------- 用例 1:完全相同 -> 同一地点 ---------- */
    {
        place_fingerprint_t a, b;
        uint8_t ids[12] = { 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12 };
        fp_of(&a, ids, 12);
        fp_of(&b, ids, 12);
        CHECK(place_fp_intersection(&a, &b) == 12);
        CHECK(place_fp_union_count(&a, &b) == 12);
        CHECK(place_fp_jaccard_permille(&a, &b) == 1000);
        CHECK(place_fp_overlap_permille(&a, &b) == 1000);
        CHECK(place_fp_match(&a, &b) == true);
    }

    /* ---------- 用例 2:部分重叠(12 中共享 9,Jaccard 0.6 / Overlap 0.75) ---------- */
    {
        place_fingerprint_t a, b;
        uint8_t ia[12] = { 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12 };
        uint8_t ib[12] = { 1, 2, 3, 4, 5, 6, 7, 8, 9, 13, 14, 15 };
        fp_of(&a, ia, 12);
        fp_of(&b, ib, 12);
        CHECK(place_fp_intersection(&a, &b) == 9);
        CHECK(place_fp_union_count(&a, &b) == 15);
        CHECK(place_fp_jaccard_permille(&a, &b) == 600);
        CHECK(place_fp_overlap_permille(&a, &b) == 750);
        CHECK(place_fp_match(&a, &b) == true);
    }

    /* ---------- 用例 3:完全不同(0 重叠) -> 新地点 ---------- */
    {
        place_fingerprint_t a, b;
        uint8_t ia[12] = { 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12 };
        uint8_t ib[12] = { 20, 21, 22, 23, 24, 25, 26, 27, 28, 29, 30, 31 };
        fp_of(&a, ia, 12);
        fp_of(&b, ib, 12);
        CHECK(place_fp_intersection(&a, &b) == 0);
        CHECK(place_fp_jaccard_permille(&a, &b) == 0);
        CHECK(place_fp_overlap_permille(&a, &b) == 0);
        CHECK(place_fp_match(&a, &b) == false);
    }

    /* ---------- 用例 4:Overlap 边界 500‰(12 中恰好共享 6 个) ---------- */
    {
        // 4a. 恰好 50%:共享 6 / min(12,12) = 500‰,阈值语义为">=50%",必须判同址。
        place_fingerprint_t a, b;
        uint8_t ia[12] = { 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12 };
        uint8_t ib[12] = { 1, 2, 3, 4, 5, 6, 13, 14, 15, 16, 17, 18 };
        fp_of(&a, ia, 12);
        fp_of(&b, ib, 12);
        CHECK(place_fp_intersection(&a, &b) == 6);
        CHECK(place_fp_overlap_permille(&a, &b) == 500);
        CHECK(place_fp_match(&a, &b) == true);

        // 4b. 略低于 50%:共享 5 / 12 = 416‰,落在灰区(350~499)——
        //     不判同址,但也绝不允许直接建新地点(由 observe 迟滞兜底)。
        place_fingerprint_t c, d;
        uint8_t ic[12] = { 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12 };
        uint8_t id_[12] = { 1, 2, 3, 4, 5, 13, 14, 15, 16, 17, 18, 19 };
        fp_of(&c, ic, 12);
        fp_of(&d, id_, 12);
        CHECK(place_fp_intersection(&c, &d) == 5);
        CHECK(place_fp_overlap_permille(&c, &d) == 416);
        CHECK(place_fp_match(&c, &d) == false);
    }

    /* ---------- 用例 5:V0.1 误判回归(真机事故现场) ----------
     * 两个 8 元素指纹共享 5 个 BSSID:旧版 Jaccard = 5/11 = 454‰ < 500‰,
     * 设备不动却误建了新地点;V0.2 Overlap = 5/min(8,8) = 625‰,必须判同址。 */
    {
        place_fingerprint_t a, b;
        uint8_t ia[8] = { 1, 2, 3, 4, 5, 6, 7, 8 };
        uint8_t ib[8] = { 4, 5, 6, 7, 8, 9, 10, 11 };
        fp_of(&a, ia, 8);
        fp_of(&b, ib, 8);
        CHECK(place_fp_intersection(&a, &b) == 5);
        CHECK(place_fp_jaccard_permille(&a, &b) == 454);   // 旧指标:误判
        CHECK(place_fp_overlap_permille(&a, &b) == 625);   // 新指标:同址
        CHECK(place_fp_match(&a, &b) == true);
    }

    /* ---------- 用例 6:Overlap 不对称规模(指纹大小不同时分母取 min) ---------- */
    {
        place_fingerprint_t a, b;   // a 存 12 个,b 只扫到 8 个,其中 6 个重合
        uint8_t ia[12] = { 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12 };
        uint8_t ib[8]  = { 1, 2, 3, 4, 5, 6, 20, 21 };
        fp_of(&a, ia, 12);
        fp_of(&b, ib, 8);
        CHECK(place_fp_intersection(&a, &b) == 6);
        CHECK(place_fp_overlap_permille(&a, &b) == 750);   // 6/8,不被 12 拖低
        CHECK(place_fp_match(&a, &b) == true);
    }

    /* ---------- 用例 7:指纹构造 = RSSI 最强 12 个 + 同 BSSID 去重 ---------- */
    {
        place_ap_t aps[16];
        // 14 个不同 AP,编号 1..14,信号 -40..-105;另加 2 个重复 BSSID(更弱)。
        for (uint8_t i = 0; i < 14; i++) aps[i] = make_ap(i + 1, (int8_t)(-40 - i * 5));
        aps[14] = make_ap(3, -110);  // 与 3 号重复且更弱,应被丢弃
        aps[15] = make_ap(9, -115);  // 与 9 号重复且更弱,应被丢弃

        place_fingerprint_t fp;
        place_fp_build(&fp, aps, 16);
        CHECK(fp.count == 12);
        // 最强的 12 个是编号 1..12(RSSI -40..-95);13/14 号(-100/-105)落选。
        for (uint8_t i = 0; i < 12; i++) {
            uint8_t expect[6];
            bssid(expect, i + 1);
            CHECK(memcmp(fp.bssid[i], expect, 6) == 0);
        }

        // 空输入安全。
        place_fingerprint_t empty;
        place_fp_build(&empty, NULL, 0);
        CHECK(empty.count == 0);
        CHECK(place_fp_match(&empty, &fp) == false);
        CHECK(place_fp_overlap_permille(&empty, &fp) == 0);
    }

    /* ---------- 用例 8:地点库最佳匹配(Overlap 得分) ---------- */
    {
        place_db_t db;
        memset(&db, 0, sizeof(db));
        db.count = 2;
        uint8_t ids0[12] = { 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12 };
        uint8_t ids1[12] = { 20, 21, 22, 23, 24, 25, 26, 27, 28, 29, 30, 31 };
        fp_of(&db.places[0].fp, ids0, 12);
        fp_of(&db.places[1].fp, ids1, 12);

        // 与地点 0 共享 10/12 -> Overlap 833‰。
        place_fingerprint_t q;
        uint8_t qids[12] = { 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 40, 41 };
        fp_of(&q, qids, 12);
        int score = -1;
        CHECK(place_db_find(&db, &q, &score) == 0);
        CHECK(score == 833);

        // 完全不相交 -> 无匹配,得分 0。
        uint8_t qids2[12] = { 50, 51, 52, 53, 54, 55, 56, 57, 58, 59, 60, 61 };
        place_fingerprint_t q2;
        fp_of(&q2, qids2, 12);
        CHECK(place_db_find(&db, &q2, &score) == -1);
        CHECK(score == 0);

        // 空库 -> 索引 -1,得分 -1(应用层据此直接建首个地点)。
        place_db_t empty_db;
        memset(&empty_db, 0, sizeof(empty_db));
        CHECK(place_db_find(&empty_db, &q, &score) == -1);
        CHECK(score == -1);
    }

    /* ---------- 用例 9:迟滞状态机 place_fp_observe ---------- */
    {
        place_session_t s;
        memset(&s, 0, sizeof(s));

        // 阈值边界:500 命中;499/350 灰区;349 进入"明确不像"。
        CHECK(place_fp_observe(&s, 500) == PLACE_OBS_MATCH);
        CHECK(s.new_streak == 0);
        CHECK(place_fp_observe(&s, 499) == PLACE_OBS_UNSURE);
        CHECK(s.new_streak == 0);
        CHECK(place_fp_observe(&s, 350) == PLACE_OBS_UNSURE);
        CHECK(s.new_streak == 0);
        CHECK(place_fp_observe(&s, 349) == PLACE_OBS_NEW_PENDING);
        CHECK(s.new_streak == 1);

        // 连续第 2 次明确不像 -> 允许建新地点,连击自动清零。
        CHECK(place_fp_observe(&s, 200) == PLACE_OBS_NEW_COMMIT);
        CHECK(s.new_streak == 0);

        // COMMIT 后再来一次低分:只给 PENDING(不会连续误建)。
        CHECK(place_fp_observe(&s, 200) == PLACE_OBS_NEW_PENDING);
        CHECK(s.new_streak == 1);

        // 灰区复位连击:PENDING 后插一次灰区,再低分必须重新从 PENDING 开始。
        CHECK(place_fp_observe(&s, 400) == PLACE_OBS_UNSURE);
        CHECK(s.new_streak == 0);
        CHECK(place_fp_observe(&s, 100) == PLACE_OBS_NEW_PENDING);
        CHECK(s.new_streak == 1);
        CHECK(place_fp_observe(&s, 100) == PLACE_OBS_NEW_COMMIT);

        // 命中复位连击:低分 PENDING 后命中,再低分仍是 PENDING。
        CHECK(place_fp_observe(&s, 900) == PLACE_OBS_MATCH);
        CHECK(s.new_streak == 0);
        CHECK(place_fp_observe(&s, 0) == PLACE_OBS_NEW_PENDING);
        CHECK(s.new_streak == 1);
        CHECK(place_fp_observe(&s, 800) == PLACE_OBS_MATCH);
        CHECK(s.new_streak == 0);

        // 库为空传 -1:第一次 PENDING、第二次 COMMIT(应用层对空库会直接建点,
        // 这里仅固定纯函数行为)。
        CHECK(place_fp_observe(&s, -1) == PLACE_OBS_NEW_PENDING);
        CHECK(s.new_streak == 1);
        CHECK(place_fp_observe(&s, -1) == PLACE_OBS_NEW_COMMIT);
        CHECK(s.new_streak == 0);
    }

    if (s_failures == 0) {
        printf("test_place_fp: ALL PASS\n");
        return 0;
    }
    printf("test_place_fp: %d CHECK(s) FAILED\n", s_failures);
    return 1;
}
