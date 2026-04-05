/**
 * ============================================================================
 * res-register.c — CoAP 电池注册资源（POST /dev/register）
 * ============================================================================
 *
 * 【功能】接收 BatteryController 的注册请求，将其加入本地电池管理列表
 *
 * 【通信协议】
 *   方法：CoAP POST
 *   路径：/dev/register
 *   请求体：<电池 ID（整数）>
 *   响应：2.01 Created（注册成功）/ 5.03 Service Unavailable（已达上限）
 *
 * 【注册流程】
 *   1. BatteryController 启动后循环发送 POST /dev/register
 *   2. uGridController 收到后记录电池 IPv6 地址，初始化默认状态
 *   3. 返回 2.01 Created，BatteryController 进入 RUNNING 状态
 *   4. uGridController 发送 PROCESS_EVENT_MSG 触发 OBSERVE 订阅建立
 *
 * 【存储结构】
 *   每个已注册电池的信息保存在 batteries[] 数组中，包括：
 *   - IPv6 地址（从请求源地址自动提取）
 *   - 实时状态（SoC/SoH/V/I/T，通过 OBSERVE 更新）
 *   - 功率指令（MPC 计算结果或手动目标）
 * ============================================================================
 */

#include "contiki.h"
#include <stdint.h>
#include <string.h>
#include "coap-engine.h"
#include "coap.h"
#include "net/ipv6/uiplib.h"
#include "../../includes/constants.h"
#include "../../includes/utility.h"
#include "sys/log.h"

/* 从 ugrid_controller.c 导入的电池管理数组 */
extern battery_node_t batteries[];
extern int battery_count;

#define LOG_MODULE "register"
#define LOG_LEVEL LOG_LEVEL_INFO

PROCESS_NAME(ugrid_controller);

/**
 * POST 请求处理函数：注册新电池
 *
 * 流程：提取源 IPv6 地址 → 初始化默认状态 → 加入 batteries[] 数组 → 触发 OBSERVE 订阅
 */
static void res_reg_h(coap_message_t *req, coap_message_t *res, uint8_t *buf, uint16_t size, int32_t *off) {
    LOG_INFO(">>> [REGISTRY] Received registration from ");  /* <- sys/log.h */
    LOG_INFO_6ADDR(&req->src_ep->ipaddr);                    /* <- sys/log.h */
    LOG_INFO_("\n");

    if (battery_count < MAX_BATTERIES) {
        /* 从请求源地址提取电池 IPv6 地址 */
        uip_ipaddr_copy(&batteries[battery_count].ip, &req->src_ep->ipaddr);  /* <- net/ipv6/uip.h */

        /* 初始化默认状态值 */
        batteries[battery_count].current_soc = 0.5f;
        batteries[battery_count].current_voltage = 0.0f;
        batteries[battery_count].current_temp = 25.0f;
        batteries[battery_count].current_soh = 1.0f;
        batteries[battery_count].current_current = 0.0f;
        batteries[battery_count].optimal_u = 0.0f;
        batteries[battery_count].actual_power = 0.0f;
        batteries[battery_count].state = 0;

        batteries[battery_count].active = 1;
        batteries[battery_count].obs_requested = 0;
        batteries[battery_count].last_update_time = clock_seconds();  /* <- sys/clock.h */
        batteries[battery_count].obs = NULL;
        batteries[battery_count].has_objective = false;
        batteries[battery_count].objective_power = 0.0f;

        LOG_INFO(">>> [REGISTRY] Registered Battery #%d: ", battery_count);  /* <- sys/log.h */
        LOG_INFO_6ADDR(&batteries[battery_count].ip);
        LOG_INFO_("\n");

        battery_count++;

        coap_set_status_code(res, CREATED_2_01);  /* <- coap-engine.h */

        /* 触发 PROCESS_EVENT_MSG 事件，通知主进程建立 OBSERVE 订阅 */
        process_post(&ugrid_controller, PROCESS_EVENT_MSG, NULL);  /* <- sys/process.h */
    } else {
        LOG_WARN(">>> [REGISTRY] Max batteries reached\n");  /* <- sys/log.h */
        coap_set_status_code(res, SERVICE_UNAVAILABLE_5_03);  /* <- coap-engine.h */
    }
}

/**
 * 注册 CoAP 资源 res_register
 *
 * 参数：
 *   - POST handler：res_reg_h（接收电池注册请求）
 *   - 其他 handler 均为 NULL（仅支持 POST）
 */
RESOURCE(res_register,
        "title=\"Reg\"",
        NULL,
        res_reg_h,
        NULL,
        NULL);

