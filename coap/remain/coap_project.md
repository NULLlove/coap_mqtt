# CoAP 项目技术文档

## 1. 项目概述

本项目实现了一个基于 CoAP (RFC 7252) 协议的嵌入式设备固件管理与升级系统，包含以下核心功能：

- **CoAP 协议栈**：报文构造/解析、UDP 收发、Block1/Block2 分块传输 (RFC 7959)
- **RD 资源目录** (RFC 9176)：集中式设备注册与资源发现
- **固件版本管理**：当前版本 + 历史版本 (最多 5 个)，支持按版本号拉取历史固件
- **设备间通信**：日志获取、固件拉取升级、版本检查

### 文件结构

| 文件                            | 说明                                   |
| ----------------------------- | ------------------------------------ |
| `coap.h` / `coap.c`           | CoAP 协议栈核心：报文构造/解析、UDP 收发、选项操作       |
| `device.c`                    | CoAP 设备实现：服务器资源暴露 + 客户端 RD 操作 + 固件升级 |
| `rd_server.h` / `rd_server.c` | RD 资源目录服务器：端点注册/更新/注销/查询             |

### 编译方式

```bash
# 编译设备程序
gcc -Wall -Wextra -O2 -o device.exe coap.c device.c -lws2_32

# 编译 RD 服务器
gcc -Wall -Wextra -O2 -o rd_server.exe coap.c rd_server.c -lws2_32
```

### 启动方式

```bash
# 启动 RD 服务器
.\rd_server.exe --port 5685

# 启动设备 A (RD 模式)
.\device.exe --id A --port 5683 --peer-id B --rd-ip 127.0.0.1 --rd-port 5685 --version 1.0.0-A

# 启动设备 B (RD 模式)
.\device.exe --id B --port 5684 --peer-id A --rd-ip 127.0.0.1 --rd-port 5685 --version 1.0.0-B
```

***

## 2. coap.h / coap.c — CoAP 协议栈

### 2.1 协议常量

| 常量                                                     | 值         | 说明                      |
| ------------------------------------------------------ | --------- | ----------------------- |
| `COAP_VER`                                             | 1         | 协议版本                    |
| `COAP_CON` / `COAP_NON` / `COAP_ACK` / `COAP_RST`      | 0/1/2/3   | 报文类型                    |
| `COAP_GET` / `COAP_POST` / `COAP_PUT` / `COAP_DELETE`  | 0x01-0x04 | 请求方法                    |
| `COAP_CREATED` \~ `COAP_CONTENT`                       | 0x41-0x45 | 2.xx 成功响应               |
| `COAP_BAD_REQUEST` \~ `COAP_UNSUPPORTED_CONTENT_FMT`   | 0x80-0x8F | 4.xx 客户端错误              |
| `COAP_INTERNAL_ERROR` \~ `COAP_PROXYING_NOT_SUPPORTED` | 0xA0-0xA5 | 5.xx 服务端错误              |
| `BLOCK_SZX`                                            | 4         | 分块大小指数，2^(4+4)=256 字节/块 |
| `COAP_MAX_MSG`                                         | 2048      | 报文最大字节数                 |
| `COAP_PAYLOAD_MARKER`                                  | 0xFF      | 报文头与负载的分隔符              |

### 2.2 选项编号

| 常量                | 值  | 说明             |
| ----------------- | -- | -------------- |
| `OPT_URI_PATH`    | 11 | URI 路径         |
| `OPT_CONTENT_FMT` | 12 | 内容格式           |
| `OPT_URI_QUERY`   | 15 | URI 查询参数       |
| `OPT_BLOCK2`      | 23 | 响应分块 (服务端→客户端) |
| `OPT_BLOCK1`      | 27 | 请求分块 (客户端→服务端) |
| `OPT_SIZE2`       | 28 | 响应总大小          |

### 2.3 结构体

#### `coap_option_t` — CoAP 选项

```c
typedef struct {
    uint16_t number;                        /* 选项编号 (如 OPT_URI_PATH=11) */
    uint16_t length;                        /* 值长度 */
    uint8_t  value[COAP_OPT_VALUE_MAX];     /* 值数据 (最大 64 字节) */
} coap_option_t;
```

