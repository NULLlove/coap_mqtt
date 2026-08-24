# MQTT 简化版协议栈项目 —— 设计思路与运行流程

## 1. 项目概述

本项目是一个**从零实现的简化版 MQTT 3.1.1 协议栈**，仅依赖 Windows Winsock2，无第三方库。项目包含三个可执行程序：

| 程序 | 源文件 | 职责 |
|------|--------|------|
| **Broker** | `broker.c` + `mqtt.c` | 消息中转站，管理连接、订阅、转发、Retain 消息 |
| **Device** | `device.c` + `mqtt.c` | IoT 设备模拟器，发布/订阅资源（日志、固件、固件信息） |

核心应用场景：**两台设备通过 Broker 互相订阅对方的日志和固件资源，实现日志实时同步和固件 OTA 升级。**

---

## 2. 整体架构

```
┌─────────────────────────────────────────────────────────┐
│                     应用层 (device.c)                    │
│  ┌─────────────┐  ┌─────────────┐  ┌────────────────┐  │
│  │ 资源发布/订阅 │  │ 固件升级管理 │  │ 日志自动发布   │  │
│  └──────┬──────┘  └──────┬──────┘  └───────┬────────┘  │
│         └────────────────┼─────────────────┘            │
│                          │                              │
│  ┌───────────────────────┴──────────────────────────┐   │
│  │            MQTT 协议 API 层 (mqtt.h)              │   │
│  │  mqtt_send_packet / mqtt_recv_packet              │   │
│  │  mqtt_make_connect / publish / subscribe / ...    │   │
│  └───────────────────────┬──────────────────────────┘   │
│                          │                              │
│  ┌───────────────────────┴──────────────────────────┐   │
│  │         协议栈核心层 (mqtt.c)                      │   │
│  │  ┌──────────┐ ┌──────────┐ ┌──────────────────┐  │   │
│  │  │ 报文编码  │ │ 报文解码  │ │ 主题通配符匹配    │  │   │
│  │  │ mqtt_build│ │mqtt_parse│ │ mqtt_topic_match │  │   │
│  │  └──────────┘ └──────────┘ └──────────────────┘  │   │
│  │  ┌──────────────────────────────────────────┐     │   │
│  │  │         TCP 网络层 (Winsock2)             │     │   │
│  │  │  connect / listen / accept / send / recv  │     │   │
│  │  └──────────────────────────────────────────┘     │   │
│  └──────────────────────────────────────────────────┘   │
└─────────────────────────────────────────────────────────┘
```

---

## 3. MQTT 协议实现 (mqtt.c / mqtt.h)

### 3.1 支持的报文类型

| 类型 | 值 | 方向 | 用途 |
|------|----|------|------|
| CONNECT | 1 | Client → Broker | 建立连接，携带 ClientID 和遗嘱消息 |
| CONNACK | 2 | Broker → Client | 连接确认，返回接受/拒绝码 |
| PUBLISH | 3 | 双向 | 发布消息到主题 |
| PUBACK | 4 | 双向 | QoS 1 的发布确认 |
| SUBSCRIBE | 8 | Client → Broker | 订阅主题过滤器 |
| SUBACK | 9 | Broker → Client | 订阅确认 |
| UNSUBSCRIBE | 10 | Client → Broker | 取消订阅 |
| UNSUBACK | 11 | Broker → Client | 取消订阅确认 |
| PINGREQ | 12 | Client → Broker | 心跳请求 |
| PINGRESP | 13 | Broker → Client | 心跳响应 |
| DISCONNECT | 14 | Client → Broker | 正常断开 |

### 3.2 报文帧格式

每条 MQTT 报文由三部分组成：

```
┌──────────────┬──────────────────┬──────────────┐
│  固定头       │  可变头           │  负载         │
│ (1+1~4 字节)  │ (视报文类型而定)  │ (视报文类型)  │
└──────────────┴──────────────────┴──────────────┘
```

**固定头** (所有报文都有)：
- 第 1 字节: `[报文类型(4位)] [标志位(4位)]`
- 剩余长度: 变长编码 (1~4 字节)，每字节低 7 位有效，最高位为续传标志

**剩余长度变长编码示例**：
```
值 0~127    → 1 字节: [0xxxxxxx]
值 128~16383 → 2 字节: [1xxxxxxx] [0xxxxxxx]
值更大时类推，最多 4 字节
```

