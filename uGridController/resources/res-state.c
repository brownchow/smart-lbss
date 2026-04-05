/**
 * ============================================================================
 * res-state.c — CoAP 微电网状态资源（GET /dev/state）
 * ============================================================================
 *
 * 【功能】返回微电网全局状态，包含所有已注册电池的实时数据和聚合信息
 *
 * 【通信协议】
 *   方法：CoAP GET
 *   路径：/dev/state
 *   响应体：CBOR 编码的二进制数据（比 JSON 更紧凑，适合低功耗网络）
 *   格式：APPLICATION_CBOR
 *
 * 【CBOR 数据结构】
 *   Map {
 *     0: active_cnt (uint)    — 活跃电池数量
 *     1: load_c (int)         — 当前负载 ×100（centi-kW）
 *     2: pv_c (int)           — 当前 PV 发电 ×100（centi-kW）
 *     3: batteries[] (array)  — 每块电池的数据数组
 *   }
 *
 *   每块电池的数据（数组元素）：
 *     [0] idx    (uint)   — 电池索引
 *     [1] u_c    (int)    — 最优功率 ×100（centi-kW）
 *     [2] S_c    (int)    — SoC ×100（百分比）
 *     [3] p_c    (int)    — 实际功率 ×100（centi-kW）
 *     [4] V_c    (int)    — 电压 ×100（centi-V）
 *     [5] I_c    (int)    — 电流 ×100（centi-A）
 *     [6] T_c    (int)    — 温度 ×100（centi-°C）
 *     [7] H_c    (int)    — SoH ×100（百分比）
 *     [8] st     (uint)   — 状态码（0=INI, 1=RUN, 2=ISO）
 *
 * 【为什么用 CBOR 而不是 JSON？】
 *   - 6LoWPAN 网络带宽极低（~250kbps），MTU 仅 127 字节
 *   - CBOR 是二进制编码，比 JSON 文本节省 30-50% 空间
 *   - 解析速度更快，内存占用更小
 * ============================================================================
 */

#include "contiki.h"
#include "coap.h"
#include "coap-engine.h"
#include "cbor.h"
#include <stdint.h>
#include <math.h>

#include "../../includes/utility.h"

/* 从 ugrid_controller.c 导入的全局状态 */
extern int battery_count;
extern float curr_load;
extern float curr_pv;
extern battery_node_t batteries[];

/**
 * GET 请求处理函数：构建 CBOR 编码的微电网状态响应
 *
 * 流程：统计活跃电池 → 打开 CBOR Map → 写入聚合数据 → 遍历电池写入详细数据
 */
    static void
res_get_state_h(coap_message_t *req, coap_message_t *res,
        uint8_t *buf, uint16_t size, int32_t *off)
{
    (void)req; (void)off;

    cbor_writer_state_t ws;
    cbor_init_writer(&ws, buf, size);  /* <- cbor.h */

    const int load_c = (int)lroundf(curr_load * 100.0f);
    const int pv_c   = (int)lroundf(curr_pv   * 100.0f);

    /* 统计活跃电池数量 */
    int active_cnt = 0;
    for(int i = 0; i < battery_count; i++) {
        if(batteries[i].active) active_cnt++;
    }

    cbor_open_map(&ws);  /* <- cbor.h */

    /* 写入聚合数据：活跃数量、负载、PV */
    cbor_write_unsigned(&ws, 0); cbor_write_unsigned(&ws, (uint64_t)active_cnt);  /* <- cbor.h */
    cbor_write_unsigned(&ws, 1); cbor_write_signed(&ws,  (int64_t)load_c);       /* <- cbor.h */
    cbor_write_unsigned(&ws, 2); cbor_write_signed(&ws,  (int64_t)pv_c);         /* <- cbor.h */

    /* 写入电池数组 */
    cbor_write_unsigned(&ws, 3);  /* <- cbor.h */
    cbor_open_array(&ws);         /* <- cbor.h */

    for(int i = 0; i < battery_count; i++) {
        if(!batteries[i].active) continue;

        /* 所有浮点值 ×100 转为整数（2 位小数精度） */
        const int u_c = (int)lroundf(batteries[i].optimal_u       * 100.0f);
        const int S_c = (int)lroundf(batteries[i].current_soc     * 100.0f);
        const int p_c = (int)lroundf(batteries[i].actual_power    * 100.0f);
        const int V_c = (int)lroundf(batteries[i].current_voltage * 100.0f);
        const int I_c = (int)lroundf(batteries[i].current_current * 100.0f);
        const int T_c = (int)lroundf(batteries[i].current_temp    * 100.0f);
        const int H_c = (int)lroundf(batteries[i].current_soh     * 100.0f);

        const uint64_t st = (uint64_t)batteries[i].state;

        cbor_open_array(&ws);         /* <- cbor.h */
        cbor_write_unsigned(&ws, (uint64_t)i);    /* <- cbor.h */
        cbor_write_signed(&ws,  (int64_t)u_c);    /* <- cbor.h */
        cbor_write_signed(&ws,  (int64_t)S_c);    /* <- cbor.h */
        cbor_write_signed(&ws,  (int64_t)p_c);    /* <- cbor.h */
        cbor_write_signed(&ws,  (int64_t)V_c);    /* <- cbor.h */
        cbor_write_signed(&ws,  (int64_t)I_c);    /* <- cbor.h */
        cbor_write_signed(&ws,  (int64_t)T_c);    /* <- cbor.h */
        cbor_write_signed(&ws,  (int64_t)H_c);    /* <- cbor.h */
        cbor_write_unsigned(&ws, st);             /* <- cbor.h */
        cbor_close_array(&ws);        /* <- cbor.h */
    }

    cbor_close_array(&ws);  /* <- cbor.h */
    cbor_close_map(&ws);    /* <- cbor.h */

    const size_t out_len = cbor_end_writer(&ws);  /* <- cbor.h */
    if(out_len == 0) {
        coap_set_status_code(res, INTERNAL_SERVER_ERROR_5_00);  /* <- coap-engine.h */
        return;
    }

    coap_set_header_content_format(res, APPLICATION_CBOR);  /* <- coap-engine.h */
    coap_set_payload(res, buf, (uint16_t)out_len);          /* <- coap-engine.h */
}

/**
 * 注册 CoAP 资源 res_ugrid_state
 *
 * 参数：
 *   - GET handler：res_get_state_h（返回 CBOR 编码的微电网状态）
 *   - 其他 handler 均为 NULL（仅支持 GET）
 */
RESOURCE(res_ugrid_state, "title=\"State\"", res_get_state_h, NULL, NULL, NULL);

