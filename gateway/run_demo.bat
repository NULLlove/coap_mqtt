@echo off
echo ============================================
echo   CoAP-MQTT Gateway Demo
echo ============================================
echo.
echo Architecture:
echo   [CoAP Device A] --CoAP--^> [Gateway] --MQTT--^> [Broker]
echo   [CoAP Device B] --CoAP--^> [Gateway] --MQTT--^> [Broker]
echo.

echo [1/4] Starting MQTT Broker...
start "MQTT Broker" cmd /k "cd /d ..\mqtt && broker.exe"

echo      Waiting for broker to start...
timeout /t 2 /nobreak >nul

echo [2/4] Starting CoAP Device A (port 5683)...
start "CoAP Device A" cmd /k "cd /d ..\coap && device.exe --id A --port 5683 --peer-ip 127.0.0.1 --peer-port 5684 --version 1.0.0-A"

echo [3/4] Starting CoAP Device B (port 5684)...
start "CoAP Device B" cmd /k "cd /d ..\coap && device.exe --id B --port 5684 --peer-ip 127.0.0.1 --peer-port 5683 --version 1.0.0-B"

echo      Waiting for devices to start...
timeout /t 2 /nobreak >nul

echo [4/4] Starting Gateway...
echo.
echo ============================================
echo   Gateway Commands:
echo     poll                - Poll all devices
echo     fwinfo A            - Get device A fwinfo
echo     log B               - Get device B log
echo     upgrade A fw.bin    - Upgrade device A
echo     status              - Show status
echo     quit                - Exit
echo ============================================
echo.

cd /d ..\gateway
gateway.exe --broker-ip 127.0.0.1 --broker-port 1883 --device A@127.0.0.1:5683 --device B@127.0.0.1:5684