### 3.3 QoS 支持

| QoS | 含义 | 实现方式 |
|-----|------|---------|
| 0 | 最多一次 | 直接 PUBLISH，不等确认 |
| 1 | 至少一次 | PUBLISH + 等待 PUBACK，通过 packet_id 匹配 |

### 3.4 主题通配符匹配

支持 MQTT 标准的两种通配符：

- `+` — 匹配**单层**：`registry/+/log` 可匹配 `registry/A/log`、`registry/B/log`
- `#` — 匹配**多层** (必须在末尾)：`registry/#` 可匹配 `registry/A/log`、`registry/B/firmware`

匹配算法采用逐字符比较的贪心策略，在 `mqtt_topic_match(filter, topic)` 中实现。

### 3.5 核心数据结构

```c
typedef struct {
    uint8_t  type;          // 报文类型
    uint8_t  qos;           // QoS 级别 (PUBLISH 用)
    uint8_t  retain;        // retain 标志 (PUBLISH 用)
    uint16_t packet_id;     // 报文标识符
    uint8_t  return_code;   // CONNACK / SUBACK 返回码
    char     topic[128];    // 主题 / ClientID (复用)
    uint8_t  payload_buf[4096]; // 解析时拷贝到此处
    const uint8_t *payload;
    size_t   payload_len;
    char     will_topic[128];   // 遗嘱消息主题
    char     will_message[4096]; // 遗嘱消息内容
} mqtt_msg_t;
```

---

## 4. Broker 实现 (broker.c)

### 4.1 职责总览

```
Broker 启动 → 监听 TCP 1883 端口
    ↓
接受客户端连接 → 为每个客户端创建独立线程 (client_thread)
    ↓
循环接收并处理报文:
    CONNECT      → 回 CONNACK，存储遗嘱消息
    SUBSCRIBE    → 记录订阅 → 推送 Retain 消息 → 回 SUBACK
    PUBLISH      → 更新注册表 → 存储 Retain → 转发给匹配订阅者 → QoS1 回 PUBACK
    UNSUBSCRIBE  → 删除订阅 → 回 UNSUBACK
    PINGREQ      → 回 PINGRESP
    DISCONNECT   → 发布遗嘱 → 清理订阅和注册 → 关闭连接
    ↓
所有客户端断开 → 自动关闭 Broker
```

### 4.2 三大核心数据结构

#### (1) 订阅链表 `g_subs`

```c
typedef struct subscription {
    char     topic[128];      // 主题过滤器 (如 "registry/B/log")
    SOCKET   sock;            // 订阅者的 socket
    char     client_id[32];   // 订阅者 ID
    struct subscription *next;
} subscription_t;
```

- **添加订阅** `sub_add()`: 头插法插入链表
- **删除订阅 (按主题)** `sub_remove_by_sock_and_topic()`: 精确匹配 socket + topic 删除
- **删除订阅 (按连接)** `sub_remove_by_sock()`: 断开时删除该客户端的所有订阅
- **消息转发** `sub_forward()`: 遍历链表，主题匹配则发送 PUBLISH（排除发布者自身）

所有操作通过 `CRITICAL_SECTION g_lock` 保证线程安全。

#### (2) 资源注册表 `g_registry`

```c
typedef struct resource_entry {
    char     client_id[32];   // 资源所有者 (如 "A")
    char     topic_type[32];  // 资源类型 ("log", "firmware", "fwinfo")
    char     full_topic[128]; // 完整主题 ("registry/A/log")
    SOCKET   sock;            // 发布者的 socket
    time_t   last_update;
    struct resource_entry *next;
} resource_entry_t;
```

每当 Broker 收到 `registry/<id>/<type>` 主题的 PUBLISH 时：
- 解析出 `client_id` 和 `topic_type`
- 在注册表中添加或更新条目
- 用于 `find_all` 功能：向 `registry/+/info` 订阅者推送所有已注册资源信息

#### (3) Retain 消息存储 `g_retains`

```c
typedef struct retain_entry {
    char     topic[128];      // 主题
    uint8_t  payload[4096];   // 消息内容
    size_t   payload_len;
    uint8_t  qos;
    struct retain_entry *next;
} retain_entry_t;
```

