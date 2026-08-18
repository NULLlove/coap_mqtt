/*
 * coap.c - CoAP 协议栈实现
 *
 * 实现要点:
 *   - 报文头 4 字节: Ver|T|TKL, Code, Message ID(16)
 *   - Token (0-8 字节)
 *   - Options: delta + length 编码 (RFC 7252 §3.1)
 *       delta <13 直接; ==13 后跟 1 字节扩展; ==14 后跟 2 字节扩展; ==15 非法
 *       length 同理
 *   - 0xFF 分隔符
 *   - Block1 选项 (RFC 7959): value = (NUM<<4) | (M<<3) | SZX, 可变长整数(1-3B)，块的最大大小256
 */
#include "coap.h"
#include <string.h>
#include <stdio.h>

static int wsa_inited = 0;

 //coap初始化函数，初始化Winsock2库
int coap_init(void) {   
    WSADATA wsa;
    if (WSAStartup(MAKEWORD(2, 2), &wsa) != 0) return -1;
    wsa_inited = 1;  //初始化标志位
    return 0;
}

void coap_cleanup(void) {
    if (wsa_inited) WSACleanup();  //清理Winsock2库
    wsa_inited = 0;  //重置标志位
}
 
 //coap打开UDP套接字函数，绑定端口
SOCKET coap_open_socket(uint16_t port) {
    SOCKET s = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (s == INVALID_SOCKET) return INVALID_SOCKET;

    /* 允许地址复用, 方便快速重启 */
    BOOL opt = TRUE;
    setsockopt(s, SOL_SOCKET, SO_REUSEADDR, (const char *)&opt, sizeof(opt));

    if (port != 0) {
        struct sockaddr_in addr;
        memset(&addr, 0, sizeof(addr));
        addr.sin_family      = AF_INET;
        addr.sin_addr.s_addr = htonl(INADDR_ANY);
        addr.sin_port        = htons(port);
        if (bind(s, (struct sockaddr *)&addr, sizeof(addr)) == SOCKET_ERROR) {
            closesocket(s);
            return INVALID_SOCKET;
        }
    }
    return s;
}

void coap_close_socket(SOCKET s) {
    if (s != INVALID_SOCKET) closesocket(s);
}

 //coap发送函数，发送UDP数据
int coap_send(SOCKET s, const char *ip, uint16_t port,
              const uint8_t *data, size_t len) {
    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_port   = htons(port);
    if (inet_pton(AF_INET, ip, &addr.sin_addr) != 1) return -1;
    return sendto(s, (const char *)data, (int)len, 0,
                  (struct sockaddr *)&addr, sizeof(addr)); 
}

 //coap接收函数，接收UDP数据
int coap_recv(SOCKET s, uint8_t *buf, size_t buflen,
              char *from_ip, uint16_t *from_port, int timeout_ms) {
    fd_set fds;
    FD_ZERO(&fds); //清空fd_set
    FD_SET(s, &fds); //将套接字添加到fd_set中   
    struct timeval tv; 
    tv.tv_sec  = timeout_ms / 1000; //超时时间，单位秒
    tv.tv_usec = (timeout_ms % 1000) * 1000; //超时时间，单位微秒

    int r = select(0, &fds, NULL, NULL, timeout_ms > 0 ? &tv : NULL); 
    if (r <= 0) return -1;  /* 超时或错误 */

    struct sockaddr_in from;
    int fromlen = sizeof(from);
    int n = recvfrom(s, (char *)buf, (int)buflen, 0,
                     (struct sockaddr *)&from, &fromlen);
    if (n <= 0) return -1;
    if (from_ip)   inet_ntop(AF_INET, &from.sin_addr, from_ip, 64);
    if (from_port) *from_port = ntohs(from.sin_port);
    return n;
}

