# 基于 MQTT 协议模拟两台设备的日志读取与固件升级

## Context（背景与目标）

此前已在 `e:\project\coap\` 下用 CoAP 协议（RFC 7252，UDP 请求/响应模型）实现了两台 PC 端模拟设备的相互日志读取和固件升级。本次需求是改用 **MQTT 协议**实现相同功能，作为协议对比与学习。

MQTT 与 CoAP 的核心差异：
- **通信模型**：MQTT 是发布/订阅（Pub/Sub）模型，需要中间 Broker 转发；CoAP 是端到端请求/响应。
- **传输层**：MQTT 基于 TCP 长连接；CoAP 基于 UDP。
- **寻址方式**：MQTT 用主题（Topic）寻址；CoAP 用 URI 路径。
- **可靠性**：MQTT 通过 QoS 级别保证（QoS 1 = PUBLISH + PUBACK）；CoAP 通过 CON + ACK 重传。

目标：在 `e:\project\mqtt\` 下新建一套自包含的 MQTT 实现（自己实现协议栈 + Broker，不依赖外部库或外部 Broker），保持与 CoAP 项目一致的代码风格和演示方式。

---

## 文件结构

```
e:\project\mqtt\
├── mqtt.h         - MQTT 协议常量、报文结构、API 声明
├── mqtt.c         - MQTT 协议栈实现（报文编解码 + TCP 收发）
├── broker.c       - MQTT Broker（消息转发中心，独立进程）
├── device.c       - MQTT 设备客户端（多进程实例化两台设备）
└── run_demo.ps1   - 启动 1 个 broker + 2 个 device
```

编译命令（在 `e:\project\mqtt\` 目录下）：
```
gcc -Wall -Wextra -O2 -o broker.exe mqtt.c broker.c -lws2_32
gcc -Wall -Wextra -O2 -o device.exe mqtt.c device.c -lws2_32
```

---

## 各文件职责与关键设计

### 1. mqtt.h / mqtt.c — MQTT 协议栈

**协议常量**：
- 报文类型：CONNECT(1), CONNACK(2), PUBLISH(3), PUBACK(4), SUBSCRIBE(8), SUBACK(9), PINGREQ(12), PINGRESP(13), DISCONNECT(14)
- QoS 级别：QOS_0(0), QOS_1(1)
- CONNACK 返回码：0=接受

**报文逻辑结构 `mqtt_msg_t`**：
```c
typedef struct {
    uint8_t  type;          // 报文类型
    uint8_t  qos;           // QoS 级别
    uint8_t  retain;        // retain 标志
    uint16_t packet_id;     // 报文标识符（QoS1 用）
    char     topic[128];    // 主题
    uint8_t  payload[2048]; // 负载缓冲（解析时拷贝，避免悬垂指针）
    size_t   payload_len;
    uint8_t  return_code;   // CONNACK 返回码
} mqtt_msg_t;
```

**核心函数**：
- `mqtt_init()` / `mqtt_cleanup()` — Winsock 初始化
- 剩余长度变长编码/解码（每字节低 7 位有效，最高位续传标志）
- `mqtt_build(buf, buflen, msg)` — 序列化报文为字节流
- `mqtt_parse(buf, len, msg)` — 反序列化字节流为报文
- TCP 网络函数：
  - `mqtt_tcp_connect(ip, port)` — 客户端连接
  - `mqtt_tcp_listen(port)` — Broker 监听
  - `mqtt_tcp_accept(listen_sock)` — Broker 接受连接
  - `mqtt_tcp_send(sock, data, len)` — 发送
  - `mqtt_tcp_recv(sock, buf, len, timeout_ms)` — 接收（带超时）
- `mqtt_recv_packet(sock, msg, timeout_ms)` — 接收并解析一个完整 MQTT 报文（先读固定头+剩余长度，再读剩余字节）
- `mqtt_send_packet(sock, msg)` — 编码并发送一个 MQTT 报文

### 2. broker.c — MQTT Broker

**职责**：作为消息转发中心，接收所有客户端的 PUBLISH 并转发给订阅者。

**核心数据结构**：
```c
typedef struct subscription {
    char     topic[128];
    SOCKET   sock;
    char     client_id[32];
    struct subscription *next;
} subscription_t;

// 全局订阅链表 + 临界区保护
```

**工作流程**：
1. `main`：监听 TCP 1883 端口，循环 `accept` 新连接
2. 每个客户端连接创建一个线程 `client_thread`
3. `client_thread` 循环接收报文：
   - **CONNECT** → 回 CONNACK(返回码=0)
   - **SUBSCRIBE** → 添加订阅到链表 → 回 SUBACK
   - **PUBLISH** → 遍历订阅链表，主题匹配的客户端转发 PUBLISH；若 QoS1 则回 PUBACK
   - **PINGREQ** → 回 PINGRESP
   - **DISCONNECT** → 清理该客户端订阅，退出线程
4. 主题匹配支持通配符 `+`（单层）和 `#`（多层）

**日志**：Broker 转发时打印日志到控制台，便于观察消息流转。

### 3. device.c — MQTT 设备客户端

