// components/bsp/include/bsp_wifi_scan.h
// 一次性 Wi-Fi 扫描:供"场所记忆"等只需 BSSID 指纹、不连网的功能使用。
#pragma once

#include "esp_err.h"
#include <stddef.h>
#include <stdint.h>

// 单次扫描最多取回的 AP 数(应用侧只取信号最强的 12 个,多取一些供挑选/余量)。
#define BSP_WIFI_SCAN_MAX 16

typedef struct {
    uint8_t bssid[6];   // AP MAC,即指纹元素
    int8_t  rssi;       // dBm,负值
    uint8_t channel;    // 主信道(调试用)
} bsp_wifi_ap_t;

// 执行一次全信道主动扫描并返回 AP 列表(原始顺序,未按 RSSI 排序;
// 排序/取 top-N 由应用侧纯逻辑 place_fp 完成)。
//
// 生命周期:内部幂等准备 NVS/netif/事件循环 -> 创建 STA netif ->
// esp_wifi_init/start -> 阻塞扫描 -> 取结果 -> 【立即 stop + deinit +
// 销毁 netif】。Wi-Fi 协议栈不常驻,调用返回后射频回到关闭状态。
//
// 线程/阻塞:阻塞约 2~4 秒,只能在 worker 任务中调用;
// 严禁在 LVGL 任务、按键回调等上下文中调用。非可重入:同一时刻仅允许一个调用方。
//
// 参数:
//   out       调用方提供的数组,容量 >= max_out
//   max_out   数组容量
//   out_count 可选,返回实际填入的 AP 数(扫描到 0 个时为 0)
// 返回:
//   ESP_OK            扫描流程完成(即使 0 个 AP);
//   其他              Wi-Fi 初始化/扫描失败;此时协议栈仍保证已释放。
//   NVS 分区异常时不擦除用户数据(遵循仓库约定),直接返回错误。
esp_err_t bsp_wifi_scan_once(bsp_wifi_ap_t *out, size_t max_out, size_t *out_count);
