/*
 * rd_server.h - CoAP RD (Resource Directory) 服务器
 *
 * 基于 RFC 9176 标准实现 (CoRE Resource Directory), 提供:
 *   - POST   /rd                注册新端点 (带资源列表)
 *   - PUT    /rd/{endpoint}     更新端点注册 (续期/修改)
 *   - DELETE /rd/{endpoint}     删除端点注册
 *   - GET    /rd                查询目录 (按资源/端点/域)
 *   - GET    /rd/{endpoint}     查询指定端点信息
 *   - GET    /.well-known/core  RD 自身资源发现
 *
 * 运行方式: rd_server.exe [--port 5685] [--ttl 3600]
 *
 * 编译: gcc -Wall -Wextra -O2 -o rd_server.exe coap.c rd_server.c -lws2_32
 */
#ifndef RD_SERVER_H
#define RD_SERVER_H

#include "coap.h"
#include <stdint.h>
#include <time.h>

/* ===================== RD 常量 ===================== */
#define RD_DEFAULT_PORT     5685    /* RD 默认端口 */
#define RD_MAX_ENDPOINTS    64      /* 最大注册端点数 */
#define RD_MAX_LINKS        32      /* 每个端点最大资源链接数 */
#define RD_MAX_LINK_LEN     256     /* 单条 link-format 条目最大长度 */
#define RD_MAX_PAYLOAD      2048    /* RD 报文最大负载 */
#define RD_DEFAULT_TTL      3600    /* 默认 TTL (秒) */
#define RD_CLEANUP_INTERVAL 60      /* 过期清理间隔 (秒) */

/* ===================== 端点注册结构 ===================== */
typedef struct {
    char    ep[32];           /* 端点名称 (endpoint) */
    char    base[64];         /* 基础 URI (base) */
    char    domain[32];       /* 域 (domain, 可选) */
    uint32_t ttl;             /* 生存时间 (秒) */
    time_t  last_update;      /* 最后更新时间 */
    int     active;           /* 是否激活 (1=已注册, 0=空槽) */

    /* 资源链接列表 (CoRE Link Format) */
    struct {
        char    uri[64];      /* 资源 URI, 如 "/fwinfo" */
        char    rt[32];       /* 资源类型, 如 "version" */
        char    ifdesc[32];   /* 接口描述 (可选) */
        char    ver[32];      /* 版本属性 (可选) */
        char    hver[128];    /* 历史版本列表 (可选, 逗号分隔) */
    } links[RD_MAX_LINKS];
    int     link_count;       /* 链接数量 */
} rd_endpoint_t;

/* ===================== RD 服务器上下文 ===================== */
typedef struct {
    SOCKET  srv_sock;         /* 服务器 Socket */
    uint16_t port;            /* 监听端口 */
    volatile int running;     /* 运行标志 */
    uint32_t default_ttl;     /* 默认 TTL */

    rd_endpoint_t endpoints[RD_MAX_ENDPOINTS];  /* 端点注册表 */
    CRITICAL_SECTION lock;    /* 线程安全锁 */
} rd_server_t;

/* ===================== API 声明 ===================== */
int  rd_server_init(rd_server_t *rd, uint16_t port, uint32_t default_ttl);
void rd_server_cleanup(rd_server_t *rd);
int  rd_server_start(rd_server_t *rd);
void rd_server_stop(rd_server_t *rd);

/* 注册操作 (供设备端主动调用) */
int  rd_register(rd_server_t *rd, const char *ep, const char *base,
                 const char *domain, uint32_t ttl,
                 const char *links_payload, size_t links_len);
int  rd_update(rd_server_t *rd, const char *ep, const char *links_payload,
               size_t links_len, uint32_t ttl);
int  rd_delete(rd_server_t *rd, const char *ep);

/* 查询操作 */
int  rd_search_by_resource(rd_server_t *rd, const char *rt_filter,
                           char *out_buf, size_t buf_size);
int  rd_search_by_endpoint(rd_server_t *rd, const char *ep_filter,
                           char *out_buf, size_t buf_size);
int  rd_search_by_domain(rd_server_t *rd, const char *domain_filter,
                         char *out_buf, size_t buf_size);

#endif /* RD_SERVER_H */
