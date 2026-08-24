/*
 * device_link.c - 基于 /.well-known/core 多播发现的直连通信设备
 *
 * 工作流程 (RFC 7252 §8.1, RFC 7390):
 *   1) 启动: srv_sock 绑定本机独占端口 (--port), 加入 CoAP 多播组 224.0.1.187;
 *           mc_sock 绑定公共多播发现端口 (--mc-port, 默认 5683) + SO_REUSEADDR,
 *           加入同一多播组, 专门接收多播 GET /.well-known/core.
 *   2) 暴露资源: /.well-known/core, /fwinfo, /log, /firmware (PUT Block1).
 *   3) 多播发现: 客户端用 cli_sock 向 224.0.1.187:5683 发 NON GET /.well-known/core,
 *                携带 4 字节随机 Token. 各设备的 mc_sock 收到后, 随机延迟 0~1000ms,
 *                用 srv_sock 单播把 link-format 响应发回请求者源地址.
 *   4) Token 匹配: 客户端只接收 Token 匹配的响应, 记录响应源 IP:port 为对端地址.
 *   5) 直连通信: 客户端用 cli_sock 单播 CON GET /fwinfo / /log, 或 PUT /firmware
 *                (Block1 分块推送) 到对端 srv_sock 的 IP:port.
 *
 * 编译: gcc -Wall -Wextra -O2 -o device_link.exe coap.c device_link.c -lws2_32
 *
 * 使用 (两个终端):
 *   终端1: .\device_link.exe --id A --port 5683 --version 1.0.0-A
 *   终端2: .\device_link.exe --id B --port 5684 --version 1.0.0-B
 *
 *   设备 A 输入: discover    -> 多播发现, 收到 B 的响应, 记录 B 的地址
 *                peers       -> 显示已发现的对端
 *                get_fwinfo 1 -> 向第 1 个对端 GET /fwinfo
 *                get_log 1   -> 向第 1 个对端 GET /log
 *                get_fw 1    -> GET /firmware (Block2) 从第 1 个对端拉取并升级自己
 *                quit        -> 退出
 *
 * 可选参数:
 *   --mc-port 5683         公共多播发现端口 (默认 5683)
 *   --mc-group 224.0.1.187 多播组地址 (默认 224.0.1.187)
 *   --mc-ttl 16            多播 TTL (默认 16)
 *   --bcast                使用受限广播 255.255.255.255 替代多播 (localhost 兜底)
 */
#include "coap.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#define FW_FILLER_LEN       2600    /* 固件镜像填充数据长度, 保证触发 Block1 分块 */
#define MAX_PEERS           16      /* 最大缓存的对端数量 */
#define DISCOVER_WAIT_MS    3000    /* 发现请求等待响应时间窗口 */
#define MAX_DELAY_MS        1000    /* 多播响应随机延迟上限 (RFC 7390 防响应风暴) */
#define TOKEN_LEN           4       /* 发现请求 Token 长度 */

#define COAP_MULTICAST_GROUP  "224.0.1.187"
#define COAP_BROADCAST_ADDR   "255.255.255.255"
#define DEFAULT_MC_PORT       5683

/* ---------- 单个资源条目 (解析 link-format 后的结果) ---------- */
#define MAX_RESOURCES   16
#define MAX_ATTRS       8

typedef struct {
    char    uri[64];                              /* 资源路径, 如 "/fwinfo" */
    struct {
        char    name[16];                         /* 属性名, 如 "rt"/"ver"/"ct" */
        char    value[64];                        /* 属性值, 如 "version" */
    } attrs[MAX_ATTRS];
    int     attr_count;
} resource_t;

/* ---------- 对端缓存条目 ---------- */
typedef struct {
    char    ip[64];
    uint16_t port;
    char    version[32];     /* 从 link-format 中解析出的 ver 属性 */
    char    raw_links[512];  /* link-format 原文 */
    time_t  discovered_at;
    int     active;
} peer_entry_t;

/* ---------- 设备上下文 ---------- */
typedef struct {
    char        id[16];
    uint16_t    port;          /* srv_sock 独占端口 */
    uint16_t    mc_port;       /* mc_sock 公共多播发现端口 */
    char        mc_group[32];  /* 多播组地址 */
    int         mc_ttl;
    int         use_broadcast;/* 1=用广播, 0=用多播 */

    char        version[32];
    char        fw_path[64];
    char        fw_orig_path[64];
    char        log_path[64];

    SOCKET      srv_sock;      /* 单播服务 socket (独占端口) */
    SOCKET      mc_sock;       /* 多播接收 socket (公共端口) */
    SOCKET      cli_sock;      /* 客户端 socket (临时端口) */
    volatile int running;
    uint16_t    next_msg_id;

    /* 单播扫描模式 (localhost 测试用) */
    int         use_scan;
    char        scan_ip[64];     /* 扫描目标 IP (默认 127.0.0.1) */
    int         scan_port_start; /* 扫描起始端口 (默认 5683) */
    int         scan_port_end;   /* 扫描结束端口 (默认 5687) */

    peer_entry_t peers[MAX_PEERS];
    int          peer_count;

    CRITICAL_SECTION lock;
    FILE       *log_fp;

    /* 协议日志: 记录每条 CoAP 消息的原始字节和解析字段 */
    CRITICAL_SECTION proto_lock;
    FILE       *proto_fp;
} device_t;

/* ============================ 日志 ============================ */
static void dev_log(device_t *d, const char *fmt, ...) {
    char line[512];
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(line, sizeof(line), fmt, ap);
    va_end(ap);
    if (n < 0) return;
    if (n > (int)sizeof(line) - 1) n = (int)sizeof(line) - 1;

    time_t now = time(NULL);
    struct tm *tm = localtime(&now);
    char ts[64];
    strftime(ts, sizeof(ts), "%H:%M:%S", tm);

    printf("[%s] [%s] %.*s\n", ts, d->id, n, line);
    fflush(stdout);

    EnterCriticalSection(&d->lock);
    if (d->log_fp) {
        fprintf(d->log_fp, "[%s] [%s] %.*s\n", ts, d->id, n, line);
        fflush(d->log_fp);
    }
    LeaveCriticalSection(&d->lock);
}

/* ============================ 协议日志 ============================ */
/* 把一条 CoAP 消息的原始字节和解析字段写入 proto_<id>.log
 * dir: "TX" / "RX"  ;  raw: UDP 报文原始字节  ;  msg: 解析后的 CoAP 消息 */
