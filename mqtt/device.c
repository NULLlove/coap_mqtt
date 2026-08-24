/*
 * device.c - MQTT 设备 (资源注册/订阅模式)
 *
 * 一个可执行程序通过命令行参数实例化为一台设备:
 *
 *   device.exe --id A --broker-ip 127.0.0.1 --broker-port 1883 --version 1.0.0-A
 *   device.exe --id B --broker-ip 127.0.0.1 --broker-port 1883 --version 1.0.0-B
 *
 * 主题方案:
 *   registry/<id>/log      - 本设备的日志资源
 *   registry/<id>/firmware - 本设备的固件资源
 *   registry/<id>/fwinfo   - 本设备的固件信息资源
 *   registry/+/info        - 订阅所有资源更新通知
 *
 * 命令:
 *   find_all               - 查询服务器中所有可订阅资源 (订阅 registry/+/info)
 *   pub_rd <topic>         - 向服务器发布资源 (log/firmware/fwinfo)
 *   sub_rd <id> <topic>   - 订阅指定客户端的资源
 *   unsub_rd <id> <topic> - 取消订阅指定客户端的资源
 *   del_rd <topic>         - 删除本客户端的指定资源
 *   status                 - 显示设备状态
 *   help                   - 显示帮助
 *   quit                   - 退出
 *
 * 启动时自动发布 log 和 fwinfo 资源。
 *
 * 编译: gcc -Wall -Wextra -O2 -o device.exe mqtt.c device.c -lws2_32
 */
#include "mqtt.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>
#include <time.h>
#include <errno.h>

#define FW_FILLER_LEN 260   /* 固件镜像中的填充数据长度 */
#define MAX_PAYLOAD     4096 /* MQTT消息最大负载 */

typedef struct {
    char        id[16];
    char        broker_ip[64];
    uint16_t    broker_port;

    char        version[32];
    char        original_version[32];
    char        last_peer_fw_ver[32];  /* 上次收到的对端固件版本, 用于判断是否需要升级 */

    char        fw_path[64];
    char        fw_orig_path[64];
    char        log_path[64];
    char        proto_log_path[64];

    CRITICAL_SECTION lock;
    CRITICAL_SECTION send_lock;   /* 保护多线程并发 MQTT 发送 */
    FILE       *log_fp;
    FILE       *proto_log_fp;
    SOCKET      sock;
    volatile int running;
    volatile int auto_pub_log;    /* 连接建立后启用: dev_log 自动发布新日志 */
    volatile int suppress_auto_pub; /* recv_thread 处理对端消息时置 1, 防止日志反馈循环 */
    uint16_t    next_packet_id;

    /* Pending response tracking (for SUBACK etc.) */
    CRITICAL_SECTION resp_lock;
    HANDLE      resp_event;
    uint16_t    pending_packet_id;  /* 0 means no pending */
    uint8_t     pending_msg_type;
    int         pending_received;
    mqtt_msg_t  pending_resp;
} device_t;

/* ==================== 日志函数 ==================== */

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
    char time_str[64];
    strftime(time_str, sizeof(time_str), "%Y-%m-%d %H:%M:%S", tm);

    printf("[%s] [%s] %.*s\n", time_str, d->id, n, line);
    fflush(stdout);

    EnterCriticalSection(&d->lock);
    if (d->log_fp) {
        fprintf(d->log_fp, "[%s] [%s] %.*s\n", time_str, d->id, n, line);
        fflush(d->log_fp);
    }
    LeaveCriticalSection(&d->lock);

    /* 自动发布新日志条目 (retain=0, QoS 0, 增量推送给订阅者) */
    if (d->auto_pub_log && !d->suppress_auto_pub && d->sock != INVALID_SOCKET) {
        char formatted[640];
        int flen = snprintf(formatted, sizeof(formatted),
                            "[%s] [%s] %.*s\n", time_str, d->id, n, line);
        if (flen > 0) {
            char topic[MQTT_TOPIC_MAX];
            snprintf(topic, sizeof(topic), "registry/%s/log", d->id);
            mqtt_msg_t msg;
            mqtt_make_publish(&msg, topic, (uint8_t *)formatted, (size_t)flen,
                              MQTT_QOS_0, 0, 0);
            EnterCriticalSection(&d->send_lock);
            mqtt_send_packet(d->sock, &msg);
            LeaveCriticalSection(&d->send_lock);
        }
    }
}

static void proto_log(device_t *d, const char *direction, const mqtt_msg_t *msg) {
    if (!d->proto_log_fp) return;

    time_t now = time(NULL);
    struct tm *tm = localtime(&now);
    char time_str[64];
    strftime(time_str, sizeof(time_str), "%Y-%m-%d %H:%M:%S", tm);

    EnterCriticalSection(&d->lock);
    fprintf(d->proto_log_fp, "=== [%s] %s ===\n", time_str, direction);
    fprintf(d->proto_log_fp, "  Type: %d, Topic: %s, QoS: %d\n",
            msg->type, msg->topic, msg->qos);

    if (msg->payload_len > 0) {
        fprintf(d->proto_log_fp, "  Payload (%zu bytes): ", msg->payload_len);
        size_t show = msg->payload_len < 256 ? msg->payload_len : 256;
        fwrite(msg->payload, 1, show, d->proto_log_fp);
        if (msg->payload_len > 256) fprintf(d->proto_log_fp, "...(truncated)");
        fprintf(d->proto_log_fp, "\n");
    }

    fprintf(d->proto_log_fp, "\n");
    fflush(d->proto_log_fp);
    LeaveCriticalSection(&d->lock);
}

