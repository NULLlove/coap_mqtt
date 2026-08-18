# CoAP 项目结构体与函数总结

## 一、coap.h / coap.c — CoAP 协议栈

### 1.1 枚举类型

#### coap_type_t — CoAP 报文类型

| 值 | 含义 | 说明 |
|----|------|------|
| COAP_CON (0) | Confirmable | 可确认的报文，需要对端返回 ACK，保证可靠传输 |
| COAP_NON (1) | Non-confirmable | 不可确认的报文，不要求 ACK，适合实时性要求高的场景 |
| COAP_ACK (2) | Acknowledgement | 对 CON 报文的响应确认 |
| COAP_RST (3) | Reset | 重置报文，表示收到但无法处理 |

### 1.2 宏定义常量

#### 请求方法 Code（高3位=class 0）

| 宏名 | 值 | 含义 |
|------|----|------|
| COAP_GET | 0x01 | 获取资源 |
| COAP_POST | 0x02 | 创建资源 |
| COAP_PUT | 0x03 | 更新/创建资源 |
| COAP_DELETE | 0x04 | 删除资源 |

#### 响应码 Code

| 宏名 | 值 | 含义 |
|------|----|------|
| COAP_CREATED | 0x41 | 2.01 Created，资源创建成功 |
| COAP_CHANGED | 0x44 | 2.04 Changed，资源修改成功 |
| COAP_CONTENT | 0x45 | 2.05 Content，请求成功，返回内容 |
| COAP_BAD_REQUEST | 0x80 | 4.00 Bad Request，请求格式错误 |
| COAP_NOT_FOUND | 0x84 | 4.04 Not Found，资源不存在 |
| COAP_METHOD_NOT_ALLOWED | 0x85 | 4.05 Method Not Allowed，方法不允许 |
| COAP_INTERNAL_ERROR | 0xA0 | 5.00 Internal Server Error，服务器内部错误 |

#### 选项编号

| 宏名 | 值 | 含义 |
|------|----|------|
| OPT_URI_PATH | 11 | Uri-Path 选项（资源路径） |
| OPT_CONTENT_FMT | 12 | Content-Format 选项（内容格式） |
| OPT_URI_QUERY | 15 | Uri-Query 选项（查询参数） |
| OPT_BLOCK2 | 23 | Block2 选项（响应分块） |
| OPT_BLOCK1 | 27 | Block1 选项（请求分块） |

#### Content-Format 内容格式

| 宏名 | 值 | 含义 |
|------|----|------|
| FMT_TEXT_PLAIN | 0 | text/plain 纯文本 |
| FMT_LINK_FORMAT | 40 | application/link-format 链接格式（RFC 6690，CoRE） |
| FMT_OCTET_STREAM | 42 | application/octet-stream 二进制流 |

#### 其他常量

| 宏名 | 值 | 含义 |
|------|----|------|
| COAP_VER | 1 | CoAP 协议版本号 |
| COAP_PAYLOAD_MARKER | 0xFF | Payload 分隔符（Options 和 Payload 之间） |
| COAP_MAX_MSG | 2048 | 报文最大长度 |
| COAP_DEFAULT_PORT | 5683 | CoAP 默认 UDP 端口 |
| BLOCK_SZX | 4 | Block1 分块大小指数 |
| BLOCK_SIZE | 256 | Block1 每块大小（= 2^(4+4)） |

### 1.3 核心结构体

#### coap_msg_t — CoAP 报文逻辑表示体

