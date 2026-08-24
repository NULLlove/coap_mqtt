@echo off
REM MQTT设备演示脚本
REM 同时启动Broker和两个设备，进行相互日志读取和固件升级

echo ==== Starting MQTT Demo (mutual log-read and firmware upgrade) ====

REM 启动Broker（后台运行）
echo Starting Broker...
start "MQTT Broker" cmd /k "broker.exe"

REM 等待Broker启动
timeout /t 2 /nobreak >nul

REM 启动设备A
echo Starting Device A...
start "Device A" cmd /k "device.exe --id A --version 1.0.0-A"

REM 启动设备B
echo Starting Device B...
start "Device B" cmd /k "device.exe --id B --version 1.0.0-B"

echo.
echo ==== All processes started ====
echo.
echo Available commands for each device:
echo   get_fwinfo       - Get peer firmware info
echo   get_fw_list      - Get peer firmware version list
echo   get_fw ^<version^> - Get peer firmware by version
echo   upgrade          - Upgrade peer firmware
echo   get_log          - Get peer log
echo   do_all           - Execute all: get_fwinfo + upgrade + get_log
echo   sub_rd ^<id^> ^<topic^> - Subscribe to peer's resource
echo   unsub_rd ^<topic^>    - Unsubscribe from topic
echo   status           - Show current device status
echo   help             - Show help
echo   quit             - Exit device
echo.
echo Close the windows to exit all processes.
echo.