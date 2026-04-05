/**
 * ============================================================================
 * uGridController — 微电网控制器（聚合层）
 * ============================================================================
 *
 * 【架构定位】
 *   本文件是 Contiki-NG 操作系统的一个应用程序，运行在聚合层（Aggregation Layer），
 *   作为多个 BatteryController 的"大脑"。它负责：
 *     - 接收 BatteryController 的注册请求
 *     - 订阅（OBSERVE）每个电池的状态变化
 *     - 运行 ML 功率预测模型
 *     - 运行 MPC（模型预测控制）优化充放电策略
 *     - 向每个电池下发功率指令
 *
 * 【与 BatteryController 的区别】
 *   边缘层（BatteryController）：管理单个电池，关注安全保护，毫秒级响应
 *   聚合层（uGridController）：  管理多个电池，关注全局优化，秒级响应
 *   类比：BatteryController = 汽车 ABS 系统
 *         uGridController  = 交通调度中心
 *
 * 【核心功能模块】
 *   1. 环境仿真（update_env）— 模拟 PV 发电、负载消耗、天气变化
 *   2. ML 功率预测（run_mpc）— 调用 emlearn 模型预测未来 PV 和负载
 *   3. MPC 优化（run_mpc）  — 投影梯度下降算法计算每块电池的最优功率
 *   4. 电池管理          — 注册、订阅 OBSERVE、下发功率指令
 *   5. CoAP 资源         — 注册、状态查询、MPC 参数调节、手动目标设定
 *
 * 【主循环流程（每 5 秒）】
 *   更新环境仿真 → ML 功率预测 → MPC 优化 → 向各电池下发功率指令 → 打印状态
 *
 * 【CoAP 资源一览】
 *   POST /dev/register  — 接收 BatteryController 注册
 *   GET  /dev/state     — 返回微电网全局状态（CBOR 格式）
 *   PUT  /ctrl/mpc      — 远程调节 MPC 参数（alpha/beta/gama/price）
 *   GET/PUT /ctrl/obj   — 查看/设置单个电池的手动功率目标
 * ============================================================================
 */

#include "contiki.h"
#include "coap-engine.h"
#include "coap-blocking-api.h"
#include "coap-observe-client.h" 
#include "dev/leds.h"
#include "sys/etimer.h"
#include "sys/log.h"
#include "net/ipv6/uip.h"
#include "net/ipv6/uip-ds6.h"
#include "net/ipv6/uiplib.h"
#include "net/routing/routing.h"
#include "lib/random.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>

#include "../includes/constants.h"
#include "../includes/utility.h"
#include "../includes/power_predictor_model.h"
#include "../includes/project-conf.h"

#define LOG_MODULE "uGrid"
#define LOG_LEVEL LOG_LEVEL_INFO

/* ========================================================================
 * 电池节点管理
 * ======================================================================== */
battery_node_t batteries[MAX_BATTERIES];  /* 已注册电池数组 */
int battery_count = 0;                     /* 当前已注册电池数量 */

/* ========================================================================
 * MPC 优化参数（可通过 /ctrl/mpc 远程调节）
 *   alpha — 电网电价权重
 *   beta  — 电池衰减惩罚权重
 *   gama  — SoC 偏离参考值的惩罚权重
 *   price — 当前电价
 * ======================================================================== */
float alpha = 1.0f;
float beta  = 1.0f;
float gama  = 20.0f;
float price = 0.25f;

/* ========================================================================
 * MPC 算法常量
 * ======================================================================== */
#define FREQ_COMPUTING  CLOCK_SECOND * 5  /* 计算周期：5 秒 */
#define K_FACT          0.05f             /* SoC 变化系数 */
#define SOC_REF         0.5f              /* SoC 参考值（目标 50%） */
#define LEARNING_RATE   0.1f              /* 梯度下降学习率 */
#define PGD_ITERATIONS  100               /* 投影梯度下降迭代次数 */

/* ========================================================================
 * ML 功率预测配置
 * ======================================================================== */
