/**
 * ============================================================================
 * BatteryController — 电池控制器（边缘层）
 * ============================================================================
 *
 * 【架构定位】
 *   本文件是 Contiki-NG 操作系统的一个应用程序，运行在边缘层（Edge Layer），
 *   目标硬件为 nRF52840 等低功耗 MCU。它不是一个独立的 Linux 程序，
 *   必须依赖 Contiki-NG 的运行时环境才能工作。
 *
 * 【为什么不能脱离 Contiki-NG 运行？】
 *   本文件依赖以下 Contiki-NG 专属组件：
 *     - PROCESS_THREAD / AUTOSTART_PROCESSES — Contiki 的协程调度模型
 *     - coap-engine / coap-blocking-api   — Contiki 内置的 CoAP 协议栈
 *     - etimer / ctimer                   — Contiki 的事件/回调定时器
 *     - uip / uip-ds6                    — Contiki 的 IPv6 协议栈
 *     - dev/leds / dev/button-hal         — Contiki 的硬件抽象层
 *   编译产物本质上是 Contiki-NG 的"插件"，必须由 contiki_main() 启动并调度。
 *
 * 【核心功能模块】
 *   1. 电池物理仿真  — 根据功率指令动态计算 V/I/T/SoC 变化（update_sensors_and_buffer）
 *   2. SoH 衰减模型  — 综合循环/温度/应力/C-rate 等多因素计算容量衰减
 *   3. ML 推理       — 调用 emlearn 导出的 C 头文件模型估算 SoH（check_safety）
 *   4. 安全检查      — 超阈值时自动隔离电池，按钮触发恢复出厂状态
 *   5. CoAP 通信     — 向 uGridController 注册、上报状态（OBSERVABLE）、接收功率指令
 *
 * 【主循环流程（每 5 秒）】
 *   更新传感器 → 运行 ML 估算 SoH → 安全检查 → 通知 uGridController → 等待下一周期
 *
 * 【分层架构说明】
 *   边缘层（本文件）vs 聚合层（uGridController）的区别：
 *     - 边缘层：管理单个电池，毫秒级安全决策，KB 级内存，叶子节点
 *     - 聚合层：管理多个电池 + 逆变器，秒级全局优化，MB/GB 级内存，网关节点
 *   类比：BatteryController = 汽车 ABS 系统（独立快速反应）
 *         uGridController  = 交通调度中心（全局协调）
 * ============================================================================
 */

#include "contiki.h"
#include "coap-engine.h"
#include "coap-blocking-api.h"
#include "os/net/linkaddr.h"
#include "dev/leds.h"
#include "dev/button-hal.h"
#include "sys/etimer.h"
#include "sys/log.h"
#include "net/ipv6/uip.h"
#include "net/ipv6/uip-ds6.h"
#include "net/ipv6/uiplib.h"
#include "net/routing/routing.h"
#include "lib/random.h"
#include <stdio.h>
#include <stdlib.h>
#include <math.h>
#include <string.h>
#include "coap-engine.h"
#include "coap.h"

#include "../includes/utility.h"
#include "../includes/constants.h"
#include "../includes/battery_soh_model.h"
#include "project-conf.h"

#define LOG_MODULE "BatCtrl"
#define LOG_LEVEL LOG_LEVEL_INFO

/* ========================================================================
 * 工具函数
 * ======================================================================== */

/* 生成指定幅度的随机噪声，用于模拟传感器波动 */
float get_random_noise(float magnitude) {
    return ((random_rand() % 100) / 50.0f - 1.0f) * magnitude;  /* <- lib/random.h */
}


/* ========================================================================
 * 全局状态变量
 * ======================================================================== */

/* 缩放配置（模拟 100 节电芯并联的家用 13.5kWh 电池包） */
#define POWER_SCALE_FACTOR 1000.0f  /* 功率缩放因子：每单位 1kW */
#define SCALED_CAPACITY_AH 200.0f   /* 缩放容量：200Ah */

/* 电池当前状态机：INIT → RUNNING / ISOLATED */
battery_state_t current_state = STATE_INIT;

