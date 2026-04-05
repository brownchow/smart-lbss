/**
 * ============================================================================
 * res-mpc.c — CoAP MPC 参数配置资源（PUT /ctrl/mpc）
 * ============================================================================
 *
 * 【功能】远程动态调整 MPC 优化算法的权重参数，无需重启设备
 *
 * 【通信协议】
 *   方法：CoAP PUT
 *   路径：/ctrl/mpc
 *   请求体：{"a":<alpha×100>, "b":<beta×100>, "g":<gama×100>, "p":<price×100>}
 *   响应：2.04 Changed
 *
 * 【参数说明】
 *   alpha — 电网电价权重（影响从电网购电的优先级）
 *   beta  — 电池衰减惩罚权重（影响充放电功率的平滑度）
 *   gama  — SoC 偏离惩罚权重（影响电池维持在参考 SoC 的优先级）
 *   price — 当前电价（影响优化目标中的成本项）
 *
 * 【使用场景】
 *   RCA 或 CA 可通过此接口实时调节 MPC 策略，例如：
 *   - 电价高峰时段调高 price，让 MPC 优先放电
 *   - 电池老化严重时调高 beta，降低充放电速率
 * ============================================================================
 */

#include "contiki.h"
#include <stdint.h>
#include <string.h>
#include <stdio.h>
#include "coap-engine.h"
#include "coap.h"
#include "../../includes/constants.h"
#include "../../includes/utility.h"
#include "sys/log.h"

extern battery_node_t batteries[];
extern int battery_count;

#define LOG_MODULE "mcp"
#define LOG_LEVEL LOG_LEVEL_INFO

/* 从 ugrid_controller.c 导入的 MPC 参数 */
extern float alpha;
extern float beta;
extern float gama;
extern float price;

/**
 * PUT 请求处理函数：解析 JSON 并更新 MPC 参数
 *
 * 输入值需要 ×100 传输（整数编码），接收端再 ÷100 还原为浮点数
 */
static void
res_mpc_put_handler(coap_message_t *req, coap_message_t *res,
        uint8_t *buf, uint16_t size, int32_t *off)
{
    const uint8_t *payload;
    coap_get_payload(req, &payload);  /* <- coap-engine.h */

    static int a,b,c,p;

    /* 解析 JSON：{"a":100, "b":100, "g":2000, "p":25} */
    sscanf((char*)payload, "{\"a\":%d, \"b\":%d, \"g\":%d, \"p\":%d}", &a, &b, &c, &p);  /* <- stdio.h */

    /* 还原为浮点数（÷100） */
    alpha = (float)a / 100.0f;
    beta = (float)b / 100.0f;
    gama = (float)c / 100.0f;
    price = (float)p / 100.0f;

    LOG_INFO("[MPC] Updated params: alpha=%d.%d beta=%d.%d gama=%d.%d price=%d.%d\n",  /* <- sys/log.h */
            (int)(alpha), ((int)(alpha) * 100) % 100,
            (int)(beta), ((int)(beta) * 100) % 100,
            (int)(gama), ((int)(gama) * 100) % 100,
            (int)(price), ((int)(price) * 100) % 100 );

    coap_set_status_code(res, CHANGED_2_04);  /* <- coap-engine.h */
}

/**
 * 注册 CoAP 资源 res_mpc_params
 *
 * 参数：
 *   - PUT handler：res_mpc_put_handler（更新 MPC 参数）
 *   - 其他 handler 均为 NULL（仅支持 PUT）
 */
RESOURCE(res_mpc_params,
        "title=\"MPC params\"",
        NULL,
        NULL,
        res_mpc_put_handler,
        NULL);

