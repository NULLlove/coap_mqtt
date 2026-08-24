# MQTT 与 CoAP 协议对比技术文档

## 1. 协议概述

### 1.1 MQTT (Message Queuing Telemetry Transport)

| 属性 | 说明 |
|------|------|
| 设计组织 | IBM (1999 年)，OASIS 标准 (2014/2019) |
| 核心标准 | MQTT v3.1.1 (2014)、MQTT v5.0 (2019) |
| 设计初衷 | 石油管道卫星链路通信，低带宽、高延迟、不可靠网络 |
| 典型应用 | 智能家居、工业物联网、消息推送、传感器数据汇聚 |

### 1.2 CoAP (Constrained Application Protocol)

| 属性 | 说明 |
|------|------|
| 设计组织 | IETF (RFC 7252, 2014 年) |
| 核心标准 | RFC 7252 (基础)、RFC 7959 (Block)、RFC 7641 (Observe)、RFC 9176 (RD) |
| 设计初衷 | 资源极度受限设备 (RFC 7228 定义的 Class 0/1 设备) |
| 典型应用 | 智能能源、楼宇自动化、轻量级 RESTful API、固件升级 |

---

## 2. 架构对比

### 2.1 通信模型

```
MQTT: 发布/订阅 (Pub/Sub)                    CoAP: 请求/响应 (Request/Response)

  Publisher ──┐                              Client A                Client B
              ├──> Broker ──> Subscriber       │                       │
  Publisher ──┘    (中心化)                     ├── GET /temp ────────>|
                                                   |<── 2.05 25°C ──────|
  解耦: 发送方不知道接收方是谁                   直接通信 或 通过 RD 发现

  特点:                                        特点:
  - 一对多、多对多                              - 一对一 (主)
  - Broker 中心化路由                           - 端到端直接通信
  - 异步推送                                    - 同步请求/响应 (也支持异步 Observe)
  - 客户端不需要知道对端地址                     - 客户端需要知道对端地址 (或通过 RD 查询)
```

### 2.2 网络拓扑

| 特性 | MQTT | CoAP |
|------|------|------|
| 拓扑结构 | 星型 (所有客户端连接 Broker) | 点对点 / RD 辅助发现 |
| 中心节点 | Broker (必需) | RD 服务器 (可选) |
| 单点故障 | Broker 宕机则全网络瘫痪 | 无中心依赖 (RD 宕机可回退直连) |
| 扩展性 | 受限于 Broker 性能 | 分布式，无瓶颈 |

---

## 3. 传输层对比

### 3.1 传输协议

| 特性 | MQTT | CoAP |
|------|------|------|
| 传输层协议 | TCP | UDP |
| 端口 | 1883 / 8883 | 5683 / 5684 |
| 连接方式 | 长连接 (三次握手 + 维持) | 无连接 |
| 连接开销 | 首次约 60+ 字节 (TCP 握手 + CONNECT) | 无连接建立开销 |

### 3.2 可靠性机制

| 特性 | MQTT | CoAP |
|------|------|------|
| 可靠性层 | TCP 保证 (有序、不丢、不重) | 应用层实现 (CON + ACK + 重传) |
| QoS 0 | 最多一次 (底层 TCP 仍可靠) | NON 报文 (不确认) |
| QoS 1 | 至少一次 (PUBACK 确认) | CON 报文 (ACK 确认 + 重传) |
| QoS 2 | 恰好一次 (四次握手) | 无直接等价 (需应用层去重) |
| 消息去重 | QoS 1/2 内置去重 | Message ID 去重 |
| 拥塞控制 | TCP 内置 (慢启动、AIMD) | 无标准拥塞控制 (需自行实现) |

### 3.3 报文头对比

```
MQTT 报文结构 (基于 TCP):
┌──────────┬──────────┬────────────────┐
│ Fixed    │ Variable │ Payload        │
│ Header   │ Header   │                │
│ (2-5字节) │ (可变)   │                │
└──────────┴──────────┴────────────────┘
TCP 头: 20 字节
总最小开销: 22 字节

CoAP 报文结构 (基于 UDP):
┌──────────────────┬───────────┬──────────┐
│ Header (4 字节)  │ Options   │ Payload  │
│ Ver|T|TKL|Code   │ (可变)    │ 0xFF+Data│
│ + Message ID     │           │          │
└──────────────────┴───────────┴──────────┘
UDP 头: 8 字节
总最小开销: 12 字节
```

---

## 4. 带宽利用率分析

### 4.1 单次通信开销

| 场景 | MQTT | CoAP |
|------|------|------|
| 最小报文 (无 payload) | 22 字节 (TCP 20 + MQTT 2) | 12 字节 (UDP 8 + CoAP 4) |
| 小数据 (5 字节 payload) | 27 字节 | 17 字节 |
| 连接建立 | TCP 握手 60+ 字节 + MQTT CONNECT | 无连接开销 |
| 心跳维持 | PINGREQ/PINGRESP (4 字节/周期) | 无 (UDP 无连接) |