**Retain 规则 (遵循 MQTT 规范)**：
- `retain=1` 且 `payload_len>0` → 保存/更新该主题的 Retain 消息
- `retain=1` 且 `payload_len=0` → 删除该主题的 Retain 消息
- 新订阅者加入时 → 立即推送所有匹配其过滤器的 Retain 消息
- Retain 消息转发给订阅者时 retain 标志仍为 1

### 4.3 消息转发流程

当 Broker 收到一条 PUBLISH 消息时，处理流程如下：

```
收到 PUBLISH (topic, payload, qos, retain, src_sock)
    │
    ├─ 1. 判断是否是资源注册主题 registry/<id>/<type>
    │      ├─ 是 __DELETE__ → 从注册表删除
    │      └─ 否 → 添加/更新注册表条目
    │
    ├─ 2. 处理 Retain 标志
    │      ├─ retain=1 + payload>0 → 保存 Retain 消息
    │      ├─ retain=1 + payload=0 → 删除 Retain 消息
    │      └─ retain=0 → 不处理 Retain
    │
    ├─ 3. sub_forward(): 遍历订阅链表转发
    │      对每个订阅 s:
    │        if (s->sock != src_sock            ← 不回传给发布者
    │            && mqtt_topic_match(s->topic, topic))  ← 主题匹配
    │          → mqtt_send_packet(s->sock, PUBLISH)
    │
    └─ 4. 如果 QoS=1 → 回 PUBACK 给发布者
```

### 4.4 自动关闭机制

Broker 维护 `g_active_clients` 计数器：
- 每接受一个连接 → `InterlockedIncrement`
- 每断开一个连接 → `InterlockedDecrement`
- 当 `g_ever_connected && g_active_clients == 0` 持续 2 秒 → Broker 自动关闭

---

## 5. 设备实现 (device.c)

### 5.1 主题方案

```
registry/<id>/log       ← 设备 <id> 的运行日志
registry/<id>/firmware  ← 设备 <id> 的固件镜像
registry/<id>/fwinfo    ← 设备 <id> 的固件版本信息
registry/<id>/status    ← 设备 <id> 的在线状态 (遗嘱消息)
registry/+/info         ← 订阅所有设备的资源更新通知
```

### 5.2 设备启动流程

```
device.exe --id B --version 1.0.0-B
    │
    ├─ 1. 初始化 (创建目录 B_log/, B_bin/)
    │
    ├─ 2. TCP 连接 Broker (127.0.0.1:1883)
    │
    ├─ 3. 发送 CONNECT (ClientID="B", Will="registry/B/status: offline")
    │      └─ 等待 CONNACK 确认
    │
    ├─ 4. 创建初始固件文件 (B_bin/firmware_B.bin)
    │      格式: 第1行=版本号, 后续=二进制填充数据
    │
    ├─ 5. 启动 recv_thread (接收线程)
    │      └─ 循环调用 mqtt_recv_packet 处理入站消息
    │
    ├─ 6. 启动 ping_thread (心跳线程)
    │      └─ 每 5 秒发送 PINGREQ 保活
    │
    ├─ 7. 启用 auto_pub_log = 1
    │      └─ dev_log() 产生的每条日志自动 PUBLISH 到 registry/B/log
    │
    └─ 8. 自动发布初始资源:
           ├─ publish_log()     → PUBLISH registry/B/log (retain=1, QoS=1)
           ├─ publish_fwinfo()  → PUBLISH registry/B/fwinfo (retain=1, QoS=1)
           └─ PUBLISH registry/B/status = "online" (retain=1, QoS=1)
```

### 5.3 多线程架构

设备运行时有 **3 个线程** 并行工作：

```
┌───────────────┐  ┌───────────────┐  ┌───────────────┐
│  主线程        │  │  recv_thread  │  │  ping_thread  │
│  (命令交互)    │  │  (消息接收)    │  │  (心跳保活)    │
├───────────────┤  ├───────────────┤  ├───────────────┤
│ 读取用户命令   │  │ mqtt_recv 循环 │  │ Sleep(5000)   │
│ 解析并执行     │  │ 处理 PUBLISH   │  │ PINGREQ       │
│ sub_rd        │  │ 捕获 SUBACK   │  │               │
│ unsub_rd      │  │ 捕获 UNSUBACK │  │               │
│ pub_rd        │  │ 保存资源到文件 │  │               │
│ find_all      │  │ 固件升级检查   │  │               │
│ del_rd        │  │               │  │               │
│ quit          │  │               │  │               │
└───────┬───────┘  └───────┬───────┘  └───────┬───────┘
        │                  │                  │
        └──────────────────┼──────────────────┘
                           │
                   ┌───────┴───────┐
                   │  TCP Socket   │
                   │  (共享, 通过   │
                   │  send_lock    │
                   │  保护并发发送) │
                   └───────────────┘
```

