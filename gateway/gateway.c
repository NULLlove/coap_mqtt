/*
 * gateway.c - CoAP ↔ MQTT 协议网关
 *
 * 架构:
 *   边缘侧 (CoAP 设备) <──CoAP──> [网关] <──MQTT──> [Broker] <──MQTT──> [云端应用]
 *
 * 网关职责:
 *   1. 上行采集: 定时用 CoAP 轮询设备的 fwinfo, 转 MQTT 发布到云端
 *   2. 下行控制: 监听 MQTT 命令主题, 转 CoAP 请求发给边缘设备
 *   3. 协议翻译: CoAP 资源 (/fwinfo, /log, /firmware) ↔ MQTT 主题 (devices/<id>/...)
 *
 * 编译: gcc -Wall -Wextra -O2 -I../coap -I../mqtt -o gateway.exe ^
 *           ../coap/coap.c ../mqtt/mqtt.c gateway.c -lws2_32
 *
 * 使用:
 *   gateway.exe --broker-ip 127.0.0.1 --broker-port 1883 ^
 *               --device A@127.0.0.1:5683 ^
 *               --device B@127.0.0.1:5684
 *
 * MQTT 主题设计:
 *   网关订阅:  gateway/<device_id>/command   (云端下发命令)
 *   网关发布:  devices/<device_id>/fwinfo    (固件信息)
 *              devices/<device_id>/log       (日志)
 *              devices/<device_id>/status    (命令执行结果)
 *
 *   命令格式 (MQTT payload):
 *     "fwinfo"                    -> CoAP GET /fwinfo
 *     "log"                       -> CoAP GET /log
 *     "get_fw <version>"          -> CoAP GET /fwinfo?version=<version>
 *     "upgrade <firmware_file>"   -> CoAP PUT /firmware (Block1 分块)
 */
#include "coap.h"
#include "mqtt.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>
#include <time.h>

#define MAX_DEVICES         16
#define COLLECT_INTERVAL_MS 30000   /* 采集间隔: 30 秒 */

/* ==================== 设备注册表 ==================== */
typedef struct {
    char     id[16];        /* 设备 ID, 如 "A" */
    char     coap_ip[64];   /* 设备 CoAP 地址 */
    uint16_t coap_port;     /* 设备 CoAP 端口 */
} device_entry_t;

/* ==================== 网关全局状态 ==================== */
typedef struct {
    /* MQTT 配置 */
    char     broker_ip[64];
    uint16_t broker_port;

    /* 设备注册表 */
    device_entry_t devices[MAX_DEVICES];
    int      device_count;

    /* 套接字 */
    SOCKET   coap_sock;    /* CoAP 客户端 UDP socket (不绑定端口) */
    SOCKET   mqtt_sock;    /* MQTT 客户端 TCP socket (连 broker) */

    /* 报文 ID 生成器 */
    uint16_t coap_msg_id;
    uint16_t mqtt_pkt_id;

    /* 同步锁 */
    CRITICAL_SECTION lock;       /* 保护日志文件 */
    CRITICAL_SECTION coap_lock;  /* 保护 CoAP 交换 (UDP socket 共享) */

    FILE     *log_fp;            /* 网关日志文件 */
    volatile int running;
} gateway_t;

/* ==================== 日志 ==================== */
static void gw_log(gateway_t *gw, const char *fmt, ...) {
    char line[512];
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(line, sizeof(line), fmt, ap);
    va_end(ap);
    if (n < 0) return;
    if (n > (int)sizeof(line) - 1) n = (int)sizeof(line) - 1;

    time_t now = time(NULL);
    struct tm *tm = localtime(&now);
    char time_str[64];
    strftime(time_str, sizeof(time_str), "%Y-%m-%d %H:%M:%S", tm);

    printf("[gateway] [%s] %.*s\n", time_str, n, line);
    fflush(stdout);

    EnterCriticalSection(&gw->lock);
    if (gw->log_fp) {
        fprintf(gw->log_fp, "[%s] %.*s\n", time_str, n, line);
        fflush(gw->log_fp);
    }
    LeaveCriticalSection(&gw->lock);
}