/* ==================== 固件管理 ==================== */

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

/* ==================== 资源发布 ==================== */

/* 发布资源到服务器 */
static void publish_resource(device_t *d, const char *topic_type,
                             const uint8_t *payload, size_t payload_len) {
    char topic[MQTT_TOPIC_MAX];
    snprintf(topic, sizeof(topic), "registry/%s/%s", d->id, topic_type);

    mqtt_msg_t msg;
    mqtt_make_publish(&msg, topic, payload, payload_len, MQTT_QOS_1, 1, d->next_packet_id++);
    proto_log(d, "SEND (PUBLISH)", &msg);
    dev_log(d, "pub_rd: -> PUBLISH to '%s' (%zu bytes)", topic, payload_len);
    EnterCriticalSection(&d->send_lock);
    mqtt_send_packet(d->sock, &msg);
    LeaveCriticalSection(&d->send_lock);
}

/* 发布日志资源 */
static void publish_log(device_t *d) {
    char payload[MAX_PAYLOAD];
    size_t payload_len = 0;

    FILE *lf = fopen(d->log_path, "rb");
    if (lf) {
        fseek(lf, 0, SEEK_END);
        long fsize = ftell(lf);
        fseek(lf, 0, SEEK_SET);
        if (fsize > 0) {
            payload_len = (size_t)fsize;
            if (payload_len > sizeof(payload)) payload_len = sizeof(payload);
            payload_len = fread(payload, 1, payload_len, lf);
        }
        fclose(lf);
    }

    publish_resource(d, "log", (uint8_t *)payload, payload_len);
}

/* 发布固件信息资源 */
static void publish_fwinfo(device_t *d) {
    char ver_buf[32] = {0};
    size_t fw_size = read_fw_info(d->fw_path, ver_buf, sizeof(ver_buf));

    char payload[256];
    size_t payload_len = snprintf(payload, sizeof(payload),
                                   "version=%s,size=%zu",
                                   ver_buf[0] ? ver_buf : d->version, fw_size);

    publish_resource(d, "fwinfo", (uint8_t *)payload, payload_len);
}

/* 发布固件资源 */
static void publish_firmware(device_t *d) {
    char payload[MAX_PAYLOAD];
    size_t payload_len = 0;

    FILE *fw = fopen(d->fw_path, "rb");
    if (fw) {
        fseek(fw, 0, SEEK_END);
        long fsize = ftell(fw);
        fseek(fw, 0, SEEK_SET);
        if (fsize > 0 && (size_t)fsize <= sizeof(payload)) {
            payload_len = fread(payload, 1, (size_t)fsize, fw);
        }
        fclose(fw);
    }

    publish_resource(d, "firmware", (uint8_t *)payload, payload_len);
}

/* ==================== 资源订阅 ==================== */

/* 设置 pending 响应等待 */
static void pending_set(device_t *d, uint16_t packet_id, uint8_t msg_type) {
    EnterCriticalSection(&d->resp_lock);
    d->pending_packet_id = packet_id;
    d->pending_msg_type = msg_type;
    d->pending_received = 0;
    ResetEvent(d->resp_event);
    LeaveCriticalSection(&d->resp_lock);
}

/* 清除 pending 状态 */
static void pending_clear(device_t *d) {
    EnterCriticalSection(&d->resp_lock);
    d->pending_packet_id = 0;
    d->pending_received = 0;
    LeaveCriticalSection(&d->resp_lock);
}

/* 等待 pending 响应。返回 0=成功, -1=超时 */
static int pending_wait(device_t *d, mqtt_msg_t *out_msg, int timeout_ms) {
    DWORD wr = WaitForSingleObject(d->resp_event, timeout_ms);
    if (wr != WAIT_OBJECT_0) {
        pending_clear(d);
        return -1;
    }
    EnterCriticalSection(&d->resp_lock);
    int rc = d->pending_received ? 0 : -1;
    if (rc == 0 && out_msg) {
        memcpy(out_msg, &d->pending_resp, sizeof(mqtt_msg_t));
    }
    LeaveCriticalSection(&d->resp_lock);
    pending_clear(d);
    return rc;
}