static void proto_log_msg(device_t *d, const char *dir,
                          const char *ip, uint16_t port,
                          const coap_msg_t *msg,
                          const uint8_t *raw, size_t raw_len) {
    if (!d->proto_fp) return;

    time_t now = time(NULL);
    struct tm *tm = localtime(&now);
    char ts[64];
    strftime(ts, sizeof(ts), "%Y-%m-%d %H:%M:%S", tm);

    EnterCriticalSection(&d->proto_lock);

    fprintf(d->proto_fp,
            "\n[%s] %s peer=%s:%u  (%zu raw bytes)\n",
            ts, dir, ip, port, raw_len);

    /* 原始字节 hex dump (最多 96 字节, 避免日志过长) */
    size_t show = raw_len < 96 ? raw_len : 96;
    fprintf(d->proto_fp, "  hex:");
    for (size_t i = 0; i < show; i++) {
        fprintf(d->proto_fp, " %02x", raw[i]);
        if ((i + 1) % 16 == 0) fprintf(d->proto_fp, "\n      ");
    }
    if (raw_len > 96) fprintf(d->proto_fp, " ...(%zu more)", raw_len - 96);
    fprintf(d->proto_fp, "\n");

    /* 解析字段 */
    const char *type_name =
        msg->type == 0 ? "CON" :
        msg->type == 1 ? "NON" :
        msg->type == 2 ? "ACK" :
        msg->type == 3 ? "RST" : "?";
    fprintf(d->proto_fp, "  type=%s(%d)  code=0x%02x(%s)  msg_id=%u\n",
            type_name, msg->type, msg->code,
            msg->code < 0x40 ? coap_method_name(msg->code)
                             : coap_response_name(msg->code),
            msg->msg_id);

    /* Token */
    fprintf(d->proto_fp, "  token(%d):", msg->token_len);
    for (int i = 0; i < msg->token_len; i++)
        fprintf(d->proto_fp, "%02x", msg->token[i]);
    fprintf(d->proto_fp, "\n");

    /* URI */
    fprintf(d->proto_fp, "  uri: /%s\n",
            msg->uri_path[0] ? msg->uri_path : "(empty)");

    /* Block1/Block2 选项 */
    if (msg->has_block1)
        fprintf(d->proto_fp, "  block1: num=%u M=%d SZX=%d\n",
                msg->block1_num, msg->block1_more, msg->block1_szx);
    if (msg->has_block2)
        fprintf(d->proto_fp, "  block2: num=%u M=%d SZX=%d\n",
                msg->block2_num, msg->block2_more, msg->block2_szx);

    /* Content-Format */
    if (msg->content_format >= 0)
        fprintf(d->proto_fp, "  content_format=%d\n", msg->content_format);

    /* Payload (可打印字符直接显示, 不可打印用 \xNN) */
    if (msg->payload_len > 0) {
        size_t pshow = msg->payload_len < 256 ? msg->payload_len : 256;
        fprintf(d->proto_fp, "  payload(%zu): ", msg->payload_len);
        for (size_t i = 0; i < pshow; i++) {
            unsigned char c = msg->payload[i];
            if (c == '\n') fprintf(d->proto_fp, "\\n");
            else if (c == '\r') fprintf(d->proto_fp, "\\r");
            else if (c == '\t') fprintf(d->proto_fp, "\\t");
            else if (c >= 32 && c < 127) fprintf(d->proto_fp, "%c", c);
            else fprintf(d->proto_fp, "\\x%02x", c);
        }
        if (msg->payload_len > 256)
            fprintf(d->proto_fp, "...(%zu more)",
                    msg->payload_len - 256);
        fprintf(d->proto_fp, "\n");
    }

    fprintf(d->proto_fp, "----\n");
    fflush(d->proto_fp);
    LeaveCriticalSection(&d->proto_lock);
}

/* ============================ 多播 socket 辅助 ============================ */
/* 加入多播组 */
static int join_multicast(SOCKET s, const char *group) {
    struct ip_mreq mreq;
    memset(&mreq, 0, sizeof(mreq));
    mreq.imr_multiaddr.s_addr = inet_addr(group);
    mreq.imr_interface.s_addr = htonl(INADDR_ANY);
    return setsockopt(s, IPPROTO_IP, IP_ADD_MEMBERSHIP,
                      (const char *)&mreq, sizeof(mreq));
}

/* 设置多播 TTL */
static int set_multicast_ttl(SOCKET s, int ttl) {
    return setsockopt(s, IPPROTO_IP, IP_MULTICAST_TTL,
                      (const char *)&ttl, sizeof(ttl));
}

/* 允许本机回环接收自己发的多播包 (默认就允许, 这里显式设置) */
static int enable_multicast_loop(SOCKET s) {
    BOOL loop = TRUE;
    return setsockopt(s, IPPROTO_IP, IP_MULTICAST_LOOP,
                      (const char *)&loop, sizeof(loop));
}

/* 允许广播发送 */
static int enable_broadcast(SOCKET s) {
    BOOL b = TRUE;
    return setsockopt(s, SOL_SOCKET, SO_BROADCAST,
                      (const char *)&b, sizeof(b));
}

/* 生成随机 Token (4 字节) */
static void gen_token(uint8_t *token, size_t len) {
    for (size_t i = 0; i < len; i++)
        token[i] = (uint8_t)(rand() & 0xff);
}

/* 随机延迟 0~max_ms 毫秒 */
static void random_delay(int max_ms) {
    if (max_ms <= 0) return;
    int ms = rand() % max_ms;
    Sleep(ms);
}

/* ============================ 文件/资源辅助 ============================ */
/* 读取固件文件: 返回大小, version_buf 写入首行版本号 */
static size_t read_fw_info(const char *path, char *version_buf, size_t vbuf_size) {
    FILE *fw = fopen(path, "rb");
    if (!fw) {
        if (version_buf && vbuf_size > 0) version_buf[0] = '\0';
        return 0;
    }
    fseek(fw, 0, SEEK_END);
    long fsize = ftell(fw);
    fseek(fw, 0, SEEK_SET);
    if (version_buf && vbuf_size > 0) {
        char line[64];
        if (fgets(line, sizeof(line), fw)) {
            size_t vi = 0;
            while (vi < vbuf_size - 1 && line[vi] != '\n' && line[vi] != '\0') {
                version_buf[vi] = line[vi];
                vi++;
            }
            version_buf[vi] = '\0';
        } else {
            version_buf[0] = '\0';
        }
    }
    fclose(fw);
    return fsize > 0 ? (size_t)fsize : 0;
}

/* 构造本设备的 link-format 字符串 */
static int build_well_known(device_t *d, char *buf, size_t buf_size) {
    return snprintf(buf, buf_size,
        "</fwinfo>;rt=\"version\";ver=\"%s\","
        "</log>;rt=\"log\","
        "</firmware>;rt=\"fw\","
        "</.well-known/core>;rt=\"core\"",
        d->version);
}

/* 从 link-format 中提取 ver="xxx" */
static void extract_ver(const char *links, char *ver, size_t ver_size) {
    ver[0] = '\0';
    const char *p = strstr(links, "ver=\"");
    if (!p) return;
    p += 5;
    const char *end = strchr(p, '"');
    if (!end) return;
    size_t n = (size_t)(end - p);
    if (n >= ver_size) n = ver_size - 1;
    memcpy(ver, p, n);
    ver[n] = '\0';
}

/* 解析 CoRE Link Format 字符串为 resource_t 数组.
 * 正确处理引号内的逗号 (如 hver="1.0.0,0.9.9"), 不会错误拆分.
 * 返回: 解析到的资源条数. */