```c
typedef struct {
    coap_type_t type;          // 报文类型: CON/ACK/NON/RST (2bit)
    uint8_t     code;          // 方法/响应码: GET/2.05等 (8bit)
    uint16_t    msg_id;        // 报文编号，事务匹配用 (16bit)
    uint8_t     token[8];      // Token 值 (0-8字节)
    uint8_t     token_len;     // Token 实际长度

    /* 选项字段 */
    char        uri_path[64];     // Uri-Path, 如 "fwinfo" 或 "well-known/core"
    char        uri_query[128];   // Uri-Query, 如 "version=1.0.0"
    int         content_format;   // Content-Format, 如 0=text/plain, 40=link-format

    /* Block1 分块传输字段 (RFC 7959) */
    int         has_block1;      // 是否包含 Block1 选项
    int         block1_num;      // 分块编号 (0-N)
    int         block1_more;     // 是否还有更多分块 (1=是, 0=最后一块)
    int         block1_szx;      // 分块大小指数 (4 表示 256 字节/块)

    /* Payload 负载 */
    uint8_t     payload_buf[COAP_MAX_MSG]; // 解析时内部拷贝缓冲
    const uint8_t *payload;                 // 负载指针 (解析时指向 payload_buf)
    size_t      payload_len;               // 负载长度
} coap_msg_t;
```

**字段说明**：
- `type + msg_id`：CoAP 可靠传输的核心，ACK 回显相同 msg_id 来匹配事务
- `uri_path`：由多段 Uri-Path 选项按 `/` 拼接而成（如 `well-known/core`）
- `block1_num + block1_more`：固件分块传输的进度跟踪
- `payload_buf`：解析时把 payload 从原始 UDP 缓冲区拷贝出来，避免悬垂指针

### 1.4 对外 API 函数

#### 协议栈生命周期

| 函数 | 参数 | 返回值 | 说明 |
|------|------|--------|------|
| `coap_init()` | 无 | 成功 0，失败 -1 | 初始化 Winsock2 库 |
| `coap_cleanup()` | 无 | 无 | 清理 Winsock2 库 |

#### Socket 管理

| 函数 | 参数 | 返回值 | 说明 |
|------|------|--------|------|
| `coap_open_socket(port)` | port: 绑定端口，0 表示不绑定 | SOCKET 句柄或 INVALID_SOCKET | 创建 UDP Socket 并绑定端口；端口=0 时作为客户端用 |
| `coap_close_socket(s)` | s: Socket 句柄 | 无 | 关闭 UDP Socket |

#### 报文编解码

| 函数 | 参数 | 返回值 | 说明 |
|------|------|--------|------|
| `coap_build(buf, buflen, m)` | buf: 输出缓冲, buflen: 缓冲大小, m: 要编码的报文 | 编码后字节数，-1 失败 | 将 coap_msg_t 结构体编码为二进制报文 |
| `coap_parse(buf, len, m)` | buf: 原始数据, len: 数据长度, m: 解析结果 | 成功 0，失败 -1 | 将二进制报文解析为 coap_msg_t 结构体 |

#### UDP 收发

| 函数 | 参数 | 返回值 | 说明 |
|------|------|--------|------|
| `coap_send(s, ip, port, data, len)` | s: socket, ip: 目标IP, port: 目标端口, data: 数据, len: 长度 | 发送字节数，-1 失败 | 发送 UDP 数据报到指定地址 |
| `coap_recv(s, buf, buflen, from_ip, from_port, timeout_ms)` | s: socket, buf: 接收缓冲, buflen: 缓冲大小, from_ip: 输出来源IP, from_port: 输出来源端口, timeout_ms: 超时 | 接收字节数，-1 超时/失败 | 接收 UDP 数据报，带超时 |

#### 名称查询

| 函数 | 参数 | 返回值 | 说明 |
|------|------|--------|------|
| `coap_method_name(code)` | code: 请求码 | 字符串指针 | 将数值请求码转为可读名称（"GET"/"PUT"等） |
| `coap_response_name(code)` | code: 响应码 | 字符串指针 | 将数值响应码转为可读名称（"2.05 Content"等） |

### 1.5 内部函数

| 函数 | 参数 | 返回值 | 说明 |
|------|------|--------|------|
| `append_option(buf, buflen, off, last_num, opt_num, value, value_len)` | buf: 输出缓冲, buflen: 大小, off: 当前偏移(读写), last_num: 上一个选项号, opt_num: 当前选项号, value: 选项值, value_len: 值长度 | 成功 0，失败 -1 | 辅助函数：按 RFC 7252 Delta 编码规则追加一个 Option |

**append_option 的编码规则**：
- Delta < 13：直接写入高4位
- Delta == 13：高4位写 1101，后跟 1 字节扩展
- Delta == 14：高4位写 1110，后跟 2 字节扩展
- Length 同理