/* ---------- Option 编码辅助 ---------- */
static int append_option(uint8_t *buf, size_t buflen, size_t *off,
                         int *last_num, int opt_num,
                         const uint8_t *value, int value_len) {
    int delta = opt_num - *last_num;
    uint8_t first = 0;
    int d_ext_bytes = 0, d_ext = 0;
    int l_ext_bytes = 0, l_ext = 0;

    if (delta < 13)        first = (uint8_t)(delta << 4);
    else if (delta < 269)  { first = 13 << 4; d_ext = delta - 13;   d_ext_bytes = 1; }
    else                   { first = 14 << 4; d_ext = delta - 269;  d_ext_bytes = 2; }

    if (value_len < 13)       first |= (uint8_t)value_len;
    else if (value_len < 269) { first |= 13; l_ext = value_len - 13;  l_ext_bytes = 1; }
    else                      { first |= 14; l_ext = value_len - 269; l_ext_bytes = 2; }

    if (*off + 1 + d_ext_bytes + l_ext_bytes + value_len > buflen) return -1;

    buf[(*off)++] = first;
    if (d_ext_bytes == 1)      buf[(*off)++] = (uint8_t)d_ext;
    else if (d_ext_bytes == 2) { buf[(*off)++] = (uint8_t)(d_ext >> 8); buf[(*off)++] = (uint8_t)(d_ext & 0xff); }
    if (l_ext_bytes == 1)      buf[(*off)++] = (uint8_t)l_ext;
    else if (l_ext_bytes == 2) { buf[(*off)++] = (uint8_t)(l_ext >> 8); buf[(*off)++] = (uint8_t)(l_ext & 0xff); }
    memcpy(buf + *off, value, value_len);
    *off += value_len;
    *last_num = opt_num;
    return 0;
}

/* ---------- 报文构造 ----------
 *
 * 统一选项编码策略 (参考 libcoap):
 *   1. 把便捷字段 (uri_path, content_format, uri_query, block1, block2)
 *      转换为临时选项条目
 *   2. 追加 m->options[] 中的额外选项
 *   3. 按选项编号升序排序 (CoAP 要求选项编号递增)
 *   4. 统一遍历编码
 * 这样既保持向后兼容 (便捷字段仍可用), 又支持通过 coap_add_option() 添加任意选项
 */