/* ==================== CoAP 交换 ==================== */
/* 封装: 构造报文 -> 发送 -> 接收响应 -> 解析
   使用 coap_lock 保证同一时刻只有一个线程使用 CoAP socket */
static int coap_exchange(gateway_t *gw, const char *ip, uint16_t port,
                         coap_msg_t *req, coap_msg_t *resp) {
    uint8_t sbuf[COAP_MAX_MSG], rbuf[COAP_MAX_MSG];
    req->type   = COAP_CON;
    req->msg_id = gw->coap_msg_id++;

    /* 无 payload 且非分块时, 不编码 Content-Format 选项 */
    if (req->payload_len == 0 && !req->has_block1) {
        req->content_format = -1;
    }

    int slen = coap_build(sbuf, sizeof(sbuf), req);
    if (slen <= 0) {
        gw_log(gw, "coap_exchange: coap_build failed");
        return -1;
    }

    int result = -1;
    EnterCriticalSection(&gw->coap_lock);
    for (int retry = 0; retry < 2; retry++) {
        if (coap_send(gw->coap_sock, ip, port, sbuf, slen) <= 0) {
            gw_log(gw, "coap_exchange: send failed (retry %d)", retry);
            continue;
        }

        char from_ip[64];
        uint16_t from_port;
        int n = coap_recv(gw->coap_sock, rbuf, sizeof(rbuf),
                          from_ip, &from_port, 2000);
        if (n > 0) {
            if (coap_parse(rbuf, (size_t)n, resp) == 0 &&
                resp->msg_id == req->msg_id) {
                result = 0;
                break;
            }
        }
    }
    LeaveCriticalSection(&gw->coap_lock);
    return result;
}

/* ==================== MQTT 发布辅助 ==================== */
static void mqtt_pub(gateway_t *gw, const char *topic,
                     const uint8_t *payload, size_t len) {
    mqtt_msg_t msg;
    mqtt_make_publish(&msg, topic, payload, len, MQTT_QOS_1, gw->mqtt_pkt_id++);
    if (mqtt_send_packet(gw->mqtt_sock, &msg) < 0) {
        gw_log(gw, "mqtt_pub: send failed for topic '%s'", topic);
    }
}

/* ==================== 查找设备 ==================== */
static device_entry_t *find_device(gateway_t *gw, const char *id) {
    for (int i = 0; i < gw->device_count; i++) {
        if (strcmp(gw->devices[i].id, id) == 0)
            return &gw->devices[i];
    }
    return NULL;
}

/* ==================== CoAP 命令执行 ==================== */

/* 获取固件信息 (query 可为 NULL / "list" / "version=XXX") */
static void cmd_fwinfo(gateway_t *gw, device_entry_t *dev, const char *query) {
    coap_msg_t req, resp;
    memset(&req, 0, sizeof(req));
    req.code = COAP_GET;
    strncpy(req.uri_path, "fwinfo", sizeof(req.uri_path) - 1);
    if (query && query[0]) {
        strncpy(req.uri_query, query, sizeof(req.uri_query) - 1);
    }

    if (coap_exchange(gw, dev->coap_ip, dev->coap_port, &req, &resp) == 0) {
        char topic[128];
        snprintf(topic, sizeof(topic), "devices/%s/fwinfo", dev->id);
        mqtt_pub(gw, topic, resp.payload, resp.payload_len);

        /* 打印响应内容 (截断显示) */
        char body[128] = {0};
        size_t cpy = resp.payload_len < sizeof(body) - 1 ?
                     resp.payload_len : sizeof(body) - 1;
        memcpy(body, resp.payload, cpy);
        gw_log(gw, "cmd_fwinfo: device=%s -> MQTT %s (%zu bytes): %s",
               dev->id, topic, resp.payload_len, body);
    } else {
        gw_log(gw, "cmd_fwinfo: device=%s, CoAP exchange failed", dev->id);

        /* 发布失败状态 */
        char topic[128], payload[128];
        snprintf(topic, sizeof(topic), "devices/%s/status", dev->id);
        snprintf(payload, sizeof(payload), "fwinfo=failed");
        mqtt_pub(gw, topic, (uint8_t *)payload, strlen(payload));
    }
}