static int parse_link_format(const char *links, resource_t *out, int max) {
    int count = 0;
    const char *p = links;

    while (*p && count < max) {
        /* 跳过条目间的逗号和空白 */
        while (*p == ',' || *p == ' ' || *p == '\t' || *p == '\n') p++;
        if (!*p) break;

        resource_t *r = &out[count];
        memset(r, 0, sizeof(*r));

        /* 解析 <uri> */
        if (*p == '<') {
            p++;
            const char *end = strchr(p, '>');
            if (!end) break;
            size_t ulen = (size_t)(end - p);
            if (ulen >= sizeof(r->uri)) ulen = sizeof(r->uri) - 1;
            memcpy(r->uri, p, ulen);
            r->uri[ulen] = '\0';
            p = end + 1;
        } else {
            /* 不是以 < 开头, 跳过本条目 */
            while (*p && *p != ',') p++;
            continue;
        }

        /* 解析属性: ;name="value" 或 ;name=value (无引号) */
        while (*p == ';') {
            p++;
            while (*p == ' ' || *p == '\t') p++;

            /* 属性名 (到 = 或 ; 或 , 为止) */
            const char *name_start = p;
            while (*p && *p != '=' && *p != ';' && *p != ',' && *p != ' ')
                p++;

            if (*p != '=') continue;  /* 无值的 flag 属性, 跳过 */
            char name[16] = {0};
            size_t nlen = (size_t)(p - name_start);
            if (nlen >= sizeof(name)) nlen = sizeof(name) - 1;
            memcpy(name, name_start, nlen);
            name[nlen] = '\0';
            p++;  /* 跳过 '=' */

            /* 属性值 (引号内可含逗号) */
            char value[64] = {0};
            if (*p == '"') {
                p++;
                const char *val_start = p;
                while (*p && *p != '"') p++;  /* 引号内的逗号不会终止 */
                size_t vlen = (size_t)(p - val_start);
                if (vlen >= sizeof(value)) vlen = sizeof(value) - 1;
                memcpy(value, val_start, vlen);
                value[vlen] = '\0';
                if (*p == '"') p++;
            } else {
                const char *val_start = p;
                while (*p && *p != ';' && *p != ',' && *p != ' ') p++;
                size_t vlen = (size_t)(p - val_start);
                if (vlen >= sizeof(value)) vlen = sizeof(value) - 1;
                memcpy(value, val_start, vlen);
                value[vlen] = '\0';
            }

            /* 保存属性 */
            if (r->attr_count < MAX_ATTRS) {
                strncpy(r->attrs[r->attr_count].name, name,
                        sizeof(r->attrs[0].name) - 1);
                strncpy(r->attrs[r->attr_count].value, value,
                        sizeof(r->attrs[0].value) - 1);
                r->attr_count++;
            }
        }

        count++;
    }
    return count;
}

/* 打印对端的资源列表 (解析后的格式化输出) */
static void print_peer_resources(const char *links) {
    resource_t resources[MAX_RESOURCES];
    int n = parse_link_format(links, resources, MAX_RESOURCES);
    if (n == 0) {
        printf("    (no resources)\n");
        return;
    }
    printf("  Resources (%d):\n", n);
    for (int i = 0; i < n; i++) {
        printf("    %-22s", resources[i].uri);
        for (int j = 0; j < resources[i].attr_count; j++) {
            if (j > 0) printf(" ");
            printf("%s=\"%s\"",
                   resources[i].attrs[j].name,
                   resources[i].attrs[j].value);
        }
        printf("\n");
    }
}

/* 添加已发现的对端 (去重: 同 IP:port 只保留首次) */
static int add_peer(device_t *d, const char *ip, uint16_t port,
                    const char *links, size_t links_len) {
    EnterCriticalSection(&d->lock);
    for (int i = 0; i < d->peer_count; i++) {
        if (d->peers[i].active &&
            strcmp(d->peers[i].ip, ip) == 0 &&
            d->peers[i].port == port) {
            LeaveCriticalSection(&d->lock);
            return 0;  /* 已存在 */
        }
    }
    if (d->peer_count >= MAX_PEERS) {
        LeaveCriticalSection(&d->lock);
        return -1;
    }
    peer_entry_t *e = &d->peers[d->peer_count++];
    strncpy(e->ip, ip, sizeof(e->ip) - 1); e->ip[sizeof(e->ip) - 1] = '\0';
    e->port = port;
    size_t n = links_len < sizeof(e->raw_links) - 1
               ? links_len : sizeof(e->raw_links) - 1;
    memcpy(e->raw_links, links, n);
    e->raw_links[n] = '\0';
    extract_ver(e->raw_links, e->version, sizeof(e->version));
    e->discovered_at = time(NULL);
    e->active = 1;
    LeaveCriticalSection(&d->lock);
    return 1;
}

/* ============================ 多播接收线程 ============================ */
/* 处理一个收到的多播 GET /.well-known/core: 随机延迟后用 srv_sock 单播回响应 */
static void handle_mc_discovery(device_t *d, const coap_msg_t *req,
                                const char *from_ip, uint16_t from_port) {
    /* 只处理 NON GET /.well-known/core (避免对 CON 请求也延迟, 区分多播 vs 直查) */
    if (req->code != COAP_GET ||
        strcmp(req->uri_path, "well-known/core") != 0) {
        return;
    }

    /* RFC 7390: 随机延迟 0~MAX_DELAY_MS, 避免多设备同时响应造成风暴 */
    random_delay(MAX_DELAY_MS);

    /* 构造 link-format 响应 */
    char core_buf[256];
    int core_len = build_well_known(d, core_buf, sizeof(core_buf));

    coap_msg_t resp;
    memset(&resp, 0, sizeof(resp));
    /* 多播请求是 NON, 响应也用 NON (避免对端被动 ACK) */
    resp.type      = COAP_NON;
    resp.msg_id    = req->msg_id;
    resp.token_len = req->token_len;
    memcpy(resp.token, req->token, req->token_len);
    resp.code           = COAP_CONTENT;
    resp.content_format = FMT_LINK_FORMAT;
    resp.payload     = (const uint8_t *)core_buf;
    resp.payload_len = (size_t)core_len;

    uint8_t sbuf[COAP_MAX_MSG];
    int slen = coap_build(sbuf, sizeof(sbuf), &resp);
    if (slen <= 0) {
        dev_log(d, "mc: coap_build failed");
        return;
    }

    /* 关键: 用 srv_sock (独占端口) 发送, 这样响应源 IP:port 就是该设备的单播地址 */
    int sent = coap_send(d->srv_sock, from_ip, from_port, sbuf, (size_t)slen);
    if (sent > 0) {
        proto_log_msg(d, "TX", from_ip, from_port, &resp, sbuf, (size_t)slen);
        dev_log(d, "mc: <- NON GET /.well-known/core from %s:%u ; delayed %dms ; "
                   "-> unicast reply via srv_sock:%u (%d bytes)",
                from_ip, from_port, MAX_DELAY_MS, d->port, slen);
    } else {
        dev_log(d, "mc: send unicast reply FAILED");
    }
}

/* 多播接收线程: 监听 mc_sock */
static DWORD WINAPI multicast_thread(LPVOID arg) {
    device_t *d = (device_t *)arg;
    uint8_t rbuf[COAP_MAX_MSG];

    while (d->running) {
        char from_ip[64];
        uint16_t from_port;
        int n = coap_recv(d->mc_sock, rbuf, sizeof(rbuf),
                          from_ip, &from_port, 200);
        if (n <= 0) continue;

        coap_msg_t req;
        if (coap_parse(rbuf, (size_t)n, &req) < 0) {
            dev_log(d, "mc: failed to parse %d bytes from %s:%u",
                    n, from_ip, from_port);
            continue;
        }

        proto_log_msg(d, "RX", from_ip, from_port, &req, rbuf, (size_t)n);

        dev_log(d, "mc: RECV %d bytes from %s:%u, code=0x%02x, uri='%s'",
                n, from_ip, from_port, req.code, req.uri_path);

        handle_mc_discovery(d, &req, from_ip, from_port);
    }
    return 0;
}

