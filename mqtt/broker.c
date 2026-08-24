/*
 * broker.c - 简化版 MQTT Broker (带资源注册表)
 *
 * 职责:
 *   1. 监听 TCP 1883 端口, 接受客户端连接
 *   2. 处理 CONNECT -> 回 CONNACK
 *   3. 处理 SUBSCRIBE -> 记录订阅 -> 回 SUBACK
 *      - 特殊处理 registry/+/info: 向新订阅者发送所有已注册资源信息
 *   4. 处理 PUBLISH -> 记录到资源注册表 -> 遍历订阅表转发 -> 若 QoS1 回 PUBACK
 *   5. 处理 PINGREQ -> 回 PINGRESP
 *   6. 处理 DISCONNECT -> 清理订阅和注册, 关闭连接
 *
 * 主题方案:
 *   registry/<id>/log      - 客户端 <id> 的日志资源
 *   registry/<id>/firmware - 客户端 <id> 的固件资源
 *   registry/<id>/fwinfo   - 客户端 <id> 的固件信息资源
 *   registry/+/info        - 订阅所有资源更新通知
 *
 * 编译: gcc -Wall -Wextra -O2 -o broker.exe mqtt.c broker.c -lws2_32
 */
#include "mqtt.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#define BROKER_PORT 1883

/* ---------- 订阅链表 ---------- */
typedef struct subscription {
    char     topic[MQTT_TOPIC_MAX];
    SOCKET   sock;
    char     client_id[MQTT_CLIENTID_MAX];
    struct subscription *next;
} subscription_t;

static subscription_t  *g_subs = NULL;
static CRITICAL_SECTION g_lock;
static volatile int     g_active_clients = 0;
static volatile int     g_ever_connected  = 0;

/* ---------- 资源注册表 ---------- */
typedef struct resource_entry {
    char     client_id[MQTT_CLIENTID_MAX];
    char     topic_type[32];      /* "log", "firmware", "fwinfo" */
    char     full_topic[MQTT_TOPIC_MAX]; /* "registry/A/log" */
    SOCKET   sock;                /* 发布者的 socket */
    time_t   last_update;
    struct resource_entry *next;
} resource_entry_t;

static resource_entry_t *g_registry = NULL;

/* ---------- Retain 消息存储 ---------- */
typedef struct retain_entry {
    char     topic[MQTT_TOPIC_MAX];
    uint8_t  payload[MQTT_MAX_MSG];
    size_t   payload_len;
    uint8_t  qos;
    struct retain_entry *next;
} retain_entry_t;

static retain_entry_t *g_retains = NULL;

/* 保存或更新 retain 消息。payload_len=0 表示删除该主题的 retain 消息 */
static void retain_save(const char *topic, const uint8_t *payload, size_t payload_len, uint8_t qos) {
    EnterCriticalSection(&g_lock);

    /* 查找已有条目 */
    retain_entry_t **pp = &g_retains;
    while (*pp) {
        if (strcmp((*pp)->topic, topic) == 0) {
            if (payload_len == 0) {
                /* 清空 retain 消息 */
                retain_entry_t *del = *pp;
                *pp = del->next;
                free(del);
                printf("[broker]  retain cleared: %s\n", topic);
                LeaveCriticalSection(&g_lock);
                return;
            }
            /* 更新已有条目 */
            (*pp)->payload_len = payload_len < sizeof((*pp)->payload) ? payload_len : sizeof((*pp)->payload);
            memcpy((*pp)->payload, payload, (*pp)->payload_len);
            (*pp)->qos = qos;
            printf("[broker]  retain updated: %s (%zu bytes)\n", topic, (*pp)->payload_len);
            LeaveCriticalSection(&g_lock);
            return;
        }
        pp = &(*pp)->next;
    }

    /* payload_len=0 但无旧条目则无需操作 */
    if (payload_len == 0) {
        LeaveCriticalSection(&g_lock);
        return;
    }

    /* 创建新条目 */
    retain_entry_t *ne = (retain_entry_t *)malloc(sizeof(retain_entry_t));
    if (!ne) {
        LeaveCriticalSection(&g_lock);
        return;
    }
    strncpy(ne->topic, topic, sizeof(ne->topic) - 1);
    ne->topic[sizeof(ne->topic) - 1] = '\0';
    ne->payload_len = payload_len < sizeof(ne->payload) ? payload_len : sizeof(ne->payload);
    memcpy(ne->payload, payload, ne->payload_len);
    ne->qos = qos;
    ne->next = g_retains;
    g_retains = ne;
    printf("[broker]  retain saved: %s (%zu bytes)\n", topic, ne->payload_len);
    LeaveCriticalSection(&g_lock);
}

