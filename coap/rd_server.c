/*
 * rd_server.c - CoAP RD (Resource Directory) 服务器实现
 *
 * 基于 RFC 9176 标准 (CoRE Resource Directory), 实现资源目录注册与查询服务。
 * 设计思路:
 *   1. 复用 coap.c/coap.h 的协议栈, 仅在应用层扩展 RD 业务逻辑
 *   2. 端点注册表用固定数组 + 锁保护, 适合嵌入式场景
 *   3. 链接 (links) 用简易格式存储, 解析时支持逗号分隔的 link-format
 *   4. 后台线程定期清理过期注册
 */
#include "rd_server.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

/* ===================== 辅助函数: 解析 link-format 负载 ===================== */
/* 解析 CoRE Link Format 负载, 提取 uri/rt/if 属性
 * 格式: </fwinfo>;rt="version",</log>;rt="log";if="core"
 */
static int parse_links_payload(const uint8_t *payload, size_t len,
                               rd_endpoint_t *ep) {
    ep->link_count = 0;
    if (!payload || len == 0) return 0;

    char buf[RD_MAX_PAYLOAD];
    size_t cpy = len < sizeof(buf) - 1 ? len : sizeof(buf) - 1;
    memcpy(buf, payload, cpy);
    buf[cpy] = '\0';

    char *saveptr = NULL;
    char *line = strtok_r(buf, ",", &saveptr);
    while (line && ep->link_count < RD_MAX_LINKS) {
        /* 去除首尾空格 */
        while (*line == ' ') line++;
        char *end = line + strlen(line) - 1;
        while (end > line && *end == ' ') *end = '\0';

        rd_endpoint_t *lp = ep;  /* 只是占位, 下面用 ep->links[count] */
        (void)lp;

        /* 解析 </uri> 部分 */
        char *uri_start = strchr(line, '<');
        char *uri_end = strchr(line, '>');
        if (uri_start && uri_end && uri_end > uri_start) {
            int idx = ep->link_count;
            size_t uri_len = (size_t)(uri_end - uri_start - 1);
            if (uri_len >= sizeof(ep->links[idx].uri))
                uri_len = sizeof(ep->links[idx].uri) - 1;
            memcpy(ep->links[idx].uri, uri_start + 1, uri_len);
            ep->links[idx].uri[uri_len] = '\0';

            /* 解析属性: rt="..." 和 if="..." */
            char *attr = uri_end + 1;
            char *rt_start = strstr(attr, "rt=\"");
            if (rt_start) {
                rt_start += 4;
                char *rt_end = strchr(rt_start, '"');
                if (rt_end && rt_end > rt_start) {
                    size_t rt_len = (size_t)(rt_end - rt_start);
                    if (rt_len >= sizeof(ep->links[idx].rt))
                        rt_len = sizeof(ep->links[idx].rt) - 1;
                    memcpy(ep->links[idx].rt, rt_start, rt_len);
                    ep->links[idx].rt[rt_len] = '\0';
                }
            }

            char *if_start = strstr(attr, "if=\"");
            if (if_start) {
                if_start += 4;
                char *if_end = strchr(if_start, '"');
                if (if_end && if_end > if_start) {
                    size_t if_len = (size_t)(if_end - if_start);
                    if (if_len >= sizeof(ep->links[idx].ifdesc))
                        if_len = sizeof(ep->links[idx].ifdesc) - 1;
                    memcpy(ep->links[idx].ifdesc, if_start, if_len);
                    ep->links[idx].ifdesc[if_len] = '\0';
                }
            }

            /* 解析 ver="..." 属性 (版本号) */
            char *ver_start = strstr(attr, "ver=\"");
            if (ver_start) {
                ver_start += 5;
                char *ver_end = strchr(ver_start, '"');
                if (ver_end && ver_end > ver_start) {
                    size_t ver_len = (size_t)(ver_end - ver_start);
                    if (ver_len >= sizeof(ep->links[idx].ver))
                        ver_len = sizeof(ep->links[idx].ver) - 1;
                    memcpy(ep->links[idx].ver, ver_start, ver_len);
                    ep->links[idx].ver[ver_len] = '\0';
                }
            }

            /* 解析 hver="..." 属性 (历史版本列表) */
            char *hver_start = strstr(attr, "hver=\"");
            if (hver_start) {
                hver_start += 6;
                char *hver_end = strchr(hver_start, '"');
                if (hver_end && hver_end > hver_start) {
                    size_t hver_len = (size_t)(hver_end - hver_start);
                    if (hver_len >= sizeof(ep->links[idx].hver))
                        hver_len = sizeof(ep->links[idx].hver) - 1;
                    memcpy(ep->links[idx].hver, hver_start, hver_len);
                    ep->links[idx].hver[hver_len] = '\0';
                }
            }

            ep->link_count++;
        }
        line = strtok_r(NULL, ",", &saveptr);
    }
    return ep->link_count;
}