**线程同步机制**：
- `send_lock`: 保护多线程并发 MQTT 发送
- `resp_lock` + `resp_event`: Pending 响应捕获机制（详见 5.5）
- `lock`: 保护日志文件写入

### 5.4 日志自动发布机制 (auto_pub_log)

这是本项目的一个核心设计：**每条日志产生时自动推送给订阅者，实现实时日志同步。**

```c
static void dev_log(device_t *d, const char *fmt, ...) {
    // 1. 格式化日志 → 打印到控制台
    // 2. 写入本地日志文件 (device_B.log)
    // 3. 如果 auto_pub_log == 1 且已连接:
    //    → PUBLISH 到 "registry/B/log" (retain=0, QoS=0)
}
```

**两种日志发布模式**：

| 模式 | 触发 | retain | 用途 |
|------|------|--------|------|
| **全量发布** | `pub_rd log` 命令 | retain=1 | 覆盖式推送完整日志文件 |
| **增量发布** | `dev_log()` 自动触发 | retain=0 | 实时推送单条新日志 |

订阅端收到日志后的处理方式：
- `retain=1` → 以 `"wb"` 模式写入（覆盖旧内容，全量同步）
- `retain=0` → 以 `"ab"` 模式写入（追加到末尾，增量同步）

### 5.5 Pending 响应捕获机制

SUBACK / UNSUBACK / PUBACK 等响应需要与请求配对。由于 `recv_thread` 在独立线程中运行，需要一个机制将响应"捕获"给请求线程。

```
请求线程 (主线程)                      recv_thread
    │                                     │
    ├─ pending_set(pkt_id, MSG_TYPE)      │
    │   (设置等待状态, 重置事件)           │
    │                                     │
    ├─ mqtt_send_packet(请求)             │
    │                                     │
    ├─ pending_wait(timeout)              ├─ mqtt_recv_packet()
    │   WaitForSingleObject(event)        │   收到 SUBACK/UNSUBACK
    │         ...阻塞等待...               │
    │                                     ├─ pending_check_and_capture()
    │                                     │   匹配 packet_id + msg_type ✓
    │                                     │   → 拷贝响应, SetEvent()
    │                                     │
    ├─ 事件触发, 返回成功                  │
    ├─ 读取响应数据                        │
    └─ pending_clear()                    │
```

### 5.6 固件升级流程

当设备 B 订阅了设备 A 的固件 (`sub_rd A firmware`) 后：

```
1. Broker 将 A 的 Retain 固件消息推送给 B
       │
2. B 的 recv_thread 收到 PUBLISH (topic="registry/A/firmware", retain=1)
       │
3. 调用 save_subscribed_resource()
   → 保存到 B_log/peer_firmware_A.bin
       │
4. 检测到 topic_type == "firmware"
   → 调用 check_and_upgrade_firmware()
       │
   ├─ 读取对端固件第 1 行获取版本号
   ├─ 与 d->version 比较
   │
   ├─ 版本相同 → 日志记录, 不升级
   │
   └─ 版本不同 → 执行升级:
       ├─ copy /y 将 peer_firmware_A.bin 覆盖到 B_bin/firmware_B.bin
       ├─ 更新 d->version 为新版本
       ├─ 日志记录 "*** Firmware upgraded ***"
       └─ 调用 publish_fwinfo() 通知其他设备版本已更新
```

### 5.7 遗嘱消息 (Last Will and Testament)

设备连接时注册遗嘱：
```
Will Topic:   registry/B/status
Will Message: "offline"
Will QoS:     1
```

- 正常退出 (`quit`) → 发送 DISCONNECT → Broker 不发布遗嘱
- 异常断开 (网络中断/进程崩溃) → Broker 自动发布遗嘱 → 其他设备收到 "offline"

