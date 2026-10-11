param([Parameter(ValueFromRemainingArguments=$true)][string[]]$IdfArgs)
$ErrorActionPreference = 'Stop'
$env:PYTHONUTF8 = '1'
. 'D:\Espressif\tools\Microsoft.v6.1.PowerShell_profile.ps1'
$env:IDF_TARGET = 'esp32s3'
$root = [IO.Path]::GetFullPath((Join-Path $PSScriptRoot '..'))
idf.py -C $root @IdfArgs
exit $LASTEXITCODE