#define ML_PRED_WINDOW 10   /* 预测滑动窗口大小 */
#define N_PRED_FEAT 6       /* 特征数量：辐照度、温度、小时、星期、PV、负载 */
float input_features[ML_PRED_WINDOW * N_PRED_FEAT];  /* ML 输入特征缓冲区 */
float output[2];                                   /* ML 输出：[预测 PV, 预测负载] */

/* ========================================================================
 * 环境仿真变量（模拟真实微电网场景）
 * ======================================================================== */
float curr_load = 2.0f;       /* 当前负载消耗（kW） */
float curr_pv = 0.0f;         /* 当前 PV 发电（kW） */
float curr_hour = 6.0f;       /* 仿真时钟（小时） */
int16_t temp_raw = 0;
float curr_temp = 22.0f;      /* 当前温度（°C） */
float curr_day = 0.5f;        /* 星期（归一化 0~1） */
float cloud_cover = 0.3f;     /* 云量覆盖（0~1） */
bool is_sunny_day = true;     /* 是否晴天 */
float base_load = 2.0f;       /* 基础负载（kW） */
bool high_demand_period = false;  /* 是否高需求时段 */

/* ========================================================================
 * CoAP 资源声明（在 resources/ 目录下定义）
 * ======================================================================== */
extern coap_resource_t 
    res_obj_ctrl,     /* GET/PUT /ctrl/obj: 查看/设置手动功率目标 */
    res_ugrid_state,  /* GET /dev/state: 返回微电网全局状态 */
    res_mpc_params,   /* PUT /ctrl/mpc: 远程调节 MPC 参数 */
    res_register;     /* POST /dev/register: 接收电池注册 */

/* 定时器定义 */
static struct etimer et_compute;  /* <- sys/etimer.h */

PROCESS_NAME(ugrid_controller);

/* ========================================================================
 * 打印所有已注册电池的状态
 * ======================================================================== */
static void print_battery_status(void) {
    LOG_INFO("\n");
    LOG_INFO("===============BATTERY STATUS================\n");
    LOG_INFO("Batteries registered:\t%d/%d\n", battery_count, MAX_BATTERIES);
    LOG_INFO("=============================================\n");

    if(battery_count == 0) {
        LOG_INFO("No batteries registered.\n");
        return;
    }

    for(int i = 0; i < battery_count; i++) {
        if(!batteries[i].active) continue;

        LOG_INFO("\n");
        LOG_INFO("---------------- Battery #%d ----------------\n", i);

        LOG_INFO("IPv6:\t\t");
        LOG_INFO_6ADDR(&batteries[i].ip);
        LOG_INFO_("\n");

        LOG_INFO("State:\t\t%s\n", batteries[i].state == 0 ? "INI" : batteries[i].state == 1 ? "RUN" : "ISO");
        LOG_INFO("Last update:\t%lu s ago\n",
                 (unsigned long)(clock_seconds() - batteries[i].last_update_time));

        /* SoC & SoH */
        LOG_INFO("SoC:\t\t%s%d.%d%%%s\tSoH:\t%d.%d%%\n",
                 batteries[i].current_soc * 100.0f >= 50.0f ? VERDE :
                 (batteries[i].current_soc * 100.0f >= 20.0f ? "" : ROSSO),
                 (int)(batteries[i].current_soc * 100.0f), abs((int)(batteries[i].current_soc * 100.0f * 100.0f)) % 10,
                 RESET,
                 (int)(batteries[i].current_soh * 100.0f), abs((int)(batteries[i].current_soh * 100.0f * 100.0f)) % 10);

        /* Electrical values */
        LOG_INFO("V:\t\t%d.%02d V\tI:\t%+d.%02d A\tT:\t%d.%d C\n",
                 (int)batteries[i].current_voltage, abs((int)(batteries[i].current_voltage * 100.0f)) % 100,
                 (int)batteries[i].current_current, abs((int)(batteries[i].current_current * 100.0f)) % 100,
                 (int)batteries[i].current_temp, abs((int)(batteries[i].current_temp * 100.0f)) % 100);

        /* Power */
        LOG_INFO("Optimal:\t%+d.%02d kW\tActual:\t%+d.%02d kW\n",
                 (int)batteries[i].optimal_u, abs((int)(batteries[i].optimal_u * 100.0f)) % 100,
                 (int)batteries[i].actual_power, abs((int)(batteries[i].actual_power * 100.0f)) % 100);

        /* Tracking error */
        float err = batteries[i].actual_power - batteries[i].optimal_u;
        LOG_INFO("Error:\t\t%s%+d.%02d kW%s\n",
                 fabs(err) < 0.5f ? VERDE : (fabs(err) < 1.0f ? "" : ROSSO),
                 (int)err, abs((int)(err * 100.0f)) % 100,
                 RESET);
    }

    LOG_INFO("\n");
}

