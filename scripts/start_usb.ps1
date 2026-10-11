param([string]$Port='COM9')
$ErrorActionPreference='Stop'
$root=[IO.Path]::GetFullPath((Join-Path $PSScriptRoot '..'))
New-Item -ItemType Directory -Force -Path (Join-Path $root '.local') | Out-Null
. (Join-Path $PSScriptRoot 'public-python.ps1')
$pythonExecutable=Resolve-PublicPython
$pidPath=Join-Path $root '.local/bridge.pid'
if(Test-Path $pidPath) {
    $bridgePid=[int](Get-Content $pidPath)
    $running=Get-CimInstance Win32_Process -Filter "ProcessId=$bridgePid" -ErrorAction SilentlyContinue
    if($running -and $running.CommandLine -match 'usb_bridge.py') {Write-Output "USB bridge already running (PID $bridgePid)";exit 0}
}
$env:PYTHONUTF8='1'
$pendingCommand=Join-Path $root '.local/bridge-command.json'
if(Test-Path $pendingCommand) {
    $command=Get-Content -LiteralPath $pendingCommand -Raw | ConvertFrom-Json
    if($command.shutdown){Remove-Item -LiteralPath $pendingCommand}
}
$bridge=Start-Process -FilePath $pythonExecutable -ArgumentList '-u','scripts/usb_bridge.py','--port',$Port,'--wait-for-usb' -WorkingDirectory $root -WindowStyle Hidden -RedirectStandardOutput (Join-Path $root '.local/bridge.stdout.log') -RedirectStandardError (Join-Path $root '.local/bridge.stderr.log') -PassThru
$bridge.Id | Set-Content $pidPath
Start-Sleep -Milliseconds 500
if($bridge.HasExited){throw 'USB bridge exited. See .local/bridge.stderr.log.'}
Write-Output "USB bridge started (PID $($bridge.Id))."
