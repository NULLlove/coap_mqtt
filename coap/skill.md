# CoAP 固件管理与升级系统 — Skill 文档

## 1. 项目简介

本项目基于 CoAP 协议 (RFC 7252) 实现了一个嵌入式设备固件管理与升级系统，运行于 Windows 平台，使用 C 语言和 Winsock2 编写，无第三方库依赖。核心能力包括：

- CoAP 协议栈：报文构造/解析、UDP 收发、Block1/Block2 分块传输 (RFC 7959)
- RD 资源目录 (RFC 9176)：集中式设备注册、资源发现、版本跟踪
- 固件版本管理：当前版本 + 历史版本 (最多 5 个)，支持按版本号拉取历史固件
- 设备间通信：日志获取、固件拉取升级、版本变更检测、连接追踪

### 技术栈

| 项 | 说明 |
|----|------|
| 语言 | C (C99) |
| 平台 | Windows (Winsock2) |
| 编译器 | GCC (MinGW) |
| 依赖 | 仅 `-lws2_32`，无第三方库 |
| 协议标准 | RFC 7252, RFC 6690, RFC 7959, RFC 9176 |

---

## 2. 文件结构

```
coap/
├── coap.h                  # CoAP 协议栈头文件
├── coap.c                  # CoAP 协议栈实现 (报文构造/解析/UDP收发)
├── device.c                # CoAP 设备实现 (服务器+客户端+固件管理)
├── rd_server.h             # RD 服务器头文件
├── rd_server.c             # RD 服务器实现 (端点注册/查询/清理)
├── device.exe              # 设备可执行程序
├── rd_server.exe           # RD 服务器可执行程序
├── build_rd.bat            # 编译脚本 (一键编译)
├── run_demo.ps1            # 演示启动脚本 (一键启动3进程)
├── A_bin/                  # 设备A固件文件目录
│   ├── firmware_A.bin      # 当前固件
│   ├── firmware_A_orig.bin # 原始固件 (用于升级对端)
│   └── versions/           # 历史固件版本目录
├── A_log/                  # 设备A日志目录
│   ├── device_A.log        # 应用日志
│   └── proto_A.log         # 协议日志 (CoAP报文记录)
├── B_bin/                  # 设备B固件文件目录
│   └── versions/           # 历史固件版本
└── B_log/                  # 设备B日志目录
```

---

## 3. 编译与运行

### 3.1 编译

```bash
# 方式1: 使用编译脚本
build_rd.bat

# 方式2: 手动编译
gcc -Wall -Wextra -O2 -o rd_server.exe coap.c rd_server.c -lws2_32
gcc -Wall -Wextra -O2 -o device.exe coap.c device.c -lws2_32
```

### 3.2 启动 (RD 模式，推荐)

```bash
# 方式1: 一键启动 (PowerShell)
powershell -ExecutionPolicy Bypass -File .\run_demo.ps1

# 方式2: 手动启动3个终端
# 终端1: RD服务器
rd_server.exe --port 5685 --ttl 3600

# 终端2: 设备A
device.exe --id A --port 5683 --peer-id B --rd-ip 127.0.0.1 --rd-port 5685 --version 1.0.0-A

# 终端3: 设备B
device.exe --id B --port 5684 --peer-id A --rd-ip 127.0.0.1 --rd-port 5685 --version 1.0.0-B
```

### 3.3 启动 (直连模式，无 RD)

```bash
device.exe --id A --port 5683 --peer-ip 127.0.0.1 --peer-port 5684 --version 1.0.0-A
device.exe --id B --port 5684 --peer-ip 127.0.0.1 --peer-port 5683 --version 1.0.0-B
```

### 3.4 命令行参数