/* ========================================================================
 * 环境仿真：模拟 PV 发电、负载消耗、天气变化
 *
 * 这是 uGridController 的"世界模型"，在没有真实硬件时模拟微电网环境：
 *   - PV 发电：基于太阳高度角、云量、湍流模拟
 *   - 负载消耗：基于时段特征（早高峰、午高峰、晚高峰）+ 随机事件
 *   - 天气变化：云量随机漂移，晴天/阴天交替
 *
 * 最后将环境数据归一化后填入 ML 预测特征缓冲区
 * ======================================================================== */
static void update_env() {
    curr_hour += 0.5f;  /* 每次推进 0.5 小时 */
    if(curr_hour >= 24.0f) {
        curr_hour = 0.0f;
        is_sunny_day = (random_rand() % 100) > 30;  /* <- lib/random.h */
        curr_day += 0.1f;
        if (curr_day > 1.0f) {
            curr_day = 0.0f;
        }
    }

    // prediction 
    float base_irradiance_for_ml = 0.0f;

    // model to simulate irradiance and pv
    if (curr_hour >= 6.0f && curr_hour < 18.0f) {
        float sun_elevation = sin(3.14159f * (curr_hour - 6.0f) / 12.0f);
        base_irradiance_for_ml = 1000.0f * sun_elevation;

        cloud_cover += ((random_rand() % 100) / 50.0f - 1.0f) * 0.15f;
        if(cloud_cover < 0.0f) cloud_cover = 0.0f;
        if(cloud_cover > 0.95f) cloud_cover = 0.95f;

        if(!is_sunny_day) {
            cloud_cover = 0.5f + (cloud_cover * 0.5f);
        }

        float cloud_factor = 1.0f - (cloud_cover * 0.85f);
        float turbulence = 1.0f;
        if(cloud_cover > 0.3f) {
            turbulence = 0.7f + ((random_rand() % 100) / 100.0f * 0.6f);
        }

        float effective_irradiance = base_irradiance_for_ml * cloud_factor * turbulence;
        float pv_peak = BAT_MAX_POWER_KW;

        curr_pv = (pv_peak * effective_irradiance / 1000.0f);
        curr_pv += ((random_rand() % 100) / 100.0f - 0.5f) * 0.3f;

        if(curr_pv < 0.0f) curr_pv = 0.0f;
        if(curr_pv > pv_peak) curr_pv = pv_peak;
    } else {
        curr_pv = 0.0f;
        cloud_cover = 0.3f;
    }


    float hour_factor = 1.0f;

    // model to simulate load
    if(curr_hour >= 0.0f && curr_hour < 6.0f) {
        hour_factor = 0.3f + ((random_rand() % 20) / 100.0f);
        high_demand_period = false;
    } 
    else if(curr_hour >= 6.0f && curr_hour < 9.0f) {
        float morning_ramp = (curr_hour - 6.0f) / 3.0f;
        hour_factor = 0.5f + (morning_ramp * 0.7f);
        high_demand_period = (curr_hour >= 7.0f && curr_hour <= 8.5f);
    }
    else if(curr_hour >= 9.0f && curr_hour < 12.0f) {
        hour_factor = 0.9f + ((random_rand() % 30) / 100.0f);
        high_demand_period = false;
    }
    else if(curr_hour >= 12.0f && curr_hour < 14.0f) {
        hour_factor = 1.1f + ((random_rand() % 20) / 100.0f);
        high_demand_period = true;
    }
    else if(curr_hour >= 14.0f && curr_hour < 17.0f) {
        hour_factor = 0.7f + ((random_rand() % 30) / 100.0f);
        high_demand_period = false;
    }
    else if(curr_hour >= 17.0f && curr_hour < 21.0f) {
        hour_factor = 1.3f + ((random_rand() % 40) / 100.0f);
        high_demand_period = true;
    }
    else {
        float evening_ramp = 1.0f - ((curr_hour - 21.0f) / 3.0f);
        hour_factor = 0.4f + (evening_ramp * 0.6f);
        high_demand_period = false;
    }

    float event_load = 0.0f;
    if((random_rand() % 100) < 15) {
        event_load = ((random_rand() % 30) / 10.0f) + 1.0f;
    }

    base_load = 2.5f;
    curr_load = (base_load * hour_factor) + event_load;
    curr_load += ((random_rand() % 100) / 100.0f - 0.5f) * 0.4f;

    if(curr_load < 0.5f) curr_load = 0.5f; 
    if(curr_load > BAT_MAX_POWER_KW * 0.8f) curr_load = BAT_MAX_POWER_KW * 0.8f;

    // update ML buffer
    for (int i=0; i<(ML_PRED_WINDOW-1)*N_PRED_FEAT; i++) {
        input_features[i] = input_features[i+N_PRED_FEAT];
    }

    int idx = (ML_PRED_WINDOW-1)*N_PRED_FEAT;
    input_features[idx] = (base_irradiance_for_ml / MAX_IRR); 
    input_features[idx+1] = curr_temp; // resolution of 0.25oC
    input_features[idx+2] = curr_hour/24.0f; // hour normalized
    input_features[idx+3] = curr_day; 
    input_features[idx+4] = curr_pv/BAT_MAX_POWER_KW;
    input_features[idx+5] = curr_load/BAT_MAX_POWER_KW;
    LOG_INFO("==================CURRENT STATUS==============\n");
    LOG_INFO("Current Load:\t%d.%d kW\n", (int)curr_load, abs((int)(curr_load * 100.0f) % 100));
    LOG_INFO("Current PV:  \t%d.%d kW\n", (int)curr_pv, abs((int)(curr_pv * 100.0f) % 100));

    float net_power = curr_pv - curr_load;
    LOG_INFO("Net Power:   \t%s%d.%d kW%s\n", net_power > 10e-2 ? VERDE : ROSSO, (int)net_power, abs((int)(net_power * 100.0f) % 100), RESET);
}