---

## 二、device.c — 设备逻辑

### 2.1 核心结构体

#### device_t — 设备状态描述体

```c
typedef struct {
    /* 身份与网络配置 */
    char        id[16];            // 设备ID，如 "A" 或 "B"
    uint16_t    port;              // 服务端监听 UDP 端口
    char        peer_ip[64];       // 对端设备 IP 地址
    uint16_t    peer_port;         // 对端设备 UDP 端口

    /* 固件版本管理 */
    char        version[32];       // 当前固件版本（被升级时会改变）
    char        original_version[32]; // 启动时的原始版本（升级对端时使用）

    /* 文件路径 */
    char        fw_path[64];       // 当前固件文件路径
    char        fw_orig_path[64];  // 原始固件文件路径（升级对端时读取）
    char        fw_versions_dir[64]; // 固件历史版本目录
    char        log_path[64];      // 应用层日志文件路径
    char        proto_log_path[64]; // 协议层日志文件路径
    char        old_fw_copy_path[128]; // 升级时保存的旧固件临时副本

    /* 历史版本管理 */
    char        fw_versions[10][32];       // 历史版本号数组
    char        fw_version_times[10][32];  // 历史版本升级时间数组
    int         fw_version_count;         // 当前历史版本数量 (最大10)

    /* 并发控制与 I/O */
    CRITICAL_SECTION lock;         // 临界区互斥锁，保护日志和版本数组
    FILE       *log_fp;            // 应用层日志文件句柄
    FILE       *proto_log_fp;      // 协议层日志文件句柄
    SOCKET      srv_sock;          // 服务端 Socket（绑定 port）
    SOCKET      cli_sock;          // 客户端 Socket（未绑定）
    volatile int running;          // 运行状态标志
    uint16_t    next_msg_id;       // 下一个可用的 CoAP Message ID
} device_t;
```

**设计要点**：
- `version` vs `original_version` 分离：升级对端时用 `original_version`（原始），防止自身升级覆盖
- `fw_path` vs `fw_orig_path` 分离：当前固件可被覆盖，原始固件保持不变
- `CRITICAL_SECTION lock`：保护文件 I/O 和版本数组的并发访问
- `srv_sock` 和 `cli_sock` 分离：服务端绑定端口收请求，客户端不绑定端口发请求

### 2.2 日志与调试函数

#### dev_log — 应用层日志

| 项目 | 说明 |
|------|------|
| 函数签名 | `static void dev_log(device_t *d, const char *fmt, ...)` |
| 参数 | d: 设备指针, fmt: 格式化字符串, ...: 可变参数 |
| 输出目标 | 控制台（printf）+ 应用日志文件（fprintf） |
| 格式 | `[时间戳] [设备ID] 日志内容` |
| 线程安全 | 用 CRITICAL_SECTION 保护文件写入 |

#### proto_log — 协议层日志

| 项目 | 说明 |
|------|------|
| 函数签名 | `static void proto_log(device_t *d, const char *direction, const coap_msg_t *msg, const uint8_t *raw_data, size_t raw_len)` |
| 参数 | direction: "RECV/SEND", msg: 解析后的报文, raw_data: 原始字节, raw_len: 字节数 |
| 输出目标 | 协议日志文件 |
| 记录内容 | 报文类型、方法/响应码、MsgID、URI、Block1 参数、原始十六进制字节、Payload 内容 |
| 用途 | 协议调试与报文分析 |

### 2.3 固件管理函数

#### read_fw_info — 读取固件信息

| 项目 | 说明 |
|------|------|
| 函数签名 | `static size_t read_fw_info(const char *path, char *version_buf, size_t vbuf_size)` |
| 参数 | path: 固件文件路径, version_buf: 版本号输出缓冲, vbuf_size: 缓冲大小 |
| 返回值 | 文件大小（字节），失败返回 0 |
| 功能 | 读取固件文件大小和首行版本号 |

#### save_fw_version_history_with_file — 保存历史版本

