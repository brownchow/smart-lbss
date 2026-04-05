/**
 * ============================================================================
 * res-obj.c — CoAP 手动功率目标资源（GET/PUT /ctrl/obj）
 * ============================================================================
 *
 * 【功能】允许远程（RCA/CA）为单个电池设置手动功率目标，覆盖 MPC 自动计算结果
 *
 * 【通信协议】
 *   GET /ctrl/obj  → 返回所有电池的手动目标状态（JSON）
 *   PUT /ctrl/obj  → 设置/清除单个电池的手动目标
 *
 * 【PUT 请求体格式】
 *   {"idx":<电池索引>, "power_kw":<功率×100>, "clear":<0或1>}
 *   - clear=1：清除该电池的手动目标，恢复 MPC 自动控制
 *   - clear=0：设置新的手动功率目标（单位 kW，×100 整数编码）
 *
 * 【优先级】
 *   手动目标 > MPC 自动计算
 *   当电池设置了 has_objective 标志后，MPC 会跳过该电池，直接使用手动值
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

extern battery_node_t batteries[];
extern int battery_count;

#define LOG_MODULE "objective"
#define LOG_LEVEL LOG_LEVEL_INFO

/**
 * GET 请求处理函数：返回所有电池的手动目标状态
 *
 * 响应格式：{"bats":[{"idx":0,"obj":1,"pkw":150},...]}
 *   - obj: 1=有手动目标, 0=无（MPC 控制）
 *   - pkw: 手动功率值（×100，单位 kW）
 */
static void
res_obj_get_handler(coap_message_t *req, coap_message_t *res,
                    uint8_t *buf, uint16_t size, int32_t *off)
{
    int len = 0;
    uint8_t first = 1;

    len += snprintf((char *)buf + len, size - len, "{ \"bats\":[");

    /* 遍历所有已注册电池，收集手动目标信息 */
    for (int i = 0; i < battery_count && len < (int)size - 64; i++) {
        
        if(!batteries[i].active) continue;
        
        if(!first) {
            len += snprintf((char *)buf + len, size - len, ",");
        }
        first = 0;

        len += snprintf((char *)buf + len, size - len,
                        "{"
                        "\"idx\":%d,"
                        "\"obj\":%d,"
                        "\"pkw\":%d.%d"
                        "}",
                        i,
                        batteries[i].has_objective ? 1 : 0,
                        (int)(batteries[i].objective_power) * 100,
                        (int)(batteries[i].objective_power) * 100) % 100;
    }

    len += snprintf((char *)buf + len, size - len, "]}");

    coap_set_header_content_format(res, APPLICATION_JSON);  /* <- coap-engine.h */
    coap_set_payload(res, buf, len);                        /* <- coap-engine.h */
}

/**
 * PUT 请求处理函数：设置或清除单个电池的手动功率目标
 *
 * 流程：解析 JSON → 验证索引 → 设置/清除目标 → 返回 2.04 Changed
 */
void res_obj_put_handler(coap_message_t *req, coap_message_t *res,
                         uint8_t *buf, uint16_t size, int32_t *off)
{
    const uint8_t *payload;
    int plen = coap_get_payload(req, &payload);  /* <- coap-engine.h */

    static char s[128];
    if(plen <= 0 || plen >= (int)sizeof(s)) {
        coap_set_status_code(res, BAD_REQUEST_4_00);  /* <- coap-engine.h */
        return;
    }
    memcpy(s, payload, plen);  /* <- string.h */
    s[plen] = '\0';

    int idx = -1;
    int power = 0;
    int clear = 0;

    /* 解析 JSON：{"idx":0, "power_kw":150, "clear":0} */
    int n = sscanf(s, "{ \"idx\" : %d , \"power_kw\" : %d , \"clear\" : %d }",
                   &idx, &power, &clear);  /* <- stdio.h */
    if(n != 3) {
        LOG_WARN("[OBJ] Bad payload (sscanf=%d): %s\n", n, s);  /* <- sys/log.h */
        coap_set_status_code(res, BAD_REQUEST_4_00);  /* <- coap-engine.h */
        return;
    }

    float power_kw = (float)power / 100.0f;

    int active = -1;
    if(idx >= 0 && idx < battery_count) active = batteries[idx].active;


    /* 验证电池索引有效性 */
    if (idx < 0 || idx >= battery_count || !batteries[idx].active) {
        LOG_WARN("[OBJ] Invalid idx=%d battery_count=%d active=%d\n", idx, battery_count, active);  /* <- sys/log.h */
        coap_set_status_code(res, BAD_REQUEST_4_00);  /* <- coap-engine.h */
        return;
    }

    /* clear=1：清除手动目标，恢复 MPC 控制 */
    if (clear) {
        batteries[idx].has_objective = 0;
        batteries[idx].objective_power = 0.0f;
        LOG_INFO("[OBJ] Cleared objective for Bat #%d\n", idx);  /* <- sys/log.h */
        coap_set_status_code(res, CHANGED_2_04);  /* <- coap-engine.h */
        return;
    }

    /* 钳位到物理功率限制 */
    if(power_kw > BAT_MAX_POWER_KW)  power_kw = BAT_MAX_POWER_KW;
    if(power_kw < -BAT_MAX_POWER_KW) power_kw = -BAT_MAX_POWER_KW;

    /* 设置手动目标 */
    batteries[idx].has_objective   = 1;
    batteries[idx].objective_power = power_kw;
    LOG_INFO("[OBJ] Set objective for Bat #%d kW\n", idx);  /* <- sys/log.h */
    coap_set_status_code(res, CHANGED_2_04);  /* <- coap-engine.h */
}

/**
 * 注册 CoAP 资源 res_obj_ctrl
 *
 * 参数：
 *   - GET handler：res_obj_get_handler（查询所有电池的手动目标状态）
 *   - PUT handler：res_obj_put_handler（设置/清除单个电池的手动目标）
 */
RESOURCE(res_obj_ctrl,
         "title=\"Objectives\"",
         res_obj_get_handler,
         NULL,
         res_obj_put_handler,
         NULL);

