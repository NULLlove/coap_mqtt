/*
 * mqtt.c - MQTT 3.1.1 协议栈实现
 *
 * 实现要点:
 *   - 固定头: 1 字节 (类型+标志) + 剩余长度变长编码 (1-4 字节)
 *   - 剩余长度: 每字节低 7 位有效, 最高位为续传标志
 *   - CONNECT: Protocol Name("MQTT") + Level(4) + Flags + KeepAlive + ClientID
 *   - CONNACK: Session Present + Return Code
 *   - PUBLISH: Topic + [PacketID(QoS1)] + Payload
 *   - PUBACK: PacketID
 *   - SUBSCRIBE: PacketID + TopicFilter + QoS
 *   - SUBACK: PacketID + ReturnCode
 *   - PINGREQ/PINGRESP/DISCONNECT: 仅固定头
 *   - 主题匹配: + (单层通配) / # (多层通配)
 */
#include "mqtt.h"
#include <string.h>
#include <stdio.h>

static int wsa_inited = 0;

/* ===================== 生命周期 ===================== */
int mqtt_init(void) {
    WSADATA wsa;
    if (WSAStartup(MAKEWORD(2, 2), &wsa) != 0) return -1;
    wsa_inited = 1;
    return 0;
}

void mqtt_cleanup(void) {
    if (wsa_inited) WSACleanup();
    wsa_inited = 0;
}

/* ===================== TCP 网络层 ===================== */
SOCKET mqtt_tcp_connect(const char *ip, uint16_t port) {
    SOCKET s = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (s == INVALID_SOCKET) return INVALID_SOCKET;

    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_port   = htons(port);
    if (inet_pton(AF_INET, ip, &addr.sin_addr) != 1) {
        closesocket(s);
        return INVALID_SOCKET;
    }
    if (connect(s, (struct sockaddr *)&addr, sizeof(addr)) == SOCKET_ERROR) {
        closesocket(s);
        return INVALID_SOCKET;
    }
    return s;
}

SOCKET mqtt_tcp_listen(uint16_t port) {
    SOCKET s = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (s == INVALID_SOCKET) return INVALID_SOCKET;

    BOOL opt = TRUE;
    setsockopt(s, SOL_SOCKET, SO_REUSEADDR, (const char *)&opt, sizeof(opt));

    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family      = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_ANY);
    addr.sin_port        = htons(port);
    if (bind(s, (struct sockaddr *)&addr, sizeof(addr)) == SOCKET_ERROR) {
        closesocket(s);
        return INVALID_SOCKET;
    }
    if (listen(s, 8) == SOCKET_ERROR) {
        closesocket(s);
        return INVALID_SOCKET;
    }
    return s;
}

/* 接受连接函数
 * 输入:
 *   - listen_sock: 监听套接字
 *   - client_ip: 输出客户端 IP 地址
 *   - client_port: 输出客户端端口
 * 返回:
 *   - 成功: 新连接套接字
 *   - 失败: INVALID_SOCKET
 */
SOCKET mqtt_tcp_accept(SOCKET listen_sock, char *client_ip, uint16_t *client_port) {
    struct sockaddr_in from;
    int fromlen = sizeof(from);
    SOCKET s = accept(listen_sock, (struct sockaddr *)&from, &fromlen);
    if (s == INVALID_SOCKET) return INVALID_SOCKET;
    if (client_ip)   inet_ntop(AF_INET, &from.sin_addr, client_ip, 64);
    if (client_port) *client_port = ntohs(from.sin_port);
    return s;
}

/* 关闭套接字函数
 * 输入:
 *   - s: 要关闭的套接字
 */
void mqtt_tcp_close(SOCKET s) {
    if (s != INVALID_SOCKET) closesocket(s);
}

/* 发送数据函数
 * 输入:
 *   - s: 套接字
 *   - data: 要发送的数据
 *   - len: 数据长度
 * 返回:
 *   - 成功: 发送的字节数
 *   - 失败: -1
 */
int mqtt_tcp_send(SOCKET s, const uint8_t *data, size_t len) {
    size_t sent = 0;
    while (sent < len) {
        int n = send(s, (const char *)data + sent, (int)(len - sent), 0);
        if (n <= 0) return -1;
        sent += n;
    }
    return (int)sent;
}

/* 精确读取函数
 * 输入:
 *   - s: 套接字
 *   - buf: 输出缓冲区
 *   - len: 缓冲区长度
 *   - timeout_ms: 超时时间 (毫秒)
 * 返回:
 *   - 成功: 读取的字节数
 *   - 失败: -1
 * */
