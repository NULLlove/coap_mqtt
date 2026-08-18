/*
 * coap.h - 简化版 CoAP (RFC 7252) 协议栈
 *
 * 覆盖功能:
 *   - 报文头部 (Ver=1, Type, TKL, Code, Message ID)
 *   - Token
 *   - Options delta 编码/解码 (Uri-Path, Uri-Query, Content-Format, Block1)
 *   - Payload marker (0xFF)
 *   - UDP 收发 (Winsock2)
 *   - Block1 分块传输 (RFC 7959, 用于固件升级)
 *
 * 仅依赖 Windows Winsock2, 无第三方库。
 */
#ifndef COAP_H
#define COAP_H

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
#define COAP_VER 1    //协议版本位（ver），定义协议版本，固定为1，占2位

/* Type */
typedef enum {
    COAP_CON = 0,   /* Confirmable    */
    COAP_NON = 1,   /* Non-confirmable */
    COAP_ACK = 2,   /* Acknowledgement */
    COAP_RST = 3    /* Reset */
} coap_type_t;    //coap报文类型位（T），占两位

/* 请求方法 (Code = class.0detail) */
//coap报文的code位，占8位，0.XX表示请求
#define COAP_GET     0x01
#define COAP_POST    0x02
#define COAP_PUT     0x03
#define COAP_DELETE  0x04

/* 响应码 (参考 RFC 7252 §5.9) */
//coap报文的code位，占8位，2.xx表示成功响应，4.xx表示客户端错误，5.xx表示服务端错误
/* 2.xx Success */
#define COAP_CREATED                 0x41  /* 2.01 */
#define COAP_DELETED                 0x42  /* 2.02 */
#define COAP_VALID                   0x43  /* 2.03 */
#define COAP_CHANGED                 0x44  /* 2.04 */
#define COAP_CONTENT                 0x45  /* 2.05 */
/* 4.xx Client Error */
#define COAP_BAD_REQUEST             0x80  /* 4.00 */
#define COAP_UNAUTHORIZED            0x81  /* 4.01 */
#define COAP_BAD_OPTION              0x82  /* 4.02 */
#define COAP_FORBIDDEN               0x83  /* 4.03 */
#define COAP_NOT_FOUND               0x84  /* 4.04 */
#define COAP_METHOD_NOT_ALLOWED      0x85  /* 4.05 */
#define COAP_NOT_ACCEPTABLE          0x86  /* 4.06 */
#define COAP_REQUEST_ENTITY_INCOMPLETE 0x88 /* 4.08 */
#define COAP_PRECONDITION_FAILED     0x8C  /* 4.12 */
#define COAP_REQUEST_ENTITY_TOO_LARGE 0x8D /* 4.13 */
#define COAP_UNSUPPORTED_CONTENT_FMT 0x8F  /* 4.15 */
/* 5.xx Server Error */
#define COAP_INTERNAL_ERROR          0xA0  /* 5.00 */
#define COAP_NOT_IMPLEMENTED         0xA1  /* 5.01 */
#define COAP_BAD_GATEWAY             0xA2  /* 5.02 */
#define COAP_SERVICE_UNAVAILABLE     0xA3  /* 5.03 */
#define COAP_GATEWAY_TIMEOUT         0xA4  /* 5.04 */
#define COAP_PROXYING_NOT_SUPPORTED  0xA5  /* 5.05 */

