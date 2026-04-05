/**
 * ============================================================================
 * res-state.c — CoAP 电池状态资源（GET + OBSERVE /dev/state）
 * ============================================================================
 *
 * 【功能】向订阅者（uGridController）推送电池实时状态数据
 *
 * 【通信协议】
 *   方法：CoAP GET（支持 OBSERVE 订阅机制）
 *   路径：/dev/state
 *   响应体：{"V":电压,"I":电流,"T":温度,"S":SoC,"H":SoH,"St":状态码}
 *   格式：APPLICATION_JSON
 *
 * 【数据编码说明】
 *   所有浮点值均放大为整数传输，避免 JSON 浮点精度问题：
 *     V（电压）  → ×100，单位 centiV（3.95V → 395）
 *     I（电流）  → ×100，单位 centiA（0.75A → 75）
 *     T（温度）  → ×100，单位 centi°C（24.36°C → 2436）
 *     S（SoC）   → ×10000，单位 permil（0.79 → 7900，即 79.00%）
 *     H（SoH）   → ×10000，单位 permil（0.91 → 9100，即 91.00%）
 *     St（状态） → 整数枚举（0=INI, 1=RUN, 2=ISO）
 *
 * 【OBSERVE 机制】
 *   uGridController 启动后会订阅此资源，当电池状态变化时
 *   自动通过 CoAP NOTIFY 推送最新数据，无需轮询。
 * ============================================================================
 */

#include "contiki.h"
#include <stdio.h>
#include <string.h>
#include "coap-engine.h"
#include "coap.h"
#include "sys/log.h"
#include "../../includes/utility.h"

#define LOG_MODULE "state"
#define LOG_LEVEL LOG_LEVEL_APP

/* 从 battery_controller.c 导入的外部变量 */
extern float bat_current;
extern float bat_voltage;
extern float bat_temp;
extern float bat_soc;
extern float bat_soh;
extern coap_resource_t res_dev_state;
extern battery_state_t current_state;

/**
 * 周期性触发函数：主动通知所有 OBSERVE 订阅者状态已更新
 * 由 EVENT_RESOURCE 的最后一个参数指定，由 Contiki 定时器自动调用
 */
static void res_state_periodic_handler(void) {
    coap_notify_observers(&res_dev_state);
}

/**
 * GET 请求处理函数
 *
 * 将电池当前状态格式化为 JSON 字符串返回。
 * 数据包含 6 个字段：V（电压）、I（电流）、T（温度）、
 * S（SoC 荷电状态）、H（SoH 健康状态）、St（运行状态码）
 */
static void res_get_state_h(coap_message_t *req, coap_message_t *res, 
                                  uint8_t *buf, uint16_t size, int32_t *off) {
    /* 导出当前电流和电压值 */
    float export_I = bat_current;
    float export_V = bat_voltage;

    /*
     * 状态字段说明：
     * V:  当前电压（×100，单位 centiV）
     * I:  当前电流（×100，单位 centiA）
     * T:  当前温度（×100，单位 centi°C）
     * S:  当前 SoC 荷电状态（×10000，单位 permil）
     * H:  当前 SoH 健康状态（×10000，单位 permil）
     * St: 当前运行状态码（0=INI 初始化, 1=RUN 运行中, 2=ISO 已隔离）
     *
     * 所有值放大为整数传输，因为我们不需要低于 10e-2 的精度。
     * 这是一种简化设计，避免浮点数在 JSON 中的精度问题。
     */
    int len = snprintf((char*)buf, size, 
            "{\"V\":%d,\"I\":%d,\"T\":%d,"
            "\"S\":%d,\"H\":%d,\"St\":%d}",
            (int)(export_V * 100),      // 3.95V → 395 centiV
            (int)(export_I * 100),      // 0.75A → 75 centiA  
            (int)(bat_temp * 100),      // 24.36°C → 2436 centi°C
            (int)(bat_soc * 10000),     // 0.79 → 7900 (79.00%)
            (int)(bat_soh * 10000),     // 0.91 → 9100 (91.00%)
            current_state);

    coap_set_header_content_format(res, APPLICATION_JSON);
    coap_set_payload(res, buf, len);
}

/**
 * 注册 EVENT_RESOURCE（事件驱动资源，支持 OBSERVE 订阅 + 周期性通知）
 *
 * 参数说明（按顺序）：
 *   - 资源名称：res_dev_state
 *   - 属性描述："title=\"State\";obs"（obs 标记支持 OBSERVE）
 *   - GET  handler：res_get_state_h（返回电池状态 JSON）
 *   - POST handler：NULL
 *   - PUT  handler：NULL
 *   - DELETE handler：NULL
 *   - 周期性触发：res_state_periodic_handler（定时推送通知）
 */
EVENT_RESOURCE(res_dev_state,
               "title=\"State\";obs",
               res_get_state_h,
               NULL,
               NULL,
               NULL,
               res_state_periodic_handler);