#### `coap_msg_t` — CoAP 报文逻辑表示

```c
typedef struct {
    coap_type_t type;          /* 报文类型 (CON/NON/ACK/RST) */
    uint8_t     code;          /* 请求方法或响应码 */
    uint16_t    msg_id;        /* 消息 ID */
    uint8_t     token[8];      /* Token 值 */
    uint8_t     token_len;     /* Token 长度 */

    /* 便捷字段 (解析时自动填充) */
    char        uri_path[64];       /* URI 路径 */
    char        uri_query[128];     /* URI 查询参数 */
    int         content_format;     /* 内容格式 */

    /* Block1: 请求分块 (客户端→服务端) */
    int         has_block1;         /* 是否包含 Block1 选项 */
    int         block1_num;         /* 分块编号 */
    int         block1_more;        /* 是否还有更多分块 */
    int         block1_szx;         /* 分块大小指数 */

    /* Block2: 响应分块 (服务端→客户端) */
    int         has_block2;
    int         block2_num;
    int         block2_more;
    int         block2_szx;

    /* 通用选项数组 */
    coap_option_t options[COAP_MAX_OPTIONS];  /* 最多 16 个选项 */
    int           option_count;

    /* Payload */
    uint8_t        payload_buf[COAP_MAX_MSG];  /* 内部缓冲区 */
    const uint8_t *payload;                     /* 负载指针 */
    size_t         payload_len;                 /* 负载长度 */
} coap_msg_t;
```

### 2.4 API 函数

| 函数签名                                                                                                                                                                           | 说明                                 |
| ------------------------------------------------------------------------------------------------------------------------------------------------------------------------------ | ---------------------------------- |
| `int coap_init(void)`                                                                                                                                                          | 初始化 CoAP 协议栈 (WSAStartup)          |
| `void coap_cleanup(void)`                                                                                                                                                      | 清理 CoAP 协议栈 (WSACleanup)           |
| `SOCKET coap_open_socket(uint16_t port)`                                                                                                                                       | 创建 UDP 套接字并绑定端口；port=0 则不绑定 (客户端用) |
| `void coap_close_socket(SOCKET s)`                                                                                                                                             | 关闭 UDP 套接字                         |
| `int coap_build(uint8_t *buf, size_t buflen, const coap_msg_t *m)`                                                                                                             | 将逻辑报文构造为二进制字节流                     |
| `int coap_parse(const uint8_t *buf, size_t len, coap_msg_t *m)`                                                                                                                | 将二进制字节流解析为逻辑报文                     |
| `int coap_send(SOCKET s, const char *ip, uint16_t port, const uint8_t *data, size_t len)`                                                                                      | 发送 UDP 数据报                         |
| `int coap_recv(SOCKET s, uint8_t *buf, size_t buflen, char *from_ip, uint16_t *from_port, int timeout_ms)`                                                                     | 接收 UDP 数据报 (带超时)                   |
| `const char *coap_method_name(uint8_t code)`                                                                                                                                   | 获取请求方法名称字符串                        |
| `const char *coap_response_name(uint8_t code)`                                                                                                                                 | 获取响应码名称字符串                         |
| `int coap_add_option(coap_msg_t *m, uint16_t number, const uint8_t *value, size_t length)`                                                                                     | 向报文添加选项                            |
| `const coap_option_t *coap_find_option(const coap_msg_t *m, uint16_t number)`                                                                                                  | 查找指定编号的选项                          |
| `void coap_make_request(coap_msg_t *m, coap_type_t type, uint8_t code, const char *uri_path, const uint8_t *payload, size_t payload_len)`                                      | 构造请求报文                             |
| `void coap_make_response(coap_msg_t *m, coap_type_t type, uint8_t code, const uint8_t *token, uint8_t token_len, uint16_t msg_id, const uint8_t *payload, size_t payload_len)` | 构造响应报文                             |

***

## 3. device.c — CoAP 设备实现

### 3.1 宏定义

