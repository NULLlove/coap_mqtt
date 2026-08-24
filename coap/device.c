/*
 * device.c - PC 端模拟的 CoAP 设备
 *
 * 一个可执行程序通过命令行参数实例化为一台设备, 多进程方式启动两台:
 *
 *   .\device.exe --id A --port 5683 --peer-ip 127.0.0.1 --peer-port 5684 --version 1.0.0-A
 *  
 *   .\device.exe --id B --port 5684 --peer-ip 127.0.0.1 --peer-port 5683 --version 1.0.0-B
 *
 * 每台设备同时扮演两个角色:
 *   1) 服务器 (后台线程, srv_sock): 暴露 CoAP 资源
 *        GET  /fwinfo    -> 读取本机固件文件的版本与大小
 *        GET  /log       -> 读取本机日志文件内容
 *        PUT  /firmware  -> 接收固件分块 (Block1), 写入固件文件, 完成后更新版本
 *   2) 客户端 (主线程, cli_sock): 主动向对端发起
 *        GET  /fwinfo    -> 读取对端固件信息
 *        GET  /log       -> 读取对端日志文件
 *        PUT  /firmware  -> 把本机固件文件分块推送给对端 (固件升级)
 *
 * 两台设备互为客户端/服务器, 即实现 "相互日志读取 + 相互固件升级"。
 * 该设备还支持 RD (Resource Directory) 模式, 可以通过 RD 查询其他设备的端点信息。
 *
 * 编译: gcc -Wall -Wextra -O2 -o device.exe coap.c device.c -lws2_32
 */
#include "coap.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>
#include <time.h>
#include <errno.h>

#define FW_FILLER_LEN 2600   /* 固件镜像中的填充数据长度, 确保需要分块传输 */
#define MAX_FW_VERSIONS 10  /* 最大固件版本历史数量 */
#define MAX_CONNECTIONS 16   /* 最大连接追踪数量 */
#define MAX_PEER_VERSIONS 8  /* 最大对端版本缓存数量 */
#define MAX_HISTORY_VERSIONS 5 /* RD 注册时携带的历史版本数量 */


// 连接追踪条目, 用于 get_link_id
typedef struct {
    char    ip[64];        /* 对端 IP */
    uint16_t port;         /* 对端端口 */
    char    peer_id[16];   /* 对端 ID (从请求中解析, 可能为空) */
    time_t  last_seen;     /* 最后一次请求时间 */
    int     active;        /* 是否活跃 */
} conn_entry_t;


// 对端版本缓存条目, 用于 rd_check <id>
typedef struct {
    char    peer_id[16];   /* 对端 ID */
    char    version[32];   /* 最后已知版本号 */
    char    history[128];  /* 历史版本列表 (逗号分隔) */
    time_t  last_check;    /* 最后检查时间 */
    int     active;        /* 是否活跃 */
} peer_ver_entry_t;

typedef struct {
    char        id[16];    //设备ID
    uint16_t    port;     //服务器监听端口
    char        peer_ip[64]; //对端IP地址 (静态配置的默认值, RD 查询失败时回退使用)
    uint16_t    peer_port;  //对端端口 (静态配置的默认值)
    char        peer_id[16]; //对端设备ID (用于 RD 查询)

    /* RD (Resource Directory) 服务器配置 */
    char        rd_ip[64];   //RD 服务器 IP
    uint16_t    rd_port;     //RD 服务器端口
    int         rd_enabled;  //是否启用 RD 模式 (1=启用, 0=传统直连模式)
    int         rd_registered; //本设备是否已成功向 RD 注册
    int         auto_poll;    //自动轮询标志 (1=启用, 0=禁用)

    /* 多对端版本缓存 (用于 rd_check <id>) */
    peer_ver_entry_t peer_vers[MAX_PEER_VERSIONS];

    /* 连接追踪 (用于 get_link_id) */
    conn_entry_t connections[MAX_CONNECTIONS];

    char        version[32]; //当前固件版本
    char        original_version[32];   // 启动时的原始版本, 升级对端时始终用它

    char        fw_path[64];      //当前固件文件 (firmware_<id>.bin), 被升级时覆盖
    char        fw_orig_path[64]; //原始固件文件 (firmware_<id>_orig.bin), 用于升级对端
    char        fw_versions_dir[64]; //固件版本历史目录
    char        log_path[64];   //日志文件路径 (device_<id>.log)
    char        proto_log_path[64]; //协议日志文件路径 (proto_<id>.log)
    
    /* 固件升级时保存的旧固件副本路径 (跨多次请求保留) */
    char        old_fw_copy_path[128];

    /* 固件版本历史: 存储历史版本号和时间戳 */
    char        fw_versions[MAX_FW_VERSIONS][32];  // 历史版本号
    char        fw_version_times[MAX_FW_VERSIONS][32]; // 历史版本升级时间
    int         fw_version_count;  // 当前历史版本数量

    CRITICAL_SECTION lock; //临界区锁
    FILE       *log_fp;     // 应用层日志文件句柄
    FILE       *proto_log_fp; // 协议层日志文件句柄
    SOCKET      srv_sock;   // 服务器: 绑定 port, 收请求/回 ACK
    SOCKET      cli_sock;   // 客户端: 未绑定, 发请求/收 ACK
    volatile int running;   //运行状态
    uint16_t    next_msg_id;  //下一个消息ID
} device_t;//设备结构体, 用于存储设备相关状态

/* ---------- 日志: 打印控制台 + 写入日志文件 (带时间戳) ---------- */
static void dev_log(device_t *d, const char *fmt, ...) {
    char line[512];
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(line, sizeof(line), fmt, ap);
    va_end(ap);
    if (n < 0) return;
    if (n > (int)sizeof(line) - 1) n = (int)sizeof(line) - 1;

    /* 生成时间戳 */
    time_t now = time(NULL);
    struct tm *tm = localtime(&now);
    char time_str[64];
    strftime(time_str, sizeof(time_str), "%Y-%m-%d %H:%M:%S", tm);

    /* 控制台输出 (带设备 ID 前缀和时间戳) */
    printf("[%s] [%s] %.*s\n", time_str, d->id, n, line);
    fflush(stdout);

    /* 写入日志文件 (带时间戳) */
    EnterCriticalSection(&d->lock);
    if (d->log_fp) {
        fprintf(d->log_fp, "[%s] [%s] %.*s\n", time_str, d->id, n, line);
        fflush(d->log_fp);
    }
    LeaveCriticalSection(&d->lock);
}

/* ---------- 协议日志: 记录 CoAP 报文传输内容 ---------- */
static void proto_log(device_t *d, const char *direction, const coap_msg_t *msg,
                      const uint8_t *raw_data, size_t raw_len) {
    if (!d->proto_log_fp) return;

    time_t now = time(NULL);
    struct tm *tm = localtime(&now);
    char time_str[64];
    strftime(time_str, sizeof(time_str), "%Y-%m-%d %H:%M:%S", tm);

    EnterCriticalSection(&d->lock);
    fprintf(d->proto_log_fp, "=== [%s] %s ===\n", time_str, direction);
    /* CoAP Code 字段: 高3位为 class, 低5位为 detail
       class=0 为请求方法 (GET/POST/PUT/DELETE),
       class=2/4/5 为响应码 (2.xx/4.xx/5.xx) */
    const char *code_name = (msg->code >> 5) == 0 ?
                            coap_method_name(msg->code) :
                            coap_response_name(msg->code);
    fprintf(d->proto_log_fp, "  Type: %s, Code: %s, MsgID: %u\n",
            msg->type == COAP_CON ? "CON" :
            msg->type == COAP_ACK ? "ACK" :
            msg->type == COAP_NON ? "NON" : "RST",
            code_name, msg->msg_id);
    fprintf(d->proto_log_fp, "  URI: /%s, Payload Len: %zu\n",
            msg->uri_path, msg->payload_len);

    if (msg->has_block1) {
        fprintf(d->proto_log_fp, "  Block1: num=%d, more=%d, szx=%d\n",
                msg->block1_num, msg->block1_more, msg->block1_szx);
    }

    if (raw_data && raw_len > 0) {
        fprintf(d->proto_log_fp, "  Raw Data (%zu bytes): ", raw_len);
        for (size_t i = 0; i < raw_len && i < 64; i++) {
            fprintf(d->proto_log_fp, "%02X ", raw_data[i]);
        }
        if (raw_len > 64) fprintf(d->proto_log_fp, "...");
        fprintf(d->proto_log_fp, "\n");
    }

    if (msg->payload_len > 0) {
        fprintf(d->proto_log_fp, "  Payload: ");
        size_t show = msg->payload_len < 128 ? msg->payload_len : 128;
        fwrite(msg->payload, 1, show, d->proto_log_fp);
        if (msg->payload_len > 128) fprintf(d->proto_log_fp, "...(truncated)");
        fprintf(d->proto_log_fp, "\n");
    }

    fprintf(d->proto_log_fp, "\n");
    fflush(d->proto_log_fp);
    LeaveCriticalSection(&d->lock);
}

/* 读取固件文件: 返回文件大小, version_buf 写入首行版本号 */
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

/* ---------- 从固件文件第一行同步版本号到内存 d->version ---------- */
/* 用于手动修改固件文件后，rd_register/rd_update 时重新读取版本再注册 */
static void sync_version_from_fwfile(device_t *d) {
    char file_ver[32] = {0};
    /* 读取固件文件第一行的版本号 */
    read_fw_info(d->fw_path, file_ver, sizeof(file_ver));

    if (file_ver[0] == '\0') {
        dev_log(d, "sync: firmware file has no version line, keep memory version '%s'", d->version);
        return;
    }

    if (strcmp(file_ver, d->version) == 0) {
        dev_log(d, "sync: firmware version '%s' matches memory, no change", d->version);
        return;
    }

    /* 文件版本与内存版本不一致 → 同步到内存 */
    dev_log(d, "sync: version synced from firmware file: '%s' -> '%s'", d->version, file_ver);
    strncpy(d->version, file_ver, sizeof(d->version) - 1);
    d->version[sizeof(d->version) - 1] = '\0';
}

/* ---------- 固件版本管理: 保存历史版本 (使用指定的固件文件) ---------- */
static void save_fw_version_history_with_file(device_t *d, const char *version, const char *fw_file_path) {
    if (d->fw_version_count >= MAX_FW_VERSIONS) {
        /* 如果超过最大数量，移除最早的版本 */
        memmove(d->fw_versions[0], d->fw_versions[1], sizeof(d->fw_versions[0]) * (MAX_FW_VERSIONS - 1));
        memmove(d->fw_version_times[0], d->fw_version_times[1], sizeof(d->fw_version_times[0]) * (MAX_FW_VERSIONS - 1));
        d->fw_version_count = MAX_FW_VERSIONS - 1;
    }

    /* 添加新版本 */
    strncpy(d->fw_versions[d->fw_version_count], version, 31);
    d->fw_versions[d->fw_version_count][31] = '\0';

    /* 记录时间戳 */
    time_t now = time(NULL);
    struct tm *tm = localtime(&now);
    strftime(d->fw_version_times[d->fw_version_count], 32, "%Y-%m-%d_%H-%M-%S", tm);

    /* 保存固件文件到历史目录 (使用指定的文件，而不是 d->fw_path) */
    char hist_path[128];
    snprintf(hist_path, sizeof(hist_path), "%s/firmware_%s_%s.bin",
             d->fw_versions_dir, version, d->fw_version_times[d->fw_version_count]);

    dev_log(d, "save_fw_version_history: src='%s', dst='%s'", fw_file_path, hist_path);

    FILE *src = fopen(fw_file_path, "rb");
    if (src) {
        FILE *dst = fopen(hist_path, "wb");
        if (dst) {
            char buf[256];
            size_t n, total = 0;
            while ((n = fread(buf, 1, sizeof(buf), src)) > 0) {
                fwrite(buf, 1, n, dst);
                total += n;
            }
            fclose(dst);
            dev_log(d, "save_fw_version_history: copied %zu bytes to %s", total, hist_path);
        } else {
            dev_log(d, "save_fw_version_history: failed to create dst file %s", hist_path);
        }
        fclose(src);
    } else {
        dev_log(d, "save_fw_version_history: src file not found %s", fw_file_path);
    }

    d->fw_version_count++;
    dev_log(d, "saved fw version '%s' to history (count=%d)", version, d->fw_version_count);
}

/* ---------- 固件版本管理: 获取版本列表字符串 ---------- */
static int get_fw_version_list(device_t *d, char *buf, size_t buf_size) {
    int offset = 0;

    /* 当前版本信息 */
    offset += snprintf(buf + offset, buf_size - offset,
                       "current=%s,time=now\n", d->version);

    /* 历史版本列表 */
    for (int i = 0; i < d->fw_version_count; i++) {
        offset += snprintf(buf + offset, buf_size - offset,
                           "version=%s,time=%s;",
                           d->fw_versions[i], d->fw_version_times[i]);
        if (offset >= (int)buf_size - 1) break;
    }

    return offset;
}

/* ---------- 固件版本管理: 查找指定版本的固件文件 ---------- */
static int find_fw_version_file(device_t *d, const char *version, char *path_buf, size_t buf_size) {
    dev_log(d, "find_fw_version_file: searching for version='%s', current='%s', history_count=%d",
            version, d->version, d->fw_version_count);

    /* 如果是当前版本 */
    if (strcmp(version, d->version) == 0) {
        strncpy(path_buf, d->fw_path, buf_size - 1);
        path_buf[buf_size - 1] = '\0';
        dev_log(d, "  found in current version: %s", path_buf);
        return 1;
    }

    /* 查找历史版本 */
    for (int i = 0; i < d->fw_version_count; i++) {
        dev_log(d, "  history[%d]: version='%s', time='%s'", i, d->fw_versions[i], d->fw_version_times[i]);
        if (strcmp(version, d->fw_versions[i]) == 0) {
            snprintf(path_buf, buf_size, "%s/firmware_%s_%s.bin",
                     d->fw_versions_dir, version, d->fw_version_times[i]);
            dev_log(d, "  found in history: %s", path_buf);
            return 1;
        }
    }

    dev_log(d, "  NOT FOUND");
    return 0;  /* 未找到 */
}