### 4.2 不同通信模式下的带宽效率

| 通信模式 | 推荐协议 | 原因 |
|----------|----------|------|
| 低频间歇 (传感器每小时上报) | CoAP | 无连接开销，UDP 即发即走 |
| 高频持续 (传感器每秒上报) | MQTT | TCP 连接复用，2 字节头 < 4 字节头 |
| 大文件传输 (固件升级) | CoAP | Block 分块传输 (RFC 7959) |
| 多播发现 | CoAP | UDP 原生多播支持 |
| 消息推送 | MQTT | Pub/Sub 天然推送，CoAP 需 Observe 或轮询 |

### 4.3 大文件传输能力

| 特性 | MQTT | CoAP |
|------|------|------|
| 分块传输 | 不支持 (需应用层自行实现) | Block1 (请求分块) + Block2 (响应分块) |
| 断点续传 | 不支持 | Block 编号机制支持 |
| 最大 payload | TCP 无硬限制 | 单块默认 256 字节，Block 机制组合大文件 |
| 流控 | TCP 流控 | Block 逐块确认，天然流控 |

---

## 5. 功能特性对比

### 5.1 核心功能

| 功能 | MQTT | CoAP |
|------|------|------|
| 请求方法 | 无 (Pub/Sub 语义) | GET / POST / PUT / DELETE (RESTful) |
| 资源标识 | Topic (分层字符串，如 `home/room1/temp`) | URI (如 `coap://host/sensor/temp`) |
| 资源发现 | 无内置机制，只能借助外部实现 | `/.well-known/core` + CoRE Link Format (RFC 6690) |
| 资源目录 | 无 | RD 服务器 (RFC 9176)，集中注册与发现 |
| 消息推送 | 原生 Pub/Sub 推送 | Observe 选项 (RFC 7641) 订阅通知 |
| 消息保留 | Retained Message (保留最新一条) | 无直接等价 (可通过 RD 版本属性模拟) |
| 遗嘱通知 | LWT (Last Will & Testament) | 无 (需应用层心跳检测) |
| 多播 | 不支持 (TCP 限制) | 支持 (UDP 多播发现) |
| 内容格式 | 二进制/文本自由 | Content-Format 选项 (text, JSON, CBOR, octet-stream 等) |

### 5.2 会话管理

| 特性 | MQTT | CoAP |
|------|------|------|
| 持久会话 | Clean Session = false，恢复订阅和未送达消息 | 无状态 (UDP)，需应用层维护 |
| 心跳机制 | Keep Alive (客户端定期 PINGREQ) | 无内置心跳 (CON 报文本身充当活性检测) |
| 断线重连 | TCP 自动重连 + 恢复会话 | UDP 无连接，每次独立请求 |
| 离线消息 | Broker 缓存 (QoS 1/2 + 持久会话) | 无 (需客户端主动重试) |

### 5.3 安全机制

| 特性 | MQTT | CoAP |
|------|------|------|
| 加密传输 | TLS (MQTT over TLS, 端口 8883) | DTLS (CoAP over DTLS, 端口 5684) |
| 认证 | 用户名/密码 (CONNECT 报文) | 应用层自定义 (无标准认证机制) |
| 双向证书 | TLS 客户端证书 | DTLS 客户端证书 |
| 访问控制 | Broker 端 ACL (Topic 级别) | 应用层实现 |

---

## 6. 订阅主题 vs URI寻址

### 6.1 MQTT Topic

```
格式: 分层字符串，用 / 分隔
示例: home/room1/sensor/temperature

通配符:
  +  → 单层匹配 (home/+/sensor/temperature)
  #  → 多层匹配 (home/#)

特点:
  - 发布时必须指定完整 Topic
  - 订阅时可使用通配符
  - Broker 负责匹配和路由
  - 无类型信息 (纯字符串)
```

### 6.2 CoAP URI + Link Format

```
格式: 标准 URI
示例: coap://192.168.1.100:5683/sensor/temperature

资源发现 (CoRE Link Format, RFC 6690):
  </sensor/temp>;rt="temperature";if="sensor";ver="1.0.0"

属性:
  rt  → 资源类型 (Resource Type)
  if  → 接口描述 (Interface Description)
  ver → 版本号
  hver→ 历史版本列表

特点:
  - URI 精确定位资源
  - Link Format 携带资源元数据 (类型、版本等)
  - 通过 RD 服务器集中注册和发现
  - 支持按资源类型 (rt) 查询
```

---

## 7. QoS 对比

### 7.1 MQTT QoS 等级