/* 向新订阅者推送所有匹配的 retain 消息 (调用方需在 g_lock 外或已加锁) */
static void retain_send_to_subscriber(const char *topic_filter, SOCKET sock, const char *subscriber_id) {
    EnterCriticalSection(&g_lock);
    retain_entry_t *e = g_retains;
    while (e) {
        if (mqtt_topic_match(topic_filter, e->topic)) {
            mqtt_msg_t pub;
            /* retain消息发送给订阅者时也设置retain=1? MQTT规范: 推送retain给新订阅者时retain标志仍为1 */
            mqtt_make_publish(&pub, e->topic, e->payload, e->payload_len, e->qos, 1,
                              (uint16_t)(rand() & 0xffff));
            if (mqtt_send_packet(sock, &pub) == 0) {
                printf("[broker]  retain deliver '%s' -> %s (%zu bytes)\n",
                       e->topic, subscriber_id, e->payload_len);
            }
        }
        e = e->next;
    }
    LeaveCriticalSection(&g_lock);
}

/* 添加订阅 */
static void sub_add(const char *topic, SOCKET sock, const char *client_id) {
    subscription_t *s = (subscription_t *)malloc(sizeof(subscription_t));
    if (!s) return;
    strncpy(s->topic, topic, sizeof(s->topic) - 1);
    s->topic[sizeof(s->topic) - 1] = '\0';
    s->sock = sock;
    strncpy(s->client_id, client_id, sizeof(s->client_id) - 1);
    s->client_id[sizeof(s->client_id) - 1] = '\0';
    EnterCriticalSection(&g_lock);
    s->next = g_subs;
    g_subs  = s;
    LeaveCriticalSection(&g_lock);
}

/* 删除指定客户端的所有订阅 */
static void sub_remove_by_sock(SOCKET sock) {
    EnterCriticalSection(&g_lock);
    subscription_t **pp = &g_subs;
    while (*pp) {
        if ((*pp)->sock == sock) {
            subscription_t *del = *pp;
            *pp = del->next;
            free(del);
        } else {
            pp = &(*pp)->next;
        }
    }
    LeaveCriticalSection(&g_lock);
}

/* 删除指定客户端对指定主题的订阅 */
static void sub_remove_by_sock_and_topic(SOCKET sock, const char *topic) {
    EnterCriticalSection(&g_lock);
    subscription_t **pp = &g_subs;
    while (*pp) {
        if ((*pp)->sock == sock && strcmp((*pp)->topic, topic) == 0) {
            subscription_t *del = *pp;
            *pp = del->next;
            free(del);
            LeaveCriticalSection(&g_lock);
            return;
        }
        pp = &(*pp)->next;
    }
    LeaveCriticalSection(&g_lock);
}

/* 转发 PUBLISH: 遍历订阅, 主题匹配则发送 */
static int sub_forward(const char *topic, const uint8_t *payload, size_t payload_len,
                       uint8_t qos, uint16_t packet_id, SOCKET src_sock) {
    int forwarded = 0;
    EnterCriticalSection(&g_lock);
    subscription_t *s = g_subs;
    while (s) {
        if (s->sock != src_sock && mqtt_topic_match(s->topic, topic)) {
            mqtt_msg_t pub;
            mqtt_make_publish(&pub, topic, payload, payload_len, qos, 0, packet_id);
            if (mqtt_send_packet(s->sock, &pub) == 0) {
                printf("[broker]  forward '%s' -> %s\n", topic, s->client_id);
                forwarded++;
            }
        }
        s = s->next;
    }
    LeaveCriticalSection(&g_lock);
    return forwarded;
}