int mqtt_tcp_recv(SOCKET s, uint8_t *buf, size_t len, int timeout_ms) {
    size_t got = 0;
    while (got < len) {
        fd_set fds;
        FD_ZERO(&fds);
        FD_SET(s, &fds);
        struct timeval tv;
        tv.tv_sec  = timeout_ms / 1000;
        tv.tv_usec = (timeout_ms % 1000) * 1000;

        int r = select(0, &fds, NULL, NULL, timeout_ms > 0 ? &tv : NULL);
        if (r == 0) return -2;  /* 超时 */
        if (r < 0) return -1;   /* select 错误 */

        int n = recv(s, (char *)buf + got, (int)(len - got), 0);
        if (n <= 0) return -1;  /* 连接关闭或错误 */
        got += n;
    }
    return (int)got;
}

/* ===================== 剩余长度变长编解码 ===================== */
/* 剩余长度编码函数
 * 输入:
 *   - buf: 输出缓冲区
 *   - buflen: 缓冲区长度
 *   - value: 要编码的值
 * 返回:
 *   - 成功: 编码字节数
 *   - 失败: -1
 */
int mqtt_encode_remaining_length(uint8_t *buf, size_t buflen, uint32_t value) {
    int off = 0;
    do {
        if ((size_t)off >= buflen) return -1;
        uint8_t byte = (uint8_t)(value % 128); /* 取低 7 位 */
        value /= 128; 
        if (value > 0) byte |= 0x80;  /* 最高位为 1 */
               buf[off++] = byte;  
    } while (value > 0); 
    return off;
}

/* 剩余长度解码函数
 * 输入:
 *   - buf: 输入缓冲区
 *   - len: 输入缓冲区长度
 *   - value: 输出解码后的值
 * 返回:
 *   - 成功: 解码字节数
 *   - 失败: -1
 */
int mqtt_decode_remaining_length(const uint8_t *buf, size_t len, uint32_t *value) {
    uint32_t result = 0;
    uint32_t multiplier = 1;
    int off = 0;
    uint8_t byte;
    do {
        if ((size_t)off >= len) return -1;
        byte = buf[off++];
        result += (uint32_t)(byte & 127) * multiplier;  // 取低 7 位
        multiplier *= 128;  // 累加乘法因子
        if (multiplier > 128U * 128 * 128 * 128) return -1;  /* 超过 4 字节 */
    } while ((byte & 128) != 0); 
    *value = result;
    return off;
}

/* ===================== 报文编码 ===================== */
/* 报文编码函数
 * 输入:
 *   - buf: 输出缓冲区
 *   - buflen: 缓冲区长度
 *   - m: 要编码的 MQTT 消息
 * 返回:
 *   - 成功: 编码字节数
 *   - 失败: -1
 */
