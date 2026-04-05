/**
 * ============================================================================
 * httpd-simple.h — Contiki-NG 轻量级 HTTP 服务器头文件
 * ============================================================================
 *
 * 定义了嵌入式 Web 服务器的数据结构和接口。
 * 专为资源受限的 Contiki-NG 设备设计，使用 protothread 模型处理请求。
 */
#ifndef HTTPD_SIMPLE_H_
#define HTTPD_SIMPLE_H_

#include "contiki-net.h"

/* URL 路径最大长度。可通过 WEBSERVER_CONF_CFS_PATHLEN 覆盖 */
#ifndef WEBSERVER_CONF_CFS_PATHLEN
#define HTTPD_PATHLEN 16
#else
#define HTTPD_PATHLEN WEBSERVER_CONF_CFS_PATHLEN
#endif

struct httpd_state;
/** 页面生成器函数指针类型：返回 0 表示成功 */
typedef char (*httpd_simple_script_t)(struct httpd_state *s);

/**
 * HTTP 连接状态结构
 *
 * 每个 TCP 连接分配一个 httpd_state，包含：
 *   - timer: 连接超时计时器
 *   - sin/sout: 输入/输出 protothread socket
 *   - outputpt: 输出 protothread 状态
 *   - inputbuf: 请求缓冲区（复用为输出缓冲区以节省内存）
 *   - filename: 请求的文件路径
 *   - script: 页面生成器函数指针
 *   - state: 连接状态（STATE_WAITING / STATE_OUTPUT）
 */
struct httpd_state {
  struct timer timer;
  struct psock sin, sout;
  struct pt outputpt;
  char inputbuf[HTTPD_PATHLEN + 24];
  char filename[HTTPD_PATHLEN];
  httpd_simple_script_t script;
  char state;
};

/** 初始化 HTTP 服务器（监听 80 端口） */
void httpd_init(void);

/** TCP 连接回调函数（由 uIP 栈调用，每个连接事件都会触发） */
void httpd_appcall(void *state);

/** 脚本路由回调：根据 URL 路径返回对应的页面生成器函数 */
httpd_simple_script_t httpd_simple_get_script(const char *name);

/** 通过 PSOCK 发送字符串的便捷宏 */
#define SEND_STRING(s, str) PSOCK_SEND(s, (uint8_t *)str, strlen(str))

#endif /* HTTPD_SIMPLE_H_ */