/* 电池实时物理参数 */
float bat_voltage = 3.7f;
float bat_current = 0.0f;
float bat_temp = 25.0f;
float bat_soc = 0.8f;
float bat_soh = 1.0f;
float bat_capacity_ah = SCALED_CAPACITY_AH;
float power_setpoint = 0.0f;   /* uGridController 下发的功率指令（W） */
int battery_id = 1;            /* 应用层设备 ID */

/* ========================================================================
 * 安全阈值配置
 * ======================================================================== */
float soh_critical = 0.65f;      /* 低于此值 → CRITICAL，立即隔离 */
float soh_warning  = 0.75f;      /* 低于此值 → WARNING 告警 */
float temp_critical = 60.0f;     /* 高于此值 → CRITICAL，立即隔离 */
float temp_warning  = 50.0f;     /* 高于此值 → WARNING 告警 */
uint32_t cycles_warning = 100;   /* 超过此循环次数 → WARNING 告警 */

/* ========================================================================
 * ML 推理缓冲区
 * ======================================================================== */
float ml_buffer[ML_WINDOW * N_FEATURES];  /* 滑动窗口传感器数据 */
float output[1];                           /* ML 模型输出（SoH 估算值） */

/* ========================================================================
 * 电池老化追踪变量
 * ======================================================================== */
uint32_t charge_cycles = 0;       /* 充放电循环次数 */
float total_ah_throughput = 0.0f; /* 累计 Ah 吞吐量 */
float peak_temp_reached = 25.0f;  /* 历史最高温度 */
uint8_t was_charging = false;     /* 上一次是否处于充电状态（用于计数循环） */

/* ========================================================================
 * CoAP 资源声明（在 resources/ 目录下定义）
 * ======================================================================== */
extern coap_resource_t
    res_dev_state,    /* GET + OBSERVE: 返回电池状态 */
    res_dev_power;    /* PUT: 接收 uGridController 的功率指令 */

/* ========================================================================
 * 定时器定义
 * ======================================================================== */
struct etimer et_loop, et_init_wait, et_notify;  /* 事件定时器 */
struct ctimer ct_led_blink;                       /* 回调定时器（LED 闪烁） */

/* ========================================================================
 * LED 状态指示
 *   INIT 状态：黄色闪烁
 *   ISOLATED 状态：红色闪烁
 *   RUNNING 状态：绿色=充电，红色=放电，蓝色=空闲
 * ======================================================================== */
void led_blink(void * pt) {
    if (current_state == STATE_INIT) {
        leds_toggle(LEDS_YELLOW);          /* <- dev/leds.h */
        ctimer_reset(&ct_led_blink);       /* <- sys/ctimer.h (via contiki.h) */
    } else if (current_state == STATE_ISOLATED) {
        leds_toggle(LEDS_RED);
        ctimer_reset(&ct_led_blink);
    }
}


/* 根据功率指令更新 LED 状态 */
void update_leds() {
    leds_off(~0);                          /* <- dev/leds.h */
    if (current_state == STATE_RUNNING ) {
        if (power_setpoint > 0.5f) {
            leds_on(LEDS_GREEN);           /* <- dev/leds.h */
        } else if (power_setpoint < -0.5f) {
            leds_on(LEDS_RED);             /* <- dev/leds.h */
        } else {
            leds_on(LEDS_BLUE);            /* <- dev/leds.h */
        }
    }
}

/* 打印电池状态到日志 */
static void print_battery_status(void) {
    int v = (int)lroundf(bat_voltage*100.0f);

    LOG_INFO("V:%d.%d, I:%d.%d, T:%d.%d, SoC: %d.%d, SoH: %d.%d\n",
           v/100, abs(v)%100,
           (int)(bat_current) / 100,(int)(bat_current) % 100,
           (int)(bat_temp) / 100,(int)(bat_temp) % 100,
           (int)(bat_soc*100.0f) / 100,(int)(bat_soc*100.0f) % 100,
           (int)(bat_soh*100.0f) / 100,(int)(bat_soh*100.0f) % 100);
}

/* ========================================================================
 * 电池物理仿真 + ML 特征缓冲区更新
 *
 * 这是本文件最核心的函数，模拟了一个真实 Li-ion 电池包的物理行为：
 *   步骤 0：根据 SoC 限制功率（防过充/过放降额）
 *   步骤 1：从功率指令计算电流（含噪声）
 *   步骤 2：更新电压（OCV + 内阻压降 + SoC 边缘修正）
 *   步骤 3：更新 SoC（考虑充放电效率）
 *   步骤 4：更新温度（I²R 发热 - 散热）
 *   步骤 5：更新 SoH（多因素衰减模型）
 *
 * 最后将 V/I/T/SoC 归一化后填入 ML 滑动窗口缓冲区
 * ======================================================================== */