/* 获取日志 */
static void cmd_log(gateway_t *gw, device_entry_t *dev) {
    coap_msg_t req, resp;
    memset(&req, 0, sizeof(req));
    req.code = COAP_GET;
    strncpy(req.uri_path, "log", sizeof(req.uri_path) - 1);

    if (coap_exchange(gw, dev->coap_ip, dev->coap_port, &req, &resp) == 0) {
        char topic[128];
        snprintf(topic, sizeof(topic), "devices/%s/log", dev->id);
        mqtt_pub(gw, topic, resp.payload, resp.payload_len);
        gw_log(gw, "cmd_log: device=%s -> MQTT %s (%zu bytes)",
               dev->id, topic, resp.payload_len);
    } else {
        gw_log(gw, "cmd_log: device=%s, CoAP exchange failed", dev->id);

        char topic[128], payload[128];
        snprintf(topic, sizeof(topic), "devices/%s/status", dev->id);
        snprintf(payload, sizeof(payload), "log=failed");
        mqtt_pub(gw, topic, (uint8_t *)payload, strlen(payload));
    }
}

/* 固件升级 (CoAP PUT /firmware, Block1 分块传输) */
static void cmd_upgrade(gateway_t *gw, device_entry_t *dev, const char *fw_path) {
    /* 读取固件文件 */
    FILE *fw = fopen(fw_path, "rb");
    if (!fw) {
        gw_log(gw, "cmd_upgrade: firmware file not found: %s", fw_path);
        return;
    }
    fseek(fw, 0, SEEK_END);
    long fsize = ftell(fw);
    fseek(fw, 0, SEEK_SET);
    if (fsize <= 0) {
        gw_log(gw, "cmd_upgrade: firmware file empty");
        fclose(fw);
        return;
    }

    size_t image_len = (size_t)fsize;
    uint8_t *image = (uint8_t *)malloc(image_len);
    if (!image) {
        gw_log(gw, "cmd_upgrade: malloc failed (%zu bytes)", image_len);
        fclose(fw);
        return;
    }
    size_t total_read = fread(image, 1, image_len, fw);
    fclose(fw);
    if (total_read != image_len) {
        gw_log(gw, "cmd_upgrade: read failed");
        free(image);
        return;
    }

    gw_log(gw, "cmd_upgrade: device=%s, firmware=%s (%zu bytes), block=%d bytes",
           dev->id, fw_path, image_len, BLOCK_SIZE);

    /* Block1 分块推送 */
    size_t offset   = 0;
    int    block_no = 0;
    int    success  = 1;

    while (1) {
        size_t chunk = BLOCK_SIZE;
        if (offset + chunk > image_len) chunk = image_len - offset;
        int more = (offset + chunk < image_len) ? 1 : 0;

        coap_msg_t req, resp;
        memset(&req, 0, sizeof(req));
        req.code           = COAP_PUT;
        strncpy(req.uri_path, "firmware", sizeof(req.uri_path) - 1);
        req.content_format = FMT_OCTET_STREAM;
        req.has_block1     = 1;
        req.block1_num     = block_no;
        req.block1_more    = more;
        req.block1_szx     = BLOCK_SZX;
        req.payload        = image + offset;
        req.payload_len    = chunk;

        if (coap_exchange(gw, dev->coap_ip, dev->coap_port, &req, &resp) == 0) {
            if (resp.code != COAP_CHANGED) {
                gw_log(gw, "cmd_upgrade: block %d rejected (code=0x%02x)",
                       block_no, resp.code);
                success = 0;
                break;
            }
            gw_log(gw, "cmd_upgrade: block %d ok (%zu bytes, M=%d)",
                   block_no, chunk, more);
        } else {
            gw_log(gw, "cmd_upgrade: block %d no ACK, abort", block_no);
            success = 0;
            break;
        }

        offset += chunk;
        block_no++;
        if (!more) break;
    }
    free(image);

    /* 发布升级结果到 MQTT */
    char topic[128], payload[128];
    snprintf(topic, sizeof(topic), "devices/%s/status", dev->id);
    snprintf(payload, sizeof(payload), "upgrade=%s,blocks=%d",
             success ? "ok" : "failed", block_no);
    mqtt_pub(gw, topic, (uint8_t *)payload, strlen(payload));
    gw_log(gw, "cmd_upgrade: device=%s, result=%s", dev->id,
           success ? "ok" : "failed");
}