/* ===================== 端点注册表操作 ===================== */

/* 查找端点索引 (按名称) */
static int find_endpoint_index(rd_server_t *rd, const char *ep) {
    for (int i = 0; i < RD_MAX_ENDPOINTS; i++) {
        if (rd->endpoints[i].active &&
            strcmp(rd->endpoints[i].ep, ep) == 0)
            return i;
    }
    return -1;
}

/* 分配空槽 */
static int alloc_slot(rd_server_t *rd) {
    for (int i = 0; i < RD_MAX_ENDPOINTS; i++) {
        if (!rd->endpoints[i].active) return i;
    }
    return -1;
}

/* 清理过期端点 (由后台线程调用) */
static void cleanup_expired(rd_server_t *rd) {
    time_t now = time(NULL);
    int cleaned = 0;
    for (int i = 0; i < RD_MAX_ENDPOINTS; i++) {
        if (rd->endpoints[i].active) {
            double elapsed = difftime(now, rd->endpoints[i].last_update);
            if (elapsed > rd->endpoints[i].ttl) {
                printf("[RD] Expired endpoint: %s (age=%.0fs, ttl=%us)\n",
                       rd->endpoints[i].ep, elapsed, rd->endpoints[i].ttl);
                rd->endpoints[i].active = 0;
                rd->endpoints[i].ep[0] = '\0';
                rd->endpoints[i].base[0] = '\0';
                rd->endpoints[i].domain[0] = '\0';
                rd->endpoints[i].link_count = 0;
                cleaned++;
            }
        }
    }
    if (cleaned > 0)
        printf("[RD] Cleaned up %d expired endpoints\n", cleaned);
}