int coap_build(uint8_t *buf, size_t buflen, const coap_msg_t *m) {
    if (m->token_len > 8) return -1;
    size_t off = 0;

    /* 头部 */
    if (off + 4 > buflen) return -1;
    buf[off++] = (uint8_t)((COAP_VER << 6) | (m->type << 4) | m->token_len);
    buf[off++] = m->code;
    buf[off++] = (uint8_t)(m->msg_id >> 8);
    buf[off++] = (uint8_t)(m->msg_id & 0xff);

    /* Token */
    if (m->token_len) {
        if (off + m->token_len > buflen) return -1;
        memcpy(buf + off, m->token, m->token_len);
        off += m->token_len;
    }

    /* 构建临时选项数组: 便捷字段最多产生 8 条 + 通用选项最多 16 条 */
    struct { uint16_t num; const uint8_t *val; int len; } tmp[COAP_MAX_OPTIONS + 8];
    uint8_t tmp_val[COAP_MAX_OPTIONS + 8][8]; /* 选项值的本地存储 (Content-Format, Block1/2) */
    int tc = 0;

    /* 便捷字段 → 临时选项 */

    /* Uri-Path: 支持多段路径, 按 '/' 拆分后逐段添加 */
    if (m->uri_path[0]) {
        const char *p = m->uri_path;
        if (*p == '/') p++;
        while (*p && tc < (int)(sizeof(tmp)/sizeof(tmp[0]))) {
            const char *slash = strchr(p, '/');
            int seg_len = slash ? (int)(slash - p) : (int)strlen(p);
            if (seg_len > 0) {
                tmp[tc].num = OPT_URI_PATH;
                tmp[tc].val = (const uint8_t *)p;
                tmp[tc].len = seg_len;
                tc++;
            }
            if (!slash) break;
            p = slash + 1;
        }
    }

    /* Content-Format (必须在 Uri-Query 之前, 因为 12 < 15) */
    if (m->content_format >= 0 && tc < (int)(sizeof(tmp)/sizeof(tmp[0]))) {
        uint8_t *cf = tmp_val[tc];
        int cflen;
        if (m->content_format > 255) {
            cf[0] = (uint8_t)(m->content_format >> 8);
            cf[1] = (uint8_t)(m->content_format & 0xff);
            cflen = 2;
        } else {
            cf[0] = (uint8_t)m->content_format;
            cflen = 1;
        }
        tmp[tc].num = OPT_CONTENT_FMT;
        tmp[tc].val = cf;
        tmp[tc].len = cflen;
        tc++;
    }

    /* Uri-Query */
    if (m->uri_query[0] && tc < (int)(sizeof(tmp)/sizeof(tmp[0]))) {
        tmp[tc].num = OPT_URI_QUERY;
        tmp[tc].val = (const uint8_t *)m->uri_query;
        tmp[tc].len = (int)strlen(m->uri_query);
        tc++;
    }

    /* Block1 (请求分块) */
    if (m->has_block1 && tc < (int)(sizeof(tmp)/sizeof(tmp[0]))) {
        uint32_t val = ((uint32_t)m->block1_num << 4)
                     | (m->block1_more ? 0x08 : 0)
                     | (m->block1_szx & 0x07);
        uint8_t *b = tmp_val[tc]; int blen;
        if (val <= 0xff)        { b[0] = (uint8_t)val; blen = 1; }
        else if (val <= 0xffff) { b[0] = (uint8_t)(val >> 8); b[1] = (uint8_t)(val & 0xff); blen = 2; }
        else                    { b[0] = (uint8_t)(val >> 16); b[1] = (uint8_t)((val >> 8) & 0xff); b[2] = (uint8_t)(val & 0xff); blen = 3; }
        tmp[tc].num = OPT_BLOCK1;
        tmp[tc].val = b;
        tmp[tc].len = blen;
        tc++;
    }

    /* Block2 (响应分块) */
    if (m->has_block2 && tc < (int)(sizeof(tmp)/sizeof(tmp[0]))) {
        uint32_t val = ((uint32_t)m->block2_num << 4)
                     | (m->block2_more ? 0x08 : 0)
                     | (m->block2_szx & 0x07);
        uint8_t *b = tmp_val[tc]; int blen;
        if (val <= 0xff)        { b[0] = (uint8_t)val; blen = 1; }
        else if (val <= 0xffff) { b[0] = (uint8_t)(val >> 8); b[1] = (uint8_t)(val & 0xff); blen = 2; }
        else                    { b[0] = (uint8_t)(val >> 16); b[1] = (uint8_t)((val >> 8) & 0xff); b[2] = (uint8_t)(val & 0xff); blen = 3; }
        tmp[tc].num = OPT_BLOCK2;
        tmp[tc].val = b;
        tmp[tc].len = blen;
        tc++;
    }

    /* 追加 m->options[] 中的额外选项 */
    for (int i = 0; i < m->option_count && tc < (int)(sizeof(tmp)/sizeof(tmp[0])); i++) {
        tmp[tc].num = m->options[i].number;
        tmp[tc].val = m->options[i].value;
        tmp[tc].len = m->options[i].length;
        tc++;
    }

    /* 按选项编号升序排序 (稳定冒泡排序, 保持相同编号选项的原顺序) */
    for (int i = 0; i < tc - 1; i++) {
        for (int j = 0; j < tc - 1 - i; j++) {
            if (tmp[j].num > tmp[j + 1].num) {
                uint16_t t_num = tmp[j].num;
                const uint8_t *t_val = tmp[j].val;
                int t_len = tmp[j].len;
                tmp[j].num = tmp[j + 1].num;
                tmp[j].val = tmp[j + 1].val;
                tmp[j].len = tmp[j + 1].len;
                tmp[j + 1].num = t_num;
                tmp[j + 1].val = t_val;
                tmp[j + 1].len = t_len;
            }
        }
    }

    /* 统一编码选项 */
    int last = 0;
    for (int i = 0; i < tc; i++) {
        if (append_option(buf, buflen, &off, &last, tmp[i].num, tmp[i].val, tmp[i].len) < 0)
            return -1;
    }

    /* Payload */
    if (m->payload_len > 0) {
        if (off + 1 + m->payload_len > buflen) return -1;
        buf[off++] = COAP_PAYLOAD_MARKER;
        memcpy(buf + off, m->payload, m->payload_len);
        off += m->payload_len;
    }
    return (int)off;
}