/* ==================== 上行采集线程 ==================== */
/* 定时轮询所有 CoAP 设备的固件信息, 转 MQTT 发布到云端 */
static DWORD WINAPI collect_thread(LPVOID arg) {
    gateway_t *gw = (gateway_t *)arg;
    while (gw->running) {
        Sleep(COLLECT_INTERVAL_MS);
        if (!gw->running) break;

        gw_log(gw, "collect: polling %d device(s)...", gw->device_count);
        for (int i = 0; i < gw->device_count; i++) {
            if (!gw->running) break;
            cmd_fwinfo(gw, &gw->devices[i], NULL);
        }
    }
    return 0;
}

/* ==================== MQTT 接收线程 (下行命令处理) ==================== */
/* 监听 gateway/<device_id>/command 主题, 将命令转为 CoAP 请求发给设备 */
static DWORD WINAPI mqtt_recv_thread(LPVOID arg) {
    gateway_t *gw = (gateway_t *)arg;

    while (gw->running) {
        mqtt_msg_t msg;
        int rc = mqtt_recv_packet(gw->mqtt_sock, &msg, 1000);
        if (rc < 0) {
            if (gw->running) {
                gw_log(gw, "mqtt_recv: connection lost");
            }
            break;
        }

        if (msg.type == MQTT_PUBLISH) {
            gw_log(gw, "mqtt_recv: PUBLISH topic='%s', payload_len=%zu",
                   msg.topic, msg.payload_len);

            /* 解析主题: gateway/<device_id>/command */
            char topic_copy[128];
            strncpy(topic_copy, msg.topic, sizeof(topic_copy) - 1);
            topic_copy[sizeof(topic_copy) - 1] = '\0';

            char *parts[4] = {0};
            char *p = strtok(topic_copy, "/");
            for (int i = 0; i < 4 && p; i++) {
                parts[i] = p;
                p = strtok(NULL, "/");
            }

            if (parts[0] && strcmp(parts[0], "gateway") == 0 &&
                parts[1] && parts[2] && strcmp(parts[2], "command") == 0) {

                char *device_id = parts[1];
                device_entry_t *dev = find_device(gw, device_id);

                if (!dev) {
                    gw_log(gw, "mqtt_recv: unknown device '%s'", device_id);
                } else {
                    /* 解析 payload 中的命令 */
                    char cmd[256] = {0};
                    size_t cpy = msg.payload_len < sizeof(cmd) - 1 ?
                                 msg.payload_len : sizeof(cmd) - 1;
                    memcpy(cmd, msg.payload, cpy);
                    cmd[cpy] = '\0';

                    /* 去除换行符 */
                    size_t len = strlen(cmd);
                    while (len > 0 && (cmd[len-1] == '\n' || cmd[len-1] == '\r'))
                        cmd[--len] = '\0';

                    gw_log(gw, "mqtt_recv: command='%s' for device=%s", cmd, device_id);

                    /* 分发命令 */
                    if (strcmp(cmd, "fwinfo") == 0) {
                        cmd_fwinfo(gw, dev, NULL);
                    } else if (strcmp(cmd, "log") == 0) {
                        cmd_log(gw, dev);
                    } else if (strncmp(cmd, "get_fw ", 7) == 0) {
                        char query[64];
                        snprintf(query, sizeof(query), "version=%.48s", cmd + 7);
                        cmd_fwinfo(gw, dev, query);
                    } else if (strncmp(cmd, "upgrade ", 8) == 0) {
                        cmd_upgrade(gw, dev, cmd + 8);
                    } else {
                        gw_log(gw, "mqtt_recv: unknown command '%s'", cmd);
                    }
                }
            }

            /* QoS 1: 回 PUBACK */
            if (msg.qos == MQTT_QOS_1) {
                mqtt_msg_t ack;
                mqtt_make_puback(&ack, msg.packet_id);
                mqtt_send_packet(gw->mqtt_sock, &ack);
            }
        } else if (msg.type == MQTT_PUBACK) {
            gw_log(gw, "mqtt_recv: PUBACK for packet_id=%u", msg.packet_id);
        } else if (msg.type == MQTT_PINGRESP) {
            /* 心跳响应, 静默处理 */
        }
    }
    return 0;
}