static void update_sensors_and_buffer() {
    if (current_state == STATE_RUNNING) {
        /* Parametri fisici della batteria Li-ion SCALATA (Pacco Domestico 13.5kWh) */
        const float BATTERY_CAPACITY_AH = SCALED_CAPACITY_AH;  /* Capacità scalata: 200Ah */
        const float NOMINAL_VOLTAGE = 3.7f;                     /* Tensione nominale [V] - manteniamo bassa per ML */
        const float V_MIN = 3.0f;                               /* Tensione minima scarica [V] */
        const float V_MAX = 4.2f;                               /* Tensione massima carica [V] */
        const float INTERNAL_RESISTANCE = 0.0008f;              /* Resistenza interna [Ω] - scalata (0.08/100) */
        const float THERMAL_MASS = 5000.0f;                     /* Massa termica scalata [J/°C] - pacco grande */
        const float HEAT_DISSIPATION = 200.0f;                  /* Coefficiente dissipazione [W/°C] - scalato */
        const float AMBIENT_TEMP = 25.0f;                       /* Temperatura ambiente [°C] */
        const float EFFICIENCY = 0.92f;                         /* Efficienza conversione - più realistica */
        
        /* Timestep di aggiornamento (1 secondo) */
        const float dt = 1.0f; /* [s] */

        /*
         * 0. LEGA LA POTENZA EROGABILE ALLO STATE OF CHARGE
         */
        const float SOC_EMPTY_CUTOFF      = 0.02f;  /* sotto 2% vietata scarica */
        const float SOC_DERATE_DISCHARGE  = 0.10f;  /* sotto 10% scarica deratata */
        const float SOC_FULL_CUTOFF       = 0.98f;  /* sopra 98% vietata carica */
        const float SOC_DERATE_CHARGE     = 0.90f;  /* sopra 90% carica deratata */

        float effective_power = power_setpoint;

        int32_t soc_permil = (int32_t)(bat_soc * 1000.0f);
        int32_t soc_pct_tenths = soc_permil;
        int32_t soc_pct_int = soc_pct_tenths / 10;
        int32_t soc_pct_dec = (soc_pct_tenths >= 0 ? soc_pct_tenths : -soc_pct_tenths) % 10;

        /* Comando di SCARICA (potenza negativa) */
        if (effective_power < -0.5f) {
            if (bat_soc <= SOC_EMPTY_CUTOFF) {
                effective_power = 0.0f;
            } else if (bat_soc < SOC_DERATE_DISCHARGE) {
                float scale = (bat_soc - SOC_EMPTY_CUTOFF) /
                              (SOC_DERATE_DISCHARGE - SOC_EMPTY_CUTOFF);
                if (scale < 0.0f) scale = 0.0f;
                effective_power *= scale;
            }
        }

        /* Comando di CARICA (potenza positiva) */
        if(effective_power > 0.5f) {
            if(bat_soc >= SOC_FULL_CUTOFF) {
                LOG_WARN("[LIMIT] SoC=%ld.%01ld%% -> carica vietata, forzo 0W\n",
                         (long)soc_pct_int, (long)soc_pct_dec);
                effective_power = 0.0f;
            } else if(bat_soc > SOC_DERATE_CHARGE) {
                float scale = (SOC_FULL_CUTOFF - bat_soc) /
                              (SOC_FULL_CUTOFF - SOC_DERATE_CHARGE);
                if(scale < 0.0f) scale = 0.0f;
                float old = effective_power;
                effective_power *= scale;

                int32_t old_w = (int32_t)(old);
                int32_t new_w = (int32_t)(effective_power);

                LOG_INFO("[LIMIT] SoC=%ld.%01ld%% -> derating carica: %ldW -> %ldW\n",
                         (long)soc_pct_int, (long)soc_pct_dec,
                         (long)old_w, (long)new_w);
            }
        }

        power_setpoint = effective_power;
        
        /* 1. CALCOLA CORRENTE dalla potenza richiesta */
        float ocv = V_MIN + (V_MAX - V_MIN) * bat_soc;
        float requested_current = (ocv > 0.1f) ? (power_setpoint / ocv) : 0.0f;
        float current_noise = get_random_noise(0.02f * fabs(requested_current));
        bat_current = requested_current + current_noise;
        
        /* Limita corrente in base a C-rate */
        float max_current = BATTERY_CAPACITY_AH * 15.0f;
        if(bat_current > max_current) bat_current = max_current;
        if(bat_current < -max_current) bat_current = -max_current;
        
        /* 2. AGGIORNA TENSIONE */
        bat_voltage = ocv - (bat_current * INTERNAL_RESISTANCE);
        
        if(bat_soc < 0.1f) {
            bat_voltage -= (0.1f - bat_soc) * 2.0f;
        }
        if(bat_soc > 0.9f) {
            bat_voltage += (bat_soc - 0.9f) * 0.5f;
        }
        
        if(bat_voltage > V_MAX) bat_voltage = V_MAX;
        if(bat_voltage < V_MIN) bat_voltage = V_MIN;
        bat_voltage += get_random_noise(0.01f);
        
        /* 3. AGGIORNA STATE OF CHARGE */
        float efficiency = (bat_current > 0) ? EFFICIENCY : (1.0f / EFFICIENCY);
        float energy_joules = power_setpoint * efficiency * dt;
        float current_capacity_ah = BATTERY_CAPACITY_AH * bat_soh;
        float capacity_joules = current_capacity_ah * NOMINAL_VOLTAGE * 3600.0f;
        float delta_soc = energy_joules / capacity_joules;
        bat_soc += delta_soc;
        
        float ah_transferred = fabs(bat_current) * (dt / 3600.0f);
        total_ah_throughput += ah_transferred;
        
        bool is_charging = (bat_current > 0.5f);
        if(is_charging && !was_charging && bat_soc < 0.5f) {
            charge_cycles++;
        }
        was_charging = is_charging;
        
        if(bat_soc > 1.0f) bat_soc = 1.0f;
        if(bat_soc < 0.0f) bat_soc = 0.0f;
        
        /* 4. AGGIORNA TEMPERATURA */
        float power_loss = bat_current * bat_current * INTERNAL_RESISTANCE;
        float heat_generated = power_loss * dt;
        float heat_dissipated = HEAT_DISSIPATION * (bat_temp - AMBIENT_TEMP) * dt;
        float delta_temp = (heat_generated - heat_dissipated) / THERMAL_MASS;
        bat_temp += delta_temp;
        bat_temp += get_random_noise(0.5f);
        
        if(bat_temp > peak_temp_reached) {
            peak_temp_reached = bat_temp;
        }
        
        if(bat_temp < 0.0f) bat_temp = 0.0f;
        if(bat_temp > 80.0f) bat_temp = 80.0f;
        
        /* 5. AGGIORNA CAPACITÀ (SoH) */
        float cycle_degradation = charge_cycles * 0.0008f;
        float throughput_degradation = total_ah_throughput * 0.00005f;
        
        float temp_degradation = 0.0f;
        if(bat_temp > 40.0f) {
            temp_degradation = (bat_temp - 40.0f) * 0.0001f;
        }
        if(bat_temp > 55.0f) {
            temp_degradation += (bat_temp - 55.0f) * 0.0005f;
        }
        
        float soc_stress_degradation = 0.0f;
        if(bat_soc < 0.15f) {
            soc_stress_degradation = (0.15f - bat_soc) * 0.0002f;
        }
        if(bat_soc > 0.95f) {
            soc_stress_degradation = (bat_soc - 0.95f) * 0.0001f;
        }
        
        float c_rate = fabs(bat_current) / BATTERY_CAPACITY_AH;
        float c_rate_degradation = 0.0f;
        if(c_rate > 3.0f) {
            c_rate_degradation = (c_rate - 3.0f) * 0.00003f;
        }
        
        float total_degradation = cycle_degradation + throughput_degradation + 
                                 temp_degradation + soc_stress_degradation + 
                                 c_rate_degradation;
        
        bat_soh -= total_degradation * dt;
        
        if(bat_soh > 1.0f) bat_soh = 1.0f;
        if(bat_soh < 0.5f) bat_soh = 0.5f;
        
        bat_capacity_ah = BATTERY_CAPACITY_AH * bat_soh;
    }

    /* Aggiorna buffer ML (ancora float) */
    for(int i=0; i<(ML_WINDOW-1)*N_FEATURES; i++) {
        ml_buffer[i] = ml_buffer[i+N_FEATURES];
    }
    int idx = (ML_WINDOW-1)*N_FEATURES;
    ml_buffer[idx] = bat_voltage / 4.2f;
    ml_buffer[idx+1] = ((bat_current + 10.0f) / 20.0f);
    ml_buffer[idx+2] = bat_temp / 80.0f;
    ml_buffer[idx+3] = bat_soc;
}

