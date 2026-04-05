/**
 * ============================================================================
 * httpd-simple.c — Contiki-NG 轻量级 HTTP 服务器核心实现
 * ============================================================================
 *
 * 【功能】一个极简的 TCP Web 服务器，专为资源受限的嵌入式设备设计
 *
 * 【设计特点】
 *   - 单缓冲区：所有连接共享同一个静态缓冲区，内存占用极小
 *   - Protothread：使用协程模型处理请求，无需多线程/多进程
 *   - 脚本回调：页面生成逻辑通过外部函数（webserver.c）提供
 *   - 连接超时：10 秒无活动自动断开，防止僵尸连接
 *
 * 【工作流程】
 *   1. httpd_init() — 监听 TCP 80 端口，初始化连接池
 *   2. 收到 TCP 连接 → 分配 httpd_state → 解析 HTTP 请求行
 *   3. 调用 httpd_simple_get_script() 获取页面生成器
 *   4. 通过 protothread 分段生成 HTML 并发送
 *   5. 连接关闭 → 释放 httpd_state
 *
 * 【限制】
 *   - 不支持并发连接（多连接会交错）
 *   - 仅支持 GET 请求
 *   - 固定 Content-Type: text/html
 * ============================================================================
 */

#include "contiki.h"
#include "contiki-net.h"

#include <stdio.h>
#include <string.h>

#include "httpd-simple.h"
#define webserver_log_file(...)
#define webserver_log(...)

#ifndef WEBSERVER_CONF_CFS_CONNS
#define CONNS UIP_TCP_CONNS
#else /* WEBSERVER_CONF_CFS_CONNS */
#define CONNS WEBSERVER_CONF_CFS_CONNS
#endif /* WEBSERVER_CONF_CFS_CONNS */

#ifndef WEBSERVER_CONF_CFS_URLCONV
#define URLCONV 0
#else /* WEBSERVER_CONF_CFS_URLCONV */
#define URLCONV WEBSERVER_CONF_CFS_URLCONV
#endif /* WEBSERVER_CONF_CFS_URLCONV */

#define STATE_WAITING 0
#define STATE_OUTPUT  1

MEMB(conns, struct httpd_state, CONNS);  /* <- sys/memb.h: 静态内存池 */

#define ISO_nl      0x0a
#define ISO_space   0x20
#define ISO_period  0x2e
#define ISO_slash   0x2f

/*---------------------------------------------------------------------------*/
/** 404 错误页面 HTML */
static const char *NOT_FOUND = "<html><body bgcolor=\"white\">"
"<center>"
"<h1>404 - file not found</h1>"
"</center>"
"</body>"
"</html>";
/*---------------------------------------------------------------------------*/
/**
 * 通过 PSOCK 发送字符串（protothread 分段输出）
 */
static
PT_THREAD(send_string(struct httpd_state *s, const char *str))
{
  PSOCK_BEGIN(&s->sout);

  SEND_STRING(&s->sout, str);

  PSOCK_END(&s->sout);
}
/*---------------------------------------------------------------------------*/
const char http_content_type_html[] = "Content-type: text/html\r\n\r\n";
/**
 * 发送 HTTP 响应头（固定 Content-Type: text/html）
 */
static
PT_THREAD(send_headers(struct httpd_state *s, const char *statushdr))
{
  PSOCK_BEGIN(&s->sout);

  SEND_STRING(&s->sout, statushdr);
  SEND_STRING(&s->sout, http_content_type_html);
  PSOCK_END(&s->sout);
}
/*---------------------------------------------------------------------------*/
const char http_header_200[] = "HTTP/1.0 200 OK\r\nServer: Contiki/2.4 http://www.sics.se/contiki/\r\nConnection: close\r\n";
const char http_header_404[] = "HTTP/1.0 404 Not found\r\nServer: Contiki/2.4 http://www.sics.se/contiki/\r\nConnection: close\r\n";
/**
 * 处理 HTTP 响应输出
 *
 * 流程：查找脚本 → 404 则返回错误页 → 200 则调用脚本生成页面
 */
static
PT_THREAD(handle_output(struct httpd_state *s))
{
  PT_BEGIN(&s->outputpt);

  s->script = NULL;
  s->script = httpd_simple_get_script(&s->filename[1]);  /* <- httpd-simple.h: 外部回调 */
  if(s->script == NULL) {
    strncpy(s->filename, "/notfound.html", sizeof(s->filename) - 1);
    s->filename[sizeof(s->filename) - 1] = '\0';
    PT_WAIT_THREAD(&s->outputpt,
                   send_headers(s, http_header_404));
    PT_WAIT_THREAD(&s->outputpt,
                   send_string(s, NOT_FOUND));
    uip_close();  /* <- contiki-net.h */
    webserver_log_file(&uip_conn->ripaddr, "404 - not found");
    PT_EXIT(&s->outputpt);
  } else {
    PT_WAIT_THREAD(&s->outputpt,
                   send_headers(s, http_header_200));
    PT_WAIT_THREAD(&s->outputpt, s->script(s));
  }
  s->script = NULL;
  PSOCK_CLOSE(&s->sout);
  PT_END(&s->outputpt);
}
/*---------------------------------------------------------------------------*/
const char http_get[] = "GET ";
const char http_index_html[] = "/index.html";