| 项目 | 说明 |
|------|------|
| 函数签名 | `static void save_fw_version_history_with_file(device_t *d, const char *version, const char *fw_file_path)` |
| 参数 | d: 设备, version: 版本号, fw_file_path: 固件文件路径 |
| 返回值 | 无 |
| 功能 | 将固件文件拷贝到 `versions/` 目录，记录版本号和时间戳，超过 10 个版本时 FIFO 淘汰 |

#### get_fw_version_list — 获取版本列表

| 项目 | 说明 |
|------|------|
| 函数签名 | `static int get_fw_version_list(device_t *d, char *buf, size_t buf_size)` |
| 参数 | d: 设备, buf: 输出缓冲, buf_size: 缓冲大小 |
| 返回值 | 写入的字节数 |
| 功能 | 生成当前版本+历史版本的列表字符串 |

#### find_fw_version_file — 查找指定版本的固件

| 项目 | 说明 |
|------|------|
| 函数签名 | `static int find_fw_version_file(device_t *d, const char *version, char *path_buf, size_t buf_size)` |
| 参数 | d: 设备, version: 目标版本号, path_buf: 输出路径缓冲, buf_size: 缓冲大小 |
| 返回值 | 找到返回 1，未找到返回 0 |
| 功能 | 先查当前版本，再遍历历史版本，找到后返回固件文件路径 |

### 2.4 日志过滤函数

#### get_log_by_time_range — 按时间范围获取日志

| 项目 | 说明 |
|------|------|
| 函数签名 | `static size_t get_log_by_time_range(device_t *d, const char *start_time, const char *end_time, uint8_t *out_buf, size_t buf_size)` |
| 参数 | d: 设备, start_time: 起始时间 "YYYY-MM-DD HH:MM:SS", end_time: 结束时间, out_buf: 输出缓冲, buf_size: 缓冲大小 |
| 返回值 | 过滤后的日志字节数 |
| 功能 | 逐行解析日志文件，按时间戳过滤出指定范围内的日志行 |

### 2.5 服务端函数

#### server_thread — 服务端主循环（后台线程）

| 项目 | 说明 |
|------|------|
| 函数签名 | `static DWORD WINAPI server_thread(LPVOID arg)` |
| 参数 | arg: 指向 device_t 的指针 |
| 返回值 | 无（线程函数，返回 DWORD） |
| 运行方式 | 独立后台线程，阻塞等待 UDP 请求 |
| 处理的资源路由 | 见下方路由表 |

**服务端资源路由表**：

| Uri-Path | 方法 | 功能 | 响应 |
|----------|------|------|------|
| `fwinfo` | GET | 返回当前固件版本和大小 | 2.05 Content |
| `fwinfo` | GET + query=`list` | 返回历史版本列表 | 2.05 Content |
| `fwinfo` | GET + query=`version=X` | 返回指定版本的固件文件 | 2.05 Content / 4.04 |
| `log` | GET | 返回全部日志内容 | 2.05 Content |
| `log` | GET + query=`start_time=X&end_time=Y` | 返回时间范围内的日志 | 2.05 Content |
| `firmware` | PUT (Block1) | 接收固件分块写入 | 2.04 Changed |
| `well-known/core` | GET | 返回 CoRE Link Format 资源列表 | 2.05 Content (FMT_LINK_FORMAT) |
| 其他 | GET/PUT | — | 4.04 Not Found |

**server_thread 核心流程**：
```
while (running):
  coap_recv() 阻塞等待请求 (500ms 超时)
  coap_parse() 解析报文
  proto_log() 记录协议日志
  strcmp(uri_path) 路由分发
  构造响应 resp
  coap_build() 编码
  coap_send() 发送响应
```

### 2.6 客户端函数

#### coap_exchange — 请求-响应交换（核心）

| 项目 | 说明 |
|------|------|
| 函数签名 | `static int coap_exchange(device_t *d, coap_msg_t *req, coap_msg_t *resp)` |
| 参数 | d: 设备, req: 请求报文, resp: 响应报文输出 |
| 返回值 | 成功 0，失败 -1 |
| 功能 | 完整的一次请求-响应流程：构造→发送→等待ACK→校验MsgID匹配→返回 |
| 重试策略 | 最多 3 次重试，每次超时 2 秒 |