int mqtt_build(uint8_t *buf, size_t buflen, const mqtt_msg_t *m) {
    /* remaining 存放可变头 + 负载 */
    uint8_t remaining[MQTT_MAX_MSG];
    size_t rlen = 0;

    switch (m->type) {
        case MQTT_CONNECT: {
            /* Protocol Name: 0x00 0x04 "MQTT" */
            remaining[rlen++] = 0x00; remaining[rlen++] = 0x04;
            memcpy(remaining + rlen, "MQTT", 4); rlen += 4;
            /* Protocol Level: 4 (MQTT 3.1.1) */
            remaining[rlen++] = 0x04;
            /* Connect Flags: Clean Session = 1, Will = (will_topic 非空时置位) */
            {
                uint8_t flags = 0x02;  /* Clean Session */
                if (m->will_topic[0] != '\0') {
                    flags |= 0x04;          /* Will Flag */
                    flags |= (0x01 << 3);   /* Will QoS 0 */
                    /* Will Retain = 0 */
                }
                remaining[rlen++] = flags;
            }
            /* Keep Alive (复用 packet_id 字段) */
            uint16_t ka = m->packet_id;
            remaining[rlen++] = (uint8_t)(ka >> 8);
            remaining[rlen++] = (uint8_t)(ka & 0xff);
            /* Payload: ClientID (复用 topic 字段) */
            const char *cid = m->topic;
            size_t cid_len = strlen(cid);
            if (cid_len > 255) cid_len = 255;
            remaining[rlen++] = (uint8_t)(cid_len >> 8);
            remaining[rlen++] = (uint8_t)(cid_len & 0xff);
            memcpy(remaining + rlen, cid, cid_len); rlen += cid_len;
            /* Will Topic + Will Message (仅当 will_flag 置位时) */
            if (m->will_topic[0] != '\0') {
                size_t wt_len = strlen(m->will_topic);
                remaining[rlen++] = (uint8_t)(wt_len >> 8);
                remaining[rlen++] = (uint8_t)(wt_len & 0xff);
                memcpy(remaining + rlen, m->will_topic, wt_len); rlen += wt_len;

                size_t wm_len = strlen(m->will_message);
                if (wm_len > 65535) wm_len = 65535;
                remaining[rlen++] = (uint8_t)(wm_len >> 8);
                remaining[rlen++] = (uint8_t)(wm_len & 0xff);
                memcpy(remaining + rlen, m->will_message, wm_len); rlen += wm_len;
            }
            break;
        }
        case MQTT_CONNACK: {
            /* Session Present = 0, Return Code */
            remaining[rlen++] = 0x00;
            remaining[rlen++] = m->return_code;
            break;
        }
        case MQTT_PUBLISH: {
            /* Topic Name: 2 字节长度 + 字符串 */
            size_t tlen = strlen(m->topic);
            if (tlen > 65535) tlen = 65535;
            remaining[rlen++] = (uint8_t)(tlen >> 8);
            remaining[rlen++] = (uint8_t)(tlen & 0xff);
            memcpy(remaining + rlen, m->topic, tlen); rlen += tlen;
            /* Packet ID (仅 QoS > 0) */
            if (m->qos > 0) {
                remaining[rlen++] = (uint8_t)(m->packet_id >> 8);
                remaining[rlen++] = (uint8_t)(m->packet_id & 0xff);
            }
            /* Payload */
            if (m->payload_len > 0 && m->payload) {
                if (rlen + m->payload_len > sizeof(remaining)) return -1;
                memcpy(remaining + rlen, m->payload, m->payload_len);
                rlen += m->payload_len;
            }
            break;
        }
        case MQTT_PUBACK: {
            remaining[rlen++] = (uint8_t)(m->packet_id >> 8);
            remaining[rlen++] = (uint8_t)(m->packet_id & 0xff);
            break;
        }
        case MQTT_SUBSCRIBE: {
            /* Packet ID */
            remaining[rlen++] = (uint8_t)(m->packet_id >> 8);
            remaining[rlen++] = (uint8_t)(m->packet_id & 0xff);
            /* Topic Filter + QoS (本实现仅 1 个) */
            size_t tlen = strlen(m->topic);
            if (tlen > 65535) tlen = 65535;
            remaining[rlen++] = (uint8_t)(tlen >> 8);
            remaining[rlen++] = (uint8_t)(tlen & 0xff);
            memcpy(remaining + rlen, m->topic, tlen); rlen += tlen;
            remaining[rlen++] = m->qos;
            break;
        }
        case MQTT_SUBACK: {
            remaining[rlen++] = (uint8_t)(m->packet_id >> 8);
            remaining[rlen++] = (uint8_t)(m->packet_id & 0xff);
            remaining[rlen++] = m->return_code;
            break;
        }
        case MQTT_UNSUBSCRIBE: {
            /* Packet ID */
            remaining[rlen++] = (uint8_t)(m->packet_id >> 8);
            remaining[rlen++] = (uint8_t)(m->packet_id & 0xff);
            /* Topic Filter */
            size_t tlen = strlen(m->topic);
            if (tlen > 65535) tlen = 65535;
            remaining[rlen++] = (uint8_t)(tlen >> 8);
            remaining[rlen++] = (uint8_t)(tlen & 0xff);
            memcpy(remaining + rlen, m->topic, tlen); rlen += tlen;
            break;
        }
        case MQTT_UNSUBACK: {
            remaining[rlen++] = (uint8_t)(m->packet_id >> 8);
            remaining[rlen++] = (uint8_t)(m->packet_id & 0xff);
            break;
        }
        case MQTT_PINGREQ:
        case MQTT_PINGRESP:
        case MQTT_DISCONNECT:
            /* 无可变头无负载 */
            break;
        default:
            return -1;
    }

    /* 组装固定头 + remaining */
    uint8_t fixed[5];
    fixed[0] = (uint8_t)(m->type << 4);
    if (m->type == MQTT_PUBLISH) {
        fixed[0] |= (uint8_t)((m->qos & 0x03) << 1);
        if (m->retain) fixed[0] |= 0x01;
    }
    if (m->type == MQTT_SUBSCRIBE)  fixed[0] |= 0x02;  /* 固定 flags */
    if (m->type == MQTT_UNSUBSCRIBE) fixed[0] |= 0x02;  /* 固定 flags */
    if (m->type == MQTT_PUBACK)     fixed[0] |= 0x00;

    int rl_len = mqtt_encode_remaining_length(fixed + 1, sizeof(fixed) - 1, (uint32_t)rlen);
    if (rl_len < 0) return -1;

    size_t total = (size_t)(1 + rl_len) + rlen;
    if (total > buflen) return -1;

    size_t off = 0;
    buf[off++] = fixed[0];
    memcpy(buf + off, fixed + 1, rl_len); off += rl_len;
    memcpy(buf + off, remaining, rlen);
    return (int)total;
}