/* ---------- 日志过滤: 按时间范围获取日志 ---------- */
static size_t get_log_by_time_range(device_t *d, const char *start_time, const char *end_time,
                                     uint8_t *out_buf, size_t buf_size) {
    size_t out_len = 0;
    FILE *lf = fopen(d->log_path, "rb");
    if (!lf) return 0;

    char line[512];
    time_t start_t = 0, end_t = 0;
    int have_start = 0, have_end = 0;

    /* 解析时间参数 */
    if (start_time && strlen(start_time) > 0) {
        struct tm tm_start;
        memset(&tm_start, 0, sizeof(tm_start));
        if (sscanf(start_time, "%d-%d-%d %d:%d:%d",
                   &tm_start.tm_year, &tm_start.tm_mon, &tm_start.tm_mday,
                   &tm_start.tm_hour, &tm_start.tm_min, &tm_start.tm_sec) >= 6) {
            tm_start.tm_year -= 1900;
            start_t = mktime(&tm_start);
            have_start = 1;
        }
    }

    if (end_time && strlen(end_time) > 0) {
        struct tm tm_end;
        memset(&tm_end, 0, sizeof(tm_end));
        if (sscanf(end_time, "%d-%d-%d %d:%d:%d",
                   &tm_end.tm_year, &tm_end.tm_mon, &tm_end.tm_mday,
                   &tm_end.tm_hour, &tm_end.tm_min, &tm_end.tm_sec) >= 6) {
            tm_end.tm_year -= 1900;
            end_t = mktime(&tm_end);
            have_end = 1;
        }
    }

    /* 如果没有时间参数，返回全部日志 */
    if (!have_start && !have_end) {
        fseek(lf, 0, SEEK_END);
        long fsize = ftell(lf);
        fseek(lf, 0, SEEK_SET);
        if (fsize > 0) {
            size_t to_read = (size_t)fsize;
            if (to_read > buf_size) to_read = buf_size;
            out_len = fread(out_buf, 1, to_read, lf);
        }
        fclose(lf);
        return out_len;
    }

    /* 按时间过滤日志 */
    rewind(lf);
    while (fgets(line, sizeof(line), lf)) {
        /* 解析日志行中的时间戳: [YYYY-MM-DD HH:MM:SS] */
        struct tm log_tm;
        memset(&log_tm, 0, sizeof(log_tm));
        if (sscanf(line, "[%d-%d-%d %d:%d:%d]",
                   &log_tm.tm_year, &log_tm.tm_mon, &log_tm.tm_mday,
                   &log_tm.tm_hour, &log_tm.tm_min, &log_tm.tm_sec) >= 6) {
            log_tm.tm_year -= 1900;
            time_t log_time = mktime(&log_tm);

            /* 检查是否在时间范围内 */
            int in_range = 1;
            if (have_start && log_time < start_t) in_range = 0;
            if (have_end && log_time > end_t) in_range = 0;

            if (in_range) {
                size_t line_len = strlen(line);
                if (out_len + line_len < buf_size) {
                    memcpy(out_buf + out_len, line, line_len);
                    out_len += line_len;
                }
            }
        }
    }

    fclose(lf);
    return out_len;
}

/* ---------- 连接追踪辅助函数 ---------- */
//追踪设备连接状态
static void track_connection(device_t *d, const char *ip, uint16_t port, const char *peer_id) {
    EnterCriticalSection(&d->lock);
    int i;
    for (i = 0; i < MAX_CONNECTIONS; i++) {
        if (d->connections[i].active &&
            strcmp(d->connections[i].ip, ip) == 0 &&
            d->connections[i].port == port) {
            d->connections[i].last_seen = time(NULL);
            if (peer_id && peer_id[0]) {
                strncpy(d->connections[i].peer_id, peer_id, sizeof(d->connections[i].peer_id) - 1);
                d->connections[i].peer_id[sizeof(d->connections[i].peer_id) - 1] = '\0';
            }
            LeaveCriticalSection(&d->lock);
            return;
        }
    }
    /* 找空槽 */
    for (i = 0; i < MAX_CONNECTIONS; i++) {
        if (!d->connections[i].active) {
            memset(&d->connections[i], 0, sizeof(conn_entry_t));
            strncpy(d->connections[i].ip, ip, sizeof(d->connections[i].ip) - 1);
            d->connections[i].port = port;
            if (peer_id) strncpy(d->connections[i].peer_id, peer_id, sizeof(d->connections[i].peer_id) - 1);
            d->connections[i].last_seen = time(NULL);
            d->connections[i].active = 1;
            LeaveCriticalSection(&d->lock);
            return;
        }
    }
    LeaveCriticalSection(&d->lock);
}

/* ---------- 对端版本缓存辅助函数 ---------- */
static void cache_peer_version(device_t *d, const char *peer_id,
                               const char *version, const char *history) {
    if (!peer_id || !peer_id[0]) return;
    EnterCriticalSection(&d->lock);
    int i;
    for (i = 0; i < MAX_PEER_VERSIONS; i++) {
        if (d->peer_vers[i].active &&
            strcmp(d->peer_vers[i].peer_id, peer_id) == 0) {
            strncpy(d->peer_vers[i].version, version ? version : "", sizeof(d->peer_vers[i].version) - 1);
            d->peer_vers[i].version[sizeof(d->peer_vers[i].version) - 1] = '\0';
            if (history) {
                strncpy(d->peer_vers[i].history, history, sizeof(d->peer_vers[i].history) - 1);
                d->peer_vers[i].history[sizeof(d->peer_vers[i].history) - 1] = '\0';
            }
            d->peer_vers[i].last_check = time(NULL);
            d->peer_vers[i].active = version && version[0] ? 1 : 0;
            LeaveCriticalSection(&d->lock);
            return;
        }
    }
    for (i = 0; i < MAX_PEER_VERSIONS; i++) {
        if (!d->peer_vers[i].active) {
            memset(&d->peer_vers[i], 0, sizeof(peer_ver_entry_t));
            strncpy(d->peer_vers[i].peer_id, peer_id, sizeof(d->peer_vers[i].peer_id) - 1);
            if (version) strncpy(d->peer_vers[i].version, version, sizeof(d->peer_vers[i].version) - 1);
            if (history) strncpy(d->peer_vers[i].history, history, sizeof(d->peer_vers[i].history) - 1);
            d->peer_vers[i].last_check = time(NULL);
            d->peer_vers[i].active = 1;
            LeaveCriticalSection(&d->lock);
            return;
        }
    }
    LeaveCriticalSection(&d->lock);
}

//获取对端版本
static const char *get_cached_peer_version(device_t *d, const char *peer_id) {
    if (!peer_id) return NULL;
    static __thread char cached_ver[32] = {0};
    cached_ver[0] = '\0';
    EnterCriticalSection(&d->lock);
    for (int i = 0; i < MAX_PEER_VERSIONS; i++) {
        if (d->peer_vers[i].active &&
            strcmp(d->peer_vers[i].peer_id, peer_id) == 0) {
            strncpy(cached_ver, d->peer_vers[i].version, sizeof(cached_ver) - 1);
            break;
        }
    }
    LeaveCriticalSection(&d->lock);
    return cached_ver[0] ? cached_ver : NULL;
}

/* ---------- 从固件历史中提取最近 N 个版本号 ---------- */
static int get_history_versions(device_t *d, char out[][32], int max_count) {
    int count = 0;
    /* fw_versions[0] 是最新版本, 向后取历史 */
    for (int i = 0; i < d->fw_version_count && count < max_count; i++) {
        if (d->fw_versions[i][0]) {
            strncpy(out[count], d->fw_versions[i], 31);
            out[count][31] = '\0';
            count++;
        }
    }
    return count;
}

/* ---------- 构建带历史版本的 link-format ---------- */
static int build_links_with_history(device_t *d, char *buf, size_t buf_size) {
    char history_str[256] = {0};
    char hist[MAX_HISTORY_VERSIONS][32];
    int hcount = get_history_versions(d, hist, MAX_HISTORY_VERSIONS);
    
    int off = 0;
    if (hcount > 0) {
        off += snprintf(history_str + off, sizeof(history_str) - (size_t)off,
                        ";hver=\"");
        for (int i = 0; i < hcount; i++) {
            if (i > 0) off += snprintf(history_str + off, sizeof(history_str) - (size_t)off, ",");
            off += snprintf(history_str + off, sizeof(history_str) - (size_t)off,
                            "%s", hist[i]);
        }
        snprintf(history_str + off, sizeof(history_str) - (size_t)off, "\"");
    }
    
    off = snprintf(buf, buf_size,
                   "</fwinfo>;rt=\"version\";ver=\"%s\"%s,</log>;rt=\"log\",</firmware>;rt=\"fw\"",
                   d->version, history_str);
    return off;
}

/* ---------- 前向声明 ---------- */
// 客户端 RD 注册
static int client_rd_register(device_t *d);

/* ---------- 服务端升级自身固件（从对端拉取后应用） ---------- */

/* 版本号累加: 1.0.0-A → 1.0.1-A → ... → 1.0.9-A → 1.1.0-A → ... → 1.9.9-A → 2.0.0-A
 * 格式: major.minor.patch-suffix
 * 累加 patch, patch>9 则进位到 minor, minor>9 则进位到 major */
static void increment_version(char *version, size_t buf_size) {
    int major = 1, minor = 0, patch = 0;
    char suffix[16] = {0};

    /* 解析 "major.minor.patch-suffix" 格式 */
    if (sscanf(version, "%d.%d.%d-%15s", &major, &minor, &patch, suffix) < 3) {
        /* 解析失败, 直接追加 "-v2" */
        snprintf(version, buf_size, "%s-v2", version);
        return;
    }

    /* patch 累加, 满十进位 */
    patch++;
    if (patch > 9) {
        patch = 0;
        minor++;
        if (minor > 9) {
            minor = 0;
            major++;
        }
    }

    snprintf(version, buf_size, "%d.%d.%d-%s", major, minor, patch, suffix);
}

