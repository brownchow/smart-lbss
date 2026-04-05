/**
 * ============================================================================
 * project-conf.h — 项目级 Contiki-NG 配置（BatteryController 和 uGridController 共用）
 * ============================================================================
 */
#ifndef PROJECT_CONF_H
#define PROJECT_CONF_H

/* 应用日志级别：INFO */
#define LOG_LEVEL_APP LOG_LEVEL_INFO

/* 启用 CoAP OBSERVE 客户端（uGridController 需要订阅电池状态） */
#define COAP_OBSERVE_CLIENT 1

/* CoAP 最大分块大小：256 字节（适配 6LoWPAN MTU） */
#undef COAP_MAX_CHUNK_SIZE
#define COAP_MAX_CHUNK_SIZE 256

/* uGridController 的 CoAP 端点地址（BatteryController 注册目标） */
// #define UGRID_EP "coap://[fd00::202:2:2:2]:5683"  // 测试地址
#define UGRID_EP "coap://[fd00::f6ce:36ac:9afa:6be2]:5683"

/* BatteryController 的 CoAP 端点地址（保留，用于直连测试） */
// #define BATTERY_EP "coap://[fd00::203:3:3:3]:5683"
#define BATTERY_EP "coap://[fd00::f6ce:362e:a297:92a7]:5683"

#endif