设备启动时发布在线状态：
```
PUBLISH registry/B/status = "online" (retain=1)
```
→ 新订阅者会收到 "online" (retain)，异常断开后自动变为 "offline"。

---

## 6. 完整运行流程示例

### 6.1 启动阶段

```bash
# run_demo.bat 依次启动:
1. broker.exe                              # Broker 启动, 监听 1883
2. device.exe --id A --version 1.0.0-A     # 设备 A 启动
3. device.exe --id B --version 1.0.0-B     # 设备 B 启动
```

启动后各设备自动：
- 连接 Broker → CONNECT/CONNACK
- 发布日志、固件信息、在线状态 (均为 retain=1)
- 启动接收线程和心跳线程

### 6.2 订阅日志 (sub_rd)

在设备 B 的控制台执行 `sub_rd A log`：

```
B: sub_rd A log
    │
    ├─ 构造 SUBSCRIBE(topic="registry/A/log", QoS=1, pkt_id=N)
    ├─ pending_set(N, SUBACK)
    ├─ 发送到 Broker
    │
    │  [Broker 侧]
    ├─ Broker 收到 SUBSCRIBE
    ├─ sub_add("registry/A/log", B_sock, "B")  → 加入订阅链表
    ├─ retain_send_to_subscriber() → 推送 A 的 Retain 日志 (全量)
    └─ 回 SUBACK(pkt_id=N)
    │
    │  [B 侧]
    ├─ recv_thread 收到 Retain PUBLISH → 以 "wb" 写入 peer_log_A.log
    ├─ recv_thread 收到 SUBACK → pending_check_and_capture() 捕获
    └─ pending_wait() 返回成功 → "SUBACK received"
    │
    │  [后续: A 产生新日志时]
    ├─ A 的 dev_log() → PUBLISH registry/A/log (retain=0)
    ├─ Broker 转发给 B
    └─ B 的 recv_thread → 以 "ab" 追加写入 peer_log_A.log
```

### 6.3 订阅固件并升级 (sub_rd firmware)

在设备 B 的控制台执行 `sub_rd A firmware`：

```
B: sub_rd A firmware
    │
    ├─ 订阅 registry/A/firmware
    │
    │  [Broker 侧]
    ├─ 记录订阅
    ├─ 推送 A 的 Retain 固件 (retain=1)
    └─ 回 SUBACK
    │
    │  [B 侧 recv_thread]
    ├─ 收到 PUBLISH registry/A/firmware (retain=1, 268 bytes)
    ├─ save_subscribed_resource() → 保存到 peer_firmware_A.bin
    ├─ check_and_upgrade_firmware():
    │   ├─ 读取 peer_firmware_A.bin 第 1 行: "1.0.0-A"
    │   ├─ 当前 d->version = "1.0.0-B"
    │   ├─ 版本不同 → 执行升级
    │   ├─ copy /y peer_firmware_A.bin → B_bin/firmware_B.bin
    │   ├─ d->version = "1.0.0-A"
    │   └─ "*** Firmware upgraded to version 1.0.0-A ***"
    └─ publish_fwinfo() → 通知其他设备版本已更新
```

### 6.4 取消订阅 (unsub_rd)

在设备 B 的控制台执行 `unsub_rd A log`：

```
B: unsub_rd A log
    │
    ├─ 构造完整主题: "registry/A/log"
    ├─ 构造 UNSUBSCRIBE(topic="registry/A/log", pkt_id=M)
    ├─ pending_set(M, UNSUBACK)
    ├─ 发送到 Broker
    │
    │  [Broker 侧]
    ├─ 收到 UNSUBSCRIBE
    ├─ sub_remove_by_sock_and_topic(B_sock, "registry/A/log")
    │   → 从订阅链表中删除匹配条目
    └─ 回 UNSUBACK(pkt_id=M)
    │
    │  [B 侧]
    ├─ recv_thread 收到 UNSUBACK → 捕获
    └─ pending_wait() 返回成功 → "UNSUBACK received"
    │
    │  [后续: A 再产生新日志时]
    ├─ A 的 dev_log() → PUBLISH registry/A/log
    ├─ Broker sub_forward() 遍历订阅链表
    └─ B 的订阅已删除 → 不转发 → B 不再收到消息
```

### 6.5 资源查询 (find_all)