/* ============================ 服务器线程 (单播 srv_sock) ============================ */
static DWORD WINAPI server_thread(LPVOID arg) {
    device_t *d = (device_t *)arg;
    uint8_t rbuf[COAP_MAX_MSG], sbuf[COAP_MAX_MSG];

    while (d->running) {
        char from_ip[64];
        uint16_t from_port;
        int n = coap_recv(d->srv_sock, rbuf, sizeof(rbuf),
                          from_ip, &from_port, 200);
        if (n <= 0) continue;

        coap_msg_t req;
        if (coap_parse(rbuf, (size_t)n, &req) < 0) {
            dev_log(d, "server: parse failed (n=%d) from %s:%u", n, from_ip, from_port);
            continue;
        }

        proto_log_msg(d, "RX", from_ip, from_port, &req, rbuf, (size_t)n);

        dev_log(d, "server: <- %s /%s from %s:%u (type=%d, msg_id=%u)",
                coap_method_name(req.code), req.uri_path,
                from_ip, from_port, req.type, req.msg_id);

        /* 准备响应: CON->ACK, NON->NON, 回显 token 和 msg_id */
        coap_msg_t resp;
        memset(&resp, 0, sizeof(resp));
        resp.type      = (req.type == COAP_CON) ? COAP_ACK : COAP_NON;
        resp.msg_id    = req.msg_id;
        resp.token_len = req.token_len;
        memcpy(resp.token, req.token, req.token_len);

        char     file_buf[COAP_MAX_MSG - 20];
        char     info[96];
        int      info_len = 0;
        uint8_t *payload_p = NULL;
        size_t   payload_n = 0;

        /* /.well-known/core: 返回 link-format 资源列表 */
        if (strcmp(req.uri_path, "well-known/core") == 0 && req.code == COAP_GET) {
            int core_len = build_well_known(d, file_buf, sizeof(file_buf));
            resp.code           = COAP_CONTENT;
            resp.content_format = FMT_LINK_FORMAT;
            payload_p = (uint8_t *)file_buf;
            payload_n = (size_t)core_len;
            dev_log(d, "server: -> 2.05 /.well-known/core to %s:%u",
                    from_ip, from_port);

        /* /fwinfo: GET 返回当前固件版本和大小 */
        } else if (strcmp(req.uri_path, "fwinfo") == 0 && req.code == COAP_GET) {
            char ver_buf[32] = {0};
            size_t fw_size = read_fw_info(d->fw_path, ver_buf, sizeof(ver_buf));
            info_len = snprintf(info, sizeof(info), "version=%s,size=%zu",
                                ver_buf[0] ? ver_buf : d->version, fw_size);
            resp.code           = COAP_CONTENT;
            resp.content_format = FMT_TEXT_PLAIN;
            payload_p = (uint8_t *)info;
            payload_n = (size_t)info_len;
            dev_log(d, "server: -> 2.05 /fwinfo %s to %s:%u", info, from_ip, from_port);

        /* /log: GET 返回日志文件内容 */
        } else if (strcmp(req.uri_path, "log") == 0 && req.code == COAP_GET) {
            size_t log_len = 0;
            EnterCriticalSection(&d->lock);
            FILE *lf = fopen(d->log_path, "rb");
            if (lf) {
                fseek(lf, 0, SEEK_END);
                long fsize = ftell(lf);
                fseek(lf, 0, SEEK_SET);
                if (fsize > 0) {
                    log_len = (size_t)fsize;
                    if (log_len > sizeof(file_buf)) log_len = sizeof(file_buf);
                    size_t rd = fread(file_buf, 1, log_len, lf);
                    log_len = rd;
                }
                fclose(lf);
            }
            LeaveCriticalSection(&d->lock);
            resp.code           = COAP_CONTENT;
            resp.content_format = FMT_TEXT_PLAIN;
            payload_p = (uint8_t *)file_buf;
            payload_n = log_len;
            dev_log(d, "server: -> 2.05 /log (%zu bytes) to %s:%u",
                    log_len, from_ip, from_port);

        /* /firmware: GET Block2 分块返回固件文件 (客户端拉取升级自己) */
        } else if (strcmp(req.uri_path, "firmware") == 0 && req.code == COAP_GET) {
            EnterCriticalSection(&d->lock);
            FILE *fw = fopen(d->fw_path, "rb");
            if (!fw) {
                resp.code = COAP_NOT_FOUND;
                const char *err = "firmware not found";
                payload_p = (uint8_t *)err;
                payload_n = strlen(err);
                dev_log(d, "server: GET /firmware from %s:%u ; file not found",
                        from_ip, from_port);
            } else {
                fseek(fw, 0, SEEK_END);
                long fsize = ftell(fw);
                fseek(fw, 0, SEEK_SET);

                if (fsize <= 0) {
                    resp.code = COAP_NOT_FOUND;
                    dev_log(d, "server: GET /firmware from %s:%u ; empty file",
                            from_ip, from_port);
                } else {
                    size_t fw_size  = (size_t)fsize;
                    int    block_no = req.has_block2 ? req.block2_num : 0;
                    size_t offset   = (size_t)block_no * BLOCK_SIZE;

                    if (offset >= fw_size) {
                        resp.code = COAP_REQUEST_ENTITY_INCOMPLETE;
                        dev_log(d, "server: GET /firmware Block2 num=%d from %s:%u ; beyond EOF",
                                block_no, from_ip, from_port);
                    } else {
                        size_t remaining = fw_size - offset;
                        size_t chunk = remaining < BLOCK_SIZE ? remaining : BLOCK_SIZE;
                        int    more  = (offset + chunk < fw_size) ? 1 : 0;

                        fseek(fw, (long)offset, SEEK_SET);
                        size_t rd = fread(file_buf, 1, chunk, fw);
                        resp.code           = COAP_CONTENT;
                        resp.content_format = FMT_OCTET_STREAM;
                        resp.has_block2     = 1;
                        resp.block2_num     = block_no;
                        resp.block2_more    = more;
                        resp.block2_szx     = BLOCK_SZX;
                        payload_p           = (uint8_t *)file_buf;
                        payload_n           = rd;
                        dev_log(d, "server: -> 2.05 /firmware Block2 num=%d (%zu bytes, M=%d) to %s:%u",
                                block_no, rd, more, from_ip, from_port);
                    }
                }
                fclose(fw);
            }
            LeaveCriticalSection(&d->lock);

        } else {
            resp.code = (req.code == COAP_GET)
                       ? COAP_NOT_FOUND : COAP_METHOD_NOT_ALLOWED;
            const char *err = "resource not found";
            payload_p = (uint8_t *)err;
            payload_n = strlen(err);
            dev_log(d, "server: -> %s /%s (not found)",
                    coap_response_name(resp.code), req.uri_path);
        }

        resp.payload     = payload_p;
        resp.payload_len = payload_n;

        int slen = coap_build(sbuf, sizeof(sbuf), &resp);
        if (slen > 0) {
            int sent = coap_send(d->srv_sock, from_ip, from_port, sbuf, (size_t)slen);
            if (sent > 0) {
                proto_log_msg(d, "TX", from_ip, from_port, &resp, sbuf, (size_t)slen);
                dev_log(d, "server: -> %s to %s:%u (slen=%d, payload=%zu)",
                        coap_response_name(resp.code), from_ip, from_port,
                        slen, resp.payload_len);
            } else {
                dev_log(d, "server: send FAILED");
            }
        }
    }
    return 0;
}

/* ============================ 客户端 CoAP 交换 (CON 单播) ============================ */
/* 用 cli_sock 向对端 ip:port 发 CON 请求, 等待 ACK */
static int coap_exchange(device_t *d, const char *ip, uint16_t port,
                         coap_msg_t *req, coap_msg_t *resp) {
    uint8_t sbuf[COAP_MAX_MSG], rbuf[COAP_MAX_MSG];
    req->type   = COAP_CON;
    req->msg_id = d->next_msg_id++;

    if (req->payload_len == 0 && !req->has_block1) {
        req->content_format = -1;
    }

    int slen = coap_build(sbuf, sizeof(sbuf), req);
    if (slen <= 0) {
        dev_log(d, "client: coap_build FAILED");
        return -1;
    }

    for (int retry = 0; retry < 2; retry++) {
        if (coap_send(d->cli_sock, ip, port, sbuf, (size_t)slen) <= 0) {
            dev_log(d, "client: send FAILED to %s:%u", ip, port);
            return -1;
        }
        proto_log_msg(d, "TX", ip, port, req, sbuf, (size_t)slen);
        dev_log(d, "client: -> %s /%s to %s:%u (msg_id=%u, retry=%d)",
                coap_method_name(req->code), req->uri_path, ip, port,
                req->msg_id, retry);

        char from_ip[64]; uint16_t from_port;
        int n = coap_recv(d->cli_sock, rbuf, sizeof(rbuf),
                          from_ip, &from_port, 2000);
        if (n > 0 && coap_parse(rbuf, (size_t)n, resp) == 0
            && resp->msg_id == req->msg_id) {
            proto_log_msg(d, "RX", from_ip, from_port, resp, rbuf, (size_t)n);
            dev_log(d, "client: <- %s from %s:%u (msg_id=%u)",
                    coap_response_name(resp->code), from_ip, from_port,
                    resp->msg_id);
            return 0;
        }
        dev_log(d, "client: no matching ACK, retry %d", retry + 1);
    }
    return -1;
}