/* ========================================================================
 * MPC 优化：投影梯度下降算法
 *
 * 目标函数：min Σ(alpha * price * u_i + beta * u_i² + gama * (soc_i + K*u_i - soc_ref)²)
 *   - 第一项：电网电价成本
 *   - 第二项：电池衰减惩罚（功率越大衰减越快）
 *   - 第三项：SoC 偏离参考值的惩罚（保持电池在健康区间）
 *
 * 约束：|u_i| <= BAT_MAX_POWER_KW（功率物理限制）
 *
 * 算法：Projected Gradient Descent（投影梯度下降）
 *   每轮迭代：计算梯度 → 沿负梯度方向更新 → 投影到可行域（钳位）
 *   迭代 100 次后停止（轻量级，适合嵌入式设备）
 * ======================================================================== */
static void run_mpc() {

    LOG_INFO("\n");
    LOG_INFO("================MPC OPTIMIZATION==============\n");

    /* 调用 ML 模型预测未来 PV 发电和负载消耗 */
    power_predictor_regress(input_features, ML_PRED_WINDOW * N_PRED_FEAT, output, 2);  /* <- includes/power_predictor_model.h (emlearn) */
    
    // clamp values 
    if ( output[0] < 0 ) {
        output[0] = 0;
    }

    if ( output[1] < 0 ) {
        output[1] = 0;
    }

    LOG_INFO("Predicted PV:\t%d.%d kW\n",
            (int)output[0], (int)(output[0] * 100.0f) % 100 );
    LOG_INFO("Predicted load:\t%d.%d kW\n",
            (int)output[1], (int)(output[1] * 100.0f) % 100 );

    // compute available power from batteries
    float avg_soc = 0.0f;
    for(int i = 0; i < battery_count; i++) {
        if(batteries[i].active) {
            avg_soc += batteries[i].current_soc;
        }
    }
    if(battery_count > 0) avg_soc /= battery_count;


    float avg_soc_pct = avg_soc * 100.0f;
    LOG_INFO("Avg SoC:\t%d.%02d%%\n",
            (int)avg_soc_pct, abs((int)(avg_soc_pct*100.0f))%100);


    // lightweight projected gradient algorithm:
    // mathematical tractation in chapter 2.3.2 of documentation
    // stop condition: static number of iteration
    for (int iter = 0; iter < PGD_ITERATIONS; iter++) {
        
        for (int i = 0; i < battery_count; i++) {
            
            // consider only active batteries
            if (!batteries[i].active) continue;
            
            // consider only non isolated batteries
            if (batteries[i].state == STATE_ISOLATED) continue;
            
            // consider only batteries that do not actually have an objective set
            if (batteries[i].has_objective) continue;

            float u = batteries[i].optimal_u;
            float soc_term = batteries[i].current_soc + (K_FACT * u) - SOC_REF;
            float grad = (alpha * price) + (2.0f * beta  * u) + (2.0f * gama * K_FACT * soc_term);

            u = u - (LEARNING_RATE * grad);
            if (u > BAT_MAX_POWER_KW)  u = BAT_MAX_POWER_KW;
            if (u < -BAT_MAX_POWER_KW) u = -BAT_MAX_POWER_KW;

            batteries[i].optimal_u = u;
        }

    }
    
    LOG_INFO("\n");
    LOG_INFO("===========OPTIMIZATION RESULTS===============\n");


    float total_command = 0;
    for (int i = 0; i < battery_count; i++) {
        if (!batteries[i].active) continue;


        float cmd_kw = batteries[i].has_objective
            ? batteries[i].objective_power
            : batteries[i].optimal_u;

        total_command += cmd_kw;


        int cmd_int = (int)cmd_kw;
        int cmd_dec = (int)((cmd_kw - cmd_int) * 100.0f);
        if (cmd_dec < 0) cmd_dec = -cmd_dec;

        int soc_int = (int)(batteries[i].current_soc * 100.0f);
        int soc_dec = (int)((batteries[i].current_soc * 100.0f - soc_int) * 100.0f);
        if (soc_dec < 0) soc_dec = -soc_dec;

        LOG_INFO("Battery #%d:\t%s%+d.%02d kW%s  (SoC = %d.%02d%%)  [%s]\n", 
                i,
                cmd_kw > 0 ? VERDE : ROSSO,
                cmd_int, cmd_dec,
                RESET,
                soc_int, soc_dec,
                batteries[i].has_objective ? "OBJ" : "MPC");
    }

    float expected_grid = curr_load - curr_pv + total_command;

    LOG_INFO("Expected:%s\t\t%d.%d kW %s", expected_grid > 0 ? ROSSO : VERDE, (int)expected_grid, abs((int)(expected_grid * 100.0f)) %100, RESET);
    if(fabs(expected_grid) < 0.5f) {
        LOG_INFO_("(Balanced)\n");
    } else if(expected_grid > 0) {
        LOG_INFO_("(Import)\n");
    } else {
        LOG_INFO_("(Export)\n");
    }

}