| 参数 | 说明 | 示例 |
|------|------|------|
| `--id` | 设备 ID (必填) | `--id A` |
| `--port` | 服务器监听端口 (必填) | `--port 5683` |
| `--peer-id` | 对端设备 ID (RD 模式) | `--peer-id B` |
| `--rd-ip` | RD 服务器 IP (启用 RD 模式) | `--rd-ip 127.0.0.1` |
| `--rd-port` | RD 服务器端口 | `--rd-port 5685` |
| `--version` | 初始固件版本号 | `--version 1.0.0-A` |
| `--peer-ip` | 对端 IP (直连模式) | `--peer-ip 127.0.0.1` |
| `--peer-port` | 对端端口 (直连模式) | `--peer-port 5684` |

---

## 4. 架构设计

### 4.1 整体架构

```
┌─────────────────────────────────────────────────────────────┐
│                        RD 服务器 (:5685)                     │
│  ┌─────────────────────────────────────────────────────┐    │
│  │  端点注册表 endpoints[64]                            │    │
│  │  每个 endpoint: ep, base, ttl, links[]               │    │
│  │  每个 link: uri, rt, ver, hver                        │    │
│  └─────────────────────────────────────────────────────┘    │
│  POST /rd → 注册  PUT /rd/{ep} → 更新  DELETE /rd/{ep} → 注销│
│  GET /rd?ep=B → 按端点查询  GET /rd?res=version → 按类型查询 │
└──────────────────────────┬──────────────────────────────────┘
       注册 ↑  查询 ↓        │         注册 ↑  查询 ↓
┌──────────┴──────────┐    │    ┌──────────────────────┴──────┐
│     设备 A (:5683)   │    │    │     设备 B (:5684)           │
│  ┌─────────────────┐ │    │    │  ┌─────────────────┐        │
│  │ server_thread   │ │    │    │  │ server_thread   │        │
│  │ (后台线程)       │ │    │    │  │ (后台线程)       │        │
│  │ GET /fwinfo     │ │    │    │  │ GET /fwinfo     │        │
│  │ GET /log        │ │    │    │  │ GET /log        │        │
│  │ GET /firmware   │ │    │    │  │ GET /firmware   │        │
│  │ PUT /firmware   │ │    │    │  │ PUT /firmware   │        │
│  └─────────────────┘ │    │    │  └─────────────────┘        │
│  ┌─────────────────┐ │    │    │  ┌─────────────────┐        │
│  │ 主线程 (命令行)  │ │    │    │  │ 主线程 (命令行)  │        │
│  │ rd_register     │ │    │    │  │ rd_register     │        │
│  │ rd_lookup <id>  │────┘    └──│ rd_lookup <id>  │        │
│  │ get_fw <id>     │───────────>│ get_fw <id>     │        │
│  │ get_log <id>    │           │ get_log <id>    │        │
│  └─────────────────┘           └─────────────────┘        │
└───────────────────────────────┘ └────────────────────────────┘
```

### 4.2 设备双角色模型

每台设备同时扮演两个角色：

