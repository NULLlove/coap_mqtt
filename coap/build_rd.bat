@echo off
REM ============================================================
REM 编译 CoAP 项目 (RD 服务器 + 设备程序)
REM ============================================================

cd /d "%~dp0"

echo === 编译 RD 服务器 ===
gcc -Wall -Wextra -O2 -o rd_server.exe coap.c rd_server.c -lws2_32
if %ERRORLEVEL% NEQ 0 goto :failed

echo.
echo === 编译设备程序 ===
gcc -Wall -Wextra -O2 -o device.exe coap.c device.c -lws2_32
if %ERRORLEVEL% NEQ 0 goto :failed

echo.
echo === 编译成功 ===
echo.
echo 运行方式 (三进程 RD 模式):
echo   1. 启动 RD 服务器:
echo      rd_server.exe --port 5685 --ttl 3600
echo.
echo   2. 启动设备 A (新终端):
echo      device.exe --id A --port 5683 --peer-id B --rd-ip 127.0.0.1 --rd-port 5685 --version 1.0.0-A
echo.
echo   3. 启动设备 B (新终端):
echo      device.exe --id B --port 5684 --peer-id A --rd-ip 127.0.0.1 --rd-port 5685 --version 1.0.0-B
echo.
echo 一键启动 (PowerShell):
echo   powershell -ExecutionPolicy Bypass -File .\run_demo.ps1
echo.
echo 传统直连模式 (不使用 RD):
echo   device.exe --id A --port 5683 --peer-ip 127.0.0.1 --peer-port 5684 --version 1.0.0-A
echo   device.exe --id B --port 5684 --peer-ip 127.0.0.1 --peer-port 5683 --version 1.0.0-B
echo.
goto :end

:failed
echo.
echo === 编译失败 ===

:end
echo.
pause
