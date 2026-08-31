// main/place_fp.h —— 离线场所记忆:地点指纹与匹配的纯逻辑模块(V0.2)。
//
// 本头文件及其实现【不依赖 ESP-IDF / LVGL / FreeRTOS】,只用标准 C,
// 可在主机上直接 `cc` 编译并由 tests/test_place_fp.c 覆盖。
//
// 核心机制:周边 Wi-Fi AP 的 BSSID 集合构成地点指纹;无需联网、无需 GPS。
//
// V0.2 抗抖动(真机实测同址得分会在 600‰~1000‰ 间抖动,旧版 Jaccard
// 单次跌破 500‰ 即误建新地点):
//   1. 指纹取信号最强的 12 个 BSSID(旧版 8 个),边界 AP 抖动占比更小;
//   2. 相似度改用 Overlap 系数 = 交集 / min(|A|,|B|):多扫到几个弱 AP
//      (并集变大)不再拉低得分——已保存地点的大部分 BSSID 仍在射频范围
//      内,就说明人还在同一物理位置(BSSID 是物理射频,异址听不到);
//   3. 新地点建立需"连续两次明确不匹配"(迟滞);350‰~499‰ 为灰区,
//      不建点、由应用层安排快速重扫。单次扫描抖动永远无法创建假地点。
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define PLACE_FP_BSSID_COUNT  12    // 每个指纹保留信号最强的 12 个 BSSID
#define PLACE_MAX_COUNT       16    // 最多保存的地点数
#define PLACE_NAME_MAX        12    // 地点名称长度(含结尾 NUL)
#define PLACE_MATCH_PERMILLE  500   // Overlap 命中阈值:500‰ = 50%(12 中至少 6 个重合)
#define PLACE_UNSURE_PERMILLE 350   // 灰区下限:350‰~499‰ 不建点,快速重扫
#define PLACE_NEW_CONFIRM_SCANS 2   // 连续 N 次"明确不像"才允许建新地点

// 一次 AP 观测。BSP 扫描结果与纯逻辑之间的中立输入类型,
// 这样指纹算法不需要认识 wifi_ap_record_t。
typedef struct {
    uint8_t bssid[6];
    int8_t  rssi;
} place_ap_t;

// 地点指纹:BSSID 集合(无序)。count 为有效条目数,<= PLACE_FP_BSSID_COUNT。
typedef struct {
    uint8_t bssid[PLACE_FP_BSSID_COUNT][6];
    uint8_t count;
} place_fingerprint_t;

// 一条已保存地点记录。字段顺序即 NVS blob 落盘布局,勿随意调整
// (当前大小:12 + 2 + 73 = 87,对齐后 88 字节)。
typedef struct {
    char                name[PLACE_NAME_MAX];
    uint16_t            visits;
    place_fingerprint_t fp;
} place_record_t;

// 地点库整体作为一个 NVS blob 读写(16 * 88 + 8 = 1416 字节)。
typedef struct {
    uint32_t magic;      // 校验用,见 demo_place.c db_load
    uint16_t version;
    uint16_t count;      // 已保存地点数,<= PLACE_MAX_COUNT
    place_record_t places[PLACE_MAX_COUNT];
} place_db_t;

// 从 AP 观测列表构造指纹:按 RSSI 降序选取,同一 BSSID 去重(保留最强),
// 最多取 PLACE_FP_BSSID_COUNT 个。
void place_fp_build(place_fingerprint_t *fp, const place_ap_t *aps, size_t n);

// 两个指纹的 BSSID 交集数量。
int place_fp_intersection(const place_fingerprint_t *a, const place_fingerprint_t *b);

// 两个指纹的 BSSID 并集数量。
int place_fp_union_count(const place_fingerprint_t *a, const place_fingerprint_t *b);

// Jaccard 相似度 = 交集/并集,单位千分比(0..1000)。两个指纹都为空时返回 0。
// 保留作调试/展示;V0.2 起判定以 Overlap 系数为准。
int place_fp_jaccard_permille(const place_fingerprint_t *a, const place_fingerprint_t *b);

// Overlap 系数 = 交集 / min(|A|,|B|),单位千分比(0..1000)。
// 不受"本次多扫到弱 AP"影响,同址稳定性显著优于 Jaccard。
int place_fp_overlap_permille(const place_fingerprint_t *a, const place_fingerprint_t *b);

// 是否判定为同一地点:Overlap >= PLACE_MATCH_PERMILLE(50%)。
// 整数比较:intersection * 1000 >= min(count_a,count_b) * 500,无浮点。
bool place_fp_match(const place_fingerprint_t *a, const place_fingerprint_t *b);

// 在地点库中查找与 fp 最相似的地点(Overlap 得分最高且达到阈值)。
// 返回地点索引(>=0);无匹配返回 -1。score_permille 可为 NULL,
// 用于回传最佳得分(库为空时为 -1,否则为未达阈值的最高分)。
int place_db_find(const place_db_t *db, const place_fingerprint_t *fp,
                  int *score_permille);

// 跨扫描的观测会话状态(迟滞判定),纯内存,进入页面时清零。
typedef struct {
    uint8_t new_streak;   // 连续"明确不像任何已知地点"的扫描次数
} place_session_t;

// 单次扫描观测结论。
typedef enum {
    PLACE_OBS_MATCH = 0,      // 命中已知地点
    PLACE_OBS_UNSURE,         // 灰区(350‰~499‰):可能是抖动,不建点,快速重扫
    PLACE_OBS_NEW_PENDING,    // 明确不像,但需第 2 次扫描确认
    PLACE_OBS_NEW_COMMIT,     // 连续 PLACE_NEW_CONFIRM_SCANS 次确认:可建新地点
} place_obs_t;

// 输入本次扫描对库中所有地点的最佳 Overlap 得分(千分比;库为空时传 -1),
// 输出观测结论并更新会话状态。MATCH/UNSURE 复位连击;
// NEW_COMMIT 返回后内部连击自动清零。
// 注意:库为空(count==0)时应用层应直接建新地点,无需走迟滞。
place_obs_t place_fp_observe(place_session_t *s, int best_score_permille);