/* ---------- 报文解析 ---------- */
int coap_parse(const uint8_t *buf, size_t len, coap_msg_t *m) {
    memset(m, 0, sizeof(*m));
    m->content_format = -1;
    if (len < 4) return -1;

    uint8_t h0 = buf[0];
    if (((h0 >> 6) & 0x03) != COAP_VER) return -1;
    m->type      = (coap_type_t)((h0 >> 4) & 0x03);
    m->token_len = h0 & 0x0f;
    if (m->token_len > 8) return -1;
    m->code   = buf[1];
    m->msg_id = ((uint16_t)buf[2] << 8) | buf[3];

    size_t off = 4;
    if (m->token_len) {
        if (off + m->token_len > len) return -1;
        memcpy(m->token, buf + off, m->token_len);
        off += m->token_len;
    }

    /* Options */
    int last_opt = 0;
    char *path_ptr = m->uri_path;
    int  path_left = (int)sizeof(m->uri_path) - 1;

    while (off < len) {
        if (buf[off] == COAP_PAYLOAD_MARKER) {
            off++;
            m->payload_len = len - off;
            if (m->payload_len > sizeof(m->payload_buf))
                m->payload_len = sizeof(m->payload_buf);
            memcpy(m->payload_buf, buf + off, m->payload_len);
            m->payload = m->payload_buf;   /* 拷贝到自身缓冲, 避免 buf 释放后悬垂 */
            return 0;
        }
        uint8_t ob = buf[off++];
        int delta = (ob >> 4) & 0x0f;
        int ol    = ob & 0x0f;

        if (delta == 13) { if (off >= len) return -1; delta = 13 + buf[off++]; }
        else if (delta == 14) { if (off + 1 >= len) return -1; delta = 269 + ((buf[off] << 8) | buf[off + 1]); off += 2; }
        else if (delta == 15) return -1;

        if (ol == 13) { if (off >= len) return -1; ol = 13 + buf[off++]; }
        else if (ol == 14) { if (off + 1 >= len) return -1; ol = 269 + ((buf[off] << 8) | buf[off + 1]); off += 2; }
        else if (ol == 15) return -1;

        if (off + (size_t)ol > len) return -1;
        int opt_num = last_opt + delta;
        last_opt = opt_num;
        const uint8_t *val = buf + off;

        /* 统一存入 options 数组 (含未知选项, 供 coap_find_option 查询) */
        if (m->option_count < COAP_MAX_OPTIONS) {
            int copy_len = ol < COAP_OPT_VALUE_MAX ? ol : COAP_OPT_VALUE_MAX;
            m->options[m->option_count].number = (uint16_t)opt_num;
            m->options[m->option_count].length = (uint16_t)copy_len;
            memcpy(m->options[m->option_count].value, val, copy_len);
            m->option_count++;
        }

        switch (opt_num) {
            case OPT_URI_PATH:
                if (path_left > 0) {
                    if (m->uri_path[0] && path_left >= 1) { *path_ptr++ = '/'; path_left--; }
                    int cpy = ol < path_left ? ol : path_left;
                    memcpy(path_ptr, val, cpy);
                    path_ptr += cpy;
                    path_left -= cpy;
                    *path_ptr = '\0';
                }
                break;
            case OPT_URI_QUERY: {
                /* 多个 Uri-Query 选项用 '&' 连接 (而非覆写) */
                size_t qlen = strlen(m->uri_query);
                if (qlen > 0 && qlen < sizeof(m->uri_query) - 1) {
                    m->uri_query[qlen++] = '&';
                    m->uri_query[qlen] = '\0';
                }
                int cpy = (int)((int)sizeof(m->uri_query) - 1 - (int)qlen);
                if (cpy > ol) cpy = ol;
                if (cpy > 0) {
                    memcpy(m->uri_query + qlen, val, (size_t)cpy);
                    m->uri_query[qlen + (size_t)cpy] = '\0';
                }
                break;
            }
            case OPT_CONTENT_FMT: {
                int cf = 0;
                for (int i = 0; i < ol; i++) cf = (cf << 8) | val[i];
                m->content_format = cf;
                break;
            }
            case OPT_BLOCK1: {
                uint32_t v = 0;
                for (int i = 0; i < ol; i++) v = (v << 8) | val[i];
                m->has_block1   = 1;
                m->block1_szx   = v & 0x07;
                m->block1_more  = (v >> 3) & 0x01;
                m->block1_num   = v >> 4;
                break;
            }
            case OPT_BLOCK2: {
                uint32_t v = 0;
                for (int i = 0; i < ol; i++) v = (v << 8) | val[i];
                m->has_block2   = 1;
                m->block2_szx   = v & 0x07;
                m->block2_more  = (v >> 3) & 0x01;
                m->block2_num   = v >> 4;
                break;
            }
            default: break;  /* 未知选项已存入 options 数组, 此处无需处理 */
        }
        off += ol;
    }
    return 0;
}