/* recv_thread 中调用: 检查收到的消息是否是 pending 响应 */
static int pending_check_and_capture(device_t *d, const mqtt_msg_t *msg) {
    EnterCriticalSection(&d->resp_lock);
    if (d->pending_packet_id != 0 &&
        msg->type == d->pending_msg_type &&
        msg->packet_id == d->pending_packet_id) {
        memcpy(&d->pending_resp, msg, sizeof(mqtt_msg_t));
        d->pending_received = 1;
        SetEvent(d->resp_event);
        LeaveCriticalSection(&d->resp_lock);
        return 1;  /* 已捕获, 不再继续普通处理 */
    }
    LeaveCriticalSection(&d->resp_lock);
    return 0;
}

/* 订阅指定客户端的资源 */
static void subscribe_resource(device_t *d, const char *peer_id, const char *topic_type) {
    char topic[MQTT_TOPIC_MAX];
    snprintf(topic, sizeof(topic), "registry/%s/%s", peer_id, topic_type);

    uint16_t pkt_id = d->next_packet_id++;
    mqtt_msg_t sub;
    mqtt_make_subscribe(&sub, topic, MQTT_QOS_1, pkt_id);
    proto_log(d, "SEND (SUBSCRIBE)", &sub);
    dev_log(d, "sub_rd: -> SUBSCRIBE to '%s'", topic);

    /* 在发送前设置 pending, 避免 recv_thread 抢先收到 SUBACK 时丢失 */
    pending_set(d, pkt_id, MQTT_SUBACK);
    if (mqtt_send_packet(d->sock, &sub) < 0) {
        pending_clear(d);
        dev_log(d, "sub_rd: send failed for '%s'", topic);
        return;
    }

    /* 等待 SUBACK (通过 pending 机制) */
    mqtt_msg_t suback;
    if (pending_wait(d, &suback, 5000) == 0) {
        dev_log(d, "sub_rd: SUBACK received for '%s'", topic);
    } else {
        dev_log(d, "sub_rd: SUBACK timeout for '%s'", topic);
    }
}

/* 取消订阅指定主题 */
static void unsubscribe_resource(device_t *d, const char *topic_filter) {
    uint16_t pkt_id = d->next_packet_id++;
    mqtt_msg_t unsub;
    mqtt_make_unsubscribe(&unsub, topic_filter, pkt_id);
    proto_log(d, "SEND (UNSUBSCRIBE)", &unsub);
    dev_log(d, "unsub_rd: -> UNSUBSCRIBE from '%s'", topic_filter);

    pending_set(d, pkt_id, MQTT_UNSUBACK);
    EnterCriticalSection(&d->send_lock);
    int rc = mqtt_send_packet(d->sock, &unsub);
    LeaveCriticalSection(&d->send_lock);
    if (rc < 0) {
        pending_clear(d);
        dev_log(d, "unsub_rd: send failed for '%s'", topic_filter);
        return;
    }

    mqtt_msg_t unsuback;
    if (pending_wait(d, &unsuback, 5000) == 0) {
        dev_log(d, "unsub_rd: UNSUBACK received for '%s'", topic_filter);
    } else {
        dev_log(d, "unsub_rd: UNSUBACK timeout for '%s'", topic_filter);
    }
}

/* 查询所有可订阅资源 (订阅 registry/+/info) */
static void find_all_resources(device_t *d) {
    uint16_t pkt_id = d->next_packet_id++;
    mqtt_msg_t sub;
    mqtt_make_subscribe(&sub, "registry/+/info", MQTT_QOS_1, pkt_id);
    proto_log(d, "SEND (SUBSCRIBE)", &sub);
    dev_log(d, "find_all: -> SUBSCRIBE to 'registry/+/info'");

    /* 使用 pending 机制等待 SUBACK, 避免与 recv_thread 竞争 */
    pending_set(d, pkt_id, MQTT_SUBACK);
    if (mqtt_send_packet(d->sock, &sub) < 0) {
        pending_clear(d);
        dev_log(d, "find_all: send failed");
        return;
    }

    /* 等待 SUBACK */
    mqtt_msg_t suback;
    if (pending_wait(d, &suback, 5000) == 0) {
        dev_log(d, "find_all: SUBACK received, waiting for resource info...");
    } else {
        dev_log(d, "find_all: SUBACK timeout");
    }

    /* 等待 2 秒让 recv_thread 接收并打印资源信息 (recv_thread 会自动处理) */
    printf("[%s]   === All Registered Resources (check output above) ===\n", d->id);
    Sleep(2000);
    printf("[%s]   === find_all done ===\n", d->id);
}

/* 删除本客户端的指定资源 */
static void delete_resource(device_t *d, const char *topic_type) {
    char topic[MQTT_TOPIC_MAX];
    snprintf(topic, sizeof(topic), "registry/%s/%s", d->id, topic_type);

    /* 如果删除的是 log 资源, 关闭日志自动发布, 防止后续 dev_log 重新注册 */
    if (strcmp(topic_type, "log") == 0) {
        d->auto_pub_log = 0;
        dev_log(d, "del_rd: auto_pub_log disabled (log resource being deleted)");
    }

    /* 发送删除消息 (retain=1, 清除 broker 上的保留消息) */
    const char *delete_payload = "__DELETE__";
    mqtt_msg_t msg;
    mqtt_make_publish(&msg, topic, (const uint8_t *)delete_payload,
                      strlen(delete_payload), MQTT_QOS_1, 1, d->next_packet_id++);
    proto_log(d, "SEND (PUBLISH delete)", &msg);
    dev_log(d, "del_rd: -> DELETE resource '%s'", topic);
    mqtt_send_packet(d->sock, &msg);
}

