/**
 * ============================================================================
 * border-router.c — RPL 边界路由器（聚合层基础设施）
 * ============================================================================
 *
 * 【架构定位】
 *   本文件是 RPL Border Router 的核心进程，运行在聚合层（Aggregation Layer），
 *   作为低功耗无线传感器网络（6LoWPAN/RPL）与 IPv6 互联网之间的桥梁。
 *
 * 【核心功能】
 *   1. 启动 RPL 路由协议，构建 DAG（有向无环图）拓扑
 *   2. 创建 IPv6 前路由（fd00::/64），使外部网络可达传感器节点
 *   3. 可选启动嵌入式 Web 服务器（用于调试，查看网络拓扑）
 *
 * 【与 uGridController 的关系】
 *   Border Router 是网络层基础设施，uGridController 是应用层控制器。
 *   类比：Border Router = 高速公路收费站（协议转换）
 *         uGridController = 交通调度中心（业务逻辑）
 *
 * 【运行方式】
 *   - 在 Cooja 模拟器中：作为 Cooja mode 节点运行
 *   - 在真实硬件上：编译为 native 平台，通过 tunslip6 连接 TUN 设备
 * ============================================================================
 */

#include "contiki.h"
#include "leds.h"

/* Log configuration */
#include "sys/log.h"
#define LOG_MODULE "RPL BR"
#define LOG_LEVEL LOG_LEVEL_INFO

/* Declare and auto-start this file's process */
PROCESS(contiki_ng_br, "Contiki-NG Border Router");
AUTOSTART_PROCESSES(&contiki_ng_br);

/*---------------------------------------------------------------------------*/
PROCESS_THREAD(contiki_ng_br, ev, data)
{
    PROCESS_BEGIN();

#if BORDER_ROUTER_CONF_WEBSERVER
    PROCESS_NAME(webserver_nogui_process);
    process_start(&webserver_nogui_process, NULL);  /* <- sys/process.h */
#endif /* BORDER_ROUTER_CONF_WEBSERVER */

    LOG_INFO("Contiki-NG Border Router started\n");  /* <- sys/log.h */
    
    leds_on(LEDS_YELLOW);  /* <- dev/leds.h */

    PROCESS_END();
}