/* ---------- 名称工具 ---------- */
const char *coap_method_name(uint8_t code) {
    switch (code) {
        case COAP_GET:    return "GET";
        case COAP_POST:   return "POST";
        case COAP_PUT:    return "PUT";
        case COAP_DELETE: return "DELETE";
        default:          return "?";
    }
}

const char *coap_response_name(uint8_t code) {
    switch (code) {
        /* 2.xx Success */
        case COAP_CREATED:            return "2.01 Created";
        case COAP_DELETED:            return "2.02 Deleted";
        case COAP_VALID:              return "2.03 Valid";
        case COAP_CHANGED:            return "2.04 Changed";
        case COAP_CONTENT:            return "2.05 Content";
        /* 4.xx Client Error */
        case COAP_BAD_REQUEST:        return "4.00 Bad Request";
        case COAP_UNAUTHORIZED:       return "4.01 Unauthorized";
        case COAP_BAD_OPTION:         return "4.02 Bad Option";
        case COAP_FORBIDDEN:          return "4.03 Forbidden";
        case COAP_NOT_FOUND:          return "4.04 Not Found";
        case COAP_METHOD_NOT_ALLOWED: return "4.05 Method Not Allowed";
        case COAP_NOT_ACCEPTABLE:     return "4.06 Not Acceptable";
        case COAP_REQUEST_ENTITY_INCOMPLETE: return "4.08 Request Entity Incomplete";
        case COAP_PRECONDITION_FAILED:return "4.12 Precondition Failed";
        case COAP_REQUEST_ENTITY_TOO_LARGE:  return "4.13 Request Entity Too Large";
        case COAP_UNSUPPORTED_CONTENT_FMT:   return "4.15 Unsupported Content-Format";
        /* 5.xx Server Error */
        case COAP_INTERNAL_ERROR:     return "5.00 Internal Server Error";
        case COAP_NOT_IMPLEMENTED:    return "5.01 Not Implemented";
        case COAP_BAD_GATEWAY:        return "5.02 Bad Gateway";
        case COAP_SERVICE_UNAVAILABLE:return "5.03 Service Unavailable";
        case COAP_GATEWAY_TIMEOUT:    return "5.04 Gateway Timeout";
        case COAP_PROXYING_NOT_SUPPORTED: return "5.05 Proxying Not Supported";
        default:                      return "?.?? Unknown";
    }
}