| 宏                      | 值    | 说明                |
| ---------------------- | ---- | ----------------- |
| `FW_FILLER_LEN`        | 2600 | 固件填充数据长度，确保需要分块传输 |
| `MAX_FW_VERSIONS`      | 10   | 最大固件版本历史数量        |
| `MAX_CONNECTIONS`      | 16   | 最大连接追踪数量          |
| `MAX_PEER_VERSIONS`    | 8    | 最大对端版本缓存数量        |
| `MAX_HISTORY_VERSIONS` | 5    | RD 注册时携带的历史版本数量   |

### 3.2 结构体

#### `conn_entry_t` — 连接追踪条目

```c
typedef struct {
    char    ip[64];        /* 对端 IP */
    uint16_t port;         /* 对端端口 */
    char    peer_id[16];   /* 对端 ID (从请求 from= 参数解析) */
    time_t  last_seen;     /* 最后一次请求时间 */
    int     active;        /* 是否活跃 */
} conn_entry_t;
```

#### `peer_ver_entry_t` — 对端版本缓存条目

```c
typedef struct {
    char    peer_id[16];   /* 对端 ID */
    char    version[32];   /* 最后已知版本号 */
    char    history[128];  /* 历史版本列表 (逗号分隔) */
    time_t  last_check;    /* 最后检查时间 */
    int     active;        /* 是否活跃 */
} peer_ver_entry_t;
```

#### `device_t` — 设备主结构

```c
typedef struct {
    char        id[16];              /* 设备 ID */
    uint16_t    port;                /* 服务器监听端口 */
    char        peer_ip[64];         /* 对端 IP (RD 查询失败时回退) */
    uint16_t    peer_port;           /* 对端端口 */
    char        peer_id[16];         /* 对端设备 ID */

    /* RD 服务器配置 */
    char        rd_ip[64];           /* RD 服务器 IP */
    uint16_t    rd_port;             /* RD 服务器端口 */
    int         rd_enabled;          /* 是否启用 RD 模式 */
    int         rd_registered;       /* 是否已成功注册 */
    int         auto_poll;           /* 自动轮询标志 */

    /* 多对端版本缓存 (用于 rd_check <id>) */
    peer_ver_entry_t peer_vers[MAX_PEER_VERSIONS];

    /* 连接追踪 (用于 get_link_id) */
    conn_entry_t connections[MAX_CONNECTIONS];

    /* 固件版本信息 */
    char        version[32];         /* 当前固件版本 */
    char        original_version[32]; /* 启动时的原始版本 */
    char        fw_path[64];         /* 当前固件文件路径 */
    char        fw_orig_path[64];    /* 原始固件文件路径 */
    char        fw_versions_dir[64]; /* 固件版本历史目录 */

    /* 日志文件 */
    char        log_path[64];        /* 应用日志路径 */
    char        proto_log_path[64];  /* 协议日志路径 */

    /* 固件版本历史记录 */
    char        fw_versions[MAX_FW_VERSIONS][32];      /* 历史版本号 */
    char        fw_version_times[MAX_FW_VERSIONS][32];  /* 历史版本时间戳 */
    int         fw_version_count;                       /* 当前历史版本数量 */

    /* 同步与网络 */
    CRITICAL_SECTION lock;           /* 临界区锁 */
    FILE       *log_fp;              /* 应用日志句柄 */
    FILE       *proto_log_fp;        /* 协议日志句柄 */
    SOCKET      srv_sock;            /* 服务器套接字 (绑定 port) */
    SOCKET      cli_sock;            /* 客户端套接字 (未绑定) */
    volatile int running;            /* 运行状态 */
    uint16_t    next_msg_id;         /* 下一个消息 ID */
} device_t;
```

### 3.3 函数列表

#### 日志与工具函数

| 函数签名                                                                                                                   | 说明                 |
| ---------------------------------------------------------------------------------------------------------------------- | ------------------ |
| `static void dev_log(device_t *d, const char *fmt, ...)`                                                               | 输出带时间戳的日志到控制台和日志文件 |
| `static void proto_log(device_t *d, const char *direction, const coap_msg_t *msg, const uint8_t *raw, size_t raw_len)` | 记录协议层报文到协议日志文件     |
| `static size_t read_fw_info(const char *fw_path, char *ver_buf, size_t ver_size)`                                      | 读取固件文件的版本号和文件大小    |

#### 固件版本管理函数