/* ===================== 注册 API ===================== */
//注册RD服务器中的资源
int rd_register(rd_server_t *rd, const char *ep, const char *base,
               const char *domain, uint32_t ttl,
               const char *links_payload, size_t links_len) {
    if (!rd || !ep || !*ep) return -1;

    EnterCriticalSection(&rd->lock);

    /* 检查是否已存在 (更新而非重复注册) */
    int idx = find_endpoint_index(rd, ep);
    if (idx >= 0) {
        /* 已存在: 转为更新操作 */
        rd_endpoint_t *existing = &rd->endpoints[idx];
        existing->last_update = time(NULL);
        if (ttl > 0) existing->ttl = ttl;
        if (links_payload && links_len > 0)
            parse_links_payload((const uint8_t *)links_payload, links_len, existing);
        LeaveCriticalSection(&rd->lock);
        printf("[RD] Updated existing endpoint: %s\n", ep);
        return 0;
    }

    /* 分配新槽位 */
    idx = alloc_slot(rd);
    if (idx < 0) {
        LeaveCriticalSection(&rd->lock);
        printf("[RD] Registration failed: registry full (%d max)\n", RD_MAX_ENDPOINTS);
        return -1;
    }

    rd_endpoint_t *new_ep = &rd->endpoints[idx];
    memset(new_ep, 0, sizeof(*new_ep));
    strncpy(new_ep->ep, ep, sizeof(new_ep->ep) - 1);
    if (base) strncpy(new_ep->base, base, sizeof(new_ep->base) - 1);
    if (domain) strncpy(new_ep->domain, domain, sizeof(new_ep->domain) - 1);
    new_ep->ttl = ttl > 0 ? ttl : rd->default_ttl;
    new_ep->last_update = time(NULL);
    new_ep->active = 1;

    if (links_payload && links_len > 0)
        parse_links_payload((const uint8_t *)links_payload, links_len, new_ep);

    LeaveCriticalSection(&rd->lock);
    printf("[RD] Registered: %s (ttl=%us, %d links)\n",
           ep, new_ep->ttl, new_ep->link_count);
    return 0;
}
//更新RD服务器中的端点
int rd_update(rd_server_t *rd, const char *ep, const char *links_payload,
               size_t links_len, uint32_t ttl) {
    if (!rd || !ep) return -1;

    EnterCriticalSection(&rd->lock);
    int idx = find_endpoint_index(rd, ep);
    if (idx < 0) {
        LeaveCriticalSection(&rd->lock);
        printf("[RD] Update failed: endpoint %s not found\n", ep);
        return -1;
    }

    rd_endpoint_t *target = &rd->endpoints[idx];
    target->last_update = time(NULL);
    if (ttl > 0) target->ttl = ttl;
    if (links_payload && links_len > 0)
        parse_links_payload((const uint8_t *)links_payload, links_len, target);

    LeaveCriticalSection(&rd->lock);
    printf("[RD] Updated: %s (ttl=%us, %d links)\n",
           ep, target->ttl, target->link_count);
    return 0;
}

//删除RD服务器中的端点
int rd_delete(rd_server_t *rd, const char *ep) {
    if (!rd || !ep) return -1;

    EnterCriticalSection(&rd->lock);
    int idx = find_endpoint_index(rd, ep);
    if (idx < 0) {
        LeaveCriticalSection(&rd->lock);
        printf("[RD] Delete failed: endpoint %s not found\n", ep);
        return -1;
    }

    rd->endpoints[idx].active = 0;
    rd->endpoints[idx].ep[0] = '\0';
    rd->endpoints[idx].base[0] = '\0';
    rd->endpoints[idx].domain[0] = '\0';
    rd->endpoints[idx].link_count = 0;

    LeaveCriticalSection(&rd->lock);
    printf("[RD] Deleted: %s\n", ep);
    return 0;
}