- **服务器** ([server_thread](file:///e:/project/coap/device.c#L560))：后台线程，监听 UDP 端口，处理 `/fwinfo`、`/log`、`/firmware` 请求
- **客户端** (主线程)：交互式命令行，通过 `coap_exchange` 向对端或 RD 服务器发起请求

两个角色共享同一个 `device_t` 结构，通过 `CRITICAL_SECTION lock` 保护线程安全。

---

## 5. 核心结构体

### 5.1 coap_msg_t — CoAP 报文逻辑表示 (coap.h:132)

```c
typedef struct {
    coap_type_t type;          // 报文类型: CON/NON/ACK/RST
    uint8_t     code;          // 请求方法或响应码
    uint16_t    msg_id;        // 消息 ID (用于去重和匹配)
    uint8_t     token[8];      // Token 值
    uint8_t     token_len;     // Token 长度

    // 便捷字段 (解析时自动填充)
    char        uri_path[64];  // URI 路径, 如 "fwinfo"
    char        uri_query[128];// URI 查询参数, 如 "version=1.0.0"
    int         content_format;// 内容格式

    // Block1: 请求分块 (客户端→服务端)
    int has_block1, block1_num, block1_more, block1_szx;

    // Block2: 响应分块 (服务端→客户端)
    int has_block2, block2_num, block2_more, block2_szx;

    // 通用选项数组
    coap_option_t options[16]; // 最多 16 个选项
    int           option_count;

    // Payload
    uint8_t        payload_buf[2048]; // 内部缓冲区
    const uint8_t *payload;           // 负载指针
    size_t         payload_len;       // 负载长度
} coap_msg_t;
```

### 5.2 device_t — 设备主结构 (device.c:56)

```c
typedef struct {
    char     id[16];           // 设备 ID
    uint16_t port;             // 服务器监听端口
    char     peer_ip[64];      // 对端 IP (RD 查询失败时回退)
    uint16_t peer_port;        // 对端端口
    char     peer_id[16];      // 对端设备 ID

    // RD 配置
    char     rd_ip[64];        // RD 服务器 IP
    uint16_t rd_port;          // RD 服务器端口
    int      rd_enabled;       // 是否启用 RD 模式
    int      rd_registered;    // 是否已成功注册

    // 多对端版本缓存 (rd_check <id>)
    peer_ver_entry_t peer_vers[8];

    // 连接追踪 (get_link_id)
    conn_entry_t connections[16];

    // 固件信息
    char version[32];          // 当前固件版本
    char original_version[32]; // 启动时的原始版本
    char fw_path[64];          // 当前固件文件路径
    char fw_orig_path[64];     // 原始固件文件路径
    char fw_versions_dir[64];  // 历史固件目录

    // 固件版本历史
    char fw_versions[10][32];      // 历史版本号
    char fw_version_times[10][32]; // 历史版本时间戳
    int  fw_version_count;         // 当前历史版本数量

    // 同步与网络
    CRITICAL_SECTION lock;     // 临界区锁
    FILE  *log_fp;             // 应用日志句柄
    FILE  *proto_log_fp;       // 协议日志句柄
    SOCKET srv_sock;           // 服务器套接字 (绑定 port)
    SOCKET cli_sock;           // 客户端套接字 (未绑定)
    volatile int running;      // 运行状态
    uint16_t next_msg_id;      // 下一个消息 ID
} device_t;
```

### 5.3 conn_entry_t — 连接追踪条目 (device.c:39)

```c
typedef struct {
    char     ip[64];       // 对端 IP
    uint16_t port;         // 对端端口
    char     peer_id[16];  // 对端 ID (从请求 from= 参数解析)
    time_t   last_seen;    // 最后一次请求时间
    int      active;       // 是否活跃
} conn_entry_t;
```

### 5.4 peer_ver_entry_t — 对端版本缓存条目 (device.c:48)

```c
typedef struct {
    char   peer_id[16];   // 对端 ID
    char   version[32];   // 最后已知版本号
    char   history[128];  // 历史版本列表 (逗号分隔)
    time_t last_check;    // 最后检查时间
    int    active;        // 是否活跃
} peer_ver_entry_t;
```

### 5.5 rd_endpoint_t — RD 端点注册结构 (rd_server.h:33)

```c
typedef struct {
    char     ep[32];          // 端点名称
    char     base[64];        // 基础 URI (coap://ip:port)
    char     domain[32];      // 域 (可选)
    uint32_t ttl;             // 生存时间 (秒)
    time_t   last_update;     // 最后更新时间
    int      active;          // 是否激活

    // 资源链接列表 (CoRE Link Format)
    struct {
        char uri[64];         // 资源 URI, 如 "/fwinfo"
        char rt[32];          // 资源类型, 如 "version"
        char ifdesc[32];      // 接口描述
        char ver[32];         // 版本属性
        char hver[128];       // 历史版本列表 (逗号分隔)
    } links[32];
    int link_count;           // 链接数量
} rd_endpoint_t;
```

### 5.6 rd_server_t — RD 服务器上下文 (rd_server.h:53)

```c
typedef struct {
    SOCKET       srv_sock;                    // 服务器 Socket
    uint16_t     port;                        // 监听端口
    volatile int running;                     // 运行标志
    uint32_t     default_ttl;                 // 默认 TTL
    rd_endpoint_t endpoints[64];              // 端点注册表
    CRITICAL_SECTION lock;                    // 线程安全锁
} rd_server_t;
```

---

## 6. 函数清单

### 6.1 coap.c — 协议栈函数

| 函数 | 位置 | 说明 |
|------|------|------|
| `coap_init()` | [coap.c:20](file:///e:/project/coap/coap.c#L20) | 初始化 Winsock2 |
| `coap_cleanup()` | [coap.c:27](file:///e:/project/coap/coap.c#L27) | 清理 Winsock2 |
| `coap_open_socket(port)` | [coap.c:33](file:///e:/project/coap/coap.c#L33) | 创建 UDP 套接字并绑定端口 |
| `coap_close_socket(s)` | [coap.c:55](file:///e:/project/coap/coap.c#L55) | 关闭套接字 |
| `coap_send(s, ip, port, data, len)` | [coap.c:60](file:///e:/project/coap/coap.c#L60) | 发送 UDP 数据报 |
| `coap_recv(s, buf, buflen, ip, port, timeout)` | [coap.c:72](file:///e:/project/coap/coap.c#L72) | 接收 UDP 数据报 (带超时) |
| `coap_build(buf, buflen, msg)` | [coap.c:134](file:///e:/project/coap/coap.c#L134) | 将逻辑报文构造为二进制字节流 |
| `coap_parse(buf, len, msg)` | [coap.c:276](file:///e:/project/coap/coap.c#L276) | 将二进制字节流解析为逻辑报文 |
| `coap_method_name(code)` | [coap.c:386](file:///e:/project/coap/coap.c#L386) | 获取请求方法名称 |
| `coap_response_name(code)` | [coap.c:396](file:///e:/project/coap/coap.c#L396) | 获取响应码名称 |
| `coap_add_option(msg, num, val, len)` | [coap.c:437](file:///e:/project/coap/coap.c#L437) | 向报文添加选项 |
| `coap_find_option(msg, num)` | [coap.c:457](file:///e:/project/coap/coap.c#L457) | 查找指定编号的选项 |
| `coap_make_request(msg, type, code, uri, payload, len)` | [coap.c:475](file:///e:/project/coap/coap.c#L475) | 构造请求报文 |
| `coap_make_response(msg, type, code, token, tkl, msgid, payload, len)` | [coap.c:503](file:///e:/project/coap/coap.c#L503) | 构造响应报文 |

### 6.2 device.c — 设备函数

#### 日志与工具

| 函数 | 位置 | 说明 |
|------|------|------|
| `dev_log(d, fmt, ...)` | [device.c:103](file:///e:/project/coap/device.c#L103) | 输出带时间戳的日志到控制台和文件 |
| `proto_log(d, dir, msg, raw, len)` | [device.c:132](file:///e:/project/coap/device.c#L132) | 记录 CoAP 报文到协议日志文件 |
| `read_fw_info(path, ver, size)` | [device.c:198](file:///e:/project/coap/device.c#L198) | 读取固件文件版本号和大小 |

#### 固件版本管理

| 函数 | 位置 | 说明 |
|------|------|------|
| `save_fw_version_history_with_file(d, ver, path)` | [device.c:213](file:///e:/project/coap/device.c#L213) | 保存固件副本到历史目录 |
| `get_fw_version_list(d, buf, size)` | [device.c:262](file:///e:/project/coap/device.c#L262) | 获取所有历史固件版本列表 |
| `find_fw_version_file(d, ver, buf, size)` | [device.c:281](file:///e:/project/coap/device.c#L281) | 按版本号查找历史固件文件路径 |
| `get_history_versions(d, out, max)` | [device.c:475](file:///e:/project/coap/device.c#L475) | 获取最近 N 个历史版本号 |
| `build_links_with_history(d, buf, size)` | [device.c:489](file:///e:/project/coap/device.c#L489) | 构建含 ver/hver 的 Link Format |
| `apply_firmware_update(d, data, len)` | [device.c:516](file:///e:/project/coap/device.c#L516) | 通用固件升级: 保存旧版本→写入新固件→更新版本→重注册RD |

#### 连接追踪与版本缓存

| 函数 | 位置 | 说明 |
|------|------|------|
| `track_connection(d, ip, port, id)` | [device.c:390](file:///e:/project/coap/device.c#L390) | 记录或更新客户端连接信息 |
| `cache_peer_version(d, id, ver, history)` | [device.c:423](file:///e:/project/coap/device.c#L423) | 缓存对端版本号和历史版本 |
| `get_cached_peer_version(d, id)` | [device.c:458](file:///e:/project/coap/device.c#L458) | 获取缓存的对端版本号 |
| `print_connections(d)` | [device.c:2069](file:///e:/project/coap/device.c#L2069) | 打印连接和版本缓存 (get_link_id) |

#### 服务器线程

| 函数 | 位置 | 说明 |
|------|------|------|
| `server_thread(arg)` | [device.c:560](file:///e:/project/coap/device.c#L560) | 后台线程: 监听并处理 CoAP 请求 |

服务器处理的资源:

| URI | 方法 | 说明 |
|-----|------|------|
| `/fwinfo` | GET | 当前固件版本和大小; `?list` 返回版本列表; `?version=XXX` 返回历史固件 |
| `/log` | GET | 日志文件内容; `?start_time=&end_time=` 按时间过滤 |
| `/firmware` | GET | 当前固件文件 (Block2 分块传输) |
| `/firmware` | PUT | 接收固件分块 (Block1), 写入固件文件 |

#### RD 客户端操作

| 函数 | 位置 | 说明 |
|------|------|------|
| `coap_exchange(d, req, resp)` | [device.c:1481](file:///e:/project/coap/device.c#L1481) | 请求-响应交换 (CON+ACK+重传), 自动附加 from=ID |
| `client_rd_register(d)` | [device.c:970](file:///e:/project/coap/device.c#L970) | 向 RD 注册资源 (POST /rd), 携带版本和历史版本 |
| `client_rd_update(d)` | [device.c:1028](file:///e:/project/coap/device.c#L1028) | 更新 RD 注册 (PUT /rd/{id}), 续期 TTL |
| `client_rd_deregister(d)` | [device.c:1076](file:///e:/project/coap/device.c#L1076) | 从 RD 注销 (DELETE /rd/{id}) |
| `client_rd_lookup(d, id)` | [device.c:1119](file:///e:/project/coap/device.c#L1119) | 查询指定对端 IP/端口/版本 (GET /rd?ep={id}) |
| `client_rd_lookup_by_resource(d, rt)` | [device.c:1249](file:///e:/project/coap/device.c#L1249) | 按资源类型查询 (GET /rd?res={rt}) |
| `client_rd_check_version(d, id)` | [device.c:1307](file:///e:/project/coap/device.c#L1307) | 检查指定对端版本是否有更新 |

#### 客户端资源操作

| 函数 | 位置 | 说明 |
|------|------|------|
| `client_get_log_by_id(d, id)` | [device.c:1857](file:///e:/project/coap/device.c#L1857) | 从指定对端获取日志 |
| `client_get_fw_by_id(d, id)` | [device.c:1893](file:///e:/project/coap/device.c#L1893) | 拉取最新固件并升级自身 (Block2) |
| `client_get_fw_by_version_upgrade(d, id, ver)` | [device.c:1956](file:///e:/project/coap/device.c#L1956) | 按版本号拉取历史固件并升级自身 |
| `client_get_fwhis(d, id)` | [device.c:2026](file:///e:/project/coap/device.c#L2026) | 查询对端固件版本历史 |
| `client_pull_firmware(d)` | [device.c:1340](file:///e:/project/coap/device.c#L1340) | 从对端拉取固件 (Block2 分块接收) |
| `client_get_fwinfo(d)` | [device.c:1558](file:///e:/project/coap/device.c#L1558) | 获取对端固件信息 (直连模式) |
| `client_get_log(d)` | [device.c:1585](file:///e:/project/coap/device.c#L1585) | 获取对端日志 (直连模式) |
| `client_upgrade_firmware(d)` | [device.c:1639](file:///e:/project/coap/device.c#L1639) | 向对端推送固件 (Block1 分块发送) |

### 6.3 rd_server.c — RD 服务器函数

| 函数 | 位置 | 说明 |
|------|------|------|
| `parse_links_payload(payload, len, ep)` | [rd_server.c:21](file:///e:/project/coap/rd_server.c#L21) | 解析 CoRE Link Format, 提取 uri/rt/ver/hver |
| `find_endpoint_index(rd, ep)` | [rd_server.c:119](file:///e:/project/coap/rd_server.c#L119) | 按名称查找端点索引 |
| `alloc_slot(rd)` | [rd_server.c:129](file:///e:/project/coap/rd_server.c#L129) | 分配空闲端点槽位 |
| `cleanup_expired(rd)` | [rd_server.c:137](file:///e:/project/coap/rd_server.c#L137) | 清理过期端点 |
| `handle_rd_request(rd, req, resp, buf, len)` | [rd_server.c:429](file:///e:/project/coap/rd_server.c#L429) | 处理 RD 请求 (POST/PUT/DELETE/GET) |
| `rd_server_thread(arg)` | [rd_server.c:634](file:///e:/project/coap/rd_server.c#L634) | RD 服务器后台线程 |
| `rd_server_init(rd, port, ttl)` | [rd_server.c:395](file:///e:/project/coap/rd_server.c#L395) | 初始化 RD 服务器 |
| `rd_server_start(rd)` | [rd_server.c:660](file:///e:/project/coap/rd_server.c#L660) | 启动 RD 服务器 |
| `rd_server_stop(rd)` | [rd_server.c:678](file:///e:/project/coap/rd_server.c#L678) | 停止 RD 服务器 |
| `rd_register(rd, ep, base, domain, ttl, links, len)` | [rd_server.c:161](file:///e:/project/coap/rd_server.c#L161) | 注册新端点 |
| `rd_update(rd, ep, links, len, ttl)` | [rd_server.c:208](file:///e:/project/coap/rd_server.c#L208) | 更新端点注册 |
| `rd_delete(rd, ep)` | [rd_server.c:232](file:///e:/project/coap/rd_server.c#L232) | 删除端点注册 |
| `rd_search_by_resource(rd, rt, buf, size)` | [rd_server.c:256](file:///e:/project/coap/rd_server.c#L256) | 按资源类型查询 |
| `rd_search_by_endpoint(rd, ep, buf, size)` | [rd_server.c:306](file:///e:/project/coap/rd_server.c#L306) | 按端点名称查询 |
| `rd_search_by_domain(rd, domain, buf, size)` | [rd_server.c:357](file:///e:/project/coap/rd_server.c#L357) | 按域查询 |

---

## 7. 命令行指令

### 7.1 RD 服务器命令

| 命令 | 说明 |
|------|------|
| `rd_register` | 向 RD 服务器注册资源，携带当前版本号 `ver` 和历史版本列表 `hver` |
| `rd_update` | 更新 TTL 和资源，同时更新历史固件版本 |
| `rd_deregister` | 从 RD 注销本设备资源 |
| `rd_lookup <id>` | 查询指定 ID 对端的 IP、端口、版本、历史版本 |
| `rd_find <rt>` | 按资源类型查找所有有该类型资源的客户端 |
| `rd_check <id>` | 检查指定 ID 对端版本是否有更新 (对比缓存版本) |

### 7.2 资源操作命令

| 命令 | 说明 |
|------|------|
| `get_link_id` | 查看所有连接的客户端 (IP、端口、ID、最后访问时间) 及对端版本缓存 |
| `get_log <id>` | 从指定 ID 对端获取日志文件 |
| `get_fw <id>` | 从指定 ID 对端拉取最新固件并升级自身 |
| `get_fw <版本号> <id>` | 按版本号从指定 ID 对端拉取历史固件并升级自身 |
| `get_fwhis <id>` | 查看指定 ID 对端的固件版本历史 |

### 7.3 状态命令

| 命令 | 说明 |
|------|------|
| `status` | 显示当前设备状态 (ID、端口、版本、历史版本数、RD 状态) |
| `help` | 显示命令帮助 |
| `quit` / `exit` | 退出设备 (自动注销 RD) |

---

## 8. 关键流程

### 8.1 RD 注册流程

```
设备 A                          RD 服务器
  |                                |
  |  POST /rd                      |
  |  Payload: </fwinfo>;rt="version";ver="1.0.0-A";hver="...",
  |           </log>;rt="log",</firmware>;rt="fw"              |
  |  Uri-Query: ep=A;base=coap://127.0.0.1:5683;lt=3600       |
  |------------------------------->|
  |                                |  解析 links, 存储 endpoint
  |  2.01 Created                  |
  |<-------------------------------|
```

### 8.2 固件升级流程 (get_fw <id>)

```
设备 A                          RD 服务器                        设备 B
  |                                |                                |
  |--- GET /rd?ep=B ------------->|                                |
  |<-- 2.05 (B 的 IP:port:ver) ----|                                |
  |                                |                                |
  |--- GET /firmware (Block2) --------------------------------->|   |
  |<-- 2.05 Content (Block2 分块) <-----------------------------|   |
  |    ... (循环拉取所有分块) ...                                 |
  |                                |                                |
  | [apply_firmware_update]        |                                |
  |  1. 保存旧版本到 versions/     |                                |
  |  2. 写入新固件文件              |                                |
  |  3. 更新版本号                  |                                |
  |  4. 重新注册 RD -------------->|                                |
  |<-- 2.01 Created ---------------|                                |
```

### 8.3 版本检查流程 (rd_check <id>)

```
设备 A                          RD 服务器
  |                                |
  |--- GET /rd?ep=B ------------->|
  |<-- 2.05 (ver="2.0.0-C") ------|
  |                                |
  |  对比缓存版本:                 |
  |  缓存=1.0.0-B, RD返回=2.0.0-C |
  |  不一致 → 版本有更新           |
  |  更新缓存 → 2.0.0-C           |
```

### 8.4 Block2 分块传输机制

```
客户端                                    服务端
  |                                         |
  |--- GET /firmware ---------------------->|
  |                                         |  读取固件文件
  |<-- 2.05 Content (Block2: num=0, more=1) |
  |    payload = 第0块 (256字节)            |
  |                                         |
  |--- GET /firmware (Block2: num=1) ------>|
  |<-- 2.05 Content (Block2: num=1, more=1) |
  |    payload = 第1块 (256字节)            |
  |                                         |
  |    ... 循环直到 more=0 ...              |
  |                                         |
  |--- GET /firmware (Block2: num=N) ------>|
  |<-- 2.05 Content (Block2: num=N, more=0) |
  |    payload = 最后一块                   |
  |                                         |
  |  组装所有块 → 完整固件文件              |
```

### 8.5 CoRE Link Format 示例

```
</fwinfo>;rt="version";ver="2.0.0-C";hver="1.0.0-B,1.0.0-A,1.0.0-C",</log>;rt="log",</firmware>;rt="fw"
```

- `rt` — 资源类型
- `ver` — 当前固件版本号
- `hver` — 历史版本列表 (逗号分隔，最多 5 个)

---

## 9. CoAP 报文格式

### 9.1 报文头 (4 字节)

```
 0                   1                   2                   3
 0 1 2 3 4 5 6 7 8 9 0 1 2 3 4 5 6 7 8 9 0 1 2 3 4 5 6 7 8 9 0 1
+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
|Ver=1| T |  TKL  |     Code    |          Message ID           |
+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
|   Token (if any, TKL bytes) ...
+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
|   Options (if any) ...
+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
|1 1 1 1 1 1 1 1|    Payload (if any) ...
+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
```

| 字段 | 位数 | 说明 |
|------|------|------|
| Ver | 2 | 协议版本, 固定为 1 |
| T | 2 | 报文类型: 0=CON, 1=NON, 2=ACK, 3=RST |
| TKL | 4 | Token 长度 (0-8) |
| Code | 8 | 请求方法 (0.01-0.04) 或响应码 (2.xx/4.xx/5.xx) |
| Message ID | 16 | 消息 ID, 用于去重和匹配请求/响应 |

### 9.2 Block1/Block2 选项编码

```
Block 选项值 (1-3 字节可变长整数):
  +-+-+-+-+-+-+-+-+-+-+-+-+-+
  |  NUM  | M |    SZX     |
  +-+-+-+-+-+-+-+-+-+-+-+-+-+

  NUM  - 分块编号
  M    - 是否还有更多分块 (1=有, 0=最后一块)
  SZX  - 分块大小指数, 实际大小 = 2^(SZX+4)
         本项目 SZX=4, 即 256 字节/块
```

---

## 10. 线程模型

```
设备进程
├── 主线程 (命令行交互)
│   ├── 解析用户输入
│   ├── 调用 client_rd_* / client_get_* 函数
│   ├── 通过 cli_sock 发送 CoAP 请求
│   └── 退出时自动注销 RD
│
└── server_thread (后台线程)
    ├── 循环调用 coap_recv (srv_sock, 500ms 超时)
    ├── 解析请求, 路由到 /fwinfo /log /firmware
    ├── 追踪连接 (track_connection)
    ├── 记录协议日志 (proto_log)
    └── 构造并发送响应

RD 服务器进程
└── rd_server_thread (后台线程)
    ├── 循环调用 coap_recv (srv_sock, 1000ms 超时)
    ├── 路由到 handle_rd_request
    │   ├── POST /rd → rd_register
    │   ├── PUT /rd/{ep} → rd_update
    │   ├── DELETE /rd/{ep} → rd_delete
    │   └── GET /rd?ep= /res= /domain= → rd_search_*
    └── 定期调用 cleanup_expired 清理过期端点
```

线程安全通过 `CRITICAL_SECTION` 保护共享数据 (device_t 字段、RD 端点注册表)。

---

## 11. 抓包调试

### 11.1 Wireshark 过滤

```
# 过滤 CoAP 流量
coap

# 过滤指定端口
udp.port == 5683 || udp.port == 5684 || udp.port == 5685

# 过滤指定设备的流量
udp.port == 5683 && udp.port == 5684
```

### 11.2 协议日志

每台设备自动生成协议日志文件 `proto_<id>.log`，记录所有收发的 CoAP 报文：

- 时间戳、方向 (RECV/SEND)
- 报文类型 (CON/ACK/NON/RST)
- 请求方法或响应码
- URI、Payload 长度
- Block1/Block2 选项信息
- 原始字节 (前 64 字节 Hex)
- Payload 内容 (前 128 字节)

### 11.3 应用日志

每台设备自动生成应用日志文件 `device_<id>.log`，记录设备操作日志。