/* ---------- 资源注册表操作 ---------- */

/* 添加或更新资源注册条目 */
static void registry_add(const char *client_id, const char *topic_type, SOCKET sock) {
    char full_topic[MQTT_TOPIC_MAX];
    snprintf(full_topic, sizeof(full_topic), "registry/%s/%s", client_id, topic_type);

    EnterCriticalSection(&g_lock);

    /* 查找是否已存在 */
    resource_entry_t *e = g_registry;
    while (e) {
        if (strcmp(e->full_topic, full_topic) == 0) {
            /* 更新已有条目 */
            e->sock = sock;
            e->last_update = time(NULL);
            LeaveCriticalSection(&g_lock);
            printf("[broker]  registry updated: %s\n", full_topic);
            return;
        }
        e = e->next;
    }

    /* 创建新条目 */
    resource_entry_t *ne = (resource_entry_t *)malloc(sizeof(resource_entry_t));
    if (!ne) {
        LeaveCriticalSection(&g_lock);
        return;
    }
    strncpy(ne->client_id, client_id, sizeof(ne->client_id) - 1);
    ne->client_id[sizeof(ne->client_id) - 1] = '\0';
    strncpy(ne->topic_type, topic_type, sizeof(ne->topic_type) - 1);
    ne->topic_type[sizeof(ne->topic_type) - 1] = '\0';
    strncpy(ne->full_topic, full_topic, sizeof(ne->full_topic) - 1);
    ne->full_topic[sizeof(ne->full_topic) - 1] = '\0';
    ne->sock = sock;
    ne->last_update = time(NULL);
    ne->next = g_registry;
    g_registry = ne;

    LeaveCriticalSection(&g_lock);
    printf("[broker]  registry added: %s\n", full_topic);
}

/* 删除资源注册条目 */
static void registry_remove(const char *client_id, const char *topic_type) {
    char full_topic[MQTT_TOPIC_MAX];
    snprintf(full_topic, sizeof(full_topic), "registry/%s/%s", client_id, topic_type);

    EnterCriticalSection(&g_lock);
    resource_entry_t **pp = &g_registry;
    while (*pp) {
        if (strcmp((*pp)->full_topic, full_topic) == 0) {
            resource_entry_t *del = *pp;
            *pp = del->next;
            free(del);
            printf("[broker]  registry removed: %s\n", full_topic);
            break;
        }
        pp = &(*pp)->next;
    }
    LeaveCriticalSection(&g_lock);
}

/* 删除指定客户端的所有资源 */
static void registry_remove_by_sock(SOCKET sock) {
    EnterCriticalSection(&g_lock);
    resource_entry_t **pp = &g_registry;
    while (*pp) {
        if ((*pp)->sock == sock) {
            resource_entry_t *del = *pp;
            *pp = del->next;
            printf("[broker]  registry removed (disconnect): %s\n", del->full_topic);
            free(del);
        } else {
            pp = &(*pp)->next;
        }
    }
    LeaveCriticalSection(&g_lock);
}

/* 向新订阅者发送所有已注册资源信息 */
static void registry_send_all(SOCKET sock, const char *subscriber_id) {
    EnterCriticalSection(&g_lock);
    resource_entry_t *e = g_registry;
    while (e) {
        /* 发送资源信息到新订阅者 */
        char payload[256];
        int plen = snprintf(payload, sizeof(payload), "resource=%s,client=%s,type=%s",
                            e->full_topic, e->client_id, e->topic_type);
        mqtt_msg_t pub;
        mqtt_make_publish(&pub, "registry/info", (uint8_t *)payload, (size_t)plen,
                          MQTT_QOS_1, 0, (uint16_t)(rand() & 0xffff));
        if (mqtt_send_packet(sock, &pub) == 0) {
            printf("[broker]  send registry info '%s' -> %s\n", e->full_topic, subscriber_id);
        }
        e = e->next;
    }
    LeaveCriticalSection(&g_lock);
}

