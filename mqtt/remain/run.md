# mqtt编译运行

## 编译

    gcc -Wall -Wextra -O2 -o broker.exe mqtt.c broker.c -lws2_32
    gcc -Wall -Wextra -O2 -o device.exe mqtt.c device.c -lws2_32

## 运行

    .\broker.exe
    .\device.exe --id A --version 1.0.0-A
    .\device.exe --id B --version 1.0.0-B

## 命令行操作

| 命令 | 描述 |
| --- | --- |
| `find_all` | 查询所有可订阅资源 |
| `pub_rd <topic>` | 发布资源 |
| `sub_rd <id> <topic>` | 订阅对端资源 |
| `unsub_rd <id> <topic>` | 取消订阅对端资源 |
| `del_rd <topic>` | 删除自己的资源 |
| `status` | 显示设备状态 |
| `help` | 显示帮助 |
| `quit` | 退出设备 |

## 文件解释

| 文件名 | 描述 |
| --- | --- |
| firmware_id.bin | 设备固件文件，自动生成，版本号 + 填充字符 |
| device_id.log | 设备 id 的日志文件,运行时产生 |
| peer_log.log | 从对端获取的日志文件 |
| proto_id.log | 传输报文的详细内容 |
| mqtt.c | mqtt协议实现 |
| mqtt.h | mqtt协议头文件 |
| broker.c | broker代理服务器实现 |
| device.c | device实现 |