/* ===================== 查询 API ===================== */
//根据资源类型查询RD服务器中的资源
int rd_search_by_resource(rd_server_t *rd, const char *rt_filter,
                           char *out_buf, size_t buf_size) {
    if (!rd || !out_buf || buf_size == 0) return 0;

    int off = 0;
    int match_count = 0;

    EnterCriticalSection(&rd->lock);
    for (int i = 0; i < RD_MAX_ENDPOINTS; i++) {
        if (!rd->endpoints[i].active) continue;
        rd_endpoint_t *ep = &rd->endpoints[i];

        for (int j = 0; j < ep->link_count; j++) {
            int match = 1;
            if (rt_filter && *rt_filter) {
                match = (strstr(ep->links[j].rt, rt_filter) != NULL);
            }
            if (match) {
                if (off > 0 && off < (int)buf_size - 1)
                    out_buf[off++] = ',';
                int written;
                if (ep->links[j].ver[0] && ep->links[j].hver[0]) {
                    written = snprintf(out_buf + off, buf_size - (size_t)off,
                                       "</%s%s>;rt=\"%s\";ver=\"%s\";hver=\"%s\"",
                                       ep->base, ep->links[j].uri,
                                       ep->links[j].rt, ep->links[j].ver,
                                       ep->links[j].hver);
                } else if (ep->links[j].ver[0]) {
                    written = snprintf(out_buf + off, buf_size - (size_t)off,
                                       "</%s%s>;rt=\"%s\";ver=\"%s\"",
                                       ep->base, ep->links[j].uri,
                                       ep->links[j].rt, ep->links[j].ver);
                } else {
                    written = snprintf(out_buf + off, buf_size - (size_t)off,
                                       "</%s%s>;rt=\"%s\"",
                                       ep->base, ep->links[j].uri,
                                       ep->links[j].rt);
                }
                if (written > 0 && off + written < (int)buf_size)
                    off += written;
                else break;
                match_count++;
            }
        }
    }
    LeaveCriticalSection(&rd->lock);
    (void)match_count;
    return off;
}
//根据端点ID查询RD服务器中的资源
int rd_search_by_endpoint(rd_server_t *rd, const char *ep_filter,
                           char *out_buf, size_t buf_size) {
    if (!rd || !out_buf || buf_size == 0) return 0;

    int off = 0;
    int match_count = 0;

    EnterCriticalSection(&rd->lock);
    for (int i = 0; i < RD_MAX_ENDPOINTS; i++) {
        if (!rd->endpoints[i].active) continue;
        rd_endpoint_t *ep = &rd->endpoints[i];

        int match = 1;
        if (ep_filter && *ep_filter) {
            match = (strstr(ep->ep, ep_filter) != NULL);
        }
        if (match) {
            /* 输出该端点的所有资源 */
            for (int j = 0; j < ep->link_count; j++) {
                if (off > 0 && off < (int)buf_size - 1)
                    out_buf[off++] = ',';
                int written;
                if (ep->links[j].ver[0] && ep->links[j].hver[0]) {
                    written = snprintf(out_buf + off, buf_size - (size_t)off,
                                       "</%s%s>;rt=\"%s\";ver=\"%s\";hver=\"%s\"",
                                       ep->base, ep->links[j].uri,
                                       ep->links[j].rt, ep->links[j].ver,
                                       ep->links[j].hver);
                } else if (ep->links[j].ver[0]) {
                    written = snprintf(out_buf + off, buf_size - (size_t)off,
                                       "</%s%s>;rt=\"%s\";ver=\"%s\"",
                                       ep->base, ep->links[j].uri,
                                       ep->links[j].rt, ep->links[j].ver);
                } else {
                    written = snprintf(out_buf + off, buf_size - (size_t)off,
                                       "</%s%s>;rt=\"%s\"",
                                       ep->base, ep->links[j].uri,
                                       ep->links[j].rt);
                }
                if (written > 0 && off + written < (int)buf_size)
                    off += written;
                else break;
            }
            match_count++;
        }
    }
    LeaveCriticalSection(&rd->lock);
    (void)match_count;
    return off;
}
//根据域名查询RD服务器中的资源
int rd_search_by_domain(rd_server_t *rd, const char *domain_filter,
                         char *out_buf, size_t buf_size) {
    if (!rd || !out_buf || buf_size == 0) return 0;

    int off = 0;
    int match_count = 0;

    EnterCriticalSection(&rd->lock);
    for (int i = 0; i < RD_MAX_ENDPOINTS; i++) {
        if (!rd->endpoints[i].active) continue;
        rd_endpoint_t *ep = &rd->endpoints[i];

        int match = 1;
        if (domain_filter && *domain_filter) {
            match = (strcmp(ep->domain, domain_filter) == 0);
        }
        if (match) {
            for (int j = 0; j < ep->link_count; j++) {
                if (off > 0 && off < (int)buf_size - 1)
                    out_buf[off++] = ',';
                int written = snprintf(out_buf + off, buf_size - (size_t)off,
                                       "</%s%s>;rt=\"%s\"",
                                       ep->base, ep->links[j].uri,
                                       ep->links[j].rt);
                if (written > 0 && off + written < (int)buf_size)
                    off += written;
                else break;
            }
            match_count++;
        }
    }
    LeaveCriticalSection(&rd->lock);
    (void)match_count;
    return off;
}

