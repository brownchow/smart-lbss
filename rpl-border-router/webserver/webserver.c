/**
 * ============================================================================
 * webserver.c — 嵌入式 Web 服务器（用于调试 RPL 网络拓扑）
 * ============================================================================
 *
 * 【功能】提供一个轻量级 HTTP 服务，通过浏览器查看 RPL 网络的邻居、路由和拓扑信息
 *
 * 【访问方式】
 *   浏览器访问 Border Router 的 IPv6 地址：http://[fd00::1]/
 *
 * 【页面内容】
 *   1. Neighbors — 直连的 IPv6 邻居节点列表
 *   2. Routes    — IPv6 路由表（目标网络/下一跳/剩余生存时间）
 *   3. Routing links — RPL DAG 拓扑链接（子节点 → 父节点）
 *
 * 【技术实现】
 *   - 使用 Contiki 的 httpd-simple（单缓冲区静态 Web 服务器）
 *   - 页面内容动态生成，通过 protothread 分段输出
 *   - 使用静态缓冲区（256 字节），不支持多连接并发
 * ============================================================================
 */

#include "contiki.h"
#include "net/routing/routing.h"
#include "net/ipv6/uip-ds6-nbr.h"
#include "net/ipv6/uip-ds6-route.h"
#include "net/ipv6/uip-sr.h"

#include <stdio.h>
#include <string.h>

/*---------------------------------------------------------------------------*/
static const char *TOP = "<html>\n  <head>\n    <title>Contiki-NG</title>\n  </head>\n<body>\n";
static const char *BOTTOM = "\n</body>\n</html>\n";
static char buf[256];
static int blen;
#define ADD(...) do {                                                   \
    blen += snprintf(&buf[blen], sizeof(buf) - blen, __VA_ARGS__);      \
  } while(0)
#define SEND(s) do { \
  SEND_STRING(s, buf); \
  blen = 0; \
} while(0);

/* 使用简单 Web 服务器以最小化内存占用。
 * 多连接可能导致 TCP 分段交错，因为所有连接共享同一个静态缓冲区。
 */
#include "httpd-simple.h"

/*---------------------------------------------------------------------------*/
/**
 * 将 IPv6 地址格式化为字符串（支持 :: 缩写）
 */
static void
ipaddr_add(const uip_ipaddr_t *addr)
{
  uint16_t a;
  int i, f;
  for(i = 0, f = 0; i < sizeof(uip_ipaddr_t); i += 2) {
    a = (addr->u8[i] << 8) + addr->u8[i + 1];
    if(a == 0 && f >= 0) {
      if(f++ == 0) {
        ADD("::");
      }
    } else {
      if(f > 0) {
        f = -1;
      } else if(i > 0) {
        ADD(":");
      }
      ADD("%x", a);
    }
  }
}
/*---------------------------------------------------------------------------*/
/**
 * 动态生成路由拓扑 HTML 页面
 *
 * 使用 Contiki 的 PSOCK（protothread socket）分段输出，
 * 避免大页面占用过多内存。每生成一段就 SEND 一次。
 */
static
PT_THREAD(generate_routes(struct httpd_state *s))
{
  static uip_ds6_nbr_t *nbr;

  PSOCK_BEGIN(&s->sout);
  SEND_STRING(&s->sout, TOP);

  ADD("  Neighbors\n  <ul>\n");
  SEND(&s->sout);
  for(nbr = uip_ds6_nbr_head();  /* <- net/ipv6/uip-ds6-nbr.h */
      nbr != NULL;
      nbr = uip_ds6_nbr_next(nbr)) {
    ADD("    <li>");
    ipaddr_add(&nbr->ipaddr);
    ADD("</li>\n");
    SEND(&s->sout);
  }
  ADD("  </ul>\n");
  SEND(&s->sout);

#if (UIP_MAX_ROUTES != 0)
  {
    static uip_ds6_route_t *r;
    ADD("  Routes\n  <ul>\n");
    SEND(&s->sout);
    for(r = uip_ds6_route_head(); r != NULL; r = uip_ds6_route_next(r)) {  /* <- net/ipv6/uip-ds6-route.h */
      ADD("    <li>");
      ipaddr_add(&r->ipaddr);
      ADD("/%u (via ", r->length);
      ipaddr_add(uip_ds6_route_nexthop(r));
      ADD(") %lus", (unsigned long)r->state.lifetime);
      ADD("</li>\n");
      SEND(&s->sout);
    }
    ADD("  </ul>\n");
    SEND(&s->sout);
  }
#endif /* UIP_MAX_ROUTES != 0 */

#if (UIP_SR_LINK_NUM != 0)
  if(uip_sr_num_nodes() > 0) {
    static uip_sr_node_t *link;
    ADD("  Routing links\n  <ul>\n");
    SEND(&s->sout);
    for(link = uip_sr_node_head(); link != NULL; link = uip_sr_node_next(link)) {  /* <- net/ipv6/uip-sr.h */
      if(link->parent != NULL) {
        uip_ipaddr_t child_ipaddr;
        uip_ipaddr_t parent_ipaddr;

        NETSTACK_ROUTING.get_sr_node_ipaddr(&child_ipaddr, link);        /* <- net/routing/routing.h */
        NETSTACK_ROUTING.get_sr_node_ipaddr(&parent_ipaddr, link->parent);

        ADD("    <li>");
        ipaddr_add(&child_ipaddr);

        ADD(" (parent: ");
        ipaddr_add(&parent_ipaddr);
        ADD(") %us", (unsigned int)link->lifetime);

        ADD("</li>\n");
        SEND(&s->sout);
      }
    }
    ADD("  </ul>");
    SEND(&s->sout);
  }
#endif /* UIP_SR_LINK_NUM != 0 */

  SEND_STRING(&s->sout, BOTTOM);

  PSOCK_END(&s->sout);
}
/*---------------------------------------------------------------------------*/
/**
 * Web 服务器进程：监听 TCP 事件并分发给 httpd 处理
 */
PROCESS(webserver_nogui_process, "Web server");
PROCESS_THREAD(webserver_nogui_process, ev, data)
{
  PROCESS_BEGIN();

  httpd_init();  /* <- httpd-simple.h */

  while(1) {
    PROCESS_WAIT_EVENT_UNTIL(ev == tcpip_event);  /* <- contiki.h */
    httpd_appcall(data);                          /* <- httpd-simple.h */
  }

  PROCESS_END();
}
/*---------------------------------------------------------------------------*/
/**
 * 脚本路由函数：将所有请求指向 generate_routes
 *
 * 这是 httpd-simple 的回调入口，httpd 收到请求后会调用此函数
 * 获取对应的页面生成器（protothread 函数指针）。
 */
httpd_simple_script_t
httpd_simple_get_script(const char *name)
{
  return generate_routes;
}
/*---------------------------------------------------------------------------*/