**命令行参数**（与 CoAP 版本一致的风格）：
```
device.exe --id A --broker-ip 127.0.0.1 --broker-port 1883 --peer-id B --version 1.0.0-A
device.exe --id B --broker-ip 127.0.0.1 --broker-port 1883 --peer-id A --version 1.0.0-B
```

**主题设计**：
| 主题 | 发布者 | 订阅者 | 用途 |
|------|--------|--------|------|
| `devices/{id}/log` | {id} | 对端 | 发布本机日志 |
| `devices/{id}/fwinfo` | {id} | 对端 | 发布本机固件信息 |
| `devices/{id}/firmware` | 对端 | {id} | 推送固件升级 |

**设备结构 `device_t`**（复用 CoAP 版本的字段设计）：
```c
typedef struct {
    char     id[16];
    char     broker_ip[64];
    uint16_t broker_port;
    char     peer_id[16];
    char     version[32];
    char     original_version[32];  // 启动时原始版本，升级对端时始终用它
    char     log_buf[4096];
    size_t   log_len;
    FILE    *log_fp;
    CRITICAL_SECTION lock;
    SOCKET   sock;                 // 与 broker 的 TCP 连接
    volatile int running;
    uint16_t next_packet_id;
} device_t;
```

**工作流程**：
1. 解析命令行参数，保存 `original_version`
2. TCP 连接到 Broker，发送 CONNECT，等待 CONNACK
3. 启动接收线程（处理来自 Broker 的转发消息）
4. 订阅主题：`devices/{self_id}/firmware`（接收对端的固件升级）
5. 等待对端就绪（Sleep）
6. 发布日志：PUBLISH `devices/{self_id}/log`（QoS 1）
7. 发布固件信息：PUBLISH `devices/{self_id}/fwinfo`（QoS 1）
8. 推送固件升级：PUBLISH `devices/{peer_id}/firmware`，payload = 原始版本号 + 填充数据
9. 持续运行一段时间，处理对端发来的日志/固件信息/固件升级
10. 打印最终版本，清理退出

**接收线程 `recv_thread`**：
- 循环接收 MQTT 报文
- PUBLISH 处理（按主题分发）：
  - `devices/{peer_id}/log` → 打印对端日志
  - `devices/{peer_id}/fwinfo` → 打印对端固件信息
  - `devices/{self_id}/firmware` → 解析首行版本号，更新本地版本，日志记录升级完成
  - QoS 1 的 PUBLISH 回 PUBACK

**固件升级设计**：
- MQTT 单条消息可承载大 payload（本实现 2048 字节缓冲），固件镜像（版本行 + 260 字节填充 ≈ 300 字节）可单条 PUBLISH 发送，无需应用层分块。
- payload 格式与 CoAP 版本一致：首行版本号 + `\n` + 填充数据（0x00, 0x01, ...）
- 接收方解析首行作为新版本号

**三路日志**（与 CoAP 版本一致）：控制台 + 内存缓冲 + 磁盘文件 `device_{id}.log`

### 4. run_demo.ps1 — 演示启动脚本

**启动顺序**：
1. 启动 `broker.exe`（监听 1883）
2. Sleep 1 秒等待 Broker 就绪
3. 同时启动 device A 和 device B（`-NoNewWindow` 输出到当前控制台）
4. Sleep 0.8 秒让 A 先连接订阅
5. 等待所有进程结束

**输出**：直接打到当前控制台，用 `[broker]`/`[A]`/`[B]` 前缀区分，不重定向避免编码乱码。

---

## 与 CoAP 版本的关键对比

| 维度 | CoAP 版本 | MQTT 版本 |
|------|-----------|-----------|
| 通信模型 | 请求/响应（端到端） | 发布/订阅（经 Broker 转发） |
| 传输层 | UDP | TCP 长连接 |
| 可靠性 | CON + ACK 重传 | QoS 1 + PUBACK |
| 寻址 | URI 路径 `/log` `/firmware` | 主题 `devices/{id}/log` |
| 固件传输 | Block1 分块（256B/块） | 单条 PUBLISH（MQTT 原生支持大消息） |
| 进程数 | 2（两个设备互为客户端/服务器） | 3（1 broker + 2 device） |
| 日志读取 | 客户端主动 GET /log | 订阅对端 log 主题，被动接收 |

---

## 验证方法

1. **编译**：
   ```
   cd e:\project\mqtt
   gcc -Wall -Wextra -O2 -o broker.exe mqtt.c broker.c -lws2_32
   gcc -Wall -Wextra -O2 -o device.exe mqtt.c device.c -lws2_32
   ```
2. **运行**：
   ```
   powershell -ExecutionPolicy Bypass -File .\run_demo.ps1
   ```
3. **预期结果**：
   - Broker 启动并打印监听信息
   - 设备 A、B 连接 Broker，订阅各自主题
   - A 收到 B 的日志和固件信息，B 收到 A 的日志和固件信息
   - A 向 B 推送固件（版本 1.0.0-A），B 向 A 推送固件（版本 1.0.0-B）
   - 最终 A 的版本变为 1.0.0-B，B 的版本变为 1.0.0-A（版本互换）
   - 生成 `device_A.log` 和 `device_B.log` 磁盘日志文件
4. **检查日志文件**：确认 `device_A.log` / `device_B.log` 内容完整，包含升级记录。