/* ===================== 报文解析 ===================== */
/* 报文解析函数
 * 输入:
 *   - buf: 输入缓冲区
 *   - len: 输入缓冲区长度
 *   - m: 解析后的 MQTT 消息结构体指针
 * 返回:
 *   - 成功: 解析字节数
 *   - 失败: -1
 */
int mqtt_parse(const uint8_t *buf, size_t len, mqtt_msg_t *m) {
    if (len < 2) return -1;
    memset(m, 0, sizeof(*m));
    m->payload = NULL;

    uint8_t h0 = buf[0];
    m->type = (uint8_t)(h0 >> 4);

    uint32_t remaining = 0;
    int rl_len = mqtt_decode_remaining_length(buf + 1, len - 1, &remaining);
    if (rl_len < 0) return -1;

    size_t off = (size_t)(1 + rl_len);
    if (off + remaining > len) return -1;  /* 数据不完整 */

    const uint8_t *p   = buf + off;
    size_t         plen = remaining;

    switch (m->type) {
        case MQTT_CONNECT: {
            /* 跳过 Protocol Name + Level + Flags + KeepAlive = 10 字节 */
            if (plen < 10) return -1;
            uint8_t connect_flags = p[7];
            /* ClientID */
            size_t cid_len = ((size_t)p[10] << 8) | p[11];
            if (12 + cid_len > plen) cid_len = plen - 12;
            if (cid_len >= sizeof(m->topic)) cid_len = sizeof(m->topic) - 1;
            memcpy(m->topic, p + 12, cid_len);
            m->topic[cid_len] = '\0';
            m->packet_id = ((uint16_t)p[8] << 8) | p[9];  /* KeepAlive */
            /* Will Topic + Will Message (仅当 Will Flag 置位时) */
            if (connect_flags & 0x04) {
                size_t off = 12 + cid_len;
                if (off + 2 <= plen) {
                    size_t wt_len = ((size_t)p[off] << 8) | p[off + 1];
                    off += 2;
                    if (off + wt_len <= plen) {
                        if (wt_len >= sizeof(m->will_topic)) wt_len = sizeof(m->will_topic) - 1;
                        memcpy(m->will_topic, p + off, wt_len);
                        m->will_topic[wt_len] = '\0';
                        off += wt_len;
                    }
                }
                if (off + 2 <= plen) {
                    size_t wm_len = ((size_t)p[off] << 8) | p[off + 1];
                    off += 2;
                    if (off + wm_len <= plen) {
                        if (wm_len >= sizeof(m->will_message)) wm_len = sizeof(m->will_message) - 1;
                        memcpy(m->will_message, p + off, wm_len);
                        m->will_message[wm_len] = '\0';
                    }
                }
            }
            break;
        }
        case MQTT_CONNACK: {
            if (plen < 2) return -1;
            m->return_code = p[1];
            break;
        }
        case MQTT_PUBLISH: {
            m->qos    = (uint8_t)((h0 >> 1) & 0x03);
            m->retain = (uint8_t)(h0 & 0x01);
            if (plen < 2) return -1;
            size_t tlen = ((size_t)p[0] << 8) | p[1];
            size_t hdr  = 2;
            if (tlen >= sizeof(m->topic)) tlen = sizeof(m->topic) - 1;
            if (2 + tlen > plen) return -1;
            memcpy(m->topic, p + 2, tlen);
            m->topic[tlen] = '\0';
            hdr += tlen;
            if (m->qos > 0) {
                if (hdr + 2 > plen) return -1;
                m->packet_id = ((uint16_t)p[hdr] << 8) | p[hdr + 1];
                hdr += 2;
            }
            /* Payload 拷贝到 payload_buf 避免悬垂指针 */
            size_t pay_len = plen - hdr;
            if (pay_len > sizeof(m->payload_buf)) pay_len = sizeof(m->payload_buf);
            memcpy(m->payload_buf, p + hdr, pay_len);
            m->payload     = m->payload_buf;
            m->payload_len = pay_len;
            break;
        }
        case MQTT_PUBACK: {
            if (plen < 2) return -1;
            m->packet_id = ((uint16_t)p[0] << 8) | p[1];
            break;
        }
        case MQTT_SUBSCRIBE: {
            if (plen < 5) return -1;
            m->packet_id = ((uint16_t)p[0] << 8) | p[1];
            size_t tlen = ((size_t)p[2] << 8) | p[3];
            if (tlen >= sizeof(m->topic)) tlen = sizeof(m->topic) - 1;
            if (4 + tlen > plen) return -1;
            memcpy(m->topic, p + 4, tlen);
            m->topic[tlen] = '\0';
            m->qos = p[4 + tlen];  /* 订阅的 QoS */
            break;
        }
        case MQTT_SUBACK: {
            if (plen < 3) return -1;
            m->packet_id = ((uint16_t)p[0] << 8) | p[1];
            m->return_code = p[2];
            break;
        }
        case MQTT_UNSUBSCRIBE: {
            if (plen < 4) return -1;
            m->packet_id = ((uint16_t)p[0] << 8) | p[1];
            size_t tlen = ((size_t)p[2] << 8) | p[3];
            if (tlen >= sizeof(m->topic)) tlen = sizeof(m->topic) - 1;
            if (4 + tlen > plen) return -1;
            memcpy(m->topic, p + 4, tlen);
            m->topic[tlen] = '\0';
            break;
        }
        case MQTT_UNSUBACK: {
            if (plen < 2) return -1;
            m->packet_id = ((uint16_t)p[0] << 8) | p[1];
            break;
        }
        case MQTT_PINGREQ:
        case MQTT_PINGRESP:
        case MQTT_DISCONNECT:
            break;
        default:
            break;  /* 忽略未知类型 */
    }
    return 0;
}

