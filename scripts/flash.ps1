param([string]$Port='COM9')
$ErrorActionPreference='Stop'
$root=[IO.Path]::GetFullPath((Join-Path $PSScriptRoot '..'))
$env:PYTHONUTF8='1'
Push-Location $root
try {
    & 'D:\Espressif\tools\python\v6.1\venv\Scripts\python.exe' -m esptool --chip esp32s3 --port $Port --baud 921600 write-flash --flash-mode dio --flash-size 16MB --flash-freq 80m 0x0 build/bootloader/bootloader.bin 0x8000 build/partition_table/partition-table.bin 0x10000 build/xiaoyun_terminal.bin 0x310000 build/srmodels/srmodels.bin 0x690000 .local/avatar-vector.bin
    exit $LASTEXITCODE
} finally { Pop-Location }