/* ===================== RD 服务器核心 ===================== */

int rd_server_init(rd_server_t *rd, uint16_t port, uint32_t default_ttl) {
    memset(rd, 0, sizeof(*rd));
    rd->port = port;
    rd->default_ttl = default_ttl > 0 ? default_ttl : RD_DEFAULT_TTL;
    rd->running = 0;
    InitializeCriticalSection(&rd->lock);

    if (coap_init() < 0) {
        printf("[RD] coap_init failed\n");
        return -1;
    }

    rd->srv_sock = coap_open_socket(port);
    if (rd->srv_sock == INVALID_SOCKET) {
        printf("[RD] Failed to bind UDP port %u\n", port);
        coap_cleanup();
        return -1;
    }

    printf("[RD] RD server initialized on port %u (default TTL=%us)\n",
           port, rd->default_ttl);
    return 0;
}

void rd_server_cleanup(rd_server_t *rd) {
    rd_server_stop(rd);
    if (rd->srv_sock != INVALID_SOCKET)
        coap_close_socket(rd->srv_sock);
    DeleteCriticalSection(&rd->lock);
    coap_cleanup();
    printf("[RD] RD server cleaned up\n");
}

/* ===================== 请求处理路由 ===================== */
static void handle_rd_request(rd_server_t *rd, const coap_msg_t *req,
                              const char *from_ip, uint16_t from_port) {
    coap_msg_t resp;
    memset(&resp, 0, sizeof(resp));
    resp.type = (req->type == COAP_CON) ? COAP_ACK : COAP_NON;
    resp.msg_id = req->msg_id;
    resp.token_len = req->token_len;
    memcpy(resp.token, req->token, req->token_len);

    uint8_t *payload_p = NULL;
    size_t payload_n = 0;
    static __thread char resp_buf[RD_MAX_PAYLOAD];

    printf("[RD] RECV: %s /%s from %s:%u (query='%s', payload_len=%zu)\n",
           coap_method_name(req->code), req->uri_path,
           from_ip, from_port, req->uri_query, req->payload_len);

    /* ---- POST /rd: 注册端点 ---- */
    if (strcmp(req->uri_path, "rd") == 0 && req->code == COAP_POST) {
        /* 从 uri_query 中提取 ep=, base=, d=, et= 参数 */
        char ep_val[32] = {0}, base_val[64] = {0}, d_val[32] = {0};
        uint32_t ttl_val = rd->default_ttl;

        if (req->uri_query[0]) {
            char qcopy[256];
            strncpy(qcopy, req->uri_query, sizeof(qcopy) - 1);
            qcopy[sizeof(qcopy) - 1] = '\0';

            char *tok = strtok(qcopy, "&");
            while (tok) {
                if (strncmp(tok, "ep=", 3) == 0)
                    strncpy(ep_val, tok + 3, sizeof(ep_val) - 1);
                else if (strncmp(tok, "base=", 5) == 0)
                    strncpy(base_val, tok + 5, sizeof(base_val) - 1);
                else if (strncmp(tok, "d=", 2) == 0)
                    strncpy(d_val, tok + 2, sizeof(d_val) - 1);
                else if (strncmp(tok, "et=", 3) == 0)
                    ttl_val = (uint32_t)atoi(tok + 3);
                tok = strtok(NULL, "&");
            }
        }

        if (!*ep_val) {
            resp.code = COAP_BAD_REQUEST;
            const char *err = "missing ep parameter";
            resp.payload = (uint8_t *)err;
            resp.payload_len = strlen(err);
        } else {
            int rc = rd_register(rd, ep_val,
                                 *base_val ? base_val : NULL,
                                 *d_val ? d_val : NULL,
                                 ttl_val,
                                 (const char *)req->payload, req->payload_len);
            if (rc == 0) {
                resp.code = COAP_CREATED;  /* 2.01 Created */
                char loc[128];
                snprintf(loc, sizeof(loc), "/rd/%s", ep_val);
                resp.content_format = FMT_TEXT_PLAIN;
                payload_p = (uint8_t *)loc;
                payload_n = strlen(loc);
            } else {
                resp.code = COAP_SERVICE_UNAVAILABLE;
                const char *err = "registration failed";
                resp.payload = (uint8_t *)err;
                resp.payload_len = strlen(err);
            }
        }

    /* ---- PUT /rd/{endpoint}: 更新注册 ---- */
    } else if (strncmp(req->uri_path, "rd/", 3) == 0 && req->code == COAP_PUT) {
        const char *ep_name = req->uri_path + 3;

        /* 从 uri_query 提取 et= TTL */
        uint32_t ttl_val = 0;
        if (req->uri_query[0]) {
            char qcopy[128];
            strncpy(qcopy, req->uri_query, sizeof(qcopy) - 1);
            qcopy[sizeof(qcopy) - 1] = '\0';
            char *tok = strtok(qcopy, "&");
            while (tok) {
                if (strncmp(tok, "et=", 3) == 0)
                    ttl_val = (uint32_t)atoi(tok + 3);
                tok = strtok(NULL, "&");
            }
        }

        int rc = rd_update(rd, ep_name,
                           (const char *)req->payload, req->payload_len,
                           ttl_val);
        if (rc == 0) {
            resp.code = COAP_CHANGED;  /* 2.04 Changed */
            const char *ok = "updated";
            resp.payload = (uint8_t *)ok;
            resp.payload_len = strlen(ok);
        } else {
            resp.code = COAP_NOT_FOUND;
            const char *err = "endpoint not found";
            resp.payload = (uint8_t *)err;
            resp.payload_len = strlen(err);
        }

    /* ---- DELETE /rd/{endpoint}: 删除注册 ---- */
    } else if (strncmp(req->uri_path, "rd/", 3) == 0 && req->code == COAP_DELETE) {
        const char *ep_name = req->uri_path + 3;
        int rc = rd_delete(rd, ep_name);
        if (rc == 0) {
            resp.code = COAP_DELETED;  /* 2.02 Deleted */
            const char *ok = "deleted";
            resp.payload = (uint8_t *)ok;
            resp.payload_len = strlen(ok);
        } else {
            resp.code = COAP_NOT_FOUND;
            const char *err = "endpoint not found";
            resp.payload = (uint8_t *)err;
            resp.payload_len = strlen(err);
        }

    /* ---- GET /rd: 查询目录 ---- */
    } else if (strcmp(req->uri_path, "rd") == 0 && req->code == COAP_GET) {
        int n = 0;
        resp_buf[0] = '\0';

        if (req->uri_query[0]) {
            /* 按资源类型查询: res=, ep=, d= (忽略 from= 等其他参数) */
            char qcopy[256];
            strncpy(qcopy, req->uri_query, sizeof(qcopy) - 1);
            qcopy[sizeof(qcopy) - 1] = '\0';

            char *tok = strtok(qcopy, "&");
            int has_filter = 0;  /* 是否有有效过滤参数 */
            while (tok) {
                if (strncmp(tok, "res=", 4) == 0) {
                    n = rd_search_by_resource(rd, tok + 4, resp_buf, sizeof(resp_buf));
                    has_filter = 1;
                } else if (strncmp(tok, "ep=", 3) == 0) {
                    n = rd_search_by_endpoint(rd, tok + 3, resp_buf, sizeof(resp_buf));
                    has_filter = 1;
                } else if (strncmp(tok, "d=", 2) == 0) {
                    n = rd_search_by_domain(rd, tok + 2, resp_buf, sizeof(resp_buf));
                    has_filter = 1;
                }
                tok = strtok(NULL, "&");
            }
            /* 只有没有有效过滤参数时 (如只有 from=) 才返回所有端点 */
            if (!has_filter) {
                n = rd_search_by_resource(rd, NULL, resp_buf, sizeof(resp_buf));
            }
        } else {
            /* 无过滤: 返回所有注册端点的 link-format */
            n = rd_search_by_resource(rd, NULL, resp_buf, sizeof(resp_buf));
        }

        resp.code = COAP_CONTENT;
        resp.content_format = FMT_LINK_FORMAT;
        payload_p = (uint8_t *)resp_buf;
        payload_n = (size_t)n;

    /* ---- GET /rd/{endpoint}: 查询指定端点 ---- */
    } else if (strncmp(req->uri_path, "rd/", 3) == 0 && req->code == COAP_GET) {
        const char *ep_name = req->uri_path + 3;
        int n = rd_search_by_endpoint(rd, ep_name, resp_buf, sizeof(resp_buf));
        if (n > 0) {
            resp.code = COAP_CONTENT;
            resp.content_format = FMT_LINK_FORMAT;
            payload_p = (uint8_t *)resp_buf;
            payload_n = (size_t)n;
        } else {
            resp.code = COAP_NOT_FOUND;
            const char *err = "endpoint not found";
            resp.payload = (uint8_t *)err;
            resp.payload_len = strlen(err);
        }

    /* ---- GET /.well-known/core: RD 自身资源发现 ---- */
    } else if (strcmp(req->uri_path, "well-known/core") == 0 && req->code == COAP_GET) {
        const char *core = "</rd>;rt=\"core\",</.well-known/core>;rt=\"core\"";
        resp.code = COAP_CONTENT;
        resp.content_format = FMT_LINK_FORMAT;
        resp.payload = (uint8_t *)core;
        resp.payload_len = strlen(core);

    } else {
        resp.code = (req->code == COAP_GET || req->code == COAP_POST ||
                     req->code == COAP_PUT || req->code == COAP_DELETE)
                    ? COAP_NOT_FOUND : COAP_METHOD_NOT_ALLOWED;
        const char *err = "resource not found";
        resp.payload = (uint8_t *)err;
        resp.payload_len = strlen(err);
    }

    /* 设置 payload */
    if (payload_p) {
        resp.payload = payload_p;
        resp.payload_len = payload_n;
    }

    /* 构造并发送响应 */
    uint8_t sbuf[RD_MAX_PAYLOAD];
    int slen = coap_build(sbuf, sizeof(sbuf), &resp);
    if (slen > 0) {
        int sent = coap_send(rd->srv_sock, from_ip, from_port, sbuf, (size_t)slen);
        if (sent > 0) {
            printf("[RD] RESP: %s (slen=%d, payload_len=%zu)\n",
                   coap_response_name(resp.code), slen, resp.payload_len);
        } else {
            printf("[RD] Send response FAILED\n");
        }
    } else {
        printf("[RD] coap_build FAILED for response\n");
    }
}