/**
 * 处理 HTTP 请求输入
 *
 * 解析 HTTP 请求行：GET /path HTTP/1.0
 * 提取路径存入 s->filename，等待后续处理
 */
static
PT_THREAD(handle_input(struct httpd_state *s))
{
  PSOCK_BEGIN(&s->sin);

  PSOCK_READTO(&s->sin, ISO_space);

  if(strncmp(s->inputbuf, http_get, 4) != 0) {  /* <- string.h */
    PSOCK_CLOSE_EXIT(&s->sin);
  }
  PSOCK_READTO(&s->sin, ISO_space);

  if(s->inputbuf[0] != ISO_slash) {
    PSOCK_CLOSE_EXIT(&s->sin);
  }

#if URLCONV
  s->inputbuf[PSOCK_DATALEN(&s->sin) - 1] = 0;
  urlconv_tofilename(s->filename, s->inputbuf, sizeof(s->filename));
#else /* URLCONV */
  if(s->inputbuf[1] == ISO_space) {
    strncpy(s->filename, http_index_html, sizeof(s->filename) - 1);  /* <- string.h */
    s->filename[sizeof(s->filename) - 1] = '\0';
  } else {
    s->inputbuf[PSOCK_DATALEN(&s->sin) - 1] = 0;
    strncpy(s->filename, s->inputbuf, sizeof(s->filename) - 1);
    s->filename[sizeof(s->filename) - 1] = '\0';
  }
#endif /* URLCONV */

  webserver_log_file(&uip_conn->ripaddr, s->filename);

  s->state = STATE_OUTPUT;

  while(1) {
    PSOCK_READTO(&s->sin, ISO_nl);
  }

  PSOCK_END(&s->sin);
}
/*---------------------------------------------------------------------------*/
/**
 * 连接状态机：先处理输入（解析请求），再处理输出（生成响应）
 */
static void
handle_connection(struct httpd_state *s)
{
  handle_input(s);
  if(s->state == STATE_OUTPUT) {
    handle_output(s);
  }
}
/*---------------------------------------------------------------------------*/
/**
 * TCP 连接回调函数（由 uIP TCP 栈调用）
 *
 * 事件处理：
 *   - uip_connected()  → 新连接：分配状态，初始化 PSOCK
 *   - uip_poll()       → 轮询：检查超时
 *   - uip_newdata()    → 收到数据：处理请求
 *   - uip_closed()     → 连接关闭：释放状态
 *   - uip_aborted()    → 连接异常：清理状态
 *   - uip_timedout()   → 连接超时：清理状态
 */
void
httpd_appcall(void *state)
{
  struct httpd_state *s = (struct httpd_state *)state;

  if(uip_closed() || uip_aborted() || uip_timedout()) {  /* <- contiki-net.h */
    if(s != NULL) {
      s->script = NULL;
      memb_free(&conns, s);  /* <- sys/memb.h */
    }
  } else if(uip_connected()) {
    s = (struct httpd_state *)memb_alloc(&conns);  /* <- sys/memb.h */
    if(s == NULL) {
      uip_abort();
      webserver_log_file(&uip_conn->ripaddr, "reset (no memory block)");
      return;
    }
    tcp_markconn(uip_conn, s);
    PSOCK_INIT(&s->sin, (uint8_t *)s->inputbuf, sizeof(s->inputbuf) - 1);
    PSOCK_INIT(&s->sout, (uint8_t *)s->inputbuf, sizeof(s->inputbuf) - 1);
    PT_INIT(&s->outputpt);
    s->script = NULL;
    s->state = STATE_WAITING;
    timer_set(&s->timer, CLOCK_SECOND * 10);  /* <- sys/timer.h */
    handle_connection(s);
  } else if(s != NULL) {
    if(uip_poll()) {
      if(timer_expired(&s->timer)) {
        uip_abort();
        s->script = NULL;
        memb_free(&conns, s);
        webserver_log_file(&uip_conn->ripaddr, "reset (timeout)");
      }
    } else {
      timer_restart(&s->timer);
    }
    handle_connection(s);
  } else {
    uip_abort();
  }
}
/*---------------------------------------------------------------------------*/
/**
 * 初始化 HTTP 服务器：监听 80 端口，初始化连接池
 */
void
httpd_init(void)
{

  tcp_listen(UIP_HTONS(80));  /* <- contiki-net.h */
  memb_init(&conns);          /* <- sys/memb.h */
#if URLCONV
  urlconv_init();
#endif /* URLCONV */
}
/*---------------------------------------------------------------------------*/
