# coap编译运行

## 直连模式
### 编译

    gcc -Wall -Wextra -O2 -o device_link.exe coap.c device_link.c -lws2_32

### 运行

    .\device_link.exe --id A --port 5683 --version 1.0.0-A --scan
    .\device_link.exe --id B --port 5684 --version 1.0.0-B --scan

### 命令行操作

| 命令 | 描述 |
| --- | --- |
| `discover` | 发现其他设备 |
| `peers` | 显示已发现的设备 |
| `peer_info <n>` | 显示设备 n 的信息 |
| `get_fwinfo <n>` | 获取设备 n 的固件信息 |
| `get_log <n>` | 获取设备 n 的日志 |
| `get_fw <n>` | 获取设备 n 的固件，升级自身 |
| `status` | 显示设备状态 |
| `help` | 显示帮助帮助 |

***

## RD模式
### 编译

    gcc -Wall -Wextra -O2 -o device.exe coap.c device.c -lws2_32
    gcc -Wall -Wextra -O2 -o rd_server.exe coap.c rd_server.c -lws2_32

### 运行

    rd_server.exe --port 5685 --ttl 3600
    device.exe --id A --port 5683 --peer-id B --rd-ip 127.0.0.1 --rd-port 5685 --version 1.0.0-A
    device.exe --id B --port 5684 --peer-id A --rd-ip 127.0.0.1 --rd-port 5685 --version 1.0.0-B

### 命令行操作

| 命令 | 描述 |
| --- | --- |
| `rd_register` | 注册资源到RD服务器 | 
| `rd_update` | 更新RD注册 | 
| `rd_deregister` | 从RD服务器注销资源 | 
| `rd_lookup <id>` | 查找设备 id 的信息 |
| `rd_find <rt>` | 查找资源类型为 rt 的设备 |
| `rd_list` | 列出所有在RD上注册的设备 |
| `rd_check <id>` | 检查设备 id 的版本是否已更改 |
| `get_link_id` | 显示连接的客户端和版本缓存 |
| `get_log <id> [start] [end]` | 获取设备 id 的日志，可选时间筛选 HH:MM |
| `get_fw <id>` | 获取设备 id 的最新固件，升级自身 |
| `get_fw <version> <id>` | 获取设备 id 的指定版本固件，升级自身 |
| `status` | 显示设备状态 |
| `help` | 显示帮助帮助 |
| `quit` | 退出设备 |

***

### 文件解释

| 文件名 | 描述 |
| --- | --- |
| firmware_id.bin | 设备固件文件，自动生成，版本号 + 填充字符 |
| device_id.log | 设备 id 的日志文件,运行时产生 |
| peer_log.log | 从对端获取的日志文件 |
| proto_id.log | 传输报文的详细内容 |
| coap.c | CoAP协议实现 |
| rd_server.c | RD服务器实现 |
| rd_server.h | RD服务器头文件 |
| device.c | 设备实现,rd模式下使用 |
| device_link.c | 直连模式实现 |