/* ========================================================================
 * 电池状态通知回调（CoAP OBSERVE 客户端）
 *
 * 当 BatteryController 的 /dev/state 资源发生变化时，
 * 此回调函数会被 Contiki 的 OBSERVE 客户端机制自动调用。
 * 它解析 JSON payload 并更新对应电池节点的本地状态。
 * ======================================================================== */
    static void 
battery_notification_handler(coap_observee_t *obs,
        void *notification, coap_notification_flag_t flag)
{
    if(!notification) {
        LOG_WARN("[OBSERVE] NULL notification (flag=%d)\n", flag);  /* <- sys/log.h */
        return;
    }

    const uint8_t *payload = NULL;
    int len = coap_get_payload(notification, &payload);  /* <- coap-engine.h */
    if(len <= 0) return;

    static char s[128];
    if(len >= (int)sizeof(s)) len = sizeof(s) - 1;
    memcpy(s, payload, len);  /* <- string.h */
    s[len] = '\0';

    int voltage=0, current=0, temperature=0, soc=0, soh=0, state=0;
    int n = sscanf(s,
            "{\"V\":%d,\"I\":%d,\"T\":%d,\"S\":%d,\"H\":%d,\"St\":%d}",
            &voltage, &current, &temperature, &soc, &soh, &state);  /* <- stdio.h */

    if(n != 6) {
        LOG_WARN("[OBS] Bad payload (sscanf=%d): %s\n", n, s);  /* <- sys/log.h */
        return;
    }

    for(int i=0; i<battery_count; i++) {
        if(uip_ipaddr_cmp(&batteries[i].ip, &obs->endpoint.ipaddr)) {  /* <- net/ipv6/uip.h */
            batteries[i].current_soc     = (float)soc / 10000.0f;
            batteries[i].current_voltage = (float)voltage / 100.0f;
            batteries[i].current_temp    = (float)temperature / 100.0f;
            batteries[i].current_soh     = (float)soh / 10000.0f;
            batteries[i].current_current = (float)current / 100.0f;
            batteries[i].actual_power    = (float)(voltage * current) / 10000000.0f;
            batteries[i].last_update_time = clock_seconds();  /* <- sys/clock.h */
            batteries[i].state = state;
            break;
        }
    }
}