/* ===================== 收发完整报文 ===================== */
int mqtt_send_packet(SOCKET s, const mqtt_msg_t *m) {
    uint8_t buf[MQTT_MAX_MSG + 8];
    int n = mqtt_build(buf, sizeof(buf), m);
    if (n <= 0) return -1;
    return mqtt_tcp_send(s, buf, (size_t)n) > 0 ? 0 : -1;
}

int mqtt_recv_packet(SOCKET s, mqtt_msg_t *m, int timeout_ms) {
    /* 1. 读固定头第 1 字节 (类型) */
    uint8_t type_byte;
    int rc1 = mqtt_tcp_recv(s, &type_byte, 1, timeout_ms);
    if (rc1 == -2) return -2;  /* 超时 - 无数据 */
    if (rc1 <= 0) return -1;   /* 错误 */

    /* 2. 逐字节读剩余长度 */
    uint8_t  rl_buf[4];
    uint32_t remaining = 0;
    uint32_t multiplier = 1;
    int      rl_bytes = 0;
    uint8_t  b;
    do {
        if (mqtt_tcp_recv(s, &b, 1, timeout_ms) <= 0) return -1;
        rl_buf[rl_bytes++] = b;
        remaining += (uint32_t)(b & 127) * multiplier;
        multiplier *= 128;
        if (rl_bytes > 4) return -1;
    } while ((b & 128) != 0);

    /* 3. 组装完整报文 */
    if (remaining > MQTT_MAX_MSG) return -1;
    uint8_t buf[MQTT_MAX_MSG + 8];
    size_t off = 0;
    buf[off++] = type_byte;
    memcpy(buf + off, rl_buf, rl_bytes); off += rl_bytes;
    if (remaining > 0) {
        if (mqtt_tcp_recv(s, buf + off, remaining, timeout_ms) <= 0) return -1;
        off += remaining;
    }
    return mqtt_parse(buf, off, m);
}

/* 主题匹配函数
 * 输入:
 *   - filter: 订阅过滤器 (e.g., "a/b/#")
 *   - topic: 发布主题 (e.g., "a/b/c")
 * 返回:
 *   - 匹配成功: 1
 *   - 匹配失败: 0
 */