/* ==================== MQTT 心跳线程 ==================== */
static DWORD WINAPI ping_thread(LPVOID arg) {
    gateway_t *gw = (gateway_t *)arg;
    while (gw->running) {
        Sleep(5000);
        if (gw->running) {
            mqtt_msg_t ping;
            mqtt_make_pingreq(&ping);
            mqtt_send_packet(gw->mqtt_sock, &ping);
        }
    }
    return 0;
}

/* ==================== 解析设备参数 ==================== */
/* 格式: <id>@<ip>:<port>  例如: A@127.0.0.1:5683 */
static int parse_device_arg(const char *arg, device_entry_t *dev) {
    const char *at = strchr(arg, '@');
    if (!at) return -1;

    size_t id_len = (size_t)(at - arg);
    if (id_len >= sizeof(dev->id)) return -1;
    memcpy(dev->id, arg, id_len);
    dev->id[id_len] = '\0';

    const char *colon = strchr(at + 1, ':');
    if (!colon) return -1;

    size_t ip_len = (size_t)(colon - (at + 1));
    if (ip_len >= sizeof(dev->coap_ip)) return -1;
    memcpy(dev->coap_ip, at + 1, ip_len);
    dev->coap_ip[ip_len] = '\0';

    dev->coap_port = (uint16_t)atoi(colon + 1);
    return 0;
}

