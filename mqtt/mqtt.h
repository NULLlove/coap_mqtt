/*
 * mqtt.h - 简化版 MQTT 3.1.1 协议栈
 *
 * 覆盖功能:
 *   - 报文类型: CONNECT / CONNACK / PUBLISH / PUBACK /
 *               SUBSCRIBE / SUBACK / PINGREQ / PINGRESP / DISCONNECT
 *   - 剩余长度变长编码 (1-4 字节)
 *   - QoS 0 (最多一次) 与 QoS 1 (至少一次, PUBLISH+PUBACK)
 *   - 主题通配符匹配 (+ 单层, # 多层)
 *   - TCP 长连接 (Winsock2)
 *
 * 仅依赖 Windows Winsock2, 无第三方库。
 */
#ifndef MQTT_H
#define MQTT_H

#ifndef _WIN32_WINNT
#define _WIN32_WINNT 0x0601
#endif

#define WIN32_LEAN_AND_MEAN
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>

#include <stdint.h>
#include <stddef.h>

/* ===================== 协议常量 ===================== */

/* MQTT 报文类型 */
#define MQTT_CONNECT     1   // 连接请求
#define MQTT_CONNACK     2   // 连接确认响应
#define MQTT_PUBLISH     3   // 发布消息
#define MQTT_PUBACK      4   // 发布确认响应
#define MQTT_SUBSCRIBE   8   // 订阅请求
#define MQTT_SUBACK      9   // 订阅确认响应
#define MQTT_PINGREQ    12  // 心跳请求
#define MQTT_PINGRESP   13  // 心跳响应
#define MQTT_DISCONNECT   14  // 断开连接请求

/* QoS 级别 */
#define MQTT_QOS_0  0   // 最多一次
#define MQTT_QOS_1  1   // 至少一次

/* CONNACK 返回码 */
#define MQTT_CONNACK_ACCEPTED       0  // 接受
#define MQTT_CONNACK_BAD_PROTOCOL   1  // 协议版本不支持
#define MQTT_CONNACK_ID_REJECTED    2  // ClientID 已存在或被拒绝
#define MQTT_CONNACK_SERVER_DOWN    3  // 服务器不可用
#define MQTT_CONNACK_BAD_AUTH       4  // 认证失败
#define MQTT_CONNACK_NOT_AUTH       5  // 未授权    

#define MQTT_DEFAULT_PORT 1883  //默认端口
#define MQTT_MAX_MSG      4096   /* 单条报文最大字节数 */
#define MQTT_TOPIC_MAX    128    // 最大主题长度
#define MQTT_CLIENTID_MAX 32     // 最大 ClientID 长度

/* ===================== 报文逻辑表示 ===================== */
typedef struct {
    uint8_t  type;          /* 报文类型 */
    uint8_t  qos;           /* QoS 级别 (PUBLISH 用) */
    uint8_t  retain;        /* retain 标志 (PUBLISH 用) */
    uint16_t packet_id;     /* 报文标识符 (QoS1 / SUBSCRIBE 用) */
    uint8_t  return_code;   /* CONNACK / SUBACK 返回码 */

    /* PUBLISH 主题 / CONNECT 的 ClientID */
    char     topic[MQTT_TOPIC_MAX];

    /* payload: 解析时拷贝到 payload_buf (避免悬垂指针);
       构造时由调用方直接将 payload 指向外部数据 */
    uint8_t        payload_buf[MQTT_MAX_MSG];
    const uint8_t *payload;
    size_t         payload_len;
} mqtt_msg_t;

/* ===================== API ===================== */

/* 生命周期 */
int  mqtt_init(void);                 /* WSAStartup */
void mqtt_cleanup(void);              /* WSACleanup */

/* TCP 网络层 */
SOCKET mqtt_tcp_connect(const char *ip, uint16_t port);  /* 客户端: 连接到服务器 */
SOCKET mqtt_tcp_listen(uint16_t port);                   /* Broker: 监听端口 */
SOCKET mqtt_tcp_accept(SOCKET listen_sock, char *client_ip, uint16_t *client_port);
void   mqtt_tcp_close(SOCKET s);
int    mqtt_tcp_send(SOCKET s, const uint8_t *data, size_t len);
int    mqtt_tcp_recv(SOCKET s, uint8_t *buf, size_t len, int timeout_ms);

/* 报文编解码 */
int mqtt_build(uint8_t *buf, size_t buflen, const mqtt_msg_t *m);  /* 返回总长度, <=0 失败 */
int mqtt_parse(const uint8_t *buf, size_t len, mqtt_msg_t *m);     /* 返回 0 成功, <0 失败 */

/* 收发完整报文 (封装 TCP + 编解码) */
int mqtt_send_packet(SOCKET s, const mqtt_msg_t *m);               /* 返回 0 成功 */
int mqtt_recv_packet(SOCKET s, mqtt_msg_t *m, int timeout_ms);     /* 返回 0 成功, <0 失败/超时 */

/* 剩余长度变长编解码 */
int  mqtt_encode_remaining_length(uint8_t *buf, size_t buflen, uint32_t value);  /* 返回字节数 */
int  mqtt_decode_remaining_length(const uint8_t *buf, size_t len, uint32_t *value); /* 返回消耗字节数, <0 失败 */

/* 主题匹配 (支持 + 和 # 通配符) */
int mqtt_topic_match(const char *filter, const char *topic);  /* 1=匹配, 0=不匹配 */

/* 构造常用报文的辅助函数 */
void mqtt_make_connect(mqtt_msg_t *m, const char *client_id, uint16_t keepalive);
void mqtt_make_publish(mqtt_msg_t *m, const char *topic, const uint8_t *payload,
                       size_t payload_len, uint8_t qos, uint8_t retain, uint16_t packet_id);
void mqtt_make_subscribe(mqtt_msg_t *m, const char *topic_filter, uint8_t qos, uint16_t packet_id);
void mqtt_make_puback(mqtt_msg_t *m, uint16_t packet_id);
void mqtt_make_pingreq(mqtt_msg_t *m);

#endif /* MQTT_H */
