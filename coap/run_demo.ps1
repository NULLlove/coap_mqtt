# =====================================================================
# run_demo.ps1 - 启动 RD 服务器 + 两台 CoAP 设备, 演示基于 RD 的资源发现
#
# 运行方式 (在 e:\project\coap 目录下, 本机终端):
#   powershell -ExecutionPolicy Bypass -File .\run_demo.ps1
#
# 三进程模式:
#   - RD 服务器 (端口 5685): 资源目录, 接收设备注册, 提供查询
#   - 设备 A (端口 5683): 向 RD 注册, 通过 RD 发现设备 B
#   - 设备 B (端口 5684): 向 RD 注册, 通过 RD 发现设备 A
#
# 输出直接打到当前控制台 (用 [RD]/[A]/[B] 前缀区分), 不做重定向, 避免编码乱码。
# =====================================================================
$ErrorActionPreference = 'Continue'
Set-Location -Path $PSScriptRoot

Write-Host "==== Starting RD server + two CoAP devices (RD-based discovery) ====" -ForegroundColor Cyan
Write-Host ""

# 1. 启动 RD 服务器 (端口 5685)
Write-Host "[启动 RD 服务器 on :5685]" -ForegroundColor Yellow
$pRD = Start-Process -FilePath ".\rd_server.exe" `
    -ArgumentList "--port","5685","--ttl","3600" `
    -NoNewWindow -PassThru

Start-Sleep -Milliseconds 1000   # 等 RD 服务器就绪

# 2. 启动设备 A: 监听 5683, 通过 RD 发现设备 B (peer-id=B)
Write-Host "[启动设备 A on :5683, peer-id=B]" -ForegroundColor Yellow
$pA = Start-Process -FilePath ".\device.exe" `
    -ArgumentList "--id","A","--port","5683","--peer-id","B",`
                   "--rd-ip","127.0.0.1","--rd-port","5685",`
                   "--version","1.0.0-A" `
    -NoNewWindow -PassThru

Start-Sleep -Milliseconds 1500   # 等 A 向 RD 注册

# 3. 启动设备 B: 监听 5684, 通过 RD 发现设备 A (peer-id=A)
Write-Host "[启动设备 B on :5684, peer-id=A]" -ForegroundColor Yellow
$pB = Start-Process -FilePath ".\device.exe" `
    -ArgumentList "--id","B","--port","5684","--peer-id","A",`
                   "--rd-ip","127.0.0.1","--rd-port","5685",`
                   "--version","1.0.0-B" `
    -NoNewWindow -PassThru

# 4. 等待两个设备进程结束 (RD 服务器会一直运行)
$pA.WaitForExit()
$pB.WaitForExit()

# 5. 关闭 RD 服务器
Write-Host ""
Write-Host "[关闭 RD 服务器]" -ForegroundColor Yellow
if (!$pRD.HasExited) {
    $pRD | Stop-Process -Force
}

Write-Host ""
Write-Host "==== Demo finished ====" -ForegroundColor Cyan