/* ============================ 客户端: 多播/广播发现 ============================ */
static int do_discover(device_t *d) {
    /* 生成唯一 Token */
    uint8_t token[TOKEN_LEN];
    gen_token(token, TOKEN_LEN);

    /* 构造 NON GET /.well-known/core */
    coap_msg_t req;
    coap_make_request(&req, COAP_NON, COAP_GET, "well-known/core", NULL, 0);
    req.token_len = TOKEN_LEN;
    memcpy(req.token, token, TOKEN_LEN);
    req.msg_id = d->next_msg_id++;

    uint8_t sbuf[COAP_MAX_MSG];
    int slen = coap_build(sbuf, sizeof(sbuf), &req);
    if (slen <= 0) {
        dev_log(d, "discover: coap_build FAILED");
        return -1;
    }

    /* 选择发送目标: 多播组 / 广播地址 / 单播扫描 */
    const char *target_ip;
    uint16_t    target_port;
    const char *mode_name;
    int send_count = 1;

    if (d->use_scan) {
        /* 单播扫描: 遍历端口范围, 逐个发送 (localhost 测试用) */
        mode_name = "SCAN";
        dev_log(d, "discover: -> NON GET /.well-known/core to %s ports %d-%d "
                   "(mode=SCAN, token=%02x%02x%02x%02x, wait=%dms)",
                d->scan_ip, d->scan_port_start, d->scan_port_end,
                token[0], token[1], token[2], token[3],
                DISCOVER_WAIT_MS);
        int sent_ok = 0;
        for (int p = d->scan_port_start; p <= d->scan_port_end; p++) {
            if (p == d->port) continue;  /* 跳过自己的端口, 避免自发现 */
            if (coap_send(d->cli_sock, d->scan_ip, (uint16_t)p,
                          sbuf, (size_t)slen) > 0) {
                proto_log_msg(d, "TX", d->scan_ip, (uint16_t)p,
                              &req, sbuf, (size_t)slen);
                sent_ok++;
            }
        }
        if (sent_ok == 0) {
            dev_log(d, "discover: scan send FAILED (WSAerr=%d)", WSAGetLastError());
            return -1;
        }
        (void)target_ip; (void)target_port; (void)send_count;
    } else {
        target_ip   = d->use_broadcast ? COAP_BROADCAST_ADDR : d->mc_group;
        target_port = d->mc_port;
        mode_name   = d->use_broadcast ? "BROADCAST" : "MULTICAST";

        dev_log(d, "discover: -> NON GET /.well-known/core to %s:%u "
                   "(mode=%s, token=%02x%02x%02x%02x, wait=%dms)",
                target_ip, target_port,
                mode_name,
                token[0], token[1], token[2], token[3],
                DISCOVER_WAIT_MS);

        if (coap_send(d->cli_sock, target_ip, target_port,
                      sbuf, (size_t)slen) <= 0) {
            dev_log(d, "discover: send FAILED (WSAerr=%d)", WSAGetLastError());
            return -1;
        }
        proto_log_msg(d, "TX", target_ip, target_port,
                      &req, sbuf, (size_t)slen);
    }

    /* 收集响应: 在时间窗口内循环收包, 按 Token 匹配 */
    int before = d->peer_count;
    uint32_t elapsed = 0;
    while (elapsed < DISCOVER_WAIT_MS) {
        uint8_t rbuf[COAP_MAX_MSG];
        char from_ip[64];
        uint16_t from_port;
        int n = coap_recv(d->cli_sock, rbuf, sizeof(rbuf),
                          from_ip, &from_port, 200);
        elapsed += 200;
        if (n <= 0) continue;

        coap_msg_t resp;
        if (coap_parse(rbuf, (size_t)n, &resp) < 0) {
            dev_log(d, "discover: parse failed from %s:%u", from_ip, from_port);
            continue;
        }

        proto_log_msg(d, "RX", from_ip, from_port, &resp, rbuf, (size_t)n);

        /* Token 匹配: 只接受与请求 Token 一致的响应 */
        if (resp.token_len != TOKEN_LEN ||
            memcmp(resp.token, token, TOKEN_LEN) != 0) {
            dev_log(d, "discover: token MISMATCH from %s:%u, skip", from_ip, from_port);
            continue;
        }

        if (resp.code != COAP_CONTENT) {
            dev_log(d, "discover: %s:%u replied %s (not 2.05), skip",
                    from_ip, from_port, coap_response_name(resp.code));
            continue;
        }

        /* 记录对端 */
        int added = add_peer(d, from_ip, from_port,
                             (const char *)resp.payload, resp.payload_len);
        if (added == 1) {
            char ver[32] = {0};
            extract_ver((const char *)resp.payload, ver, sizeof(ver));
            resource_t res[MAX_RESOURCES];
            int nres = parse_link_format((const char *)resp.payload, res, MAX_RESOURCES);
            dev_log(d, "discover: + peer %s:%u ver=%s (%d resources)",
                    from_ip, from_port,
                    ver[0] ? ver : "(unknown)", nres);
            /* 同时打印对端资源列表, 让用户在发现时直接看到 */
            for (int i = 0; i < nres; i++) {
                char attr_str[128] = {0};
                int off = 0;
                for (int j = 0; j < res[i].attr_count && off < (int)sizeof(attr_str) - 16; j++) {
                    off += snprintf(attr_str + off, sizeof(attr_str) - off,
                                    " %s=\"%s\"", res[i].attrs[j].name, res[i].attrs[j].value);
                }
                dev_log(d, "  %s%s", res[i].uri, attr_str);
            }
        }
    }

    int new_count = d->peer_count - before;
    dev_log(d, "discover: finished, %d new peer(s), total %d",
            new_count, d->peer_count);
    return new_count > 0 ? 0 : -1;
}

/* ============================ 客户端: 单播通信命令 ============================ */
/* 列出已发现的对端 (含资源数量) */
static void cmd_peers(device_t *d) {
    EnterCriticalSection(&d->lock);
    if (d->peer_count == 0) {
        printf("  (no peers discovered yet, run 'discover' first)\n");
        LeaveCriticalSection(&d->lock);
        return;
    }
    printf("%-4s %-22s %-10s %-8s %-10s\n",
           "No.", "IP:Port", "Version", "Res#", "Discovered");
    printf("---- ---------------------- ---------- -------- ----------\n");
    for (int i = 0; i < d->peer_count; i++) {
        if (!d->peers[i].active) continue;
        char addr[80];
        snprintf(addr, sizeof(addr), "%s:%u", d->peers[i].ip, d->peers[i].port);
        char tbuf[32] = {0};
        struct tm *tm = localtime(&d->peers[i].discovered_at);
        strftime(tbuf, sizeof(tbuf), "%H:%M:%S", tm);
        /* 统计资源数 */
        resource_t res[MAX_RESOURCES];
        int nres = parse_link_format(d->peers[i].raw_links, res, MAX_RESOURCES);
        printf("%-4d %-22s %-10s %-8d %-10s\n", i + 1, addr,
               d->peers[i].version[0] ? d->peers[i].version : "?",
               nres, tbuf);
    }
    LeaveCriticalSection(&d->lock);
    printf("\n(use 'peer_info <n>' for full resource details)\n");
}