/* 选项编号 ( RFC 7252 §5.10, RFC 7641, RFC 7959, RFC 7967) */
#define OPT_IF_MATCH       1    /* If-Match (RFC 7252) */
#define OPT_URI_HOST       3    /* Uri-Host (RFC 7252) */
#define OPT_ETAG           4    /* ETag (RFC 7252) */
#define OPT_IF_NONE_MATCH  5    /* If-None-Match (RFC 7252, 空选项) */
#define OPT_OBSERVE        6    /* Observe (RFC 7641, 订阅/通知) */
#define OPT_URI_PORT       7    /* Uri-Port (RFC 7252) */
#define OPT_LOCATION_PATH  8    /* Location-Path (RFC 7252, 2.01 响应) */
#define OPT_URI_PATH       11   /* Uri-Path (RFC 7252) */
#define OPT_CONTENT_FMT    12   /* Content-Format (RFC 7252) */
#define OPT_MAX_AGE        14   /* Max-Age (RFC 7252) */
#define OPT_URI_QUERY      15   /* Uri-Query (RFC 7252) */
#define OPT_ACCEPT         17   /* Accept (RFC 7252) */
#define OPT_LOCATION_QUERY 20   /* Location-Query (RFC 7252) */
#define OPT_BLOCK2         23   /* Block2 (RFC 7959, 响应分块) */
#define OPT_BLOCK1         27   /* Block1 (RFC 7959, 请求分块) */
#define OPT_SIZE2          28   /* Size2 (RFC 7959, 响应总大小) */
#define OPT_PROXY_URI      35   /* Proxy-URI (RFC 7252) */
#define OPT_PROXY_SCHEME   39   /* Proxy-Scheme (RFC 7252) */
#define OPT_SIZE1          60   /* Size1 (RFC 7959, 请求总大小) */
#define OPT_NO_RESPONSE    258  /* No-Response (RFC 7967) */

/* Content formats ( IANA CoAP Content-Formats 注册表) */
//coap报文的负载格式位（Content-Format位），占8位
#define FMT_TEXT_PLAIN    0    /* text/plain */
#define FMT_LINK_FORMAT   40   /* application/link-format (RFC 6690) */
#define FMT_OCTET_STREAM  42   /* application/octet-stream */
#define FMT_EXI           47   /* application/exi */
#define FMT_JSON          50   /* application/json */
#define FMT_CBOR          60   /* application/cbor */
#define FMT_SENML_JSON    110  /* application/senml+json (RFC 8428) */
#define FMT_SENML_CBOR    112  /* application/senml+cbor (RFC 8428) */

/* 选项值最大长度 (本地存储用) */
#define COAP_OPT_VALUE_MAX  64

#define COAP_PAYLOAD_MARKER 0xFF   //分隔符，CoAP报文和具体负载之间的分隔符，固定一个字节
#define COAP_MAX_MSG        2048  //报文最大字节数

#define COAP_DEFAULT_PORT   5683   //coap默认的UDP端口为5683

/* Block1 分块大小: SZX=4 -> 2^(4+4) = 256 字节/块 */
//一块的最大大小为256字节，超过这个大小的负载会被分块传输
#define BLOCK_SZX  4
#define BLOCK_SIZE (1 << (BLOCK_SZX + 4))

/* ===================== 通用选项结构 ===================== */
/* 参考 libcoap 的 coap_opt_t: 统一存储任意选项, 不再为每个选项单独定义字段 */
#define COAP_MAX_OPTIONS 16   /* 单条报文最多携带的选项数 */

typedef struct {
    uint16_t number;                          /* 选项编号 (如 OPT_URI_PATH=11) */
    uint16_t length;                          /* 值长度 */
    uint8_t  value[COAP_OPT_VALUE_MAX];       /* 值数据 (本地存储, 避免悬垂指针) */
} coap_option_t;