| 函数签名                                                                                                        | 说明                                          |
| ----------------------------------------------------------------------------------------------------------- | ------------------------------------------- |
| `static void save_fw_version_history_with_file(device_t *d, const char *version, const char *fw_file_path)` | 保存指定版本的固件副本到历史目录，并记录版本号和时间戳                 |
| `static int get_fw_version_list(device_t *d, char *buf, size_t buf_size)`                                   | 获取本设备所有历史固件版本列表                             |
| `static int find_fw_version_file(device_t *d, const char *version, char *path_buf, size_t buf_size)`        | 按版本号查找历史固件文件路径                              |
| `static int get_history_versions(device_t *d, char out[][32], int max_count)`                               | 获取最近 N 个历史版本号 (用于 RD 注册)                    |
| `static int build_links_with_history(device_t *d, char *buf, size_t buf_size)`                              | 构建包含 `ver` 和 `hver` 属性的 CoRE Link Format 负载 |
| `static int apply_firmware_update(device_t *d, const uint8_t *fw_data, size_t fw_len)`                      | 通用固件升级函数：保存旧版本→写入新固件→更新版本号→重新注册 RD          |

#### 连接追踪与版本缓存函数

| 函数签名                                                                                                         | 说明                                 |
| ------------------------------------------------------------------------------------------------------------ | ---------------------------------- |
| `static void track_connection(device_t *d, const char *ip, uint16_t port, const char *peer_id)`              | 记录或更新客户端连接信息                       |
| `static void cache_peer_version(device_t *d, const char *peer_id, const char *version, const char *history)` | 缓存对端版本号和历史版本列表                     |
| `static const char *get_cached_peer_version(device_t *d, const char *peer_id)`                               | 获取缓存的对端版本号                         |
| `static void print_connections(device_t *d)`                                                                 | 打印所有连接和对端版本缓存信息 (`get_link_id` 命令) |

#### 服务器线程函数

| 函数签名                                            | 说明                                                   |
| ----------------------------------------------- | ---------------------------------------------------- |
| `static DWORD WINAPI server_thread(LPVOID arg)` | 服务器后台线程：监听并处理 CoAP 请求 (`/fwinfo`、`/log`、`/firmware`) |

服务器处理的资源：

| URI         | 方法  | 说明                                                     |
| ----------- | --- | ------------------------------------------------------ |
| `/fwinfo`   | GET | 返回当前固件版本和大小；支持 `?list` 返回版本列表；支持 `?version=XXX` 返回历史固件 |
| `/log`      | GET | 返回日志文件内容；支持 `?start_time=&end_time=` 按时间过滤             |
| `/firmware` | GET | 返回当前固件文件 (Block2 分块传输)                                 |

#### RD 客户端操作函数

| 函数签名                                                                       | 说明                                                     |
| -------------------------------------------------------------------------- | ------------------------------------------------------ |
| `static int coap_exchange(device_t *d, coap_msg_t *req, coap_msg_t *resp)` | CoAP 请求-响应交换 (CON + 等待 ACK，含 1 次重传)；自动附加 `from=<自身ID>` |
| `static int client_rd_register(device_t *d)`                               | 向 RD 服务器注册资源 (POST /rd)，携带版本号和历史版本                     |
| `static int client_rd_update(device_t *d)`                                 | 更新 RD 注册 (PUT /rd/{id})，续期 TTL 并更新资源                   |
| `static int client_rd_deregister(device_t *d)`                             | 从 RD 注销 (DELETE /rd/{id})                              |
| `static int client_rd_lookup(device_t *d, const char *peer_id)`            | 查询指定对端的 IP/端口/版本/历史版本 (GET /rd?ep={id})                |
| `static int client_rd_lookup_by_resource(device_t *d, const char *rt)`     | 按资源类型查询所有端点 (GET /rd?res={rt})                         |
| `static int client_rd_check_version(device_t *d, const char *peer_id)`     | 检查指定对端版本是否有更新 (对比缓存版本)                                 |

#### 客户端资源操作函数