/* ===================== 服务器线程 ===================== */

static DWORD WINAPI rd_server_thread(LPVOID arg) {
    rd_server_t *rd = (rd_server_t *)arg;
    uint8_t rbuf[RD_MAX_PAYLOAD];

    printf("[RD] RD server thread started\n");

    time_t last_cleanup = time(NULL);

    while (rd->running) {
        char from_ip[64];
        uint16_t from_port;
        int n = coap_recv(rd->srv_sock, rbuf, sizeof(rbuf),
                          from_ip, &from_port, 500);

        /* 定期清理过期端点 (每 RD_CLEANUP_INTERVAL 秒) */
        time_t now = time(NULL);
        if (difftime(now, last_cleanup) >= RD_CLEANUP_INTERVAL) {
            EnterCriticalSection(&rd->lock);
            cleanup_expired(rd);
            LeaveCriticalSection(&rd->lock);
            last_cleanup = now;
        }

        if (n <= 0) continue;

        coap_msg_t req;
        if (coap_parse(rbuf, (size_t)n, &req) < 0) {
            printf("[RD] Failed to parse request (n=%d)\n", n);
            continue;
        }

        handle_rd_request(rd, &req, from_ip, from_port);
    }

    printf("[RD] RD server thread stopped\n");
    return 0;
}

