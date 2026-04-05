/**
 * ============================================================================
 * constants.h — 全局常量定义（BatteryController 和 uGridController 共用）
 * ============================================================================
 */
#ifndef _CONSTANTS_H
#define _CONSTANTS_H

/* ANSI 终端颜色码（用于日志输出） */
#define ROSSO   "\033[31m"   /* 红色：告警/异常 */
#define VERDE   "\033[32m"   /* 绿色：正常 */
#define RESET   "\033[0m"    /* 重置 */

/* 电池物理参数 */
#define BATTERY_TIMEOUT_SEC 30                /* 电池超时时间（秒） */
#define BAT_MAX_POWER_KW  10.0f               /* 电池最大充放电功率（kW） */
#define BAT_MAX_POWER_W   (BAT_MAX_POWER_KW * 1000.0f)  /* 换算为瓦特 */
#define MAX_IRR   1200.0f                     /* 最大太阳辐照度（W/m²） */

/* ML 特征窗口配置 */
#define ML_WINDOW 10   /* 滑动窗口大小（10 个时间步） */
#define N_FEATURES 4   /* 特征数量：电压、电流、温度、SoC */

#endif