| 函数签名                                                                                                 | 说明                                           |
| ---------------------------------------------------------------------------------------------------- | -------------------------------------------- |
| `static int client_get_log_by_id(device_t *d, const char *peer_id)`                                  | 从指定对端获取日志文件 (GET /log)                       |
| `static int client_get_fw_by_id(device_t *d, const char *peer_id)`                                   | 从指定对端拉取最新固件并升级自身 (GET /firmware, Block2)     |
| `static int client_get_fw_by_version_upgrade(device_t *d, const char *peer_id, const char *version)` | 按版本号从对端拉取历史固件并升级自身 (GET /fwinfo?version=XXX) |
| `static int client_get_fwhis(device_t *d, const char *peer_id)`                                      | 查询指定对端的固件版本历史 (`get_fwhis` 命令)               |
| `static int client_pull_firmware(device_t *d)`                                                       | 从对端拉取固件 (Block2 分块接收)                        |
| `static void client_get_fwinfo(device_t *d)`                                                         | 获取对端固件信息 (GET /fwinfo)                       |
| `static void client_get_log(device_t *d)`                                                            | 获取对端日志 (GET /log)                            |
| `static void client_upgrade_firmware(device_t *d)`                                                   | 向对端推送固件升级 (PUT /firmware, Block1 分块发送)       |
| `static void client_get_fw_version_list(device_t *d)`                                                | 获取对端固件版本列表 (GET /fwinfo?list)                |
| `static void client_get_fw_by_version(device_t *d, const char *version)`                             | 按版本号获取对端固件文件 (GET /fwinfo?version=XXX)       |
| `static void client_get_log_by_time(device_t *d, const char *start_time, const char *end_time)`      | 按时间范围获取对端日志                                  |
| `static DWORD WINAPI poll_thread_func(LPVOID arg)`                                                   | 自动轮询线程 (未启用)                                 |

### 3.4 命令行指令

| 命令                      | 说明                       |
| ----------------------- | ------------------------ |
| `rd_register`           | 向 RD 服务器注册资源 (含历史版本)     |
| `rd_update`             | 更新 RD 注册 (TTL + 资源)      |
| `rd_deregister`         | 从 RD 注销                  |
| `rd_lookup <id>`        | 查询指定 ID 对端的 IP、端口、版本     |
| `rd_find <rt>`          | 按资源类型查找所有客户端             |
| `rd_check <id>`         | 检查指定 ID 对端版本是否有更新        |
| `get_link_id`           | 显示所有连接的客户端和版本缓存          |
| `get_log <id>`          | 从指定 ID 对端获取日志            |
| `get_fw <id>`           | 从指定 ID 对端拉取最新固件并升级自身     |
| `get_fw <version> <id>` | 按版本号从指定 ID 对端拉取历史固件并升级自身 |
| `get_fwhis <id>`        | 查看指定 ID 对端的固件版本历史        |
| `status`                | 显示当前设备状态                 |
| `help`                  | 显示帮助                     |
| `quit` / `exit`         | 退出 (自动注销 RD)             |

### 3.5 命令行参数

| 参数            | 说明                   | 示例                    |
| ------------- | -------------------- | --------------------- |
| `--id`        | 设备 ID                | `--id A`              |
| `--port`      | 服务器监听端口              | `--port 5683`         |
| `--peer-ip`   | 对端 IP (传统模式)         | `--peer-ip 127.0.0.1` |
| `--peer-port` | 对端端口 (传统模式)          | `--peer-port 5684`    |
| `--peer-id`   | 对端设备 ID (RD 模式)      | `--peer-id B`         |
| `--rd-ip`     | RD 服务器 IP (启用 RD 模式) | `--rd-ip 127.0.0.1`   |
| `--rd-port`   | RD 服务器端口             | `--rd-port 5685`      |
| `--version`   | 初始固件版本号              | `--version 1.0.0-A`   |

***

## 4. rd\_server.h / rd\_server.c — RD 资源目录服务器

### 4.1 宏定义

| 宏                     | 值    | 说明                    |
| --------------------- | ---- | --------------------- |
| `RD_DEFAULT_PORT`     | 5685 | RD 默认端口               |
| `RD_MAX_ENDPOINTS`    | 64   | 最大注册端点数               |
| `RD_MAX_LINKS`        | 32   | 每个端点最大资源链接数           |
| `RD_MAX_LINK_LEN`     | 256  | 单条 link-format 条目最大长度 |
| `RD_MAX_PAYLOAD`      | 2048 | RD 报文最大负载             |
| `RD_DEFAULT_TTL`      | 3600 | 默认 TTL (秒)            |
| `RD_CLEANUP_INTERVAL` | 60   | 过期清理间隔 (秒)            |