/* 从 PUBLISH 主题中解析出 client_id 和 topic_type
 * 主题格式: registry/<id>/<topic_type>
 * 返回: 1=成功解析, 0=不是资源注册主题 */
static int parse_registry_topic(const char *topic, char *client_id, size_t cid_size,
                                char *topic_type, size_t tt_size) {
    if (strncmp(topic, "registry/", 9) != 0) return 0;
    const char *p = topic + 9;
    const char *slash = strchr(p, '/');
    if (!slash) return 0;
    size_t cid_len = (size_t)(slash - p);
    if (cid_len >= cid_size) cid_len = cid_size - 1;
    memcpy(client_id, p, cid_len);
    client_id[cid_len] = '\0';

    const char *tt = slash + 1;
    /* 排除 "info" 主题 */
    if (strcmp(tt, "info") == 0) return 0;
    strncpy(topic_type, tt, tt_size - 1);
    topic_type[tt_size - 1] = '\0';
    return 1;
}

/* ---------- 带超时的 accept ---------- */
static SOCKET accept_with_timeout(SOCKET listen_sock, int timeout_ms,
                                  char *client_ip, uint16_t *client_port) {
    fd_set fds;
    FD_ZERO(&fds);
    FD_SET(listen_sock, &fds);
    struct timeval tv;
    tv.tv_sec  = timeout_ms / 1000;
    tv.tv_usec = (timeout_ms % 1000) * 1000;
    int r = select(0, &fds, NULL, NULL, &tv);
    if (r <= 0) return INVALID_SOCKET;
    struct sockaddr_in from;
    int fromlen = sizeof(from);
    SOCKET s = accept(listen_sock, (struct sockaddr *)&from, &fromlen);
    if (s == INVALID_SOCKET) return INVALID_SOCKET;
    if (client_ip)   inet_ntop(AF_INET, &from.sin_addr, client_ip, 64);
    if (client_port) *client_port = ntohs(from.sin_port);
    return s;
}

/* ---------- 客户端线程 ---------- */
typedef struct {
    SOCKET   sock;
    char     client_id[MQTT_CLIENTID_MAX];
    char     ip[64];
    uint16_t port;
    char     will_topic[MQTT_TOPIC_MAX];
    char     will_message[MQTT_MAX_MSG];
} client_ctx_t;