//启动RD服务器
int rd_server_start(rd_server_t *rd) {
    if (rd->running) return -1;
    rd->running = 1;

    HANDLE hThread = CreateThread(NULL, 0, rd_server_thread,
                                  rd, 0, NULL);
    if (!hThread) {
        printf("[RD] CreateThread failed\n");
        rd->running = 0;
        return -1;
    }
    CloseHandle(hThread);

    /* 启动过期清理线程 */
    printf("[RD] RD server started on port %u\n", rd->port);
    return 0;
}

void rd_server_stop(rd_server_t *rd) {
    if (!rd->running) return;
    rd->running = 0;
    /* 等待线程退出 (最多 1 秒) */
    Sleep(600);
    printf("[RD] RD server stopping...\n");
}

/* ===================== main ===================== */
int main(int argc, char **argv) {
    rd_server_t rd;
    uint16_t port = RD_DEFAULT_PORT;
    uint32_t ttl = RD_DEFAULT_TTL;  // 资源过期时间，3600s

    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--port") && i + 1 < argc)
            port = (uint16_t)atoi(argv[++i]);
        else if (!strcmp(argv[i], "--ttl") && i + 1 < argc)
            ttl = (uint32_t)atoi(argv[++i]);
    }

    //初始化RD服务器
    if (rd_server_init(&rd, port, ttl) < 0) {  
        printf("[RD] Init failed\n");
        return 1;
    }

    if (rd_server_start(&rd) < 0) {
        printf("[RD] Start failed\n");
        rd_server_cleanup(&rd);
        return 1;
    }

    printf("[RD] Commands:\n");
    printf("  quit        - stop server\n");
    printf("  status      - show registered endpoints\n");
    printf("  cleanup     - force expire check\n");
    printf("\n");

    char cmd[64];
    while (rd.running) {
        printf("[RD] > ");
        fflush(stdout);
        if (!fgets(cmd, sizeof(cmd), stdin)) break;

        /* 去除换行 */
        size_t len = strlen(cmd);
        while (len > 0 && (cmd[len-1] == '\n' || cmd[len-1] == '\r'))
            cmd[--len] = '\0';

        if (!strcmp(cmd, "quit") || !strcmp(cmd, "exit")) {
            rd.running = 0;
        } else if (!strcmp(cmd, "status")) {
            EnterCriticalSection(&rd.lock);
            int count = 0;
            for (int i = 0; i < RD_MAX_ENDPOINTS; i++) {
                if (rd.endpoints[i].active) {
                    rd_endpoint_t *ep = &rd.endpoints[i];
                    time_t now = time(NULL);
                    double age = difftime(now, ep->last_update);
                    printf("  [%d] ep=%s base=%s domain=%s ttl=%us age=%.0fs links=%d\n",
                           i, ep->ep, ep->base, ep->domain,
                           ep->ttl, age, ep->link_count);
                    for (int j = 0; j < ep->link_count; j++) {
                        printf("      </%s>;rt=\"%s\"",
                               ep->links[j].uri, ep->links[j].rt);
                        if (ep->links[j].ifdesc[0])
                            printf(";if=\"%s\"", ep->links[j].ifdesc);
                        printf("\n");
                    }
                    count++;
                }
            }
            if (count == 0) printf("  (no registered endpoints)\n");
            LeaveCriticalSection(&rd.lock);
        } else if (!strcmp(cmd, "cleanup")) {
            EnterCriticalSection(&rd.lock);
            cleanup_expired(&rd);
            LeaveCriticalSection(&rd.lock);
        } else if (cmd[0]) {
            printf("  Unknown command: %s (try quit/status/cleanup)\n", cmd);
        }
    }

    rd_server_cleanup(&rd);
    return 0;
}
