/**
 * ============================================================================
 * project-conf.h — RPL Border Router 的 Contiki-NG 配置
 * ============================================================================
 */
#ifndef PROJECT_CONF_H_
#define PROJECT_CONF_H_

#ifdef CONTIKI_TARGET_SKY
/* Sky 平台资源受限，减少队列缓冲区和 IP 缓冲区以节省 RAM/ROM */
#define QUEUEBUF_CONF_NUM              4
#define UIP_CONF_BUFFER_SIZE         140
#define BORDER_ROUTER_CONF_WEBSERVER   0
#endif

/* Web 服务器最大并发连接数 */
#ifndef WEBSERVER_CONF_CFS_CONNS
#define WEBSERVER_CONF_CFS_CONNS 2
#endif

/* 启用嵌入式 Web 服务器（用于调试，查看 RPL 网络拓扑） */
#ifndef BORDER_ROUTER_CONF_WEBSERVER
#define BORDER_ROUTER_CONF_WEBSERVER 1
#endif

/* 启用 Web 服务器需要 TCP 支持 */
#if BORDER_ROUTER_CONF_WEBSERVER
#define UIP_CONF_TCP 1
#endif

#endif /* PROJECT_CONF_H_ */