/* peer_info <n>: 显示第 n 个对端的完整资源详情 */
static void cmd_peer_info(device_t *d, int peer_n) {
    EnterCriticalSection(&d->lock);
    if (peer_n < 1 || peer_n > d->peer_count) {
        printf("  invalid peer number (1..%d)\n", d->peer_count);
        LeaveCriticalSection(&d->lock);
        return;
    }
    peer_entry_t *e = &d->peers[peer_n - 1];
    if (!e->active) {
        printf("  peer %d is not active\n", peer_n);
        LeaveCriticalSection(&d->lock);
        return;
    }
    char tbuf[32] = {0};
    struct tm *tm = localtime(&e->discovered_at);
    strftime(tbuf, sizeof(tbuf), "%Y-%m-%d %H:%M:%S", tm);

    printf("\n=== Peer %d ===\n", peer_n);
    printf("  Address:      %s:%u\n", e->ip, e->port);
    printf("  Version:      %s\n", e->version[0] ? e->version : "(unknown)");
    printf("  Discovered:   %s\n", tbuf);
    printf("  Raw links:    %s\n", e->raw_links);
    printf("\n");
    print_peer_resources(e->raw_links);
    printf("\n");
    LeaveCriticalSection(&d->lock);
}

/* 取第 N 个对端 (1-based) */
static int get_peer(device_t *d, int n, char *ip, size_t ip_size, uint16_t *port) {
    if (n < 1 || n > d->peer_count) return -1;
    strncpy(ip, d->peers[n - 1].ip, ip_size - 1);
    ip[ip_size - 1] = '\0';
    *port = d->peers[n - 1].port;
    return 0;
}

/* get_fwinfo <n>: GET /fwinfo */
static void cmd_get_fwinfo(device_t *d, int peer_n) {
    char ip[64]; uint16_t port;
    if (get_peer(d, peer_n, ip, sizeof(ip), &port) < 0) {
        printf("  invalid peer number (1..%d)\n", d->peer_count);
        return;
    }
    coap_msg_t req, resp;
    coap_make_request(&req, COAP_CON, COAP_GET, "fwinfo", NULL, 0);
    if (coap_exchange(d, ip, port, &req, &resp) == 0) {
        char body[512] = {0};
        size_t cpy = resp.payload_len < sizeof(body) - 1
                     ? resp.payload_len : sizeof(body) - 1;
        memcpy(body, resp.payload, cpy);
        dev_log(d, "get_fwinfo: peer %s:%u -> %s ; %s",
                ip, port, coap_response_name(resp.code), body);
    } else {
        dev_log(d, "get_fwinfo: no response from %s:%u", ip, port);
    }
}

/* get_log <n>: GET /log, 保存到 <id>_log/peer_log_<ip>_<port>.log */
static void cmd_get_log(device_t *d, int peer_n) {
    char ip[64]; uint16_t port;
    if (get_peer(d, peer_n, ip, sizeof(ip), &port) < 0) {
        printf("  invalid peer number (1..%d)\n", d->peer_count);
        return;
    }
    coap_msg_t req, resp;
    coap_make_request(&req, COAP_CON, COAP_GET, "log", NULL, 0);
    if (coap_exchange(d, ip, port, &req, &resp) == 0) {
        char body[COAP_MAX_MSG] = {0};
        size_t cpy = resp.payload_len < sizeof(body) - 1
                     ? resp.payload_len : sizeof(body) - 1;
        memcpy(body, resp.payload, cpy);
        dev_log(d, "get_log: peer %s:%u -> %s (%zu bytes):",
                ip, port, coap_response_name(resp.code), resp.payload_len);
        printf("---- log from %s:%u ----\n%s\n---- end ----\n", ip, port, body);

        /* 落盘: 保存到 device_<id>_log/peer_log_<ip>_<port>.log (覆盖旧文件) */
        char save_path[128];
        snprintf(save_path, sizeof(save_path),
                 "device_%s_log/peer_log_%s_%u.log", d->id, ip, port);
        FILE *out = fopen(save_path, "w");
        if (out) {
            fwrite(body, 1, cpy, out);
            fclose(out);
            dev_log(d, "get_log: saved to %s (%zu bytes)", save_path, cpy);
        } else {
            dev_log(d, "get_log: failed to save %s", save_path);
        }
    } else {
        dev_log(d, "get_log: no response from %s:%u", ip, port);
    }
}

/* get_fw <n>: GET /firmware Block2 从对端拉取固件并升级自己 */
static void cmd_get_fw(device_t *d, int peer_n) {
    char ip[64]; uint16_t port;
    if (get_peer(d, peer_n, ip, sizeof(ip), &port) < 0) {
        printf("  invalid peer number (1..%d)\n", d->peer_count);
        return;
    }

    dev_log(d, "get_fw: -> GET /firmware to %s:%u (Block2 pull, block=%d bytes)",
            ip, port, BLOCK_SIZE);

    uint8_t *fw_data = NULL;
    size_t   fw_total = 0;
    int      block_num = 0;
    int      done = 0;

    while (!done) {
        coap_msg_t req, resp;
        memset(&req, 0, sizeof(req));
        req.code = COAP_GET;
        strncpy(req.uri_path, "firmware", sizeof(req.uri_path) - 1);

        /* 首块不带 Block2 选项, 后续块带 (num=N, SZX=4) */
        if (block_num > 0) {
            req.has_block2 = 1;
            req.block2_num = block_num;
            req.block2_szx = BLOCK_SZX;
        }

        dev_log(d, "get_fw: -> block %d to %s:%u", block_num, ip, port);

        if (coap_exchange(d, ip, port, &req, &resp) != 0) {
            dev_log(d, "get_fw: no response for block %d, abort", block_num);
            free(fw_data);
            return;
        }

        if (resp.code != COAP_CONTENT) {
            dev_log(d, "get_fw: server returned %s for block %d, abort",
                    coap_response_name(resp.code), block_num);
            free(fw_data);
            return;
        }

        /* 累积收到的块 */
        if (resp.payload_len > 0) {
            uint8_t *nb = (uint8_t *)realloc(fw_data, fw_total + resp.payload_len);
            if (!nb) {
                dev_log(d, "get_fw: realloc failed at block %d", block_num);
                free(fw_data);
                return;
            }
            fw_data = nb;
            memcpy(fw_data + fw_total, resp.payload, resp.payload_len);
            fw_total += resp.payload_len;
            dev_log(d, "get_fw: <- block %d (%zu bytes, M=%d), total %zu",
                    block_num, resp.payload_len,
                    resp.has_block2 ? resp.block2_more : 0, fw_total);
        }

        /* 判断是否最后一块 */
        if (!resp.has_block2 || !resp.block2_more) {
            done = 1;
        } else {
            block_num++;
        }
    }

    if (!fw_data || fw_total == 0) {
        dev_log(d, "get_fw: no firmware data received");
        free(fw_data);
        return;
    }

    dev_log(d, "get_fw: pull complete, %zu bytes from %s:%u",
            fw_total, ip, port);

    /* 落盘: 写入本机固件文件 */
    EnterCriticalSection(&d->lock);
    FILE *fw = fopen(d->fw_path, "wb");
    if (!fw) {
        LeaveCriticalSection(&d->lock);
        dev_log(d, "get_fw: failed to open local fw file for write");
        free(fw_data);
        return;
    }
    fwrite(fw_data, 1, fw_total, fw);
    fclose(fw);

    /* 版本号 bump: 1.0.0-A -> 1.0.1-A -> ... (保留自己后缀, 只递增 patch) */
    int  major = 1, minor = 0, patch = 0;
    char suffix[16] = {0};
    if (sscanf(d->version, "%d.%d.%d-%15s",
               &major, &minor, &patch, suffix) >= 3) {
        patch++;
        if (patch > 9) { patch = 0; minor++; }
        if (minor > 9) { minor = 0; major++; }
        snprintf(d->version, sizeof(d->version),
                 "%d.%d.%d-%s", major, minor, patch, suffix);
    } else {
        char newver[32];
        snprintf(newver, sizeof(newver), "%s-v2", d->version);
        strncpy(d->version, newver, sizeof(d->version) - 1);
        d->version[sizeof(d->version) - 1] = '\0';
    }

    /* 把新版本号写入固件文件首行 (覆盖原首行, 保留 body) */
    fw = fopen(d->fw_path, "r+b");
    if (fw) {
        char line[64];
        if (fgets(line, sizeof(line), fw)) {
            long body_offset = ftell(fw);
            fseek(fw, 0, SEEK_END);
            long total = ftell(fw);
            long body_len = total - body_offset;
            uint8_t *body = body_len > 0
                            ? (uint8_t *)malloc((size_t)body_len) : NULL;
            if (body && body_len > 0) {
                fseek(fw, body_offset, SEEK_SET);
                fread(body, 1, (size_t)body_len, fw);
            }
            fclose(fw);
            fw = fopen(d->fw_path, "wb");
            if (fw) {
                fprintf(fw, "%s\n", d->version);
                if (body && body_len > 0)
                    fwrite(body, 1, (size_t)body_len, fw);
                fclose(fw);
            }
            free(body);
        } else {
            fclose(fw);
        }
    }
    LeaveCriticalSection(&d->lock);

    dev_log(d, "get_fw: upgrade complete, version -> %s", d->version);
    free(fw_data);
}