/* 从主题中解析 peer_id 和 topic_type
 * 主题格式: registry/<peer_id>/<topic_type>
 * 返回 1 成功, 0 失败 */
static int parse_peer_topic(const char *topic, char *peer_id, size_t pid_size,
                            char *topic_type, size_t tt_size) {
    if (strncmp(topic, "registry/", 9) != 0) return 0;
    const char *p = topic + 9;
    const char *slash = strchr(p, '/');
    if (!slash) return 0;
    size_t pid_len = (size_t)(slash - p);
    if (pid_len >= pid_size) pid_len = pid_size - 1;
    memcpy(peer_id, p, pid_len);
    peer_id[pid_len] = '\0';

    const char *tt = slash + 1;
    if (strcmp(tt, "info") == 0) return 0;
    strncpy(topic_type, tt, tt_size - 1);
    topic_type[tt_size - 1] = '\0';
    return 1;
}

/* 将订阅到的资源保存到本地文件 */
static void save_subscribed_resource(device_t *d, const char *peer_id,
                                     const char *topic_type,
                                     const uint8_t *payload, size_t payload_len) {
    char filepath[256];

    if (strcmp(topic_type, "log") == 0) {
        /* 日志: device_<id>_log/peer_log_<peer_id>.log */
        snprintf(filepath, sizeof(filepath), "device_%s_log/peer_log_%s.log", d->id, peer_id);
        FILE *fp = fopen(filepath, "wb");
        if (fp) {
            fwrite(payload, 1, payload_len, fp);
            fclose(fp);
            dev_log(d, "Saved peer %s log to '%s' (%zu bytes)", peer_id, filepath, payload_len);
        }
    } else if (strcmp(topic_type, "firmware") == 0) {
        /* 固件: device_<id>_bin/peer_firmware_<peer_id>.bin */
        snprintf(filepath, sizeof(filepath), "device_%s_bin/peer_firmware_%s.bin", d->id, peer_id);
        FILE *fp = fopen(filepath, "wb");
        if (fp) {
            fwrite(payload, 1, payload_len, fp);
            fclose(fp);
            dev_log(d, "Saved peer %s firmware to '%s' (%zu bytes)", peer_id, filepath, payload_len);
        }
    } else if (strcmp(topic_type, "fwinfo") == 0) {
        /* 固件信息: device_<id>_log/peer_fwinfo_<peer_id>.txt */
        snprintf(filepath, sizeof(filepath), "device_%s_log/peer_fwinfo_%s.txt", d->id, peer_id);
        FILE *fp = fopen(filepath, "wb");
        if (fp) {
            fwrite(payload, 1, payload_len, fp);
            fclose(fp);
            dev_log(d, "Saved peer %s fwinfo to '%s' (%zu bytes)", peer_id, filepath, payload_len);
        }
    }
}