**coap_exchange 流程**：
```
1. req.msg_id = next_msg_id++
2. coap_build() 构造二进制报文
3. proto_log() 记录发送日志
4. coap_send() 发送到对端
5. coap_recv() 等待 ACK (超时 2000ms)
6. coap_parse() 解析响应
7. 校验 resp.msg_id == req.msg_id (事务匹配)
8. 成功返回，超时或不匹配则重试
```

#### client_get_fwinfo — 获取对端固件信息

| 项目 | 说明 |
|------|------|
| 函数签名 | `static void client_get_fwinfo(device_t *d)` |
| 参数 | d: 设备 |
| 功能 | 构造 `GET /fwinfo` 请求，调用 coap_exchange 获取对端固件版本和大小 |
| 输出 | 打印到控制台 |

#### client_get_log — 获取对端日志

| 项目 | 说明 |
|------|------|
| 函数签名 | `static void client_get_log(device_t *d)` |
| 参数 | d: 设备 |
| 功能 | 构造 `GET /log` 请求，获取对端日志并保存到 `peer_log_<id>.log` |
| 输出 | 控制台打印 + 文件保存 |

#### client_upgrade_firmware — 固件升级（核心）

| 项目 | 说明 |
|------|------|
| 函数签名 | `static void client_upgrade_firmware(device_t *d)` |
| 参数 | d: 设备 |
| 功能 | 读取本机原始固件，通过 Block1 分块 PUT 推送给对端 |
| 流程 | 读取文件 → 按 256 字节分块 → 循环 PUT → 等待对端升级完成响应 |
| 特点 | 流式读取，不把整个文件加载到内存；支持 Block1 分块确认 |

**Block1 分块升级流程**：
```
读取 fw_orig_path 文件到内存
offset = 0, block_no = 0
while offset < file_size:
  chunk = min(256, file_size - offset)
  more = (offset + chunk < file_size) ? 1 : 0
  构造 PUT /firmware, Block1(num=block_no, more=more, szx=4)
  调用 coap_exchange() 发送
  检查响应: 2.04 → 继续; 其他 → 中断
  offset += chunk, block_no++
打印升级完成信息
```

#### client_get_fw_version_list — 获取对端版本列表

| 项目 | 说明 |
|------|------|
| 函数签名 | `static void client_get_fw_version_list(device_t *d)` |
| 参数 | d: 设备 |
| 功能 | 构造 `GET /fwinfo?list` 请求，获取对端所有历史版本号和时间戳 |

#### client_get_fw_by_version — 获取指定版本固件

| 项目 | 说明 |
|------|------|
| 函数签名 | `static void client_get_fw_by_version(device_t *d, const char *version)` |
| 参数 | d: 设备, version: 目标版本号 |
| 功能 | 构造 `GET /fwinfo?version=<version>` 请求，获取指定版本的固件文件并保存 |
| 输出 | 保存到 `peer_fw_<id>_<ver>.bin` |

#### client_get_log_by_time — 按时间范围获取对端日志

| 项目 | 说明 |
|------|------|
| 函数签名 | `static void client_get_log_by_time(device_t *d, const char *start_time, const char *end_time)` |
| 参数 | d: 设备, start_time: 起始时间, end_time: 结束时间 |
| 功能 | 构造 `GET /log?start_time=X&end_time=Y` 请求，获取时间范围内的日志 |

### 2.7 主函数

#### main — 程序入口

| 项目 | 说明 |
|------|------|
| 函数签名 | `int main(int argc, char **argv)` |
| 参数 | argc: 参数个数, argv: 参数数组 |
| 返回值 | 成功 0，失败 1 |

**main 启动流程**：
```
1. 解析命令行参数: --id, --port, --peer-ip, --peer-port, --version
2. 初始化 device_t 结构体: 设置文件路径、读取历史版本、初始化锁
3. coap_init() 初始化协议栈
4. coap_open_socket() 打开服务端和客户端 Socket
5. 打开日志文件: device_<id>.log + proto_<id>.log
6. CreateThread() 启动服务端后台线程 (server_thread)
7. 进入命令循环: fgets() 读取用户输入，分发到对应 client_ 函数
8. 用户输入 "quit" → 设置 running=0 → 等待服务端线程退出 → 清理 → 退出
```