/* ============================ 主函数 ============================ */
int main(int argc, char **argv) {
    device_t d;
    memset(&d, 0, sizeof(d));
    InitializeCriticalSection(&d.lock);
    d.running     = 1;
    d.next_msg_id = (uint16_t)(time(NULL) & 0xffff);
    d.mc_port     = DEFAULT_MC_PORT;
    d.mc_ttl      = 16;
    strncpy(d.mc_group, COAP_MULTICAST_GROUP, sizeof(d.mc_group) - 1);
    strncpy(d.version, "1.0.0", sizeof(d.version) - 1);
    strncpy(d.scan_ip, "127.0.0.1", sizeof(d.scan_ip) - 1);
    d.scan_port_start = 5683;
    d.scan_port_end   = 5687;
    srand((unsigned)time(NULL) ^ (unsigned)GetCurrentProcessId());

    for (int i = 1; i < argc; i++) {
        if      (!strcmp(argv[i], "--id")        && i + 1 < argc) strncpy(d.id,        argv[++i], sizeof(d.id) - 1);
        else if (!strcmp(argv[i], "--port")      && i + 1 < argc) d.port      = (uint16_t)atoi(argv[++i]);
        else if (!strcmp(argv[i], "--version")   && i + 1 < argc) strncpy(d.version, argv[++i], sizeof(d.version) - 1);
        else if (!strcmp(argv[i], "--mc-port")   && i + 1 < argc) d.mc_port   = (uint16_t)atoi(argv[++i]);
        else if (!strcmp(argv[i], "--mc-group")  && i + 1 < argc) strncpy(d.mc_group, argv[++i], sizeof(d.mc_group) - 1);
        else if (!strcmp(argv[i], "--mc-ttl")    && i + 1 < argc) d.mc_ttl    = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--bcast"))                     d.use_broadcast = 1;
        else if (!strcmp(argv[i], "--scan"))                      d.use_scan = 1;
        else if (!strcmp(argv[i], "--scan-ip")   && i + 1 < argc) strncpy(d.scan_ip, argv[++i], sizeof(d.scan_ip) - 1);
        else if (!strcmp(argv[i], "--scan-ports") && i + 1 < argc) {
            /* 支持 "5683-5687" 或 "5683,5684,5685" 两种格式, 这里只解析前者 */
            const char *s = argv[++i];
            int a = 0, b = 0;
            if (sscanf(s, "%d-%d", &a, &b) == 2 && a > 0 && b >= a) {
                d.scan_port_start = a;
                d.scan_port_end   = b;
            }
        }
        else {
            fprintf(stderr,
                "Usage: %s --id A --port 5683 --version 1.0.0-A\n"
                "       [--mc-port 5683] [--mc-group 224.0.1.187] [--mc-ttl 16]\n"
                "       [--bcast]   (LAN broadcast, may NOT work on Windows loopback)\n"
                "       [--scan]    (unicast scan, RECOMMENDED for localhost test)\n"
                "       [--scan-ip 127.0.0.1] [--scan-ports 5683-5687]\n",
                argv[0]);
            DeleteCriticalSection(&d.lock);
            return 1;
        }
    }

    if (d.id[0] == 0 || d.port == 0) {
        fprintf(stderr, "Error: --id and --port are required\n");
        DeleteCriticalSection(&d.lock);
        return 1;
    }

    /* 创建目录和文件路径 */
    {
        char dir_cmd[512];
        snprintf(dir_cmd, sizeof(dir_cmd),
                 "cmd /c \"if not exist device_%s_log mkdir device_%s_log & "
                 "if not exist device_%s_bin mkdir device_%s_bin\"",
                 d.id, d.id, d.id, d.id);
        system(dir_cmd);
    }
    snprintf(d.fw_path,      sizeof(d.fw_path),      "device_%s_bin/firmware_%s.bin", d.id, d.id);
    snprintf(d.fw_orig_path, sizeof(d.fw_orig_path), "device_%s_bin/firmware_%s_orig.bin", d.id, d.id);
    snprintf(d.log_path,     sizeof(d.log_path),     "device_%s_log/device_%s.log", d.id, d.id);

    /* 创建初始固件文件 (当前 + 原始副本, 用于升级对端) */
    {
        const char *paths[] = { d.fw_path, d.fw_orig_path };
        for (int p = 0; p < 2; p++) {
            FILE *fw = fopen(paths[p], "wb");
            if (fw) {
                fprintf(fw, "%s\n", d.version);
                for (int i = 0; i < FW_FILLER_LEN; i++) fputc(i & 0xff, fw);
                fclose(fw);
            }
        }
    }

    d.log_fp = fopen(d.log_path, "w");

    /* 协议日志: 记录每条 CoAP 报文的原始字节和解析字段 */
    InitializeCriticalSection(&d.proto_lock);
    {
        char proto_path[96];
        snprintf(proto_path, sizeof(proto_path),
                 "device_%s_log/proto_%s.log", d.id, d.id);
        d.proto_fp = fopen(proto_path, "w");
        if (d.proto_fp) {
            time_t now = time(NULL);
            fprintf(d.proto_fp,
                    "=== CoAP Protocol Log for Device %s ===\n"
                    "Started:    %s"
                    "Listen:     :%u\n"
                    "Mode:       %s\n"
                    "Version:    %s\n"
                    "========================================\n\n",
                    d.id, ctime(&now), d.port,
                    d.use_scan     ? "SCAN" :
                    d.use_broadcast ? "BROADCAST" : "MULTICAST",
                    d.version);
            fflush(d.proto_fp);
        }
    }

    if (coap_init() != 0) {
        fprintf(stderr, "WSAStartup failed\n");
        DeleteCriticalSection(&d.lock);
        DeleteCriticalSection(&d.proto_lock);
        return 1;
    }

    /* srv_sock: 独占端口, 单播服务 */
    d.srv_sock = coap_open_socket(d.port);
    /* cli_sock: 临时端口, 客户端发送 + 接收响应 */
    d.cli_sock = coap_open_socket(0);
    if (d.srv_sock == INVALID_SOCKET || d.cli_sock == INVALID_SOCKET) {
        fprintf(stderr, "socket creation failed (port %u in use?)\n", d.port);
        if (d.proto_fp) fclose(d.proto_fp);
        coap_cleanup();
        DeleteCriticalSection(&d.lock);
        DeleteCriticalSection(&d.proto_lock);
        return 1;
    }

    /* cli_sock: 设置多播 TTL / 广播, 用于发送发现请求 */
    if (d.use_scan) {
        /* 单播扫描: cli_sock 默认即可, 无需特殊选项 */
    } else if (d.use_broadcast) {
        enable_broadcast(d.cli_sock);
    } else {
        set_multicast_ttl(d.cli_sock, d.mc_ttl);
        enable_multicast_loop(d.cli_sock);
    }

    /* mc_sock: 公共多播发现端口, 加入多播组
       Windows 上需 SO_REUSEADDR, 让多设备同机时都能绑到同一公共端口
       scan/bcast 模式不需要 mc_sock (单播扫描或广播都直接打到 srv_sock) */
    d.mc_sock = INVALID_SOCKET;
    if (!d.use_broadcast && !d.use_scan) {
        d.mc_sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
        if (d.mc_sock != INVALID_SOCKET) {
            BOOL opt = TRUE;
            setsockopt(d.mc_sock, SOL_SOCKET, SO_REUSEADDR,
                       (const char *)&opt, sizeof(opt));
            struct sockaddr_in addr;
            memset(&addr, 0, sizeof(addr));
            addr.sin_family      = AF_INET;
            addr.sin_addr.s_addr = htonl(INADDR_ANY);
            addr.sin_port        = htons(d.mc_port);
            if (bind(d.mc_sock, (struct sockaddr *)&addr, sizeof(addr)) == SOCKET_ERROR) {
                /* 绑定失败: 可能本机已有其它进程占用, 忽略, 仅不再接收多播 */
                dev_log(&d, "mc_sock bind :%u failed (WSAerr=%d), "
                            "will not receive multicast (other process on this port?)",
                            d.mc_port, WSAGetLastError());
                closesocket(d.mc_sock);
                d.mc_sock = INVALID_SOCKET;
            } else if (join_multicast(d.mc_sock, d.mc_group) == SOCKET_ERROR) {
                dev_log(&d, "mc_sock join multicast %s failed (WSAerr=%d)",
                            d.mc_group, WSAGetLastError());
                /* 加入失败仍可工作, 仅收不到多播请求 */
            } else {
                dev_log(&d, "mc_sock joined multicast %s on port %u",
                            d.mc_group, d.mc_port);
            }
        }
    }

    dev_log(&d, "==== Device %s started: srv=:%u, mode=%s, version=%s ====",
            d.id, d.port,
            d.use_scan     ? "SCAN" :
            d.use_broadcast ? "BROADCAST" : "MULTICAST",
            d.version);
    if (d.use_scan) {
        dev_log(&d, "  scan target: %s ports %d-%d",
                d.scan_ip, d.scan_port_start, d.scan_port_end);
    } else if (!d.use_broadcast) {
        dev_log(&d, "  multicast: %s:%u (ttl=%d)", d.mc_group, d.mc_port, d.mc_ttl);
    }

    /* 启动服务器线程 */
    HANDLE th_srv = CreateThread(NULL, 0, server_thread, &d, 0, NULL);
    HANDLE th_mc  = NULL;
    if (d.mc_sock != INVALID_SOCKET)
        th_mc = CreateThread(NULL, 0, multicast_thread, &d, 0, NULL);

    Sleep(500);  /* 等线程就绪 */

    /* 交互式命令循环 */
    printf("\n=== CoAP Direct-Link Device %s (port %u, mode=%s) ===\n",
           d.id, d.port,
           d.use_scan     ? "SCAN" :
           d.use_broadcast ? "BROADCAST" : "MULTICAST");
    printf("Commands:\n");
    printf("  discover              - Discover peers (per startup mode)\n");
    printf("  peers                 - List discovered peers (brief)\n");
    printf("  peer_info <n>         - Show full resource details of peer n\n");
    printf("  get_fwinfo <n>        - GET /fwinfo from peer n\n");
    printf("  get_log <n>           - GET /log from peer n\n");
    printf("  get_fw <n>            - GET /firmware (Block2) from peer n, upgrade self\n");
    printf("  status                - Show device status\n");
    printf("  help                  - Show this help\n");
    printf("  quit                  - Exit\n");
    printf("========================================\n\n");

    char cmd[256];
    while (d.running) {
        printf("[%s] command> ", d.id);
        fflush(stdout);
        if (!fgets(cmd, sizeof(cmd), stdin)) break;

        size_t len = strlen(cmd);
        while (len > 0 && (cmd[len-1] == '\n' || cmd[len-1] == '\r'))
            cmd[--len] = '\0';
        if (len == 0) continue;

        if (!strcmp(cmd, "quit") || !strcmp(cmd, "exit")) {
            break;
        } else if (!strcmp(cmd, "help")) {
            printf("  discover, peers, peer_info <n>, get_fwinfo <n>, get_log <n>, get_fw <n>, status, quit\n");
        } else if (!strcmp(cmd, "status")) {
            const char *mode = d.use_scan     ? "SCAN" :
                               d.use_broadcast ? "BROADCAST" : "MULTICAST";
            dev_log(&d, "Status: id=%s, port=%u, version=%s, peers=%d, mode=%s",
                    d.id, d.port, d.version, d.peer_count, mode);
            if (d.use_scan) {
                dev_log(&d, "  scan: %s ports %d-%d",
                        d.scan_ip, d.scan_port_start, d.scan_port_end);
            }
        } else if (!strcmp(cmd, "discover")) {
            do_discover(&d);
        } else if (!strcmp(cmd, "peers")) {
            cmd_peers(&d);
        } else if (!strncmp(cmd, "peer_info ", 10)) {
            cmd_peer_info(&d, atoi(cmd + 10));
        } else if (!strncmp(cmd, "get_fwinfo ", 11)) {
            cmd_get_fwinfo(&d, atoi(cmd + 11));
        } else if (!strncmp(cmd, "get_log ", 8)) {
            cmd_get_log(&d, atoi(cmd + 8));
        } else if (!strncmp(cmd, "get_fw ", 7)) {
            cmd_get_fw(&d, atoi(cmd + 7));
        } else {
            printf("  unknown command: %s (try 'help')\n", cmd);
        }
    }

    /* 清理 */
    d.running = 0;
    if (th_srv) { WaitForSingleObject(th_srv, 1500); CloseHandle(th_srv); }
    if (th_mc)  { WaitForSingleObject(th_mc, 1500);  CloseHandle(th_mc);  }

    if (d.mc_sock != INVALID_SOCKET) closesocket(d.mc_sock);
    coap_close_socket(d.srv_sock);
    coap_close_socket(d.cli_sock);
    if (d.log_fp) fclose(d.log_fp);
    if (d.proto_fp) {
        fprintf(d.proto_fp, "\n=== Device %s exited ===\n", d.id);
        fclose(d.proto_fp);
    }
    coap_cleanup();
    DeleteCriticalSection(&d.lock);
    DeleteCriticalSection(&d.proto_lock);
    printf("[%s] exited\n", d.id);
    return 0;
}
