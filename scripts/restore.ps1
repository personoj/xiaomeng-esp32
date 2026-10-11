param([string]$Port='COM9')
$ErrorActionPreference='Stop'
$root=[IO.Path]::GetFullPath((Join-Path $PSScriptRoot '..'))
$path=Join-Path $root '.local/backups/original-20261010-COM9-16MB.bin'
$manifest=Get-Content (Join-Path $root '.local/backups/original-20261010-manifest.json') -Raw | ConvertFrom-Json
if ((Get-Item $path).Length -ne $manifest.size -or (Get-FileHash -LiteralPath $path -Algorithm SHA256).Hash -ne $manifest.sha256) { throw 'Backup verification failed.' }
$env:PYTHONUTF8='1'
& 'D:\Espressif\tools\python\v6.1\venv\Scripts\python.exe' -m esptool --chip esp32s3 --port $Port --baud 921600 write-flash 0 $path
exit $LASTEXITCODE