**支持的交互命令**：

| 命令 | 调用函数 | 功能 |
|------|---------|------|
| `get_fwinfo` | client_get_fwinfo | 获取对端固件信息 |
| `get_fw_list` | client_get_fw_version_list | 获取对端历史版本列表 |
| `get_fw <version>` | client_get_fw_by_version | 获取对端指定版本的固件文件 |
| `upgrade` | client_upgrade_firmware | 固件升级（Block1 分块推送） |
| `get_log` | client_get_log | 获取对端全部日志 |
| `get_log_time <start> <end>` | client_get_log_by_time | 按时间范围获取日志 |
| `do_all` | 依次调用 | 一键执行 get_fwinfo + upgrade + get_log |
| `status` | 直接打印 | 查看本机状态 |
| `quit` | 直接退出 | 关闭设备 |

### 2.8 文件路径约定

| 路径模式 | 用途 |
|---------|------|
| `<id>_bin/firmware_<id>.bin` | 当前固件文件（会被升级覆盖） |
| `<id>_bin/firmware_<id>_orig.bin` | 原始固件文件（升级对端时读取） |
| `<id>_bin/versions/firmware_<id>_<ver>_<time>.bin` | 历史版本固件 |
| `<id>_bin/old_fw_tmp.bin` | 升级时的临时旧固件副本 |
| `<id>_log/device_<id>.log` | 应用层日志 |
| `<id>_log/proto_<id>.log` | 协议层日志（CoAP 报文详情） |

---

## 三、整体调用关系图

```
main()
  │
  ├── coap_init()                    // 初始化协议栈
  ├── coap_open_socket(port)          // 创建服务端 Socket
  ├── coap_open_socket(0)             // 创建客户端 Socket (不绑定)
  │
  ├── CreateThread → server_thread()  // 后台服务端线程
  │     │
  │     ├── coap_recv()               // 阻塞接收 UDP 请求
  │     ├── coap_parse()              // 解析 CoAP 报文
  │     ├── proto_log()               // 记录协议日志
  │     │
  │     ├── [GET /fwinfo]             // 路由匹配
  │     │     └── read_fw_info()      // 读取固件信息
  │     │
  │     ├── [GET /fwinfo?list]
  │     │     └── get_fw_version_list() // 获取版本列表
  │     │
  │     ├── [GET /fwinfo?version=X]
  │     │     └── find_fw_version_file() // 查找历史固件
  │     │
  │     ├── [GET /log]
  │     │     └── fopen + fread        // 读取日志文件
  │     │
  │     ├── [GET /log?start_time=X&end_time=Y]
  │     │     └── get_log_by_time_range() // 按时间过滤日志
  │     │
  │     ├── [PUT /firmware]
  │     │     └── Block1 分块写入固件文件
  │     │         ├── 块0: 保存旧副本 → 保存历史版本
  │     │         └── 最后一块: 更新版本号
  │     │             └── save_fw_version_history_with_file()
  │     │
  │     ├── [GET /.well-known/core]
  │     │     └── 返回 CoRE Link Format 资源列表
  │     │
  │     └── coap_build() + coap_send() // 构造并发送响应
  │
  └── 命令循环 (主线程)
        │
        ├── "get_fwinfo"
        │     └── client_get_fwinfo() → coap_exchange()
        │
        ├── "get_fw_list"
        │     └── client_get_fw_version_list() → coap_exchange()
        │
        ├── "get_fw <ver>"
        │     └── client_get_fw_by_version() → coap_exchange()
        │
        ├── "upgrade"
        │     └── client_upgrade_firmware() → coap_exchange() × N (Block1)
        │
        ├── "get_log"
        │     └── client_get_log() → coap_exchange()
        │
        ├── "do_all"
        │     └── 依次调用 get_fwinfo → upgrade → get_log
        │
        └── "quit" → running=0 → 等待服务端线程退出 → 清理
```