/* ===================== 主题匹配 ===================== */
int mqtt_topic_match(const char *filter, const char *topic) {
    /* 支持 + (匹配单层) 和 # (匹配多层, 必须在末尾) */
    while (*filter && *topic) {
        if (*filter == '#') {
            return 1;  /* # 匹配剩余所有层级 */
        }
        if (*filter == '+') {
            /* + 匹配到下一个 '/' 之前的内容 */
            while (*topic && *topic != '/') topic++;
            filter++;
            if (*filter == '/') filter++;
            if (*topic == '/') topic++;
            continue;
        }
        if (*filter != *topic) return 0;
        filter++;
        topic++;
    }
    if (*filter == '#' && *(filter + 1) == '\0') return 1;
    return (*filter == '\0' && *topic == '\0');
}

/* ===================== 辅助构造函数 ===================== */
/* 构造 MQTT 连接报文函数
 * 输入:
 *   - m: 要填充的 MQTT 消息结构体指针
 *   - client_id: 客户端 ID (e.g., "client123")
 *   - keepalive: 保持连接时间 (单位: 秒)
 */
void mqtt_make_connect(mqtt_msg_t *m, const char *client_id, uint16_t keepalive,
                       const char *will_topic, const char *will_message) {
    memset(m, 0, sizeof(*m));
    m->type      = MQTT_CONNECT;
    m->packet_id = keepalive;  /* 复用 packet_id 存放 KeepAlive */
    strncpy(m->topic, client_id, sizeof(m->topic) - 1);
    if (will_topic) {
        strncpy(m->will_topic, will_topic, sizeof(m->will_topic) - 1);
    }
    if (will_message) {
        strncpy(m->will_message, will_message, sizeof(m->will_message) - 1);
    }
}

/* 构造 MQTT 发布报文函数
 * 输入:
 *   - m: 要填充的 MQTT 消息结构体指针
 *   - topic: 发布主题 (e.g., "a/b/c")
 *   - payload: 发布负载
 *   - payload_len: 发布负载长度
 *   - qos: 发布 QoS
 *   - retain: 是否保留消息 (1=保留, 0=不保留)
 *   - packet_id: 发布包 ID
 */
void mqtt_make_publish(mqtt_msg_t *m, const char *topic, const uint8_t *payload,
                       size_t payload_len, uint8_t qos, uint8_t retain, uint16_t packet_id) {
    memset(m, 0, sizeof(*m));
    m->type       = MQTT_PUBLISH;
    m->qos        = qos;
    m->retain     = retain;
    m->packet_id  = packet_id;
    m->payload    = payload;
    m->payload_len = payload_len;
    strncpy(m->topic, topic, sizeof(m->topic) - 1);
}

/* 构造 MQTT 订阅报文函数
 * 输入:
 *   - m: 要填充的 MQTT 消息结构体指针
 *   - topic_filter: 订阅过滤器 (e.g., "a/b/#")
 *   - qos: 订阅 QoS
 *   - packet_id: 订阅包 ID
 */
void mqtt_make_subscribe(mqtt_msg_t *m, const char *topic_filter, uint8_t qos, uint16_t packet_id) {
    memset(m, 0, sizeof(*m));
    m->type      = MQTT_SUBSCRIBE;
    m->qos       = qos;
    m->packet_id = packet_id;
    strncpy(m->topic, topic_filter, sizeof(m->topic) - 1);
}

/* 构造 MQTT 取消订阅报文函数
 * 输入:
 *   - m: 要填充的 MQTT 消息结构体指针
 *   - topic_filter: 取消订阅的主题过滤器
 *   - packet_id: 取消订阅包 ID
 */
void mqtt_make_unsubscribe(mqtt_msg_t *m, const char *topic_filter, uint16_t packet_id) {
    memset(m, 0, sizeof(*m));
    m->type      = MQTT_UNSUBSCRIBE;
    m->packet_id = packet_id;
    strncpy(m->topic, topic_filter, sizeof(m->topic) - 1);
}

/* 构造 MQTT 发布确认报文函数
 * 输入:
 *   - m: 要填充的 MQTT 消息结构体指针
 *   - packet_id: 发布确认包 ID
 */
void mqtt_make_puback(mqtt_msg_t *m, uint16_t packet_id) {
    memset(m, 0, sizeof(*m));
    m->type      = MQTT_PUBACK;
    m->packet_id = packet_id;
}

/* 构造 MQTT PINGREQ 报文函数
 * 输入:
 *   - m: 要填充的 MQTT 消息结构体指针
 */
void mqtt_make_pingreq(mqtt_msg_t *m) {
    memset(m, 0, sizeof(*m));
    m->type = MQTT_PINGREQ;
}