// 应用固件更新
static int apply_firmware_update(device_t *d, const uint8_t *fw_data, size_t fw_len) {
    if (!fw_data || fw_len == 0) return -1;

    /* 保存旧版本到历史 */
    save_fw_version_history_with_file(d, d->version, d->fw_path);

    /* 写入新固件 */
    FILE *fw = fopen(d->fw_path, "wb");
    if (!fw) {
        dev_log(d, "apply: failed to write firmware file");
        return -1;
    }
    fwrite(fw_data, 1, fw_len, fw);
    fclose(fw);
    dev_log(d, "apply: firmware saved (%zu bytes)", fw_len);

    /* 版本号累加 (不从固件文件读取, 自身版本+1) */
    char old_ver[32];
    strncpy(old_ver, d->version, sizeof(old_ver) - 1);
    old_ver[sizeof(old_ver) - 1] = '\0';

    increment_version(d->version, sizeof(d->version));
    dev_log(d, "apply: version incremented %s -> %s", old_ver, d->version);

    /* 将新版本号写入固件文件首行 */
    fw = fopen(d->fw_path, "r+b");
    if (fw) {
        /* 读取原固件内容 (跳过首行版本号) */
        char line[64];
        if (fgets(line, sizeof(line), fw)) {
            long body_offset = ftell(fw);
            /* 读取剩余内容 */
            fseek(fw, 0, SEEK_END);
            long total = ftell(fw);
            long body_len = total - body_offset;
            uint8_t *body = (uint8_t *)malloc(body_len > 0 ? (size_t)body_len : 0);
            if (body && body_len > 0) {
                fseek(fw, body_offset, SEEK_SET);
                fread(body, 1, (size_t)body_len, fw);
            }
            /* 重写文件: 新版本号 + 原始body */
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

    /* 重新注册 RD */
    if (d->rd_enabled && d->rd_registered) {
        dev_log(d, "apply: re-registering with RD...");
        client_rd_register(d);
    }
    return 0;
}

/* ====================================================================
 * 服务器线程
 * ==================================================================== */
static DWORD WINAPI server_thread(LPVOID arg) {
    device_t *d = (device_t *)arg;
    uint8_t   rbuf[COAP_MAX_MSG];  //接收缓冲区
    uint8_t   sbuf[COAP_MAX_MSG];  //发送缓冲区

    while (d->running) {  // 主循环, 监听 CoAP 请求
        char     from_ip[64];
        uint16_t from_port;
        int n = coap_recv(d->srv_sock, rbuf, sizeof(rbuf), from_ip, &from_port, 500);
        if (n <= 0) continue;

        /* 调试: 打印原始报文 */
        dev_log(d, "server: RECV %d bytes raw from %s:%u:", n, from_ip, from_port);
        for (int i = 0; i < n && i < 32; i++) {
            printf("%02X ", rbuf[i]);
            if ((i + 1) % 16 == 0) printf("\n");
        }
        if (n > 32) printf("...");
        printf("\n");
        fflush(stdout);

        coap_msg_t req;
        if (coap_parse(rbuf, (size_t)n, &req) < 0) {
            dev_log(d, "server: failed to parse request (n=%d)", n);
            continue;
        }

        dev_log(d, "server: RECV %d bytes from %s:%u, parsed: type=%d, code=%02x, msg_id=%u, uri_path='%s', uri_query='%s' (len=%zu)",
                n, from_ip, from_port, req.type, req.code, req.msg_id, req.uri_path, req.uri_query, strlen(req.uri_query));

        /* 从 uri_query 中提取 from=<id> 参数, 用于识别连接来源 */
        char from_id[16] = {0};
        char *from_tag = strstr(req.uri_query, "from=");
        if (from_tag) {
            char *val = from_tag + 5;
            size_t vlen = 0;
            while (val[vlen] && val[vlen] != '&' && vlen < sizeof(from_id) - 1)
                vlen++;
            memcpy(from_id, val, vlen);
            from_id[vlen] = '\0';
            /* 从 uri_query 中移除 from=xxx 部分 */
            char *amp = strchr(from_tag, '&');
            if (amp) {
                memmove(from_tag, amp + 1, strlen(amp + 1) + 1);
            } else {
                /* from=xxx 在末尾, 去掉前面的 & */
                if (from_tag > req.uri_query && from_tag[-1] == '&')
                    from_tag[-1] = '\0';
                else
                    from_tag[0] = '\0';
            }
        }

        /* 追踪连接 (用于 get_link_id 命令) */
        track_connection(d, from_ip, from_port, from_id[0] ? from_id : NULL);

        /* 协议日志: 记录接收到的请求 */
        proto_log(d, "RECV (server <- client)", &req, rbuf, (size_t)n);

        /* 准备响应: ACK 回 CON, 同 msg_id, 回显 token */
        coap_msg_t resp;
        memset(&resp, 0, sizeof(resp));
        resp.type      = (req.type == COAP_CON) ? COAP_ACK : COAP_NON;
        resp.msg_id    = req.msg_id;
        resp.token_len = req.token_len;
        memcpy(resp.token, req.token, req.token_len);

        /* 栈上缓冲, 用于 payload 指向 */
        char     file_buf[COAP_MAX_MSG - 20];   // 留空间给 CoAP 头
        char     info[96];
        int      info_len = 0;
        uint8_t *payload_p = NULL;
        size_t   payload_n = 0;

        // 处理 /fwinfo 请求,收集当前固件版本和文件大小
        if (strcmp(req.uri_path, "fwinfo") == 0 && req.code == COAP_GET) {
            dev_log(d, "server: fwinfo query='%s', list_cmp=%d, version_cmp=%d",
                    req.uri_query, strcmp(req.uri_query, "list"), strncmp(req.uri_query, "version=", 8));
            /* 检查是否请求版本列表 */
            if (strcmp(req.uri_query, "list") == 0) {
                /* 返回版本列表 - 使用函数级的 file_buf 避免局部变量作用域问题 */
                int list_len = get_fw_version_list(d, (char *)file_buf, sizeof(file_buf));
                resp.code           = COAP_CONTENT;
                resp.content_format = FMT_TEXT_PLAIN;
                payload_p = (uint8_t *)file_buf;
                payload_n = (size_t)list_len;
                dev_log(d, "server: <- GET /fwinfo?list from %s:%u ; reply version list",
                        from_ip, from_port);
            } else if (strncmp(req.uri_query, "version=", 8) == 0) {
                /* 按版本获取固件文件 (Block2 分块传输) */
                char *ver = req.uri_query + 8;
                dev_log(d, "server: searching for version '%s' (uri_query='%s')", ver, req.uri_query);
                char fw_file_path[128];
                if (find_fw_version_file(d, ver, fw_file_path, sizeof(fw_file_path))) {
                    dev_log(d, "server: trying to open file '%s'", fw_file_path);
                    FILE *fwf = fopen(fw_file_path, "rb");
                    if (fwf) {
                        fseek(fwf, 0, SEEK_END);
                        long fsize = ftell(fwf);
                        fseek(fwf, 0, SEEK_SET);
                        dev_log(d, "server: file size=%ld", fsize);
                        if (fsize > 0) {
                            size_t fw_size = (size_t)fsize;
                            int block_no = req.has_block2 ? req.block2_num : 0;
                            size_t offset = (size_t)block_no * BLOCK_SIZE;
                            if (offset >= fw_size) {
                                resp.code = COAP_REQUEST_ENTITY_INCOMPLETE;
                                dev_log(d, "server: <- GET /fwinfo?version=%s Block2 num=%d ; beyond EOF",
                                        ver, block_no);
                            } else {
                                size_t remaining = fw_size - offset;
                                size_t chunk = remaining < BLOCK_SIZE ? remaining : BLOCK_SIZE;
                                int more = (offset + chunk < fw_size) ? 1 : 0;
                                fseek(fwf, (long)offset, SEEK_SET);
                                uint8_t block_buf[BLOCK_SIZE];
                                size_t rd = fread(block_buf, 1, chunk, fwf);
                                resp.code           = COAP_CONTENT;
                                resp.content_format = FMT_OCTET_STREAM;
                                resp.has_block2     = 1;
                                resp.block2_num     = block_no;
                                resp.block2_more    = more;
                                resp.block2_szx     = BLOCK_SZX;
                                payload_p           = block_buf;
                                payload_n           = rd;
                                dev_log(d, "server: <- GET /fwinfo?version=%s Block2 num=%d (%zu bytes, M=%d)",
                                        ver, block_no, rd, more);
                            }
                        } else {
                            resp.code = COAP_NOT_FOUND;
                        }
                        fclose(fwf);
                    } else {
                        resp.code = COAP_NOT_FOUND;
                        dev_log(d, "server: <- GET /fwinfo?version=%s ; file not found '%s' (errno=%d)",
                                ver, fw_file_path, errno);
                    }
                } else {
                    resp.code = COAP_NOT_FOUND;
                    dev_log(d, "server: <- GET /fwinfo?version=%s ; version not found",
                            ver, from_ip, from_port);
                }
            } else {
                /* 默认: 返回当前固件信息 */
                char ver_buf[32] = {0};
                size_t fw_size = read_fw_info(d->fw_path, ver_buf, sizeof(ver_buf));
                info_len = snprintf(info, sizeof(info), "version=%s,size=%zu",
                                    ver_buf[0] ? ver_buf : d->version, fw_size);
                resp.code           = COAP_CONTENT;
                resp.content_format = FMT_TEXT_PLAIN;
                payload_p = (uint8_t *)info;
                payload_n = (size_t)info_len;
                dev_log(d, "server: <- GET /fwinfo from %s:%u ; reply %s",
                        from_ip, from_port, info);
            }
        
        // 处理 /log 请求,收集当前日志文件内容
        } else if (strcmp(req.uri_path, "log") == 0 && req.code == COAP_GET) {
            size_t log_len = 0;
            char *start_time = NULL, *end_time = NULL;
            char start_buf[32] = {0}, end_buf[32] = {0};

            /* 解析时间参数: start_time=xxx&end_time=xxx */
            if (req.uri_query[0]) {
                char query_copy[128];
                strncpy(query_copy, req.uri_query, sizeof(query_copy) - 1);
                query_copy[sizeof(query_copy) - 1] = '\0';

                char *token = strtok(query_copy, "&");
                while (token) {
                    if (strncmp(token, "start_time=", 11) == 0) {
                        start_time = start_buf;
                        strncpy(start_buf, token + 11, sizeof(start_buf) - 1);
                    } else if (strncmp(token, "end_time=", 9) == 0) {
                        end_time = end_buf;
                        strncpy(end_buf, token + 9, sizeof(end_buf) - 1);
                    }
                    token = strtok(NULL, "&");
                }
            }

            EnterCriticalSection(&d->lock);
            if (start_time || end_time) {
                /* 按时间范围获取日志 */
                log_len = get_log_by_time_range(d, start_time, end_time,
                                                 (uint8_t *)file_buf, sizeof(file_buf));
            } else {
                /* 获取全部日志 */
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
            }
            LeaveCriticalSection(&d->lock);

            resp.code           = COAP_CONTENT;
            resp.content_format = FMT_TEXT_PLAIN;
            payload_p = (uint8_t *)file_buf;
            payload_n = log_len;
            if (start_time || end_time) {
                dev_log(d, "server: <- GET /log?start=%s&end=%s from %s:%u ; reply %zu bytes",
                        start_time ? start_time : "begin", end_time ? end_time : "now",
                        from_ip, from_port, log_len);
            } else {
                dev_log(d, "server: <- GET /log from %s:%u ; reply %zu bytes",
                        from_ip, from_port, log_len);
            }
        
        // 处理 /firmware 请求,写入新固件文件
        } else if (strcmp(req.uri_path, "firmware") == 0 && req.code == COAP_PUT) {
            int done = 0;
            char newver[32] = {0};
            size_t total = 0;
            EnterCriticalSection(&d->lock);

            /* 保存旧固件文件副本 (仅在第一块时执行!) */
            if (req.has_block1 && req.block1_num == 0) {
                snprintf(d->old_fw_copy_path, sizeof(d->old_fw_copy_path), "%s/old_fw_tmp.bin", d->fw_versions_dir);
                dev_log(d, "server: fw_path='%s', fw_versions_dir='%s', old_fw_copy='%s'",
                        d->fw_path, d->fw_versions_dir, d->old_fw_copy_path);
                FILE *old_src = fopen(d->fw_path, "rb");
                if (old_src) {
                    FILE *old_dst = fopen(d->old_fw_copy_path, "wb");
                    if (old_dst) {
                        char buf[256];
                        size_t n;
                        while ((n = fread(buf, 1, sizeof(buf), old_src)) > 0) {
                            fwrite(buf, 1, n, old_dst);
                        }
                        fclose(old_dst);
                        dev_log(d, "server: saved old fw copy to %s", d->old_fw_copy_path);
                    } else {
                        dev_log(d, "server: failed to create old fw copy '%s' (errno=%d)", d->old_fw_copy_path, errno);
                        d->old_fw_copy_path[0] = '\0';
                    }
                    fclose(old_src);
                } else {
                    dev_log(d, "server: old fw file not exist, no copy needed");
                    d->old_fw_copy_path[0] = '\0';  /* 标记为无旧固件 */
                }
            }

            /* 写入固件文件: 首块截断, 其余追加 */
            FILE *fw;
            if (req.has_block1 && req.block1_num == 0)
                fw = fopen(d->fw_path, "wb");
            else
                fw = fopen(d->fw_path, "ab");

            if (!fw) {
                LeaveCriticalSection(&d->lock);
                dev_log(d, "server: <- PUT /firmware open file failed");
                resp.code = COAP_INTERNAL_ERROR;
                goto send_resp;
            }
            fwrite(req.payload, 1, req.payload_len, fw);
            fclose(fw);

            /* 最后一块: 版本号累加 (不从固件文件读取) */
            if (req.has_block1 && !req.block1_more) {
                /* 保存当前版本到历史 (使用旧固件副本) */
                if (d->old_fw_copy_path[0] != '\0') {
                    dev_log(d, "server: saving old version '%s' to history from %s", d->version, d->old_fw_copy_path);
                    save_fw_version_history_with_file(d, d->version, d->old_fw_copy_path);
                } else {
                    dev_log(d, "server: no old fw copy, skip saving history for '%s'", d->version);
                }
                /* 版本号累加 */
                char old_ver[32];
                strncpy(old_ver, d->version, sizeof(old_ver) - 1);
                old_ver[sizeof(old_ver) - 1] = '\0';
                increment_version(d->version, sizeof(d->version));
                strncpy(newver, d->version, sizeof(newver) - 1);
                newver[sizeof(newver) - 1] = '\0';
                dev_log(d, "server: version incremented %s -> %s", old_ver, d->version);

                /* 将新版本号写入固件文件首行 */
                fw = fopen(d->fw_path, "r+b");
                if (fw) {
                    char line[64];
                    if (fgets(line, sizeof(line), fw)) {
                        long body_offset = ftell(fw);
                        fseek(fw, 0, SEEK_END);
                        long total = ftell(fw);
                        long body_len = total - body_offset;
                        uint8_t *body = (uint8_t *)malloc(body_len > 0 ? (size_t)body_len : 0);
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
                done = 1;
            }

            /* 获取固件文件总大小 */
            fw = fopen(d->fw_path, "rb");
            if (fw) {
                fseek(fw, 0, SEEK_END);
                total = (size_t)ftell(fw);
                fclose(fw);
            }

            LeaveCriticalSection(&d->lock);

            resp.code = COAP_CHANGED;
            //记录当前块信息
            if (req.has_block1) {
                resp.has_block1   = 1;
                resp.block1_num   = req.block1_num;
                resp.block1_more  = req.block1_more;
                resp.block1_szx   = req.block1_szx;
            }
            if (done) {
                
                dev_log(d, "server: <- PUT /firmware block %d (last, %zu bytes) from %s:%u ; "
                           "upgrade complete: total=%zu, version=%s",
                        req.block1_num, req.payload_len, from_ip, from_port, total, newver);
            } else {
                dev_log(d, "server: <- PUT /firmware block %d (%zu bytes) from %s:%u",
                        req.block1_num, req.payload_len, from_ip, from_port);
            }
        // 处理 GET /firmware 请求 (Block2 分块响应, 客户端拉取固件)
        } else if (strcmp(req.uri_path, "firmware") == 0 && req.code == COAP_GET) {
            FILE *fw = fopen(d->fw_path, "rb");
            if (!fw) {
                resp.code = COAP_NOT_FOUND;
                dev_log(d, "server: <- GET /firmware from %s:%u ; file not found", from_ip, from_port);
            } else {
                fseek(fw, 0, SEEK_END);
                long fsize = ftell(fw);
                fseek(fw, 0, SEEK_SET);

                if (fsize <= 0) {
                    resp.code = COAP_NOT_FOUND;
                    fclose(fw);
                } else {
                    size_t fw_size = (size_t)fsize;
                    int block_no = req.has_block2 ? req.block2_num : 0;
                    size_t offset = (size_t)block_no * BLOCK_SIZE;

                    if (offset >= fw_size) {
                        resp.code = COAP_REQUEST_ENTITY_INCOMPLETE;
                        dev_log(d, "server: <- GET /firmware Block2 num=%d from %s:%u ; beyond EOF",
                                block_no, from_ip, from_port);
                    } else {
                        size_t remaining = fw_size - offset;
                        size_t chunk = remaining < BLOCK_SIZE ? remaining : BLOCK_SIZE;
                        int more = (offset + chunk < fw_size) ? 1 : 0;

                        fseek(fw, (long)offset, SEEK_SET);
                        uint8_t block_buf[BLOCK_SIZE];
                        size_t rd = fread(block_buf, 1, chunk, fw);
                        resp.code           = COAP_CONTENT;
                        resp.content_format = FMT_OCTET_STREAM;
                        resp.has_block2     = 1;
                        resp.block2_num     = block_no;
                        resp.block2_more    = more;
                        resp.block2_szx     = BLOCK_SZX;
                        payload_p           = block_buf;
                        payload_n           = rd;
                        dev_log(d, "server: <- GET /firmware Block2 num=%d (%zu bytes, M=%d) from %s:%u",
                                block_no, rd, more, from_ip, from_port);
                    }
                    fclose(fw);
                }
            }

        // 处理 /.well-known/core 请求, 返回 CoRE Link Format 资源列表
        } else if (strcmp(req.uri_path, "well-known/core") == 0 && req.code == COAP_GET) {
            char core_buf[256];
            int core_len = snprintf(core_buf, sizeof(core_buf),
                "</fwinfo>;rt=\"version\",</log>;rt=\"log\","
                "</firmware>;rt=\"fw\",</.well-known/core>;rt=\"core\"");
            resp.code           = COAP_CONTENT;
            resp.content_format = FMT_LINK_FORMAT;
            payload_p = (uint8_t *)core_buf;
            payload_n = (size_t)core_len;
            dev_log(d, "server: <- GET /.well-known/core from %s:%u ; reply resource list",
                    from_ip, from_port);

        } else {
            resp.code      = (req.code == COAP_GET || req.code == COAP_PUT) ? COAP_NOT_FOUND
                                                                           : COAP_METHOD_NOT_ALLOWED;
            const char *msg = "resource not found";
            payload_p = (uint8_t *)msg;
            payload_n = strlen(msg);
            dev_log(d, "server: <- %s /%s from %s:%u ; reply %s",
                    coap_method_name(req.code), req.uri_path, from_ip, from_port,
                    coap_response_name(resp.code));
        }

        resp.payload     = payload_p;
        resp.payload_len = payload_n;

send_resp:
        int slen = coap_build(sbuf, sizeof(sbuf), &resp);
        if (slen > 0) {
            /* 协议日志: 记录发送的响应 */
            proto_log(d, "SEND (server -> client)", &resp, sbuf, (size_t)slen);
            int sent = coap_send(d->srv_sock, from_ip, from_port, sbuf, (size_t)slen);
            if (sent <= 0) {
                dev_log(d, "server: send response FAILED (slen=%d)", slen);
            } else {
                dev_log(d, "server: response sent to %s:%u (slen=%d, msg_id=%u, payload_len=%zu)",
                        from_ip, from_port, slen, resp.msg_id, resp.payload_len);
            }
        } else {
            dev_log(d, "server: coap_build FAILED for response (payload_len=%zu, payload_p=%p)",
                    resp.payload_len, (void *)resp.payload);
        }
    }
    return 0;
}

/* 前向声明: coap_exchange 定义在后面 */
static int coap_exchange(device_t *d, coap_msg_t *req, coap_msg_t *resp);

/* ====================================================================
 * RD (Resource Directory) 客户端操作
 *
 * 设备启动时向 RD 注册自己的资源, 访问对端前先向 RD 查询对端地址。
 * 若 RD 未启用或查询失败, 回退到静态配置的 peer_ip/peer_port。
 * ==================================================================== */

/* 向 RD 服务器注册本设备资源
 * POST /rd?ep=<id>&base=coap://<ip>:<port>&d=<domain>&et=<ttl>
 * Payload (link-format): </fwinfo>;rt="version",</log>;rt="log",</firmware>;rt="fw"
 */
static int client_rd_register(device_t *d) {
    if (!d->rd_enabled) {
        dev_log(d, "RD: disabled, skip registration");
        return -1;
    }

    /* 注册前先从固件文件同步版本号到内存 (支持手动改固件文件后重新注册) */
    sync_version_from_fwfile(d);

    coap_msg_t req, resp;
    memset(&req, 0, sizeof(req));
    req.code = COAP_POST;
    strncpy(req.uri_path, "rd", sizeof(req.uri_path) - 1);

    /* 构造查询参数: ep=, base=, d=, et= */
    char base_uri[64];
    snprintf(base_uri, sizeof(base_uri), "coap://127.0.0.1:%u", d->port);
    snprintf(req.uri_query, sizeof(req.uri_query),
             "ep=%s&base=%s&d=local&et=3600", d->id, base_uri);

    /* 构造 link-format 负载: 本设备暴露的资源列表 (含版本号和历史版本) */
    char links[512];
    int links_len = build_links_with_history(d, links, sizeof(links));
    req.payload = (const uint8_t *)links;
    req.payload_len = (size_t)links_len;
    req.content_format = FMT_LINK_FORMAT;

    dev_log(d, "RD: -> POST /rd?%s to %s:%u", req.uri_query, d->rd_ip, d->rd_port);

    /* 临时切换对端地址为 RD 服务器, 发送请求 */
    char saved_peer_ip[64];
    uint16_t saved_peer_port;
    strncpy(saved_peer_ip, d->peer_ip, sizeof(saved_peer_ip) - 1);
    saved_peer_ip[sizeof(saved_peer_ip) - 1] = '\0';
    saved_peer_port = d->peer_port;

    strncpy(d->peer_ip, d->rd_ip, sizeof(d->peer_ip) - 1);
    d->peer_ip[sizeof(d->peer_ip) - 1] = '\0';
    d->peer_port = d->rd_port;

    int rc = coap_exchange(d, &req, &resp);

    /* 恢复原对端地址 */
    strncpy(d->peer_ip, saved_peer_ip, sizeof(d->peer_ip) - 1);
    d->peer_ip[sizeof(d->peer_ip) - 1] = '\0';
    d->peer_port = saved_peer_port;

    if (rc == 0) {
        dev_log(d, "RD: <- %s (registration %s)",
                coap_response_name(resp.code),
                (resp.code == COAP_CREATED || resp.code == COAP_CHANGED) ? "SUCCESS" : "FAILED");
        d->rd_registered = (resp.code == COAP_CREATED || resp.code == COAP_CHANGED) ? 1 : 0;
        return d->rd_registered ? 0 : -1;
    } else {
        dev_log(d, "RD: registration failed (no response from RD)");
        d->rd_registered = 0;
        return -1;
    }
}

/* 向 RD 更新注册 (续期) */
static int client_rd_update(device_t *d) {
    if (!d->rd_enabled || !d->rd_registered) {
        dev_log(d, "RD: not registered, skip update");
        return -1;
    }

    /* 更新注册前也从固件文件同步版本号 */
    sync_version_from_fwfile(d);

    coap_msg_t req, resp;
    memset(&req, 0, sizeof(req));
    req.code = COAP_PUT;
    snprintf(req.uri_path, sizeof(req.uri_path), "rd/%s", d->id);
    snprintf(req.uri_query, sizeof(req.uri_query), "et=3600");

    /* 更新时也带上最新版本号和历史版本 */
    char links[512];
    int links_len = build_links_with_history(d, links, sizeof(links));
    req.payload = (const uint8_t *)links;
    req.payload_len = (size_t)links_len;
    req.content_format = FMT_LINK_FORMAT;

    dev_log(d, "RD: -> PUT /%s?%s to %s:%u",
            req.uri_path, req.uri_query, d->rd_ip, d->rd_port);

    char saved_peer_ip[64];
    uint16_t saved_peer_port;
    strncpy(saved_peer_ip, d->peer_ip, sizeof(saved_peer_ip) - 1);
    saved_peer_ip[sizeof(saved_peer_ip) - 1] = '\0';
    saved_peer_port = d->peer_port;

    strncpy(d->peer_ip, d->rd_ip, sizeof(d->peer_ip) - 1);
    d->peer_ip[sizeof(d->peer_ip) - 1] = '\0';
    d->peer_port = d->rd_port;

    int rc = coap_exchange(d, &req, &resp);

    strncpy(d->peer_ip, saved_peer_ip, sizeof(d->peer_ip) - 1);
    d->peer_ip[sizeof(d->peer_ip) - 1] = '\0';
    d->peer_port = saved_peer_port;

    if (rc == 0 && resp.code == COAP_CHANGED) {
        dev_log(d, "RD: update SUCCESS");
        return 0;
    } else {
        dev_log(d, "RD: update failed");
        return -1;
    }
}

/* 从 RD 注销 */
static int client_rd_deregister(device_t *d) {
    if (!d->rd_enabled || !d->rd_registered) return 0;

    coap_msg_t req, resp;
    memset(&req, 0, sizeof(req));
    req.code = COAP_DELETE;
    snprintf(req.uri_path, sizeof(req.uri_path), "rd/%s", d->id);

    dev_log(d, "RD: -> DELETE /%s to %s:%u",
            req.uri_path, d->rd_ip, d->rd_port);

    char saved_peer_ip[64];
    uint16_t saved_peer_port;
    strncpy(saved_peer_ip, d->peer_ip, sizeof(saved_peer_ip) - 1);
    saved_peer_ip[sizeof(saved_peer_ip) - 1] = '\0';
    saved_peer_port = d->peer_port;

    strncpy(d->peer_ip, d->rd_ip, sizeof(d->peer_ip) - 1);
    d->peer_ip[sizeof(d->peer_ip) - 1] = '\0';
    d->peer_port = d->rd_port;

    int rc = coap_exchange(d, &req, &resp);

    strncpy(d->peer_ip, saved_peer_ip, sizeof(d->peer_ip) - 1);
    d->peer_ip[sizeof(d->peer_ip) - 1] = '\0';
    d->peer_port = saved_peer_port;

    if (rc == 0 && (resp.code == COAP_DELETED || resp.code == COAP_NOT_FOUND)) {
        dev_log(d, "RD: deregister SUCCESS");
        d->rd_registered = 0;
        return 0;
    } else {
        dev_log(d, "RD: deregister failed");
        return -1;
    }
}

/* 从 RD 查询指定端点的地址
 * GET /rd?ep=<peer_id>
 * 返回 link-format: </coap://ip:port/fwinfo>;rt="version",...
 * 解析出 base URI 后, 更新 d->peer_ip / d->peer_port
 * 成功返回 0, 失败返回 -1 (调用方可回退到静态配置)
 */
static int client_rd_lookup(device_t *d, const char *peer_id) {
    if (!d->rd_enabled) {
        dev_log(d, "RD: disabled, use static peer %s:%u", d->peer_ip, d->peer_port);
        return -1;
    }

    coap_msg_t req, resp;
    memset(&req, 0, sizeof(req));
    req.code = COAP_GET;
    strncpy(req.uri_path, "rd", sizeof(req.uri_path) - 1);
    snprintf(req.uri_query, sizeof(req.uri_query), "ep=%s", peer_id);

    dev_log(d, "RD: -> GET /rd?%s to %s:%u",
            req.uri_query, d->rd_ip, d->rd_port);

    char saved_peer_ip[64];
    uint16_t saved_peer_port;
    strncpy(saved_peer_ip, d->peer_ip, sizeof(saved_peer_ip) - 1);
    saved_peer_ip[sizeof(saved_peer_ip) - 1] = '\0';
    saved_peer_port = d->peer_port;

    strncpy(d->peer_ip, d->rd_ip, sizeof(d->peer_ip) - 1);
    d->peer_ip[sizeof(d->peer_ip) - 1] = '\0';
    d->peer_port = d->rd_port;

    int rc = coap_exchange(d, &req, &resp);

    strncpy(d->peer_ip, saved_peer_ip, sizeof(d->peer_ip) - 1);
    d->peer_ip[sizeof(d->peer_ip) - 1] = '\0';
    d->peer_port = saved_peer_port;

    if (rc != 0) {
        dev_log(d, "RD: lookup failed (no response), fallback to static %s:%u",
                d->peer_ip, d->peer_port);
        return -1;
    }

    if (resp.code != COAP_CONTENT || resp.payload_len == 0) {
        dev_log(d, "RD: lookup returned %s (peer '%s' not registered), fallback to static",
                coap_response_name(resp.code), peer_id);
        return -1;
    }

    /* 解析 link-format 提取 base URI
     * 格式: </coap://127.0.0.1:5683/fwinfo>;rt="version",...
     * 提取 coap://host:port 部分 */
    char payload_buf[512];
    size_t cpy = resp.payload_len < sizeof(payload_buf) - 1
                 ? resp.payload_len : sizeof(payload_buf) - 1;
    memcpy(payload_buf, resp.payload, cpy);
    payload_buf[cpy] = '\0';

    dev_log(d, "RD: lookup response: %s", payload_buf);

    char *p = strstr(payload_buf, "coap://");
    if (!p) {
        dev_log(d, "RD: no coap:// URI found in lookup response");
        return -1;
    }

    p += 7; /* 跳过 "coap://" */
    char *slash = strchr(p, '/');
    char *gt = strchr(p, '>');
    char *end = slash ? slash : gt;
    if (!end) {
        dev_log(d, "RD: malformed URI in lookup response");
        return -1;
    }

    char host_port[80];
    size_t hp_len = (size_t)(end - p);
    if (hp_len >= sizeof(host_port)) hp_len = sizeof(host_port) - 1;
    memcpy(host_port, p, hp_len);
    host_port[hp_len] = '\0';

    /* 分离 host 和 port */
    char *colon = strrchr(host_port, ':');
    if (!colon) {
        dev_log(d, "RD: no port in URI '%s'", host_port);
        return -1;
    }

    *colon = '\0';
    uint16_t port = (uint16_t)atoi(colon + 1);

    /* 更新对端地址 */
    strncpy(d->peer_ip, host_port, sizeof(d->peer_ip) - 1);
    d->peer_ip[sizeof(d->peer_ip) - 1] = '\0';
    d->peer_port = port;

    /* 解析 ver 属性获取对端版本号 */
    char resolved_ver[32] = {0};
    char *ver_start = strstr(payload_buf, "ver=\"");
    if (ver_start) {
        ver_start += 5;
        char *ver_end = strchr(ver_start, '"');
        if (ver_end && ver_end > ver_start) {
            size_t ver_len = (size_t)(ver_end - ver_start);
            if (ver_len >= sizeof(resolved_ver)) ver_len = sizeof(resolved_ver) - 1;
            memcpy(resolved_ver, ver_start, ver_len);
            resolved_ver[ver_len] = '\0';
            dev_log(d, "RD: peer '%s' version = %s", peer_id, resolved_ver);
        }
    }

    /* 解析 hver 属性获取历史版本列表 */
    char resolved_hver[128] = {0};
    char *hver_start = strstr(payload_buf, "hver=\"");
    if (hver_start) {
        hver_start += 6;
        char *hver_end = strchr(hver_start, '"');
        if (hver_end && hver_end > hver_start) {
            size_t hver_len = (size_t)(hver_end - hver_start);
            if (hver_len >= sizeof(resolved_hver)) hver_len = sizeof(resolved_hver) - 1;
            memcpy(resolved_hver, hver_start, hver_len);
            resolved_hver[hver_len] = '\0';
            dev_log(d, "RD: peer '%s' history versions = %s", peer_id, resolved_hver);
        }
    }

    /* 缓存对端版本和历史版本 */
    cache_peer_version(d, peer_id, resolved_ver, resolved_hver);

    dev_log(d, "RD: resolved peer '%s' -> %s:%u (version=%s)",
            peer_id, d->peer_ip, d->peer_port,
            resolved_ver[0] ? resolved_ver : "unknown");
    return 0;
}

/* 从 RD 查询所有提供指定资源类型的端点 */
static int client_rd_lookup_by_resource(device_t *d, const char *rt) {
    if (!d->rd_enabled) {
        dev_log(d, "RD: disabled, cannot lookup by resource");
        return -1;
    }

    coap_msg_t req, resp;
    memset(&req, 0, sizeof(req));
    req.code = COAP_GET;
    strncpy(req.uri_path, "rd", sizeof(req.uri_path) - 1);
    snprintf(req.uri_query, sizeof(req.uri_query), "res=%s", rt);

    dev_log(d, "RD: -> GET /rd?%s to %s:%u",
            req.uri_query, d->rd_ip, d->rd_port);

    char saved_peer_ip[64];
    uint16_t saved_peer_port;
    strncpy(saved_peer_ip, d->peer_ip, sizeof(saved_peer_ip) - 1);
    saved_peer_ip[sizeof(saved_peer_ip) - 1] = '\0';
    saved_peer_port = d->peer_port;

    strncpy(d->peer_ip, d->rd_ip, sizeof(d->peer_ip) - 1);
    d->peer_ip[sizeof(d->peer_ip) - 1] = '\0';
    d->peer_port = d->rd_port;

    int rc = coap_exchange(d, &req, &resp);

    strncpy(d->peer_ip, saved_peer_ip, sizeof(d->peer_ip) - 1);
    d->peer_ip[sizeof(d->peer_ip) - 1] = '\0';
    d->peer_port = saved_peer_port;

    if (rc != 0) {
        dev_log(d, "RD: resource lookup failed (no response)");
        return -1;
    }

    if (resp.payload_len > 0) {
        char payload_buf[512];
        size_t cpy = resp.payload_len < sizeof(payload_buf) - 1
                     ? resp.payload_len : sizeof(payload_buf) - 1;
        memcpy(payload_buf, resp.payload, cpy);
        payload_buf[cpy] = '\0';
        dev_log(d, "RD: resources with rt='%s': %s", rt, payload_buf);
        printf("[%s]   RD resources (rt=%s):\n%s\n", d->id, rt, payload_buf);
    } else {
        dev_log(d, "RD: no resources found with rt='%s'", rt);
        printf("[%s]   No resources found with rt='%s'\n", d->id, rt);
    }
    return 0;
}

/* 列出 RD 服务器上所有已注册的端点和资源 */
static int client_rd_list_all(device_t *d) {
    if (!d->rd_enabled) {
        dev_log(d, "RD: disabled, cannot list endpoints");
        return -1;
    }

    coap_msg_t req, resp;
    memset(&req, 0, sizeof(req));
    req.code = COAP_GET;
    strncpy(req.uri_path, "rd", sizeof(req.uri_path) - 1);
    req.uri_query[0] = '\0';  /* 无参数: 查询所有 */

    dev_log(d, "RD: -> GET /rd (list all) to %s:%u", d->rd_ip, d->rd_port);

    char saved_peer_ip[64];
    uint16_t saved_peer_port;
    strncpy(saved_peer_ip, d->peer_ip, sizeof(saved_peer_ip) - 1);
    saved_peer_ip[sizeof(saved_peer_ip) - 1] = '\0';
    saved_peer_port = d->peer_port;

    strncpy(d->peer_ip, d->rd_ip, sizeof(d->peer_ip) - 1);
    d->peer_ip[sizeof(d->peer_ip) - 1] = '\0';
    d->peer_port = d->rd_port;

    int rc = coap_exchange(d, &req, &resp);

    strncpy(d->peer_ip, saved_peer_ip, sizeof(d->peer_ip) - 1);
    d->peer_ip[sizeof(d->peer_ip) - 1] = '\0';
    d->peer_port = saved_peer_port;

    if (rc != 0) {
        dev_log(d, "RD: list all failed (no response)");
        printf("[%s]   RD list failed: no response from RD server\n", d->id);
        return -1;
    }

    if (resp.payload_len == 0) {
        dev_log(d, "RD: no endpoints registered");
        printf("[%s]   No endpoints registered on RD server\n", d->id);
        return 0;
    }

    char payload_buf[1024];
    size_t cpy = resp.payload_len < sizeof(payload_buf) - 1
                 ? resp.payload_len : sizeof(payload_buf) - 1;
    memcpy(payload_buf, resp.payload, cpy);
    payload_buf[cpy] = '\0';

    dev_log(d, "RD: list all response (%zu bytes): %s", cpy, payload_buf);

    printf("[%s]   ===== All Registered Endpoints on RD =====\n", d->id);

    /* 解析 link-format, 按端点分行显示
     * 格式: </coap://ip:port/fwinfo>;rt="version";ver="1.0.0-A";hver="...",
     *        </coap://ip:port/log>;rt="log",...</coap://ip:port/firmware>;rt="fw" */
    int count = 0;
    char *p = payload_buf;
    while (p && *p) {
        /* 找下一个端点 (以 </coap:// 开头) */
        char *entry_start = strstr(p, "</coap://");
        if (!entry_start) break;

        /* 找该条目的结尾 (下一个 </coap:// 或字符串末尾) */
        char *next_entry = strstr(entry_start + 1, "</coap://");

        /* 找当前条目的资源分隔逗号 (不跨条目) */
        char entry_buf[512];
        size_t entry_len;
        if (next_entry) {
            /* 当前条目到下一个条目之间, 去掉最后的逗号 */
            entry_len = (size_t)(next_entry - entry_start);
            while (entry_len > 0 && entry_start[entry_len - 1] == ',')
                entry_len--;
        } else {
            entry_len = strlen(entry_start);
            while (entry_len > 0 && (entry_start[entry_len - 1] == ',' ||
                                     entry_start[entry_len - 1] == '\n'))
                entry_len--;
        }
        if (entry_len >= sizeof(entry_buf))
            entry_len = sizeof(entry_buf) - 1;
        memcpy(entry_buf, entry_start, entry_len);
        entry_buf[entry_len] = '\0';

        /* 提取 IP:Port */
        char ip_str[64] = {0};
        uint16_t port_val = 0;
        char *coap_uri = strstr(entry_buf, "coap://");
        if (coap_uri) {
            coap_uri += 7;
            char *slash = strchr(coap_uri, '/');
            char *gt = strchr(coap_uri, '>');
            char *end = slash ? slash : gt;
            if (end) {
                char host_port[80];
                size_t hp_len = (size_t)(end - coap_uri);
                if (hp_len >= sizeof(host_port)) hp_len = sizeof(host_port) - 1;
                memcpy(host_port, coap_uri, hp_len);
                host_port[hp_len] = '\0';
                char *colon = strrchr(host_port, ':');
                if (colon) {
                    *colon = '\0';
                    port_val = (uint16_t)atoi(colon + 1);
                    strncpy(ip_str, host_port, sizeof(ip_str) - 1);
                }
            }
        }

        /* 提取 ver 属性 */
        char ver_str[32] = {0};
        char *ver_start = strstr(entry_buf, "ver=\"");
        if (ver_start) {
            ver_start += 5;
            char *ver_end = strchr(ver_start, '"');
            if (ver_end && ver_end > ver_start) {
                size_t vlen = (size_t)(ver_end - ver_start);
                if (vlen >= sizeof(ver_str)) vlen = sizeof(ver_str) - 1;
                memcpy(ver_str, ver_start, vlen);
                ver_str[vlen] = '\0';
            }
        }

        /* 提取 hver 属性 */
        char hver_str[128] = {0};
        char *hver_start = strstr(entry_buf, "hver=\"");
        if (hver_start) {
            hver_start += 6;
            char *hver_end = strchr(hver_start, '"');
            if (hver_end && hver_end > hver_start) {
                size_t hlen = (size_t)(hver_end - hver_start);
                if (hlen >= sizeof(hver_str)) hlen = sizeof(hver_str) - 1;
                memcpy(hver_str, hver_start, hlen);
                hver_str[hlen] = '\0';
            }
        }

        count++;
        printf("[%s]   #%d: %s:%u  ver=%s  hver=%s\n",
               d->id, count,
               ip_str[0] ? ip_str : "?", port_val,
               ver_str[0] ? ver_str : "unknown",
               hver_str[0] ? hver_str : "none");
        printf("[%s]       %s\n", d->id, entry_buf);

        p = next_entry;
    }

    if (count == 0) {
        printf("[%s]   (no parseable entries, raw response: %s)\n", d->id, payload_buf);
    } else {
        printf("[%s]   Total endpoints: %d\n", d->id, count);
    }

    printf("[%s]   =========================================\n", d->id);
    return 0;
}

/* ====================================================================
 * RD 版本变更检测 + 固件拉取 (方法 1: RD 发现 + 主动 GET)
 * ==================================================================== */

/* 检查对端版本是否变更 (通过 RD 查询)
 * 返回: 1=版本变更, 0=版本未变或首次查询, -1=查询失败
 */
static int client_rd_check_version(device_t *d, const char *peer_id) {
    const char *old_version = get_cached_peer_version(d, peer_id);

    if (client_rd_lookup(d, peer_id) != 0) {
        dev_log(d, "RD: version check failed for '%s'", peer_id);
        return -1;
    }

    const char *new_version = get_cached_peer_version(d, peer_id);

    if (!new_version || !new_version[0]) {
        dev_log(d, "RD: peer '%s' has no version info yet (first check)", peer_id);
        return 0;
    }

    if (!old_version || !old_version[0]) {
        dev_log(d, "RD: peer '%s' version initialized: %s", peer_id, new_version);
        return 0;
    }

    if (strcmp(old_version, new_version) != 0) {
        dev_log(d, "RD: peer '%s' version CHANGED: %s -> %s",
                peer_id, old_version, new_version);
        return 1;
    }

    dev_log(d, "RD: peer '%s' version unchanged: %s", peer_id, new_version);
    return 0;
}

/* 从对端拉取固件 (GET /firmware, 使用 Block2 分块接收)
 * 成功返回 0, 失败返回 -1
 */
static int client_pull_firmware(device_t *d) {
    /* 先通过 RD 获取对端最新地址和版本 */
    if (d->rd_enabled && d->peer_id[0]) {
        if (client_rd_lookup(d, d->peer_id) != 0) {
            dev_log(d, "pull: RD lookup failed");
            return -1;
        }
    }

    dev_log(d, "pull: -> GET coap://%s:%u/firmware (Block2 pull)", d->peer_ip, d->peer_port);

    /* 第一块: 发送不带 Block2 的 GET, 服务器会返回 Block2 响应 */
    uint8_t *fw_data = NULL;
    size_t fw_total = 0;
    int block_num = 0;
    int done = 0;

    while (!done) {
        coap_msg_t req, resp;
        memset(&req, 0, sizeof(req));
        req.code = COAP_GET;
        strncpy(req.uri_path, "firmware", sizeof(req.uri_path) - 1);

        if (block_num > 0) {
            /* 后续块: 携带 Block2 选项请求 */
            req.has_block2 = 1;
            req.block2_num = block_num;
            req.block2_szx = BLOCK_SZX;
        }

        if (coap_exchange(d, &req, &resp) != 0) {
            dev_log(d, "pull: no response for block %d", block_num);
            free(fw_data);
            return -1;
        }

        if (resp.code != COAP_CONTENT) {
            dev_log(d, "pull: server returned %s for block %d",
                    coap_response_name(resp.code), block_num);
            free(fw_data);
            return -1;
        }

        /* 收集 payload */
        if (resp.payload_len > 0) {
            uint8_t *new_buf = (uint8_t *)realloc(fw_data, fw_total + resp.payload_len);
            if (!new_buf) {
                dev_log(d, "pull: realloc failed");
                free(fw_data);
                return -1;
            }
            fw_data = new_buf;
            memcpy(fw_data + fw_total, resp.payload, resp.payload_len);
            fw_total += resp.payload_len;
        }

        /* 检查 Block2 M 位: 如果没有更多块, 结束 */
        if (!resp.has_block2 || !resp.block2_more) {
            done = 1;
        } else {
            block_num++;
        }
    }

    /* 保存到本机固件文件 (作为升级) */
    if (fw_data && fw_total > 0) {
        /* 先保存旧版本 */
        save_fw_version_history_with_file(d, d->version, d->fw_path);

        /* 写入新固件 */
        FILE *fw = fopen(d->fw_path, "wb");
        if (fw) {
            fwrite(fw_data, 1, fw_total, fw);
            fclose(fw);
            dev_log(d, "pull: firmware pulled and saved (%zu bytes)", fw_total);

            /* 更新版本号 (从固件首行读取) */
            char new_ver[32] = {0};
            if (fw_data[0] != '\n') {
                size_t vi = 0;
                while (vi < sizeof(new_ver) - 1 &&
                       vi < fw_total &&
                       fw_data[vi] != '\n' && fw_data[vi] != '\0') {
                    new_ver[vi] = fw_data[vi];
                    vi++;
                }
                new_ver[vi] = '\0';
            }
            if (new_ver[0]) {
                strncpy(d->version, new_ver, sizeof(d->version) - 1);
                d->version[sizeof(d->version) - 1] = '\0';
                dev_log(d, "pull: version updated to %s", d->version);

                /* 升级后重新向 RD 注册 (更新版本号) */
                if (d->rd_enabled && d->rd_registered) {
                    dev_log(d, "pull: re-registering with RD (version changed)...");
                    client_rd_register(d);
                }
            }
        } else {
            dev_log(d, "pull: failed to write firmware file");
            free(fw_data);
            return -1;
        }

        free(fw_data);
        return 0;
    }

    dev_log(d, "pull: empty firmware received");
    free(fw_data);
    return -1;
}

/* 轮询线程: 定期检查 RD 中对端版本是否变更
 * 检测到变更时自动拉取新固件
 */
#if 0
static DWORD WINAPI poll_thread_func(LPVOID arg) {
    device_t *d = (device_t *)arg;
    dev_log(d, "poll: thread started (interval=10s)");

    while (d->running && d->auto_poll) {
        Sleep(10000);  /* 10 秒轮询间隔 */

        if (!d->running || !d->auto_poll) break;
        if (!d->rd_enabled || !d->peer_id[0]) continue;

        int changed = client_rd_check_version(d, d->peer_id);
        if (changed == 1) {
            dev_log(d, "poll: peer version changed, auto-pulling firmware...");
            client_pull_firmware(d);
        }
    }

    dev_log(d, "poll: thread stopped");
    return 0;
}
#endif
/* ====================================================================
 * 客户端: CoAP 请求-响应交换 (CON + 等待 ACK, 含 1 次重传)
 * ==================================================================== */
static int coap_exchange(device_t *d, coap_msg_t *req, coap_msg_t *resp) {
    uint8_t sbuf[COAP_MAX_MSG], rbuf[COAP_MAX_MSG];
    req->type   = COAP_CON;
    req->msg_id = d->next_msg_id++;

    /* 在 URI query 末尾附加 from=<自身ID>, 便于对端识别连接来源 */
    if (d->id[0]) {
        size_t qlen = strlen(req->uri_query);
        int need = d->id[0] ? (int)snprintf(NULL, 0, "%sfrom=%s",
                                            qlen > 0 ? "&" : "", d->id) : 0;
        if (qlen + (size_t)need < sizeof(req->uri_query)) {
            snprintf(req->uri_query + qlen, sizeof(req->uri_query) - qlen,
                     "%sfrom=%s", qlen > 0 ? "&" : "", d->id);
        }
    }

    /* 确保 content_format 正确初始化:
       memset 后 content_format 为 0，但我们需要 -1 表示未设置。
       只有当有 payload 时才保留 content_format 值，否则强制设为 -1 */
    if (req->payload_len == 0 && !req->has_block1) {
        req->content_format = -1;
    }
    
    int slen = coap_build(sbuf, sizeof(sbuf), req);
    if (slen <= 0) {
        dev_log(d, "client: coap_build FAILED (slen=%d)", slen);
        return -1;
    }

    /* 调试: 打印构造的报文 */
    dev_log(d, "client: built %d bytes for uri_path='%s' uri_query='%s':",
            slen, req->uri_path, req->uri_query);
    for (int i = 0; i < slen && i < 32; i++) {
        printf("%02X ", sbuf[i]);
        if ((i + 1) % 16 == 0) printf("\n");
    }
    if (slen > 32) printf("...");
    printf("\n");
    fflush(stdout);

    for (int retry = 0; retry < 2; retry++) {  // 最多重试 1 次
        if (coap_send(d->cli_sock, d->peer_ip, d->peer_port, sbuf, (size_t)slen) <= 0)
            return -1;

        /* 协议日志: 记录发送的请求 */
        proto_log(d, "SEND (client -> server)", req, sbuf, (size_t)slen);

        dev_log(d, "client: sent request msg_id=%u to %s:%u, waiting for ACK...",
                req->msg_id, d->peer_ip, d->peer_port);

        char from_ip[64]; uint16_t from_port;
        int n = coap_recv(d->cli_sock, rbuf, sizeof(rbuf), from_ip, &from_port, 2000);
        if (n > 0) {
            dev_log(d, "client: received %d bytes from %s:%u", n, from_ip, from_port);
            if (coap_parse(rbuf, (size_t)n, resp) == 0) {
                dev_log(d, "client: parsed response: msg_id=%u (expected %u), type=%d, code=%02x",
                        resp->msg_id, req->msg_id, resp->type, resp->code);
                if (resp->msg_id == req->msg_id) {
                    /* 协议日志: 记录接收到的响应 */
                    proto_log(d, "RECV (client <- server)", resp, rbuf, (size_t)n);
                    return 0;
                } else {
                    dev_log(d, "client: msg_id MISMATCH: got %u, expected %u",
                            resp->msg_id, req->msg_id);
                }
            } else {
                dev_log(d, "client: failed to parse response");
            }
        } else {
            dev_log(d, "client: recv timeout (n=%d)", n);
        }
        dev_log(d, "client: no matching ACK, retry %d", retry + 1);
    }
    return -1;
}

#if 0
/* GET /fwinfo */
static void client_get_fwinfo(device_t *d) {
    /* RD 模式: 先查询对端地址 */
    if (d->rd_enabled && d->peer_id[0]) {
        if (client_rd_lookup(d, d->peer_id) != 0) {
            dev_log(d, "client: RD lookup failed, abort get_fwinfo");
            return;
        }
    }

    coap_msg_t req, resp;
    memset(&req, 0, sizeof(req));
    req.code = COAP_GET;
    strncpy(req.uri_path, "fwinfo", sizeof(req.uri_path) - 1);

    dev_log(d, "client: -> GET coap://%s:%u/fwinfo", d->peer_ip, d->peer_port);
    if (coap_exchange(d, &req, &resp) == 0) {
        char body[128] = {0};
        size_t cpy = resp.payload_len < sizeof(body) - 1 ? resp.payload_len : sizeof(body) - 1;
        memcpy(body, resp.payload, cpy);
        dev_log(d, "client: <- %s ; peer fwinfo: %s",
                coap_response_name(resp.code), body);
    } else {
        dev_log(d, "client: GET /fwinfo failed (no response)");
    }
}


/* GET /log */
static void client_get_log(device_t *d) {
    /* RD 模式: 先查询对端地址 */
    if (d->rd_enabled && d->peer_id[0]) {
        if (client_rd_lookup(d, d->peer_id) != 0) {
            dev_log(d, "client: RD lookup failed, abort get_log");
            return;
        }
    }

    coap_msg_t req, resp;
    memset(&req, 0, sizeof(req));
    req.code = COAP_GET;
    strncpy(req.uri_path, "log", sizeof(req.uri_path) - 1);

    dev_log(d, "client: -> GET coap://%s:%u/log", d->peer_ip, d->peer_port);
    if (coap_exchange(d, &req, &resp) == 0) {
        dev_log(d, "client: <- %s ; peer log (%zu bytes):",
                coap_response_name(resp.code), resp.payload_len);

        /* 保存对端日志到本机日志目录 */
        {
            char peer_log_path[128];
            snprintf(peer_log_path, sizeof(peer_log_path), "device_%s_log/peer_log_%s.log",
                     d->id, d->peer_port == 5683 ? "A" : (d->peer_port == 5684 ? "B" : "peer"));
            FILE *pf = fopen(peer_log_path, "wb");
            if (pf) {
                if (resp.payload_len > 0)
                    fwrite(resp.payload, 1, resp.payload_len, pf);
                fclose(pf);
                dev_log(d, "client: peer log saved to %s", peer_log_path);
            }
        }

        printf("[%s]   -------- peer log begin --------\n", d->id);
        const uint8_t *p = resp.payload;
        size_t i = 0, start = 0;
        for (i = 0; i <= resp.payload_len; i++) {
            if (i == resp.payload_len || p[i] == '\n') {
                if (i > start) {
                    printf("[%s]   ", d->id);
                    fwrite(p + start, 1, i - start, stdout);
                    printf("\n");
                }
                start = i + 1;
            }
        }
        printf("[%s]   -------- peer log end --------\n", d->id);
        fflush(stdout);
    } else {
        dev_log(d, "client: GET /log failed (no response)");
    }
}


/* PUT /firmware 分块推送 (Block1), 从原始固件文件读取并推送给对端 */
static void client_upgrade_firmware(device_t *d) {
    /* RD 模式: 先查询对端地址 */
    if (d->rd_enabled && d->peer_id[0]) {
        if (client_rd_lookup(d, d->peer_id) != 0) {
            dev_log(d, "client: RD lookup failed, abort upgrade");
            return;
        }
    }

    /* 读取本机原始固件文件 (不会被升级覆盖) */
    FILE *fw = fopen(d->fw_orig_path, "rb");
    if (!fw) {
        dev_log(d, "client: original firmware file not found: %s", d->fw_orig_path);
        return;
    }
    fseek(fw, 0, SEEK_END);
    long fsize = ftell(fw);
    fseek(fw, 0, SEEK_SET);
    if (fsize <= 0) {
        dev_log(d, "client: firmware file empty");
        fclose(fw);
        return;
    }
    size_t image_len = (size_t)fsize;

    /* 读到内存以便分块发送 (固件较小, 一次性读取) */
    uint8_t *image = (uint8_t *)malloc(image_len);
    if (!image) {
        dev_log(d, "client: malloc failed for firmware (%zu bytes)", image_len);
        fclose(fw);
        return;
    }
    size_t total_read = fread(image, 1, image_len, fw);
    fclose(fw);
    if (total_read != image_len) {
        dev_log(d, "client: failed to read firmware file");
        free(image);
        return;
    }

    dev_log(d, "client: -> PUT firmware upgrade to %s:%u (%zu bytes, version=%s, block=%d bytes)",
            d->peer_ip, d->peer_port, image_len, d->original_version, BLOCK_SIZE);

    size_t offset   = 0;
    int    block_no = 0;
    while (1) {
        size_t chunk = BLOCK_SIZE;
        if (offset + chunk > image_len) chunk = image_len - offset;
        int more = (offset + chunk < image_len) ? 1 : 0;

        coap_msg_t req, resp;
        memset(&req, 0, sizeof(req));
        req.code          = COAP_PUT;
        strncpy(req.uri_path, "firmware", sizeof(req.uri_path) - 1);
        req.content_format = FMT_OCTET_STREAM;
        req.has_block1    = 1;
        req.block1_num    = block_no;
        req.block1_more   = more;
        req.block1_szx    = BLOCK_SZX;
        req.payload       = image + offset;
        req.payload_len   = chunk;

        dev_log(d, "client: -> PUT /firmware block %d (%zu bytes, M=%d)",
                block_no, chunk, more);

        if (coap_exchange(d, &req, &resp) == 0) {
            dev_log(d, "client: <- %s", coap_response_name(resp.code));
            if (resp.code != COAP_CHANGED) {
                dev_log(d, "client: upgrade aborted at block %d", block_no);
                free(image);
                return;
            }
        } else {
            dev_log(d, "client: no ACK for block %d, abort", block_no);
            free(image);
            return;
        }

        offset += chunk;
        block_no++;
        if (!more) break;
    }
    dev_log(d, "client: firmware upgrade to peer complete (%d blocks sent)", block_no);
    free(image);
}

/* GET /fwinfo?list - 获取对端固件版本列表 */
static void client_get_fw_version_list(device_t *d) {
    /* RD 模式: 先查询对端地址 */
    if (d->rd_enabled && d->peer_id[0]) {
        if (client_rd_lookup(d, d->peer_id) != 0) {
            dev_log(d, "client: RD lookup failed, abort get_fw_list");
            return;
        }
    }

    coap_msg_t req, resp;
    memset(&req, 0, sizeof(req));
    req.code = COAP_GET;
    strncpy(req.uri_path, "fwinfo", sizeof(req.uri_path) - 1);
    strncpy(req.uri_query, "list", sizeof(req.uri_query) - 1);

    dev_log(d, "client: -> GET coap://%s:%u/fwinfo?list", d->peer_ip, d->peer_port);
    if (coap_exchange(d, &req, &resp) == 0) {
        char body[512] = {0};
        size_t cpy = resp.payload_len < sizeof(body) - 1 ? resp.payload_len : sizeof(body) - 1;
        memcpy(body, resp.payload, cpy);
        dev_log(d, "client: <- %s ; peer firmware versions:\n%s",
                coap_response_name(resp.code), body);
    } else {
        dev_log(d, "client: GET /fwinfo?list failed (no response)");
    }
}

/* GET /fwinfo?version=XXX - 按版本获取对端固件文件 */
static void client_get_fw_by_version(device_t *d, const char *version) {
    /* RD 模式: 先查询对端地址 */
    if (d->rd_enabled && d->peer_id[0]) {
        if (client_rd_lookup(d, d->peer_id) != 0) {
            dev_log(d, "client: RD lookup failed, abort get_fw");
            return;
        }
    }

    coap_msg_t req, resp;
    memset(&req, 0, sizeof(req));
    req.code = COAP_GET;
    strncpy(req.uri_path, "fwinfo", sizeof(req.uri_path) - 1);
    snprintf(req.uri_query, sizeof(req.uri_query), "version=%s", version);

    dev_log(d, "client: -> GET coap://%s:%u/fwinfo?version=%s", d->peer_ip, d->peer_port, version);
    if (coap_exchange(d, &req, &resp) == 0) {
        dev_log(d, "client: <- %s ; peer firmware version=%s (%zu bytes)",
                coap_response_name(resp.code), version, resp.payload_len);

        /* 保存获取到的固件到本机 */
        char save_path[128];
        snprintf(save_path, sizeof(save_path), "device_%s_bin/firmware_%s_peer_%s.bin",
                 d->id, version, d->peer_port == 5683 ? "A" : (d->peer_port == 5684 ? "B" : "peer"));
        FILE *pf = fopen(save_path, "wb");
        if (pf) {
            if (resp.payload_len > 0)
                fwrite(resp.payload, 1, resp.payload_len, pf);
            fclose(pf);
            dev_log(d, "client: firmware saved to %s", save_path);
        }
    } else {
        dev_log(d, "client: GET /fwinfo?version=%s failed (no response)", version);
    }
}

/* GET /log?start_time=XXX&end_time=YYY - 按时间范围获取日志 */
static void client_get_log_by_time(device_t *d, const char *start_time, const char *end_time) {
    /* RD 模式: 先查询对端地址 */
    if (d->rd_enabled && d->peer_id[0]) {
        if (client_rd_lookup(d, d->peer_id) != 0) {
            dev_log(d, "client: RD lookup failed, abort get_log_time");
            return;
        }
    }

    coap_msg_t req, resp;
    memset(&req, 0, sizeof(req));
    req.code = COAP_GET;
    strncpy(req.uri_path, "log", sizeof(req.uri_path) - 1);

    /* 构造查询参数 */
    if (start_time && end_time) {
        snprintf(req.uri_query, sizeof(req.uri_query), "start_time=%s&end_time=%s", start_time, end_time);
    } else if (start_time) {
        snprintf(req.uri_query, sizeof(req.uri_query), "start_time=%s", start_time);
    } else if (end_time) {
        snprintf(req.uri_query, sizeof(req.uri_query), "end_time=%s", end_time);
    }

    dev_log(d, "client: -> GET coap://%s:%u/log?%s", d->peer_ip, d->peer_port, req.uri_query);
    if (coap_exchange(d, &req, &resp) == 0) {
        dev_log(d, "client: <- %s ; peer log (%zu bytes):",
                coap_response_name(resp.code), resp.payload_len);

        /* 保存对端日志到本机 */
        char peer_log_path[128];
        snprintf(peer_log_path, sizeof(peer_log_path), "device_%s_log/peer_log_%s_%s.log",
                 d->id, d->peer_port == 5683 ? "A" : (d->peer_port == 5684 ? "B" : "peer"),
                 start_time ? start_time : "all");
        FILE *pf = fopen(peer_log_path, "wb");
        if (pf) {
            if (resp.payload_len > 0)
                fwrite(resp.payload, 1, resp.payload_len, pf);
            fclose(pf);
            dev_log(d, "client: peer log saved to %s", peer_log_path);
        }

        printf("[%s]   -------- peer log begin --------\n", d->id);
        const uint8_t *p = resp.payload;
        size_t i = 0, start = 0;
        for (i = 0; i <= resp.payload_len; i++) {
            if (i == resp.payload_len || p[i] == '\n') {
                if (i > start) {
                    printf("[%s]   ", d->id);
                    fwrite(p + start, 1, i - start, stdout);
                    printf("\n");
                }
                start = i + 1;
            }
        }
        printf("[%s]   -------- peer log end --------\n", d->id);
        fflush(stdout);
    } else {
        dev_log(d, "client: GET /log failed (no response)");
    }
}

#endif
/* ====================================================================
 * 新命令函数实现
 * ==================================================================== */

/* 按对端 ID 获取日志 (先 RD 查询地址, 再 GET /log)
 * 支持可选时间过滤: get_log <id> [start_time] [end_time]
 * 时间格式: HH:MM 或 HH:MM:SS (不含空格, 客户端本地过滤) */
static int client_get_log_by_id(device_t *d, const char *peer_id,
                                const char *start_time, const char *end_time) {
    if (client_rd_lookup(d, peer_id) != 0) {
        dev_log(d, "get_log: RD lookup failed for '%s'", peer_id);
        return -1;
    }

    coap_msg_t req, resp;
    memset(&req, 0, sizeof(req));
    req.code = COAP_GET;
    strncpy(req.uri_path, "log", sizeof(req.uri_path) - 1);

    dev_log(d, "get_log: -> GET coap://%s:%u/log (peer=%s, range=%s~%s)",
            d->peer_ip, d->peer_port, peer_id,
            start_time ? start_time : "begin",
            end_time ? end_time : "now");
    if (coap_exchange(d, &req, &resp) != 0) {
        dev_log(d, "get_log: no response from %s", peer_id);
        return -1;
    }

    /* 保存完整日志 */
    char path[128];
    snprintf(path, sizeof(path), "device_%s_log/peer_log_%s.log", d->id, peer_id);
    FILE *pf = fopen(path, "wb");
    if (pf && resp.payload_len > 0) {
        fwrite(resp.payload, 1, resp.payload_len, pf);
    }
    if (pf) fclose(pf);

    /* 客户端本地时间过滤 */
    int have_filter = (start_time || end_time);

    /* 解析时间参数为秒数 (仅 HH:MM 或 HH:MM:SS 格式, 补当天日期) */
    time_t start_t = 0, end_t = 0;
    int have_start = 0, have_end = 0;
    time_t now = time(NULL);
    struct tm today;
    localtime_s(&today, &now);

    if (start_time) {
        int h = 0, m = 0, s = 0;
        if (sscanf(start_time, "%d:%d:%d", &h, &m, &s) >= 2) {
            struct tm t = today;
            t.tm_hour = h; t.tm_min = m; t.tm_sec = (sscanf(start_time, "%d:%d:%d", &h, &m, &s) >= 3) ? s : 0;
            start_t = mktime(&t);
            have_start = 1;
        }
    }
    if (end_time) {
        int h = 0, m = 0, s = 0;
        if (sscanf(end_time, "%d:%d:%d", &h, &m, &s) >= 2) {
            struct tm t = today;
            t.tm_hour = h; t.tm_min = m; t.tm_sec = (sscanf(end_time, "%d:%d:%d", &h, &m, &s) >= 3) ? s : 0;
            end_t = mktime(&t);
            have_end = 1;
        }
    }

    printf("[%s]   -------- %s log begin --------\n", d->id, peer_id);

    if (resp.payload_len > 0) {
        if (have_filter && (have_start || have_end)) {
            /* 逐行过滤输出 */
            const char *p = (const char *)resp.payload;
            const char *end = p + resp.payload_len;
            while (p < end) {
                /* 取一行 */
                const char *nl = memchr(p, '\n', (size_t)(end - p));
                size_t line_len = nl ? (size_t)(nl - p) : (size_t)(end - p);

                /* 解析行内时间戳 [YYYY-MM-DD HH:MM:SS] */
                struct tm log_tm;
                memset(&log_tm, 0, sizeof(log_tm));
                int matched = sscanf(p, "[%d-%d-%d %d:%d:%d]",
                                     &log_tm.tm_year, &log_tm.tm_mon, &log_tm.tm_mday,
                                     &log_tm.tm_hour, &log_tm.tm_min, &log_tm.tm_sec);
                if (matched >= 6) {
                    log_tm.tm_year -= 1900;
                    time_t log_time = mktime(&log_tm);

                    int in_range = 1;
                    if (have_start && log_time < start_t) in_range = 0;
                    if (have_end && log_time > end_t) in_range = 0;

                    if (in_range) {
                        fwrite(p, 1, line_len, stdout);
                        printf("\n");
                    }
                } else {
                    /* 无法解析时间戳的行直接输出 */
                    fwrite(p, 1, line_len, stdout);
                    printf("\n");
                }
                p = nl ? nl + 1 : end;
            }
        } else {
            /* 无过滤, 输出全部 */
            fwrite(resp.payload, 1, resp.payload_len, stdout);
        }
    }

    printf("\n[%s]   -------- %s log end --------\n", d->id, peer_id);
    fflush(stdout);
    return 0;
}

/* 从对端拉取最新固件 (GET /firmware Block2) 并升级自身 */
static int client_get_fw_by_id(device_t *d, const char *peer_id) {
    if (client_rd_lookup(d, peer_id) != 0) {
        dev_log(d, "get_fw: RD lookup failed for '%s'", peer_id);
        return -1;
    }

    dev_log(d, "get_fw: -> GET coap://%s:%u/firmware (Block2 pull, peer=%s)",
            d->peer_ip, d->peer_port, peer_id);

    uint8_t *fw_data = NULL;
    size_t fw_total = 0;
    int block_num = 0;
    int done = 0;

    while (!done) {
        coap_msg_t req, resp;
        memset(&req, 0, sizeof(req));
        req.code = COAP_GET;
        strncpy(req.uri_path, "firmware", sizeof(req.uri_path) - 1);

        if (block_num > 0) {
            req.has_block2 = 1;
            req.block2_num = block_num;
            req.block2_szx = BLOCK_SZX;
        }

        if (coap_exchange(d, &req, &resp) != 0) {
            dev_log(d, "get_fw: no response for block %d", block_num);
            free(fw_data);
            return -1;
        }

        if (resp.code != COAP_CONTENT) {
            dev_log(d, "get_fw: server returned %s for block %d",
                    coap_response_name(resp.code), block_num);
            free(fw_data);
            return -1;
        }

        if (resp.payload_len > 0) {
            uint8_t *nb = (uint8_t *)realloc(fw_data, fw_total + resp.payload_len);
            if (!nb) { free(fw_data); return -1; }
            fw_data = nb;
            memcpy(fw_data + fw_total, resp.payload, resp.payload_len);
            fw_total += resp.payload_len;
        }

        if (!resp.has_block2 || !resp.block2_more) {
            done = 1;
        } else {
            block_num++;
        }
    }

    int rc = -1;
    if (fw_data && fw_total > 0) {
        rc = apply_firmware_update(d, fw_data, fw_total);
    }
    free(fw_data);
    return rc;
}

/* 从对端按版本号获取历史固件 (GET /fwinfo?version=XXX, Block2 分块) 并升级自身 */
static int client_get_fw_by_version_upgrade(device_t *d, const char *peer_id, const char *version) {
    if (client_rd_lookup(d, peer_id) != 0) {
        dev_log(d, "get_fw_ver: RD lookup failed for '%s'", peer_id);
        return -1;
    }

    dev_log(d, "get_fw_ver: -> GET coap://%s:%u/fwinfo?version=%s (Block2 pull, peer=%s)",
            d->peer_ip, d->peer_port, version, peer_id);

    uint8_t *fw_data = NULL;
    size_t fw_total = 0;
    int block_num = 0;
    int done = 0;

    while (!done) {
        coap_msg_t req, resp;
        memset(&req, 0, sizeof(req));
        req.code = COAP_GET;
        strncpy(req.uri_path, "fwinfo", sizeof(req.uri_path) - 1);
        snprintf(req.uri_query, sizeof(req.uri_query), "version=%s", version);

        if (block_num > 0) {
            req.has_block2 = 1;
            req.block2_num = block_num;
            req.block2_szx = BLOCK_SZX;
        }

        if (coap_exchange(d, &req, &resp) != 0) {
            dev_log(d, "get_fw_ver: no response for block %d", block_num);
            free(fw_data);
            return -1;
        }

        if (resp.code != COAP_CONTENT) {
            dev_log(d, "get_fw_ver: server returned %s for block %d",
                    coap_response_name(resp.code), block_num);
            free(fw_data);
            return -1;
        }

        if (resp.payload_len > 0) {
            uint8_t *nb = (uint8_t *)realloc(fw_data, fw_total + resp.payload_len);
            if (!nb) { free(fw_data); return -1; }
            fw_data = nb;
            memcpy(fw_data + fw_total, resp.payload, resp.payload_len);
            fw_total += resp.payload_len;
        }

        if (!resp.has_block2 || !resp.block2_more) {
            done = 1;
        } else {
            block_num++;
        }
    }

    int rc = -1;
    if (fw_data && fw_total > 0) {
        /* 在固件数据首行添加版本号, 然后调用 apply_firmware_update */
        /* fw_data 是原始固件 body (不含版本号头), 需要加上版本号头 */
        char *full_data = (char *)malloc(fw_total + 32);
        if (full_data) {
            int hdr = snprintf(full_data, 32, "%s\n", version);
            memcpy(full_data + hdr, fw_data, fw_total);
            rc = apply_firmware_update(d, (const uint8_t *)full_data, fw_total + (size_t)hdr);
            free(full_data);
        }
    }
    free(fw_data);
    return rc;
}

/* 查询指定对端的历史固件版本 (get_fwhis <id> 命令) */
static int client_get_fwhis(device_t *d, const char *peer_id) {
    /* 通过 RD 查询对端, 同时缓存版本和历史版本 */
    if (client_rd_lookup(d, peer_id) != 0) {
        printf("[%s]   Failed to lookup peer '%s' from RD\n", d->id, peer_id);
        return -1;
    }

    /* 从缓存中读取历史版本 */
    char history[128] = {0};
    char version[32] = {0};
    EnterCriticalSection(&d->lock);
    for (int i = 0; i < MAX_PEER_VERSIONS; i++) {
        if (d->peer_vers[i].active &&
            strcmp(d->peer_vers[i].peer_id, peer_id) == 0) {
            strncpy(history, d->peer_vers[i].history, sizeof(history) - 1);
            strncpy(version, d->peer_vers[i].version, sizeof(version) - 1);
            break;
        }
    }
    LeaveCriticalSection(&d->lock);

    printf("[%s]   === Peer '%s' Firmware Versions ===\n", d->id, peer_id);
    printf("[%s]   Current version: %s\n", d->id,
           version[0] ? version : "(unknown)");

    if (history[0]) {
        /* 逐个打印历史版本 */
        printf("[%s]   History versions:\n", d->id);
        char *tok = strtok(history, ",");
        int idx = 1;
        while (tok) {
            printf("[%s]     %d. %s\n", d->id, idx, tok);
            tok = strtok(NULL, ",");
            idx++;
        }
    } else {
        printf("[%s]   History versions: (none)\n", d->id);
    }

    return 0;
}

/* 打印所有对端连接和版本缓存信息 (get_link_id 命令) */
static void print_connections(device_t *d) {
    printf("[%s]   === Connected Clients ===\n", d->id);
    int count = 0;
    EnterCriticalSection(&d->lock);
    for (int i = 0; i < MAX_CONNECTIONS; i++) {
        if (d->connections[i].active) {
            count++;
            char time_str[64] = {0};
            struct tm *tm = localtime(&d->connections[i].last_seen);
            if (tm) strftime(time_str, sizeof(time_str), "%Y-%m-%d %H:%M:%S", tm);
            printf("[%s]   #%d: %s:%u  peer_id=%s  last_seen=%s\n",
                   d->id, count,
                   d->connections[i].ip,
                   d->connections[i].port,
                   d->connections[i].peer_id[0] ? d->connections[i].peer_id : "(unknown)",
                   time_str);
        }
    }
    LeaveCriticalSection(&d->lock);
    if (count == 0) printf("[%s]   (no connections yet)\n", d->id);
    printf("[%s]   Total connections: %d\n", d->id, count);

    printf("[%s]   === Peer Version Cache ===\n", d->id);
    int vcount = 0;
    EnterCriticalSection(&d->lock);
    for (int i = 0; i < MAX_PEER_VERSIONS; i++) {
        if (d->peer_vers[i].active) {
            vcount++;
            char time_str[64] = {0};
            struct tm *tm = localtime(&d->peer_vers[i].last_check);
            if (tm) strftime(time_str, sizeof(time_str), "%Y-%m-%d %H:%M:%S", tm);
            printf("[%s]   #%d: peer_id=%s  version=%s  last_check=%s\n",
                   d->id, vcount,
                   d->peer_vers[i].peer_id,
                   d->peer_vers[i].version[0] ? d->peer_vers[i].version : "(unknown)",
                   time_str);
        }
    }
    LeaveCriticalSection(&d->lock);
    if (vcount == 0) printf("[%s]   (no peer versions cached yet)\n", d->id);
}

/* ====================================================================
 * main
 * ==================================================================== */
int main(int argc, char **argv) {   //有参main函数，传递ip等信息
    device_t d;
    memset(&d, 0, sizeof(d));
    InitializeCriticalSection(&d.lock);  
    d.running     = 1;
    d.next_msg_id = (uint16_t)(time(NULL) & 0xffff);   
    strncpy(d.version, "1.0.0", sizeof(d.version) - 1);   //初始化版本号
    strncpy(d.peer_ip, "127.0.0.1", sizeof(d.peer_ip) - 1);  //初始化对端IP，本地回环地址

    for (int i = 1; i < argc; i++) {
        if      (!strcmp(argv[i], "--id")        && i + 1 < argc) strncpy(d.id,        argv[++i], sizeof(d.id) - 1);
        else if (!strcmp(argv[i], "--port")      && i + 1 < argc) d.port      = (uint16_t)atoi(argv[++i]);
        else if (!strcmp(argv[i], "--peer-ip")   && i + 1 < argc) strncpy(d.peer_ip,   argv[++i], sizeof(d.peer_ip) - 1);
        else if (!strcmp(argv[i], "--peer-port") && i + 1 < argc) d.peer_port = (uint16_t)atoi(argv[++i]);
        else if (!strcmp(argv[i], "--peer-id")   && i + 1 < argc) strncpy(d.peer_id,   argv[++i], sizeof(d.peer_id) - 1);
        else if (!strcmp(argv[i], "--version")   && i + 1 < argc) strncpy(d.version,   argv[++i], sizeof(d.version) - 1);
        else if (!strcmp(argv[i], "--rd-ip")     && i + 1 < argc) { strncpy(d.rd_ip, argv[++i], sizeof(d.rd_ip) - 1); d.rd_enabled = 1; }
        else if (!strcmp(argv[i], "--rd-port")   && i + 1 < argc) d.rd_port = (uint16_t)atoi(argv[++i]);
    }  // 解析命令行参数，设置设备ID、端口、对端IP、对端端口、RD服务器和版本号

    /* 记录启动时的原始版本, 升级对端时始终用它 */
    strncpy(d.original_version, d.version, sizeof(d.original_version) - 1);
    d.original_version[sizeof(d.original_version) - 1] = '\0';

    /* 创建子目录 */
    {
        char dir_cmd[512];
        snprintf(dir_cmd, sizeof(dir_cmd),
                 "cmd /c \"if not exist device_%s_log mkdir device_%s_log & "
                 "if not exist device_%s_bin mkdir device_%s_bin & "
                 "if not exist device_%s_bin\\versions mkdir device_%s_bin\\versions\"",
                 d.id, d.id, d.id, d.id, d.id, d.id);
        dev_log(&d, "Creating directories: %s", dir_cmd);
        int ret = system(dir_cmd);
        dev_log(&d, "Directory creation returned: %d", ret);
    }

    /* 构造文件路径 */
    snprintf(d.fw_path,       sizeof(d.fw_path),       "device_%s_bin/firmware_%s.bin", d.id, d.id);
    snprintf(d.fw_orig_path,  sizeof(d.fw_orig_path),  "device_%s_bin/firmware_%s_orig.bin", d.id, d.id);
    snprintf(d.fw_versions_dir, sizeof(d.fw_versions_dir), "device_%s_bin/versions", d.id);
    snprintf(d.log_path,      sizeof(d.log_path),      "device_%s_log/device_%s.log", d.id, d.id);
    snprintf(d.proto_log_path, sizeof(d.proto_log_path), "device_%s_log/proto_%s.log", d.id, d.id);


    //输入参数检查
    if (d.id[0] == 0 || d.port == 0) {
        fprintf(stderr,
            "Usage: %s --id A --port 5683 [--peer-ip 127.0.0.1 --peer-port 5684] "
            "[--peer-id B] [--rd-ip 127.0.0.1 --rd-port 5685] [--version 1.0.0-A]\n"
            "\nRD mode (recommended):\n"
            "  %s --id A --port 5683 --peer-id B --rd-ip 127.0.0.1 --rd-port 5685 --version 1.0.0-A\n"
            "\nDirect mode (legacy):\n"
            "  %s --id A --port 5683 --peer-ip 127.0.0.1 --peer-port 5684 --version 1.0.0-A\n",
            argv[0], argv[0], argv[0]);
        DeleteCriticalSection(&d.lock);
        return 1;
    }

    /* RD 模式默认端口 */
    if (d.rd_enabled && d.rd_port == 0) d.rd_port = 5685;

    /* 传统模式: 必须提供 peer-port */
    if (!d.rd_enabled && d.peer_port == 0) {
        fprintf(stderr, "Error: --peer-port is required in direct mode (or use --rd-ip for RD mode)\n");
        DeleteCriticalSection(&d.lock);
        return 1;
    }

    // 初始化COAP库，创建套接字，绑定端口
    if (coap_init() != 0) {
        fprintf(stderr, "WSAStartup failed\n");
        DeleteCriticalSection(&d.lock);
        return 1;
    }  

    d.srv_sock = coap_open_socket(d.port); 
    d.cli_sock = coap_open_socket(0);   //默认分配端口 
    if (d.srv_sock == INVALID_SOCKET || d.cli_sock == INVALID_SOCKET) {
        fprintf(stderr, "socket creation failed (port %u in use?)\n", d.port);
        coap_cleanup();
        DeleteCriticalSection(&d.lock);
        return 1;
    }

    /* 创建初始固件文件 (当前固件 + 原始固件副本, 用于升级对端) */
    //模拟固件升级文件，包含版本号和填充数据
    {
        const char *paths[] = { d.fw_path, d.fw_orig_path };  //固件文件的路径信息
        for (int p = 0; p < 2; p++) {
            FILE *fw = fopen(paths[p], "wb");  //以写入模式打开固件文件，返回结构体指针fw
            if (fw) {
                fprintf(fw, "%s\n", d.original_version);  //写入原始版本号
                for (int i = 0; i < FW_FILLER_LEN; i++)
                    fputc(i & 0xff, fw);  //写入填充数据
                fclose(fw);
            } else {
                fprintf(stderr, "failed to create firmware file: %s\n", paths[p]);
            }
        }
    }

    /* 打开日志文件 */
    d.log_fp = fopen(d.log_path, "w");
    d.proto_log_fp = fopen(d.proto_log_path, "w");
    if (d.proto_log_fp) {
        fprintf(d.proto_log_fp, "=== CoAP Protocol Log for Device %s ===\n\n", d.id);
        fflush(d.proto_log_fp);
    }

    dev_log(&d, "==== Device %s started: listen=:%u  version=%s ====",
            d.id, d.port, d.version);
    if (d.rd_enabled) {
        dev_log(&d, "RD mode: RD=%s:%u, peer_id=%s", d.rd_ip, d.rd_port,
                d.peer_id[0] ? d.peer_id : "(none)");
    } else {
        dev_log(&d, "Direct mode: peer=%s:%u", d.peer_ip, d.peer_port);
    }
    dev_log(&d, "Protocol log file: %s", d.proto_log_path);

    HANDLE th = CreateThread(NULL, 0, server_thread, &d, 0, NULL);  //创建监听线程

    /* RD 模式: 启动时自动向 RD 服务器注册 */
    if (d.rd_enabled) {
        dev_log(&d, "RD: auto-registering with RD server...");
        Sleep(500);  /* 等服务线程就绪 */
        client_rd_register(&d);
    }

    /* 等待对端就绪 */
    Sleep(1500);

    /* 交互式命令循环 */
    printf("\n=== CoAP Device %s Interactive Mode ===\n", d.id);
    printf("Available commands:\n");
    printf("  --- RD Commands ---\n");
    printf("  rd_register               - Register resources with RD (incl. history versions)\n");
    printf("  rd_update                - Update RD registration (TTL + resources)\n");
    printf("  rd_deregister            - Deregister from RD server\n");
    printf("  rd_lookup <id>           - Lookup peer IP, port, resources from RD\n");
    printf("  rd_find <rt>             - Find clients by resource type (e.g. version)\n");
    printf("  rd_list                  - List all endpoints registered on RD\n");
    printf("  rd_check <id>            - Check if peer version changed via RD\n");
    printf("  --- Resource Commands ---\n");
    printf("  get_link_id              - Show connected clients and peer version cache\n");
    printf("  get_log <id> [start] [end] - Get log from peer (optional time filter HH:MM)\n");
    printf("  get_fw <id>              - Pull latest firmware from peer + self-upgrade\n");
    printf("  get_fw <version> <id>    - Pull specific version firmware + self-upgrade\n");
    printf("  --- Status ---\n");
    printf("  status                   - Show current device status\n");
    printf("  help                     - Show this help\n");
    printf("  quit                     - Exit device\n");
    printf("========================================\n\n");

    char cmd[256];
    while (d.running) {
        printf("[%s] command> ", d.id);
        fflush(stdout);

        if (!fgets(cmd, sizeof(cmd), stdin)) break;

        /* 去除换行符 */
        size_t len = strlen(cmd);
        while (len > 0 && (cmd[len-1] == '\n' || cmd[len-1] == '\r'))
            cmd[--len] = '\0';

        if (len == 0) continue;

        if (!strcmp(cmd, "quit") || !strcmp(cmd, "exit")) {
            dev_log(&d, "Command: quit");
            if (d.rd_enabled && d.rd_registered) {
                client_rd_deregister(&d);
            }
            break;
        } else if (!strcmp(cmd, "help")) {
            printf("  --- RD Commands ---\n");
            printf("  rd_register               - Register resources with RD (incl. history versions)\n");
            printf("  rd_update                - Update RD registration (TTL + resources)\n");
            printf("  rd_deregister            - Deregister from RD server\n");
            printf("  rd_lookup <id>           - Lookup peer IP, port, resources from RD\n");
            printf("  rd_find <rt>             - Find clients by resource type (e.g. version)\n");
            printf("  rd_list                  - List all endpoints registered on RD\n");
            printf("  rd_check <id>            - Check if peer version changed via RD\n");
            printf("  --- Resource Commands ---\n");
            printf("  get_link_id              - Show connected clients and peer version cache\n");
            printf("  get_log <id> [start] [end] - Get log from peer (optional time filter HH:MM)\n");
            printf("  get_fw <id>              - Pull latest firmware from peer + self-upgrade\n");
            printf("  get_fw <version> <id>    - Pull specific version firmware + self-upgrade\n");
            printf("  get_fwhis <id>           - Show peer firmware version history\n");
            printf("  --- Status ---\n");
            printf("  status                   - Show current device status\n");
            printf("  help                     - Show this help\n");
            printf("  quit                     - Exit device\n");
        } else if (!strcmp(cmd, "status")) {
            char ver[32];
            EnterCriticalSection(&d.lock);
            strncpy(ver, d.version, sizeof(ver) - 1); ver[sizeof(ver) - 1] = '\0';
            LeaveCriticalSection(&d.lock);
            dev_log(&d, "Status: id=%s, port=%u, version=%s, history=%d versions",
                    d.id, d.port, ver, d.fw_version_count);
            if (d.rd_enabled) {
                dev_log(&d, "  RD: %s:%u, registered=%s, peer_id=%s, resolved_peer=%s:%u",
                        d.rd_ip, d.rd_port,
                        d.rd_registered ? "yes" : "no",
                        d.peer_id[0] ? d.peer_id : "(none)",
                        d.peer_ip, d.peer_port);
            }
        } else if (!strcmp(cmd, "get_link_id")) {
            print_connections(&d);
        } else if (!strcmp(cmd, "rd_register")) {
            dev_log(&d, "Command: rd_register");
            client_rd_register(&d);
        } else if (!strcmp(cmd, "rd_update")) {
            dev_log(&d, "Command: rd_update");
            client_rd_update(&d);
        } else if (!strcmp(cmd, "rd_deregister")) {
            dev_log(&d, "Command: rd_deregister");
            client_rd_deregister(&d);
        } else if (!strncmp(cmd, "rd_lookup ", 10)) {
            const char *target = cmd + 10;
            dev_log(&d, "Command: rd_lookup %s", target);
            if (client_rd_lookup(&d, target) == 0) {
                const char *ver = get_cached_peer_version(&d, target);
                printf("[%s]   Resolved: %s -> %s:%u  version=%s\n",
                       d.id, target, d.peer_ip, d.peer_port,
                       ver ? ver : "unknown");
            } else {
                printf("[%s]   Lookup failed for %s\n", d.id, target);
            }
        } else if (!strncmp(cmd, "rd_find ", 8)) {
            const char *rt = cmd + 8;
            dev_log(&d, "Command: rd_find %s", rt);
            client_rd_lookup_by_resource(&d, rt);
        } else if (!strcmp(cmd, "rd_list")) {
            dev_log(&d, "Command: rd_list");
            client_rd_list_all(&d);
        } else if (!strncmp(cmd, "rd_check ", 9)) {
            const char *target = cmd + 9;
            dev_log(&d, "Command: rd_check %s", target);
            int changed = client_rd_check_version(&d, target);
            if (changed == 1) {
                const char *new_ver = get_cached_peer_version(&d, target);
                printf("[%s]   Peer '%s' version CHANGED -> %s\n",
                       d.id, target, new_ver ? new_ver : "?");
            } else if (changed == 0) {
                const char *ver = get_cached_peer_version(&d, target);
                printf("[%s]   Peer '%s' version unchanged: %s\n",
                       d.id, target, ver ? ver : "unknown");
            } else {
                printf("[%s]   Check failed for '%s'\n", d.id, target);
            }
        } else if (!strncmp(cmd, "get_log ", 8)) {
            /* get_log <id> [start_time] [end_time]
             * 时间格式: HH:MM 或 HH:MM:SS */
            char peer_id[16] = {0};
            char start_time[16] = {0};
            char end_time[16] = {0};
            int nargs = sscanf(cmd + 8, "%15s %15s %15s",
                               peer_id, start_time, end_time);
            if (nargs < 1) {
                printf("[%s]   Usage: get_log <id> [start_time] [end_time]\n", d.id);
                printf("[%s]   Time format: HH:MM or HH:MM:SS\n", d.id);
            } else {
                dev_log(&d, "Command: get_log %s%s%s%s", peer_id,
                        nargs >= 2 ? " from " : "",
                        nargs >= 2 ? start_time : "",
                        nargs >= 3 ? end_time : "");
                client_get_log_by_id(&d, peer_id,
                                     nargs >= 2 ? start_time : NULL,
                                     nargs >= 3 ? end_time : NULL);
            }
        } else if (!strncmp(cmd, "get_fw ", 7)) {
            /* get_fw <id> 或 get_fw <version> <id> */
            char *arg1 = cmd + 7;
            char *space = strchr(arg1, ' ');
            if (space) {
                /* get_fw <version> <id> */
                size_t vlen = (size_t)(space - arg1);
                char version[32] = {0};
                if (vlen >= sizeof(version)) vlen = sizeof(version) - 1;
                memcpy(version, arg1, vlen);
                version[vlen] = '\0';
                char *peer_id = space + 1;
                dev_log(&d, "Command: get_fw %s (version=%s, peer=%s)", cmd, version, peer_id);
                client_get_fw_by_version_upgrade(&d, peer_id, version);
            } else {
                /* get_fw <id> */
                dev_log(&d, "Command: get_fw %s (latest firmware)", arg1);
                client_get_fw_by_id(&d, arg1);
            }
        } else if (!strncmp(cmd, "get_fwhis ", 10)) {
            const char *peer_id = cmd + 10;
            dev_log(&d, "Command: get_fwhis %s", peer_id);
            client_get_fwhis(&d, peer_id);
        } else {
            printf("Unknown command: '%s'. Type 'help' for available commands.\n", cmd);
        }
    }

    /* 打印最终状态 */
    char ver[32];
    EnterCriticalSection(&d.lock);
    strncpy(ver, d.version, sizeof(ver) - 1); ver[sizeof(ver) - 1] = '\0';
    LeaveCriticalSection(&d.lock);
    dev_log(&d, "==== Device %s finished: final version=%s ====", d.id, ver);

    d.running = 0;
    WaitForSingleObject(th, 2000);
    CloseHandle(th);

    coap_close_socket(d.srv_sock);
    coap_close_socket(d.cli_sock);
    if (d.log_fp) fclose(d.log_fp);
    if (d.proto_log_fp) fclose(d.proto_log_fp);
    coap_cleanup();
    DeleteCriticalSection(&d.lock);
    return 0;
}