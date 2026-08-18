@echo off
echo ============================================
echo   Building CoAP-MQTT Gateway
echo ============================================

gcc -Wall -Wextra -O2 -I../coap -I../mqtt -o gateway.exe ../coap/coap.c ../mqtt/mqtt.c gateway.c -lws2_32

if %errorlevel% equ 0 (
    echo.
    echo Build successful: gateway.exe
    echo.
    echo Usage:
    echo   gateway.exe --broker-ip 127.0.0.1 --broker-port 1883 --device A@127.0.0.1:5683 --device B@127.0.0.1:5684
) else (
    echo.
    echo Build FAILED!
)
pause