```
B: find_all
    │
    ├─ SUBSCRIBE "registry/+/info"
    │
    │  [Broker 侧]
    ├─ 记录订阅
    ├─ registry_send_all(): 遍历注册表，逐条发送资源信息
    │   例: "resource=registry/A/log,client=A,type=log"
    │       "resource=registry/A/firmware,client=A,type=firmware"
    └─ retain_send_to_subscriber(): 推送匹配的 Retain 消息
    │
    │  [B 侧]
    └─ recv_thread 接收并打印所有资源信息
```

### 6.6 断开连接

```
B: quit
    │
    ├─ d.running = 0
    ├─ 等待 recv_thread 和 ping_thread 结束
    ├─ mqtt_tcp_close(sock)  → TCP 断开
    │
    │  [Broker 侧]
    ├─ client_thread 的 recv 返回错误 → 跳出循环
    ├─ 发布遗嘱消息: PUBLISH registry/B/status = "offline"
    ├─ sub_remove_by_sock(B_sock) → 清除 B 的所有订阅
    ├─ registry_remove_by_sock(B_sock) → 清除 B 的所有注册
    ├─ mqtt_tcp_close(B_sock)
    │
    │  [如果 A 订阅了 registry/B/status 或 registry/+/status]
    └─ A 收到遗嘱消息 → 得知 B 已离线
```

---

## 7. 文件结构

```
mqtt/
├── mqtt.h              # 协议常量定义、API 声明
├── mqtt.c              # MQTT 协议栈: 编解码、TCP 网络、主题匹配
├── broker.c            # Broker: 连接管理、订阅表、注册表、Retain、转发
├── device.c            # 设备: 资源发布/订阅、日志自动发布、固件升级
├── run_demo.bat        # 一键启动演示脚本
├── broker.exe          # 编译产物
├── device.exe          # 编译产物
│
├── A_bin/              # 设备 A 的固件目录
│   ├── firmware_A.bin      # 当前运行的固件
│   └── firmware_A_orig.bin # 初始固件备份
├── A_log/              # 设备 A 的日志目录
│   ├── device_A.log        # 本设备日志
│   ├── proto_A.log         # 协议层日志
│   ├── peer_log_B.log      # 从 B 同步来的日志
│   └── peer_firmware_B.bin # 从 B 下载的固件
│
├── B_bin/              # 设备 B 的固件目录 (结构同上)
└── B_log/              # 设备 B 的日志目录 (结构同上)
```

---

## 8. 编译命令

```bash
# 编译 Broker
gcc -Wall -Wextra -O2 -o broker.exe mqtt.c broker.c -lws2_32

# 编译 Device
gcc -Wall -Wextra -O2 -o device.exe mqtt.c device.c -lws2_32
```

---

## 9. 交互命令一览

| 命令 | 格式 | 说明 |
|------|------|------|
| `find_all` | `find_all` | 订阅 `registry/+/info`，查询所有已注册资源 |
| `sub_rd` | `sub_rd <id> <topic>` | 订阅对端资源，如 `sub_rd A log` |
| `unsub_rd` | `unsub_rd <id> <topic>` | 取消订阅对端资源，如 `unsub_rd A log` |
| `pub_rd` | `pub_rd <topic>` | 发布本端资源 (log/firmware/fwinfo) |
| `del_rd` | `del_rd <topic>` | 删除本端资源 (发送 `__DELETE__` + retain=1) |
| `status` | `status` | 显示设备当前状态 |
| `help` | `help` | 显示帮助信息 |
| `quit` | `quit` | 正常退出设备 |

---

## 10. 关键设计决策

| 决策 | 选择 | 原因 |
|------|------|------|
| 日志同步方式 | retain=1 全量 + retain=0 增量 | 新订阅者先获取完整日志，后续实时增量追加 |
| 固件升级触发 | 收到固件后自动检查版本 | 简化操作流程，订阅即触发升级评估 |
| 线程模型 | 每连接一个线程 | 简化并发模型，避免 I/O 多路复用复杂度 |
| 线程安全 | CRITICAL_SECTION | Windows 原生轻量级互斥锁 |
| Pending 响应 | event + packet_id 匹配 | 解决发送线程与接收线程的响应配对问题 |
| 心跳机制 | 独立 ping_thread 每 5 秒 | 与主逻辑解耦，保持连接活跃 |
| 遗嘱消息 | status 主题 + "offline" | 自动通知其他设备离线状态 |