/* 空回调：用于 COAP_BLOCKING_REQUEST 发送功率指令后不需要处理响应 */
static void empty_cb(coap_message_t *response) {
    (void)response;
    /* Silent callback */
}

/* ========================================================================
 * Contiki-NG 进程定义
 *
 * 生命周期：
 *   1. 初始化：激活 4 个 CoAP 资源（注册、状态、MPC 参数、手动目标）
 *   2. 主循环（每 5 秒）：
 *      - 更新环境仿真（PV/负载/天气）
 *      - ML 功率预测
 *      - MPC 优化计算
 *      - 向每个非隔离电池下发功率指令（CoAP PUT /dev/power）
 *      - 打印电池状态汇总
 *   3. 消息事件（PROCESS_EVENT_MSG）：
 *      - 新电池注册后触发，为该电池建立 OBSERVE 订阅
 * ======================================================================== */
PROCESS(ugrid_controller, "uGrid");
AUTOSTART_PROCESSES(&ugrid_controller);

PROCESS_THREAD(ugrid_controller, ev, data) {
    static coap_endpoint_t ep;
    static coap_message_t req[1];
    static char pl[32];
    static int i; 

    PROCESS_BEGIN();

    /* 避免 emlearn 符号被链接器优化掉 */
    printf("%p\n", eml_error_str);                      /* <- stdio.h + includes/power_predictor_model.h */
    printf("%p\n", eml_net_activation_function_strs);   /* <- stdio.h + includes/power_predictor_model.h */

    leds_on(LEDS_GREEN);  /* <- dev/leds.h */

    /* 注册 4 个 CoAP 资源 */
    coap_activate_resource(&res_register, "dev/register");    /* <- coap-engine.h */
    coap_activate_resource(&res_ugrid_state, "dev/state");    /* <- coap-engine.h */
    coap_activate_resource(&res_mpc_params, "ctrl/mpc");      /* <- coap-engine.h */
    coap_activate_resource(&res_obj_ctrl, "ctrl/obj");        /* <- coap-engine.h */

    LOG_INFO("[INIT] CoAP resources activated\n");            /* <- sys/log.h */
    LOG_INFO("[INIT] Ready to accept battery registrations\n");
    LOG_INFO("\n");

    etimer_set(&et_compute, FREQ_COMPUTING);  /* <- sys/etimer.h */

    while(1) {
        PROCESS_WAIT_EVENT();  /* <- contiki.h */

        /* ====================================================================
         * 定时器事件：每 5 秒执行一次环境仿真 + MPC 优化 + 功率下发
         * ==================================================================== */
        if(ev == PROCESS_EVENT_TIMER && data == &et_compute) {
            leds_on(LEDS_BLUE);  /* <- dev/leds.h */
            update_env(); 
            run_mpc(); 

            LOG_INFO("\n");
            LOG_INFO("===========OPTIMIZATION RESULTS===============\n");

            /* 向每个已注册电池下发功率指令 */
            for(i = 0; i < battery_count; i++) {
                if (!batteries[i].active) continue;

                /* 已隔离的电池不发送指令 */
                if (batteries[i].state == STATE_ISOLATED) {
                    LOG_INFO("Battery #%d: state=ISO, skipping command\n", i);  /* <- sys/log.h */
                    continue;
                }

                /* 构建 CoAP PUT 请求目标端点 */
                memset(&ep, 0, sizeof(ep));                          /* <- string.h */
                memset(req, 0, sizeof(coap_message_t));              /* <- string.h */
                uip_ipaddr_copy(&ep.ipaddr, &batteries[i].ip);       /* <- net/ipv6/uip.h */
                ep.port = UIP_HTONS(COAP_DEFAULT_PORT);

                coap_init_message(req, COAP_TYPE_CON, COAP_PUT, 0);  /* <- coap-engine.h */
                coap_set_header_uri_path(req, "dev/power");          /* <- coap-engine.h */

                /* 优先使用手动目标，否则使用 MPC 计算结果 */
                float cmd_kw = batteries[i].has_objective
                    ? batteries[i].objective_power
                    : batteries[i].optimal_u;

                int cmd_scaled = (int)(cmd_kw * 1000);
                snprintf(pl, sizeof(pl), "{\"u\":%d}", cmd_scaled);  /* <- stdio.h */
                coap_set_payload(req, (uint8_t*)pl, strlen(pl));     /* <- coap-engine.h */

                LOG_INFO("Battery #%d: [%s]: %s%d.%d kW%s\n",       /* <- sys/log.h */
                        i,
                        batteries[i].has_objective ? "OBJ" : "MPC",
                        cmd_kw > 0 ? VERDE : ROSSO,
                        (int)(cmd_kw), abs((int)(cmd_kw * 100.0f)) % 100,
                        RESET );

                /* 发送 CoAP PUT 请求（阻塞等待响应） */
                COAP_BLOCKING_REQUEST(&ep, req, empty_cb);           /* <- coap-blocking-api.h */
            }

            print_battery_status();

            etimer_reset(&et_compute);  /* <- sys/etimer.h */
            leds_off(LEDS_BLUE);        /* <- dev/leds.h */
        }

        /* ====================================================================
         * 消息事件：新电池注册后触发，为该电池建立 OBSERVE 订阅
         * ==================================================================== */
        if(ev == PROCESS_EVENT_MSG) {

            for(i = 0; i < battery_count; i++) {
                if(batteries[i].active && !batteries[i].obs_requested) {
                    memset(&ep, 0, sizeof(ep));                          /* <- string.h */
                    uip_ipaddr_copy(&ep.ipaddr, &batteries[i].ip);       /* <- net/ipv6/uip.h */
                    ep.port = UIP_HTONS(COAP_DEFAULT_PORT);

                    LOG_INFO("[OBSERVE] Setting up observation for Battery #%d: ", i);  /* <- sys/log.h */
                    LOG_INFO_6ADDR(&batteries[i].ip);                    /* <- sys/log.h */
                    LOG_INFO_("\n");

                    /* 向电池发送 CoAP OBSERVE 订阅请求 */
                    batteries[i].obs = coap_obs_request_registration(    /* <- coap-observe-client.h */
                            &ep, 
                            "dev/state", 
                            battery_notification_handler, 
                            NULL
                            );

                    if(batteries[i].obs != NULL) {
                        LOG_INFO("[OBSERVE] ✓ Observation registered successfully for Battery #%d\n", i);  /* <- sys/log.h */
                    } else {
                        LOG_WARN("[OBSERVE] ✗ Failed to register observation for Battery #%d\n", i);  /* <- sys/log.h */
                    }

                    batteries[i].obs_requested = true;
                }
            }
        }
    }
    PROCESS_END();
}