/* ========================================================================
 * 安全检查 + ML SoH 推理
 *
 * 1. 调用 emlearn 模型（ML 推理，C 头文件中内联实现）
 * 2. ML 输出 + 物理模型 SoH 加权融合（70% ML + 30% 物理）
 * 3. 温度/SoC 应力修正
 * 4. 三级安全检查：
 *    - CRITICAL（SoH < 65% 或 T > 60°C）→ 立即隔离电池
 *    - WARNING（SoH < 75% 或 T > 50°C 或循环 > 100）→ 日志告警
 *    - OK → 正常运行
 * 5. 隔离后只能通过物理按钮恢复出厂状态
 * ======================================================================== */
static void check_safety() {

    battery_soh_regress(ml_buffer, ML_WINDOW*N_FEATURES, output, 1);  /* <- includes/battery_soh_model.h (emlearn) */

    // clamp output to acceptable values
    output[0] = output[0] < 0 ? 0 : output[0];
    output[0] = output[0] > 100 ? 100 : output[0];

    float ml_soh = output[0] / 100.0f;  // 0..1
    float combined_soh = (ml_soh*0.7f) + (bat_soh*0.3f);
    
    // thermal effect
    if(bat_temp > 45.0f) {
        combined_soh -= (bat_temp - 45.0f) * 0.001f;
    }
    
    // low state of charge effect
    if(bat_soc < 0.1f) {
        combined_soh -= (0.1f - bat_soc) * 0.02f;
    }
    
    // to high or too low combined soh
    if(combined_soh > 1.0f) combined_soh = 1.0f;
    if(combined_soh < 0.5f) combined_soh = 0.5f;
    
    // final battery soh and capacity updated
    bat_soh = (bat_soh * 0.95f) + (combined_soh * 0.05f);
    bat_capacity_ah = SCALED_CAPACITY_AH * bat_soh;

    int32_t soh_permil = (int32_t)(bat_soh * 1000.0f);
    int32_t soh_pct_int = soh_permil / 10;
    int32_t soh_pct_dec = (soh_permil >= 0 ? soh_permil : -soh_permil) % 10;

    int32_t cap_milliah = (int32_t)(bat_capacity_ah * 1000.0f);
    int32_t cap_int = cap_milliah / 1000;
    int32_t cap_dec3 = (cap_milliah >= 0 ? cap_milliah : -cap_milliah) % 1000;

    int32_t t_deci = (int32_t)(bat_temp * 10.0f);
    int32_t t_int = t_deci / 10;
    int32_t t_dec = (t_deci >= 0 ? t_deci : -t_deci) % 10;

    bool critical_soh = (bat_soh < soh_critical);
    bool critical_temp = (bat_temp > temp_critical);
    bool warning_soh = (bat_soh < soh_warning);
    bool warning_temp = (bat_temp > temp_warning);
    bool warning_cycles = (charge_cycles > cycles_warning);
    
    if(critical_soh || critical_temp) {
        LOG_INFO_("CRITICAL ✗\n");          /* <- sys/log.h */
        LOG_ERR("!!! SAFETY CRITICAL !!! Isolating battery\n");  /* <- sys/log.h */
        LOG_ERR("    Reason: ");            /* <- sys/log.h */
        if(critical_soh) {
          LOG_ERR_("SoH=%ld.%01ld%% (min 75%%) ",  /* <- sys/log.h */
                   (long)soh_pct_int, (long)soh_pct_dec);
        }
        if(critical_temp) {
          LOG_ERR_("Temp=%ld.%01ld°C (max 60°C)", (long)t_int, (long)t_dec);  /* <- sys/log.h */
        }
        LOG_ERR_("\n");                     /* <- sys/log.h */
        LOG_ERR("Press button to reset battery to factory conditions\n");  /* <- sys/log.h */
        
        current_state = STATE_ISOLATED;
        power_setpoint = 0.0f;
        bat_current = 0.0f;
        coap_notify_observers(&res_dev_state);  /* <- coap-engine.h */
        
        ctimer_reset(&ct_led_blink);        /* <- sys/ctimer.h */

        leds_off(LEDS_ALL);                 /* <- dev/leds.h */
        leds_toggle(LEDS_RED);              /* <- dev/leds.h */
        
    } else if (warning_soh || warning_temp || warning_cycles) {
        LOG_INFO_("WARNING ⚠\n");           /* <- sys/log.h */
        if(warning_soh) {
            LOG_WARN("Battery degradation: SoH=%ld.%01ld%% (%ld.%03ld Ah remaining)\n",  /* <- sys/log.h */
                     (long)soh_pct_int, (long)soh_pct_dec,
                     (long)cap_int, (long)cap_dec3);
        }
        if (warning_temp) {
            int32_t peak_deci = (int32_t)(peak_temp_reached * 10.0f);
            int32_t peak_int = peak_deci / 10;
            int32_t peak_dec = (peak_deci >= 0 ? peak_deci : -peak_deci) % 10;
            LOG_WARN("High temperature: %ld.%01ld°C (peak: %ld.%01ld°C)\n",  /* <- sys/log.h */
                     (long)t_int, (long)t_dec,
                     (long)peak_int, (long)peak_dec);
        }
        if(warning_cycles) {
            LOG_WARN("High cycle count: %lu cycles completed\n",  /* <- sys/log.h */
                     (unsigned long)charge_cycles);
        }
    } else {
        LOG_INFO("OK\n");                   /* <- sys/log.h */
    }
}