/* ==================== main ==================== */
int main(int argc, char **argv) {
    gateway_t gw;
    memset(&gw, 0, sizeof(gw));
    InitializeCriticalSection(&gw.lock);
    InitializeCriticalSection(&gw.coap_lock);
    gw.running     = 1;
    gw.broker_port = MQTT_DEFAULT_PORT;
    strncpy(gw.broker_ip, "127.0.0.1", sizeof(gw.broker_ip) - 1);
    gw.coap_msg_id = (uint16_t)(time(NULL) & 0xffff);
    gw.mqtt_pkt_id = 1;

    /* 解析命令行参数 */
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--broker-ip") && i + 1 < argc) {
            strncpy(gw.broker_ip, argv[++i], sizeof(gw.broker_ip) - 1);
        } else if (!strcmp(argv[i], "--broker-port") && i + 1 < argc) {
            gw.broker_port = (uint16_t)atoi(argv[++i]);
        } else if (!strcmp(argv[i], "--device") && i + 1 < argc) {
            if (gw.device_count < MAX_DEVICES) {
                if (parse_device_arg(argv[++i], &gw.devices[gw.device_count]) == 0) {
                    gw.device_count++;
                } else {
                    fprintf(stderr, "Invalid --device format: %s (use id@ip:port)\n",
                            argv[i]);
                }
            }
        }
    }

    if (gw.device_count == 0) {
        fprintf(stderr,
            "Usage: %s --broker-ip 127.0.0.1 --broker-port 1883 \\\n"
            "       --device A@127.0.0.1:5683 --device B@127.0.0.1:5684\n",
            argv[0]);
        DeleteCriticalSection(&gw.lock);
        DeleteCriticalSection(&gw.coap_lock);
        return 1;
    }

    /* 打开日志文件 */
    gw.log_fp = fopen("gateway.log", "w");

    gw_log(&gw, "==== CoAP-MQTT Gateway starting ====");
    gw_log(&gw, "Broker: %s:%u", gw.broker_ip, gw.broker_port);
    for (int i = 0; i < gw.device_count; i++) {
        gw_log(&gw, "Device %s: CoAP %s:%u",
               gw.devices[i].id, gw.devices[i].coap_ip, gw.devices[i].coap_port);
    }

    /* 初始化协议栈 */
    if (coap_init() != 0) {
        fprintf(stderr, "CoAP init failed\n");
        goto fail_init;
    }
    if (mqtt_init() != 0) {
        fprintf(stderr, "MQTT init failed\n");
        coap_cleanup();
        goto fail_init;
    }

    /* CoAP 客户端 socket (不绑定端口) */
    gw.coap_sock = coap_open_socket(0);
    if (gw.coap_sock == INVALID_SOCKET) {
        fprintf(stderr, "CoAP socket creation failed\n");
        goto fail_sock;
    }

    /* MQTT 连接 Broker */
    gw.mqtt_sock = mqtt_tcp_connect(gw.broker_ip, gw.broker_port);
    if (gw.mqtt_sock == INVALID_SOCKET) {
        fprintf(stderr, "MQTT connect to %s:%u failed\n", gw.broker_ip, gw.broker_port);
        goto fail_mqtt;
    }

    /* MQTT CONNECT 握手 */
    {
        mqtt_msg_t conn;
        mqtt_make_connect(&conn, "gateway", 60);
        mqtt_send_packet(gw.mqtt_sock, &conn);

        mqtt_msg_t connack;
        if (mqtt_recv_packet(gw.mqtt_sock, &connack, 5000) < 0 ||
            connack.type != MQTT_CONNACK) {
            fprintf(stderr, "MQTT CONNACK failed\n");
            goto fail_conn;
        }
    }
    gw_log(&gw, "Connected to MQTT broker %s:%u", gw.broker_ip, gw.broker_port);

    /* 订阅命令主题: gateway/+/command */
    {
        char topic_filter[128];
        snprintf(topic_filter, sizeof(topic_filter), "gateway/+/command");
        mqtt_msg_t sub;
        mqtt_make_subscribe(&sub, topic_filter, MQTT_QOS_1, gw.mqtt_pkt_id++);
        mqtt_send_packet(gw.mqtt_sock, &sub);

        mqtt_msg_t suback;
        if (mqtt_recv_packet(gw.mqtt_sock, &suback, 5000) < 0 ||
            suback.type != MQTT_SUBACK) {
            gw_log(&gw, "WARNING: SUBACK not received");
        } else {
            gw_log(&gw, "Subscribed to %s", topic_filter);
        }
    }

    /* 启动后台线程 */
    HANDLE th_collect = CreateThread(NULL, 0, collect_thread, &gw, 0, NULL);
    HANDLE th_mqtt    = CreateThread(NULL, 0, mqtt_recv_thread, &gw, 0, NULL);
    HANDLE th_ping    = CreateThread(NULL, 0, ping_thread, &gw, 0, NULL);

    /* 交互式命令循环 (本地调试用) */
    printf("\n=== CoAP-MQTT Gateway ===\n");
    printf("Auto-collect every %ds. Manual commands:\n", COLLECT_INTERVAL_MS / 1000);
    printf("  poll                - Poll all devices now\n");
    printf("  fwinfo <id>         - Get device firmware info\n");
    printf("  log <id>            - Get device log\n");
    printf("  get_fw <id> <ver>   - Get device firmware by version\n");
    printf("  upgrade <id> <file> - Upgrade device firmware\n");
    printf("  status              - Show gateway status\n");
    printf("  quit                - Exit gateway\n");
    printf("==========================\n\n");

    char cmd[256];
    while (gw.running) {
        printf("[gateway] command> ");
        fflush(stdout);
        if (!fgets(cmd, sizeof(cmd), stdin)) break;

        /* 去除换行符 */
        size_t len = strlen(cmd);
        while (len > 0 && (cmd[len-1] == '\n' || cmd[len-1] == '\r'))
            cmd[--len] = '\0';
        if (len == 0) continue;

        if (!strcmp(cmd, "quit") || !strcmp(cmd, "exit")) {
            break;
        } else if (!strcmp(cmd, "poll")) {
            gw_log(&gw, "Manual poll: %d device(s)", gw.device_count);
            for (int i = 0; i < gw.device_count; i++) {
                cmd_fwinfo(&gw, &gw.devices[i], NULL);
            }
        } else if (!strcmp(cmd, "status")) {
            gw_log(&gw, "Status: broker=%s:%u, devices=%d",
                   gw.broker_ip, gw.broker_port, gw.device_count);
        } else if (!strncmp(cmd, "fwinfo ", 7)) {
            device_entry_t *dev = find_device(&gw, cmd + 7);
            if (dev) cmd_fwinfo(&gw, dev, NULL);
            else printf("Device '%s' not found\n", cmd + 7);
        } else if (!strncmp(cmd, "log ", 4)) {
            device_entry_t *dev = find_device(&gw, cmd + 4);
            if (dev) cmd_log(&gw, dev);
            else printf("Device '%s' not found\n", cmd + 4);
        } else if (!strncmp(cmd, "get_fw ", 7)) {
            /* get_fw <id> <version> */
            char *p = cmd + 7;
            char *space = strchr(p, ' ');
            if (space) {
                *space = '\0';
                char *id = p;
                char *ver = space + 1;
                device_entry_t *dev = find_device(&gw, id);
                if (dev) {
                    char query[64];
                    snprintf(query, sizeof(query), "version=%.48s", ver);
                    cmd_fwinfo(&gw, dev, query);
                } else {
                    printf("Device '%s' not found\n", id);
                }
            } else {
                printf("Usage: get_fw <id> <version>\n");
            }
        } else if (!strncmp(cmd, "upgrade ", 8)) {
            /* upgrade <id> <firmware_file> */
            char *p = cmd + 8;
            char *space = strchr(p, ' ');
            if (space) {
                *space = '\0';
                char *id = p;
                char *fw_path = space + 1;
                device_entry_t *dev = find_device(&gw, id);
                if (dev) cmd_upgrade(&gw, dev, fw_path);
                else printf("Device '%s' not found\n", id);
            } else {
                printf("Usage: upgrade <id> <firmware_file>\n");
            }
        } else {
            printf("Unknown command: '%s'\n", cmd);
        }
    }

    /* 清理 */
    gw_log(&gw, "==== Gateway shutting down ====");
    gw.running = 0;
    WaitForSingleObject(th_collect, 2000);
    WaitForSingleObject(th_mqtt, 2000);
    WaitForSingleObject(th_ping, 2000);
    CloseHandle(th_collect);
    CloseHandle(th_mqtt);
    CloseHandle(th_ping);

    /* 发送 MQTT DISCONNECT */
    {
        mqtt_msg_t disc;
        memset(&disc, 0, sizeof(disc));
        disc.type = MQTT_DISCONNECT;
        mqtt_send_packet(gw.mqtt_sock, &disc);
    }

    mqtt_tcp_close(gw.mqtt_sock);
    coap_close_socket(gw.coap_sock);
    if (gw.log_fp) fclose(gw.log_fp);
    coap_cleanup();
    mqtt_cleanup();
    DeleteCriticalSection(&gw.lock);
    DeleteCriticalSection(&gw.coap_lock);
    return 0;

fail_conn:
    mqtt_tcp_close(gw.mqtt_sock);
fail_mqtt:
    coap_close_socket(gw.coap_sock);
fail_sock:
    coap_cleanup();
    mqtt_cleanup();
fail_init:
    if (gw.log_fp) fclose(gw.log_fp);
    DeleteCriticalSection(&gw.lock);
    DeleteCriticalSection(&gw.coap_lock);
    return 1;
}