static DWORD WINAPI client_thread(LPVOID arg) {
    client_ctx_t *ctx = (client_ctx_t *)arg;
    SOCKET sock = ctx->sock;
    char  *cid  = ctx->client_id;
    strncpy(cid, "unknown", sizeof(ctx->client_id) - 1);
    printf("[broker]  client connected from %s:%u\n", ctx->ip, ctx->port);

    while (1) {
        mqtt_msg_t m;
        int rc = mqtt_recv_packet(sock, &m, 10000);
        if (rc < 0) {
            printf("[broker]  %s (%s:%u) disconnected (recv timeout/error)\n",
                   cid, ctx->ip, ctx->port);
            break;
        }

        switch (m.type) {
            case MQTT_CONNECT: {
                strncpy(cid, m.topic, sizeof(ctx->client_id) - 1);
                cid[sizeof(ctx->client_id) - 1] = '\0';
                printf("[broker]  CONNECT from %s (keepalive=%us)\n",
                       cid, m.packet_id);
                /* 存储遗嘱消息 */
                if (m.will_topic[0] != '\0') {
                    strncpy(ctx->will_topic, m.will_topic, sizeof(ctx->will_topic) - 1);
                    ctx->will_topic[sizeof(ctx->will_topic) - 1] = '\0';
                    strncpy(ctx->will_message, m.will_message, sizeof(ctx->will_message) - 1);
                    ctx->will_message[sizeof(ctx->will_message) - 1] = '\0';
                    printf("[broker]  %s registered will: topic='%s'\n",
                           cid, ctx->will_topic);
                }
                mqtt_msg_t ack;
                memset(&ack, 0, sizeof(ack));
                ack.type        = MQTT_CONNACK;
                ack.return_code = MQTT_CONNACK_ACCEPTED;
                mqtt_send_packet(sock, &ack);
                break;
            }
            case MQTT_SUBSCRIBE: {
                printf("[broker]  SUBSCRIBE from %s: '%s' (QoS %d)\n",
                       cid, m.topic, m.qos);
                sub_add(m.topic, sock, cid);

                /* 如果订阅的是 registry/+/info, 发送所有已注册资源 */
                if (strcmp(m.topic, "registry/+/info") == 0) {
                    printf("[broker]  find_all request from %s, sending registry info\n", cid);
                    registry_send_all(sock, cid);
                }

                /* 新订阅后立即推送匹配的 retain 消息 */
                retain_send_to_subscriber(m.topic, sock, cid);

                mqtt_msg_t ack;
                memset(&ack, 0, sizeof(ack));
                ack.type        = MQTT_SUBACK;
                ack.packet_id   = m.packet_id;
                ack.return_code = m.qos;
                mqtt_send_packet(sock, &ack);
                break;
            }
            case MQTT_PUBLISH: {
                printf("[broker]  PUBLISH from %s: topic='%s' QoS=%d (%zu bytes) retain=%d\n",
                       cid, m.topic, m.qos, m.payload_len, m.retain);

                /* 检查是否是资源注册主题 registry/<id>/<type> */
                char reg_cid[MQTT_CLIENTID_MAX];
                char reg_type[32];
                if (parse_registry_topic(m.topic, reg_cid, sizeof(reg_cid),
                                          reg_type, sizeof(reg_type))) {
                    /* 检查是否是删除消息 */
                    if (m.payload_len > 0 &&
                        strncmp((const char *)m.payload, "__DELETE__", 10) == 0) {
                        registry_remove(reg_cid, reg_type);
                        printf("[broker]  %s deleted resource: %s\n", cid, m.topic);
                        /* 删除资源时也清除对应的 retain 消息 */
                        if (m.retain) retain_save(m.topic, NULL, 0, 0);
                    } else {
                        /* 记录/更新资源注册表 */
                        registry_add(reg_cid, reg_type, sock);

                        /* 同时发送一个 info 通知到 registry/+/info 订阅者 */
                        char info_topic[] = "registry/info";
                        char info_payload[256];
                        int plen = snprintf(info_payload, sizeof(info_payload),
                                            "resource=registry/%s/%s,client=%s,type=%s,updated=yes",
                                            reg_cid, reg_type, reg_cid, reg_type);
                        sub_forward(info_topic, (uint8_t *)info_payload, (size_t)plen,
                                    MQTT_QOS_0, 0, sock);
                    }
                }

                /* Retain=1 时保存消息 (MQTT规范: retain=1+payload_len>0 保存/更新, retain=1+payload_len=0 删除) */
                if (m.retain) {
                    if (m.payload_len == 0) {
                        retain_save(m.topic, NULL, 0, 0);
                    } else {
                        /* 如果是 __DELETE__ 删除标记，也清除 retain */
                        int is_delete = (m.payload_len >= 10 &&
                                         strncmp((const char *)m.payload, "__DELETE__", 10) == 0);
                        if (is_delete) {
                            retain_save(m.topic, NULL, 0, 0);
                        } else {
                            retain_save(m.topic, m.payload, m.payload_len, m.qos);
                        }
                    }
                }

                /* 转发给匹配的订阅者 */
                sub_forward(m.topic, m.payload, m.payload_len, m.qos, m.packet_id, sock);

                /* QoS1: 回 PUBACK 给发布者 */
                if (m.qos == MQTT_QOS_1) {
                    mqtt_msg_t ack;
                    mqtt_make_puback(&ack, m.packet_id);
                    mqtt_send_packet(sock, &ack);
                }
                break;
            }
            case MQTT_PINGREQ: {
                mqtt_msg_t resp;
                memset(&resp, 0, sizeof(resp));
                resp.type = MQTT_PINGRESP;
                mqtt_send_packet(sock, &resp);
                break;
            }
            case MQTT_PUBACK: {
                break;
            }
            case MQTT_UNSUBSCRIBE: {
                printf("[broker]  UNSUBSCRIBE from %s: '%s'\n", cid, m.topic);
                sub_remove_by_sock_and_topic(sock, m.topic);
                mqtt_msg_t ack;
                memset(&ack, 0, sizeof(ack));
                ack.type      = MQTT_UNSUBACK;
                ack.packet_id = m.packet_id;
                mqtt_send_packet(sock, &ack);
                break;
            }
            case MQTT_UNSUBACK: {
                break;
            }
            case MQTT_DISCONNECT: {
                printf("[broker]  %s sent DISCONNECT\n", cid);
                goto done;
            }
            default:
                printf("[broker]  %s: unknown packet type %d\n", cid, m.type);
                break;
        }
    }

done:
    /* 发布遗嘱消息 (Last Will and Testament) */
    if (ctx->will_topic[0] != '\0') {
        uint16_t will_pkt_id = (uint16_t)(rand() & 0xffff);
        sub_forward(ctx->will_topic, (uint8_t *)ctx->will_message,
                    strlen(ctx->will_message), MQTT_QOS_1,
                    will_pkt_id, sock);
        printf("[broker]  %s will published: topic='%s'\n", cid, ctx->will_topic);
    }
    sub_remove_by_sock(sock);
    registry_remove_by_sock(sock);
    mqtt_tcp_close(sock);
    free(ctx);
    InterlockedDecrement((volatile LONG *)&g_active_clients);
    return 0;
}