/* 检查对端固件版本, 若更新则自动升级本设备固件 */
static int check_and_upgrade_firmware(device_t *d, const char *peer_id,
                                       const char *filepath) {
    /* 读取对端固件版本和大小 */
    char peer_ver[32] = {0};
    size_t peer_fw_size = read_fw_info(filepath, peer_ver, sizeof(peer_ver));
    if (peer_ver[0] == '\0' || peer_fw_size == 0) return 0;

    /* 与上次收到的对端固件版本比较, 版本没变则不升级 */
    if (d->last_peer_fw_ver[0] != '\0' && strcmp(peer_ver, d->last_peer_fw_ver) == 0) {
        dev_log(d, "Firmware from %s: version %s same as last received, no upgrade needed",
                peer_id, peer_ver);
        return 0;
    }

    dev_log(d, "New firmware from %s: version=%s (last=%s, current=%s), upgrading...",
            peer_id, peer_ver,
            d->last_peer_fw_ver[0] ? d->last_peer_fw_ver : "(none)", d->version);

    /* 记录本次收到的对端固件版本 */
    strncpy(d->last_peer_fw_ver, peer_ver, sizeof(d->last_peer_fw_ver) - 1);
    d->last_peer_fw_ver[sizeof(d->last_peer_fw_ver) - 1] = '\0';

    /* 复制对端固件到本设备固件文件 (Windows: 路径中的 / 需转为 \) */
    char fw_dst[256], fw_src[256];
    strncpy(fw_dst, d->fw_path, sizeof(fw_dst) - 1);
    fw_dst[sizeof(fw_dst) - 1] = '\0';
    strncpy(fw_src, filepath, sizeof(fw_src) - 1);
    fw_src[sizeof(fw_src) - 1] = '\0';
    for (int i = 0; fw_dst[i]; i++) { if (fw_dst[i] == '/') fw_dst[i] = '\\'; }
    for (int i = 0; fw_src[i]; i++) { if (fw_src[i] == '/') fw_src[i] = '\\'; }

    char cmd[512];
    snprintf(cmd, sizeof(cmd), "cmd /c copy /y \"%s\" \"%s\" >nul 2>&1", fw_src, fw_dst);
    int rc = system(cmd);

    if (rc == 0) {
        /* 版本号累加: 1.0.0-B → 1.0.1-B → ... → 1.0.9-B → 1.1.0-B
         * 保留本地后缀, 只递增 patch, 进位规则: patch>9 → patch=0,minor++; minor>9 → minor=0,major++ */
        int major = 1, minor = 0, patch = 0;
        char suffix[16] = {0};
        if (sscanf(d->version, "%d.%d.%d-%15s", &major, &minor, &patch, suffix) >= 3) {
            patch++;
            if (patch > 9) { patch = 0; minor++; }
            if (minor > 9) { minor = 0; major++; }
            snprintf(d->version, sizeof(d->version), "%d.%d.%d-%s", major, minor, patch, suffix);
        } else {
            char newver[32];
            snprintf(newver, sizeof(newver), "%s-v2", d->version);
            strncpy(d->version, newver, sizeof(d->version) - 1);
            d->version[sizeof(d->version) - 1] = '\0';
        }

        /* 把新版本号写入固件文件首行 (覆盖原首行, 保留 body) */
        FILE *fw = fopen(d->fw_path, "r+b");
        if (fw) {
            char line[64];
            if (fgets(line, sizeof(line), fw)) {
                long body_offset = ftell(fw);
                fseek(fw, 0, SEEK_END);
                long total = ftell(fw);
                long body_len = total - body_offset;
                uint8_t *body = body_len > 0 ? (uint8_t *)malloc((size_t)body_len) : NULL;
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

        dev_log(d, "*** Firmware upgraded: %s → %s (from %s) ***",
                peer_ver, d->version, peer_id);
        return 1;
    } else {
        dev_log(d, "Firmware upgrade failed (copy error %d)", rc);
        return 0;
    }
}

/* ==================== 接收线程 ==================== */
static DWORD WINAPI recv_thread(LPVOID arg) {
    device_t *d = (device_t *)arg;

    while (d->running) {
        mqtt_msg_t msg;
        int rc = mqtt_recv_packet(d->sock, &msg, 1000);
        if (rc == -2) continue;  /* 超时, 正常 - 无数据可读 */
        if (rc < 0) {
            if (d->running) {
                dev_log(d, "recv_thread: connection lost");
            }
            break;
        }

        /* 先检查是否是 pending 的响应 (如 SUBACK), 若是则捕获并跳过普通处理 */
        if (pending_check_and_capture(d, &msg)) {
            char type_name[16];
            switch (msg.type) {
                case MQTT_SUBACK:   strcpy(type_name, "SUBACK"); break;
                case MQTT_UNSUBACK: strcpy(type_name, "UNSUBACK"); break;
                case MQTT_PUBACK:   strcpy(type_name, "PUBACK"); break;
                case MQTT_CONNACK:  strcpy(type_name, "CONNACK"); break;
                case MQTT_PINGRESP: strcpy(type_name, "PINGRESP"); break;
                default:            snprintf(type_name, sizeof(type_name), "TYPE%u", msg.type);
            }
            dev_log(d, "recv_thread: captured pending %s (pkt_id=%u)", type_name, msg.packet_id);
            continue;
        }

        if (msg.type == MQTT_PUBLISH) {
            proto_log(d, "RECV (PUBLISH)", &msg);

            /* 抑制处理对端消息时产生的日志自动发布, 防止互相订阅时的反馈循环 */
            d->suppress_auto_pub = 1;

            /* 显示接收到的资源内容 */
            printf("\n[%s]   ===== Received Resource =====\n", d->id);
            printf("[%s]   Topic: %s\n", d->id, msg.topic);
            printf("[%s]   Size: %zu bytes\n", d->id, msg.payload_len);
            if (msg.payload_len > 0 && msg.payload_len <= 256) {
                printf("[%s]   Content: ", d->id);
                fwrite(msg.payload, 1, msg.payload_len, stdout);
                printf("\n");
            } else if (msg.payload_len > 256) {
                printf("[%s]   Content: (binary data, %zu bytes)\n", d->id, msg.payload_len);
            }
            printf("[%s]   ==============================\n", d->id);
            fflush(stdout);

            /* 如果是资源主题 (registry/<id>/<type>), 保存到本地文件 */
            char peer_id[16], topic_type[32];
            if (parse_peer_topic(msg.topic, peer_id, sizeof(peer_id),
                                 topic_type, sizeof(topic_type))) {
                /* 日志资源: retain=1 全量覆盖, retain=0 增量追加 */
                if (strcmp(topic_type, "log") == 0) {
                    char filepath[256];
                    snprintf(filepath, sizeof(filepath), "device_%s_log/peer_log_%s.log", d->id, peer_id);
                    const char *mode = msg.retain ? "wb" : "ab";
                    FILE *fp = fopen(filepath, mode);
                    if (fp) {
                        fwrite(msg.payload, 1, msg.payload_len, fp);
                        fclose(fp);
                        dev_log(d, "Saved peer %s log to '%s' (%zu bytes, %s)",
                                peer_id, filepath, msg.payload_len,
                                msg.retain ? "overwrite" : "append");
                    }
                } else {
                    save_subscribed_resource(d, peer_id, topic_type,
                                             msg.payload, msg.payload_len);
                    /* 固件文件保存后, 检查并自动升级 */
                    if (strcmp(topic_type, "firmware") == 0) {
                        char fw_filepath[256];
                        snprintf(fw_filepath, sizeof(fw_filepath),
                                 "device_%s_bin/peer_firmware_%s.bin", d->id, peer_id);
                        if (check_and_upgrade_firmware(d, peer_id, fw_filepath)) {
                            publish_fwinfo(d);
                        }
                    }
                }
            }

            /* QoS1: 发送 PUBACK */
            if (msg.qos == MQTT_QOS_1) {
                mqtt_msg_t ack;
                mqtt_make_puback(&ack, msg.packet_id);
                mqtt_send_packet(d->sock, &ack);
            }

            d->suppress_auto_pub = 0;  /* 恢复日志自动发布 */
        } else if (msg.type == MQTT_PUBACK) {
            /* 抑制 auto_pub: PUBACK 是协议级 ACK, 不应触发日志自动发布重新注册资源 */
            d->suppress_auto_pub = 1;
            dev_log(d, "recv_thread: received PUBACK for packet_id=%u", msg.packet_id);
            d->suppress_auto_pub = 0;
        } else if (msg.type == MQTT_SUBACK) {
            dev_log(d, "recv_thread: received SUBACK (pkt_id=%u, no pending)", msg.packet_id);
        } else if (msg.type == MQTT_UNSUBACK) {
            dev_log(d, "recv_thread: received UNSUBACK (pkt_id=%u, no pending)", msg.packet_id);
        } else if (msg.type == MQTT_PINGRESP) {
            /* PINGRESP 静默处理 */
        } else {
            dev_log(d, "recv_thread: received msg type=%u pkt_id=%u", msg.type, msg.packet_id);
        }
    }

    return 0;
}

/* ==================== PINGREQ 心跳线程 ==================== */
static DWORD WINAPI ping_thread(LPVOID arg) {
    device_t *d = (device_t *)arg;
    while (d->running) {
        Sleep(5000);
        if (d->running) {
            mqtt_msg_t ping;
            mqtt_make_pingreq(&ping);
            mqtt_send_packet(d->sock, &ping);
        }
    }
    return 0;
}

/* ==================== 主函数 ==================== */
int main(int argc, char **argv) {
    device_t d;
    memset(&d, 0, sizeof(d));
    InitializeCriticalSection(&d.lock);
    InitializeCriticalSection(&d.send_lock);
    InitializeCriticalSection(&d.resp_lock);
    d.resp_event = CreateEvent(NULL, TRUE, FALSE, NULL);  /* manual-reset event */
    d.running = 1;
    d.next_packet_id = (uint16_t)(time(NULL) & 0xffff);
    strncpy(d.version, "1.0.0", sizeof(d.version) - 1);
    strncpy(d.broker_ip, "127.0.0.1", sizeof(d.broker_ip) - 1);
    d.broker_port = MQTT_DEFAULT_PORT;

    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--id") && i + 1 < argc)
            strncpy(d.id, argv[++i], sizeof(d.id) - 1);
        else if (!strcmp(argv[i], "--broker-ip") && i + 1 < argc)
            strncpy(d.broker_ip, argv[++i], sizeof(d.broker_ip) - 1);
        else if (!strcmp(argv[i], "--broker-port") && i + 1 < argc)
            d.broker_port = (uint16_t)atoi(argv[++i]);
        else if (!strcmp(argv[i], "--version") && i + 1 < argc)
            strncpy(d.version, argv[++i], sizeof(d.version) - 1);
    }

    strncpy(d.original_version, d.version, sizeof(d.original_version) - 1);
    d.original_version[sizeof(d.original_version) - 1] = '\0';

    /* 创建目录 */
    {
        char dir_cmd[512];
        snprintf(dir_cmd, sizeof(dir_cmd),
                 "cmd /c \"if not exist device_%s_log mkdir device_%s_log & "
                 "if not exist device_%s_bin mkdir device_%s_bin\"",
                 d.id, d.id, d.id, d.id);
        system(dir_cmd);
    }

    /* 构造文件路径 */
    snprintf(d.fw_path, sizeof(d.fw_path), "device_%s_bin/firmware_%s.bin", d.id, d.id);
    snprintf(d.fw_orig_path, sizeof(d.fw_orig_path), "device_%s_bin/firmware_%s_orig.bin", d.id, d.id);
    snprintf(d.log_path, sizeof(d.log_path), "device_%s_log/device_%s.log", d.id, d.id);
    snprintf(d.proto_log_path, sizeof(d.proto_log_path), "device_%s_log/proto_%s.log", d.id, d.id);

    if (d.id[0] == 0) {
        fprintf(stderr, "Usage: %s --id A --broker-ip 127.0.0.1 --broker-port 1883 [--version 1.0.0-A]\n", argv[0]);
        CloseHandle(d.resp_event);
        DeleteCriticalSection(&d.resp_lock);
        DeleteCriticalSection(&d.send_lock);
        DeleteCriticalSection(&d.lock);
        return 1;
    }

    if (mqtt_init() != 0) {
        fprintf(stderr, "MQTT init failed\n");
        CloseHandle(d.resp_event);
        DeleteCriticalSection(&d.resp_lock);
        DeleteCriticalSection(&d.send_lock);
        DeleteCriticalSection(&d.lock);
        return 1;
    }

    /* 连接到 Broker */
    d.sock = mqtt_tcp_connect(d.broker_ip, d.broker_port);
    if (d.sock == INVALID_SOCKET) {
        fprintf(stderr, "Failed to connect to broker %s:%u\n", d.broker_ip, d.broker_port);
        mqtt_cleanup();
        CloseHandle(d.resp_event);
        DeleteCriticalSection(&d.resp_lock);
        DeleteCriticalSection(&d.send_lock);
        DeleteCriticalSection(&d.lock);
        return 1;
    }

    /* 发送 CONNECT (含遗嘱消息: 断开时通知其他客户端) */
    mqtt_msg_t conn;
    char will_topic_buf[MQTT_TOPIC_MAX];
    snprintf(will_topic_buf, sizeof(will_topic_buf), "registry/%s/status", d.id);
    mqtt_make_connect(&conn, d.id, 60, will_topic_buf, "offline");
    mqtt_send_packet(d.sock, &conn);

    /* 等待 CONNACK - 此时 recv_thread 尚未启动, 可直接 recv */
    mqtt_msg_t connack;
    if (mqtt_recv_packet(d.sock, &connack, 5000) < 0 || connack.type != MQTT_CONNACK) {
        fprintf(stderr, "Failed to receive CONNACK\n");
        mqtt_tcp_close(d.sock);
        mqtt_cleanup();
        CloseHandle(d.resp_event);
        DeleteCriticalSection(&d.resp_lock);
        DeleteCriticalSection(&d.send_lock);
        DeleteCriticalSection(&d.lock);
        return 1;
    }

    dev_log(&d, "Connected to broker %s:%u", d.broker_ip, d.broker_port);

    /* 创建初始固件文件 */
    {
        const char *paths[] = { d.fw_path, d.fw_orig_path };
        for (int p = 0; p < 2; p++) {
            FILE *fw = fopen(paths[p], "wb");
            if (fw) {
                fprintf(fw, "%s\n", d.original_version);
                for (int i = 0; i < FW_FILLER_LEN; i++)
                    fputc(i & 0xff, fw);
                fclose(fw);
            }
        }
    }

    /* 打开日志文件 */
    d.log_fp = fopen(d.log_path, "w");
    d.proto_log_fp = fopen(d.proto_log_path, "w");
    if (d.proto_log_fp) {
        fprintf(d.proto_log_fp, "=== MQTT Protocol Log for Device %s ===\n\n", d.id);
        fflush(d.proto_log_fp);
    }

    dev_log(&d, "==== Device %s started: broker=%s:%u  version=%s ====",
            d.id, d.broker_ip, d.broker_port, d.version);

    /* 启动接收线程 */
    HANDLE th = CreateThread(NULL, 0, recv_thread, &d, 0, NULL);

    /* 启动心跳线程 */
    HANDLE ping_th = CreateThread(NULL, 0, ping_thread, &d, 0, NULL);

    /* 启用日志自动发布: dev_log 产生的每条新日志将实时推送给订阅者 */
    d.auto_pub_log = 1;

    /* 启动时自动发布 log, fwinfo 和 status */
    Sleep(500);
    dev_log(&d, "Auto-publishing log, fwinfo and status resources...");
    publish_log(&d);
    Sleep(200);
    publish_fwinfo(&d);
    /* 发布在线状态 (retain=1, 遗嘱消息会在断开时覆盖为 "offline") */
    {
        char status_topic[MQTT_TOPIC_MAX];
        snprintf(status_topic, sizeof(status_topic), "registry/%s/status", d.id);
        const char *status_msg = "online";
        mqtt_msg_t status_pub;
        mqtt_make_publish(&status_pub, status_topic, (uint8_t *)status_msg,
                          strlen(status_msg), MQTT_QOS_1, 1, d.next_packet_id++);
        EnterCriticalSection(&d.send_lock);
        mqtt_send_packet(d.sock, &status_pub);
        LeaveCriticalSection(&d.send_lock);
    }

    /* 交互式命令循环 */
    printf("\n=== MQTT Device %s Interactive Mode ===\n", d.id);
    printf("Available commands:\n");
    printf("  find_all               - Query all subscribable resources on server\n");
    printf("  pub_rd <topic>         - Publish resource (log/firmware/fwinfo)\n");
    printf("  sub_rd <id> <topic>    - Subscribe to peer's resource\n");
    printf("  unsub_rd <id> <topic>  - Unsubscribe from peer's resource\n");
    printf("  del_rd <topic>         - Delete own resource from server\n");
    printf("  status                 - Show device status\n");
    printf("  help                   - Show this help\n");
    printf("  quit                   - Exit device\n");
    printf("==========================================\n\n");

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
            dev_log(&d, "Command: quit");
            break;
        } else if (!strcmp(cmd, "help")) {
            printf("  find_all               - Query all subscribable resources on server\n");
            printf("  pub_rd <topic>         - Publish resource (log/firmware/fwinfo)\n");
            printf("  sub_rd <id> <topic>    - Subscribe to peer's resource\n");
            printf("  unsub_rd <id> <topic>  - Unsubscribe from peer's resource\n");
            printf("  del_rd <topic>         - Delete own resource from server\n");
            printf("  status                 - Show device status\n");
            printf("  help                   - Show this help\n");
            printf("  quit                   - Exit device\n");
        } else if (!strcmp(cmd, "status")) {
            dev_log(&d, "Status: id=%s, broker=%s:%u, version=%s",
                    d.id, d.broker_ip, d.broker_port, d.version);
        } else if (!strcmp(cmd, "find_all")) {
            dev_log(&d, "Command: find_all");
            find_all_resources(&d);
        } else if (!strncmp(cmd, "pub_rd ", 7)) {
            const char *topic = cmd + 7;
            /* 去掉尾部空格 */
            while (*topic == ' ') topic++;
            dev_log(&d, "Command: pub_rd %s", topic);
            if (strcmp(topic, "log") == 0) {
                d.auto_pub_log = 1;  /* 重新启用日志自动发布 */
                publish_log(&d);
            } else if (strcmp(topic, "fwinfo") == 0) {
                publish_fwinfo(&d);
            } else if (strcmp(topic, "firmware") == 0) {
                publish_firmware(&d);
            } else {
                printf("[%s]   Unknown topic: '%s'. Use: log, firmware, fwinfo\n", d.id, topic);
            }
        } else if (!strncmp(cmd, "sub_rd ", 7)) {
            char peer_id[16] = {0};
            char topic_type[32] = {0};
            if (sscanf(cmd + 7, "%15s %31s", peer_id, topic_type) >= 2) {
                dev_log(&d, "Command: sub_rd %s %s", peer_id, topic_type);
                subscribe_resource(&d, peer_id, topic_type);
            } else {
                printf("[%s]   Usage: sub_rd <id> <topic> (e.g., sub_rd B log)\n", d.id);
            }
        } else if (!strncmp(cmd, "unsub_rd ", 9)) {
            char peer_id[16] = {0};
            char topic_type[32] = {0};
            if (sscanf(cmd + 9, "%15s %31s", peer_id, topic_type) >= 2) {
                char full_topic[MQTT_TOPIC_MAX];
                snprintf(full_topic, sizeof(full_topic), "registry/%s/%s", peer_id, topic_type);
                dev_log(&d, "Command: unsub_rd %s %s -> %s", peer_id, topic_type, full_topic);
                unsubscribe_resource(&d, full_topic);
            } else {
                printf("[%s]   Usage: unsub_rd <id> <topic> (e.g., unsub_rd B log)\n", d.id);
            }
        } else if (!strncmp(cmd, "del_rd ", 7)) {
            const char *topic = cmd + 7;
            while (*topic == ' ') topic++;
            dev_log(&d, "Command: del_rd %s", topic);
            if (strcmp(topic, "log") == 0 ||
                strcmp(topic, "firmware") == 0 ||
                strcmp(topic, "fwinfo") == 0) {
                delete_resource(&d, topic);
            } else {
                printf("[%s]   Unknown topic: '%s'. Use: log, firmware, fwinfo\n", d.id, topic);
            }
        } else {
            printf("Unknown command: '%s'. Type 'help' for available commands.\n", cmd);
        }
    }

    dev_log(&d, "==== Device %s finished: version=%s ====", d.id, d.version);

    d.auto_pub_log = 0;
    d.running = 0;
    WaitForSingleObject(th, 2000);
    WaitForSingleObject(ping_th, 2000);
    CloseHandle(th);
    CloseHandle(ping_th);

    mqtt_tcp_close(d.sock);
    if (d.log_fp) fclose(d.log_fp);
    if (d.proto_log_fp) fclose(d.proto_log_fp);
    mqtt_cleanup();
    CloseHandle(d.resp_event);
    DeleteCriticalSection(&d.resp_lock);
    DeleteCriticalSection(&d.send_lock);
    DeleteCriticalSection(&d.lock);
    return 0;
}