/* CoAP 注册回调：收到 uGridController 的 ACK 后进入 RUNNING 状态 */
static void reg_callback(coap_message_t *response) {

    if(response) {
        LOG_INFO("[INIT] Registration ACK received");
        if(response->code == CREATED_2_01 || response->code == CHANGED_2_04) {
            LOG_INFO("[INIT] Registration SUCCESS\n");
            current_state = STATE_RUNNING;
            update_leds();
            print_battery_status();
        } else {
            LOG_WARN("[INIT] Unexpected response code: %d\n", response->code);
        }
    } else {
        LOG_WARN("[INIT] Registration TIMEOUT - will retry\n");
    }
}

/* ========================================================================
 * Contiki-NG 进程定义
 *
 * PROCESS() + AUTOSTART_PROCESSES() 是 Contiki 的宏，等价于声明一个
 * 协程入口函数。这个进程会在 Contiki 事件循环中被调度执行。
 *
 * 生命周期：
 *   1. 初始化：启动 LED 定时器 → 注册 CoAP 资源
 *   2. 注册阶段：向 uGridController 发送 POST /dev/register（循环重试直到成功）
 *   3. 主循环（每 5 秒）：
 *      - 更新传感器数据 + ML 特征缓冲区
 *      - 运行安全检查（ML SoH 推理 + 阈值判断）
 *      - 每 50 秒打印一次状态日志
 *      - 通过 CoAP OBSERVE 通知 uGridController
 *   4. 隔离恢复：按钮事件触发恢复出厂状态
 * ======================================================================== */