### 4.2 结构体

#### `rd_endpoint_t` — 端点注册结构

```c
typedef struct {
    char    ep[32];           /* 端点名称 (endpoint) */
    char    base[64];         /* 基础 URI (如 coap://127.0.0.1:5683) */
    char    domain[32];       /* 域 (可选) */
    uint32_t ttl;             /* 生存时间 (秒) */
    time_t  last_update;      /* 最后更新时间 */
    int     active;           /* 是否激活 (1=已注册, 0=空槽) */

    /* 资源链接列表 (CoRE Link Format) */
    struct {
        char    uri[64];      /* 资源 URI, 如 "/fwinfo" */
        char    rt[32];       /* 资源类型, 如 "version" */
        char    ifdesc[32];   /* 接口描述 (可选) */
        char    ver[32];      /* 版本属性 (可选) */
        char    hver[128];    /* 历史版本列表 (可选, 逗号分隔) */
    } links[RD_MAX_LINKS];
    int     link_count;       /* 链接数量 */
} rd_endpoint_t;
```

#### `rd_server_t` — RD 服务器上下文

```c
typedef struct {
    SOCKET  srv_sock;         /* 服务器 Socket */
    uint16_t port;            /* 监听端口 */
    volatile int running;     /* 运行标志 */
    uint32_t default_ttl;     /* 默认 TTL */

    rd_endpoint_t endpoints[RD_MAX_ENDPOINTS];  /* 端点注册表 */
    CRITICAL_SECTION lock;    /* 线程安全锁 */
} rd_server_t;
```

### 4.3 函数列表

#### 内部函数

| 函数签名                                                                                                                           | 说明                                              |
| ------------------------------------------------------------------------------------------------------------------------------ | ----------------------------------------------- |
| `static int parse_links_payload(const uint8_t *payload, size_t len, rd_endpoint_t *ep)`                                        | 解析 CoRE Link Format 负载，提取资源 URI、rt、ver、hver 等属性 |
| `static int find_endpoint_index(rd_server_t *rd, const char *ep)`                                                              | 按端点名称查找注册表索引                                    |
| `static int alloc_slot(rd_server_t *rd)`                                                                                       | 分配一个空闲的端点注册槽位                                   |
| `static void cleanup_expired(rd_server_t *rd)`                                                                                 | 清理过期的端点注册                                       |
| `static void handle_rd_request(rd_server_t *rd, const coap_msg_t *req, coap_msg_t *resp, uint8_t *resp_buf, size_t *resp_len)` | 处理 RD 请求 (POST/PUT/DELETE/GET)                  |
| `static DWORD WINAPI rd_server_thread(LPVOID arg)`                                                                             | RD 服务器后台线程                                      |

#### 公开 API

| 函数签名                                                                                                                                                | 说明                 |
| --------------------------------------------------------------------------------------------------------------------------------------------------- | ------------------ |
| `int rd_server_init(rd_server_t *rd, uint16_t port, uint32_t default_ttl)`                                                                          | 初始化 RD 服务器         |
| `void rd_server_cleanup(rd_server_t *rd)`                                                                                                           | 清理 RD 服务器资源        |
| `int rd_server_start(rd_server_t *rd)`                                                                                                              | 启动 RD 服务器 (创建后台线程) |
| `void rd_server_stop(rd_server_t *rd)`                                                                                                              | 停止 RD 服务器          |
| `int rd_register(rd_server_t *rd, const char *ep, const char *base, const char *domain, uint32_t ttl, const char *links_payload, size_t links_len)` | 注册新端点              |
| `int rd_update(rd_server_t *rd, const char *ep, const char *links_payload, size_t links_len, uint32_t ttl)`                                         | 更新端点注册 (续期 + 资源)   |
| `int rd_delete(rd_server_t *rd, const char *ep)`                                                                                                    | 删除端点注册             |
| `int rd_search_by_resource(rd_server_t *rd, const char *rt_filter, char *out_buf, size_t buf_size)`                                                 | 按资源类型查询端点          |
| `int rd_search_by_endpoint(rd_server_t *rd, const char *ep_filter, char *out_buf, size_t buf_size)`                                                 | 按端点名称查询            |
| `int rd_search_by_domain(rd_server_t *rd, const char *domain_filter, char *out_buf, size_t buf_size)`                                               | 按域查询               |

