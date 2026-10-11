$ErrorActionPreference='Stop'
$root=[IO.Path]::GetFullPath((Join-Path $PSScriptRoot '..'))
$target=Join-Path $root '.local/avatar-tests'
New-Item -ItemType Directory -Force -Path $target | Out-Null
$vs=& 'C:\Program Files (x86)\Microsoft Visual Studio\Installer\vswhere.exe' -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath
& (Join-Path $vs 'Common7/Tools/Launch-VsDevShell.ps1') -Arch amd64 -HostArch amd64 -SkipAutomaticLocation | Out-Null
Push-Location $target
try {
    & cl /nologo /std:c11 /O2 /LD "$root/main/avatar_renderer.c" /Fe:avatar_renderer.dll
    if($LASTEXITCODE -ne 0){exit $LASTEXITCODE}
    $env:PYTHONUTF8='1'
    & 'D:\conda\envs_dirs\codex\Scripts\python.exe' "$root/tests/check_avatar.py" (Join-Path $target 'avatar_renderer.dll')
    exit $LASTEXITCODE
} finally {Pop-Location}