| QoS | 名称 | 机制 | 报文交互 | 适用场景 |
|-----|------|------|----------|----------|
| 0 | 最多一次 | 发后即忘 | PUBLISH | 实时性高、允许丢失 (如温度采样) |
| 1 | 至少一次 | 确认重传 | PUBLISH → PUBACK | 需保证送达、允许重复 (如告警) |
| 2 | 恰好一次 | 四次握手 | PUBLISH → PUBREC → PUBREL → PUBCOMP | 严格不丢不重 (如计费数据) |

### 7.2 CoAP 可靠性

| 模式 | 名称 | 机制 | 报文交互 | 对应 MQTT QoS |
|------|------|------|----------|---------------|
| NON | 非确认 | 发后即忘 | 单次 Request | QoS 0 |
| CON | 确认 | 确认 + 超时重传 | Request → ACK | QoS 1 |
| - | - | 无等价机制 | - | QoS 2 (需应用层去重) |

---

## 8. 适用场景对比

### 8.1 选择决策矩阵

| 场景特征 | 推荐协议 | 原因 |
|----------|----------|------|
| 传感器低频上报 (分钟级) | CoAP | 无连接开销，带宽利用率高 |
| 传感器高频上报 (秒级) | MQTT | TCP 连接复用，头部更小 |
| 固件升级 (大文件) | CoAP | Block 分块传输机制 |
| 消息推送/通知 | MQTT | 原生 Pub/Sub，天然推送 |
| 资源发现 | CoAP | RD + Link Format 内置发现 |
| 设备离线检测 | MQTT | LWT 遗嘱机制 |
| 局域网多播发现 | CoAP | UDP 多播支持 |
| 云端设备管理 | MQTT | Broker 集中管理，ACL 控制 |
| RESTful API 服务 | CoAP | GET/POST/PUT/DELETE 语义 |
| 一对多广播 | MQTT | Pub/Sub 一条消息多订阅者 |
| 极度受限设备 (RAM < 10KB) | CoAP | UDP 无连接状态，内存占用小 |
| 需要离线消息缓存 | MQTT | Broker 持久化 + QoS 1/2 |

### 8.2 行业应用案例

| 行业 | MQTT 典型应用 | CoAP 典型应用 |
|------|---------------|---------------|
| 智能家居 | Home Assistant, 智能灯控 | Thread/CHIP 协议栈 |
| 工业物联网 | SCADA 数据采集 | OPC UA over CoAP |
| 智慧能源 | AMI 高级计量 | DLMS/COSEM (智能电表) |
| 楼宇自动化 | BACnet 网关 | BACnet/SC (CoAP 传输) |
| 农业 | 土壤监测数据汇聚 | 传感器低功耗节点 |
| 移动支付 | 消息推送 | - |

---

## 9. 协议栈资源占用

| 资源 | MQTT (最小实现) | CoAP (最小实现) |
|------|-----------------|-----------------|
| RAM | ~10-50 KB (TCP 栈 + 缓冲) | ~2-10 KB (UDP 栈 + 缓冲) |
| Flash | ~30-100 KB | ~10-30 KB |
| CPU | TCP 状态机开销 | 无连接状态，开销极低 |
| 电池影响 | TCP Keep Alive 周期唤醒 | 可深度休眠，按需唤醒 |

---

## 10. 总结对比表

| 特性 | MQTT | CoAP |
|------|------|------|
| 传输层 | TCP | UDP |
| 通信模型 | 发布/订阅 | 请求/响应 (RESTful) |
| 协议头大小 | 2 字节 | 4 字节 |
| 最小报文开销 | 22 字节(TCP 20 字节 + 2 字节) | 12 字节(UDP 8 字节 + 4 字节) |
| 连接建立开销 | 高 (TCP 握手 + CONNECT) | 无 |
| 可靠性 | TCP + QoS 0/1/2 | CON/NON + 重传(应用层) |
| 消息推送 | 原生 Pub/Sub | Observe 订阅 |
| 资源发现 | 无 | RD + Link Format |
| 大文件传输 | 没有分块传输机制 | Block 分块 (RFC 7959) |
| 多播 | 通过发布订阅间接实现多播 | 支持IP多播 |
| 遗嘱机制 | LWT | 无 |
| 消息保留 | Retained | 无 |
| 持久会话 | 支持 | 无 (无状态) |
| 离线消息 | Broker 缓存 | 无 |
| 安全 | SSL/TLS | DTLS |
| 资源占用 | 较高 | 极低 |
| 带宽利用率 (间歇) | 较低 (连接开销) | 高 (无连接) |
| 带宽利用率 (持续) | 高 (连接复用) | 较低 (每报文 4 字节头) |
| 标准化 | OASIS | IETF |
| 适合设备 | 中等资源设备 | 极度受限设备 |