/* ===================== 报文逻辑表示 ===================== */
typedef struct {
    coap_type_t type;       //coap报文类型位（T），占两位
    uint8_t     code;       //coap报文的code位，占8位
    uint16_t    msg_id;       //coap报文的编号位（Message ID位），占16位
    uint8_t     token[8];     //coap报文的token位，占8位
    uint8_t     token_len;    //coap报文的token位的长度，占1位

    /* 便捷字段 (向后兼容: 解析时同时填充这些字段和 options 数组) */
    char        uri_path[64];  //coap报文的URI路径位（Uri-Path位），占64位
    char        uri_query[128]; //coap报文的查询参数位（Uri-Query位），占128位
    int         content_format; //coap报文的负载格式位（Content-Format位），占8位
    /* Block1: 请求分块 (RFC 7959, 客户端→服务端, 如固件升级 PUT) */
    int         has_block1;     //coap报文是否有Block1分块传输，占1位
    int         block1_num;     //coap报文的Block1分块编号位，占4位
    int         block1_more;    //coap报文的Block1分块是否还有更多分块位，占1位
    int         block1_szx;     //coap报文的Block1分块大小位，占4位
    /* Block2: 响应分块 (RFC 7959, 服务端→客户端, 如大文件 GET) */
    int         has_block2;     //是否有Block2分块传输
    int         block2_num;     //Block2分块编号
    int         block2_more;    //Block2是否还有更多分块
    int         block2_szx;     //Block2分块大小

    /* 通用选项数组: 解析时存储报文中的所有选项 (含未知的);
       构造时通过 coap_add_option() 添加额外选项, 与便捷字段合并编码 */
    coap_option_t options[COAP_MAX_OPTIONS];
    int           option_count;

    /* payload: 解析时拷贝到 payload_buf (避免指向调用方易失的接收缓冲导致悬垂指针);
       构造请求时由调用方直接将 payload 指向外部数据, 此时 payload_buf 不使用 */
    uint8_t        payload_buf[COAP_MAX_MSG];   //解析时的内部缓冲区，最大占2048位
    const uint8_t *payload;                     //coap报文的负载指针，指向payload_buf
    size_t         payload_len;                  //coap报文的负载长度，占4位
} coap_msg_t;

/* ===================== API 接口===================== */
int      coap_init(void);                 /* 初始化coap协议栈 */
void     coap_cleanup(void);              /* 清理coap协议栈 */
SOCKET   coap_open_socket(uint16_t port); /* 绑定 UDP 端口; port=0 则不绑定(客户端用) */
void     coap_close_socket(SOCKET s);     /* 关闭 UDP 套接字 */

int      coap_build(uint8_t *buf, size_t buflen, const coap_msg_t *m);  /* 构造coap报文 */
int      coap_parse(const uint8_t *buf, size_t len, coap_msg_t *m);      /* 解析coap报文 */

int      coap_send(SOCKET s, const char *ip, uint16_t port,  
                   const uint8_t *data, size_t len);  /* 发送coap报文 */
int      coap_recv(SOCKET s, uint8_t *buf, size_t buflen,
                   char *from_ip, uint16_t *from_port, int timeout_ms);  /* 接收coap报文 */

const char *coap_method_name(uint8_t code);     /* 获取coap请求方法的名称 */
const char *coap_response_name(uint8_t code);   /* 获取coap响应码的名称 */

/* ===================== 选项操作 API  ===================== */
/* 向报文添加任意选项 (构造报文时用, 与便捷字段合并编码) */
int coap_add_option(coap_msg_t *m, uint16_t number,
                    const uint8_t *value, size_t length);
/* 在报文中查找指定编号的第一个选项 (解析报文后用) */
const coap_option_t *coap_find_option(const coap_msg_t *m, uint16_t number);

/* ===================== 报文构造辅助函数 ===================== */
/* 构造请求报文 (CON/NON + GET/POST/PUT/DELETE) */
void coap_make_request(coap_msg_t *m, coap_type_t type, uint8_t code,
                       const char *uri_path,
                       const uint8_t *payload, size_t payload_len);
/* 构造响应报文 (ACK + 2.xx/4.xx/5.xx) */
void coap_make_response(coap_msg_t *m, coap_type_t type, uint8_t code,
                        const uint8_t *token, uint8_t token_len,
                        uint16_t msg_id,
                        const uint8_t *payload, size_t payload_len);

#endif /* COAP_H */