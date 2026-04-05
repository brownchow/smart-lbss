/**
 * ============================================================================
 * utility.h — 共享类型和数据结构定义
 * ============================================================================
 *
 * 本文件定义了 BatteryController 和 uGridController 之间共享的数据结构。
 */
#ifndef _UTILITY_H
#define _UTILITY_H

#include "contiki.h"
#include "random.h"
#include <stdbool.h>
#include <stdint.h>

/**
 * 电池状态枚举
 *   STATE_INIT    — 初始化中（等待注册到 uGridController）
 *   STATE_RUNNING — 正常运行（接受功率指令）
 *   STATE_ISOLATED — 已隔离（安全保护触发，拒绝功率指令）
 */
typedef enum {
    STATE_INIT,
    STATE_RUNNING,
    STATE_ISOLATED
} battery_state_t;

/* 系统支持的最大电池数量 */
#define MAX_BATTERIES 5

/**
 * 电池节点信息结构（uGridController 端维护）
 *
 * 每个已注册的 BatteryController 在此数组中占一个条目，
 * 包含其网络地址、实时状态、功率指令等信息。
 */
typedef struct {
    /* 网络信息 */
    uip_ipaddr_t ip;       /* 电池节点的 IPv6 地址 */
    bool active;           /* 是否已激活 */
    bool obs_requested;    /* 是否已建立 OBSERVE 订阅 */

    /* 实时遥测数据（通过 OBSERVE 更新） */
    float current_soc;     /* 荷电状态（0~1） */
    float current_voltage; /* 电压（V） */
    float current_temp;    /* 温度（°C） */
    float current_soh;     /* 健康状态（0~1） */
    float current_current; /* 电流（A） */

    /* 功率控制 */
    float optimal_u;       /* MPC 计算的最优功率指令（kW） */
    float actual_power;    /* 实际执行功率（kW） */
    battery_state_t state; /* 当前运行状态 */

    /* 手动目标覆盖 */
    bool  has_objective;           /* 是否设置了手动目标 */
    float objective_power;         /* 手动目标功率（kW） */

    /* 元数据 */
    uint32_t last_update_time;     /* 最后一次状态更新时间 */
    coap_observee_t *obs;          /* CoAP OBSERVE 订阅句柄 */
} battery_node_t;

#endif
