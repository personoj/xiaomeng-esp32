$ErrorActionPreference='Stop'
$root=[IO.Path]::GetFullPath((Join-Path $PSScriptRoot '..'))
'{"shutdown":true}' | Set-Content -LiteralPath (Join-Path $root '.local/bridge-command.json')
if(Test-Path (Join-Path $root '.local/bridge.pid')) {
    $bridgePid=[int](Get-Content (Join-Path $root '.local/bridge.pid'))
    Wait-Process -Id $bridgePid -Timeout 8 -ErrorAction SilentlyContinue
}
