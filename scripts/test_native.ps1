$ErrorActionPreference='Stop'
$root=[IO.Path]::GetFullPath((Join-Path $PSScriptRoot '..'))
$target=Join-Path $root '.local/native-tests'
New-Item -ItemType Directory -Force -Path $target | Out-Null
# Compile the production functions with host driver substitutions. Only includes
# and linker symbol annotations differ; all state and DMA logic stays unchanged.
$production=@('main/playback.c','main/main.c') | ForEach-Object {
    $source=Get-Content -LiteralPath (Join-Path $root $_) -Raw
    $source=[regex]::Replace($source,'(?m)^#include[^\r\n]*','')
    [regex]::Replace($source,' asm\("[^"]+"\)','')
}
$combined='#include "native_mocks.h"' + "`n" + ($production -join "`n")
[IO.File]::WriteAllText((Join-Path $target 'firmware_under_test.c'),$combined)
$vs=& 'C:\Program Files (x86)\Microsoft Visual Studio\Installer\vswhere.exe' -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath
if(-not $vs){throw 'Native simulation requires the existing Visual Studio C++ tools.'}
& (Join-Path $vs 'Common7/Tools/Launch-VsDevShell.ps1') -Arch amd64 -HostArch amd64 -SkipAutomaticLocation | Out-Null
Push-Location $target
try {
    & cl /nologo /std:c11 /W3 /I"$root/tests" /I"$target" /I"$root/managed_components/espressif__cjson/cJSON" "$root/tests/native_playback.c" "$root/main/ptt_capture.c" "$root/managed_components/espressif__cjson/cJSON/cJSON.c" /Fe:native_playback.exe
    if($LASTEXITCODE -ne 0){exit $LASTEXITCODE}
    & '.\native_playback.exe'
    if($LASTEXITCODE -ne 0){exit $LASTEXITCODE}
    & cl /nologo /std:c11 /W3 "$root/tests/native_ptt.c" "$root/main/ptt_capture.c" /Fe:native_ptt.exe
    if($LASTEXITCODE -ne 0){exit $LASTEXITCODE}
    & '.\native_ptt.exe'
    exit $LASTEXITCODE
} finally {Pop-Location}