/* ---------- 选项操作 API (参考 libcoap) ---------- */

/* 向报文添加任意选项
 * 参数:
 *   m      - 报文结构体
 *   number - 选项编号 (如 OPT_OBSERVE, OPT_ACCEPT 等)
 *   value  - 选项值数据
 *   length - 选项值长度
 * 返回: 0 成功, -1 失败 (选项数已满或值过长)
 */
int coap_add_option(coap_msg_t *m, uint16_t number,
                    const uint8_t *value, size_t length) {
    if (m->option_count >= COAP_MAX_OPTIONS) return -1;
    if (length > COAP_OPT_VALUE_MAX) return -1;

    m->options[m->option_count].number = number;
    m->options[m->option_count].length = (uint16_t)length;
    if (length > 0 && value) {
        memcpy(m->options[m->option_count].value, value, length);
    }
    m->option_count++;
    return 0;
}

/* 在报文中查找指定编号的第一个选项
 * 参数:
 *   m      - 报文结构体 (通常为解析后的报文)
 *   number - 要查找的选项编号
 * 返回: 指向选项结构的指针, 未找到返回 NULL
 */
const coap_option_t *coap_find_option(const coap_msg_t *m, uint16_t number) {
    for (int i = 0; i < m->option_count; i++) {
        if (m->options[i].number == number) return &m->options[i];
    }
    return NULL;
}

/* ---------- 报文构造辅助函数 ---------- */

/* 构造请求报文
 * 参数:
 *   m          - 报文结构体 (会被清零后填充)
 *   type       - 报文类型 (CON / NON)
 *   code       - 请求方法 (COAP_GET / COAP_POST / COAP_PUT / COAP_DELETE)
 *   uri_path   - 资源路径 (如 "fwinfo" 或 "well-known/core", 支持 '/')
 *   payload    - 负载数据 (可为 NULL)
 *   payload_len- 负载长度
 */
void coap_make_request(coap_msg_t *m, coap_type_t type, uint8_t code,
                       const char *uri_path,
                       const uint8_t *payload, size_t payload_len) {
    memset(m, 0, sizeof(*m));
    m->type = type;
    m->code = code;
    m->content_format = -1;  /* 默认无 Content-Format */
    if (uri_path) {
        strncpy(m->uri_path, uri_path, sizeof(m->uri_path) - 1);
        m->uri_path[sizeof(m->uri_path) - 1] = '\0';
    }
    if (payload && payload_len > 0) {
        m->payload     = payload;
        m->payload_len = payload_len;
    }
}

/* 构造响应报文
 * 参数:
 *   m          - 报文结构体 (会被清零后填充)
 *   type       - 报文类型 (通常为 ACK)
 *   code       - 响应码 (如 COAP_CONTENT, COAP_NOT_FOUND)
 *   token      - 请求方的 Token (可为 NULL)
 *   token_len  - Token 长度 (0-8)
 *   msg_id     - 请求方的 Message ID (ACK 需回显)
 *   payload    - 负载数据 (可为 NULL)
 *   payload_len- 负载长度
 */
void coap_make_response(coap_msg_t *m, coap_type_t type, uint8_t code,
                        const uint8_t *token, uint8_t token_len,
                        uint16_t msg_id,
                        const uint8_t *payload, size_t payload_len) {
    memset(m, 0, sizeof(*m));
    m->type = type;
    m->code = code;
    m->msg_id = msg_id;
    m->content_format = -1;
    if (token && token_len > 0 && token_len <= 8) {
        memcpy(m->token, token, token_len);
        m->token_len = token_len;
    }
    if (payload && payload_len > 0) {
        m->payload     = payload;
        m->payload_len = payload_len;
    }
}