/* ====================================================================
 * main
 * ==================================================================== */
int main(void) {
    if (mqtt_init() != 0) {
        fprintf(stderr, "WSAStartup failed\n");
        return 1;
    }
    InitializeCriticalSection(&g_lock);

    SOCKET listen_sock = mqtt_tcp_listen(BROKER_PORT);
    if (listen_sock == INVALID_SOCKET) {
        fprintf(stderr, "broker: cannot listen on port %d\n", BROKER_PORT);
        DeleteCriticalSection(&g_lock);
        mqtt_cleanup();
        return 1;
    }

    printf("[broker] ==== MQTT Broker started (with resource registry), listening on port %d ====\n", BROKER_PORT);

    while (1) {
        char     client_ip[64];
        uint16_t client_port;
        SOCKET   s = accept_with_timeout(listen_sock, 1000, client_ip, &client_port);
        if (s == INVALID_SOCKET) {
            if (g_ever_connected && g_active_clients == 0) {
                Sleep(2000);
                if (g_active_clients == 0) {
                    printf("[broker]  all clients disconnected, shutting down\n");
                    break;
                }
            }
            continue;
        }

        client_ctx_t *ctx = (client_ctx_t *)malloc(sizeof(client_ctx_t));
        if (!ctx) { mqtt_tcp_close(s); continue; }
        ctx->sock = s;
        strncpy(ctx->ip, client_ip, sizeof(ctx->ip) - 1);
        ctx->port = client_port;
        ctx->client_id[0] = '\0';
        ctx->will_topic[0] = '\0';
        ctx->will_message[0] = '\0';

        InterlockedIncrement((volatile LONG *)&g_active_clients);
        g_ever_connected = 1;

        HANDLE th = CreateThread(NULL, 0, client_thread, ctx, 0, NULL);
        if (th) CloseHandle(th);
    }

    mqtt_tcp_close(listen_sock);

    /* 清理残留订阅 */
    EnterCriticalSection(&g_lock);
    while (g_subs) {
        subscription_t *del = g_subs;
        g_subs = del->next;
        free(del);
    }
    while (g_registry) {
        resource_entry_t *del = g_registry;
        g_registry = del->next;
        free(del);
    }
    while (g_retains) {
        retain_entry_t *del = g_retains;
        g_retains = del->next;
        free(del);
    }
    LeaveCriticalSection(&g_lock);

    DeleteCriticalSection(&g_lock);
    mqtt_cleanup();

    printf("[broker] ==== MQTT Broker stopped ====\n");
    return 0;
}