PROCESS(battery_controller, "Battery Controller");
AUTOSTART_PROCESSES(&battery_controller);

PROCESS_THREAD(battery_controller, ev, data) {
    static coap_endpoint_t server_ep;
    static coap_message_t request[1];
    static int retry_count = 0;
    
    PROCESS_BEGIN();
    

    /* 避免 emlearn 符号被链接器优化掉 */
    printf("%p\n", eml_error_str);                      /* <- stdio.h + includes/battery_soh_model.h */
    printf("%p\n", eml_net_activation_function_strs);   /* <- stdio.h + includes/battery_soh_model.h */
    

    /* 启动 LED 闪烁定时器（1 秒周期） */
    ctimer_set(                                         /* <- sys/ctimer.h */
            &ct_led_blink,
            CLOCK_SECOND,
            led_blink,
            NULL);

    /* 注册 CoAP 资源：
     *   GET  /dev/state  → 返回电池状态（支持 OBSERVE 订阅）
     *   PUT  /dev/power  → 接收 uGridController 下发的功率指令 */
    coap_activate_resource(&res_dev_state, "dev/state");  /* <- coap-engine.h */
    coap_activate_resource(&res_dev_power, "dev/power");  /* <- coap-engine.h */
    LOG_INFO("[INIT] CoAP resources activated (dev/state is OBSERVABLE)\n");  /* <- sys/log.h */

    /* ====================================================================
     * 阶段一：向 uGridController 注册（循环重试直到成功）
     * ==================================================================== */
    LOG_INFO("[INIT] Starting registration to µGrid controller...\n");
    LOG_INFO("[INIT] Target endpoint: %s\n", UGRID_EP);
    

    while(current_state == STATE_INIT) {

        LOG_INFO("[INIT] Registration attempt #%d\n", retry_count++);
        
        if (retry_count > 0) {
            etimer_set(&et_init_wait, CLOCK_SECOND * 1);            /* <- sys/etimer.h */
            PROCESS_WAIT_EVENT_UNTIL(etimer_expired(&et_init_wait)); /* <- contiki.h */
        }
        update_leds();
    
        /* 构建 CoAP POST 请求：POST /dev/register，payload = battery_id */
        coap_endpoint_parse(UGRID_EP, strlen(UGRID_EP), &server_ep);  /* <- coap-engine.h */
        memset(request, 0, sizeof(coap_message_t));                     /* <- string.h */
        coap_init_message(request, COAP_TYPE_CON, COAP_POST, 0);        /* <- coap-engine.h */
        coap_set_header_uri_path(request, "dev/register");              /* <- coap-engine.h */
        static char payload[64];
        snprintf(payload, sizeof(payload), "%d", battery_id);           /* <- stdio.h */
        coap_set_payload(request, (uint8_t*)payload, strlen(payload));  /* <- coap-engine.h */

        /* 发送请求（阻塞等待响应，回调函数 reg_callback 处理结果） */
        COAP_BLOCKING_REQUEST(&server_ep, request, reg_callback);       /* <- coap-blocking-api.h */
        
        if(current_state == STATE_RUNNING) {
            LOG_INFO("[INIT] Entering main control loop\n");
            break;
        }
    }

    /* ====================================================================
     * 阶段二：主控制循环（每 5 秒执行一次）
     * ==================================================================== */
    etimer_set(&et_loop, CLOCK_SECOND * 5);  /* <- sys/etimer.h */
    
    while(1) {
        PROCESS_WAIT_EVENT();  /* <- contiki.h */
        
        /* 定时器事件：执行传感器更新 + 安全检查 + 状态通知 */
        if(ev == PROCESS_EVENT_TIMER && data == &et_loop) {
            if(current_state != STATE_ISOLATED) {
                update_sensors_and_buffer();
            }
            
            if(current_state == STATE_RUNNING) { 
                check_safety();
                
                static int status_counter = 0;
                if(++status_counter >= 10) {
                    print_battery_status();
                    status_counter = 0;
                }
            }
            
            /* 通过 CoAP OBSERVE 机制推送状态给所有订阅者（uGridController） */
            coap_notify_observers(&res_dev_state);  /* <- coap-engine.h */

            etimer_reset(&et_loop);  /* <- sys/etimer.h */
        }
        
        
        /* 按钮事件：仅在 ISOLATED 状态下有效，触发恢复出厂状态 */
        if(ev == button_hal_release_event && current_state == STATE_ISOLATED) {  /* <- dev/button-hal.h */
            LOG_INFO("[INFO] Factory Reset Triggered\n");

            current_state = STATE_RUNNING; 
            bat_soh = 1.0f;
            bat_capacity_ah = SCALED_CAPACITY_AH;
            bat_temp = 25.0f;
            power_setpoint = 0.0f;
            charge_cycles = 0;
            total_ah_throughput = 0.0f;
            peak_temp_reached = 25.0f;
            was_charging = false;
            
            update_leds();
            print_battery_status();
        }
    }
    
    PROCESS_END();
}