### 4.4 RD 服务器处理的 HTTP 接口

| URI                 | 方法     | 说明                                     |
| ------------------- | ------ | -------------------------------------- |
| `/rd`               | POST   | 注册新端点 (带资源列表)                          |
| `/rd/{endpoint}`    | PUT    | 更新端点注册 (续期/修改)                         |
| `/rd/{endpoint}`    | DELETE | 删除端点注册                                 |
| `/rd`               | GET    | 查询目录 (支持 `?ep=`、`?res=`、`?domain=` 过滤) |
| `/rd/{endpoint}`    | GET    | 查询指定端点信息                               |
| `/.well-known/core` | GET    | RD 自身资源发现                              |

***

## 5. 固件升级流程

### 5.1 固件资源暴露方式

服务端通过 RD 服务器更新资源目录，客户端发现固件版本更新后主动 GET 获取新固件。

### 5.2 RD 注册的 Link Format 示例

```
</fwinfo>;rt="version";ver="2.0.0-C";hver="1.0.0-B,1.0.0-A,1.0.0-C",</log>;rt="log",</firmware>;rt="fw"
```

- `ver` — 当前固件版本号
- `hver` — 历史版本列表 (逗号分隔，最多 5 个)

### 5.3 固件升级流程 (`get_fw <id>`)

```
设备 A                          RD 服务器                        设备 B
  |                                |                                |
  |--- GET /rd?ep=B ------------->|                                |
  |<-- 200 (B 的 IP:port:ver) ----|                                |
  |                                |                                |
  |--- GET /firmware (Block2) ------------------------------->|   |
  |<-- 2.05 Content (Block2 分块) <---------------------------|   |
  |    ... (循环拉取所有分块) ...                                 |
  |                                |                                |
  | [apply_firmware_update]        |                                |
  |  1. 保存旧版本到历史目录        |                                |
  |  2. 写入新固件文件              |                                |
  |  3. 更新版本号                  |                                |
  |  4. 重新注册 RD -------------->|                                |
  |<-- 2.01 Created ---------------|                                |
```

### 5.4 历史固件拉取流程 (`get_fw <version> <id>`)

```
设备 A                          设备 B
  |                                |
  |--- GET /rd?ep=B ------------->| (通过 RD 解析地址)
  |                                |
  |--- GET /fwinfo?version=1.0.0-B ----->|
  |<-- 2.05 Content (固件文件) <---------|
  |                                |
  | [apply_firmware_update]        |
  |  保存旧版本 → 写入新固件 → 更新版本 → 重新注册 RD
```

***

## 6. 目录结构

```
coap/
├── coap.h                  # CoAP 协议栈头文件
├── coap.c                  # CoAP 协议栈实现
├── device.c                # CoAP 设备实现
├── device.exe              # 设备可执行程序
├── rd_server.h             # RD 服务器头文件
├── rd_server.c             # RD 服务器实现
├── rd_server.exe           # RD 服务器可执行程序
├── A_bin/                  # 设备 A 的固件文件
│   ├── firmware_A.bin      # 当前固件
│   ├── firmware_A_orig.bin # 原始固件 (用于升级对端)
│   └── versions/           # 历史固件版本目录
├── A_log/                  # 设备 A 的日志
│   ├── device_A.log        # 应用日志
│   └── proto_A.log         # 协议日志
├── B_bin/                  # 设备 B 的固件文件
│   ├── firmware_B.bin
│   ├── firmware_B_orig.bin
│   └── versions/
├── B_log/                  # 设备 B 的日志
│   ├── device_B.log
│   └── proto_B.log
├── build_rd.bat            # 编译脚本
└── run_demo.ps1            # 演示启动脚本
```

