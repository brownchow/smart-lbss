/**
 * ============================================================================
 * res-power.c — CoAP 功率控制资源（PUT /dev/power）
 * ============================================================================
 *
 * 【功能】接收 uGridController 下发的功率指令，设置电池的充放电目标功率
 *
 * 【通信协议】
 *   方法：CoAP PUT
 *   路径：/dev/power
 *   请求体：{"u":<功率值，单位 W>}
 *   响应：2.04 Changed（成功）/ 4.03 Forbidden（电池未就绪）/ 4.00 Bad Request
 *
 * 【安全机制】
 *   1. 仅 RUNNING 状态接受指令，ISOLATED/INIT 状态直接拒绝（403）
 *   2. 功率值自动钳位到 BAT_MAX_POWER_W 物理限制
 *   3. 设置成功后更新 LED 状态指示
 * ============================================================================
 */

#include "contiki.h"
#include <stdint.h>
#include <string.h>
#include "coap-engine.h"
#include "coap.h"
#include "../../includes/constants.h"
#include "../../includes/utility.h"
#include "sys/log.h"

#define LOG_MODULE "state"
#define LOG_LEVEL LOG_LEVEL_APP


/* 从 battery_controller.c 导入的外部变量和函数 */
extern void update_leds();
extern float power_setpoint;              /* 当前功率设定值（W） */
extern battery_state_t current_state;     /* 电池状态机（INIT/RUNNING/ISOLATED） */

/**
 * PUT 请求处理函数
 *
 * 流程：状态检查 → 解析 JSON payload → 钳位到物理限制 → 更新功率设定值 → 更新 LED
 */
static void res_power_put_handler(coap_message_t *req, coap_message_t *res, 
                                  uint8_t *buf, uint16_t size, int32_t *off) {

    /* 安全检查：只有 RUNNING 状态的电池才接受功率指令 */
    if (current_state != STATE_RUNNING) { 
        LOG_WARN("[CMD] Rejected - Not in RUNNING state\n");
        coap_set_status_code(res, FORBIDDEN_4_03); 
        return; 
    }

    const uint8_t *chunk;
    int len = coap_get_payload(req, &chunk);
    int param;
    float req_p;


    /* 解析 JSON payload：{"u":<功率值>}，u 的单位是瓦特（W） */
    sscanf((char*)chunk,"{\"u\":%d}",&param);

    req_p = (float)param;

    if(len > 0 && len < 32) {

        /* 钳位到物理功率限制（防止过充/过放） */
        if(req_p > BAT_MAX_POWER_W)  req_p = BAT_MAX_POWER_W;
        if(req_p < -BAT_MAX_POWER_W) req_p = -BAT_MAX_POWER_W;

        power_setpoint = req_p;

        int32_t req_w = (int32_t)req_p;

        LOG_INFO("[CMD] Power setpoint: %+ld W ", (long)req_w);
        if(req_w > 0) {
            LOG_INFO_("(Charging)\n");
        } else if(req_w < 0) {
            LOG_INFO_("(Discharging)\n");
        } else {
            LOG_INFO_("(Idle)\n");
        }

        coap_set_status_code(res, CHANGED_2_04);
        update_leds();
    } else {
        LOG_WARN("[CMD] Invalid payload length: %d\n", len);
        coap_set_status_code(res, BAD_REQUEST_4_00);
    }
}

/**
 * 注册 CoAP 资源 res_dev_power
 *
 * 参数说明（按顺序）：
 *   - 资源名称：res_dev_power
 *   - 属性描述："title=\"Power\""
 *   - GET  handler：NULL（不支持读取功率设定值）
 *   - POST handler：NULL
 *   - PUT  handler：res_power_put_handler（接收功率指令）
 *   - DELETE handler：NULL
 */
RESOURCE(res_dev_power,
         "title=\"Power\"",
         NULL,
         NULL,
         res_power_put_handler,
         NULL);



